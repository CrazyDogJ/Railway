#include "RailwayPathfinding.h"

#include "Algo/Reverse.h"
#include "Components/SplineComponent.h"
#include "RailwayRouteGeometry.h"
#include "RailwayRoutePlanner.h"
#include "RailwayTrack.h"
#include "RailwayWorldIndex.h"

namespace Railway::Routing
{
namespace
{
// 每个节点属于一条轨道的一个里程。同一岔口上的不同轨道仍有各自的采样姿态。
struct FEdge
{
    int32 To;
    double SignedLength; // 正负为样条方向，零为换轨。
};

struct FTrackNode
{
    FRailwayTrackLocation Location;
    FVector WorldPosition;
    TArray<FEdge> Edges;
};

struct FSearchRecord
{
    double Cost = TNumericLimits<double>::Max();
    int32 Parent = INDEX_NONE;
    double IncomingLength = 0; // 闭环两节点间可能有长短两条边，必须记住选中的那条。
};

struct FOpenEntry
{
    int32 Node;
    double Cost;
    double Estimate;

    bool operator<(const FOpenEntry& Other) const
    {
        if (Estimate != Other.Estimate) return Estimate < Other.Estimate;
        return Node < Other.Node;
    }
};

class FTrackGraph
{
public:
    void Build(UWorld* World, const FRailwayWorldIndex& Index, const FRailwayTrackLocation& Start,
        const FRailwayTrackLocation& Goal, int32& StartNode, int32& GoalNode);
    bool FindSections(int32 Start, int32 Goal, TArray<FRailwaySection>& OutSections) const;

private:
    int32 AddCut(ARailwayTrack* Track, double Distance);
    void Link(int32 From, int32 To, double SignedLength);
    void Reconstruct(int32 Goal, const TArray<FSearchRecord>& Records, TArray<FRailwaySection>& Sections) const;

