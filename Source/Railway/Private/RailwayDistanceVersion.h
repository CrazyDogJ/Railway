#pragma once

#include "CoreMinimal.h"

namespace Railway::DistanceVersion
{
    // 标记含有厘米字段的资产。旧关卡可能省略正好等于类默认值的 Alpha。
    void SerializePosition(FArchive& Ar, float& LegacyAlpha, float OldDefault);

    // 两个 float 先提升到 double，迁移后的厘米值能还原 V1 连接签名使用的原始 Alpha。
    inline double FromLegacyAlpha(float Alpha, float SplineLength)
    {
        return static_cast<double>(Alpha) * SplineLength;
    }
}
