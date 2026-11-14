using UnrealBuildTool;

public class CursorUnrealCore : ModuleRules
{
	public CursorUnrealCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"DeveloperSettings"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"Projects",
			"DirectoryWatcher",
			"ImageCore",
			"ImageWrapper"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Credential Manager (CredReadW / CredWriteW / CredDeleteW).
			PublicSystemLibraries.Add("advapi32.lib");
			// BCryptGenRandom, for the MCP access token.
			PublicSystemLibraries.Add("bcrypt.lib");
			// DragQueryFileW, for image files pasted from the clipboard.
			PublicSystemLibraries.Add("shell32.lib");
		}
	}
}
