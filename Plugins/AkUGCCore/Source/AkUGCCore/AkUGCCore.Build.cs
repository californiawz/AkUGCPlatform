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

        PrivateDependencyModuleNames.Add("OpenSSL");

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicSystemLibraries.Add("Crypt32.lib");
        }
    }
}
