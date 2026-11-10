#include "RailwayDataBuilder.h"
#include "RailwaySourceRefCustomization.h"
#include "PropertyEditorModule.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "RailwayEditor"

class FRailwayEditorModule final : public IModuleInterface
{
    virtual void StartupModule() override
    {
        auto& Properties = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
        Properties.RegisterCustomPropertyTypeLayout("RailwaySourceRef",
            FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FRailwaySourceRefCustomization::MakeInstance));
        Properties.NotifyCustomizationModuleChanged();
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FRailwayEditorModule::RegisterMenus));
    }

    virtual void ShutdownModule() override
    {
        if (auto* Properties = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
        {
            Properties->UnregisterCustomPropertyTypeLayout("RailwaySourceRef");
            Properties->NotifyCustomizationModuleChanged();
        }
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
    }

    void RegisterMenus()
    {
        FToolMenuOwnerScoped Owner(this);
        UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Build");
        Menu->FindOrAddSection("Railway", LOCTEXT("Railway", "Railway")).AddMenuEntry(
            "BuildRailwayData", LOCTEXT("Build", "Build Railway Data"),
            LOCTEXT("Tooltip", "Bake the current level's railway splines, junctions and stations."),
            FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&Build),
                FCanExecuteAction::CreateLambda([] { return GEditor && !GEditor->PlayWorld; })));
    }

    static void Build()
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World || !World->GetCurrentLevel()) return;
        FString Message;
        const bool bSuccess = Railway::Editor::BuildData(*World->GetCurrentLevel(), Message);
        if (!bSuccess) UE_LOG(LogTemp, Error, TEXT("Build Railway Data: %s"), *Message);
        FNotificationInfo Info(FText::FromString(Message));
        Info.ExpireDuration = bSuccess ? 5.f : 10.f;
        if (auto Notification = FSlateNotificationManager::Get().AddNotification(Info))
            Notification->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
    }
};

IMPLEMENT_MODULE(FRailwayEditorModule, RailwayEditor)
#undef LOCTEXT_NAMESPACE
