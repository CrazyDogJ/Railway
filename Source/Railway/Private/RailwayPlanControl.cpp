#include "RailwayPlanControl.h"

#include "Algo/BinarySearch.h"
#include "GameFramework/PlayerController.h"
#include "RailwayDataActor.h"
#include "RailwayRouteGeometry.h"
#include "RailwayRoutePlanner.h"
#include "RailwayTrain.h"

URailwayPlanControlComponent::URailwayPlanControlComponent()
{
    SetIsReplicatedByDefault(true);
}

void URailwayPlanControlComponent::ServerSubmit_Implementation(FGuid Request, ARailwayTrain* Train, const FRailwayPlanCommand& Command)
{
    FString Message;
    APlayerController* Player = Cast<APlayerController>(GetOwner());
    const bool bSuccess = IsValid(Player) && IsValid(Train) && Train->GetWorld() == GetWorld() &&
        Train->ApplyPlayerPlanCommand(Player, Command, Message);
    if (!bSuccess && Message.IsEmpty()) Message = TEXT("列车或玩家不可用");
    ClientReply(Request, bSuccess, Message);
}

void URailwayPlanControlComponent::ClientReply_Implementation(FGuid Request, bool bSuccess, const FString& Message)
{
    OnReply.Broadcast(Request, bSuccess, Message);
}

bool ARailwayTrain::CanPlayerEditPlan_Implementation(APlayerController* Player) const
{
    return IsValid(Player) && Player->GetWorld() == GetWorld();
}

double ARailwayTrain::GetServiceEndDistance() const
{
    return ActivePlan.bFinishAtLastStop && !ActivePlan.Stops.IsEmpty() ? ActivePlan.Stops.Last().Distance : GetRouteLength();
}

void ARailwayTrain::GetPossiblePlanStops(TArray<FRailwayPlannedStop>& OutStops) const
{
    OutStops.Reset();
    const ARailwayDataActor* Data = GetRailwayData();
    if (!Data || Data->Network.BuildId != ActivePlan.BuildId || ActivePlan.Sections.IsEmpty()) return;
    FRailwayPlan Along;
    Along.Sections = ActivePlan.Sections;
    Railway::Routing::BuildStops(Data->Network, Along, true, INDEX_NONE);
    OutStops = MoveTemp(Along.Stops);
}

bool ARailwayTrain::CancelService(FString& Error, int32 SwitchApproach, int32 SwitchExit)
{
    if (State == ERailwayState::Idle || ActivePlan.Sections.IsEmpty())
    {
        Error = TEXT("没有正在执行的路线");
        return false;
    }
    int32 CabEdge;
    double Local;
    if (!GetCabEdge(CabEdge, Local)) { Error = TEXT("无法定位列车"); return false; }
    TArray<FRailwayTrackChoice> Ahead;
    GetUpcomingTrackChoices(Speed < 0 || (Speed == 0 && ActivePlan.bCabReversed), Ahead);
    if (!SetFreeEdge(CabEdge, Local)) { Error = TEXT("无法进入自由驾驶"); return false; }
    const auto& N = GetRailwayData()->Network;
    if (!Ahead.IsEmpty()) RememberExit(N, Ahead[0].ApproachEdge, Ahead[0].SelectedEdge);
    if (SwitchApproach != INDEX_NONE)
    {
        RememberExit(N, SwitchApproach, SwitchExit);
        RememberExit(N, SwitchExit ^ 1, SwitchApproach ^ 1);
    }
    Wait = ElapsedDwell = 0;
    bPreLeaveSent = false;
    CurrentStation = NAME_None;
    SetState(ERailwayState::Idle);
    PublishMoveState(true);
    return true;
}

