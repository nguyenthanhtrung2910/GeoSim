// Copyright 2023-2025 LandscapeCombinator. All Rights Reserved.

#include "MapDownloader.h"
#include "LogLandscapeGeneration.h"
#include "ImageDownloader/HMDebugFetcher.h"

#include "ImageDownloader/Downloaders/HMLocalFile.h"
#include "ImageDownloader/Downloaders/HMLocalFolder.h"
#include "ImageDownloader/Downloaders/HMURL.h"

#include "ImageDownloader/Downloaders/HMLitto3DGuadeloupe.h"
#include "ImageDownloader/Downloaders/HMViewfinderDownloader.h"
#include "ImageDownloader/Downloaders/HMListDownloader.h"
#include "HMWorldElevation3D.h"
#include "ImageDownloader/Downloaders/HMXYZ.h"
#include "ImageDownloader/Downloaders/HMNapoli.h"
#include "ImageDownloader/Downloaders/HMWMS.h"

#include "ImageDownloader/Transformers/HMSwissALTI3DRenamer.h"
#include "ImageDownloader/Transformers/HMLitto3DGuadeloupeRenamer.h"
#include "ImageDownloader/Transformers/HMViewfinder15Renamer.h"

#include "ImageDownloader/Transformers/HMDegreeFilter.h"
#include "ImageDownloader/Transformers/HMDegreeRenamer.h"
#include "ImageDownloader/Transformers/HMPreprocess.h"
#include "ImageDownloader/Transformers/HMResolution.h"
#include "ImageDownloader/Transformers/HMReproject.h"
#include "ImageDownloader/Transformers/HMEnsureOneBand.h"
#include "ImageDownloader/Transformers/HMCrop.h"
#include "HMRescaleMap.h"
#include "ImageDownloader/Transformers/HMToPNG.h"
#include "ImageDownloader/Transformers/HMMerge.h"
#include "ImageDownloader/Transformers/HMReadCRS.h"
#include "ImageDownloader/Transformers/HMConvert.h"
#include "ImageDownloader/Transformers/HMAddMissingTiles.h"
#include "ImageDownloader/Transformers/HMFunction.h"

#include "LCCommon/LCSettings.h"
#include "LandscapeUtils/LandscapeUtils.h"
#include "ConcurrencyHelpers/LCReporter.h"

#include "Engine/World.h"
#include "Misc/MessageDialog.h"
#include "Logging/StructuredLog.h"
#include "Internationalization/TextLocalizationResource.h" 

#define LOCTEXT_NAMESPACE "FLandscapeGenerationModule"


UMapDownloader::UMapDownloader()
{
	PreprocessingTool = CreateDefaultSubobject<UExternalTool>(TEXT("Preprocessing Tool"));
}

bool UMapDownloader::ConfigureForTiles(const int Zoom, const int MinX, const int MaxX, const int MinY, const int MaxY)
{
	if (IsXYZ() || IsWorldElevation3D())
	{
		XYZ_MinX = MinX;
		XYZ_MaxX = MaxX;
		XYZ_MinY = MinY;
		XYZ_MaxY = MaxY;
		XYZ_Zoom = Zoom;
		ParametersSelection.ParametersSelectionMethod = EParametersSelectionMethod::Manual;
		return true;
	}
	else if (IsWms())
	{
		double MinLong3857, MaxLat3857, MaxLong3857, MinLat3857;
		GDALInterface::XYZTileToEPSG3857(MinX, MinY, Zoom, MinLong3857, MaxLat3857);
		GDALInterface::XYZTileToEPSG3857(MaxX + 1, MaxY + 1, Zoom, MaxLong3857, MinLat3857);

		FVector4d Coordinates3857(MinLong3857, MaxLong3857, MinLat3857, MaxLat3857);
		FVector4d CoordinatesWms;

		if (!GDALInterface::ConvertCoordinates(Coordinates3857, /*bCrop=*/false, CoordinatesWms, "EPSG:3857", WMS_CRS))
		{
			LCReporter::ShowError(
				FText::Format(
					NSLOCTEXT("FLandscapeGenerationModule", "ConfigureForTilesWMSConvertFailed",
						"Failed to convert tile coordinates from EPSG:3857 to the WMS layer's CRS ('{0}'). "
						"Please check that the layer's CRS is valid."
					),
					FText::FromString(WMS_CRS)
				)
			);
			return false;
		}

		WMS_MinLong = CoordinatesWms[0];
		WMS_MaxLong = CoordinatesWms[1];
		WMS_MinLat  = CoordinatesWms[2];
		WMS_MaxLat  = CoordinatesWms[3];

		ParametersSelection.ParametersSelectionMethod = EParametersSelectionMethod::Manual;
		return true;
	}
	else
	{
		const FString SourceName = StaticEnum<ELandscapeImageSourceKind>()->GetDisplayNameTextByValue(static_cast<int64>(ImageSourceKind)).ToString();

		LCReporter::ShowError(
			FText::Format(
				NSLOCTEXT("FLandscapeGenerationModule", "ConfigureForTilesNotSupported",
					"Position Based Generation requires an XYZ or WMS based image source, but the current source is '{0}'.\n"
					"Either disable Position Based Generation, or change the Image Source Kind."
				),
				FText::FromString(SourceName)
			)
		);

		return false;
	}
}

[[maybe_unused]] static FString RenameGenerationCrs(FString Crs)
{
	if (Crs == "IGNF:UTM20W84GUAD")
	{
		return "EPSG:4559";
	}
	else if (Crs == "IGNF:RGR92UTM40S")
	{
		return "EPSG:2975";
	}
	else
	{
		return Crs;
	}
}

TArray<FString> UMapDownloader::GetTitles()
{
	return WMS_Provider.Titles;
}

