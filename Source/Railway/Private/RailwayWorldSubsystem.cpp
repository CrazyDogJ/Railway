#include "RailwayWorldSubsystem.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "RailwayDataActor.h"
#include "RailwayTrain.h"

bool URailwayWorldSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
    return Super::DoesSupportWorldType(WorldType) || WorldType == EWorldType::GamePreview;
}

void URailwayWorldSubsystem::OnWorldBeginPlay(UWorld& World)
{
    Super::OnWorldBeginPlay(World);
    // Placed trains may start their preset before their network receives BeginPlay.
    for (TActorIterator<ARailwayDataActor> It(&World); It; ++It) RegisterData(**It);
    if (World.GetNetMode() != NM_Client && !PreTickHandle.IsValid())
        PreTickHandle = FWorldDelegates::OnWorldPreActorTick.AddUObject(this, &ThisClass::BeforeActorsTick);
}

void URailwayWorldSubsystem::Deinitialize()
{
    FWorldDelegates::OnWorldPreActorTick.Remove(PreTickHandle);
    Trains.Reset();
    DataActors.Reset();
    Super::Deinitialize();
}

void URailwayWorldSubsystem::RegisterTrain(ARailwayTrain& Train)
{
    if (Train.GetWorld() != GetWorld()) return;
    Trains.AddUnique(&Train);
    bTrafficDirty = true;
}

void URailwayWorldSubsystem::UnregisterTrain(ARailwayTrain& Train)
{
    Trains.Remove(&Train);
    bTrafficDirty = true;
}

void URailwayWorldSubsystem::RegisterData(ARailwayDataActor& Data)
{
    if (Data.GetWorld() != GetWorld()) return;
    DataActors.AddUnique(&Data);
    bTrafficDirty = true;
}

void URailwayWorldSubsystem::UnregisterData(ARailwayDataActor& Data)
{
    DataActors.Remove(&Data);
    bTrafficDirty = true;
}

TArray<ARailwayTrain*> URailwayWorldSubsystem::GetRailwayTrains() const
{
    TArray<ARailwayTrain*> Result;
    for (const auto& Weak : Trains)
        if (ARailwayTrain* Train = Weak.Get()) Result.Add(Train);
    return Result;
}

TArray<ARailwayDataActor*> URailwayWorldSubsystem::GetRailwayDataActors() const
{
    TArray<ARailwayDataActor*> Result;
    for (const auto& Weak : DataActors)
        if (ARailwayDataActor* Data = Weak.Get()) Result.Add(Data);
    return Result;
}

ARailwayDataActor* URailwayWorldSubsystem::FindDataFor(const ARailwayTrain& Train) const
{
    if (DataActors.Num() == 1) return DataActors[0].Get();
    struct FCandidates
    {
        ARailwayDataActor* Only = nullptr;
        ARailwayDataActor* Local = nullptr;
        int32 Total = 0, InLevel = 0;

        void Add(ARailwayDataActor* Data, bool bLocal)
        {
            Only = Data;
            ++Total;
            if (bLocal) { Local = Data; ++InLevel; }
        }
        ARailwayDataActor* Resolve() const { return InLevel == 1 ? Local : (Total == 1 ? Only : nullptr); }
    };
    FCandidates All, MatchingBuild;
    for (const auto& Weak : DataActors)
    {
        ARailwayDataActor* Data = Weak.Get();
        if (!Data) continue;
        const bool bLocal = Data->GetLevel() == Train.GetLevel();
        All.Add(Data, bLocal);
        if (Train.ActivePlan.BuildId.IsValid() && Data->Network.BuildId == Train.ActivePlan.BuildId)
            MatchingBuild.Add(Data, bLocal);
    }
    // A streamed network must not steal an established service. The replicated plan
    // carries the same baked ID on a client even for a dynamically spawned train.
    return MatchingBuild.Total ? MatchingBuild.Resolve() : All.Resolve();
}

void URailwayWorldSubsystem::BeforeActorsTick(UWorld* World, ELevelTick TickType, float Dt)
{
    if (World != GetWorld() || TickType == LEVELTICK_TimeOnly || TickType == LEVELTICK_PauseTick) return;
    FrameSeconds = Dt;
    RefreshTraffic();
}

void URailwayWorldSubsystem::PrepareMovement(ARailwayTrain& Train, int32 CabDirection)
{
    if (bTrafficDirty || (Trains.Num() > 1 && Train.TrafficDirection != CabDirection))
        RefreshTraffic(&Train, CabDirection);
}
