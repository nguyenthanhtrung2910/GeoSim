#pragma once

#include "CoreMinimal.h"
#include "RoadActor.h"
#include "RoadScene.h"
#include "OsmRoadScene.generated.h"

/** Durable counterparts of the plugin's transient gate and connector references. */
USTRUCT()
struct FImportedJunctionReferences
{
	GENERATED_BODY()
	UPROPERTY()
	TObjectPtr<ARoadActor> Road;
	UPROPERTY()
	TArray<TObjectPtr<ARoadActor>> Links;
};

/** Saved imported junction; loading restores references without discovering or rebuilding roads. */
UCLASS(NotPlaceable)
class ROADGENERATION_API AOsmJunctionActor : public AJunctionActor
{
	GENERATED_BODY()
public:
	UPROPERTY()
	TArray<FImportedJunctionReferences> ImportedReferences;
	void CaptureImportedReferences();
	virtual void PostLoad() override;
	virtual void BeginPlay() override;
};

/** Source roads owned by our importer; loading preserves output without regeneration. */
UCLASS(NotPlaceable)
class ROADGENERATION_API AOsmRoadActor : public ARoadActor
{
	GENERATED_BODY()
public:
	UPROPERTY()
	TArray<FVector> ImportedPositions;

	UPROPERTY()
	FString StartNodeKey;

	UPROPERTY()
	FString EndNodeKey;

	UPROPERTY()
	double ImportedCurveRadius = 5000000.0;

	/** Connector styles must be exported with their actor, rather than /Engine/Transient. */
	UPROPERTY(Instanced)
	TObjectPtr<URoadStyle> ImportedConnectorStyle;

	bool RestoreImportedCurve();
	/** Removes ordinary controls in solved junction cut ranges from a temporary point list. */
	int32 CleanSolvedJunctionPoints(const TArray<FJunctionSlot>& Slots);
	virtual void PostLoad() override;
private:
	bool BuildCurveFromPositions(const TArray<FVector>& Positions);
};

/** Uses Road Constructor's geometry solver with explicit OSM endpoint connectivity.
 * Its own load/play lifecycle avoids the vendor scene's automatic XY crossing discovery.
 */
UCLASS(NotPlaceable)
class ROADGENERATION_API AOsmRoadScene : public ARoadScene
{
	GENERATED_BODY()
public:
	/** Export per-way styles with this scene, even when referencing roads are external actors. */
	UPROPERTY(Instanced)
	TMap<int64, TObjectPtr<URoadStyle>> ImportedWayStyles;

	/** Durable source and target styles for native two-arm road transition segments. */
	UPROPERTY(Instanced)
	TArray<TObjectPtr<URoadStyle>> ImportedTransitionStyles;

	/** The native scene's JunctionLinkRoads array is transient. */
	UPROPERTY()
	TArray<TObjectPtr<ARoadActor>> ImportedConnectorRoads;

	bool PrepareRoadCurves();
	bool SolveJunctions(bool bRestoreCurves = true);
	bool BuildSurfaceMeshes();
	/** Put all generated roads, junctions, connectors, and attached prop actors in this scene's Outliner folder. */
	void UpdateGeneratedActorFolders();

	/** Mesh elevation offset used when generating this scene's imported positions. */
	UPROPERTY()
	double ImportedSurfaceOffsetCm = 0.0;

	UPROPERTY()
	bool bOverrideJunctionRadius = false;

	UPROPERTY()
	double ImportedJunctionRadius = 1000.0;

	UPROPERTY()
	bool bCleanJunctionApproaches = true;

	/** Remove generated corner roads when the native junction asphalt mesh could not be built. */
	UPROPERTY()
	bool bRemoveCornersWithoutJunctionSurface = true;

	bool BuildImportedNetwork();
	int32 RemoveCornersWithoutJunctionSurface(AJunctionActor* Junction) const;
	virtual void PostLoad() override;
	virtual void BeginPlay() override;
	virtual void Destroyed() override;

private:
	friend class FRoadConnectorConversionTest;
	bool bBuildingImportedNetwork = false;
	void ClearImportedJunctions();
	
	
	bool SolveImportedJunctions();
	bool PersistImportedConnectors();
};
