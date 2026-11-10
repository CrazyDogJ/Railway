#include "RailwayTrackDecoration.h"

#include "Components/StaticMeshComponent.h"
#if WITH_EDITOR
#include "Engine/World.h"
#include "UObject/Script.h"
#endif

ARailwayTrackDecoration::ARailwayTrackDecoration()
{
    bIsEditorOnlyActor = false;
    PrimaryActorTick.bStartWithTickEnabled = false;
    GetRootComponent()->SetMobility(EComponentMobility::Static);
    DecorationMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DecorationMesh"));
    DecorationMesh->SetMobility(EComponentMobility::Static);
    DecorationMesh->SetupAttachment(GetRootComponent());
}

void ARailwayTrackDecoration::OnTrackAligned_Implementation(const FTransform& TrackTransform)
{
}

#if WITH_EDITOR
void ARailwayTrackDecoration::RegisterActorTickFunctions(bool bRegister)
{
    Super::RegisterActorTickFunctions(bRegister);
    if (bRegister && GetWorld() && GetWorld()->WorldType == EWorldType::Editor)
        SetActorTickEnabled(true);
}

void ARailwayTrackDecoration::RefreshDecorationPlacement()
{
    if (bApplyingPlacement || !GetWorld() || GetWorld()->WorldType != EWorldType::Editor) return;
    LastAlignedPose.Reset();
    RefreshAnchor();
}

void ARailwayTrackDecoration::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    RefreshDecorationPlacement();
}

FVector ARailwayTrackDecoration::GetAnchorProjectionLocation() const
{
    if (!LastAlignedPose.IsSet() || LastHost.Get() != HostTrack.Get()) return GetActorLocation();
    // Project the user's drag delta from the anchor, even when the prop is offset from the rail.
    return LastTrackFrame.GetLocation() + (GetActorLocation() - LastPlacedPose.GetLocation());
}

void ARailwayTrackDecoration::ApplyAnchorPose(const FTransform& TrackPose)
{
    if (bApplyingPlacement || !GetWorld() || GetWorld()->WorldType != EWorldType::Editor) return;
    const FTransform TrackFrame(TrackPose.GetRotation(), TrackPose.GetLocation());
    const FTransform Aligned = PlacementOffset * TrackFrame;
    const FTransform Before = GetActorTransform();
    if (LastAlignedPose.IsSet() && LastHost.Get() == HostTrack.Get() &&
        LastTrackFrame.Equals(TrackFrame) && LastAlignedPose->Equals(Aligned) && LastPlacedPose.Equals(Before)) return;

    TGuardValue<bool> Guard(bApplyingPlacement, true);
    Modify(false);
    GetRootComponent()->Modify(false);
    if (!Before.Equals(Aligned)) SetActorTransform(Aligned);

    FEditorScriptExecutionGuard ScriptGuard;
    OnTrackAligned(TrackFrame);
    LastHost = HostTrack.Get();
    LastTrackFrame = TrackFrame;
    LastAlignedPose = Aligned;
    LastPlacedPose = GetActorTransform();
    if (!LastPlacedPose.Equals(Before)) MarkPackageDirty();
}
#endif
