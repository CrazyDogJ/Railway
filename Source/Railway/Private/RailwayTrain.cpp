#include "RailwayTrain.h"

#include "Algo/BinarySearch.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Railway.h"
#include "RailwayRouteGeometry.h"
#include "RailwayRoutePlanner.h"
#include "RailwayDataActor.h"
#include "EngineUtils.h"
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
    bIsSpatiallyLoaded = false;
    SetReplicatingMovement(false);
}

ARailwayDataActor* ARailwayTrain::GetRailwayData() const
{
    if (RailwayData) return IsValid(RailwayData) && RailwayData->GetWorld() == GetWorld() ? RailwayData.Get() : nullptr;
    if (CachedRailwayData.IsValid()) return CachedRailwayData.Get();
    if (!GetWorld()) return nullptr;
    ARailwayDataActor* SameLevel = nullptr;
    ARailwayDataActor* Candidate = nullptr;
    int32 Count = 0;
    for (TActorIterator<ARailwayDataActor> It(GetWorld()); It; ++It)
    {
        Candidate = *It;
        ++Count;
        if (Candidate->GetLevel() == GetLevel())
        {
            if (SameLevel) return nullptr;
            SameLevel = Candidate;
        }
    }
    CachedRailwayData = SameLevel ? SameLevel : (Count == 1 ? Candidate : nullptr);
    return CachedRailwayData.Get();
}

bool ARailwayTrain::GetPreviewLocation_Implementation(FRailwayLeg& PreviewLeg, double& TrackDistance) const
{
    PreviewLeg = Route.IsEmpty() ? FRailwayLeg() : Route[0];
    TrackDistance = 0;
    const ARailwayDataActor* Data = GetRailwayData();
    if (!Data || !Data->Network.Edges.IsValidIndex(PreviewLeg.EdgeIndex)) return false;
    TrackDistance = Data->Network.Edges[PreviewLeg.EdgeIndex].StartDistance;
    return true;
}

#if WITH_EDITOR
void ARailwayTrain::RefreshPreview()
{
    FEditorScriptExecutionGuard ScriptGuard;
    FRailwayLeg First;
    double Local = 0;
    const ARailwayDataActor* Data = GetRailwayData();
    if (!Data || !GetPreviewLocation(First, Local) || !Data->Network.Edges.IsValidIndex(First.EdgeIndex)) return;
    const FRailwayEdge& Edge = Data->Network.Edges[First.EdgeIndex];
    FTransform Pose;
    if (Data->SampleTrack({Edge.TrackIndex, Local}, Edge.IsReverse(), Pose))
        Railway::Placement::ApplyPose(*this, Pose);
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
        ResolvePlannedStop(ActivePlan.Stops[NextStop - 1], Current)) return Current.Name;
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
    FRailwayPlan Plan;
    FString Error;
    if (!BuildRoutePlan(Route, Plan, Error) || !RestartPreparedRouteAtDistance(Plan, 0, Error)) return FailRoute(Error);
    return true;
}

bool ARailwayTrain::BuildRoutePlan(const TArray<FRailwayLeg>& Legs, FRailwayPlan& OutPlan, FString& OutError) const
{
    return Railway::Routing::Build(GetRailwayData(), Legs, TOptional<double>(), OutPlan, OutError);
}

bool ARailwayTrain::FindRoutePlan(const FRailwayTrackLocation& Start, const FRailwayTrackLocation& Destination,
    FRailwayPlan& OutPlan, FString& OutError, bool bStopAtIntermediateStations) const
{
    const ARailwayDataActor* Data = GetRailwayData();
    if (Data) return Data->FindRoutePlan(Start, Destination, OutPlan, OutError, bStopAtIntermediateStations);
    OutPlan = {};
    OutError = TEXT("Select or build this level's railway data");
    return false;
}

bool ARailwayTrain::FindRoutePlanToStation(const FRailwayTrackLocation& Start, int32 StationIndex,
    FRailwayPlan& OutPlan, FString& OutError, bool bStopAtIntermediateStations) const
{
    const ARailwayDataActor* Data = GetRailwayData();
    if (Data) return Data->FindRoutePlanToStation(Start, StationIndex, OutPlan, OutError, bStopAtIntermediateStations);
    OutPlan = {};
    OutError = TEXT("Select or build this level's railway data");
    return false;
}

