#include "LandscapeTreeScatter.h"

#include "Algo/BinarySearch.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedSkinnedMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "ImageCore.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#if WITH_EDITOR
#include "ScopedTransaction.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogLandscapeTreeScatter, Log, All);

namespace
{
	struct FLandscapeTreeSurface
	{
		ULandscapeHeightfieldCollisionComponent* Component;
		FBox Bounds;
	};

	void GatherTreeSurfaces(const ALandscapeProxy* Landscape, TArray<FLandscapeTreeSurface>& Surfaces, FBox& Bounds)
	{
		if (!IsValid(Landscape) || !Landscape->GetWorld())
		{
			return;
		}

		// Include loaded streaming proxies belonging to the selected landscape.
		const FGuid LandscapeGuid = Landscape->GetLandscapeGuid();
		for (TActorIterator<ALandscapeProxy> It(Landscape->GetWorld()); It; ++It)
		{
			if (It->GetLandscapeGuid() != LandscapeGuid)
			{
				continue;
			}

			TInlineComponentArray<ULandscapeHeightfieldCollisionComponent*> Components;
			It->GetComponents(Components);
			for (ULandscapeHeightfieldCollisionComponent* Component : Components)
			{
				if (!IsValid(Component) || !Component->IsRegistered() || !Component->IsPhysicsStateCreated() ||
					Component->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
				{
					continue;
				}

				const FBox ComponentBounds = Component->Bounds.GetBox();
				if (ComponentBounds.IsValid)
				{
					Surfaces.Add({Component, ComponentBounds});
					Bounds += ComponentBounds;
				}
			}
		}

#if WITH_EDITOR
		// Keep mask mapping stable even when only part of a World Partition landscape is loaded.
		if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
		{
			const FBox CompleteBounds = Info->GetCompleteBounds();
			if (CompleteBounds.IsValid)
			{
				Bounds = CompleteBounds;
			}
		}
#endif
	}
}

ALandscapeTreeScatter::ALandscapeTreeScatter()
{
	PrimaryActorTick.bCanEverTick = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));

	LandscapeBounds = CreateDefaultSubobject<UBoxComponent>(TEXT("LandscapeBounds"));
	LandscapeBounds->SetupAttachment(RootComponent);
	LandscapeBounds->SetAbsolute(true, true, true);
	LandscapeBounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	LandscapeBounds->SetGenerateOverlapEvents(false);
	LandscapeBounds->SetCanEverAffectNavigation(false);
	LandscapeBounds->SetHiddenInGame(true);
	LandscapeBounds->SetBoxExtent(FVector::ZeroVector);

	Instances = CreateDefaultSubobject<UInstancedSkinnedMeshComponent>(TEXT("Instances"));
	Instances->SetupAttachment(RootComponent);
	Instances->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Instances->SetGenerateOverlapEvents(false);
	Instances->SetCanEverAffectNavigation(false);
}

void ALandscapeTreeScatter::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	if (Instances->GetTransformProvider() != TransformProvider.Get())
	{
		Instances->SetTransformProvider(TransformProvider);
	}
	TArray<FLandscapeTreeSurface> Surfaces;
	FBox Bounds(ForceInit);
	GatherTreeSurfaces(TargetLandscape, Surfaces, Bounds);
	LandscapeBounds->SetWorldLocationAndRotation(Bounds.IsValid ? Bounds.GetCenter() : GetActorLocation(), FRotator::ZeroRotator);
	LandscapeBounds->SetBoxExtent(Bounds.IsValid ? Bounds.GetExtent() : FVector::ZeroVector);
}

