#pragma once

#include "CoreMinimal.h"
#if WITH_EDITOR
#include "Misc/SecureHash.h"

class ARoadGenerator;
class FProperty;
struct FImportedRoadWay;

namespace RoadGenerationSignatures
{
	FString InputSignature(const ARoadGenerator& Generator, const TArray<FImportedRoadWay>& Ways,
		FName OutputFolder, bool bGeometryOnly);

	class FSignature
	{
	public:
		explicit FSignature(bool bInGeometryOnly = false);
		void Text(const FString& Value);
		template<typename T> void Number(const T& Value)
		{
			Hash.Update(reinterpret_cast<const uint8*>(&Value), sizeof(Value));
		}
		void Vector(const FVector& Value);
		void Transform(const FTransform& Value);
		void Property(const UObject* Owner, const FProperty* Field);
		void Named(const UObject* Owner, std::initializer_list<const TCHAR*> Names);
		void Object(const UObject* Value);
		FString Finish();

	private:
		// Export structs field by field so lane Props references are excluded too.
		void GeometryValue(const FProperty* Field, const void* Value);
		// Include authored dependency contents when hashing spline output properties.
		void Dependencies(const FProperty* Field, const void* Value);
		FMD5 Hash;
		bool bGeometryOnly;
		TSet<const UObject*> Visited;
	};
}
#endif
