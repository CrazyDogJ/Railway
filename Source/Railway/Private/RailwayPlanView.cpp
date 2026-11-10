#include "RailwayPlanView.h"

#include "GameFramework/PlayerController.h"
#include "RailwayDataActor.h"
#include "RailwayTrain.h"

namespace
{
ERailwayPlanReadiness ReadPlan(ARailwayTrain* Train, FRailwayPlanViewKey& Key, const FRailwayNetworkData*& Network)
{
    Key = {};
    Network = nullptr;
    if (!IsValid(Train)) return ERailwayPlanReadiness::NoTrain;
    const auto* Data = Train->GetRailwayData();
    if (!Data || !Data->Network.BuildId.IsValid()) return ERailwayPlanReadiness::NoBakedData;
    if (!Train->IsPlanPresentationCurrent() ||
        (!Train->ActivePlan.Sections.IsEmpty() && Train->ActivePlan.BuildId != Data->Network.BuildId))
        return ERailwayPlanReadiness::Synchronizing;
    Network = &Data->Network;
    Key = {Train, Network->BuildId, Train->GetPlanRevision()};
    return ERailwayPlanReadiness::Ready;
}

FRailwaySourceOption TrackOption(const FRailwayBakedTrack& Track)
{
    return {{{Track.Id}, 0}, Track.Name, false, Track.Name, Track.Length()};
}

FRailwaySourceOption StationOption(const FRailwayNetworkData& Data, const FRailwayBakedStation& Station)
{
    const auto& Track = Data.Tracks[Data.Edges[Station.EdgeIndex].TrackIndex];
    return {{{Station.Id}, Station.Definition.TrackDistance}, Station.Definition.Name, true, Track.Name, Track.Length()};
}
}

FRailwayPlanOverview URailwayPlanViewLibrary::GetRailwayPlanOverview(ARailwayTrain* Train)
{
    FRailwayPlanOverview Result;
    const FRailwayNetworkData* N;
    Result.Readiness = ReadPlan(Train, Result.View, N);
    if (!N) return Result;
    const auto& Plan = Train->ActivePlan;
    Result.State = Train->State;
    Result.DriveMode = Train->DriveMode;
    Result.SpeedKmh = Train->Speed * .036;
    Result.bHasService = Train->State != ERailwayState::Idle && !Plan.Sections.IsEmpty();
    Result.bCabFacesAgainstPlan = Result.bHasService && Plan.bCabReversed;
    Result.bCoastingWithoutPlan = !Result.bHasService && Train->Speed != 0 && (Train->DriveMode != ERailwayDriveMode::Manual || (Train->DriveInput == 0 && Train->BrakeInput == 0));
    Result.Distance = Result.bHasService ? Train->Distance : 0;
    Result.EndDistance = Result.bHasService ? Train->GetServiceEndDistance() : 0;
    Result.RemainingDistance = FMath::Max(0.0, Result.EndDistance - Result.Distance);
    Result.Progress = Result.EndDistance > 0 ? FMath::Clamp(Result.Distance / Result.EndDistance, 0., 1.) : 0.f;
    Result.CurrentStation = Train->CurrentStation;
    Result.RemainingDwellSeconds = Train->Wait;
    FRailwayTrackLocation At;
    if (Train->GetCurrentTrackLocation(At) && N->Tracks.IsValidIndex(At.TrackIndex))
    {
        Result.bLocated = true;
        Result.CurrentTrack = N->Tracks[At.TrackIndex].Name;
        Result.CurrentTrackId = N->Tracks[At.TrackIndex].Id;
        Result.CurrentTrackDistance = At.TrackDistance;
    }
    if (!Result.bHasService) return Result;
    if (Plan.Stops.IsValidIndex(Train->NextStop))
    {
        const auto& Stop = Plan.Stops[Train->NextStop];
        if (N->Stations.IsValidIndex(Stop.StationIndex))
        {
            Result.bHasNextStop = true;
            Result.NextStation = N->Stations[Stop.StationIndex].Definition.Name;
            Result.DistanceToNextStop = Stop.Distance - Result.Distance;
        }
    }
    int32 I;
    if (Train->SampleRoute(Result.EndDistance, I, Result.DestinationTrackDistance, Result.DestinationPose))
    {
        Result.bHasDestination = true;
        const auto& Edge = N->Edges[Plan.Sections[I].Leg.EdgeIndex];
        Result.Destination = TrackOption(N->Tracks[Edge.TrackIndex]);
        Result.Destination.Point.TrackDistance = Result.DestinationTrackDistance;
        if (!Plan.Stops.IsEmpty() && FMath::IsNearlyEqual(Plan.Stops.Last().Distance, Result.EndDistance, .001) &&
            N->Stations.IsValidIndex(Plan.Stops.Last().StationIndex))
        {
            Result.Destination = StationOption(*N, N->Stations[Plan.Stops.Last().StationIndex]);
            Result.DestinationTrackDistance = Result.Destination.Point.TrackDistance;
        }
    }
    return Result;
}

