using UnrealBuildTool;

public class RoadGeneration : ModuleRules
{
    public RoadGeneration(ReadOnlyTargetRules target) : base(target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange([
            "Core", "CoreUObject", "Engine", "LCCommon", "RoadConstructor", "PCG"
        ]);

        PrivateDependencyModuleNames.AddRange([
            "Landscape", "SplineImporter", "Coordinates", "GDALInterface",
            "LandscapeUtils", "ConcurrencyHelpers", "Json", "XmlParser"
        ]);

        if (target.bBuildEditor)
        {
            PrivateDependencyModuleNames.Add("UnrealEd");
        }
    }
}
