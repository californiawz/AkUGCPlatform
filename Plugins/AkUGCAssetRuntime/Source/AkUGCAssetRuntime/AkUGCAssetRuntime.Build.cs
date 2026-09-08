using UnrealBuildTool;

public class AkUGCAssetRuntime : ModuleRules
{
    public AkUGCAssetRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "AkUGCCore",
            "AkUGCSandbox"
        });
    }
}
