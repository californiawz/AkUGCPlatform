#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FAkUGCSandboxModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FAkUGCSandboxModule, AkUGCSandbox)