void ALandscapeTreeScatter::Generate()
{
	if (!GetWorld() || !IsValid(TargetLandscape) || !IsValid(TreeMesh))
	{
		UE_LOG(LogLandscapeTreeScatter, Warning, TEXT("%s: assign Target Landscape and Tree Mesh before generating."), *GetName());
		return;
	}

	TArray<FLandscapeTreeSurface> Surfaces;
	FBox Bounds(ForceInit);
	GatherTreeSurfaces(TargetLandscape, Surfaces, Bounds);
	if (!Bounds.IsValid || Surfaces.IsEmpty() || Bounds.GetSize().X <= 0.0 || Bounds.GetSize().Y <= 0.0)
	{
		UE_LOG(LogLandscapeTreeScatter, Warning, TEXT("%s: no usable landscape collision. Load the landscape regions and enable their collision."), *GetName());
		return;
	}

	if (!IsValid(MaskTexture))
	{
		UE_LOG(LogLandscapeTreeScatter, Warning, TEXT("%s: assign a Mask Texture asset before generating."), *GetName());
		return;
	}

	FImage Mask;
	bool bReadMask = false;
#if WITH_EDITORONLY_DATA
	// Source mip zero preserves the imported mask without GPU compression or streaming.
	FImage SourceMask;
	if (MaskTexture->Source.IsValid() && MaskTexture->Source.GetNumBlocks() == 1 &&
		MaskTexture->Source.GetNumLayers() == 1 && MaskTexture->Source.GetMipImage(SourceMask, 0))
	{
		SourceMask.CopyTo(Mask, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		bReadMask = true;
	}
#endif
	if (!bReadMask)
	{
		const FSharedImageConstRef CPUMask = MaskTexture->GetCPUCopy();
		if (CPUMask.IsValid())
		{
			CPUMask->CopyTo(Mask, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			bReadMask = true;
		}
	}
	if (!bReadMask || Mask.SizeX <= 0 || Mask.SizeY <= 0 || Mask.NumSlices != 1)
	{
		UE_LOG(LogLandscapeTreeScatter, Error, TEXT("%s: could not read pixels from mask texture '%s'. Use a single-image texture asset; runtime generation requires Availability set to CPU."),
			*GetName(), *MaskTexture->GetPathName());
		return;
	}

	// Read pixels once per generation; alpha is ignored by the existing RGB threshold.
	const TArrayView64<const FColor> MaskPixels = static_cast<const FImage&>(Mask).AsBGRA8();
	const FVector BoundsSize = Bounds.GetSize();
	const float Threshold = FMath::Clamp(MaskThreshold, 0.0f, 1.0f);
	const auto IsMaskPixelIncluded = [&](int64 PixelIndex)
	{
		const FColor& Pixel = MaskPixels[PixelIndex];
		const float Brightness = (static_cast<float>(Pixel.R) + Pixel.G + Pixel.B) / (3.0f * 255.0f);
		const bool bWhite = Brightness >= Threshold;
		return bInvertMask ? !bWhite : bWhite;
	};

	// Sample white pixels through small cumulative blocks, without storing an index for every white pixel.
	constexpr int64 MaskBlockSize = 256;
	TArray<int64> IncludedPixelCounts;
	int64 IncludedPixelCount = 0;
	for (int64 PixelIndex = 0; PixelIndex < MaskPixels.Num(); ++PixelIndex)
	{
		if (IsMaskPixelIncluded(PixelIndex))
		{
			++IncludedPixelCount;
		}
		if ((PixelIndex + 1) % MaskBlockSize == 0 || PixelIndex + 1 == MaskPixels.Num())
		{
			IncludedPixelCounts.Add(IncludedPixelCount);
		}
	}

	const double IncludedFraction = static_cast<double>(IncludedPixelCount) / MaskPixels.Num();
	const double IncludedAreaSquareMeters = (BoundsSize.X / 100.0) * (BoundsSize.Y / 100.0) * IncludedFraction;
	const double RequestedCount = IncludedAreaSquareMeters * FMath::Max(0.0, TreeDensity);
	const int32 TreeLimit = FMath::Clamp(MaximumTrees, 0, 1000000);
	const int32 DesiredCount = FMath::RoundToInt(FMath::Min(RequestedCount, static_cast<double>(TreeLimit)));
	if (RequestedCount > TreeLimit)
	{
		UE_LOG(LogLandscapeTreeScatter, Warning, TEXT("%s: density requests %.0f trees; Maximum Trees limits generation to %d."),
			*GetName(), RequestedCount, TreeLimit);
	}

	FRandomStream Random(Seed);
	const int32 MaximumAttempts = DesiredCount * FMath::Clamp(AttemptsPerTree, 1, 100);
	const double Spacing = FMath::Max(0.0, static_cast<double>(MinimumSpacing));
	const double CellSize = FMath::Max(1.0, Spacing);
	const double SpacingSquared = FMath::Square(Spacing);
	const double MinimumNormalZ = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(MaximumSlopeDegrees, 0.0f, 90.0f)));
	const double MinimumScale = FMath::Max(0.01, FMath::Min(ScaleRange.X, ScaleRange.Y));
	const double MaximumScale = FMath::Max(MinimumScale, FMath::Max(ScaleRange.X, ScaleRange.Y));
	TMap<FIntPoint, TArray<FVector2D>> OccupiedCells;
	TArray<FTransform> Placements;
	Placements.Reserve(DesiredCount);
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(LandscapeTreeScatter), true);

	for (int32 Attempt = 0; Attempt < MaximumAttempts && Placements.Num() < DesiredCount; ++Attempt)
	{
		const int64 IncludedRank = FMath::Min(FMath::FloorToInt64(static_cast<double>(Random.GetFraction()) * IncludedPixelCount), IncludedPixelCount - 1);
		const int32 BlockIndex = Algo::UpperBound(IncludedPixelCounts, IncludedRank);
		int64 RankInBlock = IncludedRank - (BlockIndex > 0 ? IncludedPixelCounts[BlockIndex - 1] : 0);
		int64 PixelIndex = static_cast<int64>(BlockIndex) * MaskBlockSize;
		for (; PixelIndex < MaskPixels.Num(); ++PixelIndex)
		{
			if (IsMaskPixelIncluded(PixelIndex) && RankInBlock-- == 0)
			{
				break;
			}
		}
		const int32 PixelX = static_cast<int32>(PixelIndex % Mask.SizeX);
		const int32 PixelY = static_cast<int32>(PixelIndex / Mask.SizeX);
		double U = (PixelX + static_cast<double>(Random.GetFraction())) / Mask.SizeX;
		double V = (PixelY + static_cast<double>(Random.GetFraction())) / Mask.SizeY;
		if (bFlipMaskX) U = 1.0 - U;
		if (bFlipMaskY) V = 1.0 - V;
		const double X = Bounds.Min.X + U * BoundsSize.X;
		const double Y = Bounds.Min.Y + V * BoundsSize.Y;

		const FVector2D Position(X, Y);
		const FIntPoint Cell(FMath::FloorToInt((X - Bounds.Min.X) / CellSize), FMath::FloorToInt((Y - Bounds.Min.Y) / CellSize));
		bool bTooClose = false;
		if (Spacing > 0.0)
		{
			for (int32 DX = -1; DX <= 1 && !bTooClose; ++DX)
			{
				for (int32 DY = -1; DY <= 1 && !bTooClose; ++DY)
				{
					if (const TArray<FVector2D>* Neighbors = OccupiedCells.Find(Cell + FIntPoint(DX, DY)))
					{
						for (const FVector2D& Neighbor : *Neighbors)
						{
							if ((Position - Neighbor).SizeSquared() < SpacingSquared)
							{
								bTooClose = true;
								break;
							}
						}
					}
				}
			}
		}
		if (bTooClose)
		{
			continue;
		}

		FHitResult Hit;
		bool bFoundSurface = false;
		for (const FLandscapeTreeSurface& Surface : Surfaces)
		{
			if (X < Surface.Bounds.Min.X || X > Surface.Bounds.Max.X || Y < Surface.Bounds.Min.Y || Y > Surface.Bounds.Max.Y)
			{
				continue;
			}

			const FVector Start(X, Y, Surface.Bounds.Max.Z + 1000.0);
			const FVector End(X, Y, Surface.Bounds.Min.Z - 1000.0);
			// Trace only the selected landscape, so buildings and existing trees cannot intercept placement.
			if (FHitResult CandidateHit; Surface.Component->LineTraceComponent(CandidateHit, Start, End, QueryParams) &&
				(!bFoundSurface || CandidateHit.ImpactPoint.Z > Hit.ImpactPoint.Z))
			{
				Hit = CandidateHit;
				bFoundSurface = true;
			}
		}
		if (!bFoundSurface || Hit.ImpactNormal.Z < MinimumNormalZ)
		{
			continue;
		}

		const FQuat Alignment = bAlignToSurface
			? FQuat::FindBetweenNormals(FVector::UpVector, Hit.ImpactNormal.GetSafeNormal()) : FQuat::Identity;
		const FQuat Yaw(FVector::UpVector, Random.FRandRange(0.0f, 2.0f * PI));
		const double Scale = FMath::Lerp(MinimumScale, MaximumScale, Random.GetFraction());
		Placements.Emplace(Alignment * Yaw, Hit.ImpactPoint + Hit.ImpactNormal * SurfaceOffset, FVector(Scale));
		if (Spacing > 0.0)
		{
			OccupiedCells.FindOrAdd(Cell).Add(Position);
		}
	}

