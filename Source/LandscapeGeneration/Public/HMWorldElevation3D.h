// Copyright 2023-2025 LandscapeCombinator. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ImageDownloader/HMFetcher.h"

/** Fetches Terrain3D elevation tiles, decodes LERC, and assigns Web Mercator georeferencing. */
class LANDSCAPEGENERATION_API HMWorldElevation3D : public HMFetcher
{
public:
	HMWorldElevation3D(bool bInAllowInvalidTiles, bool bInEnableParallelDownload,
		FString InName, int InZoom, int InMinX, int InMaxX, int InMinY, int InMaxY)
		: bAllowInvalidTiles(bInAllowInvalidTiles), bEnableParallelDownload(bInEnableParallelDownload),
		  Name(MoveTemp(InName)), Zoom(InZoom), MinX(InMinX), MinY(InMinY), MaxX(InMaxX), MaxY(InMaxY) {}

protected:
	FString GetOutputDir() override { return FPaths::Combine(ImageDownloaderDir, Name + TEXT("-XYZ")); }
	bool OnFetch(FString InputCrs, TArray<FString> InputFiles) override;

	bool DecodeTiles(const FString& DownloadFile, int X, int Y, FString& DecodedFile, bool* BShowedDialog);
	bool GeoreferenceTiles(const FString& DecodedFile, int X, int Y, FString& OutputFile);

private:
	bool bAllowInvalidTiles;
	bool bEnableParallelDownload;
	FString Name;
	int Zoom;
	int MinX;
	int MinY;
	int MaxX;
	int MaxY;
};
