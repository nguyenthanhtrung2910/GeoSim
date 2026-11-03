#include "OsmBuidingImporter.h"

#include "Async/Async.h"
#include "Components/SceneComponent.h"
#include "ConcurrencyHelpers/Concurrency.h"
#include "Coordinates/LevelCoordinates.h"
#include "Engine/World.h"
#include "GDALInterface/GDALInterface.h"
#include "LandscapeUtils/LandscapeUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/UnrealType.h"
#include "String/LexFromString.h"
#include "SplineImporter/Overpass.h"
#include "UObject/StrongObjectPtr.h"
#include "XmlFile.h"
#include "XmlNode.h"

DEFINE_LOG_CATEGORY(LogOsmBuidingImporter);

namespace OsmBuidingImporterInternal
{
	bool ParseBuildingXml(const FString& Text, TArray<FBuildingFootprint>& Footprints)
	{
		Footprints.Reset();
		FString XmlText = Text.TrimStart();
		// FXmlFile discards the declaration's entire line. Overpass Turbo can put
		// the opening <osm> tag on that same line; the FString is already decoded.
		if (XmlText.StartsWith(TEXT("<?xml"), ESearchCase::CaseSensitive) && XmlText.Len() > 5 &&
			FChar::IsWhitespace(XmlText[5]))
		{
			const int32 End = XmlText.Find(TEXT("?>"), ESearchCase::CaseSensitive);
			if (End == INDEX_NONE) return false;
			XmlText.RightChopInline(End + 2);
		}
		FXmlFile Xml(XmlText, EConstructMethod::ConstructFromBuffer);
		const FXmlNode* Root = Xml.GetRootNode();
		if (!Xml.IsValid() || !Root || Root->GetTag() != TEXT("osm")) return false;

		TMap<int64, FVector2D> Nodes;
		for (const FXmlNode* Element : Root->GetChildrenNodes())
		{
			if (Element->GetTag() != TEXT("node")) continue;
			int64 Id = 0;
			FVector2D Point;
			if (!LexTryParseString(Id, *Element->GetAttribute(TEXT("id"))) ||
				!LexTryParseString(Point.X, *Element->GetAttribute(TEXT("lon"))) ||
				!LexTryParseString(Point.Y, *Element->GetAttribute(TEXT("lat"))) ||
				!FMath::IsFinite(Point.X) || !FMath::IsFinite(Point.Y) ||
				FMath::Abs(Point.X) > 180 || FMath::Abs(Point.Y) > 90) return false;
			Nodes.Add(Id, Point);
		}
		TArray<FBuildingFootprint> Parsed;
		TSet<int64> Seen;
		for (const FXmlNode* Element : Root->GetChildrenNodes())
		{
			if (Element->GetTag() != TEXT("way")) continue;
			int64 Id = 0;
			if (!LexTryParseString(Id, *Element->GetAttribute(TEXT("id")))) return false;
			FBuildingFootprint Footprint;
			TArray<int64> References;
			for (const FXmlNode* Child : Element->GetChildrenNodes())
			{
				if (Child->GetTag() == TEXT("tag"))
					Footprint.Fields.Add(Child->GetAttribute(TEXT("k")), Child->GetAttribute(TEXT("v")));
				else if (Child->GetTag() == TEXT("nd"))
				{
					int64 Reference = 0;
					if (!LexTryParseString(Reference, *Child->GetAttribute(TEXT("ref")))) return false;
					References.Add(Reference);
				}
			}
			if (const FString Building = Footprint.Fields.FindRef(TEXT("building")); Building.IsEmpty() || Building.
				Equals(TEXT("no"), ESearchCase::IgnoreCase) || Seen.Contains(Id)) continue;
			if (References.Num() < 4 || References[0] != References.Last())
			{
				UE_LOG(LogOsmBuidingImporter, Warning, TEXT("Skipping building way %lld: footprint is not a closed ring."), Id);
				continue;
			}
			TSet<int64> UniqueNodes;
			for (int64 Reference : References)
			{
				const FVector2D* Point = Nodes.Find(Reference);
				if (!Point)
				{
					UE_LOG(LogOsmBuidingImporter, Error,
					       TEXT("Building way %lld references missing node %lld. Export its referenced nodes too."), Id,
					       Reference);
					return false;
				}
				UniqueNodes.Add(Reference);
				Footprint.Points.Emplace(Point->X, Point->Y);
			}
			if (UniqueNodes.Num() < 3) continue;
			Footprint.Fields.Add(TEXT("osm_id"), LexToString(Id));
			Parsed.Add(MoveTemp(Footprint));
			Seen.Add(Id);
		}
		Footprints = MoveTemp(Parsed);
		return true;
	}
}