#if WITH_EDITOR
	const FScopedTransaction Transaction(NSLOCTEXT("LandscapeTreeScatter", "Generate", "Generate landscape trees"));
#endif
	Modify();
	Instances->Modify();
	Instances->ClearInstances();
	Instances->SetSkinnedAssetAndUpdate(TreeMesh);
	Instances->SetTransformProvider(TransformProvider);
	LandscapeBounds->Modify();
	LandscapeBounds->SetWorldLocationAndRotation(Bounds.GetCenter(), FRotator::ZeroRotator);
	LandscapeBounds->SetBoxExtent(Bounds.GetExtent());

	// Use the provider's first transform set, or the reference pose when no provider is assigned.
	TArray<int32> AnimationIndices;
	AnimationIndices.Init(0, Placements.Num());
	Instances->AddInstances(Placements, AnimationIndices, false, true);
	GeneratedTreeCount = Instances->GetInstanceCount();

	UE_LOG(LogLandscapeTreeScatter, Display, TEXT("%s: generated %d / %d skeletal trees at %.6f trees/m2 over %.2f m2 of included mask area. Mask: %dx%d. Placement uses loaded landscape collision."),
		*GetName(), GeneratedTreeCount, DesiredCount, TreeDensity, IncludedAreaSquareMeters, Mask.SizeX, Mask.SizeY);
}

void ALandscapeTreeScatter::Clear()
{
#if WITH_EDITOR
	const FScopedTransaction Transaction(NSLOCTEXT("LandscapeTreeScatter", "Clear", "Clear landscape trees"));
#endif
	Modify();
	Instances->Modify();
	Instances->ClearInstances();
	GeneratedTreeCount = 0;
}
