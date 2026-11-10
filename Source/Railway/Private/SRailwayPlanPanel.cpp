#include "SRailwayPlanPanel.h"

#include "RailwayDataActor.h"
#include "RailwayPlanView.h"
#include "RailwayPlanWidget.h"
#include "RailwayTrain.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GameFramework/PlayerController.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
const FLinearColor Paper(.035f, .052f, .066f, .98f);
const FLinearColor Light(.90f, .94f, .95f);
const FLinearColor Muted(.57f, .68f, .72f);
const FLinearColor Accent(.26f, .87f, .71f);
const FLinearColor Amber(1.f, .71f, .33f);

FText Text(const FString& S) { return FText::FromString(S); }

TSharedRef<STextBlock> Label(const FString& S, int32 Size = 11, FLinearColor Color = Light, bool bBold = false)
{
    return SNew(STextBlock).Text(Text(S)).ColorAndOpacity(Color)
        .Font(FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size)).AutoWrapText(true);
}

FString Metres(double Cm) { return FString::Printf(TEXT("%.1f m"), Cm / 100.0); }

const TCHAR* StateName(ERailwayState State)
{
    switch (State)
    {
    case ERailwayState::Idle: return TEXT("待命");
    case ERailwayState::Running: return TEXT("运行中");
    case ERailwayState::Dwelling: return TEXT("停站中");
    case ERailwayState::Finished: return TEXT("行程结束");
    default: return TEXT("路线无效");
    }
}

FString SourceName(const FRailwayNetworkData& Data, FGuid Id)
{
    if (const int32* I = Data.TrackById.Find(Id)) return Data.Tracks[*I].Name.ToString();
    if (const int32* I = Data.StationById.Find(Id))
    {
        const FRailwayBakedStation& S = Data.Stations[*I];
        return FString::Printf(TEXT("%s  ·  %s / %s"), *S.Definition.Name.ToString(),
            *Data.Tracks[Data.Edges[S.EdgeIndex].TrackIndex].Name.ToString(), *Metres(S.Definition.TrackDistance));
    }
    return {};
}
}

ARailwayTrain* SRailwayPlanPanel::Train() const
{
    ARailwayTrain* T = Owner.IsValid() ? Owner->Train.Get() : nullptr;
    return IsValid(T) ? T : nullptr;
}

