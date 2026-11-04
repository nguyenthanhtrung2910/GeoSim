#include "RoadGenerator.h"
#include "OsmRoadScene.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/ScopedSlowTask.h"
#include "ScopedTransaction.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogRoadGeneratorFoliage, Log, All);

TArray<ARoadActor*> ARoadGenerator::GetFoliageExclusionRoads() const
{
	TArray<ARoadActor*> Roads;
	TSet<ARoadActor*> Seen;
	for (const AOsmRoadScene* Scene : GetRoadScenes())
	{
		TArray<ARoadActor*> SceneRoads = Scene->Roads;
		SceneRoads.Append(Scene->JunctionLinkRoads);
		for (ARoadActor* Connector : Scene->ImportedConnectorRoads) SceneRoads.Add(Connector);
		for (ARoadActor* Road : SceneRoads)
			if (IsValid(Road) && !Road->IsActorBeingDestroyed() && !Seen.Contains(Road))
			{
				Seen.Add(Road);
				Roads.Add(Road);
			}
	}
	return Roads;
}

TArray<FString> ARoadGenerator::GetFoliageLandscapeLayerOptions() const
{
#if WITH_EDITOR
	const TArray<ARoadActor*> Roads = GetFoliageExclusionRoads();
	if (!Roads.IsEmpty()) return Roads[0]->GetAvailableLandscapeLayerOptions();
#endif
	return { TEXT("None") };
}

TArray<FString> ARoadGenerator::GetFoliageLandscapeEditLayerOptions() const
{
#if WITH_EDITOR
	const TArray<ARoadActor*> Roads = GetFoliageExclusionRoads();
	if (!Roads.IsEmpty()) return Roads[0]->GetAvailableLandscapeEditLayerOptions();
#endif
	return { TEXT("None") };
}

#if WITH_EDITOR
void ARoadGenerator::CopyFoliageExclusionSettings(ARoadActor* Road) const
{
	// Match the native property names, but copy values without invoking road editor
	// callbacks, which can rebuild an entire owning scene.
	for (TFieldIterator<FProperty> It(ARoadGenerator::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		const FProperty* Source = *It;
		if (Source->GetMetaData(TEXT("Category")) != TEXT("Terrain Foliage Exclusion")) continue;
		const FProperty* Destination = FindFProperty<FProperty>(ARoadActor::StaticClass(), Source->GetFName());
		if (Destination && Destination->SameType(Source))
			Destination->CopyCompleteValue(Destination->ContainerPtrToValuePtr<void>(Road), Source->ContainerPtrToValuePtr<void>(this));
	}
}
#endif

void ARoadGenerator::CreateFoliageExclusion()
{
#if WITH_EDITOR
	if (bGenerationInProgress || !GetWorld() || GetWorld()->IsGameWorld()) return;
	if (!AreRoadScenesLoaded())
	{
		UE_LOG(LogRoadGeneratorFoliage, Warning, TEXT("Load all generated road scenes before applying foliage exclusion."));
		return;
	}
	const TArray<ARoadActor*> Roads = GetFoliageExclusionRoads();
	if (Roads.IsEmpty()) return;
	TGuardValue<bool> GeneratingGuard(bGenerationInProgress, true);
	const FScopedTransaction Transaction(NSLOCTEXT("RoadGenerator", "CreateFoliageExclusion", "Update generated road foliage exclusion"));
	FEditorScriptExecutionGuard ScriptGuard;
	FScopedSlowTask Progress(Roads.Num(), NSLOCTEXT("RoadGenerator", "ApplyFoliageExclusionProgress", "Updating road foliage exclusion"));
	Progress.MakeDialog(false);
	// Disable this generator's old contributions before clearing/reapplying any
	// landscape paint; other generators' roads retain their native settings.
	for (ARoadActor* Road : Roads)
	{
		Road->Modify();
		Road->bEnableFoliageExclusion = false;
		GetWorld()->GetTimerManager().ClearTimer(Road->FoliageExclusionAutoReapplyTimer);
	}
	for (ARoadActor* Road : Roads)
		if (Road->bFoliageExclusionAffectsLandscapeGrass && !Road->FoliageExclusionTargetLandscapeLayer.IsNone())
			Road->RemoveFoliageExclusionActor();
	for (ARoadActor* Road : Roads)
	{
		Progress.EnterProgressFrame(1);
		CopyFoliageExclusionSettings(Road);
		Road->bEnableFoliageExclusion = true; // No separate Enable checkbox is required.
		Road->SetFoliageExclusionMode(ERoadFoliageExclusionMode::Apply);
		Road->RunFoliageExclusion();
		static_cast<void>(Road->MarkPackageDirty());
	}
#endif
}

void ARoadGenerator::ClearFoliageExclusion()
{
#if WITH_EDITOR
	if (bGenerationInProgress || !GetWorld() || GetWorld()->IsGameWorld()) return;
	if (!AreRoadScenesLoaded())
	{
		UE_LOG(LogRoadGeneratorFoliage, Warning, TEXT("Load all generated road scenes before clearing foliage exclusion."));
		return;
	}
	const TArray<ARoadActor*> Roads = GetFoliageExclusionRoads();
	TGuardValue<bool> GeneratingGuard(bGenerationInProgress, true);
	const FScopedTransaction Transaction(NSLOCTEXT("RoadGenerator", "ClearFoliageExclusion", "Clear generated road foliage exclusion"));
	FEditorScriptExecutionGuard ScriptGuard;
	FScopedSlowTask Progress(Roads.Num(), NSLOCTEXT("RoadGenerator", "ClearFoliageExclusionProgress", "Clearing road foliage exclusion"));
	Progress.MakeDialog(false);
	for (ARoadActor* Road : Roads)
	{
		Road->Modify();
		Road->bEnableFoliageExclusion = false;
		GetWorld()->GetTimerManager().ClearTimer(Road->FoliageExclusionAutoReapplyTimer);
	}
	for (ARoadActor* Road : Roads)
	{
		Progress.EnterProgressFrame(1);
		// Retain each road's last applied layer names so Clear targets the old
		// output even if the generator's pending settings have changed.
		Road->RemoveFoliageExclusionActor();
		Road->bEnableFoliageExclusion = false;
		Road->SetFoliageExclusionMode(ERoadFoliageExclusionMode::Off);
		static_cast<void>(Road->MarkPackageDirty());
	}
#endif
}
