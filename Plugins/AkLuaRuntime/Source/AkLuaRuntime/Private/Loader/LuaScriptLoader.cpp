// Copyright Akatsuki. All Rights Reserved.

#include "Loader/LuaScriptLoader.h"
#include "Settings/LuaRuntimeSettings.h"
#include "AkLuaRuntimeModule.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
	bool IsSafePathSegment(const FString& Segment)
	{
		if (Segment.IsEmpty())
		{
			return false;
		}
		for (const TCHAR Character : Segment)
		{
			if (!((Character >= TEXT('a') && Character <= TEXT('z'))
				|| (Character >= TEXT('A') && Character <= TEXT('Z'))
				|| (Character >= TEXT('0') && Character <= TEXT('9'))
				|| Character == TEXT('_')
				|| Character == TEXT('-')))
			{
				return false;
			}
		}
		return true;
	}

	bool TryNormalizeModuleName(const char* ModuleName, FString& OutRelative)
	{
		OutRelative.Reset();
		if (!ModuleName || !*ModuleName)
		{
			return false;
		}
		const FString Name = UTF8_TO_TCHAR(ModuleName);
		if (Name.IsEmpty() || Name.Len() > 256)
		{
			return false;
		}
		TArray<FString> Segments;
		Name.ParseIntoArray(Segments, TEXT("."), false);
		if (Segments.IsEmpty() || Segments.Num() > 32)
		{
			return false;
		}
		for (const FString& Segment : Segments)
		{
			if (!IsSafePathSegment(Segment))
			{
				return false;
			}
		}
		OutRelative = FString::Join(Segments, TEXT("/"));
		return true;
	}

	bool IsSafeExtension(const FString& Extension)
	{
		return Extension.Len() >= 2
			&& Extension.Len() <= 16
			&& Extension[0] == TEXT('.')
			&& IsSafePathSegment(Extension.Mid(1));
	}

	bool ReadBoundedModule(const FString& Path, TArray<uint8>& OutContent)
	{
		OutContent.Reset();
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
		const int64 Size = Reader ? Reader->TotalSize() : -1;
		if (Size <= 0
			|| Size > FLuaScriptLoader::MaxModuleBytes
			|| Size > MAX_int32)
		{
			return false;
		}
		OutContent.SetNumUninitialized(static_cast<int32>(Size));
		Reader->Serialize(OutContent.GetData(), Size);
		if (Reader->IsError() || Reader->Tell() != Size)
		{
			OutContent.Reset();
			return false;
		}
		return true;
	}
}

TArray<FString> FLuaScriptLoader::ResolveSearchRoots()
{
	const ULuaRuntimeSettings* Settings = GetDefault<ULuaRuntimeSettings>();
	const FString RootName = Settings->ScriptRootName.IsEmpty() ? TEXT("Lua") : Settings->ScriptRootName;
	if (!IsSafePathSegment(RootName))
	{
		UE_LOG(LogAkLua, Error, TEXT("[Loader] rejected invalid script root name"));
		return {};
	}

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
	OutFullPath.Reset();

	// require("a.b.c") -> a/b/c
	FString Relative;
	if (!TryNormalizeModuleName(ModuleName, Relative))
	{
		UE_LOG(LogAkLua, Warning, TEXT("[Loader] rejected invalid module name"));
		return {};
	}

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
			if (!IsSafeExtension(Ext))
			{
				continue;
			}
			FString NormalizedRoot = FPaths::ConvertRelativePathToFull(Root);
			FPaths::NormalizeDirectoryName(NormalizedRoot);
			FString FullPath = FPaths::ConvertRelativePathToFull(
				FPaths::Combine(NormalizedRoot, Relative + Ext));
			FPaths::NormalizeFilename(FullPath);
			if (FPaths::IsUnderDirectory(FullPath, NormalizedRoot)
				&& ReadBoundedModule(FullPath, Content))
			{
				OutFullPath = FullPath;
				return MoveTemp(Content);
			}
			Content.Reset();
		}
	}

	UE_LOG(LogAkLua, Warning, TEXT("[Loader] module not found: %s"), *Relative);
	return Content;
}
