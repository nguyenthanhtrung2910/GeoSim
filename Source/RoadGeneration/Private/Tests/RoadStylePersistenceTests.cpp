#include "OsmRoadScene.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "RoadGenerator.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoadStylePersistenceTest,
	"Landscapes.RoadConstructor.StylePersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadStylePersistenceTest::RunTest(const FString& Parameters)
{
	const FString Mount = TEXT("/RoadStylePersistence_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/");
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / Mount.Mid(1));
	IFileManager::Get().MakeDirectory(*Directory, true);
	FPackageName::RegisterMountPoint(Mount, Directory);
	const FString WorldName = Mount + TEXT("TestWorld");
	const FString SceneName = Mount + TEXT("Scene");
	const FString RoadName = Mount + TEXT("Road");
	const FString StyleName = TEXT("Way42Style");
	const UWorld::InitializationValues Init = UWorld::InitializationValues()
		.AllowAudioPlayback(false).CreateNavigation(false).CreateAISystem(false);
	UPackage* WorldPackage = CreatePackage(*WorldName);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("TestWorld"), WorldPackage, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Persistence test world"), World))
	{
		FPackageName::UnRegisterMountPoint(Mount, Directory);
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
	World->PersistentLevel->SetUseExternalActors(true);
	FActorSpawnParameters Params;
	Params.bCreateActorPackage = false;
	Params.ObjectFlags = RF_Public | RF_Transactional;
	AOsmRoadScene* Scene = World->SpawnActor<AOsmRoadScene>(Params);
	AOsmRoadActor* Road = World->SpawnActor<AOsmRoadActor>(Params);
	bool bSaved = false;
	FString RoadPath;
	TWeakObjectPtr<URoadStyle> OriginalStyle;
	TWeakObjectPtr<URoadStyle> OriginalTargetStyle;
	if (TestNotNull(TEXT("Scene actor"), Scene) && TestNotNull(TEXT("Road actor"), Road))
	{
		Scene->SetPackageExternal(true, false, CreatePackage(*SceneName));
		Road->SetPackageExternal(true, false, CreatePackage(*RoadName));
		Road->SetOwner(Scene);
		Scene->Roads.Add(Road);
		URoadStyle* Base = NewObject<URoadStyle>();
		Base->LeftLanes.SetNum(1);
		Base->RightLanes.SetNum(1);
		Base->LeftLanes[0].Width = 321;
		Base->RightLanes[0].Width = 345;
		URoadStyle* Style = RoadGeneratorInternal::CreateLaneCountStyle(Scene, Base, 3);
		Style->Rename(*StyleName, Scene);
		Scene->ImportedWayStyles.Add(42, Style);
		Road->LastAppliedRoadStyle = Style;
		URoadStyle* TargetStyle = DuplicateObject<URoadStyle>(Style, Scene);
		TargetStyle->Rename(TEXT("TransitionTargetStyle"), Scene);
		TargetStyle->ClearFlags(RF_Standalone);
		TargetStyle->SetFlags(RF_Public | RF_Transactional);
		TargetStyle->LeftLanes[0].Width = 123;
		Scene->ImportedTransitionStyles.Add(TargetStyle);
		Road->bEnableRoadTransitionPrototype = true;
		Road->RoadTransitionTargetStyle = TargetStyle;
		OriginalTargetStyle = TargetStyle;
		OriginalStyle = Style;
		RoadPath = Road->GetPathName();
		auto Save = [this](UPackage* Package, UObject* Asset, const FString& Extension)
		{
			FSavePackageArgs Args;
			Args.Error = GWarn;
			const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), Extension);
			return TestTrue(TEXT("Test package saves"), UPackage::SavePackage(Package, Asset, *Filename, Args));
		};
		bSaved = Save(WorldPackage, World, FPackageName::GetMapPackageExtension()) &&
			Save(Scene->GetPackage(), Scene, FPackageName::GetAssetPackageExtension()) &&
			Save(Road->GetPackage(), Road, FPackageName::GetAssetPackageExtension());
	}
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	CollectGarbage(RF_NoFlags);
	TestFalse(TEXT("Original style is gone before disk reload"), OriginalStyle.IsValid());
	TestFalse(TEXT("Original transition target is gone before disk reload"), OriginalTargetStyle.IsValid());

	if (bSaved)
	{
		// Load the road package first: its import must find the style exported by
		// the separate scene package, just as a World Partition actor does.
		UPackage* ReloadedRoadPackage = LoadPackage(nullptr, *RoadName, LOAD_None);
		TestNotNull(TEXT("External road package reloads"), ReloadedRoadPackage);
		AOsmRoadActor* ReloadedRoad = FindObject<AOsmRoadActor>(nullptr, *RoadPath);
		if (TestNotNull(TEXT("Saved road reloads"), ReloadedRoad))
		{
			URoadStyle* Style = ReloadedRoad->LastAppliedRoadStyle;
			if (TestNotNull(TEXT("Cross-package style import resolves after reload"), Style))
			{
				AOsmRoadScene* ReloadedScene = Cast<AOsmRoadScene>(Style->GetOuter());
				if (TestNotNull(TEXT("Style retains its scene owner"), ReloadedScene))
				{
					TestTrue(TEXT("Scene and road reload the same style"), ReloadedScene->ImportedWayStyles.FindRef(42) == Style);
					TestTrue(TEXT("Native transition enable survives reload"), ReloadedRoad->bEnableRoadTransitionPrototype);
					if (URoadStyle* Target = ReloadedRoad->RoadTransitionTargetStyle;
						TestNotNull(TEXT("Native transition target survives external-package reload"), Target))
					{
						TestTrue(TEXT("Scene owns the reloaded transition target"), ReloadedScene->ImportedTransitionStyles.Contains(Target));
						TestEqual(TEXT("Transition target lane widths survive reload"), Target->LeftLanes[0].Width, 123.0);
					}
					TestFalse(TEXT("Disk reload does not queue plugin geometric discovery"), ReloadedScene->bPendingDeferredPostLoadAutoConnectivityRefresh);
					TestFalse(TEXT("Disk reload does not queue a plugin scene rebuild"), ReloadedScene->bPendingDeferredSceneRebuild);
				}
				TestEqual(TEXT("Left driving count survives reload"), Style->LeftLanes.Num(), 2);
				TestEqual(TEXT("Right driving count survives reload"), Style->RightLanes.Num(), 1);
				TestEqual(TEXT("Lane width survives reload"), Style->LeftLanes[0].Width, 321.0);
			}
		}
		if (UWorld* ReloadedWorld = FindObject<UWorld>(nullptr, *(WorldName + TEXT(".TestWorld"))))
			ReloadedWorld->DestroyWorld(false);
		CollectGarbage(RF_NoFlags);
	}
	FPackageName::UnRegisterMountPoint(Mount, Directory);
	return true;
}
#endif
