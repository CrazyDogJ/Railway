#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "RailwayDataActor.h"
#include "RailwayDataBuilder.h"
#include "RailwayDispatchFixtures.h"
#include "RailwayTrack.h"
#include "RailwayTrain.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayLoopHandover, "Railway.Driving.ClosedSplineHandover",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRailwayLoopHandover::RunTest(const FString&)
{
    auto* Editor = UWorld::CreateWorld(EWorldType::Editor, false);
    auto* Game = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { Game->DestroyWorld(false); Editor->DestroyWorld(false); };
    auto* Loop = Editor->SpawnActor<ARailwayTrack>();
    Loop->Spline->SetSplinePoints({{0,0,0},{1000,0,0},{1000,1000,0},{0,1000,0}}, ESplineCoordinateSpace::Local, false);
    for (int32 I = 0; I < 4; ++I) Loop->Spline->SetSplinePointType(I, ESplinePointType::Linear, false);
    Loop->Spline->SetClosedLoop(true);
    const double Length = Loop->Spline->GetSplineLength();
    Loop->Stations.AddDefaulted_GetRef().TrackDistance = 0;
    auto* Junction = Editor->SpawnActor<ARailwayJunction>();
    Junction->SetTrackAnchorAtDistance(Loop, 500);
    auto* Branch = Editor->SpawnActor<ARailwayTrack>();
    Branch->SetActorLocation(Junction->GetActorLocation());
    Branch->StartJunction = Junction;
    auto* Baked = Editor->SpawnActor<ARailwayDataActor>();
    FString Error;
    if (!Railway::Editor::BuildData(*Editor->PersistentLevel, Error)) { AddError(Error); return false; }
    auto* Data = Game->SpawnActor<ARailwayDataActor>();
    Data->Network = Baked->Network;
    const int32 LoopIndex = Data->Network.TrackById.FindChecked(Loop->GetActorGuid());
    const auto& Track = Data->Network.Tracks[LoopIndex];
    const int32 First = Track.FirstEdge, Last = First + Track.NumEdges - 2;
    if (!TestTrue(TEXT("Baked closed spline connects across its corner seam in both directions"),
        Track.IsLoop() && Track.NumEdges == 4 && Data->Network.CanFollow(Last, First) && Data->Network.CanFollow(First+1, Last+1))) return false;
    auto* Train = Railway::Tests::MakeTrain(*Game, Data);
    FRailwayRouteSettings Settings;
    Settings.Mode = ERailwayRouteMode::OrderedTracks;
    Settings.bStopAtIntermediateStations = false;
    Settings.Tracks.Add({{Track.Id}, false, -1, -1});
    FRailwayPlan Lap;
    if (!Train->CompileRouteSettings(Settings, Lap, Error) || !Train->RestartPreparedRouteAtDistance(Lap, Length - 150, Error))
    { AddError(Error); return false; }
    TestTrue(TEXT("Current track is available as a closed spline"), Train->IsCurrentTrackLoop());
    Train->Speed = 300;
    const FTransform Before = Train->GetActorTransform();
    Settings.Tracks.Add({{Branch->GetActorGuid()}, false, -1, -1});
    FRailwayPlan Exit;
    if (!Train->CompileRouteSettings(Settings, Exit, Error) || !Train->ReplacePreparedRoute(Exit, Error)) { AddError(Error); return false; }
    TestTrue(TEXT("Joins to next lap without resetting the moving train"), Train->GetActorTransform().Equals(Before) && Train->Speed == 300 &&
        FMath::IsNearlyEqual(Train->GetRouteLength(), 150. + 500. + Branch->Spline->GetSplineLength(), .02));
    bool bCrossed = false;
    for (int32 I = 0; I < 1200 && Train->State == ERailwayState::Running; ++I)
    {
        Train->Tick(1.f / 60);
        FRailwayTrackLocation At;
        if (Train->GetCurrentTrackLocation(At) && At.TrackIndex == LoopIndex && At.TrackDistance < 500 && Train->Speed > 0) bCrossed = true;
    }
    TestTrue(TEXT("Crosses seam at speed and then takes the scheduled branch"),
        bCrossed && Train->State == ERailwayState::Finished && !Train->IsCurrentTrackLoop());

    Settings.Tracks.SetNum(1);
    Settings.bStopAtIntermediateStations = true;
    if (!Train->CompileRouteSettings(Settings, Lap, Error) || !Train->RestartPreparedRouteAtDistance(Lap, Length - 150, Error) ||
        !Train->ReplacePreparedRoute(Lap, Error)) { AddError(Error); return false; }
    TestTrue(TEXT("Requesting a full next lap before the seam keeps the pending call exactly once"),
        Train->NextStop == 0 && Train->ActivePlan.Stops.Num() == 2 && Train->ActivePlan.Stops[0].Distance == 150 &&
        Train->GetRouteLength() == Length + 150);
    Settings.Tracks.Add({{Track.Id}, false, -1, -1});
    FRailwayPlan TwoLaps;
    if (!Train->CompileRouteSettings(Settings, TwoLaps, Error) ||
        !Train->ReplacePreparedRoute(TwoLaps, Error, TwoLaps.Sections.Num() - 1)) { AddError(Error); return false; }
    TestTrue(TEXT("An explicit section hint selects the requested existing lap"), Train->Distance == 2 * Length - 150);
    Settings.Tracks.SetNum(1);
    for (bool bReverse : {false, true})
    {
        Settings.Tracks[0] = {{Track.Id}, bReverse, -1, -1};
        Settings.bStopAtIntermediateStations = true;
        if (!Train->CompileRouteSettings(Settings, Lap, Error) || !Train->RestartPreparedRouteAtDistance(Lap, Length, Error))
        { AddError(Error); return false; }
        Train->Tick(.1f);
        const double Wait = Train->Wait;
        if (!Train->ReplacePreparedRoute(Lap, Error)) { AddError(Error); return false; }
        TestTrue(TEXT("Seam arrival becomes the start of the next lap and dwells only once"),
            Train->State == ERailwayState::Dwelling && Train->Distance == 0 && Train->NextStop == 1 &&
            Train->ActivePlan.Stops.Num() == 2 && Train->Wait == Wait && Train->ActivePlan.Stops[0].bServed &&
            !Train->ActivePlan.bCabReversed);
        for (int32 I = 0; I < 300; ++I) Train->Tick(1.f / 60);
        TestTrue(TEXT("Train leaves the shared seam station in either direction"), Train->Distance > 0 && Train->State == ERailwayState::Running);
    }

    Settings.bStopAtIntermediateStations = false;
    Settings.Tracks[0] = {{Track.Id}, true, 150, Length - 200};
    FRailwayPlan Wrapped;
    if (!Train->CompileRouteSettings(Settings, Wrapped, Error) || !Train->RestartPreparedRouteAtDistance(Wrapped, 0, Error))
    { AddError(Error); return false; }
    TestTrue(TEXT("Single ordered reverse pass wraps through zero"), FMath::IsNearlyEqual(Train->GetRouteLength(), 350., .01));
    Settings.Tracks[0] = {{Track.Id}, true, Length - 300, Length - 650};
    if (!Train->CompileRouteSettings(Settings, Exit, Error) || !Train->ReplacePreparedRoute(Exit, Error))
    { AddError(Error); return false; }
    TestTrue(TEXT("Joins a replacement starting inside an edge without an illegal partial transition"),
        FMath::IsNearlyEqual(Train->GetRouteLength(), 800., .01));
    const int64 Revision = Train->GetPlanRevision();
    TestFalse(TEXT("An invalid explicit occurrence cannot trigger a loop handover"), Train->ReplacePreparedRoute(Exit, Error, 99));
    TestEqual(TEXT("Rejected replacement is atomic"), Train->GetPlanRevision(), Revision);
    const auto Save = Train->GetSaveGameData();
    Data->Network.BuildId = FGuid::NewGuid();
    auto* Restored = Railway::Tests::MakeTrain(*Game, Data);
    return TestTrue(TEXT("Wrapped handover rebinds both passes after a rebuild"),
        Restored->TryLoadSaveGameData(Save, Error) && Restored->State == ERailwayState::Running &&
        FMath::IsNearlyEqual(Restored->GetRouteLength(), 800., .01) && Restored->GetActorTransform().Equals(Train->GetActorTransform()));
}
#endif
