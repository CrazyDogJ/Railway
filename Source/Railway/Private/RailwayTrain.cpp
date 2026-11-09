#include "RailwayTrain.h"
#include "Algo/BinarySearch.h"
#if WITH_EDITOR
#include "UObject/Script.h"
#endif
#include "EngineUtils.h"
#include "Railway.h"
#include "RailwayTrack.h"
#include "RailwayTrainSaveGame.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

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

namespace
{
// Map an unwrapped distance to [0, Length) at the spline seam.
double WrapDistance(double Distance, double Length)
{
    const double Remainder = FMath::Fmod(Distance, Length);
    return Remainder < 0 ? Remainder + Length : Remainder;
}

FTransform RailPose(const FRailwayLeg& Part, double& Local)
{
    const USplineComponent* S = Part.Track->Spline;
    const double Length = S->GetSplineLength();
    Local = S->IsClosedLoop() && Length > 0 ? WrapDistance(Local, Length) : FMath::Clamp(Local, 0.0, Length);
    FTransform Pose = S->GetTransformAtDistanceAlongSpline(Local, ESplineCoordinateSpace::World);
    if (Part.bReverse) Pose.SetRotation(Pose.GetRotation() * FQuat(FVector::UpVector, PI));
    return Pose;
}

bool SampleSection(const FRailwaySection& Section, double Along, double& TrackDistance, FTransform& Pose)
{
    if (!IsValid(Section.Leg.Track) || !IsValid(Section.Leg.Track->Spline)) return false;
    TrackDistance = Section.EntryDistance + (Section.Leg.bReverse ? -Along : Along);
    Pose = RailPose(Section.Leg, TrackDistance);
    return true;
}

void PlaceTrain(ARailwayTrain& Train, const FTransform& Pose)
{
    if (!Train.GetActorLocation().Equals(Pose.GetLocation(), .0001) || !Train.GetActorQuat().Equals(Pose.GetRotation(), .000001))
        Train.SetActorLocationAndRotation(Pose.GetLocation(), Pose.GetRotation());
}
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
        if (Cast<ARailwayTrack>(GetAttachParentActor()))
            DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        return;
    }
    if (RootComponent->GetAttachParent() != First.Track->Spline)
        AttachToComponent(First.Track->Spline, FAttachmentTransformRules::KeepWorldTransform);
    PlaceTrain(*this, RailPose(First, Local));
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

void ARailwayTrain::UpdateVelocity(const bool bReverse) const
{
    if (RootComponent)
    {
        RootComponent->ComponentVelocity = GetActorForwardVector() * Speed;
    }
}

void ARailwayTrain::ResetPlan()
{
    ++RunRevision;
    Speed = 0;
    UpdateVelocity();
    ActivePlan = FRailwayPlan();
    Ends.Reset();
    Distance = Wait = 0;
    Leg = NextStop = 0;
    CurrentStation = NAME_None;
    bPreLeaveSent = false;
    ActivePlan.NetworkRevision = RunRevision;
}

bool ARailwayTrain::FailRoute(int32 Index, const TCHAR* Reason)
{
    ResetPlan();
    SetState(ERailwayState::InvalidRoute);
    PublishMoveState(true);
    UE_LOG(LogRailway, Warning, TEXT("%s: route[%d]: %s"), *GetName(), Index, Reason);
    return false;
}

bool ARailwayTrain::StartRoute()
{
    if (!HasAuthority())
    {
        return false;
    }
    
    for (const FRailwayLeg& Part : Route)
        if (IsValid(Part.Track)) Part.Track->AlignEndpoints(false);
    FRailwayPlan Plan;
    FString Error;
    if (!BuildRoutePlan(Route, Plan, Error) || !StartPreparedRoute(Plan, 0, Error))
        return FailRoute(INDEX_NONE, *Error);
    return true;
}