void SRailwayPlanPanel::Construct(const FArguments& Args)
{
    Owner = Args._Owner;
    Title = Label(TEXT("列车运行计划"), 18, Light, true);
    Status = Label({}, 13, Accent, true);
    Motion = Label({}, 12);
    Location = Label({}, 10, Muted);
    Destination = Label({}, 13, Light, true);
    Next = Label({}, 11, Muted);
    Notice = Label({}, 11, Amber);

    auto ActionButton = [this](const TCHAR* Name, ERailwayPlanAction Action)
    {
        return SNew(SButton).Text(Text(Name)).ContentPadding(FMargin(10, 5))
            .IsEnabled_Lambda([this, Action]
            {
                return CanSend() && (Action == ERailwayPlanAction::ActivatePreset ||
                    (Train()->State != ERailwayState::Idle && !Train()->ActivePlan.Sections.IsEmpty()));
            })
            .OnClicked_Lambda([this, Action]{ return Submit(MakeCommand(Action)); });
    };

    ChildSlot.HAlign(HAlign_Center).VAlign(VAlign_Center)
    [
        SNew(SBox).WidthOverride(620).MaxDesiredHeight(760)
        [
            SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(Paper).Padding(18)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().FillWidth(1)[Title.ToSharedRef()]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Status.ToSharedRef()]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 2)[Motion.ToSharedRef()]
                + SVerticalBox::Slot().AutoHeight()[Location.ToSharedRef()]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 8)
                [ SNew(SBox).HeightOverride(5)[SAssignNew(Progress, SProgressBar).FillColorAndOpacity(Accent)] ]
                + SVerticalBox::Slot().AutoHeight()[Destination.ToSharedRef()]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 8)[Next.ToSharedRef()]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 3)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth()[SNew(SComboButton)
                        .OnGetMenuContent(this, &SRailwayPlanPanel::TurnoutMenu, false)
                        .ButtonContent()[Label(TEXT("车头方向选线"))]]
                    + SHorizontalBox::Slot().AutoWidth().Padding(6, 0)[SNew(SComboButton)
                        .OnGetMenuContent(this, &SRailwayPlanPanel::TurnoutMenu, true)
                        .ButtonContent()[Label(TEXT("车尾方向选线"))]]
                ]
                + SVerticalBox::Slot().FillHeight(1)
                [
                    SNew(SScrollBox)
                    + SScrollBox::Slot()
                    [
                        SNew(SVerticalBox)
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 5)
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().FillWidth(1)[Label(TEXT("停靠时刻表"), 13, Light, true)]
                            + SHorizontalBox::Slot().AutoWidth()
                            [
                                SNew(SCheckBox).IsChecked(ECheckBoxState::Unchecked)
                                .OnCheckStateChanged_Lambda([this](ECheckBoxState S){ bShowOptional = S == ECheckBoxState::Checked; RebuildRows(); })
                                [Label(TEXT("显示可加停站"), 10)]
                            ]
                        ]
                        + SVerticalBox::Slot().AutoHeight()[SAssignNew(Visits, SVerticalBox)]
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 10)
                        [
                            SNew(SExpandableArea).InitiallyCollapsed(true).HeaderContent()[Label(TEXT("途经轨道区间"))]
                            .BodyContent()[SAssignNew(Sections, SVerticalBox)]
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 7, 0, 3)
                        [
                            SNew(SCheckBox)
                            .Visibility_Lambda([this]{ return Train() && Train()->Speed == 0 && Train()->State != ERailwayState::Running && Train()->State != ERailwayState::Dwelling ? EVisibility::Visible : EVisibility::Collapsed; })
                            .IsChecked_Lambda([this]{ return bCustomStart ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                            .OnCheckStateChanged_Lambda([this](ECheckBoxState S){ bCustomStart = S == ECheckBoxState::Checked; })
                            [Label(TEXT("指定新起点发车"))]
                        ]
                        + SVerticalBox::Slot().AutoHeight()
                        [ SNew(SBox).Visibility_Lambda([this]{ return bCustomStart ? EVisibility::Visible : EVisibility::Collapsed; })[PointPicker(true)] ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 3)[Label(TEXT("目的站或终点位置"), 13, Light, true)]
                        + SVerticalBox::Slot().AutoHeight()[PointPicker(false)]
                        + SVerticalBox::Slot().AutoHeight()
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().FillWidth(1)
                            [
                                SNew(SCheckBox).OnCheckStateChanged_Lambda([this](ECheckBoxState S){ bIntermediate = S == ECheckBoxState::Checked; })
                                [Label(TEXT("停靠沿途站点"))]
                            ]
                            + SHorizontalBox::Slot().AutoWidth()
                            [
                                SNew(SButton).Text_Lambda([this]{ return Text(bCustomStart ? TEXT("从所选起点发车") : TEXT("改设终点")); })
                                .IsEnabled_Lambda([this]{ return CanSend() && SelectedDestination.IsValid() && (!bCustomStart || SelectedDeparture.IsValid()); })
                                .OnClicked_Lambda([this]
                                {
                                    auto Cmd = MakeCommand(bCustomStart ? ERailwayPlanAction::StartBetweenPoints : ERailwayPlanAction::SetDestination);
                                    Cmd.Departure = {{SelectedDeparture}, DepartureMetres * 100.0};
                                    Cmd.SourceId = SelectedDestination;
                                    Cmd.Distance = TrackMetres * 100.0;
                                    Cmd.bStopAlongWay = bIntermediate;
                                    return Submit(Cmd);
                                })
                            ]
                        ]
                    ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth()[ActionButton(TEXT("从预设起点发车"), ERailwayPlanAction::ActivatePreset)]
                    + SHorizontalBox::Slot().AutoWidth().Padding(8, 0)[ActionButton(TEXT("停用运行计划"), ERailwayPlanAction::CancelRoute)]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)[Notice.ToSharedRef()]
            ]
        ]
    ];
    ResetBinding();
    RegisterActiveTimer(.20f, FWidgetActiveTimerDelegate::CreateSP(this, &SRailwayPlanPanel::Poll));
}

