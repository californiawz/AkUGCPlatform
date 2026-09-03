using UnrealBuildTool;

public class AkUGCPlatformEditorTarget : TargetRules
{
    public AkUGCPlatformEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        ExtraModuleNames.Add("AkUGCPlatform");
    }
}
