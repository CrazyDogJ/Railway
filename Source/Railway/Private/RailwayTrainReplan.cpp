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
    for (FRailwayPlannedStop& Stop : Stops) Stop.bServed = false;
    if (bMatchServed && Train.Distance == New.StartDistance && SameItinerary(Train.ActivePlan, New.Plan))
    {
        for (int32 I = 0; I < Train.NextStop; ++I) Stops[I].bServed = Train.ActivePlan.Stops[I].bServed;
        return Train.NextStop;
    } // 手动倒车也不会让同一份时刻表重放已服务站次。

    // Carry recorded arrivals through the unchanged prefix. Routes that start at the
    // current location have their own visit list; their occupied stop is handled below.
    if (bMatchServed && Train.Distance == New.StartDistance && SharesPrefixThrough(Train.ActivePlan, New.Plan, Train.Leg))
    {
        for (int32 I = 0; I < Train.NextStop; ++I)
        {
            const FRailwayPlannedStop& Old = Train.ActivePlan.Stops[I];
            if (!Old.bServed || Old.Distance >= Train.Distance - Tol) continue;
            for (int32 J = Algo::LowerBoundBy(Stops, Old.Distance, &FRailwayPlannedStop::Distance);
                J < Stops.Num() && Stops[J].Distance == Old.Distance; ++J)
            {
                if (!Stops[J].bServed && SameStop(Old, Stops[J])) { Stops[J].bServed = true; break; }
            }
        }
    }
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
        Served.bServed = Old.bServed;
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
    if (GetPreviewLocation(Preview, Local) && Data->Network.Edges.IsValidIndex(Preview.EdgeIndex))
    {
        const FRailwayEdge& Edge = Data->Network.Edges[Preview.EdgeIndex];
        FTransform Pose;
        if (Data->SampleTrack({Edge.TrackIndex, Local}, Edge.IsReverse(), Pose) &&
            FVector::Dist(Pose.GetLocation(), GetActorLocation()) <= Railway::Routing::JunctionToleranceCm)
        { OutLocation = {Edge.TrackIndex, Local}; return true; }
    }

    // A manually placed train can start without a timetable. At intersecting tracks
    // require an unambiguous match; an explicit preview above disambiguates the crossing.
    for (int32 I = 0; I < Data->Network.Tracks.Num(); ++I)
    {
        const auto& Track = Data->Network.Tracks[I];
        float Sq;
        const float Key = Track.Curves.Position.FindNearest(Track.SplineToWorld.InverseTransformPosition(GetActorLocation()), Sq);
        const auto& Table = Track.Curves.ReparamTable.Points;
        if (Table.IsEmpty()) continue;
        const int32 Upper = Algo::LowerBoundBy(Table, Key, [](const auto& P){ return P.OutVal; });
        double D = Track.Length();
        if (Upper == 0) D = 0;
        else if (Upper < Table.Num())
        {
            const auto& A = Table[Upper - 1]; const auto& B = Table[Upper];
            D = B.OutVal > A.OutVal ? FMath::Lerp(A.InVal, B.InVal, (Key - A.OutVal) / (B.OutVal - A.OutVal)) : A.InVal;
        }
        if (FVector::Dist(Track.PositionAt(D), GetActorLocation()) > Railway::Routing::JunctionToleranceCm) continue;
        if (OutLocation.TrackIndex != INDEX_NONE) { OutLocation = {}; return false; }
        OutLocation = {I, D};
    }
    return OutLocation.TrackIndex != INDEX_NONE;
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

    // Choose the cab end once for the new route. Never rotate the body to reverse travel.
    if (FVector::DotProduct(GetActorForwardVector(), Prepared.StartPose.GetRotation().GetForwardVector()) < 0.0)
    {
        Prepared.Plan.bCabReversed = !Prepared.Plan.bCabReversed;
        if (!Prepared.SetStartDistance(Prepared.StartDistance, OutError)) return false;
    }
    const double PathSpeed = Prepared.Plan.bCabReversed ? -Speed : Speed;
    if (PathSpeed < 0 && !AddBrakingRoom(Prepared, PathSpeed * PathSpeed / (2.0 * FMath::Max(1.f, Braking)), OutError)) return false;
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

    if (bFinished) Speed = 0.f;
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
