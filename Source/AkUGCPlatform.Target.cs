using UnrealBuildTool;

public class AkUGCPlatformTarget : TargetRules
{
    public AkUGCPlatformTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        ExtraModuleNames.Add("AkUGCPlatform");
    }
}
