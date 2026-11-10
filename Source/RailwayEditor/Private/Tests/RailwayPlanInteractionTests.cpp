#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "RailwayDispatchFixtures.h"
#include "EngineUtils.h"
#include "RailwayDataActor.h"
#include "RailwayPlanControl.h"
#include "RailwayPlanWidget.h"
#include "RailwayTrain.h"
#include "Tests/NetTestHelpers.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

namespace Railway::Tests
{
ARailwayDataActor* MakeNetwork(UWorld& World)
{
    auto* Data = World.SpawnActor<ARailwayDataActor>();
    auto& N = Data->Network;
    N.NetworkId = FGuid::NewGuid();
    N.BuildId = FGuid::NewGuid();
    auto& Track = N.Tracks.AddDefaulted_GetRef();
    Track.Id = FGuid::NewGuid();
    Track.Name = TEXT("Main");
    Track.NumEdges = 2;
    N.TrackById.Add(Track.Id, 0);
    for (int32 I = 0; I < 2; ++I)
    {
        const FVector Position(I * 10000, 0, 0);
        N.Nodes.Add(Position);
        Track.Curves.Position.AddPoint(I, Position);
        Track.Curves.Scale.AddPoint(I, FVector::OneVector);
        Track.Curves.Rotation.AddPoint(I, FQuat::Identity);
    }
    Track.Curves.UpdateSpline();
    N.Edges.Add({0, 0, 1, 0, 10000});
    N.Edges.Add({0, 1, 0, 10000, 0});
    for (double Local : {0., 5000., 9000.})
    {
        FRailwayBakedStation S;
        S.EdgeIndex = 0;
        S.Id = FGuid::NewGuid();
        S.Definition.Name = TEXT("Platform");
        S.Definition.TrackDistance = Local;
        S.Definition.DwellSeconds = 2;
        N.StationById.Add(S.Id, N.Stations.Add(S));
    }
    Track.NumStations = N.Stations.Num();
    return Data;
}

ARailwayTrain* MakeTrain(UWorld& World, ARailwayDataActor* Data)
{
    FActorSpawnParameters Spawn;
    Spawn.bDeferConstruction = true;
    auto* Train = World.SpawnActor<ARailwayTrain>(ARailwayTrain::StaticClass(), FTransform::Identity, Spawn);
    Train->RailwayData = Data;
    Train->bAutoStart = false;
    Train->Route.Mode = ERailwayRouteMode::OrderedTracks;
    Train->Route.Tracks.Add({{Data->Network.Tracks[0].Id}, false, -1, -1});
    Train->FinishSpawning(FTransform::Identity);
    return Train;
}

}

