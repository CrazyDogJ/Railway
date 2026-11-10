#include "RailwayTrain.h"

#include "Algo/Reverse.h"
#include "RailwayRouteGeometry.h"
#include "RailwayRoutePlanner.h"

namespace
{
// The baker emits the forward and reverse directed edges of each physical span consecutively.
int32 Opposite(int32 Edge) { return Edge ^ 1; }

int32 EdgeAt(const FRailwayNetworkData& N, int32 TrackIndex, double Local, bool bReverse)
{
    const auto& Track = N.Tracks[TrackIndex];
    int32 At = INDEX_NONE;
    for (int32 I = Track.FirstEdge + int32(bReverse); I < Track.FirstEdge + Track.NumEdges; I += 2)
        if (N.Edges[I].Contains(Local))
        {
            At = I;
            if (Local != N.Edges[I].EndDistance) break;
        }
    return At;
}
}

bool ARailwayTrain::GetCabEdge(int32& Edge, double& Local) const
{
    const auto* D = GetRailwayData();
    if (!D || !IsPlanPresentationCurrent()) return false;
    int32 Section;
    if (Railway::Geometry::Locate(D->Network, ActivePlan, SectionEnds, Distance, Section, Local))
    {
        Edge = ActivePlan.Sections[Section].Leg.EdgeIndex;
        if (ActivePlan.bCabReversed) Edge = Opposite(Edge);
        return D->Network.Edges.IsValidIndex(Edge);
    }
    FRailwayTrackLocation At;
    if (!ActivePlan.Sections.IsEmpty() || !GetCurrentTrackLocation(At)) return false;
    const auto& Track = D->Network.Tracks[At.TrackIndex];
    Local = At.TrackDistance;
    const bool bReverse = FVector::DotProduct(GetActorForwardVector(), Track.PoseAt(Local, false).GetRotation().GetForwardVector()) < 0;
    Edge = EdgeAt(D->Network, At.TrackIndex, Local, bReverse);
    return Edge != INDEX_NONE;
}

int32 ARailwayTrain::PreferredExit(const FRailwayNetworkData& N, int32 Approach, int32 Exclude) const
{
    const auto& In = N.Edges[Approach];
    if (!In.NumSuccessors) return INDEX_NONE;
    if (In.NumSuccessors == 1)
    {
        const int32 Only = N.Successors[In.FirstSuccessor];
        return Only != Exclude ? Only : INDEX_NONE;
    }
    if (Turnouts.BuildId == N.BuildId)
        for (const auto& S : Turnouts.Settings)
            if (S.Approach == Approach && S.Exit != Exclude && N.CanFollow(Approach, S.Exit)) return S.Exit;

    // Prefer the through track, then the shallowest legal deflection at a free junction.
    const auto& From = N.Tracks[In.TrackIndex];
    const double Step = FMath::Min(1.0, FMath::Abs(In.EndDistance - In.StartDistance) * .25);
    const FVector Incoming = (From.PositionAt(In.EndDistance) - From.PositionAt(In.EndDistance - In.Sign() * Step)).GetSafeNormal();
    int32 Best = INDEX_NONE;
    double Score = -DBL_MAX;
    for (int32 J = 0; J < In.NumSuccessors; ++J)
    {
        const int32 I = N.Successors[In.FirstSuccessor + J];
        if (I == Exclude) continue;
        const auto& E = N.Edges[I];
        const auto& To = N.Tracks[E.TrackIndex];
        const double Sample = E.Sign() * FMath::Min(1.0, FMath::Abs(E.EndDistance - E.StartDistance) * .25);
        const FVector Heading = (To.PositionAt(E.StartDistance + Sample) - To.PositionAt(E.StartDistance)).GetSafeNormal();
        const double Rank = (E.TrackIndex == In.TrackIndex ? 3.0 : 0.0) + FVector::DotProduct(Incoming, Heading);
        if (Rank > Score) { Best = I; Score = Rank; }
    }
    return Best;
}

