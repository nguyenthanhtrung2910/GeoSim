#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/SplineComponent.h"
#include "InitialShape.h"
#include "BakeableBuilding.generated.h"

class UVitruvioComponent;
class URulePackage;
class UStaticMeshComponent;
class UMaterialReplacementAsset;
class UInstanceReplacementAsset;
class UMaterial;
class UStaticMesh;
class UMaterialInterface;
class ABakeableBuilding;

/** UObject callbacks can run in editor worlds without actor script execution being enabled. */
UCLASS()
class UBuildingBakeEvents : public UObject
{
	GENERATED_BODY()
public:
	TWeakObjectPtr<ABakeableBuilding> Owner;
	UFUNCTION() void OnAttributesReady() const;
	UFUNCTION() void OnGenerated() const;
};

UENUM(BlueprintType)
enum class EBuildingBakeMode : uint8 { Editable, Baked };

UENUM()
enum class EBuildingRuleValueType : uint8 { Float, String, Bool, FloatArray, StringArray, BoolArray };

/** Asset references and instance placement survive reload; display components are recreated on demand. */
USTRUCT()
struct FBuildingBakedPart
{
	GENERATED_BODY()
	UPROPERTY() TSoftObjectPtr<UStaticMesh> Mesh;
	UPROPERTY() TArray<TSoftObjectPtr<UMaterialInterface>> Materials;
	UPROPERTY() FTransform Transform = FTransform::Identity;
	UPROPERTY() bool bInstanced = false;
	UPROPERTY() TArray<FTransform> Instances;
	UPROPERTY() int32 NumCustomDataFloats = 0;
	UPROPERTY() TArray<float> CustomData;
	UPROPERTY() uint8 Mobility = 0;
	UPROPERTY() FName CollisionProfile;
	UPROPERTY() uint8 CollisionEnabled = 0;
	UPROPERTY() bool bCastShadow = true;
};

/** Only explicit user overrides are saved; defaults continue to come from the rule package. */
USTRUCT()
struct FBuildingRuleValue
{
	GENERATED_BODY()
	UPROPERTY() FString Name;
	UPROPERTY() EBuildingRuleValueType Type = EBuildingRuleValueType::Float;
	UPROPERTY() double Number = 0;
	UPROPERTY() FString Text;
	UPROPERTY() bool bValue = false;
	UPROPERTY() TArray<double> Numbers;
	UPROPERTY() TArray<FString> Strings;
	UPROPERTY() TArray<bool> Bools;
};
// TODO: We need to handle aslo complicated generated geometry is use as simple 
/** Editable mode restores procedural content on load/PIE; Baked mode displays saved assets. */
UCLASS(NotBlueprintable)
class BUILDINGGENERATION_API ABakeableBuilding : public AActor
{
	GENERATED_BODY()
public:
	ABakeableBuilding();
	bool InitializeBuilding(URulePackage* Package, const TArray<FSplinePoint>& Points, const TMap<FString, double>& HeightOverrides, bool bHighDetail = false);
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Building")
	void Bake();
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Building", meta = (DisplayName = "Unbake / Edit"))
	void Unbake();
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Building")
	EBuildingBakeMode Mode = EBuildingBakeMode::Editable;
	/** Saved assets are created below this project Content path. Each bake uses a new folder. */
	UPROPERTY(EditAnywhere, Category = "Building")
	FString BakeDirectory = TEXT("/Game/Generated/Buildings");
	/** Exists only during an editing session. No procedural component is loaded from disk. */
	UPROPERTY(VisibleInstanceOnly, Transient, DuplicateTransient, Category = "Building")
	TObjectPtr<UVitruvioComponent> VitruvioComponent;
	UPROPERTY(VisibleInstanceOnly, Transient, DuplicateTransient, Category = "Building")
	TArray<TObjectPtr<UStaticMeshComponent>> BakedComponents;
	UPROPERTY() TArray<FBuildingBakedPart> BakedParts;
	UPROPERTY(VisibleInstanceOnly, Category = "Building|Inputs")
	TObjectPtr<URulePackage> RulePackage;
	UPROPERTY() TArray<FSplinePoint> Footprint;
	UPROPERTY() FInitialShapePolygon Polygon;
	UPROPERTY() bool bSplineShape = true;
	UPROPERTY() FTransform ShapeTransform = FTransform::Identity;
	UPROPERTY() int32 SavedSeed = 0;
	UPROPERTY() bool bHasSavedSeed = false;
	UPROPERTY() TArray<FBuildingRuleValue> RuleOverrides;
	UPROPERTY() TMap<FString, double> InitialHeightOverrides;
	UPROPERTY() TObjectPtr<UMaterialReplacementAsset> MaterialReplacement;
	UPROPERTY() TObjectPtr<UInstanceReplacementAsset> InstanceReplacement;
	UPROPERTY() bool bEnableOcclusion = false;
	UPROPERTY() bool bGeneratedNanite = true;
	UPROPERTY() bool bHideFootprint = false;
	UPROPERTY() TObjectPtr<UMaterial> OpaqueParent;
	UPROPERTY() TObjectPtr<UMaterial> MaskedParent;
	UPROPERTY() TObjectPtr<UMaterial> TranslucentParent;
	virtual void PostRegisterAllComponents() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void Destroyed() override;
#if WITH_EDITOR
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;
#endif
	bool HasEditableModel() const;
	/** Persistent footprint used by both Vitruvio authoring and PCG exclusion, including Baked mode. */
	UPROPERTY(VisibleInstanceOnly, Category = "Building")
	TObjectPtr<USplineComponent> FootprintSpline;
private:
	friend class UBuildingBakeEvents;
	UPROPERTY() bool bInitialHighDetail = false;
	UPROPERTY(Transient) TObjectPtr<UBuildingBakeEvents> Events;
	bool bBakePending = false;
	bool bEditorRefreshPending = false;
	bool bEditableRestorePending = false;
	void CreateAuthoring();
	void RestoreFootprintSpline();
	void RemoveLegacyPCGBoxes();
	void RequestEditorRefresh();
	void CaptureInputs();
	void DestroyAuthoring();
	void SetBakedVisible(bool bVisible);
	void ClearBakedDisplay();
	void RestoreBakedDisplay();
	bool FinishBake();
	void OnAttributesReady();
	void OnGenerated();
};
