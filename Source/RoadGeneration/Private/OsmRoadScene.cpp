#include "OsmRoadScene.h"

#include "Components/PrimitiveComponent.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "Templates/UnrealTemplate.h"
#include "Algo/Reverse.h"

#include "Components/StaticMeshComponent.h"
#if WITH_EDITOR
#include "RoadGenerationSignatures.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogImportedRoadConstructor, Log, All);

bool AOsmRoadActor::RestoreImportedCurve()
{
	return BuildCurveFromPositions(ImportedPositions);
}

bool AOsmRoadActor::BuildCurveFromPositions(const TArray<FVector>& Positions)
{
	if (Positions.Num() < 2 || !BaseCurve) return false;
	RoadPoints.Reset();
	HeightPoints.Reset();
	ClearSegments();
	bUseRoadStyleCornerSmoothing = false;
	for (const FVector& Position : Positions)
	{
		if (Position.ContainsNaN()) return false;
		FRoadPoint& Point = RoadPoints.AddDefaulted_GetRef();
		Point.Pos = FVector2D(Position.X, Position.Y);
		Point.MaxRadius = ImportedCurveRadius;
	}
	// First solve XY. Height stations must use the curved road's distance, not the
	// original polyline's length (rounding changes the total and intermediate stations).
	UpdateCurve();
	if (RoadPoints.Num() != Positions.Num() || !FMath::IsFinite(Length()) || Length() <= 1.0) return false;
	HeightPoints.Reset();
	for (int32 Index = 0; Index < RoadPoints.Num(); ++Index)
	{
		FHeightPoint& HeightPoint = HeightPoints.AddDefaulted_GetRef();
		HeightPoint.Dist = RoadPoints[Index].Dist;
		HeightPoint.Height = Positions[Index].Z;
	}
	UpdateCurve();
	return BaseCurve->Curve.Points.Num() >= 2;
}

int32 AOsmRoadActor::CleanSolvedJunctionPoints(const TArray<FJunctionSlot>& Slots)
{
	// This is a single correction pass immediately after RestoreImportedCurve.
	// Keep the source positions for regeneration; modify only the native controls.
	if (RoadPoints.Num() != ImportedPositions.Num()) return -1;
	TArray<FVector> CleanedPositions;
	CleanedPositions.Reserve(ImportedPositions.Num());
	for (int32 Index = 0; Index < ImportedPositions.Num(); ++Index)
	{
		bool bInsideJunction = false;
		if (Index > 0 && Index + 1 < ImportedPositions.Num())
		{
			const double Station = RoadPoints[Index].Dist;
			for (const FJunctionSlot& Slot : Slots)
			{
				if (Slot.Road != this || !Slot.IsValid()) continue;
				const double Start = FMath::Clamp(Slot.InputDist(), 0.0, Length());
				if (const double End = FMath::Clamp(Slot.OutputDist(), 0.0, Length()); FMath::IsFinite(Start) &&
					FMath::IsFinite(End) && End > Start && Station >= Start - 0.01 && Station <= End + 0.01)
				{
					bInsideJunction = true;
					break;
				}
			}
		}
		if (!bInsideJunction) CleanedPositions.Add(ImportedPositions[Index]);
	}
	const int32 Removed = ImportedPositions.Num() - CleanedPositions.Num();
	if (Removed > 0 && !BuildCurveFromPositions(CleanedPositions)) return -1;
	return Removed;
}

void AOsmRoadActor::PostLoad()
{
	// These are freshly imported roads, not vendor-authored assets requiring migrations.
	// The vendor callback queues a full XY-discovery rebuild; our scene owns restoration.
	// ReSharper disable once CppReferenceToOverriddenVirtualFunction
	AActor::PostLoad();
	RoadConstructorGeneratedMeshComponent = Cast<UPrimitiveComponent>(GetRootComponent());
	if (AOsmRoadScene* Scene = Cast<AOsmRoadScene>(GetScene())) Scene->QueueImportedRebuild();
}

#if WITH_EDITOR
void AOsmRoadActor::PostEditMove(const bool bFinished)
{
	ARoadActor::PostEditMove(bFinished);
	// Road Constructor stores its geometry in scene coordinates. Reimport controls
	// rather than moving only the mesh component away from the junction solver.
	if (bFinished)
	{
		SetActorTransform(FTransform::Identity);
		if (AOsmRoadScene* Scene = Cast<AOsmRoadScene>(GetScene())) Scene->QueueImportedRebuild();
	}
}

void AOsmRoadActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	ARoadActor::PostEditChangeProperty(PropertyChangedEvent);
	if (AOsmRoadScene* Scene = Cast<AOsmRoadScene>(GetScene())) Scene->QueueImportedRebuild();
}

void AOsmRoadActor::PostEditUndo()
{
	// The plugin queues geometric connectivity discovery for roads touching junctions.
	// ReSharper disable once CppReferenceToOverriddenVirtualFunction
	AActor::PostEditUndo();
	if (AOsmRoadScene* Scene = Cast<AOsmRoadScene>(GetScene()))
	{
		Scene->QueueImportedRebuild();
	}
}
#endif

