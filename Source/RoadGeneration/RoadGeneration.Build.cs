using UnrealBuildTool;

public class RoadGeneration : ModuleRules
{
    public RoadGeneration(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange([
            "Core", "CoreUObject", "Engine", "LCCommon", "RoadConstructor", "PCG"
        ]);

        PrivateDependencyModuleNames.AddRange([
            "Landscape", "SplineImporter", "Coordinates", "GDALInterface",
            "LandscapeUtils", "ConcurrencyHelpers", "Json", "XmlParser"
        ]);

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.Add("UnrealEd");
        }
    }
}
