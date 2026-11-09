#include "RailwayTrack.h"
#include "RailwayTrackPlacement.h"
#include "RailwayRouteGeometry.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Components/ArrowComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/World.h"

namespace Railway::Placement
{
void DetachFromTrack(AActor& Actor)
{
    if (Cast<ARailwayTrack>(Actor.GetAttachParentActor()))
        Actor.DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
}

void AttachToSpline(AActor& Actor, USplineComponent& Spline)
{
    if (Actor.GetRootComponent()->GetAttachParent() != &Spline)
        Actor.AttachToComponent(&Spline, FAttachmentTransformRules::KeepWorldTransform);
}

void ApplyPose(AActor& Actor, const FTransform& Pose, double PositionTolerance, double RotationTolerance)
{
    if (!Actor.GetActorLocation().Equals(Pose.GetLocation(), PositionTolerance) ||
        !Actor.GetActorQuat().Equals(Pose.GetRotation(), RotationTolerance))
        Actor.SetActorLocationAndRotation(Pose.GetLocation(), Pose.GetRotation());
}
}

ARailwayTrackAnchor::ARailwayTrackAnchor()
{
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
#if WITH_EDITOR
    PrimaryActorTick.bCanEverTick = true;
#endif
}

void ARailwayTrackAnchor::SetTrackAnchor(ARailwayTrack* Track, float NormalizedPosition)
{
    if (!FMath::IsFinite(NormalizedPosition) || (Track && (!IsValid(Track) || Track->GetWorld() != GetWorld()))) return;
    HostTrack = Track;
    TrackPosition = FMath::Clamp(NormalizedPosition, 0.f, 1.f);
    RefreshAnchor();
}

void ARailwayTrackAnchor::ProjectToHost()
{
    if (!IsValid(HostTrack) || !IsValid(HostTrack->Spline)) return;
    const USplineComponent* S = HostTrack->Spline;
    const float Length = S->GetSplineLength();
    if (Length > UE_SMALL_NUMBER)
        TrackPosition = FMath::Clamp(S->GetDistanceAlongSplineAtLocation(GetActorLocation(), ESplineCoordinateSpace::World) / Length, 0.f, 1.f);
}

void ARailwayTrackAnchor::RefreshAnchor()
{
    if (!IsValid(HostTrack) || !IsValid(HostTrack->Spline))
    {
        Railway::Placement::DetachFromTrack(*this);
        return;
    }
    USplineComponent* S = HostTrack->Spline;
    Railway::Placement::AttachToSpline(*this, *S);
    const double Distance = Railway::Geometry::DistanceAtPosition(*S, TrackPosition);
    const auto Pose = S->GetTransformAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
    Railway::Placement::ApplyPose(*this, Pose, .001, .00001);
}

void ARailwayTrackAnchor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    RefreshAnchor();
}

#if WITH_EDITOR
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

FRailwayStation ARailwayStationMarker::GetStationDefinition() const
{
    return FRailwayStation{StationName, TrackPosition, DwellSeconds};
}

void ARailwayStationMarker::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
#if WITH_EDITORONLY_DATA
    if (Label) Label->SetText(FText::FromName(StationName));
#endif
}

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
#endif

void ARailwayTrack::SnapEndpointsToJunctions()
{
    AlignEndpoints(true);
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
        if (!IsValid(Junction) || Junction->HostTrack == this) return;
        if (!IsValid(Junction->HostTrack) && !bIncludeFreeJunctions) return;
        Junction->RefreshAnchor();
        const FVector Location = Junction->GetActorLocation();
        if (Spline->GetLocationAtSplinePoint(Point, ESplineCoordinateSpace::World).Equals(Location, .001)) return;
#if WITH_EDITOR
        if (!bChanged && GetWorld() && GetWorld()->WorldType == EWorldType::Editor)
        {
            Modify();
            Spline->Modify();
        }
#endif
        Spline->SetLocationAtSplinePoint(Point, Location, ESplineCoordinateSpace::World, false);
        bChanged = true;
    };
    Align(0, StartJunction);
    Align(Last, EndJunction);
    if (bChanged)
    {
        Spline->UpdateSpline();
#if WITH_EDITORONLY_DATA
        Spline->bSplineHasBeenEdited = true;
#endif
    }
}
