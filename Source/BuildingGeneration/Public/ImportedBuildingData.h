#pragma once

#include "CoreMinimal.h"
#include "ImportedBuildingData.generated.h"

/** Source data independent of PCG graphs and Vitruvio rule packages. */
USTRUCT(BlueprintType)
struct BUILDINGGENERATION_API FImportedBuilding
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Building")
	FString OsmId;

	/** Distinct ring vertices in world centimeters, without a duplicated closing point. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Building")
	TArray<FVector> Points;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Building")
	TMap<FString, FString> Tags;
};
