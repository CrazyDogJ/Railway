#include "RailwayTrack.h"

#include "Components/SplineComponent.h"
#include "Misc/SecureHash.h"
#include "Net/UnrealNetwork.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/UnrealType.h"

namespace
{
// 仅序列化决定局部采样的值，避免把指针、结构体填充、世界原点或临时版本号写进持久签名。
FGuid HashCurve(const USplineComponent& Spline)
{
    TArray<uint8> Bytes;
    FMemoryWriter Writer(Bytes);
    int32 Count = Spline.GetNumberOfSplinePoints();
    bool bClosed = Spline.IsClosedLoop();
    float Length = Spline.GetSplineLength();
    FVector Up = Spline.GetDefaultUpVector(ESplineCoordinateSpace::Local);
    Writer << Count << bClosed << Length << Up;
    for (int32 Index = 0; Index < Count; ++Index)
    {
        FSplinePoint Point = Spline.GetSplinePointAt(Index, ESplineCoordinateSpace::Local);
        uint8 Type = Point.Type.GetValue();
        Writer << Point.InputKey << Point.Position << Point.ArriveTangent << Point.LeaveTangent
            << Point.Rotation << Point.Scale << Type;
    }
    // 里程采样还依赖重参数化表和闭环最后一个输入键。
    const FSplineCurves Curves = Spline.GetSplineCurves();
    for (const auto& Key : Curves.ReparamTable.Points)
    {
        float Distance = Key.InVal;
        float Input = Key.OutVal;
        Writer << Distance << Input;
    }
    FMD5 Digest;
    Digest.Update(Bytes.GetData(), Bytes.Num());
    FMD5Hash Hash;
    Hash.Set(Digest);
    return MD5HashToGuid(Hash);
}

FGuid JunctionSignature(const ARailwayJunction* Junction, const USplineComponent& Spline, int32 Point, bool bLegacyAlpha)
{
    if (!IsValid(Junction)) return FGuid();
    // 整体平移线路保持有效，只改变岔口或一侧端点会改变连接的吸附状态。
    const FVector End = Spline.GetLocationAtSplinePoint(Point, ESplineCoordinateSpace::World);
    const bool bSnapped = FVector::DistSquared(End, Junction->GetActorLocation()) <= 1.0;
    const FGuid Joint = FGuid::Combine(Junction->RailwayId, FGuid(0, 0, 0, bSnapped ? 1 : 0));
    if (!IsValid(Junction->HostTrack)) return Joint;
    const FGuid HostJoint = FGuid::Combine(Joint, Junction->HostTrack->RailwayId);
    if (bLegacyAlpha)
    {
        const USplineComponent* HostSpline = Junction->HostTrack->Spline;
        if (!IsValid(HostSpline) || HostSpline->GetSplineLength() <= 0) return FGuid();
        const float Alpha = static_cast<float>(Junction->TrackDistance / HostSpline->GetSplineLength());
        uint32 Bits;
        FMemory::Memcpy(&Bits, &Alpha, sizeof(Bits));
        return FGuid::Combine(HostJoint, FGuid(0, 0, 0, Bits));
    }
    uint64 Bits;
    FMemory::Memcpy(&Bits, &Junction->TrackDistance, sizeof(Bits));
    return FGuid::Combine(HostJoint, FGuid(0, 2, static_cast<uint32>(Bits >> 32), static_cast<uint32>(Bits)));
}
}

FGuid ARailwayTrack::GetLayoutSignature() const
{
    return CalculateLayoutSignature(false);
}

FGuid ARailwayTrack::GetLegacyLayoutSignature() const
{
    return CalculateLayoutSignature(true);
}

FGuid ARailwayTrack::CalculateLayoutSignature(bool bLegacyAlpha) const
{
    if (!IsValid(Spline)) return FGuid();
    const int32 Version = Spline->GetVersion();
    if (CachedSpline.Get() != Spline.Get() || CachedSplineVersion != Version || !CachedCurveSignature.IsValid())
    {
        CachedCurveSignature = HashCurve(*Spline);
        CachedSpline = Spline.Get();
        CachedSplineVersion = Version;
    }
    if (Spline->IsClosedLoop()) return CachedCurveSignature;
    return FGuid::Combine(FGuid::Combine(CachedCurveSignature, JunctionSignature(StartJunction, *Spline, 0, bLegacyAlpha)),
        JunctionSignature(EndJunction, *Spline, Spline->GetNumberOfSplinePoints() - 1, bLegacyAlpha));
}

void ARailwayTrack::PostLoad()
{
    Super::PostLoad();
    if (IsTemplate()) return;
    if (IsValid(Spline)) Spline->ConditionalPostLoad();
    UpgradeStationDistances();
    // 只迁移没有身份的旧关卡行。首次升级后保存地图，之后数组重排不会改变身份。
    for (int32 Index = 0; Index < Stations.Num(); ++Index)
    {
        if (!Stations[Index].StationId.IsValid())
            Stations[Index].StationId = FGuid::NewDeterministicGuid(RailwayId.ToString(), static_cast<uint64>(Index) + 1);
    }
}

void ARailwayTrack::UpgradeStationDistances()
{
    if (IsTemplate() || !IsValid(Spline)) return;
    const float Length = Spline->GetSplineLength();
    if (Length <= 0) return;
    for (FRailwayStation& Station : Stations) Station.UpgradeTrackDistance(Length);
}

void ARailwayTrack::InitializeStationIds()
{
    UpgradeStationDistances();
    if (IsTemplate() || !HasAuthority()) return;
    for (FRailwayStation& Station : Stations)
        if (!Station.StationId.IsValid()) Station.StationId = FGuid::NewGuid();
}

void ARailwayTrack::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    InitializeStationIds();
}

#if WITH_EDITOR
void ARailwayTrack::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
    if (Event.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(ARailwayTrack, Stations))
    {
        // Details 面板将复制出的新行插在原行之前，只有新行需要重新分配 ID。
        const int32 Index = Event.GetArrayIndex(TEXT("Stations"));
        if (Event.GetPropertyName() == GET_MEMBER_NAME_CHECKED(FRailwayStation, TrackDistance) && Stations.IsValidIndex(Index))
            Stations[Index].Position = -1.f;
        if (Event.ChangeType == EPropertyChangeType::Duplicate && Stations.IsValidIndex(Index))
            Stations[Index].StationId = FGuid::NewGuid();
        InitializeStationIds();
    }
    Super::PostEditChangeProperty(Event);
}
#endif

void ARailwayTrack::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ThisClass, Stations);
}

void ARailwayTrackAnchor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ThisClass, HostTrack);
    DOREPLIFETIME(ThisClass, TrackDistance);
}

void ARailwayStationMarker::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ThisClass, StationName);
    DOREPLIFETIME(ThisClass, DwellSeconds);
    DOREPLIFETIME(ThisClass, CustomData);
}
