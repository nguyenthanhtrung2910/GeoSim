// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

class USceneCaptureComponent2D;
class UMaterial;

#include "CoreMinimal.h"
#include "IImageWrapper.h" // Include the header that defines ERGBFormat
#include "GameFramework/Actor.h"
#include "FrameCaptureManager.generated.h"


UENUM(BlueprintType)
enum class ECustomImageFormat : uint8
{
    //Invalid UMETA(DisplayName = "Invalid"),
    PNG UMETA(DisplayName = "PNG"),
    JPEG UMETA(DisplayName = "JPEG"),
    //GrayscaleJPEG UMETA(DisplayName = "GrayscaleJPEG"),
    //BMP UMETA(DisplayName = "BMP"),
    //ICO UMETA(DisplayName = "ICO"),
    EXR UMETA(DisplayName = "EXR"),
    //ICNS UMETA(DisplayName = "ICNS"),
    //TGA UMETA(DisplayName = "TGA"),
    //HDR UMETA(DisplayName = "HDR"),
    //TIFF UMETA(DisplayName = "TIFF"),
    //DDS UMETA(DisplayName = "DDS"),
    //UEJPEG UMETA(DisplayName = "UEJPEG"),
    //GrayscaleUEJPEG UMETA(DisplayName = "GrayscaleUEJPEG")
};

struct FRenderRequestStruct{
    FIntPoint ImageSize;
    FRHIGPUTextureReadback Readback;
    FRenderCommandFence RenderFence;
    TArray64<uint8> PixelData;

    FRenderRequestStruct(
        const FIntPoint& ImageSize,
        const FRHIGPUTextureReadback& Readback) :
            ImageSize(ImageSize),
            Readback(Readback) {}
};


UCLASS()
class GEOSIM_API AFrameCaptureManager : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	AFrameCaptureManager();

	// Captured Data Sub-Directory Name 
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Capture File")
	FString SubDirectoryName = "/Color/";
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Capture File")
	int NumDigits = 6;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capture Format")
    ECustomImageFormat ImageFormat = ECustomImageFormat::PNG;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Capture Format")
	int FrameWidth = 640;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Capture Format")
	int FrameHeight = 480;

	// Capture component owned by this actor.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Capture Source")
	USceneCaptureComponent2D* CaptureComponent;

	// PostProcessMaterial used for segmentation
	UPROPERTY(EditAnywhere, Category="Capture Source")
	UMaterial* PostProcessMaterial = nullptr;

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

	// Render requests waiting for their GPU readback.
    TQueue<TSharedPtr<FRenderRequestStruct, ESPMode::ThreadSafe>> RenderRequestQueue;
    
	// Counter for file names
	int ImgCounter = 0;

	// Prepares SceneCapture2D component for capturing
	void SetupCaptureComponent();

	static FString ToStringWithLeadingZeros(int32 Integer, int32 MaxDigits);

	static FString GetFileEnding(ECustomImageFormat Format)
	{
		switch (Format)
		{
			case ECustomImageFormat::PNG:               return ".png";
			case ECustomImageFormat::JPEG:              return ".jpeg";
			case ECustomImageFormat::EXR:     			return ".exr";
		default:                                		return "";
		}
	};

	static ERGBFormat GetRGBFormatFromImageFormat(ECustomImageFormat MyFormat)
	{
		{
			switch (MyFormat)
			{
				case ECustomImageFormat::PNG:               return ERGBFormat::RGBA;
				case ECustomImageFormat::JPEG:              return ERGBFormat::RGBA;
				case ECustomImageFormat::EXR:               return ERGBFormat::RGBAF;
				default:                               		return ERGBFormat::RGBA;
			}
		}
	};

	static EImageFormat ConvertImageFormat(ECustomImageFormat MyFormat)
	{
		switch (MyFormat)
		{
			case ECustomImageFormat::PNG:               return EImageFormat::PNG;
			case ECustomImageFormat::JPEG:              return EImageFormat::JPEG;
			//case ECustomImageFormat::GrayscaleJPEG:     return EImageFormat::GrayscaleJPEG;
			//case ECustomImageFormat::BMP:               return EImageFormat::BMP;
			//case ECustomImageFormat::ICO:               return EImageFormat::ICO;
			case ECustomImageFormat::EXR:               return EImageFormat::EXR;
			//case ECustomImageFormat::ICNS:              return EImageFormat::ICNS;
			//case ECustomImageFormat::TGA:               return EImageFormat::TGA;
			//case ECustomImageFormat::HDR:               return EImageFormat::HDR;
			//case ECustomImageFormat::TIFF:              return EImageFormat::TIFF;
			//case ECustomImageFormat::DDS:               return EImageFormat::DDS;
			//case ECustomImageFormat::UEJPEG:            return EImageFormat::UEJPEG;
			//case ECustomImageFormat::GrayscaleUEJPEG:   return EImageFormat::GrayscaleUEJPEG;
			default:                                     return EImageFormat::PNG;
		}
	};

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	UFUNCTION(BlueprintCallable, Category = "ImageCapture")
    void CaptureNonBlocking();
};


class AsyncSaveImageToDiskTask : public FNonAbandonableTask
{
public:
    AsyncSaveImageToDiskTask(
		const TSharedPtr<FRenderRequestStruct, ESPMode::ThreadSafe>& InRenderRequest,
		FString ImageName, 
		int32 Width, 
		int32 Height, 
		ERGBFormat RGBFormat,
		EImageFormat ImageFormat);
    ~AsyncSaveImageToDiskTask();

    void DoWork() const;

    FORCEINLINE TStatId GetStatId() const
    {
        RETURN_QUICK_DECLARE_CYCLE_STAT(AsyncSaveImageToDiskTask, STATGROUP_ThreadPoolAsyncTasks);
    }

private:
    TSharedPtr<FRenderRequestStruct, ESPMode::ThreadSafe> RenderRequest;
    FString FileName;
    int32 Width;
    int32 Height;
    ERGBFormat RGBFormat;
	EImageFormat ImageFormat;
};
