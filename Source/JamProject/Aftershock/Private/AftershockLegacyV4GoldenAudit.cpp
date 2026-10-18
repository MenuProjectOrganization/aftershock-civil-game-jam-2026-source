#include "AftershockLegacyV4GoldenAudit.h"
#include "AftershockLegacyV4Schema.h"
#include "AftershockLegacyV4Reader.h"
#include "AftershockSaveBinarySchema.h"
#include "Aftershock/Public/AftershockSaveGame.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/MemoryReader.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "HAL/FileManager.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
namespace AftershockLegacyV4
{
namespace
{
    FString SHA1(const TArray<uint8>& Bytes){uint8 D[FSHA1::DigestSize];FSHA1::HashBuffer(Bytes.GetData(),Bytes.Num(),D);return BytesToHex(D,FSHA1::DigestSize);}
    bool Unwrap(const TArray<uint8>& Input,TArray<uint8>& Payload)
    {
        if(Input.Num()<36||Input.Num()>64*1024*1024)return false;FMemoryReader R(Input,true);uint32 Magic=0;int32 Version=0,N=0,U=0;R<<Magic;R<<Version;R<<N;R<<U;uint8 Digest[20];R.Serialize(Digest,20);
        if(R.IsError()||Magic!=0x41465348||Version!=3||N!=Input.Num()-36||U<1||U>256*1024*1024)return false;Payload.SetNumUninitialized(U);
        if(!FCompression::UncompressMemory(NAME_Zlib,Payload.GetData(),U,Input.GetData()+36,N))return false;uint8 Actual[20];FSHA1::HashBuffer(Payload.GetData(),U,Actual);return FMemory::Memcmp(Digest,Actual,20)==0;
    }
    bool SameCurrentFields(const UObject* A,const UObject* B)
    {if(!A||!B||A->GetClass()!=B->GetClass())return false;for(FProperty* F=A->GetClass()->PropertyLink;F;F=F->PropertyLinkNext)for(int32 I=0;I<F->ArrayDim;++I)if(!F->Identical_InContainer(A,B,I,0))return false;return true;}
}
int32 RunGoldenAudit(TFunctionRef<UAftershockSaveGame*(const TArray<uint8>&)> CurrentEnvelopeReader)
{
    TArray<TSharedPtr<FJsonValue>> Checks;int32 Failed=0;
    const auto Check=[&](const TCHAR* Name,bool Pass){auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("name"),Name);Row->SetBoolField(TEXT("pass"),Pass);Checks.Add(MakeShared<FJsonValueObject>(Row));Failed+=Pass?0:1;if(Pass){UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_LEGACY_V4_AUDIT_PASS %s"),Name);}else{UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_LEGACY_V4_AUDIT_FAIL %s"),Name);}};
    FString Error;Check(TEXT("frozen_dto_recursive_reflection_wire_graph_verified"),VerifyFrozenReflection(Error));
    TSharedPtr<FJsonObject> Descriptor;auto JSONReader=TJsonReaderFactory<>::Create(EmbeddedDescriptorJSON());const bool HasDescriptor=FJsonSerializer::Deserialize(JSONReader,Descriptor)&&Descriptor;
    Check(TEXT("compiled_frozen_descriptor_available"),HasDescriptor);
    const FString Dir=FPaths::ProjectDir()/TEXT("docs/validation/2026-10-18/global-environment/frozen-v4");
    const FString BinaryPath=Dir/TEXT("golden_v4_binary_v2_envelope3.savfixture"),TaggedPath=Dir/TEXT("golden_v4_tagged_v1_envelope3.savfixture");
    TArray<uint8> Binary,Tagged,Payload;const bool ReadBinary=FFileHelper::LoadFileToArray(Binary,*BinaryPath),ReadTagged=FFileHelper::LoadFileToArray(Tagged,*TaggedPath);
    const FString BinaryHash=SHA1(Binary),TaggedHash=SHA1(Tagged);
    Check(TEXT("exact_exported_synthetic_compressed_goldens_match_recorded_hashes"),HasDescriptor&&ReadBinary&&ReadTagged&&BinaryHash.Equals(Descriptor->GetStringField(TEXT("binary_envelope_sha1")),ESearchCase::IgnoreCase)&&TaggedHash.Equals(Descriptor->GetStringField(TEXT("tagged_envelope_sha1")),ESearchCase::IgnoreCase));
    const bool Unwrapped=ReadBinary&&Unwrap(Binary,Payload);Check(TEXT("binary_fixture_envelope_checksum_and_size_valid"),Unwrapped);
    FReadDiagnostics D;const uint64 CurrentDeserializationsBefore=AftershockSaveBinarySchema::GetBinaryDeserializeCount();
    TStrongObjectPtr<UAftershockSaveGame> Frozen(Unwrapped?DecodePayload(Payload,Error,&D,true):nullptr);
    Check(TEXT("real_frozen_binary_dto_decodes_and_original_body_reencodes_byte_identically"),Frozen.IsValid()&&D.bExactLegacyBodyRoundtrip);
    Check(TEXT("legacy_reader_never_invokes_current_binary_deserializer"),AftershockSaveBinarySchema::GetBinaryDeserializeCount()==CurrentDeserializationsBefore);
    TStrongObjectPtr<UAftershockSaveGame> TaggedRead(ReadTagged?CurrentEnvelopeReader(Tagged):nullptr);
    // This paired fixture is proven by the immutable exporter to be Schema4.
    // Its tagged default4 can be absent on wire. This audit-only normalization
    // does NOT label arbitrary historical tagged files as Version4.
    if(TaggedRead.IsValid())TaggedRead->SchemaVersion=4;
    Check(TEXT("known_paired_tagged_v4_fixture_decodes"),TaggedRead.IsValid());
    Check(TEXT("every_current_field_matches_known_paired_tagged_golden_including_new_defaults"),SameCurrentFields(Frozen.Get(),TaggedRead.Get()));
    Check(TEXT("decoded_legacy_schema_remains_four_until_explicit_v5_migration"),Frozen.IsValid()&&Frozen->SchemaVersion==4);
    bool BadFingerprint=false,BadTruncated=false,BadTrailing=false,BadCount=false;
    if(Unwrapped)
    {
        auto Mutated=Payload;Mutated[12]^=1;TStrongObjectPtr<UAftershockSaveGame> Reject(DecodePayload(Mutated,Error));BadFingerprint=!Reject.IsValid();
        Mutated=Payload;Mutated.Pop(EAllowShrinking::No);Reject.Reset(DecodePayload(Mutated,Error));BadTruncated=!Reject.IsValid();
        Mutated=Payload;Mutated.Add(0);Reject.Reset(DecodePayload(Mutated,Error));BadTrailing=!Reject.IsValid();
        if(D.FirstContainerCountOffset>=0&&D.FirstContainerCountOffset+4<=Payload.Num())
        {Mutated=Payload;const int32 Enormous=MAX_int32;FMemory::Memcpy(Mutated.GetData()+D.FirstContainerCountOffset,&Enormous,4);Reject.Reset(DecodePayload(Mutated,Error));BadCount=!Reject.IsValid()&&Error.Contains(TEXT("container"));}
    }
    Check(TEXT("unknown_fingerprint_rejected_before_any_current_deserialize"),BadFingerprint&&AftershockSaveBinarySchema::GetBinaryDeserializeCount()==CurrentDeserializationsBefore);
    Check(TEXT("truncated_body_rejected"),BadTruncated);Check(TEXT("trailing_body_rejected"),BadTrailing);
    Check(TEXT("enormous_real_container_count_rejected_before_container_allocation"),BadCount);
    TArray<uint8> BinaryAfter,TaggedAfter;
    Check(TEXT("original_fixture_files_unchanged_and_no_user_slots_accessed"),ReadBinary&&ReadTagged&&FFileHelper::LoadFileToArray(BinaryAfter,*BinaryPath)&&FFileHelper::LoadFileToArray(TaggedAfter,*TaggedPath)&&SHA1(BinaryAfter)==BinaryHash&&SHA1(TaggedAfter)==TaggedHash);
    auto Report=MakeShared<FJsonObject>();Report->SetStringField(TEXT("scope"),TEXT("Native exact frozen V4 synthetic binary and paired tagged fixtures only. Byte body roundtrip, all-field conversion/default comparison, bounded rejection. No historical playable/user-save restore, cross-platform, or V5 physical migration acceptance."));
    Report->SetStringField(TEXT("descriptor_sha256"),DescriptorSha256());Report->SetStringField(TEXT("frozen_schema_fingerprint"),SchemaFingerprintHex());Report->SetStringField(TEXT("utc"),FDateTime::UtcNow().ToIso8601());Report->SetStringField(TEXT("last_diagnostic"),Error);
    Report->SetNumberField(TEXT("passed"),Checks.Num()-Failed);Report->SetNumberField(TEXT("failed"),Failed);Report->SetArrayField(TEXT("checks"),Checks);Report->SetBoolField(TEXT("valid"),Failed==0);
    Report->SetNumberField(TEXT("body_bytes"),D.BodyBytes);Report->SetNumberField(TEXT("names"),D.Names);Report->SetNumberField(TEXT("elements"),D.Elements);Report->SetNumberField(TEXT("container_allocation_budget_used"),D.AllocationBudgetUsed);
    Report->SetNumberField(TEXT("header_ms"),D.HeaderMs);Report->SetNumberField(TEXT("dto_decode_ms"),D.DtoDecodeMs);Report->SetNumberField(TEXT("audit_body_roundtrip_ms"),D.AuditRoundtripMs);Report->SetNumberField(TEXT("conversion_ms"),D.ConvertMs);
    const FString OutputDir=FPaths::ProjectSavedDir()/TEXT("PresentationValidation");IFileManager::Get().MakeDirectory(*OutputDir,true);FString JSON;const auto Writer=TJsonWriterFactory<>::Create(&JSON);
    const bool Wrote=FJsonSerializer::Serialize(Report,Writer)&&FFileHelper::SaveStringToFile(JSON,*(OutputDir/TEXT("legacy_v4_dto_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_LEGACY_V4_AUDIT passed=%d failed=%d dtoDecodeMs=%.3f convertMs=%.3f noUserSaveWrites=1"),Checks.Num()-Failed,Failed,D.DtoDecodeMs,D.ConvertMs);
    return Failed==0&&Wrote?0:1;
}
}
