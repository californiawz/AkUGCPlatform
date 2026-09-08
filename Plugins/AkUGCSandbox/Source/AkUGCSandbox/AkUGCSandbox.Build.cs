using UnrealBuildTool;

public class AkUGCSandbox : ModuleRules
{
	public AkUGCSandbox(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject" });
		PrivateDependencyModuleNames.AddRange(new string[] { "slua_unreal" });
	}
}