void SRailwayPlanPanel::ResetBinding()
{
    if (Control.IsValid()) Control->OnReply.RemoveAll(this);
    Control.Reset();
    Pending.Invalidate();
    SelectedDestination.Invalidate();
    SelectedLabel.Reset();
    SelectedDeparture.Invalidate();
    DepartureLabel.Reset();
    bCustomStart = false;
    Revision = -1;
    Build.Invalidate();
    DisplayTrain = Train();
    Notice->SetText(FText::GetEmpty());
    Refresh();
}

EActiveTimerReturnType SRailwayPlanPanel::Poll(double Now, float Delta)
{
    if (Pending.IsValid() && FPlatformTime::Seconds() - RequestTime > 10)
    {
        Pending.Invalidate();
        Feedback(TEXT("未收到服务器回执，请核对当前计划"), true);
    }
    Refresh();
    return EActiveTimerReturnType::Continue;
}

bool SRailwayPlanPanel::CanSend() const
{
    ARailwayTrain* T = Train();
    return T && T == DisplayTrain.Get() && bReady && Build.IsValid() && !Pending.IsValid() &&
        Revision == T->GetPlanRevision() && Owner.IsValid() && IsValid(Owner->GetOwningPlayer()) &&
        (T->HasAuthority() || Control.IsValid());
}

FRailwayPlanCommand SRailwayPlanPanel::MakeCommand(ERailwayPlanAction Action) const
{
    FRailwayPlanCommand Cmd;
    Cmd.Action = Action;
    Cmd.ExpectedRevision = Revision;
    Cmd.BuildId = Build;
    return Cmd;
}

void SRailwayPlanPanel::Feedback(const FString& Message, bool bError)
{
    Notice->SetColorAndOpacity(bError ? Amber : Accent);
    Notice->SetText(Text(Message));
}

FReply SRailwayPlanPanel::Submit(FRailwayPlanCommand Cmd)
{
    ARailwayTrain* T = Train();
    if (!CanSend() || Cmd.ExpectedRevision != Revision || Cmd.BuildId != Build)
    {
        Feedback(TEXT("计划正在同步，请稍后重试"), true);
        return FReply::Handled();
    }
    if (T->HasAuthority())
    {
        FString Error;
        const bool bOk = T->ApplyPlayerPlanCommand(Owner->GetOwningPlayer(), Cmd, Error);
        Feedback(bOk ? TEXT("运行计划已更新") : Error, !bOk);
        Refresh();
    }
    else
    {
        Pending = FGuid::NewGuid();
        RequestTime = FPlatformTime::Seconds();
        Feedback(TEXT("正在等待调度回执…"), false);
        Control->ServerSubmit(Pending, T, Cmd);
    }
    return FReply::Handled();
}

void SRailwayPlanPanel::OnReply(FGuid Request, bool bSuccess, const FString& Message)
{
    if (Request != Pending) return;
    Pending.Invalidate();
    Feedback(bSuccess ? TEXT("服务器已接受调度") : Message, !bSuccess);
    Refresh();
}

TSharedRef<SWidget> SRailwayPlanPanel::PointPicker(bool bDeparture)
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SComboButton).OnGetMenuContent(this, &SRailwayPlanPanel::PointMenu, bDeparture)
            .ButtonContent()[SNew(STextBlock).Text_Lambda([this, bDeparture]
            {
                const FString& Name = bDeparture ? DepartureLabel : SelectedLabel;
                return Text(Name.IsEmpty() ? (bDeparture ? TEXT("选择起点") : TEXT("选择终点")) : Name);
            })]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 3)
        [
            SNew(SNumericEntryBox<double>).MinValue(0.0)
            .Visibility_Lambda([this, bDeparture]
            {
                const auto* D = Train() ? Train()->GetRailwayData() : nullptr;
                return D && D->Network.TrackById.Contains(bDeparture ? SelectedDeparture : SelectedDestination)
                    ? EVisibility::Visible : EVisibility::Collapsed;
            })
            .Label()[Label(TEXT("轨道里程 (m)"))]
            .Value_Lambda([this, bDeparture]{ return TOptional<double>(bDeparture ? DepartureMetres : TrackMetres); })
            .OnValueChanged_Lambda([this, bDeparture](double Value)
            {
                if (bDeparture) DepartureMetres = Value;
                else TrackMetres = Value;
            })
        ];
}

