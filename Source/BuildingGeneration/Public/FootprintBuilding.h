#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FootprintBuilding.generated.h"

class UPCGComponent;
class USplineComponent;

/** Receives an imported building footprint and generates a building through PCG. */
UCLASS(Blueprintable)
class BUILDINGGENERATION_API AFootprintBuilding : public AActor
{
	GENERATED_BODY()

public:
	AFootprintBuilding();

	/** Filled by Spline Importer when this class is selected as its Custom Actor. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Footprint Building")
	TObjectPtr<USplineComponent> FootprintSpline;

	/** Assign a graph that reads this actor's spline and supplies the building data. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Footprint Building")
	TObjectPtr<UPCGComponent> PCGComponent;

	/** Flatten each footprint to its highest world-space corner before PCG generation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Footprint Building")
	bool bFlattenFootprint = true;

	/** Validate the imported footprint and prepare its points before PCG reads them. */
	UFUNCTION(BlueprintCallable, Category = "Footprint Building")
	bool PreprocessFootprint();

	/** Run after importing the footprint and assigning the PCG graph. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Footprint Building")
	void GenerateBuilding();

	/** Remove PCG output while keeping the imported footprint. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Footprint Building")
	void ClearBuilding();
};
