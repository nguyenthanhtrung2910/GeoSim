#if WITH_DEV_AUTOMATION_TESTS

#include "OsmRoadScene.h"
#include "Misc/AutomationTest.h"
#include "UObject/SoftObjectPath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOsmRoadClassRedirectTest,
    "GeoSim.RoadGeneration.ClassRedirects",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOsmRoadClassRedirectTest::RunTest(const FString& Parameters)
{
    auto CheckSavedClass = [this](const TCHAR* OldPath, UClass* ExpectedClass)
    {
        FSoftClassPath Path(OldPath);
        TestTrue(FString::Printf(TEXT("Redirect registered for %s"), OldPath), Path.FixupCoreRedirects());
        TestEqual(TEXT("Saved class resolves directly to its new path"), Path.ToString(), ExpectedClass->GetPathName());
        TestEqual(TEXT("Saved class can still be loaded"), Path.TryLoadClass<AActor>(), ExpectedClass);
    };

    CheckSavedClass(TEXT("/Script/RoadGeneration.ImportedRoadConstructorRoad"), AOsmRoadActor::StaticClass());
    CheckSavedClass(TEXT("/Script/RoadGeneration.ImportedRoadConstructorScene"), AOsmRoadScene::StaticClass());
    CheckSavedClass(TEXT("/Script/Landscapes.ImportedRoadConstructorRoad"), AOsmRoadActor::StaticClass());
    CheckSavedClass(TEXT("/Script/Landscapes.ImportedRoadConstructorScene"), AOsmRoadScene::StaticClass());
    return true;
}

#endif
