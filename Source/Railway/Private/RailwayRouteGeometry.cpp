#include "RailwayRouteGeometry.h"

#include "Algo/BinarySearch.h"

namespace Railway::Geometry
{
void BuildSectionEnds(const FRailwayPlan& Plan, TArray<double>& Ends)
{
    Ends.Reset(Plan.Sections.Num());
    double Total = 0;
    for (const FRailwaySection& Section : Plan.Sections)
    {
        Total += Section.TravelDistance;
        Ends.Add(Total);
    }
}

bool Locate(const FRailwayNetworkData& Data, const FRailwayPlan& Plan, const TArray<double>& Ends,
    double Distance, int32& SectionIndex, double& TrackDistance)
{
    SectionIndex = INDEX_NONE;
    TrackDistance = 0;
    if (Plan.BuildId != Data.BuildId || Ends.IsEmpty() || Ends.Num() != Plan.Sections.Num() ||
        !FMath::IsFinite(Distance) || Distance < 0 || Distance > Ends.Last()) return false;
    const int32 I = Distance == 0 ? 0 : FMath::Min(Algo::UpperBound(Ends, Distance), Ends.Num() - 1);
    const FRailwaySection& Section = Plan.Sections[I];
    if (!Data.Edges.IsValidIndex(Section.Leg.EdgeIndex)) return false;
    const FRailwayEdge& Edge = Data.Edges[Section.Leg.EdgeIndex];
    const double Along = Distance == Ends[I] ? Section.TravelDistance :
        FMath::Clamp(Distance - (I ? Ends[I - 1] : 0.0), 0.0, Section.TravelDistance);
    TrackDistance = Along == FMath::Abs(Edge.EndDistance - Section.EntryDistance) ? Edge.EndDistance :
        FMath::Clamp(Section.EntryDistance + Edge.Sign() * Along,
        FMath::Min(Edge.StartDistance, Edge.EndDistance), FMath::Max(Edge.StartDistance, Edge.EndDistance));
    SectionIndex = I;
    return true;
}

bool Sample(const ARailwayDataActor* Data, const FRailwayPlan& Plan, const TArray<double>& Ends,
    double Distance, int32& SectionIndex, double& TrackDistance, FTransform& Pose)
{
    Pose = FTransform::Identity;
    SectionIndex = INDEX_NONE;
    TrackDistance = 0;
    if (!Data || !Locate(Data->Network, Plan, Ends, Distance, SectionIndex, TrackDistance)) return false;
    const FRailwayEdge& Edge = Data->Network.Edges[Plan.Sections[SectionIndex].Leg.EdgeIndex];
    return Data->SampleTrack({Edge.TrackIndex, TrackDistance}, Edge.IsReverse() != Plan.bCabReversed, Pose);
}

ELocationMatch FindPlanDistance(const FRailwayNetworkData& Data, const FRailwayPlan& Plan, const TArray<double>& Ends,
    const FRailwayTrackLocation& Location, int32 SectionHint, TOptional<double> Preferred, double& OutDistance, FString& Error)
{
    if (SectionHint != INDEX_NONE && !Plan.Sections.IsValidIndex(SectionHint))
    {
        Error = TEXT("Invalid section hint");
        return ELocationMatch::AmbiguousOrInvalid;
    }
    TArray<double> Matches;
    for (int32 I = 0; I < Plan.Sections.Num(); ++I)
    {
        if (SectionHint != INDEX_NONE && I != SectionHint) continue;
        const FRailwaySection& Section = Plan.Sections[I];
        const FRailwayEdge& Edge = Data.Edges[Section.Leg.EdgeIndex];
        if (Edge.TrackIndex != Location.TrackIndex) continue;
        auto Match = [&](double Local)
        {
            const double Along = Edge.Sign() * (Local - Section.EntryDistance);
            if (Along >= 0 && Along <= Section.TravelDistance)
                Matches.AddUnique((I ? Ends[I - 1] : 0.0) + Along);
        };
        Match(Location.TrackDistance);
        const FRailwayBakedTrack& Track = Data.Tracks[Edge.TrackIndex];
        if (Track.IsLoop() && Location.TrackDistance == 0) Match(Track.Length());
        if (Track.IsLoop() && Location.TrackDistance == Track.Length()) Match(0);
    }
    if (Preferred.IsSet())
        for (double Match : Matches)
            if (FMath::IsNearlyEqual(Match, Preferred.GetValue(), UE_KINDA_SMALL_NUMBER))
            {
                OutDistance = Match;
                return ELocationMatch::Found;
            }
    if (Matches.Num() == 1 || Matches.Contains(0.0))
    {
        OutDistance = Matches.Contains(0.0) ? 0.0 : Matches[0];
        return ELocationMatch::Found;
    }
    Error = Matches.IsEmpty() ? TEXT("New route misses the occupied track location") : TEXT("Specify the repeated location's section hint");
    return Matches.IsEmpty() ? ELocationMatch::Missing : ELocationMatch::AmbiguousOrInvalid;
}
}
