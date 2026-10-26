#include "RoadGenerator.h"
#include "RoadGenerationSignatures.h"

#include "Components/SceneComponent.h"
#include "ConcurrencyHelpers/Concurrency.h"
#include "OsmRoadScene.h"
#include "OSMRoadImporter.h"
#include "UObject/ConstructorHelpers.h"
#include "Algo/Reverse.h"
#include "Landscape.h"
#include "LandscapeSplineActor.h"

#if WITH_EDITOR
#include "UObject/StrongObjectPtr.h"
#include "Misc/ScopedSlowTask.h"
#include "LandscapeInfo.h"
#include "LandscapeEditLayer.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"
#include "Engine/World.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogRoadConstructorImport, Log, All);

namespace RoadGeneratorInternal
{
#if WITH_EDITOR
	namespace
	{
		// Used only when regenerating Landscape splines: detach old output, delete it on
		// Commit(), or restore it on failure. Road generation and clearing do not use this.
		class FScopedLandscapeSplineReplacement
		{
		public:
			FScopedLandscapeSplineReplacement(ULandscapeSplinesComponent* InSplines, ALandscape* InLandscape,
			                                  const TSet<FString>& Prefixes, bool bAllObjects = false)
				: Splines(InSplines), Landscape(InLandscape)
			{
				if (!Splines.IsValid()) return;
				auto IsPreviousOutput = [&](const UObject* Object)
				{
					if (!Object) return false;
					if (bAllObjects) return true;
					for (const FString& Prefix : Prefixes)
						if (Object->GetName().StartsWith(Prefix)) return true;
					return false;
				};
				Splines->Modify();
				auto& CurrentSegments = Splines->GetSegments();
				for (int32 Index = CurrentSegments.Num() - 1; Index >= 0; --Index)
				{
					if (!IsPreviousOutput(CurrentSegments[Index])) continue;
					Segments.Emplace(CurrentSegments[Index].Get());
					CurrentSegments.RemoveAt(Index, 1, EAllowShrinking::No);
				}
				auto& CurrentPoints = Splines->GetControlPoints();
				for (int32 Index = CurrentPoints.Num() - 1; Index >= 0; --Index)
				{
					if (!IsPreviousOutput(CurrentPoints[Index])) continue;
					Points.Emplace(CurrentPoints[Index].Get());
					CurrentPoints.RemoveAt(Index, 1, EAllowShrinking::No);
				}
				Refresh();
			}

			~FScopedLandscapeSplineReplacement() { Restore(); }

			void Restore()
			{
				if (bFinalized) return;
				if (Splines.IsValid())
				{
					for (const auto& Point : Points) Splines->GetControlPoints().AddUnique(Point.Get());
					for (const auto& Segment : Segments) Splines->GetSegments().AddUnique(Segment.Get());
				}
				bFinalized = true;
				Refresh();
			}

			void Commit()
			{
				if (bFinalized) return;
				for (const auto& Segment : Segments)
				{
					Segment->Modify();
					for (FLandscapeSplineSegmentConnection& Connection : Segment->Connections)
					{
						if (ULandscapeSplineControlPoint* Point = Connection.ControlPoint)
						{
							Point->Modify();
							Point->ConnectedSegments.RemoveAll([&](const FLandscapeSplineConnection& Link) { return Link.Segment == Segment.Get(); });
						}
					}
					Segment->ConditionalBeginDestroy();
				}
				for (const auto& Point : Points)
				{
					Point->Modify();
					Point->ConnectedSegments.Reset();
					Point->ConditionalBeginDestroy();
				}
				bFinalized = true;
				Refresh();
			}

		private:
			void Refresh() const
			{
				if (Splines.IsValid()) Splines->MarkRenderStateDirty();
				if (Landscape.IsValid())
				{
					Landscape->RequestSplineLayerUpdate();
					(void)Landscape->MarkPackageDirty();
				}
			}

			TStrongObjectPtr<ULandscapeSplinesComponent> Splines;
			TWeakObjectPtr<ALandscape> Landscape;
			TArray<TStrongObjectPtr<ULandscapeSplineControlPoint>> Points;
			TArray<TStrongObjectPtr<ULandscapeSplineSegment>> Segments;
			bool bFinalized = false;
		};
	}
#endif

	static int32 ReadLaneCount(const TMap<FString, FString>& Tags)
	{
		const FString* Value = Tags.Find(TEXT("lanes"));
		if (!Value) return 0;
		const FString Text = Value->TrimStartAndEnd();
		if (Text.IsEmpty()) return 0;
		int32 Count = 0;
		for (const TCHAR Ch : Text)
		{
			if (Ch < TEXT('0') || Ch > TEXT('9')) return 0;
			Count = Count * 10 + Ch - TEXT('0');
			// Bound allocations and reject malformed/overflowing source values.
			if (Count > 64) return 0;
		}
		return Count;
	}

	// Keep the existing lane definitions in order. Extend the outermost driving
	// lane before the sidewalk/shoulder, borrowing from the other side if needed.
	static void ResizeDrivingLanes(TArray<FRoadLaneStyle>& Lanes, const int32 Count, const FRoadLaneStyle& Fallback)
	{
		TArray<FRoadLaneStyle> Result;
		int32 Kept = 0;
		int32 InsertAt = 0;
		FRoadLaneStyle Template = Fallback;
		for (const FRoadLaneStyle& Lane : Lanes)
		{
			if (Lane.LaneType == ELaneType::Driving)
			{
				if (Kept >= Count) continue;
				++Kept;
				Template = Lane;
				Result.Add(Lane);
				InsertAt = Result.Num();
			}
			else Result.Add(Lane);
		}
		for (; Kept < Count; ++Kept) Result.Insert(Template, InsertAt++);
		Lanes = MoveTemp(Result);
	}

