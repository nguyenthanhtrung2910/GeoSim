#include "BakeableBuilding.h"

#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "VitruvioComponent.h"
#if WITH_EDITOR
#include "Editor.h"
#include "Engine/Selection.h"
#endif

void ABakeableBuilding::RestoreFootprintSpline()
{
	if (!bSplineShape || !GetRootComponent() || IsTemplate()) return;
	if (!IsValid(FootprintSpline) && IsValid(VitruvioComponent))
		FootprintSpline = Cast<USplineComponent>(VitruvioComponent->InitialShapeSceneComponent);
	if (!IsValid(FootprintSpline))
	{
		if (Footprint.Num() < 3) return;
		FootprintSpline = NewObject<USplineComponent>(this, NAME_None, RF_Transactional);
		FootprintSpline->SetupAttachment(GetRootComponent());
		FootprintSpline->SetRelativeTransform(ShapeTransform);
		FootprintSpline->ClearSplinePoints(false);
		FootprintSpline->AddPoints(Footprint, false);
		FootprintSpline->SetClosedLoop(true, true);
		FootprintSpline->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		FootprintSpline->SetCanEverAffectNavigation(false);
		FootprintSpline->OnComponentCreated();
	}
	FootprintSpline->ClearFlags(RF_Transient | RF_DuplicateTransient | RF_TextExportTransient);
	if (!GetInstanceComponents().Contains(FootprintSpline)) AddInstanceComponent(FootprintSpline);
	if (!FootprintSpline->IsRegistered()) FootprintSpline->RegisterComponent();
	if (Mode == EBuildingBakeMode::Baked)
	{
		FootprintSpline->SetVisibility(true);
		FootprintSpline->SetHiddenInGame(true);
	}
}

void ABakeableBuilding::RemoveLegacyPCGBoxes()
{
	bool bRemoved = false;
	TInlineComponentArray<UBoxComponent*> Boxes(this);
	for (UBoxComponent* Box : Boxes)
	{
		if (!Box->ComponentHasTag(TEXT("PCGBuildingExclusion"))) continue;
		Modify();
		Box->Modify();
#if WITH_EDITOR
		if (GEditor && GEditor->GetSelectedComponents()->IsSelected(Box)) GEditor->GetSelectedComponents()->Deselect(Box);
#endif
		RemoveInstanceComponent(Box);
		Box->DestroyComponent();
		bRemoved = true;
	}
	if (bRemoved)
	{
		MarkPackageDirty();
		RequestEditorRefresh();
	}
}

