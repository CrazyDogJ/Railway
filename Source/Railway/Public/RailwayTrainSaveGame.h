#pragma once

#include "CoreMinimal.h"
#include "RailwayUtils.h"
#include "RailwayTrainSaveGame.generated.h"

class ARailwayTrain;

UENUM(BlueprintType)
enum class ERailwayRestoreResult : uint8 { None, Restored, Remapped, ParkedAtFallback, Failed };

UENUM()
enum class ERailwayAnchorKind : uint8 { Distance, TrackStart, TrackEnd, Junction, Station };

USTRUCT()
struct FRailwaySavedAnchor
{
    GENERATED_BODY()

    UPROPERTY(SaveGame)
    ERailwayAnchorKind Kind = ERailwayAnchorKind::Distance;

    UPROPERTY(SaveGame)
    double Distance = 0;

    UPROPERTY(SaveGame)
    FGuid Source;
};

USTRUCT()
struct FRailwaySavedPass
{
    GENERATED_BODY()

    UPROPERTY(SaveGame)
    FGuid Track;

    UPROPERTY(SaveGame)
    bool bReverse = false;

    UPROPERTY(SaveGame)
    FRailwaySavedAnchor Entry;

    UPROPERTY(SaveGame)
    FRailwaySavedAnchor Exit;

    UPROPERTY(SaveGame)
    bool bStartOnIncomingEdge = false;
};

USTRUCT()
struct FRailwaySavedStop
{
    GENERATED_BODY()

    UPROPERTY(SaveGame)
    FGuid Station;

    UPROPERTY(SaveGame)
    int32 Pass = INDEX_NONE;

    UPROPERTY(SaveGame)
    bool bAtClosingSeam = false;
};

USTRUCT()
struct FRailwayRouteBookmark
{
    GENERATED_BODY()

    UPROPERTY(SaveGame)
    FGuid Network;

    UPROPERTY(SaveGame)
    TArray<FRailwaySavedPass> Passes;

    UPROPERTY(SaveGame)
    TArray<FRailwaySavedStop> Stops;

    UPROPERTY(SaveGame)
    int32 OccupiedPass = INDEX_NONE;

    UPROPERTY(SaveGame)
    FRailwaySavedAnchor Position;
};

/** 持久化权威列车的行程与服务进度。姿态、段号和站名从计划与游标恢复。 */
USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayTrainSaveGame
{
    GENERATED_BODY()

    /** 兼容原有 C++ 入口，由列车负责采集可保存的运行状态。 */
    static FRailwayTrainSaveGame SaveTrain(const ARailwayTrain* Train);

    /** 将记录交给目标列车恢复，旧行程无法恢复时尝试停放在默认轨道点。 */
    void LoadTrain(ARailwayTrain* Train) const;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
    bool bValid = false;

    /** 已停留的秒数。读档时用当前站点配置减去该值。 */
    UPROPERTY(SaveGame)
    double ElapsedDwell = 0;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
    ERailwayDriveMode DriveMode = ERailwayDriveMode::Automatic;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
    FRailwayPlan RailwayPlan;

    UPROPERTY(SaveGame)
    FRailwayRouteBookmark Bookmark;

    UPROPERTY(SaveGame)
    FRailwayTurnouts Turnouts;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
    double Distance = 0;

    /** 保存运行时的有符号速度（cm/s），驾驶输入在读档时回零。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
    float Speed = 0;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
    ERailwayState State = ERailwayState::Idle;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
    double Wait = 0;

    /** 读档后避免同一次停站重复发出预离站通知。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
    bool bPreLeaveSent = false;

    /** 首个未服务站次的下标。同里程可以有多站，仅靠 Distance 无法恢复此游标。 */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
    int32 NextStop = 0;
};
