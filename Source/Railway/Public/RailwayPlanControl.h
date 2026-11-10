#pragma once

#include "Components/ActorComponent.h"
#include "RailwayRouteSettings.h"
#include "RailwayPlanControl.generated.h"

class ARailwayTrain;

UENUM(BlueprintType)
enum class ERailwayPlanAction : uint8 { ActivatePreset, CancelRoute, SkipStop, AddStop, DirectToStop, SetDestination, StartBetweenPoints, ActivateTrack, DeactivateTrack };

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayPlanCommand
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    ERailwayPlanAction Action = ERailwayPlanAction::ActivatePreset;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    int64 ExpectedRevision = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    FGuid BuildId;

    /** Index of the scheduled visit for SkipStop / DirectToStop. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    int32 StopIndex = INDEX_NONE;

    /** Baked station or track identity. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    FGuid SourceId;

    /** Trip cm for AddStop; native track cm for a track destination. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    double Distance = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    bool bStopAlongWay = false;

    /** Explicit start used only for StartBetweenPoints while out of service. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    FRailwayRoutePoint Departure;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    int32 ApproachEdge = INDEX_NONE;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    int32 OutgoingEdge = INDEX_NONE;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    int32 ExpectedSelectedEdge = INDEX_NONE;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway")
    bool bTowardsRear = false;
};

DECLARE_MULTICAST_DELEGATE_ThreeParams(FRailwayCommandReply, FGuid, bool, const FString&);

/** Put this replicated component on the player controller for remote dispatch. */
UCLASS(ClassGroup=(Railway), meta=(BlueprintSpawnableComponent))
class RAILWAY_API URailwayPlanControlComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    URailwayPlanControlComponent();
    FRailwayCommandReply OnReply;

    UFUNCTION(Server, Reliable)
    void ServerSubmit(FGuid Request, ARailwayTrain* Train, const FRailwayPlanCommand& Command);

private:
    UFUNCTION(Client, Reliable)
    void ClientReply(FGuid Request, bool bSuccess, const FString& Message);
};
