#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RailwayMoveBuffer.h"
#include "RailwayTrainSaveGame.h"
#include "RailwayTrain.generated.h"

class UStaticMeshComponent;
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
    
    /** 按行驶顺序排列的轨道与方向，发车时生成实际执行的 ActivePlan。 */
    UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category="Railway")
    TArray<FRailwayLeg> Route;

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

    // Signed traction / brake lever. Zero coasts. Submit on input changes from the owning player.
    UFUNCTION(BlueprintCallable, Server, Reliable, Category="Railway|Driving")
    void ServerSetDriveInput(float Value);

    UFUNCTION(BlueprintCallable, Server, Reliable, Category="Railway|Driving")
    void ServerSetDriveMode(ERailwayDriveMode NewMode);

    UPROPERTY(Transient, BlueprintReadOnly, Replicated, Category="Railway|Driving")
    float DriveInput = 0.f;

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

    // Builds a snapshot of the current geometry without resetting this train.
    UFUNCTION(BlueprintCallable, Category="Railway|Route")
    bool BuildRoutePlan(const TArray<FRailwayLeg>& Legs, FRailwayPlan& OutPlan, FString& OutError) const;

    // A rejected replacement leaves the current run intact. Exact-start stops remain pending.
    UFUNCTION(BlueprintCallable, Category="Railway|Route", BlueprintAuthorityOnly)
    bool StartPreparedRoute(const FRailwayPlan& Plan, double StartDistance, FString& OutError);

    // Samples the active plan. TrackDistance uses the spline's native direction.
    UFUNCTION(BlueprintPure, Category="Railway|Route")
    bool SampleRoute(double RouteDistance, int32& SectionIndex, double& TrackDistance, FTransform& Pose) const;

    // Samples a plan before it is started (for editor previews and planning).
    UFUNCTION(BlueprintPure, Category="Railway|Route")
    static bool SamplePlan(const FRailwayPlan& Plan, double RouteDistance, int32& SectionIndex, double& TrackDistance, FTransform& Pose);

    UFUNCTION(BlueprintPure, Category="Railway|Route")
    double GetRouteLength() const { return SectionEnds.IsEmpty() ? 0.0 : SectionEnds.Last(); }

    UFUNCTION(BlueprintPure, Category="Railway|Route")
    bool HasSentPreLeave() const { return bPreLeaveSent; }

    // Override in a train Blueprint to choose the editor preview track, distance and direction.
    UFUNCTION(BlueprintNativeEvent, Category="Railway|Editor")
    bool GetPreviewLocation(FRailwayLeg& PreviewLeg, double& TrackDistance) const;

    UFUNCTION(BlueprintCallable, Category="Railway|Save Game", BlueprintAuthorityOnly)
    FRailwayTrainSaveGame GetSaveGameData() const;
    
    UFUNCTION(BlueprintCallable, Category="Railway|Save Game", BlueprintAuthorityOnly)
    void LoadSaveGameData(FRailwayTrainSaveGame InSaveGame);
    
    /** 当前执行的线路快照，独立于编辑器中配置的 Route。 */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Railway|Route", ReplicatedUsing=OnRep_ActivePlan)
    FRailwayPlan ActivePlan;

    /** 沿整个行程累计行驶的厘米数，反向段也递增。 */
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
    /** 计算一个模拟步的驾驶目标速度（cm/s）。子类按传入的限速计算牵引目标，公共运动逻辑负责加减速度约束和停车制动。 */
    virtual double CalculateDriverTargetSpeed(double SpeedLimit, double DeltaSeconds) const;

private:
    UFUNCTION()
    void OnRep_TrainMoveState();

    UFUNCTION()
    void OnRep_ActivePlan();

    void PublishMoveState(bool bForce);
    void TickClientPresentation();
    FRailwayMoveBuffer MoveBuffer;

    // 累计段尾里程（cm），服务端换线或客户端收到计划时重建。
    TArray<double> SectionEnds;
    bool bPreLeaveSent = false;

    bool IsInService() const;
    FName GetStationForState(ERailwayState InState) const;
    void ResetMotion();
    void ResetPlan(ERailwayState NewState);
    void InstallPlan(FRailwayPlan&& Plan);
    void ActivatePlan(FRailwayPreparedPlan&& Prepared, double StartDistance, int32 StopCursor, ERailwayState NewState);
    bool ApplyReplacementPlan(FRailwayPlan&& NewPlan, double StartDistance, FString& OutError);
    bool FailRoute(const FString& Reason);
    bool UpdatePose();

    // 消耗停站时间后，剩余的子步时间可继续用于行驶。委托换线后立即中止旧子步。
    bool AdvanceDwell(double& DeltaSeconds);
    void ArriveAtTarget(uint32 ExpectedRevision);
    void Step(double DeltaSeconds);
};
