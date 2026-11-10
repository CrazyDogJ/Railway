#pragma once

#include "Kismet/BlueprintAsyncActionBase.h"
#include "Containers/Ticker.h"
#include "RailwayPlanControl.h"
#include "RailwayPlanView.h"
#include "RailwayPlanRequest.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRailwayPlanRequestResult, const FString&, Message);

/** Each Blueprint call owns its reply pins. Accepted means the authority applied the command. */
UCLASS()
class RAILWAY_API URailwayPlanRequest : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintAssignable)
    FRailwayPlanRequestResult Accepted;

    UPROPERTY(BlueprintAssignable)
    FRailwayPlanRequestResult Failed;

    UFUNCTION(BlueprintCallable, Category="Railway|Player UI", meta=(BlueprintInternalUseOnly="true"))
    static URailwayPlanRequest* ActivateRailwayPreset(APlayerController* Player, const FRailwayPlanViewKey& View);

    UFUNCTION(BlueprintCallable, Category="Railway|Player UI", meta=(BlueprintInternalUseOnly="true"))
    static URailwayPlanRequest* CancelRailwayRoute(APlayerController* Player, const FRailwayPlanViewKey& View);

    UFUNCTION(BlueprintCallable, Category="Railway|Player UI", meta=(BlueprintInternalUseOnly="true"))
    static URailwayPlanRequest* SkipRailwayVisit(APlayerController* Player, const FRailwayVisitView& Visit);

    UFUNCTION(BlueprintCallable, Category="Railway|Player UI", meta=(BlueprintInternalUseOnly="true"))
    static URailwayPlanRequest* AddRailwayVisit(APlayerController* Player, const FRailwayVisitView& Visit);

    UFUNCTION(BlueprintCallable, Category="Railway|Player UI", meta=(BlueprintInternalUseOnly="true"))
    static URailwayPlanRequest* GoDirectToRailwayVisit(APlayerController* Player, const FRailwayVisitView& Visit);

    UFUNCTION(BlueprintCallable, Category="Railway|Player UI", meta=(BlueprintInternalUseOnly="true"))
    static URailwayPlanRequest* SetRailwayDestination(APlayerController* Player, const FRailwayPlanViewKey& View, const FRailwayRoutePoint& Destination, bool bStopAlongWay);

    UFUNCTION(BlueprintCallable, Category="Railway|Player UI", meta=(BlueprintInternalUseOnly="true"))
    static URailwayPlanRequest* StartRailwayBetweenPoints(APlayerController* Player, const FRailwayPlanViewKey& View, const FRailwayRoutePoint& Departure, const FRailwayRoutePoint& Destination, bool bStopAlongWay);

    UFUNCTION(BlueprintCallable, Category="Railway|Driving", meta=(BlueprintInternalUseOnly="true"))
    static URailwayPlanRequest* ActivateRailwayTrack(APlayerController* Player, const FRailwayTrackChoice& Choice);

    UFUNCTION(BlueprintCallable, Category="Railway|Driving", meta=(BlueprintInternalUseOnly="true"))
    static URailwayPlanRequest* DeactivateRailwayTrack(APlayerController* Player, const FRailwayTrackChoice& Choice);

    virtual void Activate() override;
    virtual void BeginDestroy() override;
private:
    static URailwayPlanRequest* Make(APlayerController* Player, const FRailwayPlanViewKey& View, ERailwayPlanAction Action);
    static URailwayPlanRequest* MakeSwitch(APlayerController* Player, const FRailwayTrackChoice& Choice, ERailwayPlanAction Action);
    static URailwayPlanRequest* MakeVisit(APlayerController* Player, const FRailwayVisitView& Visit, ERailwayPlanAction Action);
    void Reply(FGuid Id, bool bSuccess, const FString& Message);
    bool CheckConnection(float Delta);
    void Finish(bool bSuccess, const FString& Message);
    void Unbind();

    FRailwayPlanCommand Command;
    TWeakObjectPtr<APlayerController> Controller;
    TWeakObjectPtr<ARailwayTrain> Target;
    TWeakObjectPtr<URailwayPlanControlComponent> Channel;
    FDelegateHandle ReplyHandle;
    FTSTicker::FDelegateHandle TimeoutHandle;
    FGuid RequestId;
    FString LocalError;
    double Deadline = 0;
    bool bStarted = false;
    bool bFinished = false;
};
