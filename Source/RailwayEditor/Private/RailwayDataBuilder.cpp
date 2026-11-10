#include "RailwayDataBuilder.h"

#include "Engine/Level.h"
#include "Engine/World.h"
#include "RailwayDataActor.h"
#include "RailwayTrack.h"
#include "ScopedTransaction.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionHandle.h"
#include "WorldPartition/WorldPartitionHelpers.h"

namespace Railway::Editor
{
namespace
{
// Hold references until after the bake. Also collect Level.Actors below for newly placed,
// unsaved sources that do not yet have an actor descriptor.
bool LoadSources(ULevel& Level, TArray<FWorldPartitionReference>& Pins, FString& Error)
{
    UWorldPartition* Partition = Level.GetWorldPartition();
    if (!Partition) return true;
    for (UClass* Class : {ARailwayNetworkActor::StaticClass(), ARailwayDataActor::StaticClass()})
    {
        FWorldPartitionHelpers::ForEachActorDescInstance(Partition, Class, [&](const FWorldPartitionActorDescInstance* Desc)
        {
            // A descriptor's GUID is an editor loading handle only. It is never baked into the graph.
            const FWorldPartitionReference& Pin = Pins.Emplace_GetRef(Desc->GetContainerInstance(), Desc->GetGuid());
            if (!IsValid(Pin.GetActor()))
            {
                Error = FString::Printf(TEXT("Could not load railway source %s"), *Desc->GetGuid().ToString());
                return false;
            }
            return true;
        });
        if (!Error.IsEmpty()) return false;
    }
    return true;
}

// Preserve the component's actual distance-to-input-key mapping, including non-unit
// component scale and custom loop keys. UE 5.8's tangent-spline conversion builds a
// default reparam table. Recover its original knots from the authoritative component.
bool BakeSpline(const ARailwayTrack& Source, FRailwayBakedTrack& Track)
{
    const USplineComponent* S = Source.Spline;
    if (!IsValid(S) || S->GetNumberOfSplinePoints() < 2 || S->ReparamStepsPerSegment < 1) return false;
    const float Length = S->GetSplineLength();
    if (!FMath::IsFinite(Length) || Length <= 0) return false;

    Track.Name = FName(*Source.GetActorLabel());
    Track.SplineToWorld = S->GetComponentTransform();
    Track.DefaultUp = S->GetDefaultUpVector(ESplineCoordinateSpace::Local);
    if (Track.SplineToWorld.ContainsNaN() || Track.DefaultUp.ContainsNaN()) return false;
    FSplineCurves& Curves = Track.Curves;
    // When the component owns FSplineCurves, every knot and tangent can be copied verbatim.
    if (&S->GetSplinePointsPosition() == &S->SplineCurves.Position)
    {
        Curves = S->GetSplineCurves();
        Curves.Metadata_DEPRECATED = nullptr;
        return Curves.ReparamTable.Points.Num() >= 2;
    }
    Curves.Position = S->GetSplinePointsPosition();
    Curves.Rotation = S->GetSplinePointsRotation();
    Curves.Scale = S->GetSplinePointsScale();
    // The converted interp curves use point indices as keys. Map native segment
    // parameters back into that domain, including the native closing interval.
    const float LastCurveKey = Curves.Position.Points.IsEmpty() ? 0.f : Curves.Position.Points.Last().InVal;
    if (S->IsClosedLoop())
    {
        const float LoopWidth = S->GetInputKeyValueAtSplinePoint(S->GetNumberOfSplinePoints()) -
            S->GetInputKeyValueAtSplinePoint(S->GetNumberOfSplinePoints() - 1);
        Curves.Position.SetLoopKey(LastCurveKey + LoopWidth);
        Curves.Rotation.SetLoopKey(LastCurveKey + LoopWidth);
        Curves.Scale.SetLoopKey(LastCurveKey + LoopWidth);
    }

    const int32 Segments = S->GetNumberOfSplineSegments();
    if (Curves.Position.Points.Num() != S->GetNumberOfSplinePoints() ||
        Curves.Scale.Points.Num() != Curves.Position.Points.Num() || Curves.Rotation.Points.Num() != Curves.Position.Points.Num())
        return false;
    for (int32 Segment = 0; Segment < Segments; ++Segment)
    {
        const float Begin = S->GetDistanceAlongSplineAtSplinePoint(Segment);
        const float End = S->GetDistanceAlongSplineAtSplinePoint(Segment + 1);
        if (!(End > Begin) || !FMath::IsFinite(End)) return false;
        const float FirstKey = S->GetInputKeyValueAtDistanceAlongSpline(Begin);
        const float LastKey = S->GetInputKeyValueAtDistanceAlongSpline(End);
        if (!(LastKey > FirstKey)) return false;
        const float CurveA = Curves.Position.Points[Segment].InVal;
        const float CurveB = Segment + 1 < Curves.Position.Points.Num() ? Curves.Position.Points[Segment + 1].InVal :
            LastCurveKey + Curves.Position.LoopKeyOffset;
        for (int32 Step = 0; Step < S->ReparamStepsPerSegment; ++Step)
        {
            const float Fraction = static_cast<float>(Step) / S->ReparamStepsPerSegment;
            const float Key = FMath::Lerp(FirstKey, LastKey, Fraction);
            float Low = Begin, High = End;
            if (Step)
            {
                // Every knot is at a uniform fraction of the segment's parameter interval.
                for (int32 I = 0; I < 32; ++I)
                {
                    const float Mid = (Low + High) * .5f;
                    if (Mid == Low || Mid == High) break;
                    if (S->GetInputKeyValueAtDistanceAlongSpline(Mid) < Key) Low = Mid;
                    else High = Mid;
                }
            }
            Curves.ReparamTable.Points.Emplace(Step ? High : Begin, FMath::Lerp(CurveA, CurveB, Fraction), 0.f, 0.f, CIM_Linear);
        }
    }
    Curves.ReparamTable.Points.Emplace(Length, S->IsClosedLoop() ? LastCurveKey + Curves.Position.LoopKeyOffset : LastCurveKey, 0.f, 0.f, CIM_Linear);
    return true;
}

struct FBuilder
{
    FRailwayNetworkData Data;
    TArray<ARailwayTrack*> Tracks;
    TArray<ARailwayJunction*> Junctions;
    TArray<ARailwayStationMarker*> Markers;
    TMap<const ARailwayTrack*, int32> TrackIndices;
    TMap<const ARailwayJunction*, int32> JunctionNodes;
    TArray<TMap<double, int32>> Cuts;
    TArray<TArray<FRailwayStation>> StationRows;
    FString Error;

