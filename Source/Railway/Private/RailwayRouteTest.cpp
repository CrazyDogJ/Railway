#if WITH_DEV_AUTOMATION_TESTS
#include "RailwayTrack.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "Components/SplineComponent.h"
#include "RailwayTrain.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayRouteTest, "Railway.Route.BranchAndStation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRailwayRouteTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("Transient world"), World)) return false;
    auto* Fork = World->SpawnActor<ARailwayJunction>();
    Fork->SetActorLocation(FVector(2000, 0, 0));
    auto* Approach = World->SpawnActor<ARailwayTrack>();
    auto* Main = World->SpawnActor<ARailwayTrack>();
    auto* Branch = World->SpawnActor<ARailwayTrack>();
    Approach->EndJunction = Fork;
    Main->SetActorLocation(FVector(2000, 0, 0));
    Main->StartJunction = Fork;
    Branch->EndJunction = Fork;
    Branch->Spline->SetLocationAtSplinePoint(0, FVector(2000, 2000, 0), ESplineCoordinateSpace::World);
    Branch->SnapEndpointsToJunctions();
    FRailwayStation& Station = Branch->Stations.AddDefaulted_GetRef();
    Station.Position = .95f;
    Station.DwellSeconds = 2.f;
    Station.Name = TEXT("Branch Halt");

    auto* Train = World->SpawnActor<ARailwayTrain>();
    Train->Route.SetNum(2);
    Train->Route[0].Track = Approach;
    Train->Route[1].Track = Branch;
    Train->Route[1].bReverse = true;
    TestTrue(TEXT("Connected reverse branch starts"), Train->StartRoute());
    bool bBrakedBeforeFork = false;
    float LastSpeed = 0;
    for (int32 I = 0; I < 600 && Train->State == ERailwayState::Running; ++I)
    {
        Train->Tick(1.f / 30);
        if (Train->GetActorLocation().X < 2000 && Train->Speed < LastSpeed) bBrakedBeforeFork = true;
        LastSpeed = Train->Speed;
    }
    TestTrue(TEXT("Braking looks across junction"), bBrakedBeforeFork);
    TestTrue(TEXT("Stops on chosen branch"), Train->GetActorLocation().Equals(FVector(2000, 100, 0), .1));
    TestTrue(TEXT("Faces direction of reverse travel"), Train->GetActorForwardVector().Y > .99);
    TestEqual(TEXT("Station reached"), Train->CurrentStation, FName(TEXT("Branch Halt")));
    Train->Tick(1.f);
    TestTrue(TEXT("Dwelling at rest"), Train->State == ERailwayState::Dwelling && Train->Speed == 0);
    Train->Tick(20.f);
    TestTrue(TEXT("Departs and finishes at branch terminus"), Train->State == ERailwayState::Finished
        && Train->GetActorLocation().Equals(FVector(2000, 2000, 0), .1));
    Train->Route[1].Track = Main;
    Train->Route[1].bReverse = false;
    TestTrue(TEXT("Other branch starts"), Train->StartRoute());
    Train->Tick(30.f);
    TestTrue(TEXT("Main terminus"), Train->State == ERailwayState::Finished
        && Train->GetActorLocation().Equals(FVector(4000, 0, 0), .1));
    Train->Route[1].bReverse = true;
    TestFalse(TEXT("Disconnected route rejected"), Train->StartRoute());
    World->DestroyWorld(false);
    return true;
}
#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayAnchorTest, "Railway.Route.AnchoredActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRailwayAnchorTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (!TestNotNull(TEXT("Test world"), World)) return false;
    auto* Main = World->SpawnActor<ARailwayTrack>();
    auto* Spur = World->SpawnActor<ARailwayTrack>();
    auto* Fork = World->SpawnActor<ARailwayJunction>();
    Fork->HostTrack = Main;
    Fork->TrackPosition = .5f;
    Fork->RefreshAnchor();
    TestTrue(TEXT("Junction attaches to main track"), Fork->GetAttachParentActor() == Main);
    Fork->SetActorLocation(FVector(750, 120, 0));
    Fork->PostEditMove(true);
    TestTrue(TEXT("Moving the fork projects it to rail"), Fork->GetActorLocation().Equals(FVector(750, 0, 0), 1));
    Spur->StartJunction = Fork;
    Spur->Spline->SetLocationAtSplinePoint(1, FVector(750, 1500, 0), ESplineCoordinateSpace::World);
    Spur->AlignEndpoints(false);
    TestTrue(TEXT("Branch follows fork"), Spur->Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World).Equals(Fork->GetActorLocation(), .01));

    auto* Halt = World->SpawnActor<ARailwayStationMarker>();
    Halt->HostTrack = Spur;
    Halt->StationName = TEXT("Spur Halt");
    Halt->RefreshAnchor();
    Halt->SetActorLocation(FVector(800, 400, 0));
    Halt->PostEditMove(true);
    TestTrue(TEXT("Station projects to spur"), Halt->GetActorLocation().Equals(FVector(750, 400, 0), 1));
    auto* Beyond = World->SpawnActor<ARailwayStationMarker>();
    Beyond->HostTrack = Main;
    Beyond->TrackPosition = .9f;
    Beyond->StationName = TEXT("Beyond fork");

    auto* Train = World->SpawnActor<ARailwayTrain>();
    Train->Route.SetNum(2);
    Train->Route[0].Track = Main;
    Train->Route[1].Track = Spur;
    Train->RefreshPreview();
    TestTrue(TEXT("Train attaches at departure"), Train->GetAttachParentActor() == Main && Train->GetActorLocation().Equals(Main->GetActorLocation(), .01));
    TestTrue(TEXT("Mid-track departure route valid"), Train->StartRoute());
    for (int32 I = 0; I < 600 && Train->State == ERailwayState::Running; ++I) Train->Tick(1.f/30);
    TestEqual(TEXT("Off-route stop skipped"), Train->CurrentStation, Halt->StationName);
    TestTrue(TEXT("Stops at visible marker"), Train->GetActorLocation().Equals(Halt->GetActorLocation(), .1));
    Train->Tick(30);
    TestTrue(TEXT("Outbound complete"), Train->State == ERailwayState::Finished);

    Train->Route.Swap(0, 1);
    Train->Route[0].bReverse = Train->Route[1].bReverse = true;
    Train->RefreshPreview();
    TestTrue(TEXT("Reverse preview at far end facing back"), Train->GetAttachParentActor() == Spur
        && Train->GetActorLocation().Equals(FVector(750,1500,0), .1) && Train->GetActorForwardVector().Y < -.99);
    TestTrue(TEXT("Mid-track return route valid"), Train->StartRoute());
    Train->Tick(30);
    TestTrue(TEXT("Rejoins main and returns to start"), Train->State == ERailwayState::Finished
        && Train->GetActorLocation().Equals(FVector::ZeroVector, .1));
    Train->Route.Reset();
    Train->RefreshPreview();
    TestNull(TEXT("Clearing route releases train"), Train->GetAttachParentActor());
    World->DestroyWorld(false);
    return true;
}
#endif
#endif
