using UnrealBuildTool;

public class CursorUnrealMCP : ModuleRules
{
	public CursorUnrealMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"CursorUnrealCore",
			// CursorMcpServer.h holds an FHttpRouteHandle, so anything that includes it needs these headers too.
			"HTTPServer"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Engine",
			"DeveloperSettings",
			"Json",
			"Sockets",
			"Projects"
		});
	}
}
