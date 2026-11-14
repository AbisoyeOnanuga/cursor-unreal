using UnrealBuildTool;

public class CursorUnrealDevTarget : TargetRules
{
	public CursorUnrealDevTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("CursorUnrealDev");
	}
}