HMFetcher* UMapDownloader::CreateInitialFetcher(bool bIsUserInitiated, FString Name)
{
	switch (ImageSourceKind)
	{
		case ELandscapeImageSourceKind::LocalFile:
		{
			return new HMDebugFetcher("LocalFile", new HMLocalFile(LocalFilePath, CRS), true);
		}

		case ELandscapeImageSourceKind::LocalFolder:
		{
			return new HMDebugFetcher("LocalFolder", new HMLocalFolder(LocalFolderPath, CRS), true);
		}

		case ELandscapeImageSourceKind::URL:
		{
			return new HMDebugFetcher("URL", new HMURL(URL, Name + ".tif", CRS), true);
		}

		case ELandscapeImageSourceKind::Litto3D_Guadeloupe:
		{
			HMFetcher *Fetcher1 = new HMDebugFetcher("Litto3DGuadeloupe", new HMLitto3DGuadeloupe(Litto3D_Folder, bUse5mData, bSkipExtraction), true);
			HMFetcher *Fetcher2 = new HMDebugFetcher("Litto3DGuadeloupeRenamer", new HMLitto3DGuadeloupeRenamer(Name));
			return Fetcher1->AndThen(Fetcher2);
		}

		case ELandscapeImageSourceKind::Viewfinder15:
		{
			HMFetcher *Fetcher1 = new HMDebugFetcher("ViewfinderDownloader", new HMViewfinderDownloader(Viewfinder_TilesString, "http://www.viewfinderpanoramas.org/DEM/TIF15/", true), true);
			HMFetcher *Fetcher2 = new HMDebugFetcher("Viewfinder15Renamer", new HMViewfinder15Renamer(Name));
			return Fetcher1->AndThen(Fetcher2);
		}

		case ELandscapeImageSourceKind::Viewfinder3:
		{
			HMFetcher *Result = new HMDebugFetcher("ViewfinderDownloader", new HMViewfinderDownloader(Viewfinder_TilesString, "http://viewfinderpanoramas.org/dem3/", false), true);
			if (bFilterDegrees)
			{
				Result = Result->AndThen(new HMDebugFetcher("DegreeFilter", new HMDegreeFilter(Name, FilterMinLong, FilterMaxLong, FilterMinLat, FilterMaxLat)));
			}
			Result = Result->AndThen(new HMDebugFetcher("Convert", new HMConvert(Name, "tif")));
			Result = Result->AndThen(new HMDebugFetcher("DegreeRenamer", new HMDegreeRenamer(Name)));
			return Result;
		}

		case ELandscapeImageSourceKind::Viewfinder1:
		{
			HMFetcher *Result = new HMDebugFetcher("ViewfinderDownloader", new HMViewfinderDownloader(Viewfinder_TilesString, "http://viewfinderpanoramas.org/dem1/", false), true);
			if (bFilterDegrees)
			{
				Result = Result->AndThen(new HMDebugFetcher("DegreeFilter", new HMDegreeFilter(Name, FilterMinLong, FilterMaxLong, FilterMinLat, FilterMaxLat)));
			}
			Result = Result->AndThen(new HMDebugFetcher("Convert", new HMConvert(Name, "tif")));
			Result = Result->AndThen(new HMDebugFetcher("DegreeRenamer", new HMDegreeRenamer(Name)));
			return Result;
		}

		case ELandscapeImageSourceKind::SwissALTI_3D:
		{
			HMFetcher *Fetcher1 = new HMDebugFetcher("ListDownloader", new HMListDownloader(SwissALTI3D_ListOfLinks), true);
			HMFetcher *Fetcher2 = new HMDebugFetcher("ReadCRS", new HMReadCRS());
			HMFetcher *Fetcher3 = new HMDebugFetcher("SwissALTI3DRenamer", new HMSwissALTI3DRenamer(Name));
			return Fetcher1->AndThen(Fetcher2)->AndThen(Fetcher3);
		}

		case ELandscapeImageSourceKind::USGS_OneThird:
		{
			HMFetcher *Fetcher1 = new HMDebugFetcher("ListDownloader", new HMListDownloader(USGS_OneThird_ListOfLinks), true);
			HMFetcher *Fetcher2 = new HMDebugFetcher("ReadCRS", new HMReadCRS());
			HMFetcher *Fetcher3 = new HMDebugFetcher("DegreeRenamer", new HMDegreeRenamer(Name));
			return Fetcher1->AndThen(Fetcher2)->AndThen(Fetcher3);
		}

		case ELandscapeImageSourceKind::Napoli:
		{
			if (!SetSourceParametersBool(true))
			{
				return nullptr;
			}

			return new HMDebugFetcher("Napoli", new HMNapoli(Napoli_MinLong, Napoli_MaxLong, Napoli_MinLat, Napoli_MaxLat), true);
		}

		default:
		{
			if (IsWms())
			{
				if (!SetSourceParametersBool(true))
				{
					return nullptr;
				}

				if (bAutoSetResolution) AutoSetResolution();

				HMFetcher *WMSFetcher = new HMWMS(
					WMS_Provider, bEnableWMSParallelDownload,
					WMS_MaxTileWidth, WMS_MaxTileHeight,
					WMS_Name, WMS_CRS, WMS_X_IsLong,
					WMS_MinAllowedLong, WMS_MaxAllowedLong, WMS_MinAllowedLat, WMS_MaxAllowedLat,
					WMS_MinLong, WMS_MaxLong, WMS_MinLat, WMS_MaxLat,
					bWMSSingleTile, WMS_ResolutionPixelsPerUnit
				);

				if (!WMSFetcher) return nullptr;

				return new HMDebugFetcher("WMS_Download", WMSFetcher, true);
			}
			else if (IsWorldElevation3D())
			{
				if (XYZ_Zoom < 0 || XYZ_Zoom > 16)
				{
					LCReporter::ShowError(LOCTEXT("ArcGISZoom", "ArcGIS World Elevation 3D supports zoom levels 0 through 16."));
					return nullptr;
				}

				if (!SetSourceParametersBool(true))
				{
					return nullptr;
				}
				return new HMDebugFetcher("WorldElevation3D_Download", new HMWorldElevation3D(
					bAllowInvalidTiles, bEnableXYZParallelDownload, Name,
					XYZ_Zoom, XYZ_MinX, XYZ_MaxX, XYZ_MinY, XYZ_MaxY), true);
			}
			else if (IsXYZ())
			{
				if (!SetSourceParametersBool(true))
				{
					return nullptr;
				}

				FString Layer, Format;
				FString URL2;
				bool bGeoreferenceSlippyTiles2;
				bool bMaxY_IsNorth2;


				if (IsMapbox())
				{
					if (bIsUserInitiated && !ShowMapboxFreeTierWarning()) return nullptr;

					FString MapboxToken2 = GetMapboxToken();
					if (MapboxToken2.IsEmpty())
					{
						LCReporter::ShowError(
							LOCTEXT("MapboxTokenMissing", "Please add a Mapbox Token (can be obtained from a free Mapbox account) in your Editor Preferences or in the Details Panel.")
						); 
						return nullptr;
					}

					if (ImageSourceKind == ELandscapeImageSourceKind::Mapbox_Heightmaps)
					{
						Layer = "MapboxTerrainDEMV1";
						Format = "png";
						if (Mapbox_2x)
						{
							URL2 = FString("https://api.mapbox.com/v4/mapbox.mapbox-terrain-dem-v1/{z}/{x}/{y}@2x.pngraw?access_token=") + MapboxToken2;
						}
						else
						{
							URL2 = FString("https://api.mapbox.com/v4/mapbox.mapbox-terrain-dem-v1/{z}/{x}/{y}.pngraw?access_token=") + MapboxToken2;
						}

						bGeoreferenceSlippyTiles2 = true;
						bMaxY_IsNorth2 = false;
					}
					else if (ImageSourceKind == ELandscapeImageSourceKind::Mapbox_Satellite)
					{
						Layer = "MapboxSatellite";
						Format = "jpg";
						if (Mapbox_2x)
						{
							URL2 = FString("https://api.mapbox.com/v4/mapbox.satellite/{z}/{x}/{y}@2x.jpg90?access_token=") + MapboxToken2;
						}
						else
						{
							URL2 = FString("https://api.mapbox.com/v4/mapbox.satellite/{z}/{x}/{y}.jpg90?access_token=") + MapboxToken2;
						}

						bGeoreferenceSlippyTiles2 = true;
						bMaxY_IsNorth2 = false;
					}
					else
					{
						return nullptr;
					}
				}
				else if (IsMapTiler())
				{
					if (bIsUserInitiated && !ShowMapTilerFreeTierWarning()) return nullptr;

					FString MapTilerToken2 = GetMapTilerToken();
					if (MapTilerToken2.IsEmpty())
					{
						LCReporter::ShowError(
							LOCTEXT("MapTilerTokenMissing", "Please add a MapTiler Token (can be obtained from a free MapTiler account) in your Editor Preferences or in the Details Panel.")
						); 
						return nullptr;
					}

					if (ImageSourceKind == ELandscapeImageSourceKind::MapTiler_Heightmaps)
					{
						Layer = "MapTilerTerrainRGB";
						Format = "webp";
						URL2 = FString("https://api.maptiler.com/tiles/terrain-rgb-v2/{z}/{x}/{y}.webp?key=") + MapTilerToken2;

						bGeoreferenceSlippyTiles2 = true;
						bMaxY_IsNorth2 = false;
					}
					else if (ImageSourceKind == ELandscapeImageSourceKind::MapTiler_Satellite)
					{
						Layer = "MapTilerSatellite";
						Format = "jpg";
						URL2 = FString("https://api.maptiler.com/tiles/satellite-v2/{z}/{x}/{y}.jpg?key=") + MapTilerToken2;

						bGeoreferenceSlippyTiles2 = true;
						bMaxY_IsNorth2 = false;
					}
					else
					{
						return nullptr;
					}
				}
				else if (IsNextZen())
				{
					FString NextZenToken2 = GetNextZenToken();
					if (NextZenToken2.IsEmpty())
					{
						LCReporter::ShowError(
							LOCTEXT("NextZenTokenMissing", "Please add a NextZen Token (can be obtained from a free NextZen account) in your Editor Preferences or in the Details Panel.")
						); 
						return nullptr;
					}

					if (ImageSourceKind == ELandscapeImageSourceKind::NextZen_Heightmaps)
					{
						Layer = "NextZenTerrainRGB";
						Format = "png";
						URL2 = FString("https://tile.nextzen.org/tilezen/terrain/v1/256/terrarium/{z}/{x}/{y}.png?api_key=") + NextZenToken2;

						bGeoreferenceSlippyTiles2 = true;
						bMaxY_IsNorth2 = false;
					}
					else
					{
						return nullptr;
					}
				}
				else
				{
					Layer = XYZ_Name;
					Format = XYZ_Format;
					URL2 = XYZ_URL;
					bGeoreferenceSlippyTiles2 = bGeoreferenceSlippyTiles;
					bMaxY_IsNorth2 = bMaxY_IsNorth;
				}
				
				HMFetcher *Result = new HMDebugFetcher(
					"XYZ_Download",
					new HMXYZ(
						bAllowInvalidTiles, bEnableXYZParallelDownload,
						Name, Layer, Format, URL2, XYZ_Zoom, XYZ_MinX, XYZ_MaxX, XYZ_MinY, XYZ_MaxY,
						bMaxY_IsNorth2, bGeoreferenceSlippyTiles2,
							ImageSourceKind == ELandscapeImageSourceKind::MapTiler_Heightmaps ||
							ImageSourceKind == ELandscapeImageSourceKind::Mapbox_Heightmaps ||
							ImageSourceKind == ELandscapeImageSourceKind::NextZen_Heightmaps,
							ImageSourceKind == ELandscapeImageSourceKind::NextZen_Heightmaps,
						XYZ_CRS
					),
					true
				);

				return Result;
			}
			else
			{
				LCReporter::ShowError(
					FText::Format(
						LOCTEXT("InterfaceFromKindError", "Internal error: heightmap kind '{0}' is not supprted."),
						FText::AsNumber(static_cast<int>(ImageSourceKind))
					)
				); 
				return nullptr;
			}
		}
	}
}