	static bool GetLaneCountLayout(const URoadStyle* Source, const int32 LaneCount,
		TArray<FRoadLaneStyle>& Left, TArray<FRoadLaneStyle>& Right)
	{
		if (!Source) return false;
		Left = Source->LeftLanes;
		Right = Source->RightLanes;
		if (LaneCount <= 0) return false;
		auto IsDriving = [](const FRoadLaneStyle& Lane) { return Lane.LaneType == ELaneType::Driving; };
		const FRoadLaneStyle* Template = Source->LeftLanes.FindByPredicate(IsDriving);
		if (!Template) Template = Source->RightLanes.FindByPredicate(IsDriving);
		if (!Template) return false; // No driving attributes to inherit from this style.
		ResizeDrivingLanes(Left, LaneCount / 2 + LaneCount % 2, *Template);
		ResizeDrivingLanes(Right, LaneCount / 2, *Template);
		return true;
	}

	URoadStyle* CreateLaneCountStyle(UObject* Outer, const URoadStyle* Source, const int32 LaneCount)
	{
		if (!IsValid(Source)) return nullptr;
		URoadStyle* Style = DuplicateObject<URoadStyle>(Source, Outer);
		Style->ClearFlags(RF_Standalone);
		// World Partition roads can live in another package than their owning scene.
		Style->SetFlags(RF_Public | RF_Transactional);
		TArray<FRoadLaneStyle> Left, Right;
		if (GetLaneCountLayout(Source, LaneCount, Left, Right))
		{
			Style->LeftLanes = MoveTemp(Left);
			Style->RightLanes = MoveTemp(Right);
		}
		return Style;
	}

	static bool MatchesHighwayFilter(const FImportedRoadWay& Way, const TArray<FName>& Filter)
	{
		const FString* Highway = Way.Tags.Find(TEXT("highway"));
		return Highway && !Highway->IsEmpty() && (Filter.IsEmpty() || Filter.Contains(FName(**Highway)));
	}

	static FString NodeKey(const FImportedRoadWay& Way, const FImportedRoadPoint& Point)
	{
		if (Point.NodeId <= 0) return FString();
		const FString* Layer = Way.Tags.Find(TEXT("layer"));
		return FString::Printf(TEXT("%lld:%d"), Point.NodeId, Layer ? FCString::Atoi(**Layer) : 0);
	}

#if WITH_EDITOR
	static FString GetRoadSplinePrefix(const ARoadActor* Road)
	{
		FString Key;
		for (const TCHAR Ch : Road->GetName().Left(80))
			Key.AppendChar((FChar::IsAlnum(Ch) || Ch == static_cast<TCHAR>('_')) ? Ch : static_cast<TCHAR>('_'));
		return FString::Printf(TEXT("RC_LandscapeSplinePrototype_%s_"), *Key);
	}

	static TSet<FString> GetRoadSplinePrefixes(const AOsmRoadScene* Scene)
	{
		TSet<FString> Prefixes;
		if (!IsValid(Scene)) return Prefixes;
		for (const ARoadActor* Road : Scene->Roads)
		{
			if (!IsValid(Road)) continue;
			Prefixes.Add(GetRoadSplinePrefix(Road));
		}
		return Prefixes;
	}

	static void FlushLandscapeSplineChanges(ALandscape* PreviousLandscape, ALandscape* TargetLandscape)
	{
		auto rebuild = [](ALandscape* Landscape)
		{
			if (!IsValid(Landscape)) return;
			// The vendor also stamps deformation directly into the reserved layer. Clear
			// every component and reapply remaining splines, including other owners' splines.
			if (Landscape->FindEditLayerOfTypeConst(ULandscapeEditLayerSplines::StaticClass()))
				Landscape->UpdateAllLandscapeSplines(FGuid(), true);
			Landscape->ForceLayersFullUpdate();
			(void)Landscape->MarkPackageDirty();
		};
		rebuild(PreviousLandscape);
		if (TargetLandscape != PreviousLandscape) rebuild(TargetLandscape);
	}

	// Road Constructor stores its persistent splines on the Landscape, outside the road actor.
	// Match only the objects its public Terrain Deform command creates for our roads.
	static void RemoveRoadConstructorSplines(const TSet<FString>& Prefixes, ALandscape* Landscape)
	{
		if (Prefixes.IsEmpty() || !IsValid(Landscape)) return;
		ULandscapeSplinesComponent* Splines = Landscape->GetSplinesComponent();
		if (!Splines) return;
		auto is_ours = [&Prefixes](const UObject* Object)
		{
			if (!Object) return false;
			for (const FString& Prefix : Prefixes)
				if (Object->GetName().StartsWith(Prefix)) return true;
			return false;
		};
		Splines->Modify();
		auto& Segments = Splines->GetSegments();
		for (int32 Index = Segments.Num() - 1; Index >= 0; --Index)
		{
			ULandscapeSplineSegment* Segment = Segments[Index];
			if (!is_ours(Segment)) continue;
			Segment->Modify();
			for (FLandscapeSplineSegmentConnection& Connection : Segment->Connections)
			{
				if (ULandscapeSplineControlPoint* Point = Connection.ControlPoint)
				{
					Point->Modify();
					Point->ConnectedSegments.RemoveAll([Segment](const FLandscapeSplineConnection& Link) { return Link.Segment == Segment; });
				}
			}
			Segments.RemoveAt(Index, 1, EAllowShrinking::No);
			Segment->ConditionalBeginDestroy();
		}
		auto& Points = Splines->GetControlPoints();
		for (int32 Index = Points.Num() - 1; Index >= 0; --Index)
		{
			ULandscapeSplineControlPoint* Point = Points[Index];
			if (!is_ours(Point)) continue;
			Point->Modify();
			Point->ConnectedSegments.Reset();
			Points.RemoveAt(Index, 1, EAllowShrinking::No);
			Point->ConditionalBeginDestroy();
		}
		Splines->MarkRenderStateDirty();
		Landscape->RequestSplineLayerUpdate();
		[[maybe_unused]] bool Marked = Landscape->MarkPackageDirty();
	}

