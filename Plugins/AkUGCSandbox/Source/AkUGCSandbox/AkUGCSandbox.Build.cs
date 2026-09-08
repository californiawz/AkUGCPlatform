using UnrealBuildTool;

public class AkUGCSandbox : ModuleRules
{
	public AkUGCSandbox(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject" });
		// slua_unreal 的公共头文件（LuaState.h / LuaVar.h / SluaUtil.h）引用了
		// Engine/Slate/SlateCore/UMG/InputCore/NetCore 的类型，需补齐其 include 路径。
		PrivateDependencyModuleNames.AddRange(new string[] { "slua_unreal", "Engine", "Slate", "SlateCore", "UMG", "InputCore", "NetCore" });
	}
}
