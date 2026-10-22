#include "VitruvioBuildingsFromSplines.h"

#include "VitruvioActor.h"
#include "VitruvioComponent.h"
#include "RulePackage.h"

#include "SplineImporter/SplineCollection.h"
#include "LCCommon/LCBlueprintLibrary.h"
#include "ConcurrencyHelpers/Concurrency.h"
#include "ConcurrencyHelpers/LCReporter.h"
#include "OSMUserData/OSMUserData.h"

#include "Components/SplineComponent.h"
#include "Engine/World.h"
#include "Algo/Reverse.h"

DEFINE_LOG_CATEGORY_STATIC(LogVitruvioBuildingsFromSplines, Log, All);

AVitruvioBuildingsFromSplines::AVitruvioBuildingsFromSplines()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AVitruvioBuildingsFromSplines::GenerateBuildingsEditor()
{
	GenerateBuildings(NAME_None, true);
}

void AVitruvioBuildingsFromSplines::ClearBuildings()
{
	Execute_Cleanup(this, false);
}

bool AVitruvioBuildingsFromSplines::OnGenerate(FName SpawnedActorsPathOverride, bool bIsUserInitiated)
{
	return Concurrency::RunOnGameThreadAndWait([this, SpawnedActorsPathOverride, bIsUserInitiated]() {
		return GenerateBuildings(SpawnedActorsPathOverride, bIsUserInitiated);
	});
}

TArray<UObject*> AVitruvioBuildingsFromSplines::GetGeneratedObjects() const
{
	TArray<UObject*> Result;
	for (const TSoftObjectPtr<AVitruvioActor>& Actor : GeneratedActors)
	{
		if (Actor.IsValid()) Result.Add(Actor.Get());
	}
	return Result;
}

bool AVitruvioBuildingsFromSplines::Cleanup_Implementation(bool bSkipPrompt)
{
	if (!DeleteGeneratedObjects(bSkipPrompt)) return false;
	Modify();
	GeneratedActors.Empty();
	return true;
}

bool AVitruvioBuildingsFromSplines::GenerateBuildings(FName SpawnedActorsPathOverride, bool bIsUserInitiated)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogVitruvioBuildingsFromSplines, Error, TEXT("Building generation must run on the game thread."));
		return false;
	}

	if (!IsValid(RulePackage))
	{
		LCReporter::ShowError(FText::FromString(TEXT("Assign a Vitruvio Rule Package before generating buildings.")));
		return false;
	}

	TArray<USplineComponent*> Splines;
	if (IsValid(SourceSplineCollection))
	{
		for (USplineComponent* Spline : SourceSplineCollection->SplineComponents)
		{
			if (IsValid(Spline) && (SplineComponentsTag.IsNone() || Spline->ComponentHasTag(SplineComponentsTag)))
			{
				Splines.Add(Spline);
			}
		}
	}
	else if (!SplinesTag.IsNone())
	{
		for (USplineComponent* Spline : ULCBlueprintLibrary::FindSplineComponents(GetWorld(), bIsUserInitiated, SplinesTag, SplineComponentsTag))
		{
			Splines.Add(Spline);
		}
	}
	else
	{
		LCReporter::ShowError(FText::FromString(TEXT("Assign a Source Spline Collection or an actor Splines Tag.")));
		return false;
	}

	if (Splines.IsEmpty())
	{
		LCReporter::ShowError(FText::FromString(TEXT("No matching spline components were found.")));
		return false;
	}

	if (bDeleteOldBuildingsWhenGenerating && !Execute_Cleanup(this, !bIsUserInitiated)) return false;

	Modify();
	const int32 PreviousCount = GeneratedActors.Num();
	for (USplineComponent* Spline : Splines)
	{
		if (!GenerateBuilding(Spline, SpawnedActorsPathOverride)) return false;
	}
	if (GeneratedActors.Num() == PreviousCount)
	{
		LCReporter::ShowError(FText::FromString(TEXT("No closed footprint splines with at least three points were found.")));
		return false;
	}
	return true;
}