void URailwayPlanViewLibrary::GetRailwayTimetable(ARailwayTrain* Train, bool bIncludeAddable,
    TArray<FRailwayVisitView>& Scheduled, TArray<FRailwayVisitView>& Addable)
{
    Scheduled.Reset();
    Addable.Reset();
    FRailwayPlanViewKey Key;
    const FRailwayNetworkData* N;
    ReadPlan(Train, Key, N);
    if (!N || Train->State == ERailwayState::Idle) return;
    const auto& Stops = Train->ActivePlan.Stops;
    const int32 Cursor = Train->NextStop;
    const bool bEditing = Train->State == ERailwayState::Running || Train->State == ERailwayState::Dwelling;
    TSet<TPair<int32, double>> ScheduledLocations;
    auto AddRow = [&](const FRailwayPlannedStop& Stop, int32 Index, ERailwayVisitStatus Status, bool bCanEdit, TArray<FRailwayVisitView>& Rows)
    {
        if (!N->Stations.IsValidIndex(Stop.StationIndex)) return;
        const auto& S = N->Stations[Stop.StationIndex];
        const auto& Track = N->Tracks[N->Edges[S.EdgeIndex].TrackIndex];
        Rows.Add({Key, Index, S.Id, Track.Name, S.Definition, Stop.Distance, Status, bCanEdit});
    };
    for (int32 I = 0; I < Stops.Num(); ++I)
    {
        ERailwayVisitStatus Status = ERailwayVisitStatus::Scheduled;
        if (Train->State == ERailwayState::Dwelling && I == Cursor - 1) Status = ERailwayVisitStatus::Dwelling;
        else if (I < Cursor) Status = Stops[I].bServed ? ERailwayVisitStatus::Served : ERailwayVisitStatus::Passed;
        else if (I == Cursor) Status = ERailwayVisitStatus::Next;
        AddRow(Stops[I], I, Status, bEditing && I >= Cursor, Scheduled);
        if (bIncludeAddable && bEditing) ScheduledLocations.Add({Stops[I].StationIndex, Stops[I].Distance});
    }
    if (!bIncludeAddable || !bEditing) return;
    TArray<FRailwayPlannedStop> Possible;
    Train->GetPossiblePlanStops(Possible);
    const double First = Stops.IsValidIndex(Cursor - 1) ? FMath::Max(Train->Distance, Stops[Cursor - 1].Distance) : Train->Distance;
    for (const auto& S : Possible)
        if (S.Distance >= First && S.Distance <= Train->GetServiceEndDistance() && !ScheduledLocations.Contains({S.StationIndex, S.Distance}))
            AddRow(S, INDEX_NONE, ERailwayVisitStatus::Available, true, Addable);
}

void URailwayPlanViewLibrary::GetRailwayRouteSections(ARailwayTrain* Train, TArray<FRailwaySectionView>& Sections)
{
    Sections.Reset();
    FRailwayPlanViewKey Key;
    const FRailwayNetworkData* N;
    ReadPlan(Train, Key, N);
    if (!N || Train->State == ERailwayState::Idle) return;
    const double End = Train->GetServiceEndDistance();
    double Begin = 0;
    const auto& Plan = Train->ActivePlan;
    for (int32 I = 0; I < Plan.Sections.Num(); ++I)
    {
        const auto& S = Plan.Sections[I];
        if (!N->Edges.IsValidIndex(S.Leg.EdgeIndex) || Begin > End) break;
        const auto& Edge = N->Edges[S.Leg.EdgeIndex];
        const auto& Track = N->Tracks[Edge.TrackIndex];
        const double Travel = FMath::Min(S.TravelDistance, End - Begin);
        Sections.Add({I, Track.Name, Track.Id, Edge.IsReverse(), S.EntryDistance, S.EntryDistance + Edge.Sign() * Travel, Begin, Begin + Travel});
        Begin += S.TravelDistance;
    }
}

void URailwayPlanViewLibrary::GetRailwayRouteSources(ARailwayTrain* Train, TArray<FRailwaySourceOption>& Sources)
{
    Sources.Reset();
    const auto* Data = IsValid(Train) ? Train->GetRailwayData() : nullptr;
    if (!Data) return;
    const auto& N = Data->Network;
    Sources.Reserve(N.Stations.Num() + N.Tracks.Num());
    for (const auto& S : N.Stations) if (S.Id.IsValid()) Sources.Add(StationOption(N, S));
    for (const auto& T : N.Tracks) if (T.Id.IsValid()) Sources.Add(TrackOption(T));
}

bool URailwayPlanViewLibrary::CanSendRailwayPlanCommand(APlayerController* Player, const FRailwayPlanViewKey& View, FString& Reason)
{
    Reason.Reset();
    const auto* T = View.Train.Get();
    if (!IsValid(Player) || !Player->IsLocalController() || !IsValid(T) || Player->GetWorld() != T->GetWorld())
        Reason = TEXT("需要本地玩家和同一世界中的有效列车");
    else if (const auto* D = T->GetRailwayData(); !D || !View.BuildId.IsValid() || D->Network.BuildId != View.BuildId ||
        T->GetPlanRevision() != View.Revision || !T->IsPlanPresentationCurrent() ||
        (!T->ActivePlan.Sections.IsEmpty() && T->ActivePlan.BuildId != View.BuildId))
        Reason = TEXT("铁路数据或计划已更新，请刷新面板");
    else if (!T->HasAuthority() && !Player->FindComponentByClass<URailwayPlanControlComponent>())
        Reason = TEXT("玩家控制器缺少 Railway Plan Control 组件");
    return Reason.IsEmpty();
}
