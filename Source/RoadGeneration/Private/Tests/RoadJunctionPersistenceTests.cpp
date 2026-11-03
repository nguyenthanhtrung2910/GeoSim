#include "OsmRoadScene.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "RoadGenerator.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoadJunctionPersistenceTest, "Landscapes.RoadConstructor.JunctionPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoadConnectorConversionTest, "Landscapes.RoadConstructor.ConnectorConversion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadConnectorConversionTest::RunTest(const FString& Parameters)
{
	URoadStyle* Style = LoadObject<URoadStyle>(nullptr, TEXT("/RoadConstructor/RoadStyles/RS_Street_2Lane.RS_Street_2Lane"));
	if (!TestNotNull(TEXT("Street style is available"), Style)) return false;
	const UWorld::InitializationValues Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Conversion test world"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
	AOsmRoadScene* Scene = World->SpawnActor<AOsmRoadScene>();
	AOsmJunctionActor* Junction = World->SpawnActor<AOsmJunctionActor>();
	Junction->SetOwner(Scene);
	Junction->AttachToActor(Scene, FAttachmentTransformRules::KeepWorldTransform);
	Scene->Junctions.Add(Junction);
	ARoadActor* Original = World->SpawnActor<ARoadActor>();
	Original->SetFlags(RF_Transient);
	Original->SetOwner(Scene);
	Original->AttachToActor(Junction, FAttachmentTransformRules::KeepWorldTransform);
	Original->InitWithStyle(Style, 100);
	Original->SetGeneratedJunctionInternalRoad(true);
	Original->bHasGround = false;
	Original->RoadPoints.AddDefaulted_GetRef().Pos = FVector2D(0, 0);
	Original->RoadPoints.AddDefaulted_GetRef().Pos = FVector2D(10000, 0);
	Original->UpdateCurve();
	Scene->JunctionLinkRoads.Add(Original);
	Junction->Gates.AddDefaulted_GetRef().Links.AddDefaulted_GetRef().Road = Original;
	TArray<FPolyline> ExpectedCurves;
	if (TestTrue(TEXT("Style has an outer boundary"), Original->Boundaries.Num() > 1))
	{
		// A solved connector may have a collapsed outer edge even though its base
		// curve is valid. Conversion must preserve this result without UpdateLanes.
		Original->Boundaries.Last()->Curve.Points.SetNum(1);
		for (URoadBoundary* Boundary : Original->Boundaries) ExpectedCurves.Add(Boundary->Curve);
		if (TestTrue(TEXT("Conversion accepts the native collapsed boundary"), Scene->PersistImportedConnectors()) &&
			TestEqual(TEXT("Converted connector stays registered"), Scene->JunctionLinkRoads.Num(), 1))
		{
			ARoadActor* Saved = Scene->JunctionLinkRoads[0];
			TestTrue(TEXT("Connector uses the persistent actor class"), Saved->IsA<AOsmRoadActor>());
			TestEqual(TEXT("Collapsed boundary remains collapsed"), Saved->Boundaries.Last()->Curve.Points.Num(), 1);
			TestEqual(TEXT("Boundary count is preserved"), Saved->Boundaries.Num(), ExpectedCurves.Num());
			for (int32 Index = 0; Index < FMath::Min(Saved->Boundaries.Num(), ExpectedCurves.Num()); ++Index)
			{
				TestTrue(TEXT("Copied boundary is owned by the saved connector"), Saved->Boundaries[Index]->GetOuter() == Saved);
				TestTrue(TEXT("Conversion preserves every solved curve exactly"), FPolyline::StaticStruct()->CompareScriptStruct(
					&Saved->Boundaries[Index]->Curve, &ExpectedCurves[Index], 0));
			}
		}
	}
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}

