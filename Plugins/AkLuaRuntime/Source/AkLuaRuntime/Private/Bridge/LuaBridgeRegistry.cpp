// Copyright Akatsuki. All Rights Reserved.

#include "Bridge/LuaBridgeRegistry.h"
#include "Bridge/ILuaBridge.h"
#include "AkLuaRuntimeModule.h"

FAkLuaBridgeRegistry& FAkLuaBridgeRegistry::Get()
{
	static FAkLuaBridgeRegistry Instance;
	return Instance;
}

void FAkLuaBridgeRegistry::Register(FName BridgeName, FBridgeFactory Factory)
{
	if (BridgeName.IsNone() || !Factory)
	{
		UE_LOG(LogAkLua, Warning, TEXT("[BridgeRegistry] ignored invalid registration."));
		return;
	}

	if (Factories.Contains(BridgeName))
	{
		UE_LOG(LogAkLua, Warning, TEXT("[BridgeRegistry] '%s' already registered, overriding."), *BridgeName.ToString());
	}

	Factories.Add(BridgeName, MoveTemp(Factory));
	UE_LOG(LogAkLua, Verbose, TEXT("[BridgeRegistry] registered '%s'."), *BridgeName.ToString());
}

void FAkLuaBridgeRegistry::Unregister(FName BridgeName)
{
	Factories.Remove(BridgeName);
}

bool FAkLuaBridgeRegistry::IsRegistered(FName BridgeName) const
{
	return Factories.Contains(BridgeName);
}

TArray<TSharedRef<IAkLuaBridge>> FAkLuaBridgeRegistry::CreateBridges() const
{
	TArray<TSharedRef<IAkLuaBridge>> Result;
	Result.Reserve(Factories.Num());
	for (const TPair<FName, FBridgeFactory>& Pair : Factories)
	{
		if (Pair.Value)
		{
			Result.Add(Pair.Value());
		}
	}
	return Result;
}