namespace
{
FRailwayPlanCommand Command(ARailwayTrain& T, ERailwayPlanAction Action, int32 Visit = INDEX_NONE)
{
    FRailwayPlanCommand C;
    C.Action = Action;
    C.StopIndex = Visit;
    C.BuildId = T.GetRailwayData()->Network.BuildId;
    C.ExpectedRevision = T.GetPlanRevision();
    return C;
}

bool HasText(TSharedRef<SWidget> W, const FString& Value)
{
    if (W->GetType() == TEXT("STextBlock") && StaticCastSharedRef<STextBlock>(W)->GetText().ToString() == Value) return true;
    FChildren* Children = W->GetChildren();
    for (int32 I = 0; I < Children->Num(); ++I)
        if (HasText(Children->GetChildAt(I), Value)) return true;
    return false;
}

TSharedPtr<SButton> FindButton(TSharedRef<SWidget> W, const TCHAR* Label)
{
    if (W->GetType() == TEXT("SButton") && HasText(W, Label)) return StaticCastSharedRef<SButton>(W);
    FChildren* Children = W->GetChildren();
    for (int32 I = 0; I < Children->Num(); ++I)
        if (auto Result = FindButton(Children->GetChildAt(I), Label)) return Result;
    return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayDispatchTest, "Railway.Dispatch.PlanEdits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRailwayDispatchTest::RunTest(const FString&)
{
    UE::Net::FTestWorlds Worlds;
    UWorld* World = Worlds.Server.GetWorld();
    if (!TestNotNull(TEXT("Test world"), World)) return false;
    auto* Data = Railway::Tests::MakeNetwork(*World);
    auto* T = Railway::Tests::MakeTrain(*World, Data);
    auto* Player = World->SpawnActor<APlayerController>();
    FString Error;
    auto Apply = [&](FRailwayPlanCommand C) { return T->ApplyPlayerPlanCommand(Player, C, Error); };
    const auto Activate = Command(*T, ERailwayPlanAction::ActivatePreset);
    TestFalse(TEXT("A command requires a player"), T->ApplyPlayerPlanCommand(nullptr, Activate, Error));
    if (!TestTrue(TEXT("Preset starts"), Apply(Activate))) return false;
    if (!TestEqual(TEXT("Three visits"), T->ActivePlan.Stops.Num(), 3)) return false;
    TestFalse(TEXT("Stale command refused"), Apply(Activate));
    T->Tick(.01f);
    if (!TestTrue(TEXT("Physical arrival recorded"), T->State == ERailwayState::Dwelling && T->ActivePlan.Stops[0].bServed)) return false;
    const double Wait = T->Wait;
    TestFalse(TEXT("Occupied visit protected"), Apply(Command(*T, ERailwayPlanAction::SkipStop, 0)));
    TestTrue(TEXT("Skip upcoming visit"), Apply(Command(*T, ERailwayPlanAction::SkipStop, 1)));
    TestEqual(TEXT("Does not restart dwell"), T->Wait, Wait);
    if (!TestEqual(TEXT("One visit removed"), T->ActivePlan.Stops.Num(), 2)) return false;
    TestEqual(TEXT("Other same-name stop retained"), T->ActivePlan.Stops[1].StationIndex, 2);
    auto Add = Command(*T, ERailwayPlanAction::AddStop);
    Add.SourceId = Data->Network.Stations[1].Id;
    Add.Distance = 5010;
    TestFalse(TEXT("Off-route visit rejected"), Apply(Add));
    Add.Distance = 5000;
    TestTrue(TEXT("Restore visit"), Apply(Add));
    Add.ExpectedRevision = T->GetPlanRevision();
    TestFalse(TEXT("Duplicate visit rejected"), Apply(Add));
    TestTrue(TEXT("Express keeps the served prefix"), Apply(Command(*T, ERailwayPlanAction::DirectToStop, 2)));
    TestTrue(TEXT("Express terminus"), T->ActivePlan.Stops.Num() == 2 && T->ActivePlan.Stops[0].bServed && T->GetServiceEndDistance() == 9000);
    TestTrue(TEXT("Cancel express destination"), Apply(Command(*T, ERailwayPlanAction::SkipStop, 1)));
    TestEqual(TEXT("Uses geometric end"), T->GetServiceEndDistance(), 10000.0);
    const FRailwayPlan Old = T->ActivePlan;
    TestTrue(TEXT("Start mid-route"), T->RestartPreparedRouteAtDistance(Old, 2000, Error));
    TestTrue(TEXT("Passed stop is not a physical arrival"), T->NextStop == 1 && !T->ActivePlan.Stops[0].bServed);
    const FTransform Before = T->GetActorTransform();
    TestTrue(TEXT("Cancel parks"), Apply(Command(*T, ERailwayPlanAction::CancelRoute)));
    TestTrue(TEXT("Parked at occupied pose"), T->State == ERailwayState::Idle && T->Speed == 0 && T->GetActorTransform().Equals(Before));
    auto NewGoal = Command(*T, ERailwayPlanAction::SetDestination);
    NewGoal.SourceId = Data->Network.Stations[2].Id;
    TestTrue(TEXT("Route to station from parked position"), Apply(NewGoal));
    TestTrue(TEXT("Station destination schedules arrival"), T->ActivePlan.Stops.Num() == 1 && T->ActivePlan.Stops[0].StationIndex == 2);
    NewGoal = Command(*T, ERailwayPlanAction::SetDestination);
    NewGoal.SourceId = Data->Network.Tracks[0].Id;
    NewGoal.Distance = 8000;
    TestTrue(TEXT("Route to a track location"), Apply(NewGoal));
    TestTrue(TEXT("Track terminus may have no station"), T->ActivePlan.Stops.IsEmpty() && T->GetServiceEndDistance() == 6000);
    const int64 Revision = T->GetPlanRevision();
    T->Route.Tracks.Reset();
    TestFalse(TEXT("Invalid preset fails"), Apply(Command(*T, ERailwayPlanAction::ActivatePreset)));
    TestEqual(TEXT("Failed activation preserves live service"), T->GetPlanRevision(), Revision);
    auto Draft = Command(*T, ERailwayPlanAction::StartBetweenPoints);
    Draft.Departure = {{Data->Network.Stations[1].Id}, 0};
    Draft.SourceId = Data->Network.Tracks[0].Id;
    Draft.Distance = 7500;
    TestFalse(TEXT("Cannot relocate a running service using a custom departure"), Apply(Draft));
    TestTrue(TEXT("Cancel before custom departure"), Apply(Command(*T, ERailwayPlanAction::CancelRoute)));
    Draft.ExpectedRevision = T->GetPlanRevision();
    TestTrue(TEXT("Activate player-authored route without a preset"), Apply(Draft));
    TestTrue(TEXT("Custom departure and destination"), T->GetActorLocation().Equals(FVector(5000, 0, 0), .1) && T->GetRouteLength() == 2500);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayPlayerPanelTest, "Railway.Dispatch.RemoteWidget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRailwayPlayerPanelTest::RunTest(const FString&)
{
    UE::Net::FTestWorlds Worlds;
    if (!TestTrue(TEXT("Connect a real PIE client"), Worlds.CreateAndConnectClient())) return false;
    UWorld* Server = Worlds.Server.GetWorld();
    UWorld* Client = Worlds.Clients[0].GetWorld();
    auto* ServerPC = Worlds.GetServerPlayerControllerOfClient(0);
    auto* ServerControl = NewObject<URailwayPlanControlComponent>(ServerPC);
    ServerPC->AddInstanceComponent(ServerControl);
    ServerControl->RegisterComponent();
    ServerPC->ForceNetUpdate();
    auto* ServerData = Railway::Tests::MakeNetwork(*Server);
    // A placed data actor is loaded independently on each peer in a cooked level.
    Client->SpawnActor<ARailwayDataActor>()->Network = ServerData->Network;
    auto* ServerTrain = Railway::Tests::MakeTrain(*Server, ServerData);
    auto* PC = Client->GetFirstPlayerController();
    ARailwayTrain* Train = nullptr;
    const bool bReady = Worlds.TickUntil([&]
    {
        for (TActorIterator<ARailwayTrain> It(Client); It; ++It) Train = *It;
        return Train && PC->FindComponentByClass<URailwayPlanControlComponent>();
    }, 240);
    if (!TestTrue(TEXT("Train and owner command channel replicated"), bReady)) return false;
    if (!TestFalse(TEXT("Client does not own the train"), Train->HasLocalNetOwner())) return false;

    auto* Widget = CreateWidget<URailwayPlanWidget>(PC, URailwayPlanWidget::StaticClass());
    if (!TestNotNull(TEXT("Create local UMG widget"), Widget)) return false;
    Widget->SetTrain(Train);
    auto Root = Widget->TakeWidget();
    auto Activate = FindButton(Root, TEXT("从预设起点发车"));
    if (!TestTrue(TEXT("Activation is available"), Activate && Activate->IsEnabled())) return false;
    Activate->SimulateClick();
    if (!TestTrue(TEXT("Click activates server plan and client presents it"), Worlds.TickUntil([&]
    {
        return ServerTrain->ActivePlan.Stops.Num() == 3 && Train->ActivePlan.Stops.Num() == 3 && Train->IsPlanPresentationCurrent();
    }, 240))) return false;
    Widget->SetTrain(Train);
    auto Skip = FindButton(Root, TEXT("取消停靠"));
    if (!TestTrue(TEXT("Upcoming visit button"), Skip && Skip->IsEnabled())) return false;
    Skip->SimulateClick();
    if (!TestTrue(TEXT("Remote visit removed"), Worlds.TickUntil([&]
    {
        return ServerTrain->ActivePlan.Stops.Num() == 2 && Train->ActivePlan.Stops.Num() == 2 && Train->IsPlanPresentationCurrent();
    }, 240))) return false;
    Widget->SetTrain(Train);
    auto Cancel = FindButton(Root, TEXT("停用运行计划"));
    if (!TestTrue(TEXT("Cancel enabled"), Cancel && Cancel->IsEnabled())) return false;
    Cancel->SimulateClick();
    TestTrue(TEXT("Both peers present parked state"), Worlds.TickUntil([&]
    {
        return ServerTrain->State == ERailwayState::Idle && Train->State == ERailwayState::Idle &&
            Train->GetPlanRevision() == ServerTrain->GetPlanRevision() && Train->IsPlanPresentationCurrent();
    }, 240));
    Widget->SetTrain(nullptr);
    TestTrue(TEXT("Unbinding leaves empty panel"), HasText(Root, TEXT("未绑定列车")));
    return true;
}
#endif
