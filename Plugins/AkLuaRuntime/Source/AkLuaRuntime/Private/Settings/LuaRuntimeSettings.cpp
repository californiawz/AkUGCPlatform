// Copyright Akatsuki. All Rights Reserved.

#include "Settings/LuaRuntimeSettings.h"

ULuaRuntimeSettings::ULuaRuntimeSettings()
	: EntryScript(TEXT("Main"))
	, bAutoBootstrap(true)
	, bEnableDefaultBridges(true)
	, ScriptRootName(TEXT("Lua"))
	, bUseSiblingDirInEditor(true)
{
	ScriptExtensions = { TEXT(".lua"), TEXT(".luac") };
}
