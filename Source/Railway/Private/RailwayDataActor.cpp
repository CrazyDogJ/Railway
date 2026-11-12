#include "RailwayDataActor.h"

#include "Math/RotationMatrix.h"
#include "Engine/World.h"
#include "RailwayWorldSubsystem.h"

void ARailwayDataActor::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();
    // Covers editor previews and the interval before a streamed level dispatches BeginPlay.
    if (auto* Registry = UWorld::GetSubsystem<URailwayWorldSubsystem>(GetWorld())) Registry->RegisterData(*this);
}

void ARailwayDataActor::PostUnregisterAllComponents()
{
    if (auto* Registry = UWorld::GetSubsystem<URailwayWorldSubsystem>(GetWorld())) Registry->UnregisterData(*this);
    Super::PostUnregisterAllComponents();
}

void ARailwayDataActor::BeginPlay()
{
    if (auto* Registry = UWorld::GetSubsystem<URailwayWorldSubsystem>(GetWorld())) Registry->RegisterData(*this);
    Super::BeginPlay();
}

void ARailwayDataActor::EndPlay(const EEndPlayReason::Type Reason)
{
    if (auto* Registry = UWorld::GetSubsystem<URailwayWorldSubsystem>(GetWorld())) Registry->UnregisterData(*this);
    Super::EndPlay(Reason);
}

ARailwayDataActor::ARailwayDataActor()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = false;
    bRelevantForLevelBounds = false;
#if WITH_EDITORONLY_DATA
    bIsSpatiallyLoaded = false;
#endif
}

FVector FRailwayBakedTrack::PositionAt(double Distance) const
{
    const float Key = Curves.ReparamTable.Eval(static_cast<float>(Distance), 0.f);
    return SplineToWorld.TransformPosition(Curves.Position.Eval(Key, FVector::ZeroVector));
}

FTransform FRailwayBakedTrack::PoseAt(double Distance, bool bReverse) const
{
    const float Key = Curves.ReparamTable.Eval(static_cast<float>(Distance), 0.f);
    FVector Forward = Curves.Position.EvalDerivative(Key, FVector::ZeroVector).GetSafeNormal();
    if (Forward.IsNearlyZero())
    {
        const double Step = FMath::Min(1.0, Length() * .001);
        Forward = SplineToWorld.InverseTransformVector(PositionAt(FMath::Min(Length(), Distance + Step))
            - PositionAt(FMath::Max(0.0, Distance - Step))).GetSafeNormal();
    }
    const FVector Up = Curves.Rotation.Eval(Key, FQuat::Identity).GetNormalized().RotateVector(DefaultUp);
    const FQuat Rotation = FRotationMatrix::MakeFromXZ(bReverse ? -Forward : Forward, Up).ToQuat();
    return FTransform(Rotation, Curves.Position.Eval(Key, FVector::ZeroVector),
        Curves.Scale.Eval(Key, FVector::OneVector)) * SplineToWorld;
}

bool ARailwayDataActor::SampleTrack(FRailwayTrackLocation Location, bool bReverse, FTransform& Pose) const
{
    Pose = FTransform::Identity;
    if (!Network.Tracks.IsValidIndex(Location.TrackIndex) || !FMath::IsFinite(Location.TrackDistance)) return false;
    const FRailwayBakedTrack& Track = Network.Tracks[Location.TrackIndex];
    if (Location.TrackDistance < 0 || Location.TrackDistance > Track.Length()) return false;
    Pose = Track.PoseAt(Location.TrackDistance, bReverse);
    return true;
}

bool FRailwayNetworkData::CanFollow(int32 From, int32 To) const
{
    if (!Edges.IsValidIndex(From) || !Edges.IsValidIndex(To)) return false;
    const FRailwayEdge& Edge = Edges[From];
    return MakeArrayView(Successors).Slice(Edge.FirstSuccessor, Edge.NumSuccessors).Contains(To);
}

void ARailwayDataActor::ApplyWorldOffset(const FVector& Offset, bool bWorldShift)
{
    Super::ApplyWorldOffset(Offset, bWorldShift);
    for (FRailwayBakedTrack& Track : Network.Tracks) Track.SplineToWorld.AddToTranslation(Offset);
    for (FVector& Node : Network.Nodes) Node += Offset;
}

bool FRailwayNetworkData::ResolvePoint(const FRailwayRoutePoint& Point, FRailwayTrackLocation& Location, int32& StationIndex) const
{
    Location = {};
    StationIndex = INDEX_NONE;
    if (const int32* Station = StationById.Find(Point.Source.Id))
    {
        const FRailwayBakedStation& Row = Stations[*Station];
        Location = {Edges[Row.EdgeIndex].TrackIndex, Row.Definition.TrackDistance};
        StationIndex = *Station;
        return true;
    }
    if (const int32* Track = TrackById.Find(Point.Source.Id))
    {
        if (!FMath::IsFinite(Point.TrackDistance) || Point.TrackDistance < 0 || Point.TrackDistance > Tracks[*Track].Length()) return false;
        Location = {*Track, Point.TrackDistance};
        return true;
    }
    return false;
}
