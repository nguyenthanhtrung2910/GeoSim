// Fill out your copyright notice in the Description page of Project Settings.
#include "FrameCaptureManager.h"

#include "Components/SceneCaptureComponent2D.h"
#include "ShowFlags.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/Material.h"
#include "RHICommandList.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"

// Sets default values
AFrameCaptureManager::AFrameCaptureManager()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
	CaptureComponent = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("CaptureComponent"));
	RootComponent = CaptureComponent;
	CaptureComponent->bCaptureEveryFrame = false;
	CaptureComponent->bCaptureOnMovement = false;
	CaptureComponent->bAlwaysPersistRenderingState = true;
}

// Called when the game starts or when spawned
void AFrameCaptureManager::BeginPlay()
{
	Super::BeginPlay();
	
	SetupCaptureComponent();
}

// Called every frame
void AFrameCaptureManager::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

    TSharedPtr<FRenderRequestStruct, ESPMode::ThreadSafe>* PendingRequest = RenderRequestQueue.Peek();
    if (!PendingRequest || !(*PendingRequest)->RenderFence.IsFenceComplete() || !(*PendingRequest)->Readback.IsReady())
    {
        return;
    }

    // Lock and unlock the GPU readback on the rendering thread. The save task
    // receives its own packed pixel buffer, so it never accesses mapped GPU memory.
    TSharedPtr<FRenderRequestStruct, ESPMode::ThreadSafe> Request = *PendingRequest;
    RenderRequestQueue.Pop();

    const ECustomImageFormat CaptureFormat = ImageFormat;
    const int32 BytesPerPixel = CaptureFormat == ECustomImageFormat::EXR ? sizeof(FFloat16Color) : sizeof(FColor);
    FString FileName = FPaths::ProjectSavedDir() + SubDirectoryName + TEXT("/img_")
        + ToStringWithLeadingZeros(ImgCounter++, NumDigits) + GetFileEnding(CaptureFormat);
    const ERGBFormat RGBFormat = GetRGBFormatFromImageFormat(CaptureFormat);
    const EImageFormat OutputFormat = ConvertImageFormat(CaptureFormat);

    ENQUEUE_RENDER_COMMAND(CopyCaptureReadback)(
        [Request, FileName = MoveTemp(FileName), BytesPerPixel, RGBFormat, OutputFormat](FRHICommandListImmediate& RHICmdList) mutable
        {
            int32 RowPitchInPixels = 0;
            const void* RawData = Request->Readback.Lock(RowPitchInPixels, nullptr);
            if (!RawData)
            {
                UE_LOG(LogTemp, Error, TEXT("Failed to lock capture readback"));
                return;
            }

            const int32 Width = Request->ImageSize.X;
            const int32 Height = Request->ImageSize.Y;
            const int64 RowBytes = static_cast<int64>(Width) * BytesPerPixel;
            Request->PixelData.SetNumUninitialized(RowBytes * Height);
            const uint8* Source = static_cast<const uint8*>(RawData);
            uint8* Destination = Request->PixelData.GetData();
            for (int32 Row = 0; Row < Height; ++Row)
            {
                FMemory::Memcpy(Destination + static_cast<int64>(Row) * RowBytes,
                    Source + static_cast<int64>(Row) * RowPitchInPixels * BytesPerPixel, RowBytes);
            }
            Request->Readback.Unlock();

            (new FAutoDeleteAsyncTask<AsyncSaveImageToDiskTask>(
                Request, MoveTemp(FileName), Width, Height, RGBFormat, OutputFormat))->StartBackgroundTask();
        });
}

void AFrameCaptureManager::SetupCaptureComponent(){
    if(!IsValid(CaptureComponent)){
        UE_LOG(LogTemp, Error, TEXT("SetupCaptureComponent: CaptureComponent is not valid!"));
        return;
    }

    // Create RenderTargets
    UTextureRenderTarget2D* renderTarget2D = NewObject<UTextureRenderTarget2D>(this);
    renderTarget2D->InitAutoFormat(256, 256); // some random format, got crashing otherwise

    // Float Capture
    if(ImageFormat == ECustomImageFormat::EXR){ // handle float case
        renderTarget2D->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA32f;
        renderTarget2D->InitCustomFormat(FrameWidth, FrameHeight, PF_FloatRGBA, true); // PF_B8G8R8A8 disables HDR which will boost storing to disk due to less image information
    }
    // Color Capture
    else{
        renderTarget2D->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8_SRGB; //ETextureRenderTargetFormat::RTF_RGBA8; //8-bit color format
        renderTarget2D->InitCustomFormat(FrameWidth, FrameHeight, PF_R8G8B8A8, true); // PF_R8G8B8A8 //PF_B8G8R8A8 // PF... disables HDR, which is most important since HDR gives gigantic overhead, and is not needed!
        renderTarget2D->bForceLinearGamma = true; // Important for viewport-like color reproduction.
    }
    renderTarget2D->bGPUSharedFlag = true; // demand buffer on GPU

    // Assign RenderTarget
    CaptureComponent->TextureTarget = renderTarget2D;
    // Set Camera Properties
    CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    CaptureComponent->TextureTarget->TargetGamma = GEngine->GetDisplayGamma();
    CaptureComponent->ShowFlags.SetTemporalAA(true);
    // lookup more show flags in documentation

    // Assign PostProcess Material if assigned
    if(PostProcessMaterial){ // check nullptr
        CaptureComponent->AddOrUpdateBlendable(PostProcessMaterial);
    } else {
        UE_LOG(LogTemp, Log, TEXT("No PostProcessMaterial is assigned"));
    }
    UE_LOG(LogTemp, Warning, TEXT("Initialized RenderTarget!"));
}