	static bool HasRoadConstructorSpline(const ARoadActor* Road, const ALandscape* Landscape)
	{
		if (!Road || !Landscape || !Landscape->GetSplinesComponent()) return false;
		const FString Prefix = GetRoadSplinePrefix(Road);
		for (const ULandscapeSplineControlPoint* Point : Landscape->GetSplinesComponent()->GetControlPoints())
			if (Point && Point->GetName().StartsWith(Prefix)) return true;
		return false;
	}

#endif

	TArray<FImportSpan> PrepareSpans(const TArray<FImportedRoadWay>& Ways, const TArray<FName>& Filter, bool bRequireTerrain, double HeightOffset, double MergeDistance)
	{
		TArray<const FImportedRoadWay*> Selected;
		TMap<FString, int32> Occurrences;
		for (const FImportedRoadWay& Way : Ways)
		{
			if (!MatchesHighwayFilter(Way, Filter)) continue;
			Selected.Add(&Way);
			for (const FImportedRoadPoint& Point : Way.Points)
			{
				if (Point.WorldLocation.ContainsNaN() || (bRequireTerrain && !Point.bHasTerrain)) continue;
				if (const FString Key = NodeKey(Way, Point); !Key.IsEmpty()) ++Occurrences.FindOrAdd(Key);
			}
		}

		TArray<FImportSpan> Result;
		for (const FImportedRoadWay* Way : Selected)
		{
			FImportSpan Span;
			auto reset_span = [&]()
			{
				Span = FImportSpan();
				Span.WayId = Way->WayId;
				Span.SourceWayIds.Add(Way->WayId);
				Span.Highway = FName(*Way->Tags.FindChecked(TEXT("highway")));
				Span.LaneCount = ReadLaneCount(Way->Tags);
			};
			auto flush_span = [&]()
			{
				if (Span.Positions.Num() >= 2) Result.Add(MoveTemp(Span));
				reset_span();
			};
			reset_span();
			for (const FImportedRoadPoint& Point : Way->Points)
			{
				if (Point.WorldLocation.ContainsNaN() || (bRequireTerrain && !Point.bHasTerrain))
				{
					flush_span();
					continue;
				}
				FVector Position = Point.WorldLocation;
				Position.Z += HeightOffset;
				if (!Span.Positions.IsEmpty() && FVector::DistSquared2D(Span.Positions.Last(), Position) < 1.0) continue;
				const FString Key = NodeKey(*Way, Point);
				if (Span.Positions.IsEmpty()) Span.StartKey = Key;
				Span.Positions.Add(Position);
				Span.EndKey = Key;
				if (const int32* Count = Occurrences.Find(Key); Count && *Count > 1 && Span.Positions.Num() >= 2)
				{
					flush_span();
					Span.StartKey = Span.EndKey = Key;
					Span.Positions.Add(Position);
				}
			}
			flush_span();
		}

		// A closed way needs two distinct source actors: the vendor intentionally
		// suppresses same-road self-crossing junction geometry.
		for (int32 Index = Result.Num() - 1; Index >= 0; --Index)
		{
			FImportSpan& Span = Result[Index];
			if (Span.Positions.Num() < 4 || !Span.Positions[0].Equals(Span.Positions.Last(), 1.0)) continue;
			const int32 Middle = Span.Positions.Num() / 2;
			FImportSpan Tail;
			Tail.WayId = Span.WayId;
			Tail.SourceWayIds = Span.SourceWayIds;
			Tail.Highway = Span.Highway;
			Tail.LaneCount = Span.LaneCount;
			Tail.StartKey = FString::Printf(TEXT("loop:%lld:%d"), Span.WayId, Index);
			Tail.EndKey = Span.EndKey;
			Tail.Positions.Append(Span.Positions.GetData() + Middle, Span.Positions.Num() - Middle);
			Span.EndKey = Tail.StartKey;
			Span.Positions.SetNum(Middle + 1);
			Result.Add(MoveTemp(Tail));
		}
		// Native junction gates need enough road length for their approach. Contract
		// tiny corridors between branching nodes rather than letting both ends disappear.
		TMap<FString, int32> Degree;
		for (const FImportSpan& Span : Result)
		{
			if (!Span.StartKey.IsEmpty()) ++Degree.FindOrAdd(Span.StartKey);
			if (!Span.EndKey.IsEmpty()) ++Degree.FindOrAdd(Span.EndKey);
		}
		TMap<FString, FString> Parent;
		auto root = [&Parent](FString Key)
		{
			while (const FString* Next = Parent.Find(Key)) Key = *Next;
			return Key;
		};
		TSet<int32> Contracted;
		for (int32 Index = 0; Index < Result.Num() && MergeDistance > 0; ++Index)
		{
			const FImportSpan& Span = Result[Index];
			if (Degree.FindRef(Span.StartKey) < 3 || Degree.FindRef(Span.EndKey) < 3 || Span.StartKey == Span.EndKey) continue;
			double Length = 0;
			for (int32 Point = 1; Point < Span.Positions.Num(); ++Point) Length += FVector::Dist2D(Span.Positions[Point - 1], Span.Positions[Point]);
			if (Length > MergeDistance) continue;
			const FString Start = root(Span.StartKey);
			const FString End = root(Span.EndKey);
			if (Start != End) Parent.Add(End, Start);
			Contracted.Add(Index);
		}
		for (int32 Index = Result.Num() - 1; Index >= 0; --Index)
		{
			if (Contracted.Contains(Index)) Result.RemoveAt(Index);
			else
			{
				Result[Index].StartKey = root(Result[Index].StartKey);
				Result[Index].EndKey = root(Result[Index].EndKey);
			}
		}
		return Result;
	}

