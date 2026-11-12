#include "RailwayWorldSubsystem.h"

#include "RailwayDataActor.h"
#include "RailwayTrain.h"

namespace
{
struct FTrafficTrain
{
    ARailwayTrain* Train;
    int32 CabEdge;
    double Local;
    int32 Direction; // relative to the cab, zero means stationary
    double BrakeDistance;
    double Allowance = DBL_MAX;

    void HoldAt(double Ahead) { Allowance = FMath::Min(Allowance, FMath::Max(0., Ahead)); }
};

struct FSpanVisit
{
    int32 Train;
    int32 Edge;
    double Entry;
    double Exit;
    double Ahead; // arc length from the current train center to Entry
    bool bCurrent;

    double Low() const { return FMath::Min(Entry, Exit); }
    double High() const { return FMath::Max(Entry, Exit); }
};

struct FNodeVisit
{
    int32 Train;
    double Distance;
    bool bOccupied; // a parked train or the part of a moving train clearing the junction
};

double Separation(const FTrafficTrain& A, const FTrafficTrain& B)
{
    return .5 * (FMath::Max(0., A.Train->TrainLength) + FMath::Max(0., B.Train->TrainLength)) +
        FMath::Max3(0., A.Train->TrafficClearance, B.Train->TrafficClearance);
}

// Preserve the occupied span even for trains without a movement request.
int32 TravelDirection(const ARailwayTrain& T, double Dt)
{
    if (T.Speed != 0) return T.Speed > 0 ? 1 : -1;
    if (T.CruiseSpeed <= 0 || T.ExternalSpeedLimit == 0) return 0;
    if (T.State == ERailwayState::Dwelling && T.Wait > Dt) return 0;
    if (T.DriveMode == ERailwayDriveMode::Manual)
    {
        const int32 Input = T.BrakeInput > 0 ? 0 : int32(FMath::Sign(T.DriveInput));
        const int32 AlongPlan = T.ActivePlan.bCabReversed ? -Input : Input;
        if (T.State == ERailwayState::Finished && (AlongPlan >= 0 || T.Distance <= 0)) return 0;
        if (T.State == ERailwayState::Running && AlongPlan < 0 && T.Distance <= 0) return 0;
        return Input;
    }
    if (T.State == ERailwayState::Running || T.State == ERailwayState::Dwelling)
        return T.ActivePlan.bCabReversed ? -1 : 1;
    return 0;
}

bool HasPriority(double DistanceA, int32 A, double DistanceB, int32 B)
{
    // Train indices preserve registration order for simultaneous arrivals.
    return DistanceA < DistanceB || (DistanceA == DistanceB && A < B);
}

void ProtectSpan(const FRailwayNetworkData& Network, TArray<FTrafficTrain>& Trains,
    const FSpanVisit& A, const FSpanVisit& B)
{
    if (A.Train == B.Train) return;
    auto& First = Trains[A.Train];
    auto& Second = Trains[B.Train];
    const double Gap = Separation(First, Second);
    auto ProtectOccupant = [&](const FSpanVisit& In, FTrafficTrain& Driver, const FSpanVisit& Other)
    {
        if (!Other.bCurrent || !Driver.Direction) return;
        const double Along = (Other.Entry - In.Entry) * Network.Edges[In.Edge].Sign();
        if (Along < 0 || Along > FMath::Abs(In.Exit - In.Entry) + Gap) return;
        // Permit one of two co-located trains to pull out in the same direction.
        if (Along == 0 && In.bCurrent && In.Edge == Other.Edge && In.Train < Other.Train &&
            Trains[Other.Train].Direction != 0) return;
        Driver.HoldAt(In.Ahead + Along - Gap);
    };
    ProtectOccupant(A, First, B);
    ProtectOccupant(B, Second, A);
    if (A.Edge == B.Edge || !First.Direction || !Second.Direction) return;
    const double Sign = Network.Edges[A.Edge].Sign();
    const double Between = (B.Entry - A.Entry) * Sign;
    if (Between < 0 || (B.Exit - A.Exit) * Sign > Gap) return;

    if (A.bCurrent && B.bCurrent)
    {
        // Each driver receives a share of the same free rail. Even a long frame
        // cannot allow both centers to spend the entire closing distance.
        const double Free = FMath::Max(0., Between - Gap);
        const double WeightA = First.BrakeDistance + 1.;
        const double Share = Free * WeightA / (WeightA + Second.BrakeDistance + 1.);
        First.HoldAt(Share);
        Second.HoldAt(Free - Share);
        return;
    }

    // Opposite entries into a single-track span. Its occupant has right of way;
    // otherwise the nearer entry wins. The other driver waits outside the throat.
    if (A.bCurrent || (!B.bCurrent && HasPriority(A.Ahead, A.Train, B.Ahead, B.Train)))
        Second.HoldAt(B.Ahead - Gap);
    else
        First.HoldAt(A.Ahead - Gap);
}

void ProtectNode(TArray<FTrafficTrain>& Trains, const FNodeVisit& A, const FNodeVisit& B)
{
    if (A.Train == B.Train || (A.bOccupied && B.bOccupied)) return;
    const double Gap = Separation(Trains[A.Train], Trains[B.Train]);
    if (A.bOccupied || B.bOccupied)
    {
        const auto& Occupant = A.bOccupied ? A : B;
        const auto& Incoming = A.bOccupied ? B : A;
        if (Occupant.Distance < Gap)
            Trains[Incoming.Train].HoldAt(Incoming.Distance - (Gap - Occupant.Distance));
        return;
    }
    const auto& Yield = HasPriority(A.Distance, A.Train, B.Distance, B.Train) ? B : A;
    Trains[Yield.Train].HoldAt(Yield.Distance - Gap);
}
}

