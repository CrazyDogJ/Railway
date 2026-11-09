#include "RailwayRouteGeometry.h"

#include "Algo/BinarySearch.h"
#include "Components/SplineComponent.h"
#include "RailwayTrack.h"

namespace Railway::Geometry
{
double WrapDistance(double Distance, double Length)
{
    const double Remainder = FMath::Fmod(Distance, Length);
    return Remainder < 0 ? Remainder + Length : Remainder;
}

double DistanceAtPosition(const USplineComponent& Spline, float Position)
{
    return Spline.GetSplineLength() * Position;
}

static double NormalizeDistance(const USplineComponent& Spline, double Distance)
{
    const double Length = Spline.GetSplineLength();
    return Spline.IsClosedLoop() && Length > 0
        ? WrapDistance(Distance, Length) : FMath::Clamp(Distance, 0.0, Length);
}

static FTransform DirectedPose(const FRailwayLeg& Leg, double TrackDistance)
{
    FTransform Pose = Leg.Track->Spline->GetTransformAtDistanceAlongSpline(TrackDistance, ESplineCoordinateSpace::World);
    if (Leg.bReverse) Pose.SetRotation(Pose.GetRotation() * FQuat(FVector::UpVector, PI));
    return Pose;
}

FTransform SampleTrack(const FRailwayLeg& Leg, double TrackDistance)
{
    return DirectedPose(Leg, NormalizeDistance(*Leg.Track->Spline, TrackDistance));
}

static bool LocateInSection(const FRailwayPlan& Plan, int32 Index, double Along, int32& SectionIndex, double& TrackDistance)
{
    const FRailwaySection& Section = Plan.Sections[Index];
    if (!IsValid(Section.Leg.Track) || !IsValid(Section.Leg.Track->Spline)) return false;
    const double Local = Section.EntryDistance + (Section.Leg.bReverse ? -Along : Along);
    TrackDistance = NormalizeDistance(*Section.Leg.Track->Spline, Local);
    SectionIndex = Index;
    return true;
}

bool SamplePlan(const FRailwayPlan& Plan, double Distance, int32& SectionIndex, double& TrackDistance, FTransform& Pose)
{
    SectionIndex = INDEX_NONE;
    TrackDistance = 0;
    Pose = FTransform::Identity;
    if (!FMath::IsFinite(Distance) || Distance < 0) return false;

    double Start = 0;
    for (int32 Index = 0; Index < Plan.Sections.Num(); ++Index)
    {
        const FRailwaySection& Section = Plan.Sections[Index];
        if (!FMath::IsFinite(Section.TravelDistance) || Section.TravelDistance <= 0) return false;
        const double End = Start + Section.TravelDistance;
        if (Distance < End || (Index == Plan.Sections.Num() - 1 && Distance == End))
        {
            if (!LocateInSection(Plan, Index, Distance - Start, SectionIndex, TrackDistance)) return false;
            Pose = DirectedPose(Section.Leg, TrackDistance);
            return true;
        }
        Start = End;
    }
    return false;
}

bool Locate(const FRailwayPlan& Plan, const TArray<double>& SectionEnds, double Distance, int32& SectionIndex, double& TrackDistance)
{
    SectionIndex = INDEX_NONE;
    TrackDistance = 0;
    if (SectionEnds.IsEmpty() || SectionEnds.Num() != Plan.Sections.Num() ||
        !FMath::IsFinite(Distance) || Distance < 0 || Distance > SectionEnds.Last()) return false;

    const int32 Index = FMath::Min(Algo::UpperBound(SectionEnds, Distance), SectionEnds.Num() - 1);
    const double Start = Index == 0 ? 0.0 : SectionEnds[Index - 1];
    return LocateInSection(Plan, Index, Distance - Start, SectionIndex, TrackDistance);
}

bool SampleIndexedPlan(const FRailwayPlan& Plan, const TArray<double>& SectionEnds, double Distance,
    int32& SectionIndex, double& TrackDistance, FTransform& Pose)
{
    if (!Locate(Plan, SectionEnds, Distance, SectionIndex, TrackDistance))
    {
        Pose = FTransform::Identity;
        return false;
    }
    Pose = DirectedPose(Plan.Sections[SectionIndex].Leg, TrackDistance);
    return true;
}

void BuildSectionEnds(const FRailwayPlan& Plan, TArray<double>& OutEnds)
{
    OutEnds.Reset(Plan.Sections.Num());
    double Total = 0;
    for (const FRailwaySection& Section : Plan.Sections)
    {
        Total += Section.TravelDistance;
        OutEnds.Add(Total);
    }
}
}
