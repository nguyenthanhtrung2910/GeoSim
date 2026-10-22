#pragma once

#include "CoreMinimal.h"

/** A source OSM node, projected and placed using Landscape Combinator. */
struct FImportedRoadPoint
{
	int64 NodeId = 0;
	FVector WorldLocation = FVector::ZeroVector;
	bool bHasTerrain = false;
};

/** Preserves topology and tags independently of any spline or mesh representation. */
struct FImportedRoadWay
{
	int64 WayId = 0;
	TMap<FString, FString> Tags;
	TArray<FImportedRoadPoint> Points;
};
