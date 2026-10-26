#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeCounter.h"
#include "LCCommon/ActorSelection.h"
#include "ImportedRoadData.h"
#include "OSMRoadImporter.generated.h"

namespace OsmRoadImporterInternal
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

	bool ParseRoadJson(const FString& Text, TMap<int64, FSourceRoadNode>& Nodes, TArray<FSourceRoadWay>& Ways);
}

UENUM(BlueprintType)
enum class EOsmRoadSource : uint8
{
    Roads,
    OverpassShortQuery,
    OverpassQuery,
    LocalFile
};

UENUM(BlueprintType)
enum class EOsmRoadBounds : uint8
{
    BoundingActor,
    TileNumbers
};

/** Reads OSM highway ways into a shared memory cache without creating actors, splines or meshes. */
UCLASS()
class ROADGENERATION_API AOsmRoadImporter : public AActor
{
    GENERATED_BODY()

public:
    AOsmRoadImporter();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Source")
    EOsmRoadSource Source = EOsmRoadSource::Roads;

    /** Raw OSM XML (.osm) or Overpass JSON. The entire local file is read. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Source",
        meta = (EditCondition = "Source == EOsmRoadSource::LocalFile", EditConditionHides))
    FString LocalFile;

    /** Complete Overpass query body, including its own bounds and output statement. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Source",
        meta = (EditCondition = "Source == EOsmRoadSource::OverpassQuery", EditConditionHides, MultiLine = "true"))
    FString OverpassQuery;

    /** Way selection wrapped with the configured bounds and referenced-node output. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Source",
        meta = (EditCondition = "Source == EOsmRoadSource::OverpassShortQuery", EditConditionHides, MultiLine = "true"))
    FString OverpassShortQuery = TEXT("way[\"highway\"];");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Bounds",
        meta = (EditCondition = "Source == EOsmRoadSource::Roads || Source == EOsmRoadSource::OverpassShortQuery", EditConditionHides))
    EOsmRoadBounds BoundingMethod = EOsmRoadBounds::BoundingActor;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Bounds",
        meta = (EditCondition = "(Source == EOsmRoadSource::Roads || Source == EOsmRoadSource::OverpassShortQuery) && BoundingMethod == EOsmRoadBounds::BoundingActor", EditConditionHides))
    FActorSelection BoundingActorSelection;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Bounds",
        meta = (ClampMin = "0", ClampMax = "22", EditCondition = "(Source == EOsmRoadSource::Roads || Source == EOsmRoadSource::OverpassShortQuery) && BoundingMethod == EOsmRoadBounds::TileNumbers", EditConditionHides))
    int32 BoundingZoneZoom = 14;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Bounds",
        meta = (ClampMin = "0", EditCondition = "(Source == EOsmRoadSource::Roads || Source == EOsmRoadSource::OverpassShortQuery) && BoundingMethod == EOsmRoadBounds::TileNumbers", EditConditionHides))
    int32 BoundingZoneMinX = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Bounds",
        meta = (ClampMin = "0", EditCondition = "(Source == EOsmRoadSource::Roads || Source == EOsmRoadSource::OverpassShortQuery) && BoundingMethod == EOsmRoadBounds::TileNumbers", EditConditionHides))
    int32 BoundingZoneMaxX = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Bounds",
        meta = (ClampMin = "0", EditCondition = "(Source == EOsmRoadSource::Roads || Source == EOsmRoadSource::OverpassShortQuery) && BoundingMethod == EOsmRoadBounds::TileNumbers", EditConditionHides))
    int32 BoundingZoneMinY = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Bounds",
        meta = (ClampMin = "0", EditCondition = "(Source == EOsmRoadSource::Roads || Source == EOsmRoadSource::OverpassShortQuery) && BoundingMethod == EOsmRoadBounds::TileNumbers", EditConditionHides))
    int32 BoundingZoneMaxY = 0;

    /** Actors/Landscapes whose collision supplies imported point elevations. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Placement")
    FActorSelection TerrainSelection;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Placement", meta = (Units = "cm"))
    FVector PointsOffset = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Placement", AdvancedDisplay)
    bool bDebugLineTraces = false;

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "OSM Data", meta = (DisplayName = "Generate"))
    void GenerateData();

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "OSM Data", meta = (DisplayName = "Clear"))
    void ClearData();

    /** Background thread only. Copies valid cached data; never requests or imports. */
    bool GetCachedRoadData(TArray<FImportedRoadWay>& OutWays);

    bool ConfigureForTiles(int Zoom, int MinX, int MaxX, int MinY, int MaxY);

private:
    friend class FOsmRoadImporterCacheTest;

    bool bGenerationInProgress = false;
    FCriticalSection RoadDataCacheMutex;
    FThreadSafeCounter RoadDataCacheRevision;
    int32 CachedRoadDataRevision = -1;
    FString CachedRoadDataKey;
    TArray<FImportedRoadWay> CachedRoadWays;

    void ClearRoadDataCache();
    FString MakeRoadDataCacheKey();
    bool ImportRoadData(TArray<FImportedRoadWay>& OutWays, bool bIsUserInitiated);
    bool ImportRoadDataUncached(TArray<FImportedRoadWay>& OutWays, bool bIsUserInitiated) const;
    bool LoadSourceText(FString& OutText, bool bIsUserInitiated) const;
};
