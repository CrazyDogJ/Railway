#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RailwayMoveBuffer.h"
#include "RailwayRouteSettings.h"
#include "RailwayTrainSaveGame.h"
#include "RailwayTrain.generated.h"

class UStaticMeshComponent;
class ARailwayDataActor;
struct FRailwayPreparedPlan;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRailwayArrival, FName, StationName);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FRailwayStateChanged, ERailwayState, PrevState, ERailwayState, NewRaywayState);

UCLASS(Blueprintable)
class RAILWAY_API ARailwayTrain : public AActor
{
    GENERATED_BODY()
public:
    ARailwayTrain();
#if WITH_EDITOR
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void OnConstruction(const FTransform& Transform) override;
    void RefreshPreview();
#endif
    virtual void BeginPlay() override;
    /** 仅在状态实际变化时通知监听器。 */
    void SetState(ERailwayState InState);
    /** 将速度沿车头方向写入根组件。bReverse 保留供已有 C++ 调用，姿态已包含行驶方向。 */
    void UpdateVelocity(bool bReverse = false) const;
    virtual void Tick(float DeltaSeconds) override;

    virtual void GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const override;
    
    /** 多个铁路网共存时显式指定。留空时优先使用同关卡的数据，再尝试世界中唯一的铁路网。 */
    UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category="Railway")
    TObjectPtr<ARailwayDataActor> RailwayData;

    UFUNCTION(BlueprintPure, Category="Railway")
    ARailwayDataActor* GetRailwayData() const;

    /** 按站点或轨道点寻路，或按指定轨道与方向行驶。 */
    UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category="Railway")
    FRailwayRouteSettings Route;

    UFUNCTION(BlueprintCallable, Category="Railway|Route")
    bool CompileRouteSettings(const FRailwayRouteSettings& Settings, FRailwayPlan& OutPlan, FString& OutError) const;

    /** 恢复失败时停放的位置。未设置或已移除时使用当前铁路网第一条轨道的起点。 */
    UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category="Railway|Save Game")
    FRailwayRoutePoint RecoveryPoint;

    UPROPERTY(Transient, BlueprintReadOnly, Category="Railway|Save Game")
    ERailwayRestoreResult LastRestoreResult = ERailwayRestoreResult::None;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    TObjectPtr<UStaticMeshComponent> Body;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    bool bAutoStart = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion", meta=(ClampMin="0", Units="cm/s"))
    float CruiseSpeed = 800.f;

    /** 最大牵引加速度，单位 cm/s²。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion", meta=(ClampMin="1"))
    float Acceleration = 180.f;

    /** 制动减速度的正值，单位 cm/s²，用于计算停车距离。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion", meta=(ClampMin="1"))
    float Braking = 240.f;

    // Seconds for the cruise speed error to decay by about 63% below the acceleration limit.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion", meta=(ClampMin="0.01", Units="s"))
    float ResponseTime = 0.45f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Replicated, Category="Railway|Driving")
    ERailwayDriveMode DriveMode = ERailwayDriveMode::Automatic;

    /** 手动牵引轴 [-1, 1]。正值向车头方向牵引，负值向车尾方向牵引，零为惰行。 */
    UFUNCTION(BlueprintCallable, Server, Reliable, Category="Railway|Driving")
    void ServerSetDriveInput(float Value);

    /** 独立制动轴 [0, 1]。制动优先于牵引，在任一行驶方向上减速至零。 */
    UFUNCTION(BlueprintCallable, Server, Reliable, Category="Railway|Driving")
    void ServerSetBrakeInput(float Value);

    UFUNCTION(BlueprintCallable, Server, Reliable, Category="Railway|Driving")
    void ServerSetDriveMode(ERailwayDriveMode NewMode);

    UPROPERTY(Transient, BlueprintReadOnly, Replicated, Category="Railway|Driving")
    float DriveInput = 0.f;

    UPROPERTY(Transient, BlueprintReadOnly, Replicated, Category="Railway|Driving")
    float BrakeInput = 0.f;

    // Index selects an occurrence in Plan.Stops, including repeated names and laps.
    UFUNCTION(BlueprintCallable, Category="Railway|Route")
    static bool MakeDirectPlan(const FRailwayPlan& Plan, int32 DestinationStopIndex, FRailwayPlan& OutPlan);

    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Railway|Route")
    bool GoDirectToStop(int32 DestinationStopIndex, FString& OutError);

    // Supply the current directed track first, then the desired connected tracks.
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Railway|Route")
    bool ChangeRemainingRoute(const TArray<FRailwayLeg>& NewLegs, FString& OutError);

    /** 当前运行状态，由 SetState 统一分发状态变化事件。 */
    UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category="Railway")
    ERailwayState State = ERailwayState::Idle;

    /** 沿计划的有符号速度（cm/s），倒车时为负。 */
    UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category="Railway")
    float Speed = 0.f;

    UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category="Railway")
    FName CurrentStation;

    /** 剩余停车时间低于此秒数时触发 PreStationLeave，每次停站最多触发一次。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway", meta=(ClampMin="0"))
    float PreStationLeaveTime = 2.0f;
    
    UPROPERTY(BlueprintAssignable, Category="Railway")
    FRailwayArrival OnStationArrival;
    
    UPROPERTY(BlueprintAssignable, Category="Railway")
    FRailwayArrival PreStationLeave;
    
    UPROPERTY(BlueprintAssignable, Category="Railway")
    FRailwayArrival OnStationLeave;
    
    UPROPERTY(BlueprintAssignable, Category="Railway")
    FRailwayStateChanged OnStateChanged;

    UFUNCTION(BlueprintCallable, Category="Railway", BlueprintAuthorityOnly)
    bool StartRoute();
    
    /** 返回下一停站及距该站的行程厘米数。无待停站时距离为 -1。 */
    UFUNCTION(BlueprintPure, Category="Railway")
    void GetNextStopInfo(double& OutDistanceRemain, FRailwayStation& OutStationInfo) const;

    /** 从烘焙站点表读取站名、时长和 CustomData。 */
    UFUNCTION(BlueprintPure, Category="Railway|Route")
    bool ResolvePlannedStop(const FRailwayPlannedStop& Stop, FRailwayStation& OutStation) const;

    /** 返回是否完成恢复或停放；LastRestoreResult 区分结果。 */
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Railway|Save Game")
    bool TryLoadSaveGameData(const FRailwayTrainSaveGame& Save, FString& OutError);

    // Builds a snapshot of the current geometry without resetting this train.
    UFUNCTION(BlueprintCallable, Category="Railway|Route")
    bool BuildRoutePlan(const TArray<FRailwayLeg>& Legs, FRailwayPlan& OutPlan, FString& OutError) const;

    /** 按弧长寻找最短路线。勾选 Stop At Intermediate Stations 时将实际经过的站点加入停站表。 */
    UFUNCTION(BlueprintCallable, Category="Railway|Pathfinding")
    bool FindRoutePlan(const FRailwayTrackLocation& Start, const FRailwayTrackLocation& Destination,
        FRailwayPlan& OutPlan, FString& OutError, bool bStopAtIntermediateStations = false) const;

    /** 按烘焙索引前往站点。可选择沿途停站，指定目的站始终是最后一次停靠。 */
    UFUNCTION(BlueprintCallable, Category="Railway|Pathfinding")
    bool FindRoutePlanToStation(const FRailwayTrackLocation& Start, int32 StationIndex,
        FRailwayPlan& OutPlan, FString& OutError, bool bStopAtIntermediateStations = false) const;

    /** 返回车体当前所在的轨道与样条原生里程，可直接连接寻路节点的 Start。 */
    UFUNCTION(BlueprintPure, Category="Railway|Route")
    bool GetCurrentTrackLocation(FRailwayTrackLocation& OutLocation) const;

    /** 从当前位置接续新计划；同轨反复经过时，SectionHint 可指定新计划中的区间下标。 */
    UFUNCTION(BlueprintCallable, Category="Railway|Route", BlueprintAuthorityOnly, meta=(AdvancedDisplay="SectionHint"))
    bool ReplacePreparedRoute(const FRailwayPlan& Plan, FString& OutError, int32 SectionHint = -1);

    /** 明确从给定的新计划累计里程重新发车，会定位车体并清零速度和驾驶输入。 */
    UFUNCTION(BlueprintCallable, Category="Railway|Route", BlueprintAuthorityOnly)
    bool RestartPreparedRouteAtDistance(const FRailwayPlan& Plan, double StartDistance, FString& OutError);

    /** 首次发车使用 StartDistance；已有计划时自动从实际车位换线。 */
    UFUNCTION(BlueprintCallable, Category="Railway|Route", BlueprintAuthorityOnly)
    bool StartPreparedRoute(const FRailwayPlan& Plan, double StartDistance, FString& OutError);

    // Samples the active plan. TrackDistance uses the spline's native direction.
    UFUNCTION(BlueprintPure, Category="Railway|Route")
    bool SampleRoute(double RouteDistance, int32& SectionIndex, double& TrackDistance, FTransform& Pose) const;

    // Samples a plan before it is started (for editor previews and planning).
    UFUNCTION(BlueprintPure, Category="Railway|Route")
    bool SamplePlan(const FRailwayPlan& Plan, double RouteDistance, int32& SectionIndex, double& TrackDistance, FTransform& Pose) const;

    UFUNCTION(BlueprintPure, Category="Railway|Route")
    double GetRouteLength() const { return SectionEnds.IsEmpty() ? 0.0 : SectionEnds.Last(); }

    UFUNCTION(BlueprintPure, Category="Railway|Route")
    bool HasSentPreLeave() const { return bPreLeaveSent; }

    // Override in a train Blueprint to choose the editor preview track, distance and direction.
    UFUNCTION(BlueprintNativeEvent, Category="Railway|Editor")
    bool GetPreviewLocation(FRailwayLeg& PreviewLeg, double& TrackDistance) const;

    // If your train is stay horizontal, update here.
    UFUNCTION(BlueprintNativeEvent, Category="Railway|Editor")
    bool UpdatePreviewRotation();
    
    UFUNCTION(BlueprintCallable, Category="Railway|Save Game", BlueprintAuthorityOnly)
    FRailwayTrainSaveGame GetSaveGameData() const;
    
    UFUNCTION(BlueprintCallable, Category="Railway|Save Game", BlueprintAuthorityOnly)
    void LoadSaveGameData(FRailwayTrainSaveGame InSaveGame);
    
    /** 当前执行的线路快照，独立于编辑器中配置的 Route。 */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Railway|Route", ReplicatedUsing=OnRep_ActivePlan)
    FRailwayPlan ActivePlan;

    /** 行程坐标（cm）。按计划前进时递增，倒车时递减。 */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Railway|Route")
    double Distance = 0;

    /** 当前站剩余停留秒数。 */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Railway|Route")
    double Wait = 0;

    /** 当前占用的 ActivePlan.Sections 下标。 */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Railway|Route")
    int32 Leg = 0;

    /** 首个未服务站次的下标，到站时即递增。 */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Railway|Route")
    int32 NextStop = 0;

    // Optional external speed ceiling in cm/s. Negative disables the limit.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion")
    float ExternalSpeedLimit = -1.f;

    /** 服务端正常行驶时的快照发送频率，单位 Hz。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Railway|Networking", meta=(ClampMin="1", ClampMax="60"))
    float SnapshotRateHz = 15.f;

    /** 客户端播放时刻相对服务器时钟的延迟秒数。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Railway|Networking", meta=(ClampMin="0", ClampMax="0.5", Units="s"))
    float InterpolationDelay = 0.15f;

    UPROPERTY(Transient, BlueprintReadOnly, ReplicatedUsing=OnRep_TrainMoveState, Category="Railway|Networking")
    FRailwayMoveState TrainMoveState;

protected:
    /** 计算一个模拟步的牵引目标速度（cm/s），手动模式允许负值。独立制动和计划边界由运动逻辑处理。 */
    virtual double CalculateDriverTargetSpeed(double SpeedLimit, double DeltaSeconds) const;

