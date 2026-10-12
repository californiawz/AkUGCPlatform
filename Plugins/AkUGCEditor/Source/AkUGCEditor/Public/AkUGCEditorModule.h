#pragma once

#include "Modules/ModuleManager.h"

class FAkUGCEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    virtual bool SupportsDynamicReloading() override { return false; }

private:
    void RegisterMenus();
    TSharedRef<class SDockTab> SpawnCreatorTab(const class FSpawnTabArgs& Args);

    TArray<TWeakPtr<class SDockTab>> CreatorTabs;
    static const FName CreatorTabName;
};