    TArray<FTrackNode> Nodes;
    TMap<ARailwayTrack*, TMap<float, int32>> Cuts;
    double HeuristicSlack = 0;
};

int32 FTrackGraph::AddCut(ARailwayTrack* Track, double Distance)
{
    const USplineComponent& Spline = *Track->Spline;
    float Local = Geometry::DistanceOnSpline(Spline, Distance);
    if (Local == 0.f || (Spline.IsClosedLoop() && Local == Spline.GetSplineLength())) Local = 0.f;
    TMap<float, int32>& TrackCuts = Cuts.FindChecked(Track);
    if (const int32* Existing = TrackCuts.Find(Local)) return *Existing;
    const int32 NodeIndex = Nodes.AddDefaulted();
    FTrackNode& Node = Nodes[NodeIndex];
    Node.Location = FRailwayTrackLocation{Track, Local};
    Node.WorldPosition = Spline.GetLocationAtDistanceAlongSpline(Local, ESplineCoordinateSpace::World);
    TrackCuts.Add(Local, NodeIndex);
    return NodeIndex;
}

void FTrackGraph::Link(int32 From, int32 To, double SignedLength)
{
    if (From == To) return;
    const double Chord = FVector::Dist(Nodes[From].WorldPosition, Nodes[To].WorldPosition);
    if (!FMath::IsFinite(Chord)) return;
    Nodes[From].Edges.Add(FEdge{To, SignedLength});
    Nodes[To].Edges.Add(FEdge{From, -SignedLength});
    // 零成本换轨允许小幅吸附误差，样条重参数化也有数值误差。
    // 最短简单路径至多经过每条边一次，扣除所有正差后，直线距离仍是行驶成本的下界。
    HeuristicSlack += FMath::Max(0.0, Chord - FMath::Abs(SignedLength));
}

void FTrackGraph::Build(UWorld* World, const FRailwayWorldIndex& Index, const FRailwayTrackLocation& Start,
    const FRailwayTrackLocation& Goal, int32& StartNode, int32& GoalNode)
{
    const TArray<ARailwayTrack*> Tracks = Index.GetTracks();
    for (ARailwayTrack* Track : Tracks)
    {
        if (CheckRoutingTrack(Track, World, Index)) continue;
        Cuts.Add(Track);
        AddCut(Track, 0.0);
        if (!Track->Spline->IsClosedLoop()) AddCut(Track, Track->Spline->GetSplineLength());
    }
    StartNode = AddCut(Start.Track, Start.TrackDistance);
    GoalNode = AddCut(Goal.Track, Goal.TrackDistance);

    TMap<ARailwayJunction*, TArray<int32>> JunctionPorts;
    auto AddEndpoint = [&](ARailwayTrack* Track, ARailwayJunction* Junction, double Distance)
    {
        if (!IsValid(Junction)) return;
        const int32 Node = AddCut(Track, Distance);
        if (FVector::Dist(Nodes[Node].WorldPosition, Junction->GetActorLocation()) <= JunctionToleranceCm)
            JunctionPorts.FindOrAdd(Junction).AddUnique(Node);
    };
    for (ARailwayTrack* Track : Tracks)
    {
        if (!Cuts.Contains(Track) || Track->Spline->IsClosedLoop()) continue;
        AddEndpoint(Track, Track->StartJunction, 0.0);
        AddEndpoint(Track, Track->EndJunction, Track->Spline->GetSplineLength());
    }

    for (auto& Pair : JunctionPorts)
    {
        ARailwayJunction& Junction = *Pair.Key;
        TArray<int32>& Ports = Pair.Value;
        if (Cuts.Contains(Junction.HostTrack.Get()) && FMath::IsFinite(Junction.TrackDistance))
        {
            const int32 Host = AddCut(Junction.HostTrack, Junction.TrackDistance);
            if (FVector::Dist(Nodes[Host].WorldPosition, Junction.GetActorLocation()) <= JunctionToleranceCm)
                Ports.AddUnique(Host);
        }
        for (int32 A = 0; A < Ports.Num(); ++A)
        {
            for (int32 B = A + 1; B < Ports.Num(); ++B)
            {
                if (FVector::Dist(Nodes[Ports[A]].WorldPosition, Nodes[Ports[B]].WorldPosition) <= JunctionToleranceCm)
                    Link(Ports[A], Ports[B], 0.0);
            }
        }
    }

    for (const auto& Pair : Cuts)
    {
        TArray<int32> Ordered;
        Pair.Value.GenerateValueArray(Ordered);
        Ordered.Sort([this](int32 A, int32 B) { return Nodes[A].Location.TrackDistance < Nodes[B].Location.TrackDistance; });
        for (int32 I = 1; I < Ordered.Num(); ++I)
            Link(Ordered[I - 1], Ordered[I], Nodes[Ordered[I]].Location.TrackDistance - Nodes[Ordered[I - 1]].Location.TrackDistance);
        if (Pair.Key->Spline->IsClosedLoop() && Ordered.Num() > 1)
        {
            const double Wrap = Pair.Key->Spline->GetSplineLength() - Nodes[Ordered.Last()].Location.TrackDistance
                + Nodes[Ordered[0]].Location.TrackDistance;
            Link(Ordered.Last(), Ordered[0], Wrap);
        }
    }
}

bool FTrackGraph::FindSections(int32 Start, int32 Goal, TArray<FRailwaySection>& OutSections) const
{
    const auto Heuristic = [&](int32 Node)
    {
        return FMath::Max(0.0, FVector::Dist(Nodes[Node].WorldPosition, Nodes[Goal].WorldPosition) - HeuristicSlack);
    };

    TArray<FSearchRecord> Records;
    Records.SetNum(Nodes.Num());
    Records[Start].Cost = 0;
    TArray<FOpenEntry> Open;
    Open.HeapPush(FOpenEntry{Start, 0.0, Heuristic(Start)});
    while (!Open.IsEmpty())
    {
        FOpenEntry Current;
        Open.HeapPop(Current, EAllowShrinking::No);
        if (Current.Cost != Records[Current.Node].Cost) continue;
        if (Current.Node == Goal)
        {
            Reconstruct(Goal, Records, OutSections);
            return true;
        }
        for (const FEdge& Edge : Nodes[Current.Node].Edges)
        {
            const double NewCost = Current.Cost + FMath::Abs(Edge.SignedLength);
            FSearchRecord& Next = Records[Edge.To];
            if (NewCost >= Next.Cost) continue;
            Next = FSearchRecord{NewCost, Current.Node, Edge.SignedLength};
            Open.HeapPush(FOpenEntry{Edge.To, NewCost, NewCost + Heuristic(Edge.To)});
        }
    }
    return false;
}

void FTrackGraph::Reconstruct(int32 Goal, const TArray<FSearchRecord>& Records, TArray<FRailwaySection>& Sections) const
{
    TArray<int32> Path;
    for (int32 Node = Goal; Node != INDEX_NONE; Node = Records[Node].Parent) Path.Add(Node);
    Algo::Reverse(Path);

    auto AddSection = [&](int32 Node)
    {
        const FRailwayTrackLocation& Location = Nodes[Node].Location;
        FRailwaySection& Section = Sections.AddDefaulted_GetRef();
        Section.Leg.Track = Location.Track;
        Section.EntryDistance = Location.TrackDistance;
        Section.TrackId = Location.Track->RailwayId;
        Section.LayoutSignature = Location.Track->GetLayoutSignature();
    };
    AddSection(Path[0]);
    for (int32 I = 1; I < Path.Num(); ++I)
    {
        const double Travel = Records[Path[I]].IncomingLength;
        if (Travel == 0.0)
        {
            AddSection(Path[I]);
            continue;
        }
        const bool bReverse = Travel < 0;
        if (Sections.Last().TravelDistance > 0 && Sections.Last().Leg.bReverse != bReverse) AddSection(Path[I - 1]);
        FRailwaySection& Section = Sections.Last();
        Section.Leg.bReverse = bReverse;
        Section.TravelDistance += FMath::Abs(Travel);
    }

    // 同点或只换轨即可到达时，只保留目标点，不制造虚假的移动或额外绕行。
    const int32 FirstTravel = Sections.IndexOfByPredicate([](const FRailwaySection& S) { return S.TravelDistance > 0; });
    if (FirstTravel == INDEX_NONE)
    {
        Sections.Reset();
        AddSection(Goal);
        return;
    }

    // 零长度边界区间也需要车体朝向。选与紧邻行驶区间最接近的轨道方向。
    auto FaceAlong = [](FRailwaySection& Point, const FVector& Forward)
    {
        const FVector Tangent = Geometry::SampleTrack(Point.Leg, Point.EntryDistance).GetRotation().GetForwardVector();
        Point.Leg.bReverse = FVector::DotProduct(Tangent, Forward) < 0;
    };
    for (int32 I = FirstTravel - 1; I >= 0; --I)
    {
        const FRailwaySection& Next = Sections[I + 1];
        FaceAlong(Sections[I], Geometry::SampleTrack(Next.Leg, Next.EntryDistance).GetRotation().GetForwardVector());
    }
    for (int32 I = FirstTravel + 1; I < Sections.Num(); ++I)
    {
        if (Sections[I].TravelDistance > 0) continue;
        const FRailwaySection& Prev = Sections[I - 1];
        const double Exit = Prev.EntryDistance + (Prev.Leg.bReverse ? -Prev.TravelDistance : Prev.TravelDistance);
        FaceAlong(Sections[I], Geometry::SampleTrack(Prev.Leg, Exit).GetRotation().GetForwardVector());
    }
}

bool CheckEndpoint(const FRailwayTrackLocation& Location, const TCHAR* Label, UWorld* World,
    const FRailwayWorldIndex& Index, FString& Error)
{
    if (const TCHAR* Reason = CheckRoutingTrack(Location.Track, World, Index))
    {
        Error = FString::Printf(TEXT("%s: %s"), Label, Reason);
        return false;
    }
    if (!FMath::IsFinite(Location.TrackDistance) || Location.TrackDistance < 0 ||
        Location.TrackDistance > Location.Track->Spline->GetSplineLength())
    {
        Error = FString::Printf(TEXT("%s distance is outside track %s"), Label, *Location.Track->GetName());
        return false;
    }
    const FVector Point = Location.Track->Spline->GetLocationAtDistanceAlongSpline(
        Location.TrackDistance, ESplineCoordinateSpace::World);
    if (Point.ContainsNaN())
    {
        Error = FString::Printf(TEXT("%s does not have a finite world position"), Label);
        return false;
    }
    return true;
}

bool Search(UWorld* World, const FRailwayWorldIndex& Index, const FRailwayTrackLocation& Start,
    const FRailwayTrackLocation& Destination, bool bStopAtIntermediateStations, const FRailwayStationRef* DestinationStation,
    FRailwayPlan& OutPlan, FString& Error)
{
    if (!CheckEndpoint(Start, TEXT("Start"), World, Index, Error) ||
        !CheckEndpoint(Destination, TEXT("Destination"), World, Index, Error)) return false;

    FTrackGraph Graph;
    int32 From, To;
    Graph.Build(World, Index, Start, Destination, From, To);
    FRailwayPlan Plan;
    if (!Graph.FindSections(From, To, Plan.Sections))
    {
        Error = TEXT("No path between these locations on loaded tracks. Check junction references and endpoint snapping");
        return false;
    }
    if (!BuildPathStops(Index, bStopAtIntermediateStations, DestinationStation, Plan, Error)) return false;
    OutPlan = MoveTemp(Plan);
    return true;
}
}

bool FindPath(UWorld* World, const FRailwayTrackLocation& Start, const FRailwayTrackLocation& Destination,
    bool bStopAtIntermediateStations, FRailwayPlan& OutPlan, FString& OutError)
{
    OutPlan = FRailwayPlan();
    OutError.Reset();
    const FRailwayWorldIndex Index(World);
    return Search(World, Index, Start, Destination, bStopAtIntermediateStations, nullptr, OutPlan, OutError);
}

bool FindPathToStation(UWorld* World, const FRailwayTrackLocation& Start, ARailwayTrack* HostTrack,
    ERailwayStationSource Source, FGuid StationId, bool bStopAtIntermediateStations, FRailwayPlan& OutPlan, FString& OutError)
{
    OutPlan = FRailwayPlan();
    OutError.Reset();
    const FRailwayWorldIndex Index(World);
    if (!StationId.IsValid() || !IsValid(HostTrack) || HostTrack->GetWorld() != World || !IsValid(HostTrack->Spline))
    {
        OutError = TEXT("A destination station with a valid ID and host track in this world is required");
        return false;
    }
    FRailwayPlannedStop DestinationStop;
    if (!Index.VisitStations(*HostTrack, [&](const FRailwayStation&, const FRailwayStationRef& Ref)
    {
        if (Ref.Source == Source && Ref.StationId == StationId) DestinationStop.Reference = Ref;
        return true;
    }, OutError)) return false;
    const FRailwayStationRef& Ref = DestinationStop.Reference;
    if (Ref.Source != Source || Ref.StationId != StationId || !FMath::IsFinite(Ref.PlannedTrackDistance) ||
        Ref.PlannedTrackDistance < 0)
    {
        OutError = FString::Printf(TEXT("Destination station %s is missing or invalid on its host track"), *StationId.ToString());
        return false;
    }
    if (!Index.BindStop(DestinationStop, OutError)) return false;
    const FRailwayTrackLocation Destination{HostTrack, Geometry::DistanceOnSpline(*HostTrack->Spline, Ref.PlannedTrackDistance)};
    return Search(World, Index, Start, Destination, bStopAtIntermediateStations, &Ref, OutPlan, OutError);
}
}
