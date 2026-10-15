// Copyright Akatsuki. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FLuaScriptLoader
 *
 * Resolves `require("a.b.c")` to bytes on disk. Search roots are derived from
 * ULuaRuntimeSettings, in priority order:
 *   1) PersistentDownloadDir/<Root>/   (hot-update / patched scripts win)
 *   2) project-sibling ../<Root>/      (editor only, hot-reload friendly)
 *   3) ProjectContentDir/<Root>/       (cooked / packaged scripts)
 *
 * Exposed as static functions because slua's LoadFileDelegate is a plain
 * function pointer (no captured state allowed).
 */
class AKLUARUNTIME_API FLuaScriptLoader
{
public:
	/** Hard cap applied before allocating or reading one source/bytecode module. */
	static constexpr int64 MaxModuleBytes = 4 * 1024 * 1024;

	/** Matches NS_SLUA::LuaState::LoadFileDelegate. */
	static TArray<uint8> LoadLuaModule(const char* ModuleName, FString& OutFullPath);

	/** Ordered list of absolute directories scripts are searched in. */
	static TArray<FString> ResolveSearchRoots();
};
