#pragma once

#include "CoreMinimal.h"
#include "RailwayRouteSettings.generated.h"

/** The Details picker displays names from the train's baked network. */
USTRUCT(BlueprintType)
struct RAILWAY_API FRailwaySourceRef
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    FGuid Id;
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayRoutePoint
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    FRailwaySourceRef Source;

    /** Used for a track. A station supplies its own distance. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway", meta=(ClampMin="0", Units="cm"))
    double TrackDistance = 0;
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayTrackPass
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway", meta=(RailwayTracksOnly))
    FRailwaySourceRef Track;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    bool bReverse = false;

    /** -1 uses the connecting junction, or the beginning in the chosen direction on the first pass. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway", meta=(ClampMin="-1", Units="cm"))
    double EntryDistance = -1;

    /** -1 uses the connecting junction, or the end in the chosen direction on the last pass. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway", meta=(ClampMin="-1", Units="cm"))
    double ExitDistance = -1;
};

UENUM(BlueprintType)
enum class ERailwayRouteMode : uint8 { BetweenPoints, OrderedTracks };

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayRouteSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    ERailwayRouteMode Mode = ERailwayRouteMode::BetweenPoints;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway", meta=(EditCondition="Mode == ERailwayRouteMode::BetweenPoints", EditConditionHides))
    FRailwayRoutePoint Start;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway", meta=(EditCondition="Mode == ERailwayRouteMode::BetweenPoints", EditConditionHides))
    FRailwayRoutePoint Destination;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway", meta=(EditCondition="Mode == ERailwayRouteMode::OrderedTracks", EditConditionHides))
    TArray<FRailwayTrackPass> Tracks;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    bool bStopAtIntermediateStations = true;
};