bool ARailwayTrain::BuildRoutePlan(const TArray<FRailwayLeg>& Legs, FRailwayPlan& OutPlan, FString& OutError) const
{
    OutPlan = FRailwayPlan();
    OutError.Reset();
    FRailwayPlan Plan;
    auto Reject = [&](int32 Index, const TCHAR* Reason)
    {
        OutError = FString::Printf(TEXT("route[%d]: %s"), Index, Reason);
        return false;
    };
    if (Legs.IsEmpty() || !GetWorld()) return Reject(0, TEXT("Empty route or no world"));
    TArray<double> Entries;
    TArray<double> Exits;
    for (int32 I = 0; I < Legs.Num(); ++I)
    {
        const FRailwayLeg& Part = Legs[I];
        ARailwayTrack* Track = Part.Track;
        if (!IsValid(Track)) return Reject(I, TEXT("Missing track"));
        if (!IsValid(Track->Spline) || Track->GetWorld() != GetWorld()) return Reject(I, TEXT("Invalid spline or world"));
        const USplineComponent* S = Track->Spline;
        const double Length = S->GetSplineLength();
        if (!FMath::IsFinite(Length) || S->GetNumberOfSplinePoints() < 2 || Length < 1)
            return Reject(I, TEXT("Track must have at least two points and be at least 1 cm long"));
        if (!S->GetComponentScale().Equals(FVector::OneVector, 0.0001))
            return Reject(I, TEXT("Use unit track scale; edit spline points to set length"));
        Entries.Add(Part.bReverse ? Length : 0);
        Exits.Add(Part.bReverse ? 0 : Length);
    }

    // Trim each pair at its common junction; a host track can be entered or left mid-spline.
    for (int32 I = 1; I < Legs.Num(); ++I)
    {
        const FRailwayLeg& From = Legs[I - 1];
        const FRailwayLeg& To = Legs[I];
        const bool bFromLoop = From.Track->Spline->IsClosedLoop();
        const bool bToLoop = To.Track->Spline->IsClosedLoop();
        if (bFromLoop && From.Track == To.Track)
        {
            if (From.bReverse != To.bReverse)
                return Reject(I, TEXT("Consecutive laps on the same track must use the same direction"));
            continue;
        }
        // A loop hosts junction actors anywhere along its arc. Only open tracks have endpoints.
        ARailwayJunction* Exit = nullptr;
        ARailwayJunction* Entry = nullptr;
        if (!bFromLoop) Exit = From.bReverse ? From.Track->StartJunction : From.Track->EndJunction;
        if (!bToLoop) Entry = To.bReverse ? To.Track->EndJunction : To.Track->StartJunction;
        ARailwayJunction* Joint = nullptr;
        if (IsValid(Exit) && Exit == Entry) Joint = Exit;
        else if (IsValid(Entry) && Entry->HostTrack == From.Track)
        {
            Joint = Entry;
            Exits[I-1] = From.Track->Spline->GetSplineLength() * Entry->TrackPosition;
        }
        else if (IsValid(Exit) && Exit->HostTrack == To.Track)
        {
            Joint = Exit;
            Entries[I] = To.Track->Spline->GetSplineLength() * Exit->TrackPosition;
        }
        if (!Joint) return Reject(I, TEXT("No shared junction between these directed legs"));
        const FVector A = From.Track->Spline->GetLocationAtDistanceAlongSpline(Exits[I-1], ESplineCoordinateSpace::World);
        const FVector B = To.Track->Spline->GetLocationAtDistanceAlongSpline(Entries[I], ESplineCoordinateSpace::World);
        if (FVector::Dist(A,B) > 1 || FVector::Dist(A,Joint->GetActorLocation()) > 1 || FVector::Dist(B,Joint->GetActorLocation()) > 1)
            return Reject(I, TEXT("Snap track endpoints to their junction"));
    }

    TMultiMap<ARailwayTrack*, FRailwayStation> Markers;
    for (TActorIterator<ARailwayStationMarker> It(GetWorld()); It; ++It)
    {
        if (!IsValid(It->HostTrack)) continue;
        FRailwayStation Station;
        Station.Position = It->TrackPosition;
        Station.Name = It->StationName;
        Station.DwellSeconds = It->DwellSeconds;
        Markers.Add(It->HostTrack, Station);
    }

    double Base = 0;
    for (int32 I = 0; I < Legs.Num(); ++I)
    {
        const FRailwayLeg& Part = Legs[I];
        const double Direction = Part.bReverse ? -1.0 : 1.0;
        const bool bLoop = Part.Track->Spline->IsClosedLoop();
        const double Length = Part.Track->Spline->GetSplineLength();
        double Travel = Direction * (Exits[I] - Entries[I]);
        if (bLoop)
        {
            Travel = WrapDistance(Travel, Length);
            // Entering and leaving at the same point means one complete circuit.
            if (Travel == 0.0) Travel = Length;
        }
        if (Travel <= UE_SMALL_NUMBER) return Reject(I, TEXT("Check junction order and Reverse flag on this leg"));
        TArray<FRailwayStation> Stations = Part.Track->Stations;
        for (auto It = Markers.CreateConstKeyIterator(Part.Track.Get()); It; ++It)
            Stations.Add(It.Value());
        for (const FRailwayStation& Station : Stations)
        {
            if (!FMath::IsFinite(Station.Position) || Station.Position < 0 || Station.Position > 1 ||
                !FMath::IsFinite(Station.DwellSeconds) || Station.DwellSeconds < 0)
                return Reject(I, TEXT("Invalid station position or dwell time"));
            const double Local = Part.Track->Spline->GetSplineLength() * Station.Position;
            double Along = Direction * (Local - Entries[I]);
            if (bLoop)
            {
                Along = WrapDistance(Along, Length);
                // Serve a station at the lap boundary once, on return.
                if (Along == 0.0 && Travel == Length) Along = Length;
            }
            if (Along >= 0 && Along <= Travel)
                Plan.Stops.Add(FRailwayPlannedStop{Base + Along, Station});
        }
        Base += Travel;
        Plan.Sections.Add(FRailwaySection{Part, Entries[I], Travel});
    }
    OutPlan = MoveTemp(Plan);
    return true;
}

