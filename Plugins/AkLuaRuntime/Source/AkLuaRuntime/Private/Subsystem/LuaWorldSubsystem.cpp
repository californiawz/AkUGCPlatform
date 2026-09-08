// Copyright Akatsuki. All Rights Reserved.

#include "Subsystem/LuaWorldSubsystem.h"
#include "Subsystem/LuaScriptSubsystem.h"
#include "Engine/World.h"

bool ULuaWorldSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	// Restrict to real game worlds (Game / PIE). Editor, preview and inactive
	// worlds never call OnWorldBeginPlay, so creating the subsystem there is
	// pointless.
	if (const UWorld* World = Cast<UWorld>(Outer))
	{
		return World->IsGameWorld();
	}
	return false;
}

void ULuaWorldSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// Forward to the GameInstance-scoped script subsystem, which owns the VM and
	// the once-per-GameInstance GamePlay latch. ULuaScriptSubsystem::Get does
	// the GameInstance lookup from the world context.
	if (ULuaScriptSubsystem* Script = ULuaScriptSubsystem::Get(&InWorld))
	{
		Script->DispatchWorldBeginPlay(&InWorld);
	}
}
