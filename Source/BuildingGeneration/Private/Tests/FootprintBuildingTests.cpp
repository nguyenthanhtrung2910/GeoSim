#if WITH_DEV_AUTOMATION_TESTS

#include "FootprintBuilding.h"

#include "Components/SplineComponent.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "PCGComponent.h"
#include "UObject/SoftObjectPath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFootprintBuildingRedirectTest,
	"GeoSim.BuildingGeneration.ClassRedirects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFootprintBuildingRedirectTest::RunTest(const FString& Parameters)
{
	FSoftObjectPath Path(TEXT("/Script/GeoSim.FootprintBuilding"));
	TestTrue(TEXT("Old footprint building class path is redirected"), Path.FixupCoreRedirects());
	TestEqual(TEXT("Footprint building resolves to BuildingGeneration"),
		Path.ToString(), AFootprintBuilding::StaticClass()->GetPathName());
	TestEqual(TEXT("Saved footprint building class can be loaded"),
		Path.TryLoad(), static_cast<UObject*>(AFootprintBuilding::StaticClass()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFootprintBuildingImportTest,
	"GeoSim.FootprintBuilding.ImportTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFootprintBuildingImportTest::RunTest(const FString& Parameters)
{
	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Test world"), World)) return false;

	AFootprintBuilding* Building = World->SpawnActor<AFootprintBuilding>();
	if (TestNotNull(TEXT("Building can be spawned by Spline Importer"), Building))
	{
		// Match the importer: locate an existing spline and replace its points in world space.
		USplineComponent* Spline = Building->GetComponentByClass<USplineComponent>();
		if (TestNotNull(TEXT("Importer can find the footprint spline"), Spline))
		{
			TestEqual(TEXT("Importer finds the building's footprint"), Spline, Building->FootprintSpline.Get());
			TestEqual(TEXT("No placeholder footprint before import"), Spline->GetNumberOfSplinePoints(), 0);
			const TArray<FVector> Points = {
				FVector(1000, 2000, 300), FVector(2000, 2000, 300),
				FVector(2000, 3000, 300), FVector(1000, 3000, 300)
			};
			Spline->SetSplinePoints(Points, ESplineCoordinateSpace::World, false);
			Spline->SetClosedLoop(true);
			TestEqual(TEXT("Imported footprint retains all corners"), Spline->GetNumberOfSplinePoints(), Points.Num());
			for (int32 Index = 0; Index < Points.Num(); ++Index)
			{
				TestTrue(TEXT("Imported corner retains its world location"),
					Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World).Equals(Points[Index]));
			}
		}

		if (TestNotNull(TEXT("PCG component"), Building->PCGComponent.Get()))
		{
			TestEqual(TEXT("PCG waits until import is complete and generation is requested"),
				Building->PCGComponent->GenerationTrigger, EPCGComponentGenerationTrigger::GenerateOnDemand);
			TestFalse(TEXT("Dropping the actor does not generate an incomplete building"),
				Building->PCGComponent->bGenerateOnDropWhenTriggerOnDemand);
			Building->GenerateBuilding();
			TestFalse(TEXT("A footprint without a graph does not schedule generation"),
				Building->PCGComponent->IsGenerating());
		}
	}

	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFootprintBuildingPreprocessTest,
	"GeoSim.FootprintBuilding.Preprocess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFootprintBuildingPreprocessTest::RunTest(const FString& Parameters)
{
	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Test world"), World)) return false;

	AFootprintBuilding* Building = World->SpawnActor<AFootprintBuilding>();
	if (TestNotNull(TEXT("Building"), Building))
	{
		// A tilted, scaled actor catches flattening in local space instead of world space.
		Building->SetActorTransform(FTransform(FRotator(15, 30, 10), FVector(400, 500, 600), FVector(2, 3, 1.5)));
		USplineComponent* Spline = Building->FootprintSpline;
		const TArray<FVector> Points = {
			FVector(1000, 2000, -400), FVector(2000, 2000, -100),
			FVector(2000, 3000, -250), FVector(1000, 3000, -300)
		};
		Spline->SetSplinePoints(Points, ESplineCoordinateSpace::World, false);
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			Spline->SetSplinePointType(Index, ESplinePointType::Linear, false);
		}
		Spline->SetTangentsAtSplinePoint(1, FVector(100, 0, 200), FVector(100, 0, -200), ESplineCoordinateSpace::World, false);
		Spline->SetClosedLoop(true);

		TestTrue(TEXT("Flattening is enabled by default"), Building->bFlattenFootprint);
		TestTrue(TEXT("Closed footprint can be preprocessed"), Building->PreprocessFootprint());
		TestTrue(TEXT("Footprint remains closed"), Spline->IsClosedLoop());
		TestEqual(TEXT("Corner count is unchanged"), Spline->GetNumberOfSplinePoints(), Points.Num());
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			const FVector Expected(Points[Index].X, Points[Index].Y, -100);
			TestTrue(TEXT("World X/Y are preserved and Z equals the highest corner"),
				Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World).Equals(Expected, 0.001));
			TestEqual(TEXT("Point interpolation type is preserved"), Spline->GetSplinePointType(Index),
				Index == 1 ? ESplinePointType::CurveCustomTangent : ESplinePointType::Linear);
		}
		for (float Key = 0; Key < Points.Num(); Key += 0.25f)
		{
			TestTrue(TEXT("Spline segments stay in the horizontal plane"),
				FMath::IsNearlyEqual(Spline->GetLocationAtSplineInputKey(Key, ESplineCoordinateSpace::World).Z, -100.0, 0.001));
		}
		TestTrue(TEXT("Repeated preprocessing succeeds"), Building->PreprocessFootprint());
		TestTrue(TEXT("Repeated preprocessing keeps the base elevation"),
			Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World).Equals(FVector(1000, 2000, -100), 0.001));

		Spline->SetSplinePoints(Points, ESplineCoordinateSpace::World);
		Building->bFlattenFootprint = false;
		TestTrue(TEXT("Flattening can be disabled"), Building->PreprocessFootprint());
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			TestTrue(TEXT("Disabled flattening preserves imported heights"),
				Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World).Equals(Points[Index], 0.001));
		}

		Building->bFlattenFootprint = true;
		Spline->SetClosedLoop(false);
		TestFalse(TEXT("Open footprints are rejected"), Building->PreprocessFootprint());
		TestTrue(TEXT("Rejected footprint is unchanged"),
			Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World).Equals(Points[0], 0.001));
		Spline->ClearSplinePoints();
		Spline->SetClosedLoop(true);
		TestFalse(TEXT("Empty footprints are rejected"), Building->PreprocessFootprint());
	}

	World->DestroyWorld(false);
	return true;
}

#endif
