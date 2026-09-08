// Copyright Akatsuki. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "LuaScriptSubsystem.generated.h"

class FLuaVirtualMachine;

/**
 * ULuaScriptSubsystem
 *
 * The zero-touch entry point of the runtime. Because it is a
 * UGameInstanceSubsystem, merely enabling the plugin gives any game a Lua VM
 * bound to the GameInstance lifetime -- no custom UGameInstance subclass and
 * no engine.ini wiring required.
 *
 * Lifecycle forwarded to the Lua entry table (all optional):
 *   - OnInitialize(gameInstance)     VM booted, GameInstance available
 *   - OnTick(deltaSeconds)
 *   - OnWorldBeginPlay(world)        a world has begun play (driven by
 *                                    ULuaWorldSubsystem::OnWorldBeginPlay;
 *                                    first call triggers Initialize+GamePlay)
 *   - OnLoadComplete(world)          a map finished loading in the given world
 *   - OnShutdown()
 */
UCLASS()
class AKLUARUNTIME_API ULuaScriptSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Convenience accessor from any world context. */
	static ULuaScriptSubsystem* Get(const UObject* WorldContextObject);

	/** The managed VM (valid only after a successful bootstrap). */
	TSharedPtr<FLuaVirtualMachine> GetVirtualMachine() const { return VM; }

	/** Manually boot the VM (used when bAutoBootstrap is disabled). Idempotent. */
	bool Bootstrap();

	/** Tear the VM down ahead of Deinitialize if needed. Idempotent. */
	void Teardown();

	/**
	 * Drive the Lua "world begin play" lifecycle for the given world. Called by
	 * ULuaWorldSubsystem::OnWorldBeginPlay. Forwards OnWorldBeginPlay(world).
	 * First call triggers Lua-side Initialize+GamePlay startup flow internally.
	 */
	void DispatchWorldBeginPlay(UWorld* World);

private:
	bool HandleTick(float DeltaTime);
	void HandlePostLoadMap(UWorld* LoadedWorld);

	TSharedPtr<FLuaVirtualMachine> VM;
	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle PostLoadMapHandle;
	bool bBootstrapped = false;
};
