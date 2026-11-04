#pragma once

#include "CoreMinimal.h"

class ABakeableBuilding;
class UStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class UTexture;
class UPackage;

#if WITH_EDITOR
/** Editor conversion to independent Content assets. Never changes Vitruvio's transient resources. */
class FBuildingAssetBaker
{
public:
	bool Bake(ABakeableBuilding* Actor, const TArray<UStaticMeshComponent*>& Inputs, TArray<UStaticMeshComponent*>& Outputs);
private:
	FString Folder;
	bool bFailed = false;
	TMap<UStaticMesh*, UStaticMesh*> Meshes;
	TMap<UMaterialInterface*, UMaterialInterface*> Materials;
	TMap<UTexture*, UTexture*> Textures;
	TArray<UObject*> Assets;
	UPackage* CreateAssetPackage(const FString& Subfolder, const FString& Prefix, FString& Name);
	UStaticMesh* BakeMesh(UStaticMesh* Source);
	UMaterialInterface* BakeMaterial(UMaterialInterface* Source);
	UTexture* BakeTexture(UTexture* Source);
};
#endif
