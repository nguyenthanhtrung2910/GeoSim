using UnrealBuildTool;

public class BuildingGeneration : ModuleRules
{
	public BuildingGeneration(ReadOnlyTargetRules target) : base(target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange([
			"Core", "CoreUObject", "Engine", "PCG", "LCCommon", "Vitruvio"
		]);
		PrivateDependencyModuleNames.AddRange([
			"XmlParser", "OSMUserData", "Coordinates", "Landscape", "GDALInterface", "LandscapeUtils", "SplineImporter", "ConcurrencyHelpers"
		]);
		if (target.bBuildEditor) PrivateDependencyModuleNames.Add("UnrealEd");
	}
}
