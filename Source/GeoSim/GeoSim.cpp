// Copyright Epic Games, Inc. All Rights Reserved.

#include "GeoSim.h"
#include "Modules/ModuleManager.h"

#if WITH_EDITOR
#include "LandscapeTreeScatter.h"
#include "Components/InstancedSkinnedMeshComponent.h"
#include "DetailLayoutBuilder.h"
#include "IDetailCustomization.h"
#include "PropertyEditorModule.h"

/** Hide generated tree arrays without deriving from the Engine's MinimalAPI component. */
class FLandscapeTreeInstancesDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance()
	{
		return MakeShared<FLandscapeTreeInstancesDetails>();
	}

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override
	{
		TArray<TWeakObjectPtr<UObject>> Objects;
		DetailBuilder.GetObjectsBeingCustomized(Objects);
		for (const TWeakObjectPtr<UObject>& Object : Objects)
		{
			const UInstancedSkinnedMeshComponent* Component = Cast<UInstancedSkinnedMeshComponent>(Object.Get());
			if (Component && Cast<ALandscapeTreeScatter>(Component->GetOwner()))
			{
				DetailBuilder.HideCategory(TEXT("Instances"));
				return;
			}
		}
	}
};
#endif

class FGeoSimModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();
#if WITH_EDITOR
		FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		PropertyEditor.RegisterCustomClassLayout(TEXT("InstancedSkinnedMeshComponent"),
			FOnGetDetailCustomizationInstance::CreateStatic(&FLandscapeTreeInstancesDetails::MakeInstance));
		PropertyEditor.NotifyCustomizationModuleChanged();
#endif
	}

	virtual void ShutdownModule() override
	{
#if WITH_EDITOR
		if (FPropertyEditorModule* PropertyEditor = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
		{
			PropertyEditor->UnregisterCustomClassLayout(TEXT("InstancedSkinnedMeshComponent"));
		}
#endif
		FDefaultGameModuleImpl::ShutdownModule();
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FGeoSimModule, GeoSim, "GeoSim");
