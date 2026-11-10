#include "RailwayTrain.h"

#include "Algo/BinarySearch.h"
#include "RailwayRouteGeometry.h"
#include "RailwayRoutePlanner.h"

namespace
{
bool SharesPrefixThrough(const FRailwayPlan& Old, const FRailwayPlan& New, int32 Occupied)
{
    if (!Old.Sections.IsValidIndex(Occupied) || !New.Sections.IsValidIndex(Occupied)) return false;
    for (int32 I = 0; I <= Occupied; ++I)
    {
        const FRailwaySection& A = Old.Sections[I];
        const FRailwaySection& B = New.Sections[I];
        if (A.Leg.EdgeIndex != B.Leg.EdgeIndex || A.EntryDistance != B.EntryDistance ||
            (I < Occupied && A.TravelDistance != B.TravelDistance)) return false;
    }
    return true;
}

bool SameStop(const FRailwayPlannedStop& A, const FRailwayPlannedStop& B)
{
    return A.StationIndex == B.StationIndex;
}

bool SameItinerary(const FRailwayPlan& A, const FRailwayPlan& B)
{
    if (A.Sections.Num() != B.Sections.Num() || A.Stops.Num() != B.Stops.Num() || A.Sections.IsEmpty() ||
        !SharesPrefixThrough(A, B, A.Sections.Num() - 1) ||
        A.Sections.Last().TravelDistance != B.Sections.Last().TravelDistance) return false;
    for (int32 I = 0; I < A.Stops.Num(); ++I)
        if (A.Stops[I].Distance != B.Stops[I].Distance || !SameStop(A.Stops[I], B.Stops[I])) return false;
    return true;
}

// 将当前位置已服务的站次移到待停站之前。按身份逐个消费，保留同里程尚未服务的其他站。
int32 ContinueStopCursor(const ARailwayTrain& Train, FRailwayPreparedPlan& New, bool bMatchServed)
{
    TArray<FRailwayPlannedStop>& Stops = New.Plan.Stops;
    constexpr double Tol = .0001;
    if (bMatchServed && Train.Distance == New.StartDistance &&
        SameItinerary(Train.ActivePlan, New.Plan))
        return Train.NextStop; // 手动倒车也不会让同一份时刻表重放已服务站次。

    TArray<FRailwayPlannedStop> ServedHere;
    const bool bDwelling = Train.State == ERailwayState::Dwelling;
    const int32 First = Algo::LowerBoundBy(Train.ActivePlan.Stops, Train.Distance - Tol, &FRailwayPlannedStop::Distance);
    for (int32 I = First; I < Train.NextStop; ++I)
    {
        const FRailwayPlannedStop& Old = Train.ActivePlan.Stops[I];
        if (Old.Distance > Train.Distance + Tol) break;
        const bool bOccupied = bDwelling && I == Train.NextStop - 1;
        int32 Match = INDEX_NONE;
        if (bMatchServed)
        {
            const int32 Begin = Algo::LowerBoundBy(Stops, New.StartDistance - Tol, &FRailwayPlannedStop::Distance);
            for (int32 J = Begin; J < Stops.Num() && Stops[J].Distance <= New.StartDistance + Tol; ++J)
            {
                if (SameStop(Old, Stops[J]))
                {
                    Match = J;
                    break;
                }
            }
        }
        if (Match == INDEX_NONE && !bOccupied) continue;
        FRailwayPlannedStop& Served = ServedHere.Add_GetRef(Match == INDEX_NONE ? Old : Stops[Match]);
        Served.Distance = New.StartDistance;
        if (Match != INDEX_NONE) Stops.RemoveAt(Match);
    }

    const int32 FirstPending = Algo::LowerBoundBy(Stops, New.StartDistance, &FRailwayPlannedStop::Distance);
    if (!ServedHere.IsEmpty()) Stops.Insert(ServedHere, FirstPending);
    return FirstPending + ServedHere.Num();
}
}

bool ARailwayTrain::GetCurrentTrackLocation(FRailwayTrackLocation& OutLocation) const
{
    OutLocation = FRailwayTrackLocation();
    const ARailwayDataActor* Data = GetRailwayData();
    if (!Data) return false;
    if (!ActivePlan.Sections.IsEmpty())
    {
        // 客户端使用已应用到车体的计划版本；新计划与运动快照可能分帧到达。
        if (!HasAuthority() && PresentedPlanRevision != ActivePlan.NetworkRevision) return false;
        double Local;
        int32 Section;
        if (!Railway::Geometry::Locate(Data->Network, ActivePlan, SectionEnds, Distance, Section, Local)) return false;
        OutLocation = {Data->Network.Edges[ActivePlan.Sections[Section].Leg.EdgeIndex].TrackIndex, Local};
        return true;
    }

    FRailwayLeg Preview;
    double Local = 0;
    if (!GetPreviewLocation(Preview, Local) || !Data->Network.Edges.IsValidIndex(Preview.EdgeIndex)) return false;
    const FRailwayEdge& Edge = Data->Network.Edges[Preview.EdgeIndex];
    FTransform Pose;
    OutLocation = {Edge.TrackIndex, Local};
    return Data->SampleTrack(OutLocation, Edge.IsReverse(), Pose) &&
        FVector::Dist(Pose.GetLocation(), GetActorLocation()) <= Railway::Routing::JunctionToleranceCm;
}

