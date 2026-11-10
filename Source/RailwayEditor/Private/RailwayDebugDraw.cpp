#include "RailwayDebugDraw.h"

#include "Debug/DebugDrawService.h"
#include "CanvasItem.h"
#include "UObject/Script.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "RailwayDataActor.h"
#include "RailwayTrain.h"
#include "SceneInterface.h"
#include "SceneView.h"

namespace Railway::Editor
{
static TAutoConsoleVariable<int32> TrainDebug(TEXT("railway.Debug.Trains"), 0,
    TEXT("0 off, 1 train overview, 2 extended diagnostics. Select a train to isolate it."));

static void DrawTrainDebug(UCanvas* Canvas, APlayerController*)
{
    const int32 Detail = TrainDebug.GetValueOnGameThread();
    if (Detail <= 0 || !Canvas || !Canvas->SceneView || !Canvas->SceneView->Family || !Canvas->SceneView->Family->Scene) return;
    UWorld* World = Canvas->SceneView->Family->Scene->GetWorld();
    if (!World || (World->WorldType != EWorldType::Editor && World->WorldType != EWorldType::PIE && World->WorldType != EWorldType::Game)) return;

    TArray<ARailwayTrain*> Trains;
    bool bSelected = false;
    for (TActorIterator<ARailwayTrain> It(World); It; ++It)
    {
        Trains.Add(*It);
        bSelected |= It->IsSelected();
    }
    const float Left = 18;
    float Y = 52;
    auto Line = [&](const FString& S, FColor Color)
    {
        if (Y > Canvas->ClipY - 20) return;
        FCanvasTileItem Backdrop(FVector2D(Left - 4, Y), FVector2D(FMath::Min(820.f, Canvas->ClipX - Left), 17), FLinearColor(.015f,.025f,.035f,.82f));
        Backdrop.BlendMode = SE_BLEND_Translucent;
        Canvas->DrawItem(Backdrop);
        Canvas->SetDrawColor(Color);
        Canvas->DrawText(GEngine->GetSmallFont(), S, Left, Y);
        Y += 17;
    };
    const FColor Header(91, 245, 198);
    Line(FString::Printf(TEXT("RAILWAY   %s   |   trains %d%s"), *World->GetName(), Trains.Num(), bSelected ? TEXT("   |   selection") : TEXT("")), Header);
    for (ARailwayTrain* T : Trains)
    {
        if (bSelected && !T->IsSelected()) continue;
        if (Y > Canvas->ClipY - 52) break;
        const auto* D = T->GetRailwayData();
        const FRailwayPlan& Plan = T->ActivePlan;
        Line(FString::Printf(TEXT("%s   %s / %s   %.1f km/h    %.1f / %.1f m   calls %d/%d"),
            *T->GetActorNameOrLabel(), *UEnum::GetValueAsString(T->State), *UEnum::GetValueAsString(T->DriveMode),
            T->Speed * .036, T->Distance / 100, T->GetServiceEndDistance() / 100, T->NextStop, Plan.Stops.Num()), FColor::White);
        if (Detail < 2) continue;
        FEditorScriptExecutionGuard ScriptGuard;
        if (World->WorldType == EWorldType::Editor)
        {
            Line(FString::Printf(TEXT("  preset %s   ordered tracks %d   intermediate stops %d   auto-start %d"),
                *UEnum::GetValueAsString(T->Route.Mode), T->Route.Tracks.Num(), T->Route.bStopAtIntermediateStations, T->bAutoStart), FColor::Silver);
        }
        FRailwayTrackLocation At;
        FString Track = TEXT("unlocated");
        if (D && T->GetCurrentTrackLocation(At) && D->Network.Tracks.IsValidIndex(At.TrackIndex))
            Track = FString::Printf(TEXT("%s @ %.1f cm"), *D->Network.Tracks[At.TrackIndex].Name.ToString(), At.TrackDistance);
        Line(FString::Printf(TEXT("  %s   section %d/%d   XYZ %s"), *Track, T->Leg + 1, Plan.Sections.Num(), *T->GetActorLocation().ToCompactString()), FColor::Silver);
        FRailwayStation Next;
        double Remaining;
        T->GetNextStopInfo(Remaining, Next);
        Line(FString::Printf(TEXT("  next %s  %.1f m   dwelling %s  %.1fs   direct %d"),
            Remaining >= 0 ? *Next.Name.ToString() : TEXT("-"), Remaining / 100, *T->CurrentStation.ToString(), T->Wait, Plan.bFinishAtLastStop), FColor::Silver);
        Line(FString::Printf(TEXT("  throttle %.2f   brake %.2f   cruise %.1f   limit %.1f cm/s   accel %.1f   decel %.1f"),
            T->DriveInput, T->BrakeInput, T->CruiseSpeed, T->ExternalSpeedLimit, T->Acceleration, T->Braking), FColor::Silver);
        Line(FString::Printf(TEXT("  %s   plan %u   snapshot %u @ %.2fs   %.1f Hz   interpolation %.3fs   restore %s"),
            *UEnum::GetValueAsString(T->GetLocalRole()), Plan.NetworkRevision, T->TrainMoveState.PlanRevision,
            T->TrainMoveState.ServerTime, T->SnapshotRateHz, T->InterpolationDelay, *UEnum::GetValueAsString(T->LastRestoreResult)), FColor::Silver);
        if (D) Line(FString::Printf(TEXT("  build %s   tracks %d   edges %d   stations %d"),
            *D->Network.BuildId.ToString(), D->Network.Tracks.Num(), D->Network.Edges.Num(), D->Network.Stations.Num()), FColor::Silver);
        Y += 5;
    }
}

FDelegateHandle RegisterTrainDebugDraw()
{
    return UDebugDrawService::Register(TEXT("OnScreenDebug"), FDebugDrawDelegate::CreateStatic(&DrawTrainDebug));
}

void UnregisterTrainDebugDraw(FDelegateHandle Handle)
{
    UDebugDrawService::Unregister(Handle);
}
}
