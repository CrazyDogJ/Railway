#if WITH_DEV_AUTOMATION_TESTS

#include "RailwayDispatchFixtures.h"
#include "RailwayDataActor.h"
#include "RailwayPlanRequest.h"
#include "RailwayPlanWidget.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"
#include "RailwayTrain.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "EdGraphSchema_K2.h"
#include "Engine/World.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Tests/NetTestHelpers.h"
#include "UObject/StructOnScope.h"
#include "WidgetBlueprint.h"

namespace
{
FEdGraphPinType PinType(FName Category, UObject* Type = nullptr)
{
    FEdGraphPinType Pin;
    Pin.PinCategory = Category;
    Pin.PinSubCategoryObject = Type;
    return Pin;
}

template<class T>
T* NewNode(UEdGraph* Graph)
{
    T* N = NewObject<T>(Graph);
    Graph->AddNode(N, false, false);
    N->CreateNewGuid();
    return N;
}

UK2Node_CustomEvent* AddEvent(UEdGraph* Graph, FName Name)
{
    auto* N = NewNode<UK2Node_CustomEvent>(Graph);
    N->CustomFunctionName = Name;
    N->AllocateDefaultPins();
    return N;
}

UK2Node_VariableSet* WriteMember(UEdGraph* Graph, FName Name)
{
    auto* N = NewNode<UK2Node_VariableSet>(Graph);
    N->VariableReference.SetSelfMember(Name);
    N->AllocateDefaultPins();
    return N;
}

UK2Node_CallFunction* Call(UEdGraph* Graph, UClass* Owner, FName Function)
{
    auto* N = NewNode<UK2Node_CallFunction>(Graph);
    N->SetFromFunction(Owner->FindFunctionByName(Function));
    N->AllocateDefaultPins();
    return N;
}

UWidgetBlueprint* MakeTestWidget()
{
    using K2 = UEdGraphSchema_K2;
    UPackage* Outer = GetTransientPackage();
    const FName Name = MakeUniqueObjectName(Outer, UWidgetBlueprint::StaticClass(), TEXT("WBP_RailwayDispatchTest"));
    auto* BP = CastChecked<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), Outer,
        Name, BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
    BP->WidgetTree->RootWidget = BP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("MyDesignerText"));
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Displayed"), PinType(K2::PC_Struct, FRailwayPlanOverview::StaticStruct()));
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("ListDirty"), PinType(K2::PC_Boolean));
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Ack"), PinType(K2::PC_Boolean));
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Failure"), PinType(K2::PC_String));

    auto* Graph = FBlueprintEditorUtils::FindEventGraph(BP);
    auto* Changed = AddEvent(Graph, TEXT("RefreshRows"));
    auto* Dirty = WriteMember(Graph, TEXT("ListDirty"));
    Dirty->FindPinChecked(TEXT("ListDirty"), EGPD_Input)->DefaultValue = TEXT("true");
    Changed->FindPinChecked(K2::PN_Then)->MakeLinkTo(Dirty->GetExecPin());
    auto* Read = AddEvent(Graph, TEXT("ReadTrain"));
    auto* Input = Read->CreateUserDefinedPin(TEXT("Train"), PinType(K2::PC_Object, ARailwayTrain::StaticClass()), EGPD_Output);
    auto* Query = Call(Graph, URailwayPlanViewLibrary::StaticClass(), TEXT("GetRailwayPlanOverview"));
    auto* Write = WriteMember(Graph, TEXT("Displayed"));
    Input->MakeLinkTo(Query->FindPinChecked(TEXT("Train")));
    Read->FindPinChecked(K2::PN_Then)->MakeLinkTo(Write->GetExecPin());
    Query->GetReturnValuePin()->MakeLinkTo(Write->FindPinChecked(TEXT("Displayed"), EGPD_Input));

    // Use the engine's real latent K2 nodes so compiler expansion, pin types and result delegates are exercised.
    for (const TCHAR* Action : {TEXT("ActivateRailwayPreset"), TEXT("CancelRailwayRoute"), TEXT("SkipRailwayVisit"),
        TEXT("AddRailwayVisit"), TEXT("GoDirectToRailwayVisit"), TEXT("SetRailwayDestination"), TEXT("StartRailwayBetweenPoints"),
        TEXT("ActivateRailwayTrack"), TEXT("DeactivateRailwayTrack")})
    {
        auto* Event = AddEvent(Graph, FName(*(FString(TEXT("Do")) + Action)));
        auto* Async = NewNode<UK2Node_AsyncAction>(Graph);
        Async->InitializeProxyFromFunction(URailwayPlanRequest::StaticClass()->FindFunctionByName(Action));
        Async->AllocateDefaultPins();
        Event->FindPinChecked(K2::PN_Then)->MakeLinkTo(Async->GetExecPin());
        auto* Owner = Call(Graph, UUserWidget::StaticClass(), TEXT("GetOwningPlayer"));
        Owner->GetReturnValuePin()->MakeLinkTo(Async->FindPinChecked(TEXT("Player")));
        for (UEdGraphPin* P : Async->Pins)
        {
            if (P->Direction != EGPD_Input || P->PinType.PinCategory == K2::PC_Exec || P->PinName == TEXT("Player") || P->bHidden) continue;
            FEdGraphPinType Type = P->PinType;
            Type.bIsReference = Type.bIsConst = false;
            Event->CreateUserDefinedPin(P->PinName, Type, EGPD_Output)->MakeLinkTo(P);
        }
        auto* Accepted = WriteMember(Graph, TEXT("Ack"));
        Accepted->FindPinChecked(TEXT("Ack"), EGPD_Input)->DefaultValue = TEXT("true");
        Async->FindPinChecked(TEXT("Accepted"))->MakeLinkTo(Accepted->GetExecPin());
        auto* Failed = WriteMember(Graph, TEXT("Failure"));
        Async->FindPinChecked(TEXT("Failed"))->MakeLinkTo(Failed->GetExecPin());
        Async->FindPinChecked(TEXT("Message"))->MakeLinkTo(Failed->FindPinChecked(TEXT("Failure"), EGPD_Input));
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);
    return BP;
}