HMFetcher* UMapDownloader::CreateFetcher(
	const bool bIsUserInitiated, const FString& Name, const bool bEnsureOneBand, const bool bScaleAltitude,
	const bool bConvertToPNG, const bool bConvertFirstOnly, const bool bAddMissingTiles,
	const TFunction<bool(HMFetcher*)>& RunBeforePNG, const TObjectPtr<UGlobalCoordinates> GlobalCoordinates
)
{
	HMFetcher *Result = CreateInitialFetcher(bIsUserInitiated, Name);
	if (!Result) return nullptr;
	Result->SetIsUserInitiated(bIsUserInitiated);
	if (bRemap)
	{
		Result = Result->AndThen(new HMDebugFetcher("Convert", new HMConvert(Name, "tif")));
		Result = Result->AndThen(new HMDebugFetcher("FixNoData", new HMFunction([this](float x) { return x == OriginalValue ? TransformedValue : x; })));
	}
	
	if (bPreprocess)
	{
		Result = Result->AndThen(new HMDebugFetcher("Preprocess", new HMPreprocess(Name, PreprocessingTool)));
	}

	if (bEnsureOneBand && IsWms())
	{
		Result = Result->AndThen(new HMDebugFetcher("EnsureOneBand", new HMEnsureOneBand()));
	}

	if (GlobalCoordinates)
	{
		Result = Result->AndThen(new HMDebugFetcher("Reproject", new HMReproject(Name, GlobalCoordinates->CRS)));
	}

	if (bMergeImages)
	{
		Result = Result->AndThen(new HMDebugFetcher("Merge", new HMMerge(Name)));
	}

#if WITH_EDITOR
	if (bAdaptResolution)
	{
		FIntPoint ImageSize;
		if (!IsValid(TargetLandscape))
		{
			LCReporter::ShowError(
				LOCTEXT("UMapDowloader::CreateFetcher::NoLandscape", "Please select a target landscape if you want to resize the image.")
			);
			return nullptr;
		}

		ImageSize.X = TargetLandscape->ComputeComponentCounts().X * TargetLandscape->ComponentSizeQuads + 1;
		ImageSize.Y = TargetLandscape->ComputeComponentCounts().Y * TargetLandscape->ComponentSizeQuads + 1;

		Result = Result->AndThen(new HMDebugFetcher("AdaptImage", new HMResolution(Name, ImageSize)));
	}
#else
	if (bAdaptResolution)
	{
		UE_LOG(LogLandscapeGeneration, Error, TEXT("Adapt Resolution is only supported in editor mode"));
		return nullptr;
	}

#endif

	if (bCropCoordinates)
	{
		if (!IsValid(GlobalCoordinates))
		{
			if ((AllowsParametersSelection() && bCropFollowingParametersSelection && ParametersSelection.ParametersSelectionMethod == EParametersSelectionMethod::FromBoundingActor) ||
				!AllowsParametersSelection() || !bCropFollowingParametersSelection)
			{
				LCReporter::ShowError(
					LOCTEXT("UMapDowloader::CreateFetcher::CropActor", "Bounding actor cannot be used to crop coordinates if there is no Level Coordinates.")
				);
				return nullptr;
			}
		}

		Result = Result->AndThen(new HMDebugFetcher("Crop", new HMCrop(Name, AllowsParametersSelection() && bCropFollowingParametersSelection, ParametersSelection, CroppingActor)));
	}

	if (RunBeforePNG)
	{
		Result = Result->AndRun(RunBeforePNG);
	}
	
	if (bConvertToPNG)
	{
		Result = Result->AndThen(new HMDebugFetcher("ToPNG", new HMToPNG(Name, bScaleAltitude, bHideNoDataFlag, bConvertFirstOnly)));
	}

	if (bAddMissingTiles)
	{
		Result = Result->AndThen(new HMDebugFetcher("AddMissingTiles", new HMAddMissingTiles()));
	}

	if (bScaleResolution)
	{
		Result = Result->AndThen(new HMDebugFetcher("RescaleMap", new HMRescaleMap(Name, PrecisionPercent, ResamplingMethod)));
	}

	return Result;
}