TSharedRef<SWidget> SRailwayPlanPanel::TurnoutMenu(bool bTowardsRear)
{
    TArray<FRailwayTrackChoice> Choices;
    if (auto* T = Train()) T->GetUpcomingTrackChoices(bTowardsRear, Choices);
    if (Choices.IsEmpty()) return Label(TEXT("该方向没有可切换的道岔"));
    FMenuBuilder Menu(true, nullptr);
    Menu.BeginSection("Turnout", Text(FString::Printf(TEXT("距道岔 %s"), *Metres(Choices[0].DistanceToJunction))));
    for (const auto& C : Choices)
    {
        auto Cmd = MakeCommand(C.bActive ? ERailwayPlanAction::DeactivateTrack : ERailwayPlanAction::ActivateTrack);
        Cmd.ExpectedRevision = C.View.Revision;
        Cmd.BuildId = C.View.BuildId;
        Cmd.ApproachEdge = C.ApproachEdge;
        Cmd.OutgoingEdge = C.OutgoingEdge;
        Cmd.ExpectedSelectedEdge = C.SelectedEdge;
        Cmd.bTowardsRear = C.bTowardsRear;
        Menu.AddMenuEntry(Text(FString::Printf(TEXT("%s  ·  %s%s"), *C.TrackName.ToString(),
            C.bContinuesSameTrack ? TEXT("本线") : TEXT("转线"), C.bActive ? TEXT("  ✓") : TEXT(""))),
            Text(C.bActive ? TEXT("停用此出口并改走其他合法轨道") : TEXT("激活此出口；与班次冲突时停用计划")),
            FSlateIcon(), FUIAction(FExecuteAction::CreateSPLambda(SharedThis(this), [this, Cmd]{ Submit(Cmd); }),
                FCanExecuteAction::CreateSP(this, &SRailwayPlanPanel::CanSend)));
    }
    Menu.EndSection();
    return Menu.MakeWidget();
}

TSharedRef<SWidget> SRailwayPlanPanel::PointMenu(bool bDeparture)
{
    FMenuBuilder Menu(true, nullptr);
    const ARailwayDataActor* Data = Train() ? Train()->GetRailwayData() : nullptr;
    if (!Data) return SNew(STextBlock).Text(Text(TEXT("没有可用铁路网")));
    auto Add = [&](FGuid Id)
    {
        const FString Name = SourceName(Data->Network, Id);
        Menu.AddMenuEntry(Text(Name), Text(Id.ToString()), FSlateIcon(), FUIAction(FExecuteAction::CreateSPLambda(SharedThis(this), [this, Id, Name, bDeparture]
        {
            if (bDeparture)
            {
                SelectedDeparture = Id;
                DepartureLabel = Name;
                DepartureMetres = 0;
                return;
            }
            SelectedDestination = Id;
            SelectedLabel = Name;
            TrackMetres = 0;
        })));
    };
    Menu.BeginSection("Stations", Text(TEXT("站点")));
    for (const auto& S : Data->Network.Stations) Add(S.Id);
    Menu.EndSection();
    Menu.BeginSection("Tracks", Text(TEXT("轨道位置")));
    for (const auto& T : Data->Network.Tracks) Add(T.Id);
    Menu.EndSection();
    return SNew(SBox).MaxDesiredHeight(390)[Menu.MakeWidget()];
}