    bool Fail(const AActor& Source, const TCHAR* Reason)
    {
        Error = FString::Printf(TEXT("%s: %s"), *Source.GetActorLabel(), Reason);
        return false;
    }

    bool AddPort(int32 TrackIndex, double Distance, int32 Node)
    {
        const FRailwayBakedTrack& Track = Data.Tracks[TrackIndex];
        if (FVector::Dist(Track.PositionAt(Distance), Data.Nodes[Node]) > 1.0)
            return Fail(*Tracks[TrackIndex], TEXT("Snap the track endpoint to its junction (1 cm tolerance)"));
        const double Key = Track.IsLoop() && Distance == Track.Length() ? 0.0 : Distance;
        if (const int32* Existing = Cuts[TrackIndex].Find(Key); Existing && *Existing != Node)
            return Fail(*Tracks[TrackIndex], TEXT("Two different junctions occupy the same track distance"));
        Cuts[TrackIndex].Add(Key, Node);
        return true;
    }

    bool ConnectEndpoint(int32 TrackIndex, const TSoftObjectPtr<ARailwayJunction>& Junction, double Distance)
    {
        if (Junction.IsNull()) return true;
        const int32* Node = JunctionNodes.Find(Junction.Get());
        if (!Node) return Fail(*Tracks[TrackIndex], TEXT("Referenced junction is missing or belongs to another level"));
        return AddPort(TrackIndex, Distance, *Node);
    }

