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

namespace RoadGeneratorInternal
{
	struct FImportSpan
	{
		int64 WayId = 0;
		TArray<int64> SourceWayIds;
		FName Highway;
		/** Zero means retain the selected highway style's driving lanes. */
		int32 LaneCount = 0;
		TArray<FVector> Positions;
		FString StartKey;
		FString EndKey;
	};

	TArray<FImportSpan> PrepareSpans(const TArray<FImportedRoadWay>& Ways, const TArray<FName>& Filter, bool bRequireTerrain, double HeightOffset, double MergeDistance = 1000.0);
	/** Always copies the base style; absent/invalid lane counts preserve its layout. */
	URoadStyle* CreateLaneCountStyle(UObject* Outer, const URoadStyle* Source, int32 LaneCount);
	int32 MergeStraightSpans(TArray<FImportSpan>& Spans, double AngleTolerance, const TFunctionRef<URoadStyle*(const FImportSpan&)>& ResolveStyle);
}

/** Converts the OSMRoadImporter road cache into Road Constructor roads. */
UCLASS()
class ROADGENERATION_API ARoadGenerator : public AActor, public ILCGenerator
{
	GENERATED_BODY()
public:
	ARoadGenerator();

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Road Constructor|Import")
	TObjectPtr<AOsmRoadImporter> OsmRoadImporter;

	/** Base for ways without a Highway Styles entry; each way gets a copy with tag overrides. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Highway")
	TObjectPtr<URoadStyle> DefaultRoadStyle;

	/** Base styles keyed by OSM highway value, e.g. residential or primary. Empty entries use Default Road Style. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Highway")
	TMap<FName, TObjectPtr<URoadStyle>> HighwayStyles;

	/** Only these OSM highway values are selected. Empty includes every highway value; no other tags exclude ways. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Import")
	TArray<FName> HighwayFilter;

	/** Road mesh elevation relative to cached imported points. Negative values place roads below those points. */
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

	/** Join compatible two-arm straight continuations before spawning Road Constructor actors. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape")
	bool bMergeStraightRoads = true;

	/** Maximum deviation from 180 degrees between outward XY directions. 90-degree corners remain junctions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape", meta = (ClampMin = "0", ClampMax = "45", Units = "deg", EditCondition = "bMergeStraightRoads"))
	double StraightConnectionToleranceDegrees = 10.0;

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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor Terrain")
	bool bCreateSplineLayerIfMissing = true;

	/** Blend distance beyond each side of the Landscape spline corridor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor Terrain", meta = (ClampMin = "0", Units = "cm"))
	double LandscapeSplineSideFalloffCm = 700.0;

	/** Blend distance beyond the first and last Landscape spline points. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor Terrain", meta = (ClampMin = "0", Units = "cm"))
	double LandscapeSplineEndFalloffCm = 2000.0;

	/** Landscape spline elevation relative to cached imported points, independent of Surface Offset Cm. Negative values lower the splines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor Terrain", meta = (Units = "cm"))
	double LandscapeHeightOffsetCm = 0.0;

	UFUNCTION(CallInEditor, Category = "Road Constructor")
	void GenerateRoads();

	UFUNCTION(CallInEditor, Category = "Road Constructor")
	void ClearRoads();

	/** Replace this generator's Landscape deformation splines using its currently generated roads. */
	UFUNCTION(CallInEditor, Category = "Road Constructor Terrain")
	void GenerateLandscapeSplines();

	/** Remove only this generator's Landscape splines and their deformation, preserving roads. */
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
	friend class FRoadGeneratorLandscapeRegenerationTest;
	friend class FRoadGeneratorWayStylesTest;
	friend class FRoadGeneratorChangeDetectionTest;

	bool bGenerationInProgress = false;
	UPROPERTY()
	TSoftObjectPtr<ALandscape> GeneratedSplineLandscape;
	/** Persist spline ownership independently of the road actors that supplied their geometry. */
	UPROPERTY()
	TSet<FString> GeneratedSplinePrefixes;
	// Session-local fingerprints: only successful generation records a reusable baseline.
	FString CachedRoadInputs;
	FString CachedRoadOutput;
	FString CachedSplineInputs;
	FString CachedSplineOutput;
	void RememberLandscapeSplineOwnership();
	bool GenerateLandscapeSplinesInternal();
	bool ClearLandscapeSplinesInternal();
	bool BuildRoads(const TArray<FImportedRoadWay>& Ways, FName OutputFolderPath);
	bool EnsureLandscapeSplineLayer() const;
	bool BuildLandscapeSplines(AOsmRoadScene* Scene) const;
};