bool UMapDownloader::HasMapTilerToken()
{
	return !GetDefault<ULCSettings>()->MapTiler_Token.IsEmpty();
}

bool UMapDownloader::HasMapboxToken()
{
	return !GetDefault<ULCSettings>()->Mapbox_Token.IsEmpty();
}

bool UMapDownloader::HasNextZenToken()
{
	return !GetDefault<ULCSettings>()->NextZen_Token.IsEmpty();
}

FString UMapDownloader::GetMapTilerToken() const
{
	if (const FString ProjectSettingsMapTilerToken = GetDefault<ULCSettings>()->MapTiler_Token; !ProjectSettingsMapTilerToken.IsEmpty())
	{
		return ProjectSettingsMapTilerToken.TrimStartAndEnd();
	}
	else
	{
		return MapTiler_Token.TrimStartAndEnd();
	}
}

FString UMapDownloader::GetMapboxToken() const
{
	if (const FString ProjectSettingsMapboxToken = GetDefault<ULCSettings>()->Mapbox_Token; !ProjectSettingsMapboxToken.IsEmpty())
	{
		return ProjectSettingsMapboxToken.TrimStartAndEnd();
	}
	else
	{
		return Mapbox_Token.TrimStartAndEnd();
	}
}

FString UMapDownloader::GetNextZenToken()
{
	if (FString ProjectSettingsNextZenToken = GetDefault<ULCSettings>()->NextZen_Token; !ProjectSettingsNextZenToken.
		IsEmpty())
	{
		return ProjectSettingsNextZenToken;
	}
	else
	{
		return NextZen_Token;
	}
}