void ARailwayTrain::RememberExit(const FRailwayNetworkData& N, int32 Approach, int32 Exit)
{
    if (!N.CanFollow(Approach, Exit) || N.Edges[Approach].NumSuccessors < 2) return;
    if (Turnouts.BuildId != N.BuildId)
    {
        Turnouts = {};
        Turnouts.NetworkId = N.NetworkId;
        Turnouts.BuildId = N.BuildId;
    }
    FRailwayTurnoutSetting* Setting = Turnouts.Settings.FindByPredicate([&](const auto& S){ return S.Approach == Approach; });
    if (Setting && Setting->Exit == Exit) return;
    if (!Setting) Setting = &Turnouts.Settings.AddDefaulted_GetRef();
    const auto& A = N.Edges[Approach];
    const auto& B = N.Edges[Exit];
    Setting->Approach = Approach;
    Setting->Exit = Exit;
    Setting->Junction = N.JunctionByNode.FindRef(A.ToNode);
    Setting->FromTrack = N.Tracks[A.TrackIndex].Id;
    Setting->ToTrack = N.Tracks[B.TrackIndex].Id;
    Setting->bFromReverse = A.IsReverse();
    Setting->bToReverse = B.IsReverse();
}

void ARailwayTrain::RestoreTurnouts(const FRailwayTurnouts& Saved)
{
    Turnouts = {};
    const auto* D = GetRailwayData();
    if (!D || !Saved.NetworkId.IsValid() || Saved.NetworkId != D->Network.NetworkId) return;
    const auto& N = D->Network;
    for (const auto& S : Saved.Settings)
    {
        if (Saved.BuildId == N.BuildId)
        {
            RememberExit(N, S.Approach, S.Exit);
            continue;
        }
        const int32* From = N.TrackById.Find(S.FromTrack);
        const int32* To = N.TrackById.Find(S.ToTrack);
        if (!From || !To || !S.Junction.IsValid()) continue;
        const auto& Track = N.Tracks[*From];
        int32 Count = 0, A = INDEX_NONE, B = INDEX_NONE;
        for (int32 I = Track.FirstEdge + int32(S.bFromReverse); I < Track.FirstEdge + Track.NumEdges; I += 2)
        {
            const auto& E = N.Edges[I];
            if (N.JunctionByNode.FindRef(E.ToNode) != S.Junction) continue;
            for (int32 J = 0; J < E.NumSuccessors; ++J)
            {
                const int32 Next = N.Successors[E.FirstSuccessor + J];
                if (N.Edges[Next].TrackIndex == *To && N.Edges[Next].IsReverse() == S.bToReverse)
                { ++Count; A = I; B = Next; }
            }
        }
        if (Count == 1) RememberExit(N, A, B);
    }
}

void ARailwayTrain::GetUpcomingTrackChoices(bool bTowardsRear, TArray<FRailwayTrackChoice>& Choices) const
{
    Choices.Reset();
    const auto* D = GetRailwayData();
    int32 Edge;
    double Local;
    if (!D || !GetCabEdge(Edge, Local)) return;
    const auto& N = D->Network;
    if (bTowardsRear) Edge = Opposite(Edge);
    const bool bAlongPlan = bTowardsRear == ActivePlan.bCabReversed;
    int32 Section = State == ERailwayState::Idle ? INDEX_NONE : Leg;
    double Ahead = FMath::Abs(N.Edges[Edge].EndDistance - Local);
    TSet<int32> Seen;
    while (!Seen.Contains(Edge))
    {
        Seen.Add(Edge);
        const auto& In = N.Edges[Edge];
        if (In.NumSuccessors == 0) break;
        int32 Selected = PreferredExit(N, Edge);
        const int32 NextSection = Section + (bAlongPlan ? 1 : -1);
        bool bPlanned = false;
        if (Section != INDEX_NONE && ActivePlan.Sections.IsValidIndex(NextSection) &&
            (!bAlongPlan || Distance + Ahead < GetServiceEndDistance()) &&
            (bAlongPlan || Distance > Ahead))
        {
            int32 Planned = ActivePlan.Sections[NextSection].Leg.EdgeIndex;
            if (!bAlongPlan) Planned = Opposite(Planned);
            if (N.CanFollow(Edge, Planned)) { Selected = Planned; Section = NextSection; bPlanned = true; }
            else Section = INDEX_NONE;
        }
        else Section = INDEX_NONE;
        if (In.NumSuccessors > 1)
        {
            for (int32 J = 0; J < In.NumSuccessors; ++J)
            {
                const int32 I = N.Successors[In.FirstSuccessor + J];
                const auto& Track = N.Tracks[N.Edges[I].TrackIndex];
                FRailwayTrackChoice& C = Choices.AddDefaulted_GetRef();
                C.View = {const_cast<ARailwayTrain*>(this), N.BuildId, GetPlanRevision()};
                C.ApproachEdge = Edge; C.OutgoingEdge = I; C.SelectedEdge = Selected;
                C.TrackId = Track.Id; C.TrackName = Track.Name; C.DistanceToJunction = Ahead;
                C.bTowardsRear = bTowardsRear;
                C.bContinuesSameTrack = N.Edges[I].TrackIndex == In.TrackIndex;
                C.bActive = I == Selected;
                C.bUsedByCurrentPlan = bPlanned && C.bActive;
            }
            return;
        }
        Edge = Selected;
        Ahead += FMath::Abs(N.Edges[Edge].EndDistance - N.Edges[Edge].StartDistance);
    }
}

