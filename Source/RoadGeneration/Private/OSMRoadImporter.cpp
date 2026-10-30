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

DEFINE_LOG_CATEGORY_STATIC(LogOSMRoadImporter, Log, All);

namespace OsmRoadImporterInternal
{
	bool ParseRoadXml(const FString& Text, TMap<int64, FSourceRoadNode>& Nodes, TArray<FSourceRoadWay>& Ways)
	{
		FString XmlText = Text.TrimStart();
		// Overpass Turbo can put <?xml ...?><osm ...> on one line. FXmlFile
		// discards the declaration's line, including that root tag. The file has
		// already been decoded to FString, so the encoding declaration is redundant.
		if (XmlText.StartsWith(TEXT("<?xml"), ESearchCase::CaseSensitive) && XmlText.Len() > 5 && FChar::IsWhitespace(XmlText[5]))
		{
			const int32 DeclarationEnd = XmlText.Find(TEXT("?>"), ESearchCase::CaseSensitive);
			if (DeclarationEnd == INDEX_NONE) return false;
			XmlText.RightChopInline(DeclarationEnd + 2);
		}
		FXmlFile Xml(XmlText, EConstructMethod::ConstructFromBuffer);
		const FXmlNode* Root = Xml.GetRootNode();
		if (!Xml.IsValid() || !Root || Root->GetTag() != TEXT("osm")) return false;
		for (const FXmlNode* Element : Root->GetChildrenNodes())
		{
			if (Element->GetTag() == TEXT("node"))
			{
				FSourceRoadNode Node;
				int64 Id = 0;
				if (
					!LexTryParseString(Id, *Element->GetAttribute(TEXT("id"))) ||
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
				if (FSourceRoadNode Node; Element->TryGetNumberField(TEXT("lon"), Node.Longitude) && Element->
					TryGetNumberField(TEXT("lat"), Node.Latitude))
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
				if (FString TagValue; Tag.Value->TryGetString(TagValue)) Way.Tags.Add(FString(*Tag.Key), TagValue);
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
					if (FSourceRoadNode Node; Point && Point->TryGetNumberField(TEXT("lon"), Node.Longitude) && Point->
						TryGetNumberField(TEXT("lat"), Node.Latitude))
						Nodes.Add(NodeId, Node);
				}
			}
			if (Way.Nodes.Num() >= 2) Ways.Add(MoveTemp(Way));
		}
		return true;
	}
}

using namespace OsmRoadImporterInternal;

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
	UE_LOG(LogOSMRoadImporter, Log,
	       TEXT("Shared road data cleared. Click Generate on OSMRoadImporter before generating roads."));
}

FString AOsmRoadImporter::MakeRoadDataCacheKey()
{
	check(IsInGameThread());
	FString Key;
	auto append_property = [&Key](UObject* Object, const FName PropertyName)
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
		append_property(this, FName(Name));

	if (Source == EOsmRoadSource::LocalFile)
	{
		Key += FString::Printf(TEXT("LocalFileStat=%lld/%lld\n"), IFileManager::Get().FileSize(*LocalFile),
			IFileManager::Get().GetTimeStamp(*LocalFile).GetTicks());
	}
	if (BoundingMethod == EOsmRoadBounds::BoundingActor && Source != EOsmRoadSource::LocalFile && Source !=
		EOsmRoadSource::OverpassQuery)
	{
		if (const AActor* BoundingActor = BoundingActorSelection.GetActor(GetWorld()))
		{
			FVector Origin, Extent;
			BoundingActor->GetActorBounds(true, Origin, Extent);
			// Z bounds can change when ground roads deform terrain; only XY defines the query area.
			Origin.Z = Extent.Z = 0.0;
			Key += TEXT("Bounds=") + BoundingActor->GetPathName() + Origin.ToString() + Extent.ToString() + TEXT("\n");
		}
	}
	TArray<FString> TargetKeys;
	for (const AActor* Target : TerrainSelection.GetAllActors(GetWorld()))
	{
		if (!IsValid(Target)) continue;
		TargetKeys.Add(Target->GetPathName() + Target->GetActorTransform().ToString());
	}
	TargetKeys.Sort();
	for (const FString& TargetKey : TargetKeys) Key += TEXT("Terrain=") + TargetKey + TEXT("\n");
	if (const UWorld* World = GetWorld()) Key += TEXT("Origin=") + World->OriginLocation.ToString() + TEXT("\n");
	if (UGlobalCoordinates* Coordinates = ALevelCoordinates::GetGlobalCoordinates(GetWorld(), false))
	{
		Key += TEXT("Coordinates=") + Coordinates->GetPathName() + TEXT("\n");
		for (const TCHAR* Name : {
			     TEXT("CRS"), TEXT("CmPerLongUnit"), TEXT("CmPerLatUnit"), TEXT("WorldOriginLong"),
			     TEXT("WorldOriginLat")
		     })
			append_property(Coordinates, FName(Name));
	}
	return Key;
}