	static bool HasSymmetricLaneStyle(const URoadStyle* Style, const int32 LaneCount)
	{
		if (!Style) return true;
		TArray<FRoadLaneStyle> LeftLanes, RightLanes;
		GetLaneCountLayout(Style, LaneCount, LeftLanes, RightLanes);
		if (LeftLanes.Num() != RightLanes.Num()) return false;
		for (int32 Index = 0; Index < LeftLanes.Num(); ++Index)
		{
			const FRoadLaneStyle& Left = LeftLanes[Index];
			const FRoadLaneStyle& Right = RightLanes[Index];
			if (Left.Width != Right.Width || Left.LaneType != Right.LaneType || Left.PhysicalMaterial != Right.PhysicalMaterial ||
				Left.LaneShapeSource != Right.LaneShapeSource || Left.LaneShape != Right.LaneShape ||
				Left.LaneMarking != Right.LaneMarking || Left.Props != Right.Props) return false;
		}
		return true;
	}

	int32 MergeStraightSpans(TArray<FImportSpan>& Spans, const double AngleTolerance, const TFunctionRef<URoadStyle*(const FImportSpan&)>& ResolveStyle)
	{
		struct FEndpoint { int32 SpanIndex; bool bStart; };
		TMap<FString, TArray<FEndpoint>> Endpoints;
		for (int32 Index = 0; Index < Spans.Num(); ++Index)
		{
			if (!Spans[Index].StartKey.IsEmpty()) Endpoints.FindOrAdd(Spans[Index].StartKey).Add({ Index, true });
			if (!Spans[Index].EndKey.IsEmpty()) Endpoints.FindOrAdd(Spans[Index].EndKey).Add({ Index, false });
		}
		TArray<FString> Keys;
		Endpoints.GetKeys(Keys);
		Keys.Sort();
		TArray<bool> Active;
		Active.Init(true, Spans.Num());
		const double MaxDot = -FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(AngleTolerance, 0.0, 45.0)));
		int32 MergeCount = 0;
		for (const FString& Key : Keys)
		{
			const TArray<FEndpoint>& Arms = Endpoints.FindChecked(Key);
			if (Arms.Num() != 2 || Arms[0].SpanIndex == Arms[1].SpanIndex) continue;
			FEndpoint First = Arms[0], Second = Arms[1];
			if (!Active[First.SpanIndex] || !Active[Second.SpanIndex]) continue;
			// Prefer end->start without changing either source's orientation.
			if (First.bStart && !Second.bStart) Swap(First, Second);
			FImportSpan& A = Spans[First.SpanIndex];
			FImportSpan& B = Spans[Second.SpanIndex];
			if (A.Positions.Num() < 2 || B.Positions.Num() < 2) continue;
			URoadStyle* Style = ResolveStyle(A);
			if (Style != ResolveStyle(B) || A.LaneCount != B.LaneCount) continue;
			if ((First.bStart || !Second.bStart) && !HasSymmetricLaneStyle(Style, A.LaneCount)) continue;
			const FVector& JoinA = First.bStart ? A.Positions[0] : A.Positions.Last();
			const FVector& JoinB = Second.bStart ? B.Positions[0] : B.Positions.Last();
			// Contracted junction aliases may share a key but be physically separated.
			if (FVector::DistSquared(JoinA, JoinB) > 1.0) continue;
			const FVector& NextA = First.bStart ? A.Positions[1] : A.Positions[A.Positions.Num() - 2];
			const FVector& NextB = Second.bStart ? B.Positions[1] : B.Positions[B.Positions.Num() - 2];
			const FVector2D OutA = FVector2D(NextA - JoinA).GetSafeNormal();
			const FVector2D OutB = FVector2D(NextB - JoinB).GetSafeNormal();
			if (OutA.IsNearlyZero() || OutB.IsNearlyZero() || FVector2D::DotProduct(OutA, OutB) > MaxDot + 1.e-8) continue;
			const FString NewStart = First.bStart ? A.EndKey : A.StartKey;
			const FString NewEnd = Second.bStart ? B.EndKey : B.StartKey;
			// Keep closed loops split: native same-road junction geometry is suppressed.
			if (!NewStart.IsEmpty() && NewStart == NewEnd) continue;
			if (First.bStart) Algo::Reverse(A.Positions);
			if (!Second.bStart) Algo::Reverse(B.Positions);
			A.Positions.Append(B.Positions.GetData() + 1, B.Positions.Num() - 1);
			for (int64 Id : B.SourceWayIds) A.SourceWayIds.AddUnique(Id);
			A.StartKey = NewStart;
			A.EndKey = NewEnd;
			Active[Second.SpanIndex] = false;
			// Update only the two surviving ends, so entire chains merge in one pass.
			for (bool bStart : { true, false })
			{
				const FString& OuterKey = bStart ? A.StartKey : A.EndKey;
				if (OuterKey.IsEmpty()) continue;
				TArray<FEndpoint>& OuterArms = Endpoints.FindChecked(OuterKey);
				OuterArms.RemoveAll([&](const FEndpoint& Endpoint) { return Endpoint.SpanIndex == First.SpanIndex || Endpoint.SpanIndex == Second.SpanIndex; });
				OuterArms.Add({ First.SpanIndex, bStart });
			}
			Endpoints.FindChecked(Key).Reset();
			++MergeCount;
		}
		for (int32 Index = Spans.Num() - 1; Index >= 0; --Index)
		{
			if (!Active[Index]) Spans.RemoveAt(Index);
		}
		return MergeCount;
	}
}

using namespace RoadGeneratorInternal;

