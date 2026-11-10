#include "RailwayRouteBookmark.h"

#include "Algo/BinarySearch.h"
#include "RailwayRouteGeometry.h"
#include "RailwayRoutePlanner.h"

namespace Railway::Persistence
{
namespace
{
FRailwaySavedAnchor AnchorAt(const FRailwayNetworkData& Data, const FRailwayEdge& Edge, double Local)
{
    FRailwaySavedAnchor Anchor;
    Anchor.Distance = Local;
    if (Local == 0) Anchor.Kind = ERailwayAnchorKind::TrackStart;
    else if (Local == Data.Tracks[Edge.TrackIndex].Length()) Anchor.Kind = ERailwayAnchorKind::TrackEnd;
    else
    {
        const int32 Node = Local == Edge.StartDistance ? Edge.FromNode : (Local == Edge.EndDistance ? Edge.ToNode : INDEX_NONE);
        if (const FGuid* Junction = Data.JunctionByNode.Find(Node))
        {
            Anchor.Kind = ERailwayAnchorKind::Junction;
            Anchor.Source = *Junction;
        }
    }
    return Anchor;
}

bool ResolveAnchor(const FRailwayNetworkData& Data, int32 TrackIndex, const FRailwaySavedAnchor& Anchor, double& Local)
{
    const FRailwayBakedTrack& Track = Data.Tracks[TrackIndex];
    switch (Anchor.Kind)
    {
    case ERailwayAnchorKind::TrackStart: Local = 0; return true;
    case ERailwayAnchorKind::TrackEnd: Local = Track.Length(); return true;
    case ERailwayAnchorKind::Distance:
        Local = Anchor.Distance;
        return FMath::IsFinite(Local) && Local >= 0 && Local <= Track.Length();
    case ERailwayAnchorKind::Station:
        if (const int32* Id = Data.StationById.Find(Anchor.Source))
        {
            const FRailwayBakedStation& Station = Data.Stations[*Id];
            if (Data.Edges[Station.EdgeIndex].TrackIndex != TrackIndex) return false;
            Local = Station.Definition.TrackDistance;
            return true;
        }
        return false;
    case ERailwayAnchorKind::Junction:
        for (int32 I = Track.FirstEdge; I < Track.FirstEdge + Track.NumEdges; I += 2)
        {
            const FRailwayEdge& E = Data.Edges[I];
            if (const FGuid* Id = Data.JunctionByNode.Find(E.FromNode); Id && *Id == Anchor.Source)
            {
                Local = E.StartDistance;
                return true;
            }
            if (const FGuid* Id = Data.JunctionByNode.Find(E.ToNode); Id && *Id == Anchor.Source)
            {
                Local = E.EndDistance;
                return true;
            }
        }
        return false;
    }
    return false;
}

struct FResolvedPass
{
    int32 Track;
    double Begin;
    double End;
    double Entry;
    double Exit;