void SRailwayPlanPanel::Refresh()
{
    ARailwayTrain* T = Train();
    if (T != DisplayTrain.Get()) { ResetBinding(); return; }
    APlayerController* PC = Owner.IsValid() ? Owner->GetOwningPlayer() : nullptr;
    auto* NewControl = PC ? PC->FindComponentByClass<URailwayPlanControlComponent>() : nullptr;
    if (NewControl != Control.Get())
    {
        if (Control.IsValid()) Control->OnReply.RemoveAll(this);
        Pending.Invalidate();
        Control = NewControl;
        if (NewControl) NewControl->OnReply.AddSP(this, &SRailwayPlanPanel::OnReply);
    }
    const ARailwayDataActor* Data = T ? T->GetRailwayData() : nullptr;
    const FGuid CurrentBuild = Data ? Data->Network.BuildId : FGuid();
    const FRailwayPlanOverview Overview = URailwayPlanViewLibrary::GetRailwayPlanOverview(T);
    const bool Ready = Overview.Readiness == ERailwayPlanReadiness::Ready;
    const int64 Rev = T ? T->GetPlanRevision() : -1;
    const int32 NewCursor = T ? T->NextStop : INDEX_NONE;
    const int32 NewState = T ? static_cast<int32>(T->State) : INDEX_NONE;
    const uint32 Serial = T ? T->GetTimetableUpdateSerial() : 0;
    const bool bChanged = TimetableSerial != Serial || Build != CurrentBuild || Revision != Rev || Cursor != NewCursor || DisplayState != NewState || bReady != Ready;
    TimetableSerial = Serial;
    Build = CurrentBuild;
    Revision = Rev;
    Cursor = NewCursor;
    DisplayState = NewState;
    bReady = Ready;
    if (T && (T->Speed != 0 || T->State == ERailwayState::Running || T->State == ERailwayState::Dwelling)) bCustomStart = false;
    if (SelectedDeparture.IsValid() && bChanged)
    {
        DepartureLabel = Data ? SourceName(Data->Network, SelectedDeparture) : FString();
        if (DepartureLabel.IsEmpty()) SelectedDeparture.Invalidate();
    }
    if (SelectedDestination.IsValid() && bChanged)
    {
        SelectedLabel = Data ? SourceName(Data->Network, SelectedDestination) : FString();
        if (SelectedLabel.IsEmpty()) SelectedDestination.Invalidate();
    }
    if (bChanged || Visits->NumSlots() == 0) RebuildRows();
    Title->SetText(Text(T ? T->GetActorNameOrLabel() : TEXT("列车运行计划")));
    Status->SetText(Text(!T ? TEXT("未绑定列车") : !Data ? TEXT("未找到铁路数据") :
        !Ready ? TEXT("等待计划同步") : (Overview.bCoastingWithoutPlan ? TEXT("无计划惰行") : (T->State == ERailwayState::Idle && T->DriveMode == ERailwayDriveMode::Manual ? TEXT("自由驾驶") : StateName(T->State)))));
    if (!Ready)
    {
        Motion->SetText(FText::GetEmpty()); Location->SetText(FText::GetEmpty());
        Destination->SetText(FText::GetEmpty()); Next->SetText(FText::GetEmpty()); Progress->SetPercent(0.f);
        return;
    }
    if (!T->HasAuthority() && !Control.IsValid())
        Feedback(TEXT("只读 · 玩家控制器缺少 Railway Plan Control 组件"), true);
    Motion->SetText(Text(FString::Printf(TEXT("%s驾驶   ·   %.1f km/h   ·   行程 %s / %s"),
        Overview.DriveMode == ERailwayDriveMode::Manual ? TEXT("手动") : TEXT("自动"), Overview.SpeedKmh,
        *Metres(Overview.Distance), *Metres(Overview.EndDistance))));
    Progress->SetPercent(Overview.Progress);
    Location->SetText(Text(Overview.bLocated
        ? FString::Printf(TEXT("当前位置  %s / %s"), *Overview.CurrentTrack.ToString(), *Metres(Overview.CurrentTrackDistance))
        : TEXT("当前位置未定位")));
    if (Overview.bHasDestination)
    {
        const auto& End = Overview.Destination;
        const FString EndName = End.bStation ? End.Name.ToString() + TEXT("  ·  ") + End.TrackName.ToString() : End.Name.ToString();
        Destination->SetText(Text(FString::Printf(TEXT("终点  %s / %s"), *EndName, *Metres(Overview.DestinationTrackDistance))));
        Destination->SetToolTipText(Text(Overview.DestinationPose.GetLocation().ToString()));
    }
    else { Destination->SetText(Text(TEXT("尚未激活路线"))); Destination->SetToolTipText(FText::GetEmpty()); }
    FString Detail;
    if (Overview.bHasService)
    {
        Detail = FString::Printf(TEXT("距终点 %s"), *Metres(Overview.RemainingDistance));
        if (Overview.bHasNextStop) Detail += FString::Printf(TEXT("   ·   下一站 %s / %s"), *Overview.NextStation.ToString(), *Metres(Overview.DistanceToNextStop));
        if (Overview.State == ERailwayState::Dwelling) Detail += FString::Printf(TEXT("   ·   %s 停站剩余 %.1f s"), *Overview.CurrentStation.ToString(), Overview.RemainingDwellSeconds);
    }
    Next->SetText(Text(Detail));
}

