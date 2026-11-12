#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "RailwayWorldSubsystem.generated.h"

class ARailwayDataActor;
class ARailwayTrain;

/** Per-world registry on both peers. Movement authorities are calculated only on the server. */
UCLASS()
class RAILWAY_API URailwayWorldSubsystem final : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual void OnWorldBeginPlay(UWorld& World) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintPure, Category="Railway")
    TArray<ARailwayTrain*> GetRailwayTrains() const;

    UFUNCTION(BlueprintPure, Category="Railway")
    TArray<ARailwayDataActor*> GetRailwayDataActors() const;

    void RegisterTrain(ARailwayTrain& Train);
    void UnregisterTrain(ARailwayTrain& Train);
    void RegisterData(ARailwayDataActor& Data);
    void UnregisterData(ARailwayDataActor& Data);
    ARailwayDataActor* FindDataFor(const ARailwayTrain& Train) const;

private:
    friend class ARailwayTrain;
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
    TArray<TWeakObjectPtr<ARailwayTrain>> Trains;
    TArray<TWeakObjectPtr<ARailwayDataActor>> DataActors;
    FDelegateHandle PreTickHandle;
    bool bTrafficDirty = true;
    double FrameSeconds = 0;
    void BeforeActorsTick(UWorld* World, ELevelTick TickType, float Dt);
    void RefreshTraffic(const ARailwayTrain* Requester = nullptr, int32 RequestedDirection = 0);
    void PrepareMovement(ARailwayTrain& Train, int32 CabDirection);
};
