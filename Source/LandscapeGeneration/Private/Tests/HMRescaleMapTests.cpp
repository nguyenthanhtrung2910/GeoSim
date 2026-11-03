#if WITH_DEV_AUTOMATION_TESTS

#include "HMRescaleMap.h"
#include "GDALInterface/GDALInterface.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"

namespace
{
	class FTestRescaleMap : public HMRescaleMap
	{
	public:
		FTestRescaleMap(const FString& InOutputDir, EMapResamplingMethod Method)
			: HMRescaleMap(TEXT("Test"), 200, Method)
		{
			OutputDir = InOutputDir;
		}

		using HMRescaleMap::OnFetch;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHMRescaleMapTest,
	"Landscapes.MapDownloader.RescaleMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHMRescaleMapTest::RunTest(const FString& Parameters)
{
	const FString TestDir = FPaths::Combine(FPaths::ProjectIntermediateDir(),
		TEXT("HMRescaleMapTests"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*TestDir, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*TestDir, false, true); };

	// A horizontal slope: nearest repeats samples; bilinear must create intermediate heights.
	const FString SourceFile = FPaths::Combine(TestDir, TEXT("Slope.asc"));
	if (!TestTrue(TEXT("Create source raster"), FFileHelper::SaveStringToFile(
		TEXT("ncols 2\nnrows 2\nxllcorner 0\nyllcorner 0\ncellsize 1\n0 100\n0 100\n"),
		*SourceFile, FFileHelper::EEncodingOptions::ForceAnsi)))
	{
		return false;
	}
	// Convert once to a floating-point TIFF so every method keeps fractional heights.
	const FString InputFile = FPaths::Combine(TestDir, TEXT("Slope.tif"));
	if (!TestTrue(TEXT("Create TIFF"), GDALInterface::Translate(SourceFile, InputFile,
		{TEXT("-ot"), TEXT("Float32")})))
	{
		return false;
	}

	const EMapResamplingMethod Methods[] = {
		EMapResamplingMethod::Nearest, EMapResamplingMethod::Bilinear,
		EMapResamplingMethod::Cubic, EMapResamplingMethod::CubicSpline,
		EMapResamplingMethod::Lanczos, EMapResamplingMethod::Average,
		EMapResamplingMethod::RMS, EMapResamplingMethod::Mode
	};
	for (EMapResamplingMethod Method : Methods)
	{
		const FString OutputDir = FPaths::Combine(TestDir, FString::FromInt(static_cast<int32>(Method)));
		IFileManager::Get().MakeDirectory(*OutputDir);
		FTestRescaleMap Fetcher(OutputDir, Method);
		if (!TestTrue(TEXT("Resample raster"), Fetcher.OnFetch(TEXT("EPSG:3857"), {InputFile})))
		{
			continue;
		}
		TestEqual(TEXT("CRS preserved"), Fetcher.OutputCRS, FString(TEXT("EPSG:3857")));
		if (!TestEqual(TEXT("One output raster"), Fetcher.OutputFiles.Num(), 1)) continue;
		int Width = 0;
		int Height = 0;
		TArray<float> Values;
		if (!TestTrue(TEXT("Read resampled heights"), GDALInterface::ReadHeightmapFromFile(
			Fetcher.OutputFiles[0], Width, Height, Values))) continue;
		TestEqual(TEXT("Doubled width"), Width, 4);
		TestEqual(TEXT("Doubled height"), Height, 4);
		if (!TestEqual(TEXT("Sixteen output samples"), Values.Num(), 16)) continue;
		if (Method == EMapResamplingMethod::Nearest)
		{
			TestEqual(TEXT("Nearest repeats left height"), Values[1], 0.0f);
			TestEqual(TEXT("Nearest repeats right height"), Values[2], 100.0f);
		}
		else if (Method == EMapResamplingMethod::Bilinear)
		{
			TestTrue(TEXT("Bilinear interpolates left height"), FMath::IsNearlyEqual(Values[1], 25.0f));
			TestTrue(TEXT("Bilinear interpolates right height"), FMath::IsNearlyEqual(Values[2], 75.0f));
		}
	}
	return true;
}

#endif