bool UMapDownloader::IsWms() const
{
	return
		ImageSourceKind == ELandscapeImageSourceKind::GenericWMS ||
		ImageSourceKind == ELandscapeImageSourceKind::Australia_LiDAR5m || 
		ImageSourceKind == ELandscapeImageSourceKind::IGN_Heightmaps ||
		ImageSourceKind == ELandscapeImageSourceKind::IGN_Satellite ||
		ImageSourceKind == ELandscapeImageSourceKind::SHOM ||
		ImageSourceKind == ELandscapeImageSourceKind::USGS_Imagery ||
		ImageSourceKind == ELandscapeImageSourceKind::USGS_Topo ||
		ImageSourceKind == ELandscapeImageSourceKind::USGS_3DEPElevation;
}

bool UMapDownloader::IsMapbox() const
{
	return
		ImageSourceKind == ELandscapeImageSourceKind::Mapbox_Heightmaps ||
		ImageSourceKind == ELandscapeImageSourceKind::Mapbox_Satellite;

}

bool UMapDownloader::IsMapTiler() const
{
	return
		ImageSourceKind == ELandscapeImageSourceKind::MapTiler_Heightmaps ||
		ImageSourceKind == ELandscapeImageSourceKind::MapTiler_Satellite;

}

bool UMapDownloader::IsNextZen() const
{
	return ImageSourceKind == ELandscapeImageSourceKind::NextZen_Heightmaps;
}

bool UMapDownloader::IsWorldElevation3D() const
{
	return ImageSourceKind == ELandscapeImageSourceKind::ArcGIS_WorldElevation3D;
}

bool UMapDownloader::IsXYZ() const
{
	return IsMapbox() || IsMapTiler() || IsNextZen()
		|| ImageSourceKind == ELandscapeImageSourceKind::GenericXYZ;
}

bool UMapDownloader::HasMultipleLayers() const
{
	return WMS_Provider.Titles.Num() >= 2;
}

void UMapDownloader::SetLargestPossibleCoordinates()
{
	WMS_MinLong = WMS_MinAllowedLong;
	WMS_MinLat = WMS_MinAllowedLat;
	WMS_MaxLong = WMS_MaxAllowedLong;
	WMS_MaxLat = WMS_MaxAllowedLat;
}

void UMapDownloader::SetSourceParameters()
{
	SetSourceParametersBool(true);
}

bool UMapDownloader::AllowsParametersSelection() const
{
	return IsWms() || ((IsXYZ() || IsWorldElevation3D()) && bGeoreferenceSlippyTiles)
		|| ImageSourceKind == ELandscapeImageSourceKind::Napoli;
}

bool UMapDownloader::CanAutoSetResolution() const
{
	return ImageSourceKind == ELandscapeImageSourceKind::IGN_Heightmaps;
}

void UMapDownloader::AutoSetResolution()
{
	if (ImageSourceKind == ELandscapeImageSourceKind::IGN_Heightmaps)
	{
		WMS_ResolutionPixelsPerUnit = 1;
		WMS_MinLat = FMath::RoundToZero(WMS_MinLat);
		WMS_MaxLat = FMath::RoundToZero(WMS_MaxLat);
		WMS_MinLong = FMath::RoundToZero(WMS_MinLong);
		WMS_MaxLong = FMath::RoundToZero(WMS_MaxLong);
	}
}

bool UMapDownloader::SetSourceParametersBool(bool bDialog)
{
	if (ParametersSelection.ParametersSelectionMethod == EParametersSelectionMethod::Manual) return true;

	if (!AllowsParametersSelection())
	{
		if (bDialog)
		{
			LCReporter::ShowError(
				LOCTEXT("UMapDowloader::SetSourceParameters::0", "Custom Parameters Selection is supported only for WMS, XYZ and Napoli.")
			);
		}
		return false;
	}

	if (ParametersSelection.ParametersSelectionMethod == EParametersSelectionMethod::FromBoundingActor)
	{
		return SetSourceParametersFromActor(bDialog);
	}
	else if (ParametersSelection.ParametersSelectionMethod == EParametersSelectionMethod::FromEPSG4326Box)
	{
		return SetSourceParametersFromEpsg4326Box(bDialog);
	}
	else if (ParametersSelection.ParametersSelectionMethod == EParametersSelectionMethod::FromEPSG4326Coordinates)
	{
		return SetSourceParametersFromEpsg4326Coordinates(bDialog);
	}
	else
	{
		LCReporter::ShowError(
			LOCTEXT("UMapDowloader::SetSourceParameters::1", "Internal Error: Unsupported parameter selection method.")
		);
		return false;
	}
}