ARoadGenerator::ARoadGenerator()
{
	PrimaryActorTick.bCanEverTick = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	static ConstructorHelpers::FObjectFinder<URoadStyle> StreetStyle(TEXT("/RoadConstructor/RoadStyles/RS_Street_2Lane.RS_Street_2Lane"));
	DefaultRoadStyle = StreetStyle.Object;
	HighwayFilter = {
		TEXT("motorway"), TEXT("trunk"), TEXT("primary"), TEXT("secondary"), TEXT("tertiary"),
		TEXT("unclassified"), TEXT("residential"), TEXT("living_street"),
		TEXT("motorway_link"), TEXT("trunk_link"), TEXT("primary_link"), TEXT("secondary_link"), TEXT("tertiary_link")
	};
}

void ARoadGenerator::GenerateRoads()
{
#if WITH_EDITOR
	if (bGenerationInProgress) return;
	bGenerationInProgress = true;
	GenerateFromGameThread(NAME_None, true, [WeakThis = TWeakObjectPtr<ARoadGenerator>(this)](bool)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis]()
		{
			if (WeakThis.IsValid())
			{
				WeakThis->bGenerationInProgress = false;
			}
		});
	});
#endif
}

void ARoadGenerator::ClearRoads()
{
	if (!bGenerationInProgress) Execute_Cleanup(this, true);
}

void ARoadGenerator::GenerateLandscapeSplines()
{
#if WITH_EDITOR
	if (bGenerationInProgress) return;
	TGuardValue<bool> GeneratingGuard(bGenerationInProgress, true);
	GenerateLandscapeSplinesInternal();
#endif
}

void ARoadGenerator::ClearLandscapeSplines()
{
#if WITH_EDITOR
	if (bGenerationInProgress) return;
	TGuardValue<bool> GeneratingGuard(bGenerationInProgress, true);
	ClearLandscapeSplinesInternal();
#endif
}

void ARoadGenerator::RememberLandscapeSplineOwnership()
{
#if WITH_EDITOR
	// Migrate ownership from levels saved before splines were independently tracked.
	if (GeneratedSplinePrefixes.IsEmpty() && !GeneratedSplineLandscape.IsNull() && GeneratedScene.IsValid())
	{
		Modify();
		GeneratedSplinePrefixes = GetRoadSplinePrefixes(GeneratedScene.Get());
	}
#endif
}

bool ARoadGenerator::GenerateLandscapeSplinesInternal()
{
#if WITH_EDITOR
	if (!GeneratedScene.IsValid() || !IsValid(TargetLandscape) || !TargetLandscape->GetLandscapeInfo())
	{
		UE_LOG(LogRoadConstructorImport, Error,
		       TEXT("Generate Roads first and select a loaded Target Landscape before generating Landscape splines."));
		return false;
	}
	if ((!GeneratedSplineLandscape.IsNull() && !GeneratedSplineLandscape.IsValid()) ||
		(!GeneratedLandscapeSplineActor.IsNull() && !GeneratedLandscapeSplineActor.IsValid()))
	{
		UE_LOG(LogRoadConstructorImport, Error,
		       TEXT("Load the previous Landscape and legacy spline actor before replacing Landscape splines."));
		return false;
	}
	if (!FMath::IsFinite(LandscapeHeightOffsetCm) || !FMath::IsFinite(LandscapeSplineSideFalloffCm) ||
		LandscapeSplineSideFalloffCm < 0.0 ||
		!FMath::IsFinite(LandscapeSplineEndFalloffCm) || LandscapeSplineEndFalloffCm < 0.0) return false;
	for (const ARoadActor* Road : GeneratedScene->Roads)
	{
		if (!IsValid(Road) || !Road->BaseCurve || Road->BaseCurve->Curve.Points.Num() < 2)
		{
			UE_LOG(LogRoadConstructorImport, Error, TEXT("Load all generated roads before generating Landscape splines."));
			return false;
		}
	}
	if (GeneratedScene->Roads.IsEmpty()) return false;
	const FString SplineInputs = RoadGenerationSignatures::LandscapeInputs(*this, *GeneratedScene.Get());
	if (!CachedSplineOutput.IsEmpty() && CachedSplineInputs == SplineInputs && GeneratedSplineLandscape.Get() == TargetLandscape.Get() &&
		CachedSplineOutput == RoadGenerationSignatures::LandscapeOutput(TargetLandscape, GeneratedSplinePrefixes))
	{
		UE_LOG(LogRoadConstructorImport, Display, TEXT("Landscape spline inputs and output are unchanged; skipped generation."));
		return true;
	}
	if (!EnsureLandscapeSplineLayer()) return false;
	RememberLandscapeSplineOwnership();
	ALandscape* PreviousLandscape = GeneratedSplineLandscape.Get();
	TUniquePtr<FScopedLandscapeSplineReplacement> PreviousSplines;
	if (IsValid(PreviousLandscape))
		PreviousSplines = MakeUnique<FScopedLandscapeSplineReplacement>(PreviousLandscape->GetSplinesComponent(),
		                                                                PreviousLandscape, GeneratedSplinePrefixes);
	TUniquePtr<FScopedLandscapeSplineReplacement> LegacySplines;
	if (GeneratedLandscapeSplineActor.IsValid())
		LegacySplines = MakeUnique<FScopedLandscapeSplineReplacement>(
			GeneratedLandscapeSplineActor->GetSplinesComponent(), PreviousLandscape, TSet<FString>(), true);
	FlushLandscapeSplineChanges(PreviousLandscape, TargetLandscape);
	if (!BuildLandscapeSplines(GeneratedScene.Get()))
	{
		RemoveRoadConstructorSplines(GetRoadSplinePrefixes(GeneratedScene.Get()), TargetLandscape);
		if (PreviousSplines) PreviousSplines->Restore();
		if (LegacySplines) LegacySplines->Restore();
		FlushLandscapeSplineChanges(PreviousLandscape, TargetLandscape);
		return false;
	}
	if (PreviousSplines) PreviousSplines->Commit();
	if (LegacySplines) LegacySplines->Commit();
	if (GeneratedLandscapeSplineActor.IsValid()) GeneratedLandscapeSplineActor->Destroy();
	Modify();
	GeneratedLandscapeSplineActor.Reset();
	GeneratedSplineLandscape = TargetLandscape;
	GeneratedSplinePrefixes = GetRoadSplinePrefixes(GeneratedScene.Get());
	FlushLandscapeSplineChanges(PreviousLandscape, TargetLandscape);
	CachedSplineInputs = RoadGenerationSignatures::LandscapeInputs(*this, *GeneratedScene.Get());
	CachedSplineOutput = RoadGenerationSignatures::LandscapeOutput(TargetLandscape, GeneratedSplinePrefixes);
	(void)MarkPackageDirty();
	return true;
#else
	return false;
#endif
}

