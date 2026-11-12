#pragma once

#include "Components/SplineComponent.h"
#include "GameFramework/Actor.h"
#include "RailwayUtils.h"
#include "RailwayRouteSettings.h"
#include "RailwayDataActor.generated.h"

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayBakedTrack
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    FName Name;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    FGuid Id;

    UPROPERTY()
    FSplineCurves Curves;

    UPROPERTY()
    FTransform SplineToWorld;

    UPROPERTY()
    FVector DefaultUp = FVector::UpVector;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    int32 FirstEdge = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    int32 NumEdges = 0;

    UPROPERTY()
    int32 FirstStation = 0;

    UPROPERTY()
    int32 NumStations = 0;

    double Length() const { return Curves.GetSplineLength(); }
    bool IsLoop() const { return Curves.Position.bIsLooped; }
    FVector PositionAt(double Distance) const;
    FTransform PoseAt(double Distance, bool bReverse) const;
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayEdge
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    int32 TrackIndex = INDEX_NONE;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    int32 FromNode = INDEX_NONE;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    int32 ToNode = INDEX_NONE;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    double StartDistance = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    double EndDistance = 0;

    UPROPERTY()
    int32 FirstSuccessor = 0;

    UPROPERTY()
    int32 NumSuccessors = 0;

    bool IsReverse() const { return EndDistance < StartDistance; }
    double Sign() const { return IsReverse() ? -1.0 : 1.0; }
    bool Contains(double D) const { return D >= FMath::Min(StartDistance, EndDistance) && D <= FMath::Max(StartDistance, EndDistance); }
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayBakedStation
{
    GENERATED_BODY()

    /** Canonical forward edge. Definition.TrackDistance is in native spline cm. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    int32 EdgeIndex = INDEX_NONE;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    FRailwayStation Definition;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    FGuid Id;
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayNetworkData
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    TArray<FRailwayBakedTrack> Tracks;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    TArray<FRailwayEdge> Edges;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    TArray<FRailwayBakedStation> Stations;

    UPROPERTY()
    TArray<FVector> Nodes;

    UPROPERTY()
    TArray<int32> Successors;

    /** Lower bound on full-edge cost per cm of node-to-node displacement. */
    UPROPERTY()
    double MinCostPerCm = 1.0;

    UPROPERTY()
    FGuid BuildId;

    /** Source IDs are only resolved when translating settings or restoring saves. */
    UPROPERTY()
    TMap<FGuid, int32> TrackById;

    UPROPERTY()
    TMap<FGuid, int32> StationById;

    UPROPERTY()
    TMap<int32, FGuid> JunctionByNode;

    UPROPERTY()
    FGuid NetworkId;

    bool ResolvePoint(const FRailwayRoutePoint& Point, FRailwayTrackLocation& Location, int32& StationIndex) const;
    bool CanFollow(int32 From, int32 To) const;
};

/** Cooked per-level railway network. Geometry is already in map coordinates. */
UCLASS(NotBlueprintable, HideCategories=(Transform, Collision, Replication, Input, HLOD, DataLayers))
class RAILWAY_API ARailwayDataActor final : public AActor
{
    GENERATED_BODY()

public:
    ARailwayDataActor();
    virtual void PostRegisterAllComponents() override;
    virtual void PostUnregisterAllComponents() override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    FRailwayNetworkData Network;

    UFUNCTION(BlueprintPure, Category="Railway")
    bool SampleTrack(FRailwayTrackLocation Location, bool bReverse, FTransform& Pose) const;

    UFUNCTION(BlueprintCallable, Category="Railway")
    bool FindRoutePlan(FRailwayTrackLocation Start, FRailwayTrackLocation Destination,
        FRailwayPlan& OutPlan, FString& OutError, bool bStopAtIntermediateStations = false) const;

    UFUNCTION(BlueprintCallable, Category="Railway")
    bool FindRoutePlanToStation(FRailwayTrackLocation Start, int32 StationIndex,
        FRailwayPlan& OutPlan, FString& OutError, bool bStopAtIntermediateStations = false) const;

    virtual void ApplyWorldOffset(const FVector& Offset, bool bWorldShift) override;
#if WITH_EDITOR
    virtual bool ActorTypeSupportsDataLayer() const override { return false; }
    virtual bool ActorTypeSupportsExternalDataLayer() const override { return false; }
    virtual bool CanChangeIsSpatiallyLoadedFlag() const override { return false; }
#endif
};
