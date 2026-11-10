#pragma once

#include "RailwayPlanControl.h"
#include "Widgets/SCompoundWidget.h"

class URailwayPlanWidget;
class ARailwayTrain;
class STextBlock;
class SProgressBar;
class SVerticalBox;

class SRailwayPlanPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SRailwayPlanPanel) {}
        SLATE_ARGUMENT(URailwayPlanWidget*, Owner)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args);
    void ResetBinding();
private:
    ARailwayTrain* Train() const;
    EActiveTimerReturnType Poll(double Now, float Delta);
    void Refresh();
    void RebuildRows();
    bool CanSend() const;
    FRailwayPlanCommand MakeCommand(ERailwayPlanAction Action) const;
    FReply Submit(FRailwayPlanCommand Command);
    TSharedRef<SWidget> PointMenu(bool bDeparture);
    TSharedRef<SWidget> PointPicker(bool bDeparture);
    void OnReply(FGuid Request, bool bSuccess, const FString& Message);
    void Feedback(const FString& Text, bool bError);

    TWeakObjectPtr<URailwayPlanWidget> Owner;
    TWeakObjectPtr<ARailwayTrain> DisplayTrain;
    TWeakObjectPtr<URailwayPlanControlComponent> Control;
    FGuid Build, Pending, SelectedDestination, SelectedDeparture;
    int64 Revision = -1;
    uint32 TimetableSerial = 0;
    int32 Cursor = INDEX_NONE;
    int32 DisplayState = INDEX_NONE;
    bool bReady = false;
    bool bShowOptional = false;
    bool bIntermediate = false;
    bool bCustomStart = false;
    double TrackMetres = 0;
    double DepartureMetres = 0;
    double RequestTime = 0;
    FString SelectedLabel, DepartureLabel;
    TSharedPtr<STextBlock> Title, Status, Motion, Location, Destination, Next, Notice;
    TSharedPtr<SProgressBar> Progress;
    TSharedPtr<SVerticalBox> Visits, Sections;
};
