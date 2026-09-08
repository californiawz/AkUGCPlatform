// Copyright Akatsuki. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LuaState.h"
#include "Core/LuaCallScope.h"
#include "AkLuaRuntimeModule.h"

class IAkLuaBridge;

/**
 * FLuaVirtualMachine
 *
 * Object-oriented (non-singleton) wrapper around a single NS_SLUA::LuaState.
 * Owns the VM lifetime, wires the script loader, installs bridges on state
 * init, runs the entry script and offers a stack-safe call gateway into the
 * entry table.
 *
 * Multiple VMs may coexist (e.g. an in-process logic server VM later on);
 * because slua's onInitEvent is a shared static delegate, init dispatch is
 * filtered to the owning state.
 */
class AKLUARUNTIME_API FLuaVirtualMachine
{
public:
	FLuaVirtualMachine() = default;
	~FLuaVirtualMachine();

	FLuaVirtualMachine(const FLuaVirtualMachine&) = delete;
	FLuaVirtualMachine& operator=(const FLuaVirtualMachine&) = delete;

	/** Add a bridge. Must be called before Initialize() to take effect. */
	void AddBridge(const TSharedRef<IAkLuaBridge>& Bridge);

	/** Create and initialize the underlying LuaState. */
	bool Initialize(const FString& InName, UGameInstance* InGameInstance);

	/** Close the state, uninstall bridges, release references. Idempotent. */
	void Shutdown();

	/** Run the entry script; its returned table becomes the callback env. */
	bool LaunchEntry(const FString& EntryName);

	bool IsValid() const { return State != nullptr; }
	bool HasEntry() const { return EntryEnv.isValid(); }

	NS_SLUA::LuaState* GetState() const { return State; }
	NS_SLUA::lua_State* GetRawState() const { return State ? State->getLuaState() : nullptr; }

	/**
	 * Call a function on the entry table. Silently no-ops when the entry table
	 * or function is absent, so optional Lua callbacks stay optional. The call
	 * is wrapped in a stack scope to guarantee balance.
	 */
	template<typename... ARGS>
	void CallEntry(const char* FuncName, ARGS&&... Args)
	{
		if (!EntryEnv.isValid())
		{
			return;
		}

		NS_SLUA::LuaVar Func = EntryEnv.getFromTable<NS_SLUA::LuaVar>(FuncName);
		if (Func.isValid() && Func.isFunction())
		{
			FLuaCallScope Scope(Func.getState());
			Func.call(std::forward<ARGS>(Args)...);
		}
	}

private:
	/** Static onInitEvent dispatcher; installs bridges for our state only. */
	void HandleStateInit(NS_SLUA::lua_State* L);

	NS_SLUA::LuaState* State = nullptr;
	NS_SLUA::LuaVar EntryEnv;
	TWeakObjectPtr<UGameInstance> OwningGameInstance;
	TArray<TSharedRef<IAkLuaBridge>> Bridges;
	bool bBridgesInstalled = false;
};
