#pragma once

#include "RailwayUtils.h"

class ARailwayStationMarker;
class ARailwayNetworkActor;
class UWorld;

/** 当前已加载世界的短期索引。在编制或绑定计划时构造，不跨关卡缓存 Actor 和站点定义。 */
class FRailwayWorldIndex
{
public:
    explicit FRailwayWorldIndex(UWorld* World);
    ARailwayNetworkActor* Find(const FGuid& Id) const;
    TArray<ARailwayTrack*> GetTracks() const;
    bool BindStop(FRailwayPlannedStop& Stop, FString& Error) const;

    /** 枚举同轨道上可使用的世界站点。重号会导致报错，避免把存档绑定到任意一个对象。 */
    bool VisitStations(ARailwayTrack& Track,
        TFunctionRef<bool(const FRailwayStation&, const FRailwayStationRef&)> Visitor, FString& Error) const;

private:
    UWorld* World;
    TMap<FGuid, ARailwayNetworkActor*> Actors;
    TMultiMap<ARailwayTrack*, ARailwayStationMarker*> Markers;
};

namespace Railway::Stations
{
    bool Read(const UWorld* World, const FRailwayPlannedStop& Stop, FRailwayStation& OutStation);
    bool SameSource(const FRailwayStationRef& A, const FRailwayStationRef& B);
}
