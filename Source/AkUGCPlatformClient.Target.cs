using UnrealBuildTool;

public class AkUGCPlatformClientTarget : TargetRules
{
    public AkUGCPlatformClientTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Client;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        ExtraModuleNames.Add("AkUGCPlatform");
    }
}
