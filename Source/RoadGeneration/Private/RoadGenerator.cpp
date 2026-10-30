#include "RoadGenerator.h"
#include "RoadGeneratorInternal.h"
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
#include "UObject/UnrealType.h"
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
	FScopedLandscapeSplineReplacement::FScopedLandscapeSplineReplacement(ULandscapeSplinesComponent* InSplines, ALandscape* InLandscape,
	                                  const TSet<FString>& Prefixes, bool bAllObjects)
		: Splines(InSplines), Landscape(InLandscape)
	{
		if (!Splines.IsValid()) return;
		auto is_previous_output = [&](const UObject* Object)
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
			if (!is_previous_output(CurrentSegments[Index])) continue;
			Segments.Emplace(CurrentSegments[Index].Get());
			CurrentSegments.RemoveAt(Index, 1, EAllowShrinking::No);
		}
		auto& CurrentPoints = Splines->GetControlPoints();
		for (int32 Index = CurrentPoints.Num() - 1; Index >= 0; --Index)
		{
			if (!is_previous_output(CurrentPoints[Index])) continue;
			Points.Emplace(CurrentPoints[Index].Get());
			CurrentPoints.RemoveAt(Index, 1, EAllowShrinking::No);
		}
		Refresh();
	}

	FScopedLandscapeSplineReplacement::~FScopedLandscapeSplineReplacement() { Restore(); }

	void FScopedLandscapeSplineReplacement::Restore()
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

	void FScopedLandscapeSplineReplacement::Commit()
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

	void FScopedLandscapeSplineReplacement::Refresh() const
	{
		if (Splines.IsValid()) Splines->MarkRenderStateDirty();
		if (Landscape.IsValid())
		{
			Landscape->RequestSplineLayerUpdate();
			(void)Landscape->MarkPackageDirty();
		}
	}
#endif

	int32 ReadLaneCount(const TMap<FString, FString>& Tags)
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
	void ResizeDrivingLanes(TArray<FRoadLaneStyle>& Lanes, const int32 Count, const FRoadLaneStyle& Fallback)
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

	bool GetLaneCountLayout(const URoadStyle* Source, const int32 LaneCount,
		TArray<FRoadLaneStyle>& Left, TArray<FRoadLaneStyle>& Right)
	{
		if (!Source) return false;
		Left = Source->LeftLanes;
		Right = Source->RightLanes;
		if (LaneCount <= 0) return false;
		auto is_driving = [](const FRoadLaneStyle& Lane) { return Lane.LaneType == ELaneType::Driving; };
		const FRoadLaneStyle* Template = Source->LeftLanes.FindByPredicate(is_driving);
		if (!Template) Template = Source->RightLanes.FindByPredicate(is_driving);
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
		TArray<FRoadLaneStyle> Right;
		if (TArray<FRoadLaneStyle> Left; GetLaneCountLayout(Source, LaneCount, Left, Right))
		{
			Style->LeftLanes = MoveTemp(Left);
			Style->RightLanes = MoveTemp(Right);
		}
		return Style;
	}

	bool MatchesHighwayFilter(const FImportedRoadWay& Way, const TArray<FName>& Filter)
	{
		const FString* Highway = Way.Tags.Find(TEXT("highway"));
		return Highway && !Highway->IsEmpty() && (Filter.IsEmpty() || Filter.Contains(FName(**Highway)));
	}

	FString NodeKey(const FImportedRoadWay& Way, const FImportedRoadPoint& Point)
	{
		if (Point.NodeId <= 0) return FString();
		const FString* Layer = Way.Tags.Find(TEXT("layer"));
		return FString::Printf(TEXT("%lld:%d"), Point.NodeId, Layer ? FCString::Atoi(**Layer) : 0);
	}

#if WITH_EDITOR
	FString GetRoadSplinePrefix(const ARoadActor* Road)
	{
		FString Key;
		for (const TCHAR Ch : Road->GetName().Left(80))
			Key.AppendChar((FChar::IsAlnum(Ch) || Ch == static_cast<TCHAR>('_')) ? Ch : static_cast<TCHAR>('_'));
		return FString::Printf(TEXT("RC_LandscapeSplinePrototype_%s_"), *Key);
	}

	TSet<FString> GetRoadSplinePrefixes(const AOsmRoadScene* Scene)
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

	TSet<FString> GetRoadSplinePrefixes(const TArray<AOsmRoadScene*>& Scenes)
	{
		TSet<FString> Prefixes;
		for (const AOsmRoadScene* Scene : Scenes) Prefixes.Append(GetRoadSplinePrefixes(Scene));
		return Prefixes;
	}

	void FlushLandscapeSplineChanges(ALandscape* PreviousLandscape, ALandscape* TargetLandscape)
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
	void RemoveRoadConstructorSplines(const TSet<FString>& Prefixes, ALandscape* Landscape)
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

	bool HasRoadConstructorSpline(const ARoadActor* Road, const ALandscape* Landscape)
	{
		if (!Road || !Landscape || !Landscape->GetSplinesComponent()) return false;
		const FString Prefix = GetRoadSplinePrefix(Road);
		for (const ULandscapeSplineControlPoint* Point : Landscape->GetSplinesComponent()->GetControlPoints())
			if (Point && Point->GetName().StartsWith(Prefix)) return true;
		return false;
	}

