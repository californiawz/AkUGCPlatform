#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Loader/LuaScriptLoader.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Settings/LuaRuntimeSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLuaScriptLoaderInputBudgetTest,
	"AkUGC.Lua.ScriptLoader.InputBudgetAndPathContainment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLuaScriptLoaderInputBudgetTest::RunTest(const FString& Parameters)
{
	const ULuaRuntimeSettings* Settings = GetDefault<ULuaRuntimeSettings>();
	const FString RootName =
		Settings->ScriptRootName.IsEmpty() ? TEXT("Lua") : Settings->ScriptRootName;
	const FString Root = FPaths::Combine(
		FPaths::ProjectPersistentDownloadDir(),
		RootName);
	IFileManager::Get().MakeDirectory(*Root, true);

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ValidModule = TEXT("AkUGCLoaderFixture_") + Suffix;
	const FString ValidPath = FPaths::Combine(Root, ValidModule + TEXT(".lua"));
	const TArray<uint8> Expected = {0x41, 0x6B, 0x55, 0x47, 0x43};
	TestTrue(
		TEXT("Bounded Lua fixture is written"),
		FFileHelper::SaveArrayToFile(Expected, *ValidPath));

	FString LoadedPath = TEXT("stale");
	const TArray<uint8> Loaded = FLuaScriptLoader::LoadLuaModule(
		TCHAR_TO_UTF8(*ValidModule),
		LoadedPath);
	TestEqual(TEXT("Bounded Lua module bytes load exactly"), Loaded, Expected);
	TestEqual(
		TEXT("Lua loader reports the contained file"),
		FPaths::ConvertRelativePathToFull(LoadedPath),
		FPaths::ConvertRelativePathToFull(ValidPath));

	const FString OversizedModule = TEXT("AkUGCLoaderOversized_") + Suffix;
	const FString OversizedPath =
		FPaths::Combine(Root, OversizedModule + TEXT(".lua"));
	TArray<uint8> Oversized;
	Oversized.SetNumZeroed(
		static_cast<int32>(FLuaScriptLoader::MaxModuleBytes + 1));
	TestTrue(
		TEXT("Oversized Lua fixture is written"),
		FFileHelper::SaveArrayToFile(Oversized, *OversizedPath));
	Oversized.Reset();
	LoadedPath = TEXT("stale");
	TestTrue(
		TEXT("Oversized Lua module is rejected before reading"),
		FLuaScriptLoader::LoadLuaModule(
			TCHAR_TO_UTF8(*OversizedModule),
			LoadedPath).IsEmpty());
	TestTrue(TEXT("Rejected oversized module clears its output path"),
		LoadedPath.IsEmpty());

	LoadedPath = TEXT("stale");
	TestTrue(
		TEXT("Traversal-like Lua module names are rejected"),
		FLuaScriptLoader::LoadLuaModule(
			"../AkUGCOutsideFixture",
			LoadedPath).IsEmpty());
	TestTrue(TEXT("Rejected traversal clears its output path"),
		LoadedPath.IsEmpty());

	IFileManager::Get().Delete(*ValidPath, false, true);
	IFileManager::Get().Delete(*OversizedPath, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