void SRailwayPlanPanel::RebuildRows()
{
    Visits->ClearChildren();
    Sections->ClearChildren();
    ARailwayTrain* T = Train();
    const auto* D = T ? T->GetRailwayData() : nullptr;
    if (!bReady || !D || T->State == ERailwayState::Idle || T->ActivePlan.Sections.IsEmpty())
    {
        Visits->AddSlot().AutoHeight()[Label(TEXT("暂无停靠时刻表"), 11, Muted)];
        return;
    }
    TArray<FRailwayVisitView> Scheduled, Addable;
    URailwayPlanViewLibrary::GetRailwayTimetable(T, bShowOptional, Scheduled, Addable);
    auto Button = [this](const TCHAR* Caption, FRailwayPlanCommand Cmd)
    {
        return SNew(SButton).Text(Text(Caption))
            .IsEnabled_Lambda([this, Cmd]
            {
                return CanSend() && Cmd.ExpectedRevision == Revision &&
                    (Cmd.Action != ERailwayPlanAction::AddStop || Cmd.Distance >= Train()->Distance);
            }).OnClicked_Lambda([this, Cmd]{ return Submit(Cmd); });
    };
    for (const auto& Stop : Scheduled)
    {
        const int32 I = Stop.StopIndex;
        const auto& Station = Stop.Station;
        const bool bHere = Stop.Status == ERailwayVisitStatus::Dwelling;
        const TCHAR* Phase = TEXT("待停靠");
        switch (Stop.Status)
        {
        case ERailwayVisitStatus::Dwelling: Phase = TEXT("正在停靠"); break;
        case ERailwayVisitStatus::Served: Phase = TEXT("已停靠"); break;
        case ERailwayVisitStatus::Passed: Phase = TEXT("已越过"); break;
        case ERailwayVisitStatus::Next: Phase = TEXT("下一站"); break;
        default: break;
        }
        auto Row = SNew(SHorizontalBox);
        Row->AddSlot().FillWidth(1).VAlign(VAlign_Center)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()[Label(FString::Printf(TEXT("%d.  %s    %s"), I + 1, *Station.Name.ToString(), Phase),
                12, bHere || I == Cursor ? Accent : Light, true)]
            + SVerticalBox::Slot().AutoHeight()[Label(FString::Printf(TEXT("行程 %s   ·   停留 %.1f s"), *Metres(Stop.RouteDistance), Station.DwellSeconds), 10, Muted)]
        ];
        if (Stop.bCanEdit)
        {
            auto Cmd = MakeCommand(ERailwayPlanAction::DirectToStop); Cmd.StopIndex = I;
            Row->AddSlot().AutoWidth().VAlign(VAlign_Center)[Button(TEXT("直达"), Cmd)];
            Cmd.Action = ERailwayPlanAction::SkipStop;
            Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(5, 0, 0, 0)[Button(TEXT("取消停靠"), Cmd)];
        }
        Visits->AddSlot().AutoHeight().Padding(0, 4)[Row];
    }
    if (Scheduled.IsEmpty()) Visits->AddSlot().AutoHeight()[Label(TEXT("本次行程未安排停站"), 11, Muted)];
    for (const auto& S : Addable)
    {
        auto Cmd = MakeCommand(ERailwayPlanAction::AddStop);
        Cmd.SourceId = S.StationId;
        Cmd.Distance = S.RouteDistance;
        Visits->AddSlot().AutoHeight().Padding(0, 3)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
            [Label(S.Station.Name.ToString() + TEXT("   ·   ") + Metres(S.RouteDistance), 11, Muted)]
            + SHorizontalBox::Slot().AutoWidth()[Button(TEXT("加停"), Cmd)]
        ];
    }
    TArray<FRailwaySectionView> Route;
    URailwayPlanViewLibrary::GetRailwayRouteSections(T, Route);
    for (const auto& S : Route)
        Sections->AddSlot().AutoHeight().Padding(0, 2)
        [Label(FString::Printf(TEXT("%s  ·  %s   %s → %s"), *S.TrackName.ToString(),
            S.bReverse ? TEXT("逆向") : TEXT("顺向"), *Metres(S.EntryDistance), *Metres(S.ExitDistance)), 10, Muted)];
}