bool ARailwayTrain::StartPreparedRoute(const FRailwayPlan& Plan, double StartDistance, FString& OutError)
{
    if (!HasAuthority())
    {
        return false;
    }
    
    return BeginPlan(Plan, StartDistance, nullptr, OutError);
}

bool ARailwayTrain::BeginPlan(const FRailwayPlan& Plan, double StartDistance, const FRailwayTrainSaveGame* Saved, FString& OutError)
{
    OutError.Reset();
    TArray<double> NewEnds;
    double Total = 0;
    FVector PreviousExit = FVector::ZeroVector;
    for (const FRailwaySection& Section : Plan.Sections)
    {
        const ARailwayTrack* Track = Section.Leg.Track;
        if (!IsValid(Track) || !IsValid(Track->Spline) || Track->GetWorld() != GetWorld())
        {
            OutError = TEXT("Section has no valid track in this world");
            return false;
        }
        const USplineComponent* S = Track->Spline;
        const double Length = S->GetSplineLength();
        double Entry = Section.EntryDistance;
        double Exit = Entry + (Section.Leg.bReverse ? -Section.TravelDistance : Section.TravelDistance);
        if (!FMath::IsFinite(Length) || Length < 1 || S->GetNumberOfSplinePoints() < 2 ||
            !S->GetComponentScale().Equals(FVector::OneVector, .0001) ||
            !FMath::IsFinite(Entry) || Entry < 0 || Entry > Length ||
            !FMath::IsFinite(Section.TravelDistance) || Section.TravelDistance <= 0 ||
            Section.TravelDistance > Length + UE_SMALL_NUMBER ||
            (!S->IsClosedLoop() && (Exit < -UE_SMALL_NUMBER || Exit > Length + UE_SMALL_NUMBER)))
        {
            OutError = TEXT("Invalid track geometry or section distance");
            return false;
        }
        const FVector Start = RailPose(Section.Leg, Entry).GetLocation();
        if (!NewEnds.IsEmpty() && FVector::Dist(Start, PreviousExit) > 1)
        {
            OutError = TEXT("Gap between consecutive sections exceeds 1 cm");
            return false;
        }
        PreviousExit = RailPose(Section.Leg, Exit).GetLocation();
        Total += Section.TravelDistance;
        NewEnds.Add(Total);
    }
    if (NewEnds.IsEmpty() || !FMath::IsFinite(StartDistance) || StartDistance < 0 || StartDistance > Total)
    {
        OutError = TEXT("Empty plan or start outside the route");
        return false;
    }
    for (const FRailwayPlannedStop& Stop : Plan.Stops)
    {
        if (!FMath::IsFinite(Stop.Distance) || Stop.Distance < 0 || Stop.Distance > Total ||
            !FMath::IsFinite(Stop.Station.DwellSeconds) || Stop.Station.DwellSeconds < 0)
        {
            OutError = TEXT("Invalid planned stop distance or dwell time");
            return false;
        }
    }

    FRailwayPlan SortedPlan = Plan;
    SortedPlan.Stops.StableSort([](const FRailwayPlannedStop& A, const FRailwayPlannedStop& B) { return A.Distance < B.Distance; });
    if (Saved)
    {
        const int32 Cursor = Saved->NextStop;
        if (Cursor < 0 || Cursor > SortedPlan.Stops.Num() ||
            (Cursor > 0 && SortedPlan.Stops[Cursor-1].Distance > StartDistance) ||
            (SortedPlan.Stops.IsValidIndex(Cursor) && SortedPlan.Stops[Cursor].Distance < StartDistance) ||
            (Saved->State == ERailwayState::Dwelling && (Cursor == 0 || SortedPlan.Stops[Cursor-1].Distance != StartDistance)) ||
            (Saved->State == ERailwayState::Finished && (StartDistance != Total || Cursor != SortedPlan.Stops.Num())) ||
            !FMath::IsFinite(Saved->Speed) || Saved->Speed < 0 || !FMath::IsFinite(Saved->Wait) || Saved->Wait < 0)
        {
            OutError = TEXT("Invalid train progress in save");
            return false;
        }
    }

    ++RunRevision;
    ActivePlan = MoveTemp(SortedPlan);
    ActivePlan.NetworkRevision = RunRevision;
    Ends = MoveTemp(NewEnds);
    Distance = StartDistance;
    Speed = 0;
    Wait = 0;
    bPreLeaveSent = false;
    NextStop = 0;
    while (ActivePlan.Stops.IsValidIndex(NextStop) && ActivePlan.Stops[NextStop].Distance < Distance) ++NextStop;
    CurrentStation = NAME_None;
    const ERailwayState InitialState = Saved ? Saved->State : ERailwayState::Running;
    if (Saved)
    {
        NextStop = Saved->NextStop;
        if (InitialState == ERailwayState::Running) Speed = Saved->Speed;
        if (InitialState == ERailwayState::Dwelling)
        {
            CurrentStation = ActivePlan.Stops[NextStop-1].Station.Name;
            Wait = Saved->Wait;
            bPreLeaveSent = Saved->bPreLeaveSent;
        }
    }
    if (Cast<ARailwayTrack>(GetAttachParentActor()))
        DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    if (!UpdatePose()) return false;
    SetState(InitialState);
    PublishMoveState(true);
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
    SectionIndex = INDEX_NONE;
    TrackDistance = 0;
    Pose = FTransform::Identity;
    if (!FMath::IsFinite(RouteDistance) || RouteDistance < 0) return false;
    double Start = 0;
    for (int32 I = 0; I < Plan.Sections.Num(); ++I)
    {
        const FRailwaySection& Section = Plan.Sections[I];
        if (!FMath::IsFinite(Section.TravelDistance) || Section.TravelDistance <= 0) return false;
        const double End = Start + Section.TravelDistance;
        if (RouteDistance < End || (I == Plan.Sections.Num()-1 && RouteDistance == End))
        {
            if (!SampleSection(Section, RouteDistance - Start, TrackDistance, Pose)) return false;
            SectionIndex = I;
            return true;
        }
        Start = End;
    }
    return false;
}

