#include "RailwayMoveBuffer.h"

void FRailwayMoveBuffer::Push(const FRailwayMoveState& Sample)
{
    if (Samples.IsEmpty() || Samples.Last().PlanRevision != Sample.PlanRevision)
    {
        Samples.Reset();
        LastPresentedTime = -1.0;
    }
    else if (Sample.ServerTime <= Samples.Last().ServerTime)
    {
        if (Sample.ServerTime == Samples.Last().ServerTime) Samples.Last() = Sample;
        return;
    }

    // GameState 或轨道尚未复制时，历史仍有固定上限。
    constexpr int32 MaxSamples = 64;
    if (Samples.Num() == MaxSamples) Samples.RemoveAt(0, 1, EAllowShrinking::No);
    Samples.Add(Sample);
}

bool FRailwayMoveBuffer::Sample(uint32 PlanRevision, double RequestedTime, FRailwayMoveState& OutSample)
{
    if (Samples.IsEmpty() || Samples.Last().PlanRevision != PlanRevision) return false;

    const double Time = FMath::Clamp(FMath::Max(LastPresentedTime, RequestedTime),
        Samples[0].ServerTime, Samples.Last().ServerTime);
    while (Samples.Num() > 1 && Samples[1].ServerTime <= Time)
        Samples.RemoveAt(0, 1, EAllowShrinking::No);

    const FRailwayMoveState& From = Samples[0];
    OutSample = From;
    OutSample.ServerTime = Time;
    if (Samples.Num() > 1)
    {
        const FRailwayMoveState& To = Samples[1];
        // 即使丢了离站样本，停车倒计时结束之前也应保持到站位置。
        const double Start = From.State == ERailwayState::Dwelling
            ? FMath::Clamp(From.DwellEndTime, From.ServerTime, To.ServerTime) : From.ServerTime;
        const double Alpha = Start < To.ServerTime ? FMath::Clamp((Time - Start) / (To.ServerTime - Start), 0.0, 1.0) : 0;
        OutSample.Distance = FMath::Lerp(From.Distance, To.Distance, Alpha);
        OutSample.Speed = FMath::Lerp(From.Speed, To.Speed, static_cast<float>(Alpha));
    }
    return true;
}
