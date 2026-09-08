// Copyright Akatsuki. All Rights Reserved.

#include "Core/LuaVirtualMachine.h"
#include "Bridge/ILuaBridge.h"
#include "Loader/LuaScriptLoader.h"
#include "AkLuaRuntimeModule.h"

using namespace NS_SLUA;

FLuaVirtualMachine::~FLuaVirtualMachine()
{
	Shutdown();
}

void FLuaVirtualMachine::AddBridge(const TSharedRef<IAkLuaBridge>& Bridge)
{
	if (State)
	{
		UE_LOG(LogAkLua, Warning, TEXT("[VM] AddBridge('%s') ignored: VM already initialized."),
			*Bridge->GetBridgeName().ToString());
		return;
	}
	Bridges.AddUnique(Bridge);
}

bool FLuaVirtualMachine::Initialize(const FString& InName, UGameInstance* InGameInstance)
{
	if (State)
	{
		UE_LOG(LogAkLua, Warning, TEXT("[VM] '%s' already initialized."), *InName);
		return true;
	}

	OwningGameInstance = InGameInstance;
	bBridgesInstalled = false;

	State = new LuaState(TCHAR_TO_UTF8(*InName), InGameInstance);

	// onInitEvent is a shared static multicast; HandleStateInit filters to ours.
	LuaState::onInitEvent.AddRaw(this, &FLuaVirtualMachine::HandleStateInit);
	State->setLoadFileDelegate(&FLuaScriptLoader::LoadLuaModule);

	if (!State->init())
	{
		UE_LOG(LogAkLua, Error, TEXT("[VM] '%s' failed to initialize."), *InName);
		Shutdown();
		return false;
	}

	UE_LOG(LogAkLua, Log, TEXT("[VM] '%s' initialized with %d bridge(s)."), *InName, Bridges.Num());
	return true;
}

void FLuaVirtualMachine::HandleStateInit(lua_State* L)
{
	// The static delegate fires for every state; only react to our own.
	if (!State || LuaState::get(L) != State || bBridgesInstalled)
	{
		return;
	}

	for (const TSharedRef<IAkLuaBridge>& Bridge : Bridges)
	{
		Bridge->Install(L);
		UE_LOG(LogAkLua, Verbose, TEXT("[VM] bridge installed: %s"), *Bridge->GetBridgeName().ToString());
	}
	bBridgesInstalled = true;
}

bool FLuaVirtualMachine::LaunchEntry(const FString& EntryName)
{
	if (!State)
	{
		UE_LOG(LogAkLua, Error, TEXT("[VM] LaunchEntry('%s') on an uninitialized VM."), *EntryName);
		return false;
	}

	EntryEnv = State->doFile(TCHAR_TO_UTF8(*EntryName));
	if (!EntryEnv.isValid())
	{
		UE_LOG(LogAkLua, Error, TEXT("[VM] entry script '%s' did not return a table."), *EntryName);
		return false;
	}

	UE_LOG(LogAkLua, Log, TEXT("[VM] entry '%s' launched."), *EntryName);
	return true;
}

void FLuaVirtualMachine::Shutdown()
{
	if (!State)
	{
		return;
	}

	lua_State* L = State->getLuaState();
	for (int32 i = Bridges.Num() - 1; i >= 0; --i)
	{
		Bridges[i]->Uninstall(L);
	}

	LuaState::onInitEvent.RemoveAll(this);

	EntryEnv = LuaVar();
	State->close();
	// Mirrors slua's expected teardown: close() releases VM resources; the
	// object itself is managed by slua's GC bookkeeping, so we drop the pointer
	// rather than delete it to avoid a double-free.
	State = nullptr;

	bBridgesInstalled = false;
	Bridges.Empty();
	OwningGameInstance.Reset();
}