FRailwayTrainSaveGame ARailwayTrain::GetSaveGameData() const
{
    if (!HasAuthority())
    {
        return FRailwayTrainSaveGame();
    }
    
    return FRailwayTrainSaveGame::SaveTrain(this);
}

void ARailwayTrain::LoadSaveGameData(const FRailwayTrainSaveGame InSaveGame)
{
    if (!HasAuthority())
    {
        return;
    }
    
    if (InSaveGame.bValid)
    {
        InSaveGame.LoadTrain(this);
    }
}

bool ARailwayTrain::SampleRoute(double RouteDistance, int32& SectionIndex, double& TrackDistance, FTransform& Pose) const
{
    SectionIndex = INDEX_NONE;
    TrackDistance = 0;
    Pose = FTransform::Identity;
    if (Ends.IsEmpty() || !FMath::IsFinite(RouteDistance) || RouteDistance < 0 || RouteDistance > Ends.Last()) return false;
    const int32 Index = FMath::Min(Algo::UpperBound(Ends, RouteDistance), Ends.Num() - 1);
    const FRailwaySection& Section = ActivePlan.Sections[Index];
    const double Along = RouteDistance - (Index ? Ends[Index-1] : 0.);
    if (!SampleSection(Section, Along, TrackDistance, Pose)) return false;
    SectionIndex = Index;
    return true;
}

