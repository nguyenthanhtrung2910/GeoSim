#include "OSMRoadImporter.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Coordinates/LevelCoordinates.h"
#include "ConcurrencyHelpers/Concurrency.h"
#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"
#include "UObject/UnrealType.h"

using namespace OsmRoadImporterInternal;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOsmRoadImporterCacheTest, "Landscapes.OSMRoadImporter.LocalFileCache",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOsmRoadImporterCacheTest::RunTest(const FString& Parameters)
{
    const UWorld::InitializationValues Init = UWorld::InitializationValues()
        .AllowAudioPlayback(false).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
    if (!TestNotNull(TEXT("Isolated data import world"), World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
    AOsmRoadImporter* Importer = World->SpawnActor<AOsmRoadImporter>();
    ALevelCoordinates* Coordinates = World->SpawnActor<ALevelCoordinates>();
    if (!Importer || !Coordinates)
    {
        AddError(TEXT("Could not create data importer and coordinates"));
        World->DestroyWorld(false);
        GEngine->DestroyWorldContext(World);
        return false;
    }
    TestTrue(TEXT("Data importer does not inherit output-folder settings"),
        FindFProperty<FProperty>(Importer->GetClass(), TEXT("SpawnedActorsPath")) == nullptr);
    TestTrue(TEXT("Data importer does not inherit spline-generation settings"),
        FindFProperty<FProperty>(Importer->GetClass(), TEXT("bUseLandscapeSplines")) == nullptr);

    const FString File = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("OSMRoadImporter"), TEXT(".osm"));
    const FString Xml = TEXT(
        "<osm version=\"0.6\">"
        "<node id=\"1\" lon=\"0\" lat=\"0\"/><node id=\"2\" lon=\"0.00001\" lat=\"0\"/>"
        "<node id=\"3\" lon=\"0.00002\" lat=\"0.00001\"/>"
        "<way id=\"10\"><nd ref=\"1\"/><nd ref=\"2\"/><tag k=\"highway\" v=\"residential\"/></way>"
        "<way id=\"20\"><nd ref=\"2\"/><nd ref=\"3\"/><tag k=\"highway\" v=\"primary\"/>"
        "<tag k=\"bridge\" v=\"yes\"/><tag k=\"layer\" v=\"1\"/></way>"
        "<way id=\"30\"><nd ref=\"3\"/><nd ref=\"1\"/><tag k=\"highway\" v=\"footway\"/></way>"
        "<way id=\"40\"><nd ref=\"1\"/><nd ref=\"2\"/><tag k=\"building\" v=\"yes\"/></way>"
        "</osm>");
    if (!FFileHelper::SaveStringToFile(Xml, *File))
    {
        AddError(TEXT("Could not write temporary OSM source"));
        World->DestroyWorld(false);
        GEngine->DestroyWorldContext(World);
        return false;
    }
    Importer->Source = EOsmRoadSource::LocalFile;
    Importer->LocalFile = File;
    Importer->PointsOffset = FVector(0, 0, 5);
    Importer->TerrainSelection.ActorSelectionMode = EActorSelectionMode::Actor;
    Importer->TerrainSelection.Actor = Importer; // No collision: missing samples must remain in the cache.

    struct FResult
    {
        bool bImported = false;
        bool bCopied = false;
        bool bCopyIndependent = false;
        bool bStaleRejected = false;
        bool bRefreshed = false;
        bool bClearRejected = false;
        TArray<FImportedRoadWay> Ways;
    };
    AddExpectedError(TEXT("Cached road data is stale."), EAutomationExpectedErrorFlags::Contains, 1);
    AddExpectedError(TEXT("No cached road data."), EAutomationExpectedErrorFlags::Contains, 1);
    TSharedRef<TFuture<FResult>> Future = MakeShared<TFuture<FResult>>(Async(EAsyncExecution::Thread, [Importer]()
    {
        FResult Result;
        Result.bImported = Importer->ImportRoadData(Result.Ways, false);
        TArray<FImportedRoadWay> Copy;
        Result.bCopied = Importer->GetCachedRoadData(Copy);
        if (!Copy.IsEmpty()) Copy[0].Tags.Add(TEXT("test-copy-only"), TEXT("yes"));
        Result.bCopyIndependent = Importer->GetCachedRoadData(Copy) && !Copy.IsEmpty() && !Copy[0].Tags.Contains(TEXT("test-copy-only"));
        Concurrency::RunOnGameThreadAndWait([Importer]() { Importer->PointsOffset.Z += 10; return true; });
        Result.bStaleRejected = !Importer->GetCachedRoadData(Copy);
        TArray<FImportedRoadWay> Refreshed;
        Result.bRefreshed = Importer->ImportRoadData(Refreshed, false);
        Concurrency::RunOnGameThreadAndWait([Importer]() { Importer->ClearData(); return true; });
        Result.bClearRejected = !Importer->GetCachedRoadData(Copy);
        return Result;
    }));

    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Future, World, File]()
    {
        if (!Future->IsReady()) return false;
    const auto [bImported, bCopied, bCopyIndependent, bStaleRejected, bRefreshed, bClearRejected, Ways]= Future->Get();
        TestTrue(TEXT("Local OSM file imports"), bImported);
        TestTrue(TEXT("Road data can be copied without reimport"), bCopied);
        TestTrue(TEXT("Generator changes cannot mutate the shared cache"), bCopyIndependent);
        TestTrue(TEXT("Changed import settings invalidate cached data"), bStaleRejected);
        TestTrue(TEXT("Explicit generation refreshes invalidated data"), bRefreshed);
        TestTrue(TEXT("Clear removes cached data"), bClearRejected);
        TestEqual(TEXT("All highway classes remain; building ways are excluded"), Ways.Num(), 3);
        if (Ways.Num() == 3)
        {
            TestEqual(TEXT("Source way identity is retained"), Ways[0].WayId, static_cast<int64>(10));
            TestEqual(TEXT("Shared node IDs are retained"), Ways[0].Points[1].NodeId, Ways[1].Points[0].NodeId);
            TestEqual(TEXT("Bridge tags are retained for generator filtering"), Ways[1].Tags.FindRef(TEXT("bridge")),
	    FString(TEXT("yes")));
            TestEqual(TEXT("Footways are cached for other generators"), Ways[2].Tags.FindRef(TEXT("highway")), FString(
	    TEXT("footway")));
            TestEqual(TEXT("Placement offset is applied"), Ways[0].Points[0].WorldLocation.Z, 5.0);
            TestFalse(TEXT("Missing terrain is marked without filtering the point"), Ways[0].Points[0].bHasTerrain);
        }
        IFileManager::Get().Delete(*File);
        World->DestroyWorld(false);
        GEngine->DestroyWorldContext(World);
        return true;
    }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOSMRoadImporterJsonTest, "Landscapes.OSMRoadImporter.JsonTopology",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOSMRoadImporterJsonTest::RunTest(const FString& Parameters)
{
    const FString Json = TEXT(R"({"elements":[
        {"type":"way","id":77,"nodes":[11,12],"geometry":[{"lon":1,"lat":2},{"lon":3,"lat":4}],
         "tags":{"highway":"primary","bridge":"yes","layer":"1","lanes":"2"}},
        {"type":"way","id":78,"nodes":[11,12],"tags":{"building":"yes"}}
    ]})");
    TMap<int64, FSourceRoadNode> Nodes;
    TArray<FSourceRoadWay> Ways;
    TestTrue(TEXT("Overpass JSON with inline geometry parses"), ParseRoadJson(Json, Nodes, Ways));
    TestEqual(TEXT("Only highway ways are cached"), Ways.Num(), 1);
    TestEqual(TEXT("Inline geometry supplies referenced nodes"), Nodes.Num(), 2);
    if (Ways.Num() == 1)
    {
        TestEqual(TEXT("Node order is retained"), Ways[0].Nodes[1], static_cast<int64>(12));
        TestEqual(TEXT("Non-ground highway attributes are retained"), Ways[0].Tags.FindRef(TEXT("bridge")), FString(TEXT("yes")));
        TestEqual(TEXT("Other way attributes are retained"), Ways[0].Tags.FindRef(TEXT("lanes")), FString(TEXT("2")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOSMRoadImporterXmlTest, "Landscapes.OSMRoadImporter.XmlTopology",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOSMRoadImporterXmlTest::RunTest(const FString& Parameters)
{
    const FString Body = TEXT(
        "<osm version=\"0.6\" generator=\"Overpass API\">\n"
        "<note>OpenStreetMap data</note>\n<meta osm_base=\"2026-10-27T22:20:17Z\"/>\n"
        "<node id=\"265665810\" lat=\"44.6033254\" lon=\"33.5209404\"/>\n"
        "<node id=\"268807404\" lat=\"44.6088627\" lon=\"33.5179027\"/>\n"
        "<way id=\"12345678901\"><nd ref=\"265665810\"/><nd ref=\"268807404\"/>"
        "<tag k=\"highway\" v=\"footway\"/><tag k=\"lanes\" v=\"3\"/></way>\n</osm>");
    for (const FString Prefix : { FString(), FString(TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")),
        FString(TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>")) })
    {
        TMap<int64, FSourceRoadNode> Nodes;
        TArray<FSourceRoadWay> Ways;
        TestTrue(TEXT("OSM XML parses with no declaration, a separate declaration, or an inline declaration"),
            ParseRoadXml(Prefix + Body, Nodes, Ways));
        TestEqual(TEXT("OSM XML node count"), Nodes.Num(), 2);
        TestEqual(TEXT("OSM XML highway count"), Ways.Num(), 1);
        if (Ways.Num() == 1)
        {
            TestEqual(TEXT("64-bit way identity is retained"), Ways[0].Id, int64(12345678901));
            TestEqual(TEXT("Node references are retained"), Ways[0].Nodes[1], int64(268807404));
            TestEqual(TEXT("Highway tags are retained"), Ways[0].Tags.FindRef(TEXT("highway")), FString(TEXT("footway")));
        }
    }
    // Optional local reproduction without making downloaded data a test dependency.
    FString SourceFile;
    if (FParse::Value(FCommandLine::Get(), TEXT("OSMRoadXmlFile="), SourceFile))
    {
        FString Xml;
        if (!TestTrue(TEXT("Read supplied OSM XML file"), FFileHelper::LoadFileToString(Xml, *SourceFile))) return false;
        TMap<int64, FSourceRoadNode> Nodes;
        TArray<FSourceRoadWay> Ways;
        TestTrue(TEXT("Downloaded OSM XML parses"), ParseRoadXml(Xml, Nodes, Ways));
        TestTrue(TEXT("Downloaded XML contains nodes and highway ways"), !Nodes.IsEmpty() && !Ways.IsEmpty());
        for (const FSourceRoadWay& Way : Ways)
            for (int64 NodeId : Way.Nodes)
                TestTrue(TEXT("Downloaded highway references have node coordinates"), Nodes.Contains(NodeId));
        AddInfo(FString::Printf(TEXT("Downloaded OSM: %d nodes, %d highway ways."), Nodes.Num(), Ways.Num()));
    }
    return true;
}
#endif