bool UMapDownloader::SetSourceParametersFromEpsg4326Coordinates(bool bDialog)
{
	FVector4d Coordinates = UGlobalCoordinates::GetCoordinatesFromSize(ParametersSelection.Longitude, ParametersSelection.Latitude, ParametersSelection.RealWorldWidth, ParametersSelection.RealWorldHeight);
	ParametersSelection.MinLong = Coordinates[0];
	ParametersSelection.MaxLong = Coordinates[1];
	ParametersSelection.MinLat = Coordinates[2];
	ParametersSelection.MaxLat = Coordinates[3];
	return SetSourceParametersFromEpsg4326Box(bDialog);
}

bool UMapDownloader::SetSourceParametersFromEpsg4326Box(bool bDialog)
{
	if (ImageSourceKind == ELandscapeImageSourceKind::Napoli)
	{
		FVector4d InCoordinates, OutCoordinates;
		InCoordinates[0] = ParametersSelection.MinLong;
		InCoordinates[1] = ParametersSelection.MaxLong;
		InCoordinates[2] = ParametersSelection.MinLat;
		InCoordinates[3] = ParametersSelection.MaxLat;

		if (!GDALInterface::ConvertCoordinates(InCoordinates, OutCoordinates, "EPSG:4326", "EPSG:32633")) return false;

		Napoli_MinLong = OutCoordinates[0];
		Napoli_MaxLong = OutCoordinates[1];
		Napoli_MinLat = OutCoordinates[2];
		Napoli_MaxLat = OutCoordinates[3];

		return true;
	}
	else if (IsWms())
	{
		FVector4d InCoordinates, OutCoordinates;
		InCoordinates[0] = ParametersSelection.MinLong;
		InCoordinates[1] = ParametersSelection.MaxLong;
		InCoordinates[2] = ParametersSelection.MinLat;
		InCoordinates[3] = ParametersSelection.MaxLat;

		if (!GDALInterface::ConvertCoordinates(InCoordinates, OutCoordinates, "EPSG:4326", WMS_CRS)) return false;

		WMS_MinLong = OutCoordinates[0];
		WMS_MaxLong = OutCoordinates[1];
		WMS_MinLat = OutCoordinates[2];
		WMS_MaxLat = OutCoordinates[3];

		return true;
	}
	else if ((IsXYZ() || IsWorldElevation3D()) && bGeoreferenceSlippyTiles)
	{
		const double MinLatRad = FMath::DegreesToRadians(ParametersSelection.MinLat);
		const double MaxLatRad = FMath::DegreesToRadians(ParametersSelection.MaxLat);
		const double n = 1 << XYZ_Zoom;
		XYZ_MinX = (ParametersSelection.MinLong + 180) / 360 * n;
		XYZ_MaxX = (ParametersSelection.MaxLong + 180) / 360 * n;
		
		XYZ_MinY = (1.0 - asinh(FMath::Tan(MaxLatRad)) / UE_PI) / 2.0 * n;
		XYZ_MaxY = (1.0 - asinh(FMath::Tan(MinLatRad)) / UE_PI) / 2.0 * n;

		return true;
	}
	else
	{
		return false;
	}
}


bool UMapDownloader::SetSourceParametersFromActor(bool bDialog)
{
	FVector4d Coordinates;

	if (!IsValid(ParametersSelection.ParametersBoundingActor))
	{
		if (bDialog)
		{
			LCReporter::ShowError(LOCTEXT("UMapDowloader::SetSourceParameters", "Please select a bounding actor."));
		}
		return false;
	}

	UE_LOG(LogLandscapeGeneration, Log, TEXT("Set Source Parameters From Actor %s"), *ParametersSelection.ParametersBoundingActor->GetActorNameOrLabel());

	FString SourceCrs = "";
	
	if (ImageSourceKind == ELandscapeImageSourceKind::Napoli)
	{
		SourceCrs = "EPSG:32633";
	}
	else if (IsWms())
	{
		SourceCrs = WMS_CRS;
	}
	else if ((IsXYZ() || IsWorldElevation3D()) && bGeoreferenceSlippyTiles)
	{
		SourceCrs = "EPSG:3857";
	}

	if (SourceCrs.IsEmpty())
	{
		if (bDialog)
		{
			LCReporter::ShowError(FText::Format(
				LOCTEXT("UMapDowloader::SetSourceParameters::1", "Please make sure that the CRS is not empty."),
				FText::FromString(ParametersSelection.ParametersBoundingActor->GetActorNameOrLabel())
			));
		}
		return false;
	}

	if (!LandscapeUtils::GetActorCRSBounds(ParametersSelection.ParametersBoundingActor, SourceCrs, Coordinates))
	{
		if (bDialog)
		{
			LCReporter::ShowError(FText::Format(
				LOCTEXT("UMapDowloader::SetSourceParameters::2", "Could not read coordinates from Actor {0}."),
				FText::FromString(ParametersSelection.ParametersBoundingActor->GetActorNameOrLabel())
			));
		}
		return false;
	}

	const double ActorMinLong = Coordinates[0];
	const double ActorMaxLong = Coordinates[1];
	const double ActorMinLat = Coordinates[2];
	const double ActorMaxLat = Coordinates[3];
	
	if (ImageSourceKind == ELandscapeImageSourceKind::Napoli)
	{
		Napoli_MinLong = ActorMinLong;
		Napoli_MaxLong = ActorMaxLong;
		Napoli_MinLat = ActorMinLat;
		Napoli_MaxLat = ActorMaxLat;

		return true;
	}
	if (IsWms())
	{
		WMS_MinLong = ActorMinLong;
		WMS_MaxLong = ActorMaxLong;
		WMS_MinLat = ActorMinLat;
		WMS_MaxLat = ActorMaxLat;

		return true;
	}
	else if ((IsXYZ() || IsWorldElevation3D()) && bGeoreferenceSlippyTiles)
	{
		GDALInterface::EPSG3857ToXYZTile(ActorMinLong, ActorMaxLat, XYZ_Zoom, XYZ_MinX, XYZ_MinY);
		GDALInterface::EPSG3857ToXYZTile(ActorMaxLong, ActorMinLat, XYZ_Zoom, XYZ_MaxX, XYZ_MaxY);

		return true;
	}
	else
	{
		return false;
	}
}

