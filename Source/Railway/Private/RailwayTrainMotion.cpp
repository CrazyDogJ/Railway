#include "RailwayTrain.h"

#include "RailwayDataActor.h"
#include "RailwayWorldSubsystem.h"

double ARailwayTrain::LimitTrafficTravel(int32 CabDirection, double BeforeSpeed, double Dt, bool bTrapezoidal)
{
    if (BeforeSpeed == 0 && Speed == 0) return 0;
    if (auto* Registry = GetRailwaySubsystem()) Registry->PrepareMovement(*this, CabDirection);
    double Magnitude = FMath::Abs(Speed);
    if (TrafficRemaining != DBL_MAX)
    {
        const double Brake = FMath::Max(1.f, Braking);
        const double BrakeStep = Brake * Dt;
        const double Safe = FMath::Sqrt(BrakeStep * BrakeStep + 2 * Brake * TrafficRemaining) - BrakeStep;
        Magnitude = FMath::Min(Magnitude, FMath::Max(Safe, BeforeSpeed - BrakeStep));
        Speed = CabDirection * Magnitude;
    }
    const double Travel = (bTrapezoidal ? .5 * (BeforeSpeed + Magnitude) : Magnitude) * Dt;
    if (TrafficRemaining == DBL_MAX) return Travel;
    const double Granted = FMath::Min(Travel, TrafficRemaining);
    TrafficRemaining -= Granted;
    if (TrafficRemaining <= UE_KINDA_SMALL_NUMBER)
    {
        Speed = 0.f;
        TrafficRemaining = 0;
    }
    return Granted;
}

void ARailwayTrain::ServerSetDriveMode_Implementation(ERailwayDriveMode NewMode)
{
    if (NewMode != ERailwayDriveMode::Manual && NewMode != ERailwayDriveMode::Automatic) return;
    if (NewMode == DriveMode) return;
    ClearDriverInputs();
    DriveMode = NewMode;
    InvalidateTraffic();
    ForceNetUpdate();
}

void ARailwayTrain::ServerSetDriveInput_Implementation(float Value)
{
    if (DriveMode != ERailwayDriveMode::Manual || !FMath::IsFinite(Value)) return;
    if (!Speed && FMath::Sign(DriveInput) != FMath::Sign(Value)) InvalidateTraffic();
    DriveInput = FMath::Clamp(Value, -1.f, 1.f);
}

void ARailwayTrain::ServerSetBrakeInput_Implementation(float Value)
{
    if (DriveMode != ERailwayDriveMode::Manual || !FMath::IsFinite(Value)) return;
    if (!Speed && (BrakeInput > 0) != (Value > 0)) InvalidateTraffic();
    BrakeInput = FMath::Clamp(Value, 0.f, 1.f);
}

double ARailwayTrain::CalculateDriverTargetSpeed(double SpeedLimit, double DeltaSeconds) const
{
    if (DriveMode == ERailwayDriveMode::Manual)
    {
        if (DriveInput == 0)
            return FMath::Sign(Speed) * FMath::Max(0.0, FMath::Abs(Speed) - FMath::Max(0.f, RollingResistance) * DeltaSeconds);
        const double Traction = DriveInput * FMath::Max(1.f, Acceleration);
        return FMath::Clamp(Speed + Traction * DeltaSeconds, -SpeedLimit, SpeedLimit);
    }
    const double CabSign = ActivePlan.bCabReversed ? -1.0 : 1.0;
    const double PathSpeed = CabSign * Speed;
    if (PathSpeed < 0) return 0.0;
    const double Response = 1.0 - FMath::Exp(-DeltaSeconds / FMath::Max(.01f, ResponseTime));
    return CabSign * (PathSpeed + (SpeedLimit - PathSpeed) * Response);
}

bool ARailwayTrain::RefreshDwell()
{
    if (!ActivePlan.Sections.IsValidIndex(Leg)) return FailRoute(TEXT("Missing occupied section"));
    const ARailwayDataActor* Data = GetRailwayData();
    if (!Data || Data->Network.BuildId != ActivePlan.BuildId) return FailRoute(TEXT("Missing active railway network"));
    FRailwayStation Station;
    if (!ActivePlan.Stops.IsValidIndex(NextStop - 1) ||
        !ResolvePlannedStop(ActivePlan.Stops[NextStop - 1], Station))
        return FailRoute(TEXT("Occupied station was removed, moved or became invalid"));
    CurrentStation = Station.Name;
    Wait = FMath::Max(0.0, Station.DwellSeconds - ElapsedDwell);
    return true;
}

