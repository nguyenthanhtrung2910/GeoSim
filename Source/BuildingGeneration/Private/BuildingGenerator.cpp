#include "BuildingGenerator.h"
#include "BakeableBuilding.h"

#include "OsmBuidingImporter.h"
#include "Algo/Reverse.h"
#include "Components/SplineComponent.h"
#include "Engine/World.h"
#include "LCCommon/LCBlueprintLibrary.h"
#include "OSMUserData/OSMUserData.h"
#include "RuleAttributes.h"
#include "RulePackage.h"
#include "VitruvioComponent.h"

DEFINE_LOG_CATEGORY(LogBuildingGenerator);

bool BuildingGeneratorInternal::ResolveHeight(const FImportedBuilding& Building,
                                              const FBuildingHeightSettings& Settings,
                                              FResolvedBuildingHeight& OutHeight)
{
	if (!FMath::IsFinite(Settings.MinFloorHeightMeters) || !FMath::IsFinite(Settings.MaxFloorHeightMeters) ||
		!FMath::IsFinite(Settings.DefaultBuildingHeightMeters) || Settings.MinFloorHeightMeters <= 0 ||
		Settings.MaxFloorHeightMeters < Settings.MinFloorHeightMeters || Settings.DefaultBuildingHeightMeters <= 0) return false;
	const FRandomStream Random(HashCombine(GetTypeHash(Building.OsmId), GetTypeHash(Settings.Seed)));
	OutHeight.FloorHeightMeters = FMath::Lerp(Settings.MinFloorHeightMeters, Settings.MaxFloorHeightMeters, static_cast<double>(Random.FRand()));
	// Match RoadForge: positive height (meters), otherwise levels * floor height, otherwise default.
	double Height = FCString::Atod(*Building.Tags.FindRef(TEXT("height")));
	if (!FMath::IsFinite(Height) || Height <= 0)
	{
		const double Levels = FCString::Atod(*Building.Tags.FindRef(TEXT("building:levels")));
		Height = FMath::IsFinite(Levels) && Levels > 0 ? Levels * OutHeight.FloorHeightMeters : Settings.DefaultBuildingHeightMeters;
	}
	if (!FMath::IsFinite(Height) || Height <= 0 || Height / OutHeight.FloorHeightMeters >= MAX_int32) return false;
	OutHeight.BuildingHeightMeters = Height;
	OutHeight.FloorCount = FMath::Max(1, FMath::FloorToInt(Height / OutHeight.FloorHeightMeters + 1.e-6));
	return true;
}

bool BuildingGeneratorInternal::ApplyNumericRuleAttributes(UVitruvioComponent* Component,
                                                           const TMap<FString, double>& Overrides)
{
	if (!IsValid(Component)) return false;
	TMap<FString, double> ResolvedOverrides;
	for (const auto& [Name, Value] : Overrides)
	{
		FString Key = Name;
		URuleAttribute* Attribute = Component->GetAttributes().FindRef(Key);
		if (!Attribute)
		{
			int32 Matches = 0;
			for (const auto& [CandidateKey, Candidate] : Component->GetAttributes())
			{
				if (Candidate && Candidate->DisplayName == Name)
				{
					Key = CandidateKey;
					Attribute = Candidate;
					++Matches;
				}
			}
			if (Matches > 1)
			{
				UE_LOG(LogBuildingGenerator, Error,
				       TEXT("Rule attribute '%s' is ambiguous. Specify its full style/import key."), *Name);
				return false;
			}
		}
		if (!Cast<UFloatAttribute>(Attribute) || !FMath::IsFinite(Value))
		{
			TArray<FString> Available;
			for (const auto& [CandidateKey, Candidate] : Component->GetAttributes())
				if (Cast<UFloatAttribute>(Candidate)) Available.Add(CandidateKey);
			Available.Sort();
			UE_LOG(LogBuildingGenerator, Error, TEXT("Rule package has no numeric attribute '%s'. Available numeric keys: %s"),
				*Name, *FString::Join(Available, TEXT(", ")));
			return false;
		}
		if (ResolvedOverrides.Contains(Key))
		{
			UE_LOG(LogBuildingGenerator, Error, TEXT("Multiple mappings resolve to rule attribute '%s'. Use distinct attributes."), *Key);
			return false;
		}
		ResolvedOverrides.Add(Key, Value);
	}
	for (const auto& [Name, Value] : ResolvedOverrides) Component->SetFloatAttribute(Name, Value, false, false);
	return true;
}

bool BuildingGeneratorInternal::ApplyHighDetailRuleAttribute(UVitruvioComponent* Component)
{
	if (!IsValid(Component)) return false;
	// The annotation type is reflected but not exported by Vitruvio.
	const UClass* EnumClass = FindObject<UClass>(nullptr, TEXT("/Script/Vitruvio.StringEnumAnnotation"));
	if (!EnumClass) return false;
	bool bApplied = false;
	for (const auto& [Key, Attribute] : Component->GetAttributes())
	{
		if (!Attribute || (Key != TEXT("Level_of_Detail") && Attribute->DisplayName != TEXT("Level_of_Detail"))) continue;
		if (!Cast<UStringAttribute>(Attribute)) continue;
		UAttributeAnnotation* Annotation = Attribute->GetAnnotation();
		if (!Annotation || !Annotation->IsA(EnumClass)) continue;
		for (const FString& Option : static_cast<UStringEnumAnnotation*>(Annotation)->Values)
		{
			if (!Option.Equals(TEXT("high"), ESearchCase::IgnoreCase)) continue;
			Component->SetStringAttribute(Key, Option, false, false);
			bApplied = true;
			break;
		}
	}
	return bApplied;
}

