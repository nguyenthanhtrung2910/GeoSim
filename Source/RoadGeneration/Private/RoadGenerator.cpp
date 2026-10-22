#include "RoadGenerator.h"

#include "Components/SceneComponent.h"
#include "ConcurrencyHelpers/Concurrency.h"
#include "OsmRoadScene.h"
#include "OSMRoadImporter.h"
#include "UObject/ConstructorHelpers.h"
#include "Algo/Reverse.h"
#include "Landscape.h"
#include "LandscapeSplineActor.h"

#if WITH_EDITOR
#include "Misc/ScopedSlowTask.h"
#include "LandscapeInfo.h"
#include "LandscapeEditLayer.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"
#include "Engine/World.h"
#endif
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogRoadConstructorImport, Log, All);

namespace
{
	struct FImportSpan
	{
		int64 WayId = 0;
		TArray<int64> SourceWayIds;
		FName Highway;
		TArray<FVector> Positions;
		FString StartKey;
		FString EndKey;
	};

	bool IsEnabledTag(const TMap<FString, FString>& Tags, const TCHAR* Key)
	{
		const FString* Value = Tags.Find(Key);
		return Value && !Value->IsEmpty() && !Value->Equals(TEXT("no"), ESearchCase::IgnoreCase) && *Value != TEXT("0") && !Value->Equals(TEXT("false"), ESearchCase::IgnoreCase);
	}

	bool IsGroundRoad(const FImportedRoadWay& Way, const TArray<FName>& Filter)
	{
		const FString* Highway = Way.Tags.Find(TEXT("highway"));
		if (!Highway || Highway->IsEmpty() || IsEnabledTag(Way.Tags, TEXT("area")) || IsEnabledTag(Way.Tags, TEXT("bridge")) || IsEnabledTag(Way.Tags, TEXT("tunnel"))) return false;
		static const TSet<FName> Excluded = { TEXT("footway"), TEXT("path"), TEXT("steps"), TEXT("cycleway"), TEXT("pedestrian"), TEXT("bridleway"), TEXT("construction"), TEXT("proposed"), TEXT("platform") };
		const FName Kind(**Highway);
		if (Excluded.Contains(Kind) || (!Filter.IsEmpty() && !Filter.Contains(Kind))) return false;
		const FString* Layer = Way.Tags.Find(TEXT("layer"));
		return !Layer || FCString::Atoi(**Layer) == 0;
	}

	FString NodeKey(const FImportedRoadWay& Way, const FImportedRoadPoint& Point)
	{
		if (Point.NodeId <= 0) return FString();
		const FString* Layer = Way.Tags.Find(TEXT("layer"));
		return FString::Printf(TEXT("%lld:%d"), Point.NodeId, Layer ? FCString::Atoi(**Layer) : 0);
	}

#if WITH_EDITOR
	// Road Constructor stores its persistent splines on the Landscape, outside the road actor.
	// Match only the objects its public Terrain Deform command creates for our roads.
	void RemoveRoadConstructorSplines(AOsmRoadScene* Scene, ALandscape* Landscape)
	{
		if (!IsValid(Scene) || !IsValid(Landscape)) return;
		ULandscapeSplinesComponent* Splines = Landscape->GetSplinesComponent();
		if (!Splines) return;
		TSet<FString> Prefixes;
		for (ARoadActor* Road : Scene->Roads)
		{
			if (!IsValid(Road)) continue;
			FString Key;
			for (TCHAR Ch : Road->GetName().Left(80))
				Key.AppendChar((FChar::IsAlnum(Ch) || Ch == TCHAR('_')) ? Ch : TCHAR('_'));
			Prefixes.Add(FString::Printf(TEXT("RC_LandscapeSplinePrototype_%s_"), *Key));
		}
		auto IsOurs = [&Prefixes](const UObject* Object)
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
			if (!IsOurs(Segment)) continue;
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
			if (!IsOurs(Point)) continue;
			Point->Modify();
			Point->ConnectedSegments.Reset();
			Points.RemoveAt(Index, 1, EAllowShrinking::No);
			Point->ConditionalBeginDestroy();
		}
		Splines->MarkRenderStateDirty();
		Landscape->RequestSplineLayerUpdate();
		Landscape->MarkPackageDirty();
	}

	bool HasRoadConstructorSpline(const ARoadActor* Road, const ALandscape* Landscape)
	{
		if (!Road || !Landscape || !Landscape->GetSplinesComponent()) return false;
		FString Key;
		for (TCHAR Ch : Road->GetName().Left(80))
			Key.AppendChar((FChar::IsAlnum(Ch) || Ch == TCHAR('_')) ? Ch : TCHAR('_'));
		const FString Prefix = FString::Printf(TEXT("RC_LandscapeSplinePrototype_%s_"), *Key);
		for (const ULandscapeSplineControlPoint* Point : Landscape->GetSplinesComponent()->GetControlPoints())
			if (Point && Point->GetName().StartsWith(Prefix)) return true;
		return false;
	}

