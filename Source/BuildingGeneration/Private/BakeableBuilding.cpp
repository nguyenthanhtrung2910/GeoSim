#include "BakeableBuilding.h"

#include "BuildingAssetBaker.h"
#include "BuildingGenerator.h"
#include "ContentStreaming.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"
#include "GeneratedModelStaticMeshComponent.h"
#include "GeneratedModelHISMComponent.h"
#include "RuleAttributes.h"
#include "RulePackage.h"
#include "VitruvioComponent.h"
#if WITH_EDITOR
#include "Editor.h"
#include "Engine/Selection.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "UObject/ObjectSaveContext.h"
#endif

ABakeableBuilding::ABakeableBuilding()
{
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
}

void UBuildingBakeEvents::OnAttributesReady() const { if (Owner.IsValid()) Owner->OnAttributesReady(); }
void UBuildingBakeEvents::OnGenerated() const { if (Owner.IsValid()) Owner->OnGenerated(); }

bool ABakeableBuilding::InitializeBuilding(URulePackage* Package, const TArray<FSplinePoint>& Points,
                                           const TMap<FString, double>& HeightOverrides, bool bHighDetail)
{
	if (!Package || Points.Num() < 3) return false;
	RulePackage = Package;
	Footprint = Points;
	InitialHeightOverrides = HeightOverrides;
	bInitialHighDetail = bHighDetail;
	Unbake();
	return IsValid(VitruvioComponent);
}

void ABakeableBuilding::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	RemoveLegacyPCGBoxes();
	RestoreFootprintSpline();
	// Recreate transient content according to the saved mode. Defer procedural
	// evaluation until the next tick, after loaded/duplicated inputs are ready.
	if (!IsValid(VitruvioComponent))
	{
		RestoreBakedDisplay();
		SetBakedVisible(true);
		if (Mode == EBuildingBakeMode::Editable && RulePackage && GetWorld() && !IsTemplate())
		{
			bEditableRestorePending = true;
			SetActorTickEnabled(true);
		}
	}
}

void ABakeableBuilding::ClearBakedDisplay()
{
	for (UStaticMeshComponent* Component : BakedComponents)
		if (IsValid(Component)) Component->DestroyComponent();
	BakedComponents.Reset();
}

void ABakeableBuilding::RestoreBakedDisplay()
{
	if (!BakedComponents.IsEmpty() || !GetWorld()) return;
	for (const FBuildingBakedPart& Part : BakedParts)
	{
		UStaticMesh* Mesh = Part.Mesh.LoadSynchronous();
		if (!Mesh) continue;
		UStaticMeshComponent* Component = Part.bInstanced
			? NewObject<UHierarchicalInstancedStaticMeshComponent>(this, NAME_None, RF_Transient | RF_DuplicateTransient)
			: NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient | RF_DuplicateTransient);
		Component->SetMobility(static_cast<EComponentMobility::Type>(Part.Mobility));
		Component->SetStaticMesh(Mesh);
		Component->SetupAttachment(RootComponent);
		Component->SetRelativeTransform(Part.Transform);
		Component->SetCollisionProfileName(Part.CollisionProfile);
		Component->SetCollisionEnabled(static_cast<ECollisionEnabled::Type>(Part.CollisionEnabled));
		Component->SetCastShadow(Part.bCastShadow);
		for (int32 Index = 0; Index < Part.Materials.Num(); ++Index)
			Component->SetMaterial(Index, Part.Materials[Index].LoadSynchronous());
		if (auto* Instances = Cast<UHierarchicalInstancedStaticMeshComponent>(Component))
		{
			Instances->NumCustomDataFloats = Part.NumCustomDataFloats;
			for (const FTransform& Transform : Part.Instances) Instances->AddInstance(Transform);
			Instances->PerInstanceSMCustomData = Part.CustomData;
		}
		AddOwnedComponent(Component);
		Component->RegisterComponent();
		BakedComponents.Add(Component);
	}
}

void ABakeableBuilding::SetBakedVisible(bool bVisible)
{
	for (UStaticMeshComponent* Component : BakedComponents)
	{
		if (!IsValid(Component)) continue;
		Component->SetVisibility(bVisible, true);
		Component->SetHiddenInGame(!bVisible);
		if (bVisible && Component->IsRegistered())
		{
			IStreamingManager::Get().NotifyPrimitiveUpdated(Component);
			// Warm up a newly baked/reloaded display without permanently pinning
			// every building's full-resolution textures in video memory.
			Component->PrestreamTextures(5.0f, false);
		}
	}
}

