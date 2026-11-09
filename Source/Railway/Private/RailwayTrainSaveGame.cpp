#include "RailwayTrainSaveGame.h"

#include "RailwayTrain.h"

FRailwayTrainSaveGame FRailwayTrainSaveGame::SaveTrain(const ARailwayTrain* Train)
{
    return IsValid(Train) ? Train->GetSaveGameData() : FRailwayTrainSaveGame();
}

void FRailwayTrainSaveGame::LoadTrain(ARailwayTrain* Train) const
{
    if (IsValid(Train)) Train->LoadSaveGameData(*this);
}
