#pragma once

#include "CoreMinimal.h"

class ARoadGenerator;
class AOsmRoadScene;
class ALandscape;
struct FImportedRoadWay;

namespace RoadGenerationSignatures
{
	FString RoadInputs(const ARoadGenerator& Generator, const TArray<FImportedRoadWay>& Ways, FName OutputFolder);
	FString RoadGeometryInputs(const ARoadGenerator& Generator, const TArray<FImportedRoadWay>& Ways, FName OutputFolder);
	FString JunctionInputs(const ARoadGenerator& Generator, const FString& CurveInputs);
	FString LandscapeInputs(const ARoadGenerator& Generator, const AOsmRoadScene& Scene);
	FString LandscapeOutput(const ALandscape* Landscape, const TSet<FString>& Prefixes);
}
