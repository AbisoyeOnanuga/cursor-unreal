using UnrealBuildTool;

public class CursorUnrealDevEditorTarget : TargetRules
{
	public CursorUnrealDevEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("CursorUnrealDev");
	}
}
