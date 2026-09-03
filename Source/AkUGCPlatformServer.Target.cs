using UnrealBuildTool;

public class AkUGCPlatformServerTarget : TargetRules
{
    public AkUGCPlatformServerTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Server;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        ExtraModuleNames.Add("AkUGCPlatform");
    }
}
