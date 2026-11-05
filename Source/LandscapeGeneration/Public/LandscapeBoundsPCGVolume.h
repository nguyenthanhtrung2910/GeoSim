#pragma once

#include "CoreMinimal.h"
#include "PCGVolume.h"
#include "LandscapeBoundsPCGVolume.generated.h"

class ALandscapeProxy;

/** A native PCG volume whose brush can be fitted to a landscape with an editor button. */
UCLASS(BlueprintType, DisplayName = "Landscape Bounds PCG Volume")
class LANDSCAPEGENERATION_API ALandscapeBoundsPCGVolume : public APCGVolume
{
	GENERATED_BODY()

public:
	/** Selecting a streaming proxy also fits the complete landscape it belongs to. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Landscape Bounds")
	TObjectPtr<ALandscapeProxy> TargetLandscape;

	/** Extra space on each side of the landscape's world bounds, in cm. Z padding includes flat terrain inside the volume. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landscape Bounds", meta = (ClampMin = "0", Units = "cm"))
	FVector BoundsPadding = FVector(0.0, 0.0, 100.0);

	/** Rebuild the box brush around the landscape. The resulting volume is aligned with world axes. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Landscape Bounds")
	void MatchLandscapeBounds();

private:
#if WITH_EDITOR
	bool ResolveLandscapeBounds(FBox& OutBounds) const;
#endif
};
