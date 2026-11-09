#pragma once

#include "CoreMinimal.h"
#include "RailwayTrainSaveGame.h"
#include "RailwayTrain.generated.h"

class UStaticMeshComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRailwayArrival, FName, StationName);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FRailwayStateChanged, ERailwayState, PrevState, ERailwayState, NewRaywayState);

UCLASS(Blueprintable)
class RAILWAY_API ARailwayTrain : public AActor
{
    GENERATED_BODY()
public:
    ARailwayTrain();
#if WITH_EDITOR
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void OnConstruction(const FTransform& Transform) override;
    void RefreshPreview();
#endif
    virtual void BeginPlay() override;
    void SetState(ERailwayState InState);
    void UpdateVelocity(bool bReverse = false) const;
    virtual void Tick(float DeltaSeconds) override;

    virtual void GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const override;
    
    UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category="Railway")
    TArray<FRailwayLeg> Route;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway")
    TObjectPtr<UStaticMeshComponent> Body;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    bool bAutoStart = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion", meta=(ClampMin="0", Units="cm/s"))
    float CruiseSpeed = 800.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion", meta=(ClampMin="1"))
    float Acceleration = 180.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion", meta=(ClampMin="1"))
    float Braking = 240.f;

    // Seconds for the cruise speed error to decay by about 63% below the acceleration limit.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion", meta=(ClampMin="0.01", Units="s"))
    float ResponseTime = 0.45f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Replicated, Category="Railway|Driving")
    ERailwayDriveMode DriveMode = ERailwayDriveMode::Automatic;

    // Signed traction / brake lever. Zero coasts. Submit on input changes from the owning player.
    UFUNCTION(BlueprintCallable, Server, Reliable, Category="Railway|Driving")
    void ServerSetDriveInput(float Value);

    UFUNCTION(BlueprintCallable, Server, Reliable, Category="Railway|Driving")
    void ServerSetDriveMode(ERailwayDriveMode NewMode);

    UPROPERTY(Transient, BlueprintReadOnly, Replicated, Category="Railway|Driving")
    float DriveInput = 0.f;

    // Index selects an occurrence in Plan.Stops, including repeated names and laps.
    UFUNCTION(BlueprintCallable, Category="Railway|Route")
    static bool MakeDirectPlan(const FRailwayPlan& Plan, int32 DestinationStopIndex, FRailwayPlan& OutPlan);

    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Railway|Route")
    bool GoDirectToStop(int32 DestinationStopIndex, FString& OutError);

    // Supply the current directed track first, then the desired connected tracks.
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Railway|Route")
    bool ChangeRemainingRoute(const TArray<FRailwayLeg>& NewLegs, FString& OutError);

    // Do not modify this, use SetState instead.
    UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category="Railway")
    ERailwayState State = ERailwayState::Idle;

    UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category="Railway")
    float Speed = 0.f;

    UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category="Railway")
    FName CurrentStation;

    // Usually for door close event to make sure pre leaving the station. The door is fully closed.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway", meta=(ClampMin="0"))
    float PreStationLeaveTime = 2.0f;
    
    UPROPERTY(BlueprintAssignable, Category="Railway")
    FRailwayArrival OnStationArrival;
    
    UPROPERTY(BlueprintAssignable, Category="Railway")
    FRailwayArrival PreStationLeave;
    
    UPROPERTY(BlueprintAssignable, Category="Railway")
    FRailwayArrival OnStationLeave;
    
    UPROPERTY(BlueprintAssignable, Category="Railway")
    FRailwayStateChanged OnStateChanged;

    UFUNCTION(BlueprintCallable, Category="Railway", BlueprintAuthorityOnly)
    bool StartRoute();
    
    UFUNCTION(BlueprintPure, Category="Railway")
    void GetNextStopInfo(double& OutDistanceRemain, FRailwayStation& OutStationInfo) const;

    // Builds a snapshot of the current geometry without resetting this train.
    UFUNCTION(BlueprintCallable, Category="Railway|Route")
    bool BuildRoutePlan(const TArray<FRailwayLeg>& Legs, FRailwayPlan& OutPlan, FString& OutError) const;

    // A rejected replacement leaves the current run intact. Exact-start stops remain pending.
    UFUNCTION(BlueprintCallable, Category="Railway|Route", BlueprintAuthorityOnly)
    bool StartPreparedRoute(const FRailwayPlan& Plan, double StartDistance, FString& OutError);

    // Samples the active plan. TrackDistance uses the spline's native direction.
    UFUNCTION(BlueprintPure, Category="Railway|Route")
    bool SampleRoute(double RouteDistance, int32& SectionIndex, double& TrackDistance, FTransform& Pose) const;

    // Samples a plan before it is started (for editor previews and planning).
    UFUNCTION(BlueprintPure, Category="Railway|Route")
    static bool SamplePlan(const FRailwayPlan& Plan, double RouteDistance, int32& SectionIndex, double& TrackDistance, FTransform& Pose);

    UFUNCTION(BlueprintPure, Category="Railway|Route")
    double GetRouteLength() const { return Ends.IsEmpty() ? 0.0 : Ends.Last(); }

    UFUNCTION(BlueprintPure, Category="Railway|Route")
    bool HasSentPreLeave() const { return bPreLeaveSent; }

    // Override in a train Blueprint to choose the editor preview track, distance and direction.
    UFUNCTION(BlueprintNativeEvent, Category="Railway|Editor")
    bool GetPreviewLocation(FRailwayLeg& PreviewLeg, double& TrackDistance) const;

    UFUNCTION(BlueprintCallable, Category="Railway|Save Game", BlueprintAuthorityOnly)
    FRailwayTrainSaveGame GetSaveGameData() const;
    
    UFUNCTION(BlueprintCallable, Category="Railway|Save Game", BlueprintAuthorityOnly)
    void LoadSaveGameData(FRailwayTrainSaveGame InSaveGame);
    
    UPROPERTY(Transient, BlueprintReadOnly, Category="Railway|Route", ReplicatedUsing=OnRep_ActivePlan)
    FRailwayPlan ActivePlan;

    UPROPERTY(Transient, BlueprintReadOnly, Category="Railway|Route")
    double Distance = 0;

    UPROPERTY(Transient, BlueprintReadOnly, Category="Railway|Route")
    double Wait = 0;

    UPROPERTY(Transient, BlueprintReadOnly, Category="Railway|Route")
    int32 Leg = 0;

    UPROPERTY(Transient, BlueprintReadOnly, Category="Railway|Route")
    int32 NextStop = 0;

    // Optional external speed ceiling in cm/s. Negative disables the limit.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion")
    float ExternalSpeedLimit = -1.f;

    // Server sample frequency and client playback delay, in seconds for the latter.
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Railway|Networking", meta=(ClampMin="1", ClampMax="60"))
    float SnapshotRateHz = 15.f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Railway|Networking", meta=(ClampMin="0", ClampMax="0.5", Units="s"))
    float InterpolationDelay = 0.15f;

    UPROPERTY(Transient, BlueprintReadOnly, ReplicatedUsing=OnRep_TrainMoveState, Category="Railway|Networking")
    FRailwayMoveState TrainMoveState;

private:
    friend struct FRailwayTrainSaveGame;

    UFUNCTION()
    void OnRep_TrainMoveState();

    UFUNCTION()
    void OnRep_ActivePlan();

    void PublishMoveState(bool bForce);
    void TickClientPresentation();
    void ResetPlan();
    bool BuildPlanFrom(const TArray<FRailwayLeg>& Legs, TOptional<double> FirstEntry, FRailwayPlan& OutPlan, FString& OutError) const;
    bool ApplyReplacementPlan(FRailwayPlan& NewPlan, double StartDistance, FString& OutError);
    bool BeginPlan(const FRailwayPlan& Plan, double StartDistance, const FRailwayTrainSaveGame* Saved, FString& OutError, bool bKeepDriveInput = false);
    TArray<FRailwayMoveState> MoveSamples;
    double LastRenderTime = -1.0;
    TArray<double> Ends;
    uint32 RunRevision = 0;
    bool bPreLeaveSent = false;
    bool FailRoute(int32 Index, const TCHAR* Reason);
    bool UpdatePose();
    void Step(double Dt);
};
