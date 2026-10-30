#include "RoadGenerationSignatures.h"
#include "RoadGenerationSignaturesInternal.h"

#if WITH_EDITOR
#include "RoadGenerator.h"
#include "OsmRoadScene.h"
#include "ImportedRoadData.h"
#include "Engine/DataAsset.h"
#include "Landscape.h"
#include "LandscapeEditLayer.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"
#include "UObject/UnrealType.h"

namespace RoadGenerationSignatures
{
	FSignature::FSignature(bool bInGeometryOnly) : bGeometryOnly(bInGeometryOnly) {}

	void FSignature::Text(const FString& Value)
	{
		const FTCHARToUTF8 Bytes(*Value);
		const int32 Length = Bytes.Length();
		Number(Length);
		Hash.Update(reinterpret_cast<const uint8*>(Bytes.Get()), Length);
	}

	void FSignature::Vector(const FVector& Value) { Number(Value.X); Number(Value.Y); Number(Value.Z); }

	void FSignature::Transform(const FTransform& Value)
	{
		Vector(Value.GetLocation());
		Vector(Value.GetScale3D());
		const FQuat Rotation = Value.GetRotation();
		Number(Rotation.X); Number(Rotation.Y); Number(Rotation.Z); Number(Rotation.W);
	}

	void FSignature::Property(const UObject* Owner, const FProperty* Field)
	{
		if (!Owner || !Field) return;
		Text(Field->GetName());
		if (bGeometryOnly)
		{
			GeometryValue(Field, Field->ContainerPtrToValuePtr<void>(Owner));
			return;
		}
		FString Export;
		Field->ExportText_InContainer(0, Export, Owner, nullptr, const_cast<UObject*>(Owner), PPF_None);
		Text(Export);
		Dependencies(Field, Field->ContainerPtrToValuePtr<void>(Owner));
	}

	void FSignature::Named(const UObject* Owner, std::initializer_list<const TCHAR*> Names)
	{
		if (!Owner) return;
		for (const TCHAR* Name : Names) Property(Owner, FindFProperty<FProperty>(Owner->GetClass(), Name));
	}

