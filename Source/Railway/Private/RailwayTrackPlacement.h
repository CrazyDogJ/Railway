#pragma once

#include "CoreMinimal.h"

class AActor;
class USplineComponent;

namespace Railway::Placement
{
    // 预览和锚点共用附着规则。只解除轨道父级，保留调用方设置的其他父级。
    void DetachFromTrack(AActor& Actor);
    void AttachToSpline(AActor& Actor, USplineComponent& Spline);

    // 只写入发生变化的位置、旋转，避免编辑器每帧触发变换更新，并保留 Actor 自身缩放。
    void ApplyPose(AActor& Actor, const FTransform& Pose, double PositionTolerance = .0001, double RotationTolerance = .000001);
}
