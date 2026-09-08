// Copyright Akatsuki. All Rights Reserved.

#include "Subsystem/LuaScriptSubsystem.h"
#include "Core/LuaVirtualMachine.h"
#include "Bridge/ILuaBridge.h"
#include "Bridge/LuaBridgeRegistry.h"
#include "Settings/LuaRuntimeSettings.h"
#include "AkLuaRuntimeModule.h"
#include "Engine/GameInstance.h"
#include "Engine/Engine.h"

void ULuaScriptSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const ULuaRuntimeSettings* Settings = GetDefault<ULuaRuntimeSettings>();
	if (Settings->bAutoBootstrap)
	{
		Bootstrap();
	}
}

void ULuaScriptSubsystem::Deinitialize()
{
	Teardown();
	Super::Deinitialize();
}

bool ULuaScriptSubsystem::Bootstrap()
{
	if (bBootstrapped)
	{
		return true;
	}

	const ULuaRuntimeSettings* Settings = GetDefault<ULuaRuntimeSettings>();

	VM = MakeShared<FLuaVirtualMachine>();

	if (Settings->bEnableDefaultBridges)
	{
		for (const TSharedRef<IAkLuaBridge>& Bridge : FAkLuaBridgeRegistry::Get().CreateBridges())
		{
			VM->AddBridge(Bridge);
		}
	}

	if (!VM->Initialize(TEXT("MainState"), GetGameInstance()))
	{
		VM.Reset();
		return false;
	}

	if (!VM->LaunchEntry(Settings->EntryScript))
	{
		UE_LOG(LogAkLua, Error, TEXT("[Subsystem] entry '%s' failed; tearing down."), *Settings->EntryScript);
		Teardown();
		return false;
	}

	// Hand the owning GameInstance to Lua. Game-specific environment helpers
	// (run mode/platform/command line) are exposed by the game module instead
	// of this reusable runtime plugin.
	VM->CallEntry("OnInitialize", GetGameInstance());

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &ULuaScriptSubsystem::HandleTick));

	// World "begin play" is driven by ULuaWorldSubsystem (a per-world subsystem)
	// via DispatchWorldBeginPlay, so we no longer bind OnPostWorldInitialization
	// here. Map load completion still uses PostLoadMapWithWorld.
	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
		this, &ULuaScriptSubsystem::HandlePostLoadMap);

	bBootstrapped = true;
	UE_LOG(LogAkLua, Log, TEXT("[Subsystem] Lua bootstrapped for GameInstance '%s'."),
		*GetNameSafe(GetGameInstance()));
	return true;
}

void ULuaScriptSubsystem::Teardown()
{
	if (!bBootstrapped && !VM.IsValid())
	{
		return;
	}

	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}

	if (PostLoadMapHandle.IsValid())
	{
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
		PostLoadMapHandle.Reset();
	}

	if (VM.IsValid())
	{
		VM->CallEntry("OnShutdown");
		VM->Shutdown();
		VM.Reset();
	}

	bBootstrapped = false;
}

bool ULuaScriptSubsystem::HandleTick(float DeltaTime)
{
	if (VM.IsValid())
	{
		VM->CallEntry("OnTick", DeltaTime);
	}
	return true; // keep ticking
}

void ULuaScriptSubsystem::DispatchWorldBeginPlay(UWorld* World)
{
	if (!VM.IsValid() || !World)
	{
		return;
	}

	// Only forward worlds that belong to this subsystem's GameInstance.
	if (World->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	// Forward to Lua; first call triggers Initialize+GamePlay internally.
	VM->CallEntry("OnWorldBeginPlay", World);
}

void ULuaScriptSubsystem::HandlePostLoadMap(UWorld* LoadedWorld)
{
	if (!VM.IsValid() || !LoadedWorld)
	{
		return;
	}
	if (LoadedWorld->GetGameInstance() != GetGameInstance())
	{
		return;
	}
	VM->CallEntry("OnLoadComplete", LoadedWorld);
}

ULuaScriptSubsystem* ULuaScriptSubsystem::Get(const UObject* WorldContextObject)
{
	if (!GEngine || !WorldContextObject)
	{
		return nullptr;
	}

	if (const UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull))
	{
		if (UGameInstance* GameInstance = World->GetGameInstance())
		{
			return GameInstance->GetSubsystem<ULuaScriptSubsystem>();
		}
	}
	return nullptr;
}
