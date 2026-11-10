#include "RailwayTrain.h"

#include "Railway.h"
#include "RailwayRouteBookmark.h"
#include "RailwayRoutePlanner.h"
#include "UObject/Class.h"

namespace
{
// NextStop 同时编码已服务站次和正在停靠的站次。相同里程的站不能仅按距离推导。
bool IsValidProgress(const FRailwayTrainSaveGame& Saved, const FRailwayPreparedPlan& Prepared)
{
    const TArray<FRailwayPlannedStop>& Stops = Prepared.Plan.Stops;
    const int32 Cursor = Saved.NextStop;
    // 倒车可退到已服务站次之前，游标仍指向下一待服务站。
    if (Cursor < 0 || Cursor > Stops.Num() ||
        (Stops.IsValidIndex(Cursor) && Stops[Cursor].Distance < Prepared.StartDistance) ||
        !FMath::IsFinite(Saved.Speed) || !FMath::IsFinite(Saved.Wait) || Saved.Wait < 0)
        return false;

    if (Saved.State == ERailwayState::Dwelling)
        return Cursor > 0 && Stops[Cursor - 1].Distance == Prepared.StartDistance;
    if (Saved.State == ERailwayState::Finished)
    {
        const double Terminus = Prepared.Plan.bFinishAtLastStop ? Stops.Last().Distance : Prepared.SectionEnds.Last();
        return Prepared.StartDistance == Terminus && Cursor == Stops.Num();
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
    if (const ARailwayDataActor* Data = GetRailwayData(); Data && Data->Network.NetworkId.IsValid() && !ActivePlan.Sections.IsEmpty() &&
        !Railway::Persistence::Capture(*Data, ActivePlan, SectionEnds, Distance, Saved.Bookmark))
        UE_LOG(LogRailway, Warning, TEXT("%s: could only save the current build's indexed plan"), *GetName());
    Saved.Distance = Distance;
    Saved.State = State;
    Saved.NextStop = NextStop;
    if (State == ERailwayState::Running) Saved.Speed = Speed;
    if (State == ERailwayState::Dwelling)
    {
        Saved.Wait = Wait;
        Saved.ElapsedDwell = ElapsedDwell;
        Saved.bPreLeaveSent = bPreLeaveSent;
    }
    return Saved;
}

void ARailwayTrain::LoadSaveGameData(FRailwayTrainSaveGame InSaveGame)
{
    FString Error;
    if (!TryLoadSaveGameData(InSaveGame, Error))
        UE_LOG(LogRailway, Warning, TEXT("%s: could not restore train: %s"), *GetName(), *Error);
}

bool ARailwayTrain::TryLoadSaveGameData(const FRailwayTrainSaveGame& Save, FString& OutError)
{
    OutError.Reset();
    LastRestoreResult = ERailwayRestoreResult::Failed;
    if (!HasAuthority() || !Save.bValid)
    {
        OutError = TEXT("A valid save and an authoritative train are required");
        return false;
    }
    if (RestoreSavedService(Save, OutError)) return true;
    const FString RestoreError = OutError;
    FString ParkError;
    if (!ParkAtRecoveryPoint(Save.DriveMode, ParkError))
    {
        OutError = RestoreError + TEXT(". ") + ParkError;
        return false;
    }
    UE_LOG(LogRailway, Warning, TEXT("%s: parked at recovery point: %s"), *GetName(), *RestoreError);
    OutError.Reset();
    return true;
}

bool ARailwayTrain::ParkAtRecoveryPoint(ERailwayDriveMode Mode, FString& Error)
{
    const ARailwayDataActor* Data = GetRailwayData();
    if (!Data || Data->Network.Edges.IsEmpty())
    {
        Error = TEXT("No baked railway point is available for recovery");
        return false;
    }
    FRailwayTrackLocation Location;
    int32 StationIndex;
    if (!Data->Network.ResolvePoint(RecoveryPoint, Location, StationIndex))
        Location = {Data->Network.Edges[0].TrackIndex, Data->Network.Edges[0].StartDistance};

    FRailwayPlan Parked;
    Parked.BuildId = Data->Network.BuildId;
    if (!Railway::Routing::AppendTrackPass(Data->Network, Location.TrackIndex, false, Location.TrackDistance,
        Location.TrackDistance, false, Parked, Error)) return false;
    FRailwayPreparedPlan Prepared;
    if (!Railway::Routing::Prepare(Data, MoveTemp(Parked), 0, Prepared, Error)) return false;
    ResetMotion();
    DriveMode = Mode == ERailwayDriveMode::Manual ? Mode : ERailwayDriveMode::Automatic;
    LastRestoreResult = ERailwayRestoreResult::ParkedAtFallback;
    ActivatePlan(MoveTemp(Prepared), 0, ERailwayState::Idle);
    return true;
}

bool ARailwayTrain::RestoreSavedService(const FRailwayTrainSaveGame& Save, FString& OutError)
{
    if (!StaticEnum<ERailwayState>()->IsValidEnumValue(static_cast<int64>(Save.State)) ||
        (Save.DriveMode != ERailwayDriveMode::Automatic && Save.DriveMode != ERailwayDriveMode::Manual))
    {
        OutError = TEXT("Unrecognized saved train state or drive mode");
        return false;
    }
    if (Save.RailwayPlan.Sections.IsEmpty() && Save.RailwayPlan.Stops.IsEmpty() &&
        (Save.State == ERailwayState::Idle || Save.State == ERailwayState::InvalidRoute))
    {
        DriveMode = Save.DriveMode;
        LastRestoreResult = ERailwayRestoreResult::Restored;
        ResetPlan(Save.State);
        return true;
    }

    FRailwayPreparedPlan Prepared;
    const ARailwayDataActor* Data = GetRailwayData();
    if (!Data)
    {
        OutError = TEXT("The train's railway data is unavailable");
        return false;
    }
    const bool bRemap = Data->Network.BuildId != Save.RailwayPlan.BuildId;
    if (bRemap)
    {
        FRailwayPlan Plan;
        double RemappedDistance = 0;
        if (!Railway::Persistence::Remap(*Data, Save, Plan, RemappedDistance, OutError) ||
            !Railway::Routing::Prepare(Data, MoveTemp(Plan), RemappedDistance, Prepared, OutError)) return false;
    }
    else if (!Railway::Routing::Prepare(Data, Save.RailwayPlan, Save.Distance, Prepared, OutError)) return false;
    if (!IsValidProgress(Save, Prepared))
    {
        OutError = TEXT("Invalid train progress in save");
        return false;
    }

    double Elapsed = 0;
    double Remaining = 0;
    if (Save.State == ERailwayState::Dwelling)
    {
        const int32 Occupied = Save.NextStop - 1;
        Elapsed = Save.ElapsedDwell;
        FRailwayStation Station;
        if (!FMath::IsFinite(Elapsed) || Elapsed < 0 ||
            !ResolvePlannedStop(Prepared.Plan.Stops[Occupied], Station))
        {
            OutError = TEXT("Invalid occupied station or elapsed dwell time");
            return false;
        }
        Remaining = FMath::Max(0.0, Station.DwellSeconds - Elapsed);
    }

    ResetMotion();
    DriveMode = Save.DriveMode;
    if (Save.State == ERailwayState::Running) Speed = Save.Speed;
    if (Save.State == ERailwayState::Dwelling)
    {
        ElapsedDwell = Elapsed;
        Wait = Remaining;
        bPreLeaveSent = Save.bPreLeaveSent;
    }
    LastRestoreResult = bRemap ? ERailwayRestoreResult::Remapped : ERailwayRestoreResult::Restored;
    ActivatePlan(MoveTemp(Prepared), Save.NextStop, Save.State);
    return true;
}
