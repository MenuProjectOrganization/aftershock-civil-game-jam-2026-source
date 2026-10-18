#include "AftershockLegacyV4Schema.h"
#include "Serialization/MemoryWriter.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"

namespace AftershockLegacyV4
{
namespace
{
    TSharedPtr<FJsonObject> Descriptor;
    TMap<FString,UStruct*> Types;
    TMap<const UStruct*,FString> OriginalIDs;
    TMap<const FProperty*,TSharedPtr<FJsonObject>> FieldDescriptions;
    TMap<FString,TSharedPtr<FJsonObject>> EnumDescriptions;
    bool Attempted=false,Verified=false;
    FString Failure;
    constexpr EPropertyFlags WireFilterFlags=static_cast<EPropertyFlags>(CPF_Transient|CPF_DuplicateTransient|CPF_NonPIEDuplicateTransient|CPF_NonTransactional|CPF_Deprecated|CPF_DevelopmentAssets|CPF_SkipSerialization|CPF_SaveGame|CPF_EditorOnly);
    bool Fail(const FString& Message){if(Failure.IsEmpty())Failure=Message;return false;}
    bool IsEnumWire(const FJsonObject& Row)
    {const FString Kind=Row.GetStringField(TEXT("property_class"));FString ID;Row.TryGetStringField(TEXT("enum_type"),ID);return Kind==TEXT("EnumProperty")||(Kind==TEXT("ByteProperty")&&!ID.IsEmpty()&&ID!=TEXT("None"));}
    FString OriginalID(const UStruct* Type)
    {if(const auto* ID=OriginalIDs.Find(Type))return *ID;return Type?Type->GetPathName():FString();}
    bool VerifyProperty(FProperty* Field,const TSharedPtr<FJsonObject>& Row,bool IsProject,int32 Depth)
    {
        if(!Field||!Row||Depth>64)return Fail(TEXT("Frozen property nesting/null limit"));
        const FString Kind=Row->GetStringField(TEXT("property_class"));
        const bool EnumWire=IsEnumWire(*Row);
        const FString WireKind=EnumWire&&IsProject?TEXT("NameProperty"):Kind;
        if(Field->GetName()!=Row->GetStringField(TEXT("name"))||Field->GetClass()->GetName()!=WireKind||Field->ArrayDim!=Row->GetIntegerField(TEXT("array_dim")))return Fail(TEXT("Frozen property order/name/class/ArrayDim mismatch: ")+Field->GetPathName());
        const uint64 Flags=FCString::Strtoui64(*Row->GetStringField(TEXT("flags_hex")),nullptr,16);
        if((Flags&CPF_ExperimentalOverridableLogic)!=0)return Fail(TEXT("Experimental container override wire is unsupported"));
        if(IsProject)
        {
            // Only flags selecting wire inclusion are restored on DTO metadata.
            // Never mutate any current-game/native-core reflection properties.
            Field->ClearPropertyFlags(WireFilterFlags);
            Field->SetPropertyFlags(static_cast<EPropertyFlags>(Flags&static_cast<uint64>(WireFilterFlags)));
        }
        if((static_cast<uint64>(Field->GetPropertyFlags())&WireFilterFlags)!=(Flags&WireFilterFlags))return Fail(TEXT("Frozen property serialization-filter flags changed: ")+Field->GetPathName());
        FieldDescriptions.Add(Field,Row);
        if(EnumWire)
        {
            const FString ID=Row->GetStringField(TEXT("enum_type"));
            if(!EnumDescriptions.Contains(ID))return Fail(TEXT("Missing frozen enum name/ordinal table: ")+ID);
            if(IsProject)return CastField<FNameProperty>(Field)!=nullptr;
            UEnum* NativeEnum=nullptr;
            if(const auto* E=CastField<FEnumProperty>(Field))NativeEnum=E->GetEnum();
            else if(const auto* ByteEnum=CastField<FByteProperty>(Field))NativeEnum=ByteEnum->Enum;
            if(!NativeEnum||NativeEnum->GetPathName()!=ID)return Fail(TEXT("Native enum identity changed"));
            const auto& Entries=EnumDescriptions.FindChecked(ID)->GetArrayField(TEXT("entries"));
            if(NativeEnum->NumEnums()!=Entries.Num())return Fail(TEXT("Native enum entry count changed"));
            for(int32 I=0;I<Entries.Num();++I){const auto E=Entries[I]->AsObject();if(NativeEnum->GetNameByIndex(I).ToString()!=E->GetStringField(TEXT("name"))||NativeEnum->GetValueByIndex(I)!=FCString::Atoi64(*E->GetStringField(TEXT("numeric_value"))))return Fail(TEXT("Native enum name/ordinal changed"));}
            return true;
        }
        if(auto* A=CastField<FArrayProperty>(Field))return VerifyProperty(A->Inner,Row->GetObjectField(TEXT("inner")),IsProject,Depth+1);
        if(auto* M=CastField<FMapProperty>(Field))return VerifyProperty(M->KeyProp,Row->GetObjectField(TEXT("key")),IsProject,Depth+1)&&VerifyProperty(M->ValueProp,Row->GetObjectField(TEXT("value")),IsProject,Depth+1);
        if(auto* S=CastField<FSetProperty>(Field))return VerifyProperty(S->ElementProp,Row->GetObjectField(TEXT("element")),IsProject,Depth+1);
        if(auto* S=CastField<FStructProperty>(Field))return OriginalID(S->Struct)==Row->GetStringField(TEXT("struct_type"))||Fail(TEXT("Frozen struct identity mismatch"));
        if(auto* C=CastField<FClassProperty>(Field))return C->PropertyClass->GetPathName()==Row->GetStringField(TEXT("property_class_target"))&&C->MetaClass->GetPathName()==Row->GetStringField(TEXT("meta_class"));
        if(auto* C=CastField<FSoftClassProperty>(Field))return C->PropertyClass->GetPathName()==Row->GetStringField(TEXT("property_class_target"))&&C->MetaClass->GetPathName()==Row->GetStringField(TEXT("meta_class"));
        if(auto* O=CastField<FObjectPropertyBase>(Field))return O->PropertyClass->GetPathName()==Row->GetStringField(TEXT("object_target"));
        return true;
    }
}
bool VerifyFrozenReflection(FString& Error)
{
    if(!IsInGameThread()){Error=TEXT("Frozen V4 verification requires game thread");return false;}
    if(Attempted){Error=Failure;return Verified;}Attempted=true;
    const auto Reader=TJsonReaderFactory<>::Create(EmbeddedDescriptorJSON());
    if(!FJsonSerializer::Deserialize(Reader,Descriptor)||!Descriptor||!Descriptor->GetBoolField(TEXT("valid"))||Descriptor->GetIntegerField(TEXT("save_schema_version"))!=4||Descriptor->GetStringField(TEXT("schema_fingerprint"))!=SchemaFingerprintHex())Fail(TEXT("Embedded frozen V4 descriptor invalid"));
    if(Failure.IsEmpty())
    {
        RegisterGeneratedTypes(Types,OriginalIDs);
        for(const auto& E:Descriptor->GetArrayField(TEXT("enums"))){auto Row=E->AsObject();EnumDescriptions.Add(Row->GetStringField(TEXT("type_id")),Row);}
        for(const auto& Value:Descriptor->GetArrayField(TEXT("structs")))
        {
            auto Row=Value->AsObject();const FString ID=Row->GetStringField(TEXT("type_id"));UStruct* Type=Types.FindRef(ID);const bool IsProject=ID.StartsWith(TEXT("/Script/JamProject."));
            if(!Type)Type=FindObject<UStruct>(nullptr,*ID);
            if(!Type){Fail(TEXT("Native/DTO frozen type unavailable: ")+ID);break;}
            if(const auto* Script=Cast<UScriptStruct>(Type))
            {const uint64 OldFlags=FCString::Strtoui64(*Row->GetStringField(TEXT("struct_flags_hex")),nullptr,16);constexpr uint64 Mask=STRUCT_SerializeNative|STRUCT_Immutable|STRUCT_Atomic;if((Script->StructFlags&Mask)!=(OldFlags&Mask)){Fail(TEXT("Frozen native serializer flags changed: ")+ID);break;}}
            const auto& Fields=Row->GetArrayField(TEXT("ordered_fields_including_inherited"));FProperty* Field=Type->PropertyLink;
            for(int32 I=0;I<Fields.Num();++I){if(!Field||!VerifyProperty(Field,Fields[I]->AsObject(),IsProject,0)){if(Failure.IsEmpty())Fail(TEXT("Frozen field verification failed: ")+ID);break;}Field=Field->PropertyLinkNext;}
            if(!Failure.IsEmpty())break;if(Field){Fail(TEXT("Extra field in frozen DTO/native type: ")+ID);break;}
        }
    }
    Verified=Failure.IsEmpty();Error=Failure;return Verified;
}
bool IsFrozenProjectStruct(const UStruct* Type){return OriginalIDs.Contains(Type);}
FString GetOriginalTypeID(const UStruct* Type){return OriginalID(Type);}
const FJsonObject* FindOriginalProperty(const FProperty* Property){const auto* Row=FieldDescriptions.Find(Property);return Row?Row->Get():nullptr;}
const FJsonObject* FindOriginalEnum(const FString& ID){const auto* Row=EnumDescriptions.Find(ID);return Row?Row->Get():nullptr;}
bool VerifyStoredWireMetadata(const FBlake3Hash& Stored,const FPackageFileVersion& Package,const FEngineVersion& Engine,const FCustomVersionContainer& Versions,FString& Error)
{
    if(!VerifyFrozenReflection(Error))return false;
    FEngineVersion FrozenEngine;if(!FEngineVersion::Parse(Descriptor->GetStringField(TEXT("engine_version")),FrozenEngine)){Error=TEXT("Frozen engine version invalid");return false;}
    const FEngineVersion Current=FEngineVersion::Current();
    if(Package.FileVersionUE4!=Descriptor->GetIntegerField(TEXT("package_ue4"))||Package.FileVersionUE5!=Descriptor->GetIntegerField(TEXT("package_ue5"))||!(Package==GPackageFileUEVersion)
        ||Engine.GetMajor()!=FrozenEngine.GetMajor()||Engine.GetMinor()!=FrozenEngine.GetMinor()||Engine.GetPatch()!=FrozenEngine.GetPatch()
        ||Current.GetMajor()!=FrozenEngine.GetMajor()||Current.GetMinor()!=FrozenEngine.GetMinor()||Current.GetPatch()!=FrozenEngine.GetPatch())
    {Error=TEXT("V4 native engine/package serializer version unsupported");return false;}
#if !PLATFORM_LITTLE_ENDIAN
    Error=TEXT("Frozen V4 codec byte order unsupported");return false;
#else
    if(Versions.GetAllVersions().Num()>1024){Error=TEXT("Frozen V4 metadata version count");return false;}
    // Exact immutable v1 binding algorithm. Do not delegate to a future current
    // codec whose domain/version hashing implementation could have changed.
    FBlake3 Hash;const auto Number=[&](uint64 V){uint8 B[8];for(int32 I=0;I<8;++I)B[I]=static_cast<uint8>(V>>(I*8));Hash.Update(B,8);};
    const auto Text=[&](const FString& V){const FTCHARToUTF8 UTF8(*V);Number(UTF8.Length());if(UTF8.Length())Hash.Update(UTF8.Get(),UTF8.Length());};
    Text(TEXT("AFTERSHOCK.BinaryProperty.NativeHeaderBinding.v1"));FBlake3Hash Frozen;LexFromString(Frozen,SchemaFingerprintHex());Hash.Update(Frozen.GetBytes(),32);
    TArray<uint8> PackageBytes;FMemoryWriter VersionWriter(PackageBytes,true);FPackageFileVersion Copy=Package;VersionWriter<<Copy;Number(PackageBytes.Num());Hash.Update(PackageBytes.GetData(),PackageBytes.Num());
    Number(Engine.GetMajor());Number(Engine.GetMinor());Number(Engine.GetPatch());Number(Engine.GetChangelist());
    struct FVersion{FString Key;int32 Value=0;};TArray<FVersion> Ordered;
    for(const auto& V:Versions.GetAllVersions())Ordered.Add({V.Key.ToString(EGuidFormats::Digits),V.Version});Ordered.Sort([](const auto& A,const auto& B){return A.Key<B.Key;});Number(Ordered.Num());
    for(const auto& V:Ordered){Text(V.Key);Number(static_cast<uint64>(static_cast<int64>(V.Value)));}
    if(Stored!=Hash.Finalize()){Error=TEXT("Unknown V4 fingerprint/native metadata binding");return false;}return true;
#endif
}
}
