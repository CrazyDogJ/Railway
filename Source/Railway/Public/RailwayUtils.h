#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "RailwayUtils.generated.h"

/** Native distance from the first spline point, in cm. Both travel directions are eligible. */
USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayTrackLocation
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    int32 TrackIndex = INDEX_NONE;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway", meta=(Units="cm"))
    double TrackDistance = 0;
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayLeg
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
    int32 EdgeIndex = INDEX_NONE;
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwaySection
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
    FRailwayLeg Leg;

    /** Native spline distance at entry. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway", meta=(Units="cm"))
    double EntryDistance = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway", meta=(Units="cm"))
    double TravelDistance = 0;
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayStation
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Station")
    FName Name = TEXT("Station");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Station", meta=(ClampMin="0", Units="cm"))
    double TrackDistance = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Station", meta=(ClampMin="0", Units="s"))
    float DwellSeconds = 3.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Station")
    FInstancedStruct CustomData;
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayPlannedStop
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
    double Distance = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
    int32 StationIndex = INDEX_NONE;
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayPlan
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
	TArray<FRailwaySection> Sections;

    /** Identifies a complete bake. Track, edge and station references are int32 indices. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, SaveGame, Category="Railway")
    FGuid BuildId;

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

    /** 沿当前计划的有符号速度（cm/s）。客户端按此方向更新组件速度。 */
    UPROPERTY(BlueprintReadOnly, Category="Railway")
    float Speed = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    ERailwayState State = ERailwayState::Idle;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    int32 NextStop = 0;

    /** 预计停站结束的服务器绝对时间，单位秒，仅在 Dwelling 时使用。 */
    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double DwellEndTime = 0;

    /** 站点网络引用仍在传输时，也能显示权威站名。 */
    UPROPERTY()
    FName StationName;
};
