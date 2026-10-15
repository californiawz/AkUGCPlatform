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
            "OpenSSL",
            "Json",
            "InputCore",
            "LevelEditor",
            "Slate",
            "SlateCore",
            "ToolMenus",
            "UnrealEd"
        });
    }
}
