#include "RailwayTrain.h"

#include "Algo/BinarySearch.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Railway.h"
#include "RailwayRouteGeometry.h"
#include "RailwayRoutePlanner.h"
#include "RailwayTrack.h"
#include "RailwayTrackPlacement.h"
#include "UObject/ConstructorHelpers.h"
#if WITH_EDITOR
#include "UObject/Script.h"
#endif

ARailwayTrain::ARailwayTrain()
{
    PrimaryActorTick.bCanEverTick = true;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
    Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
    Body->SetupAttachment(RootComponent);
    Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (Cube.Succeeded()) Body->SetStaticMesh(Cube.Object);
    Body->SetRelativeScale3D(FVector(2.5, 1, 1));
    Body->SetRelativeLocation(FVector(0, 0, 50));

    bReplicates = true;
    SetReplicateMovement(false);
}

bool ARailwayTrain::GetPreviewLocation_Implementation(FRailwayLeg& PreviewLeg, double& TrackDistance) const
{
    PreviewLeg = Route.IsEmpty() ? FRailwayLeg() : Route[0];
    TrackDistance = 0;
    if (!IsValid(PreviewLeg.Track) || !IsValid(PreviewLeg.Track->Spline)) return false;
    if (PreviewLeg.bReverse) TrackDistance = PreviewLeg.Track->Spline->GetSplineLength();
    return true;
}

#if WITH_EDITOR
void ARailwayTrain::RefreshPreview()
{
    FEditorScriptExecutionGuard ScriptGuard;
    FRailwayLeg First;
    double Local = 0;
    if (!GetPreviewLocation(First, Local) || !IsValid(First.Track) || !IsValid(First.Track->Spline) || !FMath::IsFinite(Local))
    {
        Railway::Placement::DetachFromTrack(*this);
        return;
    }
    Railway::Placement::AttachToSpline(*this, *First.Track->Spline);
    Railway::Placement::ApplyPose(*this, Railway::Geometry::SampleTrack(First, Local));
}

void ARailwayTrain::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    if (GetWorld() && GetWorld()->WorldType == EWorldType::Editor) RefreshPreview();
}
#endif

void ARailwayTrain::BeginPlay()
{
    if (HasAuthority()) SetReplicateMovement(false); // Also override older Blueprint movement defaults.
    Super::BeginPlay();
    if (HasAuthority() && bAutoStart) StartRoute();
}

void ARailwayTrain::SetState(const ERailwayState InState)
{
    if (State != InState)
    {
        const auto PrevState = State;
        State = InState;
        OnStateChanged.Broadcast(PrevState, State);
    }
}

void ARailwayTrain::UpdateVelocity(bool /*bReverse*/) const
{
    if (RootComponent)
    {
        RootComponent->ComponentVelocity = GetActorForwardVector() * Speed;
    }
}

bool ARailwayTrain::IsInService() const
{
    return State == ERailwayState::Running || State == ERailwayState::Dwelling;
}

FName ARailwayTrain::GetStationForState(ERailwayState InState) const
{
    return InState == ERailwayState::Dwelling && ActivePlan.Stops.IsValidIndex(NextStop - 1)
        ? ActivePlan.Stops[NextStop - 1].Station.Name : NAME_None;
}

void ARailwayTrain::ResetMotion()
{
    DriveInput = 0.f;
    Speed = 0.f;
    Wait = 0;
    bPreLeaveSent = false;
}

void ARailwayTrain::InstallPlan(FRailwayPlan&& Plan)
{
    // 版本同时用于网络配对和委托重入检查。跳过表示“尚未发车”的版本零。
    Plan.NetworkRevision = ActivePlan.NetworkRevision + 1;
    if (Plan.NetworkRevision == 0) Plan.NetworkRevision = 1;
    ActivePlan = MoveTemp(Plan);
}

void ARailwayTrain::ResetPlan(ERailwayState NewState)
{
    InstallPlan(FRailwayPlan());
    SectionEnds.Reset();
    ResetMotion();
    Distance = 0;
    Leg = NextStop = 0;
    CurrentStation = NAME_None;
    UpdateVelocity();
    SetState(NewState);
    PublishMoveState(true);
}

