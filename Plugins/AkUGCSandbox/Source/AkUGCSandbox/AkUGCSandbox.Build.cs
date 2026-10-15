using UnrealBuildTool;

public class AkUGCSandbox : ModuleRules
{
	public AkUGCSandbox(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// slua_unreal 以 C++ 异常实现 lua_error；沙箱 C 回调必须提供完整展开信息。
		bEnableExceptions = true;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject" });
		// slua_unreal 的公共头文件（LuaState.h / LuaVar.h / SluaUtil.h）引用了
		// Engine/Slate/SlateCore/UMG/InputCore/NetCore 的类型，需补齐其 include 路径。
		PrivateDependencyModuleNames.AddRange(new string[] { "AkLuaRuntime", "slua_unreal", "Engine", "Slate", "SlateCore", "UMG", "InputCore", "NetCore" });
	}
}