bool ARailwayTrain::StartPreparedRoute(const FRailwayPlan& Plan, double StartDistance, FString& OutError)
{
    if (!ActivePlan.Sections.IsEmpty()) return ReplacePreparedRoute(Plan, OutError);
    return RestartPreparedRouteAtDistance(Plan, StartDistance, OutError);
}

bool ARailwayTrain::RestartPreparedRouteAtDistance(const FRailwayPlan& Plan, double StartDistance, FString& OutError)
{
    if (!HasAuthority())
    {
        OutError = TEXT("Starting a route requires authority");
        return false;
    }
    FRailwayPreparedPlan Prepared;
    if (!Railway::Routing::Prepare(GetRailwayData(), Plan, StartDistance, Prepared, OutError)) return false;
    // 严格小于起点的站才算跳过，恰好在起点的多个站仍按顺序服务。
    const int32 StopCursor = Algo::LowerBoundBy(Prepared.Plan.Stops, StartDistance, &FRailwayPlannedStop::Distance);
    const bool bAlreadyAtDestination = Prepared.SectionEnds.Last() == 0.0 && Prepared.Plan.Stops.IsEmpty();
    ResetMotion();
    ActivatePlan(MoveTemp(Prepared), StopCursor,
        bAlreadyAtDestination ? ERailwayState::Finished : ERailwayState::Running);
    return true;
}

void ARailwayTrain::ActivatePlan(FRailwayPreparedPlan&& Prepared, int32 StopCursor, ERailwayState NewState)
{
    InstallPlan(MoveTemp(Prepared.Plan));
    SectionEnds = MoveTemp(Prepared.SectionEnds);
    Distance = Prepared.StartDistance;
    Leg = Prepared.StartSection;
    NextStop = StopCursor;
    CurrentStation = GetStationForState(NewState);
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
    // 裁剪停站表不会改变几何；这里的累计里程仍属于同一组区间。
    FRailwayPreparedPlan Prepared;
    if (!Railway::Routing::Prepare(GetRailwayData(), MoveTemp(Direct), Distance, Prepared, OutError)) return false;
    return ContinuePreparedPlan(MoveTemp(Prepared), false, OutError);
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
    const ARailwayDataActor* Data = GetRailwayData();
    if (!Data || !Railway::Geometry::Locate(Data->Network, ActivePlan, SectionEnds, Distance, SectionIndex, Local))
    {
        OutError = TEXT("Cannot locate the train on its current track");
        return false;
    }
    const FRailwayLeg& Current = ActivePlan.Sections[SectionIndex].Leg;
    if (NewLegs[0].EdgeIndex != Current.EdgeIndex)
    {
        OutError = TEXT("The first leg must match the occupied track and direction");
        return false;
    }
    FRailwayPlan RemainingPlan;
    if (!Railway::Routing::Build(GetRailwayData(), NewLegs, TOptional<double>(Local), RemainingPlan, OutError)) return false;

    // Build 已将占用点裁为剩余行程的起点，含恰好在岔口换轨的情况。
    FRailwayPreparedPlan Prepared;
    if (!Railway::Routing::Prepare(GetRailwayData(), MoveTemp(RemainingPlan), 0, Prepared, OutError) ||
        !ContinuePreparedPlan(MoveTemp(Prepared), true, OutError)) return false;
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
    const ARailwayDataActor* Data = GetRailwayData();
    if (Data && Data->Network.Stations.IsValidIndex(Stop.StationIndex))
    {
        OutStation = Data->Network.Stations[Stop.StationIndex].Definition;
        return true;
    }
    OutStation = FRailwayStation();
    return false;
}

bool ARailwayTrain::SamplePlan(const FRailwayPlan& Plan, double RouteDistance, int32& SectionIndex, double& TrackDistance, FTransform& Pose) const
{
    TArray<double> Ends;
    Railway::Geometry::BuildSectionEnds(Plan, Ends);
    return Railway::Geometry::Sample(GetRailwayData(), Plan, Ends, RouteDistance, SectionIndex, TrackDistance, Pose);
}

bool ARailwayTrain::SampleRoute(double RouteDistance, int32& SectionIndex, double& TrackDistance, FTransform& Pose) const
{
    return Railway::Geometry::Sample(GetRailwayData(), ActivePlan, SectionEnds, RouteDistance, SectionIndex, TrackDistance, Pose);
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
