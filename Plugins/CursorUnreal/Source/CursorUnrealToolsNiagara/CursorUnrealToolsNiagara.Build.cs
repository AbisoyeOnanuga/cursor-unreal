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
			"AssetRegistry",
			"Slate",
			"SlateCore",
			// Asset creation rules and path resolution shared with the other packs.
			"CursorUnrealTools",
			"Niagara",
			"NiagaraCore",
			"NiagaraEditor"
		});
	}
}