bool FRoadJunctionPersistenceTest::RunTest(const FString& Parameters)
{
	URoadStyle* Base = LoadObject<URoadStyle>(nullptr, TEXT("/RoadConstructor/RoadStyles/RS_Street_2Lane.RS_Street_2Lane"));
	if (!TestNotNull(TEXT("Street style is available"), Base)) return false;
	const FString Mount = TEXT("/RoadJunctionPersistence_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/");
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / Mount.Mid(1));
	IFileManager::Get().MakeDirectory(*Directory, true);
	FPackageName::RegisterMountPoint(Mount, Directory);
	const FString WorldName = Mount + TEXT("TestWorld");
	UPackage* WorldPackage = CreatePackage(*WorldName);
	const UWorld::InitializationValues Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("TestWorld"), WorldPackage, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Persistence test world"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
	World->PersistentLevel->SetUseExternalActors(true);
	FActorSpawnParameters Params;
	Params.bCreateActorPackage = false;
	Params.ObjectFlags = RF_Public | RF_Transactional;
	AOsmRoadScene* Scene = World->SpawnActor<AOsmRoadScene>(Params);
	if (!Scene) return false;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		AOsmRoadActor* Road = World->SpawnActor<AOsmRoadActor>(Params);
		if (!Road) return false;
		Road->SetOwner(Scene);
		Road->AttachToActor(Scene, FAttachmentTransformRules::KeepWorldTransform);
		URoadStyle* Style = RoadGeneratorInternal::CreateLaneCountStyle(Scene, Base, 2);
		Scene->ImportedWayStyles.Add(Index + 1, Style);
		Road->InitWithStyle(Style, 100);
		Road->bHasGround = false;
		Road->StartNodeKey = TEXT("shared");
		Road->EndNodeKey = FString::Printf(TEXT("end:%d"), Index);
		const FVector Ends[] = { FVector(-10000, 0, 100), FVector(10000, 0, 100), FVector(0, 10000, 100) };
		Road->ImportedPositions = { FVector(0, 0, 100), Ends[Index] };
		Scene->Roads.Add(Road);
	}
	bool bSaved = false;
	FString ScenePath = Scene->GetPathName();
	FString FirstConnectorPackage;
	int32 ExpectedLinks = 0;
	TMap<FString, bool> ExpectedMeshes;
	TMap<FString, FVector> ExpectedBounds;
	TWeakObjectPtr<AOsmRoadScene> OriginalScene = Scene;
	if (TestTrue(TEXT("Imported junction network builds"), Scene->BuildImportedNetwork()))
	{
		TestEqual(TEXT("Shared node creates one junction"), Scene->Junctions.Num(), 1);
		ExpectedLinks = Scene->JunctionLinkRoads.Num();
		TestTrue(TEXT("Junction has connector roads to persist"), ExpectedLinks > 0);
		TArray<AActor*> Actors = { Scene };
		for (ARoadActor* Road : Scene->Roads) Actors.Add(Road);
		for (AJunctionActor* Junction : Scene->Junctions) Actors.Add(Junction);
		for (ARoadActor* Link : Scene->JunctionLinkRoads)
		{
			Actors.Add(Link);
			TestTrue(TEXT("Connector uses the load-safe imported actor class"), Link->IsA<AOsmRoadActor>());
			TestFalse(TEXT("Connector is not transient"), Link->HasAnyFlags(RF_Transient));
			TestTrue(TEXT("Connector owns its base curve"), Link->BaseCurve && Link->BaseCurve->IsIn(Link));
			for (const URoadBoundary* Boundary : Link->Boundaries) TestTrue(TEXT("Connector owns its boundaries"), Boundary && Boundary->IsIn(Link));
			TestTrue(TEXT("Connector style is owned by the connector"), Link->LastAppliedRoadStyle && Link->LastAppliedRoadStyle->IsIn(Link));
		}
		for (int32 Index = 0; Index < Actors.Num(); ++Index)
		{
			const UStaticMeshComponent* Component = Cast<UStaticMeshComponent>(Actors[Index]->GetRootComponent());
			const UStaticMesh* Mesh = Component ? Component->GetStaticMesh().Get() : nullptr;
			ExpectedMeshes.Add(Actors[Index]->GetPathName(), Mesh != nullptr);
			if (Mesh) ExpectedBounds.Add(Actors[Index]->GetPathName(), Mesh->GetBounds().BoxExtent);
			Actors[Index]->SetPackageExternal(true, false, CreatePackage(*(Mount + FString::Printf(TEXT("Actor_%d"), Index))));
			Actors[Index]->SetFlags(RF_Public | RF_Transactional);
		}
		if (ExpectedLinks > 0) FirstConnectorPackage = Scene->JunctionLinkRoads[0]->GetPackage()->GetName();
		auto Save = [this](UPackage* Package, UObject* Asset, const FString& Extension)
		{
			FSavePackageArgs Args;
			Args.Error = GWarn;
			return TestTrue(TEXT("Generated actor package saves"), UPackage::SavePackage(Package, Asset,
				*FPackageName::LongPackageNameToFilename(Package->GetName(), Extension), Args));
		};
		bSaved = Save(WorldPackage, World, FPackageName::GetMapPackageExtension());
		for (AActor* Actor : Actors) bSaved = Save(Actor->GetPackage(), Actor, FPackageName::GetAssetPackageExtension()) && bSaved;
	}
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	CollectGarbage(RF_NoFlags);
	TestFalse(TEXT("Original scene is gone before disk reload"), OriginalScene.IsValid());
	if (bSaved && !FirstConnectorPackage.IsEmpty())
	{
		// Loading a connector package first exercises cross-package references, as
		// World Partition may load external actors in a different order each time.
		TestNotNull(TEXT("External connector package reloads"), LoadPackage(nullptr, *FirstConnectorPackage, LOAD_None));
		AOsmRoadScene* Reloaded = FindObject<AOsmRoadScene>(nullptr, *ScenePath);
		if (TestNotNull(TEXT("Imported scene reloads from disk"), Reloaded))
		{
			TestEqual(TEXT("Junction survives disk reload"), Reloaded->Junctions.Num(), 1);
			TestEqual(TEXT("Connector registry survives disk reload"), Reloaded->JunctionLinkRoads.Num(), ExpectedLinks);
			TArray<AActor*> Actors;
			TSet<ARoadActor*> ReferencedConnectors;
			for (ARoadActor* Road : Reloaded->Roads) Actors.Add(Road);
			for (ARoadActor* Link : Reloaded->JunctionLinkRoads) Actors.Add(Link);
			for (AJunctionActor* Junction : Reloaded->Junctions)
			{
				Actors.Add(Junction);
				TestTrue(TEXT("Junction uses the load-safe imported class"), Junction->IsA<AOsmJunctionActor>());
				for (const FJunctionGate& Gate : Junction->Gates)
				{
					TestTrue(TEXT("Transient gate road reference is restored"), IsValid(Gate.Road) && Reloaded->Roads.Contains(Gate.Road));
					for (const FJunctionLink& Link : Gate.Links)
						if (Link.Road)
						{
							TestTrue(TEXT("Gate connector reference is restored"), Reloaded->JunctionLinkRoads.Contains(Link.Road));
							ReferencedConnectors.Add(Link.Road);
						}
				}
			}
			TestEqual(TEXT("Every saved connector is reconnected to the junction graph"), ReferencedConnectors.Num(), ExpectedLinks);
			TArray<UStaticMesh*> Meshes;
			for (AActor* Actor : Actors)
			{
				const UStaticMeshComponent* Component = Cast<UStaticMeshComponent>(Actor->GetRootComponent());
				UStaticMesh* Mesh = Component ? Component->GetStaticMesh().Get() : nullptr;
				const bool bHadMesh = ExpectedMeshes.FindRef(Actor->GetPathName());
				TestEqual(TEXT("Disk reload retains the original mesh presence, including intentionally empty connectors"), Mesh != nullptr, bHadMesh);
				if (bHadMesh && Mesh)
				{
					TestTrue(TEXT("Saved surface mesh data remains renderable"), Mesh->GetNumLODs() > 0);
					TestTrue(TEXT("Saved surface dimensions survive reload"), Mesh->GetBounds().BoxExtent.Equals(ExpectedBounds.FindChecked(Actor->GetPathName()), 0.01));
				}
				Meshes.Add(Mesh);
			}
			for (int32 Tick = 0; Tick < 5; ++Tick)
			{
				FTSTicker::GetCoreTicker().Tick(0.1f);
				if (UWorld* LoadedWorld = Reloaded->GetWorld()) LoadedWorld->GetTimerManager().Tick(0.1f);
			}
			for (int32 Index = 0; Index < Actors.Num(); ++Index)
			{
				const UStaticMeshComponent* Component = Cast<UStaticMeshComponent>(Actors[Index]->GetRootComponent());
				TestTrue(TEXT("Loading does not rebuild saved meshes on deferred ticks"), Component && Component->GetStaticMesh() == Meshes[Index]);
			}
			TestFalse(TEXT("Loading does not queue plugin scene generation"), Reloaded->bPendingDeferredSceneRebuild);
			TestFalse(TEXT("Loading does not queue plugin crossing discovery"), Reloaded->bPendingDeferredPostLoadAutoConnectivityRefresh);
			if (UWorld* LoadedWorld = Reloaded->GetWorld()) LoadedWorld->DestroyWorld(false);
		}
		CollectGarbage(RF_NoFlags);
	}
	FPackageName::UnRegisterMountPoint(Mount, Directory);
	return true;
}
#endif