bool ARailwayTrain::FailRoute(const FString& Reason)
{
    ResetPlan(ERailwayState::InvalidRoute);
    UE_LOG(LogRailway, Warning, TEXT("%s: %s"), *GetName(), *Reason);
    return false;
}

bool ARailwayTrain::StartRoute()
{
    if (!HasAuthority()) return false;
    for (const FRailwayLeg& Part : Route)
    {
        if (IsValid(Part.Track)) Part.Track->AlignEndpoints(false);
    }
    FRailwayPlan Plan;
    FString Error;
    if (!BuildRoutePlan(Route, Plan, Error) || !StartPreparedRoute(Plan, 0, Error)) return FailRoute(Error);
    return true;
}

bool ARailwayTrain::BuildRoutePlan(const TArray<FRailwayLeg>& Legs, FRailwayPlan& OutPlan, FString& OutError) const
{
    return Railway::Routing::Build(GetWorld(), Legs, TOptional<double>(), OutPlan, OutError);
}

bool ARailwayTrain::StartPreparedRoute(const FRailwayPlan& Plan, double StartDistance, FString& OutError)
{
    if (!HasAuthority())
    {
        OutError = TEXT("Starting a route requires authority");
        return false;
    }
    FRailwayPreparedPlan Prepared;
    if (!Railway::Routing::Prepare(GetWorld(), Plan, StartDistance, Prepared, OutError)) return false;
    // 严格小于起点的站才算跳过，恰好在起点的多个站仍按顺序服务。
    const int32 StopCursor = Algo::LowerBoundBy(Prepared.Plan.Stops, StartDistance, &FRailwayPlannedStop::Distance);
    ResetMotion();
    ActivatePlan(MoveTemp(Prepared), StartDistance, StopCursor, ERailwayState::Running);
    return true;
}

void ARailwayTrain::ActivatePlan(FRailwayPreparedPlan&& Prepared, double StartDistance, int32 StopCursor, ERailwayState NewState)
{
    InstallPlan(MoveTemp(Prepared.Plan));
    SectionEnds = MoveTemp(Prepared.SectionEnds);
    Distance = StartDistance;
    Leg = Prepared.StartSection;
    NextStop = StopCursor;
    CurrentStation = GetStationForState(NewState);
    Railway::Placement::DetachFromTrack(*this);
    Railway::Placement::ApplyPose(*this, Prepared.StartPose);
    UpdateVelocity();
    SetState(NewState);
    PublishMoveState(true);
}

bool ARailwayTrain::MakeDirectPlan(const FRailwayPlan& Plan, int32 DestinationStopIndex, FRailwayPlan& OutPlan)
{
    if (!Plan.Stops.IsValidIndex(DestinationStopIndex)) return false;
    const FRailwayPlannedStop Destination = Plan.Stops[DestinationStopIndex];
    OutPlan = Plan;
    OutPlan.Stops.Reset();
    OutPlan.Stops.Add(Destination);
    OutPlan.bFinishAtLastStop = true;
    OutPlan.NetworkRevision = 0;
    return true;
}

bool ARailwayTrain::GoDirectToStop(int32 DestinationStopIndex, FString& OutError)
{
    if (!HasAuthority() || !IsInService() ||
        DestinationStopIndex < NextStop || !ActivePlan.Stops.IsValidIndex(DestinationStopIndex))
    {
        OutError = TEXT("Select an unserved stop on an active server train");
        return false;
    }
    FRailwayPlan Direct;
    MakeDirectPlan(ActivePlan, DestinationStopIndex, Direct);
    return ApplyReplacementPlan(MoveTemp(Direct), Distance, OutError);
}

bool ARailwayTrain::ApplyReplacementPlan(FRailwayPlan&& NewPlan, double StartDistance, FString& OutError)
{
    const bool bDwelling = State == ERailwayState::Dwelling;
    if (bDwelling)
    {
        // 编入当前停站并将游标越过它。倒计时、预离站标记和驾驶输入延续。
        FRailwayPlannedStop CurrentStop = ActivePlan.Stops[NextStop - 1];
        CurrentStop.Distance = StartDistance;
        NewPlan.Stops.Insert(CurrentStop, 0);
    }
    FRailwayPreparedPlan Prepared;
    if (!Railway::Routing::Prepare(GetWorld(), MoveTemp(NewPlan), StartDistance, Prepared, OutError)) return false;
    if (!bDwelling)
    {
        Wait = 0;
        bPreLeaveSent = false;
    }
    ActivatePlan(MoveTemp(Prepared), StartDistance, bDwelling ? 1 : 0, State);
    return true;
}

