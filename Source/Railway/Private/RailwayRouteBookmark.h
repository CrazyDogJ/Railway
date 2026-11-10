#pragma once

#include "RailwayTrainSaveGame.h"

class ARailwayDataActor;

namespace Railway::Persistence
{
    bool Capture(const ARailwayDataActor& Data, const FRailwayPlan& Plan, const TArray<double>& SectionEnds,
        double Distance, FRailwayRouteBookmark& Bookmark);
    bool Remap(const ARailwayDataActor& Data, const FRailwayTrainSaveGame& Save,
        FRailwayPlan& Plan, double& Distance, FString& Error);
}
