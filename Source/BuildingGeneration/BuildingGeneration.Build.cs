using UnrealBuildTool;

public class BuildingGeneration : ModuleRules
{
	public BuildingGeneration(ReadOnlyTargetRules target) : base(target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(["Core", "CoreUObject", "Engine", "PCG"]);
	}
}
