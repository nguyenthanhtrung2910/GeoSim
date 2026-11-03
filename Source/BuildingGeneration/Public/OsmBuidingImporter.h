#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LCCommon/ActorSelection.h"
#include "ImportedBuildingData.h"
#include "OsmBuidingImporter.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOsmBuidingImporter, Log, All);

namespace OsmBuidingImporterInternal
{
	struct FBuildingFootprint
	{
		TArray<FVector2D> Points;
		TMap<FString, FString> Fields;
	};

	bool ParseBuildingXml(const FString& Text, TArray<FBuildingFootprint>& Footprints);
}

UENUM(BlueprintType)
enum class EOsmBuildingSource : uint8
{
	LocalFile,
	Buildings,
	OverpassShortQuery,
	OverpassQuery
};

UENUM(BlueprintType)
enum class EOsmBuildingBounds : uint8
{
	BoundingActor,
	TileNumbers
};

/** Imports OSM building ways directly, without inheriting the plugin's spline importer. */
UCLASS(Blueprintable, meta = (DisplayName = "OSM Building Importer"))
class BUILDINGGENERATION_API AOsmBuidingImporter : public AActor
{
	GENERATED_BODY()

public:
	AOsmBuidingImporter();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Source")
	EOsmBuildingSource Source = EOsmBuildingSource::LocalFile;

	/** Raw OSM XML (.osm or .xml), including the nodes referenced by building ways. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Source", meta = (EditCondition = "Source == EOsmBuildingSource::LocalFile", EditConditionHides))
	FString LocalFile;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Source", meta = (EditCondition = "Source == EOsmBuildingSource::OverpassQuery", EditConditionHides, MultiLine = "true"))
	FString OverpassQuery;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Source", meta = (EditCondition = "Source == EOsmBuildingSource::OverpassShortQuery", EditConditionHides, MultiLine = "true"))
	FString OverpassShortQuery = TEXT("way[\"building\"][\"building\"!=\"no\"];");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Bounds", meta = (EditCondition = "Source == EOsmBuildingSource::Buildings || Source == EOsmBuildingSource::OverpassShortQuery", EditConditionHides))
	EOsmBuildingBounds BoundingMethod = EOsmBuildingBounds::BoundingActor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Bounds", meta = (EditCondition = "BoundingMethod == EOsmBuildingBounds::BoundingActor && (Source == EOsmBuildingSource::Buildings || Source == EOsmBuildingSource::OverpassShortQuery)", EditConditionHides))
	FActorSelection BoundingActorSelection;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Bounds", meta = (ClampMin = "0", ClampMax = "22", EditCondition = "BoundingMethod == EOsmBuildingBounds::TileNumbers", EditConditionHides))
	int32 BoundingZoneZoom = 14;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Bounds", meta = (ClampMin = "0", EditCondition = "BoundingMethod == EOsmBuildingBounds::TileNumbers", EditConditionHides))
	int32 BoundingZoneMinX = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Bounds", meta = (ClampMin = "0", EditCondition = "BoundingMethod == EOsmBuildingBounds::TileNumbers", EditConditionHides))
	int32 BoundingZoneMaxX = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Bounds", meta = (ClampMin = "0", EditCondition = "BoundingMethod == EOsmBuildingBounds::TileNumbers", EditConditionHides))
	int32 BoundingZoneMinY = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Bounds", meta = (ClampMin = "0", EditCondition = "BoundingMethod == EOsmBuildingBounds::TileNumbers", EditConditionHides))
	int32 BoundingZoneMaxY = 0;

	/** Collision actors that supply each footprint corner's elevation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Placement")
	FActorSelection TerrainSelection;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Placement", meta = (Units = "cm"))
	FVector PointsOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Placement", AdvancedDisplay)
	bool bDebugLineTraces = false;

	/** Local-file coordinates can instead be interpreted as scaled Unreal X/Y. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Placement", AdvancedDisplay, meta = (EditCondition = "Source == EOsmBuildingSource::LocalFile", EditConditionHides))
	bool bSkipCoordinateConversion = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Placement", AdvancedDisplay, meta = (EditCondition = "Source == EOsmBuildingSource::LocalFile && bSkipCoordinateConversion", EditConditionHides))
	double ScaleX = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSM Placement", AdvancedDisplay, meta = (EditCondition = "Source == EOsmBuildingSource::LocalFile && bSkipCoordinateConversion", EditConditionHides))
	double ScaleY = 1;

	UPROPERTY(VisibleInstanceOnly, Transient, Category = "OSM Cache")
	int32 CachedBuildingCount = 0;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "OSM Buildings", meta = (DisplayName = "Import"))
	void ImportBuildings();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "OSM Buildings", meta = (DisplayName = "Clear"))
	void ClearBuildings();

	/** Game thread only. Copies the current cache; never downloads or imports. */
	UFUNCTION(BlueprintCallable, Category = "OSM Cache")
	bool GetCachedBuildingData(TArray<FImportedBuilding>& OutBuildings);

	bool ConfigureForTiles(int Zoom, int MinX, int MaxX, int MinY, int MaxY);

private:
	friend class FOsmBuildingImporterLocalFileTest;
	bool bImportInProgress = false;
	int32 CacheRevision = 0;
	FString CachedDataKey;
	TArray<FImportedBuilding> CachedBuildings;
	FString MakeCacheKey();
	bool ImportBuildingData(bool bIsUserInitiated, int32 ExpectedRevision = INDEX_NONE);
	bool LoadSourceText(FString& OutText, bool bIsUserInitiated) const;
	bool ProjectBuildings(const TArray<OsmBuidingImporterInternal::FBuildingFootprint>& Footprints, TArray<FImportedBuilding>& OutBuildings) const;
};
