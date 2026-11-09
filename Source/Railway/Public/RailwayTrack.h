#pragma once

#include "CoreMinimal.h"
#include "RailwayUtils.h"
#include "GameFramework/Actor.h"
#include "RailwayTrack.generated.h"

class USplineComponent;

class ARailwayTrack;

UCLASS(Abstract)
class RAILWAY_API ARailwayTrackAnchor : public AActor
{
    GENERATED_BODY()
public:
    ARailwayTrackAnchor();

    /** 位置和朝向所依据的轨道，置空后可自由放置锚点。 */
    UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category="Railway")
    TObjectPtr<ARailwayTrack> HostTrack;

    /** 从样条首点起算的弧长比例，轨道长度改变后仍按此比例定位。 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Railway", meta=(ClampMin="0", ClampMax="1"))
    float TrackPosition = 0.5f;

    /** 将保存的弧长比例应用到世界姿态和父级附着。 */
    UFUNCTION(BlueprintCallable, Category="Railway|Anchor")
    void RefreshAnchor();

    // Bind to a normalized position and immediately update the attachment.
    UFUNCTION(BlueprintCallable, Category="Railway|Anchor")
    void SetTrackAnchor(ARailwayTrack* Track, float NormalizedPosition);

    /** 将锚点当前位置投影到宿主样条，供拖动或更换宿主时更新比例。 */
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

    /** 构建行程时读取的站点快照。子类可按场景规则提供站名、位置或停站时长。 */
    virtual FRailwayStation GetStationDefinition() const;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Station")
    FName StationName = TEXT("Station");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Station", meta=(ClampMin="0", Units="s"))
    float DwellSeconds = 3.f;

    virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITORONLY_DATA
    /** 编辑器视口中的站名标签。 */
    UPROPERTY()
    TObjectPtr<class UTextRenderComponent> Label;
#endif
};

UCLASS(Blueprintable)
class RAILWAY_API ARailwayTrack : public AActor
{
    GENERATED_BODY()
public:
    ARailwayTrack();
#if WITH_EDITOR
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void Tick(float DeltaSeconds) override;
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
    UPROPERTY(EditAnywhere, BlueprintReadOnly, AdvancedDisplay, Category="Railway", meta=(DisplayName="Legacy Stations"))
    TArray<FRailwayStation> Stations;

    UFUNCTION(CallInEditor, BlueprintCallable, Category="Railway")
    void SnapEndpointsToJunctions();
};
