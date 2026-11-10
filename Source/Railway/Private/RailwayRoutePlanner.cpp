#include "RailwayRoutePlanner.h"

#include "Algo/BinarySearch.h"
#include "RailwayRouteGeometry.h"

bool FRailwayPreparedPlan::SetStartDistance(double Distance, FString& Error)
{
    if (Plan.bFinishAtLastStop && (Plan.Stops.IsEmpty() || Distance > Plan.Stops.Last().Distance))
    {
        Error = TEXT("Direct service requires a destination ahead of the start");
        return false;
    }
    double Local;
    if (!Railway::Geometry::Sample(Data, Plan, SectionEnds, Distance, StartSection, Local, StartPose))
    {
        Error = TEXT("Start distance is outside the prepared route");
        return false;
    }
    StartDistance = Distance;
    return true;
}

namespace Railway::Routing
{
void BuildStops(const FRailwayNetworkData& Data, FRailwayPlan& Plan, bool bIntermediate, int32 Destination)
{
    Plan.Stops.Reset();
    double Total = 0;
    for (const FRailwaySection& Section : Plan.Sections)
    {
        if (bIntermediate)
        {
            const FRailwayEdge& Edge = Data.Edges[Section.Leg.EdgeIndex];
            const FRailwayBakedTrack& Track = Data.Tracks[Edge.TrackIndex];
            const double Exit = Section.EntryDistance + Edge.Sign() * Section.TravelDistance;
            const double Low = FMath::Min(Section.EntryDistance, Exit);
            const double High = FMath::Max(Section.EntryDistance, Exit);
            auto Rows = MakeArrayView(Data.Stations).Slice(Track.FirstStation, Track.NumStations);
            auto AddRange = [&](double Min, double Max, double Offset)
            {
                const int32 First = Algo::LowerBoundBy(Rows, Min,
                    [](const FRailwayBakedStation& S) { return S.Definition.TrackDistance; });
                for (int32 I = First; I < Rows.Num() && Rows[I].Definition.TrackDistance <= Max; ++I)
                {
                    const double Local = Rows[I].Definition.TrackDistance + Offset;
                    Plan.Stops.Add({Total + FMath::Abs(Local - Section.EntryDistance), Track.FirstStation + I});
                }
            };
            AddRange(Low, High, 0);
            if (Track.IsLoop() && High == Track.Length()) AddRange(0, 0, Track.Length());
        }
        Total += Section.TravelDistance;
    }
    Plan.Stops.Sort([](const FRailwayPlannedStop& A, const FRailwayPlannedStop& B)
    {
        return A.Distance == B.Distance ? A.StationIndex < B.StationIndex : A.Distance < B.Distance;
    });
    // A station at a shared section boundary is served once, including at the closed-loop seam.
    int32 Count = 0;
    for (const FRailwayPlannedStop& Stop : Plan.Stops)
    {
        if (Stop.StationIndex == Destination && Stop.Distance == Total) continue;
        if (Count && Plan.Stops[Count - 1].Distance == Stop.Distance && Plan.Stops[Count - 1].StationIndex == Stop.StationIndex) continue;
        Plan.Stops[Count++] = Stop;
    }
    Plan.Stops.SetNum(Count, EAllowShrinking::No);
    if (Destination != INDEX_NONE) Plan.Stops.Add({Total, Destination});
}

bool Build(const ARailwayDataActor* Data, const TArray<FRailwayLeg>& Legs, TOptional<double> FirstEntry,
    FRailwayPlan& OutPlan, FString& Error)
{
    OutPlan = {};
    Error.Reset();
    if (!Data || Legs.IsEmpty())
    {
        Error = TEXT("Build railway data and select at least one directed edge");
        return false;
    }
    FRailwayPlan Plan;
    Plan.BuildId = Data->Network.BuildId;
    int32 Previous = INDEX_NONE;
    for (const FRailwayLeg& Leg : Legs)
    {
        if (!Data->Network.Edges.IsValidIndex(Leg.EdgeIndex) ||
            (Previous != INDEX_NONE && !Data->Network.CanFollow(Previous, Leg.EdgeIndex)))
        {
            Error = FString::Printf(TEXT("Directed edge %d is missing or cannot follow %d"), Leg.EdgeIndex, Previous);
            return false;
        }
        const FRailwayEdge& Edge = Data->Network.Edges[Leg.EdgeIndex];
        const double Entry = Previous == INDEX_NONE && FirstEntry.IsSet() ? FirstEntry.GetValue() : Edge.StartDistance;
        if (!Edge.Contains(Entry))
        {
            Error = TEXT("Entry distance is outside the first edge");
            return false;
        }
        Plan.Sections.Add({Leg, Entry, FMath::Abs(Edge.EndDistance - Entry)});
        Previous = Leg.EdgeIndex;
    }
    BuildStops(Data->Network, Plan, true, INDEX_NONE);
    OutPlan = MoveTemp(Plan);
    return true;
}

static bool StopMatches(const FRailwayNetworkData& Data, const FRailwayPreparedPlan& Prepared, const FRailwayPlannedStop& Stop)
{
    if (!Data.Stations.IsValidIndex(Stop.StationIndex) || !FMath::IsFinite(Stop.Distance) ||
        Stop.Distance < 0 || Stop.Distance > Prepared.SectionEnds.Last()) return false;
    const FRailwayBakedStation& Station = Data.Stations[Stop.StationIndex];
    const int32 Host = Data.Edges[Station.EdgeIndex].TrackIndex;
    const double Local = Station.Definition.TrackDistance;
    const FRailwayBakedTrack& Track = Data.Tracks[Host];
    // At a boundary both adjacent sections (and any zero-length endpoint sections) own the stop.
    constexpr double Tol = UE_KINDA_SMALL_NUMBER;
    for (int32 I = Algo::LowerBound(Prepared.SectionEnds, Stop.Distance - Tol); I < Prepared.Plan.Sections.Num(); ++I)
    {
        const double Begin = I ? Prepared.SectionEnds[I - 1] : 0.0;
        if (Begin > Stop.Distance + Tol) break;
        const FRailwaySection& Section = Prepared.Plan.Sections[I];
        const FRailwayEdge& Edge = Data.Edges[Section.Leg.EdgeIndex];
        if (Edge.TrackIndex != Host) continue;
        const double At = Section.EntryDistance + Edge.Sign() * (Stop.Distance - Begin);
        if (FMath::Abs(At - Local) <= Tol || (Track.IsLoop() && Local == 0 && FMath::Abs(At - Track.Length()) <= Tol)) return true;
    }
    return false;
}

bool Prepare(const ARailwayDataActor* Data, FRailwayPlan Plan, double Start, FRailwayPreparedPlan& Prepared, FString& Error)
{
    Error.Reset();
    if (!Data || !Plan.BuildId.IsValid() || Data->Network.BuildId != Plan.BuildId || Plan.Sections.IsEmpty())
    {
        Error = TEXT("Route requires the same railway data build and at least one section");
        return false;
    }
    const auto& Network = Data->Network;
    for (int32 I = 0; I < Plan.Sections.Num(); ++I)
    {
        const FRailwaySection& S = Plan.Sections[I];
        if (!Network.Edges.IsValidIndex(S.Leg.EdgeIndex))
        {
            Error = TEXT("Route contains an unknown directed edge");
            return false;
        }
        const FRailwayEdge& E = Network.Edges[S.Leg.EdgeIndex];
        const double Remaining = FMath::Abs(E.EndDistance - S.EntryDistance);
        if (!E.Contains(S.EntryDistance) || !FMath::IsFinite(S.TravelDistance) ||
            S.TravelDistance < 0 || S.TravelDistance > Remaining ||
            (I > 0 && (S.EntryDistance != E.StartDistance || !Network.CanFollow(Plan.Sections[I - 1].Leg.EdgeIndex, S.Leg.EdgeIndex))) ||
            (I + 1 < Plan.Sections.Num() && S.TravelDistance != Remaining))
        {
            Error = FString::Printf(TEXT("Invalid interval or illegal junction transition at section %d"), I);
            return false;
        }
    }
    FRailwayPreparedPlan New;
    New.Data = Data;
    New.Plan = MoveTemp(Plan);
    Geometry::BuildSectionEnds(New.Plan, New.SectionEnds);
    double Previous = -1;
    for (const FRailwayPlannedStop& Stop : New.Plan.Stops)
    {
        if (Stop.Distance < Previous || !StopMatches(Network, New, Stop))
        {
            Error = TEXT("Stop is out of order or off its baked track");
            return false;
        }
        Previous = Stop.Distance;
    }
    if (!New.SetStartDistance(Start, Error)) return false;
    Prepared = MoveTemp(New);
    return true;
}
}
