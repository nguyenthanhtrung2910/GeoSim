#include "OSMRoadImporter.h"

#include "Components/SceneComponent.h"
#include "Coordinates/LevelCoordinates.h"
#include "SplineImporter/Overpass.h"
#include "Landscape.h"
#include "Async/Async.h"
#include "UObject/StrongObjectPtr.h"

#include "ConcurrencyHelpers/Concurrency.h"
#include "Dom/JsonObject.h"
#include "GDALInterface/GDALInterface.h"
#include "LandscapeUtils/LandscapeUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"
#include "HAL/FileManager.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"
#include "String/LexFromString.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "XmlFile.h"
#include "XmlNode.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Engine/Engine.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogOSMRoadImporter, Log, All);

namespace
{
	struct FSourceRoadNode
	{
		double Longitude = 0;
		double Latitude = 0;
	};

	struct FSourceRoadWay
	{
		int64 Id = 0;
		TArray<int64> Nodes;
		TMap<FString, FString> Tags;
	};

	bool ParseRoadXml(const FString& Text, TMap<int64, FSourceRoadNode>& Nodes, TArray<FSourceRoadWay>& Ways)
	{
		FXmlFile Xml(Text, EConstructMethod::ConstructFromBuffer);
		const FXmlNode* Root = Xml.GetRootNode();
		if (!Xml.IsValid() || !Root || Root->GetTag() != TEXT("osm")) return false;
		for (const FXmlNode* Element : Root->GetChildrenNodes())
		{
			if (Element->GetTag() == TEXT("node"))
			{
				FSourceRoadNode Node;
				int64 Id = 0;
				if (!LexTryParseString(Id, *Element->GetAttribute(TEXT("id"))) ||
					!LexTryParseString(Node.Longitude, *Element->GetAttribute(TEXT("lon"))) ||
					!LexTryParseString(Node.Latitude, *Element->GetAttribute(TEXT("lat")))) return false;
				Nodes.Add(Id, Node);
			}
			else if (Element->GetTag() == TEXT("way"))
			{
				FSourceRoadWay Way;
				Way.Id = FCString::Atoi64(*Element->GetAttribute(TEXT("id")));
				for (const FXmlNode* Child : Element->GetChildrenNodes())
				{
					if (Child->GetTag() == TEXT("nd"))
						Way.Nodes.Add(FCString::Atoi64(*Child->GetAttribute(TEXT("ref"))));
					else if (Child->GetTag() == TEXT("tag"))
						Way.Tags.Add(Child->GetAttribute(TEXT("k")), Child->GetAttribute(TEXT("v")));
				}
				if (Way.Tags.Contains(TEXT("highway")) && Way.Nodes.Num() >= 2) Ways.Add(MoveTemp(Way));
			}
		}
		return true;
	}