bool ARailwayTrain::EditScheduledVisit(const FRailwayPlanCommand& Command, FString& Error)
{
    if (!IsInService())
    {
        Error = TEXT("列车当前没有运行中的班次");
        return false;
    }
    if (Command.Action == ERailwayPlanAction::DirectToStop) return GoDirectToStop(Command.StopIndex, Error);
    FRailwayPlan Edited = ActivePlan;
    if (Command.Action == ERailwayPlanAction::SkipStop)
    {
        if (Command.StopIndex < NextStop || !Edited.Stops.IsValidIndex(Command.StopIndex))
        {
            Error = TEXT("只能取消尚未到达的站次");
            return false;
        }
        if (Command.StopIndex == Edited.Stops.Num() - 1) Edited.bFinishAtLastStop = false;
        Edited.Stops.RemoveAt(Command.StopIndex);
    }
    else
    {
        const int32* Station = GetRailwayData()->Network.StationById.Find(Command.SourceId);
        if (!Station || !FMath::IsFinite(Command.Distance) || Command.Distance < Distance || Command.Distance > GetServiceEndDistance())
        {
            Error = TEXT("所选站次不在剩余行程内");
            return false;
        }
        auto Matches = [&](const FRailwayPlannedStop& S) { return S.StationIndex == *Station && S.Distance == Command.Distance; };
        TArray<FRailwayPlannedStop> Possible;
        GetPossiblePlanStops(Possible);
        const FRailwayPlannedStop* Visit = Possible.FindByPredicate(Matches);
        if (!Visit || Edited.Stops.ContainsByPredicate(Matches) || (NextStop && Visit->Distance < Edited.Stops[NextStop - 1].Distance))
        {
            Error = TEXT("该站次已排入时刻表或无法加入");
            return false;
        }
        const int32 Insert = FMath::Max(NextStop, Algo::LowerBoundBy(Edited.Stops, Visit->Distance, &FRailwayPlannedStop::Distance));
        Edited.Stops.Insert(*Visit, Insert);
    }
    InstallPlan(MoveTemp(Edited));
    PublishMoveState(true);
    return true;
}

bool ARailwayTrain::ApplyPlayerPlanCommand(APlayerController* Player, const FRailwayPlanCommand& Command, FString& Error)
{
    Error.Reset();
    if (!HasAuthority() || !IsValid(Player) || Player->GetWorld() != GetWorld() || !CanPlayerEditPlan(Player))
    {
        Error = TEXT("无权调度此列车");
        return false;
    }
    const ARailwayDataActor* Data = GetRailwayData();
    if (!Data || Command.ExpectedRevision != GetPlanRevision() || Data->Network.BuildId != Command.BuildId ||
        (!ActivePlan.Sections.IsEmpty() && ActivePlan.BuildId != Data->Network.BuildId))
    {
        Error = TEXT("铁路数据或运行计划已更新，请刷新后重试");
        return false;
    }
    switch (Command.Action)
    {
    case ERailwayPlanAction::ActivatePreset:
    {
        FRailwayPlan Plan;
        return CompileRouteSettings(Route, Plan, Error) && RestartPreparedRouteAtDistance(Plan, 0, Error);
    }
    case ERailwayPlanAction::StartBetweenPoints:
    {
        if (IsInService() || Speed != 0)
        {
            Error = TEXT("请先停用计划并停稳，再从指定起点发车");
            return false;
        }
        FRailwayRouteSettings Settings;
        Settings.Start = Command.Departure;
        Settings.Destination = {{Command.SourceId}, Command.Distance};
        Settings.bStopAtIntermediateStations = Command.bStopAlongWay;
        FRailwayPlan Plan;
        return CompileRouteSettings(Settings, Plan, Error) && RestartPreparedRouteAtDistance(Plan, 0, Error);
    }
    case ERailwayPlanAction::ActivateTrack:
    case ERailwayPlanAction::DeactivateTrack: return ChangeTurnout(Command, Error);
    case ERailwayPlanAction::CancelRoute: return CancelService(Error);
    case ERailwayPlanAction::SkipStop:
    case ERailwayPlanAction::DirectToStop:
    case ERailwayPlanAction::AddStop: return EditScheduledVisit(Command, Error);
    case ERailwayPlanAction::SetDestination:
    {
        FRailwayTrackLocation Start, Goal;
        int32 Station;
        if (!GetCurrentTrackLocation(Start) || !Data->Network.ResolvePoint({{Command.SourceId}, Command.Distance}, Goal, Station))
        {
            Error = TEXT("无法解析列车位置或目的地");
            return false;
        }
        FRailwayPlan Plan;
        const bool bFound = Station == INDEX_NONE
            ? Data->FindRoutePlan(Start, Goal, Plan, Error, Command.bStopAlongWay)
            : Data->FindRoutePlanToStation(Start, Station, Plan, Error, Command.bStopAlongWay);
        return bFound && ReplacePreparedRoute(Plan, Error, 0);
    }
    }
    Error = TEXT("未知调度操作");
    return false;
}
