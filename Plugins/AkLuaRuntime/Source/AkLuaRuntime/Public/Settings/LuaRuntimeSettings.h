// Copyright Akatsuki. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "LuaRuntimeSettings.generated.h"

/**
 * Project Settings -> Plugins -> Ak Lua Runtime.
 *
 * Drives the runtime without hard-coding paths or entry points, so different
 * games reuse the same plugin yet tune their own layout.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Ak Lua Runtime"))
class AKLUARUNTIME_API ULuaRuntimeSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	ULuaRuntimeSettings();

	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	/** Entry module name resolved through the loader (returns the main callback table). */
	UPROPERTY(EditAnywhere, config, Category = "Bootstrap")
	FString EntryScript;

	/** When true the GameInstance subsystem boots the VM automatically on Initialize. */
	UPROPERTY(EditAnywhere, config, Category = "Bootstrap")
	bool bAutoBootstrap;

	/** Install every bridge found in the registry (logging, protobuf, ...). */
	UPROPERTY(EditAnywhere, config, Category = "Bootstrap")
	bool bEnableDefaultBridges;

	/** Folder name that holds Lua scripts (relative to each search root). */
	UPROPERTY(EditAnywhere, config, Category = "Script Loading")
	FString ScriptRootName;

	/** File extensions tried in order when resolving a required module. */
	UPROPERTY(EditAnywhere, config, Category = "Script Loading")
	TArray<FString> ScriptExtensions;

	/** In editor, load scripts from the project's own Lua/ folder (ProjectDir/Lua) so local
	 *  edits hot-reload without cooking; packaged builds compile them to Content/Lua .luac. */
	UPROPERTY(EditAnywhere, config, Category = "Script Loading")
	bool bUseProjectLuaDirInEditor;
};
