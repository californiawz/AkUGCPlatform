// Copyright Akatsuki. All Rights Reserved.

using UnrealBuildTool;

public class AkLuaRuntime : ModuleRules
{
	public AkLuaRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			// Lua VM provider. Public because our headers expose slua types (LuaState/LuaVar).
			"slua_unreal",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// UDeveloperSettings for project-settings driven configuration.
			"DeveloperSettings",
		});

		PublicIncludePathModuleNames.AddRange(new string[] { "slua_unreal" });
	}
}