bool AOsmRoadImporter::ImportRoadData(TArray<FImportedRoadWay>& OutWays, const bool bIsUserInitiated)
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
		UE_LOG(LogOSMRoadImporter, Warning,
		       TEXT(
			       "Road import settings or cache revision changed during import. Generate again to use the updated source."
		       ));
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
		UE_LOG(LogOSMRoadImporter, Error,
		       TEXT("No cached road data. Click Generate on the referenced OSMRoadImporter first."));
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

bool AOsmRoadImporter::ImportRoadDataUncached(TArray<FImportedRoadWay>& OutWays, bool bIsUserInitiated) const
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
	if (const bool bParsed = Text.StartsWith(TEXT("<"))
		                         ? ParseRoadXml(Text, SourceNodes, SourceWays)
		                         : ParseRoadJson(Text, SourceNodes, SourceWays); !bParsed || SourceWays.IsEmpty())
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
		for (const auto& [Id, Nodes, Tags] : SourceWays)
		{
			if (ImportedWayIds.Contains(Id)) continue;
			ImportedWayIds.Add(Id);
			FImportedRoadWay Way;
			Way.WayId = Id;
			Way.Tags = Tags;
			for (int64 NodeId : Nodes)
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
		UE_LOG(LogOSMRoadImporter, Log,
		       TEXT("Imported %d road ways, %d nodes; %d nodes lack source geometry or terrain collision."),
		       OutWays.Num(), Projected.Num(), MissingTerrain);
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

bool AOsmRoadImporter::ConfigureForTiles(const int Zoom, const int MinX, const int MaxX, const int MinY, const int MaxY)
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

bool AOsmRoadImporter::LoadSourceText(FString& OutText, const bool bIsUserInitiated) const
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
                return ALevelCoordinates::GetCRSCoordinatesFromOriginExtent(
	                GetWorld(), Origin, Extent, TEXT("EPSG:4326"), Bounds);
            }
            if (BoundingZoneZoom < 0 || BoundingZoneZoom > 22) return false;
            const double TileCount = static_cast<double>(1 << BoundingZoneZoom);
            if (BoundingZoneMinX < 0 || BoundingZoneMinY < 0 ||
                BoundingZoneMinX > BoundingZoneMaxX || BoundingZoneMinY > BoundingZoneMaxY ||
                BoundingZoneMaxX >= TileCount || BoundingZoneMaxY >= TileCount) return false;
            const double West = BoundingZoneMinX / TileCount * 360.0 - 180.0;
            const double East = (BoundingZoneMaxX + 1) / TileCount * 360.0 - 180.0;
            const double North = FMath::RadiansToDegrees(
	            FMath::Atan(FMath::Sinh(UE_PI * (1.0 - 2.0 * BoundingZoneMinY / TileCount))));
            const double South = FMath::RadiansToDegrees(
	            FMath::Atan(FMath::Sinh(UE_PI * (1.0 - 2.0 * (BoundingZoneMaxY + 1) / TileCount))));
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
