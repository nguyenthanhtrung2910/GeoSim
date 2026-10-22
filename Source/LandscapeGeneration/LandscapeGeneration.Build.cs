using UnrealBuildTool;

public class LandscapeGeneration : ModuleRules
{
	public LandscapeGeneration(ReadOnlyTargetRules target) : base(target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;
		PublicDependencyModuleNames.AddRange([
			"Core", "CoreUObject", "Engine", "LandscapeCombinator", "ImageDownloader",
			"Landscape", "Coordinates", "LCCommon", "ConcurrencyHelpers", "HeightmapModifier", "ConsoleHelpers"
		]);
		PrivateDependencyModuleNames.AddRange([
			"GDALInterface", "FileDownloader", "Projects", "GeometryCore", "MapboxHelpers", "LandscapeUtils", "SlateCore"
		]);
		if (target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(["UnrealEd", "EditorFramework", "LandscapeEditor"]);
		}
	}
}
