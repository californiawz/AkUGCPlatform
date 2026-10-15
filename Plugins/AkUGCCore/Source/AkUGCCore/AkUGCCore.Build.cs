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

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "SSL",
            "OpenSSL"
        });

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicSystemLibraries.Add("Crypt32.lib");
        }

        if (Target.Platform == UnrealTargetPlatform.Win64 && !Target.WindowsPlatform.bUseXCurl)
        {
            AddEngineThirdPartyPrivateStaticDependencies(Target, "libcurl");
            PrivateDefinitions.Add("AKUGC_HTTP_CURL=1");
        }
        else
        {
            PrivateDefinitions.Add("AKUGC_HTTP_CURL=0");
        }
    }
}
