#include "RailwayPlanRequest.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "RailwayTrain.h"

URailwayPlanRequest* URailwayPlanRequest::Make(APlayerController* Player, const FRailwayPlanViewKey& View, ERailwayPlanAction Action)
{
    auto* Request = NewObject<URailwayPlanRequest>();
    Request->Controller = Player;
    Request->Target = View.Train.Get();
    Request->Command.Action = Action;
    Request->Command.BuildId = View.BuildId;
    Request->Command.ExpectedRevision = View.Revision;
    if (IsValid(Player)) Request->RegisterWithGameInstance(Player);
    return Request;
}

URailwayPlanRequest* URailwayPlanRequest::MakeVisit(APlayerController* Player, const FRailwayVisitView& Visit, ERailwayPlanAction Action)
{
    auto* Request = Make(Player, Visit.View, Action);
    Request->Command.StopIndex = Visit.StopIndex;
    Request->Command.SourceId = Visit.StationId;
    Request->Command.Distance = Visit.RouteDistance;
    if (!Visit.bCanEdit || ((Action == ERailwayPlanAction::AddStop) != (Visit.Status == ERailwayVisitStatus::Available)))
        Request->LocalError = TEXT("该站次不支持此操作");
    return Request;
}

URailwayPlanRequest* URailwayPlanRequest::ActivateRailwayPreset(APlayerController* Player, const FRailwayPlanViewKey& View)
{
    return Make(Player, View, ERailwayPlanAction::ActivatePreset);
}

URailwayPlanRequest* URailwayPlanRequest::CancelRailwayRoute(APlayerController* Player, const FRailwayPlanViewKey& View)
{
    return Make(Player, View, ERailwayPlanAction::CancelRoute);
}

URailwayPlanRequest* URailwayPlanRequest::SkipRailwayVisit(APlayerController* Player, const FRailwayVisitView& Visit)
{
    return MakeVisit(Player, Visit, ERailwayPlanAction::SkipStop);
}

URailwayPlanRequest* URailwayPlanRequest::AddRailwayVisit(APlayerController* Player, const FRailwayVisitView& Visit)
{
    return MakeVisit(Player, Visit, ERailwayPlanAction::AddStop);
}

URailwayPlanRequest* URailwayPlanRequest::GoDirectToRailwayVisit(APlayerController* Player, const FRailwayVisitView& Visit)
{
    return MakeVisit(Player, Visit, ERailwayPlanAction::DirectToStop);
}

URailwayPlanRequest* URailwayPlanRequest::SetRailwayDestination(APlayerController* Player, const FRailwayPlanViewKey& View, const FRailwayRoutePoint& Destination, bool bStopAlongWay)
{
    auto* Request = Make(Player, View, ERailwayPlanAction::SetDestination);
    Request->Command.SourceId = Destination.Source.Id;
    Request->Command.Distance = Destination.TrackDistance;
    Request->Command.bStopAlongWay = bStopAlongWay;
    return Request;
}

URailwayPlanRequest* URailwayPlanRequest::StartRailwayBetweenPoints(APlayerController* Player, const FRailwayPlanViewKey& View, const FRailwayRoutePoint& Departure, const FRailwayRoutePoint& Destination, bool bStopAlongWay)
{
    auto* Request = Make(Player, View, ERailwayPlanAction::StartBetweenPoints);
    Request->Command.SourceId = Destination.Source.Id;
    Request->Command.Distance = Destination.TrackDistance;
    Request->Command.bStopAlongWay = bStopAlongWay;
    Request->Command.Departure = Departure;
    return Request;
}

void URailwayPlanRequest::Activate()
{
    if (bStarted) return;
    bStarted = true;
    FString Error;
    APlayerController* Player = Controller.Get();
    ARailwayTrain* Train = Target.Get();
    const FRailwayPlanViewKey View{Train, Command.BuildId, Command.ExpectedRevision};
    if (!LocalError.IsEmpty() || !URailwayPlanViewLibrary::CanSendRailwayPlanCommand(Player, View, Error))
    {
        Finish(false, LocalError.IsEmpty() ? Error : LocalError);
        return;
    }
    if (Train->HasAuthority())
    {
        const bool bSuccess = Train->ApplyPlayerPlanCommand(Player, Command, Error);
        Finish(bSuccess, Error);
        return;
    }
    Channel = Player->FindComponentByClass<URailwayPlanControlComponent>();
    RequestId = FGuid::NewGuid();
    ReplyHandle = Channel->OnReply.AddUObject(this, &URailwayPlanRequest::Reply);
    Deadline = FPlatformTime::Seconds() + 10.0;
    TimeoutHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &URailwayPlanRequest::CheckConnection), .25f);
    Channel->ServerSubmit(RequestId, Train, Command);
}

void URailwayPlanRequest::Reply(FGuid Id, bool bSuccess, const FString& Message)
{
    if (Id == RequestId) Finish(bSuccess, Message);
}

bool URailwayPlanRequest::CheckConnection(float)
{
    if (Channel.IsValid() && Controller.IsValid() && Target.IsValid() &&
        Controller->GetWorld() && !Controller->GetWorld()->bIsTearingDown && FPlatformTime::Seconds() < Deadline) return true;
    TimeoutHandle.Reset(); // Returning false removes this ticker.
    Finish(false, TEXT("连接已关闭或回执超时，操作结果未知，请重新读取列车计划"));
    return false;
}

void URailwayPlanRequest::Unbind()
{
    if (Channel.IsValid()) Channel->OnReply.Remove(ReplyHandle);
    if (TimeoutHandle.IsValid())
    {
        FTSTicker::RemoveTicker(TimeoutHandle);
        TimeoutHandle.Reset();
    }
}

void URailwayPlanRequest::Finish(bool bSuccess, const FString& Message)
{
    if (bFinished) return;
    bFinished = true;
    Unbind();
    if (bSuccess) Accepted.Broadcast(Message);
    else Failed.Broadcast(Message);
    SetReadyToDestroy();
}

void URailwayPlanRequest::BeginDestroy()
{
    Unbind();
    Super::BeginDestroy();
}