bool ABakeableBuilding::HasEditableModel() const
{
	if (!IsValid(VitruvioComponent) || !VitruvioComponent->HasGeneratedModel()) return false;
	const UGeneratedModelStaticMeshComponent* Model = VitruvioComponent->GetGeneratedModelComponent();
	if (Model && Model->GetStaticMesh()) return true;
	for (UGeneratedModelHISMComponent* Props : VitruvioComponent->GetGeneratedModelHISMComponents())
		if (Props && Props->GetStaticMesh() && Props->GetInstanceCount() > 0) return true;
	return false;
}

void ABakeableBuilding::Unbake()
{
#if WITH_EDITOR
	if (!GetWorld() || GetWorld()->IsGameWorld() || bBakePending) return;
	if (IsValid(VitruvioComponent))
	{
		RequestEditorRefresh();
		return;
	}
	Modify();
	// A manually placed building has no imported inputs yet. Give it a clockwise
	// 10 m footprint so the new component can be configured normally in Details.
	if (bSplineShape && Footprint.Num() < 3 && BakedParts.IsEmpty())
	{
		Footprint = {
			FSplinePoint(0, FVector(0, 0, 0), ESplinePointType::Linear),
			FSplinePoint(1, FVector(0, 1000, 0), ESplinePointType::Linear),
			FSplinePoint(2, FVector(1000, 1000, 0), ESplinePointType::Linear),
			FSplinePoint(3, FVector(1000, 0, 0), ESplinePointType::Linear)};
	}
	CaptureInputs();
	CreateAuthoring();
	[[maybe_unused]] const bool bMarkedDirty = MarkPackageDirty();
	RequestEditorRefresh();
#endif
}

void ABakeableBuilding::CreateAuthoring()
{
	if (!GetWorld() || IsValid(VitruvioComponent)) return;
	bEditableRestorePending = false;
	VitruvioComponent = NewObject<UVitruvioComponent>(this, NAME_None, RF_Transient | RF_DuplicateTransient | RF_TextExportTransient);
	AddOwnedComponent(VitruvioComponent);
	VitruvioComponent->GenerateAutomatically = false;
	VitruvioComponent->RegisterComponent();
	VitruvioComponent->bEnableNanite = bGeneratedNanite;
	VitruvioComponent->MaterialReplacement = MaterialReplacement;
	VitruvioComponent->InstanceReplacement = InstanceReplacement;
	VitruvioComponent->bEnableOcclusionQueries = bEnableOcclusion;
	VitruvioComponent->HideAfterGeneration = bHideFootprint;
	if (OpaqueParent) VitruvioComponent->OpaqueParent = OpaqueParent;
	if (MaskedParent) VitruvioComponent->MaskedParent = MaskedParent;
	if (TranslucentParent) VitruvioComponent->TranslucentParent = TranslucentParent;
	if (bSplineShape)
	{
		RestoreFootprintSpline();
		VitruvioComponent->InitialShape = NewObject<USplineInitialShape>(this, NAME_None, RF_Transient);
		VitruvioComponent->InitialShapeSceneComponent = FootprintSpline;
		VitruvioComponent->InitialShape->UpdatePolygon(VitruvioComponent);
	}
	else VitruvioComponent->SetPolygonInitialShape(Polygon, false, false);
	if (VitruvioComponent->InitialShapeSceneComponent)
		VitruvioComponent->InitialShapeSceneComponent->SetRelativeTransform(ShapeTransform);
	if (bHasSavedSeed) VitruvioComponent->SetRandomSeed(SavedSeed, false, false);
	Events = NewObject<UBuildingBakeEvents>(this, NAME_None, RF_Transient);
	Events->Owner = this;
	VitruvioComponent->OnAttributesEvaluated.AddDynamic(Events, &UBuildingBakeEvents::OnAttributesReady);
	VitruvioComponent->OnGenerateCompleted.AddDynamic(Events, &UBuildingBakeEvents::OnGenerated);
	VitruvioComponent->GenerateAutomatically = true;
	if (RulePackage) VitruvioComponent->SetRpk(RulePackage, true, false);
	if (BakedParts.IsEmpty()) Mode = EBuildingBakeMode::Editable;
}

