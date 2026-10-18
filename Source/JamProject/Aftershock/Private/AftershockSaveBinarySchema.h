#pragma once
#include "CoreMinimal.h"
#include "Hash/Blake3.h"
#include "Misc/EngineVersion.h"
#include "Serialization/CustomVersion.h"
#include "UObject/ObjectVersion.h"

class UAftershockSaveGame;
class UClass;

namespace AftershockSaveBinarySchema
{
	constexpr uint32 PayloadVersion=2;
	/** Bump when custom/native wire serialization changes without a reflected schema change. */
	constexpr uint32 NativeCodecVersion=1;
	struct FSchema
	{
		FBlake3Hash Fingerprint;
		int32 PropertyCount=0,StructCount=0;
		FString FailureReason,FailedField,FailedFieldClass;
	};
	/** Runtime reflection only; no editor-only GetSchemaHash, addresses, offsets or sizeof layouts. */
	bool Build(const UClass* Root,FSchema& Out);
	/** Bind the writer's explicit native-version metadata, independent of local plugin registration order. */
	bool BindMetadata(const FSchema& Schema,const FPackageFileVersion& Package,const FEngineVersion& Engine,
		const FCustomVersionContainer& Versions,FBlake3Hash& Out);
	/** Opt-in native compatibility fixtures only. Production writes PayloadVersion2. */
	bool EncodeLegacyTaggedFixture(UAftershockSaveGame& Saved,TArray<uint8>& Payload);
	uint64 GetBinaryDeserializeCount();
	void RecordBinaryDeserialize();
}