#endif

	TArray<FImportSpan> PrepareSpans(const TArray<FImportedRoadWay>& Ways, const TArray<FName>& Filter, bool bRequireTerrain, double HeightOffset, double MergeDistance = 1000.0)
	{
		TArray<const FImportedRoadWay*> Selected;
		TMap<FString, int32> Occurrences;
		for (const FImportedRoadWay& Way : Ways)
		{
			if (!IsGroundRoad(Way, Filter)) continue;
			Selected.Add(&Way);
			for (const FImportedRoadPoint& Point : Way.Points)
			{
				if (Point.WorldLocation.ContainsNaN() || (bRequireTerrain && !Point.bHasTerrain)) continue;
				const FString Key = NodeKey(Way, Point);
				if (!Key.IsEmpty()) ++Occurrences.FindOrAdd(Key);
			}
		}

		TArray<FImportSpan> Result;
		for (const FImportedRoadWay* Way : Selected)
		{
			FImportSpan Span;
			auto ResetSpan = [&]()
			{
				Span = FImportSpan();
				Span.WayId = Way->WayId;
				Span.SourceWayIds.Add(Way->WayId);
				Span.Highway = FName(*Way->Tags.FindChecked(TEXT("highway")));
			};
			auto FlushSpan = [&]()
			{
				if (Span.Positions.Num() >= 2) Result.Add(MoveTemp(Span));
				ResetSpan();
			};
			ResetSpan();
			for (const FImportedRoadPoint& Point : Way->Points)
			{
				if (Point.WorldLocation.ContainsNaN() || (bRequireTerrain && !Point.bHasTerrain))
				{
					FlushSpan();
					continue;
				}
				FVector Position = Point.WorldLocation;
				Position.Z += HeightOffset;
				if (!Span.Positions.IsEmpty() && FVector::DistSquared2D(Span.Positions.Last(), Position) < 1.0) continue;
				const FString Key = NodeKey(*Way, Point);
				if (Span.Positions.IsEmpty()) Span.StartKey = Key;
				Span.Positions.Add(Position);
				Span.EndKey = Key;
				const int32* Count = Occurrences.Find(Key);
				if (Count && *Count > 1 && Span.Positions.Num() >= 2)
				{
					FlushSpan();
					Span.StartKey = Span.EndKey = Key;
					Span.Positions.Add(Position);
				}
			}
			FlushSpan();
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
		auto Root = [&Parent](FString Key)
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
			const FString Start = Root(Span.StartKey);
			const FString End = Root(Span.EndKey);
			if (Start != End) Parent.Add(End, Start);
			Contracted.Add(Index);
		}
		for (int32 Index = Result.Num() - 1; Index >= 0; --Index)
		{
			if (Contracted.Contains(Index)) Result.RemoveAt(Index);
			else
			{
				Result[Index].StartKey = Root(Result[Index].StartKey);
				Result[Index].EndKey = Root(Result[Index].EndKey);
			}
		}
		return Result;
	}

	bool HasSymmetricLaneStyle(const URoadStyle* Style)
	{
		if (!Style) return true;
		if (Style->LeftLanes.Num() != Style->RightLanes.Num()) return false;
		for (int32 Index = 0; Index < Style->LeftLanes.Num(); ++Index)
		{
			const FRoadLaneStyle& Left = Style->LeftLanes[Index];
			const FRoadLaneStyle& Right = Style->RightLanes[Index];
			if (Left.Width != Right.Width || Left.LaneType != Right.LaneType || Left.PhysicalMaterial != Right.PhysicalMaterial ||
				Left.LaneShapeSource != Right.LaneShapeSource || Left.LaneShape != Right.LaneShape ||
				Left.LaneMarking != Right.LaneMarking || Left.Props != Right.Props) return false;
		}
		return true;
	}

	int32 MergeStraightSpans(TArray<FImportSpan>& Spans, double AngleTolerance, TFunctionRef<URoadStyle*(FName)> ResolveStyle)
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
			URoadStyle* Style = ResolveStyle(A.Highway);
			if (Style != ResolveStyle(B.Highway)) continue;
			if ((First.bStart || !Second.bStart) && !HasSymmetricLaneStyle(Style)) continue;
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
	StartGeneration(false);
}

