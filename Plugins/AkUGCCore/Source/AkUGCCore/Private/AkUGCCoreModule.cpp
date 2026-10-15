#include "AkUGCCoreModule.h"

#include "Network/AkUGCHttpTransport.h"

void FAkUGCCoreModule::StartupModule()
{
#if !WITH_EDITOR
	FAkUGCHttpTransport::Initialize();
#endif
}

void FAkUGCCoreModule::ShutdownModule()
{
#if !WITH_EDITOR
	FAkUGCHttpTransport::Shutdown();
#endif
}

IMPLEMENT_MODULE(FAkUGCCoreModule, AkUGCCore)
