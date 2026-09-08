// Copyright Akatsuki. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/** Unified log category for the Lua runtime infrastructure. */
AKLUARUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogAkLua, Log, All);

/**
 * AkLuaRuntime module.
 * On startup it registers the built-in core bridge (print/log) into the
 * global bridge registry so that every VM created afterwards picks it up.
 */
class FAkLuaRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
