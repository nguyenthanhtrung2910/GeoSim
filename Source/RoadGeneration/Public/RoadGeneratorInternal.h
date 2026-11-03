#pragma once

#include "CoreMinimal.h"
#if WITH_EDITOR
#include "UObject/StrongObjectPtr.h"
#endif

class ALandscape;
class AOsmRoadScene;
class ARoadActor;
class URoadStyle;
class ULandscapeSplinesComponent;
class ULandscapeSplineControlPoint;
class ULandscapeSplineSegment;
struct FImportedRoadWay;
struct FImportedRoadPoint;
struct FRoadLaneStyle;
struct FExcludedRoadTag;

namespace RoadGeneratorInternal
{
	struct FImportSpan;
	double SharedNodeBendAngle(const TArray<FVector>& A, bool bAStart, const TArray<FVector>& B, bool bBStart);
	TSet<FString> FindSharpSharedNodes(const TArray<FImportSpan>& Spans, double MaxBendAngleDegrees);
	double SpanLength(const TArray<FVector>& Positions);
	bool SplitSpanPositions(const TArray<FVector>& Positions, double Distance, TArray<FVector>& Head, TArray<FVector>& Tail);
	int32 CreateTransitionSpans(TArray<FImportSpan>& Spans, AOsmRoadScene* Owner, double TransitionLengthCm,
		const TFunctionRef<URoadStyle*(const FImportSpan&)>& ResolveStyle, double MaxBendAngleDegrees = 45.0);
	int32 ReadLaneCount(const TMap<FString, FString>& Tags);
	void ResizeDrivingLanes(TArray<FRoadLaneStyle>& Lanes, int32 Count, const FRoadLaneStyle& Fallback);
	bool GetLaneCountLayout(const URoadStyle* Source, int32 LaneCount, TArray<FRoadLaneStyle>& Left, TArray<FRoadLaneStyle>& Right);
	bool MatchesHighwayFilter(const FImportedRoadWay& Way, const TArray<FName>& Filter);
	bool MatchesExcludedRoadTags(const FImportedRoadWay& Way, const TArray<FExcludedRoadTag>& ExcludedTags);
	FString NodeKey(const FImportedRoadWay& Way, const FImportedRoadPoint& Point);
	bool HasSymmetricLaneStyle(const URoadStyle* Style, int32 LaneCount);
	bool HasMatchingLaneCountLayout(const URoadStyle* Style, int32 FirstCount, int32 SecondCount);

#if WITH_EDITOR
	FString GetRoadSplinePrefix(const ARoadActor* Road);
	TSet<FString> GetRoadSplinePrefixes(const AOsmRoadScene* Scene);
	TSet<FString> GetRoadSplinePrefixes(const TArray<AOsmRoadScene*>& Scenes);
	void FlushLandscapeSplineChanges(ALandscape* PreviousLandscape, ALandscape* TargetLandscape);
	void RemoveRoadConstructorSplines(const TSet<FString>& Prefixes, ALandscape* Landscape);
	bool HasRoadConstructorSpline(const ARoadActor* Road, const ALandscape* Landscape);

	/** Detach old spline output, committing its deletion on success or restoring it on failure. */
	class FScopedLandscapeSplineReplacement
	{
	public:
		FScopedLandscapeSplineReplacement(ULandscapeSplinesComponent* InSplines, ALandscape* InLandscape,
			const TSet<FString>& Prefixes, bool bAllObjects = false);
		~FScopedLandscapeSplineReplacement();
		void Restore();
		void Commit();

	private:
		void Refresh() const;
		TStrongObjectPtr<ULandscapeSplinesComponent> Splines;
		TWeakObjectPtr<ALandscape> Landscape;
		TArray<TStrongObjectPtr<ULandscapeSplineControlPoint>> Points;
		TArray<TStrongObjectPtr<ULandscapeSplineSegment>> Segments;
		bool bFinalized = false;
	};
#endif
}
