#include "OsmRoadScene.h"

#include "Components/PrimitiveComponent.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "Templates/UnrealTemplate.h"
#include "Algo/Reverse.h"

#include "Components/StaticMeshComponent.h"
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
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
				const double End = FMath::Clamp(Slot.OutputDist(), 0.0, Length());
				if (FMath::IsFinite(Start) && FMath::IsFinite(End) && End > Start && Station >= Start - 0.01 && Station <= End + 0.01)
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
	AActor::PostLoad();
	RoadConstructorGeneratedMeshComponent = Cast<UPrimitiveComponent>(GetRootComponent());
}

#if WITH_EDITOR
void AOsmRoadActor::PostEditMove(bool bFinished)
{
	AActor::PostEditMove(bFinished);
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
	AActor::PostEditChangeProperty(PropertyChangedEvent);
	if (AOsmRoadScene* Scene = Cast<AOsmRoadScene>(GetScene())) Scene->QueueImportedRebuild();
}

void AOsmRoadActor::PostEditUndo()
{
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
		for (const FEndpoint& Arm : Arms) Junction->AddRoad(Arm.Road, Arm.Distance);
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
		const TMap<ARoadActor*, TArray<FJunctionSlot>> SlotsByRoad = GetAllJunctionSlots();
		for (const auto& Pair : SlotsByRoad)
		{
			for (int32 Index = 1; Index < Pair.Value.Num(); ++Index)
			{
				FJunctionSlot Before = Pair.Value[Index - 1];
				FJunctionSlot After = Pair.Value[Index];
				if (Before.Junction != After.Junction && Before.OutputDist() >= After.InputDist())
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

int32 AOsmRoadScene::RemoveCornersWithoutJunctionSurface(AJunctionActor* Junction)
{
	if (!bRemoveCornersWithoutJunctionSurface || !IsValid(Junction)) return 0;
	// Road Constructor builds the central asphalt on the junction root. Its mesh
	// builder leaves that component without a static mesh when triangulation fails,
	// while the independent corner roads may still contain sidewalks and curbs.
	const UStaticMeshComponent* Surface = Cast<UStaticMeshComponent>(Junction->GetRootComponent());
	if (!Surface || Surface->GetStaticMesh()) return 0;

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
		AOsmRoadActor* ImportedRoad = Cast<AOsmRoadActor>(Road);
		if (!IsValid(ImportedRoad) || !ImportedRoad->RestoreImportedCurve())
		{
			UE_LOG(LogImportedRoadConstructor, Error, TEXT("Cannot restore imported road %s."), *GetNameSafe(Road));
			return false;
		}
	}
	if (!SolveImportedJunctions()) return false;
	int32 RemovedPoints = 0;
	if (bCleanJunctionApproaches)
	{
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
	UE_LOG(LogImportedRoadConstructor, Display, TEXT("Built %d Road Constructor roads and %d OSM junctions."), Roads.Num(), Junctions.Num());
	return !Roads.IsEmpty();
}

void AOsmRoadScene::QueueImportedRebuild()
{
	if (bImportedRebuildQueued || HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || IsActorBeingDestroyed()) return;
	bImportedRebuildQueued = true;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float)
	{
		bImportedRebuildQueued = false;
		if (GetWorld() && !IsActorBeingDestroyed()) BuildImportedNetwork();
		return false;
	}));
}

void AOsmRoadScene::PostLoad()
{
	// ARoadScene::PostLoad/BeginPlay schedule geometry-based connectivity discovery.
	// Restore our serialized control points and OSM connectivity instead.
	AActor::PostLoad();
	QueueImportedRebuild();
}

void AOsmRoadScene::BeginPlay()
{
	AActor::BeginPlay();
	QueueImportedRebuild();
}