void AFrameCaptureManager::CaptureNonBlocking(){
    if (!IsValid(CaptureComponent) || !IsValid(CaptureComponent->TextureTarget)) {
        UE_LOG(LogTemp, Error, TEXT("CaptureNonBlocking: capture component or render target is not valid!"));
        return;
    }
    CaptureComponent->TextureTarget->TargetGamma = GEngine->GetDisplayGamma();

    // Render a fresh image before enqueuing its GPU readback.
    CaptureComponent->CaptureScene();

    // Get RenderContext
    FTextureRenderTargetResource* renderTargetResource = CaptureComponent->TextureTarget->GameThread_GetRenderTargetResource();
    
    TSharedPtr<FRenderRequestStruct, ESPMode::ThreadSafe> renderRequest =
        MakeShared<FRenderRequestStruct, ESPMode::ThreadSafe>(
            renderTargetResource->GetSizeXY(), 
            FRHIGPUTextureReadback(TEXT("CameraCaptureManagerReadback")
        )
    );

    ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
    [renderRequest, renderTargetResource](FRHICommandListImmediate& RHICmdList) {
        FTextureRHIRef Target = renderTargetResource->GetRenderTargetTexture();
        renderRequest->Readback.EnqueueCopy(RHICmdList, Target);
    });

    // Notify new task in RenderQueue
    RenderRequestQueue.Enqueue(renderRequest);

    // Set RenderCommandFence
    renderRequest->RenderFence.BeginFence();
}



FString AFrameCaptureManager::ToStringWithLeadingZeros(int32 Integer, int32 MaxDigits){
    FString Result = FString::FromInt(Integer);
    const int32 StringSize = Result.Len();
    const int32 StringDelta = MaxDigits - StringSize;
    if(StringDelta < 0){
        UE_LOG(LogTemp, Error, TEXT("MaxDigits of ImageCounter Overflow!"));
        return Result;
    }
    //FIXME: Smarter function for this
    FString LeadingZeros = "";
    for(size_t i=0;i<StringDelta;i++){
        LeadingZeros += "0";
    }
    Result = LeadingZeros + Result;

    return Result;
}

/*
*******************************************************************
*/

AsyncSaveImageToDiskTask::AsyncSaveImageToDiskTask(
        const TSharedPtr<FRenderRequestStruct, ESPMode::ThreadSafe>& InRenderRequest,
        FString ImageName,
        const int32 Width,
        const int32 Height,
        const ERGBFormat RGBFormat,
        const EImageFormat ImageFormat
    ):
    RenderRequest(InRenderRequest),
    FileName(MoveTemp(ImageName)),
    Width(Width),
    Height(Height),
    RGBFormat(RGBFormat),
    ImageFormat(ImageFormat)
{
}

AsyncSaveImageToDiskTask::~AsyncSaveImageToDiskTask(){
    UE_LOG(LogTemp, Warning, TEXT("AsyncTaskDone"));
}


void AsyncSaveImageToDiskTask::DoWork() const
{
    UE_LOG(LogTemp, Warning, TEXT("Starting Work"));

    // Load the image wrapper module (if not already loaded)
    IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));

    // Create an image wrapper
    TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(ImageFormat);

    // Error handling
    if (!ImageWrapper.IsValid()) {
        UE_LOG(LogTemp, Error, TEXT("Failed to create IImageWrapper!"));
        return;
    }

    // Set the raw data 
    int32 PixelDepth = 8;
    if(ImageFormat == EImageFormat::EXR){ // Adjust pixel depth for EXR (float) data  // Has to be EImageFormat because already converted
        PixelDepth = 16;
    }
    UE_LOG(LogTemp, Warning, TEXT("PixelDepth: %d"), PixelDepth);
    ImageWrapper->SetRaw(RenderRequest->PixelData.GetData(), RenderRequest->PixelData.Num(), Width, Height, RGBFormat, PixelDepth);

    // Compress the image 
    /** from /Engine/Source/Runtime/ImageWrapper/Public/IImageWrapper.h
     * Enumerates available image compression qualities.
     * 
     * JPEG interprets Quality as 1-100
     * JPEG default quality is 85 , Uncompressed means 100
     * 
     * for PNG:
     * Negative qualities in [-1,-9] set PNG zlib level
     * PNG interprets "Uncompressed" as zlib level 0 (none)
     * otherwise default zlib level 3 is used.
     * 
     * EXR respects the "Uncompressed" flag to turn off compression; otherwise ZIP_COMPRESSION is used.
    */
    const TArray64<uint8>& CompressedData = ImageWrapper->GetCompressed(100);

    // Save the compressed image to disk
    FFileHelper::SaveArrayToFile(CompressedData, *FileName);

    //UE_LOG(LogTemp, Log, TEXT("Stored Image: %s"), *FileName);
}
