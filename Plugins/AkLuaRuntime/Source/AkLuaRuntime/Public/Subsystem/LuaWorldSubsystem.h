// Copyright Akatsuki. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "LuaWorldSubsystem.generated.h"

/**
 * ULuaWorldSubsystem
 *
 * Per-world subsystem that drives Lua's "world begin play" lifecycle from
 * UWorldSubsystem::OnWorldBeginPlay -- i.e. the moment gameplay actually
 * starts: actors have run BeginPlay and the game viewport is up. This is a
 * later, stronger guarantee than OnPostWorldInitialization (which only means
 * the world finished InitWorld()).
 *
 * It owns no VM state; it merely forwards to the GameInstance-scoped
 * ULuaScriptSubsystem, which holds the VM and the once-per-GameInstance
 * GamePlay latch. One instance is created per game world.
 */
UCLASS()
class AKLUARUNTIME_API ULuaWorldSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Only create for actual game/PIE worlds (editor/preview worlds never BeginPlay). */
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	/** Engine calls this once when the owning world begins play; we drive Lua here. */
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
};
