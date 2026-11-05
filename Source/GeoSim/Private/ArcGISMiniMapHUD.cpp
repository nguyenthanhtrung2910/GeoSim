#include "ArcGISMiniMapHUD.h"

#include "ArcGISMiniMapWidget.h"
#include "Coordinates/GlobalCoordinates.h"
#include "Coordinates/LevelCoordinates.h"
#include "GDALInterface/GDALInterface.h"

#include "Blueprint/GameViewportSubsystem.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HttpModule.h"
#include "ImageUtils.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Kismet/GameplayStatics.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogArcGISMiniMapHUD, Log, All);

AArcGisMiniMapHUD::AArcGisMiniMapHUD()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.1f;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
#if WITH_EDITOR
	SetIsSpatiallyLoaded(false);
#endif
}

void AArcGisMiniMapHUD::BeginPlay()
{
	Super::BeginPlay();
	if (EnsureWidget())
	{
		RefreshMap();
	}
}

bool AArcGisMiniMapHUD::EnsureWidget()
{
	if (IsValid(MiniMapWidget))
	{
		return true;
	}
	APlayerController* Controller = UGameplayStatics::GetPlayerController(this, LocalPlayerIndex);
	if (!IsValid(Controller) || !Controller->IsLocalController())
	{
		return false;
	}
	LocalController = Controller;
	MiniMapWidget = CreateWidget<UArcGisMiniMapWidget>(Controller, UArcGisMiniMapWidget::StaticClass());
	if (!MiniMapWidget)
	{
		return false;
	}
	MiniMapWidget->Configure(MapSize, MapAttribution);
	if (!MiniMapWidget->AddToPlayerScreen(10))
	{
		UE_LOG(LogArcGISMiniMapHUD, Error, TEXT("%s: could not add the HUD to the local player's screen."), *GetName());
		MiniMapWidget = nullptr;
		return false;
	}
	UpdateWidgetLayout();
	UE_LOG(LogArcGISMiniMapHUD, Display, TEXT("%s: added mini map HUD to the local player's screen."), *GetName());
	return true;
}

void AArcGisMiniMapHUD::UpdateWidgetLayout() const
{
	if (!MiniMapWidget)
	{
		return;
	}
	MiniMapWidget->Configure(MapSize, MapAttribution);
	if (UGameViewportSubsystem* Viewport = UGameViewportSubsystem::Get())
	{
		// Position/size convenience setters reset anchors. Set the complete slot in one operation.
		FGameViewportWidgetSlot Slot = Viewport->GetWidgetSlot(MiniMapWidget);
		const float Size = FMath::Clamp(MapSize, 100.0f, 1000.0f);
		Slot.Anchors = FAnchors(1.0f, 0.0f);
		Slot.Alignment = FVector2D(1.0, 0.0);
		Slot.Offsets = FMargin(-FMath::Max(0.0, ScreenMargin.X), FMath::Max(0.0, ScreenMargin.Y), Size, Size + 80.0f);
		Viewport->SetWidgetSlot(MiniMapWidget, Slot);
	}
}

