#pragma once

#include "Modules/ModuleManager.h"

class FAkUGCEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void RegisterMenus();
    TSharedRef<class SDockTab> SpawnCreatorTab(const class FSpawnTabArgs& Args);

    static const FName CreatorTabName;
};
