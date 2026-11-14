using UnrealBuildTool;

public class CursorUnrealTools : ModuleRules
{
	public CursorUnrealTools(ReadOnlyTargetRules Target) : base(Target)
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
			"JsonUtilities",
			"UnrealEd",
			"AssetRegistry",
			"AssetTools",
			"ContentBrowser",
			"Slate",
			"SlateCore",
			"Projects",
			"PythonScriptPlugin",
			// Blueprint pack: K2 node classes, the compiler log, and the schema.
			"BlueprintGraph",
			"KismetCompiler",
			"Kismet",
			// Material pack: UMaterialEditingLibrary. RHI for GMaxRHIFeatureLevel at compile time.
			"MaterialEditor",
			"RHI",
			// Animation pack: UAnimBlueprint / factory (UnrealEd). AnimGraph for schema types used by graphs.
			"AnimGraph",
			// Audio pack: SoundCue factory and editor preview playback.
			"AudioEditor"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// cpp_build asks the running editor's Live Coding module to compile.
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
