#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "RailwayDataActor.h"
#include "RailwayDispatchFixtures.h"
#include "RailwayTrain.h"
#include "RailwayWorldSubsystem.h"
#include "Tests/NetTestHelpers.h"

namespace
{
bool Dispatch(ARailwayTrain& T, FRailwayTrackLocation From, FRailwayTrackLocation To, FAutomationTestBase& Test)
{
    FString Error;
    FRailwayPlan P;
    if (T.FindRoutePlan(From, To, P, Error) && T.RestartPreparedRouteAtDistance(P, 0, Error)) return true;
    Test.AddError(Error);
    return false;
}

double Gap(const ARailwayTrain& A, const ARailwayTrain& B)
{
    return .5 * (A.TrainLength + B.TrainLength) + FMath::Max(A.TrafficClearance, B.TrafficClearance);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayRegistryTest, "Railway.Traffic.WorldRegistry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRailwayRegistryTest::RunTest(const FString&)
{
    UE::Net::FTestWorlds Worlds;
    if (!TestTrue(TEXT("Client connected"), Worlds.CreateAndConnectClient())) return false;
    UWorld* W = Worlds.Server.GetWorld();
    auto* Registry = W->GetSubsystem<URailwayWorldSubsystem>();
    auto* Client = Worlds.Clients[0].GetWorld();
    auto* ClientRegistry = Client->GetSubsystem<URailwayWorldSubsystem>();
    if (!TestTrue(TEXT("Per-peer registries"), Registry && ClientRegistry && Registry != ClientRegistry)) return false;
    const FGuid TrackId = FGuid::NewGuid();
    auto* T = W->SpawnActorDeferred<ARailwayTrain>(ARailwayTrain::StaticClass(), FTransform::Identity);
    T->Route.Start = {{TrackId}, 1000};
    T->Route.Destination = {{TrackId}, 3500};
    T->FinishSpawning(FTransform::Identity);
    TestTrue(TEXT("Train can register before the network"), Registry->GetRailwayTrains().Contains(T) && !T->GetRailwayData());
    auto* D = Railway::Tests::MakeNetwork(*W);
    D->Network.TrackById.Reset();
    D->Network.TrackById.Add(TrackId, 0);
    D->Network.Tracks[0].Id = TrackId;
    TestTrue(TEXT("Late data becomes available"), T->GetRailwayData() == D && Registry->GetRailwayDataActors().Num() == 1);
    TestTrue(TEXT("Pending auto-start executes its configured preset after data registers"),
        Worlds.TickUntil([&]{ return T->State == ERailwayState::Running && T->GetActorLocation().X > 1050; }, 300));
    auto* Other = Railway::Tests::MakeNetwork(*W);
    TestTrue(TEXT("Loading another railway preserves an existing service"), T->GetRailwayData() == D);
    auto* Unbound = W->SpawnActor<ARailwayTrain>();
    TestNull(TEXT("Ambiguous level has no implicit selection for a new train"), Unbound->GetRailwayData());
    Unbound->RailwayData = D;
    TestTrue(TEXT("Explicit network resolves ambiguity"), Unbound->GetRailwayData() == D);
    Unbound->RailwayData = nullptr;
    Other->Destroy();
    TestTrue(TEXT("Removing second network immediately restores lookup"), Unbound->GetRailwayData() == D);
    Unbound->Destroy();
    if (!TestTrue(TEXT("Client independently registers replicated train"), Worlds.TickUntil([&]
        { return ClientRegistry->GetRailwayTrains().Num() == 1; }, 180))) return false;
    TestTrue(TEXT("No authority actors leak into client registry"), ClientRegistry->GetRailwayTrains()[0] != T &&
        ClientRegistry->GetRailwayDataActors().IsEmpty());
    auto* ClientData = Client->SpawnActor<ARailwayDataActor>();
    ClientData->Network = D->Network;
    Railway::Tests::MakeNetwork(*Client);
    auto* Proxy = ClientRegistry->GetRailwayTrains()[0];
    TestTrue(TEXT("Client binds the replicated service among two local networks and presents motion"),
        Worlds.TickUntil([&]{ return Proxy->GetRailwayData() == ClientData && Proxy->GetActorLocation().X > 1050; }, 300));
    T->Destroy();
    D->Destroy();
    TestTrue(TEXT("Authority registry releases both actors"), Registry->GetRailwayTrains().IsEmpty() && Registry->GetRailwayDataActors().IsEmpty());
    return TestTrue(TEXT("Client removes the destroyed proxy"), Worlds.TickUntil([&]{ return ClientRegistry->GetRailwayTrains().IsEmpty(); }, 180));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayFollowingTest, "Railway.Traffic.FollowAndRelease",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRailwayFollowingTest::RunTest(const FString&)
{
    UE::Net::FTestWorlds Worlds;
    auto& W = *Worlds.Server.GetWorld();
    auto* D = Railway::Tests::MakeNetwork(W);
    auto* Lead = Railway::Tests::MakeTrain(W, D);
    auto* Rear = Railway::Tests::MakeTrain(W, D);
    Lead->CruiseSpeed = 180;
    Lead->TrainLength = 450;
    Rear->TrafficClearance = 220;
    Rear->ExternalSpeedLimit = 620;
    if (!Dispatch(*Lead, {0, 3900}, {0, 6200}, *this) || !Dispatch(*Rear, {0, 1200}, {0, 9400}, *this)) return false;
    double Minimum = DBL_MAX;
    bool bSawFollowing = false;
    const bool bStopped = Worlds.TickAllUntil([&]
    {
        const double Between = Lead->GetActorLocation().X - Rear->GetActorLocation().X;
        Minimum = FMath::Min(Minimum, Between);
        bSawFollowing |= Lead->Speed > 100 && Between < Gap(*Lead,*Rear) + 150 && Rear->Speed > 80 && Rear->Speed < 300;
        return Lead->State == ERailwayState::Finished && Rear->Speed == 0 && Between < Gap(*Lead,*Rear) + 1.;
    }, .05f, 1600);
    TestTrue(TEXT("Faster train catches and follows the moving leader"), bSawFollowing);
    TestTrue(TEXT("Follower waits behind finished train without finishing its own route"), bStopped && Rear->State == ERailwayState::Running);
    TestTrue(TEXT("Length and requested clearance respected throughout"), Minimum >= Gap(*Lead,*Rear) - .02);
    Lead->Destroy();
    TestTrue(TEXT("Destroying blocker releases follower"), Worlds.TickUntil([&]{ return Rear->Speed > 200; }, 600));
    return TestEqual(TEXT("External speed limit is preserved"), Rear->ExternalSpeedLimit, 620.f);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayMeetingTest, "Railway.Traffic.HeadOnAndReverse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRailwayMeetingTest::RunTest(const FString&)
{
    UE::Net::FTestWorlds Worlds;
    auto& W = *Worlds.Server.GetWorld();
    auto* D = Railway::Tests::MakeNetwork(W);
    auto* A = Railway::Tests::MakeTrain(W, D);
    auto* B = Railway::Tests::MakeTrain(W, D);
    if (!Dispatch(*A, {0, 1700}, {0, 9500}, *this)) return false;
    B->SetActorLocation(FVector(8200,0,0));
    B->ServerSetDriveMode(ERailwayDriveMode::Manual);
    B->ServerSetDriveInput(-1);
    A->Speed = 570;
    B->Speed = -450;
    const FQuat Cab = B->GetActorQuat();
    bool bBothSlowed = false;
    double Minimum = DBL_MAX;
    const bool bStopped = Worlds.TickAllUntil([&]
    {
        const double Between = B->GetActorLocation().X - A->GetActorLocation().X;
        Minimum = FMath::Min(Minimum, Between);
        bBothSlowed |= A->Speed > 10 && A->Speed < 300 && B->Speed < -10 && B->Speed > -300;
        return A->Speed == 0 && B->Speed == 0;
    }, .4f, 150);
    TestTrue(TEXT("Both trains brake while approaching"), bBothSlowed);
    TestTrue(TEXT("Both stop without crossing, including on a long frame"), bStopped && Minimum >= Gap(*A,*B) - .02);
    TestTrue(TEXT("Negative manual traction cannot override traffic or rotate cab"), B->DriveInput == -1 && B->GetActorQuat().Equals(Cab));
    Worlds.TickAll(20);
    TestTrue(TEXT("Opposing authority remains held at rest"), A->Speed == 0 && B->Speed == 0);
    B->ServerSetDriveInput(1);
    return TestTrue(TEXT("Changing direction clears the conflict"), Worlds.TickUntil([&]{ return B->Speed > 80 && A->Speed > 50; }, 600));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayMergeTest, "Railway.Traffic.MergeAndIndependentTracks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRailwayMergeTest::RunTest(const FString&)
{
    UE::Net::FTestWorlds Worlds;
    auto& W = *Worlds.Server.GetWorld();
    auto* D = Railway::Tests::MakeFork(W);
    auto* Main = Railway::Tests::MakeTrain(W, D);
    auto* Branch = Railway::Tests::MakeTrain(W, D);
    if (!Dispatch(*Main, {0, 7900}, {0, 600}, *this) || !Dispatch(*Branch, {1, 2900}, {0, 600}, *this)) return false;
    double Minimum = DBL_MAX;
    const bool bSettled = Worlds.TickAllUntil([&]
    {
        FRailwayTrackLocation A, B;
        if (!Main->GetCurrentTrackLocation(A) || !Branch->GetCurrentTrackLocation(B)) return false;
        const double Between = A.TrackIndex == B.TrackIndex ? FMath::Abs(A.TrackDistance - B.TrackDistance) :
            FMath::Abs(A.TrackDistance - 5000.) + B.TrackDistance;
        Minimum = FMath::Min(Minimum, Between);
        return Main->Speed == 0 && Branch->Speed == 0 &&
            (Main->State == ERailwayState::Finished || Branch->State == ERailwayState::Finished);
    }, .12f, 900);
    TestTrue(TEXT("One train clears the merge, the other follows"), bSettled && Minimum >= Gap(*Main,*Branch) - .02 &&
        FMath::Max(Main->GetActorLocation().X, Branch->GetActorLocation().X) < 1100);
    // The same geometry in a distinct network must not reserve this track.
    auto* OtherData = Railway::Tests::MakeFork(W);
    auto* Independent = Railway::Tests::MakeTrain(W, OtherData);
    if (!Dispatch(*Independent, {0, 1200}, {0, 200}, *this)) return false;
    if (!TestTrue(TEXT("Separate network at the exact same coordinates runs independently"),
        Worlds.TickUntil([&]{ return Independent->State == ERailwayState::Finished; }, 1800))) return false;
    Main->Destroy();
    Branch->Destroy();
    auto* Parked = Railway::Tests::MakeTrain(W, D);
    auto* Through = Railway::Tests::MakeTrain(W, D);
    if (!Dispatch(*Parked, {1, 1300}, {1, 1301}, *this) || !Dispatch(*Through, {0, 1200}, {0, 9000}, *this)) return false;
    return TestTrue(TEXT("Parked train on unselected siding leaves main clear"), Worlds.TickUntil([&]{ return Through->State == ERailwayState::Finished; }, 1800));
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayFouledTurnoutTest, "Railway.Traffic.CoastingPastStationarySiding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRailwayFouledTurnoutTest::RunTest(const FString&)
{
    UE::Net::FTestWorlds Worlds;
    auto& W = *Worlds.Server.GetWorld();
    auto* D = Railway::Tests::MakeFork(W);
    auto* Parked = Railway::Tests::MakeTrain(W, D);
    if (!Dispatch(*Parked, {1, 100}, {1, 110}, *this) ||
        !TestTrue(TEXT("Park within the throat"), Worlds.TickUntil([&]{ return Parked->State == ERailwayState::Finished; }))) return false;
    auto* Free = Railway::Tests::MakeTrain(W, D);
    Free->SetActorLocation(FVector(2900,0,0));
    Free->RollingResistance = 0;
    Free->Speed = 720;
    const double StopAt = 5000. - Gap(*Free, *Parked) + 110.;
    TestTrue(TEXT("Free coasting is braked even with zero drag and no service"),
        Worlds.TickAllUntil([&]{ return Free->Speed == 0; }, .25f, 200) &&
        FMath::IsNearlyEqual(Free->GetActorLocation().X, StopAt, .1) && Free->State == ERailwayState::Idle);
    Free->ServerSetDriveMode(ERailwayDriveMode::Manual);
    Free->ServerSetDriveInput(1);
    Worlds.TickAll(30);
    TestTrue(TEXT("A held throttle cannot enter the occupied throat"), Free->Speed == 0 && Free->GetActorLocation().X <= StopAt + .02);
    Parked->Destroy();
    return TestTrue(TEXT("Free driving resumes on the chosen through track"),
        Worlds.TickUntil([&]{ return Free->GetActorLocation().X > 5500 && Free->Speed > 0; }, 600));
}
#endif
