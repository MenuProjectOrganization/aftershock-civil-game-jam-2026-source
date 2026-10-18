#include "AftershockSaveCodecAuditCommandlet.h"

#if WITH_EDITOR
#include "AftershockSaveBinarySchema.h"
#include "AftershockLegacyV4GoldenAudit.h"
#include "Aftershock/Public/AftershockSaveGame.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Parse.h"
#include "Misc/EngineVersion.h"
#include "Serialization/CustomVersion.h"
#include "UObject/ObjectVersion.h"

namespace
{
	struct FCodecCheck{FString Name;bool bPass=false;};
	struct FCodecHeaderLayout{int32 PackageAt=INDEX_NONE,EngineAt=INDEX_NONE,CustomRowsAt=INDEX_NONE,CustomCount=0;};
	bool ReadCodecHeaderLayout(const TArray<uint8>& Bytes,FCodecHeaderLayout& Out)
	{
		FMemoryReader Reader(Bytes,true);uint32 Magic=0,Version=0,Root=0;FBlake3Hash Fingerprint;
		Reader<<Magic;Reader<<Version;Reader<<Root;Reader<<Fingerprint;
		if(Reader.IsError()||Version!=2)return false;
		Out.PackageAt=Reader.Tell();FPackageFileVersion Package;Reader<<Package;
		Out.EngineAt=Reader.Tell();uint16 Major=0,Minor=0,Patch=0;uint32 Changelist=0;Reader<<Major;Reader<<Minor;Reader<<Patch;Reader<<Changelist;
		int32 BranchBytes=0;Reader<<BranchBytes;if(Reader.IsError()||BranchBytes<0||BranchBytes>1024||BranchBytes>Reader.TotalSize()-Reader.Tell())return false;
		Reader.Seek(Reader.Tell()+BranchBytes);Reader<<Out.CustomCount;Out.CustomRowsAt=Reader.Tell();
		return !Reader.IsError()&&Out.CustomCount>0&&Out.CustomCount<=1024&&static_cast<int64>(Out.CustomCount)*20<=Reader.TotalSize()-Reader.Tell();
	}
	bool SameSaveFields(const UAftershockSaveGame* A,const UAftershockSaveGame* B)
	{
		if(!A||!B||A->GetClass()!=B->GetClass())return false;
		// UClass has no CompareScriptStruct API. Compare each reflected field,
		// including inherited properties and fixed-size array elements.
		for(TFieldIterator<FProperty> Field(A->GetClass());Field;++Field)
			for(int32 Element=0;Element<Field->ArrayDim;++Element)
				if(!Field->Identical_InContainer(A,B,Element,0))return false;
		return true;
	}
	bool WrapCodecEnvelope(const TArray<uint8>& Payload,int32 Version,TArray<uint8>& Out)
	{
		Out.Reset();if(Payload.IsEmpty()||Version<1||Version>3)return false;
		TArray<uint8> Stored;int32 Count=Payload.Num();
		if(Version==1)Stored=Payload;
		else
		{
			Count=FCompression::CompressMemoryBound(NAME_Zlib,Payload.Num());Stored.SetNumUninitialized(Count);
			if(!FCompression::CompressMemory(NAME_Zlib,Stored.GetData(),Count,Payload.GetData(),Payload.Num(),COMPRESS_BiasSpeed))return false;
			Stored.SetNum(Count,EAllowShrinking::No);
		}
		FMemoryWriter Writer(Out,true);uint32 Magic=0x41465348;int32 Uncompressed=Payload.Num();uint8 Digest[FSHA1::DigestSize];FSHA1::HashBuffer(Payload.GetData(),Payload.Num(),Digest);
		Writer<<Magic;Writer<<Version;Writer<<Count;if(Version>=2)Writer<<Uncompressed;Writer.Serialize(Digest,FSHA1::DigestSize);Writer.Serialize(Stored.GetData(),Stored.Num());
		return !Writer.IsError();
	}
    struct FFrozenV4Descriptor
    {
        TSet<const UStruct*> SeenStructs;
        TSet<const UEnum*> SeenEnums;
        TArray<TSharedPtr<FJsonValue>> Structs,Enums;
        int32 PropertyVisits=0;
        bool bValid=true;

