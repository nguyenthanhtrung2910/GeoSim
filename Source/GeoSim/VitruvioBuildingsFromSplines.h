#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LCCommon/LCGenerator.h"
#include "VitruvioBuildingsFromSplines.generated.h"

class ASplineCollection;
class AVitruvioActor;
class URulePackage;
class USplineComponent;

/** Creates one Vitruvio actor for each building footprint spline. */
UCLASS()
class GEOSIM_API AVitruvioBuildingsFromSplines : public AActor, public ILCGenerator
{
	GENERATED_BODY()

public:
	AVitruvioBuildingsFromSplines();

	/** When assigned, only splines in this collection are used. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Vitruvio Buildings|Source")
	TObjectPtr<ASplineCollection> SourceSplineCollection;

	/** Used when Source Spline Collection is unset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vitruvio Buildings|Source")
	FName SplinesTag;

	/** Restricts components within the selected collection or tagged actors. None accepts all. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vitruvio Buildings|Source")
	FName SplineComponentsTag;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vitruvio Buildings")
	TObjectPtr<URulePackage> RulePackage;

	/** Optional: place every footprint point at the highest boundary elevation instead of following terrain. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vitruvio Buildings")
	bool bFlattenFootprints = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vitruvio Buildings")
	bool bDeleteOldBuildingsWhenGenerating = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vitruvio Buildings")
	FName SpawnedActorsPath = TEXT("VitruvioBuildingsFromSplines");

	UPROPERTY(VisibleInstanceOnly, Category = "Vitruvio Buildings|Output")
	TArray<TSoftObjectPtr<AVitruvioActor>> GeneratedActors;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Vitruvio Buildings", meta = (DisplayName = "Generate Vitruvio Buildings"))
	void GenerateBuildingsEditor();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Vitruvio Buildings", meta = (DisplayName = "Clear Vitruvio Buildings"))
	void ClearBuildings();

	virtual bool OnGenerate(FName SpawnedActorsPathOverride, bool bIsUserInitiated) override;
	virtual TArray<UObject*> GetGeneratedObjects() const override;
	virtual bool Cleanup_Implementation(bool bSkipPrompt) override;

private:
	bool GenerateBuildings(FName SpawnedActorsPathOverride, bool bIsUserInitiated);
	bool GenerateBuilding(USplineComponent* SourceSpline, FName SpawnedActorsPathOverride);
};
