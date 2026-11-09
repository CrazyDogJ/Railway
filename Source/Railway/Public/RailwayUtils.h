#pragma once

#include "CoreMinimal.h"
#include "RailwayUtils.generated.h"

class ARailwayTrack;

/** 一个有方向的轨道引用，允许在同一路线中多次经过同一轨道。 */
USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayLeg
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Route")
	TObjectPtr<ARailwayTrack> Track;

	/** 为真时沿样条里程减小的方向行驶。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Route")
	bool bReverse = false;
};

/** 实际经过的轨道区间，入口可位于中段，闭环区间可跨越首尾接缝。 */
USTRUCT(BlueprintType)
struct RAILWAY_API FRailwaySection
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
	FRailwayLeg Leg;

	/** 入口相对样条首点的弧长，单位 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
	double EntryDistance = 0;

	/** 沿 Leg 指定方向行驶的正长度，单位 cm。 */
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

/** 一次停靠，Distance 是整条行程的累计厘米数。同名站或不同圈次各占一项。 */
/** 运行时计划保留完整的行驶区间，停站表可以独立裁剪为直达服务。 */
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

    // Finish service after dwelling at the last scheduled stop, even mid-section.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
    bool bFinishAtLastStop = false;

    // Replicated with the plan, omitted from disk saves. Zero means no run yet.
    UPROPERTY(Transient)
    uint32 NetworkRevision = 0;
};

UENUM(BlueprintType)
enum class ERailwayDriveMode : uint8 { Automatic, Manual };

UENUM(BlueprintType)
enum class ERailwayState : uint8 { Idle, Running, Dwelling, Finished, InvalidRoute };

/** 权威运动样本。服务器时间用于客户端插值，里程相对当前版本的计划。 */
USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayMoveState
{
    GENERATED_BODY()

    /** 与 ActivePlan.NetworkRevision 一致后才可在客户端采用。 */
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

    /** 预计停站结束的服务器绝对时间，单位秒，仅在 Dwelling 时使用。 */
    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double DwellEndTime = 0;
};