	bool ParseRoadJson(const FString& Text, TMap<int64, FSourceRoadNode>& Nodes, TArray<FSourceRoadWay>& Ways)
	{
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()) return false;
		const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
		if (!Root->TryGetArrayField(TEXT("elements"), Elements)) return false;
		for (const TSharedPtr<FJsonValue>& Value : *Elements)
		{
			const TSharedPtr<FJsonObject> Element = Value->AsObject();
			FString Type;
			double Id = 0;
			if (!Element || !Element->TryGetStringField(TEXT("type"), Type) || !Element->TryGetNumberField(TEXT("id"), Id)) continue;
			if (Type == TEXT("node"))
			{
				FSourceRoadNode Node;
				if (Element->TryGetNumberField(TEXT("lon"), Node.Longitude) && Element->TryGetNumberField(TEXT("lat"), Node.Latitude))
					Nodes.Add(static_cast<int64>(Id), Node);
				continue;
			}
			const TSharedPtr<FJsonObject>* Tags = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Refs = nullptr;
			if (Type != TEXT("way") || !Element->TryGetObjectField(TEXT("tags"), Tags) ||
				!Element->TryGetArrayField(TEXT("nodes"), Refs)) continue;
			FSourceRoadWay Way;
			Way.Id = static_cast<int64>(Id);
			for (const auto& Tag : (*Tags)->Values)
			{
				FString TagValue;
				if (Tag.Value->TryGetString(TagValue)) Way.Tags.Add(FString(*Tag.Key), TagValue);
			}
			if (!Way.Tags.Contains(TEXT("highway"))) continue;
			const TArray<TSharedPtr<FJsonValue>>* Geometry = nullptr;
			Element->TryGetArrayField(TEXT("geometry"), Geometry);
			for (int32 i = 0; i < Refs->Num(); ++i)
			{
				double Ref = 0;
				if (!(*Refs)[i]->TryGetNumber(Ref)) return false;
				const int64 NodeId = static_cast<int64>(Ref);
				Way.Nodes.Add(NodeId);
				if (Geometry && Geometry->IsValidIndex(i))
				{
					const TSharedPtr<FJsonObject> Point = (*Geometry)[i]->AsObject();
					FSourceRoadNode Node;
					if (Point && Point->TryGetNumberField(TEXT("lon"), Node.Longitude) && Point->TryGetNumberField(TEXT("lat"), Node.Latitude))
						Nodes.Add(NodeId, Node);
				}
			}
			if (Way.Nodes.Num() >= 2) Ways.Add(MoveTemp(Way));
		}
		return true;
	}
}

void AOsmRoadImporter::ClearRoadDataCache()
{
	// An import can be waiting for the game thread while holding the worker lock.
	// A revision change invalidates both cached and in-flight data without blocking the editor.
	RoadDataCacheRevision.Increment();
	if (RoadDataCacheMutex.TryLock())
	{
		CachedRoadDataRevision = -1;
		CachedRoadDataKey.Reset();
		CachedRoadWays.Reset();
		RoadDataCacheMutex.Unlock();
	}
	UE_LOG(LogOSMRoadImporter, Log, TEXT("Shared road data cleared. Click Generate on OSMRoadImporter before generating roads."));
}

FString AOsmRoadImporter::MakeRoadDataCacheKey()
{
	check(IsInGameThread());
	FString Key;
	auto AppendProperty = [&Key](UObject* Object, FName PropertyName)
	{
		if (!Object) return;
		const FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), PropertyName);
		if (!Property) return;
		FString Value;
		Property->ExportText_InContainer(0, Value, Object, nullptr, Object, PPF_None);
		Key += PropertyName.ToString() + TEXT("=") + Value + TEXT("\n");
	};
	for (const TCHAR* Name : { TEXT("Source"), TEXT("LocalFile"), TEXT("OverpassQuery"), TEXT("OverpassShortQuery"),
		TEXT("BoundingMethod"), TEXT("BoundingZoneZoom"), TEXT("BoundingZoneMinX"), TEXT("BoundingZoneMaxX"),
		TEXT("BoundingZoneMinY"), TEXT("BoundingZoneMaxY"), TEXT("BoundingActorSelection"),
		TEXT("TerrainSelection"), TEXT("PointsOffset") })
		AppendProperty(this, FName(Name));

	if (Source == EOsmRoadSource::LocalFile)
	{
		Key += FString::Printf(TEXT("LocalFileStat=%lld/%lld\n"), IFileManager::Get().FileSize(*LocalFile),
			IFileManager::Get().GetTimeStamp(*LocalFile).GetTicks());
	}
	if (BoundingMethod == EOsmRoadBounds::BoundingActor && Source != EOsmRoadSource::LocalFile && Source != EOsmRoadSource::OverpassQuery)
	{
		if (AActor* BoundingActor = BoundingActorSelection.GetActor(GetWorld()))
		{
			FVector Origin, Extent;
			BoundingActor->GetActorBounds(true, Origin, Extent);
			// Z bounds can change when ground roads deform terrain; only XY defines the query area.
			Origin.Z = Extent.Z = 0.0;
			Key += TEXT("Bounds=") + BoundingActor->GetPathName() + Origin.ToString() + Extent.ToString() + TEXT("\n");
		}
	}
	TArray<FString> TargetKeys;
	for (AActor* Target : TerrainSelection.GetAllActors(GetWorld()))
	{
		if (!IsValid(Target)) continue;
		TargetKeys.Add(Target->GetPathName() + Target->GetActorTransform().ToString());
	}
	TargetKeys.Sort();
	for (const FString& TargetKey : TargetKeys) Key += TEXT("Terrain=") + TargetKey + TEXT("\n");
	if (UWorld* World = GetWorld()) Key += TEXT("Origin=") + World->OriginLocation.ToString() + TEXT("\n");
	if (UGlobalCoordinates* Coordinates = ALevelCoordinates::GetGlobalCoordinates(GetWorld(), false))
	{
		Key += TEXT("Coordinates=") + Coordinates->GetPathName() + TEXT("\n");
		for (const TCHAR* Name : { TEXT("CRS"), TEXT("CmPerLongUnit"), TEXT("CmPerLatUnit"), TEXT("WorldOriginLong"), TEXT("WorldOriginLat") })
			AppendProperty(Coordinates, FName(Name));
	}
	return Key;
}

