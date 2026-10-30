#include "OsmRoadScene.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "Templates/UnrealTemplate.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogImportedRoadConstructor, Log, All);

void AOsmJunctionActor::CaptureImportedReferences()
{
	ImportedReferences.Reset(Gates.Num());
	for (const FJunctionGate& Gate : Gates)
	{
		auto& [Road, Links] = ImportedReferences.AddDefaulted_GetRef();
		Road = Gate.Road;
		for (const FJunctionLink& Link : Gate.Links) Links.Add(Link.Road);
	}
}

void AOsmJunctionActor::PostLoad()
{
	// The native callback can discard serialized meshes and queue a full refresh.
	// ReSharper disable once CppReferenceToOverriddenVirtualFunction
	AActor::PostLoad();
	RoadConstructorGeneratedMeshComponent = Cast<UPrimitiveComponent>(GetRootComponent());
	for (int32 GateIndex = 0; GateIndex < FMath::Min(Gates.Num(), ImportedReferences.Num()); ++GateIndex)
	{
		FJunctionGate& Gate = Gates[GateIndex];
		const auto& [Road, Links] = ImportedReferences[GateIndex];
		Gate.Road = Road;
		for (int32 LinkIndex = 0; LinkIndex < FMath::Min(Gate.Links.Num(), Links.Num()); ++LinkIndex)
			Gate.Links[LinkIndex].Road = Links[LinkIndex];
	}
}

void AOsmJunctionActor::BeginPlay()
{
	// ReSharper disable once CppReferenceToOverriddenVirtualFunction
	AActor::BeginPlay();
}

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
	// Loading must preserve saved output without queuing the vendor's full rebuild.
	// ReSharper disable once CppReferenceToOverriddenVirtualFunction
	AActor::PostLoad();
	RoadConstructorGeneratedMeshComponent = Cast<UPrimitiveComponent>(GetRootComponent());
}