bool ARailwayTrain::ChangeRemainingRoute(const TArray<FRailwayLeg>& NewLegs, FString& OutError)
{
    if (!HasAuthority() || !IsInService() ||
        NewLegs.IsEmpty())
    {
        OutError = TEXT("An active server train and a remaining route are required");
        return false;
    }
    int32 SectionIndex;
    double Local;
    if (!Railway::Geometry::Locate(ActivePlan, SectionEnds, Distance, SectionIndex, Local))
    {
        OutError = TEXT("Cannot locate the train on its current track");
        return false;
    }
    const FRailwayLeg& Current = ActivePlan.Sections[SectionIndex].Leg;
    if (NewLegs[0].Track != Current.Track || NewLegs[0].bReverse != Current.bReverse)
    {
        OutError = TEXT("The first leg must match the occupied track and direction");
        return false;
    }
    FRailwayPlan RemainingPlan;
    if (!Railway::Routing::Build(GetWorld(), NewLegs, TOptional<double>(Local), RemainingPlan, OutError)) return false;

    // Do not serve the same marker twice when rerouting at a platform. Consume at most one
    // matching new stop per served occurrence so co-located stops still work.
    for (int32 I = NextStop - 1; I >= 0 && FMath::IsNearlyEqual(ActivePlan.Stops[I].Distance, Distance, .001); --I)
    {
        const FRailwayStation& Served = ActivePlan.Stops[I].Station;
        const int32 Match = RemainingPlan.Stops.IndexOfByPredicate([&](const FRailwayPlannedStop& S)
        {
            return S.Distance <= .001 && S.Station.Name == Served.Name && S.Station.Position == Served.Position;
        });
        if (Match != INDEX_NONE) RemainingPlan.Stops.RemoveAt(Match);
    }

    if (!ApplyReplacementPlan(MoveTemp(RemainingPlan), 0, OutError)) return false;
    Route = NewLegs;
    return true;
}

void ARailwayTrain::GetNextStopInfo(double& OutDistanceRemain, FRailwayStation& OutStationInfo) const
{
    OutDistanceRemain = -1;
    OutStationInfo = FRailwayStation();
    if (ActivePlan.Stops.IsValidIndex(NextStop))
    {
        OutStationInfo = ActivePlan.Stops[NextStop].Station;
        OutDistanceRemain = ActivePlan.Stops[NextStop].Distance - Distance;
    }
}

bool ARailwayTrain::SamplePlan(const FRailwayPlan& Plan, double RouteDistance, int32& SectionIndex, double& TrackDistance, FTransform& Pose)
{
    return Railway::Geometry::SamplePlan(Plan, RouteDistance, SectionIndex, TrackDistance, Pose);
}

bool ARailwayTrain::SampleRoute(double RouteDistance, int32& SectionIndex, double& TrackDistance, FTransform& Pose) const
{
    return Railway::Geometry::SampleIndexedPlan(ActivePlan, SectionEnds, RouteDistance, SectionIndex, TrackDistance, Pose);
}

bool ARailwayTrain::UpdatePose()
{
    FTransform Pose;
    double TrackDistance;
    if (!SampleRoute(Distance, Leg, TrackDistance, Pose))
        return FailRoute(FString::Printf(TEXT("Cannot sample active route at %.2f cm"), Distance));
    Railway::Placement::ApplyPose(*this, Pose);
    UpdateVelocity();
    return true;
}

void ARailwayTrain::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
#if WITH_EDITOR
    if (GetWorld()->WorldType == EWorldType::Editor)
    {
        RefreshPreview();
        return;
    }
#endif
    if (HasAuthority())
    {
        // 权威模拟以至多 1/60 秒的子步推进，避免低帧率时越过站点。
        double Left = DeltaSeconds;
        while (Left > 0 && IsInService())
        {
            const double Dt = FMath::Min(Left, 1.0 / 60.0);
            Step(Dt);
            Left -= Dt;
        }
        PublishMoveState(false);
    }
    else
    {
        TickClientPresentation();
    }
}
