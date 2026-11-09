#include "RailwayNetworkActor.h"

#include "Net/UnrealNetwork.h"

ARailwayNetworkActor::ARailwayNetworkActor()
{
    bReplicates = true;
    bIsSpatiallyLoaded = false;
}

void ARailwayNetworkActor::InitializeId()
{
    if (IsTemplate() || RailwayId.IsValid()) return;
#if WITH_EDITOR
    // 旧地图在首次打开、PIE 与 cook 中可从同一个编辑器 GUID 升级。
    RailwayId = GetActorGuid();
#endif
    if (!RailwayId.IsValid()) RailwayId = FGuid::NewGuid();
}

void ARailwayNetworkActor::PostLoad()
{
    Super::PostLoad();
    InitializeId();
}

void ARailwayNetworkActor::PostActorCreated()
{
    Super::PostActorCreated();
    InitializeId();
}

void ARailwayNetworkActor::PostDuplicate(EDuplicateMode::Type DuplicateMode)
{
    Super::PostDuplicate(DuplicateMode);
    if (DuplicateMode == EDuplicateMode::Normal && !IsTemplate())
        RailwayId = FGuid::NewGuid();
}

#if WITH_EDITOR
void ARailwayNetworkActor::PostEditImport()
{
    Super::PostEditImport();
    if (!IsTemplate()) RailwayId = FGuid::NewGuid();
}
#endif

void ARailwayNetworkActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ThisClass, RailwayId);
}
