#include "RailwayDataActor.h"

#include "GraphAStar.h"
#include "RailwayRoutePlanner.h"

namespace
{
struct FEdgePoint
{
    int32 Edge;
    double Distance;
};

using FEdgePoints = TArray<FEdgePoint, TInlineAllocator<4>>;

bool CollectEdges(const FRailwayNetworkData& Data, FRailwayTrackLocation Location, FEdgePoints& Points)
{
    if (!Data.Tracks.IsValidIndex(Location.TrackIndex) || !FMath::IsFinite(Location.TrackDistance)) return false;
    const FRailwayBakedTrack& Track = Data.Tracks[Location.TrackIndex];
    if (Location.TrackDistance < 0 || Location.TrackDistance > Track.Length()) return false;
    const bool bSeam = Track.IsLoop() && (Location.TrackDistance == 0 || Location.TrackDistance == Track.Length());
    for (int32 I = Track.FirstEdge; I < Track.FirstEdge + Track.NumEdges; ++I)
    {
        const FRailwayEdge& E = Data.Edges[I];
        if (E.Contains(Location.TrackDistance)) Points.Add({I, Location.TrackDistance});
        if (bSeam)
        {
            const double Other = Location.TrackDistance == 0 ? Track.Length() : 0.0;
            if (E.Contains(Other)) Points.Add({I, Other});
        }
    }
    return !Points.IsEmpty();
}

// Real nodes mean "enter this directed edge". Query-local nodes start part-way along
// one eligible edge, so returning to that edge later has the normal full-edge cost.
struct FRailwaySearch
{
    using FNodeRef = int32;

    const FRailwayNetworkData& Data;
    const FEdgePoints& Starts;
    const FEdgePoints& Goals;

    int32 Source() const { return Data.Edges.Num() + Starts.Num(); }
    int32 Sink() const { return Source() + 1; }
    bool IsValidRef(int32 N) const { return N >= 0 && N <= Sink(); }

    FEdgePoint Entry(int32 N) const
    {
        if (N >= Data.Edges.Num()) return Starts[N - Data.Edges.Num()];
        return {N, Data.Edges[N].StartDistance};
    }

    TOptional<double> DistanceToGoal(FEdgePoint P) const
    {
        TOptional<double> Best;
        for (FEdgePoint G : Goals)
        {
            if (G.Edge != P.Edge) continue;
            const double Along = (G.Distance - P.Distance) * Data.Edges[P.Edge].Sign();
            if (Along >= 0 && (!Best.IsSet() || Along < Best.GetValue())) Best = Along;
        }
        return Best;
    }

    int32 GetNeighbourCount(int32 N) const
    {
        if (N == Source()) return Starts.Num();
        if (N == Sink()) return 0;
        return Data.Edges[Entry(N).Edge].NumSuccessors + 1;
    }

    int32 GetNeighbour(int32 N, int32 I) const
    {
        if (N == Source()) return Data.Edges.Num() + I;
        const FEdgePoint P = Entry(N);
        const FRailwayEdge& E = Data.Edges[P.Edge];
        if (I < E.NumSuccessors) return Data.Successors[E.FirstSuccessor + I];
        return DistanceToGoal(P).IsSet() ? Sink() : INDEX_NONE;
    }

    bool IsTraversalAllowed(int32, int32) const { return true; }
    bool WantsPartialSolution() const { return false; }
    double GetHeuristicScale() const { return 1.0; }

    double GetTraversalCost(int32 A, int32 B) const
    {
        if (A == Source()) return 0;
        const FEdgePoint P = Entry(A);
        return B == Sink() ? DistanceToGoal(P).GetValue() : FMath::Abs(Data.Edges[P.Edge].EndDistance - P.Distance);
    }

    double Estimate(FEdgePoint P) const
    {
        if (const auto Direct = DistanceToGoal(P); Direct.IsSet()) return Direct.GetValue();
        const FRailwayEdge& E = Data.Edges[P.Edge];
        double Best = TNumericLimits<double>::Max();
        for (const FEdgePoint G : Goals)
        {
            const FRailwayEdge& GoalEdge = Data.Edges[G.Edge];
            const double Chord = FVector::Dist(Data.Nodes[E.ToNode], Data.Nodes[GoalEdge.FromNode]);
            Best = FMath::Min(Best, Chord * Data.MinCostPerCm + FMath::Abs(G.Distance - GoalEdge.StartDistance));
        }
        return FMath::Abs(E.EndDistance - P.Distance) + Best;
    }

    double GetHeuristicCost(int32 N, int32) const
    {
        if (N == Sink()) return 0;
        if (N != Source()) return Estimate(Entry(N));
        double Best = TNumericLimits<double>::Max();
        for (const FEdgePoint P : Starts) Best = FMath::Min(Best, Estimate(P));
        return Best;
    }
};

bool FindRailwayPath(const FRailwayNetworkData& Data, FRailwayTrackLocation Start, FRailwayTrackLocation Destination,
    int32 StationIndex, bool bIntermediate, FRailwayPlan& OutPlan, FString& Error)
{
    OutPlan = {};
    Error.Reset();
    FEdgePoints Starts, Goals;
    if (!CollectEdges(Data, Start, Starts) || !CollectEdges(Data, Destination, Goals))
    {
        Error = TEXT("Select a valid baked track and distance for both endpoints");
        return false;
    }
    const FRailwaySearch Graph{Data, Starts, Goals};
    FGraphAStar<FRailwaySearch> Search(Graph);
    TArray<int32> Path;
    const auto Result = Search.FindPath(Graph.Source(), Graph.Sink(), Graph, Path);
    if (Result != EGraphAStarResult::SearchSuccess || Path.Num() < 2 || Path.Last() != Graph.Sink())
    {
        Error = TEXT("No route through the permitted junction transitions");
        return false;
    }
    OutPlan.BuildId = Data.BuildId;
    for (int32 I = 0; I + 1 < Path.Num(); ++I)
    {
        const FEdgePoint P = Graph.Entry(Path[I]);
        OutPlan.Sections.Add({{P.Edge}, P.Distance, Graph.GetTraversalCost(Path[I], Path[I + 1])});
    }
    Railway::Routing::BuildStops(Data, OutPlan, bIntermediate, StationIndex);
    return true;
}
}

bool ARailwayDataActor::FindRoutePlan(FRailwayTrackLocation Start, FRailwayTrackLocation Destination,
    FRailwayPlan& OutPlan, FString& OutError, bool bStopAtIntermediateStations) const
{
    return FindRailwayPath(Network, Start, Destination, INDEX_NONE, bStopAtIntermediateStations, OutPlan, OutError);
}

bool ARailwayDataActor::FindRoutePlanToStation(FRailwayTrackLocation Start, int32 StationIndex,
    FRailwayPlan& OutPlan, FString& OutError, bool bStopAtIntermediateStations) const
{
    if (!Network.Stations.IsValidIndex(StationIndex))
    {
        OutPlan = {};
        OutError = TEXT("Invalid baked station index");
        return false;
    }
    const FRailwayBakedStation& Station = Network.Stations[StationIndex];
    return FindRailwayPath(Network, Start, {Network.Edges[Station.EdgeIndex].TrackIndex, Station.Definition.TrackDistance},
        StationIndex, bStopAtIntermediateStations, OutPlan, OutError);
}