#if WITH_EDITOR

void UMapDownloader::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	if (!Event.Property)
	{
		Super::PostEditChangeProperty(Event);
		return;
	}

	if (const FName PropertyName = Event.Property->GetFName(); PropertyName == GET_MEMBER_NAME_CHECKED(UMapDownloader, ImageSourceKind))
	{
		OnImageSourceChanged(nullptr);
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UMapDownloader, CapabilitiesURL))
	{
		ResetWmsProvider(TArray<FString>(), nullptr, nullptr);
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UMapDownloader, WMS_Title))
	{
		OnLayerChanged();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UMapDownloader, ParametersSelection))
	{
		SetSourceParametersBool(false);
	}
	else if (
		PropertyName == GET_MEMBER_NAME_CHECKED(UMapDownloader, ParametersSelection.ParametersBoundingActor) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UMapDownloader, ParametersSelection.MinLong) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UMapDownloader, ParametersSelection.MaxLong) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UMapDownloader, ParametersSelection.MinLat) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UMapDownloader, ParametersSelection.MaxLat)
	)
	{
		SetSourceParametersBool(true);
	}

	Super::PostEditChangeProperty(Event);
}

#endif


void UMapDownloader::OnLayerChanged()
{
	const int LayerIndex = WMS_Provider.Titles.Find(WMS_Title);
	if (LayerIndex == INDEX_NONE)
	{
		WMS_Help = FString::Format(TEXT("Could not find Layer {0}"), { WMS_Title });
		return;
	}
	WMS_Name = WMS_Provider.Names[LayerIndex];
	WMS_Help = "Please enter the coordinates using the following CRS:";
	WMS_CRS = WMS_Provider.CRSs[LayerIndex];
	if (WMS_X_IsLong)
	{
		WMS_MinAllowedLong = WMS_Provider.MinXs[LayerIndex];
		WMS_MaxAllowedLong = WMS_Provider.MaxXs[LayerIndex];
		WMS_MinAllowedLat = WMS_Provider.MinYs[LayerIndex];
		WMS_MaxAllowedLat = WMS_Provider.MaxYs[LayerIndex];
	}
	else
	{
		WMS_MinAllowedLong = WMS_Provider.MinYs[LayerIndex];
		WMS_MaxAllowedLong = WMS_Provider.MaxYs[LayerIndex];
		WMS_MinAllowedLat = WMS_Provider.MinXs[LayerIndex];
		WMS_MaxAllowedLat = WMS_Provider.MaxXs[LayerIndex];
	}
	WMS_SearchCRS = FString::Format(TEXT("https://duckduckgo.com/?q={0}+site%3Aepsg.io"), { WMS_CRS });
	WMS_Abstract = WMS_Provider.Abstracts[LayerIndex];
}

void UMapDownloader::OnImageSourceChanged(const TFunction<void(bool)>& OnComplete)
{
	if (IsWorldElevation3D())
	{
		CapabilitiesURL.Empty();
		XYZ_Zoom = FMath::Clamp(XYZ_Zoom, 0, 16);
		ResetWmsProvider(TArray<FString>(), nullptr, OnComplete);
		return;
	}
	if (ImageSourceKind == ELandscapeImageSourceKind::IGN_Heightmaps)
	{
		CapabilitiesURL = "https://data.geopf.fr/wms-r?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetCapabilities";
		ResetWmsProvider(TArray<FString>(), nullptr, OnComplete);
		WMS_X_IsLong = true;
	}
	else if (ImageSourceKind == ELandscapeImageSourceKind::IGN_Satellite)
	{
		CapabilitiesURL = "https://data.geopf.fr/annexes/ressources/wms-r/satellite.xml";
		ResetWmsProvider(TArray<FString>(), nullptr, OnComplete);
		WMS_X_IsLong = true;
	}
	else if (ImageSourceKind == ELandscapeImageSourceKind::Australia_LiDAR5m)
	{
		CapabilitiesURL = "http://gaservices.ga.gov.au/site_9/services/DEM_LiDAR_5m/MapServer/WMSServer?request=GetCapabilities&service=WMS";
		ResetWmsProvider(TArray<FString>(), nullptr, OnComplete);
		WMS_X_IsLong = true;
	}
	else if (ImageSourceKind == ELandscapeImageSourceKind::SHOM)
	{
		CapabilitiesURL = "https://services.data.shom.fr/INSPIRE/wms/r?service=WMS&version=1.3.0&request=GetCapabilities";
		ResetWmsProvider(TArray<FString>(), nullptr, OnComplete);
		WMS_X_IsLong = true;
	}
	else if (ImageSourceKind == ELandscapeImageSourceKind::USGS_3DEPElevation)
	{
		CapabilitiesURL = "https://elevation.nationalmap.gov/arcgis/services/3DEPElevation/ImageServer/WMSServer?request=GetCapabilities&service=WMS";
		ResetWmsProvider(TArray<FString>(), nullptr, OnComplete);
		WMS_X_IsLong = true;
	}
	else if (ImageSourceKind == ELandscapeImageSourceKind::USGS_Topo)
	{
		CapabilitiesURL = "https://basemap.nationalmap.gov/arcgis/services/USGSTopo/MapServer/WMSServer?request=GetCapabilities&service=WMS";
		ResetWmsProvider(TArray<FString>(), nullptr, OnComplete);
		WMS_X_IsLong = true;
	}
	else if (ImageSourceKind == ELandscapeImageSourceKind::USGS_Imagery)
	{
		CapabilitiesURL = "https://basemap.nationalmap.gov/arcgis/services/USGSImageryOnly/MapServer/WMSServer?request=GetCapabilities&service=WMS";
		ResetWmsProvider(TArray<FString>(), nullptr, OnComplete);
		WMS_X_IsLong = true;
	}
	else if (IsMapbox())
	{
		if (!ShowMapboxFreeTierWarning())
		{
			if (OnComplete) OnComplete(false);
			return;
		}
		ResetWmsProvider(TArray<FString>(), nullptr, OnComplete);
	}
	else if (IsMapTiler())
	{
		if (!ShowMapTilerFreeTierWarning())
		{
			if (OnComplete) OnComplete(false);
			return;
		}
		ResetWmsProvider(TArray<FString>(), nullptr, OnComplete);
	}
	else
	{
		ResetWmsProvider(TArray<FString>(), nullptr, OnComplete);
	}
}

