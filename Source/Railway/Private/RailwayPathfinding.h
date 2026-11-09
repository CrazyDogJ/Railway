#pragma once

#include "RailwayUtils.h"

class UWorld;

namespace Railway::Routing
{
    bool FindPath(UWorld* World, const FRailwayTrackLocation& Start, const FRailwayTrackLocation& Destination,
        bool bStopAtIntermediateStations, FRailwayPlan& OutPlan, FString& OutError);

    bool FindPathToStation(UWorld* World, const FRailwayTrackLocation& Start, ARailwayTrack* HostTrack,
        ERailwayStationSource Source, FGuid StationId, bool bStopAtIntermediateStations, FRailwayPlan& OutPlan, FString& OutError);
}