void AOsmRoadScene::ClearImportedJunctions()
{
	const TArray<AJunctionActor*> PreviousJunctions = Junctions;
	Junctions.Reset();
	for (AJunctionActor* Junction : PreviousJunctions)
	{
		if (IsValid(Junction)) Junction->Destroy(); // Also destroys its internal connector roads.
	}
	JunctionLinkRoads.Reset();
}

bool AOsmRoadScene::SolveImportedJunctions()
{
	ClearImportedJunctions();
	Octree = TOctree2<FRoadOctreeElement, FRoadOctreeSemantics>(FVector::ZeroVector, HALF_WORLD_MAX);

	struct FEndpoint
	{
		ARoadActor* Road;
		double Distance;
	};
	TMap<FString, TArray<FEndpoint>> Endpoints;
	for (ARoadActor* Road : Roads)
	{
		AOsmRoadActor* ImportedRoad = Cast<AOsmRoadActor>(Road);
		if (!IsValid(ImportedRoad) || !ImportedRoad->BaseCurve || ImportedRoad->BaseCurve->Curve.Points.Num() < 2)
		{
			UE_LOG(LogImportedRoadConstructor, Error, TEXT("Cannot solve junctions for invalid imported road %s."), *GetNameSafe(Road));
			return false;
		}
		for (URoadBoundary* Boundary : Road->Boundaries) Boundary->OctreeIds.Reset();
		OctreeAddRoad(Road);
		if (!ImportedRoad->StartNodeKey.IsEmpty()) Endpoints.FindOrAdd(ImportedRoad->StartNodeKey).Add({ Road, 0.0 });
		if (!ImportedRoad->EndNodeKey.IsEmpty()) Endpoints.FindOrAdd(ImportedRoad->EndNodeKey).Add({ Road, Road->Length() });
	}

	// One actor per known OSM node, including degree-two way seams. No XY crossing discovery.
	TArray<FString> Keys;
	Endpoints.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		const TArray<FEndpoint>& Arms = Endpoints[Key];
		if (Arms.Num() < 2) continue;
		FActorSpawnParameters Params;
		Params.Owner = this;
		Params.OverrideLevel = GetLevel();
		Params.ObjectFlags = RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AJunctionActor* Junction = GetWorld()->SpawnActor<AJunctionActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!Junction) return false;
		Junction->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
		Junctions.Add(Junction);
		for (const auto& [Road, Distance] : Arms) Junction->AddRoad(Road, Distance);
		if (bOverrideJunctionRadius)
		{
			for (FJunctionGate& Gate : Junction->Gates)
			{
				Gate.Links.SetNum(Junction->Gates.Num());
				for (FJunctionLink& Link : Gate.Links) Link.Radius = ImportedJunctionRadius;
			}
		}
	}

	// Use the vendor solver, including combining neighbouring junctions whose cuts
	// overlap on a short connecting road. Each successful join reduces the actor count.
	bool bJoined;
	do
	{
		for (AJunctionActor* Junction : Junctions) Junction->Update(Octree);
		bJoined = false;
		for (const TMap<ARoadActor*, TArray<FJunctionSlot>> SlotsByRoad = GetAllJunctionSlots(); const auto& Pair :
		     SlotsByRoad)
		{
			for (int32 Index = 1; Index < Pair.Value.Num(); ++Index)
			{
				FJunctionSlot Before = Pair.Value[Index - 1];
				if (FJunctionSlot After = Pair.Value[Index]; Before.Junction != After.Junction && Before.OutputDist() >=
					After.InputDist())
				{
					Before.Combine(After); // Vendor function transfers gates, destroys and unregisters After.
					bJoined = true;
					break;
				}
			}
			if (bJoined) break;
		}
	} while (bJoined);

	for (int32 Index = Junctions.Num() - 1; Index >= 0; --Index)
	{
		if (Junctions[Index]->Gates.Num() < 2)
		{
			Junctions[Index]->Destroy();
			Junctions.RemoveAt(Index);
		}
	}
	return true;
}

int32 AOsmRoadScene::RemoveCornersWithoutJunctionSurface(AJunctionActor* Junction) const
{
	if (!bRemoveCornersWithoutJunctionSurface || !IsValid(Junction)) return 0;
	// Road Constructor builds the central asphalt on the junction root. Its mesh
	// builder leaves that component without a static mesh when triangulation fails,
	// while the independent corner roads may still contain sidewalks and curbs.
	if (const UStaticMeshComponent* Surface = Cast<UStaticMeshComponent>(Junction->GetRootComponent()); !Surface ||
		Surface->GetStaticMesh()) return 0;

	int32 Removed = 0;
	for (FJunctionGate& Gate : Junction->Gates)
	{
		if (!Gate.Links.IsValidIndex(AJunctionActor::CornerIndex)) continue;
		FJunctionLink& Corner = Gate.Links[AJunctionActor::CornerIndex];
		if (!IsValid(Corner.Road)) continue;
		Corner.Destroy(); // Unregisters and deletes the generated sidewalk/curb road.
		++Removed;
	}
	if (Removed > 0)
	{
		UE_LOG(LogImportedRoadConstructor, Warning, TEXT("Junction %s has no asphalt surface; removed %d generated sidewalk/curb corner roads."), *GetNameSafe(Junction), Removed);
	}
	return Removed;
}

