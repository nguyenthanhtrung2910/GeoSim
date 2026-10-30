#include "OsmRoadScene.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Algo/Reverse.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Editor.h"
#include "Misc/AutomationTest.h"
#include "ScopedTransaction.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportedRoadLifecycleTest, "Landscapes.RoadConstructor.ImportedLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportedRoadLifecycleTest::RunTest(const FString& Parameters)
{
	URoadStyle* Style = LoadObject<URoadStyle>(nullptr, TEXT("/RoadConstructor/RoadStyles/RS_Street_2Lane.RS_Street_2Lane"));
	if (!TestNotNull(TEXT("Installed street style"), Style)) return false;
	for (const EWorldType::Type WorldType : { EWorldType::Editor, EWorldType::Game })
	{
		const UWorld::InitializationValues Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreateNavigation(false).CreateAISystem(false);
		UWorld* World = UWorld::CreateWorld(WorldType, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
		if (!TestNotNull(TEXT("Isolated lifecycle world"), World)) return false;
		GEngine->CreateNewWorldContext(WorldType).SetCurrentWorld(World);
		AOsmRoadScene* Scene = World->SpawnActor<AOsmRoadScene>();
		if (!TestNotNull(TEXT("Imported lifecycle scene"), Scene))
		{
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
			return false;
		}
		auto AddRoad = [this, World, Scene, Style](FString Start, FString End, FVector A, FVector B)
		{
			AOsmRoadActor* Road = World->SpawnActor<AOsmRoadActor>();
			if (!TestNotNull(TEXT("Lifecycle road"), Road)) return;
			Road->AttachToActor(Scene, FAttachmentTransformRules::KeepWorldTransform);
			Road->InitWithStyle(Style, 100.0);
			Road->bHasGround = false;
			Road->ImportedPositions = { A, B };
			Road->StartNodeKey = Start;
			Road->EndNodeKey = End;
			Scene->Roads.Add(Road);
		};
		AddRoad(TEXT("west"), TEXT("shared"), FVector(-10000, 0, 100), FVector(0, 0, 100));
		AddRoad(TEXT("shared"), TEXT("north"), FVector(0, 0, 100), FVector(0, 10000, 100));
		AddRoad(TEXT("shared"), TEXT("east"), FVector(0, 0, 100), FVector(10000, 0, 100));
		// This road crosses the first road geometrically, without a shared OSM node.
		AddRoad(TEXT("cross-south"), TEXT("cross-north"), FVector(-5000, -10000, 100), FVector(-5000, 10000, 100));
		TestTrue(TEXT("Shared-node network builds"), Scene->BuildImportedNetwork());
		auto CheckConnectivity = [this, Scene]()
		{
			TestEqual(TEXT("Lifecycle preserves the four source roads"), Scene->Roads.Num(), 4);
			TestEqual(TEXT("Only the shared node creates a junction; XY crossing stays disconnected"), Scene->Junctions.Num(), 1);
			TestFalse(TEXT("No plugin connectivity refresh is queued"), Scene->bPendingDeferredConnectivityRefresh);
			TestFalse(TEXT("No plugin scene rebuild is queued"), Scene->bPendingDeferredSceneRebuild);
			TestFalse(TEXT("No plugin post-load discovery is queued"), Scene->bPendingDeferredPostLoadAutoConnectivityRefresh);
			TestFalse(TEXT("No plugin post-load junction validation is queued"), Scene->bPendingDeferredPostLoadJunctionValidation);
			TestFalse(TEXT("No plugin railing junction refresh is queued"), Scene->bPendingDeferredPostLoadGeneratedRailingJunctionRefresh);
		};
		auto DrainDeferredWork = [World]()
		{
			TGuardValue<uint64> RestoreFrameCounter(GFrameCounter, GFrameCounter);
			for (int32 Tick = 0; Tick < 5; ++Tick)
			{
				++GFrameCounter;
				FTSTicker::GetCoreTicker().Tick(0.1f);
				World->GetTimerManager().Tick(0.1f);
			}
		};
		CheckConnectivity();
		// Even missing output must not trigger implicit regeneration on load/play.
		UStaticMeshComponent* MeshComponent = CastChecked<UStaticMeshComponent>(Scene->Roads[0]->GetRootComponent());
		UStaticMesh* SavedMesh = MeshComponent->GetStaticMesh();
		AJunctionActor* SavedJunction = Scene->Junctions[0];
		MeshComponent->SetStaticMesh(nullptr);
		auto CheckNoLifecycleRebuild = [this, Scene, MeshComponent, SavedMesh, SavedJunction]()
		{
			TestNull(TEXT("Load/play leaves missing road output unchanged"), MeshComponent->GetStaticMesh());
			TestTrue(TEXT("Load/play preserves existing junction actors"), Scene->Junctions.Num() == 1 && Scene->Junctions[0] == SavedJunction);
			MeshComponent->SetStaticMesh(SavedMesh);
		};
		if (WorldType == EWorldType::Editor)
		{
			// Exercise the load callbacks against a complete, renderable network.
			Scene->PostLoad();
			for (ARoadActor* Road : Scene->Roads) Road->PostLoad();
			CheckConnectivity();
			DrainDeferredWork();
			CheckConnectivity();
			CheckNoLifecycleRebuild();

		}
		else
		{
			Scene->DispatchBeginPlay();
			for (ARoadActor* Road : Scene->Roads) Road->DispatchBeginPlay();
			TestTrue(TEXT("Normal actor BeginPlay is preserved"), Scene->HasActorBegunPlay());
			DrainDeferredWork();
			CheckNoLifecycleRebuild();
		}
		DrainDeferredWork();
		CheckConnectivity();
		Scene->Destroy();
		World->DestroyWorld(false);
		GEngine->DestroyWorldContext(World);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImportedRoadSceneUndoTest, "Landscapes.RoadConstructor.SceneScopedUndo", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FImportedRoadSceneUndoTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("Editor transaction support"), GEditor)) return false;
	URoadStyle* Style = LoadObject<URoadStyle>(nullptr, TEXT("/RoadConstructor/RoadStyles/RS_Street_2Lane.RS_Street_2Lane"));
	if (!TestNotNull(TEXT("Installed street style"), Style)) return false;
	const UWorld::InitializationValues Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Scene undo world"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
	TGuardValue<UWorldProxy, UWorld*> WorldGuard(GWorld, World);
	auto MakeScene = [World, Style](double Y)
	{
		AOsmRoadScene* Scene = World->SpawnActor<AOsmRoadScene>();
		const TArray<FVector> Ends = { FVector(-10000, Y, 100), FVector(10000, Y, 100), FVector(0, Y + 10000, 100) };
		for (int32 Index = 0; Index < Ends.Num(); ++Index)
		{
			AOsmRoadActor* Road = World->SpawnActor<AOsmRoadActor>();
			Road->AttachToActor(Scene, FAttachmentTransformRules::KeepWorldTransform);
			Road->SetFlags(RF_Transactional);
			Road->InitWithStyle(Style, 100);
			Road->bHasGround = false;
			Road->ImportedPositions = { FVector(0, Y, 100), Ends[Index] };
			Road->StartNodeKey = TEXT("shared");
			Road->EndNodeKey = FString::FromInt(Index);
			Scene->Roads.Add(Road);
		}
		return Scene;
	};
	AOsmRoadScene* EditedScene = MakeScene(0);
	AOsmRoadScene* OtherScene = MakeScene(50000);
	if (TestTrue(TEXT("Edited scene builds"), EditedScene->BuildImportedNetwork()) &&
		TestTrue(TEXT("Other scene builds"), OtherScene->BuildImportedNetwork()))
	{
		ARoadActor* Road = EditedScene->Roads[0];
		UStaticMeshComponent* Mesh = CastChecked<UStaticMeshComponent>(Road->GetRootComponent());
		UStaticMesh* OtherMesh = CastChecked<UStaticMeshComponent>(OtherScene->Roads[0]->GetRootComponent())->GetStaticMesh();
		const auto OtherJunctions = OtherScene->Junctions;
		const FVector2D OriginalPoint = Road->RoadPoints.Last().Pos;
		{
			FScopedTransaction Transaction(NSLOCTEXT("GeoSimTests", "SceneRoadUndo", "Edit imported road"));
			Road->Modify();
			Mesh->Modify();
			Road->RoadPoints.Last().Pos.Y += 100;
			Road->UpdateCurve();
			Road->BuildMesh(EditedScene->GetJunctionSlots(Road));
		}
		TestTrue(TEXT("Real road edit can be undone"), GEditor->UndoTransaction());
		{
			TGuardValue<uint64> FrameGuard(GFrameCounter, GFrameCounter);
			for (int32 Tick = 0; Tick < 5; ++Tick)
			{
				++GFrameCounter;
				FTSTicker::GetCoreTicker().Tick(0.1f);
				World->GetTimerManager().Tick(0.1f);
			}
		}
		TestTrue(TEXT("Undo restores road authoring data"), Road->RoadPoints.Last().Pos.Equals(OriginalPoint));
		TestTrue(TEXT("Undo restores a visible road mesh"), Mesh->GetStaticMesh() != nullptr);
		TestTrue(TEXT("Undo preserves the other scene mesh"), CastChecked<UStaticMeshComponent>(OtherScene->Roads[0]->GetRootComponent())->GetStaticMesh() == OtherMesh);
		TestTrue(TEXT("Undo preserves the other scene junctions"), OtherScene->Junctions == OtherJunctions);
		TestFalse(TEXT("Undo queues no refresh on the other scene"), OtherScene->bPendingDeferredConnectivityRefresh);
	}
	EditedScene->Destroy();
	OtherScene->Destroy();
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}

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
	TestEqual(TEXT("Two arms do not create a junction actor"), Scene->Junctions.Num(), 0);
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
