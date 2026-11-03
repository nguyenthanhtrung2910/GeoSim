#if WITH_DEV_AUTOMATION_TESTS

#include "ArcGISMiniMapHUD.h"
#include "ArcGISMiniMapWidget.h"
#include "FrameCaptureManager.h"
#include "LandscapeTreeScatter.h"
#include "ThirdPersonCharacter.h"
#include "Misc/AutomationTest.h"
#include "UObject/SoftObjectPath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeoSimModuleRedirectTest,
    "GeoSim.ModuleRedirects.SavedTypes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeoSimModuleRedirectTest::RunTest(const FString& Parameters)
{
    auto check_saved_type = [this](const TCHAR* OldPath, UObject* ExpectedType)
    {
        FSoftObjectPath Path(OldPath);
        TestTrue(FString::Printf(TEXT("Redirect registered for %s"), OldPath), Path.FixupCoreRedirects());
        TestEqual(TEXT("Saved type resolves to its current path"), Path.ToString(), ExpectedType->GetPathName());
        TestEqual(TEXT("Saved type can still be loaded"), Path.TryLoad(), ExpectedType);
    };

    check_saved_type(TEXT("/Script/Landscapes.ThirdPersonCharacter"), AThirdPersonCharacter::StaticClass());
    check_saved_type(TEXT("/Script/Landscapes.FrameCaptureManager"), AFrameCaptureManager::StaticClass());
    check_saved_type(TEXT("/Script/Landscapes.LandscapeTreeScatter"), ALandscapeTreeScatter::StaticClass());
    check_saved_type(TEXT("/Script/Landscapes.ArcGISMiniMapHUD"), AArcGisMiniMapHUD::StaticClass());
    check_saved_type(TEXT("/Script/Landscapes.ArcGISMiniMapWidget"), UArcGisMiniMapWidget::StaticClass());
    check_saved_type(TEXT("/Script/Landscapes.ECustomImageFormat"), StaticEnum<ECustomImageFormat>());
    return true;
}

#endif
