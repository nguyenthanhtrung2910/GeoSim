#if WITH_DEV_AUTOMATION_TESTS

#include "ArcGISMiniMapHUD.h"
#include "ArcGISMiniMapWidget.h"
#include "FrameCaptureManager.h"
#include "LandscapeTreeScatter.h"
#include "ThirdPersonCharacter.h"
#include "VitruvioBuildingsFromSplines.h"
#include "Misc/AutomationTest.h"
#include "UObject/SoftObjectPath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeoSimModuleRedirectTest,
    "GeoSim.ModuleRedirects.SavedTypes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeoSimModuleRedirectTest::RunTest(const FString& Parameters)
{
    auto CheckSavedType = [this](const TCHAR* OldPath, UObject* ExpectedType)
    {
        FSoftObjectPath Path(OldPath);
        TestTrue(FString::Printf(TEXT("Redirect registered for %s"), OldPath), Path.FixupCoreRedirects());
        TestEqual(TEXT("Saved type resolves to its current path"), Path.ToString(), ExpectedType->GetPathName());
        TestEqual(TEXT("Saved type can still be loaded"), Path.TryLoad(), ExpectedType);
    };

    CheckSavedType(TEXT("/Script/Landscapes.ThirdPersonCharacter"), AThirdPersonCharacter::StaticClass());
    CheckSavedType(TEXT("/Script/Landscapes.FrameCaptureManager"), AFrameCaptureManager::StaticClass());
    CheckSavedType(TEXT("/Script/Landscapes.LandscapeTreeScatter"), ALandscapeTreeScatter::StaticClass());
    CheckSavedType(TEXT("/Script/Landscapes.VitruvioBuildingsFromSplines"), AVitruvioBuildingsFromSplines::StaticClass());
    CheckSavedType(TEXT("/Script/Landscapes.ArcGISMiniMapHUD"), AArcGisMiniMapHUD::StaticClass());
    CheckSavedType(TEXT("/Script/Landscapes.ArcGISMiniMapWidget"), UArcGisMiniMapWidget::StaticClass());
    CheckSavedType(TEXT("/Script/Landscapes.ECustomImageFormat"), StaticEnum<ECustomImageFormat>());
    return true;
}

#endif