bool ARailwayTrain::ChangeTurnout(const FRailwayPlanCommand& Cmd, FString& Error)
{
    TArray<FRailwayTrackChoice> Choices;
    GetUpcomingTrackChoices(Cmd.bTowardsRear, Choices);
    const auto* Choice = Choices.FindByPredicate([&](const auto& C)
    {
        return C.ApproachEdge == Cmd.ApproachEdge && C.OutgoingEdge == Cmd.OutgoingEdge && C.SelectedEdge == Cmd.ExpectedSelectedEdge;
    });
    if (!Choice)
    {
        Error = TEXT("列车已越过该道岔或选线已更新，请重新选择");
        return false;
    }
    const auto& N = GetRailwayData()->Network;
    int32 Exit = Choice->OutgoingEdge;
    if (Cmd.Action == ERailwayPlanAction::DeactivateTrack)
    {
        if (!Choice->bActive) { Error = TEXT("这条轨道当前未激活"); return false; }
        Exit = PreferredExit(N, Choice->ApproachEdge, Exit);
        if (Exit == INDEX_NONE) { Error = TEXT("没有其他合法出口"); return false; }
    }
    const bool bOverridesService = Choices.ContainsByPredicate([](const auto& C){ return C.bUsedByCurrentPlan; }) && Exit != Choice->SelectedEdge;
    if (bOverridesService) return CancelService(Error, Cmd.ApproachEdge, Exit);
    RememberExit(N, Cmd.ApproachEdge, Exit);
    RememberExit(N, Opposite(Exit), Opposite(Cmd.ApproachEdge));
    OnRep_Turnouts();
    ForceNetUpdate();
    return true;
}

void ARailwayTrain::OnRep_Turnouts()
{
    OnRailwayTrackChanged.Broadcast();
}

bool ARailwayTrain::SetFreeEdge(int32 CabEdge, double Local)
{
    const auto* D = GetRailwayData();
    if (!D || !D->Network.Edges.IsValidIndex(CabEdge)) return false;
    const auto& E = D->Network.Edges[CabEdge];
    if (!E.Contains(Local)) return false;
    FRailwayPlan Rail;
    Rail.BuildId = D->Network.BuildId;
    Rail.Sections.Add({{CabEdge}, E.StartDistance, FMath::Abs(E.EndDistance - E.StartDistance)});
    InstallPlan(MoveTemp(Rail));
    Railway::Geometry::BuildSectionEnds(ActivePlan, SectionEnds);
    Distance = FMath::Abs(Local - E.StartDistance);
    Leg = NextStop = 0;
    return true;
}

void ARailwayTrain::StepFree(double Dt)
{
    const bool bPowered = DriveMode == ERailwayDriveMode::Manual && DriveInput != 0 && BrakeInput == 0;
    if (Speed == 0 && !bPowered) return;
    int32 CabEdge;
    double Local;
    if (!GetCabEdge(CabEdge, Local)) { FailRoute(TEXT("Cannot locate train for free driving")); return; }
    const auto& N = GetRailwayData()->Network;
    const auto& E = N.Edges[CabEdge];
    if (ActivePlan.Sections.Num() != 1 || ActivePlan.bCabReversed || ActivePlan.Sections[0].EntryDistance != E.StartDistance ||
        ActivePlan.Sections[0].TravelDistance != FMath::Abs(E.EndDistance - E.StartDistance))
    {
        if (!SetFreeEdge(CabEdge, Local)) return;
    }
    const double Before = Speed;
    if (bPowered)
    {
        const double Limit = FMath::Max(0.f, ExternalSpeedLimit >= 0 ? FMath::Min(CruiseSpeed, ExternalSpeedLimit) : CruiseSpeed);
        const double Requested = CalculateDriverTargetSpeed(Limit, Dt);
        const double Change = FMath::Max(1.f, Acceleration) * Dt;
        Speed += FMath::Clamp(Requested - Speed, -Change, Change);
        if (Before * Speed < 0) Speed = 0;
    }
    else
    {
        const double Decel = FMath::Max(0.f, RollingResistance) + FMath::Max(1.f, Braking) * BrakeInput;
        Speed = FMath::Sign(Before) * FMath::Max(0.0, FMath::Abs(Before) - Dt * Decel);
    }
    const double Travel = .5 * (Before + Speed) * Dt;
    const bool bRear = Travel < 0;
    double Left = FMath::Abs(Travel);
    while (Left > 0)
    {
        const double Length = GetRouteLength();
        const double Available = bRear ? Distance : Length - Distance;
        const double Move = FMath::Min(Left, Available);
        Distance = FMath::Clamp(Distance + (bRear ? -Move : Move), 0.0, Length);
        Left -= Move;
        if (Left <= 0) break;
        const int32 Approach = bRear ? Opposite(CabEdge) : CabEdge;
        const int32 Exit = PreferredExit(N, Approach);
        if (Exit == INDEX_NONE) { Speed = 0; break; } // physical buffer stop
        RememberExit(N, Opposite(Exit), Opposite(Approach));
        CabEdge = bRear ? Opposite(Exit) : Exit;
        if (!SetFreeEdge(CabEdge, N.Edges[Exit].StartDistance) || GetRouteLength() <= 0)
        { FailRoute(TEXT("Invalid free-driving span")); return; }
    }
    UpdatePose();
}

