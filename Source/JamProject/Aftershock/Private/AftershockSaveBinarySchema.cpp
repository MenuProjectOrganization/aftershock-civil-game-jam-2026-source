#include "AftershockSaveBinarySchema.h"
#include "Aftershock/Public/AftershockSaveGame.h"
#include "Misc/EngineVersion.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/ObjectVersion.h"
#include "UObject/Utf8StrProperty.h"
#include "UObject/AnsiStrProperty.h"

namespace
{
	uint64 BinaryDeserializeCount=0; // Game-thread diagnostic; never persistence authority.
	struct FSchemaBuilder
	{
		FBlake3 Hash;
		TSet<const UStruct*> Active;
		int32 Properties=0,Structs=0;
		FString FailureReason,FailedField,FailedFieldClass;
		bool Fail(const TCHAR* Reason,const FProperty* Field=nullptr)
		{
			if(FailureReason.IsEmpty())
			{
				FailureReason=Reason;FailedField=Field?Field->GetPathName():TEXT("none");FailedFieldClass=Field?Field->GetClass()->GetName():TEXT("none");
			}
			return false;
		}
		void Number(uint64 Value)
		{
			uint8 Bytes[8];for(int32 I=0;I<8;++I)Bytes[I]=static_cast<uint8>(Value>>(I*8));Hash.Update(Bytes,8);
		}
		void Text(const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value);Number(Utf8.Length());if(Utf8.Length())Hash.Update(Utf8.Get(),Utf8.Length());
		}
		void TypeIdentity(const UObject* Type)
		{
			// Canonical script package/type IDs, never a machine path or object address.
			Text(Type?Type->GetOutermost()->GetName().ToLower():TEXT("none"));Text(Type?Type->GetName().ToLower():TEXT("none"));
		}
		bool Enum(const UEnum* Type)
		{
			TypeIdentity(Type);if(!Type){Number(0);return true;}
			if(Type->NumEnums()>65536)return Fail(TEXT("EnumEntryLimit"));Number(Type->NumEnums());
			for(int32 I=0;I<Type->NumEnums();++I){Text(Type->GetNameByIndex(I).ToString().ToLower());Number(static_cast<uint64>(Type->GetValueByIndex(I)));}
			return true;
		}
		bool Property(const FProperty* Field,int32 Depth)
		{
			if(!Field||Depth>64||++Properties>16384)return Fail(TEXT("PropertyTraversalLimit"),Field);
			Text(Field->GetName().ToLower());Text(Field->GetClass()->GetName().ToLower());Number(Field->ArrayDim);
			// Include flags controlling save/transient/editor filtering and future changes.
			Number(static_cast<uint64>(Field->GetPropertyFlags()));
			if(const auto* Array=CastField<FArrayProperty>(Field))return Property(Array->Inner,Depth+1);
			if(const auto* Map=CastField<FMapProperty>(Field))return Property(Map->KeyProp,Depth+1)&&Property(Map->ValueProp,Depth+1);
			if(const auto* Set=CastField<FSetProperty>(Field))return Property(Set->ElementProp,Depth+1);
			if(const auto* Value=CastField<FEnumProperty>(Field))return Enum(Value->GetEnum())&&Property(Value->GetUnderlyingProperty(),Depth+1);
			if(const auto* Byte=CastField<FByteProperty>(Field))return Enum(Byte->Enum);
			if(const auto* Structure=CastField<FStructProperty>(Field))return Struct(Structure->Struct,Depth+1);
			if(const auto* Class=CastField<FClassProperty>(Field)){TypeIdentity(Class->PropertyClass);TypeIdentity(Class->MetaClass);return true;}
			if(const auto* SoftClass=CastField<FSoftClassProperty>(Field)){TypeIdentity(SoftClass->PropertyClass);TypeIdentity(SoftClass->MetaClass);return true;}
			if(const auto* Object=CastField<FObjectPropertyBase>(Field)){TypeIdentity(Object->PropertyClass);return true;}
			if(const auto* Interface=CastField<FInterfaceProperty>(Field)){TypeIdentity(Interface->InterfaceClass);return true;}
			// New property categories must be explicitly fingerprinted before enabling
			// them in this codec, rather than silently omitting their nested schema.
			if(CastField<FNumericProperty>(Field)||CastField<FBoolProperty>(Field)||CastField<FNameProperty>(Field)||CastField<FStrProperty>(Field)
				||CastField<FUtf8StrProperty>(Field)||CastField<FAnsiStrProperty>(Field)||CastField<FTextProperty>(Field))return true;
			return Fail(TEXT("UnsupportedPropertyCategory"),Field);
		}
		bool Struct(const UStruct* Type,int32 Depth)
		{
			if(!Type||Depth>64||++Structs>4096)return Fail(TEXT("StructTraversalLimit"));
			TypeIdentity(Type);
			if(const auto* Script=Cast<UScriptStruct>(Type))Number(static_cast<uint64>(Script->StructFlags&(STRUCT_SerializeNative|STRUCT_Immutable|STRUCT_Atomic)));
			else Number(0);
			if(Active.Contains(Type)){Text(TEXT("RecursiveTypeReference"));return true;}
			Active.Add(Type);int32 Count=0;for(const FProperty* Field=Type->PropertyLink;Field;Field=Field->PropertyLinkNext)++Count;
			Number(Count);
			// This is the exact order UStruct::SerializeBin walks, including inherited fields.
			for(const FProperty* Field=Type->PropertyLink;Field;Field=Field->PropertyLinkNext)if(!Property(Field,Depth+1))return false;
			Active.Remove(Type);return true;
		}
	};
}

