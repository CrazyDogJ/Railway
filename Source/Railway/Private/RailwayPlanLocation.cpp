#include "RailwayRouteGeometry.h"

#include "Components/SplineComponent.h"
#include "RailwayRoutePlanner.h"
#include "RailwayTrack.h"

namespace Railway::Geometry
{
namespace
{
// A* 按样条的 float 采样精度生成切点。端点比较沿用该精度，段内计算保留 double。
float SampleKey(const USplineComponent& Spline, double Local)
{
    const double Length = Spline.GetSplineLength();
    const bool bLoop = Spline.IsClosedLoop();
    const float Key = DistanceOnSpline(Spline, bLoop ? WrapDistance(Local, Length) : Local);
    return bLoop && Key == Length ? 0.f : Key;
}

void AddOccurrences(const FRailwaySection& Section, double Begin, double End, double Local, TArray<double>& Matches)
{
    const USplineComponent& Spline = *Section.Leg.Track->Spline;
    const double Sign = Section.Leg.bReverse ? -1.0 : 1.0;
    const float Key = SampleKey(Spline, Local);
    const bool bAtEntry = Key == SampleKey(Spline, Section.EntryDistance);
    const bool bAtExit = Key == SampleKey(Spline, Section.EntryDistance + Sign * Section.TravelDistance);
    if (bAtEntry) Matches.AddUnique(Begin);
    if (bAtExit) Matches.AddUnique(End);
    if (bAtEntry || bAtExit) return;

    double Along = Sign * (Local - Section.EntryDistance);
    if (Spline.IsClosedLoop()) Along = WrapDistance(Along, Spline.GetSplineLength());
    if (Along > 0 && Along < Section.TravelDistance) Matches.AddUnique(Begin + Along);
}

using FPorts = TArray<double, TInlineAllocator<3>>;

FPorts GetPorts(const ARailwayTrack& Track, const ARailwayJunction& Junction)
{
    FPorts Ports;
    if (Junction.HostTrack.Get() == &Track && FMath::IsFinite(Junction.TrackDistance))
        Ports.Add(DistanceOnSpline(*Track.Spline, Junction.TrackDistance));
    if (!Track.Spline->IsClosedLoop())
    {
        if (Track.StartJunction.Get() == &Junction) Ports.AddUnique(0.0);
        if (Track.EndJunction.Get() == &Junction) Ports.AddUnique(Track.Spline->GetSplineLength());
    }
    return Ports;
}

void AddJunctionOccurrences(const FRailwayTrackLocation& Location, const FRailwaySection& Section,
    double Begin, double End, TArray<double>& Matches)
{
    const ARailwayTrack& From = *Location.Track;
    const ARailwayTrack& To = *Section.Leg.Track;
    const float Key = SampleKey(*From.Spline, Location.TrackDistance);
    const FVector FromPoint = From.Spline->GetLocationAtDistanceAlongSpline(Key, ESplineCoordinateSpace::World);
    for (const ARailwayJunction* Joint : {From.StartJunction.Get(), From.EndJunction.Get(), To.StartJunction.Get(), To.EndJunction.Get()})
    {
        if (!IsValid(Joint) || FVector::Dist(FromPoint, Joint->GetActorLocation()) > Routing::JunctionToleranceCm) continue;
        if (!GetPorts(From, *Joint).ContainsByPredicate([&](double Port) { return SampleKey(*From.Spline, Port) == Key; })) continue;
        for (const double Port : GetPorts(To, *Joint))
        {
            const FVector Point = To.Spline->GetLocationAtDistanceAlongSpline(Port, ESplineCoordinateSpace::World);
            if (FVector::Dist(Point, Joint->GetActorLocation()) <= Routing::JunctionToleranceCm &&
                FVector::Dist(Point, FromPoint) <= Routing::JunctionToleranceCm)
                AddOccurrences(Section, Begin, End, Port, Matches);
        }
    }
}
}

bool FindPlanDistance(const FRailwayPlan& Plan, const TArray<double>& SectionEnds, const FRailwayTrackLocation& Location,
    int32 SectionHint, TOptional<double> SharedPrefixDistance, double& OutDistance, FString& Error)
{
    if (SectionHint != INDEX_NONE && !Plan.Sections.IsValidIndex(SectionHint))
    {
        Error = TEXT("Section Hint is outside the new plan");
        return false;
    }

    if (SharedPrefixDistance.IsSet())
    {
        // 反算后在端点附近可能发生 float 舍入。共同前缀内先正向采样，确认物理位置后可精确保留原圈次。
        int32 SectionIndex;
        double Local;
        const double Preferred = SharedPrefixDistance.GetValue();
        if (Locate(Plan, SectionEnds, Preferred, SectionIndex, Local) &&
            (SectionHint == INDEX_NONE || SectionIndex == SectionHint) &&
            Plan.Sections[SectionIndex].Leg.Track == Location.Track &&
            SampleKey(*Location.Track->Spline, Local) == SampleKey(*Location.Track->Spline, Location.TrackDistance))
        {
            OutDistance = Preferred;
            return true;
        }
    }

    const int32 First = SectionHint == INDEX_NONE ? 0 : SectionHint;
    const int32 PastLast = SectionHint == INDEX_NONE ? Plan.Sections.Num() : SectionHint + 1;
    TArray<double> Matches;
    for (int32 I = First; I < PastLast; ++I)
    {
        if (Plan.Sections[I].Leg.Track == Location.Track)
            AddOccurrences(Plan.Sections[I], I ? SectionEnds[I - 1] : 0.0, SectionEnds[I], Location.TrackDistance, Matches);
    }
    // 相交或平行的样条可能有相同世界坐标，只有同一已吸附岔口的端口才可跨轨匹配。
    if (Matches.IsEmpty())
    {
        for (int32 I = First; I < PastLast; ++I)
            if (Plan.Sections[I].Leg.Track != Location.Track)
                AddJunctionOccurrences(Location, Plan.Sections[I], I ? SectionEnds[I - 1] : 0.0, SectionEnds[I], Matches);
    }

    if (Matches.IsEmpty())
    {
        Error = FString::Printf(TEXT("New plan does not pass through %s at %.3f cm%s"),
            *Location.Track->GetName(), Location.TrackDistance, SectionHint == INDEX_NONE ? TEXT("") : TEXT(" in the hinted section"));
        return false;
    }
    // 明确从车位出发的路线优先使用起点；指定整圈区间时，其首尾重合点取入口。
    if (Matches.Num() == 1 || Matches.Contains(0.0) || SectionHint != INDEX_NONE)
    {
        OutDistance = Matches.Contains(0.0) ? 0.0 : Matches[0];
        return true;
    }
    Error = TEXT("New plan visits this track location more than once; specify its Section Hint");
    return false;
}
}
