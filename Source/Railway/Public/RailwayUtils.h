#pragma once

#include "CoreMinimal.h"
#include "RailwayUtils.generated.h"

class ARailwayTrack;

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayLeg
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Route")
	TObjectPtr<ARailwayTrack> Track;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Route")
	bool bReverse = false;
};

// Distances are in cm; EntryDistance is measured from spline point zero.
USTRUCT(BlueprintType)
struct RAILWAY_API FRailwaySection
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
	FRailwayLeg Leg;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
	double EntryDistance = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
	double TravelDistance = 0;
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayStation
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Station")
	FName Name = TEXT("Station");

	// Fraction of arc length measured from spline point zero.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Station", meta=(ClampMin="0", ClampMax="1"))
	float Position = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Station", meta=(ClampMin="0", Units="s"))
	float DwellSeconds = 3.f;
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayPlannedStop
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
	double Distance = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
	FRailwayStation Station;
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayPlan
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
	TArray<FRailwaySection> Sections;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
	TArray<FRailwayPlannedStop> Stops;

    // Replicated with the plan, omitted from disk saves. Zero means no run yet.
    UPROPERTY(Transient)
    uint32 NetworkRevision = 0;
};

UENUM(BlueprintType)
enum class ERailwayState : uint8 { Idle, Running, Dwelling, Finished, InvalidRoute };
// One authoritative sample. Distance always increases along the plan, including reverse legs.
USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayMoveState
{
    GENERATED_BODY()

    UPROPERTY()
    uint32 PlanRevision = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double ServerTime = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double Distance = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    float Speed = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    ERailwayState State = ERailwayState::Idle;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    int32 NextStop = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double DwellEndTime = 0;
};