void UMapDownloader::ResetWmsProvider(const TArray<FString>& ExcludeCrs, const TFunction<bool(FString)>& LayerFilter, TFunction<void(bool)> OnComplete)
{
	if (CapabilitiesURL.IsEmpty())
	{
		WMS_Abstract = "";
		WMS_Title = "";
		WMS_Name = "";
		WMS_Help = "";
		WMS_CRS = "";
		WMS_SearchCRS = "";
		WMS_MinAllowedLong = 0;
		WMS_MinAllowedLat = 0;
		WMS_MaxAllowedLat = 0;
		WMS_MaxAllowedLong = 0;
		WMS_MaxWidth = 0;
		WMS_MaxHeight = 0;
	}
	else
	{
		WMS_Provider.SetFromURL(
			CapabilitiesURL,
			ExcludeCrs,
			LayerFilter,
			[this, OnComplete](const bool bSuccess)
			{
				WMS_Abstract = "";
				WMS_Title = "";
				WMS_Name = "";
				WMS_Help = "";
				WMS_CRS = "";
				WMS_SearchCRS = "";
				WMS_MinAllowedLong = 0;
				WMS_MinAllowedLat = 0;
				WMS_MaxAllowedLat = 0;
				WMS_MaxAllowedLong = 0;
				WMS_MaxWidth = 0;
				WMS_MaxHeight = 0;

				if (!bSuccess)
				{
					WMS_Help = FString::Format(TEXT("There was an error while loading WMS Provider from URL: {0}"), { CapabilitiesURL });
				}
				
				WMS_MaxWidth = WMS_Provider.MaxWidth;
				WMS_MaxHeight = WMS_Provider.MaxHeight;

				if (!WMS_Provider.Titles.Contains(WMS_Title) && !WMS_Provider.Titles.IsEmpty())
				{
					WMS_Title = WMS_Provider.Titles[0];
					OnLayerChanged();
				}

				if (OnComplete) OnComplete(bSuccess);
			}
		);
	}
}

bool UMapDownloader::DownloadImages(const bool bIsUserInitiated, const bool bEnsureOneBand, const TObjectPtr<UGlobalCoordinates> GlobalCoordinates, TArray<FString> &OutDownloadedImages, FString &OutImagesCRS)
{
	const AActor *Owner = GetOwner();
	if (!IsValid(Owner))
	{
		LCReporter::ShowError(
			LOCTEXT("UMapDowloader::DownloadImages::NoOwner", "Internal Error: Could not find UMapDowloader Owner.")
		);
		return false;
	}

	const FString Name = Owner->GetWorld()->GetName() + "-" + Owner->GetActorNameOrLabel();
	HMFetcher *Fetcher = CreateFetcher(bIsUserInitiated, Name, bEnsureOneBand, false, false, false, false, nullptr, GlobalCoordinates);

	if (!Fetcher)
	{
		LCReporter::ShowError(
			LOCTEXT("UMapDowloader::DownloadImages::NoFetcher", "Could not make image fetcher.")
		);
		return false;
	}

	if (!Fetcher->Fetch("", {}))
	{
		delete Fetcher;
		LCReporter::ShowError(
			LOCTEXT("UMapDowloader::DownloadImages::Failure", "There was an error while downloading or preparing the files.")
		);
		return false;
	}

	else
	{
		OutDownloadedImages = Fetcher->OutputFiles;
		OutImagesCRS = Fetcher->OutputCRS;
		delete Fetcher;
		return true;
	}
}

bool UMapDownloader::ShowMapTilerFreeTierWarning()
{
	return LCReporter::ShowMessage(
		LOCTEXT(
			"UMapDowloader::OnImageSourceChanged::MapTilerWarning",
			"Please check your MapTiler account to make sure you remain within the free tier.\nRequests can be expensive once you go beyond the free tier.\nContinue?"
		),
		"SuppressFreeTierWarning",
		LOCTEXT("MapTilerWarningTitle", "MapTiler Free Tier Warning")
	);
}

bool UMapDownloader::ShowMapboxFreeTierWarning()
{
	return LCReporter::ShowMessage(
		LOCTEXT(
			"UMapDowloader::OnImageSourceChanged::MapboxWarning",
			"Please check your Mapbox account to make sure you remain within the free tier.\nRequests can be expensive once you go beyond the free tier.\n"
			"The 2x option counts for more API requests than usual.\nContinue?"
		),
		"SuppressFreeTierWarning",
		LOCTEXT("MapboxWarningTitle", "Mapbox Free Tier Warning")
	);
}

#undef LOCTEXT_NAMESPACE