void ABakeableBuilding::OnAttributesReady()
{
	if (!IsValid(VitruvioComponent)) return;
	VitruvioComponent->OnAttributesEvaluated.RemoveDynamic(Events, &UBuildingBakeEvents::OnAttributesReady);
	if (!BuildingGeneratorInternal::ApplyNumericRuleAttributes(VitruvioComponent, InitialHeightOverrides)) return;
	if (bInitialHighDetail)
	{
		if (!BuildingGeneratorInternal::ApplyHighDetailRuleAttribute(VitruvioComponent))
			UE_LOG(LogBuildingGenerator, Warning, TEXT("%s: rule package has no Level_of_Detail enum with a high option; keeping its default."), *GetName());
		bInitialHighDetail = false;
	}
	for (const FBuildingRuleValue& Value : RuleOverrides)
	{
		URuleAttribute* Attribute = VitruvioComponent->GetAttributes().FindRef(Value.Name);
		if (!Attribute) continue; // A changed package can remove an attribute.
		switch (Value.Type)
		{
		case EBuildingRuleValueType::Float:
			if (Cast<UFloatAttribute>(Attribute)) VitruvioComponent->SetFloatAttribute(Value.Name, Value.Number, false, false);
			break;
		case EBuildingRuleValueType::String:
			if (Cast<UStringAttribute>(Attribute)) VitruvioComponent->SetStringAttribute(Value.Name, Value.Text, false, false);
			break;
		case EBuildingRuleValueType::Bool:
			if (Cast<UBoolAttribute>(Attribute)) VitruvioComponent->SetBoolAttribute(Value.Name, Value.bValue, false, false);
			break;
		case EBuildingRuleValueType::FloatArray:
			if (Cast<UFloatArrayAttribute>(Attribute)) VitruvioComponent->SetFloatArrayAttribute(Value.Name, Value.Numbers, false, false);
			break;
		case EBuildingRuleValueType::StringArray:
			if (Cast<UStringArrayAttribute>(Attribute)) VitruvioComponent->SetStringArrayAttribute(Value.Name, Value.Strings, false, false);
			break;
		case EBuildingRuleValueType::BoolArray:
			if (Cast<UBoolArrayAttribute>(Attribute)) VitruvioComponent->SetBoolArrayAttribute(Value.Name, Value.Bools, false, false);
			break;
		}
	}
	VitruvioComponent->GenerateAutomatically = true;
	VitruvioComponent->EvaluateRuleAttributes(true);
}

void ABakeableBuilding::CaptureInputs()
{
	if (!IsValid(VitruvioComponent))
	{
		if (bSplineShape && IsValid(FootprintSpline))
		{
			Footprint.Reset();
			for (int32 Index = 0; Index < FootprintSpline->GetNumberOfSplinePoints(); ++Index)
				Footprint.Add(FootprintSpline->GetSplinePointAt(Index, ESplineCoordinateSpace::Local));
			ShapeTransform = FootprintSpline->GetRelativeTransform();
		}
		return;
	}
	RulePackage = VitruvioComponent->GetRpk();
	SavedSeed = VitruvioComponent->GetRandomSeed();
	bHasSavedSeed = true;
	MaterialReplacement = VitruvioComponent->MaterialReplacement;
	InstanceReplacement = VitruvioComponent->InstanceReplacement;
	bEnableOcclusion = VitruvioComponent->bEnableOcclusionQueries;
	bGeneratedNanite = VitruvioComponent->bEnableNanite;
	bHideFootprint = VitruvioComponent->HideAfterGeneration;
	OpaqueParent = VitruvioComponent->OpaqueParent;
	MaskedParent = VitruvioComponent->MaskedParent;
	TranslucentParent = VitruvioComponent->TranslucentParent;
	if (IsValid(VitruvioComponent->InitialShapeSceneComponent))
	{
		ShapeTransform = VitruvioComponent->InitialShapeSceneComponent->GetRelativeTransform();
		if (USplineComponent* Spline = Cast<USplineComponent>(VitruvioComponent->InitialShapeSceneComponent))
		{
			bSplineShape = true;
			RestoreFootprintSpline();
			Footprint.Reset();
			for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
				Footprint.Add(Spline->GetSplinePointAt(Index, ESplineCoordinateSpace::Local));
		}
		else if (VitruvioComponent->InitialShape)
		{
			bSplineShape = false;
			Polygon = VitruvioComponent->InitialShape->GetPolygon();
		}
	}
	if (!VitruvioComponent->GetAttributesReady()) return;
	RuleOverrides.Reset();
	InitialHeightOverrides.Reset();
	for (const auto& [Name, Attribute] : VitruvioComponent->GetAttributes())
	{
		if (!Attribute || !Attribute->bUserSet) continue;
		FBuildingRuleValue Value;
		Value.Name = Name;
		if (const auto* Number = Cast<UFloatAttribute>(Attribute))
		{
			Value.Type = EBuildingRuleValueType::Float;
			Value.Number = Number->Value;
		}
		else if (auto* Text = Cast<UStringAttribute>(Attribute))
		{
			Value.Type = EBuildingRuleValueType::String;
			Value.Text = Text->Value;
		}
		else if (const auto* Bool = Cast<UBoolAttribute>(Attribute))
		{
			Value.Type = EBuildingRuleValueType::Bool;
			Value.bValue = Bool->Value;
		}
		else if (const auto* Numbers = Cast<UFloatArrayAttribute>(Attribute))
		{
			Value.Type = EBuildingRuleValueType::FloatArray;
			Value.Numbers = Numbers->Values;
		}
		else if (const auto* Strings = Cast<UStringArrayAttribute>(Attribute))
		{
			Value.Type = EBuildingRuleValueType::StringArray;
			Value.Strings = Strings->Values;
		}
		else if (const auto* Bools = Cast<UBoolArrayAttribute>(Attribute))
		{
			Value.Type = EBuildingRuleValueType::BoolArray;
			Value.Bools = Bools->Values;
		}
		else continue;
		RuleOverrides.Add(MoveTemp(Value));
	}
}

