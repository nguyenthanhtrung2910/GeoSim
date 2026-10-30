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
		/** Optional native transition styles, owned by the candidate scene. */
		URoadStyle* TransitionSourceStyle = nullptr;
		URoadStyle* TransitionTargetStyle = nullptr;
	};
	struct FSceneSpans
	{
		FIntPoint Cell;
		TArray<FImportSpan> Spans;
	};

	/** Keep each junction in one grid cell; cut roads between cells at their midpoint. */
	TArray<FSceneSpans> PartitionSpans(const TArray<FImportSpan>& Spans, double SceneSizeCm);

	TArray<FImportSpan> PrepareSpans(const TArray<FImportedRoadWay>& Ways, const TArray<FName>& Filter, bool bRequireTerrain, double HeightOffset, double MergeDistance = 1000.0);
	/** Always copies the base style; absent/invalid lane counts preserve its layout. */
	URoadStyle* CreateLaneCountStyle(UObject* Outer, const URoadStyle* Source, int32 LaneCount);
	/** Join compatible ways only at two-arm shared nodes, retaining intermediate controls and source IDs. */
	int32 MergeSharedNodeSpans(TArray<FImportSpan>& Spans, const TFunctionRef<URoadStyle*(const FImportSpan&)>& ResolveStyle);
}

/**
 * Converts the OSMRoadImporter road cache into Road Constructor roads.
 *
 * Importer-driven regeneration and Landscape deformation use explicit buttons.
 * Generated roads retain native plugin editor behavior for scene-scoped edits,
 * moves, and undo/redo; connected-road undo may refresh the owning RoadScene.
 * Imported load/play callbacks still preserve saved output without native refresh.
 * Native asset callbacks remain independent: shared RoadStyle live updates can
 * rebuild several scenes, and RoadProps edits select a scene through the plugin.
 */
UCLASS()
class ROADGENERATION_API ARoadGenerator : public AActor, public ILCGenerator
{
	GENERATED_BODY()
public:
	ARoadGenerator();

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Road Constructor|Import")
	TObjectPtr<AOsmRoadImporter> OsmRoadImporter;

	/** World Outliner folder for generated actors, e.g. Generated/Roads. Empty uses this generator's folder; a generation override takes priority. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Output")
	FString SpawnedActorsPath;

	/** Base for ways without a Highway Styles entry; each way gets a copy with tag overrides. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Highway")
	TObjectPtr<URoadStyle> DefaultRoadStyle;

	/** Base styles keyed by OSM highway value, e.g. residential or primary. Empty entries use Default Road Style. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Highway")
	TMap<FName, TObjectPtr<URoadStyle>> HighwayStyles;

	/** Only these OSM highway values are selected. Empty includes every highway value; no other tags exclude ways. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Import")
	TArray<FName> HighwayFilter;

	/** Grid size for scene ownership. Junctions stay intact; roads between cells are split at their midpoint. This is not a hard bound on road count or scene extent. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Import", meta = (ClampMin = "1000", Units = "cm"))
	double RoadSceneSizeCm = 100000.0;

	/** Road mesh elevation relative to cached imported points. Negative values place roads below those points. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Placement", meta = (Units = "cm"))
	double SurfaceOffsetCm = 3.0;

	/** Maximum radius of bends through interior OSM points. Adjacent span lengths still limit the actual curve; two-point roads remain straight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape", meta = (ClampMin = "1", ClampMax = "5000000", Units = "cm"))
	double RoadCurveRadiusCm = 5000000.0;

	/** Desired combined length of a native style transition around a two-arm node. Each approach uses at most a quarter of its available length. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape", meta = (ClampMin = "100", Units = "cm"))
	double RoadTransitionLengthCm = 6000.0;
	
	/** Remove junction sidewalk/curb corner roads if Road Constructor fails to build the central asphalt surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape")
	bool bRemoveCornersWithoutJunctionSurface = true;

	/** Override Road Constructor's initial link radii. Disabled uses the plugin's own initialization and corner solver. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape")
	bool bOverrideJunctionRadius = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape", meta = (ClampMin = "0", Units = "cm", EditCondition = "bOverrideJunctionRadius"))
	double JunctionRadiusCm = 1000.0;

	/** Remove short corridors between branching junctions before calling the plugin solver. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape", meta = (ClampMin = "0", Units = "cm"))
	double JunctionMergeDistanceCm = 0.0;

	/** Solve junctions, remove ordinary controls inside their cut ranges, then solve again. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Shape")
	bool bCleanJunctionApproaches = false;
	
	/** Missing terrain samples split roads instead of producing long triangles across unloaded terrain. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Road Constructor|Import")
	bool bRequireTerrainSamples = true;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Terrain Deform")
	TObjectPtr<ALandscape> TargetLandscape;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain Deform")
	bool bCreateSplineLayerIfMissing = true;

	/** Only the reserved Landscape spline edit layer named Splines is supported. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain Deform", meta = (DisplayName = "Spline Edit Layer", GetOptions = "GetLandscapeSplineEditLayerOptions"))
	FName TerrainDeformTargetLandscapeSplineEditLayer = TEXT("Splines");

	/** Matches RoadActor's scale: 0.5 uses half the total lane width as the native spline half-width. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain Deform", meta = (DisplayName = "Road Width Scale", ClampMin = "0.2", ClampMax = "2.0"))
	double LandscapeSplineWidthScale = 0.5;

	/** Uses Road Constructor's default base distance and native falloff clamping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain Deform", meta = (DisplayName = "Side Falloff Scale", ClampMin = "0.05", ClampMax = "3.0"))
	double LandscapeSplineFalloffScale = 1.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain Deform", meta = (DisplayName = "Start Falloff", ClampMin = "0", ClampMax = "100000", Units = "cm"))
	double LandscapeSplineStartFalloffCm = 2000.0;

	/** Blend distance beyond the last Landscape spline point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain Deform", meta = (DisplayName = "End Falloff", ClampMin = "0", ClampMax = "100000", Units = "cm"))
	double LandscapeSplineEndFalloffCm = 2000.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain Deform", meta = (DisplayName = "Start Extension", ClampMin = "0", ClampMax = "100000", Units = "cm"))
	double LandscapeSplineStartExtensionCm = 1000.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain Deform", meta = (DisplayName = "End Extension", ClampMin = "0", ClampMax = "100000", Units = "cm"))
	double LandscapeSplineEndExtensionCm = 1000.0;

	/** Landscape spline elevation relative to cached imported points, independent of Surface Offset Cm. Negative values lower the splines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain Deform", meta = (DisplayName = "Roadbed Height Offset", Units = "cm"))
	double LandscapeHeightOffsetCm = 0.0;

	UFUNCTION(CallInEditor, Category = "Road Constructor")
	void GenerateRoads();

	UFUNCTION(CallInEditor, Category = "Road Constructor")
	void ClearRoads();

	/** Replace this generator's Landscape deformation splines using its currently generated roads. */
	UFUNCTION(CallInEditor, Category = "Terrain Deform")
	void GenerateLandscapeSplines();