bool AOsmRoadImporter::ImportRoadData(TArray<FImportedRoadWay>& OutWays, bool bIsUserInitiated)
{
	OutWays.Reset();
	if (IsInGameThread())
	{
		UE_LOG(LogOSMRoadImporter, Error, TEXT("ImportRoadData must be called on a background thread."));
		return false;
	}
	// Only explicit importer generation refreshes data. Mesh generation never calls this path.
	FScopeLock CacheLock(&RoadDataCacheMutex);
	const int32 Revision = RoadDataCacheRevision.GetValue();
	FString Key;
	Concurrency::RunOnGameThreadAndWait([&]() { Key = MakeRoadDataCacheKey(); return true; });

	// A failed refresh must not make previously cached data valid again.
	CachedRoadDataRevision = -1;
	CachedRoadWays.Reset();
	if (!ImportRoadDataUncached(OutWays, bIsUserInitiated))
	{
		OutWays.Reset();
		return false;
	}
	FString CompletedKey;
	Concurrency::RunOnGameThreadAndWait([&]() { CompletedKey = MakeRoadDataCacheKey(); return true; });
	if (RoadDataCacheRevision.GetValue() != Revision || CompletedKey != Key)
	{
		OutWays.Reset();
		UE_LOG(LogOSMRoadImporter, Warning, TEXT("Road import settings or cache revision changed during import. Generate again to use the updated source."));
		return false;
	}
	CachedRoadWays = OutWays;
	CachedRoadDataKey = MoveTemp(Key);
	CachedRoadDataRevision = Revision;
	return true;
}

bool AOsmRoadImporter::GetCachedRoadData(TArray<FImportedRoadWay>& OutWays)
{
	OutWays.Reset();
	if (IsInGameThread())
	{
		UE_LOG(LogOSMRoadImporter, Error, TEXT("GetCachedRoadData must be called on a background thread."));
		return false;
	}
	FScopeLock CacheLock(&RoadDataCacheMutex);
	const int32 Revision = RoadDataCacheRevision.GetValue();
	if (CachedRoadDataRevision != Revision || CachedRoadWays.IsEmpty())
	{
		UE_LOG(LogOSMRoadImporter, Error, TEXT("No cached road data. Click Generate on the referenced OSMRoadImporter first."));
		return false;
	}
	FString Key;
	Concurrency::RunOnGameThreadAndWait([&]() { Key = MakeRoadDataCacheKey(); return true; });
	if (RoadDataCacheRevision.GetValue() != Revision || CachedRoadDataKey != Key)
	{
		CachedRoadDataRevision = -1;
		CachedRoadWays.Reset();
		UE_LOG(LogOSMRoadImporter, Error, TEXT("Cached road data is stale. Click Generate on the referenced OSMRoadImporter again."));
		return false;
	}
	OutWays = CachedRoadWays;
	UE_LOG(LogOSMRoadImporter, Log, TEXT("Read %d cached road ways from OSMRoadImporter."), OutWays.Num());
	return true;
}

