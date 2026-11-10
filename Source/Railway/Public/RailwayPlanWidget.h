#pragma once

#include "Blueprint/UserWidget.h"
#include "RailwayPlanWidget.generated.h"

class ARailwayTrain;
class SRailwayPlanPanel;

/** Ready-to-use player timetable. Create with a local player controller, set Train and add to player screen. */
UCLASS(BlueprintType, Blueprintable, meta=(DisplayName="Railway Plan Widget"))
class RAILWAY_API URailwayPlanWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    /** Use the Widget Blueprint designer tree, including an intentionally empty layout. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Railway")
    bool bUseDesignerLayout = false;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Railway", meta=(ExposeOnSpawn="true"))
    TObjectPtr<ARailwayTrain> Train;

    UFUNCTION(BlueprintCallable, Category="Railway")
    void SetTrain(ARailwayTrain* NewTrain);

    UFUNCTION(BlueprintImplementableEvent, Category="Railway")
    void OnDisplayedTrainChanged(ARailwayTrain* PreviousTrain, ARailwayTrain* NewTrain);

    virtual void ReleaseSlateResources(bool bReleaseChildren) override;
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
private:
    TSharedPtr<SRailwayPlanPanel> Panel;
};