void ARoadGenerator::GenerateLandscapeSplines()
{
	StartGeneration(true);
}

void ARoadGenerator::StartGeneration(bool bOnlyLandscapeSplines)
{
#if WITH_EDITOR
	if (bGenerationInProgress) return;
	bGenerationInProgress = true;
	bLandscapeSplinesOnly = bOnlyLandscapeSplines;
	GenerateFromGameThread(NAME_None, true, [WeakThis = TWeakObjectPtr<ARoadGenerator>(this)](bool)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis]()
		{
			if (WeakThis.IsValid())
			{
				WeakThis->bGenerationInProgress = false;
				WeakThis->bLandscapeSplinesOnly = false;
			}
		});
	});
#endif
}

void ARoadGenerator::ClearRoads()
{
	if (!bGenerationInProgress) Execute_Cleanup(this, true);
}

void ARoadGenerator::ClearLandscapeSplines()
{
#if WITH_EDITOR
	if (bGenerationInProgress) return;
	if (!GeneratedLandscapeSplineActor.IsNull() && !GeneratedLandscapeSplineActor.IsValid())
	{
		UE_LOG(LogRoadConstructorImport, Error, TEXT("Load the previous Landscape spline actor before clearing it."));
		return;
	}
	if (GeneratedScene.IsValid() && GeneratedSplineLandscape.IsValid())
	{
		RemoveRoadConstructorSplines(GeneratedScene.Get(), GeneratedSplineLandscape.Get());
	}
	if (GeneratedLandscapeSplineActor.IsValid()) GeneratedLandscapeSplineActor->Destroy();
	Modify();
	GeneratedLandscapeSplineActor.Reset();
	if (ALandscape* Landscape = GeneratedSplineLandscape.Get()) Landscape->RequestSplineLayerUpdate();
	GeneratedSplineLandscape.Reset();
	MarkPackageDirty();
#endif
}

