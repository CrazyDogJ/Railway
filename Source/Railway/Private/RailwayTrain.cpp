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
#include "RailwayPathfinding.h"
#include "RailwayTrack.h"
#include "RailwayTrackPlacement.h"
#include "RailwayWorldIndex.h"
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
    FRailwayStation Current;
    if (InState == ERailwayState::Dwelling && ActivePlan.Stops.IsValidIndex(NextStop - 1) &&
        Railway::Stations::Read(GetWorld(), ActivePlan.Stops[NextStop - 1], Current)) return Current.Name;
    return NAME_None;
}

void ARailwayTrain::ClearDriverInputs()
{
    DriveInput = 0.f;
    BrakeInput = 0.f;
}

void ARailwayTrain::ResetMotion()
{
    ClearDriverInputs();
    Speed = 0.f;
    Wait = 0;
    ElapsedDwell = 0;
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

bool ARailwayTrain::FindRoutePlan(const FRailwayTrackLocation& Start, const FRailwayTrackLocation& Destination,
    FRailwayPlan& OutPlan, FString& OutError, bool bStopAtIntermediateStations) const
{
    return Railway::Routing::FindPath(GetWorld(), Start, Destination, bStopAtIntermediateStations, OutPlan, OutError);
}

bool ARailwayTrain::FindRoutePlanToStation(const FRailwayTrackLocation& Start, ARailwayStationMarker* DestinationStation,
    FRailwayPlan& OutPlan, FString& OutError, bool bStopAtIntermediateStations) const
{
    if (!IsValid(DestinationStation) || DestinationStation->GetWorld() != GetWorld())
    {
        OutPlan = FRailwayPlan();
        OutError = TEXT("Select a destination station in this world");
        return false;
    }
    return Railway::Routing::FindPathToStation(GetWorld(), Start, DestinationStation->HostTrack,
        ERailwayStationSource::Marker, DestinationStation->RailwayId, bStopAtIntermediateStations, OutPlan, OutError);
}

bool ARailwayTrain::FindRoutePlanToTrackStation(const FRailwayTrackLocation& Start, ARailwayTrack* DestinationTrack,
    FGuid StationId, FRailwayPlan& OutPlan, FString& OutError, bool bStopAtIntermediateStations) const
{
    return Railway::Routing::FindPathToStation(GetWorld(), Start, DestinationTrack,
        ERailwayStationSource::TrackStation, StationId, bStopAtIntermediateStations, OutPlan, OutError);
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
    const bool bAlreadyAtDestination = Prepared.SectionEnds.Last() == 0.0 && Prepared.Plan.Stops.IsEmpty();
    ResetMotion();
    ActivatePlan(MoveTemp(Prepared), StartDistance, StopCursor,
        bAlreadyAtDestination ? ERailwayState::Finished : ERailwayState::Running);
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
        // 改线把当前位置设为新计划起点时，车后已没有可行驶区间。
        if (StartDistance == 0 && Speed < 0) Speed = 0.f;
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
        const FRailwayStationRef& Served = ActivePlan.Stops[I].Reference;
        const int32 Match = RemainingPlan.Stops.IndexOfByPredicate([&](const FRailwayPlannedStop& S)
        {
            return S.Distance <= .001 && Railway::Stations::SameSource(S.Reference, Served);
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
    if (ActivePlan.Stops.IsValidIndex(NextStop) && ResolvePlannedStop(ActivePlan.Stops[NextStop], OutStationInfo))
        OutDistanceRemain = ActivePlan.Stops[NextStop].Distance - Distance;
}

bool ARailwayTrain::ResolvePlannedStop(const FRailwayPlannedStop& Stop, FRailwayStation& OutStation) const
{
    if (Railway::Stations::Read(GetWorld(), Stop, OutStation)) return true;
    // 刚反序列化的引用尚未绑定。只在这种情况下按世界 ID 查找，正常运行直接使用已绑定对象。
    if (Stop.Reference.Source != ERailwayStationSource::Inline &&
        (!IsValid(Stop.Reference.SourceActor) || Stop.Reference.PlannedTrackDistance == -1))
    {
        FRailwayPlannedStop Bound = Stop;
        FString Error;
        const FRailwayWorldIndex WorldIndex(GetWorld());
        if (WorldIndex.BindStop(Bound, Error) && Railway::Stations::Read(GetWorld(), Bound, OutStation)) return true;
    }
    OutStation = FRailwayStation();
    return false;
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
        return FailRoute(FString::Printf(TEXT("Cannot sample active route at %.2f cm: check track identity, geometry and connections"), Distance));
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
        // 行程结束后仍可沿现有计划退回。制动松开且驾驶员给反向牵引时才恢复运动。
        if (State == ERailwayState::Finished && DriveMode == ERailwayDriveMode::Manual &&
            DriveInput < 0.f && BrakeInput == 0.f && Distance > 0 && !ActivePlan.Sections.IsEmpty())
            SetState(ERailwayState::Running);
        if (State == ERailwayState::Dwelling && !RefreshDwell()) return;
        if (State == ERailwayState::Running && ActivePlan.Stops.IsValidIndex(NextStop))
        {
            FRailwayStation Upcoming;
            if (!Railway::Stations::Read(GetWorld(), ActivePlan.Stops[NextStop], Upcoming))
            {
                FailRoute(FString::Printf(TEXT("Upcoming station %s was removed, moved or became invalid"),
                    *ActivePlan.Stops[NextStop].Reference.StationId.ToString()));
                return;
            }
        }
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