bool ARoadGenerator::ClearLandscapeSplinesInternal()
{
#if WITH_EDITOR
	if ((!GeneratedSplineLandscape.IsNull() && !GeneratedSplineLandscape.IsValid()) ||
		(!GeneratedLandscapeSplineActor.IsNull() && !GeneratedLandscapeSplineActor.IsValid()))
	{
		UE_LOG(LogRoadConstructorImport, Error,
		       TEXT("Load the previous Landscape and legacy spline actor before clearing Landscape splines."));
		return false;
	}
	RememberLandscapeSplineOwnership();
	ALandscape* PreviousLandscape = GeneratedSplineLandscape.Get();
	RemoveRoadConstructorSplines(GeneratedSplinePrefixes, PreviousLandscape);
	if (GeneratedLandscapeSplineActor.IsValid()) GeneratedLandscapeSplineActor->Destroy();
	Modify();
	GeneratedLandscapeSplineActor.Reset();
	GeneratedSplineLandscape.Reset();
	GeneratedSplinePrefixes.Reset();
	CachedSplineInputs.Reset();
	CachedSplineOutput.Reset();
	FlushLandscapeSplineChanges(PreviousLandscape, nullptr);
	(void)MarkPackageDirty();
	return true;
#else
	return false;
#endif
}

bool ARoadGenerator::EnsureLandscapeSplineLayer() const
{
#if WITH_EDITOR
	if (!IsValid(TargetLandscape) || !TargetLandscape->GetLandscapeInfo()) return false;
	// UE 5.8 GetEditLayersOfType currently returns all layers without filtering.
	ULandscapeEditLayerBase* SplineLayer = TargetLandscape->FindEditLayerOfType(ULandscapeEditLayerSplines::StaticClass());
	// Road Constructor recognizes these reserved layer names when applying deformation.
	const FName SupportedNames[] = {
		TEXT("Splines"), TEXT("Landscape Splines"), TEXT("Spline"), TEXT("Landscape Spline")
	};
	if (SplineLayer)
	{
		for (FName Name : SupportedNames)
			if (SplineLayer->GetName() == Name) return true;
	}
	else if (!bCreateSplineLayerIfMissing)
	{
		UE_LOG(LogRoadConstructorImport, Error,
		       TEXT(
			       "Create a reserved Landscape Splines edit layer on Target Landscape, or enable Create Spline Layer If Missing."
		       ));
		return false;
	}
	for (FName Name : SupportedNames)
	{
		if (TargetLandscape->GetEditLayerConst(Name)) continue;
		TargetLandscape->Modify();
		if (SplineLayer)
		{
			SplineLayer->SetName(Name, true);
			return true;
		}
		if (TargetLandscape->CreateLayer(Name, ULandscapeEditLayerSplines::StaticClass()) != INDEX_NONE) return true;
		break;
	}
	UE_LOG(LogRoadConstructorImport, Error,
	       TEXT("Could not prepare a reserved Landscape spline edit layer with a name supported by Road Constructor."));
	return false;
#else
	return false;
#endif
}