bool AOsmRoadImporter::ImportRoadDataUncached(TArray<FImportedRoadWay>& OutWays, bool bIsUserInitiated)
{
	OutWays.Reset();
	if (IsInGameThread())
	{
		UE_LOG(LogOSMRoadImporter, Error, TEXT("ImportRoadData must be called on a background thread."));
		return false;
	}

	FString Text;
	if (!LoadSourceText(Text, bIsUserInitiated)) return false;
	Text.TrimStartInline();
	TMap<int64, FSourceRoadNode> SourceNodes;
	TArray<FSourceRoadWay> SourceWays;
	const bool bParsed = Text.StartsWith(TEXT("<")) ? ParseRoadXml(Text, SourceNodes, SourceWays) : ParseRoadJson(Text, SourceNodes, SourceWays);
	if (!bParsed || SourceWays.IsEmpty())
	{
		UE_LOG(LogOSMRoadImporter, Error, TEXT("Road data requires OSM XML or Overpass JSON with highway ways and node IDs."));
		return false;
	}

	return Concurrency::RunOnGameThreadAndWait([&]()
	{
		const TArray<AActor*> Targets = TerrainSelection.GetAllActors(GetWorld());
		FCollisionQueryParams QueryParams;
		if (Targets.IsEmpty() || !LandscapeUtils::CustomCollisionQueryParams(Targets, QueryParams))
		{
			UE_LOG(LogOSMRoadImporter, Error, TEXT("Select terrain collision actors on OSMRoadImporter before generating data."));
			return false;
		}
		UGlobalCoordinates* Coordinates = ALevelCoordinates::GetGlobalCoordinates(GetWorld(), true);
		if (!Coordinates) return false;
		OGRCoordinateTransformation* Transform = Coordinates->GetCRSTransformer(TEXT("EPSG:4326"));
		if (!Transform) return false;
		TMap<int64, FImportedRoadPoint> Projected;
		TSet<int64> ImportedWayIds;
		int32 MissingTerrain = 0;
		for (const FSourceRoadWay& SourceWay : SourceWays)
		{
			if (ImportedWayIds.Contains(SourceWay.Id)) continue;
			ImportedWayIds.Add(SourceWay.Id);
			FImportedRoadWay Way;
			Way.WayId = SourceWay.Id;
			Way.Tags = SourceWay.Tags;
			for (int64 NodeId : SourceWay.Nodes)
			{
				if (!Projected.Contains(NodeId))
				{
					FImportedRoadPoint Point;
					Point.NodeId = NodeId;
					if (const FSourceRoadNode* Node = SourceNodes.Find(NodeId))
					{
						if (!FMath::IsFinite(Node->Longitude) || !FMath::IsFinite(Node->Latitude) ||
							FMath::Abs(Node->Longitude) > 180.0 || FMath::Abs(Node->Latitude) > 90.0)
						{
							OGRCoordinateTransformation::DestroyCT(Transform);
							OutWays.Reset();
							UE_LOG(LogOSMRoadImporter, Error, TEXT("Invalid OSM longitude/latitude at node %lld."), NodeId);
							return false;
						}
						FVector2D XY;
						double Z = 0;
						if (!Coordinates->GetUnrealCoordinatesFromCRS(Node->Longitude, Node->Latitude, Transform, XY))
						{
							OGRCoordinateTransformation::DestroyCT(Transform);
							OutWays.Reset();
							return false;
						}
						Point.bHasTerrain = LandscapeUtils::GetZ(GetWorld(), QueryParams, XY.X, XY.Y, Z, bDebugLineTraces);
						Point.WorldLocation = FVector(XY.X, XY.Y, Z) + PointsOffset;
					}
					if (!Point.bHasTerrain) ++MissingTerrain;
					Projected.Add(NodeId, Point);
				}
				Way.Points.Add(Projected[NodeId]);
			}
			OutWays.Add(MoveTemp(Way));
		}
		OGRCoordinateTransformation::DestroyCT(Transform);
		UE_LOG(LogOSMRoadImporter, Log, TEXT("Imported %d road ways, %d nodes; %d nodes lack source geometry or terrain collision."), OutWays.Num(), Projected.Num(), MissingTerrain);
		return true;
	});
}


