#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "RailwayDispatchFixtures.h"
#include "RailwayDataActor.h"
#include "RailwayPlanRequest.h"
#include "RailwayTrain.h"
#include "Tests/NetTestHelpers.h"

namespace
{
ARailwayDataActor* MakeFork(UWorld& World)
{
    auto* D = Railway::Tests::MakeNetwork(World);
    auto& N = D->Network;
    const int32 Joint = N.Nodes.Add(FVector(5000,0,0));
    const int32 End = N.Nodes.Add(FVector(10000,2000,0));
    N.JunctionByNode.Add(Joint, FGuid::NewGuid());
    N.Tracks[0].NumEdges = 4;
    auto& Branch = N.Tracks.AddDefaulted_GetRef();
    Branch.Name = TEXT("Siding");
    Branch.Id = FGuid::NewGuid();
    N.TrackById.Add(Branch.Id, 1);
    for (int32 I = 0; I < 2; ++I)
    {
        Branch.Curves.Position.AddPoint(I, N.Nodes[I ? End : Joint]);
        Branch.Curves.Rotation.AddPoint(I, FQuat::Identity);
        Branch.Curves.Scale.AddPoint(I, FVector::OneVector);
    }
    Branch.Curves.UpdateSpline();
    Branch.FirstEdge = 4;
    Branch.NumEdges = 2;
    N.Edges.Reset();
    N.Edges.Add({0, 0, Joint, 0, 5000, 0, 2});
    N.Edges.Add({0, Joint, 0, 5000, 0});
    N.Edges.Add({0, Joint, 1, 5000, 10000});
    N.Edges.Add({0, 1, Joint, 10000, 5000, 2, 1});
    N.Edges.Add({1, Joint, End, 0, Branch.Length()});
    N.Edges.Add({1, End, Joint, Branch.Length(), 0, 3, 1});
    N.Successors = {2,4,1,1};
    N.Stations[1].EdgeIndex = N.Stations[2].EdgeIndex = 2;
    return D;
}

void Simulate(ARailwayTrain* T, int32 Frames)
{
    for (int32 I = 0; I < Frames; ++I) T->Tick(1.f / 60);
}

FRailwayPlanCommand DrivingCommand(ARailwayTrain* T, ERailwayPlanAction Action)
{
    FRailwayPlanCommand Cmd;
    Cmd.Action = Action;
    Cmd.ExpectedRevision = T->GetPlanRevision();
    Cmd.BuildId = T->GetRailwayData()->Network.BuildId;
    return Cmd;
}

bool StartAt(ARailwayTrain* T, double Distance, FString& Error)
{
    FRailwayPlan Plan;
    return T->CompileRouteSettings(T->Route, Plan, Error) && T->RestartPreparedRouteAtDistance(Plan, Distance, Error);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayReverseMotion, "Railway.Driving.ReverseDestination",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRailwayReverseMotion::RunTest(const FString&)
{
    auto* W = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { W->DestroyWorld(false); };
    auto* Data = MakeFork(*W);
    auto* T = Railway::Tests::MakeTrain(*W, Data);
    auto* PC = W->SpawnActor<APlayerController>();
    FString Error;
    if (!TestTrue(TEXT("Start on main"), StartAt(T, 7000, Error))) return false;
    Simulate(T, 60);
    const FVector Before = T->GetActorLocation();
    const FVector Head = T->GetActorForwardVector();
    const double Speed = T->Speed;
    TestTrue(TEXT("Moving forward before retarget"), Speed > 100);
    auto Goal = DrivingCommand(T, ERailwayPlanAction::SetDestination);
    Goal.SourceId = Data->Network.Stations[1].Id;
    if (!T->ApplyPlayerPlanCommand(PC, Goal, Error)) { AddError(Error); return false; }
    TestTrue(TEXT("Replan preserves cab, speed and physical position"),
        T->GetActorLocation().Equals(Before, .01) && Head.Equals(T->GetActorForwardVector(), .001) &&
        T->Speed == Speed && T->ActivePlan.bCabReversed);
    Simulate(T, 8);
    TestTrue(TEXT("Physically brakes in original direction"), T->GetActorLocation().X > Before.X && T->Speed > 0 && T->Speed < Speed);
    Simulate(T, 130);
    TestTrue(TEXT("Backs towards station with original cab leading axis"), T->Speed < 0 &&
        T->GetRootComponent()->ComponentVelocity.X < 0 && T->GetActorForwardVector().Equals(Head,.001));
    const auto Save = T->GetSaveGameData();
    auto* Restored = Railway::Tests::MakeTrain(*W, Data);
    TestTrue(TEXT("Restore backward service"), Restored->TryLoadSaveGameData(Save, Error));
    TestTrue(TEXT("Restored cab and signed speed"), Restored->GetActorForwardVector().Equals(Head,.001) && Restored->Speed == T->Speed);
    const float ReverseSpeed = Restored->Speed;
    const double BeforeRelease = Restored->GetActorLocation().X;
    if (!Restored->ApplyPlayerPlanCommand(PC, DrivingCommand(Restored, ERailwayPlanAction::CancelRoute), Error)) { AddError(Error); return false; }
    Restored->RollingResistance = 180;
    Simulate(Restored, 10);
    TestTrue(TEXT("Automatic reverse service coasts backwards after release"), Restored->Speed < 0 && Restored->Speed > ReverseSpeed &&
        Restored->GetActorLocation().X < BeforeRelease && Restored->GetActorForwardVector().Equals(Head,.001));
    Simulate(Restored, 300);
    TestTrue(TEXT("Rolling resistance brings unpowered train to rest"), Restored->Speed == 0);
    for (int32 I = 0; I < 1800 && T->State == ERailwayState::Running; ++I) Simulate(T, 1);
    TestTrue(TEXT("Arrives in reverse at the requested passed station"), T->State == ERailwayState::Dwelling &&
        FMath::IsNearlyEqual(T->GetActorLocation().X, 5000., .1) && T->GetActorForwardVector().Equals(Head,.001));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayBrakingAcrossTurnout, "Railway.Driving.BrakingAcrossTurnout",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRailwayBrakingAcrossTurnout::RunTest(const FString&)
{
    auto* W = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { W->DestroyWorld(false); };
    auto* D = MakeFork(*W);
    auto* T = Railway::Tests::MakeTrain(*W, D);
    auto* PC = W->SpawnActor<APlayerController>();
    T->Route.Tracks.Add({{D->Network.Tracks[1].Id}, false, -1, -1});
    T->Route.bStopAtIntermediateStations = false;
    FString Error;
    if (!StartAt(T, 4800, Error)) { AddError(Error); return false; }
    T->Speed = 480;
    auto Goal = DrivingCommand(T, ERailwayPlanAction::SetDestination);
    Goal.SourceId = D->Network.Tracks[0].Id;
    Goal.Distance = 4000;
    if (!T->ApplyPlayerPlanCommand(PC, Goal, Error)) { AddError(Error); return false; }
    Simulate(T, 55);
    FRailwayTrackLocation At;
    if (!TestTrue(TEXT("Braking run-out uses the old siding, even though the default is straight"),
        T->GetCurrentTrackLocation(At) && At.TrackIndex == 1 && At.TrackDistance > 0 && T->Speed > 0 && T->GetActorLocation().Y > 0)) return false;
    Simulate(T, 600);
    return TestTrue(TEXT("Reverses from siding back onto the original approach to the goal"),
        T->State == ERailwayState::Finished && T->GetCurrentTrackLocation(At) && At.TrackIndex == 0 &&
        FMath::IsNearlyEqual(At.TrackDistance, 4000., .01) && T->GetActorForwardVector().X > .999);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayTurnoutMotion, "Railway.Driving.ManualTurnoutAndCoasting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRailwayTurnoutMotion::RunTest(const FString&)
{
    UE::Net::FTestWorlds Worlds;
    auto* W = Worlds.Server.GetWorld();
    auto* D = MakeFork(*W);
    auto* T = Railway::Tests::MakeTrain(*W, D);
    auto* PC = W->SpawnActor<APlayerController>();
    auto Switch = [&](const FRailwayTrackChoice& C, ERailwayPlanAction Action)
    {
        auto Cmd = DrivingCommand(T, Action);
        Cmd.ExpectedRevision = C.View.Revision;
        Cmd.ApproachEdge = C.ApproachEdge;
        Cmd.OutgoingEdge = C.OutgoingEdge;
        Cmd.ExpectedSelectedEdge = C.SelectedEdge;
        Cmd.bTowardsRear = C.bTowardsRear;
        FString Reason;
        return T->ApplyPlayerPlanCommand(PC, Cmd, Reason);
    };
    FString Error;
    if (!StartAt(T, 4400, Error)) { AddError(Error); return false; }
    T->ServerSetDriveMode(ERailwayDriveMode::Manual);
    T->ServerSetDriveInput(1);
    if (!TestTrue(TEXT("Drive inputs applied in a playing world"), T->DriveMode == ERailwayDriveMode::Manual && T->DriveInput == 1)) return false;
    Simulate(T, 60);
    T->ServerSetDriveInput(0);
    const FVector Before = T->GetActorLocation();
    const float Speed = T->Speed;
    auto Cancel = DrivingCommand(T, ERailwayPlanAction::CancelRoute);
    if (!TestTrue(TEXT("Disengage moving service"), T->ApplyPlayerPlanCommand(PC, Cancel, Error))) return false;
    TestTrue(TEXT("Disengaging keeps velocity and pose"), T->State == ERailwayState::Idle && Speed > 0 &&
        Speed == T->Speed && T->GetActorLocation().Equals(Before, .001));
    Simulate(T, 30);
    TestTrue(TEXT("Coasts forward under rolling resistance"), T->GetActorLocation().X > Before.X && T->Speed > 0 && T->Speed < Speed);
    TArray<FRailwayTrackChoice> Options;
    T->GetUpcomingTrackChoices(false, Options);
    if (!TestTrue(TEXT("Through track and branch available before junction"), Options.Num()==2 && Options[0].bActive && !Options[1].bActive)) return false;
    const auto Stale = Options[0];
    TestTrue(TEXT("Activate branch"), Switch(Options[1], ERailwayPlanAction::ActivateTrack));
    T->GetUpcomingTrackChoices(false, Options);
    TestTrue(TEXT("Selected siding becomes active"), Options[1].bActive && !Options[0].bActive);
    TestFalse(TEXT("Stale choice rejected"), Switch(Stale, ERailwayPlanAction::DeactivateTrack));
    TestTrue(TEXT("Deactivate branch"), Switch(Options[1], ERailwayPlanAction::DeactivateTrack));
    T->GetUpcomingTrackChoices(false, Options);
    TestTrue(TEXT("Deactivate siding restores through track"), Options[0].bActive);
    TestTrue(TEXT("Activate branch"), Switch(Options[1], ERailwayPlanAction::ActivateTrack));
    T->GetUpcomingTrackChoices(true, Options);
    TestTrue(TEXT("No illegal turnout behind this cab"), Options.IsEmpty());
    const auto Save = T->GetSaveGameData();
    D->Network.BuildId = FGuid::NewGuid();
    auto* Restored = Railway::Tests::MakeTrain(*W, D);
    if (!TestTrue(TEXT("Rebind moving free train after rebake"), Restored->TryLoadSaveGameData(Save, Error))) return false;
    Restored->GetUpcomingTrackChoices(false, Options);
    TestTrue(TEXT("Momentum and selected exit survived rebake"), Restored->Speed == T->Speed && Options.Num()==2 && Options[1].bActive);
    Restored->ServerSetDriveInput(1);
    Simulate(Restored, 180);
    FRailwayTrackLocation At;
    TestTrue(TEXT("Transitions only at junction, continues down selected branch"), Restored->GetCurrentTrackLocation(At) &&
        At.TrackIndex == 1 && At.TrackDistance > 0 && Restored->GetActorLocation().Y > 0);
    Restored->ServerSetBrakeInput(1);
    Simulate(Restored, 300);
    const FVector BranchHead = Restored->GetActorForwardVector();
    TestTrue(TEXT("Independent brake stops the free train"), Restored->Speed == 0);
    Restored->ServerSetBrakeInput(0);
    Restored->ServerSetDriveInput(-1);
    Simulate(Restored, 10);
    TestTrue(TEXT("Backs down siding without rotating cab"), Restored->Speed < 0 && BranchHead.Equals(Restored->GetActorForwardVector(), .001));
    Simulate(Restored, 400);
    TestTrue(TEXT("Reverse traversal returns to approach track"), Restored->GetCurrentTrackLocation(At) && At.TrackIndex == 0 && At.TrackDistance < 5000);
    Simulate(Restored, 1200);
    TestTrue(TEXT("Buffer stop bounds free travel"), Restored->Speed == 0 && Restored->GetActorLocation().Equals(FVector::ZeroVector, .01));

    auto* Loose = Railway::Tests::MakeTrain(*W, D);
    Loose->Route = {};
    Loose->SetActorLocation(FVector(1500,0,0));
    Loose->ServerSetDriveMode(ERailwayDriveMode::Manual);
    Loose->ServerSetDriveInput(1);
    Simulate(Loose, 30);
    TestTrue(TEXT("Manually placed train drives without ever having a service"), Loose->Speed > 0 &&
        Loose->State == ERailwayState::Idle && Loose->GetActorLocation().X > 1500);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayClientTurnoutMotion, "Railway.Driving.RemoteTurnoutAndCab",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRailwayClientTurnoutMotion::RunTest(const FString&)
{
    UE::Net::FTestWorlds Worlds;
    if (!TestTrue(TEXT("Connect driver UI"), Worlds.CreateAndConnectClient())) return false;
    auto* Server = Worlds.Server.GetWorld();
    auto* Client = Worlds.Clients[0].GetWorld();
    auto* OwnerPC = Worlds.GetServerPlayerControllerOfClient(0);
    auto* Control = NewObject<URailwayPlanControlComponent>(OwnerPC);
    OwnerPC->AddInstanceComponent(Control);
    Control->RegisterComponent();
    OwnerPC->ForceNetUpdate();
    auto* D = MakeFork(*Server);
    Client->SpawnActor<ARailwayDataActor>()->Network = D->Network;
    auto* T = Railway::Tests::MakeTrain(*Server, D);
    T->RollingResistance = 3.f;
    FString Error;
    if (!StartAt(T, 4100, Error)) { AddError(Error); return false; }
    T->ServerSetDriveMode(ERailwayDriveMode::Manual);
    T->ServerSetDriveInput(1);
    auto* PC = Client->GetFirstPlayerController();
    ARailwayTrain* Proxy = nullptr;
    TArray<FRailwayTrackChoice> Choices;
    if (!TestTrue(TEXT("Client sees scheduled main and legal siding"), Worlds.TickUntil([&]
    {
        for (TActorIterator<ARailwayTrain> It(Client); It; ++It) Proxy = *It;
        if (!Proxy || !PC->FindComponentByClass<URailwayPlanControlComponent>()) return false;
        Proxy->GetUpcomingTrackChoices(false, Choices);
        return Choices.Num() == 2 && Choices[0].bUsedByCurrentPlan && Proxy->Speed > 150;
    }, 240))) return false;
    T->ServerSetDriveInput(0);
    URailwayPlanRequest::ActivateRailwayTrack(PC, Choices[1])->Activate();
    if (!TestTrue(TEXT("Remote selection releases conflicting timetable and preserves coasting"), Worlds.TickUntil([&]
    {
        Proxy->GetUpcomingTrackChoices(false, Choices);
        return Proxy->State == ERailwayState::Idle && Proxy->Speed > 0 &&
            Choices.Num() == 2 && Choices[1].bActive && T->State == ERailwayState::Idle;
    }, 180))) return false;
    FRailwayTrackLocation At;
    if (!TestTrue(TEXT("Replicated free motion crosses onto selected track"), Worlds.TickUntil([&]
    {
        return Proxy->GetCurrentTrackLocation(At) && At.TrackIndex == 1 && At.TrackDistance > 380;
    }, 1200))) return false;
    const FVector Cab = Proxy->GetActorForwardVector();
    T->ServerSetDriveMode(ERailwayDriveMode::Automatic);
    const auto View = URailwayPlanViewLibrary::GetRailwayPlanOverview(Proxy);
    const FRailwayRoutePoint Goal{{D->Network.Tracks[1].Id}, 80};
    URailwayPlanRequest::SetRailwayDestination(PC, View.View, Goal, false)->Activate();
    if (!TestTrue(TEXT("Remote player sees backing service, heading preserved"), Worlds.TickUntil([&]
    {
        return Proxy->ActivePlan.bCabReversed && Proxy->IsPlanPresentationCurrent() && Proxy->Speed < -10 &&
            FVector::DotProduct(Cab, Proxy->GetActorForwardVector()) > .999;
    }, 480))) return false;
    return TestTrue(TEXT("Both peers reach rear destination with same cab facing"), Worlds.TickUntil([&]
    {
        return T->State == ERailwayState::Finished && Proxy->State == ERailwayState::Finished &&
            T->GetActorLocation().Equals(Proxy->GetActorLocation(), .2) && FVector::DotProduct(Cab, Proxy->GetActorForwardVector()) > .999;
    }, 1500));
}
#endif