bool AOsmRoadScene::BuildImportedNetwork()
{
	if (bBuildingImportedNetwork || IsActorBeingDestroyed() || !GetWorld()) return false;
	TGuardValue<bool> BuildingGuard(bBuildingImportedNetwork, true);
	ClearImportedJunctions();
	for (ARoadActor* Road : Roads)
	{
		if (AOsmRoadActor* ImportedRoad = Cast<AOsmRoadActor>(Road); !IsValid(ImportedRoad) || !ImportedRoad->
			RestoreImportedCurve())
		{
			UE_LOG(LogImportedRoadConstructor, Error, TEXT("Cannot restore imported road %s."), *GetNameSafe(Road));
			return false;
		}
	}
	if (!SolveImportedJunctions()) return false;
	if (bCleanJunctionApproaches)
	{
		int32 RemovedPoints = 0;
		const TMap<ARoadActor*, TArray<FJunctionSlot>> FirstPassSlots = GetAllJunctionSlots();
		// Each road reads its own intervals before changing its curve. The slots
		// reference first-pass gates; they are discarded before the second solve.
		for (ARoadActor* Road : Roads)
		{
			const TArray<FJunctionSlot>* Slots = FirstPassSlots.Find(Road);
			if (!Slots) continue;
			const int32 Removed = CastChecked<AOsmRoadActor>(Road)->CleanSolvedJunctionPoints(*Slots);
			if (Removed < 0) return false;
			RemovedPoints += Removed;
		}
		if (RemovedPoints > 0)
		{
			// Discard first-pass gates/cuts and rebuild the octree and all junctions.
			// Never reuse old distances after the fitted road length has changed.
			if (!SolveImportedJunctions()) return false;
			UE_LOG(LogImportedRoadConstructor, Display, TEXT("Removed %d ordinary controls inside solved junction cut ranges; solved junctions again before meshing. Imported source positions are unchanged."), RemovedPoints);
		}
	}
	for (AJunctionActor* Junction : Junctions)
	{
		Junction->Build();
		RemoveCornersWithoutJunctionSurface(Junction);
	}
	// Obtain slots from the final junctions so both surface types use the same cuts.
	const TMap<ARoadActor*, TArray<FJunctionSlot>> SlotsByRoad = GetAllJunctionSlots();
	for (ARoadActor* Road : Roads)
	{
		const TArray<FJunctionSlot>* Slots = SlotsByRoad.Find(Road);
		Road->BuildMesh(Slots ? *Slots : TArray<FJunctionSlot>());
	}
	ReconcileTrackedJunctionLinkRoads();
#if WITH_EDITOR
	LastBuiltOutputSignature = RoadGenerationSignatures::RoadOutput(this);
#endif
	UE_LOG(LogImportedRoadConstructor, Display, TEXT("Built %d Road Constructor roads and %d OSM junctions."), Roads.Num(), Junctions.Num());
	return !Roads.IsEmpty();
}

void AOsmRoadScene::QueueImportedRebuild()
{
	if (bPreparingImportedRoads || bBuildingImportedNetwork || bImportedRebuildQueued ||
		HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || IsActorBeingDestroyed()) return;
	bImportedRebuildQueued = true;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float)
	{
		bImportedRebuildQueued = false;
		if (!GetWorld() || IsActorBeingDestroyed()) return false;
#if WITH_EDITOR
		// Labels and editor bookkeeping also emit property-change events. Reuse
		// unchanged output instead of replacing the generator cache's mesh/junctions.
		if (!LastBuiltOutputSignature.IsEmpty() && LastBuiltOutputSignature == RoadGenerationSignatures::RoadOutput(this)) return false;
#endif
		BuildImportedNetwork();
		return false;
	}));
}

void AOsmRoadScene::PostLoad()
{
	// ARoadScene::PostLoad/BeginPlay schedule geometry-based connectivity discovery.
	// Restore our serialized control points and OSM connectivity instead.
	// ReSharper disable once CppReferenceToOverriddenVirtualFunction
	AActor::PostLoad();
	QueueImportedRebuild();
}

void AOsmRoadScene::BeginPlay()
{
	// Keep normal actor/component BeginPlay, without the plugin's next-tick Refresh All.
	// ReSharper disable once CppReferenceToOverriddenVirtualFunction
	AActor::BeginPlay();
	QueueImportedRebuild();
}

#if WITH_EDITOR
void AOsmRoadScene::PostEditUndo()
{
	ARoadScene::PostEditUndo();
	QueueImportedRebuild();
}
#endif

void AOsmRoadScene::Destroyed()
{
	ClearImportedJunctions();
	const TArray<ARoadActor*> PreviousRoads = Roads;
	Roads.Reset();
	for (ARoadActor* Road : PreviousRoads)
	{
		if (IsValid(Road)) Road->Destroy();
	}
	Super::Destroyed();
}