bool ARailwayTrain::ReplacePreparedRoute(const FRailwayPlan& Plan, FString& OutError, int32 SectionHint)
{
    if (!HasAuthority())
    {
        OutError = TEXT("Replacing a route requires authority");
        return false;
    }
    FRailwayTrackLocation Current;
    if (!GetCurrentTrackLocation(Current))
    {
        OutError = TEXT("Cannot locate the train on its occupied track");
        return false;
    }

    FRailwayPreparedPlan Prepared;
    if (!Railway::Routing::Prepare(GetRailwayData(), Plan, 0.0, Prepared, OutError)) return false;
    // 多圈重叠时，只有几何前缀相同才允许原行程里程用作圈次提示。
    TOptional<double> SharedPrefixDistance;
    if (SharesPrefixThrough(ActivePlan, Prepared.Plan, Leg) && Distance <= Prepared.SectionEnds[Leg])
        SharedPrefixDistance = Distance;
    double Mapped;
    if (!Railway::Geometry::FindPlanDistance(Prepared.Data->Network, Prepared.Plan, Prepared.SectionEnds, Current, SectionHint,
        SharedPrefixDistance, Mapped, OutError)) return false;
    if (Prepared.Plan.bFinishAtLastStop && FMath::IsNearlyEqual(Mapped, Prepared.Plan.Stops.Last().Distance, UE_SMALL_NUMBER))
        Mapped = Prepared.Plan.Stops.Last().Distance;
    if (!Prepared.SetStartDistance(Mapped, OutError)) return false;
    return ContinuePreparedPlan(MoveTemp(Prepared), true, OutError);
}

bool ARailwayTrain::ContinuePreparedPlan(FRailwayPreparedPlan&& Prepared, bool bMatchServedStops, FString& OutError)
{
    if (Prepared.StartPose.ContainsNaN() ||
        FVector::Dist(Prepared.StartPose.GetLocation(), GetActorLocation()) > Railway::Routing::JunctionToleranceCm)
    {
        OutError = TEXT("Replacement would move the train off its current position");
        return false;
    }
    const bool bDwelling = State == ERailwayState::Dwelling;
    if (NextStop < 0 || NextStop > ActivePlan.Stops.Num() ||
        (bDwelling && (!ActivePlan.Stops.IsValidIndex(NextStop - 1) || ActivePlan.Stops[NextStop - 1].Distance != Distance)))
    {
        OutError = TEXT("Invalid occupied station or stop cursor");
        return false;
    }

    const int32 Cursor = ContinueStopCursor(*this, Prepared, bMatchServedStops);
    if (bDwelling)
    {
        // 占用站可能由旧计划带入；提交前按新轨道重新核对它的来源、里程和世界位置。
        FRailwayPreparedPlan Checked;
        if (!Railway::Routing::Prepare(GetRailwayData(), MoveTemp(Prepared.Plan), Prepared.StartDistance, Checked, OutError)) return false;
        Prepared = MoveTemp(Checked);
    }
    const double Terminus = Prepared.Plan.bFinishAtLastStop ? Prepared.Plan.Stops.Last().Distance : Prepared.SectionEnds.Last();
    const bool bAtEnd = Prepared.StartDistance == Terminus;
    const bool bFinished = !bDwelling && bAtEnd && Cursor == Prepared.Plan.Stops.Num();

    // 新区间朝向相反时，同一世界运动方向在新计划中的速度符号相反。
    const FVector Forward = Prepared.StartPose.GetRotation().GetForwardVector();
    if (FVector::DotProduct(GetActorForwardVector(), Forward) < 0) Speed = -Speed;
    if (bFinished || (Prepared.StartDistance == 0 && Speed < 0) || (bAtEnd && Speed > 0)) Speed = 0.f;
    if (!bDwelling)
    {
        Wait = 0;
        ElapsedDwell = 0;
        bPreLeaveSent = false;
    }
    const ERailwayState NewState = bDwelling ? ERailwayState::Dwelling :
        (bFinished ? ERailwayState::Finished : ERailwayState::Running);
    ActivatePlan(MoveTemp(Prepared), Cursor, NewState);
    return true;
}
