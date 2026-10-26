#pragma once

#include "CoreMinimal.h"

class ARoadGenerator;
class AOsmRoadScene;
class ALandscape;
struct FImportedRoadWay;

namespace RoadGenerationSignatures
{
	FString RoadInputs(const ARoadGenerator& Generator, const TArray<FImportedRoadWay>& Ways, FName OutputFolder);
	FString RoadOutput(const AOsmRoadScene* Scene);
	FString LandscapeInputs(const ARoadGenerator& Generator, const AOsmRoadScene& Scene);
	FString LandscapeOutput(const ALandscape* Landscape, const TSet<FString>& Prefixes);
}