AOsmBuidingImporter::AOsmBuidingImporter()
{
	PrimaryActorTick.bCanEverTick = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

void AOsmBuidingImporter::ImportBuildings()
{
	if (bImportInProgress) return;
	bImportInProgress = true;
	ClearBuildings();
	const int32 Revision = CacheRevision;
	Async(EAsyncExecution::Thread, [StrongThis = TStrongObjectPtr<AOsmBuidingImporter>(this), Revision]()
	{
		const bool bSuccess = StrongThis->ImportBuildingData(true, Revision);
		UE_LOG(LogOsmBuidingImporter, Log, TEXT("Building import %s."), bSuccess ? TEXT("succeeded") : TEXT("failed"));
		AsyncTask(ENamedThreads::GameThread, [WeakThis = TWeakObjectPtr<AOsmBuidingImporter>(StrongThis.Get())]()
		{
			if (WeakThis.IsValid()) WeakThis->bImportInProgress = false;
		});
	});
}

void AOsmBuidingImporter::ClearBuildings()
{
	check(IsInGameThread());
	++CacheRevision;
	CachedBuildings.Reset();
	CachedDataKey.Reset();
	CachedBuildingCount = 0;
}

FString AOsmBuidingImporter::MakeCacheKey()
{
	check(IsInGameThread());
	FString Key;
	for (const TCHAR* Name : { TEXT("Source"), TEXT("LocalFile"), TEXT("OverpassQuery"), TEXT("OverpassShortQuery"),
		TEXT("BoundingMethod"), TEXT("BoundingActorSelection"), TEXT("BoundingZoneZoom"), TEXT("BoundingZoneMinX"),
		TEXT("BoundingZoneMaxX"), TEXT("BoundingZoneMinY"), TEXT("BoundingZoneMaxY"), TEXT("TerrainSelection"),
		TEXT("PointsOffset"), TEXT("bSkipCoordinateConversion"), TEXT("ScaleX"), TEXT("ScaleY") })
	{
		FString Value;
		if (const FProperty* Property = FindFProperty<FProperty>(GetClass(), Name))
			Property->ExportText_InContainer(0, Value, this, nullptr, this, PPF_None);
		Key += FString(Name) + TEXT("=") + Value + TEXT("\n");
	}
	if (Source == EOsmBuildingSource::LocalFile)
		Key += FString::Printf(TEXT("File=%lld/%lld"), IFileManager::Get().FileSize(*LocalFile),
		                       IFileManager::Get().GetTimeStamp(*LocalFile).GetTicks());
	TArray<AActor*> Targets = TerrainSelection.GetAllActors(GetWorld(), false);
	if (AActor* Bounds = BoundingActorSelection.GetActor(GetWorld(), false)) Targets.Add(Bounds);
	for (const AActor* Target : Targets)
	{
		if (!IsValid(Target)) continue;
		FVector Origin, Extent;
		Target->GetActorBounds(false, Origin, Extent);
		Key += Target->GetPathName() + Target->GetActorTransform().ToString() + Origin.ToString() + Extent.ToString();
	}
	if (const UGlobalCoordinates* Coordinates = ALevelCoordinates::GetGlobalCoordinates(GetWorld(), false))
		Key += Coordinates->CRS + FString::Printf(TEXT("/%.17g/%.17g/%.17g/%.17g"), Coordinates->WorldOriginLong,
			Coordinates->WorldOriginLat, Coordinates->CmPerLongUnit, Coordinates->CmPerLatUnit);
	Key += GetWorld()->OriginLocation.ToString();
	return Key;
}

bool AOsmBuidingImporter::GetCachedBuildingData(TArray<FImportedBuilding>& OutBuildings)
{
	check(IsInGameThread());
	OutBuildings.Reset();
	if (CachedBuildings.IsEmpty()) return false;
	if (CachedDataKey != MakeCacheKey())
	{
		ClearBuildings();
		return false;
	}
	OutBuildings = CachedBuildings;
	return true;
}

bool AOsmBuidingImporter::ConfigureForTiles(const int Zoom, const int MinX, const int MaxX, const int MinY, const int MaxY)
{
	BoundingMethod = EOsmBuildingBounds::TileNumbers;
	BoundingZoneZoom = Zoom;
	BoundingZoneMinX = MinX;
	BoundingZoneMaxX = MaxX;
	BoundingZoneMinY = MinY;
	BoundingZoneMaxY = MaxY;
	return true;
}

bool AOsmBuidingImporter::LoadSourceText(FString& OutText, const bool bIsUserInitiated) const
{
	if (Source == EOsmBuildingSource::LocalFile)
		return FFileHelper::LoadFileToString(OutText, *LocalFile);

	FString URL;
	if (Source == EOsmBuildingSource::OverpassQuery)
	{
		URL = TEXT("https://overpass-api.de/api/interpreter?data=") + OverpassQuery;
	}
	else
	{
		FVector4d Bounds;
		const bool bHasBounds = Concurrency::RunOnGameThreadAndWait([this, &Bounds]()
		{
			if (BoundingMethod == EOsmBuildingBounds::BoundingActor)
			{
				AActor* Actor = BoundingActorSelection.GetActor(GetWorld());
				if (!IsValid(Actor)) return false;
				if (ALandscape* Landscape = Cast<ALandscape>(Actor))
					return LandscapeUtils::GetLandscapeCRSBounds(Landscape, TEXT("EPSG:4326"), Bounds);
				FVector Origin, Extent;
				Actor->GetActorBounds(true, Origin, Extent);
				return ALevelCoordinates::GetCRSCoordinatesFromOriginExtent(GetWorld(), Origin, Extent, TEXT("EPSG:4326"), Bounds);
			}
			if (BoundingZoneZoom < 0 || BoundingZoneZoom > 22) return false;
			const double Count = static_cast<double>(1 << BoundingZoneZoom);
			if (BoundingZoneMinX < 0 || BoundingZoneMinY < 0 || BoundingZoneMinX > BoundingZoneMaxX ||
				BoundingZoneMinY > BoundingZoneMaxY || BoundingZoneMaxX >= Count || BoundingZoneMaxY >= Count) return false;
			const double West = BoundingZoneMinX / Count * 360.0 - 180.0;
			const double East = (BoundingZoneMaxX + 1) / Count * 360.0 - 180.0;
			const double North = FMath::RadiansToDegrees(
				FMath::Atan(FMath::Sinh(UE_PI * (1.0 - 2.0 * BoundingZoneMinY / Count))));
			const double South = FMath::RadiansToDegrees(
				FMath::Atan(FMath::Sinh(UE_PI * (1.0 - 2.0 * (BoundingZoneMaxY + 1) / Count))));
			Bounds = FVector4d(West, East, South, North);
			return true;
		});
		if (!bHasBounds)
		{
			UE_LOG(LogOsmBuidingImporter, Error, TEXT("Select a valid bounding actor or tile range."));
			return false;
		}
		const FString Selection = Source == EOsmBuildingSource::Buildings
			                          ? TEXT("way[\"building\"][\"building\"!=\"no\"];")
			                          : OverpassShortQuery;
		URL = Overpass::QueryFromShortQuery(
			TEXT("https://overpass-api.de/api/interpreter"), Bounds.Z, Bounds.X, Bounds.W, Bounds.Y, Selection);
	}
	GDALDataset* Dataset = GDALInterface::LoadGDALVectorDatasetFromQuery(URL, bIsUserInitiated);
	if (!Dataset) return false;
	const FString File = UTF8_TO_TCHAR(Dataset->GetDescription());
	GDALClose(Dataset);
	return FFileHelper::LoadFileToString(OutText, *File);
}

bool AOsmBuidingImporter::ImportBuildingData(const bool bIsUserInitiated, const int32 ExpectedRevision)
{
	if (IsInGameThread())
	{
		UE_LOG(LogOsmBuidingImporter, Error, TEXT("Use Import to load building data on a background thread."));
		return false;
	}
	FString ImportKey;
	int32 Revision = 0;
	if (!Concurrency::RunOnGameThreadAndWait([&]()
	{
		if (ExpectedRevision != INDEX_NONE && ExpectedRevision != CacheRevision) return false;
		if (ExpectedRevision == INDEX_NONE) ClearBuildings();
		ImportKey = MakeCacheKey();
		Revision = CacheRevision;
		return true;
	})) return false;
	TArray<OsmBuidingImporterInternal::FBuildingFootprint> Footprints;
	if (FString Text; !LoadSourceText(Text, bIsUserInitiated) || !
		OsmBuidingImporterInternal::ParseBuildingXml(Text, Footprints) || Footprints.IsEmpty())
	{
		UE_LOG(LogOsmBuidingImporter, Error,
		       TEXT(
			       "No building footprints could be read. Supply OSM XML with closed building ways and their referenced nodes."
		       ));
		return false;
	}
	return Concurrency::RunOnGameThreadAndWait([this, &Footprints, &ImportKey, Revision]()
	{
		if (CacheRevision != Revision || MakeCacheKey() != ImportKey) return false;
		TArray<FImportedBuilding> Buildings;
		if (!ProjectBuildings(Footprints, Buildings)) return false;
		CachedBuildings = MoveTemp(Buildings);
		CachedDataKey = ImportKey;
		CachedBuildingCount = CachedBuildings.Num();
		return true;
	});
}

bool AOsmBuidingImporter::ProjectBuildings(const TArray<OsmBuidingImporterInternal::FBuildingFootprint>& Footprints,
                                           TArray<FImportedBuilding>& OutBuildings) const
{
	check(IsInGameThread());
	OutBuildings.Reset();
	FCollisionQueryParams QueryParams;
	if (const TArray<AActor*> Targets = TerrainSelection.GetAllActors(GetWorld(), false); Targets.IsEmpty() || !
		LandscapeUtils::CustomCollisionQueryParams(Targets, QueryParams))
	{
		UE_LOG(LogOsmBuidingImporter, Error, TEXT("Select terrain collision actors before importing buildings."));
		return false;
	}
	const bool bUseLocalXY = Source == EOsmBuildingSource::LocalFile && bSkipCoordinateConversion;
	UGlobalCoordinates* Coordinates = bUseLocalXY ? nullptr : ALevelCoordinates::GetGlobalCoordinates(GetWorld(), true);
	OGRCoordinateTransformation* Transform = Coordinates ? Coordinates->GetCRSTransformer(TEXT("EPSG:4326")) : nullptr;
	if (!bUseLocalXY && !Transform) return false;

	// Keep complete rings only: query results can include buildings that cross the
	// terrain boundary. One such building must not block all the valid buildings.

	int32 MissingTerrainCount = 0;
	for (int32 FootprintIndex = 0; FootprintIndex < Footprints.Num(); ++FootprintIndex)
	{
		const auto& Footprint = Footprints[FootprintIndex];
		TArray<FVector> Points;
		bool bMissingTerrain = false;
		for (int32 Index = 0; Index + 1 < Footprint.Points.Num(); ++Index)
		{
			const FVector2D Point = Footprint.Points[Index];
			FVector2D XY(Point.X * ScaleX, Point.Y * ScaleY);
			double Z = 0;
			if ((!bUseLocalXY && !Coordinates->GetUnrealCoordinatesFromCRS(Point.X, Point.Y, Transform, XY)) ||
				!FMath::IsFinite(XY.X) || !FMath::IsFinite(XY.Y))
			{
				if (Transform) OGRCoordinateTransformation::DestroyCT(Transform);
				UE_LOG(LogOsmBuidingImporter, Error,
					TEXT("Coordinate conversion failed for building %s corner %d (lon=%.9f lat=%.9f). No cache was published."),
					*Footprint.Fields.FindRef(TEXT("osm_id")), Index, Point.X, Point.Y);
				return false;
			}
			if (!LandscapeUtils::GetZ(GetWorld(), QueryParams, XY.X, XY.Y, Z, bDebugLineTraces))
			{
				UE_LOG(LogOsmBuidingImporter, Warning,
					TEXT("Skipping building %s: no selected-terrain collision at corner %d (lon=%.9f lat=%.9f, world X=%.3f Y=%.3f)."),
					*Footprint.Fields.FindRef(TEXT("osm_id")), Index, Point.X, Point.Y, XY.X, XY.Y);
				bMissingTerrain = true;
				++MissingTerrainCount;
				break;
			}
			Points.Add(FVector(XY.X, XY.Y, Z) + PointsOffset);
		}
		if (bMissingTerrain) continue;
		FImportedBuilding Building;
		Building.OsmId = Footprint.Fields.FindRef(TEXT("osm_id"));
		Building.Points = MoveTemp(Points);
		Building.Tags = Footprint.Fields;
		OutBuildings.Add(MoveTemp(Building));
	}
	if (Transform) OGRCoordinateTransformation::DestroyCT(Transform);
	if (OutBuildings.IsEmpty())
	{
		UE_LOG(LogOsmBuidingImporter, Error,
			TEXT(
				"Could not place any complete building footprints on the selected terrain (%d checked). No cache was published."
			), Footprints.Num());
		return false;
	}
	UE_LOG(LogOsmBuidingImporter, Log,
	       TEXT("Imported %d of %d building footprints; skipped %d without complete terrain coverage."),
		OutBuildings.Num(), Footprints.Num(), MissingTerrainCount);
	return true;
}