    bool BuildNodes()
    {
        Cuts.SetNum(Tracks.Num());
        StationRows.SetNum(Tracks.Num());
        for (int32 I = 0; I < Tracks.Num(); ++I)
        {
            TrackIndices.Add(Tracks[I], I);
            if (!BakeSpline(*Tracks[I], Data.Tracks.AddDefaulted_GetRef()))
                return Fail(*Tracks[I], TEXT("Spline must have finite geometry and positive-length segments"));
            StationRows[I] = Tracks[I]->Stations;
        }
        for (const ARailwayJunction* Junction : Junctions)
        {
            const int32 Node = Data.Nodes.Num();
            JunctionNodes.Add(Junction, Node);
            if (Junction->HostTrack.IsNull()) Data.Nodes.Add(Junction->GetActorLocation());
            else
            {
                const int32* Host = TrackIndices.Find(Junction->HostTrack.Get());
                if (!Host || !FMath::IsFinite(Junction->TrackDistance))
                    return Fail(*Junction, TEXT("Invalid host track or junction distance"));
                const FRailwayBakedTrack& Track = Data.Tracks[*Host];
                const double Local = FMath::Clamp(Junction->TrackDistance, 0.0, Track.Length());
                Data.Nodes.Add(Track.PositionAt(Local));
                if (!AddPort(*Host, Local, Node)) return false;
            }
        }
        for (int32 I = 0; I < Tracks.Num(); ++I)
        {
            const FRailwayBakedTrack& Track = Data.Tracks[I];
            const double Length = Track.Length();
            if (!Track.IsLoop() && (!ConnectEndpoint(I, Tracks[I]->StartJunction, 0) ||
                !ConnectEndpoint(I, Tracks[I]->EndJunction, Length))) return false;
            auto& Ports = Cuts[I];
            if (!Ports.Contains(0.0)) Ports.Add(0.0, Data.Nodes.Add(Track.PositionAt(0)));
            if (Track.IsLoop())
            {
                const int32 SeamNode = Ports.FindChecked(0.0);
                Ports.Add(Length, SeamNode);
            }
            else if (!Ports.Contains(Length)) Ports.Add(Length, Data.Nodes.Add(Track.PositionAt(Length)));
        }
        return true;
    }

    void BuildEdges()
    {
        TArray<TArray<int32>> Departures;
        Departures.SetNum(Data.Nodes.Num());
        TArray<FVector> EntryHeadings, ExitHeadings;
        for (int32 I = 0; I < Tracks.Num(); ++I)
        {
            FRailwayBakedTrack& Track = Data.Tracks[I];
            Track.FirstEdge = Data.Edges.Num();
            TArray<double> Distances;
            Cuts[I].GenerateKeyArray(Distances);
            Distances.Sort();
            for (int32 J = 1; J < Distances.Num(); ++J)
            {
                const double A = Distances[J - 1], B = Distances[J];
                const int32 NodeA = Cuts[I].FindChecked(A), NodeB = Cuts[I].FindChecked(B);
                const double Chord = FVector::Dist(Data.Nodes[NodeA], Data.Nodes[NodeB]);
                if (Chord > 0) Data.MinCostPerCm = FMath::Min(Data.MinCostPerCm, (B - A) / Chord);

                // One-sided headings also handle linear corners and zero endpoint tangents.
                const double Step = FMath::Min(1.0, (B - A) * .25);
                const FVector AtA = (Track.PositionAt(A + Step) - Track.PositionAt(A)).GetSafeNormal();
                const FVector AtB = (Track.PositionAt(B) - Track.PositionAt(B - Step)).GetSafeNormal();
                const int32 Forward = Data.Edges.Add({I, NodeA, NodeB, A, B});
                const int32 Reverse = Data.Edges.Add({I, NodeB, NodeA, B, A});
                Departures[NodeA].Add(Forward);
                Departures[NodeB].Add(Reverse);
                EntryHeadings.Add(AtA);
                ExitHeadings.Add(AtB);
                EntryHeadings.Add(-AtB);
                ExitHeadings.Add(-AtA);
            }
            Track.NumEdges = Data.Edges.Num() - Track.FirstEdge;
        }
        for (int32 I = 0; I < Data.Edges.Num(); ++I)
        {
            FRailwayEdge& Edge = Data.Edges[I];
            Edge.FirstSuccessor = Data.Successors.Num();
            for (int32 Next : Departures[Edge.ToNode])
            {
                if (Next == (I ^ 1)) continue; // opposite direction on the same physical span
                if (FVector::DotProduct(ExitHeadings[I], EntryHeadings[Next]) > UE_KINDA_SMALL_NUMBER)
                    Data.Successors.Add(Next);
            }
            Edge.NumSuccessors = Data.Successors.Num() - Edge.FirstSuccessor;
        }
    }