    bool Map(double Local, double& Distance) const
    {
        if (!FMath::IsFinite(Local) || Local < FMath::Min(Entry, Exit) || Local > FMath::Max(Entry, Exit)) return false;
        Distance = Local == Exit ? End : FMath::Clamp(Begin + FMath::Abs(Local - Entry), Begin, End);
        return true;
    }
};
}

bool Capture(const ARailwayDataActor& Actor, const FRailwayPlan& Plan, const TArray<double>& Ends,
    double Distance, FRailwayRouteBookmark& Bookmark)
{
    const FRailwayNetworkData& Data = Actor.Network;
    int32 Occupied;
    double Local;
    if (!Data.NetworkId.IsValid() || !Geometry::Locate(Data, Plan, Ends, Distance, Occupied, Local)) return false;
    FRailwayRouteBookmark New;
    New.Network = Data.NetworkId;
    TArray<int32> SectionToPass;
    for (const FRailwaySection& S : Plan.Sections)
    {
        if (!Data.Edges.IsValidIndex(S.Leg.EdgeIndex)) return false;
        const FRailwayEdge& Edge = Data.Edges[S.Leg.EdgeIndex];
        const FGuid Track = Data.Tracks[Edge.TrackIndex].Id;
        if (!Track.IsValid()) return false;
        // Merge adjacent cuts on the same traversal. A seam, reversal or another lap starts a new pass.
        if (New.Passes.IsEmpty() || New.Passes.Last().Track != Track || New.Passes.Last().bReverse != Edge.IsReverse() ||
            New.Passes.Last().Exit.Distance != S.EntryDistance)
        {
            FRailwaySavedPass& Pass = New.Passes.AddDefaulted_GetRef();
            Pass.Track = Track;
            Pass.bReverse = Edge.IsReverse();
            Pass.Entry = AnchorAt(Data, Edge, S.EntryDistance);
            Pass.bStartOnIncomingEdge = S.EntryDistance == Edge.EndDistance;
        }
        const double Exit = S.TravelDistance == FMath::Abs(Edge.EndDistance - S.EntryDistance) ? Edge.EndDistance :
            S.EntryDistance + Edge.Sign() * S.TravelDistance;
        New.Passes.Last().Exit = AnchorAt(Data, Edge, Exit);
        SectionToPass.Add(New.Passes.Num() - 1);
    }
    New.OccupiedPass = SectionToPass[Occupied];

    constexpr double Tol = UE_KINDA_SMALL_NUMBER;
    for (const FRailwayPlannedStop& Stop : Plan.Stops)
    {
        if (!Data.Stations.IsValidIndex(Stop.StationIndex)) return false;
        const FRailwayBakedStation& Row = Data.Stations[Stop.StationIndex];
        const int32 Host = Data.Edges[Row.EdgeIndex].TrackIndex;
        const FRailwayBakedTrack& Track = Data.Tracks[Host];
        FRailwaySavedStop Binding;
        Binding.Station = Row.Id;
        for (int32 I = Algo::LowerBound(Ends, Stop.Distance - Tol); I < Ends.Num(); ++I)
        {
            const double Begin = I ? Ends[I - 1] : 0.0;
            if (Begin > Stop.Distance + Tol) break;
            const FRailwaySection& S = Plan.Sections[I];
            const FRailwayEdge& Edge = Data.Edges[S.Leg.EdgeIndex];
            if (Edge.TrackIndex != Host) continue;
            const double At = S.EntryDistance + Edge.Sign() * (Stop.Distance - Begin);
            const bool bClosing = Track.IsLoop() && Row.Definition.TrackDistance == 0 && FMath::Abs(At - Track.Length()) <= Tol;
            if (bClosing || FMath::Abs(At - Row.Definition.TrackDistance) <= Tol)
            {
                Binding.Pass = SectionToPass[I];
                Binding.bAtClosingSeam = bClosing;
                break;
            }
        }
        if (!Binding.Station.IsValid() || Binding.Pass == INDEX_NONE) return false;
        New.Stops.Add(Binding);
        if (Stop.Distance == 0 && Binding.Pass == 0 && New.Passes[0].Entry.Kind == ERailwayAnchorKind::Distance)
        {
            New.Passes[0].Entry.Kind = ERailwayAnchorKind::Station;
            New.Passes[0].Entry.Source = Binding.Station;
        }
        // A service ending part-way along a track at a station should still reach it after a small move.
        if (&Stop == &Plan.Stops.Last() && Stop.Distance == Ends.Last() && Binding.Pass == New.Passes.Num() - 1)
        {
            FRailwaySavedAnchor& End = New.Passes.Last().Exit;
            if (End.Kind == ERailwayAnchorKind::Distance)
            {
                End.Kind = ERailwayAnchorKind::Station;
                End.Source = Binding.Station;
            }
        }
    }
    const FRailwaySavedPass& Current = New.Passes[New.OccupiedPass];
    New.Position.Distance = Local;
    if (Local == Current.Entry.Distance) New.Position = Current.Entry;
    else if (Local == Current.Exit.Distance) New.Position = Current.Exit;
    Bookmark = MoveTemp(New);
    return true;
}

bool Remap(const ARailwayDataActor& Actor, const FRailwayTrainSaveGame& Save,
    FRailwayPlan& Plan, double& Distance, FString& Error)
{
    const FRailwayNetworkData& Data = Actor.Network;
    const FRailwayRouteBookmark& B = Save.Bookmark;
    if (!B.Network.IsValid() || B.Network != Data.NetworkId || !B.Passes.IsValidIndex(B.OccupiedPass) ||
        B.Stops.Num() != Save.RailwayPlan.Stops.Num())
    {
        Error = TEXT("Save has no compatible stable railway references");
        return false;
    }
    Plan = {};
    Plan.BuildId = Data.BuildId;
    Plan.bFinishAtLastStop = Save.RailwayPlan.bFinishAtLastStop;
    TArray<FResolvedPass> Passes;
    double Total = 0;
    for (const FRailwaySavedPass& Pass : B.Passes)
    {
        const int32* Track = Data.TrackById.Find(Pass.Track);
        double Entry, Exit;
        if (!Track || !ResolveAnchor(Data, *Track, Pass.Entry, Entry) || !ResolveAnchor(Data, *Track, Pass.Exit, Exit))
        {
            Error = TEXT("A saved track or interval endpoint no longer exists");
            return false;
        }
        const int32 First = Plan.Sections.Num();
        if (!Routing::AppendTrackPass(Data, *Track, Pass.bReverse, Entry, Exit, Pass.bStartOnIncomingEdge, Plan, Error)) return false;
        const double Begin = Total;
        for (int32 I = First; I < Plan.Sections.Num(); ++I) Total += Plan.Sections[I].TravelDistance;
        Passes.Add({*Track, Begin, Total, Entry, Exit});
    }
    for (const FRailwaySavedStop& Stop : B.Stops)
    {
        const int32* Index = Data.StationById.Find(Stop.Station);
        if (!Index || !Passes.IsValidIndex(Stop.Pass))
        {
            Error = TEXT("A scheduled station or its saved visit no longer exists");
            return false;
        }
        const FRailwayBakedStation& Row = Data.Stations[*Index];
        const FResolvedPass& Pass = Passes[Stop.Pass];
        const FRailwayBakedTrack& Track = Data.Tracks[Pass.Track];
        double Local = Row.Definition.TrackDistance;
        if (Stop.bAtClosingSeam && Track.IsLoop() && Local == 0) Local = Track.Length();
        double At;
        if (Data.Edges[Row.EdgeIndex].TrackIndex != Pass.Track || !Pass.Map(Local, At))
        {
            Error = TEXT("A scheduled station moved off its saved traversal");
            return false;
        }
        Plan.Stops.Add({At, *Index});
    }
    if (Save.State == ERailwayState::Dwelling && Save.NextStop > 0 && Plan.Stops.IsValidIndex(Save.NextStop - 1))
        Distance = Plan.Stops[Save.NextStop - 1].Distance;
    else if (Save.State == ERailwayState::Finished && (!Plan.bFinishAtLastStop || !Plan.Stops.IsEmpty()))
        Distance = Plan.bFinishAtLastStop ? Plan.Stops.Last().Distance : Total;
    else
    {
        const FResolvedPass& Pass = Passes[B.OccupiedPass];
        double Local;
        if (!ResolveAnchor(Data, Pass.Track, B.Position, Local) || !Pass.Map(Local, Distance))
        {
            Error = TEXT("The occupied position moved outside its saved traversal");
            return false;
        }
    }
    return true;
}
}
