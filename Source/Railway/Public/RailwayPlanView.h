#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "RailwayRouteSettings.h"
#include "RailwayUtils.h"
#include "RailwayPlanView.generated.h"

class APlayerController;
class ARailwayTrain;

UENUM(BlueprintType)
enum class ERailwayPlanReadiness : uint8 { NoTrain, NoBakedData, Synchronizing, Ready };

UENUM(BlueprintType)
enum class ERailwayVisitStatus : uint8 { Passed, Served, Dwelling, Next, Scheduled, Available };

/** Keep the key returned with the displayed data. It identifies the train and revision the player saw. */
USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayPlanViewKey
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    TObjectPtr<ARailwayTrain> Train;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FGuid BuildId;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    int64 Revision = -1;
};

/** One legal exit at the next reachable turnout in the requested cab direction. */
USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayTrackChoice
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FRailwayPlanViewKey View;
    UPROPERTY(BlueprintReadOnly, Category="Railway")
    int32 ApproachEdge = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly, Category="Railway")
    int32 OutgoingEdge = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly, Category="Railway")
    int32 SelectedEdge = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FName TrackName;
    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FGuid TrackId;
    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double DistanceToJunction = 0;
    UPROPERTY(BlueprintReadOnly, Category="Railway")
    bool bTowardsRear = false;
    UPROPERTY(BlueprintReadOnly, Category="Railway")
    bool bContinuesSameTrack = false;
    UPROPERTY(BlueprintReadOnly, Category="Railway")
    bool bUsedByCurrentPlan = false;
    UPROPERTY(BlueprintReadOnly, Category="Railway")
    bool bActive = false;
};

/** A lightweight picker item. A station's Point already targets that station. For a track, set Point.TrackDistance in cm. */
USTRUCT(BlueprintType)
struct RAILWAY_API FRailwaySourceOption
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FRailwayRoutePoint Point;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FName Name;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    bool bStation = false;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FName TrackName;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double TrackLength = 0;
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayPlanOverview
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    ERailwayPlanReadiness Readiness = ERailwayPlanReadiness::NoTrain;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FRailwayPlanViewKey View;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    ERailwayState State = ERailwayState::Idle;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    ERailwayDriveMode DriveMode = ERailwayDriveMode::Automatic;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double SpeedKmh = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    bool bCabFacesAgainstPlan = false;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    bool bCoastingWithoutPlan = false;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    bool bHasService = false;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double Distance = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double EndDistance = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double RemainingDistance = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    float Progress = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    bool bLocated = false;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FName CurrentTrack;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FGuid CurrentTrackId;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    bool bCurrentTrackIsLoop = false;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double CurrentTrackDistance = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FName CurrentStation;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double RemainingDwellSeconds = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    bool bHasNextStop = false;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FName NextStation;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double DistanceToNextStop = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    bool bHasDestination = false;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FRailwaySourceOption Destination;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double DestinationTrackDistance = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FTransform DestinationPose = FTransform::Identity;
};

/** One scheduled or addable occurrence. RouteDistance is measured from the trip origin in cm. */
USTRUCT(BlueprintType)
struct RAILWAY_API FRailwayVisitView
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FRailwayPlanViewKey View;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    int32 StopIndex = INDEX_NONE;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FGuid StationId;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FName TrackName;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FRailwayStation Station;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double RouteDistance = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    ERailwayVisitStatus Status = ERailwayVisitStatus::Scheduled;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    bool bCanEdit = false;
};

USTRUCT(BlueprintType)
struct RAILWAY_API FRailwaySectionView
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    int32 SectionIndex = INDEX_NONE;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FName TrackName;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    FGuid TrackId;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    bool bReverse = false;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double EntryDistance = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double ExitDistance = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double RouteBegin = 0;

    UPROPERTY(BlueprintReadOnly, Category="Railway")
    double RouteEnd = 0;
};

UCLASS()
class RAILWAY_API URailwayPlanViewLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    /** Cheap live data for a dashboard timer. Distances are cm, dwell is seconds, speed is km/h. */
    UFUNCTION(BlueprintPure, Category="Railway|Player UI")
    static FRailwayPlanOverview GetRailwayPlanOverview(ARailwayTrain* Train);

    /** Rebuild list rows on changes. Enable Include Addable to enumerate eligible unscheduled visits. */
    UFUNCTION(BlueprintCallable, Category="Railway|Player UI")
    static void GetRailwayTimetable(ARailwayTrain* Train, bool bIncludeAddable,
        TArray<FRailwayVisitView>& Scheduled, TArray<FRailwayVisitView>& Addable);

    UFUNCTION(BlueprintCallable, Category="Railway|Player UI")
    static void GetRailwayRouteSections(ARailwayTrain* Train, TArray<FRailwaySectionView>& Sections);

    UFUNCTION(BlueprintCallable, Category="Railway|Player UI")
    static void GetRailwayRouteSources(ARailwayTrain* Train, TArray<FRailwaySourceOption>& Sources);

    /** Local availability and freshness check. The authority decides player permissions on submission. */
    UFUNCTION(BlueprintPure, Category="Railway|Player UI")
    static bool CanSendRailwayPlanCommand(APlayerController* Player, const FRailwayPlanViewKey& View, FString& Reason);
};
