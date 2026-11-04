#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "LandscapeBoundsPCGVolume.h"
#include "Components/BrushComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "Misc/AutomationTest.h"
#include "PCGComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapePCGVolumeBoundsTest,
	"GeoSim.PCG.LandscapeVolumeBounds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLandscapePCGVolumeBoundsTest::RunTest(const FString& Parameters)
{
	const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Bounds test world"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
	ALandscape* Landscape = World->SpawnActor<ALandscape>();
	Landscape->SetActorLocation(FVector(5000, -2000, 300));
	Landscape->SetActorScale3D(FVector(100, 200, 100));
	TArray<uint16> Heights;
	Heights.Init(32768, 64 * 64);
	Landscape->Import(FGuid::NewGuid(), 0, 0, 63, 63, 1, 63,
		TMap<FGuid, TArray<uint16>> { { FGuid(), Heights } }, nullptr,
		TMap<FGuid, TArray<FLandscapeImportLayerInfo>> { { FGuid(), {} } }, ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
	ALandscapeBoundsPCGVolume* Volume = World->SpawnActor<ALandscapeBoundsPCGVolume>();
	TestNotNull(TEXT("Native PCG component is inherited"), Volume->PCGComponent.Get());
	Volume->TargetLandscape = Landscape;
	Volume->BoundsPadding = FVector(25, 50, 200);
	Volume->SetActorTransform(FTransform(FRotator(0, 35, 0), FVector(-1000, 500, 100), FVector(2, 3, 4)));
	Volume->MatchLandscapeBounds();
	const FBox Expected = Landscape->GetLandscapeInfo()->GetCompleteBounds().ExpandBy(Volume->BoundsPadding);
	UBrushComponent* BrushComponent = Volume->GetBrushComponent();
	const FBox Actual = BrushComponent->Bounds.GetBox();
	TestTrue(TEXT("Brush minimum matches landscape plus padding"), Actual.Min.Equals(Expected.Min, 0.1));
	TestTrue(TEXT("Brush maximum matches landscape plus padding"), Actual.Max.Equals(Expected.Max, 0.1));
	TestTrue(TEXT("Fit uses world axes regardless of the previous rotation"), Volume->GetActorQuat().Equals(FQuat::Identity));
	TestTrue(TEXT("Fit avoids retaining the previous scale"), Volume->GetActorScale3D().Equals(FVector::OneVector));
	TestTrue(TEXT("The rebuilt brush contains the landscape center"), Volume->EncompassesPoint(Expected.GetCenter()));
	TestFalse(TEXT("The rebuilt brush excludes distant points"), Volume->EncompassesPoint(Expected.Max + FVector(1000)));
	Volume->MatchLandscapeBounds();
	TestTrue(TEXT("Repeated matching does not accumulate scale or padding"), BrushComponent->Bounds.GetBox().Equals(Actual, 0.1));
	Volume->BoundsPadding = FVector::ZeroVector;
	Volume->MatchLandscapeBounds();
	TestTrue(TEXT("A flat landscape still produces a nonzero volume height"), BrushComponent->Bounds.BoxExtent.Z > 0.0);
	Volume->Destroy();
	Landscape->Destroy();
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}

#endif
