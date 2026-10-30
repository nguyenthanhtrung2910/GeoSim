using UnrealBuildTool;

public class BuildingGeneration : ModuleRules
{
	public BuildingGeneration(ReadOnlyTargetRules target) : base(target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange([
			"Core", "CoreUObject", "Engine", "PCG", "SplineImporter", "LCCommon",
			"Coordinates", "Landscape", "GDALInterface", "ConcurrencyHelpers"
		]);
		PrivateDependencyModuleNames.AddRange(["XmlParser", "OSMUserData"]);
	}
}
