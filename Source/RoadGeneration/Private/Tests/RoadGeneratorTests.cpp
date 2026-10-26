#include "RoadGenerator.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "ImportedRoadData.h"
#include "OsmRoadScene.h"
#include "RoadGenerationSignatures.h"
#include "Algo/Reverse.h"
#include "Misc/AutomationTest.h"
#if WITH_EDITOR
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Containers/Ticker.h"
#include "TimerManager.h"
#include "Components/StaticMeshComponent.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "Landscape.h"
#include "LandscapeSplinesComponent.h"
#include "LandscapeSplineControlPoint.h"
#endif

using namespace RoadGeneratorInternal;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoadGeneratorLaneCountTest, "Landscapes.RoadConstructor.LaneCounts", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoadGeneratorLaneCountTest::RunTest(const FString& Parameters)
{
	URoadStyle* Source = NewObject<URoadStyle>();
	FRoadLaneStyle Driving;
	Driving.Width = 325;
	Driving.LaneShapeSource = ERoadLaneShapeSource::UseCustomLaneShape;
	Driving.LaneShape = NewObject<ULaneShape>();
	Driving.PhysicalMaterial = NewObject<URoadStyle>(); // Identity sentinel; no geometry is built in this test.
	Driving.LaneMarking = NewObject<ULaneMarkStyle>();
	Driving.Props = NewObject<URoadProps>();
	FRoadLaneStyle Sidewalk;
	Sidewalk.LaneType = ELaneType::Sidewalk;
	Sidewalk.Width = 180;
	FRoadLaneStyle Shoulder;
	Shoulder.LaneType = ELaneType::Shoulder;
	Shoulder.Width = 75;
	Source->LeftLanes = { Driving, Sidewalk };
	Driving.Width = 375;
	Source->RightLanes = { Driving, Shoulder, Sidewalk };
	Source->bHasGround = false;
	Source->GeneratedRailingHeight = 147;
	auto CountDriving = [](const TArray<FRoadLaneStyle>& Lanes)
	{
		int32 Count = 0;
		for (const FRoadLaneStyle& Lane : Lanes) Count += Lane.LaneType == ELaneType::Driving;
		return Count;
	};
	UObject* Owner = NewObject<URoadStyle>();
	for (const int32 Count : { 1, 2, 3, 4, 5 })
	{
		URoadStyle* Style = CreateLaneCountStyle(Owner, Source, Count);
		TestTrue(TEXT("Overrides use an owned copy of the selected highway style"), Style != Source && Style->GetOuter() == Owner);
		TestTrue(TEXT("Embedded styles support cross-package road references without becoming standalone assets"),
			Style->HasAnyFlags(RF_Public) && !Style->HasAnyFlags(RF_Standalone));
		TestEqual(TEXT("Left receives the extra lane for odd totals"), CountDriving(Style->LeftLanes), (Count + 1) / 2);
		TestEqual(TEXT("Right receives half the total"), CountDriving(Style->RightLanes), Count / 2);
		TestEqual(TEXT("Left sidewalk is retained outside the driving lanes"), Style->LeftLanes.Last().Width, Sidewalk.Width);
		TestEqual(TEXT("Right non-driving lanes remain in order"), Style->RightLanes[Style->RightLanes.Num() - 2].LaneType, ELaneType::Shoulder);
		TestEqual(TEXT("Unrelated style attributes are copied"), Style->GeneratedRailingHeight, Source->GeneratedRailingHeight);
		TestEqual(TEXT("Ground setting is copied"), Style->bHasGround, Source->bHasGround);
		for (const FRoadLaneStyle& Lane : Style->LeftLanes)
		{
			if (Lane.LaneType != ELaneType::Driving) continue;
			TestEqual(TEXT("Left driving width is inherited"), Lane.Width, 325.0);
			TestTrue(TEXT("Driving profiles, materials, markings and props are inherited"),
				Lane.LaneShapeSource == Driving.LaneShapeSource && Lane.LaneShape == Driving.LaneShape &&
				Lane.PhysicalMaterial == Driving.PhysicalMaterial && Lane.LaneMarking == Driving.LaneMarking && Lane.Props == Driving.Props);
		}
		for (const FRoadLaneStyle& Lane : Style->RightLanes)
			if (Lane.LaneType == ELaneType::Driving) TestEqual(TEXT("Right driving width is inherited independently"), Lane.Width, 375.0);
	}
	TestEqual(TEXT("Source left lane count is unchanged"), Source->LeftLanes.Num(), 2);
	TestEqual(TEXT("Source right lane count is unchanged"), Source->RightLanes.Num(), 3);
	URoadStyle* Unchanged = CreateLaneCountStyle(Owner, Source, 0);
	TestTrue(TEXT("Absent count still creates an independent copy"), Unchanged != Source);
	TestEqual(TEXT("Absent count retains the source layout"), Unchanged->LeftLanes.Num(), Source->LeftLanes.Num());
	Source->LeftLanes = { Sidewalk };
	URoadStyle* Borrowed = CreateLaneCountStyle(Owner, Source, 3);
	TestEqual(TEXT("A side with no driving lane borrows opposite-side attributes"), Borrowed->LeftLanes[0].Width, 375.0);
	Source->RightLanes = { Shoulder, Sidewalk };
	URoadStyle* NoDriving = CreateLaneCountStyle(Owner, Source, 3);
	TestTrue(TEXT("Styles without driving lanes are copied"), NoDriving != Source);
	TestEqual(TEXT("Styles without driving lanes retain their layout"), NoDriving->RightLanes.Num(), Source->RightLanes.Num());

	FImportedRoadWay Way;
	Way.WayId = 1;
	Way.Tags.Add(TEXT("highway"), TEXT("primary"));
	Way.Points = { { 1, FVector(0, 0, 0), true }, { 2, FVector(5000, 0, 0), true } };
	for (const FString Value : { TEXT(""), TEXT("0"), TEXT("-1"), TEXT("3.5"), TEXT("2;3"), TEXT("2junk"), TEXT("65"), TEXT("99999999999999999999") })
	{
		Way.Tags.Add(TEXT("lanes"), Value);
		const TArray<FImportSpan> Spans = PrepareSpans({ Way }, {}, true, 0, 0);
		if (TestEqual(TEXT("Way is retained when lanes is invalid"), Spans.Num(), 1))
			TestEqual(TEXT("Invalid lane count keeps the style layout"), Spans[0].LaneCount, 0);
	}
	Way.Tags.Add(TEXT("lanes"), TEXT(" 3 "));
	FImportedRoadWay Branch;
	Branch.WayId = 2;
	Branch.Tags.Add(TEXT("highway"), TEXT("primary"));
	Branch.Points = { { 2, FVector(5000, 0, 0), true }, { 3, FVector(10000, 0, 0), true } };
	TArray<FImportSpan> Spans = PrepareSpans({ Way, Branch }, {}, true, 0, 0);
	TestEqual(TEXT("Lane tag reaches the generator"), Spans[0].LaneCount, 3);
	TestEqual(TEXT("Missing lane tag retains the highway default"), Spans[1].LaneCount, 0);
	auto SameStyle = [Source](const FImportSpan&) { return Source; };
	TestEqual(TEXT("Lane-count changes cannot be lost through merging"), MergeStraightSpans(Spans, 10, SameStyle), 0);
	Branch.Tags.Add(TEXT("lanes"), TEXT("3"));
	Spans = PrepareSpans({ Way, Branch }, {}, true, 0, 0);
	TestEqual(TEXT("Equal lane counts can merge without reversal"), MergeStraightSpans(Spans, 10, SameStyle), 1);
	Source->LeftLanes = Source->RightLanes = { Driving, Sidewalk };
	Spans = PrepareSpans({ Way, Branch }, {}, true, 0, 0);
	Algo::Reverse(Spans[1].Positions);
	Swap(Spans[1].StartKey, Spans[1].EndKey);
	TestEqual(TEXT("Odd lane layouts cannot reverse during merging"), MergeStraightSpans(Spans, 10, SameStyle), 0);
	Way.Points.Append({ { 3, FVector(5000, 5000, 0), true }, { 1, FVector(0, 0, 0), true } });
	Spans = PrepareSpans({ Way }, {}, true, 0, 0);
	if (TestEqual(TEXT("Closed way splits into two spans"), Spans.Num(), 2))
	{
		TestEqual(TEXT("Loop head retains lanes"), Spans[0].LaneCount, 3);
		TestEqual(TEXT("Loop tail retains lanes"), Spans[1].LaneCount, 3);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoadConstructorImportTopologyTest, "Landscapes.RoadConstructor.ImportTopology", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoadConstructorImportTopologyTest::RunTest(const FString& Parameters)
{
	auto make_way = [](const int64 Id, const std::initializer_list<FImportedRoadPoint> Points)
	{
		FImportedRoadWay Way;
		Way.WayId = Id;
		Way.Tags.Add(TEXT("highway"), TEXT("residential"));
		for (const FImportedRoadPoint& Point : Points) Way.Points.Add(Point);
		return Way;
	};
	FImportedRoadWay Main = make_way(1, {{ 10, FVector(-5000, 0, 100), true }, { 11, FVector(0, 0, 200), true }, { 12, FVector(5000, 0, 300), true }});
	FImportedRoadWay Branch = make_way(2, {{ 11, FVector(0, 0, 200), true }, { 13, FVector(0, 5000, 400), true }});
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
	TestEqual(TEXT("Bridge tag does not exclude a selected way"), PrepareSpans({ Main, Branch }, {}, true, 0).Num(), 2);
	Main.Points[1].bHasTerrain = false;
	TestEqual(TEXT("Missing terrain does not create a spanning road"), PrepareSpans({ Main }, {}, true, 0).Num(), 0);
	TestEqual(TEXT("Terrain requirement can be disabled"), PrepareSpans({ Main }, {}, false, 0).Num(), 1);
	FImportedRoadWay Loop = make_way(3, {{ 20, FVector(0, 0, 0), true }, { 21, FVector(5000, 0, 0), true }, { 22, FVector(5000, 5000, 0), true }, { 20, FVector(0, 0, 0), true }});
	TestEqual(TEXT("Closed way uses two distinct native roads"), PrepareSpans({ Loop }, {}, true, 0).Num(), 2);
	FImportedRoadWay Connector = make_way(4, {{ 30, FVector(0, 0, 0), true }, { 31, FVector(500, 0, 0), true }});
	FImportedRoadWay CrossA = make_way(5, {{ 32, FVector(0, -5000, 0), true }, { 30, FVector(0, 0, 0), true }, { 33, FVector(0, 5000, 0), true }});
	FImportedRoadWay CrossB = make_way(6, {{ 34, FVector(500, -5000, 0), true }, { 31, FVector(500, 0, 0), true }, { 35, FVector(500, 5000, 0), true }});
	Spans = PrepareSpans({ Connector, CrossA, CrossB }, {}, true, 0);
	TestEqual(TEXT("Short junction connector is contracted"), Spans.Num(), 4);
	if (Spans.Num() == 4) TestEqual(TEXT("Contracted nodes share connectivity"), Spans[0].EndKey, Spans[2].EndKey);
	TestEqual(TEXT("Contraction can be disabled"), PrepareSpans({ Connector, CrossA, CrossB }, {}, true, 0, 0).Num(), 5);

	FImportedRoadWay StraightA = make_way(100, {{ 1000, FVector(0, 0, 0), true }, { 1001, FVector(5000, 0, 0), true }});
	FImportedRoadWay StraightB = make_way(101, {{ 1001, FVector(5000, 0, 0), true }, { 1002, FVector(10000, 0, 0), true }});
	FImportedRoadWay StraightC = make_way(102, {{ 1003, FVector(15000, 0, 0), true }, { 1002, FVector(10000, 0, 0), true }});
	auto SameStyle = [](const FImportSpan&) -> URoadStyle* { return nullptr; };
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
	FImportedRoadWay ThirdArm = make_way(103, {{ 1001, FVector(5000, 0, 0), true }, { 1004, FVector(5000, 5000, 0), true }});
	Spans = PrepareSpans({ StraightA, StraightB, ThirdArm }, {}, true, 0, 0);
	TestEqual(TEXT("A branch prevents merging the two opposite arms"), MergeStraightSpans(Spans, 10, SameStyle), 0);
	URoadStyle* StyleA = NewObject<URoadStyle>();
	URoadStyle* StyleB = NewObject<URoadStyle>();
	StraightB.Tags.Add(TEXT("highway"), TEXT("primary"));
	auto DifferentStyles = [&](const FImportSpan& Span) { return Span.Highway == TEXT("primary") ? StyleB : StyleA; };
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
	TestEqual(TEXT("Reversal cannot swap an asymmetric lane layout"), MergeStraightSpans(Spans, 10, [&](const FImportSpan&) { return StyleA; }), 0);
	Spans = PrepareSpans({ StraightA, StraightB }, {}, true, 0, 0);
	Spans[1].Positions[0].Z += 2;
	TestEqual(TEXT("Separated elevations at aliased nodes cannot be joined"), MergeStraightSpans(Spans, 10, SameStyle), 0);
	Spans = PrepareSpans({ Loop }, {}, true, 0, 0);
	TestEqual(TEXT("Merging cannot turn split loops into unsupported same-road junctions"), MergeStraightSpans(Spans, 45, SameStyle), 0);
	FImportedRoadWay Footway = StraightA;
	Footway.Tags.Add(TEXT("highway"), TEXT("footway"));
	TestEqual(TEXT("Explicit highway filter includes footways"), PrepareSpans({ Footway }, { TEXT("footway") }, true, 0, 0).Num(), 1);
	TestEqual(TEXT("Empty highway filter includes non-driving ways"), PrepareSpans({ Footway }, {}, true, 0, 0).Num(), 1);
	TestEqual(TEXT("A different highway filter still excludes footways"), PrepareSpans({ Footway }, { TEXT("residential") }, true, 0, 0).Num(), 0);
	Footway.Tags.Add(TEXT("bridge"), TEXT("yes"));
	TestEqual(TEXT("Explicit footway entry includes bridges"), PrepareSpans({ Footway }, { TEXT("footway") }, true, 0, 0).Num(), 1);
	Footway.Tags.Remove(TEXT("bridge"));
	Footway.Tags.Add(TEXT("highway"), TEXT("construction"));
	TestEqual(TEXT("Construction is included when listed"), PrepareSpans({ Footway }, { TEXT("construction") }, true, 0, 0).Num(), 1);
	for (const TCHAR* Kind : { TEXT("path"), TEXT("steps"), TEXT("cycleway"), TEXT("pedestrian"), TEXT("bridleway"), TEXT("proposed"), TEXT("platform"), TEXT("custom_highway") })
	{
		Footway.Tags.Add(TEXT("highway"), Kind);
		TestEqual(TEXT("Every highway value is eligible with an empty filter"), PrepareSpans({ Footway }, {}, true, 0, 0).Num(), 1);
		TestEqual(TEXT("Every highway value is eligible when listed"), PrepareSpans({ Footway }, { FName(Kind) }, true, 0, 0).Num(), 1);
	}
	Footway.Tags.Add(TEXT("tunnel"), TEXT("yes"));
	Footway.Tags.Add(TEXT("layer"), TEXT("-1"));
	Footway.Tags.Add(TEXT("area"), TEXT("yes"));
	TestEqual(TEXT("Tunnel, layer and area tags do not exclude ways"), PrepareSpans({ Footway }, { TEXT("custom_highway") }, true, 0, 0).Num(), 1);
	TestEqual(TEXT("Other tags cannot bypass the highway filter"), PrepareSpans({ Footway }, { TEXT("residential") }, true, 0, 0).Num(), 0);
	return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoadGeneratorChangeDetectionTest,
	"Landscapes.RoadConstructor.ChangeDetection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoadGeneratorChangeDetectionTest::RunTest(const FString& Parameters)
{
	const UWorld::InitializationValues Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Change detection world"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
	ARoadGenerator* Generator = World->SpawnActor<ARoadGenerator>();
	if (TestNotNull(TEXT("Change detection generator"), Generator))
	{
		URoadStyle* Style = DuplicateObject<URoadStyle>(Generator->DefaultRoadStyle.Get(), Generator);
		URoadProps* Props = NewObject<URoadProps>();
		FRoadProp Prop{};
		Prop.Spacing = 500;
		Props->Props.Add(Prop);
		Style->BaseCurveProps = Props;
		Generator->DefaultRoadStyle = Style;
		Generator->HighwayFilter = { TEXT("residential") };
		Generator->bMergeStraightRoads = false;
		FImportedRoadWay Main, Branch;
		Main.WayId = 1;
		Main.Tags.Add(TEXT("highway"), TEXT("residential"));
		Main.Points = { { 1, FVector(-10000, 0, 100), true }, { 2, FVector(0, 0, 100), true }, { 3, FVector(10000, 0, 100), true } };
		Branch.WayId = 2;
		Branch.Tags = Main.Tags;
		Branch.Points = { { 2, FVector(0, 0, 100), true }, { 4, FVector(0, 10000, 100), true } };
		TArray<FImportedRoadWay> Ways = { Main, Branch };
		if (TestTrue(TEXT("Initial generation succeeds"), Generator->BuildRoads(Ways, NAME_None)))
		{
			AOsmRoadScene* Initial = Generator->GeneratedScene.Get();
			const auto Junctions = Initial->Junctions;
			// Real button clicks are separated by editor ticks. Labels/transactions can
			// queue a deferred scene rebuild after the initial generation returns.
			{
				TGuardValue<uint64> RestoreFrameCounter(GFrameCounter, GFrameCounter);
				for (int32 Tick = 0; Tick < 3; ++Tick)
				{
					++GFrameCounter;
					FTSTicker::GetCoreTicker().Tick(0.1f);
					World->GetTimerManager().Tick(0.1f);
				}
			}
			TestTrue(TEXT("Deferred editor callbacks do not solve unchanged junctions again"), Initial->Junctions == Junctions);
			Initial->Roads[0]->SetActorLabel(TEXT("Renamed imported road"));
			FTSTicker::GetCoreTicker().Tick(0.1f);
			TestTrue(TEXT("Renaming a road does not solve junctions again"), Initial->Junctions == Junctions);
			if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Initial->Roads[0]->GetRootComponent()))
			{
				UStaticMesh* OriginalMesh = Mesh->GetStaticMesh();
				Mesh->SetStaticMesh(nullptr);
				TestFalse(TEXT("Mesh-less spans still have an output fingerprint"), RoadGenerationSignatures::RoadOutput(Initial).IsEmpty());
				Mesh->SetStaticMesh(OriginalMesh);
			}
			TestFalse(TEXT("Successful road output establishes a cache"), Generator->CachedRoadOutput.IsEmpty());
			TestTrue(TEXT("Unchanged generation succeeds"), Generator->BuildRoads(Ways, NAME_None));
			TestTrue(TEXT("Unchanged request retains the exact scene and roads"), Generator->GeneratedScene.Get() == Initial);
			TestTrue(TEXT("Unchanged request does not solve junctions again"), Initial->Junctions == Junctions);
			Generator->LandscapeHeightOffsetCm = 500;
			Generator->LandscapeSplineSideFalloffCm = 250;
			Generator->JunctionRadiusCm = 12345; // Override is disabled.
			Generator->StraightConnectionToleranceDegrees = 45; // Merging is disabled.
			Generator->HighwayStyles.Add(TEXT("footway"), NewObject<URoadStyle>());
			Ways[0].Tags.Add(TEXT("name"), TEXT("Unused generation tag"));
			Algo::Reverse(Ways);
			TestTrue(TEXT("Changes unrelated to road output succeed"), Generator->BuildRoads(Ways, NAME_None));
			TestTrue(TEXT("Unused styles, terrain settings, disabled parameters and input ordering do not regenerate roads"), Generator->GeneratedScene.Get() == Initial);
			Style->LeftLanes[0].Width += 25;
			TestTrue(TEXT("An in-place source style edit regenerates"), Generator->BuildRoads(Ways, NAME_None));
			TestTrue(TEXT("Style content changes replace road output"), Generator->GeneratedScene.Get() != Initial);
			AOsmRoadScene* Previous = Generator->GeneratedScene.Get();
			const FString PreviousInputs = Generator->CachedRoadInputs;
			Props->Props[0].Spacing = 750;
			TestTrue(TEXT("Referenced prop data edits regenerate"), Generator->BuildRoads(Ways, NAME_None));
			TestTrue(TEXT("Referenced asset content participates in input detection"), Generator->CachedRoadInputs != PreviousInputs);
			TestTrue(TEXT("Changed props replace output"), Generator->GeneratedScene.Get() != Previous);
			Previous = Generator->GeneratedScene.Get();
			Ways[0].Points.Last().WorldLocation.Z += 20;
			TestTrue(TEXT("Changed cached source coordinates regenerate"), Generator->BuildRoads(Ways, NAME_None));
			TestTrue(TEXT("Source changes replace road output"), Generator->GeneratedScene.Get() != Previous);
			Previous = Generator->GeneratedScene.Get();
			Generator->HighwayFilter = { TEXT("footway") };
			AddExpectedError(TEXT("cache contains no usable roads"), EAutomationExpectedErrorFlags::Contains, 1);
			TestFalse(TEXT("Unusable inputs do not overwrite the successful baseline"), Generator->BuildRoads(Ways, NAME_None));
			TestTrue(TEXT("Failure preserves output"), Generator->GeneratedScene.Get() == Previous);
			Generator->HighwayFilter = { TEXT("residential") };
			TestTrue(TEXT("Restored inputs reuse successful output"), Generator->BuildRoads(Ways, NAME_None));
			TestTrue(TEXT("Failed generation did not replace the cache"), Generator->GeneratedScene.Get() == Previous);
			Previous->Roads[0]->LastAppliedRoadStyle->LeftLanes[0].Width += 100;
			TestTrue(TEXT("Edited output invalidates the no-change shortcut"), Generator->BuildRoads(Ways, NAME_None));
			TestTrue(TEXT("Output edits are repaired"), Generator->GeneratedScene.Get() != Previous);
			Previous = Generator->GeneratedScene.Get();
			Previous->Roads[0]->Destroy();
			TestTrue(TEXT("A deleted road invalidates the shortcut"), Generator->BuildRoads(Ways, NAME_None));
			TestTrue(TEXT("Missing output is regenerated"), Generator->GeneratedScene.Get() != Previous);
			Generator->ClearRoads();
			TestTrue(TEXT("Clear resets the successful baseline"), Generator->CachedRoadInputs.IsEmpty());
			TestTrue(TEXT("Generation works again after Clear"), Generator->BuildRoads(Ways, NAME_None));
		}
		Generator->ClearRoads();
	}
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoadGeneratorWayStylesTest,
	"Landscapes.RoadConstructor.WayStyles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoadGeneratorWayStylesTest::RunTest(const FString& Parameters)
{
	const UWorld::InitializationValues Init = UWorld::InitializationValues()
		.AllowAudioPlayback(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Isolated way-style world"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
	ARoadGenerator* Generator = World->SpawnActor<ARoadGenerator>();
	if (TestNotNull(TEXT("Road generator"), Generator) && TestNotNull(TEXT("Default street style"), Generator->DefaultRoadStyle.Get()))
	{
		URoadStyle* Default = Generator->DefaultRoadStyle;
		URoadStyle* Primary = DuplicateObject<URoadStyle>(Default, Generator);
		Primary->GeneratedRailingHeight = Default->GeneratedRailingHeight + 25;
		Generator->HighwayStyles.Add(TEXT("primary"), Primary);
		Generator->HighwayStyles.Add(TEXT("service"), nullptr); // Empty entries use the default base.
		Generator->HighwayFilter.Reset();
		auto MakeWay = [](int64 Id, const TCHAR* Highway, const TCHAR* Lanes, TArray<FImportedRoadPoint> Points)
		{
			FImportedRoadWay Way;
			Way.WayId = Id;
			Way.Tags.Add(TEXT("highway"), Highway);
			if (Lanes) Way.Tags.Add(TEXT("lanes"), Lanes);
			Way.Points = MoveTemp(Points);
			return Way;
		};
		const TArray<FImportedRoadWay> Ways = {
			MakeWay(1, TEXT("residential"), TEXT("3"), { { 1, FVector(-10000, 0, 100), true }, { 2, FVector(0, 0, 100), true }, { 3, FVector(10000, 0, 100), true } }),
			MakeWay(2, TEXT("residential"), TEXT("3"), { { 3, FVector(10000, 0, 100), true }, { 4, FVector(20000, 0, 100), true } }),
			MakeWay(3, TEXT("primary"), nullptr, { { 2, FVector(0, 0, 100), true }, { 5, FVector(0, 10000, 100), true } }),
			MakeWay(4, TEXT("service"), nullptr, { { 6, FVector(0, 20000, 100), true }, { 7, FVector(10000, 20000, 100), true } }),
			MakeWay(5, TEXT("primary"), TEXT("invalid"), { { 8, FVector(0, 30000, 100), true }, { 9, FVector(10000, 30000, 100), true } })
		};
		if (TestTrue(TEXT("Roads build using per-way styles"), Generator->BuildRoads(Ways, NAME_None)))
		{
			AOsmRoadScene* Scene = Generator->GeneratedScene.Get();
			TestEqual(TEXT("Different ways remain separate even with equal lane counts"), Scene->Roads.Num(), 6);
			TestEqual(TEXT("Scene persistently owns every generated way style"), Scene->ImportedWayStyles.Num(), 5);
			TMap<int64, URoadStyle*> Styles;
			int32 MainSpans = 0;
			for (ARoadActor* Road : Scene->Roads)
			{
				for (int64 Id = 1; Id <= 5; ++Id)
				{
					if (!Road->Tags.Contains(FName(*FString::Printf(TEXT("OSMWay_%lld"), Id)))) continue;
					URoadStyle* Style = Road->LastAppliedRoadStyle;
					if (URoadStyle** Existing = Styles.Find(Id))
						TestTrue(TEXT("Split spans of one way share its style"), *Existing == Style);
					else Styles.Add(Id, Style);
					TestTrue(TEXT("Generated style belongs to the output scene"), Style && Style->GetOuter() == Scene);
					TestTrue(TEXT("Road references the scene's serialized style"), Scene->ImportedWayStyles.FindRef(Id) == Style);
					if (Id == 1) ++MainSpans;
				}
			}
			TestEqual(TEXT("Junction splits the main way"), MainSpans, 2);
			if (TestEqual(TEXT("Every source way has a generated style"), Styles.Num(), 5))
			{
				TestTrue(TEXT("Identical tags on different ways do not share styles"), Styles[1] != Styles[2]);
				TestTrue(TEXT("Untagged and invalid roads also get independent styles"), Styles[3] != Primary && Styles[4] != Default && Styles[5] != Primary && Styles[3] != Styles[5]);
				TestEqual(TEXT("Highway override settings are inherited"), Styles[3]->GeneratedRailingHeight, Primary->GeneratedRailingHeight);
				TestEqual(TEXT("Empty highway override falls back to default settings"), Styles[4]->GeneratedRailingHeight, Default->GeneratedRailingHeight);
				TestEqual(TEXT("Missing lane tag retains highway layout"), Styles[3]->LeftLanes.Num(), Primary->LeftLanes.Num());
				TestEqual(TEXT("Invalid lane tag retains highway layout"), Styles[5]->RightLanes.Num(), Primary->RightLanes.Num());
				const double BaseWidth = Default->LeftLanes[0].Width;
				const double OtherWidth = Styles[2]->LeftLanes[0].Width;
				Styles[1]->LeftLanes[0].Width += 25;
				TestEqual(TEXT("Editing one way does not affect another"), Styles[2]->LeftLanes[0].Width, OtherWidth);
				TestEqual(TEXT("Editing generated styles does not affect the asset"), Default->LeftLanes[0].Width, BaseWidth);
			}
		}
		Generator->ClearRoads();
	}
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoadGeneratorLandscapeRegenerationTest,
	"Landscapes.RoadConstructor.LandscapeRegeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoadGeneratorLandscapeRegenerationTest::RunTest(const FString& Parameters)
{
	const UWorld::InitializationValues Init = UWorld::InitializationValues()
		.AllowAudioPlayback(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Isolated regeneration world"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
	ARoadGenerator* Generator = World->SpawnActor<ARoadGenerator>();
	ALandscape* Landscape = World->SpawnActor<ALandscape>();
	if (!Generator || !Landscape)
	{
		AddError(TEXT("Could not spawn the road generator and Landscape"));
		World->DestroyWorld(false);
		GEngine->DestroyWorldContext(World);
		return false;
	}
	Landscape->SetActorScale3D(FVector(100, 100, 100));
	TArray<uint16> BaseHeights;
	BaseHeights.Init(32768, 64 * 64);
	Landscape->Import(FGuid::NewGuid(), 0, 0, 63, 63, 1, 63,
		TMap<FGuid, TArray<uint16>> { { FGuid(), BaseHeights } }, nullptr,
		TMap<FGuid, TArray<FLandscapeImportLayerInfo>> { { FGuid(), {} } }, ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
	ULandscapeEditLayerBase* SculptLayer = Landscape->GetEditLayer(0);
	SculptLayer->SetName(TEXT("Splines"), true); // Names cannot make a sculpt layer eligible.
	const FGuid SculptGuid = SculptLayer->GetGuid();
	auto ReadHeights = [Landscape](FGuid Layer)
	{
		FLandscapeEditDataInterface Edit(Landscape->GetLandscapeInfo(), Layer);
		TArray<uint16> Heights;
		Heights.Init(32768, 64 * 64);
		Edit.GetHeightDataFast(0, 0, 63, 63, Heights.GetData(), 0);
		return Heights;
	};
	auto SplineHeights = [Landscape, &ReadHeights]()
	{
		const ULandscapeEditLayerBase* Layer = Landscape->FindEditLayerOfTypeConst(ULandscapeEditLayerSplines::StaticClass());
		return Layer ? ReadHeights(Layer->GetGuid()) : TArray<uint16>();
	};
	auto HasDeformation = [&SplineHeights]()
	{
		return SplineHeights().ContainsByPredicate([](uint16 Height) { return Height != 32768; });
	};
	Generator->TargetLandscape = Landscape;
	Generator->SurfaceOffsetCm = 0;
	Generator->LandscapeHeightOffsetCm = 50;
	Generator->LandscapeSplineSideFalloffCm = 175;
	Generator->LandscapeSplineEndFalloffCm = 250;
	URoadStyle* Style = DuplicateObject<URoadStyle>(Generator->DefaultRoadStyle.Get(), Generator);
	FRoadLaneStyle Driving, Shoulder, Sidewalk;
	Driving.Width = 300;
	Shoulder.LaneType = ELaneType::Shoulder;
	Shoulder.Width = 100;
	Sidewalk.LaneType = ELaneType::Sidewalk;
	Sidewalk.Width = 800;
	Style->LeftLanes = { Driving, Shoulder, Sidewalk };
	Driving.Width = 400;
	Style->RightLanes = { Driving, Shoulder, Sidewalk };
	Generator->DefaultRoadStyle = Style;
	FImportedRoadWay Way;
	Way.WayId = 100;
	Way.Tags.Add(TEXT("highway"), TEXT("residential"));
	Way.Tags.Add(TEXT("lanes"), TEXT("3"));
	Way.Points = { { 1, FVector(1000, 3000, 1000), true }, { 2, FVector(5000, 3000, 1000), true } };
	const TArray<FImportedRoadWay> CachedWays = { Way };
	TestNotNull(TEXT("Generate Landscape Splines button"), Generator->FindFunction(TEXT("GenerateLandscapeSplines")));
	TestNotNull(TEXT("Clear Landscape Splines button"), Generator->FindFunction(TEXT("ClearLandscapeSplines")));
	if (TestTrue(TEXT("Generate Roads builds road output alone"), Generator->BuildRoads(CachedWays, NAME_None)))
	{
		TestNull(TEXT("Road generation does not create a spline layer"), Landscape->FindEditLayerOfTypeConst(ULandscapeEditLayerSplines::StaticClass()));
		if (TestTrue(TEXT("Separate spline generation succeeds"), Generator->GenerateLandscapeSplinesInternal()))
		{
			ULandscapeSplinesComponent* Splines = Landscape->GetSplinesComponent();
			const int32 InitialCount = Splines->GetControlPoints().Num();
			TestTrue(TEXT("Native control points were generated"), InitialCount >= 2);
			TestTrue(TEXT("Spline generation deforms the reserved layer"), HasDeformation());
			auto CheckDimensions = [this, Generator, Splines](double Height)
			{
				for (const ULandscapeSplineControlPoint* Point : Splines->GetControlPoints())
				{
					TestEqual(TEXT("Driving and Shoulder lanes set the half-width"), Point->Width, 600.0f);
					TestEqual(TEXT("Side falloff is applied"), Point->SideFalloff, static_cast<float>(Generator->LandscapeSplineSideFalloffCm));
					TestEqual(TEXT("Endpoint falloff is applied"), Point->EndFalloff,
						Point->ConnectedSegments.Num() <= 1 ? static_cast<float>(Generator->LandscapeSplineEndFalloffCm) : 0.0f);
					TestTrue(TEXT("Spline height uses the imported baseline and its independent offset"),
						FMath::IsNearlyEqual(Splines->GetComponentTransform().TransformPosition(Point->Location).Z, Height, 0.1));
				}
			};
			CheckDimensions(1050);
			const auto OriginalPoints = Splines->GetControlPoints();
			const TArray<uint16> OriginalDeformation = SplineHeights();
			TestFalse(TEXT("Spline generation establishes a reusable output baseline"), Generator->CachedSplineOutput.IsEmpty());
			TestTrue(TEXT("Unchanged spline generation succeeds"), Generator->GenerateLandscapeSplinesInternal());
			TestTrue(TEXT("Unchanged spline generation preserves exact controls"), Splines->GetControlPoints() == OriginalPoints);
			TestTrue(TEXT("Unchanged spline generation preserves exact deformation"), SplineHeights() == OriginalDeformation);
			const AOsmRoadScene* UnchangedRoadScene = Generator->GeneratedScene.Get();
			Generator->LandscapeSplineSideFalloffCm = 200;
			TestTrue(TEXT("Terrain settings do not regenerate roads"), Generator->BuildRoads(CachedWays, NAME_None));
			TestTrue(TEXT("Road cache excludes terrain parameters"), Generator->GeneratedScene.Get() == UnchangedRoadScene);
			TestTrue(TEXT("Changed falloff regenerates splines"), Generator->GenerateLandscapeSplinesInternal());
			TestTrue(TEXT("Changed falloff replaces the previous controls"), Splines->GetControlPoints() != OriginalPoints);
			TestTrue(TEXT("Spline changes never regenerate roads"), Generator->GeneratedScene.Get() == UnchangedRoadScene);
			Generator->LandscapeSplineSideFalloffCm = 175;
			TestTrue(TEXT("Restoring falloff updates the output baseline"), Generator->GenerateLandscapeSplinesInternal());
			const auto RetainedPoints = Splines->GetControlPoints();
			// Simulate a level saved before independent prefix ownership was introduced.
			Generator->GeneratedSplinePrefixes.Reset();
			Generator->SurfaceOffsetCm = 12;
			Generator->TargetLandscape = nullptr;
			Generator->LandscapeSplineSideFalloffCm = -1; // Invalid terrain settings must not block roads.
			TestTrue(TEXT("Road regeneration needs no valid terrain settings"), Generator->BuildRoads(CachedWays, NAME_None));
			TestTrue(TEXT("Road regeneration preserves exact spline objects"), Splines->GetControlPoints() == RetainedPoints);
			TestTrue(TEXT("Road regeneration preserves exact deformation"), SplineHeights() == OriginalDeformation);
			Generator->ClearRoads();
			TestFalse(TEXT("Clear Roads removes road output"), Generator->GeneratedScene.IsValid());
			TestTrue(TEXT("Clear Roads preserves exact spline objects"), Splines->GetControlPoints() == RetainedPoints);
			TestTrue(TEXT("Clear Roads preserves exact deformation"), SplineHeights() == OriginalDeformation);
			TestTrue(TEXT("Splines remain independently clearable after roads are deleted"), Generator->ClearLandscapeSplinesInternal());
			TestEqual(TEXT("Clear Landscape Splines removes owned points"), Splines->GetControlPoints().Num(), 0);
			TestFalse(TEXT("Clear Landscape Splines removes deformation"), HasDeformation());

			TestTrue(TEXT("Roads can be regenerated independently"), Generator->BuildRoads(CachedWays, NAME_None));
			AOsmRoadScene* CurrentScene = Generator->GeneratedScene.Get();
			Generator->TargetLandscape = Landscape;
			Generator->LandscapeHeightOffsetCm = -25;
			Generator->LandscapeSplineSideFalloffCm = 0;
			Generator->LandscapeSplineEndFalloffCm = 0;
			Generator->SurfaceOffsetCm = 300; // Mesh was generated at +12, not this new value.
			TestTrue(TEXT("Spline generation uses the saved mesh offset"), Generator->GenerateLandscapeSplinesInternal());
			TestTrue(TEXT("Spline generation preserves the road scene"), Generator->GeneratedScene.Get() == CurrentScene);
			TestTrue(TEXT("Spline generation leaves the road at its generated height"), FMath::IsNearlyEqual(CurrentScene->Roads[0]->GetHeight(0), 1012.0, 0.1));
			CheckDimensions(975);
			Generator->LandscapeHeightOffsetCm = 150;
			TestTrue(TEXT("Only the spline button applies changed terrain settings"), Generator->GenerateLandscapeSplinesInternal());
			TestEqual(TEXT("Spline replacement does not accumulate controls"), Splines->GetControlPoints().Num(), InitialCount);
			CheckDimensions(1150);
			const auto ValidPoints = Splines->GetControlPoints();
			const TArray<uint16> ValidDeformation = SplineHeights();
			Generator->LandscapeSplineEndFalloffCm = -1;
			TestFalse(TEXT("Invalid spline settings reject replacement"), Generator->GenerateLandscapeSplinesInternal());
			TestTrue(TEXT("Failed replacement preserves points"), Splines->GetControlPoints() == ValidPoints);
			TestTrue(TEXT("Failed replacement preserves deformation"), SplineHeights() == ValidDeformation);
			Generator->LandscapeSplineEndFalloffCm = 0;

			ARoadGenerator* Other = World->SpawnActor<ARoadGenerator>();
			if (TestNotNull(TEXT("Second independent generator"), Other))
			{
				Other->TargetLandscape = Landscape;
				FImportedRoadWay OtherWay = Way;
				OtherWay.WayId = 200;
				for (FImportedRoadPoint& Point : OtherWay.Points) Point.WorldLocation.Y = 4500;
				TestTrue(TEXT("Second generator builds roads"), Other->BuildRoads({ OtherWay }, NAME_None));
				TestTrue(TEXT("Second generator builds splines"), Other->GenerateLandscapeSplinesInternal());
				const int32 OtherPointCount = Splines->GetControlPoints().Num() - InitialCount;
				Generator->ClearLandscapeSplines();
				TestTrue(TEXT("Clearing splines preserves road output"), Generator->GeneratedScene.Get() == CurrentScene);
				TestEqual(TEXT("Clearing splines preserves the other generator's points"), Splines->GetControlPoints().Num(), OtherPointCount);
				TestTrue(TEXT("The other generator's deformation remains"), HasDeformation());
				Other->ClearLandscapeSplines();
				Other->ClearRoads();
			}
			TestEqual(TEXT("Clearing all owned splines leaves no controls"), Splines->GetControlPoints().Num(), 0);
			TestFalse(TEXT("Clearing splines removes actual deformation"), HasDeformation());
		}
	}
	TestTrue(TEXT("Original sculpt layer data is preserved"), ReadHeights(SculptGuid) == BaseHeights);
	TestEqual(TEXT("Original sculpt layer name is preserved"), Landscape->GetEditLayerConst(SculptGuid)->GetName(), FName(TEXT("Splines")));
	TestEqual(TEXT("Cached input elevations are unchanged"), CachedWays[0].Points[0].WorldLocation.Z, 1000.0);
	Generator->ClearLandscapeSplines();
	Generator->ClearRoads();
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}
#endif

#endif
