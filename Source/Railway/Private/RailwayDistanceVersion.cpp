#include "RailwayDistanceVersion.h"

#include "RailwayUtils.h"
#include "Serialization/Archive.h"
#include "Serialization/CustomVersion.h"

namespace Railway::DistanceVersion
{
static const FGuid VersionId(0xDB32F8A7, 0x8957475C, 0xAD589FA4, 0x12093CF2);
static const FCustomVersionRegistration Registration(VersionId, 1, TEXT("RailwayTrackDistance"));

void SerializePosition(FArchive& Ar, float& LegacyAlpha, float OldDefault)
{
    Ar.UsingCustomVersion(VersionId);
    if (Ar.IsLoading() && Ar.IsPersistent() && !Ar.IsTransacting() &&
        Ar.CustomVer(VersionId) < 1 && LegacyAlpha == -1.f)
        LegacyAlpha = OldDefault;
}
}

bool FRailwayStation::Serialize(FArchive& Ar)
{
    Railway::DistanceVersion::SerializePosition(Ar, Position, .5f);
    return false; // 继续走引擎按属性名读写的路径。
}

bool FRailwayPlannedStop::Serialize(FArchive& Ar)
{
    // 旧站次可能连整个默认 Station 都没有写入，不能只在子结构反序列化时补默认值。
    Railway::DistanceVersion::SerializePosition(Ar, Station.Position, .5f);
    return false;
}

void FRailwayStation::UpgradeTrackDistance(float SplineLength)
{
    if (Position == -1.f) return;
    TrackDistance = FMath::IsFinite(Position) && Position >= 0.f && Position <= 1.f
        ? Railway::DistanceVersion::FromLegacyAlpha(Position, SplineLength) : -1.0;
    Position = -1.f;
}