        void AddEnum(const UEnum* Type)
        {
            if(!Type||SeenEnums.Contains(Type))return;
            if(Type->NumEnums()>65536){bValid=false;return;}
            SeenEnums.Add(Type);auto Row=MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("type_id"),Type->GetPathName());
            TArray<TSharedPtr<FJsonValue>> Values;
            for(int32 I=0;I<Type->NumEnums();++I)
            {
                auto Value=MakeShared<FJsonObject>();Value->SetNumberField(TEXT("declared_index"),I);
                Value->SetStringField(TEXT("name"),Type->GetNameByIndex(I).ToString());
                // String preserves all64bits, including a signed enum sentinel.
                Value->SetStringField(TEXT("numeric_value"),LexToString(Type->GetValueByIndex(I)));
                Values.Add(MakeShared<FJsonValueObject>(Value));
            }
            Row->SetArrayField(TEXT("entries"),Values);Enums.Add(MakeShared<FJsonValueObject>(Row));
        }
        TSharedRef<FJsonObject> Property(const FProperty* Field,int32 Depth)
        {
            auto Row=MakeShared<FJsonObject>();
            if(!Field||Depth>64||++PropertyVisits>16384){bValid=false;return Row;}
            Row->SetStringField(TEXT("name"),Field->GetName());
            Row->SetStringField(TEXT("path"),Field->GetPathName());
            Row->SetStringField(TEXT("property_class"),Field->GetClass()->GetName());
            Row->SetNumberField(TEXT("array_dim"),Field->ArrayDim);
            Row->SetStringField(TEXT("flags_hex"),FString::Printf(TEXT("%016llx"),static_cast<unsigned long long>(Field->GetPropertyFlags())));
            FString Extended;Row->SetStringField(TEXT("cpp_type"),Field->GetCPPType(&Extended));Row->SetStringField(TEXT("cpp_extended"),Extended);
            if(const auto* Array=CastField<FArrayProperty>(Field))Row->SetObjectField(TEXT("inner"),Property(Array->Inner,Depth+1));
            else if(const auto* Map=CastField<FMapProperty>(Field))
            {Row->SetObjectField(TEXT("key"),Property(Map->KeyProp,Depth+1));Row->SetObjectField(TEXT("value"),Property(Map->ValueProp,Depth+1));}
            else if(const auto* Set=CastField<FSetProperty>(Field))Row->SetObjectField(TEXT("element"),Property(Set->ElementProp,Depth+1));
            else if(const auto* Enum=CastField<FEnumProperty>(Field))
            {Row->SetStringField(TEXT("enum_type"),GetPathNameSafe(Enum->GetEnum()));Row->SetObjectField(TEXT("underlying"),Property(Enum->GetUnderlyingProperty(),Depth+1));AddEnum(Enum->GetEnum());}
            else if(const auto* Byte=CastField<FByteProperty>(Field))
            {Row->SetStringField(TEXT("enum_type"),GetPathNameSafe(Byte->Enum));AddEnum(Byte->Enum);}
            else if(const auto* Struct=CastField<FStructProperty>(Field))
            {Row->SetStringField(TEXT("struct_type"),GetPathNameSafe(Struct->Struct));AddStruct(Struct->Struct,Depth+1);}
            else if(const auto* Class=CastField<FClassProperty>(Field))
            {Row->SetStringField(TEXT("property_class_target"),GetPathNameSafe(Class->PropertyClass));Row->SetStringField(TEXT("meta_class"),GetPathNameSafe(Class->MetaClass));}
            else if(const auto* SoftClass=CastField<FSoftClassProperty>(Field))
            {Row->SetStringField(TEXT("property_class_target"),GetPathNameSafe(SoftClass->PropertyClass));Row->SetStringField(TEXT("meta_class"),GetPathNameSafe(SoftClass->MetaClass));}
            else if(const auto* Object=CastField<FObjectPropertyBase>(Field))Row->SetStringField(TEXT("object_target"),GetPathNameSafe(Object->PropertyClass));
            else if(const auto* Interface=CastField<FInterfaceProperty>(Field))Row->SetStringField(TEXT("interface_target"),GetPathNameSafe(Interface->InterfaceClass));
            return Row;
        }
        void AddStruct(const UStruct* Type,int32 Depth)
        {
            if(!Type||Depth>64||SeenStructs.Num()>=4096){bValid=false;return;}
            if(SeenStructs.Contains(Type))return;
            SeenStructs.Add(Type);auto Row=MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("type_id"),Type->GetPathName());
            Row->SetStringField(TEXT("super_type"),GetPathNameSafe(Type->GetSuperStruct()));
            if(const auto* Script=Cast<UScriptStruct>(Type))
            {
                Row->SetStringField(TEXT("struct_flags_hex"),FString::Printf(TEXT("%016llx"),static_cast<unsigned long long>(Script->StructFlags)));
                Row->SetBoolField(TEXT("native_serialize"),(Script->StructFlags&STRUCT_SerializeNative)!=0);
            }
            // Add before descending so recursive references are represented once.
            Structs.Add(MakeShared<FJsonValueObject>(Row));
            TArray<TSharedPtr<FJsonValue>> Fields;int32 Order=0;
            for(const FProperty* Field=Type->PropertyLink;Field;Field=Field->PropertyLinkNext)
            {
                auto Value=Property(Field,Depth+1);Value->SetNumberField(TEXT("serialize_bin_order"),Order++);
                Fields.Add(MakeShared<FJsonValueObject>(Value));
            }
            Row->SetArrayField(TEXT("ordered_fields_including_inherited"),Fields);
        }
    };
    bool ExportFrozenV4Descriptor(UAftershockSaveGame& Fixture,const FString& Params,
        bool (*Encode)(UAftershockSaveGame&,TArray<uint8>&),
        UAftershockSaveGame* (*Decode)(const TArray<uint8>&),
        UAftershockSaveGame* (*ReadEnvelope)(const TArray<uint8>&))
    {
        const FString Dir=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("FrozenSaveSchemas/R16_V4"));
        const FString DescriptorPath=Dir/TEXT("frozen_v4_schema.json");
        IFileManager::Get().MakeDirectory(*Dir,true);auto Report=MakeShared<FJsonObject>();
        Report->SetBoolField(TEXT("valid"),false);Report->SetBoolField(TEXT("in_progress"),true);
        Report->SetStringField(TEXT("exported_utc"),FDateTime::UtcNow().ToIso8601());
        FString Revision;FParse::Value(*Params,TEXT("FrozenSourceRevision="),Revision);Report->SetStringField(TEXT("source_revision_label"),Revision);
        const auto Write=[&]()
        {
            FString JSON;const auto Writer=TJsonWriterFactory<>::Create(&JSON);
            if(!FJsonSerializer::Serialize(Report,Writer))return false;
            const FString Temp=DescriptorPath+TEXT(".tmp");
            return FFileHelper::SaveStringToFile(JSON,*Temp,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
                &&IFileManager::Get().Move(*DescriptorPath,*Temp,true,false,false,true);
        };
        if(!Write())return false;
        AftershockSaveBinarySchema::FSchema Schema;FBlake3Hash Wire;
        const FPackageFileVersion Package=GPackageFileUEVersion;const auto Engine=FEngineVersion::Current();
        const FCustomVersionContainer Versions=FCurrentCustomVersions::GetAll();
        if(UAftershockSaveGame::CurrentVersion!=4||static_cast<int32>(EDisasterType::Drought)!=6
            ||StaticEnum<EDisasterType>()->NumEnums()!=8
            ||!AftershockSaveBinarySchema::Build(UAftershockSaveGame::StaticClass(),Schema)
            ||!AftershockSaveBinarySchema::BindMetadata(Schema,Package,Engine,Versions,Wire))
        {Report->SetBoolField(TEXT("in_progress"),false);Report->SetStringField(TEXT("error"),TEXT("R16 V4 original7-hazard reflection/native schema required before extension."));Write();return false;}
        FFrozenV4Descriptor Descriptor;Descriptor.AddStruct(UAftershockSaveGame::StaticClass(),0);
        Report->SetStringField(TEXT("root_type"),UAftershockSaveGame::StaticClass()->GetPathName());
        Report->SetNumberField(TEXT("save_schema_version"),4);Report->SetNumberField(TEXT("payload_version"),AftershockSaveBinarySchema::PayloadVersion);
        Report->SetNumberField(TEXT("native_codec_version"),AftershockSaveBinarySchema::NativeCodecVersion);
        Report->SetStringField(TEXT("schema_fingerprint"),BytesToHex(Schema.Fingerprint.GetBytes(),32));
        Report->SetStringField(TEXT("wire_fingerprint_current_native_metadata"),BytesToHex(Wire.GetBytes(),32));
        Report->SetNumberField(TEXT("fingerprinted_property_visits"),Schema.PropertyCount);Report->SetNumberField(TEXT("fingerprinted_struct_visits"),Schema.StructCount);
        Report->SetStringField(TEXT("engine_version"),Engine.ToString());Report->SetStringField(TEXT("engine_branch"),Engine.GetBranchDescriptor());
        Report->SetNumberField(TEXT("package_ue4"),Package.FileVersionUE4);Report->SetNumberField(TEXT("package_ue5"),Package.FileVersionUE5);
        TArray<TSharedPtr<FJsonValue>> Custom;
        for(const auto& Version:Versions.GetAllVersions())
        {auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("guid"),Version.Key.ToString());Row->SetNumberField(TEXT("version"),Version.Version);Custom.Add(MakeShared<FJsonValueObject>(Row));}
        Report->SetArrayField(TEXT("custom_versions"),Custom);Report->SetArrayField(TEXT("structs"),Descriptor.Structs);Report->SetArrayField(TEXT("enums"),Descriptor.Enums);
        Report->SetBoolField(TEXT("user_save_files_read_or_modified"),false);
        Report->SetStringField(TEXT("scope"),TEXT("Frozen complete ordered reflection descriptor plus current native wire bindings and synthetic golden payloads. Descriptor is not yet a legacy decoder. No claim of gameplay-valid historical save migration."));
        TArray<uint8> Binary,Tagged;
        constexpr int32 MaximumGoldenPayloadBytes=64*1024*1024;
        const bool Encoded=Descriptor.bValid&&Encode(Fixture,Binary)
            &&AftershockSaveBinarySchema::EncodeLegacyTaggedFixture(Fixture,Tagged)
            &&Binary.Num()>0&&Binary.Num()<=MaximumGoldenPayloadBytes&&Tagged.Num()>0&&Tagged.Num()<=MaximumGoldenPayloadBytes;
        TStrongObjectPtr<UAftershockSaveGame> BinaryRead(Encoded?Decode(Binary):nullptr);
        TStrongObjectPtr<UAftershockSaveGame> TaggedRead(Encoded?Decode(Tagged):nullptr);
        const bool Same=Encoded&&SameSaveFields(&Fixture,BinaryRead.Get())&&SameSaveFields(&Fixture,TaggedRead.Get());
        TArray<uint8> BinaryEnvelope,TaggedEnvelope;
        const bool Wrapped=Same&&WrapCodecEnvelope(Binary,3,BinaryEnvelope)&&WrapCodecEnvelope(Tagged,3,TaggedEnvelope)
            &&BinaryEnvelope.Num()<=MaximumGoldenPayloadBytes&&TaggedEnvelope.Num()<=MaximumGoldenPayloadBytes;
        TStrongObjectPtr<UAftershockSaveGame> BinaryEnvelopeRead(Wrapped?ReadEnvelope(BinaryEnvelope):nullptr);
        TStrongObjectPtr<UAftershockSaveGame> TaggedEnvelopeRead(Wrapped?ReadEnvelope(TaggedEnvelope):nullptr);
        const bool EnvelopeSame=Wrapped&&SameSaveFields(&Fixture,BinaryEnvelopeRead.Get())&&SameSaveFields(&Fixture,TaggedEnvelopeRead.Get());
        const bool Saved=EnvelopeSame&&FFileHelper::SaveArrayToFile(BinaryEnvelope,*(Dir/TEXT("golden_v4_binary_v2_envelope3.savfixture")))
            &&FFileHelper::SaveArrayToFile(TaggedEnvelope,*(Dir/TEXT("golden_v4_tagged_v1_envelope3.savfixture")));
        const auto Hash=[](const TArray<uint8>& Bytes){uint8 Digest[FSHA1::DigestSize];FSHA1::HashBuffer(Bytes.GetData(),Bytes.Num(),Digest);return BytesToHex(Digest,FSHA1::DigestSize);};
        Report->SetBoolField(TEXT("synthetic_both_payloads_roundtrip_all_fields"),Same);
        Report->SetBoolField(TEXT("synthetic_both_compressed_envelopes_roundtrip_all_fields"),EnvelopeSame);
        Report->SetNumberField(TEXT("golden_payload_hard_limit_bytes_each"),MaximumGoldenPayloadBytes);
        Report->SetNumberField(TEXT("binary_envelope_bytes"),BinaryEnvelope.Num());Report->SetStringField(TEXT("binary_envelope_sha1"),Hash(BinaryEnvelope));
        Report->SetNumberField(TEXT("tagged_envelope_bytes"),TaggedEnvelope.Num());Report->SetStringField(TEXT("tagged_envelope_sha1"),Hash(TaggedEnvelope));
        Report->SetNumberField(TEXT("binary_payload_bytes"),Binary.Num());Report->SetStringField(TEXT("binary_payload_sha1"),Hash(Binary));
        Report->SetNumberField(TEXT("tagged_payload_bytes"),Tagged.Num());Report->SetStringField(TEXT("tagged_payload_sha1"),Hash(Tagged));
        Report->SetBoolField(TEXT("in_progress"),false);Report->SetBoolField(TEXT("valid"),Saved);
        const bool ReceiptSaved=Write();UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_FROZEN_V4_SCHEMA valid=%d structs=%d enums=%d path=%s"),Saved&&ReceiptSaved,Descriptor.Structs.Num(),Descriptor.Enums.Num(),*DescriptorPath);
        return Saved&&ReceiptSaved;
    }

    bool ExportFrozenV5Descriptor(UAftershockSaveGame& Fixture,const FString& Params,
        bool (*Encode)(UAftershockSaveGame&,TArray<uint8>&),
        UAftershockSaveGame* (*Decode)(const TArray<uint8>&),
        UAftershockSaveGame* (*ReadEnvelope)(const TArray<uint8>&))
    {
        const FString Dir=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("FrozenSaveSchemas/R22_V5_6210f1c"));
        const FString DescriptorPath=Dir/TEXT("frozen_v5_schema.json");
        IFileManager::Get().MakeDirectory(*Dir,true);auto Report=MakeShared<FJsonObject>();
        Report->SetBoolField(TEXT("valid"),false);Report->SetBoolField(TEXT("in_progress"),true);
        Report->SetStringField(TEXT("exported_utc"),FDateTime::UtcNow().ToIso8601());
        FString Revision;FParse::Value(*Params,TEXT("FrozenSourceRevision="),Revision);Report->SetStringField(TEXT("source_revision_label"),Revision);
        const auto Write=[&]()
        {
            FString JSON;const auto Writer=TJsonWriterFactory<>::Create(&JSON);
            if(!FJsonSerializer::Serialize(Report,Writer))return false;
            const FString Temp=DescriptorPath+TEXT(".tmp");
            return FFileHelper::SaveStringToFile(JSON,*Temp,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
                &&IFileManager::Get().Move(*DescriptorPath,*Temp,true,false,false,true);
        };
        if(!Write())return false;
        AftershockSaveBinarySchema::FSchema Schema;FBlake3Hash Wire;
        const FPackageFileVersion Package=GPackageFileUEVersion;const auto Engine=FEngineVersion::Current();
        const FCustomVersionContainer Versions=FCurrentCustomVersions::GetAll();
        if(UAftershockSaveGame::CurrentVersion!=5||static_cast<int32>(EDisasterType::Landslide)!=8
            ||StaticEnum<EDisasterType>()->NumEnums()!=10
            ||!AftershockSaveBinarySchema::Build(UAftershockSaveGame::StaticClass(),Schema)
            ||!AftershockSaveBinarySchema::BindMetadata(Schema,Package,Engine,Versions,Wire))
        {Report->SetBoolField(TEXT("in_progress"),false);Report->SetStringField(TEXT("error"),TEXT("Published V5 nine-hazard reflection/native schema required before any V6 extension."));Write();return false;}
        // Candidate expected fingerprint observed in the existing V5 codec receipt.
        // Root must bind this export's binary/source to published6210f1c before
        // accepting it as immutable V5. Never relax this after adding fields.
        const FString CandidateV5Fingerprint=TEXT("F09E547CAE91D60107D52D0A312C6D6BF849903C42C19AB40A57497088A1F352");
        if(BytesToHex(Schema.Fingerprint.GetBytes(),32)!=CandidateV5Fingerprint
            ||Schema.PropertyCount!=972||Schema.StructCount!=125
            ||FindFProperty<FProperty>(FAftershockPhysicalHazardSaveState::StaticStruct(),TEXT("ObstructionModelVersion")))
        {Report->SetBoolField(TEXT("in_progress"),false);Report->SetStringField(TEXT("error"),TEXT("V5 source/schema drift; export rejected before freezing a different layout."));Write();return false;}
        FFrozenV4Descriptor Descriptor;Descriptor.AddStruct(UAftershockSaveGame::StaticClass(),0);
        Report->SetStringField(TEXT("root_type"),UAftershockSaveGame::StaticClass()->GetPathName());
        Report->SetNumberField(TEXT("save_schema_version"),5);Report->SetNumberField(TEXT("payload_version"),AftershockSaveBinarySchema::PayloadVersion);
        Report->SetNumberField(TEXT("native_codec_version"),AftershockSaveBinarySchema::NativeCodecVersion);
        Report->SetStringField(TEXT("schema_fingerprint"),BytesToHex(Schema.Fingerprint.GetBytes(),32));
        Report->SetStringField(TEXT("wire_fingerprint_current_native_metadata"),BytesToHex(Wire.GetBytes(),32));
        Report->SetNumberField(TEXT("fingerprinted_property_visits"),Schema.PropertyCount);Report->SetNumberField(TEXT("fingerprinted_struct_visits"),Schema.StructCount);
        Report->SetStringField(TEXT("engine_version"),Engine.ToString());Report->SetStringField(TEXT("engine_branch"),Engine.GetBranchDescriptor());
        Report->SetNumberField(TEXT("package_ue4"),Package.FileVersionUE4);Report->SetNumberField(TEXT("package_ue5"),Package.FileVersionUE5);
        TArray<TSharedPtr<FJsonValue>> Custom;
        for(const auto& Version:Versions.GetAllVersions())
        {auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("guid"),Version.Key.ToString());Row->SetNumberField(TEXT("version"),Version.Version);Custom.Add(MakeShared<FJsonValueObject>(Row));}
        Report->SetArrayField(TEXT("custom_versions"),Custom);Report->SetArrayField(TEXT("structs"),Descriptor.Structs);Report->SetArrayField(TEXT("enums"),Descriptor.Enums);
        Report->SetBoolField(TEXT("user_save_files_read_or_modified"),false);
        Report->SetStringField(TEXT("scope"),TEXT("Frozen complete ordered reflection descriptor plus current native wire bindings and synthetic golden payloads. V5 is published at6210f1c; this export is not yet a V5 legacy decoder. No claim of gameplay-valid historical save migration."));
        TArray<uint8> Binary,Tagged;
        constexpr int32 MaximumGoldenPayloadBytes=64*1024*1024;
        const bool Encoded=Descriptor.bValid&&Encode(Fixture,Binary)
            &&AftershockSaveBinarySchema::EncodeLegacyTaggedFixture(Fixture,Tagged)
            &&Binary.Num()>0&&Binary.Num()<=MaximumGoldenPayloadBytes&&Tagged.Num()>0&&Tagged.Num()<=MaximumGoldenPayloadBytes;
        TStrongObjectPtr<UAftershockSaveGame> BinaryRead(Encoded?Decode(Binary):nullptr);
        TStrongObjectPtr<UAftershockSaveGame> TaggedRead(Encoded?Decode(Tagged):nullptr);
        const bool Same=Encoded&&SameSaveFields(&Fixture,BinaryRead.Get())&&SameSaveFields(&Fixture,TaggedRead.Get());
        TArray<uint8> BinaryEnvelope,TaggedEnvelope;
        const bool Wrapped=Same&&WrapCodecEnvelope(Binary,3,BinaryEnvelope)&&WrapCodecEnvelope(Tagged,3,TaggedEnvelope)
            &&BinaryEnvelope.Num()<=MaximumGoldenPayloadBytes&&TaggedEnvelope.Num()<=MaximumGoldenPayloadBytes;
        TStrongObjectPtr<UAftershockSaveGame> BinaryEnvelopeRead(Wrapped?ReadEnvelope(BinaryEnvelope):nullptr);
        TStrongObjectPtr<UAftershockSaveGame> TaggedEnvelopeRead(Wrapped?ReadEnvelope(TaggedEnvelope):nullptr);
        const bool EnvelopeSame=Wrapped&&SameSaveFields(&Fixture,BinaryEnvelopeRead.Get())&&SameSaveFields(&Fixture,TaggedEnvelopeRead.Get());
        const bool Saved=EnvelopeSame&&FFileHelper::SaveArrayToFile(BinaryEnvelope,*(Dir/TEXT("golden_v5_binary_v2_envelope3.savfixture")))
            &&FFileHelper::SaveArrayToFile(TaggedEnvelope,*(Dir/TEXT("golden_v5_tagged_v1_envelope3.savfixture")));
        const auto Hash=[](const TArray<uint8>& Bytes){uint8 Digest[FSHA1::DigestSize];FSHA1::HashBuffer(Bytes.GetData(),Bytes.Num(),Digest);return BytesToHex(Digest,FSHA1::DigestSize);};
        Report->SetBoolField(TEXT("synthetic_both_payloads_roundtrip_all_fields"),Same);
        Report->SetBoolField(TEXT("synthetic_both_compressed_envelopes_roundtrip_all_fields"),EnvelopeSame);
        Report->SetNumberField(TEXT("golden_payload_hard_limit_bytes_each"),MaximumGoldenPayloadBytes);
        Report->SetNumberField(TEXT("binary_envelope_bytes"),BinaryEnvelope.Num());Report->SetStringField(TEXT("binary_envelope_sha1"),Hash(BinaryEnvelope));
        Report->SetNumberField(TEXT("tagged_envelope_bytes"),TaggedEnvelope.Num());Report->SetStringField(TEXT("tagged_envelope_sha1"),Hash(TaggedEnvelope));
        Report->SetNumberField(TEXT("binary_payload_bytes"),Binary.Num());Report->SetStringField(TEXT("binary_payload_sha1"),Hash(Binary));
        Report->SetNumberField(TEXT("tagged_payload_bytes"),Tagged.Num());Report->SetStringField(TEXT("tagged_payload_sha1"),Hash(Tagged));
        Report->SetBoolField(TEXT("in_progress"),false);Report->SetBoolField(TEXT("valid"),Saved);
        const bool ReceiptSaved=Write();UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_FROZEN_V5_SCHEMA valid=%d structs=%d enums=%d path=%s"),Saved&&ReceiptSaved,Descriptor.Structs.Num(),Descriptor.Enums.Num(),*DescriptorPath);
        return Saved&&ReceiptSaved;
    }

}