bool AftershockSaveBinarySchema::Build(const UClass* Root,FSchema& Out)
{
	Out={};if(!IsInGameThread()){Out.FailureReason=TEXT("NotGameThread");return false;}
	if(Root!=UAftershockSaveGame::StaticClass()){Out.FailureReason=TEXT("UnexpectedRootClass");return false;}
#if !PLATFORM_LITTLE_ENDIAN
	Out.FailureReason=TEXT("UnsupportedByteOrder");return false; // Never misread another byte order.
#else
	FSchemaBuilder Builder;Builder.Text(TEXT("AFTERSHOCK.BinaryProperty.IndexedFName.LE"));
	Builder.Number(PayloadVersion);Builder.Number(NativeCodecVersion);Builder.Number(UAftershockSaveGame::CurrentVersion);
	const auto Engine=FEngineVersion::Current();Builder.Number(Engine.GetMajor());Builder.Number(Engine.GetMinor());Builder.Number(Engine.GetPatch());
	// Package/native version IDs are stable wire metadata, not C++ allocation layout.
	TArray<uint8> VersionBytes;FMemoryWriter VersionWriter(VersionBytes,true);FPackageFileVersion Package=GPackageFileUEVersion;VersionWriter<<Package;
	Builder.Number(VersionBytes.Num());Builder.Hash.Update(VersionBytes.GetData(),VersionBytes.Num());
	const bool Success=Builder.Struct(Root,0);Out.PropertyCount=Builder.Properties;Out.StructCount=Builder.Structs;
	if(!Success)
	{
		Out.FailureReason=Builder.FailureReason;Out.FailedField=Builder.FailedField;Out.FailedFieldClass=Builder.FailedFieldClass;
		UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_SAVE_SCHEMA_REJECTED reason=%s field=%s fieldClass=%s properties=%d structs=%d"),*Out.FailureReason,*Out.FailedField,*Out.FailedFieldClass,Out.PropertyCount,Out.StructCount);
		return false;
	}
	Out.Fingerprint=Builder.Hash.Finalize();
	return !Out.Fingerprint.IsZero();
#endif
}

uint64 AftershockSaveBinarySchema::GetBinaryDeserializeCount(){check(IsInGameThread());return BinaryDeserializeCount;}
void AftershockSaveBinarySchema::RecordBinaryDeserialize(){check(IsInGameThread());++BinaryDeserializeCount;}

bool AftershockSaveBinarySchema::BindMetadata(const FSchema& Schema,const FPackageFileVersion& Package,const FEngineVersion& Engine,
	const FCustomVersionContainer& Versions,FBlake3Hash& Out)
{
	Out.Reset();if(Schema.Fingerprint.IsZero()||Versions.GetAllVersions().Num()>1024)return false;
	FSchemaBuilder Builder;Builder.Text(TEXT("AFTERSHOCK.BinaryProperty.NativeHeaderBinding.v1"));
	Builder.Hash.Update(Schema.Fingerprint.GetBytes(),32);
	TArray<uint8> PackageBytes;FMemoryWriter Writer(PackageBytes,true);FPackageFileVersion PackageCopy=Package;Writer<<PackageCopy;
	Builder.Number(PackageBytes.Num());Builder.Hash.Update(PackageBytes.GetData(),PackageBytes.Num());
	Builder.Number(Engine.GetMajor());Builder.Number(Engine.GetMinor());Builder.Number(Engine.GetPatch());Builder.Number(Engine.GetChangelist());
	// A reader can have different irrelevant plugins registered. Bind the exact
	// stored writer metadata, rather than comparing it with local GetAll().
	struct FVersion{FString Key;int32 Value=0;};TArray<FVersion> Ordered;
	for(const auto& Version:Versions.GetAllVersions())Ordered.Add({Version.Key.ToString(EGuidFormats::Digits),Version.Version});
	Ordered.Sort([](const FVersion& A,const FVersion& B){return A.Key<B.Key;});Builder.Number(Ordered.Num());
	for(const auto& Version:Ordered){Builder.Text(Version.Key);Builder.Number(static_cast<uint64>(static_cast<int64>(Version.Value)));}
	Out=Builder.Hash.Finalize();return !Out.IsZero();
}