bool ARoadGenerator::BuildLandscapeSplines(AOsmRoadScene* Scene) const
{
#if WITH_EDITOR
	if (!IsValid(Scene) || !EnsureLandscapeSplineLayer()) return false;
	const FName SplineLayerName = TargetLandscape->FindEditLayerOfTypeConst(ULandscapeEditLayerSplines::StaticClass())->GetName();
	int32 CreatedRoads = 0;
	for (ARoadActor* Road : Scene->Roads)
	{
		if (!IsValid(Road)) continue;
		Road->Modify();
		Road->bEnableTerrainDeform = true;
		Road->TerrainDeformTargetLandscapeSplineEditLayer = SplineLayerName;
		// The native command supplies topology and XY samples. Its roadbed heights
		// are replaced below using the imported baseline, independent of mesh offset.
		Road->TerrainDeformLandscapeSplineHeightOffset = 0.0;
		Road->CreatePersistentLandscapeSplineEditLayerPrototype();
		if (!HasRoadConstructorSpline(Road, TargetLandscape))
		{
			UE_LOG(LogRoadConstructorImport, Error,
			       TEXT("Road Constructor failed to create a Landscape spline for %s on Target Landscape."),
			       *GetNameSafe(Road));
			RemoveRoadConstructorSplines(GetRoadSplinePrefixes(Scene), TargetLandscape);
			return false;
		}
		// Use only Driving and Shoulder widths, with explicit falloff values,
		// after the plugin creates the native control points.
		double RoadWidth = 0.0;
		if (const URoadStyle* Style = Road->LastAppliedRoadStyle)
		{
			for (const TArray<FRoadLaneStyle>* Lanes : { &Style->LeftLanes, &Style->RightLanes })
				for (const FRoadLaneStyle& Lane : *Lanes)
					if ((Lane.LaneType == ELaneType::Driving || Lane.LaneType == ELaneType::Shoulder) && FMath::IsFinite(Lane.Width))
						RoadWidth += FMath::Max(0.0, Lane.Width);
		}
		const FString Prefix = GetRoadSplinePrefix(Road);
		TSet<ULandscapeSplineSegment*> SegmentsToUpdate;
		for (ULandscapeSplineControlPoint* Point : TargetLandscape->GetSplinesComponent()->GetControlPoints())
		{
			if (!Point || !Point->GetName().StartsWith(Prefix)) continue;
			Point->Modify();
			ULandscapeSplinesComponent* Splines = TargetLandscape->GetSplinesComponent();
			FVector WorldPosition = Splines->GetComponentTransform().TransformPosition(Point->Location);
			const double Distance = FMath::Clamp(Road->BaseCurve->Curve.GetUV(WorldPosition).X, 0.0, Road->Length());
			WorldPosition.Z = Road->GetHeight(Distance) - Scene->ImportedSurfaceOffsetCm + LandscapeHeightOffsetCm;
			Point->Location = Splines->GetComponentTransform().InverseTransformPosition(WorldPosition);
			const FVector Direction = Splines->GetComponentTransform().InverseTransformVectorNoScale(Road->GetDir(Distance));
			if (!Direction.IsNearlyZero()) Point->Rotation = Direction.Rotation();
			Point->Width = static_cast<float>(RoadWidth * 0.5); // Native Width is a half-width.
			Point->SideFalloff = static_cast<float>(LandscapeSplineSideFalloffCm);
			Point->EndFalloff = Point->ConnectedSegments.Num() <= 1 ? static_cast<float>(LandscapeSplineEndFalloffCm) : 0.0f;
			for (const FLandscapeSplineConnection& Connection : Point->ConnectedSegments)
				if (Connection.Segment) SegmentsToUpdate.Add(Connection.Segment);
			Point->UpdateSplinePoints(false, false);
		}
		for (ULandscapeSplineSegment* Segment : SegmentsToUpdate)
		{
			Segment->Modify();
			Segment->UpdateSplinePoints();
		}
		++CreatedRoads;
	}
	if (CreatedRoads == 0) return false;
	// Discard the plugin's wider direct stamps and rebuild from the final native
	// spline dimensions, including existing splines belonging to other generators.
	FlushLandscapeSplineChanges(nullptr, TargetLandscape);
	[[maybe_unused]] bool LandscapeMarked = TargetLandscape->MarkPackageDirty();
	[[maybe_unused]] bool Marked =MarkPackageDirty();
	UE_LOG(LogRoadConstructorImport, Display, TEXT("Applied Road Constructor Terrain Deform to %d generated roads."),
	       CreatedRoads);
	return true;
#else
	return false;
#endif
}
TArray<UObject*> ARoadGenerator::GetGeneratedObjects() const
{
	TArray<UObject*> Objects;
	if (GeneratedScene.IsValid()) Objects.Add(GeneratedScene.Get());
	return Objects;
}

bool ARoadGenerator::Cleanup_Implementation(const bool bSkipPrompt)
{
	if (!GeneratedScene.IsNull() && !GeneratedScene.IsValid())
	{
		UE_LOG(LogRoadConstructorImport, Error, TEXT("Load the previous Road Constructor scene before clearing roads."));
		return false;
	}
	RememberLandscapeSplineOwnership();
	if (!DeleteGeneratedObjects(bSkipPrompt)) return false;
	Modify();
	GeneratedScene.Reset();
	CachedRoadInputs.Reset();
	CachedRoadOutput.Reset();
	return true;
}

bool ARoadGenerator::OnGenerate(const FName SpawnedActorsPathOverride, bool bIsUserInitiated)
{
#if WITH_EDITOR
	const bool bConfigured = Concurrency::RunOnGameThreadAndWait([this]()
	{
		return IsValid(OsmRoadImporter) && IsValid(DefaultRoadStyle);
	});
	if (!bConfigured)
	{
		UE_LOG(LogRoadConstructorImport, Error, TEXT("Select an OSMRoadImporter with cached data and a RoadStyle."));
		return false;
	}
	TArray<FImportedRoadWay> Ways;
	if (!OsmRoadImporter->GetCachedRoadData(Ways)) return false;
	return Concurrency::RunOnGameThreadAndWait([&]()
	{
		return BuildRoads(Ways, SpawnedActorsPathOverride);
	});
#else
	return false;
#endif
}

