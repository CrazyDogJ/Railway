#include "RailwayRouteSettings.h"
#include "RailwayTrain.h"

#include "RailwayRoutePlanner.h"

namespace Railway::Routing
{
// Resolve a native-distance interval against the current split points. Only points at the two
// ends may be partial. A zero-length section preserves the selected track at a junction.
bool AppendTrackPass(const FRailwayNetworkData& Data, int32 TrackIndex, bool bReverse, double Entry,
    double Exit, bool bPreferIncoming, FRailwayPlan& Plan, FString& Error)
{
    const FRailwayBakedTrack& Track = Data.Tracks[TrackIndex];
    if (!FMath::IsFinite(Entry) || !FMath::IsFinite(Exit) || FMath::Min(Entry, Exit) < 0 ||
        FMath::Max(Entry, Exit) > Track.Length() || (bReverse ? Exit > Entry : Entry > Exit))
    {
        Error = FString::Printf(TEXT("Invalid interval %.2f to %.2f cm on %s"), Entry, Exit, *Track.Name.ToString());
        return false;
    }
    const int32 First = Track.FirstEdge;
    const int32 End = First + Track.NumEdges;
    const int32 Before = Plan.Sections.Num();
    int32 AtPoint = INDEX_NONE;
    for (int32 I = bReverse ? End - 1 : First; I >= First && I < End; I += bReverse ? -2 : 2)
    {
        const FRailwayEdge& E = Data.Edges[I];
        const double Low = FMath::Max(FMath::Min(Entry, Exit), FMath::Min(E.StartDistance, E.EndDistance));
        const double High = FMath::Min(FMath::Max(Entry, Exit), FMath::Max(E.StartDistance, E.EndDistance));
        if (Low < High)
            Plan.Sections.Add({{I}, bReverse ? High : Low, High - Low});
        else if (Entry == Exit && E.Contains(Entry) &&
            (Plan.Sections.IsEmpty() || Data.CanFollow(Plan.Sections.Last().Leg.EdgeIndex, I)))
        {
            if (AtPoint == INDEX_NONE || Entry == (bPreferIncoming ? E.EndDistance : E.StartDistance)) AtPoint = I;
        }
    }
    if (AtPoint != INDEX_NONE) Plan.Sections.Add({{AtPoint}, Entry, 0});
    if (Plan.Sections.Num() > Before) return true;
    Error = FString::Printf(TEXT("No directed interval on %s at %.2f cm"), *Track.Name.ToString(), Entry);
    return false;
}
}

namespace
{
struct FPass
{
    int32 Track;
    bool bReverse;
    double Entry;
    double Exit;
};

bool Connect(const FRailwayNetworkData& Data, FPass& A, FPass& B, FString& Error)
{
    const FRailwayBakedTrack& Track = Data.Tracks[A.Track];
    // Consecutive passes on a closed track mean another lap.
    if (A.Track == B.Track && A.bReverse == B.bReverse)
    {
        if (A.Exit == -1) A.Exit = A.bReverse ? 0.0 : Track.Length();
        if (B.Entry == -1) B.Entry = B.bReverse ? Track.Length() : 0.0;
        return true;
    }

    int32 Matches = 0;
    double Exit = 0, Entry = 0;
    for (int32 I = Track.FirstEdge; I < Track.FirstEdge + Track.NumEdges; ++I)
    {
        const FRailwayEdge& E = Data.Edges[I];
        if (E.IsReverse() != A.bReverse || (A.Exit != -1 && A.Exit != E.EndDistance)) continue;
        if (A.Entry >= 0 && (E.EndDistance - A.Entry) * E.Sign() < 0) continue;
        for (int32 J = 0; J < E.NumSuccessors; ++J)
        {
            const FRailwayEdge& Next = Data.Edges[Data.Successors[E.FirstSuccessor + J]];
            if (Next.TrackIndex != B.Track || Next.IsReverse() != B.bReverse ||
                (B.Entry != -1 && B.Entry != Next.StartDistance)) continue;
            if (B.Exit >= 0 && (B.Exit - Next.StartDistance) * Next.Sign() < 0) continue;
            ++Matches;
            Exit = E.EndDistance;
            Entry = Next.StartDistance;
        }
    }
    if (Matches != 1)
    {
        Error = FString::Printf(TEXT("%s to %s: %s"), *Track.Name.ToString(), *Data.Tracks[B.Track].Name.ToString(),
            Matches ? TEXT("multiple junctions; set an explicit entry or exit distance") : TEXT("no permitted junction in these directions"));
        return false;
    }
    A.Exit = Exit;
    B.Entry = Entry;
    return true;
}
}

bool ARailwayTrain::CompileRouteSettings(const FRailwayRouteSettings& Settings, FRailwayPlan& OutPlan, FString& Error) const
{
    OutPlan = {};
    Error.Reset();
    const ARailwayDataActor* Actor = GetRailwayData();
    if (!Actor || !Actor->Network.NetworkId.IsValid())
    {
        Error = TEXT("Build railway data for this level before translating route settings");
        return false;
    }
    const FRailwayNetworkData& Data = Actor->Network;
    if (Settings.Mode == ERailwayRouteMode::BetweenPoints)
    {
        FRailwayTrackLocation Start, Goal;
        int32 StartStation, GoalStation;
        if (!Data.ResolvePoint(Settings.Start, Start, StartStation) || !Data.ResolvePoint(Settings.Destination, Goal, GoalStation))
        {
            Error = TEXT("Select valid start and destination points from this railway network");
            return false;
        }
        return GoalStation != INDEX_NONE
            ? Actor->FindRoutePlanToStation(Start, GoalStation, OutPlan, Error, Settings.bStopAtIntermediateStations)
            : Actor->FindRoutePlan(Start, Goal, OutPlan, Error, Settings.bStopAtIntermediateStations);
    }
    if (Settings.Tracks.IsEmpty())
    {
        Error = TEXT("Add at least one directed track pass");
        return false;
    }
    TArray<FPass> Passes;
    for (const FRailwayTrackPass& Row : Settings.Tracks)
    {
        const int32* Index = Data.TrackById.Find(Row.Track.Id);
        if (!Index)
        {
            Error = FString::Printf(TEXT("Select a baked track for pass %d"), Passes.Num() + 1);
            return false;
        }
        Passes.Add({*Index, Row.bReverse, Row.EntryDistance, Row.ExitDistance});
    }
    for (int32 I = 1; I < Passes.Num(); ++I)
        if (!Connect(Data, Passes[I - 1], Passes[I], Error)) return false;

    FRailwayPlan Plan;
    Plan.BuildId = Data.BuildId;
    for (const FPass& Pass : Passes)
    {
        const double Length = Data.Tracks[Pass.Track].Length();
        const double Entry = Pass.Entry != -1 ? Pass.Entry : (Pass.bReverse ? Length : 0.0);
        const double Exit = Pass.Exit != -1 ? Pass.Exit : (Pass.bReverse ? 0.0 : Length);
        if (!Railway::Routing::AppendTrackPass(Data, Pass.Track, Pass.bReverse, Entry, Exit, false, Plan, Error)) return false;
    }
    Railway::Routing::BuildStops(Data, Plan, Settings.bStopAtIntermediateStations, INDEX_NONE);
    FRailwayPreparedPlan Prepared;
    if (!Railway::Routing::Prepare(Actor, MoveTemp(Plan), 0, Prepared, Error)) return false;
    OutPlan = MoveTemp(Prepared.Plan);
    return true;
}
