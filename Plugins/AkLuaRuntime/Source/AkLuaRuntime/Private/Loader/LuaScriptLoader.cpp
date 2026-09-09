// Copyright Akatsuki. All Rights Reserved.

#include "Loader/LuaScriptLoader.h"
#include "Settings/LuaRuntimeSettings.h"
#include "AkLuaRuntimeModule.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

TArray<FString> FLuaScriptLoader::ResolveSearchRoots()
{
	const ULuaRuntimeSettings* Settings = GetDefault<ULuaRuntimeSettings>();
	const FString RootName = Settings->ScriptRootName.IsEmpty() ? TEXT("Lua") : Settings->ScriptRootName;

	TArray<FString> Roots;

	// In editor prefer the project's own Lua source folder so local edits and newly added
	// modules are testable without cooking. Packaged builds instead load the compiled
	// .luac emitted into Content/Lua by the build pipeline.
#if WITH_EDITOR
	if (Settings->bUseProjectLuaDirInEditor)
	{
		const FString ProjectLuaDir = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), RootName));
		Roots.Add(ProjectLuaDir);
	}
#endif

	// Packaged builds still prefer downloaded hot-update scripts over cooked content.
	Roots.Add(FPaths::Combine(FPaths::ProjectPersistentDownloadDir(), RootName));
	Roots.Add(FPaths::Combine(FPaths::ProjectContentDir(), RootName));

	return Roots;
}

TArray<uint8> FLuaScriptLoader::LoadLuaModule(const char* ModuleName, FString& OutFullPath)
{
	const ULuaRuntimeSettings* Settings = GetDefault<ULuaRuntimeSettings>();

	// require("a.b.c") -> a/b/c
	FString Relative = UTF8_TO_TCHAR(ModuleName);
	Relative.ReplaceInline(TEXT("."), TEXT("/"));

	TArray<FString> Extensions = Settings->ScriptExtensions;
	if (Extensions.Num() == 0)
	{
		Extensions = { TEXT(".lua"), TEXT(".luac") };
	}

	const TArray<FString> Roots = ResolveSearchRoots();

	TArray<uint8> Content;
	for (const FString& Root : Roots)
	{
		for (const FString& Ext : Extensions)
		{
			const FString FullPath = FPaths::Combine(Root, Relative + Ext);
			if (FPaths::FileExists(FullPath)
				&& FFileHelper::LoadFileToArray(Content, *FullPath)
				&& Content.Num() > 0)
			{
				OutFullPath = FullPath;
				return MoveTemp(Content);
			}
			Content.Reset();
		}
	}

	UE_LOG(LogAkLua, Warning, TEXT("[Loader] module not found: %hs"), ModuleName);
	return Content;
}
