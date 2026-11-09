#pragma once

#include "CoreMinimal.h"
#include "RailwayUtils.h"
#include "RailwayNetworkActor.h"
#include "RailwayTrack.generated.h"

class USplineComponent;

class ARailwayTrack;

UCLASS(Abstract)
class RAILWAY_API ARailwayTrackAnchor : public ARailwayNetworkActor
{
    GENERATED_BODY()
public:
    ARailwayTrackAnchor();
    virtual void Serialize(FArchive& Ar) override;
    virtual void PostLoad() override;
    virtual void PostRegisterAllComponents() override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    /** 位置和朝向所依据的轨道，置空后可自由放置锚点。 */
    UPROPERTY(EditInstanceOnly, BlueprintReadOnly, ReplicatedUsing=OnRep_Anchor, Category="Railway")
    TObjectPtr<ARailwayTrack> HostTrack;

    /** 从宿主样条首点起算的厘米数。缩短轨道时暂时贴到末端，保留设定里程。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, ReplicatedUsing=OnRep_Anchor, Category="Railway", meta=(ClampMin="0", Units="cm"))
    double TrackDistance = 0;

    /** 将保存的里程应用到世界姿态和父级附着。 */
    UFUNCTION(BlueprintCallable, Category="Railway|Anchor")
    void RefreshAnchor();

    /** 按样条里程（cm）绑定锚点。传空轨道可解除附着。 */
    UFUNCTION(BlueprintCallable, Category="Railway|Anchor")
    void SetTrackAnchorAtDistance(ARailwayTrack* Track, double DistanceCm);

    /** 旧蓝图仍按原输入单位调用，绑定时立即换算成厘米。 */
    UFUNCTION(BlueprintCallable, Category="Railway|Anchor", meta=(DeprecatedFunction, DeprecationMessage="Use Set Track Anchor At Distance (cm)."))
    void SetTrackAnchor(ARailwayTrack* Track, float NormalizedPosition);

    /** 将当前位置投影到宿主样条，记录投影点距首点的弧长。 */
    UFUNCTION(BlueprintCallable, Category="Railway|Anchor")
    void ProjectToHost();
    virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void Tick(float DeltaSeconds) override;
    virtual void PostEditMove(bool bFinished) override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif

#if WITH_EDITORONLY_DATA
protected:
    void CreateMarkerArrow(FName SubobjectName, FColor Color);
#endif

private:
    void UpgradeTrackDistance();

    UFUNCTION()
    void OnRep_Anchor();

    UFUNCTION(BlueprintGetter, Category="Railway|Anchor")
    float GetLegacyTrackPosition() const;

    // 保留旧反射名加载已有 Actor 和蓝图。仅等待宿主时保存非负 Alpha。
    UPROPERTY(BlueprintGetter=GetLegacyTrackPosition, Category="Railway|Anchor", meta=(AllowPrivateAccess="true", DeprecatedProperty, DeprecationMessage="Use Track Distance (cm)."))
    float TrackPosition = -1.f;
};

UCLASS(Blueprintable)
class RAILWAY_API ARailwayJunction : public ARailwayTrackAnchor
{
    GENERATED_BODY()
public:
    ARailwayJunction();
};

UCLASS(Blueprintable, meta=(DisplayName="Railway Station"))
class RAILWAY_API ARailwayStationMarker : public ARailwayTrackAnchor
{
    GENERATED_BODY()
public:
    ARailwayStationMarker();

    /** 查询或到站时从站点读取当前定义。子类可按场景规则提供站名、位置或停站时长。 */
    virtual FRailwayStation GetStationDefinition() const;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category="Station")
    FName StationName = TEXT("Station");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Station", meta=(ClampMin="0", Units="s"))
    float DwellSeconds = 3.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category="Station")
    FInstancedStruct CustomData;

    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITORONLY_DATA
    /** 编辑器视口中的站名标签。 */
    UPROPERTY()
    TObjectPtr<class UTextRenderComponent> Label;
#endif
};

UCLASS(Blueprintable)
class RAILWAY_API ARailwayTrack : public ARailwayNetworkActor
{
    GENERATED_BODY()
public:
    ARailwayTrack();
    virtual void PostLoad() override;
    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    /** 为运行时新增的内嵌站点分配身份。重复的现有 ID 由规划器报告。 */
    void InitializeStationIds();
    FGuid GetLayoutSignature() const;
    /** 仅用于辨认使用 Alpha 计算岔口签名的旧计划。 */
    FGuid GetLegacyLayoutSignature() const;
#if WITH_EDITOR
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void Tick(float DeltaSeconds) override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
    /** 对齐开放轨道端点。编辑器按钮可同时吸附无宿主的自由岔口。 */
    UFUNCTION(BlueprintCallable, Category="Railway")
    void AlignEndpoints(bool bIncludeFreeJunctions);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    TObjectPtr<USplineComponent> Spline;

    UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category="Railway")
    TObjectPtr<ARailwayJunction> StartJunction;

    UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category="Railway")
    TObjectPtr<ARailwayJunction> EndJunction;

    /** 随轨道序列化的旧版站点，与场景中的 Station Marker 一起参与规划。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Replicated, AdvancedDisplay, Category="Railway", meta=(DisplayName="Legacy Stations"))
    TArray<FRailwayStation> Stations;

    UFUNCTION(CallInEditor, BlueprintCallable, Category="Railway")
    void SnapEndpointsToJunctions();

private:
    FGuid CalculateLayoutSignature(bool bLegacyAlpha) const;
    void UpgradeStationDistances();
    mutable TWeakObjectPtr<USplineComponent> CachedSpline;
    mutable int32 CachedSplineVersion = INDEX_NONE;
    mutable FGuid CachedCurveSignature;
};