    bool BuildStations()
    {
        for (const ARailwayStationMarker* Marker : Markers)
        {
            const int32* Host = TrackIndices.Find(Marker->HostTrack.Get());
            if (!Host) return Fail(*Marker, TEXT("Select a host track in the current level"));
            StationRows[*Host].Add(Marker->GetStationDefinition());
        }
        for (int32 I = 0; I < Tracks.Num(); ++I)
        {
            FRailwayBakedTrack& Track = Data.Tracks[I];
            Track.FirstStation = Data.Stations.Num();
            for (FRailwayStation& Row : StationRows[I])
            {
                if (!FMath::IsFinite(Row.TrackDistance) || !FMath::IsFinite(Row.DwellSeconds) || Row.DwellSeconds < 0)
                    return Fail(*Tracks[I], TEXT("Invalid station distance or dwell time"));
                Row.TrackDistance = FMath::Clamp(Row.TrackDistance, 0.0, Track.Length());
                if (Track.IsLoop() && Row.TrackDistance == Track.Length()) Row.TrackDistance = 0;
            }
            StationRows[I].StableSort([](const FRailwayStation& A, const FRailwayStation& B) { return A.TrackDistance < B.TrackDistance; });
            int32 Edge = Track.FirstEdge;
            for (const FRailwayStation& Row : StationRows[I])
            {
                while (Edge + 2 < Track.FirstEdge + Track.NumEdges && Row.TrackDistance >= Data.Edges[Edge].EndDistance) Edge += 2;
                Data.Stations.Add({Edge, Row});
            }
            Track.NumStations = Data.Stations.Num() - Track.FirstStation;
        }
        return true;
    }
};
}

bool BuildData(ULevel& Level, FString& Message)
{
    Message.Reset();
    TArray<FWorldPartitionReference> Pins;
    if (!LoadSources(Level, Pins, Message)) return false;

    FBuilder Builder;
    ARailwayDataActor* Target = nullptr;
    for (AActor* Actor : Level.Actors)
    {
        if (!IsValid(Actor) || Actor->GetLevel() != &Level) continue;
        if (auto* Track = Cast<ARailwayTrack>(Actor)) Builder.Tracks.Add(Track);
        else if (auto* Junction = Cast<ARailwayJunction>(Actor)) Builder.Junctions.Add(Junction);
        else if (auto* Station = Cast<ARailwayStationMarker>(Actor)) Builder.Markers.Add(Station);
        else if (auto* DataActor = Cast<ARailwayDataActor>(Actor))
        {
            if (Target)
            {
                Message = TEXT("The current level contains more than one Railway Data Actor");
                return false;
            }
            Target = DataActor;
        }
    }
    // Editor actor names are unique within the level and independent of partition loading order.
    const auto ByName = [](const AActor& A, const AActor& B) { return A.GetFName().LexicalLess(B.GetFName()); };
    Builder.Tracks.Sort(ByName);
    Builder.Junctions.Sort(ByName);
    Builder.Markers.Sort(ByName);
    if (!Builder.BuildNodes())
    {
        Message = MoveTemp(Builder.Error);
        return false;
    }
    Builder.BuildEdges();
    if (!Builder.BuildStations())
    {
        Message = MoveTemp(Builder.Error);
        return false;
    }

    FScopedTransaction Transaction(NSLOCTEXT("RailwayEditor", "BuildTransaction", "Build Railway Data"));
    if (!Target)
    {
        FActorSpawnParameters Spawn;
        Spawn.OverrideLevel = &Level;
        Spawn.ObjectFlags |= RF_Transactional;
        Spawn.bCreateActorPackage = Level.IsUsingExternalActors();
        Level.Modify();
        Target = Level.GetWorld()->SpawnActor<ARailwayDataActor>(ARailwayDataActor::StaticClass(), FTransform::Identity, Spawn);
        if (!Target)
        {
            Transaction.Cancel();
            Message = TEXT("Could not create a Railway Data Actor in the current level");
            return false;
        }
        Target->SetActorLabel(TEXT("Railway Data"));
    }
    Target->Modify();
    Builder.Data.BuildId = FGuid::NewGuid();
    Target->Network = MoveTemp(Builder.Data);
    Target->MarkPackageDirty();
    Message = FString::Printf(TEXT("Built %d tracks, %d directed edges and %d stations. Save the level."),
        Target->Network.Tracks.Num(), Target->Network.Edges.Num(), Target->Network.Stations.Num());
    return true;
}
}
