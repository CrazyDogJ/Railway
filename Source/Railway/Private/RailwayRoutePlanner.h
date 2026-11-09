#pragma once

#include "RailwayUtils.h"

class UWorld;
class FRailwayWorldIndex;

// 仅在切换计划时使用。先准备索引和起始姿态，全部成功后再交给列车。
struct FRailwayPreparedPlan
{
    FRailwayPlan Plan;
    TArray<double> SectionEnds;
    int32 StartSection = INDEX_NONE;
    FTransform StartPose;
};

namespace Railway::Routing
{
    inline constexpr double JunctionToleranceCm = 1.0;
    const TCHAR* CheckRoutingTrack(const ARailwayTrack* Track, const UWorld* World, const FRailwayWorldIndex& WorldIndex);

    // 按 A* 实际区间生成行车停站表。显式目的站只在终点里程处排到最后。
    bool BuildPathStops(const FRailwayWorldIndex& Index, bool bStopAtIntermediateStations,
        const FRailwayStationRef* DestinationStation, FRailwayPlan& Plan, FString& OutError);

    // FirstEntry 仅用于途中改线，单位是第一条样条上的厘米数。
    bool Build(UWorld* World, const TArray<FRailwayLeg>& Legs, TOptional<double> FirstEntry,
        FRailwayPlan& OutPlan, FString& OutError);

    enum class EPlanOrigin { Prepared, IdentifiedSave, AlphaIdentifiedSave, LegacySave };

    // 对外部、存档或新生成的计划使用同一组检查。入参按值接收，排序不会改变调用者的计划。
    bool Prepare(UWorld* World, FRailwayPlan Plan, double StartDistance,
        FRailwayPreparedPlan& OutPrepared, FString& OutError, EPlanOrigin Origin = EPlanOrigin::Prepared);
}