#if WITH_EDITOR
void AOsmRoadScene::PostEditUndo()
{
	AActor::PostEditUndo();
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

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportedRoadConstructorMeshTest, "Landscapes.RoadConstructor.NativeJunctionMeshes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportedRoadConstructorMeshTest::RunTest(const FString& Parameters)
{
	URoadStyle* Style = LoadObject<URoadStyle>(nullptr, TEXT("/RoadConstructor/RoadStyles/RS_Street_2Lane.RS_Street_2Lane"));
	if (!TestNotNull(TEXT("Installed street style is available"), Style)) return false;
	const UWorld::InitializationValues Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Isolated test world"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
	AOsmRoadScene* Scene = World->SpawnActor<AOsmRoadScene>();
	if (!Scene)
	{
		World->DestroyWorld(false);
		GEngine->DestroyWorldContext(World);
		return false;
	}
	AOsmRoadActor* CurvedRoad = World->SpawnActor<AOsmRoadActor>();
	if (TestNotNull(TEXT("Curved road test actor"), CurvedRoad))
	{
		CurvedRoad->InitWithStyle(Style, 100.0);
		CurvedRoad->ImportedPositions = { FVector(0, 0, 100), FVector(10000, 0, 100), FVector(20000, 10000, 100) };
		CurvedRoad->ImportedCurveRadius = 5000000.0;
		TestTrue(TEXT("Three-point road restores its native curve"), CurvedRoad->RestoreImportedCurve());
		TestTrue(TEXT("Broad radius produces curved road geometry between imported controls"),
			CurvedRoad->RoadSegments.ContainsByPredicate([](const FRoadSegment& Segment)
			{
				return FMath::Abs(Segment.StartCurv) > 1e-8 || FMath::Abs(Segment.EndCurv) > 1e-8;
			}));
		CurvedRoad->Destroy();
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		AOsmRoadActor* Road = World->SpawnActor<AOsmRoadActor>();
		if (!Road) continue;
		Road->SetOwner(Scene);
		Road->AttachToActor(Scene, FAttachmentTransformRules::KeepWorldTransform);
		Road->InitWithStyle(Style, 100.0);
		Road->bHasGround = false;
		Road->StartNodeKey = TEXT("junction:0");
		Road->EndNodeKey = FString::Printf(TEXT("end:%d"), Index);
		const FVector Ends[] = { FVector(-10000, 0, 200), FVector(10000, 0, 300), FVector(0, 10000, 400) };
		const FVector Start(0, 0, 100);
		FVector NearPoint = FMath::Lerp(Start, Ends[Index], 0.02);
		NearPoint += Index == 2 ? FVector(5, 0, 0) : FVector(0, 5, 0);
		Road->ImportedPositions = { Start, NearPoint, FMath::Lerp(Start, Ends[Index], 0.6), Ends[Index] };
		if (Index == 1)
		{
			Algo::Reverse(Road->ImportedPositions);
			Swap(Road->StartNodeKey, Road->EndNodeKey);
		}
		Scene->Roads.Add(Road);
	}
	TestTrue(TEXT("Native solver builds an imported T junction"), Scene->BuildImportedNetwork());
	TestEqual(TEXT("One shared OSM node creates one junction"), Scene->Junctions.Num(), 1);
	for (ARoadActor* Road : Scene->Roads)
	{
		const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Road->GetRootComponent());
		TestTrue(TEXT("Source road receives a generated mesh"), Mesh && Mesh->GetStaticMesh() && Mesh->GetStaticMesh()->GetNumLODs() > 0);
		const AOsmRoadActor* ImportedRoad = CastChecked<AOsmRoadActor>(Road);
		TestEqual(TEXT("Source points remain available for regeneration"), ImportedRoad->ImportedPositions.Num(), 4);
		TestEqual(TEXT("Only the ordinary control inside the solved junction is removed"), Road->RoadPoints.Num(), 3);
		TestEqual(TEXT("Imported start elevation is retained"), Road->GetHeight(0), ImportedRoad->ImportedPositions[0].Z);
		TestEqual(TEXT("Imported end elevation is retained"), Road->GetHeight(Road->Length()), ImportedRoad->ImportedPositions.Last().Z);
		TestTrue(TEXT("OSM endpoints remain fixed"), Road->RoadPoints[0].Pos.Equals(FVector2D(ImportedRoad->ImportedPositions[0]), 0.001) &&
			Road->RoadPoints.Last().Pos.Equals(FVector2D(ImportedRoad->ImportedPositions.Last()), 0.001));
	}
	if (Scene->Junctions.Num() == 1)
	{
		const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Scene->Junctions[0]->GetRootComponent());
		TestTrue(TEXT("Junction receives its central asphalt mesh"), Mesh && Mesh->GetStaticMesh() && Mesh->GetStaticMesh()->GetNumLODs() > 0);
		TestTrue(TEXT("Junction generates corner connector roads"), Scene->JunctionLinkRoads.Num() > 0);
		for (const FJunctionGate& Gate : Scene->Junctions[0]->Gates)
		{
			for (const FJunctionLink& Link : Gate.Links)
			{
				if (Link.Road && Link.Road->bGeneratedJunctionUVContinuation)
				{
					TestTrue(TEXT("Generated arm uses the final source-road cut station"), FMath::IsNearlyEqual(Link.Road->GeneratedJunctionUVSourceDistance, Gate.Dist, 0.01));
				}
			}
		}
	}
	if (!Scene->Roads.IsEmpty())
	{
		const UStaticMeshComponent* BeforeMesh = Cast<UStaticMeshComponent>(Scene->Roads[0]->GetRootComponent());
		const double BeforeMeshZ = BeforeMesh && BeforeMesh->GetStaticMesh() ? BeforeMesh->GetStaticMesh()->GetBounds().Origin.Z : 0.0;
		for (ARoadActor* Road : Scene->Roads)
		{
			for (FVector& Position : CastChecked<AOsmRoadActor>(Road)->ImportedPositions) Position.Z += 100.0;
		}
		TestTrue(TEXT("Raised source roads regenerate"), Scene->BuildImportedNetwork());
		const UStaticMeshComponent* AfterMesh = Cast<UStaticMeshComponent>(Scene->Roads[0]->GetRootComponent());
		TestTrue(TEXT("A 100 cm source elevation change raises the rendered road mesh 100 cm"),
			BeforeMesh && BeforeMesh->GetStaticMesh() && AfterMesh && AfterMesh->GetStaticMesh() &&
			FMath::IsNearlyEqual(AfterMesh->GetStaticMesh()->GetBounds().Origin.Z - BeforeMeshZ, 100.0, 1.0));
		for (ARoadActor* Road : Scene->Roads)
		{
			for (FVector& Position : CastChecked<AOsmRoadActor>(Road)->ImportedPositions) Position.Z -= 100.0;
		}
	}
	TestTrue(TEXT("Regeneration succeeds without geometric crossing discovery"), Scene->BuildImportedNetwork());
	TestEqual(TEXT("Regeneration does not duplicate junctions"), Scene->Junctions.Num(), 1);
	for (ARoadActor* Road : Scene->Roads)
	{
		TestEqual(TEXT("Regeneration repeats cleanup from the preserved source"), Road->RoadPoints.Num(), 3);
		for (const FJunctionSlot& Slot : Scene->GetJunctionSlots(Road))
		{
			if (Slot.HasOutput()) TestTrue(TEXT("Final outgoing cut belongs to the updated road"), Slot.OutputDist() > 0 && Slot.OutputDist() < Road->Length());
			if (Slot.HasInput()) TestTrue(TEXT("Final incoming cut belongs to the updated road"), Slot.InputDist() > 0 && Slot.InputDist() < Road->Length());
		}
	}
	if (Scene->Junctions.Num() == 1)
	{
		// Simulate the native triangulator leaving the central junction surface
		// empty. Only that junction's generated corner roads should be removed.
		AJunctionActor* Junction = Scene->Junctions[0];
		UStaticMeshComponent* Surface = Cast<UStaticMeshComponent>(Junction->GetRootComponent());
		if (TestNotNull(TEXT("Junction has a static surface component"), Surface))
		{
			TestEqual(TEXT("Healthy junction keeps its corner roads"), Scene->RemoveCornersWithoutJunctionSurface(Junction), 0);
			Surface->SetStaticMesh(nullptr);
			TestEqual(TEXT("Failed surface removes only corner roads"), Scene->RemoveCornersWithoutJunctionSurface(Junction), Junction->Gates.Num());
			for (const FJunctionGate& Gate : Junction->Gates)
			{
				TestNull(TEXT("Failed junction has no sidewalk/curb corner road"), Gate.Links[AJunctionActor::CornerIndex].Road);
			}
			for (ARoadActor* Road : Scene->Roads)
			{
				const UStaticMeshComponent* RoadMesh = Cast<UStaticMeshComponent>(Road->GetRootComponent());
				TestTrue(TEXT("Failed junction retains its approach road meshes"), RoadMesh && RoadMesh->GetStaticMesh());
			}
		}
	}
	TestTrue(TEXT("Regeneration restores a healthy junction after corner removal"), Scene->BuildImportedNetwork());
	if (Scene->Junctions.Num() == 1)
	{
		TestTrue(TEXT("Healthy junction regenerates its corner roads"), Scene->JunctionLinkRoads.Num() > 0);
	}
	ARoadActor* Branch = Scene->Roads.Pop();
	Branch->Destroy();
	for (ARoadActor* Road : Scene->Roads)
	{
		AOsmRoadActor* ImportedRoad = CastChecked<AOsmRoadActor>(Road);
		ImportedRoad->ImportedPositions = { ImportedRoad->ImportedPositions[0], ImportedRoad->ImportedPositions.Last() };
	}
	TestTrue(TEXT("Two straight way ends can be rebuilt"), Scene->BuildImportedNetwork());
	for (ARoadActor* Road : Scene->Roads)
	{
		const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Road->GetRootComponent());
		TestTrue(TEXT("Straight seam keeps both approach meshes"), Mesh && Mesh->GetStaticMesh() && Mesh->GetStaticMesh()->GetNumLODs() > 0);
		for (const FJunctionSlot& Slot : Scene->GetJunctionSlots(Road))
		{
			const double VisibleLength = Slot.HasOutput() ? Road->Length() - Slot.OutputDist() : Slot.InputDist();
			TestTrue(TEXT("Straight seam does not consume an entire road"), VisibleLength > 1.0);
		}
	}
	Scene->Destroy();
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}
#endif
