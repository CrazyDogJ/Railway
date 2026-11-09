#include "RailwayWorldIndex.h"

#include "EngineUtils.h"
#include "RailwayTrack.h"
#include "Serialization/Archive.h"

void FRailwayPlannedStop::PostSerialize(const FArchive& Ar)
{
    if (Ar.IsLoading() && Ar.IsPersistent() && !Ar.IsTransacting() && Reference.Source == ERailwayStationSource::Unspecified)
        Reference.Source = ERailwayStationSource::LegacySnapshot;
}

FRailwayWorldIndex::FRailwayWorldIndex(UWorld* InWorld) : World(InWorld)
{
    if (!World) return;
    for (TActorIterator<ARailwayNetworkActor> It(World); It; ++It)
    {
        ARailwayNetworkActor* Actor = *It;
        if (!IsValid(Actor)) continue;
        if (Actor->RailwayId.IsValid())
        {
            if (Actors.Contains(Actor->RailwayId)) Actors[Actor->RailwayId] = nullptr;
            else Actors.Add(Actor->RailwayId, Actor);
        }
        if (auto* Marker = Cast<ARailwayStationMarker>(Actor))
            if (IsValid(Marker->HostTrack)) Markers.Add(Marker->HostTrack, Marker);
    }
}

ARailwayNetworkActor* FRailwayWorldIndex::Find(const FGuid& Id) const
{
    return Actors.FindRef(Id);
}

namespace Railway::Stations
{
bool SameSource(const FRailwayStationRef& A, const FRailwayStationRef& B)
{
    return (A.Source == ERailwayStationSource::Marker || A.Source == ERailwayStationSource::TrackStation) && A.Source == B.Source &&
        A.StationId == B.StationId && A.HostTrackId == B.HostTrackId;
}

bool Read(const UWorld* World, const FRailwayPlannedStop& Stop, FRailwayStation& OutStation)
{
    const FRailwayStationRef& Ref = Stop.Reference;
    if (Ref.Source == ERailwayStationSource::Inline || Ref.Source == ERailwayStationSource::Unspecified)
    {
        if (Ref.StationId.IsValid() || Ref.HostTrackId.IsValid()) return false;
        OutStation = Stop.Station;
    }
    else
    {
        const ARailwayNetworkActor* Actor = Ref.SourceActor;
        if (!IsValid(Actor) || Actor->GetWorld() != World || !Ref.HostTrackId.IsValid() || !Ref.StationId.IsValid())
            return false;
        if (Ref.Source == ERailwayStationSource::Marker)
        {
            const auto* Marker = Cast<ARailwayStationMarker>(Actor);
            if (!Marker || Marker->RailwayId != Ref.StationId || !IsValid(Marker->HostTrack) ||
                Marker->HostTrack->RailwayId != Ref.HostTrackId) return false;
            OutStation = Marker->GetStationDefinition();
            OutStation.StationId = Marker->RailwayId;
        }
        else if (Ref.Source == ERailwayStationSource::TrackStation)
        {
            const auto* Track = Cast<ARailwayTrack>(Actor);
            if (!Track || Track->RailwayId != Ref.HostTrackId) return false;
            const FRailwayStation* Found = nullptr;
            for (const FRailwayStation& Row : Track->Stations)
            {
                if (Row.StationId != Ref.StationId) continue;
                if (Found) return false;
                Found = &Row;
            }
            if (!Found) return false;
            OutStation = *Found;
        }
        else return false;
        if (OutStation.Position != Ref.PlannedPosition) return false;
    }
    return FMath::IsFinite(OutStation.DwellSeconds) && OutStation.DwellSeconds >= 0;
}
}

bool FRailwayWorldIndex::BindStop(FRailwayPlannedStop& Stop, FString& Error) const
{
    FRailwayStationRef& Ref = Stop.Reference;
    if (Ref.Source == ERailwayStationSource::Unspecified) Ref.Source = ERailwayStationSource::Inline;
    if (Ref.Source != ERailwayStationSource::Inline)
    {
        Ref.SourceActor = Find(Ref.Source == ERailwayStationSource::Marker ? Ref.StationId : Ref.HostTrackId);
        if (!Cast<ARailwayTrack>(Find(Ref.HostTrackId)))
        {
            Error = FString::Printf(TEXT("Missing, unloaded or ambiguous host track %s"), *Ref.HostTrackId.ToString());
            return false;
        }
    }
    FRailwayStation Current;
    if (!Railway::Stations::Read(World, Stop, Current))
    {
        Error = FString::Printf(TEXT("Station %s is missing, ambiguous, moved or invalid"), *Ref.StationId.ToString());
        return false;
    }
    if (Ref.Source != ERailwayStationSource::Inline) Stop.Station = FRailwayStation();
    return true;
}

bool FRailwayWorldIndex::VisitStations(ARailwayTrack& Track,
    TFunctionRef<bool(const FRailwayStation&, const FRailwayStationRef&)> Visitor, FString& Error) const
{
    Track.InitializeStationIds();
    FRailwayStationRef Ref;
    Ref.HostTrackId = Track.RailwayId;
    Ref.Source = ERailwayStationSource::TrackStation;
    Ref.SourceActor = &Track;
    TSet<FGuid> RowIds;
    for (const FRailwayStation& Station : Track.Stations)
    {
        if (!Station.StationId.IsValid() || RowIds.Contains(Station.StationId))
        {
            Error = FString::Printf(TEXT("Missing or duplicate station ID on %s"), *Track.GetName());
            return false;
        }
        RowIds.Add(Station.StationId);
        Ref.StationId = Station.StationId;
        Ref.PlannedPosition = Station.Position;
        if (!Visitor(Station, Ref)) return false;
    }
    Ref.Source = ERailwayStationSource::Marker;
    for (auto It = Markers.CreateConstKeyIterator(&Track); It; ++It)
    {
        ARailwayStationMarker* Marker = It.Value();
        if (Find(Marker->RailwayId) != Marker)
        {
            Error = FString::Printf(TEXT("Missing or duplicate station ID on %s"), *Marker->GetName());
            return false;
        }
        const FRailwayStation Station = Marker->GetStationDefinition();
        Ref.SourceActor = Marker;
        Ref.StationId = Marker->RailwayId;
        Ref.PlannedPosition = Station.Position;
        if (!Visitor(Station, Ref)) return false;
    }
    return true;
}