void AOsmRoadScene::ClearImportedJunctions()
{
	const TArray<AJunctionActor*> PreviousJunctions = Junctions;
	Junctions.Reset();
	for (AJunctionActor* Junction : PreviousJunctions)
	{
		if (IsValid(Junction)) Junction->Destroy(); // Also destroys its internal connector roads.
	}
	JunctionLinkRoads.Reset();
	ImportedConnectorRoads.Reset();
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

	// Junctions require at least three connecting road arms. Two-arm seams are
	// merged during import when compatible; no XY crossing discovery is performed.
	TArray<FString> Keys;
	Endpoints.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		const TArray<FEndpoint>& Arms = Endpoints[Key];
		if (Arms.Num() < 3) continue;
		FActorSpawnParameters Params;
		Params.Owner = this;
		Params.OverrideLevel = GetLevel();
		Params.ObjectFlags = RF_Public | RF_Transactional;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AJunctionActor* Junction = GetWorld()->SpawnActor<AOsmJunctionActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
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
		if (Junctions[Index]->Gates.Num() < 3)
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

bool AOsmRoadScene::PrepareRoadCurves()
{
	for (ARoadActor* Road : Roads)
	{
		if (AOsmRoadActor* ImportedRoad = Cast<AOsmRoadActor>(Road); !IsValid(ImportedRoad) || !ImportedRoad->
			RestoreImportedCurve())
		{
			UE_LOG(LogImportedRoadConstructor, Error, TEXT("Cannot restore imported road %s."), *GetNameSafe(Road));
			return false;
		}
	}
	return !Roads.IsEmpty();
}

bool AOsmRoadScene::SolveJunctions(const bool bRestoreCurves)
{
	// Approach cleanup depends on solved cuts. Re-solve from the untrimmed controls,
	// including when only junction settings changed since the last generation.
	if (bRestoreCurves && !PrepareRoadCurves()) return false;
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
	return true;
}

bool AOsmRoadScene::PersistImportedConnectors()
{
	// Native connector construction always spawns transient ARoadActors. Convert
	// those results to our load-safe actor type before publishing the generated scene.
	TMap<UObject*, UObject*> Replacements;
	for (ARoadActor* Road : JunctionLinkRoads)
	{
		if (Road->IsA<AOsmRoadActor>()) continue;
		FActorSpawnParameters Params;
		Params.Owner = this;
		Params.OverrideLevel = GetLevel();
		Params.ObjectFlags = RF_Public | RF_Transactional;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AOsmRoadActor* Saved = GetWorld()->SpawnActor<AOsmRoadActor>(AOsmRoadActor::StaticClass(), Road->GetActorTransform(), Params);
		if (!Saved) return false;
		Replacements.Add(Road, Saved);
	}
	for (const auto& Pair : Replacements)
	{
		ARoadActor* Original = CastChecked<ARoadActor>(Pair.Key);
		AOsmRoadActor* Saved = CastChecked<AOsmRoadActor>(Pair.Value);
		AActor* Parent = Original->GetAttachParentActor();
		const FTransform Transform = Original->GetActorTransform();
		Saved->UnregisterAllComponents();
		// Copy road settings, never the actor/component hierarchy or actor GUID.
		// Surfaces and props are generated on the new actor later in this stage.
		for (TFieldIterator<FProperty> Property(ARoadActor::StaticClass(), EFieldIteratorFlags::ExcludeSuper); Property; ++Property)
		{
			if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
			if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(*Property);
				Object && Object->PropertyClass->IsChildOf(UActorComponent::StaticClass())) continue;
			Property->CopyCompleteValue_InContainer(Saved, Original);
		}
		Saved->Tags = Original->Tags;
		// Duplicate the ordinary UObject model graph explicitly so no lane or
		// boundary remains owned by a temporary actor that will be destroyed.
		TArray<UObject*> Models;
		for (URoadBoundary* Boundary : Original->Boundaries) Models.AddUnique(Boundary);
		for (URoadConstructorLane* Lane : Original->Lanes) Models.AddUnique(Lane);
		for (URoadMarking* Marking : Original->Markings) Models.AddUnique(Marking);
		ForEachObjectWithOuter(Original, [&](UObject* Object)
		{
			if (Object->GetClass()->GetOutermost()->GetName() == TEXT("/Script/RoadConstructor") &&
				!Object->IsA<UStaticMesh>() && !Object->IsA<UActorComponent>()) Models.AddUnique(Object);
		}, EGetObjectsFlags::IncludeNestedObjects);
		TMap<UObject*, UObject*> ModelCopies = Replacements;
		for (UObject* Model : Models)
		{
			if (!IsValid(Model) || ModelCopies.Contains(Model)) continue;
			const FName Name = MakeUniqueObjectName(Saved, Model->GetClass(), FName(*(TEXT("Imported_") + Model->GetName())));
			UObject* Result = NewObject<UObject>(Saved, Model->GetClass(), Name, RF_Transactional);
			if (!Result) return false;
			ModelCopies.Add(Model, Result);
		}
		for (UObject* Model : Models)
			for (TFieldIterator<FProperty> Property(Model->GetClass()); Property; ++Property)
				Property->CopyCompleteValue_InContainer(ModelCopies.FindChecked(Model), Model);
		Saved->BaseCurve = CastChecked<URoadBoundary>(ModelCopies.FindChecked(Original->BaseCurve));
		Saved->Boundaries.Reset();
		for (URoadBoundary* Boundary : Original->Boundaries) Saved->Boundaries.Add(CastChecked<URoadBoundary>(ModelCopies.FindChecked(Boundary)));
		Saved->Lanes.Reset();
		for (URoadConstructorLane* Lane : Original->Lanes) Saved->Lanes.Add(CastChecked<URoadConstructorLane>(ModelCopies.FindChecked(Lane)));
		Saved->Markings.Reset();
		for (URoadMarking* Marking : Original->Markings) Saved->Markings.Add(CastChecked<URoadMarking>(ModelCopies.FindChecked(Marking)));
		for (UObject* Model : Models)
			if (UObject* const* Result = ModelCopies.Find(Model))
				FArchiveReplaceObjectRef<UObject> Reconnect(*Result, ModelCopies,
					EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
		FArchiveReplaceObjectRef<UObject> ReconnectActor(Saved, ModelCopies,
			EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
		Saved->ClearFlags(RF_Transient);
		Saved->SetFlags(RF_Public | RF_Transactional);
		Saved->SetOwner(this);
		if (Saved->GetRootComponent()) Saved->GetRootComponent()->SetupAttachment(nullptr);
		Saved->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
		Saved->SetActorTransform(Transform);
		if (!IsValid(Original->LastAppliedRoadStyle)) return false;
		Saved->ImportedConnectorStyle = DuplicateObject<URoadStyle>(Original->LastAppliedRoadStyle, Saved);
		Saved->ImportedConnectorStyle->ClearFlags(RF_Transient | RF_Standalone);
		Saved->ImportedConnectorStyle->SetFlags(RF_Public | RF_Transactional);
		Saved->LastAppliedRoadStyle = Saved->ImportedConnectorStyle;
		Saved->SetGeneratedJunctionInternalRoad(true);
		// Preserve the solved model verbatim. Native connector boundaries can collapse
		// to one point; persistence must neither reject nor recompute that geometry.
		for (int32 Index = 0; Index < Saved->Boundaries.Num(); ++Index)
		{
			const URoadBoundary* Boundary = Saved->Boundaries[Index];
			const URoadBoundary* Source = Original->Boundaries[Index];
			if (!Boundary || !Boundary->IsIn(Saved) || !Source ||
				!FPolyline::StaticStruct()->CompareScriptStruct(&Boundary->Curve, &Source->Curve, 0))
			{
				UE_LOG(LogImportedRoadConstructor, Error, TEXT("Connector conversion failed: road=%s segments=%d boundary=%s owner=%s points=%d."),
					*Saved->GetName(), Saved->RoadSegments.Num(), *GetNameSafe(Boundary), *GetNameSafe(Boundary ? Boundary->GetOuter() : nullptr), Boundary ? Boundary->Curve.Points.Num() : 0);
				return false;
			}
		}
		Saved->ReregisterAllComponents();
	}
	auto replace = [&](ARoadActor*& Road)
	{
		if (UObject* const* Saved = Replacements.Find(Road)) Road = CastChecked<ARoadActor>(*Saved);
	};
	for (AJunctionActor* Junction : Junctions)
		for (FJunctionGate& Gate : Junction->Gates)
			for (FJunctionLink& Link : Gate.Links)
			{
				replace(Link.Road);
				replace(Link.InputRoad);
				replace(Link.OutputRoad);
			}
	for (ARoadActor*& Road : JunctionLinkRoads) replace(Road);
	for (const auto& Pair : Replacements) CastChecked<ARoadActor>(Pair.Key)->Destroy();
	ImportedConnectorRoads.Reset();
	for (ARoadActor* Road : JunctionLinkRoads) ImportedConnectorRoads.Add(Road);
	for (AJunctionActor* Junction : Junctions)
		if (AOsmJunctionActor* Imported = Cast<AOsmJunctionActor>(Junction)) Imported->CaptureImportedReferences();
	return true;
}

void AOsmRoadScene::UpdateGeneratedActorFolders()
{
#if WITH_EDITOR
	TSet<AActor*> Actors;
	TArray<AActor*> Pending = { this };
	for (ARoadActor* Road : Roads) Pending.Add(Road);
	for (AJunctionActor* Junction : Junctions) Pending.Add(Junction);
	for (ARoadActor* Link : JunctionLinkRoads) Pending.Add(Link);
	while (!Pending.IsEmpty())
	{
		AActor* Actor = Pending.Pop(EAllowShrinking::No);
		if (!IsValid(Actor) || Actor->IsActorBeingDestroyed() || Actors.Contains(Actor)) continue;
		Actors.Add(Actor);
		// Ownership also covers props whose native attachment was unsuccessful.
		for (AActor* Child : Actor->Children) Pending.Add(Child);
		TArray<AActor*> Attached;
		Actor->GetAttachedActors(Attached);
		Pending.Append(Attached);
	}
	const FName Folder = GetFolderPath();
	for (AActor* Actor : Actors)
		if (Actor != this && Actor->GetFolderPath() != Folder)
		{
			Actor->Modify();
			Actor->SetFolderPath(Folder);
		}
#endif
}

bool AOsmRoadScene::BuildSurfaceMeshes()
{
	for (AJunctionActor* Junction : Junctions)
	{
		Junction->Build();
		RemoveCornersWithoutJunctionSurface(Junction);
	}
	ReconcileTrackedJunctionLinkRoads();
	if (!PersistImportedConnectors()) return false;
	// Native mesh construction includes boundary props from the applied style.
	// Obtain slots from the final junctions so meshes and props use the same cuts.
	const TMap<ARoadActor*, TArray<FJunctionSlot>> SlotsByRoad = GetAllJunctionSlots();
	for (ARoadActor* Road : Roads)
	{
		const TArray<FJunctionSlot>* Slots = SlotsByRoad.Find(Road);
		Road->BuildMesh(Slots ? *Slots : TArray<FJunctionSlot>());
	}
	ReconcileTrackedJunctionLinkRoads();
	// Rebuild persisted connectors with their native style-derived boundary props.
	for (ARoadActor* Link : JunctionLinkRoads) Link->BuildMesh({});
	UpdateGeneratedActorFolders();
	return true;
}

bool AOsmRoadScene::BuildImportedNetwork()
{
	if (bBuildingImportedNetwork || IsActorBeingDestroyed() || !GetWorld()) return false;
	TGuardValue<bool> BuildingGuard(bBuildingImportedNetwork, true);
	if (!SolveJunctions() || !BuildSurfaceMeshes()) return false;
	UE_LOG(LogImportedRoadConstructor, Display, TEXT("Built %d Road Constructor roads and %d OSM junctions."), Roads.Num(), Junctions.Num());
	return !Roads.IsEmpty();
}

void AOsmRoadScene::PostLoad()
{
	// ARoadScene::PostLoad/BeginPlay schedule geometry-based connectivity discovery.
	// Keep saved output unchanged; generation is an explicit editor action.
	// ReSharper disable once CppReferenceToOverriddenVirtualFunction
	AActor::PostLoad();
	JunctionLinkRoads.Reset();
	for (ARoadActor* Road : ImportedConnectorRoads) if (IsValid(Road)) JunctionLinkRoads.AddUnique(Road);
}

void AOsmRoadScene::BeginPlay()
{
	// Keep normal actor/component BeginPlay, without the plugin's next-tick Refresh All.
	// ReSharper disable once CppReferenceToOverriddenVirtualFunction
	AActor::BeginPlay();
}

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
