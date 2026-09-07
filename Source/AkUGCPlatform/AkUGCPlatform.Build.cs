using UnrealBuildTool;

public class AkUGCPlatform : ModuleRules
{
    public AkUGCPlatform(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "AkUGCCore",
            "AkUGCAssetRuntime"
        });
    }
}
