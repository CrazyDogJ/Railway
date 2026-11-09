#pragma once

#include "CoreMinimal.h"
#include "RailwayUtils.h"
#include "RailwayTrainSaveGame.generated.h"

class ARailwayTrain;

/** 持久化权威列车的行程与服务进度。姿态、段号和站名从计划与游标恢复。 */
USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayTrainSaveGame
{
    GENERATED_BODY()

    /** 兼容原有 C++ 入口，由列车负责采集可保存的运行状态。 */
    static FRailwayTrainSaveGame SaveTrain(const ARailwayTrain* Train);

    /** 将记录交给目标列车恢复，计划或进度无效时保留原行程。 */
    void LoadTrain(ARailwayTrain* Train) const;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
    bool bValid = false;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
    ERailwayDriveMode DriveMode = ERailwayDriveMode::Automatic;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
    FRailwayPlan RailwayPlan;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame)
    double Distance = 0;

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