bool AArcGisMiniMapHUD::PrepareCoordinates()
{
	Coordinates.Reset();
	if (CoordinateTransform)
	{
		OGRCoordinateTransformation::DestroyCT(CoordinateTransform);
		CoordinateTransform = nullptr;
	}
	if (LevelCoordinatesActor)
	{
		if (!IsValid(LevelCoordinatesActor) || LevelCoordinatesActor->GetWorld() != GetWorld())
		{
			ReportMapError(TEXT("The assigned LevelCoordinates Actor is not valid in this play world."));
			return false;
		}
		Coordinates = LevelCoordinatesActor->GlobalCoordinates;
	}
	else
	{
		Coordinates = ALevelCoordinates::GetGlobalCoordinates(GetWorld(), false);
		if (!Coordinates.IsValid())
		{
			// A coordinate actor carries configuration and does not need to be visible to provide it.
			TArray<ALevelCoordinates*> Candidates;
			for (TActorIterator<ALevelCoordinates> It(GetWorld()); It; ++It)
			{
				if (IsValid(*It))
				{
					Candidates.Add(*It);
				}
			}
			if (Candidates.IsEmpty())
			{
				ReportMapError(TEXT("No LevelCoordinates actor is loaded in this play world. Check its runtime level/data layer and, for World Partition, disable Is Spatially Loaded on the coordinate actor."));
				return false;
			}
			if (Candidates.Num() > 1)
			{
				ReportMapError(FString::Printf(TEXT("Found %d LevelCoordinates actors without a unique visible choice. Assign Level Coordinates Actor on the HUD explicitly."), Candidates.Num()));
				return false;
			}
			Coordinates = Candidates[0]->GlobalCoordinates;
		}
	}
	if (!Coordinates.IsValid())
	{
		ReportMapError(TEXT("The selected LevelCoordinates actor has no valid Global Coordinates component."));
		return false;
	}
	if (Coordinates->CRS.TrimStartAndEnd().IsEmpty() ||
		!FMath::IsFinite(Coordinates->CmPerLongUnit) || !FMath::IsFinite(Coordinates->CmPerLatUnit) ||
		!FMath::IsFinite(Coordinates->WorldOriginLong) || !FMath::IsFinite(Coordinates->WorldOriginLat) ||
		FMath::IsNearlyZero(Coordinates->CmPerLongUnit) || FMath::IsNearlyZero(Coordinates->CmPerLatUnit))
	{
		ReportMapError(FString::Printf(TEXT("LevelCoordinates '%s' has invalid settings: CRS='%s', scale=(%g, %g), origin=(%g, %g)."),
			*GetNameSafe(Coordinates->GetOwner()), *Coordinates->CRS, Coordinates->CmPerLongUnit, Coordinates->CmPerLatUnit,
			Coordinates->WorldOriginLong, Coordinates->WorldOriginLat));
		return false;
	}
	CoordinateTransform = GDALInterface::MakeTransform(Coordinates->CRS, TEXT("EPSG:3857"));
	if (!CoordinateTransform)
	{
		ReportMapError(TEXT("Could not create the LevelCoordinates to EPSG:3857 transformation."));
		return false;
	}
	UE_LOG(LogArcGISMiniMapHUD, Display, TEXT("%s: using coordinate actor '%s' with CRS '%s'."),
		*GetName(), *GetNameSafe(Coordinates->GetOwner()), *Coordinates->CRS);
	return true;
}

bool AArcGisMiniMapHUD::WorldToMap(const FVector& WorldPosition, FVector2D& OutPosition) const
{
	if (!Coordinates.IsValid() || !CoordinateTransform)
	{
		return false;
	}
	Coordinates->GetCRSCoordinatesFromUnrealLocation(FVector2D(WorldPosition.X, WorldPosition.Y), OutPosition);
	// Use the cached GDAL transform directly to avoid recreating it or showing repeated dialogs during marker updates.
	return CoordinateTransform->Transform(1, &OutPosition.X, &OutPosition.Y) &&
		FMath::IsFinite(OutPosition.X) && FMath::IsFinite(OutPosition.Y);
}

AActor* AArcGisMiniMapHUD::GetMarkerActor() const
{
	if (IsValid(TrackedActor))
	{
		return TrackedActor;
	}
	return LocalController.IsValid() ? LocalController->GetPawn() : nullptr;
}

bool AArcGisMiniMapHUD::GetRequestedExtent(FVector2D& OutMin, FVector2D& OutMax) const
{
	if (IsValid(TargetLandscape))
	{
		FBox Bounds(ForceInit);
		if (ULandscapeInfo* Info = TargetLandscape->GetLandscapeInfo())
		{
#if WITH_EDITOR
			Bounds = Info->GetCompleteBounds();
#else
			Bounds = Info->GetLoadedBounds();
#endif
		}
		if (!Bounds.IsValid)
		{
			FVector Origin, Extent;
			TargetLandscape->GetActorBounds(false, Origin, Extent);
			Bounds = FBox(Origin - Extent, Origin + Extent);
		}
		if (Bounds.GetSize().X <= 0.0 || Bounds.GetSize().Y <= 0.0)
		{
			return false;
		}
		OutMin = FVector2D(TNumericLimits<double>::Max());
		OutMax = FVector2D(-TNumericLimits<double>::Max());
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			const FVector Position((Corner & 1) ? Bounds.Max.X : Bounds.Min.X,
				(Corner & 2) ? Bounds.Max.Y : Bounds.Min.Y, 0.0);
			FVector2D Projected;
			if (!WorldToMap(Position, Projected))
			{
				return false;
			}
			OutMin.X = FMath::Min(OutMin.X, Projected.X);
			OutMin.Y = FMath::Min(OutMin.Y, Projected.Y);
			OutMax.X = FMath::Max(OutMax.X, Projected.X);
			OutMax.Y = FMath::Max(OutMax.Y, Projected.Y);
		}
		const FVector2D Center = (OutMin + OutMax) * 0.5;
		const FVector2D Size = OutMax - OutMin;
		const double HalfWidth = FMath::Max(Size.X, Size.Y) * (0.5 + FMath::Clamp(LandscapePadding, 0.0, 1.0));
		OutMin = Center - FVector2D(HalfWidth);
		OutMax = Center + FVector2D(HalfWidth);
	}
	else
	{
		AActor* MarkerActor = GetMarkerActor();
		FVector2D Center;
		if (!IsValid(MarkerActor) || !WorldToMap(MarkerActor->GetActorLocation(), Center))
		{
			return false;
		}
		const double HalfWidth = FMath::Max(10.0, FallbackAreaWidthMeters) * 0.5;
		OutMin = Center - FVector2D(HalfWidth);
		OutMax = Center + FVector2D(HalfWidth);
	}
	return OutMax.X > OutMin.X && OutMax.Y > OutMin.Y;
}

