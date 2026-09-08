// Copyright Akatsuki. All Rights Reserved.

#include "AkLuaRuntimeModule.h"
#include "Bridge/LuaBridgeRegistry.h"
#include "Bridge/LuaCoreBridge.h"

DEFINE_LOG_CATEGORY(LogAkLua);

void FAkLuaRuntimeModule::StartupModule()
{
	// Self-register the always-available core bridge. Optional bridges
	// (e.g. protobuf) live in their own plugins and register the same way,
	// so the runtime core never needs to know about them.
	FAkLuaBridgeRegistry::Get().Register(
		FLuaCoreBridge::BridgeName,
		[]() -> TSharedRef<IAkLuaBridge> { return MakeShared<FLuaCoreBridge>(); });

	UE_LOG(LogAkLua, Log, TEXT("AkLuaRuntime started."));
}

void FAkLuaRuntimeModule::ShutdownModule()
{
	FAkLuaBridgeRegistry::Get().Unregister(FLuaCoreBridge::BridgeName);
	UE_LOG(LogAkLua, Log, TEXT("AkLuaRuntime shut down."));
}

IMPLEMENT_MODULE(FAkLuaRuntimeModule, AkLuaRuntime)
