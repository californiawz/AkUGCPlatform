#include "AkUGCEditorModule.h"

#include "Editor.h"
#include "Subsystem/AkUGCEditorSubsystem.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SAkUGCCreatorPanel.h"

const FName FAkUGCEditorModule::CreatorTabName(TEXT("AkUGCCreator"));

void FAkUGCEditorModule::StartupModule()
{
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        CreatorTabName,
        FOnSpawnTab::CreateRaw(this, &FAkUGCEditorModule::SpawnCreatorTab))
        .SetDisplayName(FText::FromString(TEXT("AkUGC Creator Studio")))
        .SetMenuType(ETabSpawnerMenuType::Hidden);

    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FAkUGCEditorModule::RegisterMenus));
}

void FAkUGCEditorModule::ShutdownModule()
{
    UToolMenus::UnRegisterStartupCallback(this);
    UToolMenus::UnregisterOwner(this);
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(CreatorTabName);
}

void FAkUGCEditorModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"));
    FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("WindowLayout"));
    Section.AddMenuEntry(
        TEXT("OpenAkUGCCreator"),
        FText::FromString(TEXT("AkUGC Creator Studio")),
        FText::FromString(TEXT("Open the AkUGC Creator Studio panel.")),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([]()
        {
            FGlobalTabmanager::Get()->TryInvokeTab(CreatorTabName);
        })));
}

TSharedRef<SDockTab> FAkUGCEditorModule::SpawnCreatorTab(const FSpawnTabArgs& Args)
{
    UAkUGCEditorSubsystem* Subsystem = GEditor
        ? GEditor->GetEditorSubsystem<UAkUGCEditorSubsystem>()
        : nullptr;

    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            SNew(SAkUGCCreatorPanel, Subsystem)
        ];
}

IMPLEMENT_MODULE(FAkUGCEditorModule, AkUGCEditor)
