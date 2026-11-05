using UnrealBuildTool;

public class ComputerVision : ModuleRules
{
	public ComputerVision(ReadOnlyTargetRules target) : base(target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange([
			"Core", "CoreUObject", "Engine", "ImageWrapper", "RenderCore", "RHI"
		]);
	}
}
