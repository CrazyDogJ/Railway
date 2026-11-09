#include "RailwayTrainSaveGame.h"
#include "RailwayTrain.h"
#include "Railway.h"

FRailwayTrainSaveGame FRailwayTrainSaveGame::SaveTrain(const ARailwayTrain* Train)
{
    FRailwayTrainSaveGame Out;
    if (!IsValid(Train) || !Train->HasAuthority()) return Out;
    Out.bValid = true;
    Out.RailwayPlan = Train->ActivePlan;
    Out.RailwayPlan.NetworkRevision = 0;
    Out.Distance = Train->Distance;
    Out.State = Train->State;
    // A distance can have several stops, including ones already served.
    Out.NextStop = Train->NextStop;
    if (Out.State == ERailwayState::Running) Out.Speed = Train->Speed;
    if (Out.State == ERailwayState::Dwelling)
    {
        Out.Wait = Train->Wait;
        Out.bPreLeaveSent = Train->bPreLeaveSent;
    }
    return Out;
}

void FRailwayTrainSaveGame::LoadTrain(ARailwayTrain* Train) const
{
    if (!bValid || !IsValid(Train) || !Train->HasAuthority()) return;
    if (RailwayPlan.Sections.IsEmpty() && (State == ERailwayState::Idle || State == ERailwayState::InvalidRoute))
    {
        Train->ResetPlan();
        Train->SetState(State);
        Train->PublishMoveState(true);
        return;
    }

    FString Error;
    if (!Train->BeginPlan(RailwayPlan, Distance, this, Error))
        UE_LOG(LogRailway, Warning, TEXT("%s: could not restore train: %s"), *Train->GetName(), *Error);
}
