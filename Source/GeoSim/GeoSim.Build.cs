// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class GeoSim : ModuleRules
{
	public GeoSim(ReadOnlyTargetRules target) : base(target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(["Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "PCG", "ImageWrapper", "RenderCore", "Renderer", "RHI", "UMG"]);
        PublicDependencyModuleNames.Add("LCCommon");
        //PrivateDependencyModuleNames.AddRange(["ArcGISMapsSDK", "GeoReferencing", "Landscape", "SplineImporter", "Coordinates", "GDALInterface", "LCCommon", "ConcurrencyHelpers", "OSMUserData", "Vitruvio", "ImageCore"]);
        PrivateDependencyModuleNames.AddRange(["GeoReferencing", "Landscape", "SplineImporter", "Coordinates", "GDALInterface", "ConcurrencyHelpers", "OSMUserData", "Vitruvio", "ImageCore"]);
        PrivateDependencyModuleNames.AddRange(["HTTP", "Json", "Slate", "SlateCore"]);

        // ArcGISMapActor.h includes SDK headers located under the plugin's Internal directory.
        // PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory("ArcGISMapsSDK"), "Internal"));

        if (!Target.bBuildEditor) return;
        PrivateDependencyModuleNames.Add("UnrealEd");
        PrivateDependencyModuleNames.Add("PropertyEditor");

        // Uncomment if you are using Slate UI
        // PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

        // Uncomment if you are using online features
        // PrivateDependencyModuleNames.Add("OnlineSubsystem");

        // To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
    }
}
