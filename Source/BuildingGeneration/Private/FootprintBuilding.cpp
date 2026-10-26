#include "FootprintBuilding.h"

#include "Components/SplineComponent.h"
#include "PCGComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogFootprintBuilding, Log, All);

AFootprintBuilding::AFootprintBuilding()
{
	PrimaryActorTick.bCanEverTick = false;

	FootprintSpline = CreateDefaultSubobject<USplineComponent>(TEXT("FootprintSpline"));
	SetRootComponent(FootprintSpline);
	FootprintSpline->ClearSplinePoints(false);
	FootprintSpline->SetClosedLoop(true);

	PCGComponent = CreateDefaultSubobject<UPCGComponent>(TEXT("PCGComponent"));
	PCGComponent->GenerationTrigger = EPCGComponentGenerationTrigger::GenerateOnDemand;
	PCGComponent->bGenerateOnDropWhenTriggerOnDemand = false;
#if WITH_EDITORONLY_DATA
	PCGComponent->bRegenerateInEditor = false;
#endif
}

bool AFootprintBuilding::PreprocessFootprint()
{
	if (!FootprintSpline->IsClosedLoop() || FootprintSpline->GetNumberOfSplinePoints() < 3)
	{
		UE_LOG(LogFootprintBuilding, Warning,
			TEXT("%s: Import a closed footprint with at least three points before generating."), *GetName());
		return false;
	}

	if (bFlattenFootprint)
	{
		const int32 NumPoints = FootprintSpline->GetNumberOfSplinePoints();
		TArray<FVector> WorldPoints;
		WorldPoints.Reserve(NumPoints);
		double HighestZ = TNumericLimits<double>::Lowest();
		for (int32 Index = 0; Index < NumPoints; ++Index)
		{
			const FVector Point = FootprintSpline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World);
			WorldPoints.Add(Point);
			HighestZ = FMath::Max(HighestZ, Point.Z);
		}

		Modify();
		FootprintSpline->Modify();
		for (int32 Index = 0; Index < NumPoints; ++Index)
		{
			WorldPoints[Index].Z = HighestZ;
			FootprintSpline->SetLocationAtSplinePoint(Index, WorldPoints[Index], ESplineCoordinateSpace::World, false);
			// Imported custom tangents must also lie in the horizontal plane.
			if (FootprintSpline->GetSplinePointType(Index) == ESplinePointType::CurveCustomTangent)
			{
				FVector ArriveTangent = FootprintSpline->GetArriveTangentAtSplinePoint(Index, ESplineCoordinateSpace::World);
				FVector LeaveTangent = FootprintSpline->GetLeaveTangentAtSplinePoint(Index, ESplineCoordinateSpace::World);
				ArriveTangent.Z = 0;
				LeaveTangent.Z = 0;
				FootprintSpline->SetTangentsAtSplinePoint(Index, ArriveTangent, LeaveTangent, ESplineCoordinateSpace::World, false);
			}
		}
#if WITH_EDITORONLY_DATA
		FootprintSpline->bSplineHasBeenEdited = true;
#endif
	}

	FootprintSpline->UpdateSpline();
	return true;
}

void AFootprintBuilding::GenerateBuilding()
{
	if (!PCGComponent->GetGraph())
	{
		UE_LOG(LogFootprintBuilding, Warning,
			TEXT("%s: Assign a graph to the PCG component before generating."), *GetName());
		return;
	}

	if (!PreprocessFootprint()) return;

	PCGComponent->DirtyGenerated(EPCGComponentDirtyFlag::All);
	PCGComponent->Generate(true);
}

void AFootprintBuilding::ClearBuilding()
{
	PCGComponent->Cleanup(true);
}
