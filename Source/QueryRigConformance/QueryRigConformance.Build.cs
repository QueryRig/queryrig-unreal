using UnrealBuildTool;

// Editor-only module carrying the SDK conformance commandlet (spec §7).
// Never ships in a packaged game.
public class QueryRigConformance : ModuleRules
{
	public QueryRigConformance(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HTTP",
			"Json",
			"QueryRig",
		});
	}
}
