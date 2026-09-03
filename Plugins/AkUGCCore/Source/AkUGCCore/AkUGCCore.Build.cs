using UnrealBuildTool;

public class AkUGCCore : ModuleRules
{
    public AkUGCCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Json",
            "JsonUtilities"
        });
    }
}