// A new route often begins exactly under the train. If the train still has velocity
// away from its new goal, give it physical rail on which to brake before backing up.
bool ARailwayTrain::AddBrakingRoom(FRailwayPreparedPlan& Prepared, double RequiredDistance, FString& Error) const
{
    double Need = RequiredDistance - Prepared.StartDistance;
    if (Need <= 0) return true;
    const auto& N = Prepared.Data->Network;
    auto& First = Prepared.Plan.Sections[0];
    int32 Outward = Opposite(First.Leg.EdgeIndex);
    const auto& E = N.Edges[First.Leg.EdgeIndex];
    const double OldEntry = First.EntryDistance;
    const double OldExit = First.TravelDistance == FMath::Abs(E.EndDistance - OldEntry) ? E.EndDistance :
        OldEntry + E.Sign() * First.TravelDistance;
    const double Available = FMath::Abs(OldEntry - E.StartDistance);
    const double Extended = FMath::Min(Need, Available);
    First.EntryDistance = Extended == Available ? E.StartDistance : OldEntry - E.Sign() * Extended;
    First.TravelDistance = FMath::Abs(OldExit - First.EntryDistance);
    double Added = Extended;
    Need -= Extended;
    TArray<FRailwaySection> Prefix;
    // Continue along the prior itinerary, where available, while the train runs out.
    const bool bOldForward = (Speed > 0) != ActivePlan.bCabReversed;
    int32 OldSection = Leg;
    while (Need > 0)
    {
        int32 Next = PreferredExit(N, Outward);
        if (Next == INDEX_NONE) break; // the rail physically ends before a full braking distance
        const int32 OldNext = OldSection + (bOldForward ? 1 : -1);
        if (OldSection != INDEX_NONE && ActivePlan.Sections.IsValidIndex(OldNext) && State != ERailwayState::Idle)
        {
            int32 Prior = ActivePlan.Sections[OldNext].Leg.EdgeIndex;
            if (!bOldForward) Prior = Opposite(Prior);
            if (N.CanFollow(Outward, Prior)) { Next = Prior; OldSection = OldNext; }
            else OldSection = INDEX_NONE;
        }
        else OldSection = INDEX_NONE;
        const auto& Span = N.Edges[Next];
        const double Length = FMath::Abs(Span.EndDistance - Span.StartDistance);
        if (Length <= 0 || !N.CanFollow(Opposite(Next), Opposite(Outward))) break;
        const double Take = FMath::Min(Need, Length);
        const double Entry = Take == Length ? Span.EndDistance : Span.StartDistance + Span.Sign() * Take;
        Prefix.Add({{Opposite(Next)}, Entry, FMath::Abs(Entry - Span.StartDistance)});
        Added += Take;
        if (Need - Take == Need) break;
        Need -= Take;
        Outward = Next;
    }
    if (Added == 0) return true;
    Algo::Reverse(Prefix);
    Prepared.Plan.Sections.Insert(Prefix, 0);
    for (auto& Stop : Prepared.Plan.Stops) Stop.Distance += Added;
    const double Start = Prepared.StartDistance + Added;
    return Railway::Routing::Prepare(Prepared.Data, MoveTemp(Prepared.Plan), Start, Prepared, Error);
}
