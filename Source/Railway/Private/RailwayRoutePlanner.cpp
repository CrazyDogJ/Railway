#include "RailwayRoutePlanner.h"

#include "Components/SplineComponent.h"
#include "RailwayWorldIndex.h"
#include "RailwayRouteGeometry.h"
#include "RailwayTrack.h"

namespace Railway::Routing
{
namespace
{
// 开放轨道可在中途进出。闭环的出口里程也可能小于入口里程。
struct FTrackInterval
{
    double Entry;
    double Exit;
};

constexpr double JunctionToleranceCm = 1.0;


const TCHAR* CheckTrack(const ARailwayTrack* Track, const UWorld* World)
{
    if (!IsValid(Track)) return TEXT("Missing track");
    if (!IsValid(Track->Spline) || Track->GetWorld() != World) return TEXT("Invalid spline or world");

    const USplineComponent& Spline = *Track->Spline;
    const double Length = Spline.GetSplineLength();
    if (!FMath::IsFinite(Length) || Length < 1 || Spline.GetNumberOfSplinePoints() < 2)
        return TEXT("Track must have at least two points and be at least 1 cm long");
    if (!Spline.GetComponentScale().Equals(FVector::OneVector, .0001))
        return TEXT("Use unit track scale; edit spline points to set length");
    return nullptr;
}

bool RejectLeg(int32 Index, const TCHAR* Reason, FString& OutError)
{
    OutError = FString::Printf(TEXT("route[%d]: %s"), Index, Reason);
    return false;
}

// 端点可以共用岔口，也可以挂在相邻轨道的中段。连续同向环线表示再跑一圈。
const TCHAR* Connect(const FRailwayLeg& From, const FRailwayLeg& To, double& ExitDistance, double& EntryDistance)
{
    const USplineComponent& FromSpline = *From.Track->Spline;
    const USplineComponent& ToSpline = *To.Track->Spline;
    if (FromSpline.IsClosedLoop() && From.Track == To.Track)
    {
        return From.bReverse == To.bReverse ? nullptr : TEXT("Consecutive laps on the same track must use the same direction");
    }

    const ARailwayJunction* Exit = FromSpline.IsClosedLoop() ? nullptr :
        (From.bReverse ? From.Track->StartJunction.Get() : From.Track->EndJunction.Get());
    const ARailwayJunction* Entry = ToSpline.IsClosedLoop() ? nullptr :
        (To.bReverse ? To.Track->EndJunction.Get() : To.Track->StartJunction.Get());

    const ARailwayJunction* Joint;
    if (IsValid(Exit) && Exit == Entry)
    {
        Joint = Exit;
    }
    else if (IsValid(Entry) && Entry->HostTrack == From.Track)
    {
        Joint = Entry;
        ExitDistance = Geometry::DistanceAtPosition(FromSpline, Entry->TrackPosition);
    }
    else if (IsValid(Exit) && Exit->HostTrack == To.Track)
    {
        Joint = Exit;
        EntryDistance = Geometry::DistanceAtPosition(ToSpline, Exit->TrackPosition);
    }
    else
    {
        return TEXT("No shared junction between these directed legs");
    }

    const FVector A = FromSpline.GetLocationAtDistanceAlongSpline(ExitDistance, ESplineCoordinateSpace::World);
    const FVector B = ToSpline.GetLocationAtDistanceAlongSpline(EntryDistance, ESplineCoordinateSpace::World);
    if (FVector::Dist(A, B) > JunctionToleranceCm || FVector::Dist(A, Joint->GetActorLocation()) > JunctionToleranceCm ||
        FVector::Dist(B, Joint->GetActorLocation()) > JunctionToleranceCm)
        return TEXT("Snap track endpoints to their junction");
    return nullptr;
}

const TCHAR* CheckIdentities(const FRailwayWorldIndex& WorldIndex, const ARailwayTrack& Track)
{
    if (WorldIndex.Find(Track.RailwayId) != &Track) return TEXT("Missing or ambiguous track ID");
    if (!Track.Spline->IsClosedLoop())
    {
        for (const ARailwayJunction* Joint : {Track.StartJunction.Get(), Track.EndJunction.Get()})
        {
            if (!IsValid(Joint)) continue;
            if (WorldIndex.Find(Joint->RailwayId) != Joint) return TEXT("Missing or ambiguous junction ID");
            if (IsValid(Joint->HostTrack) && WorldIndex.Find(Joint->HostTrack->RailwayId) != Joint->HostTrack.Get())
                return TEXT("Missing or ambiguous junction host ID");
        }
    }
    return nullptr;
}

// 同一套投影用于旧关卡内嵌站点和场景站点。整圈接缝上的站只在返回时停靠。
bool AppendStop(const FRailwayStation& Station, const FRailwayStationRef& Ref, const FRailwaySection& Section, double Length, double Base,
    TArray<FRailwayPlannedStop>& Stops)
{
    if (!FMath::IsFinite(Station.Position) || Station.Position < 0 || Station.Position > 1 ||
        !FMath::IsFinite(Station.DwellSeconds) || Station.DwellSeconds < 0)
        return false;

    const double Offset = Geometry::DistanceAtPosition(*Section.Leg.Track->Spline, Station.Position) - Section.EntryDistance;
    double Along = Section.Leg.bReverse ? -Offset : Offset;
    if (Section.Leg.Track->Spline->IsClosedLoop())
    {
        Along = Geometry::WrapDistance(Along, Length);
        if (Along == 0.0 && Section.TravelDistance == Length) Along = Length;
    }
    if (Along >= 0 && Along <= Section.TravelDistance)
    {
        FRailwayPlannedStop& Stop = Stops.AddDefaulted_GetRef();
        Stop.Distance = Base + Along;
        Stop.Reference = Ref;
    }
    return true;
}

bool ResolveIntervals(const UWorld* World, const FRailwayWorldIndex& WorldIndex, const TArray<FRailwayLeg>& Legs,
    TArray<FTrackInterval>& Intervals, FString& OutError)
{
    Intervals.Reserve(Legs.Num());
    for (int32 Index = 0; Index < Legs.Num(); ++Index)
    {
        const FRailwayLeg& Leg = Legs[Index];
        if (const TCHAR* Error = CheckTrack(Leg.Track, World)) return RejectLeg(Index, Error, OutError);
        if (const TCHAR* Error = CheckIdentities(WorldIndex, *Leg.Track)) return RejectLeg(Index, Error, OutError);
        const double Length = Leg.Track->Spline->GetSplineLength();
        Intervals.Add(FTrackInterval{Leg.bReverse ? Length : 0, Leg.bReverse ? 0 : Length});
    }
    for (int32 Index = 1; Index < Legs.Num(); ++Index)
    {
        if (const TCHAR* Error = Connect(Legs[Index - 1], Legs[Index], Intervals[Index - 1].Exit, Intervals[Index].Entry))
            return RejectLeg(Index, Error, OutError);
    }
    return true;
}

double DirectedTravel(const FRailwayLeg& Leg, const FTrackInterval& Interval)
{
    const double Travel = Leg.bReverse ? Interval.Entry - Interval.Exit : Interval.Exit - Interval.Entry;
    if (!Leg.Track->Spline->IsClosedLoop()) return Travel;
    const double Length = Leg.Track->Spline->GetSplineLength();
    const double Wrapped = Geometry::WrapDistance(Travel, Length);
    return Wrapped == 0.0 ? Length : Wrapped;
}

bool CollectSectionStops(const FRailwayWorldIndex& WorldIndex, const FRailwaySection& Section, double Base,
    TArray<FRailwayPlannedStop>& Stops, FString& Error)
{
    const double Length = Section.Leg.Track->Spline->GetSplineLength();
    return WorldIndex.VisitStations(*Section.Leg.Track, [&](const FRailwayStation& Station, const FRailwayStationRef& Ref)
    {
        if (AppendStop(Station, Ref, Section, Length, Base, Stops)) return true;
        Error = TEXT("Invalid station position or dwell time");
        return false;
    }, Error);
}

void SortStops(FRailwayPlan& Plan)
{
    // 同里程站点沿用原始顺序，以便通过 NextStop 区分已经服务的站次。
    Plan.Stops.StableSort([](const FRailwayPlannedStop& A, const FRailwayPlannedStop& B)
    {
        return A.Distance < B.Distance;
    });
}

const TCHAR* CheckSection(const FRailwaySection& Section, const UWorld* World, bool bHasPrevious, FVector& PreviousExit)
{
    const ARailwayTrack* Track = Section.Leg.Track;
    if (const TCHAR* Error = CheckTrack(Track, World)) return Error;

    const double Length = Track->Spline->GetSplineLength();
    const double Entry = Section.EntryDistance;
    const double Exit = Entry + (Section.Leg.bReverse ? -Section.TravelDistance : Section.TravelDistance);
    if (!FMath::IsFinite(Entry) || Entry < 0 || Entry > Length ||
        !FMath::IsFinite(Section.TravelDistance) || Section.TravelDistance <= 0 ||
        Section.TravelDistance > Length + UE_SMALL_NUMBER ||
        (!Track->Spline->IsClosedLoop() && (Exit < -UE_SMALL_NUMBER || Exit > Length + UE_SMALL_NUMBER)))
        return TEXT("Invalid track geometry or section distance");

    const FVector Start = Geometry::SampleTrack(Section.Leg, Entry).GetLocation();
    if (bHasPrevious && FVector::Dist(Start, PreviousExit) > JunctionToleranceCm)
        return TEXT("Gap between consecutive sections exceeds 1 cm");
    PreviousExit = Geometry::SampleTrack(Section.Leg, Exit).GetLocation();
    return nullptr;
}
}

bool Build(UWorld* World, const TArray<FRailwayLeg>& Legs, TOptional<double> FirstEntry,
    FRailwayPlan& OutPlan, FString& OutError)
{
    OutPlan = FRailwayPlan();
    OutError.Reset();
    if (Legs.IsEmpty() || !World) return RejectLeg(0, TEXT("Empty route or no world"), OutError);

    const FRailwayWorldIndex WorldIndex(World);
    TArray<FTrackInterval> Intervals;
    if (!ResolveIntervals(World, WorldIndex, Legs, Intervals, OutError)) return false;
    if (FirstEntry.IsSet()) Intervals[0].Entry = FirstEntry.GetValue();

    FRailwayPlan Plan;
    Plan.Sections.Reserve(Legs.Num());
    double Base = 0;
    for (int32 Index = 0; Index < Legs.Num(); ++Index)
    {
        const FRailwayLeg& Leg = Legs[Index];
        const FTrackInterval& Interval = Intervals[Index];
        const double Length = Leg.Track->Spline->GetSplineLength();
        const bool bLoop = Leg.Track->Spline->IsClosedLoop();
        const double Span = Interval.Exit - Interval.Entry;

        // 在岔口改线时，第一段可能已经走完。环线里程 0 和 Length 表示同一点。
        if (FirstEntry.IsSet() && Index == 0 && Legs.Num() > 1 && Legs[1].Track != Leg.Track &&
            (FMath::Abs(Span) <= UE_SMALL_NUMBER || (bLoop && FMath::IsNearlyEqual(FMath::Abs(Span), Length, UE_SMALL_NUMBER))))
            continue;

        const double Travel = DirectedTravel(Leg, Interval);
        if (!FMath::IsFinite(Travel) || Travel <= UE_SMALL_NUMBER)
            return RejectLeg(Index, TEXT("Check junction order and Reverse flag on this leg"), OutError);

        FRailwaySection& Section = Plan.Sections.Add_GetRef(FRailwaySection{Leg, Interval.Entry, Travel});
        Section.TrackId = Leg.Track->RailwayId;
        Section.LayoutSignature = Leg.Track->GetLayoutSignature();
        if (!CollectSectionStops(WorldIndex, Section, Base, Plan.Stops, OutError))
        {
            OutError = FString::Printf(TEXT("route[%d]: %s"), Index, *OutError);
            return false;
        }
        Base += Travel;
    }

    SortStops(Plan);
    OutPlan = MoveTemp(Plan);
    return true;
}

// 旧版没有身份和签名。仅用当时经过的区间和站次位置识别候选，遇到歧义保持原列车不动。
static bool MigrateLegacyStops(UWorld* World, const FRailwayWorldIndex& WorldIndex, FRailwayPlan& Plan, bool bLegacySave, FString& Error)
{
    TArray<FRailwayLeg> Legs;
    for (const FRailwaySection& Section : Plan.Sections) Legs.Add(Section.Leg);
    TArray<FTrackInterval> Intervals;
    if (!ResolveIntervals(World, WorldIndex, Legs, Intervals, Error)) return false;
    TArray<FRailwayPlannedStop> Candidates;
    double Base = 0;
    for (int32 I = 0; I < Plan.Sections.Num(); ++I)
    {
        const FRailwaySection& Section = Plan.Sections[I];
        // 途中改线允许首段从任意位置开始。其余进出口必须仍与当前岔口和轨道长度吻合。
        if (I == 0) Intervals[I].Entry = Section.EntryDistance;
        if (!FMath::IsNearlyEqual(Intervals[I].Entry, Section.EntryDistance, .001) ||
            !FMath::IsNearlyEqual(DirectedTravel(Section.Leg, Intervals[I]), Section.TravelDistance, .001))
            return RejectLeg(I, TEXT("Legacy section no longer matches track length or junctions"), Error);
        if (!CollectSectionStops(WorldIndex, Section, Base, Candidates, Error)) return false;
        Base += Section.TravelDistance;
    }
    for (FRailwayPlannedStop& Stop : Plan.Stops)
    {
        if (!bLegacySave && Stop.Reference.Source != ERailwayStationSource::LegacySnapshot) continue;
        const FRailwayStationRef* Match = nullptr;
        for (const FRailwayPlannedStop& Candidate : Candidates)
        {
            if (!FMath::IsNearlyEqual(Stop.Distance, Candidate.Distance, .001) ||
                Stop.Station.Position != Candidate.Reference.PlannedPosition) continue;
            if (Match && !Stations::SameSource(*Match, Candidate.Reference))
            {
                Error = FString::Printf(TEXT("Ambiguous legacy stop at %.3f cm"), Stop.Distance);
                return false;
            }
            Match = &Candidate.Reference;
        }
        if (!Match)
        {
            Error = FString::Printf(TEXT("Legacy stop at %.3f cm has no identifiable source"), Stop.Distance);
            return false;
        }
        Stop.Reference = *Match;
    }
    return true;
}

bool Prepare(UWorld* World, FRailwayPlan Plan, double StartDistance, FRailwayPreparedPlan& OutPrepared,
    FString& OutError, EPlanOrigin Origin)
{
    OutError.Reset();
    const FRailwayWorldIndex WorldIndex(World);
    FVector PreviousExit = FVector::ZeroVector;
    for (int32 Index = 0; Index < Plan.Sections.Num(); ++Index)
    {
        FRailwaySection& Section = Plan.Sections[Index];
        if (Origin == EPlanOrigin::IdentifiedSave && (!Section.TrackId.IsValid() || !Section.LayoutSignature.IsValid()))
            return RejectLeg(Index, TEXT("Saved section is missing its track ID or layout signature"), OutError);
        if (Section.TrackId.IsValid())
        {
            Section.Leg.Track = Cast<ARailwayTrack>(WorldIndex.Find(Section.TrackId));
            if (!Section.Leg.Track)
            {
                OutError = FString::Printf(TEXT("route[%d]: track %s is missing, unloaded or ambiguous"), Index, *Section.TrackId.ToString());
                return false;
            }
        }
        if (const TCHAR* Error = CheckSection(Section, World, Index > 0, PreviousExit))
            return RejectLeg(Index, Error, OutError);
        ARailwayTrack& Track = *Section.Leg.Track;
        if (const TCHAR* Error = CheckIdentities(WorldIndex, Track)) return RejectLeg(Index, Error, OutError);
        const FGuid Signature = Track.GetLayoutSignature();
        if (Section.LayoutSignature.IsValid() && Section.LayoutSignature != Signature)
            return RejectLeg(Index, TEXT("Track geometry or junction connections changed since this plan was built"), OutError);
        Section.TrackId = Track.RailwayId;
        Section.LayoutSignature = Signature;
    }
    const bool bNeedsMigration = Origin == EPlanOrigin::LegacySave || Plan.Stops.ContainsByPredicate([](const FRailwayPlannedStop& Stop)
    {
        return Stop.Reference.Source == ERailwayStationSource::LegacySnapshot;
    });
    if (bNeedsMigration && Origin != EPlanOrigin::IdentifiedSave &&
        !MigrateLegacyStops(World, WorldIndex, Plan, Origin == EPlanOrigin::LegacySave, OutError)) return false;

    TArray<double> Ends;
    Geometry::BuildSectionEnds(Plan, Ends);
    if (Ends.IsEmpty() || !FMath::IsFinite(StartDistance) || StartDistance < 0 || StartDistance > Ends.Last())
    {
        OutError = TEXT("Empty plan or start outside the route");
        return false;
    }
    for (FRailwayPlannedStop& Stop : Plan.Stops)
    {
        if (Origin == EPlanOrigin::IdentifiedSave && (Stop.Reference.Source == ERailwayStationSource::Unspecified ||
            Stop.Reference.Source == ERailwayStationSource::LegacySnapshot))
        {
            OutError = TEXT("Saved stop is missing its source");
            return false;
        }
        if (!WorldIndex.BindStop(Stop, OutError)) return false;
        if (!FMath::IsFinite(Stop.Distance) || Stop.Distance < 0 || Stop.Distance > Ends.Last())
        {
            OutError = TEXT("Invalid planned stop distance");
            return false;
        }
        if (Stop.Reference.Source != ERailwayStationSource::Inline)
        {
            const auto* Host = Cast<ARailwayTrack>(WorldIndex.Find(Stop.Reference.HostTrackId));
            int32 SectionIndex;
            double Local;
            FTransform Pose;
            if (!Host || !IsValid(Host->Spline) ||
                !Geometry::SampleIndexedPlan(Plan, Ends, Stop.Distance, SectionIndex, Local, Pose))
            {
                OutError = TEXT("Cannot locate planned world station");
                return false;
            }
            const double StationDistance = Geometry::DistanceAtPosition(*Host->Spline, Stop.Reference.PlannedPosition);
            const FVector StationLocation = Host->Spline->GetLocationAtDistanceAlongSpline(StationDistance, ESplineCoordinateSpace::World);
            if (FVector::Dist(Pose.GetLocation(), StationLocation) > JunctionToleranceCm)
            {
                OutError = FString::Printf(TEXT("Station %s no longer lies at its planned distance"), *Stop.Reference.StationId.ToString());
                return false;
            }
        }
    }
    SortStops(Plan);
    if (Plan.bFinishAtLastStop && (Plan.Stops.IsEmpty() || StartDistance > Plan.Stops.Last().Distance))
    {
        OutError = TEXT("Direct service requires a destination at or ahead of the start");
        return false;
    }

    double TrackDistance;
    if (!Geometry::SampleIndexedPlan(Plan, Ends, StartDistance, OutPrepared.StartSection, TrackDistance, OutPrepared.StartPose))
    {
        OutError = TEXT("Cannot sample the start of the prepared route");
        return false;
    }
    OutPrepared.Plan = MoveTemp(Plan);
    OutPrepared.SectionEnds = MoveTemp(Ends);
    return true;
}
}