bool ARoadGenerator::BuildLandscapeSplines(AOsmRoadScene* Scene)
{
#if WITH_EDITOR
	if (!IsValid(Scene) || !IsValid(TargetLandscape) || !TargetLandscape->GetLandscapeInfo()) return false;
	if (!GeneratedLandscapeSplineActor.IsNull() && !GeneratedLandscapeSplineActor.IsValid())
	{
		UE_LOG(LogRoadConstructorImport, Error, TEXT("Load the previous Landscape spline actor before replacing it."));
		return false;
	}
	if (!bDeformLandscape) return true;
	if (TargetLandscape->GetEditLayersOfType(ULandscapeEditLayerSplines::StaticClass()).IsEmpty())
	{
		TargetLandscape->Modify();
		if (!bCreateSplineLayerIfMissing || TargetLandscape->CreateLayer(NAME_None, ULandscapeEditLayerSplines::StaticClass()) == INDEX_NONE)
		{
			UE_LOG(LogRoadConstructorImport, Error, TEXT("Create a reserved Landscape Splines edit layer on Target Landscape, or enable Create Spline Layer If Missing."));
			return false;
		}
	}
	int32 CreatedRoads = 0;
	for (ARoadActor* Road : Scene->Roads)
	{
		if (!IsValid(Road)) continue;
		Road->Modify();
		Road->bEnableTerrainDeform = true;
		Road->TerrainDeformLandscapeSplineHeightOffset =
			GetDefault<AOsmRoadActor>()->TerrainDeformLandscapeSplineHeightOffset - SurfaceOffsetCm;
		Road->CreatePersistentLandscapeSplineEditLayerPrototype();
		if (!HasRoadConstructorSpline(Road, TargetLandscape))
		{
			UE_LOG(LogRoadConstructorImport, Error, TEXT("Road Constructor failed to create a Landscape spline for %s on Target Landscape."), *GetNameSafe(Road));
			RemoveRoadConstructorSplines(Scene, TargetLandscape);
			return false;
		}
		++CreatedRoads;
	}
	if (CreatedRoads == 0) return false;
	if (Scene == GeneratedScene.Get() && GeneratedSplineLandscape.IsValid() && GeneratedSplineLandscape.Get() != TargetLandscape)
		RemoveRoadConstructorSplines(Scene, GeneratedSplineLandscape.Get());
	if (Scene == GeneratedScene.Get())
	{
		if (GeneratedLandscapeSplineActor.IsValid()) GeneratedLandscapeSplineActor->Destroy();
		Modify();
		GeneratedLandscapeSplineActor.Reset();
		GeneratedSplineLandscape = TargetLandscape;
	}
	TargetLandscape->RequestSplineLayerUpdate();
	TargetLandscape->MarkPackageDirty();
	MarkPackageDirty();
	UE_LOG(LogRoadConstructorImport, Display, TEXT("Applied Road Constructor Terrain Deform to %d generated roads."), CreatedRoads);
	return true;
#else
	return false;
#endif
}
TArray<UObject*> ARoadGenerator::GetGeneratedObjects() const
{
	TArray<UObject*> Objects;
	if (GeneratedScene.IsValid()) Objects.Add(GeneratedScene.Get());
	if (GeneratedLandscapeSplineActor.IsValid()) Objects.Add(GeneratedLandscapeSplineActor.Get());
	return Objects;
}

bool ARoadGenerator::Cleanup_Implementation(bool bSkipPrompt)
{
	if ((!GeneratedScene.IsNull() && !GeneratedScene.IsValid()) ||
		(!GeneratedLandscapeSplineActor.IsNull() && !GeneratedLandscapeSplineActor.IsValid()))
	{
		UE_LOG(LogRoadConstructorImport, Error, TEXT("Load the previous Road Constructor scene and Landscape spline actor before clearing them."));
		return false;
	}
#if WITH_EDITOR
	if (GeneratedScene.IsValid() && GeneratedSplineLandscape.IsValid())
		RemoveRoadConstructorSplines(GeneratedScene.Get(), GeneratedSplineLandscape.Get());
#endif
	if (!DeleteGeneratedObjects(bSkipPrompt)) return false;
	Modify();
	GeneratedScene.Reset();
	GeneratedLandscapeSplineActor.Reset();
#if WITH_EDITOR
	if (ALandscape* Landscape = GeneratedSplineLandscape.Get()) Landscape->RequestSplineLayerUpdate();
#endif
	GeneratedSplineLandscape.Reset();
	return true;
}