	void FSignature::Object(const UObject* Value)
	{
		Text(GetPathNameSafe(Value));
		if (!IsValid(Value) || Visited.Contains(Value)) return;
		Visited.Add(Value);
		for (TFieldIterator<FProperty> It(Value->GetClass()); It; ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_Edit) || It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) ||
				It->GetFName() == TEXT("bLiveRoadStyleUpdate")) continue;
			Property(Value, *It);
		}
	}

	FString FSignature::Finish()
	{
		uint8 Digest[16];
		Hash.Final(Digest);
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	}

	void FSignature::GeometryValue(const FProperty* Field, const void* Value)
	{
		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Field);
			Object && Object->PropertyClass->IsChildOf(URoadProps::StaticClass())) return;
		if (const FStructProperty* Struct = CastField<FStructProperty>(Field))
		{
			for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
				Text(It->GetName());
				GeometryValue(*It, It->ContainerPtrToValuePtr<void>(Value));
			}
			return;
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(Field))
		{
			FScriptArrayHelper Helper(Array, Value);
			Number(Helper.Num());
			for (int32 Index = 0; Index < Helper.Num(); ++Index) GeometryValue(Array->Inner, Helper.GetRawPtr(Index));
			return;
		}
		FString Export;
		Field->ExportTextItem_Direct(Export, Value, nullptr, nullptr, PPF_None);
		Text(Export);
		Dependencies(Field, Value);
	}

	void FSignature::Dependencies(const FProperty* Field, const void* Value)
	{
		if (const FObjectPropertyBase* ObjectField = CastField<FObjectPropertyBase>(Field))
		{
			const UObject* ObjectValue = ObjectField->GetObjectPropertyValue(Value);
			if (!IsValid(ObjectValue) || ObjectValue->IsA<AActor>() || ObjectValue->IsA<UActorComponent>()) return;
			if (const FString Package = ObjectValue->GetClass()->GetOutermost()->GetName(); Package ==
				TEXT("/Script/RoadConstructor") || Package == TEXT("/Script/PhysicsCore") || ObjectValue->IsA<
					UDataAsset>()) Object(ObjectValue);
		}
		else if (const FStructProperty* Struct = CastField<FStructProperty>(Field))
		{
			for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
				if (!It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) Dependencies(*It, It->ContainerPtrToValuePtr<void>(Value));
		}
		else if (const FArrayProperty* Array = CastField<FArrayProperty>(Field))
		{
			FScriptArrayHelper Helper(Array, Value);
			for (int32 Index = 0; Index < Helper.Num(); ++Index) Dependencies(Array->Inner, Helper.GetRawPtr(Index));
		}
		else if (const FMapProperty* Map = CastField<FMapProperty>(Field))
		{
			FScriptMapHelper Helper(Map, Value);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
				if (Helper.IsValidIndex(Index))
				{
					Dependencies(Map->KeyProp, Helper.GetKeyPtr(Index));
					Dependencies(Map->ValueProp, Helper.GetValuePtr(Index));
				}
		}
		else if (const FSetProperty* Set = CastField<FSetProperty>(Field))
		{
			FScriptSetHelper Helper(Set, Value);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
				if (Helper.IsValidIndex(Index)) Dependencies(Set->ElementProp, Helper.GetElementPtr(Index));
		}
	}

	FString InputSignature(const ARoadGenerator& Generator, const TArray<FImportedRoadWay>& Ways, const FName /*OutputFolder*/, bool bGeometryOnly)
	{
		FSignature Hash(bGeometryOnly);
		Hash.Named(&Generator, { TEXT("SurfaceOffsetCm"), TEXT("RoadCurveRadiusCm"), TEXT("RoadTransitionLengthCm"), TEXT("JunctionMergeDistanceCm"),
			TEXT("bRequireTerrainSamples"), TEXT("RoadSceneSizeCm") });
		if (!bGeometryOnly)
		{
			Hash.Named(&Generator, { TEXT("bOverrideJunctionRadius"), TEXT("bCleanJunctionApproaches"), TEXT("bRemoveCornersWithoutJunctionSurface") });
			if (Generator.bOverrideJunctionRadius) Hash.Named(&Generator, { TEXT("JunctionRadiusCm") });
		}
		// Outliner folder routing is applied separately and never invalidates geometry.
		Hash.Text(Generator.GetActorLabel());
		TArray<const FImportedRoadWay*> Selected;
		for (const FImportedRoadWay& Way : Ways)
		{
			if (const FString* Highway = Way.Tags.Find(TEXT("highway")); Highway && !Highway->IsEmpty() && (Generator.
				HighwayFilter.IsEmpty() || Generator.HighwayFilter.Contains(FName(**Highway)))) Selected.Add(&Way);
		}
		Selected.Sort([](const FImportedRoadWay& A, const FImportedRoadWay& B) { return A.WayId < B.WayId; });
		Hash.Number(Selected.Num());
		for (const FImportedRoadWay* Way : Selected)
		{
			Hash.Number(Way->WayId);
			for (const TCHAR* Tag : { TEXT("highway"), TEXT("lanes"), TEXT("layer") }) Hash.Text(Way->Tags.FindRef(Tag));
			Hash.Number(Way->Points.Num());
			for (const auto& [NodeId, WorldLocation, bHasTerrain] : Way->Points)
			{
				Hash.Number(NodeId);
				Hash.Vector(WorldLocation);
				if (Generator.bRequireTerrainSamples) Hash.Number(bHasTerrain);
			}
			const TObjectPtr<URoadStyle>* Override = Generator.HighwayStyles.Find(FName(*Way->Tags.FindChecked(TEXT("highway"))));
			// The selected asset is a Details parameter; its internal props/profile
			// contents do not participate in importer/generator change detection.
			Hash.Text(GetPathNameSafe(Override && IsValid(Override->Get()) ? Override->Get() : Generator.DefaultRoadStyle.Get()));
		}
		return Hash.Finish();
	}

	FString RoadInputs(const ARoadGenerator& Generator, const TArray<FImportedRoadWay>& Ways, FName OutputFolder)
	{
		return InputSignature(Generator, Ways, OutputFolder, false);
	}

	FString RoadGeometryInputs(const ARoadGenerator& Generator, const TArray<FImportedRoadWay>& Ways, FName OutputFolder)
	{
		return InputSignature(Generator, Ways, OutputFolder, true);
	}

	FString JunctionInputs(const ARoadGenerator& Generator, const FString& CurveInputs)
	{
		FSignature Hash(true);
		Hash.Text(CurveInputs);
		Hash.Named(&Generator, { TEXT("bOverrideJunctionRadius"), TEXT("bCleanJunctionApproaches") });
		if (Generator.bOverrideJunctionRadius) Hash.Named(&Generator, { TEXT("JunctionRadiusCm") });
		return Hash.Finish();
	}

	FString LandscapeInputs(const ARoadGenerator& Generator, const AOsmRoadScene& Scene)
	{
		FSignature Hash;
		Hash.Named(&Generator, {
			           TEXT("LandscapeHeightOffsetCm"),
			           TEXT("LandscapeSplineEndFalloffCm"), TEXT("LandscapeSplineStartFalloffCm"),
			           TEXT("LandscapeSplineStartExtensionCm"), TEXT("LandscapeSplineEndExtensionCm"),
			           TEXT("LandscapeSplineWidthScale"), TEXT("LandscapeSplineFalloffScale"),
			           TEXT("TerrainDeformTargetLandscapeSplineEditLayer")
		           });
		Hash.Text(Scene.GetPathName());
		Hash.Number(Scene.ImportedSurfaceOffsetCm);
		Hash.Text(GetPathNameSafe(Generator.TargetLandscape));
		Hash.Transform(Generator.TargetLandscape->GetActorTransform());
		if (const auto* Layer = Generator.TargetLandscape->FindEditLayerOfTypeConst(ULandscapeEditLayerSplines::StaticClass()))
		{
			Hash.Text(Layer->GetGuid().ToString());
			Hash.Text(Layer->GetName().ToString());
		}
		for (const ARoadActor* Road : Scene.Roads)
		{
			Hash.Text(Road->GetPathName());
			Hash.Transform(Road->GetActorTransform());
			Hash.Named(Road, { TEXT("RoadPoints"), TEXT("HeightPoints") });
			for (TFieldIterator<FProperty> It(Road->GetClass()); It; ++It)
				if (It->GetName().StartsWith(TEXT("TerrainDeform")) && !It->HasAnyPropertyFlags(CPF_Transient)) Hash.
					Property(Road, *It);
			double Width = 0.0;
			if (const URoadStyle* Style = Road->LastAppliedRoadStyle)
				for (const TArray<FRoadLaneStyle>* Lanes : { &Style->LeftLanes, &Style->RightLanes })
					for (const FRoadLaneStyle& Lane : *Lanes)
						if (FMath::IsFinite(Lane.Width)) Width += FMath::Max(0.0, Lane.Width);
			Hash.Number(Width);
		}
		return Hash.Finish();
	}

	FString LandscapeOutput(const ALandscape* Landscape, const TSet<FString>& Prefixes)
	{
		if (!IsValid(Landscape) || Prefixes.IsEmpty() || !Landscape->GetSplinesComponent() ||
			!Landscape->FindEditLayerOfTypeConst(ULandscapeEditLayerSplines::StaticClass())) return FString();
		auto IsOwned = [&Prefixes](const UObject* Object)
		{
			if (!IsValid(Object)) return false;
			for (const FString& Prefix : Prefixes) if (Object->GetName().StartsWith(Prefix)) return true;
			return false;
		};
		TArray<UObject*> Objects;
		int32 Points = 0, Segments = 0;
		for (ULandscapeSplineControlPoint* Point : Landscape->GetSplinesComponent()->GetControlPoints())
			if (IsOwned(Point)) { Objects.Add(Point); ++Points; }
		for (ULandscapeSplineSegment* Segment : Landscape->GetSplinesComponent()->GetSegments())
			if (IsOwned(Segment)) { Objects.Add(Segment); ++Segments; }
		if (!Points || !Segments) return FString();
		Objects.Sort([](const UObject& A, const UObject& B) { return A.GetPathName() < B.GetPathName(); });
		FSignature Hash;
		Hash.Number(Points); Hash.Number(Segments);
		for (const UObject* Object : Objects)
		{
			Hash.Object(Object);
			Hash.Named(Object, { TEXT("ConnectedSegments"), TEXT("Connections") });
		}
		return Hash.Finish();
	}
}
#endif
