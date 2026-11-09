#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "RailwayUtils.generated.h"

class ARailwayTrack;
class ARailwayNetworkActor;

/** 寻路端点。TrackDistance 从样条首点沿原生方向量起，单位 cm。 */
USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayTrackLocation
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    TObjectPtr<ARailwayTrack> Track;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway", meta=(ClampMin="0", Units="cm"))
    double TrackDistance = 0;
};

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

	/** 沿 Leg 指定方向的行驶长度（cm）。零长度区间记录恰好位于换轨点的端点姿态。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
	double TravelDistance = 0;

    /** 跨加载的轨道身份。Leg.Track 是本次装载的运行时引用。 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, SaveGame, Category="Railway")
    FGuid TrackId;

    /** 编制计划时的局部几何与连接签名，用于拒绝已失效的里程。 */
    UPROPERTY(SaveGame)
    FGuid LayoutSignature;
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayStation
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Station")
	FName Name = TEXT("Station");

    /** 从宿主样条首点起算的厘米数，延长轨道不会改变该值。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Station", meta=(ClampMin="0", Units="cm"))
    double TrackDistance = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Station", meta=(ClampMin="0", Units="s"))
	float DwellSeconds = 3.f;

    /** 项目自定义的数据，由站点定义持有。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Station")
    FInstancedStruct CustomData;

    /** 内嵌站点和所属轨道的 ID 一起唯一定位一行。 */
    UPROPERTY(EditInstanceOnly, BlueprintReadWrite, SaveGame, AdvancedDisplay, Category="Station")
    FGuid StationId;

    bool Serialize(FArchive& Ar);
    void UpgradeTrackDistance(float SplineLength);

    // 旧属性名用于读取带标签的关卡和存档。没有宿主时暂存，转换后重置为 -1。
    UPROPERTY(SaveGame, NotReplicated)
    float Position = -1.f;
};

template<>
struct TStructOpsTypeTraits<FRailwayStation> : TStructOpsTypeTraitsBase2<FRailwayStation>
{
    enum { WithSerializer = true };
};

UENUM(BlueprintType)
enum class ERailwayStationSource : uint8
{
    Unspecified UMETA(Hidden),
    Inline,
    Marker,
    TrackStation,
    LegacySnapshot UMETA(Hidden)
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayStationRef
{
    GENERATED_BODY()

    /** Inline 表示由计划定义的临时站。来源丢失的世界站点不能降级为 Inline。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Station")
    ERailwayStationSource Source = ERailwayStationSource::Unspecified;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, SaveGame, Category="Station")
    FGuid StationId;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, SaveGame, Category="Station")
    FGuid HostTrackId;

    /** 编制站次时站点相对宿主首点的厘米数；-1 表示尚待绑定的旧引用。 */
    UPROPERTY(SaveGame)
    double PlannedTrackDistance = -1;

    // 仅用于将缺少 PlannedTrackDistance 的旧站次按其宿主长度升级。
    UPROPERTY(SaveGame, NotReplicated)
    float PlannedPosition = 0.f;

    /** 发车时解析、网络传递的场景引用；存档只带身份。 */
    UPROPERTY(Transient)
    TObjectPtr<ARailwayNetworkActor> SourceActor;
};

/** 一次停靠，Distance 是整条行程的累计厘米数。同名站或不同圈次各占一项。 */
USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayPlannedStop
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
	double Distance = 0;

	/** 计划自定义的 Inline 站点值。场景站点使用列车的 Resolve Planned Stop 蓝图节点。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
	FRailwayStation Station;

    /** 世界站次通过 Resolve Planned Stop 取得实时定义。Station 字段仅用于 Inline 及旧存档。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category="Railway")
    FRailwayStationRef Reference;

    // 没有来源字段的磁盘对象是旧快照。内存中新建的手工站次仍可直接使用 Station。
    bool Serialize(FArchive& Ar);
    void PostSerialize(const FArchive& Ar);
};

template<>
struct TStructOpsTypeTraits<FRailwayPlannedStop> : TStructOpsTypeTraitsBase2<FRailwayPlannedStop>
{
    enum { WithSerializer = true, WithPostSerialize = true };
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