void ABakeableBuilding::OnGenerated()
{
	if (!HasEditableModel())
	{
		UE_LOG(LogBuildingGenerator, Warning, TEXT("%s generated no building geometry; keeping the last baked display."), *GetName());
		return;
	}
	CaptureInputs();
	Mode = EBuildingBakeMode::Editable;
	// Asset paths remain saved, but an editing session does not retain an inactive
	// baked display and its hard references to the old meshes/materials.
	ClearBakedDisplay();
	if (GetWorld() && !GetWorld()->IsGameWorld())
	{
		[[maybe_unused]] const bool bMarkedDirty = MarkPackageDirty();
		RequestEditorRefresh();
	}
}

void ABakeableBuilding::Bake()
{
#if WITH_EDITOR
	if (!GetWorld() || GetWorld()->IsGameWorld() || bBakePending || !IsValid(VitruvioComponent)) return;
	if (!HasEditableModel() || !VitruvioComponent->GetAttributesReady() || VitruvioComponent->IsGenerating())
	{
		UE_LOG(LogBuildingGenerator, Warning, TEXT("Wait for editable generation to finish before baking %s."), *GetActorLabel());
		return;
	}
	if (!VitruvioComponent->HasValidInputData() || VitruvioComponent->IsBatchGenerated())
	{
		UE_LOG(LogBuildingGenerator, Error, TEXT("Bake requires valid inputs and individual Vitruvio generation."));
		return;
	}
	bBakePending = true;
	// Save the current live result on the next tick, outside Details/delegate calls.
	SetActorTickEnabled(true);
#endif
}

void ABakeableBuilding::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bEditableRestorePending)
	{
		bEditableRestorePending = false;
		if (Mode == EBuildingBakeMode::Editable && RulePackage) CreateAuthoring();
	}
#if WITH_EDITOR
	if (bEditorRefreshPending)
	{
		bEditorRefreshPending = false;
		if (GUnrealEd) GUnrealEd->UpdateFloatingPropertyWindows(true);
	}
	if (bBakePending)
	{
		// An edit may have started another live update since Bake was clicked.
		if (HasEditableModel() && VitruvioComponent->GetAttributesReady() && !VitruvioComponent->IsGenerating())
		{
			const bool bWasGeneratingAutomatically = VitruvioComponent->GenerateAutomatically;
			VitruvioComponent->GenerateAutomatically = false;
			if (!FinishBake() && IsValid(VitruvioComponent))
				VitruvioComponent->GenerateAutomatically = bWasGeneratingAutomatically;
		}
		else
		{
			UE_LOG(LogBuildingGenerator, Warning, TEXT("Wait for editable generation to finish, then bake %s again."), *GetActorLabel());
		}
		bBakePending = false;
	}
