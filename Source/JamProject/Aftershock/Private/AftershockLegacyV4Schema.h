#pragma once
#include "CoreMinimal.h"
#include "Hash/Blake3.h"
#include "Serialization/CustomVersion.h"
#include "UObject/ObjectVersion.h"
#include "Misc/EngineVersion.h"
class UClass;class UStruct;class FProperty;class FJsonObject;
namespace AftershockLegacyV4
{
    // Generated against the accepted immutable original V4 native descriptor.
    const TCHAR* DescriptorSha256();
    const TCHAR* SchemaFingerprintHex();
    FString EmbeddedDescriptorJSON();
    void RegisterGeneratedTypes(TMap<FString,UStruct*>&,TMap<const UStruct*,FString>&);
    UClass* FrozenRootClass();
    bool VerifyFrozenReflection(FString& Error);
    bool VerifyStoredWireMetadata(const FBlake3Hash& Stored,const FPackageFileVersion& Package,
        const FEngineVersion& Engine,const FCustomVersionContainer& Versions,FString& Error);
    bool IsFrozenProjectStruct(const UStruct* Type);
    FString GetOriginalTypeID(const UStruct* Type);
    const FJsonObject* FindOriginalProperty(const FProperty* Property);
    const FJsonObject* FindOriginalEnum(const FString& ID);
}
