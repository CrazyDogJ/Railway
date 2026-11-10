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

// Keep the index revision when every route can still be sampled and stopped at the
// same distances. In particular, editing dwell time or CustomData does not invalidate saves.
bool SameIndexLayout(const FRailwayNetworkData& Old, const FRailwayNetworkData& New)
{
    if (!Old.BuildId.IsValid() || Old.Tracks.Num() != New.Tracks.Num() || Old.Edges.Num() != New.Edges.Num() ||
        Old.Stations.Num() != New.Stations.Num() || Old.Nodes != New.Nodes || Old.Successors != New.Successors) return false;
    const bool bLegacy = !Old.NetworkId.IsValid();
    if (!bLegacy && Old.NetworkId != New.NetworkId) return false;
    TSet<FName> LegacyTrackNames;
    for (int32 I = 0; I < Old.Tracks.Num(); ++I)
    {
        const FRailwayBakedTrack& A = Old.Tracks[I];
        const FRailwayBakedTrack& B = New.Tracks[I];
        if (bLegacy)
        {
            // Index-only bakes have no identity. Accept an unchanged, unambiguous track layout.
            if (A.Name != B.Name || LegacyTrackNames.Contains(A.Name)) return false;
            LegacyTrackNames.Add(A.Name);
        }
        else if (A.Id != B.Id) return false;
        if (A.FirstEdge != B.FirstEdge || A.NumEdges != B.NumEdges || A.FirstStation != B.FirstStation ||
            A.NumStations != B.NumStations || A.DefaultUp != B.DefaultUp || !A.SplineToWorld.Equals(B.SplineToWorld, 0) ||
            A.Curves != B.Curves || A.Curves.ReparamTable != B.Curves.ReparamTable) return false;
    }
    for (int32 I = 0; I < Old.Edges.Num(); ++I)
        if (!FRailwayEdge::StaticStruct()->CompareScriptStruct(&Old.Edges[I], &New.Edges[I], 0)) return false;
    TSet<TPair<int32, FName>> LegacyStations;
    for (int32 I = 0; I < Old.Stations.Num(); ++I)
    {
        const auto& A = Old.Stations[I];
        const auto& B = New.Stations[I];
        if (A.EdgeIndex != B.EdgeIndex || A.Definition.TrackDistance != B.Definition.TrackDistance) return false;
        if (bLegacy)
        {
            const TPair<int32, FName> Key(Old.Edges[A.EdgeIndex].TrackIndex, A.Definition.Name);
            if (A.Definition.Name != B.Definition.Name || LegacyStations.Contains(Key)) return false;
            LegacyStations.Add(Key);
        }
        else if (A.Id != B.Id) return false;
    }
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
    TArray<TArray<FRailwayBakedStation>> StationRows;
    struct FNewRowId { ARailwayTrack* Track; int32 Row; FGuid Id; };
    TArray<FNewRowId> NewRowIds;
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
            const FGuid TrackId = Tracks[I]->GetActorGuid();
            if (!TrackId.IsValid() || Data.TrackById.Contains(TrackId))
                return Fail(*Tracks[I], TEXT("Missing or duplicate track GUID"));
            Data.Tracks[I].Id = TrackId;
            Data.TrackById.Add(TrackId, I);
            TSet<FGuid> RowIds;
            for (int32 Row = 0; Row < Tracks[I]->Stations.Num(); ++Row)
            {
                const FRailwayStation& Definition = Tracks[I]->Stations[Row];
                FGuid Id = Definition.AuthoringId;
                if (!Id.IsValid())
                {
                    Id = FGuid::NewGuid();
                    NewRowIds.Add({Tracks[I], Row, Id});
                }
                if (RowIds.Contains(Id)) return Fail(*Tracks[I], TEXT("Duplicate inline station ID; recreate the copied row"));
                RowIds.Add(Id);
                StationRows[I].Add({INDEX_NONE, Definition, FGuid::Combine(TrackId, Id)});
            }
        }
        for (const ARailwayJunction* Junction : Junctions)
        {
            const int32 Node = Data.Nodes.Num();
            JunctionNodes.Add(Junction, Node);
            Data.JunctionByNode.Add(Node, Junction->GetActorGuid());
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
            StationRows[*Host].Add({INDEX_NONE, Marker->GetStationDefinition(), Marker->GetActorGuid()});
        }
        for (int32 I = 0; I < Tracks.Num(); ++I)
        {
            FRailwayBakedTrack& Track = Data.Tracks[I];
            Track.FirstStation = Data.Stations.Num();
            for (FRailwayBakedStation& Station : StationRows[I])
            {
                FRailwayStation& Row = Station.Definition;
                if (!FMath::IsFinite(Row.TrackDistance) || !FMath::IsFinite(Row.DwellSeconds) || Row.DwellSeconds < 0)
                    return Fail(*Tracks[I], TEXT("Invalid station distance or dwell time"));
                Row.TrackDistance = FMath::Clamp(Row.TrackDistance, 0.0, Track.Length());
                if (Track.IsLoop() && Row.TrackDistance == Track.Length()) Row.TrackDistance = 0;
            }
            StationRows[I].StableSort([](const FRailwayBakedStation& A, const FRailwayBakedStation& B)
            {
                return A.Definition.TrackDistance < B.Definition.TrackDistance;
            });
            int32 Edge = Track.FirstEdge;
            for (FRailwayBakedStation& Station : StationRows[I])
            {
                const FRailwayStation& Row = Station.Definition;
                while (Edge + 2 < Track.FirstEdge + Track.NumEdges && Row.TrackDistance >= Data.Edges[Edge].EndDistance) Edge += 2;
                if (!Station.Id.IsValid() || Data.TrackById.Contains(Station.Id) || Data.StationById.Contains(Station.Id))
                    return Fail(*Tracks[I], TEXT("Missing or duplicate station GUID"));
                Station.EdgeIndex = Edge;
                Data.StationById.Add(Station.Id, Data.Stations.Num());
                Data.Stations.Add(MoveTemp(Station));
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
    for (const FBuilder::FNewRowId& New : Builder.NewRowIds)
    {
        New.Track->Modify();
        New.Track->Stations[New.Row].AuthoringId = New.Id;
        New.Track->MarkPackageDirty();
    }
    Target->Modify();
    Builder.Data.NetworkId = Target->GetActorGuid();
    Builder.Data.BuildId = SameIndexLayout(Target->Network, Builder.Data) ? Target->Network.BuildId : FGuid::NewGuid();
    Target->Network = MoveTemp(Builder.Data);
    Target->MarkPackageDirty();
    Message = FString::Printf(TEXT("Built %d tracks, %d directed edges and %d stations. Save the level and modified railway actors."),
        Target->Network.Tracks.Num(), Target->Network.Edges.Num(), Target->Network.Stations.Num());
    return true;
}
}
