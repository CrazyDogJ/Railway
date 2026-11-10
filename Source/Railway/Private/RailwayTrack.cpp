#include "RailwayTrack.h"
#include "RailwayTrackPlacement.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#if WITH_EDITORONLY_DATA
#include "Components/ArrowComponent.h"
#include "Components/TextRenderComponent.h"
#endif
#if WITH_EDITOR
#include "Engine/World.h"
#endif

namespace Railway::Placement
{
void ApplyPose(AActor& Actor, const FTransform& Pose, double PositionTolerance, double RotationTolerance)
{
    if (!Actor.GetActorLocation().Equals(Pose.GetLocation(), PositionTolerance) ||
        !Actor.GetActorQuat().Equals(Pose.GetRotation(), RotationTolerance))
        Actor.SetActorLocationAndRotation(Pose.GetLocation(), Pose.GetRotation());
}
}

ARailwayTrackAnchor::ARailwayTrackAnchor()
{
    bIsEditorOnlyActor = true;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
#if WITH_EDITOR
    PrimaryActorTick.bCanEverTick = true;
#endif
}

#if WITH_EDITOR
void ARailwayTrackAnchor::SetTrackAnchorAtDistance(ARailwayTrack* Track, double DistanceCm)
{
    if (!FMath::IsFinite(DistanceCm) || (Track && (!IsValid(Track) || Track->GetWorld() != GetWorld()))) return;
    HostTrack = Track;
    TrackDistance = FMath::Max(0.0, DistanceCm);
    RefreshAnchor();
}

void ARailwayTrackAnchor::ProjectToHost()
{
    if (!HostTrack.IsValid() || !IsValid(HostTrack->Spline)) return;
    const USplineComponent* S = HostTrack->Spline;
    TrackDistance = S->GetDistanceAlongSplineAtLocation(GetActorLocation(), ESplineCoordinateSpace::World);
}

void ARailwayTrackAnchor::RefreshAnchor()
{
    if (!HostTrack.IsValid() || !IsValid(HostTrack->Spline)) return;
    const USplineComponent* S = HostTrack->Spline;
    const float Distance = FMath::Clamp(TrackDistance, 0.0, static_cast<double>(S->GetSplineLength()));
    const auto Pose = S->GetTransformAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
    Railway::Placement::ApplyPose(*this, Pose, .001, .00001);
}

void ARailwayTrackAnchor::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
    if (Event.GetPropertyName() == GET_MEMBER_NAME_CHECKED(ARailwayTrackAnchor, HostTrack))
        ProjectToHost();
    Super::PostEditChangeProperty(Event);
}

void ARailwayTrackAnchor::PostEditMove(bool bFinished)
{
    ProjectToHost();
    Super::PostEditMove(bFinished);
    RefreshAnchor();
}

void ARailwayTrackAnchor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (GetWorld()->WorldType == EWorldType::Editor) RefreshAnchor();
}
#endif

#if WITH_EDITORONLY_DATA
void ARailwayTrackAnchor::CreateMarkerArrow(FName SubobjectName, FColor Color)
{
    auto* Marker = CreateEditorOnlyDefaultSubobject<UArrowComponent>(SubobjectName);
    if (Marker)
    {
        Marker->SetupAttachment(RootComponent);
        Marker->ArrowColor = Color;
    }
}
#endif

ARailwayJunction::ARailwayJunction()
{
#if WITH_EDITORONLY_DATA
    CreateMarkerArrow(TEXT("JunctionMarker"), FColor::Yellow);
#endif
}

ARailwayStationMarker::ARailwayStationMarker()
{
#if WITH_EDITORONLY_DATA
    Label = CreateEditorOnlyDefaultSubobject<UTextRenderComponent>(TEXT("StationLabel"));
    if (Label)
    {
        Label->SetupAttachment(RootComponent);
        Label->SetRelativeLocation(FVector(0, 0, 80));
        Label->SetHorizontalAlignment(EHTA_Center);
        Label->SetTextRenderColor(FColor::Cyan);
        Label->SetWorldSize(55.f);
        Label->SetHiddenInGame(true);
    }
    CreateMarkerArrow(TEXT("StationMarker"), FColor::Cyan);
#endif
}

#if WITH_EDITOR
FRailwayStation ARailwayStationMarker::GetStationDefinition() const
{
    return FRailwayStation{StationName, TrackDistance, DwellSeconds, CustomData};
}

void ARailwayStationMarker::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    if (Label) Label->SetText(FText::FromName(StationName));
}
#endif

ARailwayTrack::ARailwayTrack()
{
#if WITH_EDITOR
    PrimaryActorTick.bCanEverTick = true;
#endif
    Spline = CreateDefaultSubobject<USplineComponent>(TEXT("Spline"));
    SetRootComponent(Spline);
    Spline->bInputSplinePointsToConstructionScript = true;
    Spline->SetLocationAtSplinePoint(1, FVector(2000, 0, 0), ESplineCoordinateSpace::Local);
}

#if WITH_EDITOR
void ARailwayTrack::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (GetWorld()->WorldType == EWorldType::Editor) AlignEndpoints(false);
}

void ARailwayTrack::SnapEndpointsToJunctions()
{
    if (GetWorld() && GetWorld()->WorldType == EWorldType::Editor) AlignEndpoints(true);
}

void ARailwayTrack::AlignEndpoints(bool bIncludeFreeJunctions)
{
    if (!Spline || Spline->IsClosedLoop())
    {
        return;
    }
    
    const int32 Last = Spline->GetNumberOfSplinePoints() - 1;
    if (Last < 1) return;
    bool bChanged = false;
    auto Align = [&](int32 Point, ARailwayJunction* Junction)
    {
        if (!IsValid(Junction) || Junction->HostTrack.Get() == this) return;
        if (!Junction->HostTrack.IsNull() && !Junction->HostTrack.IsValid()) return;
        if (Junction->HostTrack.IsNull() && !bIncludeFreeJunctions) return;
        Junction->RefreshAnchor();
        const FVector Location = Junction->GetActorLocation();
        if (Spline->GetLocationAtSplinePoint(Point, ESplineCoordinateSpace::World).Equals(Location, .001)) return;
        if (!bChanged && GetWorld() && GetWorld()->WorldType == EWorldType::Editor)
        {
            Modify();
            Spline->Modify();
        }
        Spline->SetLocationAtSplinePoint(Point, Location, ESplineCoordinateSpace::World, false);
        bChanged = true;
    };
    Align(0, StartJunction.Get());
    Align(Last, EndJunction.Get());
    if (bChanged)
    {
        Spline->UpdateSpline();
        Spline->bSplineHasBeenEdited = true;
    }
}
#endif
