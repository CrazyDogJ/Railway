#pragma once

class ARailwayDataActor;
class ARailwayTrain;
class UWorld;

namespace Railway::Tests
{
    ARailwayDataActor* MakeNetwork(UWorld& World);
    ARailwayDataActor* MakeFork(UWorld& World);
    ARailwayTrain* OnlyTrain(UWorld& World);
    ARailwayTrain* MakeTrain(UWorld& World, ARailwayDataActor* Data);
}
