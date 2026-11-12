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

// Only the final uninterrupted pass up to this seam is handed over. An earlier lap,
// a different branch, or a direct service ending at an intermediate station keeps its own cursor.
bool EndsAtNextLoopSeam(const ARailwayTrain& Train, const FRailwayNetworkData& N, const FRailwayEdge& First, double Local)
{
    if (Train.State == ERailwayState::Idle || !Train.ActivePlan.Sections.IsValidIndex(Train.Leg)) return false;
    const double Length = N.Tracks[First.TrackIndex].Length();
    const double Closing = First.IsReverse() ? 0.0 : Length;
    const double Remaining = Train.GetServiceEndDistance() - Train.Distance;
    if (!FMath::IsNearlyEqual(Remaining, FMath::Abs(Closing - Local), UE_KINDA_SMALL_NUMBER) &&
        !(Remaining == 0 && (Local == 0 || Local == Length))) return false;

    double Left = Remaining;
    for (int32 I = Train.Leg; I < Train.ActivePlan.Sections.Num(); ++I)
    {
        const auto& S = Train.ActivePlan.Sections[I];
        const auto& E = N.Edges[S.Leg.EdgeIndex];
        if (E.TrackIndex != First.TrackIndex || E.IsReverse() != First.IsReverse()) return false;
        Left -= S.TravelDistance - (I == Train.Leg ? FMath::Abs(Local - S.EntryDistance) : 0.);
        if (Left <= UE_KINDA_SMALL_NUMBER) return true;
    }
    return false;
}

bool JoinLoopEntry(const ARailwayTrain& Train, const FRailwayTrackLocation& Current, bool bKeepRemainingCalls,
    FRailwayPreparedPlan& Prepared, FString& Error)
{
    const auto& N = Prepared.Data->Network;
    auto& Plan = Prepared.Plan;
    auto& First = Plan.Sections[0];
    const auto& E = N.Edges[First.Leg.EdgeIndex];
    const auto& Track = N.Tracks[E.TrackIndex];
    const double Length = Track.Length();
    const double Closing = E.IsReverse() ? 0. : Length;
    if (Current.TrackDistance == Closing && First.EntryDistance != Closing)
    {
        const int32 LastForward = Track.FirstEdge + Track.NumEdges - 2;
        const int32 Incoming = E.IsReverse() ? Track.FirstEdge + 1 : LastForward;
        const int32 Outgoing = E.IsReverse() ? LastForward + 1 : Track.FirstEdge;
        if (!N.CanFollow(Incoming, Outgoing))
        {
            Error = TEXT("No through connection at the closed track seam; rebuild railway data");
            return false;
        }
    }
    const bool bSameSeam = FMath::Abs(Current.TrackDistance - First.EntryDistance) == Length;
    double Added = 0;
    if (!bSameSeam && Current.TrackDistance != First.EntryDistance)
    {
        FRailwayPlan Prefix;
        if (!Railway::Routing::AppendTrackPass(N, E.TrackIndex, E.IsReverse(), Current.TrackDistance,
            First.EntryDistance, false, Prefix, Error)) return false;
        for (const auto& S : Prefix.Sections) Added += S.TravelDistance;
        // The new entry may be in the middle of an edge. Only the first and last
        // intervals of a valid plan may be partial; merge the two pieces at that point.
        const auto& Tail = Prefix.Sections.Last();
        if (Tail.Leg.EdgeIndex == First.Leg.EdgeIndex && First.EntryDistance != E.StartDistance)
        {
            const double Exit = First.TravelDistance == FMath::Abs(E.EndDistance - First.EntryDistance) ?
                E.EndDistance : First.EntryDistance + E.Sign() * First.TravelDistance;
            First.EntryDistance = Tail.EntryDistance;
            First.TravelDistance = FMath::Abs(Exit - First.EntryDistance);
            Prefix.Sections.Pop();
        }
        Plan.Sections.Insert(Prefix.Sections, 0);
    }

    for (auto& Stop : Plan.Stops) Stop.Distance += Added;
    if (bKeepRemainingCalls)
    {
        TArray<FRailwayPlannedStop> Calls;
        for (int32 I = Train.NextStop; Train.ActivePlan.Stops.IsValidIndex(I); ++I)
        {
            auto Call = Train.ActivePlan.Stops[I];
            if (Call.Distance > Train.GetServiceEndDistance()) break;
            Call.Distance = FMath::Clamp(Call.Distance - Train.Distance, 0., Added);
            // Two timetables may both name a station at the handover point.
            if (Call.Distance == Added && Plan.Stops.ContainsByPredicate([&](const auto& S)
                { return S.Distance == Added && S.StationIndex == Call.StationIndex; })) continue;
            Calls.Add(Call);
        }
        Plan.Stops.Insert(Calls, 0);
    }
    return Railway::Routing::Prepare(Prepared.Data, MoveTemp(Plan), 0, Prepared, Error);
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
    const FVector Origin = GetActorLocation();
    if (ProjectionData.Get() == Data && ProjectionBuild == Data->Network.BuildId && ProjectionOrigin == Origin)
    {
        OutLocation = ProjectedLocation;
        return OutLocation.TrackIndex != INDEX_NONE;
    }
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
        if (OutLocation.TrackIndex != INDEX_NONE) { OutLocation = {}; break; }
        OutLocation = {I, D};
    }
    ProjectionData = Data;
    ProjectionBuild = Data->Network.BuildId;
    ProjectionOrigin = Origin;
    ProjectedLocation = OutLocation;
    return OutLocation.TrackIndex != INDEX_NONE;
}

