#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LandscapeTreeScatter.generated.h"

class ALandscapeProxy;
class UBoxComponent;
class UInstancedSkinnedMeshComponent;
class USkeletalMesh;
class UTransformProviderData;
class UTexture2D;

/** Places skeletal trees on landscape collision, using a texture mask stretched across the landscape bounds. */
UCLASS()
class GEOSIM_API ALandscapeTreeScatter : public AActor
{
	GENERATED_BODY()

public:
	ALandscapeTreeScatter();
	virtual void OnConstruction(const FTransform& Transform) override;

	/** Select a landscape or one of its streaming proxies. Load its regions before generating. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Tree Scatter")
	TObjectPtr<ALandscapeProxy> TargetLandscape;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter")
	TObjectPtr<USkeletalMesh> TreeMesh;

	/** Transform provider for the instanced skinned mesh. Unset uses the mesh reference pose. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter")
	TObjectPtr<UTransformProviderData> TransformProvider;

	/** White includes trees, black excludes them. Runtime generation requires texture Availability set to CPU. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter|Mask")
	TObjectPtr<UTexture2D> MaskTexture = nullptr;

	/** Compare average RGB brightness to this threshold. Alpha is ignored. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter|Mask", meta = (ClampMin = "0", ClampMax = "1"))
	float MaskThreshold = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter|Mask")
	bool bInvertMask = false;

	/** Default: image left/right map to minimum/maximum world X. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter|Mask")
	bool bFlipMaskX = false;

	/** Default: image top/bottom map to minimum/maximum world Y. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter|Mask")
	bool bFlipMaskY = false;

	/** Trees per square meter of included mask area, measured in Unreal XY. 0.01 means one tree per 100 m2. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter|Placement", meta = (ClampMin = "0", DisplayName = "Density (trees per m2)"))
	double TreeDensity = 0.01;

	/** Upper limit on generated instances. Spacing, slope, and unloaded regions can also reduce the output. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter|Placement", meta = (ClampMin = "0", ClampMax = "1000000"))
	int32 MaximumTrees = 10000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter|Placement")
	int32 Seed = 12345;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter|Placement", meta = (ClampMin = "1", ClampMax = "100"))
	int32 AttemptsPerTree = 20;

	/** Minimum horizontal distance between tree origins, in centimeters. Zero disables it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter|Placement", meta = (ClampMin = "0", Units = "cm"))
	float MinimumSpacing = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter|Placement", meta = (ClampMin = "0", ClampMax = "90", Units = "deg"))
	float MaximumSlopeDegrees = 35.0f;

	/** Leave disabled for upright trees; enable to orient local Z along the surface normal. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter|Placement")
	bool bAlignToSurface = false;

	/** Offset along the surface normal, in centimeters. The tree mesh pivot should be at its base. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter|Placement", meta = (Units = "cm"))
	float SurfaceOffset = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tree Scatter|Placement", meta = (ClampMin = "0.01"))
	FVector2D ScaleRange = FVector2D(0.8, 1.2);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Tree Scatter|Output")
	TObjectPtr<UBoxComponent> LandscapeBounds;

	/** Trees share one skinned mesh component and its assigned transform provider. */
	UPROPERTY()
	TObjectPtr<UInstancedSkinnedMeshComponent> Instances;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Tree Scatter|Output")
	int32 GeneratedTreeCount = 0;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Tree Scatter")
	void Generate();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Tree Scatter")
	void Clear();
};
