using UnrealBuildTool;

public class AkUGCEditor : ModuleRules
{
    public AkUGCEditor(ReadOnlyTargetRules Target) : base(Target)
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

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "EditorSubsystem",
            "ApplicationCore",
            "HTTP",
            "SSL",
            "Json",
            "InputCore",
            "LevelEditor",
            "Slate",
            "SlateCore",
            "ToolMenus",
            "UnrealEd"
        });
        if (Target.Platform == UnrealTargetPlatform.Win64 && !Target.WindowsPlatform.bUseXCurl)
        {
            AddEngineThirdPartyPrivateStaticDependencies(Target, "libcurl");
            PrivateDefinitions.Add("AKUGC_COPILOT_CURL=1");
        }
        else
        {
            PrivateDefinitions.Add("AKUGC_COPILOT_CURL=0");
        }
    }
}
