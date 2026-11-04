#include "LandscapeBoundsPCGVolume.h"

#if WITH_EDITOR
#include "BSPOps.h"
#include "Builders/CubeBuilder.h"
#include "Components/BrushComponent.h"
#include "Engine/Polys.h"
#include "Engine/World.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "Model.h"
#include "PCGComponent.h"
#include "ScopedTransaction.h"

DEFINE_LOG_CATEGORY_STATIC(LogLandscapeBoundsPCGVolume, Log, All);

bool ALandscapeBoundsPCGVolume::ResolveLandscapeBounds(FBox& OutBounds) const
{
	if (!IsValid(TargetLandscape) || TargetLandscape->GetWorld() != GetWorld()) return false;
	if (const ULandscapeInfo* Info = TargetLandscape->GetLandscapeInfo())
		OutBounds = Info->GetCompleteBounds();
	else
		OutBounds = TargetLandscape->GetProxyBounds();
	return OutBounds.IsValid && !OutBounds.Min.ContainsNaN() && !OutBounds.Max.ContainsNaN() &&
		OutBounds.GetSize().X > 0.0 && OutBounds.GetSize().Y > 0.0;
}
#endif

void ALandscapeBoundsPCGVolume::MatchLandscapeBounds()
{
#if WITH_EDITOR
	if (!GetWorld() || GetWorld()->IsGameWorld()) return;
	FBox LandscapeBounds(ForceInit);
	if (!ResolveLandscapeBounds(LandscapeBounds))
	{
		UE_LOG(LogLandscapeBoundsPCGVolume, Warning, TEXT("%s: assign a landscape with valid bounds before matching."), *GetActorLabel());
		return;
	}
	if (BoundsPadding.ContainsNaN() || BoundsPadding.GetMin() < 0.0)
	{
		UE_LOG(LogLandscapeBoundsPCGVolume, Warning, TEXT("%s: bounds padding must be finite and nonnegative."), *GetActorLabel());
		return;
	}
	UBrushComponent* VolumeBrush = GetBrushComponent();
	if (!VolumeBrush) return;
	const FBox Bounds = LandscapeBounds.ExpandBy(BoundsPadding);
	FVector Size = Bounds.GetSize();
	Size.Z = FMath::Max(Size.Z, 1.0); // A flat landscape must still produce a solid box.

	const FScopedTransaction Transaction(NSLOCTEXT("LandscapeBoundsPCGVolume", "MatchBounds", "Match PCG volume to landscape bounds"));
	Modify();
	VolumeBrush->Modify();
	if (!Brush)
	{
		Brush = NewObject<UModel>(this, NAME_None, RF_Transactional);
		Brush->Initialize(nullptr, true);
	}
	Brush->Modify();
	if (Brush->Polys) Brush->Polys->Modify();
	VolumeBrush->Brush = Brush;
	UCubeBuilder* Builder = Cast<UCubeBuilder>(BrushBuilder);
	if (!Builder)
	{
		Builder = NewObject<UCubeBuilder>(this, NAME_None, RF_Transactional);
		BrushBuilder = Builder;
	}
	Builder->Modify();
	Builder->X = static_cast<float>(Size.X);
	Builder->Y = static_cast<float>(Size.Y);
	Builder->Z = static_cast<float>(Size.Z);
	Builder->Hollow = false;
	Builder->Tessellated = false;
	if (!Builder->Build(GetWorld(), this))
	{
		UE_LOG(LogLandscapeBoundsPCGVolume, Error, TEXT("%s: could not rebuild the PCG volume's box brush."), *GetActorLabel());
		return;
	}
	SetActorTransform(FTransform(FQuat::Identity, Bounds.GetCenter(), FVector::OneVector));
	// CubeBuilder creates polygons; prepare the solid BSP/hulls used by volume queries.
	FBSPOps::csgPrepMovingBrush(this);
	VolumeBrush->BuildSimpleBrushCollision();
	VolumeBrush->RecreatePhysicsState();
	VolumeBrush->UpdateBounds();
	VolumeBrush->MarkRenderStateDirty();
	if (PCGComponent)
	{
		PCGComponent->Modify();
		PCGComponent->DirtyGenerated(EPCGComponentDirtyFlag::All);
	}
	PostEditChange();
	PostEditMove(true);
	static_cast<void>(MarkPackageDirty());
#endif
}