void AArcGisMiniMapHUD::RefreshMap()
{
	if (!HasActorBegunPlay() || !EnsureWidget())
	{
		return;
	}
	if (!IsValid(TargetLandscape) && !IsValid(GetMarkerActor()))
	{
		bInitialMapRequested = false;
		MiniMapWidget->SetStatus(FText::FromString(TEXT("Waiting for player...")));
		return;
	}
	bInitialMapRequested = true;
	UpdateWidgetLayout();
	CancelRequests();
	const int32 Serial = ++RequestSerial;
	if (!PrepareCoordinates())
	{
		return;
	}
	FVector2D RequestedMin, RequestedMax;
	if (!GetRequestedExtent(RequestedMin, RequestedMax))
	{
		ReportMapError(TEXT("Unable to determine map area. Select a loaded landscape or a valid tracked actor/pawn."));
		return;
	}
	FString ServiceUrl = MapServiceUrl.TrimStartAndEnd();
	while (ServiceUrl.RemoveFromEnd(TEXT("/"))) {}
	if (ServiceUrl.IsEmpty())
	{
		ReportMapError(TEXT("Map Service URL is empty."));
		return;
	}
	const int32 Resolution = FMath::Clamp(ImageResolution, 64, 2048);
	FString Url = FString::Printf(TEXT("%s/export?f=json&bbox=%.8f,%.8f,%.8f,%.8f&bboxSR=3857&imageSR=3857&size=%d,%d&format=png32&transparent=false"),
		*ServiceUrl, RequestedMin.X, RequestedMin.Y, RequestedMax.X, RequestedMax.Y, Resolution, Resolution);
	if (!ServiceToken.IsEmpty())
	{
		Url += TEXT("&token=") + FGenericPlatformHttp::UrlEncode(ServiceToken);
	}
	MiniMapWidget->SetStatus(FText::FromString(TEXT("Loading map...")));
	ExportRequest = FHttpModule::Get().CreateRequest();
	ExportRequest->SetURL(Url);
	ExportRequest->SetVerb(TEXT("GET"));
	ExportRequest->SetTimeout(30.0f);
	ExportRequest->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
	ExportRequest->OnProcessRequestComplete().BindWeakLambda(this,
		[this, Serial](FHttpRequestPtr Request, const FHttpResponsePtr& Response, const bool bSucceeded)
		{
			if (Serial != RequestSerial) return;
			ExportRequest.Reset();
			if (!bSucceeded || !Response.IsValid() || Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
			{
				ReportMapError(TEXT("ArcGIS export request failed."));
				return;
			}
			TSharedPtr<FJsonObject> Json;
			if (const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString()); !FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
			{
				ReportMapError(TEXT("ArcGIS export returned invalid JSON."));
				return;
			}
			const TSharedPtr<FJsonObject>* Error = nullptr;
			if (Json->TryGetObjectField(TEXT("error"), Error))
			{
				FString Message;
				(*Error)->TryGetStringField(TEXT("message"), Message);
				ReportMapError(TEXT("ArcGIS export error: ") + Message);
				return;
			}
			FString ImageUrl;
			const TSharedPtr<FJsonObject>* Extent = nullptr;
			FVector2D ActualMin, ActualMax;
			if (!Json->TryGetStringField(TEXT("href"), ImageUrl) || !ImageUrl.StartsWith(TEXT("https://")) ||
				!Json->TryGetObjectField(TEXT("extent"), Extent) ||
				!(*Extent)->TryGetNumberField(TEXT("xmin"), ActualMin.X) || !(*Extent)->TryGetNumberField(TEXT("ymin"), ActualMin.Y) ||
				!(*Extent)->TryGetNumberField(TEXT("xmax"), ActualMax.X) || !(*Extent)->TryGetNumberField(TEXT("ymax"), ActualMax.Y) ||
				!FMath::IsFinite(ActualMin.X) || !FMath::IsFinite(ActualMin.Y) || !FMath::IsFinite(ActualMax.X) || !FMath::IsFinite(ActualMax.Y) ||
				ActualMax.X <= ActualMin.X || ActualMax.Y <= ActualMin.Y)
			{
				ReportMapError(TEXT("ArcGIS export returned an invalid image URL or extent."));
				return;
			}
			const TSharedPtr<FJsonObject>* SpatialReference = nullptr;
			int32 Wkid = 0;
			if (!(*Extent)->TryGetObjectField(TEXT("spatialReference"), SpatialReference) ||
				!(*SpatialReference)->TryGetNumberField(TEXT("wkid"), Wkid) ||
				(Wkid != 3857 && Wkid != 102100 && Wkid != 102113))
			{
				ReportMapError(TEXT("ArcGIS export did not return an EPSG:3857 image extent."));
				return;
			}
			ImageRequest = FHttpModule::Get().CreateRequest();
			ImageRequest->SetURL(ImageUrl);
			ImageRequest->SetVerb(TEXT("GET"));
			ImageRequest->SetTimeout(30.0f);
			ImageRequest->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
			ImageRequest->OnProcessRequestComplete().BindWeakLambda(this,
				[this, Serial, ActualMin, ActualMax](FHttpRequestPtr ImageHttpRequest, const FHttpResponsePtr& ImageResponse, const bool bImageSucceeded)
				{
					if (Serial != RequestSerial) return;
					ImageRequest.Reset();
					if (!bImageSucceeded || !ImageResponse.IsValid() || ImageResponse->GetResponseCode() < 200 || ImageResponse->GetResponseCode() >= 300)
					{
						ReportMapError(TEXT("Could not download the ArcGIS map image."));
						return;
					}
					UTexture2D* Texture = FImageUtils::ImportBufferAsTexture2D(ImageResponse->GetContent());
					if (!Texture)
					{
						ReportMapError(TEXT("Could not decode the ArcGIS map image."));
						return;
					}
					MapTexture = Texture;
					MapMin = ActualMin;
					MapMax = ActualMax;
					MiniMapWidget->SetMapTexture(MapTexture);
					UE_LOG(LogArcGISMiniMapHUD, Display, TEXT("%s: loaded ArcGIS HUD map (%dx%d)."), *GetName(), Texture->GetSizeX(), Texture->GetSizeY());
				});
			if (!ImageRequest->ProcessRequest())
			{
				ImageRequest.Reset();
				ReportMapError(TEXT("Could not start the map image download."));
			}
		});
	if (!ExportRequest->ProcessRequest())
	{
		ExportRequest.Reset();
		ReportMapError(TEXT("Could not start the ArcGIS export request."));
	}
}

void AArcGisMiniMapHUD::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!IsValid(MiniMapWidget))
	{
		if (EnsureWidget()) RefreshMap();
		return;
	}
	if (!bInitialMapRequested)
	{
		RefreshMap();
		return;
	}
	const AActor* MarkerActor = GetMarkerActor();
	FVector2D Position;
	if (!MapTexture || !IsValid(MarkerActor) || !WorldToMap(MarkerActor->GetActorLocation(), Position))
	{
		MiniMapWidget->SetPlayerPosition(FVector2D::ZeroVector, false);
		return;
	}
	const FVector2D UV((Position.X - MapMin.X) / (MapMax.X - MapMin.X),
		(MapMax.Y - Position.Y) / (MapMax.Y - MapMin.Y));
	const bool bInside = UV.X >= 0.0 && UV.X <= 1.0 && UV.Y >= 0.0 && UV.Y <= 1.0;
	MiniMapWidget->SetPlayerPosition(UV, bInside);
}

void AArcGisMiniMapHUD::ReportMapError(const FString& Message) const
{
	UE_LOG(LogArcGISMiniMapHUD, Error, TEXT("%s: %s"), *GetName(), *Message);
	if (MiniMapWidget)
	{
		MiniMapWidget->SetStatus(FText::FromString(TEXT("Map unavailable")));
	}
}

void AArcGisMiniMapHUD::CancelRequests()
{
	for (const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>& Request : {ExportRequest, ImageRequest})
	{
		if (Request.IsValid())
		{
			Request->OnProcessRequestComplete().Unbind();
			Request->CancelRequest();
		}
	}
	ExportRequest.Reset();
	ImageRequest.Reset();
}

void AArcGisMiniMapHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	++RequestSerial;
	CancelRequests();
	if (MiniMapWidget)
	{
		MiniMapWidget->RemoveFromParent();
		MiniMapWidget = nullptr;
	}
	MapTexture = nullptr;
	if (CoordinateTransform)
	{
		OGRCoordinateTransformation::DestroyCT(CoordinateTransform);
		CoordinateTransform = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}
