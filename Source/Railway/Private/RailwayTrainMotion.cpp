#include "RailwayTrain.h"

void ARailwayTrain::ServerSetDriveMode_Implementation(ERailwayDriveMode NewMode)
{
    if (NewMode != ERailwayDriveMode::Manual && NewMode != ERailwayDriveMode::Automatic) return;
    if (NewMode == DriveMode) return;
    DriveInput = 0.f;
    DriveMode = NewMode;
    ForceNetUpdate();
}

void ARailwayTrain::ServerSetDriveInput_Implementation(float Value)
{
    if (DriveMode == ERailwayDriveMode::Manual && FMath::IsFinite(Value))
        DriveInput = FMath::Clamp(Value, -1.f, 1.f);
}

double ARailwayTrain::CalculateDriverTargetSpeed(double SpeedLimit, double DeltaSeconds) const
{
    if (DriveMode == ERailwayDriveMode::Manual)
    {
        const double Rate = FMath::Max(1.f, DriveInput >= 0.f ? Acceleration : Braking);
        return FMath::Clamp(Speed + DriveInput * Rate * DeltaSeconds, 0.0, SpeedLimit);
    }
    const double Response = 1.0 - FMath::Exp(-DeltaSeconds / FMath::Max(.01f, ResponseTime));
    return Speed + (SpeedLimit - Speed) * Response;
}

bool ARailwayTrain::AdvanceDwell(double& DeltaSeconds)
{
    const double Consumed = FMath::Min(Wait, DeltaSeconds);
    Wait -= Consumed;
    DeltaSeconds -= Consumed;

    if (ActivePlan.bFinishAtLastStop && NextStop == ActivePlan.Stops.Num())
    {
        if (Wait <= 0)
        {
            CurrentStation = NAME_None;
            SetState(ERailwayState::Finished);
        }
        return false;
    }

    const uint32 Revision = ActivePlan.NetworkRevision;
    if (!bPreLeaveSent && Wait <= FMath::Max(0.f, PreStationLeaveTime))
    {
        bPreLeaveSent = true;
        PreStationLeave.Broadcast(CurrentStation);
        if (ActivePlan.NetworkRevision != Revision) return false;
    }
    if (Wait > 0) return false;

    // 手动驾驶需给正牵引才从中间站发车。几何终点无需再次给牵引。
    const bool bAtTrackEnd = NextStop == ActivePlan.Stops.Num() && Distance == GetRouteLength();
    if (!bAtTrackEnd && DriveMode == ERailwayDriveMode::Manual && DriveInput <= 0.f) return false;

    OnStationLeave.Broadcast(CurrentStation);
    if (ActivePlan.NetworkRevision != Revision) return false;
    CurrentStation = NAME_None;
    SetState(ERailwayState::Running);
    return ActivePlan.NetworkRevision == Revision;
}

void ARailwayTrain::ArriveAtTarget(uint32 ExpectedRevision)
{
    if (!ActivePlan.Stops.IsValidIndex(NextStop))
    {
        SetState(ERailwayState::Finished);
        return;
    }
    const FRailwayStation& Station = ActivePlan.Stops[NextStop++].Station;
    DriveInput = 0.f;
    Wait = Station.DwellSeconds;
    bPreLeaveSent = false;
    CurrentStation = Station.Name;
    SetState(ERailwayState::Dwelling);
    if (ActivePlan.NetworkRevision == ExpectedRevision) OnStationArrival.Broadcast(CurrentStation);
}

void ARailwayTrain::Step(double DeltaSeconds)
{
    if (State == ERailwayState::Dwelling && !AdvanceDwell(DeltaSeconds)) return;

    const uint32 Revision = ActivePlan.NetworkRevision;
    const double Target = ActivePlan.Stops.IsValidIndex(NextStop) ? ActivePlan.Stops[NextStop].Distance :
        (ActivePlan.bFinishAtLastStop ? ActivePlan.Stops.Last().Distance : GetRouteLength());
    const double Remaining = Target - Distance;
    const double Brake = FMath::Max(1.f, Braking);
    const double BrakeStep = Brake * DeltaSeconds;
    // 在连续制动距离之外预留下一个模拟步的行程，跨岔口时也能对准目标站。
    const double SafeSpeed = FMath::Sqrt(BrakeStep * BrakeStep + 2 * Brake * Remaining) - BrakeStep;
    const double SpeedLimit = FMath::Max(0.f, ExternalSpeedLimit >= 0 ? FMath::Min(ExternalSpeedLimit, CruiseSpeed) : CruiseSpeed);
    const double DriverTarget = CalculateDriverTargetSpeed(SpeedLimit, DeltaSeconds);
    const double Desired = FMath::Clamp(DriverTarget, 0.0, SafeSpeed);
    Speed += FMath::Clamp(Desired - Speed, -BrakeStep, FMath::Max(1.f, Acceleration) * DeltaSeconds);
    Distance += FMath::Min(Remaining, Speed * DeltaSeconds);
    const bool bArrived = Target - Distance <= .01;
    if (bArrived)
    {
        Distance = Target;
        Speed = 0;
    }

    if (UpdatePose() && bArrived && ActivePlan.NetworkRevision == Revision)
        ArriveAtTarget(Revision);
}