bool ARailwayTrain::AdvanceDwell(double& DeltaSeconds)
{
    const double Consumed = FMath::Min(Wait, DeltaSeconds);
    ElapsedDwell += Consumed;
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

    // 中间站停满后，松开制动即可向任一方向发车。几何终点仍可正常结束行程。
    const bool bAtTrackEnd = NextStop == ActivePlan.Stops.Num() && Distance == GetRouteLength();
    if (!bAtTrackEnd && DriveMode == ERailwayDriveMode::Manual && (DriveInput == 0.f || BrakeInput > 0.f))
    {
        ElapsedDwell += DeltaSeconds;
        return false;
    }

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
    FRailwayStation Station;
    if (!ResolvePlannedStop(ActivePlan.Stops[NextStop], Station))
    {
        FailRoute(TEXT("Cannot arrive at a removed, moved or invalid station"));
        return;
    }
    ActivePlan.Stops[NextStop].bServed = true;
    ++NextStop;
    ElapsedDwell = 0;
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
    if (DeltaSeconds <= 0.0) return;

    const uint32 Revision = ActivePlan.NetworkRevision;
    const double Brake = FMath::Max(1.f, Braking);
    const double BrakeStep = Brake * DeltaSeconds;
    const double SpeedLimit = FMath::Max(0.f, ExternalSpeedLimit >= 0.f ? FMath::Min(ExternalSpeedLimit, CruiseSpeed) : CruiseSpeed);
    const double CabSign = ActivePlan.bCabReversed ? -1.0 : 1.0;
    const double PathSpeed = CabSign * Speed;
    const double Magnitude = FMath::Abs(Speed);
    const bool bManual = DriveMode == ERailwayDriveMode::Manual;
    double RequestedSpeed;
    if (bManual && BrakeInput > 0.f)
    {
        // 踩下制动时切断牵引，只减小速度绝对值；即使持续踩住也不会越过零速。
        const double AfterBraking = FMath::Max(0.0, Magnitude - BrakeInput * BrakeStep);
        RequestedSpeed = FMath::Sign(PathSpeed) * FMath::Min(AfterBraking, SpeedLimit);
    }
    else
    {
        RequestedSpeed = CabSign * CalculateDriverTargetSpeed(SpeedLimit, DeltaSeconds);
        if (!bManual) RequestedSpeed = FMath::Max(0.0, RequestedSpeed);
    }

    // Integrate in route coordinates; traction, published speed and the cab remain in body coordinates.
    const bool bReverseFromRest = RequestedSpeed < 0.0 || (RequestedSpeed == 0.0 && bManual && CabSign * DriveInput < 0.f);
    const bool bBacking = GetRouteLength() > 0.0 && (PathSpeed < 0.0 || (PathSpeed == 0.0 && bReverseFromRest));
    const double Direction = bBacking ? -1.0 : 1.0;
    double Target = 0.0;
    if (!bBacking)
    {
        if (ActivePlan.Stops.IsValidIndex(NextStop)) Target = ActivePlan.Stops[NextStop].Distance;
        else Target = ActivePlan.bFinishAtLastStop ? ActivePlan.Stops.Last().Distance : GetRouteLength();
    }
    const double Remaining = FMath::Max(0.0, Direction * (Target - Distance));
    const double SafeSpeed = FMath::Sqrt(BrakeStep * BrakeStep + 2.0 * Brake * Remaining) - BrakeStep;
    const double DesiredMagnitude = FMath::Clamp(Direction * RequestedSpeed, 0.0, SafeSpeed);
    const double SpeedChange = FMath::Clamp(DesiredMagnitude - Magnitude, -BrakeStep, FMath::Max(1.f, Acceleration) * DeltaSeconds);
    Speed = static_cast<float>(CabSign * Direction * (Magnitude + SpeedChange));
    const double Travel = FMath::Min(Remaining, LimitTrafficTravel(int32(CabSign * Direction), Magnitude, DeltaSeconds, false));
    Distance += Direction * Travel;

    const bool bAtTarget = Travel >= Remaining || (Travel > 0.0 && Remaining - Travel <= .01);
    if (bAtTarget)
    {
        Distance = Target;
        Speed = 0.f;
    }
    // 倒车是沿既有行程退行。到达起点后保持可驾驶，已服务站次的游标保持原值。
    if (UpdatePose() && bAtTarget && !bBacking && ActivePlan.NetworkRevision == Revision)
        ArriveAtTarget(Revision);
}