#endif
	SetActorTickEnabled(bEditableRestorePending || bBakePending || bEditorRefreshPending);
}

void ABakeableBuilding::RequestEditorRefresh()
{
#if WITH_EDITOR
	// Refresh on the next editor tick, after a Details button/delegate returns.
	if (!GetWorld() || GetWorld()->IsGameWorld() || !IsSelected()) return;
	bEditorRefreshPending = true;
	SetActorTickEnabled(true);
#endif
}

bool ABakeableBuilding::FinishBake()
{
#if WITH_EDITOR
	if (!HasEditableModel()) return false;
	TArray<UStaticMeshComponent*> Inputs;
	for (UActorComponent* Component : GetComponents())
		if (Component->IsA<UGeneratedModelStaticMeshComponent>() || Component->IsA<UGeneratedModelHISMComponent>())
			Inputs.Add(CastChecked<UStaticMeshComponent>(Component));
	TArray<UStaticMeshComponent*> Outputs;
	FBuildingAssetBaker Baker;
	if (!Baker.Bake(this, Inputs, Outputs)) return false;
	Modify();
	CaptureInputs();
	ClearBakedDisplay();
	BakedParts.Reset();
	for (UStaticMeshComponent* Component : Outputs)
	{
		FBuildingBakedPart Part;
		Part.Mesh = Component->GetStaticMesh();
		Part.Transform = Component->GetRelativeTransform();
		Part.Mobility = Component->Mobility;
		Part.CollisionProfile = Component->GetCollisionProfileName();
		Part.CollisionEnabled = Component->GetCollisionEnabled();
		Part.bCastShadow = Component->CastShadow;
		for (UMaterialInterface* Material : Component->OverrideMaterials) Part.Materials.Add(Material);
		if (auto* Instances = Cast<UHierarchicalInstancedStaticMeshComponent>(Component))
		{
			Part.bInstanced = true;
			Part.NumCustomDataFloats = Instances->NumCustomDataFloats;
			Part.CustomData = Instances->PerInstanceSMCustomData;
			for (int32 Index = 0; Index < Instances->GetInstanceCount(); ++Index)
			{
				FTransform Transform;
				Instances->GetInstanceTransform(Index, Transform);
				Part.Instances.Add(Transform);
			}
		}
		BakedParts.Add(MoveTemp(Part));
		Component->SetFlags(RF_Transient | RF_DuplicateTransient);
		AddOwnedComponent(Component);
		Component->OnComponentCreated();
		Component->RegisterComponent();
		BakedComponents.Add(Component);
	}
	Mode = EBuildingBakeMode::Baked;
	SetBakedVisible(true);
	DestroyAuthoring();
	RestoreFootprintSpline();
	[[maybe_unused]] const bool bMarkedDirty = MarkPackageDirty();
	RequestEditorRefresh();
	return true;
#else
	return false;
#endif
}

void ABakeableBuilding::DestroyAuthoring()
{
	if (!IsValid(VitruvioComponent)) { VitruvioComponent = nullptr; return; }
#if WITH_EDITOR
	// Do not leave Details pointing at a component that is about to be destroyed.
	if (GEditor)
		for (UActorComponent* Component : GetComponents())
			if (GEditor->GetSelectedComponents()->IsSelected(Component))
				GEditor->GetSelectedComponents()->Deselect(Component);
#endif
	VitruvioComponent->OnAttributesEvaluated.RemoveAll(Events);
	VitruvioComponent->OnGenerateCompleted.RemoveAll(Events);
	VitruvioComponent->RemoveGeneratedMeshes();
	// Keep the actor's footprint; only the transient authoring component goes away.
	if (VitruvioComponent->InitialShapeSceneComponent == FootprintSpline)
		VitruvioComponent->InitialShapeSceneComponent = nullptr;
	VitruvioComponent->DestroyInitialShapeComponent();
	VitruvioComponent->DestroyComponent();
	VitruvioComponent = nullptr;
	Events = nullptr;
}

void ABakeableBuilding::Destroyed()
{
	DestroyAuthoring();
	Super::Destroyed();
}

#if WITH_EDITOR
void ABakeableBuilding::PreSave(FObjectPreSaveContext SaveContext)
{
	CaptureInputs();
	Super::PreSave(SaveContext);
}
#endif
