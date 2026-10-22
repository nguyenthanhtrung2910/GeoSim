#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ArcGISMiniMapWidget.generated.h"

class UImage;
class UTextBlock;
class UTexture2D;

/** A north-up map image and player marker, built in C++ without a Widget Blueprint. */
UCLASS()
class GEOSIM_API UArcGisMiniMapWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void Configure(float InMapSize, const FText& InAttribution);
	void SetMapTexture(UTexture2D* Texture) const;
	void SetStatus(const FText& Text) const;
	void SetPlayerPosition(const FVector2D& UV, bool bVisible) const;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UImage> Background;

	UPROPERTY(Transient)
	TObjectPtr<UImage> MapImage;

	UPROPERTY(Transient)
	TObjectPtr<UImage> PlayerMarker;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> Status;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> Attribution;

	float MapSize = 320.0f;
	FText AttributionText;
};