AOsmRoadImporter::AOsmRoadImporter()
{
    PrimaryActorTick.bCanEverTick = false;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

void AOsmRoadImporter::GenerateData()
{
    if (bGenerationInProgress) return;
    bGenerationInProgress = true;
    Async(EAsyncExecution::Thread, [StrongThis = TStrongObjectPtr<AOsmRoadImporter>(this)]()
    {
        TArray<FImportedRoadWay> Ways;
        const bool bSuccess = StrongThis->ImportRoadData(Ways, true);
        UE_LOG(LogOSMRoadImporter, Log, TEXT("OSM data generation %s."), bSuccess ? TEXT("succeeded") : TEXT("failed"));
        AsyncTask(ENamedThreads::GameThread, [WeakThis = TWeakObjectPtr<AOsmRoadImporter>(StrongThis.Get())]()
        {
            if (WeakThis.IsValid()) WeakThis->bGenerationInProgress = false;
        });
    });
}

void AOsmRoadImporter::ClearData()
{
    ClearRoadDataCache();
}

bool AOsmRoadImporter::ConfigureForTiles(int Zoom, int MinX, int MaxX, int MinY, int MaxY)
{
    BoundingMethod = EOsmRoadBounds::TileNumbers;
    BoundingZoneZoom = Zoom;
    BoundingZoneMinX = MinX;
    BoundingZoneMaxX = MaxX;
    BoundingZoneMinY = MinY;
    BoundingZoneMaxY = MaxY;
    ClearRoadDataCache();
    return true;
}

bool AOsmRoadImporter::LoadSourceText(FString& OutText, bool bIsUserInitiated)
{
    if (Source == EOsmRoadSource::LocalFile)
        return FFileHelper::LoadFileToString(OutText, *LocalFile);

    FString URL;
    if (Source == EOsmRoadSource::OverpassQuery)
    {
        URL = TEXT("https://overpass-api.de/api/interpreter?data=") + OverpassQuery;
    }
    else
    {
        FVector4d Bounds;
        const bool bHasBounds = Concurrency::RunOnGameThreadAndWait([this, &Bounds]()
        {
            if (BoundingMethod == EOsmRoadBounds::BoundingActor)
            {
                AActor* BoundsActor = BoundingActorSelection.GetActor(GetWorld());
                if (!IsValid(BoundsActor)) return false;
                if (ALandscape* Landscape = Cast<ALandscape>(BoundsActor))
                    return LandscapeUtils::GetLandscapeCRSBounds(Landscape, TEXT("EPSG:4326"), Bounds);
                FVector Origin, Extent;
                BoundsActor->GetActorBounds(true, Origin, Extent);
                return ALevelCoordinates::GetCRSCoordinatesFromOriginExtent(GetWorld(), Origin, Extent, TEXT("EPSG:4326"), Bounds);
            }
            if (BoundingZoneZoom < 0 || BoundingZoneZoom > 22) return false;
            const double TileCount = static_cast<double>(1 << BoundingZoneZoom);
            if (BoundingZoneMinX < 0 || BoundingZoneMinY < 0 ||
                BoundingZoneMinX > BoundingZoneMaxX || BoundingZoneMinY > BoundingZoneMaxY ||
                BoundingZoneMaxX >= TileCount || BoundingZoneMaxY >= TileCount) return false;
            const double West = BoundingZoneMinX / TileCount * 360.0 - 180.0;
            const double East = (BoundingZoneMaxX + 1) / TileCount * 360.0 - 180.0;
            const double North = FMath::RadiansToDegrees(FMath::Atan(FMath::Sinh(UE_PI * (1.0 - 2.0 * BoundingZoneMinY / TileCount))));
            const double South = FMath::RadiansToDegrees(FMath::Atan(FMath::Sinh(UE_PI * (1.0 - 2.0 * (BoundingZoneMaxY + 1) / TileCount))));
            Bounds = FVector4d(West, East, South, North);
            return true;
        });
        if (!bHasBounds)
        {
            UE_LOG(LogOSMRoadImporter, Error, TEXT("Select a valid bounding actor or tile range on OSMRoadImporter."));
            return false;
        }
        const FString Selection = Source == EOsmRoadSource::Roads ? TEXT("way[\"highway\"];") : OverpassShortQuery;
        URL = Overpass::QueryFromShortQuery(TEXT("https://overpass-api.de/api/interpreter"), Bounds.Z, Bounds.X, Bounds.W, Bounds.Y, Selection);
    }
    GDALDataset* Dataset = GDALInterface::LoadGDALVectorDatasetFromQuery(URL, bIsUserInitiated);
    if (!Dataset) return false;
    const FString File = UTF8_TO_TCHAR(Dataset->GetDescription());
    GDALClose(Dataset);
    return FFileHelper::LoadFileToString(OutText, *File);
}

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
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
        const FResult Result = Future->Get();
        TestTrue(TEXT("Local OSM file imports"), Result.bImported);
        TestTrue(TEXT("Road data can be copied without reimport"), Result.bCopied);
        TestTrue(TEXT("Generator changes cannot mutate the shared cache"), Result.bCopyIndependent);
        TestTrue(TEXT("Changed import settings invalidate cached data"), Result.bStaleRejected);
        TestTrue(TEXT("Explicit generation refreshes invalidated data"), Result.bRefreshed);
        TestTrue(TEXT("Clear removes cached data"), Result.bClearRejected);
        TestEqual(TEXT("All highway classes remain; building ways are excluded"), Result.Ways.Num(), 3);
        if (Result.Ways.Num() == 3)
        {
            TestEqual(TEXT("Source way identity is retained"), Result.Ways[0].WayId, int64(10));
            TestEqual(TEXT("Shared node IDs are retained"), Result.Ways[0].Points[1].NodeId, Result.Ways[1].Points[0].NodeId);
            TestEqual(TEXT("Bridge tags are retained for generator filtering"), Result.Ways[1].Tags.FindRef(TEXT("bridge")), FString(TEXT("yes")));
            TestEqual(TEXT("Footways are cached for other generators"), Result.Ways[2].Tags.FindRef(TEXT("highway")), FString(TEXT("footway")));
            TestEqual(TEXT("Placement offset is applied"), Result.Ways[0].Points[0].WorldLocation.Z, 5.0);
            TestFalse(TEXT("Missing terrain is marked without filtering the point"), Result.Ways[0].Points[0].bHasTerrain);
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
        TestEqual(TEXT("Node order is retained"), Ways[0].Nodes[1], int64(12));
        TestEqual(TEXT("Non-ground highway attributes are retained"), Ways[0].Tags.FindRef(TEXT("bridge")), FString(TEXT("yes")));
        TestEqual(TEXT("Other way attributes are retained"), Ways[0].Tags.FindRef(TEXT("lanes")), FString(TEXT("2")));
    }
    return true;
}
#endif
