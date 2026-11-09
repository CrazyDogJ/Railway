#pragma once

#include "CoreMinimal.h"
#include "RailwayUtils.h"
#include "GameFramework/Actor.h"
#include "RailwayTrack.generated.h"

class USplineComponent;

class ARailwayTrack;

UCLASS(Abstract)
class RAILWAY_API ARailwayTrackAnchor : public AActor
{
    GENERATED_BODY()
public:
    ARailwayTrackAnchor();

    UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category="Railway")
    TObjectPtr<ARailwayTrack> HostTrack;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Railway", meta=(ClampMin="0", ClampMax="1"))
    float TrackPosition = 0.5f;

    UFUNCTION(BlueprintCallable, Category="Railway|Anchor")
    void RefreshAnchor();

    // Bind to a normalized position and immediately update the attachment.
    UFUNCTION(BlueprintCallable, Category="Railway|Anchor")
    void SetTrackAnchor(ARailwayTrack* Track, float NormalizedPosition);

    UFUNCTION(BlueprintCallable, Category="Railway|Anchor")
    void ProjectToHost();
    virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void Tick(float DeltaSeconds) override;
    virtual void PostEditMove(bool bFinished) override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif

};

UCLASS(Blueprintable)
class RAILWAY_API ARailwayJunction : public ARailwayTrackAnchor
{
    GENERATED_BODY()
public:
    ARailwayJunction();
};

UCLASS(Blueprintable, meta=(DisplayName="Railway Station"))
class RAILWAY_API ARailwayStationMarker : public ARailwayTrackAnchor
{
    GENERATED_BODY()
public:
    ARailwayStationMarker();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Station")
    FName StationName = TEXT("Station");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Station", meta=(ClampMin="0", Units="s"))
    float DwellSeconds = 3.f;

    virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITORONLY_DATA
    UPROPERTY()
    TObjectPtr<class UTextRenderComponent> Label;
#endif
};

UCLASS(Blueprintable)
class RAILWAY_API ARailwayTrack : public AActor
{
    GENERATED_BODY()
public:
    ARailwayTrack();
#if WITH_EDITOR
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void Tick(float DeltaSeconds) override;
#endif
    UFUNCTION(BlueprintCallable, Category="Railway")
    void AlignEndpoints(bool bIncludeFreeJunctions);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    TObjectPtr<USplineComponent> Spline;

    UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category="Railway")
    TObjectPtr<ARailwayJunction> StartJunction;

    UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category="Railway")
    TObjectPtr<ARailwayJunction> EndJunction;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, AdvancedDisplay, Category="Railway", meta=(DisplayName="Legacy Stations"))
    TArray<FRailwayStation> Stations;

    UFUNCTION(CallInEditor, BlueprintCallable, Category="Railway")
    void SnapEndpointsToJunctions();
};
