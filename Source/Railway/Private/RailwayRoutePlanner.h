#pragma once

#include "RailwayDataActor.h"

struct FRailwayPreparedPlan
{
    const ARailwayDataActor* Data = nullptr;
    FRailwayPlan Plan;
    TArray<double> SectionEnds;
    double StartDistance = 0;
    int32 StartSection = INDEX_NONE;
    FTransform StartPose;

    bool SetStartDistance(double Distance, FString& Error);
};

namespace Railway::Routing
{
    inline constexpr double JunctionToleranceCm = 1.0;
    bool AppendTrackPass(const FRailwayNetworkData& Data, int32 TrackIndex, bool bReverse,
        double Entry, double Exit, bool bPreferIncoming, FRailwayPlan& Plan, FString& Error);
    void BuildStops(const FRailwayNetworkData& Data, FRailwayPlan& Plan, bool bIntermediate, int32 Destination);
    bool Build(const ARailwayDataActor* Data, const TArray<FRailwayLeg>& Legs, TOptional<double> FirstEntry,
        FRailwayPlan& OutPlan, FString& Error);
    bool Prepare(const ARailwayDataActor* Data, FRailwayPlan Plan, double Start,
        FRailwayPreparedPlan& Prepared, FString& Error);
}