bool ARoadGenerator::OnGenerate(FName SpawnedActorsPathOverride, bool bIsUserInitiated)
{
#if WITH_EDITOR
	const bool bConfigured = Concurrency::RunOnGameThreadAndWait([this]()
	{
		return (bLandscapeSplinesOnly ? GeneratedScene.IsValid() : (IsValid(OSMRoadImporter) && IsValid(DefaultRoadStyle))) &&
			(!(bLandscapeSplinesOnly || bGenerateLandscapeSplinesWithRoads) || (IsValid(TargetLandscape) && TargetLandscape->GetLandscapeInfo()));
	});
	if (!bConfigured)
	{
		UE_LOG(LogRoadConstructorImport, Error, TEXT("Select an OSMRoadImporter with cached data and RoadStyle for roads, or generate roads first for Terrain Deform. A loaded Target Landscape is required for deformation."));
		return false;
	}
	if (bLandscapeSplinesOnly)
		return Concurrency::RunOnGameThreadAndWait([this]() { return BuildLandscapeSplines(GeneratedScene.Get()); });
	TArray<FImportedRoadWay> Ways;
	if (!OSMRoadImporter->GetCachedRoadData(Ways)) return false;
	return Concurrency::RunOnGameThreadAndWait([&]()
	{
		return BuildRoads(Ways, SpawnedActorsPathOverride);
	});
#else
	return false;
#endif
}

