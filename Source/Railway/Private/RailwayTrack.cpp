#include "RailwayTrack.h"
#include "RailwayTrackPlacement.h"
#include "RailwayRouteGeometry.h"
#include "RailwayDistanceVersion.h"
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

void ARailwayTrackAnchor::Serialize(FArchive& Ar)
{
    Railway::DistanceVersion::SerializePosition(Ar, TrackPosition, .5f);
    Super::Serialize(Ar);
}

void ARailwayTrackAnchor::PostLoad()
{
    Super::PostLoad();
    if (IsTemplate() || TrackPosition == -1.f || !IsValid(HostTrack)) return;
    HostTrack->ConditionalPostLoad();
    if (IsValid(HostTrack->Spline)) HostTrack->Spline->ConditionalPostLoad();
    UpgradeTrackDistance();
}

void ARailwayTrackAnchor::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();
    UpgradeTrackDistance();
}

void ARailwayTrackAnchor::UpgradeTrackDistance()
{
    if (TrackPosition == -1.f || !IsValid(HostTrack) || !IsValid(HostTrack->Spline)) return;
    const float Length = HostTrack->Spline->GetSplineLength();
    if (Length <= 0) return;
    TrackDistance = Railway::DistanceVersion::FromLegacyAlpha(FMath::Clamp(TrackPosition, 0.f, 1.f), Length);
    TrackPosition = -1.f;
}

float ARailwayTrackAnchor::GetLegacyTrackPosition() const
{
    if (TrackPosition != -1.f) return TrackPosition;
    if (!IsValid(HostTrack) || !IsValid(HostTrack->Spline)) return 0.f;
    const float Length = HostTrack->Spline->GetSplineLength();
    return Length > 0 ? static_cast<float>(FMath::Clamp(TrackDistance / Length, 0.0, 1.0)) : 0.f;
}

void ARailwayTrackAnchor::SetTrackAnchorAtDistance(ARailwayTrack* Track, double DistanceCm)
{
    if (!FMath::IsFinite(DistanceCm) || (Track && (!IsValid(Track) || Track->GetWorld() != GetWorld()))) return;
    HostTrack = Track;
    TrackPosition = -1.f;
    TrackDistance = FMath::Max(0.0, DistanceCm);
    RefreshAnchor();
}

void ARailwayTrackAnchor::SetTrackAnchor(ARailwayTrack* Track, float NormalizedPosition)
{
    if (!FMath::IsFinite(NormalizedPosition) || (Track && (!IsValid(Track) || Track->GetWorld() != GetWorld()))) return;
    if (Track && !IsValid(Track->Spline)) return;
    const double DistanceCm = Track ? Railway::DistanceVersion::FromLegacyAlpha(
        FMath::Clamp(NormalizedPosition, 0.f, 1.f), Track->Spline->GetSplineLength()) : TrackDistance;
    SetTrackAnchorAtDistance(Track, DistanceCm);
}

void ARailwayTrackAnchor::ProjectToHost()
{
    if (!IsValid(HostTrack) || !IsValid(HostTrack->Spline)) return;
    const USplineComponent* S = HostTrack->Spline;
    TrackDistance = Railway::Geometry::DistanceOnSpline(*S,
        S->GetDistanceAlongSplineAtLocation(GetActorLocation(), ESplineCoordinateSpace::World));
    TrackPosition = -1.f;
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
    UpgradeTrackDistance();
    const float Distance = Railway::Geometry::DistanceOnSpline(*S, TrackDistance);
    const auto Pose = S->GetTransformAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
    Railway::Placement::ApplyPose(*this, Pose, .001, .00001);
}

void ARailwayTrackAnchor::OnRep_Anchor()
{
    // 服务端复制的是已换算的厘米数。客户端不再应用本地遗留的 Alpha。
    TrackPosition = -1.f;
    RefreshAnchor();
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
    else if (Event.GetPropertyName() == GET_MEMBER_NAME_CHECKED(ARailwayTrackAnchor, TrackDistance))
        TrackPosition = -1.f;
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
    return FRailwayStation{StationName, TrackDistance, DwellSeconds, CustomData, RailwayId};
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
