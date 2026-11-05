#include "ArcGISMiniMapWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateColorBrush.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Engine/Texture2D.h"

TSharedRef<SWidget> UArcGisMiniMapWidget::RebuildWidget()
{
	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"));
	}
	if (!WidgetTree->RootWidget)
	{
		UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("MapCanvas"));
		WidgetTree->RootWidget = Canvas;
		Canvas->SetClipping(EWidgetClipping::ClipToBounds);

		Background = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Background"));
		Background->SetBrush(FSlateColorBrush(FLinearColor(0.025f, 0.025f, 0.025f, 0.95f)));
		Canvas->AddChildToCanvas(Background);

		MapImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("MapImage"));
		MapImage->SetBrush(FSlateColorBrush(FLinearColor(0.06f, 0.06f, 0.06f, 1.0f)));
		Canvas->AddChildToCanvas(MapImage);

		PlayerMarker = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("PlayerMarker"));
		PlayerMarker->SetBrush(FSlateColorBrush(FLinearColor(0.05f, 1.0f, 0.1f, 1.0f)));
		PlayerMarker->SetVisibility(ESlateVisibility::Hidden);
		UCanvasPanelSlot* MarkerSlot = Canvas->AddChildToCanvas(PlayerMarker);
		MarkerSlot->SetSize(FVector2D(10.0, 10.0));
		MarkerSlot->SetAlignment(FVector2D(0.5, 0.5));

		Status = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("MapStatus"));
		Status->SetText(FText::FromString(TEXT("Loading map...")));
		Status->SetJustification(ETextJustify::Center);
		FSlateFontInfo StatusFont = Status->GetFont();
		StatusFont.Size = 14;
		Status->SetFont(StatusFont);
		UCanvasPanelSlot* StatusSlot = Canvas->AddChildToCanvas(Status);
		StatusSlot->SetAlignment(FVector2D(0.5, 0.5));

		Attribution = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("MapAttribution"));
		Attribution->SetAutoWrapText(true);
		FSlateFontInfo CreditFont = Attribution->GetFont();
		CreditFont.Size = 8;
		Attribution->SetFont(CreditFont);
		Canvas->AddChildToCanvas(Attribution);
		Configure(MapSize, AttributionText);
	}
	return Super::RebuildWidget();
}

void UArcGisMiniMapWidget::Configure(float InMapSize, const FText& InAttribution)
{
	MapSize = FMath::Clamp(InMapSize, 100.0f, 1000.0f);
	AttributionText = InAttribution;
	SetVisibility(ESlateVisibility::HitTestInvisible);
	if (!MapImage)
	{
		return;
	}
	CastChecked<UCanvasPanelSlot>(Background->Slot)->SetSize(FVector2D(MapSize, MapSize + 80.0f));
	CastChecked<UCanvasPanelSlot>(MapImage->Slot)->SetSize(FVector2D(MapSize, MapSize));
	UCanvasPanelSlot* StatusSlot = CastChecked<UCanvasPanelSlot>(Status->Slot);
	StatusSlot->SetPosition(FVector2D(MapSize * 0.5f, MapSize * 0.5f));
	StatusSlot->SetSize(FVector2D(MapSize - 20.0f, 40.0f));
	Attribution->SetText(AttributionText);
	UCanvasPanelSlot* CreditSlot = CastChecked<UCanvasPanelSlot>(Attribution->Slot);
	CreditSlot->SetPosition(FVector2D(4.0f, MapSize + 4.0f));
	CreditSlot->SetSize(FVector2D(MapSize - 8.0f, 72.0f));
}

void UArcGisMiniMapWidget::SetMapTexture(UTexture2D* Texture) const
{
	if (MapImage)
	{
		MapImage->SetBrushFromTexture(Texture);
		MapImage->SetBrushTintColor(FSlateColor(FLinearColor::White));
		MapImage->SetColorAndOpacity(FLinearColor::White);
		SetStatus(FText::GetEmpty());
	}
}

void UArcGisMiniMapWidget::SetStatus(const FText& Text) const
{
	if (Status)
	{
		Status->SetText(Text);
		Status->SetVisibility(Text.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
}

void UArcGisMiniMapWidget::SetPlayerPosition(const FVector2D& UV, bool bVisible) const
{
	if (PlayerMarker)
	{
		PlayerMarker->SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
		if (bVisible)
		{
			CastChecked<UCanvasPanelSlot>(PlayerMarker->Slot)->SetPosition(UV * MapSize);
		}
	}
}
