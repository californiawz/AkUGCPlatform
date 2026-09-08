// Copyright Akatsuki. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Templates/Function.h"

class IAkLuaBridge;

/**
 * FAkLuaBridgeRegistry
 *
 * Process-wide registry of bridge factories. Plugins register a factory under
 * a unique name during their StartupModule; a VM asks the registry to
 * instantiate a fresh set of bridges when it boots.
 *
 * This decouples the runtime core from optional capabilities: the core never
 * references protobuf/networking directly, it only iterates whatever has been
 * registered. Classic service-locator style extension point.
 *
 * Not thread-safe by design: registration happens on the game thread during
 * module load, instantiation happens on the game thread during VM boot.
 */
class AKLUARUNTIME_API FAkLuaBridgeRegistry
{
public:
	using FBridgeFactory = TFunction<TSharedRef<IAkLuaBridge>()>;

	static FAkLuaBridgeRegistry& Get();

	/** Register (or replace) a bridge factory under the given name. */
	void Register(FName BridgeName, FBridgeFactory Factory);

	/** Remove a previously registered factory. */
	void Unregister(FName BridgeName);

	bool IsRegistered(FName BridgeName) const;

	/** Instantiate one bridge per registered factory. */
	TArray<TSharedRef<IAkLuaBridge>> CreateBridges() const;

	int32 Num() const { return Factories.Num(); }

private:
	FAkLuaBridgeRegistry() = default;

	TMap<FName, FBridgeFactory> Factories;
};
