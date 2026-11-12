#include "RailwayDebugDraw.h"

#include "Debug/DebugDrawService.h"
#include "CanvasItem.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/Script.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "RailwayWorldSubsystem.h"
#include "HAL/IConsoleManager.h"
#include "RailwayDataActor.h"
#include "RailwayTrain.h"
#include "SceneInterface.h"
#include "SceneView.h"

namespace Railway::Editor
{
static TAutoConsoleVariable<int32> TrainDebug(TEXT("railway.Debug.Trains"), 0,
    TEXT("0 off, 1 train overview and traffic bounds, 2 extended diagnostics. Select a train to isolate it."));

static const FColor TrainLengthColor(91, 245, 198);
static const FColor ClearanceColor(255, 179, 57);

static void DrawTrafficLine(UCanvas& Canvas, FVector Start, FVector End, const FLinearColor& Color)
{
    const FSceneView& View = *Canvas.SceneView;
    if (View.bHasNearClippingPlane)
    {
        const FPlane& Near = View.NearClippingPlane;
        const double A = Near.PlaneDot(Start);
        const double B = Near.PlaneDot(End);
        if (A > 0 && B > 0) return;
        if (A > 0) Start = FMath::LinePlaneIntersection(Start, End, Near);
        else if (B > 0) End = FMath::LinePlaneIntersection(Start, End, Near);
    }
    Canvas.K2_DrawLine(FVector2D(Canvas.Project(Start)), FVector2D(Canvas.Project(End)), 1.5f, Color);
}

static void DrawTrafficBounds(UCanvas& Canvas, const ARailwayTrain& Train)
{
    // Dispatch measures world centimeters from the rail anchor, even on a scaled train.
    const FTransform RailFrame(Train.GetActorQuat(), Train.GetActorLocation());
    FBox Bounds(FVector(0, -50, 0), FVector(0, 50, 100));
    if (Train.Body && Train.Body->GetStaticMesh())
    {
        const FTransform BodyToRail = Train.Body->GetComponentTransform().GetRelativeTransform(RailFrame);
        Bounds = Train.Body->CalcBounds(BodyToRail).GetBox();
    }

    auto DrawBox = [&](double HalfLength, const FColor& Color)
    {
        const FLinearColor LineColor(Color);
        const FVector Corners[] = {
            {0, Bounds.Min.Y, Bounds.Min.Z}, {0, Bounds.Max.Y, Bounds.Min.Z},
            {0, Bounds.Max.Y, Bounds.Max.Z}, {0, Bounds.Min.Y, Bounds.Max.Z}
        };
        FVector Front[4], Rear[4];
        const FVector EndOffset(HalfLength, 0, 0);
        for (int32 I = 0; I < 4; ++I)
        {
            Front[I] = RailFrame.TransformPosition(Corners[I] + EndOffset);
            Rear[I] = RailFrame.TransformPosition(Corners[I] - EndOffset);
        }
        for (int32 I = 0; I < 4; ++I)
        {
            const int32 Next = (I + 1) % 4;
            DrawTrafficLine(Canvas, Front[I], Front[Next], LineColor);
            DrawTrafficLine(Canvas, Rear[I], Rear[Next], LineColor);
            DrawTrafficLine(Canvas, Rear[I], Front[I], LineColor);
        }
    };

    const double HalfLength = .5 * FMath::Max(0., Train.TrainLength);
    if (Train.TrafficClearance > 0) DrawBox(HalfLength + Train.TrafficClearance, ClearanceColor);
    DrawBox(HalfLength, TrainLengthColor);
}

static void DrawTrainDebug(UCanvas* Canvas, APlayerController*)
{
    const int32 Detail = TrainDebug.GetValueOnGameThread();
    if (Detail <= 0 || !Canvas || !Canvas->SceneView || !Canvas->SceneView->Family || !Canvas->SceneView->Family->Scene) return;
    UWorld* World = Canvas->SceneView->Family->Scene->GetWorld();
    if (!World || (World->WorldType != EWorldType::Editor && World->WorldType != EWorldType::PIE && World->WorldType != EWorldType::Game)) return;

    const auto* Registry = World->GetSubsystem<URailwayWorldSubsystem>();
    if (!Registry) return;
    const TArray<ARailwayTrain*> Trains = Registry->GetRailwayTrains();
    const bool bSelected = Trains.ContainsByPredicate([](const ARailwayTrain* Train){ return Train->IsSelected(); });
    // Submit the wireframes to this viewport before the HUD, including trains below the text cutoff.
    for (const ARailwayTrain* Train : Trains)
        if (!bSelected || Train->IsSelected()) DrawTrafficBounds(*Canvas, *Train);
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
    Line(TEXT("Green: Train Length     Amber: Traffic Clearance at each end"), FColor::Silver);
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
        Line(FString::Printf(TEXT("  train length %.1f cm   clearance %.1f cm per end"),
            T->TrainLength, T->TrafficClearance), FColor::Silver);
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
