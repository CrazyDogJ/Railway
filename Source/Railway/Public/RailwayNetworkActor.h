#pragma once

#include "GameFramework/Actor.h"
#include "RailwayNetworkActor.generated.h"

/** 轨道、岔口和场景站点共用的持久身份。显示名、Actor 路径和数组顺序均不参与寻址。 */
UCLASS(Abstract)
class RAILWAY_API ARailwayNetworkActor : public AActor
{
    GENERATED_BODY()

public:
    ARailwayNetworkActor();

    /** 程序生成的网络可在发车前恢复自己保存的 ID。已发布的关卡实例应保留此值。 */
    UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Replicated, TextExportTransient, Category="Railway|Identity")
    FGuid RailwayId;

    virtual void PostLoad() override;
    virtual void PostActorCreated() override;
    virtual void PostDuplicate(EDuplicateMode::Type DuplicateMode) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

#if WITH_EDITOR
    virtual void PostEditImport() override;
#endif
private:
    void InitializeId();
};