void UBuildingRuleAttributeApplier::Initialize(UVitruvioComponent* Component, const TMap<FString, double>& Values,
	const bool bGenerateModel)
{
	Target = Component;
	Overrides = Values;
	bGenerateAfterApply = bGenerateModel;
	Target->OnAttributesEvaluated.AddDynamic(this, &UBuildingRuleAttributeApplier::OnAttributesReady);
}

void UBuildingRuleAttributeApplier::OnAttributesReady()
{
	if (IsValid(Target))
	{
		Target->OnAttributesEvaluated.RemoveDynamic(this, &UBuildingRuleAttributeApplier::OnAttributesReady);
		if (BuildingGeneratorInternal::ApplyNumericRuleAttributes(Target, Overrides))
		{
			Target->GenerateAutomatically = true;
			Target->EvaluateRuleAttributes(bGenerateAfterApply);
		}
	}
	DestroyComponent();
}

ABuildingGenerator::ABuildingGenerator()
{
	PrimaryActorTick.bCanEverTick = false;
}

void ABuildingGenerator::ClearBuildings()
{
	Modify();
	for (int32 Index = GeneratedActors.Num() - 1; Index >= 0; --Index)
	{
		if (AActor* Building = GeneratedActors[Index].Get(); !IsValid(Building) || Building->Destroy()) GeneratedActors.
			RemoveAt(Index);
	}
}

void ABuildingGenerator::GenerateBuildings()
{
	TSet<FString> Names;
	for (const FString& Name : { FloorHeightAttribute, BuildingHeightAttribute, FloorCountAttribute })
	{
		if (Name.IsEmpty()) continue;
		if (Names.Contains(Name))
		{
			UE_LOG(LogBuildingGenerator, Error, TEXT("Each rule attribute mapping must have a distinct name: '%s'."), *Name);
			return;
		}
		Names.Add(Name);
	}
	TArray<FImportedBuilding> Buildings;
	if (!IsValid(Importer) || !Importer->GetCachedBuildingData(Buildings))
	{
		UE_LOG(LogBuildingGenerator, Error,
		       TEXT("Assign an importer and click Import on it first. Cache is missing or source settings changed."));
		return;
	}
	if (!IsValid(RulePackage) || !FMath::IsFinite(RuleUnitsPerMeter) || RuleUnitsPerMeter <= 0)
	{
		UE_LOG(LogBuildingGenerator, Error, TEXT("Assign a valid rule package and positive rule units."));
		return;
	}
	TArray<FResolvedBuildingHeight> Resolved;
	for (const auto& Building : Buildings)
	{
		FResolvedBuildingHeight Height;
		if (!BuildingGeneratorInternal::ResolveHeight(Building, Heights, Height))
		{
			UE_LOG(LogBuildingGenerator, Error,
			       TEXT("Invalid floor-height range, fallback height, or OSM height for %s."), *Building.OsmId);
			return;
		}
		Resolved.Add(Height);
	}
	if (bDeleteOldBuildingsWhenGenerating)
	{
		ClearBuildings();
		if (!GeneratedActors.IsEmpty()) return;
	}
	Modify();
	for (int32 Index = 0; Index < Buildings.Num(); ++Index)
		if (!GenerateBuilding(Buildings[Index], Resolved[Index])) return;
}

bool ABuildingGenerator::GenerateBuilding(const FImportedBuilding& Source, const FResolvedBuildingHeight& Height)
{
	if (Source.Points.Num() < 3) return false;
	FVector Origin = Source.Points[0];
	if (bFlattenFootprints)
		for (const FVector& Point : Source.Points) Origin.Z = FMath::Max(Origin.Z, Point.Z);
	TArray<FSplinePoint> Points;
	double Area = 0;
	for (int32 Index = 0; Index < Source.Points.Num(); ++Index)
	{
		FVector Position = Source.Points[Index] - Origin;
		if (bFlattenFootprints) Position.Z = 0;
		Points.Emplace(static_cast<float>(Index), Position, ESplinePointType::Linear);
		const FVector A = Source.Points[Index] - Origin;
		const FVector B = Source.Points[(Index + 1) % Source.Points.Num()] - Origin;
		Area += A.X * B.Y - B.X * A.Y;
	}
	if (FMath::IsNearlyZero(Area, 1.0)) return true;
	// Vitruvio expects clockwise initial shapes.
	if (Area > 0) Algo::Reverse(Points);
	for (int32 Index = 0; Index < Points.Num(); ++Index) Points[Index].InputKey = Index;
	ABakeableBuilding* Building = GetWorld()->SpawnActor<ABakeableBuilding>(Origin, FRotator::ZeroRotator);
	if (!Building) return false;
	Building->SetFlags(RF_Transactional);
	TMap<FString, double> Overrides;
	if (!FloorHeightAttribute.IsEmpty()) Overrides.Add(FloorHeightAttribute, Height.FloorHeightMeters * RuleUnitsPerMeter);
	if (!BuildingHeightAttribute.IsEmpty()) Overrides.Add(BuildingHeightAttribute, Height.BuildingHeightMeters * RuleUnitsPerMeter);
	if (!FloorCountAttribute.IsEmpty()) Overrides.Add(FloorCountAttribute, Height.FloorCount);
	if (!Building->InitializeBuilding(RulePackage, Points, Overrides, bHighDetail))
	{
		Building->Destroy();
		return false;
	}
	GeneratedActors.Add(Building);
	UOSMUserData* Metadata = NewObject<UOSMUserData>(Building->GetRootComponent());
	Metadata->Fields = Source.Tags;
	Building->GetRootComponent()->AddAssetUserData(Metadata);
#if WITH_EDITOR
	Building->SetActorLabel(TEXT("Building_") + Source.OsmId);
	ULCBlueprintLibrary::SetFolderPath2(Building, NAME_None, SpawnedActorsPath);
#endif
	return true;
}