#endif

	TArray<FSceneSpans> PartitionSpans(const TArray<FImportSpan>& Spans, double SceneSizeCm)
	{
		TMap<FIntPoint, TArray<FImportSpan>> Cells;
		auto CellFor = [SceneSizeCm](const FVector& Point)
		{
			return FIntPoint(FMath::FloorToInt(Point.X / SceneSizeCm + 0.5), FMath::FloorToInt(Point.Y / SceneSizeCm + 0.5));
		};
		if (!FMath::IsFinite(SceneSizeCm) || SceneSizeCm < 1000) return {};
		for (int32 SpanIndex = 0; SpanIndex < Spans.Num(); ++SpanIndex)
		{
			const FImportSpan& Span = Spans[SpanIndex];
			if (Span.Positions.Num() < 2) continue;
			const FIntPoint StartCell = CellFor(Span.Positions[0]);
			const FIntPoint EndCell = CellFor(Span.Positions.Last());
			if (StartCell == EndCell)
			{
				Cells.FindOrAdd(StartCell).Add(Span);
				continue;
			}
			double HalfLength = 0;
			for (int32 Index = 1; Index < Span.Positions.Num(); ++Index)
				HalfLength += FVector::Dist2D(Span.Positions[Index - 1], Span.Positions[Index]);
			HalfLength *= 0.5;
			double Distance = 0;
			for (int32 Index = 1; Index < Span.Positions.Num(); ++Index)
			{
				const double Length = FVector::Dist2D(Span.Positions[Index - 1], Span.Positions[Index]);
				if (Distance + Length < HalfLength) { Distance += Length; continue; }
				const FVector Midpoint = FMath::Lerp(Span.Positions[Index - 1], Span.Positions[Index],
					Length > UE_SMALL_NUMBER ? (HalfLength - Distance) / Length : 0.0);
				FImportSpan Left = Span, Right = Span;
				Left.EndKey = Right.StartKey = FString::Printf(TEXT("SceneSeam:%d"), SpanIndex);
				Left.Positions.SetNum(Index);
				if (!Left.Positions.Last().Equals(Midpoint, 0.01)) Left.Positions.Add(Midpoint);
				Right.Positions = { Midpoint };
				for (int32 Tail = Index; Tail < Span.Positions.Num(); ++Tail)
					if (!Right.Positions.Last().Equals(Span.Positions[Tail], 0.01)) Right.Positions.Add(Span.Positions[Tail]);
				if (Left.Positions.Num() >= 2) Cells.FindOrAdd(StartCell).Add(MoveTemp(Left));
				if (Right.Positions.Num() >= 2) Cells.FindOrAdd(EndCell).Add(MoveTemp(Right));
				break;
			}
		}
		TArray<FIntPoint> Keys;
		Cells.GetKeys(Keys);
		Keys.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.X == B.X ? A.Y < B.Y : A.X < B.X; });
		TArray<FSceneSpans> Result;
		for (const FIntPoint& Key : Keys) Result.Add({ Key, MoveTemp(Cells.FindChecked(Key)) });
		return Result;
	}

	TArray<FImportSpan> PrepareSpans(const TArray<FImportedRoadWay>& Ways, const TArray<FName>& Filter,
	                                 bool bRequireTerrain, double HeightOffset, double MergeDistance)
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
			for (int32 Point = 1; Point < Span.Positions.Num(); ++Point) Length += FVector::Dist2D(
				Span.Positions[Point - 1], Span.Positions[Point]);
			if (Length > MergeDistance) continue;
			const FString Start = root(Span.StartKey);
			if (const FString End = root(Span.EndKey); Start != End) Parent.Add(End, Start);
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

	bool HasSymmetricLaneStyle(const URoadStyle* Style, const int32 LaneCount)
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

	double SpanLength(const TArray<FVector>& Positions)
	{
		double Length = 0;
		for (int32 Index = 1; Index < Positions.Num(); ++Index) Length += FVector::Dist2D(Positions[Index - 1], Positions[Index]);
		return Length;
	}

	bool SplitSpanPositions(const TArray<FVector>& Positions, double Distance, TArray<FVector>& Head, TArray<FVector>& Tail)
	{
		Head.Reset();
		Tail.Reset();
		if (Positions.Num() < 2 || Distance <= 0 || Distance >= SpanLength(Positions)) return false;
		Head.Add(Positions[0]);
		for (int32 Index = 1; Index < Positions.Num(); ++Index)
		{
			const double Length = FVector::Dist2D(Positions[Index - 1], Positions[Index]);
			if (Distance > Length) { Head.Add(Positions[Index]); Distance -= Length; continue; }
			const FVector Cut = FMath::Lerp(Positions[Index - 1], Positions[Index], Length > UE_SMALL_NUMBER ? Distance / Length : 0.0);
			if (!Head.Last().Equals(Cut, 0.001)) Head.Add(Cut);
			Tail.Add(Cut);
			for (int32 Rest = Index; Rest < Positions.Num(); ++Rest)
				if (!Tail.Last().Equals(Positions[Rest], 0.001)) Tail.Add(Positions[Rest]);
			return Head.Num() >= 2 && Tail.Num() >= 2;
		}
		return false;
	}

	int32 CreateTransitionSpans(TArray<FImportSpan>& Spans, AOsmRoadScene* Owner, double TransitionLengthCm,
		const TFunctionRef<URoadStyle*(const FImportSpan&)>& ResolveStyle)
	{
		struct FEndpoint { int32 Index; bool bStart; };
		TMap<FString, TArray<FEndpoint>> Endpoints;
		for (int32 Index = 0; Index < Spans.Num(); ++Index)
		{
			if (!Spans[Index].StartKey.IsEmpty()) Endpoints.FindOrAdd(Spans[Index].StartKey).Add({ Index, true });
			if (!Spans[Index].EndKey.IsEmpty()) Endpoints.FindOrAdd(Spans[Index].EndKey).Add({ Index, false });
		}
		TArray<FString> Keys;
		Endpoints.GetKeys(Keys);
		Keys.Sort();
		TArray<FImportSpan> Transitions;
		for (const FString& Key : Keys)
		{
			const auto& Ends = Endpoints.FindChecked(Key);
			if (Ends.Num() != 2 || Ends[0].Index == Ends[1].Index) continue;
			const FEndpoint AEnd = Ends[0], BEnd = Ends[1];
			FImportSpan& A = Spans[AEnd.Index];
			FImportSpan& B = Spans[BEnd.Index];
			const FVector ANode = AEnd.bStart ? A.Positions[0] : A.Positions.Last();
			if (const FVector BNode = BEnd.bStart ? B.Positions[0] : B.Positions.Last(); !ANode.Equals(BNode, 1.0)) continue;
			URoadStyle* AStyle = ResolveStyle(A);
			URoadStyle* BStyle = ResolveStyle(B);
			if (!AStyle || !BStyle) continue;
			// Keep intentional split loops with one unchanged style; they need no taper.
			if (AStyle == BStyle && (AEnd.bStart != BEnd.bStart || HasSymmetricLaneStyle(AStyle, 0))) continue;
			const double ALength = SpanLength(A.Positions), BLength = SpanLength(B.Positions);
			const double ACut = FMath::Min(TransitionLengthCm * 0.5, ALength * 0.25);
			const double BCut = FMath::Min(TransitionLengthCm * 0.5, BLength * 0.25);
			TArray<FVector> AHead, ATail, BHead, BTail;
			if (!SplitSpanPositions(A.Positions, AEnd.bStart ? ACut : ALength - ACut, AHead, ATail) ||
				!SplitSpanPositions(B.Positions, BEnd.bStart ? BCut : BLength - BCut, BHead, BTail)) continue;
			URoadStyle* Source = DuplicateObject<URoadStyle>(AStyle, Owner);
			URoadStyle* Target = DuplicateObject<URoadStyle>(BStyle, Owner);
			Source->ClearFlags(RF_Standalone);
			Target->ClearFlags(RF_Standalone);
			Source->SetFlags(RF_Public | RF_Transactional);
			Target->SetFlags(RF_Public | RF_Transactional);
			if (AEnd.bStart) Swap(Source->LeftLanes, Source->RightLanes);
			if (!BEnd.bStart) Swap(Target->LeftLanes, Target->RightLanes);
			// Supply the union lane stack so the native taper can both collapse and
			// introduce lanes. Its outer-edge mapping keeps sidewalks at the outside.
			for (int32 Side = 0; Side < 2; ++Side)
			{
				auto& SourceLanes = Side ? Source->LeftLanes : Source->RightLanes;
				const auto& TargetLanes = Side ? Target->LeftLanes : Target->RightLanes;
				const int32 Extra = TargetLanes.Num() - SourceLanes.Num();
				for (int32 Index = 0; Index < Extra; ++Index)
				{
					FRoadLaneStyle Lane = TargetLanes[Index];
					Lane.Width = 0;
					SourceLanes.Insert(Lane, Index);
				}
			}
			Owner->ImportedTransitionStyles.Add(Source);
			Owner->ImportedTransitionStyles.Add(Target);
			FImportSpan Transition = A;
			Transition.StartKey = FString::Printf(TEXT("Transition:%s:A"), *Key);
			Transition.EndKey = FString::Printf(TEXT("Transition:%s:B"), *Key);
			Transition.TransitionSourceStyle = Source;
			Transition.TransitionTargetStyle = Target;
			Transition.Positions = AEnd.bStart ? AHead : ATail;
			if (AEnd.bStart) Algo::Reverse(Transition.Positions);
			TArray<FVector> Finish = BEnd.bStart ? BHead : BTail;
			if (!BEnd.bStart) Algo::Reverse(Finish);
			Transition.Positions.Append(Finish.GetData() + 1, Finish.Num() - 1);
			for (int64 Id : B.SourceWayIds) Transition.SourceWayIds.AddUnique(Id);
			A.Positions = AEnd.bStart ? MoveTemp(ATail) : MoveTemp(AHead);
			B.Positions = BEnd.bStart ? MoveTemp(BTail) : MoveTemp(BHead);
			(AEnd.bStart ? A.StartKey : A.EndKey) = Transition.StartKey;
			(BEnd.bStart ? B.StartKey : B.EndKey) = Transition.EndKey;
			Transitions.Add(MoveTemp(Transition));
		}
		const int32 Count = Transitions.Num();
		Spans.Append(MoveTemp(Transitions));
		return Count;
	}

	bool HasMatchingLaneCountLayout(const URoadStyle* Style, int32 FirstCount, int32 SecondCount)
	{
		if (FirstCount == SecondCount) return true;
		if (!Style) return false;
		TArray<FRoadLaneStyle> FirstLeft, FirstRight, SecondLeft, SecondRight;
		GetLaneCountLayout(Style, FirstCount, FirstLeft, FirstRight);
		GetLaneCountLayout(Style, SecondCount, SecondLeft, SecondRight);
		// The same base style and same resulting side counts produce the same layout,
		// even when one way omits lanes and another explicitly repeats the default.
		return FirstLeft.Num() == SecondLeft.Num() && FirstRight.Num() == SecondRight.Num();
	}

	int32 MergeSharedNodeSpans(TArray<FImportSpan>& Spans, const TFunctionRef<URoadStyle*(const FImportSpan&)>& ResolveStyle)
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
			const URoadStyle* Style = ResolveStyle(A);
			if (Style != ResolveStyle(B) || !HasMatchingLaneCountLayout(Style, A.LaneCount, B.LaneCount)) continue;
			if ((First.bStart || !Second.bStart) && !HasSymmetricLaneStyle(Style, A.LaneCount)) continue;
			const FVector& JoinA = First.bStart ? A.Positions[0] : A.Positions.Last();
			const FVector& JoinB = Second.bStart ? B.Positions[0] : B.Positions.Last();
			// Contracted junction aliases may share a key but be physically separated.
			if (FVector::DistSquared(JoinA, JoinB) > 1.0) continue;
			const FVector& NextA = First.bStart ? A.Positions[1] : A.Positions[A.Positions.Num() - 2];
			const FVector& NextB = Second.bStart ? B.Positions[1] : B.Positions[B.Positions.Num() - 2];
			const FVector2D OutA = FVector2D(NextA - JoinA).GetSafeNormal();
			const FVector2D OutB = FVector2D(NextB - JoinB).GetSafeNormal();
			if (OutA.IsNearlyZero() || OutB.IsNearlyZero()) continue;
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
			for (const bool bStart : { true, false })
			{
				const FString& OuterKey = bStart ? A.StartKey : A.EndKey;
				if (OuterKey.IsEmpty()) continue;
				TArray<FEndpoint>& OuterArms = Endpoints.FindChecked(OuterKey);
				OuterArms.RemoveAll([&](const FEndpoint& Endpoint)
				{
					return Endpoint.SpanIndex == First.SpanIndex || Endpoint.SpanIndex == Second.SpanIndex;
				});
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

TArray<FName> ARoadGenerator::GetLandscapeSplineEditLayerOptions()
{
	return { TEXT("Splines") };
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
	if (GeneratedSplinePrefixes.IsEmpty() && !GeneratedSplineLandscape.IsNull() && !GetRoadScenes().IsEmpty())
	{
		Modify();
		GeneratedSplinePrefixes = GetRoadSplinePrefixes(GetRoadScenes());
	}
#endif
}

bool ARoadGenerator::GenerateLandscapeSplinesInternal()
{
#if WITH_EDITOR
	const TArray<AOsmRoadScene*> Scenes = GetRoadScenes();
	if (!AreRoadScenesLoaded() || Scenes.IsEmpty() || !IsValid(TargetLandscape) || !TargetLandscape->GetLandscapeInfo())
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
	if (!FMath::IsFinite(LandscapeHeightOffsetCm) ||
		!FMath::IsFinite(LandscapeSplineEndFalloffCm) || LandscapeSplineEndFalloffCm < 0.0) return false;
	if (!FMath::IsFinite(LandscapeSplineWidthScale) || LandscapeSplineWidthScale < 0.2 || LandscapeSplineWidthScale >
		2.0 ||
		!FMath::IsFinite(LandscapeSplineFalloffScale) || LandscapeSplineFalloffScale < 0.05 ||
		LandscapeSplineFalloffScale > 3.0)
		return false;
	for (const double Value : { LandscapeSplineStartFalloffCm, LandscapeSplineEndFalloffCm,
		LandscapeSplineStartExtensionCm, LandscapeSplineEndExtensionCm })
		if (!FMath::IsFinite(Value) || Value < 0.0 || Value > 100000.0) return false;
	for (const AOsmRoadScene* Scene : Scenes)
	for (const ARoadActor* Road : Scene->Roads)
	{
		if (!IsValid(Road) || !Road->BaseCurve || Road->BaseCurve->Curve.Points.Num() < 2)
		{
			UE_LOG(LogRoadConstructorImport, Error, TEXT("Load all generated roads before generating Landscape splines."));
			return false;
		}
	}
	auto calculate_inputs = [&]()
	{
		FString Inputs;
		for (const AOsmRoadScene* Scene : Scenes) Inputs += RoadGenerationSignatures::LandscapeInputs(*this, *Scene);
		return Inputs;
	};
	if (const FString SplineInputs = calculate_inputs(); !
		CachedSplineOutput.IsEmpty() && CachedSplineInputs == SplineInputs && GeneratedSplineLandscape.Get() ==
		TargetLandscape.Get() &&
		CachedSplineOutput == RoadGenerationSignatures::LandscapeOutput(TargetLandscape, GeneratedSplinePrefixes))
	{
		UE_LOG(LogRoadConstructorImport, Display,
		       TEXT("Landscape spline inputs and output are unchanged; skipped generation."));
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
	bool bBuilt = true;
	for (AOsmRoadScene* Scene : Scenes)
		if (!BuildLandscapeSplines(Scene)) { bBuilt = false; break; }
	if (!bBuilt)
	{
		RemoveRoadConstructorSplines(GetRoadSplinePrefixes(Scenes), TargetLandscape);
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
	GeneratedSplinePrefixes = GetRoadSplinePrefixes(GetRoadScenes());
	FlushLandscapeSplineChanges(PreviousLandscape, TargetLandscape);
	CachedSplineInputs = calculate_inputs();
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
	const FName RequiredLayerName(TEXT("Splines"));
	if (TerrainDeformTargetLandscapeSplineEditLayer != RequiredLayerName)
	{
		UE_LOG(LogRoadConstructorImport, Error, TEXT("Spline Edit Layer must be Splines."));
		return false;
	}
	// A matching name is insufficient: this must be the reserved spline layer type.
	if (const ULandscapeEditLayerBase* SplineLayer = TargetLandscape->FindEditLayerOfType(
		ULandscapeEditLayerSplines::StaticClass()))
	{
		if (SplineLayer->GetName() == RequiredLayerName) return true;
		UE_LOG(LogRoadConstructorImport, Error,
			TEXT("Rename the reserved Landscape spline edit layer to Splines before generating."));
		return false;
	}
	if (TargetLandscape->GetEditLayerConst(RequiredLayerName))
	{
		UE_LOG(LogRoadConstructorImport, Error,
			TEXT("Splines is occupied by a non-spline layer. Rename that layer and create a reserved spline layer named Splines."));
		return false;
	}
	if (!bCreateSplineLayerIfMissing)
	{
		UE_LOG(LogRoadConstructorImport, Error,
			TEXT("Create a reserved Landscape spline edit layer named Splines, or enable Create Spline Layer If Missing."));
		return false;
	}
	TargetLandscape->Modify();
	if (TargetLandscape->CreateLayer(RequiredLayerName, ULandscapeEditLayerSplines::StaticClass()) != INDEX_NONE) return true;
	UE_LOG(LogRoadConstructorImport, Error, TEXT("Could not create the reserved Landscape spline edit layer Splines."));
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
		Road->TerrainDeformLandscapeSplineWidthScale = LandscapeSplineWidthScale;
		Road->TerrainDeformLandscapeSplineFalloffScale = LandscapeSplineFalloffScale;
		// Reset distances written by the former custom control; use the plugin's
		// default base distance and let its native spline builder apply the scale.
		Road->TerrainDeformFalloffDistance = GetDefault<ARoadActor>()->TerrainDeformFalloffDistance;
		Road->TerrainDeformLandscapeSplineStartFalloff = LandscapeSplineStartFalloffCm;
		Road->TerrainDeformLandscapeSplineEndFalloff = LandscapeSplineEndFalloffCm;
		Road->TerrainDeformLandscapeSplineStartExtension = LandscapeSplineStartExtensionCm;
		Road->TerrainDeformLandscapeSplineEndExtension = LandscapeSplineEndExtensionCm;
		// The native command supplies topology and XY samples. Its roadbed heights
		// are replaced below using the imported baseline, independent of mesh offset.
		Road->TerrainDeformLandscapeSplineHeightOffset = LandscapeHeightOffsetCm - Scene->ImportedSurfaceOffsetCm;
		Road->CreatePersistentLandscapeSplineEditLayerPrototype();
		if (!HasRoadConstructorSpline(Road, TargetLandscape))
		{
			UE_LOG(LogRoadConstructorImport, Error,
			       TEXT("Road Constructor failed to create a Landscape spline for %s on Target Landscape."),
			       *GetNameSafe(Road));
			RemoveRoadConstructorSplines(GetRoadSplinePrefixes(Scene), TargetLandscape);
			return false;
		}
		// Use all lane widths, with explicit falloff values,
		// after the plugin creates the native control points.
		double RoadWidth = 0.0;
		if (const URoadStyle* Style = Road->LastAppliedRoadStyle)
		{
			for (const TArray<FRoadLaneStyle>* Lanes : { &Style->LeftLanes, &Style->RightLanes })
				for (const FRoadLaneStyle& Lane : *Lanes)
					if (FMath::IsFinite(Lane.Width))
						RoadWidth += FMath::Max(0.0, Lane.Width);
		}
		const FString Prefix = GetRoadSplinePrefix(Road);
		TSet<ULandscapeSplineSegment*> SegmentsToUpdate;
		for (ULandscapeSplineControlPoint* Point : TargetLandscape->GetSplinesComponent()->GetControlPoints())
		{
			if (!Point || !Point->GetName().StartsWith(Prefix)) continue;
			Point->Modify();
			const ULandscapeSplinesComponent* Splines = TargetLandscape->GetSplinesComponent();
			FVector WorldPosition = Splines->GetComponentTransform().TransformPosition(Point->Location);
			const double Distance = FMath::Clamp(Road->BaseCurve->Curve.GetUV(WorldPosition).X, 0.0, Road->Length());
			WorldPosition.Z = Road->GetHeight(Distance) - Scene->ImportedSurfaceOffsetCm + LandscapeHeightOffsetCm;
			Point->Location = Splines->GetComponentTransform().InverseTransformPosition(WorldPosition);
			if (const FVector Direction = Splines->GetComponentTransform().
			                                       InverseTransformVectorNoScale(Road->GetDir(Distance)); !Direction.
				IsNearlyZero()) Point->Rotation = Direction.Rotation();
			// RoadActor's default 0.5 scale yields the native half-width of all lanes.
			Point->Width = static_cast<float>(RoadWidth * LandscapeSplineWidthScale);
			// Preserve native side falloff, separate start/end falloffs and extension positions.
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
	[[maybe_unused]] bool Marked = MarkPackageDirty();
	UE_LOG(LogRoadConstructorImport, Display, TEXT("Applied Road Constructor Terrain Deform to %d generated roads."),
	       CreatedRoads);
	return true;
#else
	return false;
#endif
}
TArray<AOsmRoadScene*> ARoadGenerator::GetRoadScenes() const
{
	TArray<AOsmRoadScene*> Scenes;
	if (GeneratedScene.IsValid()) Scenes.Add(GeneratedScene.Get());
	for (const auto& Scene : GeneratedScenes)
		if (Scene.IsValid()) Scenes.AddUnique(Scene.Get());
	return Scenes;
}

bool ARoadGenerator::AreRoadScenesLoaded() const
{
	if (!GeneratedScene.IsNull() && !GeneratedScene.IsValid()) return false;
	for (const auto& Scene : GeneratedScenes)
		if (!Scene.IsNull() && !Scene.IsValid()) return false;
	return true;
}

TArray<UObject*> ARoadGenerator::GetGeneratedObjects() const
{
	TArray<UObject*> Objects;
	for (AOsmRoadScene* Scene : GetRoadScenes()) Objects.Add(Scene);
	return Objects;
}

bool ARoadGenerator::Cleanup_Implementation(const bool bSkipPrompt)
{
	if (!AreRoadScenesLoaded())
	{
		UE_LOG(LogRoadConstructorImport, Error, TEXT("Load all previous Road Constructor scenes before clearing roads."));
		return false;
	}
	RememberLandscapeSplineOwnership();
	if (!DeleteGeneratedObjects(bSkipPrompt)) return false;
	Modify();
	GeneratedScene.Reset();
	GeneratedScenes.Reset();
	CachedRoadInputs.Reset();
	CachedRoadGeometryInputs.Reset();
	CachedJunctionInputs.Reset();
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

FName ARoadGenerator::ResolveOutputFolder(const FName Override) const
{
#if WITH_EDITOR
	FString Folder = Override.IsNone() ? SpawnedActorsPath.TrimStartAndEnd() : Override.ToString();
	if (Folder.IsEmpty()) return GetFolderPath();
	Folder.ReplaceInline(TEXT("\\"), TEXT("/"));
	while (Folder.Contains(TEXT("//"))) Folder.ReplaceInline(TEXT("//"), TEXT("/"));
	Folder.RemoveFromStart(TEXT("/"));
	Folder.RemoveFromEnd(TEXT("/"));
	return FName(*Folder);
#else
	return NAME_None;
#endif
}

bool ARoadGenerator::BuildRoads(const TArray<FImportedRoadWay>& Ways, const FName OutputFolderPath)
{
#if WITH_EDITOR
	using namespace RoadGenerationSignatures;
	if (!AreRoadScenesLoaded())
	{
		UE_LOG(LogRoadConstructorImport, Error, TEXT("Load all previous road scenes before replacing roads."));
		return false;
	}
	if (!FMath::IsFinite(SurfaceOffsetCm) || !FMath::IsFinite(RoadCurveRadiusCm) ||
		!FMath::IsFinite(RoadSceneSizeCm) || RoadSceneSizeCm < 1000 ||
		!FMath::IsFinite(RoadTransitionLengthCm) || RoadTransitionLengthCm < 100 ||
		!FMath::IsFinite(JunctionMergeDistanceCm) ||
		(bOverrideJunctionRadius && !FMath::IsFinite(JunctionRadiusCm))) return false;

	const FName Folder = ResolveOutputFolder(OutputFolderPath);
	const FString CurveInputs = RoadGeometryInputs(*this, Ways, Folder);
	const FString Inputs = RoadInputs(*this, Ways, Folder);
	const FString JunctionSettings = JunctionInputs(*this, CurveInputs);
	TArray<AOsmRoadScene*> Scenes = GetRoadScenes();
	const bool bPrepareCurves = Scenes.IsEmpty() || CachedRoadGeometryInputs != CurveInputs;
	if (bPrepareCurves)
	{
		Scenes = PrepareRoadCurves(Ways, Folder);
		if (Scenes.IsEmpty()) return false;
	}
	auto failed = [&]()
	{
		// Keep existing scenes until every replacement has succeeded.
		if (bPrepareCurves) for (AOsmRoadScene* Scene : Scenes) Scene->Destroy();
		return false;
	};
	const bool bSolveJunctions = bPrepareCurves || CachedJunctionInputs != JunctionSettings;
	const bool bBuildSurfaces = bSolveJunctions || CachedRoadInputs != Inputs;
	for (AOsmRoadScene* Scene : Scenes)
	{
		Scene->bOverrideJunctionRadius = bOverrideJunctionRadius;
		if (bOverrideJunctionRadius) Scene->ImportedJunctionRadius = FMath::Max(0.0, JunctionRadiusCm);
		Scene->bCleanJunctionApproaches = bCleanJunctionApproaches;
		Scene->bRemoveCornersWithoutJunctionSurface = bRemoveCornersWithoutJunctionSurface;
		if (bSolveJunctions && !Scene->SolveJunctions(!bPrepareCurves)) return failed();
		if (Scene->GetFolderPath() != Folder)
		{
			Scene->Modify();
			Scene->SetFolderPath(Folder);
		}
		if (bBuildSurfaces && !Scene->BuildSurfaceMeshes()) return failed();
		if (!bBuildSurfaces) Scene->UpdateGeneratedActorFolders();
	}
	UE_LOG(LogRoadConstructorImport, Display, TEXT("Road generation: %d scenes; curves=%s junctions=%s surfaces=%s."),
		Scenes.Num(), bPrepareCurves ? TEXT("built") : TEXT("skipped"),
		bSolveJunctions ? TEXT("built") : TEXT("skipped"), bBuildSurfaces ? TEXT("built") : TEXT("skipped"));
	if (bPrepareCurves)
	{
		RememberLandscapeSplineOwnership();
		for (AOsmRoadScene* Previous : GetRoadScenes()) Previous->Destroy();
		Modify();
		GeneratedScenes.Reset();
		for (AOsmRoadScene* Scene : Scenes) GeneratedScenes.Add(Scene);
		GeneratedScene = Scenes[0];
	}
	CachedRoadInputs = Inputs;
	CachedRoadGeometryInputs = CurveInputs;
	CachedJunctionInputs = JunctionSettings;
	if (bPrepareCurves || bSolveJunctions || bBuildSurfaces)
	{
		for (AOsmRoadScene* Scene : Scenes) Scene->MarkPackageDirty();
		MarkPackageDirty();
	}
	return true;
#else
	return false;
#endif
}

TArray<AOsmRoadScene*> ARoadGenerator::PrepareRoadCurves(const TArray<FImportedRoadWay>& Ways, const FName OutputFolderPath)
{
#if WITH_EDITOR
	const FName Folder = ResolveOutputFolder(OutputFolderPath);
	const TArray<FImportSpan> ImportedSpans = PrepareSpans(Ways, HighwayFilter, bRequireTerrainSamples, SurfaceOffsetCm, JunctionMergeDistanceCm);
	if (ImportedSpans.IsEmpty())
	{
		UE_LOG(LogRoadConstructorImport, Error, TEXT("The cache contains no usable roads for the selected filter. Previous output was kept."));
		return {};
	}
	TArray<FSceneSpans> Partitions = PartitionSpans(ImportedSpans, RoadSceneSizeCm);
	TArray<AOsmRoadScene*> Candidates;
	auto failed = [&]() -> TArray<AOsmRoadScene*>
	{
		for (AOsmRoadScene* Scene : Candidates) Scene->Destroy();
		return {};
	};
	FScopedSlowTask Progress(ImportedSpans.Num() * 2 + Partitions.Num(), NSLOCTEXT("Landscapes", "ImportRoadConstructor", "Generating Road Constructor scenes"));
	Progress.MakeDialog(false);
	for (auto& [Cell, Spans] : Partitions)
	{
		FActorSpawnParameters Params;
		Params.Owner = this;
		Params.OverrideLevel = GetLevel();
		Params.ObjectFlags = RF_Transactional;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AOsmRoadScene* Candidate = GetWorld()->SpawnActor<AOsmRoadScene>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!Candidate) return failed();
		Candidates.Add(Candidate);
		Candidate->ImportedSurfaceOffsetCm = SurfaceOffsetCm;
		auto resolve_style = [this, Candidate](const FImportSpan& Span) -> URoadStyle*
		{
			if (Span.TransitionSourceStyle) return Span.TransitionSourceStyle;
			if (const auto* Existing = Candidate->ImportedWayStyles.Find(Span.WayId)) return Existing->Get();
			const auto* Override = HighwayStyles.Find(Span.Highway);
			const URoadStyle* Base = Override && IsValid(Override->Get()) ? Override->Get() : DefaultRoadStyle.Get();
			URoadStyle* Style = CreateLaneCountStyle(Candidate, Base, Span.LaneCount);
			Candidate->ImportedWayStyles.Add(Span.WayId, Style);
			return Style;
		};
		// Compare selected source assets, not the independent per-way style copies.
		auto resolve_source_style = [this](const FImportSpan& Span) -> URoadStyle*
		{
			const auto* Override = HighwayStyles.Find(Span.Highway);
			return Override && IsValid(Override->Get()) ? Override->Get() : DefaultRoadStyle.Get();
		};
		const int32 Merged = MergeSharedNodeSpans(Spans, resolve_source_style);
		CreateTransitionSpans(Spans, Candidate, RoadTransitionLengthCm, resolve_style);
		UE_LOG(LogRoadConstructorImport, Display, TEXT("Merged %d compatible two-arm shared-node connections in scene cell %d,%d."),
			Merged, Cell.X, Cell.Y);
		Candidate->bOverrideJunctionRadius = bOverrideJunctionRadius;
		if (bOverrideJunctionRadius) Candidate->ImportedJunctionRadius = FMath::Max(0.0, JunctionRadiusCm);
		Candidate->bCleanJunctionApproaches = bCleanJunctionApproaches;
		Candidate->bRemoveCornersWithoutJunctionSurface = bRemoveCornersWithoutJunctionSurface;
		Candidate->SetActorLabel(FString::Printf(TEXT("%s_RoadConstructor_%d_%d"), *GetActorLabel(), Cell.X, Cell.Y));
		Candidate->SetFolderPath(Folder);
		for (int32 Index = 0; Index < Spans.Num(); ++Index)
		{
			Progress.EnterProgressFrame(1);
			const FImportSpan& Span = Spans[Index];
			Params.Owner = Candidate;
			AOsmRoadActor* Road = GetWorld()->SpawnActor<AOsmRoadActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
			if (!Road) return failed();
			Road->AttachToActor(Candidate, FAttachmentTransformRules::KeepWorldTransform);
			Candidate->Roads.Add(Road);
			Road->InitWithStyle(resolve_style(Span), Span.Positions[0].Z);
			if (Span.TransitionTargetStyle)
			{
				Road->bEnableRoadTransitionPrototype = true;
				Road->RoadTransitionTargetStyle = Span.TransitionTargetStyle;
				Road->RoadTransitionStartDistance = 0;
				Road->RoadTransitionLength = 1.e12; // Clamped to the full fitted segment below.
				Road->RoadTransitionLaneMappingMode = ERoadTransitionLaneMappingMode::PreserveOuterEdge;
			}
			Road->bHasGround = false;
			Road->ImportedPositions = Span.Positions;
			Road->ImportedCurveRadius = FMath::Clamp(RoadCurveRadiusCm, 1.0, 5000000.0);
			Road->StartNodeKey = Span.StartKey;
			Road->EndNodeKey = Span.EndKey;
			Road->Tags.AddUnique(TEXT("RC_DisableLegacyHighwaySideConnector"));
			for (int64 WayId : Span.SourceWayIds) Road->Tags.AddUnique(FName(*FString::Printf(TEXT("OSMWay_%lld"), WayId)));
			Road->SetActorLabel(FString::Printf(TEXT("OSM_%lld_%d"), Span.WayId, Index));
			Road->SetFolderPath(Folder);
		}
		Progress.EnterProgressFrame(1);
		if (!Candidate->PrepareRoadCurves()) return failed();
		for (ARoadActor* Road : Candidate->Roads)
			if (Road->bEnableRoadTransitionPrototype)
			{
				Road->RoadTransitionLength = Road->Length();
				Road->UpdateCurve();
			}
	}
	return Candidates;
#else
	return {};
#endif
}
