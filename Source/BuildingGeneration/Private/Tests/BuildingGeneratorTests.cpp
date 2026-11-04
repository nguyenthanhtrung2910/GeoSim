#if WITH_DEV_AUTOMATION_TESTS
#include "BuildingGenerator.h"
#include "Misc/AutomationTest.h"
#include "VitruvioComponent.h"
#include "RuleAttributes.h"
#include "VitruvioActor.h"
#include "Engine/World.h"
#include "RulePackage.h"
#include "HAL/PlatformTime.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildingLegacyRedirectTest, "GeoSim.BuildingGeneration.LegacyGeneratorRedirects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingLegacyRedirectTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Plugin Nanite generation is opt-in for ordinary Vitruvio components"), GetDefault<UVitruvioComponent>()->bEnableNanite);
	for (const TCHAR* OldName : { TEXT("/Script/GeoSim.VitruvioBuildingsFromSplines"), TEXT("/Script/Landscapes.VitruvioBuildingsFromSplines") })
	{
		FSoftObjectPath Path(OldName);
		TestTrue(TEXT("Legacy spline generator class is redirected"), Path.FixupCoreRedirects());
		TestEqual(TEXT("Saved actor references resolve to BuildingGenerator"), Path.TryLoad(), static_cast<UObject*>(ABuildingGenerator::StaticClass()));
	}
	TestNull(TEXT("PCG method selector has been removed"), FindFProperty<FProperty>(ABuildingGenerator::StaticClass(), TEXT("Method")));
	TestNull(TEXT("PCG graph option has been removed"), FindFProperty<FProperty>(ABuildingGenerator::StaticClass(), TEXT("PCGGraph")));
	FSoftObjectPath OldActor(TEXT("/Script/BuildingGeneration.PersistentBuildingActor"));
	TestTrue(TEXT("Removed persistent actor is redirected"), OldActor.FixupCoreRedirects());
	TestEqual(TEXT("Saved actor class resolves to standard Vitruvio"), OldActor.TryLoad(), static_cast<UObject*>(AVitruvioActor::StaticClass()));
	FSoftObjectPath OldComponent(TEXT("/Script/BuildingGeneration.PersistentVitruvioComponent"));
	TestTrue(TEXT("Removed persistent component is redirected"), OldComponent.FixupCoreRedirects());
	TestEqual(TEXT("Saved component class resolves to standard Vitruvio"), OldComponent.TryLoad(), static_cast<UObject*>(UVitruvioComponent::StaticClass()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildingHeightResolutionTest, "GeoSim.BuildingGeneration.HeightResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingHeightResolutionTest::RunTest(const FString& Parameters)
{
	FImportedBuilding Building;
	Building.OsmId = TEXT("1234");
	Building.Tags.Add(TEXT("building:levels"), TEXT("10"));
	FBuildingHeightSettings Settings;
	FResolvedBuildingHeight Height;
	TestTrue(TEXT("Floor height range resolves"), BuildingGeneratorInternal::ResolveHeight(Building, Settings, Height));
	TestTrue(TEXT("Chosen floor height is in range"), Height.FloorHeightMeters >= 3 && Height.FloorHeightMeters <= 3.5);
	TestTrue(TEXT("Level height is calculated like RoadForge"), FMath::IsNearlyEqual(Height.BuildingHeightMeters, 10 * Height.FloorHeightMeters));
	TestEqual(TEXT("Resolved floor count"), Height.FloorCount, 10);
	FResolvedBuildingHeight Repeated;
	BuildingGeneratorInternal::ResolveHeight(Building, Settings, Repeated);
	TestEqual(TEXT("Seed and OSM ID preserve the chosen floor height"), Repeated.FloorHeightMeters, Height.FloorHeightMeters);
	Building.Tags.Add(TEXT("height"), TEXT("27 m"));
	BuildingGeneratorInternal::ResolveHeight(Building, Settings, Height);
	TestEqual(TEXT("Explicit height takes priority over levels"), Height.BuildingHeightMeters, 27.0);
	Building.Tags.Add(TEXT("height"), TEXT("-1"));
	Building.Tags.Add(TEXT("building:levels"), TEXT("invalid"));
	BuildingGeneratorInternal::ResolveHeight(Building, Settings, Height);
	TestEqual(TEXT("Missing or invalid positive tags use the fallback"), Height.BuildingHeightMeters, 12.0);
	Settings.MinFloorHeightMeters = 4;
	TestFalse(TEXT("Reversed range is rejected"), BuildingGeneratorInternal::ResolveHeight(Building, Settings, Height));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildingRuleMappingTest, "GeoSim.BuildingGeneration.RuleAttributeMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingRuleMappingTest::RunTest(const FString& Parameters)
{
	// Supply evaluated attributes directly to exercise Vitruvio's actual typed
	// setters without requiring a specific external rule package or PRT runtime.
	const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Isolated attribute test world"), World)) return false;
	AVitruvioActor* Actor = World->SpawnActor<AVitruvioActor>();
	if (!Actor)
	{
		World->DestroyWorld(false);
		return false;
	}
	UVitruvioComponent* Component = Actor->VitruvioComponent;
	Component->GenerateAutomatically = false;
	const TArray<FSplinePoint> Shape = {
		FSplinePoint(0, FVector(0, 0, 0), ESplinePointType::Linear),
		FSplinePoint(1, FVector(0, 1000, 0), ESplinePointType::Linear),
		FSplinePoint(2, FVector(1000, 1000, 0), ESplinePointType::Linear),
		FSplinePoint(3, FVector(1000, 0, 0), ESplinePointType::Linear)
	};
	Component->SetSplineInitialShape(Shape, false, false);
	Component->Initialize();
	auto& Attributes = const_cast<TMap<FString, URuleAttribute*>&>(Component->GetAttributes());
	UFloatAttribute* Floor = NewObject<UFloatAttribute>(Component);
	UFloatAttribute* Height = NewObject<UFloatAttribute>(Component);
	Attributes.Add(TEXT("rules.floorHeight"), Floor);
	Attributes.Add(TEXT("rules.height"), Height);
	Floor->DisplayName = TEXT("Floor_Height");
	Height->DisplayName = TEXT("Eave_Height");
	TMap<FString, double> Overrides = {{TEXT("rules.floorHeight"), 3.2}, {TEXT("rules.height"), 32}};
	TestTrue(TEXT("Configured package attribute keys are applied"), BuildingGeneratorInternal::ApplyNumericRuleAttributes(Component, Overrides));
	TestEqual(TEXT("Floor height override"), Floor->Value, 3.2);
	TestEqual(TEXT("Building height override"), Height->Value, 32.0);
	TestTrue(TEXT("Vitruvio treats mapped values as user overrides"), Floor->bUserSet && Height->bUserSet);
	Overrides.Add(TEXT("unknown"), 99);
	AddExpectedError(TEXT("no numeric attribute"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("Unknown rule attribute does not silently generate with defaults"), BuildingGeneratorInternal::ApplyNumericRuleAttributes(Component, Overrides));
	Overrides = {{TEXT("Floor_Height"), 3.4}, {TEXT("Eave_Height"), 34}};
	TestTrue(TEXT("Unique displayed names resolve to full package keys"), BuildingGeneratorInternal::ApplyNumericRuleAttributes(Component, Overrides));
	TestEqual(TEXT("Short floor-height name updates the real attribute"), Floor->Value, 3.4);
	UFloatAttribute* Duplicate = NewObject<UFloatAttribute>(Component);
	Duplicate->DisplayName = TEXT("Floor_Height");
	Attributes.Add(TEXT("other.floorHeight"), Duplicate);
	AddExpectedError(TEXT("is ambiguous"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("Ambiguous short names are rejected"), BuildingGeneratorInternal::ApplyNumericRuleAttributes(Component, Overrides));
	Overrides = {{TEXT("rules.floorHeight"), 3.6}};
	TestTrue(TEXT("Full keys resolve ambiguity"), BuildingGeneratorInternal::ApplyNumericRuleAttributes(Component, Overrides));
	const UClass* EnumClass = FindObject<UClass>(nullptr, TEXT("/Script/Vitruvio.StringEnumAnnotation"));
	if (TestNotNull(TEXT("Vitruvio enum annotation is reflected"), EnumClass))
	{
		UStringAttribute* Detail = NewObject<UStringAttribute>(Component);
		Detail->DisplayName = TEXT("Level_of_Detail");
		Detail->Value = TEXT("low");
		Attributes.Add(TEXT("rules.detail"), Detail);
		TestFalse(TEXT("Plain strings are not treated as detail enums"), BuildingGeneratorInternal::ApplyHighDetailRuleAttribute(Component));
		auto* Annotation = static_cast<UStringEnumAnnotation*>(NewObject<UObject>(Detail, EnumClass));
		Annotation->Values = {TEXT("low"), TEXT("HIGH")};
		Detail->SetAnnotation(Annotation);
		TestTrue(TEXT("High detail resolves the displayed name and enum option"), BuildingGeneratorInternal::ApplyHighDetailRuleAttribute(Component));
		TestEqual(TEXT("Uses the exact enum spelling"), Detail->Value, FString(TEXT("HIGH")));
		TestTrue(TEXT("High detail becomes a saved user override"), Detail->bUserSet);
		Annotation->Values = {TEXT("low"), TEXT("medium")};
		Detail->Value = TEXT("low");
		Detail->bUserSet = false;
		TestFalse(TEXT("Unsupported detail enum keeps defaults"), BuildingGeneratorInternal::ApplyHighDetailRuleAttribute(Component));
		TestFalse(TEXT("Unsupported detail is not overridden"), Detail->bUserSet);
	}
	World->DestroyWorld(false);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildingEvaluatedRuleMappingTest, "GeoSim.BuildingGeneration.EvaluatedRuleMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingEvaluatedRuleMappingTest::RunTest(const FString& Parameters)
{
	for (const bool bTestDefaults : {false, true})
	{
		URulePackage* Package = LoadObject<URulePackage>(nullptr, bTestDefaults
			? TEXT("/Game/ErsiRulePackages/Paris_2014.Paris_2014")
			: TEXT("/Game/ErsiRulePackages/BuildingTextures.BuildingTextures"));
		if (!TestNotNull(TEXT("Real rule package"), Package)) return false;
		const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
		UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
		if (!World) return false;
		AVitruvioActor* Actor = World->SpawnActor<AVitruvioActor>();
		if (!Actor)
		{
			World->DestroyWorld(false);
			return false;
		}
		UVitruvioComponent* Component = Actor->VitruvioComponent;
		Component->GenerateAutomatically = false;
		const TArray<FSplinePoint> Shape = {
			FSplinePoint(0, FVector(0, 0, 0), ESplinePointType::Linear),
			FSplinePoint(1, FVector(0, 1000, 0), ESplinePointType::Linear),
			FSplinePoint(2, FVector(1000, 1000, 0), ESplinePointType::Linear),
			FSplinePoint(3, FVector(1000, 0, 0), ESplinePointType::Linear)
		};
		Component->SetSplineInitialShape(Shape, false, false);
		UBuildingRuleAttributeApplier* Applier = NewObject<UBuildingRuleAttributeApplier>(Actor);
		Actor->AddInstanceComponent(Applier);
		Applier->RegisterComponent();
		// Evaluate real CGA attributes and reevaluate overrides; omit mesh rendering under NullRHI.
		const TMap<FString, double> Values = bTestDefaults ? TMap<FString, double>()
			: TMap<FString, double>{{TEXT("Floor_Height"), 3.4}, {TEXT("Eave_Height"), 34}};
		Applier->Initialize(Component, Values, false);
		Component->SetRpk(Package, true, false);
		const double Deadline = FPlatformTime::Seconds() + 45;
		const TWeakObjectPtr<UBuildingRuleAttributeApplier> WeakApplier(Applier);
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, World, Component, WeakApplier, Values, Deadline]()
		{
			Component->TickComponent(0, LEVELTICK_All, nullptr);
			if ((!Component->GetAttributesReady() || (WeakApplier.IsValid() && WeakApplier->IsRegistered())) && FPlatformTime::Seconds() < Deadline)
				return false;
			TestTrue(TEXT("Attribute evaluation completed"), Component->GetAttributesReady());
			TestTrue(TEXT("Auto Generate is enabled only after successful override application"), Component->GenerateAutomatically);
			for (const auto& [Key, Attribute] : Component->GetAttributes())
			{
				if (Values.Contains(Key) || Values.Contains(Attribute->DisplayName)) continue;
				TestFalse(FString::Printf(TEXT("Unmapped parameter %s retains rule defaults"), *Key), Attribute->bUserSet);
			}
			for (const auto& [Name, Expected] : Values)
			{
				bool bFound = false;
				for (const auto& [Key, Attribute] : Component->GetAttributes())
				{
					if (Attribute->DisplayName != Name) continue;
					if (const UFloatAttribute* Numeric = Cast<UFloatAttribute>(Attribute))
					{
						TestEqual(FString::Printf(TEXT("%s survives rule reevaluation"), *Key), Numeric->Value, Expected);
						TestTrue(TEXT("Override remains user-set"), Numeric->bUserSet);
						bFound = true;
					}
				}
				TestTrue(FString::Printf(TEXT("Rule package exposes %s"), *Name), bFound);
			}
			World->DestroyWorld(false);
			return true;
		}));
	}
	return true;
}
#endif