bool ARoadGenerator::BuildRoads(const TArray<FImportedRoadWay>& Ways, FName OutputFolderPath)
{
#if WITH_EDITOR
	if (!GeneratedScene.IsNull() && !GeneratedScene.IsValid())
	{
		UE_LOG(LogRoadConstructorImport, Error, TEXT("Load the previous generated scene before replacing it."));
		return false;
	}
	if (!FMath::IsFinite(SurfaceOffsetCm) || !FMath::IsFinite(LandscapeHeightOffsetCm) ||
		!FMath::IsFinite(RoadCurveRadiusCm) || !FMath::IsFinite(JunctionMergeDistanceCm)) return false;
	if (bOverrideJunctionRadius && !FMath::IsFinite(JunctionRadiusCm)) return false;
	if (!FMath::IsFinite(StraightConnectionToleranceDegrees)) return false;
	const double RoadHeightOffset = LandscapeHeightOffsetCm + SurfaceOffsetCm;
	if (!FMath::IsFinite(RoadHeightOffset)) return false;
	TArray<FImportSpan> Spans = PrepareSpans(Ways, HighwayFilter, bRequireTerrainSamples, RoadHeightOffset, JunctionMergeDistanceCm);
	auto ResolveStyle = [this](FName Highway) -> URoadStyle*
	{
		const TObjectPtr<URoadStyle>* Override = HighwayStyles.Find(Highway);
		return Override && IsValid(Override->Get()) ? Override->Get() : DefaultRoadStyle.Get();
	};
	const int32 MergeCount = bMergeStraightRoads ? MergeStraightSpans(Spans, StraightConnectionToleranceDegrees, ResolveStyle) : 0;
	UE_LOG(LogRoadConstructorImport, Display, TEXT("Merged %d compatible straight two-arm connections before actor creation; %d road spans remain."), MergeCount, Spans.Num());
	if (Spans.IsEmpty())
	{
		UE_LOG(LogRoadConstructorImport, Error, TEXT("The cache contains no usable ground roads for the selected filter. Previous output was kept."));
		return false;
	}
	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.OverrideLevel = GetLevel();
	Params.ObjectFlags = RF_Transactional;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AOsmRoadScene* Candidate = GetWorld()->SpawnActor<AOsmRoadScene>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (!Candidate) return false;
	Candidate->bOverrideJunctionRadius = bOverrideJunctionRadius;
	if (bOverrideJunctionRadius) Candidate->ImportedJunctionRadius = FMath::Max(0.0, JunctionRadiusCm);
	Candidate->bCleanJunctionApproaches = bCleanJunctionApproaches;
	Candidate->bRemoveCornersWithoutJunctionSurface = bRemoveCornersWithoutJunctionSurface;
	Candidate->SetActorLabel(GetActorLabel() + TEXT("_RoadConstructor"));
	const FName EffectiveFolder = OutputFolderPath.IsNone() ? GetFolderPath() : OutputFolderPath;
	Candidate->SetFolderPath(EffectiveFolder);
	FScopedSlowTask Progress(Spans.Num() + 1, NSLOCTEXT("Landscapes", "ImportRoadConstructor", "Generating Road Constructor roads"));
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
		Road->InitWithStyle(ResolveStyle(Span.Highway), Span.Positions[0].Z);
		Road->bHasGround = false; // Terrain remains Landscape Combinator's responsibility.
		Road->ImportedPositions = Span.Positions;
		Road->ImportedCurveRadius = FMath::Clamp(RoadCurveRadiusCm, 1.0, 5000000.0);
		Road->StartNodeKey = Span.StartKey;
		Road->EndNodeKey = Span.EndKey;
		Road->Tags.AddUnique(TEXT("RC_DisableLegacyHighwaySideConnector"));
		for (int64 WayId : Span.SourceWayIds) Road->Tags.AddUnique(FName(*FString::Printf(TEXT("OSMWay_%lld"), WayId)));
		Road->SetActorLabel(FString::Printf(TEXT("OSM_%lld_%d"), Span.WayId, Index));
		Road->SetFolderPath(EffectiveFolder);
	}
	Progress.EnterProgressFrame(1);
	if (!Candidate->BuildImportedNetwork())
	{
		Candidate->Destroy();
		return false;
	}
	if (bGenerateLandscapeSplinesWithRoads && !BuildLandscapeSplines(Candidate))
	{
		RemoveRoadConstructorSplines(Candidate, TargetLandscape);
		Candidate->Destroy();
		return false;
	}
	// Publish only after the replacement has built successfully.
	if (GeneratedScene.IsValid() && GeneratedSplineLandscape.IsValid())
		RemoveRoadConstructorSplines(GeneratedScene.Get(), GeneratedSplineLandscape.Get());
	if (GeneratedScene.IsValid()) GeneratedScene->Destroy();
	if (GeneratedLandscapeSplineActor.IsValid()) GeneratedLandscapeSplineActor->Destroy();
	Modify();
	GeneratedScene = Candidate;
	GeneratedLandscapeSplineActor.Reset();
	GeneratedSplineLandscape = bGenerateLandscapeSplinesWithRoads && bDeformLandscape ? TargetLandscape : nullptr;
	Candidate->MarkPackageDirty();
	MarkPackageDirty();
	UE_LOG(LogRoadConstructorImport, Display, TEXT("Road Constructor generated from the OSMRoadImporter cache. Bridges, tunnels and nonzero layers were excluded. Styles supply widths and sidewalks."));
	return true;
#else
	return false;
