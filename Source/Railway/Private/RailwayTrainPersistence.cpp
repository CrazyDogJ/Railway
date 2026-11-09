#include "RailwayTrain.h"

#include "Railway.h"
#include "RailwayRoutePlanner.h"

namespace
{
// NextStop 同时编码已服务站次和正在停靠的站次。相同里程的站不能仅按距离推导。
bool IsValidProgress(const FRailwayTrainSaveGame& Saved, const FRailwayPreparedPlan& Prepared)
{
    const TArray<FRailwayPlannedStop>& Stops = Prepared.Plan.Stops;
    const int32 Cursor = Saved.NextStop;
    if (Cursor < 0 || Cursor > Stops.Num() ||
        (Cursor > 0 && Stops[Cursor - 1].Distance > Saved.Distance) ||
        (Stops.IsValidIndex(Cursor) && Stops[Cursor].Distance < Saved.Distance) ||
        !FMath::IsFinite(Saved.Speed) || Saved.Speed < 0 || !FMath::IsFinite(Saved.Wait) || Saved.Wait < 0)
        return false;

    if (Saved.State == ERailwayState::Dwelling)
        return Cursor > 0 && Stops[Cursor - 1].Distance == Saved.Distance;
    if (Saved.State == ERailwayState::Finished)
    {
        const double Terminus = Prepared.Plan.bFinishAtLastStop ? Stops.Last().Distance : Prepared.SectionEnds.Last();
        return Saved.Distance == Terminus && Cursor == Stops.Num();
    }
    return true;
}
}

FRailwayTrainSaveGame ARailwayTrain::GetSaveGameData() const
{
    FRailwayTrainSaveGame Saved;
    if (!HasAuthority()) return Saved;

    Saved.bValid = true;
    Saved.DriveMode = DriveMode;
    Saved.RailwayPlan = ActivePlan;
    Saved.RailwayPlan.NetworkRevision = 0;
    Saved.Distance = Distance;
    Saved.State = State;
    Saved.NextStop = NextStop;
    if (State == ERailwayState::Running) Saved.Speed = Speed;
    if (State == ERailwayState::Dwelling)
    {
        Saved.Wait = Wait;
        Saved.bPreLeaveSent = bPreLeaveSent;
    }
    return Saved;
}

void ARailwayTrain::LoadSaveGameData(FRailwayTrainSaveGame InSaveGame)
{
    if (!HasAuthority() || !InSaveGame.bValid) return;
    if (InSaveGame.RailwayPlan.Sections.IsEmpty() &&
        (InSaveGame.State == ERailwayState::Idle || InSaveGame.State == ERailwayState::InvalidRoute))
    {
        DriveMode = InSaveGame.DriveMode;
        ResetPlan(InSaveGame.State);
        return;
    }

    FString Error;
    FRailwayPreparedPlan Prepared;
    if (!Railway::Routing::Prepare(GetWorld(), MoveTemp(InSaveGame.RailwayPlan), InSaveGame.Distance, Prepared, Error))
    {
        UE_LOG(LogRailway, Warning, TEXT("%s: could not restore train: %s"), *GetName(), *Error);
        return;
    }
    if (!IsValidProgress(InSaveGame, Prepared))
    {
        UE_LOG(LogRailway, Warning, TEXT("%s: could not restore train: invalid train progress in save"), *GetName());
        return;
    }

    ResetMotion();
    DriveMode = InSaveGame.DriveMode;
    if (InSaveGame.State == ERailwayState::Running) Speed = InSaveGame.Speed;
    if (InSaveGame.State == ERailwayState::Dwelling)
    {
        Wait = InSaveGame.Wait;
        bPreLeaveSent = InSaveGame.bPreLeaveSent;
    }
    ActivatePlan(MoveTemp(Prepared), InSaveGame.Distance, InSaveGame.NextStop, InSaveGame.State);
}