template<typename Fill>
void Fire(UUserWidget* Widget, FName Name, Fill SetInputs)
{
    auto* Function = Widget->FindFunctionChecked(Name);
    FStructOnScope Params(Function);
    SetInputs(Function, Params.GetStructMemory());
    Widget->ProcessEvent(Function, Params.GetStructMemory());
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayBlueprintWidgetTest, "Railway.Dispatch.DesignerBlueprint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRailwayBlueprintWidgetTest::RunTest(const FString&)
{
    auto* BP = MakeTestWidget();
    if (!TestTrue(TEXT("Ordinary Widget Blueprint compiles all dispatch actions"), BP->Status == BS_UpToDate)) return false;
    UE::Net::FTestWorlds Worlds;
    if (!TestTrue(TEXT("Connect player"), Worlds.CreateAndConnectClient())) return false;
    UWorld* Server = Worlds.Server.GetWorld();
    UWorld* Client = Worlds.Clients[0].GetWorld();
    auto* RemoteOwner = Worlds.GetServerPlayerControllerOfClient(0);
    auto* Control = NewObject<URailwayPlanControlComponent>(RemoteOwner);
    RemoteOwner->AddInstanceComponent(Control);
    Control->RegisterComponent();
    RemoteOwner->ForceNetUpdate();
    auto* Data = Railway::Tests::MakeNetwork(*Server);
    Client->SpawnActor<ARailwayDataActor>()->Network = Data->Network;
    auto* Authority = Railway::Tests::MakeTrain(*Server, Data);
    auto* PC = Client->GetFirstPlayerController();
    ARailwayTrain* Proxy = nullptr;
    if (!Worlds.TickUntil([&]{
        Proxy = Railway::Tests::OnlyTrain(*Client);
        return Proxy && PC->FindComponentByClass<URailwayPlanControlComponent>();
    }, 240)) return false;

    auto* Widget = CreateWidget<UUserWidget>(PC, BP->GeneratedClass.Get());
    if (!TestNotNull(TEXT("User-defined Designer tree"), Widget ? Widget->GetWidgetFromName(TEXT("MyDesignerText")) : nullptr)) return false;
    auto* DesignerPanel = CreateWidget<URailwayPlanWidget>(PC);
    DesignerPanel->bUseDesignerLayout = true;
    auto* CustomRoot = DesignerPanel->WidgetTree->ConstructWidget<UTextBlock>();
    DesignerPanel->WidgetTree->RootWidget = CustomRoot;
    const TSharedRef<SWidget> DesignerSlate = DesignerPanel->TakeWidget();
    TestTrue(TEXT("Optional Railway widget parent renders its Designer root"), CustomRoot->GetCachedWidget().IsValid());

    FScriptDelegate Changed;
    Changed.BindUFunction(Widget, TEXT("RefreshRows"));
    Proxy->OnRailwayTimetableChanged.AddUnique(Changed);
    auto Overview = [&] {
        Fire(Widget, TEXT("ReadTrain"), [&](UFunction* F, void* P)
        {
            FindFieldChecked<FObjectProperty>(F, TEXT("Train"))->SetObjectPropertyValue_InContainer(P, Proxy);
        });
        return *FindFieldChecked<FStructProperty>(BP->GeneratedClass, TEXT("Displayed"))->ContainerPtrToValuePtr<FRailwayPlanOverview>(Widget);
    };
    auto WithView = [&](FName Name, const FRailwayPlanViewKey& View)
    {
        Fire(Widget, Name, [&](UFunction* F, void* P)
        {
            *FindFieldChecked<FStructProperty>(F, TEXT("View"))->ContainerPtrToValuePtr<FRailwayPlanViewKey>(P) = View;
        });
    };
    const auto Before = Overview();
    if (!TestTrue(TEXT("Overview supplies a valid key while idle"), Before.Readiness == ERailwayPlanReadiness::Ready && !Before.bHasService)) return false;
    WithView(TEXT("DoActivateRailwayPreset"), Before.View);
    if (!TestTrue(TEXT("Blueprint Accepted pin runs on remote player"), Worlds.TickUntil([&]
    {
        return FindFieldChecked<FBoolProperty>(BP->GeneratedClass, TEXT("Ack"))->GetPropertyValue_InContainer(Widget) &&
            Proxy->ActivePlan.Stops.Num() == 3 && Proxy->IsPlanPresentationCurrent();
    }, 240))) return false;

    TestTrue(TEXT("Client timetable event reaches widget"), FindFieldChecked<FBoolProperty>(BP->GeneratedClass, TEXT("ListDirty"))->GetPropertyValue_InContainer(Widget));
    auto Current = Overview();
    TestTrue(TEXT("Geometric terminus at track end"), Current.bHasDestination && !Current.Destination.bStation &&
        Current.DestinationTrackDistance == 10000 && Current.DestinationPose.GetLocation().Equals(FVector(10000, 0, 0), .01));
    TArray<FRailwaySourceOption> Sources;
    URailwayPlanViewLibrary::GetRailwayRouteSources(Proxy, Sources);
    TestEqual(TEXT("Station and track picker options"), Sources.Num(), 4);
    TArray<FRailwayVisitView> Rows, Available;
    URailwayPlanViewLibrary::GetRailwayTimetable(Proxy, true, Rows, Available);
    if (!TestEqual(TEXT("Three independent visits"), Rows.Num(), 3)) return false;
    TestTrue(TEXT("Same-name scheduled visits have distinct identities"), Rows[1].Station.Name == Rows[2].Station.Name && Rows[1].StationId != Rows[2].StationId);
    auto WithVisit = [&](FName Name, const FRailwayVisitView& Visit)
    {
        Fire(Widget, Name, [&](UFunction* F, void* P)
        {
            *FindFieldChecked<FStructProperty>(F, TEXT("Visit"))->ContainerPtrToValuePtr<FRailwayVisitView>(P) = Visit;
        });
    };
    WithVisit(TEXT("DoSkipRailwayVisit"), Rows[1]);
    if (!Worlds.TickUntil([&]{ return Proxy->ActivePlan.Stops.Num() == 2 && Proxy->IsPlanPresentationCurrent(); }, 240)) return false;
    URailwayPlanViewLibrary::GetRailwayTimetable(Proxy, true, Rows, Available);
    if (!TestTrue(TEXT("Exact skipped occurrence becomes addable"), Available.Num() == 1 && Available[0].RouteDistance == 5000)) return false;
    WithVisit(TEXT("DoAddRailwayVisit"), Available[0]);
    if (!Worlds.TickUntil([&]{ return Proxy->ActivePlan.Stops.Num() == 3 && Proxy->IsPlanPresentationCurrent(); }, 240)) return false;
    URailwayPlanViewLibrary::GetRailwayTimetable(Proxy, false, Rows, Available);
    WithVisit(TEXT("DoGoDirectToRailwayVisit"), Rows.Last());
    if (!Worlds.TickUntil([&]{ return Proxy->ActivePlan.bFinishAtLastStop && Proxy->IsPlanPresentationCurrent(); }, 240)) return false;
    Current = Overview();
    TestTrue(TEXT("Direct station and effective route end"), Current.Destination.bStation && Current.EndDistance == 9000);
    TArray<FRailwaySectionView> Sections;
    URailwayPlanViewLibrary::GetRailwayRouteSections(Proxy, Sections);
    TestTrue(TEXT("Section list clipped at direct terminus"), Sections.Num() == 1 && Sections[0].ExitDistance == 9000);

    const auto Revision = Authority->GetPlanRevision();
    WithView(TEXT("DoCancelRailwayRoute"), Before.View);
    auto* Failure = FindFieldChecked<FStrProperty>(BP->GeneratedClass, TEXT("Failure"));
    TestFalse(TEXT("Stale displayed key reports failure through Blueprint"), Failure->GetPropertyValue_InContainer(Widget).IsEmpty());
    Failure->SetPropertyValue_InContainer(Widget, {});
    WithView(TEXT("DoSetRailwayDestination"), Current.View); // empty destination: server must reject
    TestTrue(TEXT("Server rejection reaches Blueprint Failed pin"), Worlds.TickUntil([&]{ return !Failure->GetPropertyValue_InContainer(Widget).IsEmpty(); }, 240));
    TestEqual(TEXT("Rejected requests leave plan intact"), Authority->GetPlanRevision(), Revision);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRailwayLocalBlueprintRequestTest, "Railway.Dispatch.LocalBlueprintRequest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRailwayLocalBlueprintRequestTest::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    auto* PC = World->SpawnActor<APlayerController>();
    PC->SetAsLocalPlayerController();
    auto* Data = Railway::Tests::MakeNetwork(*World);
    auto* Train = Railway::Tests::MakeTrain(*World, Data);
    const auto Idle = URailwayPlanViewLibrary::GetRailwayPlanOverview(Train);
    FString Reason;
    if (!TestTrue(TEXT("Standalone player needs no relay"), URailwayPlanViewLibrary::CanSendRailwayPlanCommand(PC, Idle.View, Reason))) return false;
    URailwayPlanRequest::ActivateRailwayPreset(PC, Idle.View)->Activate();
    TestTrue(TEXT("Local action runs immediately"), Train->State == ERailwayState::Running && Train->ActivePlan.Stops.Num() == 3);
    URailwayPlanRequest::CancelRailwayRoute(PC, Idle.View)->Activate();
    TestTrue(TEXT("Old local row cannot cancel new service"), Train->State == ERailwayState::Running);
    return true;
}
#endif