bool AVitruvioBuildingsFromSplines::GenerateBuilding(USplineComponent* SourceSpline, FName SpawnedActorsPathOverride)
{
	if (!IsValid(SourceSpline) || SourceSpline->GetNumberOfSplinePoints() < 3 || !SourceSpline->IsClosedLoop())
	{
		UE_LOG(LogVitruvioBuildingsFromSplines, Warning, TEXT("Skipping a spline that is not a closed footprint with at least three points."));
		return true;
	}

	const int32 NumPoints = SourceSpline->GetNumberOfSplinePoints();
	FVector Origin = SourceSpline->GetWorldLocationAtSplinePoint(0);
	if (bFlattenFootprints)
	{
		for (int32 Index = 1; Index < NumPoints; ++Index)
		{
			Origin.Z = FMath::Max(Origin.Z, SourceSpline->GetWorldLocationAtSplinePoint(Index).Z);
		}
	}

	TArray<FSplinePoint> SplinePoints;
	SplinePoints.Reserve(NumPoints);
	for (int32 Index = 0; Index < NumPoints; ++Index)
	{
		FSplinePoint Point = SourceSpline->GetSplinePointAt(Index, ESplineCoordinateSpace::World);
		Point.Position -= Origin;
		if (bFlattenFootprints)
		{
			Point.Position.Z = 0;
			Point.ArriveTangent.Z = 0;
			Point.LeaveTangent.Z = 0;
		}
		SplinePoints.Add(Point);
	}

	// Vitruvio does not correct winding when its initial shape comes from a spline.
	// Match its default spline footprint, whose XY signed area is negative.
	double TwiceSignedArea = 0;
	for (int32 Index = 0; Index < NumPoints; ++Index)
	{
		const FVector& A = SplinePoints[Index].Position;
		const FVector& B = SplinePoints[(Index + 1) % NumPoints].Position;
		TwiceSignedArea += A.X * B.Y - B.X * A.Y;
	}
	if (FMath::IsNearlyZero(TwiceSignedArea, 1.0))
	{
		UE_LOG(LogVitruvioBuildingsFromSplines, Warning, TEXT("Skipping a footprint with zero projected area."));
		return true;
	}
	if (TwiceSignedArea > 0)
	{
		Algo::Reverse(SplinePoints);
		for (FSplinePoint& Point : SplinePoints)
		{
			Swap(Point.ArriveTangent, Point.LeaveTangent);
			Point.ArriveTangent *= -1;
			Point.LeaveTangent *= -1;
		}
	}
	for (int32 Index = 0; Index < NumPoints; ++Index) SplinePoints[Index].InputKey = Index;

	AVitruvioActor* Building = GetWorld()->SpawnActor<AVitruvioActor>(Origin, FRotator::ZeroRotator);
	if (!IsValid(Building) || !IsValid(Building->VitruvioComponent))
	{
		if (IsValid(Building)) Building->Destroy();
		LCReporter::ShowError(FText::FromString(TEXT("Could not spawn a Vitruvio actor.")));
		return false;
	}

#if WITH_EDITOR
	ULCBlueprintLibrary::SetFolderPath2(Building, SpawnedActorsPathOverride, SpawnedActorsPath);
#endif

	if (UOSMUserData* SourceData = Cast<UOSMUserData>(SourceSpline->GetAssetUserDataOfClass(UOSMUserData::StaticClass())))
	{
		UOSMUserData* BuildingData = NewObject<UOSMUserData>(Building->GetRootComponent());
		BuildingData->Fields = SourceData->Fields;
		Building->GetRootComponent()->AddAssetUserData(BuildingData);
	}

	GeneratedActors.Add(Building);
	Building->VitruvioComponent->SetSplineInitialShape(SplinePoints, false, false);
	Building->VitruvioComponent->SetRpk(RulePackage, true, true);
	return true;
}
