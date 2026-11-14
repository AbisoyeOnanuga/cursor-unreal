using UnrealBuildTool;

public class CursorUnrealEditor : ModuleRules
{
	public CursorUnrealEditor(ReadOnlyTargetRules Target) : base(Target)
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
			"ApplicationCore",
			"ContentBrowser",
			"DesktopPlatform",
			"DeveloperSettings",
			"Engine",
			"Json",
			"Slate",
			"SlateCore",
			"InputCore",
			"PropertyEditor",
			"RenderCore",
			"UnrealEd",
			"ToolMenus",
			"UMG",
			"WorkspaceMenuStructure",
			"CursorUnrealAgent",
			"CursorUnrealMCP"
		});
	}
}
