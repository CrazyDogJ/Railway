#include "RailwayDataActor.h"

#include "Math/RotationMatrix.h"

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
