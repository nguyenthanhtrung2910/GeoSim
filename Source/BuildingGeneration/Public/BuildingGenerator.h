#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "ImportedBuildingData.h"
#include "BuildingGenerator.generated.h"

class AOsmBuidingImporter;
class URulePackage;
class UVitruvioComponent;
DECLARE_LOG_CATEGORY_EXTERN(LogBuildingGenerator, Log, All);

USTRUCT(BlueprintType)
struct BUILDINGGENERATION_API FBuildingHeightSettings
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Height", meta = (ClampMin = "0.1", Units = "m"))
	double MinFloorHeightMeters = 3.0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Height", meta = (ClampMin = "0.1", Units = "m"))
	double MaxFloorHeightMeters = 3.5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Height", meta = (ClampMin = "0.1", Units = "m"))
	double DefaultBuildingHeightMeters = 12.0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Height")
	int32 Seed = 0;
};

struct FResolvedBuildingHeight
{
	double FloorHeightMeters = 0;
	double BuildingHeightMeters = 0;
	int32 FloorCount = 0;
};

namespace BuildingGeneratorInternal
{
	bool ResolveHeight(const FImportedBuilding& Building, const FBuildingHeightSettings& Settings, FResolvedBuildingHeight& OutHeight);
	bool ApplyNumericRuleAttributes(UVitruvioComponent* Component, const TMap<FString, double>& Overrides);
	bool ApplyHighDetailRuleAttribute(UVitruvioComponent* Component);
}

/** Lives on each output actor until rule attributes are ready. */
UCLASS()
class BUILDINGGENERATION_API UBuildingRuleAttributeApplier : public UActorComponent
{
	GENERATED_BODY()
public:
	void Initialize(UVitruvioComponent* Component, const TMap<FString, double>& Values, bool bGenerateModel = true);
private:
	UPROPERTY(Transient)
	TObjectPtr<UVitruvioComponent> Target;
	UPROPERTY(Transient)
	TMap<FString, double> Overrides;
	bool bGenerateAfterApply = true;
	UFUNCTION()
	void OnAttributesReady();
};

/** Generates editable buildings with Bake and Unbake support from the importer cache. */
UCLASS(Blueprintable)
class BUILDINGGENERATION_API ABuildingGenerator : public AActor
{
	GENERATED_BODY()
public:
	ABuildingGenerator();
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Buildings")
	TObjectPtr<AOsmBuidingImporter> Importer;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buildings")
	FBuildingHeightSettings Heights;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buildings")
	bool bFlattenFootprints = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buildings")
	bool bDeleteOldBuildingsWhenGenerating = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buildings")
	FName SpawnedActorsPath = TEXT("Buildings");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buildings|Vitruvio")
	TObjectPtr<URulePackage> RulePackage;
	/** Sets supported Level_of_Detail enum attributes to high on newly generated buildings. Off keeps package defaults. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buildings|Vitruvio")
	bool bHighDetail = false;
	/** Full numeric key or unique displayed name in the assigned package. Empty skips this override. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buildings|Vitruvio")
	FString FloorHeightAttribute;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buildings|Vitruvio")
	FString BuildingHeightAttribute;
	/** Optional numeric count of floors, derived from resolved building/floor heights. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buildings|Vitruvio")
	FString FloorCountAttribute;
	/** Most CGA rule packages use meters (1). Use 100 for attributes expecting centimeters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Buildings|Vitruvio", meta = (ClampMin = "0.0001"))
	double RuleUnitsPerMeter = 1.0;

	UPROPERTY(VisibleInstanceOnly, Category = "Buildings|Output")
	TArray<TSoftObjectPtr<AActor>> GeneratedActors;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Buildings")
	void GenerateBuildings();
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Buildings")
	void ClearBuildings();
private:
	bool GenerateBuilding(const FImportedBuilding& Source, const FResolvedBuildingHeight& Height);
};
