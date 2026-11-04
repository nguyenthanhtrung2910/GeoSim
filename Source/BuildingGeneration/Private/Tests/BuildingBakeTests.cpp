#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "BakeableBuilding.h"
#include "AssetCompilingManager.h"
#include "BuildingAssetBaker.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Engine/Selection.h"
#include "GeneratedModelStaticMeshComponent.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "RulePackage.h"
#include "RuleAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "VitruvioComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildingManualEditingTest, "GeoSim.BuildingGeneration.ManualEditing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingManualEditingTest::RunTest(const FString& Parameters)
{
	const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	ABakeableBuilding* Actor = World->SpawnActor<ABakeableBuilding>();
	TestNull(TEXT("Freshly placed actor has no authoring component"), Actor->VitruvioComponent.Get());
	Actor->Unbake();
	UVitruvioComponent* Source = Actor->VitruvioComponent;
	if (TestNotNull(TEXT("Unbake creates a component without imported inputs or a rule package"), Source))
	{
		TestTrue(TEXT("Manual authoring component is registered and transient"), Source->IsRegistered() && Source->HasAnyFlags(RF_Transient));
		TestTrue(TEXT("Assigning a rule package will automatically generate"), Source->GenerateAutomatically);
		const USplineComponent* Spline = Cast<USplineComponent>(Source->InitialShapeSceneComponent);
		if (TestNotNull(TEXT("Manual building receives an editable footprint"), Spline))
		{
			TestEqual(TEXT("Default footprint has four corners"), Spline->GetNumberOfSplinePoints(), 4);
			TestTrue(TEXT("Default footprint is closed"), Spline->IsClosedLoop());
		}
		Actor->Unbake();
		TestTrue(TEXT("Repeated Unbake keeps the current editing session"), Actor->VitruvioComponent == Source);
	}
	Actor->Destroy();
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildingBakeAssetsTest, "GeoSim.BuildingGeneration.BakeAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingBakeAssetsTest::RunTest(const FString& Parameters)
{
	const FString Mount = TEXT("/Game/__BakeAssets_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/");
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / Mount.Mid(6));
	IFileManager::Get().MakeDirectory(*Directory, true);
	FPackageName::RegisterMountPoint(Mount, Directory);
	const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	ABakeableBuilding* Actor = World->SpawnActor<ABakeableBuilding>();
	Actor->BakeDirectory = Mount + TEXT("Assets");
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, TEXT("/Vitruvio/Materials/M_OpaqueParent.M_OpaqueParent"));
	if (!Cube || !Parent) return false;
	UStaticMesh* Preview = DuplicateObject<UStaticMesh>(Cube, GetTransientPackage());
	Preview->ClearFlags(RF_Standalone);
	Preview->SetFlags(RF_Transient);
	UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Parent, GetTransientPackage());
	Material->SetFlags(RF_Transient);
	UTexture2D* Texture = UTexture2D::CreateTransient(1024, 1024, PF_B8G8R8A8);
	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	void* Pixels = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memset(Pixels, 255, Mip.BulkData.GetBulkDataSize());
	Mip.BulkData.Unlock();
	TArray<FMaterialParameterInfo> Infos;
	TArray<FGuid> Ids;
	Parent->GetAllTextureParameterInfo(Infos, Ids);
	if (Infos.IsEmpty()) return false;
	Material->SetTextureParameterValue(Infos[0].Name, Texture);
	Preview->SetMaterial(0, Material);
	UStaticMeshComponent* Surface = NewObject<UStaticMeshComponent>(Actor, NAME_None, RF_Transient);
	Surface->SetStaticMesh(Preview);
	Surface->SetupAttachment(Actor->GetRootComponent());
	Surface->RegisterComponent();
	auto* Props = NewObject<UHierarchicalInstancedStaticMeshComponent>(Actor, NAME_None, RF_Transient);
	Props->SetStaticMesh(Preview);
	Props->SetupAttachment(Actor->GetRootComponent());
	Props->NumCustomDataFloats = 1;
	Props->RegisterComponent();
	Props->AddInstance(FTransform(FVector(100, 200, 300)));
	Props->SetCustomDataValue(0, 0, 0.75f);
	TArray<UStaticMeshComponent*> Outputs;
	FBuildingAssetBaker Baker;
	TestTrue(TEXT("Bake saves source geometry and instanced props"), Baker.Bake(Actor, {Surface, Props}, Outputs));
	if (TestEqual(TEXT("Two baked display parts"), Outputs.Num(), 2))
	{
		UStaticMesh* Mesh = Outputs[0]->GetStaticMesh();
		TestTrue(TEXT("Bake creates an independent Content mesh asset"), Mesh != Preview && Mesh->GetOutermost()->GetName().StartsWith(Mount));
		TestTrue(TEXT("Repeated meshes are baked once per operation"), Outputs[1]->GetStaticMesh() == Mesh);
		TestTrue(TEXT("Baking enables Nanite"), Mesh->GetNaniteSettings().bEnabled);
		FAssetCompilingManager::Get().FinishAllCompilation();
		TestTrue(TEXT("Baked mesh contains built Nanite data"), Mesh->HasValidNaniteData());
		TestEqual(TEXT("Nanite baking preserves the collision profile"), Outputs[0]->GetCollisionProfileName(), Surface->GetCollisionProfileName());
		TestTrue(TEXT("Dynamic material is baked to a constant asset"), Mesh->GetMaterial(0)->IsA<UMaterialInstanceConstant>());
		UTexture* BakedTexture = nullptr;
		Mesh->GetMaterial(0)->GetTextureParameterValue(Infos[0], BakedTexture);
		TestTrue(TEXT("Texture source data is durable"), BakedTexture != Texture && Cast<UTexture2D>(BakedTexture)->Source.IsValid());
		if (UTexture2D* BakedTexture2D = Cast<UTexture2D>(BakedTexture))
		{
			TestEqual(TEXT("Full resolution is retained after texture compilation"), BakedTexture2D->GetSizeX(), 1024);
			TestTrue(TEXT("Baked textures retain a mip chain for normal distance streaming"), BakedTexture2D->GetNumMips() > 1);
			TestFalse(TEXT("Texture streaming remains enabled"), BakedTexture2D->NeverStream);
		}
		for (UObject* Asset : {static_cast<UObject*>(Mesh), static_cast<UObject*>(Mesh->GetMaterial(0)), static_cast<UObject*>(BakedTexture)})
			TestTrue(TEXT("Baked assets are written to disk"), IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension())));
		auto* BakedProps = CastChecked<UHierarchicalInstancedStaticMeshComponent>(Outputs[1]);
		TestEqual(TEXT("Prop count survives conversion"), BakedProps->GetInstanceCount(), 1);
		TestEqual(TEXT("Per-instance data survives conversion"), BakedProps->PerInstanceSMCustomData[0], 0.75f);
	}
	// Missing CPU pixels must fail safely instead of dereferencing a null bulk-data lock.
	Mip.BulkData.RemoveBulkData();
	FBuildingAssetBaker FailedBaker;
	TArray<UStaticMeshComponent*> FailedOutputs;
	AddExpectedError(TEXT("Bake failed"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("Unavailable texture data leaves the original editable geometry intact"), FailedBaker.Bake(Actor, {Surface}, FailedOutputs));
	TestTrue(TEXT("Failed bake creates no display replacement"), FailedOutputs.IsEmpty() && Surface->GetStaticMesh() == Preview);
	for (UStaticMeshComponent* Output : Outputs) Output->DestroyComponent();
	World->DestroyWorld(false);
	const double CleanupTime = FPlatformTime::Seconds() + 1;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Mount, Directory, CleanupTime]()
	{
		if (FPlatformTime::Seconds() < CleanupTime) return false;
		FPackageName::UnRegisterMountPoint(Mount, Directory);
		return true;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildingHighDetailTest, "GeoSim.BuildingGeneration.HighDetail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingHighDetailTest::RunTest(const FString& Parameters)
{
	const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	ABakeableBuilding* Actor = World->SpawnActor<ABakeableBuilding>();
	URulePackage* Rule = LoadObject<URulePackage>(nullptr, TEXT("/Game/ErsiRulePackages/Paris_2014.Paris_2014"));
	Actor->Unbake();
	const TArray<FSplinePoint> Footprint = Actor->Footprint;
	Actor->Destroy();
	Actor = World->SpawnActor<ABakeableBuilding>();
	if (!Actor->InitializeBuilding(Rule, Footprint, {}, true))
	{
		AddError(TEXT("Could not initialize high detail building"));
		World->DestroyWorld(false);
		return false;
	}
	const double Deadline = FPlatformTime::Seconds() + 60;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, World, Actor, Deadline]()
	{
		Actor->VitruvioComponent->TickComponent(0, LEVELTICK_All, nullptr);
		if (!Actor->HasEditableModel() && FPlatformTime::Seconds() < Deadline) return false;
		TestTrue(TEXT("High detail building generated"), Actor->HasEditableModel());
		bool bFound = false;
		for (const auto& [Key, Attribute] : Actor->VitruvioComponent->GetAttributes())
		{
			if (Attribute->DisplayName != TEXT("Level_of_Detail")) continue;
			const UStringAttribute* Detail = Cast<UStringAttribute>(Attribute);
			if (!Detail) continue;
			bFound = true;
			TestTrue(TEXT("Real rule enum remains high after evaluation"), Detail->Value.Equals(TEXT("high"), ESearchCase::IgnoreCase));
			TestTrue(TEXT("Detail override is saved for Bake and Unbake"), Actor->RuleOverrides.ContainsByPredicate([&Key](const FBuildingRuleValue& Value)
			{
				return Value.Name == Key && Value.Type == EBuildingRuleValueType::String && Value.Text.Equals(TEXT("high"), ESearchCase::IgnoreCase);
			}));
		}
		TestTrue(TEXT("Real rule exposes detail attribute"), bFound);
		Actor->Destroy();
		World->DestroyWorld(false);
		return true;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildingBakeRoundTripTest, "GeoSim.BuildingGeneration.BakeUnbakeReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBuildingBakeRoundTripTest::RunTest(const FString& Parameters)
{
	struct FState
	{
		UWorld* World = nullptr;
		ABakeableBuilding* Actor = nullptr;
		UWorld* PIEWorld = nullptr;
		ABakeableBuilding* PIEActor = nullptr;
		FString Mount, Directory, ActorPath, ActorPackage, FirstMeshPath;
		int32 Phase = 0;
		double Deadline = FPlatformTime::Seconds() + 120;
		double ReloadTime = 0;
		TWeakObjectPtr<UStaticMesh> EditingMesh;
		TWeakObjectPtr<USplineComponent> ExclusionSpline;
	};
	TSharedRef<FState> State = MakeShared<FState>();
	State->Mount = TEXT("/Game/__BakeRoundTrip_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/");
	State->Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / State->Mount.Mid(6));
	IFileManager::Get().MakeDirectory(*State->Directory, true);
	FPackageName::RegisterMountPoint(State->Mount, State->Directory);
	UPackage* Package = CreatePackage(*(State->Mount + TEXT("TestWorld")));
	const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
	State->World = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("TestWorld"), Package, true, ERHIFeatureLevel::Num, &Init);
	GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(State->World);
	State->World->PersistentLevel->SetUseExternalActors(true);
	FActorSpawnParameters Spawn;
	Spawn.bCreateActorPackage = false;
	Spawn.ObjectFlags = RF_Public | RF_Transactional;
	State->Actor = State->World->SpawnActor<ABakeableBuilding>(Spawn);
	State->Actor->BakeDirectory = State->Mount + TEXT("Assets");
	State->Actor->SavedSeed = 12345;
	State->Actor->bHasSavedSeed = true;
	TestNull(TEXT("No Rebake button"), State->Actor->FindFunction(TEXT("Rebake")));
	URulePackage* Rule = LoadObject<URulePackage>(nullptr, TEXT("/Game/ErsiRulePackages/BuildingTextures.BuildingTextures"));
	const TArray<FSplinePoint> Points = {
		FSplinePoint(0, FVector(0, 0, 0), ESplinePointType::Linear),
		FSplinePoint(1, FVector(0, 1000, 0), ESplinePointType::Linear),
		FSplinePoint(2, FVector(1000, 1000, 0), ESplinePointType::Linear),
		FSplinePoint(3, FVector(1000, 0, 0), ESplinePointType::Linear)};
	if (!TestTrue(TEXT("New buildings enter an editing session"), State->Actor->InitializeBuilding(Rule, Points, {{TEXT("Floor_Height"), 3.2}, {TEXT("Eave_Height"), 32}}))) return false;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]()
	{
		if (State->Actor && State->Actor->VitruvioComponent)
			State->Actor->VitruvioComponent->TickComponent(0, LEVELTICK_All, nullptr);
		if (State->Actor) State->Actor->Tick(0);
		if (State->PIEActor)
		{
			State->PIEActor->Tick(0);
			if (State->PIEActor->VitruvioComponent) State->PIEActor->VitruvioComponent->TickComponent(0, LEVELTICK_All, nullptr);
		}
		if (FPlatformTime::Seconds() > State->Deadline)
		{
			AddError(FString::Printf(TEXT("Bake workflow timed out in phase %d"), State->Phase));
			State->Actor->Destroy();
			if (State->PIEWorld)
			{
				State->PIEWorld->DestroyWorld(false);
				GEngine->DestroyWorldContext(State->PIEWorld);
			}
			State->World->DestroyWorld(false);
			GEngine->DestroyWorldContext(State->World);
			FPackageName::UnRegisterMountPoint(State->Mount, State->Directory);
			return true;
		}
		if (State->Phase == 0)
		{
			if (!State->Actor->HasEditableModel()) return false;
			TestTrue(TEXT("Editable changes generate automatically"), State->Actor->VitruvioComponent->GenerateAutomatically);
			TestTrue(TEXT("Editable building opts into plugin Nanite generation"), State->Actor->VitruvioComponent->bEnableNanite);
			TestTrue(TEXT("Editable building already contains Nanite data before Bake"), State->Actor->VitruvioComponent->GetGeneratedModelComponent()->GetStaticMesh()->HasValidNaniteData());
			const auto PIEInit = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
			// Exercise PIE duplication in a game world without announcing a partial
			// editor play session to unrelated editor subsystems.
			State->PIEWorld = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &PIEInit);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(State->PIEWorld);
			State->PIEActor = CastChecked<ABakeableBuilding>(StaticDuplicateObject(State->Actor, State->PIEWorld->PersistentLevel,
				TEXT("EditablePIE"), RF_AllFlags, nullptr, EDuplicateMode::PIE));
			State->PIEWorld->PersistentLevel->Actors.Add(State->PIEActor);
			State->PIEActor->RegisterAllComponents();
			State->Phase = 20;
			return false;
		}
		if (State->Phase == 20)
		{
			if (!State->PIEActor->HasEditableModel()) return false;
			TestTrue(TEXT("Editable PIE displays generated content"), State->PIEActor->Mode == EBuildingBakeMode::Editable);
			TestEqual(TEXT("PIE generation restores the current seed"), State->PIEActor->VitruvioComponent->GetRandomSeed(), 12345);
			TestTrue(TEXT("PIE has its own authoring component"), State->PIEActor->VitruvioComponent != State->Actor->VitruvioComponent);
			TestTrue(TEXT("PIE generation leaves the editor building editable"), State->Actor->HasEditableModel());
			State->PIEActor->Destroy();
			State->PIEActor = nullptr;
			GEditor->GetSelectedComponents()->Select(State->Actor->VitruvioComponent);
			State->Actor->Tags.AddUnique(TEXT("BuildingExclusion"));
			State->ExclusionSpline = State->Actor->FootprintSpline;
			State->Actor->Bake();
			TestFalse(TEXT("Bake does not request another procedural generation"), State->Actor->VitruvioComponent->IsGenerating());
			State->Phase = 1;
			return false;
		}
		if (State->Phase == 1)
		{
			if (State->Actor->Mode != EBuildingBakeMode::Baked) return false;
			TestNull(TEXT("Bake removes procedural component and preview"), State->Actor->VitruvioComponent.Get());
			TestTrue(TEXT("Bake keeps the same footprint spline and actor tag"), State->Actor->FootprintSpline == State->ExclusionSpline.Get() && State->Actor->ActorHasTag(TEXT("BuildingExclusion")));
			TestTrue(TEXT("Baked footprint retains its points"), State->Actor->FootprintSpline && State->Actor->FootprintSpline->GetNumberOfSplinePoints() == 4);
			for (FSelectionIterator It(*GEditor->GetSelectedComponents()); It; ++It)
				TestFalse(TEXT("Bake clears the deleted authoring selection"), Cast<UActorComponent>(*It)->GetOwner() == State->Actor);
			if (!TestTrue(TEXT("Bake records asset-backed display"), !State->Actor->BakedParts.IsEmpty())) return true;
			State->FirstMeshPath = State->Actor->BakedParts[0].Mesh.ToSoftObjectPath().ToString();
			TestEqual(TEXT("Seed preserved for Unbake"), State->Actor->SavedSeed, 12345);
			State->PIEActor = CastChecked<ABakeableBuilding>(StaticDuplicateObject(State->Actor, State->PIEWorld->PersistentLevel,
				TEXT("BakedPIE"), RF_AllFlags, nullptr, EDuplicateMode::PIE));
			State->PIEWorld->PersistentLevel->Actors.Add(State->PIEActor);
			State->PIEActor->RegisterAllComponents();
			State->PIEActor->Tick(0);
			TestNull(TEXT("Baked PIE never creates an authoring component"), State->PIEActor->VitruvioComponent.Get());
			TestTrue(TEXT("Baked PIE displays saved assets"), !State->PIEActor->BakedComponents.IsEmpty());
			State->PIEActor->Destroy();
			State->PIEActor = nullptr;
			State->PIEWorld->DestroyWorld(false);
			GEngine->DestroyWorldContext(State->PIEWorld);
			State->PIEWorld = nullptr;
			State->ActorPath = State->Actor->GetPathName();
			State->ActorPackage = State->Mount + TEXT("Actor");
			State->Actor->SetPackageExternal(true, false, CreatePackage(*State->ActorPackage));
			FSavePackageArgs Args;
			Args.Error = GWarn;
			TestTrue(TEXT("Baked world saves"), UPackage::SavePackage(State->World->GetOutermost(), State->World,
				*FPackageName::LongPackageNameToFilename(State->World->GetOutermost()->GetName(), FPackageName::GetMapPackageExtension()), Args));
			TestTrue(TEXT("Baked external actor saves"), UPackage::SavePackage(State->Actor->GetPackage(), State->Actor,
				*FPackageName::LongPackageNameToFilename(State->ActorPackage, FPackageName::GetAssetPackageExtension()), Args));
			State->ReloadTime = FPlatformTime::Seconds() + 1;
			State->Phase = 10;
			return false;
		}
		if (State->Phase == 10)
		{
			// Allow save validation and asset-build notifications to finish before
			// unloading the test world and collecting its objects.
			if (FPlatformTime::Seconds() < State->ReloadTime) return false;
			FAssetCompilingManager::Get().FinishAllCompilation();
			TWeakObjectPtr<ABakeableBuilding> Original(State->Actor);
			State->Actor->Destroy();
			State->World->DestroyWorld(false);
			GEngine->DestroyWorldContext(State->World);
			GEditor->ResetTransaction(FText::FromString(TEXT("Bake reload test")));
			CollectGarbage(RF_NoFlags);
			TestFalse(TEXT("Original actor collected before reload"), Original.IsValid());
			LoadPackage(nullptr, *State->ActorPackage, LOAD_None);
			State->Actor = FindObject<ABakeableBuilding>(nullptr, *State->ActorPath);
			if (!TestNotNull(TEXT("Baked actor loads from disk"), State->Actor)) return true;
			State->World = State->Actor->GetWorld();
			GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(State->World);
			State->Actor->RegisterAllComponents();
			TestTrue(TEXT("Baked mode survives reload"), State->Actor->Mode == EBuildingBakeMode::Baked);
			TestNotNull(TEXT("Footprint spline survives save and reload"), State->Actor->FootprintSpline.Get());
			TestTrue(TEXT("Exclusion actor tag survives save and reload"), State->Actor->ActorHasTag(TEXT("BuildingExclusion")));
			State->ExclusionSpline = State->Actor->FootprintSpline;
			TestNull(TEXT("Reload does not create a procedural component"), State->Actor->FindComponentByClass<UVitruvioComponent>());
			TestTrue(TEXT("Reload displays static assets"), !State->Actor->BakedComponents.IsEmpty());
			if (!State->Actor->BakedComponents.IsEmpty())
				TestTrue(TEXT("Nanite setting survives asset reload"), State->Actor->BakedComponents[0]->GetStaticMesh()->GetNaniteSettings().bEnabled);
			State->Actor->Unbake();
			TestTrue(TEXT("Last bake stays visible while Unbake generates"), !State->Actor->BakedComponents.IsEmpty());
			State->Phase = 2;
			return false;
		}
		if (State->Phase == 2)
		{
			if (!State->Actor->HasEditableModel()) return false;
			TestTrue(TEXT("Unbake returns to Editable mode"), State->Actor->Mode == EBuildingBakeMode::Editable);
			TestTrue(TEXT("Unbake reuses the same footprint spline and actor tag"), State->Actor->FootprintSpline == State->ExclusionSpline.Get() && State->Actor->ActorHasTag(TEXT("BuildingExclusion")));
			TestTrue(TEXT("Authoring uses the retained spline"), State->Actor->VitruvioComponent->InitialShapeSceneComponent == State->Actor->FootprintSpline);
			TestTrue(TEXT("Unbake restores Nanite generation"), State->Actor->VitruvioComponent->bEnableNanite);
			TestTrue(TEXT("Unbake builds Nanite data for the editable mesh"), State->Actor->VitruvioComponent->GetGeneratedModelComponent()->GetStaticMesh()->HasValidNaniteData());
			TestTrue(TEXT("Unbake releases inactive baked display components"), State->Actor->BakedComponents.IsEmpty());
			TestEqual(TEXT("Unbake restores the seed"), State->Actor->VitruvioComponent->GetRandomSeed(), 12345);
			for (const auto& [Name, Attribute] : State->Actor->VitruvioComponent->GetAttributes())
			{
				if (Attribute->DisplayName == TEXT("Eave_Height")) TestEqual(TEXT("Height override restored"), CastChecked<UFloatAttribute>(Attribute)->Value, 32.0);
				else if (Attribute->DisplayName == TEXT("Floor_Height")) TestEqual(TEXT("Floor height restored"), CastChecked<UFloatAttribute>(Attribute)->Value, 3.2);
				else TestFalse(TEXT("Unrelated rule parameters remain defaults"), Attribute->bUserSet);
			}
			State->EditingMesh = State->Actor->VitruvioComponent->GetGeneratedModelComponent()->GetStaticMesh();
			USplineComponent* Spline = CastChecked<USplineComponent>(State->Actor->VitruvioComponent->InitialShapeSceneComponent);
			Spline->SetLocationAtSplinePoint(1, FVector(0, 1400, 0), ESplineCoordinateSpace::Local);
			FPropertyChangedEvent Event(FindFProperty<FProperty>(USplineComponent::StaticClass(), TEXT("SplineCurves")), EPropertyChangeType::ValueSet);
			FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(Spline, Event);
			TestTrue(TEXT("Live footprint update is still pending"), State->Actor->VitruvioComponent->IsGenerating());
			State->Actor->Bake();
			State->Actor->Tick(0);
			TestTrue(TEXT("Bake during a live update keeps the building editable"), State->Actor->Mode == EBuildingBakeMode::Editable && State->Actor->VitruvioComponent != nullptr);
			State->Phase = 3;
			return false;
		}
		if (State->Phase == 3)
		{
			if (State->Actor->VitruvioComponent->GetGeneratedModelComponent()->GetStaticMesh() == State->EditingMesh.Get()) return false;
			TestEqual(TEXT("Footprint edits update saved inputs"), State->Actor->Footprint[1].Position.Y, 1400.0);
			TestTrue(TEXT("Footprint regeneration retains Nanite data"), State->Actor->VitruvioComponent->GetGeneratedModelComponent()->GetStaticMesh()->HasValidNaniteData());
			State->EditingMesh = State->Actor->VitruvioComponent->GetGeneratedModelComponent()->GetStaticMesh();
			State->Actor->VitruvioComponent->bEnableNanite = false;
			FPropertyChangedEvent Event(FindFProperty<FProperty>(UVitruvioComponent::StaticClass(), TEXT("bEnableNanite")), EPropertyChangeType::ValueSet);
			FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(State->Actor->VitruvioComponent, Event);
			State->Phase = 11;
			return false;
		}
		if (State->Phase == 11 || State->Phase == 12)
		{
			UStaticMesh* Mesh = State->Actor->VitruvioComponent->GetGeneratedModelComponent()->GetStaticMesh();
			if (Mesh == State->EditingMesh.Get()) return false;
			if (State->Phase == 11)
			{
				TestFalse(TEXT("Disabling Nanite returns to the fast preview path"), Mesh->GetNaniteSettings().bEnabled);
				TestFalse(TEXT("Disabling Nanite saves the editing preference"), State->Actor->bGeneratedNanite);
				State->EditingMesh = Mesh;
				State->Actor->VitruvioComponent->bEnableNanite = true;
				FPropertyChangedEvent Event(FindFProperty<FProperty>(UVitruvioComponent::StaticClass(), TEXT("bEnableNanite")), EPropertyChangeType::ValueSet);
				FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(State->Actor->VitruvioComponent, Event);
				State->Phase = 12;
				return false;
			}
			TestTrue(TEXT("Enabling Nanite rebuilds actual Nanite data"), Mesh->HasValidNaniteData());
			FSavePackageArgs Args;
			Args.Error = GWarn;
			TestTrue(TEXT("Editable external actor saves its inputs"), UPackage::SavePackage(State->Actor->GetPackage(), State->Actor,
				*FPackageName::LongPackageNameToFilename(State->ActorPackage, FPackageName::GetAssetPackageExtension()), Args));
			State->ReloadTime = FPlatformTime::Seconds() + 1;
			State->Phase = 30;
			return false;
		}
		if (State->Phase == 30)
		{
			if (FPlatformTime::Seconds() < State->ReloadTime) return false;
			FAssetCompilingManager::Get().FinishAllCompilation();
			State->Actor->Destroy();
			State->World->DestroyWorld(false);
			GEngine->DestroyWorldContext(State->World);
			GEditor->ResetTransaction(FText::FromString(TEXT("Editable building reload test")));
			CollectGarbage(RF_NoFlags);
			LoadPackage(nullptr, *State->ActorPackage, LOAD_None);
			State->Actor = FindObject<ABakeableBuilding>(nullptr, *State->ActorPath);
			if (!TestNotNull(TEXT("Editable actor loads from disk"), State->Actor)) return true;
			State->World = State->Actor->GetWorld();
			GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(State->World);
			State->Actor->RegisterAllComponents();
			TestTrue(TEXT("Editable mode survives reload"), State->Actor->Mode == EBuildingBakeMode::Editable);
			State->Phase = 31;
			return false;
		}
		if (State->Phase == 31)
		{
			if (!State->Actor->HasEditableModel()) return false;
			TestTrue(TEXT("Editable reload automatically restores generated content"), State->Actor->VitruvioComponent != nullptr);
			TestTrue(TEXT("Editable reload releases the old baked display"), State->Actor->BakedComponents.IsEmpty());
			TestEqual(TEXT("Editable reload restores the edited footprint"), State->Actor->Footprint[1].Position.Y, 1400.0);
			TestTrue(TEXT("Editable reload restores the Nanite preference"), State->Actor->VitruvioComponent->GetGeneratedModelComponent()->GetStaticMesh()->HasValidNaniteData());
			State->Actor->Bake();
			State->Phase = 4;
			return false;
		}
		if (State->Actor->Mode != EBuildingBakeMode::Baked) return false;
		TestTrue(TEXT("Bake after editing creates a new asset revision"), State->Actor->BakedParts[0].Mesh.ToSoftObjectPath().ToString() != State->FirstMeshPath);
		TestTrue(TEXT("Unbake never deletes the previous assets"), FSoftObjectPath(State->FirstMeshPath).TryLoad() != nullptr);
		State->Actor->Destroy();
		State->World->DestroyWorld(false);
		GEngine->DestroyWorldContext(State->World);
		FPackageName::UnRegisterMountPoint(State->Mount, State->Directory);
		return true;
	}));
	return true;
}
#endif