bool ARailwayTrain::IsCurrentTrackLoop() const
{
    FRailwayTrackLocation At;
    const auto* Data = GetRailwayData();
    return Data && GetCurrentTrackLocation(At) && Data->Network.Tracks.IsValidIndex(At.TrackIndex) &&
        Data->Network.Tracks[At.TrackIndex].IsLoop();
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
    const auto& N = Prepared.Data->Network;
    double Mapped = 0;
    using Railway::Geometry::ELocationMatch;
    const auto Match = Railway::Geometry::FindPlanDistance(N, Prepared.Plan, Prepared.SectionEnds, Current, SectionHint,
        SharedPrefixDistance, Mapped, OutError);
    const auto& First = Prepared.Plan.Sections[0];
    const auto& E = N.Edges[First.Leg.EdgeIndex];
    if ((SectionHint == INDEX_NONE || SectionHint == 0) && E.TrackIndex == Current.TrackIndex && N.Tracks[E.TrackIndex].IsLoop())
    {
        const bool bAtTerminus = Distance == GetServiceEndDistance();
        const bool bEntryBehind = (First.EntryDistance - Current.TrackDistance) * E.Sign() < 0;
        const bool bFollowingService = Speed == 0 || (Speed > 0) != ActivePlan.bCabReversed;
        const bool bHandover = bFollowingService && (bEntryBehind || (bAtTerminus && First.EntryDistance == Current.TrackDistance)) &&
            EndsAtNextLoopSeam(*this, N, E, Current.TrackDistance);
        const bool bNextLap = bHandover && (SectionHint == INDEX_NONE || bAtTerminus);
        const bool bBeyondDestination = Match == ELocationMatch::Found && Prepared.Plan.bFinishAtLastStop &&
            Mapped > Prepared.Plan.Stops.Last().Distance + UE_SMALL_NUMBER;
        if (bNextLap || Match == ELocationMatch::Missing || (bBeyondDestination && SectionHint == INDEX_NONE))
        {
            if (!JoinLoopEntry(*this, Current, bHandover, Prepared, OutError)) return false;
            return ContinuePreparedPlan(MoveTemp(Prepared), true, OutError);
        }
    }
    if (Match != ELocationMatch::Found) return false;
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
    bool bFlipCab = FVector::DotProduct(GetActorForwardVector(), Prepared.StartPose.GetRotation().GetForwardVector()) < 0.0;
    const auto& N = Prepared.Data->Network;
    const auto& NewEdge = N.Edges[Prepared.Plan.Sections[Prepared.StartSection].Leg.EdgeIndex];
    int32 CabEdge;
    double Local;
    if (N.Tracks[NewEdge.TrackIndex].IsLoop() && GetCabEdge(CabEdge, Local) && N.Edges[CabEdge].TrackIndex == NewEdge.TrackIndex)
    {
        // At a linear seam the two tangents can form a right angle. The cab still faces
        // the same native spline direction when passing from Length to zero.
        const bool bShouldReverseCab = N.Edges[CabEdge].IsReverse() != NewEdge.IsReverse();
        bFlipCab = Prepared.Plan.bCabReversed != bShouldReverseCab;
    }
    if (bFlipCab)
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
