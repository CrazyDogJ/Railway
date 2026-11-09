#pragma once

#include "RailwayUtils.h"

// 单个客户端的运动快照历史，只处理服务器时间和计划版本，不持有列车或场景引用。
class RAILWAY_API FRailwayMoveBuffer
{
public:
    void Push(const FRailwayMoveState& Sample);

    // 返回指定版本的显示样本。连续量在相邻样本间插值，停站游标和状态取播放时刻已到达的样本。
    bool Sample(uint32 PlanRevision, double RequestedTime, FRailwayMoveState& OutSample);

    // 轨道引用解析并应用姿态后再推进此时钟，以便流加载期间重试。
    void MarkPresented(double ServerTime) { LastPresentedTime = ServerTime; }

private:
    TArray<FRailwayMoveState> Samples;
    double LastPresentedTime = -1.0;
};
