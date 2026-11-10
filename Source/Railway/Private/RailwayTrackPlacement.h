#pragma once

#include "CoreMinimal.h"

class AActor;
namespace Railway::Placement
{
    void ApplyPose(AActor& Actor, const FTransform& Pose, double PositionTolerance = .0001, double RotationTolerance = .000001);
}
