#if WITH_DEV_AUTOMATION_TESTS

#include "FrameCaptureManager.h"
#include "Misc/AutomationTest.h"
#include "UObject/SoftObjectPath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFrameCaptureRedirectTest,
	"GeoSim.ModuleRedirects.FrameCaptureManager",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFrameCaptureRedirectTest::RunTest(const FString& Parameters)
{
	for (const TCHAR* OldModule : {TEXT("GeoSim"), TEXT("Landscapes")})
	{
		FSoftClassPath ClassPath(FString::Printf(TEXT("/Script/%s.FrameCaptureManager"), OldModule));
		TestTrue(TEXT("Saved capture class is redirected"), ClassPath.FixupCoreRedirects());
		TestEqual(TEXT("Capture class resolves to ComputerVision"),
			ClassPath.ToString(), AFrameCaptureManager::StaticClass()->GetPathName());
		TestEqual(TEXT("Saved capture class can be loaded"),
			ClassPath.TryLoadClass<AActor>(), AFrameCaptureManager::StaticClass());

		FSoftObjectPath EnumPath(FString::Printf(TEXT("/Script/%s.ECustomImageFormat"), OldModule));
		TestTrue(TEXT("Saved image format enum is redirected"), EnumPath.FixupCoreRedirects());
		TestEqual(TEXT("Image format enum resolves to ComputerVision"),
			EnumPath.ToString(), StaticEnum<ECustomImageFormat>()->GetPathName());
		TestEqual(TEXT("Saved image format enum can be loaded"),
			EnumPath.TryLoad(), static_cast<UObject*>(StaticEnum<ECustomImageFormat>()));
	}
	return true;
}

#endif
