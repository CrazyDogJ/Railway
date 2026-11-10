#include "RailwayTrain.h"

#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"
#include "RailwayRouteGeometry.h"
#include "RailwayTrackPlacement.h"

void ARailwayTrain::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ThisClass, ActivePlan);
    DOREPLIFETIME(ThisClass, TrainMoveState);
    DOREPLIFETIME(ThisClass, DriveMode);
    DOREPLIFETIME(ThisClass, DriveInput);
    DOREPLIFETIME(ThisClass, BrakeInput);
}

void ARailwayTrain::PublishMoveState(bool bForce)
{
    if (!HasAuthority() || GetNetMode() == NM_Standalone) return;
    const double Now = GetWorld()->GetTimeSeconds();
    const double DwellEnd = State == ERailwayState::Dwelling ? Now + Wait : 0;
    const bool bDwellEdited = State == ERailwayState::Dwelling &&
        (Wait > 0 || TrainMoveState.DwellEndTime > Now) && !FMath::IsNearlyEqual(DwellEnd, TrainMoveState.DwellEndTime, .02);
    const bool bTransition = TrainMoveState.PlanRevision != ActivePlan.NetworkRevision ||
        TrainMoveState.State != State || TrainMoveState.NextStop != NextStop ||
        TrainMoveState.StationName != CurrentStation || bDwellEdited;

    // 巡航按配置的频率发送；换线和停站游标变化立即发送。
    if (!bForce && !bTransition &&
        (State != ERailwayState::Running || Now - TrainMoveState.ServerTime < 1.0 / FMath::Clamp(SnapshotRateHz, 1.f, 60.f)))
        return;

    TrainMoveState.PlanRevision = ActivePlan.NetworkRevision;
    TrainMoveState.ServerTime = Now;
    TrainMoveState.Distance = Distance;
    TrainMoveState.Speed = Speed;
    TrainMoveState.State = State;
    TrainMoveState.NextStop = NextStop;
    TrainMoveState.DwellEndTime = DwellEnd;
    TrainMoveState.StationName = CurrentStation;
    if (bForce || bTransition) ForceNetUpdate();
}

void ARailwayTrain::OnRep_ActivePlan()
{
    Railway::Geometry::BuildSectionEnds(ActivePlan, SectionEnds);
}

void ARailwayTrain::OnRep_TrainMoveState()
{
    MoveBuffer.Push(TrainMoveState);
}

void ARailwayTrain::TickClientPresentation()
{
    const AGameStateBase* Clock = GetWorld()->GetGameState();
    if (!Clock) return;

    const double RequestedTime = Clock->GetServerWorldTimeSeconds() - FMath::Clamp(InterpolationDelay, 0.f, .5f);
    FRailwayMoveState Sample;
    // 两个 RepNotify 的顺序没有保证，在显示时按版本配对。
    if (!MoveBuffer.Sample(ActivePlan.NetworkRevision, RequestedTime, Sample)) return;

    int32 SectionIndex = 0;
    if (!ActivePlan.Sections.IsEmpty())
    {
        FTransform Pose;
        double TrackDistance;
        if (!SampleRoute(Sample.Distance, SectionIndex, TrackDistance, Pose)) return;
        Railway::Placement::ApplyPose(*this, Pose);
    }
    else if (Sample.State != ERailwayState::Idle && Sample.State != ERailwayState::InvalidRoute)
    {
        return;
    }

    MoveBuffer.MarkPresented(Sample.ServerTime);
    PresentedPlanRevision = Sample.PlanRevision;
    Leg = SectionIndex;
    Distance = Sample.Distance;
    Speed = Sample.Speed;
    NextStop = Sample.NextStop;
    const bool bDwelling = Sample.State == ERailwayState::Dwelling;
    Wait = bDwelling ? FMath::Max(0.0, Sample.DwellEndTime - FMath::Max(Sample.ServerTime, RequestedTime)) : 0;
    CurrentStation = bDwelling ? Sample.StationName : NAME_None;
    UpdateVelocity();
    SetState(Sample.State);
}
