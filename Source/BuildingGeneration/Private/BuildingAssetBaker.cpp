#include "BuildingAssetBaker.h"

#if WITH_EDITOR
#include "BakeableBuilding.h"
#include "BuildingGenerator.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

UPackage* FBuildingAssetBaker::CreateAssetPackage(const FString& Subfolder, const FString& Prefix, FString& Name)
{
	FString PackageName;
	FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateUniqueAssetName(
		Folder / Subfolder / Prefix, TEXT(""), PackageName, Name);
	return CreatePackage(*PackageName);
}

UTexture* FBuildingAssetBaker::BakeTexture(UTexture* Source)
{
	if (!Source || (!Source->HasAnyFlags(RF_Transient) && Source->GetOutermost() != GetTransientPackage())) return Source;
	if (UTexture** Existing = Textures.Find(Source)) return *Existing;
	UTexture2D* Texture = Cast<UTexture2D>(Source);
	const FTexturePlatformData* Data = Texture ? Texture->GetPlatformData() : nullptr;
	ETextureSourceFormat Format = TSF_Invalid;
	if (Data)
	{
		switch (Data->PixelFormat)
		{
		case PF_B8G8R8A8: Format = TSF_BGRA8; break;
		case PF_A16B16G16R16: Format = TSF_RGBA16; break;
		case PF_FloatRGBA: Format = TSF_RGBA16F; break;
		case PF_G8: Format = TSF_G8; break;
		default: break;
		}
	}
	if (!Data || Data->Mips.IsEmpty() || Format == TSF_Invalid) { bFailed = true; return nullptr; }
	const FTexture2DMipMap& Mip = Data->Mips[0];
	const int64 BytesPerPixel = Format == TSF_RGBA16 || Format == TSF_RGBA16F ? 8 : Format == TSF_G8 ? 1 : 4;
	if (Mip.SizeX <= 0 || Mip.SizeY <= 0 || Mip.BulkData.GetBulkDataSize() < int64(Mip.SizeX) * Mip.SizeY * BytesPerPixel)
	{
		bFailed = true;
		return nullptr;
	}
	const uint8* Pixels = static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
	if (!Pixels)
	{
		Mip.BulkData.Unlock();
		UE_LOG(LogBuildingGenerator, Error, TEXT("Texture %s has no readable CPU pixels for baking; editable output was kept."), *Source->GetName());
		bFailed = true;
		return nullptr;
	}
	FString Name;
	UPackage* Package = CreateAssetPackage(TEXT("Textures"), TEXT("T_Building"), Name);
	UTexture2D* Result = NewObject<UTexture2D>(Package, *Name, RF_Public | RF_Standalone);
	Result->CompressionSettings = Texture->CompressionSettings;
	Result->SRGB = Texture->SRGB;
	Result->AddressX = Texture->AddressX;
	Result->AddressY = Texture->AddressY;
	Result->Filter = Texture->Filter;
	Result->LODGroup = Texture->LODGroup;
	Result->bFlipGreenChannel = Texture->bFlipGreenChannel;
	Result->Source.Init(Mip.SizeX, Mip.SizeY, 1, 1, Format, Pixels);
	Mip.BulkData.Unlock();
	Result->PostEditChange();
	Textures.Add(Source, Result);
	Assets.Add(Result);
	return Result;
}

UMaterialInterface* FBuildingAssetBaker::BakeMaterial(UMaterialInterface* Source)
{
	if (!Source || (!Source->HasAnyFlags(RF_Transient) && Source->GetOutermost() != GetTransientPackage())) return Source;
	if (UMaterialInterface** Existing = Materials.Find(Source)) return *Existing;
	UMaterialInstance* Material = Cast<UMaterialInstance>(Source);
	if (!Material || !Material->Parent) { bFailed = true; return nullptr; }
	UMaterialInterface* Parent = BakeMaterial(Material->Parent);
	if (bFailed) return nullptr;
	FString Name;
	UPackage* Package = CreateAssetPackage(TEXT("Materials"), TEXT("MI_Building"), Name);
	UMaterialInstanceConstant* Result = NewObject<UMaterialInstanceConstant>(Package, *Name, RF_Public | RF_Standalone);
	Result->SetParentEditorOnly(Parent);
	Result->BasePropertyOverrides = Material->BasePropertyOverrides;
	TArray<FMaterialParameterInfo> Infos;
	TArray<FGuid> Ids;
	Material->GetAllScalarParameterInfo(Infos, Ids);
	for (const auto& Info : Infos)
	{
		float Value;
		if (Material->GetScalarParameterValue(Info, Value)) Result->SetScalarParameterValueEditorOnly(Info, Value);
	}
	Material->GetAllVectorParameterInfo(Infos, Ids);
	for (const auto& Info : Infos)
	{
		FLinearColor Value;
		if (Material->GetVectorParameterValue(Info, Value)) Result->SetVectorParameterValueEditorOnly(Info, Value);
	}
	Material->GetAllTextureParameterInfo(Infos, Ids);
	for (const auto& Info : Infos)
	{
		UTexture* Texture = nullptr;
		if (Material->GetTextureParameterValue(Info, Texture)) Result->SetTextureParameterValueEditorOnly(Info, BakeTexture(Texture));
	}
	if (bFailed) return nullptr;
	Result->PostEditChange();
	Materials.Add(Source, Result);
	Assets.Add(Result);
	return Result;
}