private:
    mutable TWeakObjectPtr<ARailwayDataActor> CachedRailwayData;
    mutable FGuid PreviewBuild;
    mutable FRailwayRouteSettings PreviewSettings;
    mutable FRailwaySection PreviewSection;

    UFUNCTION()
    void OnRep_TrainMoveState();

    UFUNCTION()
    void OnRep_ActivePlan();

    void PublishMoveState(bool bForce);
    void TickClientPresentation();
    FRailwayMoveBuffer MoveBuffer;
    // 客户端当前显示姿态所属的计划版本。
    uint32 PresentedPlanRevision = 0;

    // 累计段尾里程（cm），服务端换线或客户端收到计划时重建。
    TArray<double> SectionEnds;
    bool bPreLeaveSent = false;

    bool RestoreSavedService(const FRailwayTrainSaveGame& Save, FString& Error);
    bool ParkAtRecoveryPoint(ERailwayDriveMode Mode, FString& Error);
    bool IsInService() const;
    FName GetStationForState(ERailwayState InState) const;
    bool RefreshDwell();
    double ElapsedDwell = 0;
    void ClearDriverInputs();
    void ResetMotion();
    void ResetPlan(ERailwayState NewState);
    void InstallPlan(FRailwayPlan&& Plan);
    void ActivatePlan(FRailwayPreparedPlan&& Prepared, int32 StopCursor, ERailwayState NewState);
    bool ContinuePreparedPlan(FRailwayPreparedPlan&& Prepared, bool bMatchServedStops, FString& OutError);
    bool FailRoute(const FString& Reason);
    bool UpdatePose();

    // 消耗停站时间后，剩余的子步时间可继续用于行驶。委托换线后立即中止旧子步。
    bool AdvanceDwell(double& DeltaSeconds);
    void ArriveAtTarget(uint32 ExpectedRevision);
    void Step(double DeltaSeconds);
};