#endif
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoadConstructorImportTopologyTest, "Landscapes.RoadConstructor.ImportTopology", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoadConstructorImportTopologyTest::RunTest(const FString& Parameters)
{
	auto MakeWay = [](int64 Id, std::initializer_list<FImportedRoadPoint> Points)
	{
		FImportedRoadWay Way;
		Way.WayId = Id;
		Way.Tags.Add(TEXT("highway"), TEXT("residential"));
		for (const FImportedRoadPoint& Point : Points) Way.Points.Add(Point);
		return Way;
	};
	FImportedRoadWay Main = MakeWay(1, {{ 10, FVector(-5000, 0, 100), true }, { 11, FVector(0, 0, 200), true }, { 12, FVector(5000, 0, 300), true }});
	FImportedRoadWay Branch = MakeWay(2, {{ 11, FVector(0, 0, 200), true }, { 13, FVector(0, 5000, 400), true }});
	TArray<FImportSpan> Spans = PrepareSpans({ Main, Branch }, {}, true, 5.0);
	TestEqual(TEXT("T junction splits the through way"), Spans.Num(), 3);
	if (Spans.Num() == 3)
	{
		TestEqual(TEXT("All arms retain one shared node key"), Spans[0].EndKey, Spans[2].StartKey);
		TestEqual(TEXT("Both halves meet at the same node"), Spans[1].StartKey, Spans[2].StartKey);
		TestEqual(TEXT("Elevation offset is applied once"), Spans[0].Positions.Last().Z, 205.0);
	}
	Branch.Points[0].NodeId = 99;
	TestEqual(TEXT("Same position with a different node ID is not a junction"), PrepareSpans({ Main, Branch }, {}, true, 0).Num(), 2);
	Branch.Tags.Add(TEXT("bridge"), TEXT("yes"));
	TestEqual(TEXT("Bridge is excluded before connectivity is calculated"), PrepareSpans({ Main, Branch }, {}, true, 0).Num(), 1);
	Main.Points[1].bHasTerrain = false;
	TestEqual(TEXT("Missing terrain does not create a spanning road"), PrepareSpans({ Main }, {}, true, 0).Num(), 0);
	TestEqual(TEXT("Terrain requirement can be disabled"), PrepareSpans({ Main }, {}, false, 0).Num(), 1);
	FImportedRoadWay Loop = MakeWay(3, {{ 20, FVector(0, 0, 0), true }, { 21, FVector(5000, 0, 0), true }, { 22, FVector(5000, 5000, 0), true }, { 20, FVector(0, 0, 0), true }});
	TestEqual(TEXT("Closed way uses two distinct native roads"), PrepareSpans({ Loop }, {}, true, 0).Num(), 2);
	FImportedRoadWay Connector = MakeWay(4, {{ 30, FVector(0, 0, 0), true }, { 31, FVector(500, 0, 0), true }});
	FImportedRoadWay CrossA = MakeWay(5, {{ 32, FVector(0, -5000, 0), true }, { 30, FVector(0, 0, 0), true }, { 33, FVector(0, 5000, 0), true }});
	FImportedRoadWay CrossB = MakeWay(6, {{ 34, FVector(500, -5000, 0), true }, { 31, FVector(500, 0, 0), true }, { 35, FVector(500, 5000, 0), true }});
	Spans = PrepareSpans({ Connector, CrossA, CrossB }, {}, true, 0);
	TestEqual(TEXT("Short junction connector is contracted"), Spans.Num(), 4);
	if (Spans.Num() == 4) TestEqual(TEXT("Contracted nodes share connectivity"), Spans[0].EndKey, Spans[2].EndKey);
	TestEqual(TEXT("Contraction can be disabled"), PrepareSpans({ Connector, CrossA, CrossB }, {}, true, 0, 0).Num(), 5);

	FImportedRoadWay StraightA = MakeWay(100, {{ 1000, FVector(0, 0, 0), true }, { 1001, FVector(5000, 0, 0), true }});
	FImportedRoadWay StraightB = MakeWay(101, {{ 1001, FVector(5000, 0, 0), true }, { 1002, FVector(10000, 0, 0), true }});
	FImportedRoadWay StraightC = MakeWay(102, {{ 1003, FVector(15000, 0, 0), true }, { 1002, FVector(10000, 0, 0), true }});
	auto SameStyle = [](FName) -> URoadStyle* { return nullptr; };
	Spans = PrepareSpans({ StraightA, StraightB, StraightC }, {}, true, 0, 0);
	TestEqual(TEXT("Straight chain merges regardless of endpoint order"), MergeStraightSpans(Spans, 10, SameStyle), 2);
	TestEqual(TEXT("Chain becomes one actor input"), Spans.Num(), 1);
	if (Spans.Num() == 1)
	{
		TestEqual(TEXT("Shared samples appear once, with all original controls retained"), Spans[0].Positions.Num(), 4);
		TestEqual(TEXT("All original OSM way IDs are retained"), Spans[0].SourceWayIds.Num(), 3);
		TestTrue(TEXT("Merged ends are the original outer nodes"),
			(Spans[0].StartKey == TEXT("1000:0") && Spans[0].EndKey == TEXT("1003:0")) ||
			(Spans[0].EndKey == TEXT("1000:0") && Spans[0].StartKey == TEXT("1003:0")));
	}
	StraightB.Points.Last().WorldLocation = FVector(5000, 5000, 0);
	Spans = PrepareSpans({ StraightA, StraightB }, {}, true, 0, 0);
	TestEqual(TEXT("90 degree connection stays a corner junction"), MergeStraightSpans(Spans, 10, SameStyle), 0);
	StraightB.Points.Last().WorldLocation = FVector(10000, 400, 0);
	Spans = PrepareSpans({ StraightA, StraightB }, {}, true, 0, 0);
	TestEqual(TEXT("Small deviation inside tolerance merges"), MergeStraightSpans(Spans, 10, SameStyle), 1);
	Spans = PrepareSpans({ StraightA, StraightB }, {}, true, 0, 0);
	TestEqual(TEXT("Stricter angle tolerance preserves the connection"), MergeStraightSpans(Spans, 1, SameStyle), 0);
	StraightB.Points.Last().WorldLocation = FVector(10000, 0, 0);
	FImportedRoadWay ThirdArm = MakeWay(103, {{ 1001, FVector(5000, 0, 0), true }, { 1004, FVector(5000, 5000, 0), true }});
	Spans = PrepareSpans({ StraightA, StraightB, ThirdArm }, {}, true, 0, 0);
	TestEqual(TEXT("A branch prevents merging the two opposite arms"), MergeStraightSpans(Spans, 10, SameStyle), 0);
	URoadStyle* StyleA = NewObject<URoadStyle>();
	URoadStyle* StyleB = NewObject<URoadStyle>();
	StraightB.Tags.Add(TEXT("highway"), TEXT("primary"));
	auto DifferentStyles = [&](FName Highway) { return Highway == TEXT("primary") ? StyleB : StyleA; };
	Spans = PrepareSpans({ StraightA, StraightB }, {}, true, 0, 0);
	TestEqual(TEXT("Different selected styles remain separate"), MergeStraightSpans(Spans, 10, DifferentStyles), 0);
	// Empty override entries and different highway tags may still resolve to one asset.
	Spans = PrepareSpans({ StraightA, StraightB }, {}, true, 0, 0);
	TestEqual(TEXT("Different highway tags merge when their resolved style is identical"), MergeStraightSpans(Spans, 10, SameStyle), 1);
	StyleA->LeftLanes.SetNum(1);
	StyleA->RightLanes.Reset();
	Spans = PrepareSpans({ StraightA, StraightB }, {}, true, 0, 0);
	Algo::Reverse(Spans[1].Positions);
	Swap(Spans[1].StartKey, Spans[1].EndKey);
	TestEqual(TEXT("Reversal cannot swap an asymmetric lane layout"), MergeStraightSpans(Spans, 10, [&](FName) { return StyleA; }), 0);
	Spans = PrepareSpans({ StraightA, StraightB }, {}, true, 0, 0);
	Spans[1].Positions[0].Z += 2;
	TestEqual(TEXT("Separated elevations at aliased nodes cannot be joined"), MergeStraightSpans(Spans, 10, SameStyle), 0);
	Spans = PrepareSpans({ Loop }, {}, true, 0, 0);
	TestEqual(TEXT("Merging cannot turn split loops into unsupported same-road junctions"), MergeStraightSpans(Spans, 45, SameStyle), 0);
	return true;
}
#endif
