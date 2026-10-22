#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArcGISMiniMapHUD.generated.h"

class ALandscapeProxy;
class ALevelCoordinates;
class APlayerController;
class UArcGisMiniMapWidget;
class UGlobalCoordinates;
class UTexture2D;
class IHttpRequest;
class OGRCoordinateTransformation;

/** Downloads a fixed ArcGIS map image and displays a locally updated player marker in UMG. */
UCLASS()
class GEOSIM_API AArcGisMiniMapHUD : public AActor
{
	GENERATED_BODY()

public:
	AArcGisMiniMapHUD();
	virtual void Tick(float DeltaSeconds) override;

	/** Fit the map to this landscape, including its streaming proxies. Unset uses the tracked actor as map center. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Mini Map|Area")
	TObjectPtr<ALandscapeProxy> TargetLandscape;

	/** Optional explicit coordinate actor. Unset prefers the plugin's visible actor, then a single loaded actor, including hidden actors. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Mini Map|Coordinates")
	TObjectPtr<ALevelCoordinates> LevelCoordinatesActor;

	/** Optional marker actor. Unset tracks the local player's possessed pawn. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Mini Map|Area")
	TObjectPtr<AActor> TrackedActor;

	/** Width in Web Mercator meters when no landscape is assigned. The map stays fixed until Refresh Map. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mini Map|Area", meta = (ClampMin = "10"))
	double FallbackAreaWidthMeters = 5000.0;

	/** Extra margin around the landscape, as a fraction of its map width. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mini Map|Area", meta = (ClampMin = "0", ClampMax = "1"))
	double LandscapePadding = 0.05;

	/** ArcGIS MapServer URL, without /export or query parameters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mini Map|Service")
	FString MapServiceUrl = TEXT("https://services.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer");

	/** Optional token for a service requiring authentication. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mini Map|Service", meta = (PasswordField = true))
	FString ServiceToken;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mini Map|Service", meta = (ClampMin = "64", ClampMax = "2048"))
	int32 ImageResolution = 512;

	/** Attribution for the configured service, shown below the image. Update when changing map services. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mini Map|Service", meta = (MultiLine = true))
	FText MapAttribution = FText::FromString(TEXT("Sources: Esri, HERE, Garmin, USGS, Intermap, INCREMENT P, NRCan, Esri Japan, METI, Esri China (Hong Kong), Esri Korea, Esri (Thailand), NGCC, (c) OpenStreetMap contributors, and the GIS User Community"));

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mini Map|HUD", meta = (ClampMin = "100", ClampMax = "1000"))
	float MapSize = 320.0f;

	/** Distance from the top-right corner, in UMG layout units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mini Map|HUD")
	FVector2D ScreenMargin = FVector2D(24.0, 24.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mini Map|HUD", meta = (ClampMin = "0"))
	int32 LocalPlayerIndex = 0;

	/** Download a fresh image for the current area. Intended for use during play. */
	UFUNCTION(BlueprintCallable, Category = "Mini Map")
	void RefreshMap();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	bool EnsureWidget();
	void UpdateWidgetLayout() const;
	bool PrepareCoordinates();
	bool WorldToMap(const FVector& WorldPosition, FVector2D& OutPosition) const;
	bool GetRequestedExtent(FVector2D& OutMin, FVector2D& OutMax) const;
	AActor* GetMarkerActor() const;
	void CancelRequests();
	void ReportMapError(const FString& Message) const;

	UPROPERTY(Transient)
	TObjectPtr<UArcGisMiniMapWidget> MiniMapWidget;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> MapTexture;

	UPROPERTY(Transient)
	TWeakObjectPtr<APlayerController> LocalController;

	UPROPERTY(Transient)
	TWeakObjectPtr<UGlobalCoordinates> Coordinates;

	OGRCoordinateTransformation* CoordinateTransform = nullptr;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ExportRequest;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ImageRequest;
	FVector2D MapMin = FVector2D::ZeroVector;
	FVector2D MapMax = FVector2D::ZeroVector;
	int32 RequestSerial = 0;
	bool bInitialMapRequested = false;
};