bool ARailwayTrain::UpdatePose()
{
    FTransform Pose;
    double Local = 0;
    if (!SampleRoute(Distance, Leg, Local, Pose)) return FailRoute(Leg, TEXT("Invalid active track or route distance"));
    PlaceTrain(*this, Pose);
    UpdateVelocity();
    return true;
}

void ARailwayTrain::Step(double Dt)
{
    const uint32 Revision = RunRevision;
    if (State == ERailwayState::Dwelling)
    {
        const double Consumed = FMath::Min(Wait, Dt);
        Wait -= Consumed;
        Dt -= Consumed;
        if (!bPreLeaveSent && Wait <= FMath::Max(0.f, PreStationLeaveTime))
        {
            bPreLeaveSent = true;
            PreStationLeave.Broadcast(CurrentStation);
            if (RunRevision != Revision) return;
        }
        if (Wait > 0) return;
        OnStationLeave.Broadcast(CurrentStation);
        if (RunRevision != Revision) return;
        CurrentStation = NAME_None;
        SetState(ERailwayState::Running);
        if (RunRevision != Revision) return;
    }

    const double Target = ActivePlan.Stops.IsValidIndex(NextStop) ? ActivePlan.Stops[NextStop].Distance : Ends.Last();
    const double Remaining = Target - Distance;
    const double Brake = FMath::Max(1.f, Braking);
    // Reserve the next step's travel as well as the continuous braking distance.
    const double BrakeStep = Brake * Dt;
    const double SafeSpeed = FMath::Sqrt(BrakeStep * BrakeStep + 2 * Brake * Remaining) - BrakeStep;
    const double Requested = FMath::Max(0.f, ExternalSpeedLimit >= 0 ? FMath::Min(ExternalSpeedLimit, CruiseSpeed) : CruiseSpeed);
    const double CruiseDelta = (Requested - Speed) * (1.0 - FMath::Exp(-Dt / FMath::Max(.01f, ResponseTime)));
    const double Desired = FMath::Min(Speed + CruiseDelta, SafeSpeed);
    Speed += FMath::Clamp(Desired - Speed, -BrakeStep, FMath::Max(1.f, Acceleration) * Dt);
    Distance += FMath::Min(Remaining, Speed * Dt);
    const bool bArrived = Target - Distance <= 0.01;
    if (bArrived)
    {
        Distance = Target;
        Speed = 0;
    }
    if (!UpdatePose() || !bArrived) return;

    if (ActivePlan.Stops.IsValidIndex(NextStop))
    {
        const FRailwayStation Station = ActivePlan.Stops[NextStop++].Station;
        Wait = Station.DwellSeconds;
        bPreLeaveSent = false;
        CurrentStation = Station.Name;
        SetState(ERailwayState::Dwelling);
        if (RunRevision == Revision) OnStationArrival.Broadcast(CurrentStation);
    }
    else
    {
        SetState(ERailwayState::Finished);
    }
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
        // Only server deal with these things
        double Left = DeltaSeconds;
        while (Left > 0 && (State == ERailwayState::Running || State == ERailwayState::Dwelling))
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

void ARailwayTrain::PublishMoveState(bool bForce)
{
    if (!HasAuthority() || GetNetMode() == NM_Standalone) return;
    const double Now = GetWorld()->GetTimeSeconds(); // Same clock as GameState's server time.
    const bool bTransition = TrainMoveState.PlanRevision != RunRevision ||
        TrainMoveState.State != State || TrainMoveState.NextStop != NextStop;
    if (!bForce && !bTransition)
    {
        if (State != ERailwayState::Running || Now - TrainMoveState.ServerTime < 1.0 / FMath::Clamp(SnapshotRateHz, 1.f, 60.f))
            return;
    }
    TrainMoveState.PlanRevision = RunRevision;
    TrainMoveState.ServerTime = Now;
    TrainMoveState.Distance = Distance;
    TrainMoveState.Speed = Speed;
    TrainMoveState.State = State;
    TrainMoveState.NextStop = NextStop;
    TrainMoveState.DwellEndTime = State == ERailwayState::Dwelling ? Now + Wait : 0;
    if (bForce || bTransition) ForceNetUpdate();
}

void ARailwayTrain::OnRep_ActivePlan()
{
    Ends.Reset(ActivePlan.Sections.Num());
    double Total = 0;
    for (const FRailwaySection& Section : ActivePlan.Sections)
    {
        Total += Section.TravelDistance;
        Ends.Add(Total);
    }
    // OnRep order is unspecified. Only consume a matched plan/sample in the client tick.
}

void ARailwayTrain::OnRep_TrainMoveState()
{
    if (MoveSamples.IsEmpty() || MoveSamples.Last().PlanRevision != TrainMoveState.PlanRevision)
    {
        MoveSamples.Reset();
        LastRenderTime = -1;
    }
    else if (TrainMoveState.ServerTime <= MoveSamples.Last().ServerTime)
    {
        if (TrainMoveState.ServerTime == MoveSamples.Last().ServerTime) MoveSamples.Last() = TrainMoveState;
        return;
    }
    // Bounded even when tracks stream in late or the client has no game state yet.
    if (MoveSamples.Num() == 64) MoveSamples.RemoveAt(0, 1, EAllowShrinking::No);
    MoveSamples.Add(TrainMoveState);
}

void ARailwayTrain::TickClientPresentation()
{
    if (MoveSamples.IsEmpty() || MoveSamples.Last().PlanRevision != ActivePlan.NetworkRevision) return;
    const FRailwayMoveState& Newest = MoveSamples.Last();
    const AGameStateBase* Clock = GetWorld()->GetGameState();
    if (!Clock) return;
    // Holding the newest sample during starvation also prevents overshooting a station.
    const double RequestedTime = Clock->GetServerWorldTimeSeconds() - FMath::Clamp(InterpolationDelay, 0.f, 0.5f);
    const double Time = FMath::Clamp(FMath::Max(LastRenderTime, RequestedTime), MoveSamples[0].ServerTime, Newest.ServerTime);
    while (MoveSamples.Num() > 1 && MoveSamples[1].ServerTime <= Time)
        MoveSamples.RemoveAt(0, 1, EAllowShrinking::No);

    const FRailwayMoveState& A = MoveSamples[0];
    double RenderDistance = A.Distance;
    float RenderSpeed = A.Speed;
    if (MoveSamples.Num() > 1)
    {
        const FRailwayMoveState& B = MoveSamples[1];
        // A lost departure sample must not make the train creep during a long dwell.
        const double Start = A.State == ERailwayState::Dwelling ?
            FMath::Clamp(A.DwellEndTime, A.ServerTime, B.ServerTime) : A.ServerTime;
        const double Alpha = Start < B.ServerTime ? FMath::Clamp((Time - Start) / (B.ServerTime - Start), 0.0, 1.0) : 0;
        RenderDistance = FMath::Lerp(A.Distance, B.Distance, Alpha);
        RenderSpeed = FMath::Lerp(A.Speed, B.Speed, static_cast<float>(Alpha));
    }

    int32 SectionIndex = 0;
    if (!ActivePlan.Sections.IsEmpty())
    {
        FTransform Pose;
        double Local;
        // Net references may be unresolved on the first tick. Retry without changing train state.
        if (!SampleRoute(RenderDistance, SectionIndex, Local, Pose)) return;
        if (Cast<ARailwayTrack>(GetAttachParentActor()))
            DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        PlaceTrain(*this, Pose);
    }
    else if (A.State != ERailwayState::Idle && A.State != ERailwayState::InvalidRoute) return;

    LastRenderTime = Time;
    Leg = SectionIndex;
    Distance = RenderDistance;
    Speed = RenderSpeed;
    NextStop = A.NextStop;
    const bool bDwelling = A.State == ERailwayState::Dwelling;
    Wait = bDwelling ? FMath::Max(0.0, A.DwellEndTime - FMath::Max(Time, RequestedTime)) : 0;
    CurrentStation = bDwelling && ActivePlan.Stops.IsValidIndex(NextStop-1) ?
        ActivePlan.Stops[NextStop-1].Station.Name : NAME_None;
    UpdateVelocity();
    SetState(A.State);
}

void ARailwayTrain::GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    
    // Update transform for client.
    DOREPLIFETIME(ThisClass, ActivePlan);
    DOREPLIFETIME(ThisClass, TrainMoveState);
}
