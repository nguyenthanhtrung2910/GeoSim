#pragma once

#include "CoreMinimal.h"
#include "RoadActor.h"
#include "RoadScene.h"
#include "OsmRoadScene.generated.h"

/** Source roads owned by our importer; the scene restores their generated meshes after loading. */
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

	bool RestoreImportedCurve();
	/** Removes ordinary controls in solved junction cut ranges from a temporary point list. */
	int32 CleanSolvedJunctionPoints(const TArray<FJunctionSlot>& Slots);
	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostEditUndo() override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
#endif
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
	void QueueImportedRebuild();
	virtual void PostLoad() override;
	virtual void BeginPlay() override;
	virtual void Destroyed() override;
#if WITH_EDITOR
	virtual void PostEditUndo() override;
#endif

private:
	friend class ARoadGenerator;
	bool bPreparingImportedRoads = false;
	bool bImportedRebuildQueued = false;
	bool bBuildingImportedNetwork = false;
#if WITH_EDITOR
	FString LastBuiltOutputSignature;
#endif
	void ClearImportedJunctions();
	bool SolveImportedJunctions();
};
