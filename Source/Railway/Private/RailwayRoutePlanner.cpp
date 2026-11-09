#include "RailwayRoutePlanner.h"

#include "Components/SplineComponent.h"
#include "EngineUtils.h"
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
using FStationMap = TMultiMap<ARailwayTrack*, FRailwayStation>;

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

FStationMap CollectStations(UWorld* World)
{
    FStationMap Markers;
    for (TActorIterator<ARailwayStationMarker> It(World); It; ++It)
    {
        if (IsValid(It->HostTrack))
            Markers.Add(It->HostTrack, It->GetStationDefinition());
    }
    return Markers;
}

// 同一套投影用于旧关卡内嵌站点和场景站点。整圈接缝上的站只在返回时停靠。
bool AppendStop(const FRailwayStation& Station, const FRailwaySection& Section, double Length, double Base,
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
        Stops.Add(FRailwayPlannedStop{Base + Along, Station});
    return true;
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

    TArray<FTrackInterval> Intervals;
    Intervals.Reserve(Legs.Num());
    for (int32 Index = 0; Index < Legs.Num(); ++Index)
    {
        const FRailwayLeg& Leg = Legs[Index];
        if (const TCHAR* Error = CheckTrack(Leg.Track, World)) return RejectLeg(Index, Error, OutError);
        const double Length = Leg.Track->Spline->GetSplineLength();
        Intervals.Add(FTrackInterval{Leg.bReverse ? Length : 0, Leg.bReverse ? 0 : Length});
    }
    for (int32 Index = 1; Index < Legs.Num(); ++Index)
    {
        if (const TCHAR* Error = Connect(Legs[Index - 1], Legs[Index], Intervals[Index - 1].Exit, Intervals[Index].Entry))
            return RejectLeg(Index, Error, OutError);
    }
    if (FirstEntry.IsSet()) Intervals[0].Entry = FirstEntry.GetValue();

    const FStationMap Markers = CollectStations(World);
    FRailwayPlan Plan;
    Plan.Sections.Reserve(Legs.Num());
    double Base = 0;
    for (int32 Index = 0; Index < Legs.Num(); ++Index)
    {
        const FRailwayLeg& Leg = Legs[Index];
        const FTrackInterval& Interval = Intervals[Index];
        const double Length = Leg.Track->Spline->GetSplineLength();
        const bool bLoop = Leg.Track->Spline->IsClosedLoop();
        double Travel = Leg.bReverse ? Interval.Entry - Interval.Exit : Interval.Exit - Interval.Entry;

        // 在岔口改线时，第一段可能已经走完。环线里程 0 和 Length 表示同一点。
        if (FirstEntry.IsSet() && Index == 0 && Legs.Num() > 1 && Legs[1].Track != Leg.Track &&
            (FMath::Abs(Travel) <= UE_SMALL_NUMBER || (bLoop && FMath::IsNearlyEqual(FMath::Abs(Travel), Length, UE_SMALL_NUMBER))))
            continue;

        if (bLoop)
        {
            Travel = Geometry::WrapDistance(Travel, Length);
            if (Travel == 0.0) Travel = Length;
        }
        if (!FMath::IsFinite(Travel) || Travel <= UE_SMALL_NUMBER)
            return RejectLeg(Index, TEXT("Check junction order and Reverse flag on this leg"), OutError);

        const FRailwaySection& Section = Plan.Sections.Add_GetRef(FRailwaySection{Leg, Interval.Entry, Travel});
        for (const FRailwayStation& Station : Leg.Track->Stations)
        {
            if (!AppendStop(Station, Section, Length, Base, Plan.Stops))
                return RejectLeg(Index, TEXT("Invalid station position or dwell time"), OutError);
        }
        for (auto It = Markers.CreateConstKeyIterator(Leg.Track.Get()); It; ++It)
        {
            if (!AppendStop(It.Value(), Section, Length, Base, Plan.Stops))
                return RejectLeg(Index, TEXT("Invalid station position or dwell time"), OutError);
        }
        Base += Travel;
    }

    SortStops(Plan);
    OutPlan = MoveTemp(Plan);
    return true;
}

bool Prepare(const UWorld* World, FRailwayPlan Plan, double StartDistance, FRailwayPreparedPlan& OutPrepared, FString& OutError)
{
    OutError.Reset();
    FVector PreviousExit = FVector::ZeroVector;
    for (int32 Index = 0; Index < Plan.Sections.Num(); ++Index)
    {
        if (const TCHAR* Error = CheckSection(Plan.Sections[Index], World, Index > 0, PreviousExit))
            return RejectLeg(Index, Error, OutError);
    }

    TArray<double> Ends;
    Geometry::BuildSectionEnds(Plan, Ends);
    if (Ends.IsEmpty() || !FMath::IsFinite(StartDistance) || StartDistance < 0 || StartDistance > Ends.Last())
    {
        OutError = TEXT("Empty plan or start outside the route");
        return false;
    }
    for (const FRailwayPlannedStop& Stop : Plan.Stops)
    {
        if (!FMath::IsFinite(Stop.Distance) || Stop.Distance < 0 || Stop.Distance > Ends.Last() ||
            !FMath::IsFinite(Stop.Station.DwellSeconds) || Stop.Station.DwellSeconds < 0)
        {
            OutError = TEXT("Invalid planned stop distance or dwell time");
            return false;
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
