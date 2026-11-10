#include "RailwayPlanWidget.h"

#include "RailwayTrain.h"
#include "SRailwayPlanPanel.h"

void URailwayPlanWidget::SetTrain(ARailwayTrain* NewTrain)
{
    ARailwayTrain* Previous = Train;
    Train = NewTrain;
    if (Panel) Panel->ResetBinding();
    if (Previous != NewTrain) OnDisplayedTrainChanged(Previous, NewTrain);
}

TSharedRef<SWidget> URailwayPlanWidget::RebuildWidget()
{
    if (bUseDesignerLayout)
    {
        Panel.Reset();
        return Super::RebuildWidget();
    }
    return SAssignNew(Panel, SRailwayPlanPanel).Owner(this);
}

void URailwayPlanWidget::ReleaseSlateResources(bool bReleaseChildren)
{
    Super::ReleaseSlateResources(bReleaseChildren);
    Panel.Reset();
}
