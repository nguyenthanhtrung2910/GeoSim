#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LCCommon/LCGenerator.h"
#include "RoadGenerator.generated.h"

class AOsmRoadImporter;
class AOsmRoadScene;
class URoadStyle;
class ALandscape;
class ALandscapeSplineActor;
struct FImportedRoadWay;

/** Converts the OSMRoadImporter road cache into Road Constructor roads. */
UCLASS()
class ROADGENERATION_API ARoadGenerator : public AActor, public ILCGenerator
{
	GENERATED_BODY()
public:
	ARoadGenerator();

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Road Constructor|Import")
	TObjectPtr<AOsmRoadImporter> OSMRoadImporter;

	/** Includes lane widths, materials, sidewalk profiles, curbs and props. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Styles")
	TObjectPtr<URoadStyle> DefaultRoadStyle;

	/** Optional overrides keyed by the OSM highway value, for example residential or primary. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Styles")
	TMap<FName, TObjectPtr<URoadStyle>> HighwayStyles;

	/** Allowed OSM highway values. Clear the list to include all drivable classes; footpaths and areas remain excluded. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Import")
	TArray<FName> HighwayFilter;

	/** Additional road-to-Landscape gap applied to Road Constructor's default roadbed height offset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Placement", meta = (Units = "cm"))
	double SurfaceOffsetCm = 3.0;

	/** Maximum radius of bends through interior OSM points. Adjacent span lengths still limit the actual curve; two-point roads remain straight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape", meta = (ClampMin = "1", ClampMax = "5000000", Units = "cm"))
	double RoadCurveRadiusCm = 5000000.0;

	/** Override Road Constructor's initial link radii. Disabled uses the plugin's own initialization and corner solver. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape")
	bool bOverrideJunctionRadius = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape", meta = (ClampMin = "0", Units = "cm", EditCondition = "bOverrideJunctionRadius"))
	double JunctionRadiusCm = 1000.0;

	/** Contract short corridors between branching junctions before calling the plugin solver. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape", meta = (ClampMin = "0", Units = "cm"))
	double JunctionMergeDistanceCm = 1000.0;

	/** Maximum deviation from 180 degrees between outward XY directions. 90-degree corners remain junctions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape", meta = (ClampMin = "0", ClampMax = "45", Units = "deg", EditCondition = "bMergeStraightRoads"))
	double StraightConnectionToleranceDegrees = 10.0;
	
	/** Join compatible two-arm straight continuations before spawning Road Constructor actors. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape")
	bool bMergeStraightRoads = true;

	/** Solve junctions, remove ordinary controls inside their cut ranges, then solve again. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape")
	bool bCleanJunctionApproaches = true;

	/** Remove junction sidewalk/curb corner roads if Road Constructor fails to build the central asphalt surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape")
	bool bRemoveCornersWithoutJunctionSurface = true;

	/** Missing terrain samples split roads instead of producing long triangles across unloaded terrain. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Import")
	bool bRequireTerrainSamples = true;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Road Constructor Terrain")
	TObjectPtr<ALandscape> TargetLandscape;

	/** Create Road Constructor's Landscape spline deformation after building the roads. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor Terrain")
	bool bGenerateLandscapeSplinesWithRoads = false;

	/** Enable Road Constructor's persistent Landscape spline deformation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor Terrain")
	bool bDeformLandscape = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor Terrain")
	bool bCreateSplineLayerIfMissing = true;

	/** Relative to cached elevations; road controls use this height plus Surface Offset Cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor Terrain", meta = (Units = "cm"))
	double LandscapeHeightOffsetCm = 0.0;

	UFUNCTION(CallInEditor, Category = "Road Constructor")
	void GenerateRoads();

	UFUNCTION(CallInEditor, Category = "Road Constructor")
	void ClearRoads();

	/** Apply Road Constructor's Terrain Deform to the already generated roads. */
	UFUNCTION(CallInEditor, Category = "Road Constructor Terrain")
	void GenerateLandscapeSplines();

	UFUNCTION(CallInEditor, Category = "Road Constructor Terrain")
	void ClearLandscapeSplines();

	virtual bool OnGenerate(FName SpawnedActorsPathOverride, bool bIsUserInitiated) override;
	virtual TArray<UObject*> GetGeneratedObjects() const override;
	virtual bool Cleanup_Implementation(bool bSkipPrompt) override;

	UPROPERTY(EditInstanceOnly, AdvancedDisplay, DuplicateTransient, Category = "Road Constructor|Output")
	TSoftObjectPtr<AOsmRoadScene> GeneratedScene;

	/** Previous importer-owned spline actor, retained only so existing levels can clear it. */
	UPROPERTY()
	TSoftObjectPtr<ALandscapeSplineActor> GeneratedLandscapeSplineActor;

private:
	bool bGenerationInProgress = false;
	bool bLandscapeSplinesOnly = false;
	UPROPERTY()
	TSoftObjectPtr<ALandscape> GeneratedSplineLandscape;
	void StartGeneration(bool bOnlyLandscapeSplines);
	bool BuildRoads(const TArray<FImportedRoadWay>& Ways, FName OutputFolderPath);
	bool BuildLandscapeSplines(AOsmRoadScene* Scene);
};
