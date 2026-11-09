#include "RailwayTrain.h"

#include "Railway.h"
#include "RailwayRoutePlanner.h"
#include "RailwayWorldIndex.h"
#include "RailwayTrack.h"
#include "UObject/Class.h"

namespace
{
constexpr int32 RailwaySaveVersion = 2;

// NextStop 同时编码已服务站次和正在停靠的站次。相同里程的站不能仅按距离推导。
bool IsValidProgress(const FRailwayTrainSaveGame& Saved, const FRailwayPreparedPlan& Prepared)
{
    const TArray<FRailwayPlannedStop>& Stops = Prepared.Plan.Stops;
    const int32 Cursor = Saved.NextStop;
    // 倒车可退到已服务站次之前，游标仍指向下一待服务站。
    if (Cursor < 0 || Cursor > Stops.Num() ||
        (Stops.IsValidIndex(Cursor) && Stops[Cursor].Distance < Saved.Distance) ||
        !FMath::IsFinite(Saved.Speed) || !FMath::IsFinite(Saved.Wait) || Saved.Wait < 0)
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
    Saved.FormatVersion = RailwaySaveVersion;
    Saved.DriveMode = DriveMode;
    Saved.RailwayPlan = ActivePlan;
    Saved.RailwayPlan.NetworkRevision = 0;
    // 磁盘存档不能依靠 Actor 路径。世界站点的描述和 CustomData 只在源对象上持久化。
    for (FRailwaySection& Section : Saved.RailwayPlan.Sections)
        Section.Leg.Track = nullptr;
    for (FRailwayPlannedStop& Stop : Saved.RailwayPlan.Stops)
    {
        Stop.Reference.SourceActor = nullptr;
        if (Stop.Reference.Source != ERailwayStationSource::Inline) Stop.Station = FRailwayStation();
    }
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
    if (!HasAuthority() || !Save.bValid)
    {
        OutError = TEXT("A valid save and an authoritative train are required");
        return false;
    }
    if (Save.FormatVersion < 0 || Save.FormatVersion > RailwaySaveVersion)
    {
        OutError = FString::Printf(TEXT("Unsupported railway save version %d"), Save.FormatVersion);
        return false;
    }
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
        ResetPlan(Save.State);
        return true;
    }

    const bool bLegacy = Save.FormatVersion == 0;
    using Railway::Routing::EPlanOrigin;
    EPlanOrigin Origin = EPlanOrigin::IdentifiedSave;
    if (Save.FormatVersion == 1) Origin = EPlanOrigin::AlphaIdentifiedSave;
    if (bLegacy) Origin = EPlanOrigin::LegacySave;
    FRailwayPreparedPlan Prepared;
    if (!Railway::Routing::Prepare(GetWorld(), Save.RailwayPlan, Save.Distance, Prepared, OutError, Origin)) return false;
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
        if (bLegacy)
        {
            // 旧格式只保存剩余时间，先用旧站时长反推已经消耗的部分。
            Elapsed = FMath::Max(0.0, Save.RailwayPlan.Stops[Occupied].Station.DwellSeconds - Save.Wait);
        }
        else Elapsed = Save.ElapsedDwell;
        FRailwayStation Station;
        if (!FMath::IsFinite(Elapsed) || Elapsed < 0 ||
            !Railway::Stations::Read(GetWorld(), Prepared.Plan.Stops[Occupied], Station))
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
    ActivatePlan(MoveTemp(Prepared), Save.Distance, Save.NextStop, Save.State);
    return true;
}
