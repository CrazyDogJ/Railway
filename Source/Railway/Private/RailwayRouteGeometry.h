#pragma once

#include "RailwayDataActor.h"

namespace Railway::Geometry
{
    void BuildSectionEnds(const FRailwayPlan& Plan, TArray<double>& Ends);
    bool Locate(const FRailwayNetworkData& Data, const FRailwayPlan& Plan, const TArray<double>& Ends,
        double Distance, int32& SectionIndex, double& TrackDistance);

    bool Sample(const ARailwayDataActor* Data, const FRailwayPlan& Plan, const TArray<double>& Ends,
        double Distance, int32& SectionIndex, double& TrackDistance, FTransform& Pose);

    bool FindPlanDistance(const FRailwayNetworkData& Data, const FRailwayPlan& Plan, const TArray<double>& Ends,
        const FRailwayTrackLocation& Location, int32 SectionHint, TOptional<double> Preferred, double& OutDistance, FString& Error);
}
