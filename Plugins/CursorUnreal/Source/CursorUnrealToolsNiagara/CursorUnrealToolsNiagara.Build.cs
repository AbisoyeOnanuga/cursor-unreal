using UnrealBuildTool;

public class CursorUnrealToolsNiagara : ModuleRules
{
	public CursorUnrealToolsNiagara(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"CursorUnrealCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Engine",
			"Json",
			"UnrealEd",
			"Niagara",
			"NiagaraEditor"
		});
	}
}