UStaticMesh* FBuildingAssetBaker::BakeMesh(UStaticMesh* Source)
{
	if (!Source) return nullptr;
	if (!Source->HasAnyFlags(RF_Transient) && Source->GetOutermost() != GetTransientPackage()) return Source;
	if (UStaticMesh** Existing = Meshes.Find(Source)) return *Existing;
	const FMeshDescription* Description = Source->GetMeshDescription(0);
	if (!Description) { bFailed = true; return nullptr; }
	TArray<FStaticMaterial> Slots = Source->GetStaticMaterials();
	for (FStaticMaterial& Slot : Slots)
	{
		Slot.MaterialInterface = BakeMaterial(Slot.MaterialInterface);
	}
	if (bFailed) return nullptr;
	FString Name;
	UPackage* Package = CreateAssetPackage(TEXT("Geometry"), TEXT("SM_Building"), Name);
	UStaticMesh* Result = NewObject<UStaticMesh>(Package, *Name, RF_Public | RF_Standalone);
	Result->GetStaticMaterials() = MoveTemp(Slots);
	Result->GetNaniteSettings().bEnabled = true;
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild = false;
	Params.bAllowCpuAccess = false;
	Params.PerLODOverrides = {{false, false}};
	Result->BuildFromMeshDescriptions({Description}, Params);
	Result->CreateBodySetup();
	Result->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
	Result->GetBodySetup()->CreatePhysicsMeshes();
	Meshes.Add(Source, Result);
	Assets.Add(Result);
	return Result;
}

bool FBuildingAssetBaker::Bake(ABakeableBuilding* Actor, const TArray<UStaticMeshComponent*>& Inputs, TArray<UStaticMeshComponent*>& Outputs)
{
	if (!Actor || !Actor->BakeDirectory.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Actor->BakeDirectory))
	{
		UE_LOG(LogBuildingGenerator, Error, TEXT("Bake Directory must be a valid project Content path below /Game/."));
		return false;
	}
	Folder = Actor->BakeDirectory / (TEXT("Bake_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	for (UStaticMeshComponent* Input : Inputs)
	{
		if (!IsValid(Input) || !Input->GetStaticMesh()) continue;
		UStaticMesh* Mesh = BakeMesh(Input->GetStaticMesh());
		if (!Mesh) { bFailed = true; break; }
		auto* Instances = Cast<UHierarchicalInstancedStaticMeshComponent>(Input);
		UStaticMeshComponent* Output = Instances
			? NewObject<UHierarchicalInstancedStaticMeshComponent>(Actor, NAME_None, RF_Transactional)
			: NewObject<UStaticMeshComponent>(Actor, NAME_None, RF_Transactional);
		Outputs.Add(Output);
		Output->SetVisibility(false);
		Output->SetMobility(Input->Mobility);
		Output->SetStaticMesh(Mesh);
		Output->SetupAttachment(Actor->GetRootComponent());
		Output->SetRelativeTransform(Input->GetComponentTransform().GetRelativeTransform(Actor->GetRootComponent()->GetComponentTransform()));
		Output->SetCollisionProfileName(Input->GetCollisionProfileName());
		Output->SetCollisionEnabled(Input->GetCollisionEnabled());
		Output->SetCastShadow(Input->CastShadow);
		for (int32 Index = 0; Index < Input->GetNumOverrideMaterials(); ++Index)
			Output->SetMaterial(Index, BakeMaterial(Input->OverrideMaterials[Index]));
		if (Instances)
		{
			auto* BakedInstances = CastChecked<UHierarchicalInstancedStaticMeshComponent>(Output);
			BakedInstances->NumCustomDataFloats = Instances->NumCustomDataFloats;
			for (int32 Index = 0; Index < Instances->GetInstanceCount(); ++Index)
			{
				FTransform Transform;
				Instances->GetInstanceTransform(Index, Transform);
				BakedInstances->AddInstance(Transform);
			}
			BakedInstances->PerInstanceSMCustomData = Instances->PerInstanceSMCustomData;
		}
		if (bFailed) break;
	}
	if (Outputs.IsEmpty()) bFailed = true;
	if (!bFailed)
	{
		for (UObject* Asset : Assets)
		{
			UPackage* Package = Asset->GetOutermost();
			Package->MarkPackageDirty();
			const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.Error = GWarn;
			if (!UPackage::SavePackage(Package, Asset, *File, Args)) { bFailed = true; break; }
			FAssetRegistryModule::AssetCreated(Asset);
		}
	}
	if (bFailed)
	{
		for (UStaticMeshComponent* Output : Outputs) Output->DestroyComponent();
		Outputs.Reset();
		UE_LOG(LogBuildingGenerator, Error, TEXT("Bake failed for %s; current display and inputs were kept."), *Actor->GetActorLabel());
		return false;
	}
	return true;
}
#endif
