#pragma once

#include "RailwayUtils.h"

class USplineComponent;

namespace Railway::Geometry
{
    // 闭合样条的首尾对应同一点，返回 [0, Length) 内的里程。调用方保证 Length > 0。
    double WrapDistance(double Distance, double Length);

    // 岔口、站点和锚点按同一规则换算弧长，避免接缝处舍入不同。
    double DistanceAtPosition(const USplineComponent& Spline, float Position);

    // TrackDistance 从样条首点起算（cm），采样时按开放轨道或闭环规范化。
    FTransform SampleTrack(const FRailwayLeg& Leg, double TrackDistance);

    // 无缓存时按段遍历，适用于尚未发车的计划。接缝属于下一段，终点属于最后一段。
    bool SamplePlan(const FRailwayPlan& Plan, double Distance, int32& SectionIndex, double& TrackDistance, FTransform& Pose);

    // 仅定位到轨道和局部里程，改线时无需额外求世界姿态。
    bool Locate(const FRailwayPlan& Plan, const TArray<double>& SectionEnds, double Distance,
        int32& SectionIndex, double& TrackDistance);

    // 已装载计划使用累计段尾里程做二分查找，服务端运动和客户端显示共用相同的接缝规则。
    bool SampleIndexedPlan(const FRailwayPlan& Plan, const TArray<double>& SectionEnds, double Distance,
        int32& SectionIndex, double& TrackDistance, FTransform& Pose);

    void BuildSectionEnds(const FRailwayPlan& Plan, TArray<double>& OutEnds);
}
