#include "OsmBuidingImporter.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "BuildingGenerator.h"
#include "Async/Async.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "LCCommon/LCGenerator.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RulePackage.h"
#include "VitruvioActor.h"
#include "VitruvioComponent.h"
#include "UObject/SoftObjectPath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOsmBuildingImporterLocalFileTest, "GeoSim.BuildingGeneration.LocalOsmImport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOsmBuildingImporterLocalFileTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT(
		"<osm version=\"0.6\">"
		"<node id=\"1\" lon=\"0\" lat=\"0\"/><node id=\"2\" lon=\"0.001\" lat=\"0\"/>"
		"<node id=\"3\" lon=\"0.001\" lat=\"0.001\"/><node id=\"4\" lon=\"0\" lat=\"0.001\"/>"
		"<way id=\"12345678901\"><nd ref=\"1\"/><nd ref=\"2\"/><nd ref=\"3\"/><nd ref=\"4\"/><nd ref=\"1\"/>"
		"<tag k=\"building\" v=\"yes\"/><tag k=\"height\" v=\"12\"/><tag k=\"building:levels\" v=\"3\"/></way>"
		"<way id=\"20\"><nd ref=\"1\"/><nd ref=\"2\"/><tag k=\"highway\" v=\"residential\"/></way></osm>");
	const FString Declaration = TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
	for (const FString Prefix : { FString(), Declaration, Declaration + TEXT("\n") })
	{
		TArray<OsmBuidingImporterInternal::FBuildingFootprint> Footprints;
		TestTrue(TEXT("Inline and separate XML declarations parse"), OsmBuidingImporterInternal::ParseBuildingXml(Prefix + Body, Footprints));
		TestEqual(TEXT("Only building ways are read"), Footprints.Num(), 1);
		if (Footprints.Num() == 1)
		{
			TestEqual(TEXT("Full OSM identity"), Footprints[0].Fields.FindRef(TEXT("osm_id")), FString(TEXT("12345678901")));
			TestEqual(TEXT("Raw floor tag"), Footprints[0].Fields.FindRef(TEXT("building:levels")), FString(TEXT("3")));
		}
	}
	const FString Mixed = Body.Replace(TEXT("<osm version=\"0.6\">"), TEXT(
		"<osm version=\"0.6\"><node id=\"100\" lon=\"1\" lat=\"1\"/><node id=\"101\" lon=\"1.001\" lat=\"1\"/>"
		"<node id=\"102\" lon=\"1.001\" lat=\"1.001\"/><way id=\"99\"><nd ref=\"100\"/><nd ref=\"101\"/>"
		"<nd ref=\"102\"/><nd ref=\"100\"/><tag k=\"building\" v=\"yes\"/></way>"));
	const FString File = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("BuildingCache"), TEXT(".osm"));
	if (!FFileHelper::SaveStringToFile(Declaration + Mixed, *File)) return false;
	const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Isolated test world"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
	AOsmBuidingImporter* Importer = World->SpawnActor<AOsmBuidingImporter>();
	AStaticMeshActor* Terrain = World->SpawnActor<AStaticMeshActor>();
	Terrain->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	Terrain->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Terrain->SetActorScale3D(FVector(10, 10, 1));
	Terrain->SetActorLocation(FVector(0, 0, -50));
	Importer->TerrainSelection.ActorSelectionMode = EActorSelectionMode::Actor;
	Importer->TerrainSelection.Actor = Terrain;
	Importer->LocalFile = File;
	Importer->bSkipCoordinateConversion = true;
	Importer->ScaleX = Importer->ScaleY = 100000;
	TestFalse(TEXT("Importer has no generator interface"), Importer->GetClass()->ImplementsInterface(ULCGenerator::StaticClass()));
	FSoftObjectPath OldClass(TEXT("/Script/BuildingGeneration.OsmBuildingImporter"));
	OldClass.FixupCoreRedirects();
	TestEqual(TEXT("Existing class references redirect"), OldClass.TryLoad(), static_cast<UObject*>(AOsmBuidingImporter::StaticClass()));
	TArray<FImportedBuilding> Empty;
	TestFalse(TEXT("Cache read never imports automatically"), Importer->GetCachedBuildingData(Empty));
	const int32 ActorCount = World->GetActorCount();
	TSharedRef<TFuture<bool>> Future = MakeShared<TFuture<bool>>(Async(EAsyncExecution::Thread, [Importer]() { return Importer->ImportBuildingData(false); }));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Future, World, Importer, File, ActorCount]()
	{
		if (!Future->IsReady()) return false;
		TestTrue(TEXT("Import succeeds with one valid and one out-of-terrain building"), Future->Get());
		TestEqual(TEXT("Importer creates no output actors"), World->GetActorCount(), ActorCount);
		TArray<FImportedBuilding> Data;
		TestTrue(TEXT("Cache is readable without the source being imported again"), Importer->GetCachedBuildingData(Data));
		TestEqual(TEXT("Only complete terrain-supported rings are cached"), Data.Num(), 1);
		if (Data.Num() == 1)
		{
			TestEqual(TEXT("Skipping preserves building identity"), Data[0].OsmId, FString(TEXT("12345678901")));
			TestEqual(TEXT("Distinct vertices only"), Data[0].Points.Num(), 4);
			TestEqual(TEXT("Raw attributes survive caching"), Data[0].Tags.FindRef(TEXT("height")), FString(TEXT("12")));
			ABuildingGenerator* Generator = World->SpawnActor<ABuildingGenerator>();
			Generator->Importer = Importer;
			Generator->RulePackage = LoadObject<URulePackage>(nullptr, TEXT("/Game/ErsiRulePackages/BuildingTextures.BuildingTextures"));
			Generator->GenerateBuildings();
			TestEqual(TEXT("Generator consumes cached record"), Generator->GeneratedActors.Num(), 1);
			if (Generator->GeneratedActors.Num() == 1)
			{
				const AVitruvioActor* Building = Cast<AVitruvioActor>(Generator->GeneratedActors[0].Get());
				if (TestNotNull(TEXT("Generator creates a standard Vitruvio actor"), Building))
				{
					TestTrue(TEXT("Generator uses the normal Vitruvio class"), Building->GetClass() == AVitruvioActor::StaticClass());
					const USplineComponent* Spline = Building->FindComponentByClass<USplineComponent>();
					if (TestNotNull(TEXT("Vitruvio actor receives the cached footprint"), Spline))
						TestTrue(TEXT("Generated spline is closed"), Spline->IsClosedLoop());
				}
			}
			Generator->ClearBuildings();
			TestTrue(TEXT("Clearing generated output keeps imported data"), Importer->GetCachedBuildingData(Data));
		}
		Importer->PointsOffset.Z = 100;
		TestFalse(TEXT("Placement changes invalidate cache reads"), Importer->GetCachedBuildingData(Data));
		Importer->PointsOffset.Z = 0;
		Importer->ClearBuildings();
		TestEqual(TEXT("Clear resets cache count"), Importer->CachedBuildingCount, 0);
		TestFalse(TEXT("Clear removes cached source records"), Importer->GetCachedBuildingData(Data));
		IFileManager::Get().Delete(*File);
		World->DestroyWorld(false);
		GEngine->DestroyWorldContext(World);
		return true;
	}));
	return true;
}
#endif
