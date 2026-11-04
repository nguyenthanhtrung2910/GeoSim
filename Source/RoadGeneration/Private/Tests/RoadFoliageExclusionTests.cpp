#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "RoadGenerator.h"
#include "ImportedRoadData.h"
#include "OsmRoadScene.h"
#include "FoliageExclusionActor.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoadGeneratorFoliageExclusionTest,
	"Landscapes.RoadConstructor.GeneratorFoliageExclusion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadGeneratorFoliageExclusionTest::RunTest(const FString& Parameters)
{
	const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Foliage test world"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
	ARoadGenerator* Generator = World->SpawnActor<ARoadGenerator>();
	ARoadGenerator* OtherGenerator = World->SpawnActor<ARoadGenerator>();
	TestTrue(TEXT("PCG exclusion is enabled by default"), Generator->bFoliageExclusionAffectsPCG);
	TestNull(TEXT("Generator has no separate exclusion Enable checkbox"), FindFProperty<FProperty>(Generator->GetClass(), TEXT("bEnableFoliageExclusion")));
	Generator->FoliageExclusionSampleSpacing = 750;
	Generator->FoliageExclusionLeftExtraWidth = 25;
	Generator->FoliageExclusionRightExtraWidth = 50;
	Generator->FoliageExclusionPCGSideFalloff = 75;
	Generator->FoliageExclusionTargetPCGTag = TEXT("GeneratedRoadExclusion");
	Generator->bFoliageExclusionAffectsLandscapeGrass = false;
	Generator->bFoliageExclusionAffectsProceduralFoliage = false;
	OtherGenerator->bFoliageExclusionAffectsLandscapeGrass = false;
	OtherGenerator->bFoliageExclusionAffectsProceduralFoliage = false;
	auto MakeWay = [](int64 Id, TArray<FImportedRoadPoint> Points)
	{
		FImportedRoadWay Way;
		Way.WayId = Id;
		Way.Tags.Add(TEXT("highway"), TEXT("residential"));
		Way.Points = MoveTemp(Points);
		return Way;
	};
	const TArray<FImportedRoadWay> Ways = {
		MakeWay(1, { { 1, FVector(-10000, 0, 100), true }, { 2, FVector(0, 0, 100), true } }),
		MakeWay(2, { { 2, FVector(0, 0, 100), true }, { 3, FVector(10000, 0, 100), true } }),
		MakeWay(3, { { 2, FVector(0, 0, 100), true }, { 4, FVector(0, 10000, 100), true } }),
		MakeWay(4, { { 5, FVector(200000, 0, 100), true }, { 6, FVector(210000, 0, 100), true } }) };
	if (TestTrue(TEXT("Road network builds"), Generator->BuildRoads(Ways, NAME_None)) &&
		TestTrue(TEXT("Other generator builds"), OtherGenerator->BuildRoads({ MakeWay(20,
			{ { 20, FVector(0, 30000, 100), true }, { 21, FVector(10000, 30000, 100), true } }) }, NAME_None)))
	{
		const TArray<AOsmRoadScene*> Scenes = Generator->GetRoadScenes();
		TestEqual(TEXT("Bulk controls span multiple generated scenes"), Scenes.Num(), 2);
		const TArray<ARoadActor*> Roads = Generator->GetFoliageExclusionRoads();
		TestTrue(TEXT("Junction connectors are included as well as source roads"), Roads.Num() > 4);
		TArray<UStaticMesh*> Meshes;
		for (ARoadActor* Road : Roads) Meshes.Add(CastChecked<UStaticMeshComponent>(Road->GetRootComponent())->GetStaticMesh());
		OtherGenerator->CreateFoliageExclusion();
		ARoadActor* OtherRoad = OtherGenerator->GetFoliageExclusionRoads()[0];
		AFoliageExclusionActor* OtherCorridor = OtherRoad->FoliageExclusionActor;
		TestNotNull(TEXT("Other generator has an independent exclusion actor"), OtherCorridor);
		Generator->CreateFoliageExclusion();
		int32 CorridorCount = 0;
		for (ARoadActor* Road : Roads)
		{
			TestTrue(TEXT("Apply enables each road without a checkbox"), Road->bEnableFoliageExclusion);
			TestEqual(TEXT("Spacing is copied to each road"), Road->FoliageExclusionSampleSpacing, 750.0);
			TestEqual(TEXT("Extra width is copied to each road"), Road->FoliageExclusionRightExtraWidth, 50.0);
			TestEqual(TEXT("PCG padding is copied to each road"), Road->FoliageExclusionPCGSideFalloff, 75.0);
			if (AFoliageExclusionActor* Corridor = Road->FoliageExclusionActor)
			{
				++CorridorCount;
				TestTrue(TEXT("Native exclusion actor receives the configured tag"), Corridor->ActorHasTag(TEXT("GeneratedRoadExclusion")));
				TInlineComponentArray<UBoxComponent*> Boxes;
				Corridor->GetComponents(Boxes);
				TestTrue(TEXT("Native PCG corridor contains spatial boxes"), !Boxes.IsEmpty());
				TestEqual(TEXT("Corridor follows generated actor folders"), Corridor->GetFolderPath(), Road->GetFolderPath());
			}
		}
		TestTrue(TEXT("All source roads get actual exclusion corridors"), CorridorCount >= 4);
		ARoadActor* First = Roads[0];
		AFoliageExclusionActor* FirstCorridor = First->FoliageExclusionActor;
		Generator->FoliageExclusionSampleSpacing = 1000;
		Generator->FoliageExclusionTargetPCGTag = TEXT("PendingRoadExclusion");
		FPropertyChangedEvent Event(FindFProperty<FProperty>(Generator->GetClass(), TEXT("FoliageExclusionSampleSpacing")), EPropertyChangeType::ValueSet);
		Generator->PostEditChangeProperty(Event);
		TestEqual(TEXT("Editing pending settings does not change applied road settings"), First->FoliageExclusionSampleSpacing, 750.0);
		TestTrue(TEXT("Editing settings does not replace the native corridor"), First->FoliageExclusionActor == FirstCorridor);
		TestTrue(TEXT("Foliage parameters do not invalidate road generation"), Generator->BuildRoads(Ways, NAME_None));
		TestTrue(TEXT("Foliage parameters preserve generated scene identities"), Generator->GetRoadScenes() == Scenes);
		for (int32 Index = 0; Index < Roads.Num(); ++Index)
			TestTrue(TEXT("Foliage changes preserve road meshes"), CastChecked<UStaticMeshComponent>(Roads[Index]->GetRootComponent())->GetStaticMesh() == Meshes[Index]);
		Generator->ClearFoliageExclusion();
		for (ARoadActor* Road : Roads)
		{
			TestNull(TEXT("Clear removes only owned native exclusion actors"), Road->FoliageExclusionActor);
			TestFalse(TEXT("Clear disables automatic reapplication"), Road->bEnableFoliageExclusion);
			TestEqual(TEXT("Clear preserves the last applied road settings"), Road->FoliageExclusionSampleSpacing, 750.0);
		}
		TestTrue(TEXT("Other generator's corridor survives clear"), IsValid(OtherCorridor) && !OtherCorridor->IsActorBeingDestroyed() && OtherRoad->FoliageExclusionActor == OtherCorridor);
		Generator->CreateFoliageExclusion();
		for (ARoadActor* Road : Roads)
		{
			TestEqual(TEXT("Next Apply copies the pending settings"), Road->FoliageExclusionSampleSpacing, 1000.0);
			if (AFoliageExclusionActor* Corridor = Road->FoliageExclusionActor)
				TestTrue(TEXT("Next Apply updates the PCG exclusion tag"), Corridor->ActorHasTag(TEXT("PendingRoadExclusion")));
		}
		Generator->ClearFoliageExclusion();
		Generator->ClearFoliageExclusion();
	}
	Generator->ClearRoads();
	OtherGenerator->ClearRoads();
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}

#endif