	/** Remove only this generator's Landscape splines and their deformation, preserving roads. */
	UFUNCTION(CallInEditor, Category = "Terrain Deform", meta = (DisplayName = "Clear Landscape Spline Points"))
	void ClearLandscapeSplines();

	UFUNCTION()
	static TArray<FName> GetLandscapeSplineEditLayerOptions();

	virtual bool OnGenerate(FName SpawnedActorsPathOverride, bool bIsUserInitiated) override;
	virtual TArray<UObject*> GetGeneratedObjects() const override;
	virtual bool Cleanup_Implementation(bool bSkipPrompt) override;

	/** First scene, retained for compatibility with previously saved generator output. */
	UPROPERTY(DuplicateTransient)
	TSoftObjectPtr<AOsmRoadScene> GeneratedScene;

	UPROPERTY(VisibleInstanceOnly, AdvancedDisplay, DuplicateTransient, Category = "Road Constructor|Output")
	TArray<TSoftObjectPtr<AOsmRoadScene>> GeneratedScenes;

	TArray<AOsmRoadScene*> GetRoadScenes() const;

	/** Previous importer-owned spline actor, retained only so existing levels can clear it. */
	UPROPERTY()
	TSoftObjectPtr<ALandscapeSplineActor> GeneratedLandscapeSplineActor;

private:
	friend class FRoadGeneratorLandscapeRegenerationTest;
	friend class FRoadGeneratorWayStylesTest;
	friend class FRoadGeneratorChangeDetectionTest;
	friend class FRoadGeneratorScenePartitionTest;
	friend class FRoadGeneratorSharedNodeTest;

	bool bGenerationInProgress = false;
	UPROPERTY()
	TSoftObjectPtr<ALandscape> GeneratedSplineLandscape;
	/** Persist spline ownership independently of the road actors that supplied their geometry. */
	UPROPERTY()
	TSet<FString> GeneratedSplinePrefixes;
	// Session-local fingerprints: only successful generation records a reusable baseline.
	FString CachedRoadInputs;
	FString CachedRoadGeometryInputs;
	FString CachedJunctionInputs;
	FString CachedSplineInputs;
	FString CachedSplineOutput;
	void RememberLandscapeSplineOwnership();
	bool GenerateLandscapeSplinesInternal();
	bool ClearLandscapeSplinesInternal();
	bool BuildRoads(const TArray<FImportedRoadWay>& Ways, FName OutputFolderPath);
	TArray<AOsmRoadScene*> PrepareRoadCurves(const TArray<FImportedRoadWay>& Ways, FName OutputFolderPath);
	bool AreRoadScenesLoaded() const;
	FName ResolveOutputFolder(FName Override) const;
	bool EnsureLandscapeSplineLayer() const;
	bool BuildLandscapeSplines(AOsmRoadScene* Scene) const;
};