void URailwayWorldSubsystem::RefreshTraffic(const ARailwayTrain* Requester, int32 RequestedDirection)
{
    bTrafficDirty = false;
    if (Trains.IsEmpty()) return;
    if (Trains.Num() == 1)
    {
        if (auto* T = Trains[0].Get()) T->TrafficRemaining = DBL_MAX;
        return;
    }
    TMap<const ARailwayDataActor*, TArray<FTrafficTrain>> Networks;
    // GetCabEdge may execute a Blueprint preview override on an unplaced train.
    // Iterate a snapshot so a spawn/destroy callback cannot invalidate the registry iterator.
    for (ARailwayTrain* T : GetRailwayTrains())
    {
        if (!IsValid(T)) continue;
        T->TrafficRemaining = DBL_MAX;
        T->TrafficDirection = T == Requester ? RequestedDirection : TravelDirection(*T, FrameSeconds);
        if (!T->HasAuthority()) continue;
        const auto* Data = T->GetRailwayData();
        int32 Edge;
        double Local;
        if (!Data || !T->GetCabEdge(Edge, Local) || !IsValid(T)) continue;
        const double BrakeDistance = FMath::Square(double(T->Speed)) / (2 * FMath::Max(1.f, T->Braking));
        Networks.FindOrAdd(Data).Add({T, Edge, Local, T->TrafficDirection, BrakeDistance});
    }
    for (auto& Group : Networks)
    {
        auto& Fleet = Group.Value;
        if (Fleet.Num() < 2) continue;
        const auto& N = Group.Key->Network;
        double MaxTravel = 0;
        double MaxLength = 0, MaxClearance = 0;
        for (const auto& F : Fleet)
        {
            MaxLength = FMath::Max(MaxLength, F.Train->TrainLength);
            MaxClearance = FMath::Max(MaxClearance, F.Train->TrafficClearance);
            const double Speed = FMath::Abs(F.Train->Speed) + FMath::Max(1.f, F.Train->Acceleration) * FrameSeconds;
            MaxTravel = FMath::Max(MaxTravel, Speed * FrameSeconds + FMath::Square(Speed) / (2 * FMath::Max(1.f, F.Train->Braking)));
        }
        const double MaxGap = MaxLength + MaxClearance;
        const double Horizon = 2 * MaxTravel + MaxGap + 1.;
        TMap<int32, TArray<FSpanVisit>> Spans;
        TMap<int32, TArray<FNodeVisit>> Nodes;
        for (int32 I = 0; I < Fleet.Num(); ++I)
        {
            const FTrafficTrain& F = Fleet[I];
            const ARailwayTrain& T = *F.Train;
            // Walk a bounded rail corridor. Planned sections can revisit an edge;
            // in free driving one full lap exposes all conflicts on a deterministic loop.
            auto Trace = [&](int32 CabDirection, double Range, bool bFootprint)
            {
                int32 Edge = F.CabEdge ^ int32(CabDirection < 0);
                int32 Section = T.Leg;
                const bool bFree = T.ActivePlan.Sections.IsEmpty() || T.State == ERailwayState::Idle || T.State == ERailwayState::InvalidRoute;
                const bool bForward = (CabDirection < 0) == T.ActivePlan.bCabReversed;
                if (!bFootprint && !bFree)
                {
                    double End = T.GetServiceEndDistance();
                    if (T.ActivePlan.Stops.IsValidIndex(T.NextStop)) End = T.ActivePlan.Stops[T.NextStop].Distance;
                    Range = FMath::Min(Range, FMath::Max(0., bForward ? End - T.Distance : T.Distance));
                }
                double Local = F.Local, Ahead = 0;
                TSet<int32> Seen;
                bool bCurrent = true;
                while (N.Edges.IsValidIndex(Edge))
                {
                    const auto& E = N.Edges[Edge];
                    const double ToEnd = FMath::Abs(E.EndDistance - Local);
                    const double Left = Range - Ahead;
                    if (!bFootprint)
                    {
                        const double End = Local + E.Sign() * FMath::Min(ToEnd, Left);
                        Spans.FindOrAdd(Edge / 2).Add({I, Edge, Local, End, Ahead, bCurrent});
                    }
                    if (ToEnd > Left)
                    {
                        // A station or route endpoint just short of a turnout can foul the junction.
                        if (!bFootprint && ToEnd - Left <= MaxGap) Nodes.FindOrAdd(E.ToNode).Add({I, Ahead + ToEnd, false});
                        break;
                    }
                    Ahead += ToEnd;
                    Nodes.FindOrAdd(E.ToNode).Add({I, Ahead, bFootprint});
                    if (Ahead >= Range) break;
                    Section += bForward ? 1 : -1;
                    if (!bFree && T.ActivePlan.Sections.IsValidIndex(Section))
                        Edge = T.ActivePlan.Sections[Section].Leg.EdgeIndex ^ int32(!bForward);
                    else
                    {
                        if (!bFree && !bFootprint) break;
                        Edge = T.PreferredExit(N, Edge);
                        if (Seen.Contains(Edge)) break;
                        Seen.Add(Edge);
                    }
                    if (!N.Edges.IsValidIndex(Edge)) break;
                    Local = N.Edges[Edge].StartDistance;
                    bCurrent = false;
                }
            };
            if (F.Direction)
            {
                Trace(F.Direction, Horizon, false);
                Trace(-F.Direction, MaxGap, true);
            }
            else
            {
                Spans.FindOrAdd(F.CabEdge / 2).Add({I, F.CabEdge, F.Local, F.Local, 0, true});
                Trace(1, MaxGap, true);
                Trace(-1, MaxGap, true);
            }
        }
        for (auto& GroupOfSpans : Spans)
        {
            auto& Visits = GroupOfSpans.Value;
            Visits.Sort([](const FSpanVisit& A, const FSpanVisit& B){ return A.Low() < B.Low(); });
            for (int32 A = 0; A < Visits.Num(); ++A)
            {
                const double Reach = Visits[A].High() + MaxGap;
                for (int32 B = A + 1; B < Visits.Num() && Visits[B].Low() <= Reach; ++B)
                    ProtectSpan(N, Fleet, Visits[A], Visits[B]);
            }
        }
        for (const auto& GroupOfNodes : Nodes)
        {
            const auto& Visits = GroupOfNodes.Value;
            for (int32 A = 0; A < Visits.Num(); ++A)
                for (int32 B = A + 1; B < Visits.Num(); ++B)
                    ProtectNode(Fleet, Visits[A], Visits[B]);
        }
        for (const auto& F : Fleet) F.Train->TrafficRemaining = F.Allowance;
    }
}