bool ARoadGenerator::BuildRoads(const TArray<FImportedRoadWay>& Ways, const FName OutputFolderPath)
{
#if WITH_EDITOR
	if (!GeneratedScene.IsNull() && !GeneratedScene.IsValid())
	{
		UE_LOG(LogRoadConstructorImport, Error, TEXT("Load the previous road scene before replacing roads."));
		return false;
	}
	if (!FMath::IsFinite(SurfaceOffsetCm) ||
		!FMath::IsFinite(RoadCurveRadiusCm) || !FMath::IsFinite(JunctionMergeDistanceCm)) return false;
	if (bOverrideJunctionRadius && !FMath::IsFinite(JunctionRadiusCm)) return false;
	if (!FMath::IsFinite(StraightConnectionToleranceDegrees)) return false;
	const double RoadHeightOffset = SurfaceOffsetCm;
	if (!FMath::IsFinite(RoadHeightOffset)) return false;
	const FName EffectiveFolder = OutputFolderPath.IsNone() ? GetFolderPath() : OutputFolderPath;
	const FString RoadInputs = RoadGenerationSignatures::RoadInputs(*this, Ways, EffectiveFolder);
	const FString CurrentRoadOutput = !CachedRoadOutput.IsEmpty() && CachedRoadInputs == RoadInputs
		? RoadGenerationSignatures::RoadOutput(GeneratedScene.Get()) : FString();
	if (!CachedRoadOutput.IsEmpty() && CachedRoadInputs == RoadInputs && CachedRoadOutput == CurrentRoadOutput)
	{
		UE_LOG(LogRoadConstructorImport, Display, TEXT("Road inputs and output are unchanged; skipped road generation and junction solving."));
		return true;
	}
	UE_LOG(LogRoadConstructorImport, Display, TEXT("Regenerating roads: %s."),
		CachedRoadInputs.IsEmpty() ? TEXT("no successful baseline in this session") :
		CachedRoadInputs != RoadInputs ? TEXT("source data or settings changed") : TEXT("generated output changed or is missing"));
	TArray<FImportSpan> Spans = PrepareSpans(Ways, HighwayFilter, bRequireTerrainSamples, RoadHeightOffset,
	                                         JunctionMergeDistanceCm);
	if (Spans.IsEmpty())
	{
		UE_LOG(LogRoadConstructorImport, Error,
		       TEXT("The cache contains no usable roads for the selected filter. Previous output was kept."));
		return false;
	}
	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.OverrideLevel = GetLevel();
	Params.ObjectFlags = RF_Transactional;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AOsmRoadScene* Candidate = GetWorld()->SpawnActor<AOsmRoadScene>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (!Candidate) return false;
	Candidate->bPreparingImportedRoads = true;
	Candidate->ImportedSurfaceOffsetCm = SurfaceOffsetCm;
	auto resolve_style = [this, Candidate](const FImportSpan& Span) -> URoadStyle*
	{
		if (const TObjectPtr<URoadStyle>* Existing = Candidate->ImportedWayStyles.Find(Span.WayId)) return Existing->Get();
		const TObjectPtr<URoadStyle>* Override = HighwayStyles.Find(Span.Highway);
		const URoadStyle* Base = Override && IsValid(Override->Get()) ? Override->Get() : DefaultRoadStyle.Get();
		URoadStyle* Style = CreateLaneCountStyle(Candidate, Base, Span.LaneCount);
		Candidate->ImportedWayStyles.Add(Span.WayId, Style);
		return Style;
	};
	const int32 MergeCount = bMergeStraightRoads
		                         ? MergeStraightSpans(Spans, StraightConnectionToleranceDegrees, resolve_style)
		                         : 0;
	UE_LOG(LogRoadConstructorImport, Display,
	       TEXT("Merged %d compatible straight two-arm connections before actor creation; %d road spans remain."),
	       MergeCount, Spans.Num());
	Candidate->bOverrideJunctionRadius = bOverrideJunctionRadius;
	if (bOverrideJunctionRadius) Candidate->ImportedJunctionRadius = FMath::Max(0.0, JunctionRadiusCm);
	Candidate->bCleanJunctionApproaches = bCleanJunctionApproaches;
	Candidate->bRemoveCornersWithoutJunctionSurface = bRemoveCornersWithoutJunctionSurface;
	Candidate->SetActorLabel(GetActorLabel() + TEXT("_RoadConstructor"));
	Candidate->SetFolderPath(EffectiveFolder);
	FScopedSlowTask Progress(Spans.Num() + 1,
	                         NSLOCTEXT("Landscapes", "ImportRoadConstructor", "Generating Road Constructor roads"));
	Progress.MakeDialog(false);
	for (int32 Index = 0; Index < Spans.Num(); ++Index)
	{
		Progress.EnterProgressFrame(1);
		const FImportSpan& Span = Spans[Index];
		Params.Owner = Candidate;
		AOsmRoadActor* Road = GetWorld()->SpawnActor<AOsmRoadActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!Road)
		{
			Candidate->Destroy();
			return false;
		}
		Road->AttachToActor(Candidate, FAttachmentTransformRules::KeepWorldTransform);
		Candidate->Roads.Add(Road);
		Road->InitWithStyle(resolve_style(Span), Span.Positions[0].Z);
		Road->bHasGround = false; // Terrain remains Landscape Combinator's responsibility.
		Road->ImportedPositions = Span.Positions;
		Road->ImportedCurveRadius = FMath::Clamp(RoadCurveRadiusCm, 1.0, 5000000.0);
		Road->StartNodeKey = Span.StartKey;
		Road->EndNodeKey = Span.EndKey;
		Road->Tags.AddUnique(TEXT("RC_DisableLegacyHighwaySideConnector"));
		for (const int64 WayId : Span.SourceWayIds) Road->Tags.AddUnique(FName(*FString::Printf(TEXT("OSMWay_%lld"), WayId)));
		Road->SetActorLabel(FString::Printf(TEXT("OSM_%lld_%d"), Span.WayId, Index));
		Road->SetFolderPath(EffectiveFolder);
	}
	Progress.EnterProgressFrame(1);
	Candidate->bPreparingImportedRoads = false;
	if (!Candidate->BuildImportedNetwork())
	{
		Candidate->Destroy();
		return false;
	}
	// Preserve the independently generated Landscape output and its ownership.
	RememberLandscapeSplineOwnership();
	if (GeneratedScene.IsValid()) GeneratedScene->Destroy();
	Modify();
	GeneratedScene = Candidate;
	CachedRoadInputs = RoadInputs;
	CachedRoadOutput = Candidate->LastBuiltOutputSignature;
	[[maybe_unused]] bool SceneMarked = Candidate->MarkPackageDirty();
	[[maybe_unused]] bool Marked = MarkPackageDirty();
	UE_LOG(LogRoadConstructorImport, Display,
	       TEXT(
		       "Road Constructor generated from the OSMRoadImporter cache using Highway Filter. Styles supply widths and sidewalks."
	       ));
	return true;
#else
	return false;
#endif
}
