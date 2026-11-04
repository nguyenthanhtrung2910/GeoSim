#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "BakeableBuilding.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "PCGComponent.h"
#include "Data/PCGSplineData.h"
#include "VitruvioComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildingPCGExclusionTest, "GeoSim.BuildingGeneration.PCGExclusion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingPCGExclusionTest::RunTest(const FString& Parameters)
{
	const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	ABakeableBuilding* Editable = World->SpawnActor<ABakeableBuilding>();
	Editable->SetActorTransform(FTransform(FRotator(0, 40, 0), FVector(250, 500, 100), FVector(2, 3, 1.5)));
	Editable->Unbake();
	Editable->Tags.Add(TEXT("KeepMe"));
	Editable->Tags.Add(TEXT("BuildingExclusion"));
	ABakeableBuilding* Baked = World->SpawnActor<ABakeableBuilding>();
	Baked->Mode = EBuildingBakeMode::Baked;
	Baked->Footprint = Editable->Footprint;
	Baked->Tags.Add(TEXT("BuildingExclusion"));
	UBoxComponent* OldBox = NewObject<UBoxComponent>(Baked, NAME_None, RF_Transactional);
	OldBox->ComponentTags.Add(TEXT("PCGBuildingExclusion"));
	OldBox->SetupAttachment(Baked->GetRootComponent());
	Baked->AddInstanceComponent(OldBox);
	OldBox->RegisterComponent();
	TWeakObjectPtr<UBoxComponent> LegacyBox(OldBox);
	Baked->RegisterAllComponents();
	if (TestNotNull(TEXT("Editable building retains its footprint"), Editable->FootprintSpline.Get()) &&
		TestNotNull(TEXT("Baked building restores its saved footprint"), Baked->FootprintSpline.Get()))
	{
		USplineComponent* Spline = Editable->FootprintSpline;
		TestTrue(TEXT("Authoring and exclusion use the same spline"), Editable->VitruvioComponent->InitialShapeSceneComponent == Spline);
		TestTrue(TEXT("Spline is a saved actor-root child"), Spline->GetAttachParent() == Editable->GetRootComponent() && Editable->GetInstanceComponents().Contains(Spline) && !Spline->HasAnyFlags(RF_Transient));
		TestTrue(TEXT("Footprint remains closed"), Spline->IsClosedLoop());
		TestTrue(TEXT("Both modes preserve manually assigned actor tags"), Editable->ActorHasTag(TEXT("BuildingExclusion")) && Baked->ActorHasTag(TEXT("BuildingExclusion")));
		TestFalse(TEXT("Legacy exclusion box is removed"), LegacyBox.IsValid());
		Editable->RegisterAllComponents();
		TInlineComponentArray<USplineComponent*> Splines(Editable);
		TestEqual(TEXT("Registration keeps a single footprint"), Splines.Num(), 1);
		TestTrue(TEXT("Registration reuses the spline"), Spline == Editable->FootprintSpline);
		const FPCGDataCollection PCGData = UPCGComponent::CreateActorPCGDataCollection(Baked, nullptr, EPCGDataType::Spline, true);
		bool bFoundSpline = false;
		for (const FPCGTaggedData& Tagged : PCGData.TaggedData)
			bFoundSpline |= Cast<UPCGSplineData>(Tagged.Data) != nullptr;
		TestTrue(TEXT("PCG reads the baked footprint as spline data"), bFoundSpline);
		TestTrue(TEXT("Registration keeps unrelated tags"), Editable->ActorHasTag(TEXT("KeepMe")));
	}
	Editable->Destroy();
	Baked->Destroy();
	World->DestroyWorld(false);
	return true;
}
#endif
