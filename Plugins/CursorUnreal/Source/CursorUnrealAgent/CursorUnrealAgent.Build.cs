using UnrealBuildTool;

public class CursorUnrealAgent : ModuleRules
{
	public CursorUnrealAgent(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Json",
			"CursorUnrealCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Engine",
			"DeveloperSettings",
			"HTTP",
			"Projects"
		});
	}
}