UAftershockSaveCodecAuditCommandlet::UAftershockSaveCodecAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}

int32 UAftershockSaveCodecAuditCommandlet::Main(const FString& Params)
{
    if(FParse::Param(*Params,TEXT("AuditFrozenV4Decoder")))return AftershockLegacyV4::RunGoldenAudit([](const TArray<uint8>& Bytes){return UAftershockSaveSubsystem::ReadTypedEnvelope(Bytes);});
	TArray<FCodecCheck> Checks;int32 Failed=0;double BinaryMs=0.,TaggedMs=0.,BinaryReadMs=0.;int32 BinaryBytes=0,TaggedBytes=0;
	AftershockSaveBinarySchema::FSchema Schema;
	const auto Check=[&](const TCHAR* Name,bool bPass)
	{
		Checks.Add({Name,bPass});Failed+=bPass?0:1;
		if(bPass){UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SAVE_CODEC_AUDIT_PASS %s"),Name);}
		else{UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_SAVE_CODEC_AUDIT_FAIL %s"),Name);}
	};
	const auto Finish=[&]()
	{
		const FString Dir=FPaths::ProjectSavedDir()/TEXT("PresentationValidation");IFileManager::Get().MakeDirectory(*Dir,true);
		FString JSON=FString::Printf(TEXT("{\"schema_version\":1,\"scope\":\"Same-process isolated synthetic8500-resident/15625-cell serialization fixture. Codec fields, rejection and generated legacy envelope compatibility only; not a valid playable city, historical golden-file, cross-build, cross-platform, packaged load or asynchronous save acceptance.\",\"payload_codec\":2,\"native_codec\":1,\"save_schema\":%d,\"fixture_residents\":8500,\"fixture_cells\":15625,\"binary_encode_ms\":%.6f,\"tagged_encode_ms\":%.6f,\"binary_decode_ms\":%.6f,\"binary_bytes\":%d,\"tagged_bytes\":%d,\"schema_properties\":%d,\"schema_structs\":%d,\"schema_fingerprint\":\"%s\",\"passed\":%d,\"failed\":%d,\"checks\":["),UAftershockSaveGame::CurrentVersion,BinaryMs,TaggedMs,BinaryReadMs,BinaryBytes,TaggedBytes,Schema.PropertyCount,Schema.StructCount,*BytesToHex(Schema.Fingerprint.GetBytes(),32),Checks.Num()-Failed,Failed);
		for(int32 I=0;I<Checks.Num();++I)JSON+=FString::Printf(TEXT("%s{\"name\":\"%s\",\"passed\":%s}"),I?TEXT(","):TEXT(""),*Checks[I].Name,Checks[I].bPass?TEXT("true"):TEXT("false"));
		JSON+=FString::Printf(TEXT("],\"fingerprint_failure_reason\":\"%s\",\"fingerprint_failed_field\":\"%s\",\"fingerprint_failed_field_class\":\"%s\"}"),*Schema.FailureReason,*Schema.FailedField,*Schema.FailedFieldClass);
		if(!FFileHelper::SaveStringToFile(JSON,*(Dir/TEXT("save_codec_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return 2;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SAVE_CODEC_AUDIT_RESULT passed=%d failed=%d binaryEncodeMs=%.3f taggedEncodeMs=%.3f binaryDecodeMs=%.3f binaryBytes=%d taggedBytes=%d"),Checks.Num()-Failed,Failed,BinaryMs,TaggedMs,BinaryReadMs,BinaryBytes,TaggedBytes);return Failed?1:0;
	};
	TStrongObjectPtr<UAftershockSaveGame> Source(NewObject<UAftershockSaveGame>(GetTransientPackage()));
	Source->SessionID=FGuid(0xAF531826,0x11223344,0x12345678,0x87654321);Source->SavedAt=FDateTime(2026,10,18,12,0,0);Source->Economy.Treasury=9876543;
	Source->Terrain.GridWidth=125;Source->Terrain.GridHeight=125;Source->Terrain.CellSize=1000.f;Source->Terrain.Cells.Reserve(15625);
	for(int32 I=0;I<15625;++I){FGridCell Cell;Cell.Coords=FIntPoint(I%125,I/125);Cell.Elevation=static_cast<float>(I%100)-20.f;Cell.SoilMoisture01=.3f;Source->Terrain.Cells.Add(Cell);}
	Source->CityLife.Residents.Reserve(8500);const FText Name=FText::FromString(TEXT("Çağla İpek"));
	for(int32 I=0;I<8500;++I){FCityResidentRecord Resident;Resident.ResidentID=I+1;Resident.HomeBuildingID=1+I%1000;Resident.Age=18+I%70;Resident.FullName=Name;Resident.Happiness01=.61f;Source->CityLife.Residents.Add(Resident);}
	Source->DisasterVisual.Type=EDisasterType::Flood;Source->DisasterVisual.FloodModelVersion=1;Source->DisasterVisual.RainfallPulseMm=120.f;Source->DisasterVisual.FloodPulseDurationSeconds=3600.f;
	Source->DisasterVisual.WaterDepthCm.Add(FIntPoint(1,2),12.f);Source->DisasterVisual.WaterSurfaceElevationCm.Add(FIntPoint(1,2),-8.f);Source->DisasterVisual.WaterArrivalFraction.Add(FIntPoint(1,2),1.f);
	Source->Rubble.AddDefaulted_GetRef().FracturedAssetPath=FSoftObjectPath(TEXT("/Game/Buildings/Fractured/GC_House_01.GC_House_01:Fragment_Çağla"));
    if(FParse::Param(*Params,TEXT("ExportFrozenV5Schema")))
    {
        // Synthetic serializer fixture, not a playable physical world. Exercise
        // V5 new fields/negative coords/nonzero accepted forcing/new hazard enums.
        Source->Physics.ModelVersion=2;Source->Physics.bWaterInitialized=true;
        Source->Physics.CapturedCellAreaM2=100.;Source->Physics.PhysicalSeconds=31.25;
        Source->Physics.PendingPhysicalSeconds=.0625;Source->Physics.PendingAcceptedSeconds=.03125;
        Source->Physics.PublishedStep=7;Source->Physics.DomainRevision=11;
        Source->Physics.WaterCells.Reserve(2);
        auto& A=Source->Physics.WaterCells.AddDefaulted_GetRef();A.Coord=FIntPoint(-1,0);A.BedElevationCm=-225.;A.VolumeM3=12.5;A.DischargeXM2PerSecond=.125;A.CumulativeInfiltrationM3=3.5;
        auto& B=Source->Physics.WaterCells.AddDefaulted_GetRef();B.Coord=FIntPoint(0,0);B.BedElevationCm=-200.;B.VolumeM3=25.;B.DischargeYM2PerSecond=-.25;B.CumulativeInfiltrationM3=1.25;
        auto& F=Source->Physics.PendingForcingCells.AddDefaulted_GetRef();F.Coord=A.Coord;F.RainMPerSecond=.00001;F.InfiltrationCapacityMPerSecond=.000002;
        Source->Physics.PendingForcingCells.AddDefaulted_GetRef().Coord=B.Coord;
        auto& Boundary=Source->Physics.PendingForcingBoundaries.AddDefaulted_GetRef();Boundary.FaceStableID=123456;Boundary.HeadM=-1.75;Boundary.VelocityXMps=.25;
        Source->Physics.WaterBudget.InitialSurfaceM3=42.25;Source->Physics.WaterBudget.FinalSurfaceM3=37.5;
        Source->Physics.WaterBudget.InfiltrationM3=4.75;Source->Physics.WaterBudget.AcceptedSteps=7;Source->Physics.WaterBudget.FaceEvaluations=56;
        Source->Physics.Episode.Type=EDisasterType::Landslide;
        Source->DisasterVisual.Type=EDisasterType::Avalanche;
        Source->Physics.MaterialScenarioReceiptJSON=TEXT("{\"scope\":\"synthetic-v5-wire-only\"}");
        return ExportFrozenV5Descriptor(*Source,Params,&UAftershockSaveSubsystem::EncodeIndexedPayload,
            &UAftershockSaveSubsystem::DecodeIndexedPayload,&UAftershockSaveSubsystem::ReadTypedEnvelope)?0:1;
    }
	if(FParse::Param(*Params,TEXT("ExportFrozenV4Schema")))return ExportFrozenV4Descriptor(*Source,Params,
        &UAftershockSaveSubsystem::EncodeIndexedPayload,&UAftershockSaveSubsystem::DecodeIndexedPayload,
        &UAftershockSaveSubsystem::ReadTypedEnvelope)?0:1;
	Check(TEXT("runtime_recursive_fingerprint_available"),AftershockSaveBinarySchema::Build(Source->GetClass(),Schema)&&!Schema.Fingerprint.IsZero()&&Schema.PropertyCount>100);
	AftershockSaveBinarySchema::FSchema RepeatSchema;
	Check(TEXT("runtime_fingerprint_repeatable"),AftershockSaveBinarySchema::Build(Source->GetClass(),RepeatSchema)&&Schema.Fingerprint==RepeatSchema.Fingerprint);
	TArray<uint8> Binary;double Start=FPlatformTime::Seconds();const bool BinaryEncoded=UAftershockSaveSubsystem::EncodeIndexedPayload(*Source,Binary);BinaryMs=(FPlatformTime::Seconds()-Start)*1000.;BinaryBytes=Binary.Num();
	Check(TEXT("default_writer_emits_binary_payload_version_two"),BinaryEncoded&&Binary.Num()>44&&Binary[4]==2&&Binary[5]==0&&Binary[6]==0&&Binary[7]==0);
	Start=FPlatformTime::Seconds();TStrongObjectPtr<UAftershockSaveGame> BinaryRead(BinaryEncoded?UAftershockSaveSubsystem::DecodeIndexedPayload(Binary):nullptr);BinaryReadMs=(FPlatformTime::Seconds()-Start)*1000.;
	Check(TEXT("binary_roundtrip_preserves_all_reflected_save_fields"),SameSaveFields(Source.Get(),BinaryRead.Get()));
	Check(TEXT("binary_roundtrip_preserves_new_physical_maps_and_negative_head"),BinaryRead.IsValid()&&BinaryRead->DisasterVisual.FloodModelVersion==1&&BinaryRead->DisasterVisual.WaterSurfaceElevationCm.FindRef(FIntPoint(1,2))==-8.f&&BinaryRead->DisasterVisual.WaterArrivalFraction.FindRef(FIntPoint(1,2))==1.f);
	Check(TEXT("binary_roundtrip_preserves_utf8_soft_object_subpath"),BinaryRead.IsValid()&&BinaryRead->Rubble.Num()==1&&BinaryRead->Rubble[0].FracturedAssetPath==Source->Rubble[0].FracturedAssetPath);
	const uint64 DeserializeBefore=AftershockSaveBinarySchema::GetBinaryDeserializeCount();
	if(Binary.Num()>44)
	{
		TArray<uint8> Mutated=Binary;Mutated[12]^=1;
		Check(TEXT("mismatched_schema_rejected_before_binary_deserialize"),UAftershockSaveSubsystem::DecodeIndexedPayload(Mutated)==nullptr&&AftershockSaveBinarySchema::GetBinaryDeserializeCount()==DeserializeBefore);
		Mutated=Binary;Mutated[4]=99;
		Check(TEXT("unknown_payload_codec_rejected"),UAftershockSaveSubsystem::DecodeIndexedPayload(Mutated)==nullptr&&AftershockSaveBinarySchema::GetBinaryDeserializeCount()==DeserializeBefore);
		Mutated=Binary;Mutated[8]^=1;
		Check(TEXT("wrong_root_class_rejected"),UAftershockSaveSubsystem::DecodeIndexedPayload(Mutated)==nullptr&&AftershockSaveBinarySchema::GetBinaryDeserializeCount()==DeserializeBefore);
		Mutated=Binary;Mutated.SetNum(Mutated.Num()-1,EAllowShrinking::No);
		Check(TEXT("truncated_binary_body_rejected_before_deserialize"),UAftershockSaveSubsystem::DecodeIndexedPayload(Mutated)==nullptr&&AftershockSaveBinarySchema::GetBinaryDeserializeCount()==DeserializeBefore);
		Mutated=Binary;Mutated.Add(0);
		Check(TEXT("trailing_binary_bytes_rejected_before_deserialize"),UAftershockSaveSubsystem::DecodeIndexedPayload(Mutated)==nullptr&&AftershockSaveBinarySchema::GetBinaryDeserializeCount()==DeserializeBefore);
		FCodecHeaderLayout Layout;const bool LayoutValid=ReadCodecHeaderLayout(Binary,Layout);
		Check(TEXT("binary_native_header_locations_valid"),LayoutValid);
		if(LayoutValid)
		{
			Mutated=Binary;int32 UE4=0;FMemory::Memcpy(&UE4,Mutated.GetData()+Layout.PackageAt,4);--UE4;FMemory::Memcpy(Mutated.GetData()+Layout.PackageAt,&UE4,4);
			Check(TEXT("binary_older_compatible_package_header_rejected_before_deserialize"),UAftershockSaveSubsystem::DecodeIndexedPayload(Mutated)==nullptr&&AftershockSaveBinarySchema::GetBinaryDeserializeCount()==DeserializeBefore);
			Mutated=Binary;uint16 Patch=0;FMemory::Memcpy(&Patch,Mutated.GetData()+Layout.EngineAt+4,2);Patch=Patch?Patch-1:1;FMemory::Memcpy(Mutated.GetData()+Layout.EngineAt+4,&Patch,2);
			Check(TEXT("binary_different_engine_patch_header_rejected_before_deserialize"),UAftershockSaveSubsystem::DecodeIndexedPayload(Mutated)==nullptr&&AftershockSaveBinarySchema::GetBinaryDeserializeCount()==DeserializeBefore);
			Mutated=Binary;Mutated[Layout.CustomRowsAt+16]^=1;
			Check(TEXT("binary_custom_version_tamper_rejected_before_deserialize"),UAftershockSaveSubsystem::DecodeIndexedPayload(Mutated)==nullptr&&AftershockSaveBinarySchema::GetBinaryDeserializeCount()==DeserializeBefore);
		}
	}
	const FCustomVersionContainer Versions=FCurrentCustomVersions::GetAll();FCustomVersionContainer ReversedVersions;
	for(int32 I=Versions.GetAllVersions().Num()-1;I>=0;--I){const auto& Version=Versions.GetAllVersions()[I];ReversedVersions.SetVersion(Version.Key,Version.Version,NAME_None);}
	FBlake3Hash Bound,ReversedBound;
	Check(TEXT("native_header_digest_independent_of_custom_registration_order"),AftershockSaveBinarySchema::BindMetadata(Schema,GPackageFileUEVersion,FEngineVersion::Current(),Versions,Bound)
		&&AftershockSaveBinarySchema::BindMetadata(Schema,GPackageFileUEVersion,FEngineVersion::Current(),ReversedVersions,ReversedBound)&&Bound==ReversedBound);
	TArray<uint8> Tagged;Start=FPlatformTime::Seconds();const bool TaggedEncoded=AftershockSaveBinarySchema::EncodeLegacyTaggedFixture(*Source,Tagged);TaggedMs=(FPlatformTime::Seconds()-Start)*1000.;TaggedBytes=Tagged.Num();
	TStrongObjectPtr<UAftershockSaveGame> TaggedRead(TaggedEncoded?UAftershockSaveSubsystem::DecodeIndexedPayload(Tagged):nullptr);
	Check(TEXT("legacy_tagged_payload_one_still_decodes_losslessly"),TaggedEncoded&&Tagged.Num()>12&&Tagged[4]==1&&SameSaveFields(Source.Get(),TaggedRead.Get()));
	Check(TEXT("tagged_reader_does_not_use_binary_dispatch"),AftershockSaveBinarySchema::GetBinaryDeserializeCount()==DeserializeBefore);
	TArray<uint8> LegacyMemory;const bool LegacyMemoryEncoded=UGameplayStatics::SaveGameToMemory(Source.Get(),LegacyMemory);
	Check(TEXT("legacy_native_savegame_memory_fixture_ready"),LegacyMemoryEncoded&&!LegacyMemory.IsEmpty());
	for(int32 EnvelopeVersion:{1,2,3})
	{
		TArray<uint8> Envelope;const auto& Body=EnvelopeVersion==3?Tagged:LegacyMemory;
		const bool Wrapped=WrapCodecEnvelope(Body,EnvelopeVersion,Envelope);
		TStrongObjectPtr<UAftershockSaveGame> EnvelopeRead(Wrapped?UAftershockSaveSubsystem::ReadTypedEnvelope(Envelope):nullptr);
		Check(*FString::Printf(TEXT("legacy_envelope_%d_decodes_same_reflected_fields"),EnvelopeVersion),Wrapped&&SameSaveFields(Source.Get(),EnvelopeRead.Get()));
	}
	TArray<uint8> NewEnvelope;const bool NewWrapped=WrapCodecEnvelope(Binary,3,NewEnvelope);
	TStrongObjectPtr<UAftershockSaveGame> NewEnvelopeRead(NewWrapped?UAftershockSaveSubsystem::ReadTypedEnvelope(NewEnvelope):nullptr);
	Check(TEXT("current_envelope_three_dispatches_binary_codec_two"),NewWrapped&&SameSaveFields(Source.Get(),NewEnvelopeRead.Get()));
	Check(TEXT("codec_operations_leave_source_snapshot_unchanged"),SameSaveFields(Source.Get(),BinaryRead.Get())&&Source->Economy.Treasury==9876543&&Source->CityLife.Residents.Num()==8500&&Source->Terrain.Cells.Num()==15625);
	if(FParse::Param(*Params,TEXT("AuditPhysicalSaveV5")))
	{
		const FString FrozenPath=FPaths::ProjectDir()/TEXT("docs/validation/2026-10-18/global-environment/frozen-v4/golden_v4_binary_v2_envelope3.savfixture");
		TArray<uint8> Frozen;const bool Loaded=FFileHelper::LoadFileToArray(Frozen,*FrozenPath);
		const uint64 BeforeFrozenRead=AftershockSaveBinarySchema::GetBinaryDeserializeCount();
		TStrongObjectPtr<UAftershockSaveGame> FrozenRead(Loaded?UAftershockSaveSubsystem::ReadTypedEnvelope(Frozen):nullptr);
		Check(TEXT("v5_production_dispatch_uses_frozen_v4_decoder_on_exact_old_binding"),FrozenRead.IsValid()&&FrozenRead->SchemaVersion==4&&FrozenRead->Physics.ModelVersion==0
			&&AftershockSaveBinarySchema::GetBinaryDeserializeCount()==BeforeFrozenRead);
		TArray<uint8> FrozenPayload;
		bool Unwrapped=false;
		if(Loaded&&Frozen.Num()>=36)
		{
			FMemoryReader R(Frozen,true);uint32 Magic=0;int32 Version=0,N=0,U=0;uint8 Digest[20];R<<Magic<<Version<<N<<U;R.Serialize(Digest,20);
			if(!R.IsError()&&Magic==0x41465348&&Version==3&&N==Frozen.Num()-36&&U>44&&U<=256*1024*1024)
			{FrozenPayload.SetNumUninitialized(U);Unwrapped=FCompression::UncompressMemory(NAME_Zlib,FrozenPayload.GetData(),U,Frozen.GetData()+36,N);}
		}
		Check(TEXT("v5_frozen_fixture_payload_ready_for_rejection_tests"),Unwrapped);
		if(Unwrapped)
		{
			TArray<uint8> Foreign=FrozenPayload,Envelope;Foreign[12]^=0x55;
			const bool Wrapped=WrapCodecEnvelope(Foreign,3,Envelope);
			Check(TEXT("v5_unknown_old_binding_rejected_without_current_binary_dispatch"),Wrapped&&UAftershockSaveSubsystem::ReadTypedEnvelope(Envelope)==nullptr
				&&AftershockSaveBinarySchema::GetBinaryDeserializeCount()==BeforeFrozenRead);
			Foreign=FrozenPayload;Foreign.SetNum(Foreign.Num()-1,EAllowShrinking::No);Envelope.Reset();
			const bool TruncatedWrapped=WrapCodecEnvelope(Foreign,3,Envelope);
			Check(TEXT("v5_exact_old_binding_truncated_body_rejected_without_current_dispatch"),TruncatedWrapped&&UAftershockSaveSubsystem::ReadTypedEnvelope(Envelope)==nullptr
				&&AftershockSaveBinarySchema::GetBinaryDeserializeCount()==BeforeFrozenRead);
		}
	}
	return Finish();
}
#else
UAftershockSaveCodecAuditCommandlet::UAftershockSaveCodecAuditCommandlet(){}
int32 UAftershockSaveCodecAuditCommandlet::Main(const FString& Params){return 1;}
#endif
