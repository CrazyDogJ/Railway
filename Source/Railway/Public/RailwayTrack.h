#pragma once

#include "RailwayNetworkActor.h"
#include "RailwayUtils.h"
#include "RailwayTrack.generated.h"

class USplineComponent;
class ARailwayTrack;

UCLASS(Abstract)
class RAILWAY_API ARailwayTrackAnchor : public ARailwayNetworkActor
{
    GENERATED_BODY()
public:
    ARailwayTrackAnchor();

#if WITH_EDITORONLY_DATA
    UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category="Railway")
    TSoftObjectPtr<ARailwayTrack> HostTrack;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Railway", meta=(ClampMin="0", Units="cm"))
    double TrackDistance = 0;
#endif

#if WITH_EDITOR
    UFUNCTION(BlueprintCallable, Category="Railway|Anchor")
    void SetTrackAnchorAtDistance(ARailwayTrack* Track, double DistanceCm);
    void RefreshAnchor();
    void ProjectToHost();
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void Tick(float DeltaSeconds) override;
    virtual void PostEditMove(bool bFinished) override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
#if WITH_EDITORONLY_DATA
protected:
    void CreateMarkerArrow(FName SubobjectName, FColor Color);
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
#if WITH_EDITOR
    virtual FRailwayStation GetStationDefinition() const;
    virtual void OnConstruction(const FTransform& Transform) override;
#endif

#if WITH_EDITORONLY_DATA
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Station")
    FName StationName = TEXT("Station");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Station", meta=(ClampMin="0", Units="s"))
    float DwellSeconds = 3.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Station")
    FInstancedStruct CustomData;

    UPROPERTY()
    TObjectPtr<class UTextRenderComponent> Label;
#endif
};

UCLASS(Blueprintable)
class RAILWAY_API ARailwayTrack : public ARailwayNetworkActor
{
    GENERATED_BODY()
public:
    ARailwayTrack();
#if WITH_EDITOR
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void Tick(float DeltaSeconds) override;
#endif
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    TObjectPtr<USplineComponent> Spline;

#if WITH_EDITORONLY_DATA
    UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category="Railway")
    TSoftObjectPtr<ARailwayJunction> StartJunction;

    UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category="Railway")
    TSoftObjectPtr<ARailwayJunction> EndJunction;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Railway")
    TArray<FRailwayStation> Stations;
#endif

#if WITH_EDITOR
    UFUNCTION(CallInEditor, BlueprintCallable, Category="Railway")
    void SnapEndpointsToJunctions();

private:
    void AlignEndpoints(bool bIncludeFreeJunctions);
#endif
};
