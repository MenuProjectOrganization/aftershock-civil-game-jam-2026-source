#include "AftershockLegacyV4Converter.h"
#include "AftershockLegacyV4Schema.h"
#include "Aftershock/Public/AftershockSaveGame.h"
#include "Dom/JsonObject.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/StrongObjectPtr.h"
#include "Serialization/MemoryReader.h"

namespace AftershockLegacyV4
{
namespace
{
    struct FConverter
    {
        FConversionStats Stats;FString Error;int64 Elements=0;
        static constexpr int64 MaximumElements=256ll*1024ll*1024ll; // old raw-payload byte ceiling, min one byte/entry
        bool Fail(const FString& What){if(Error.IsEmpty())Error=What;return false;}
        bool Count(int32 N){Elements+=N;return N>=0&&Elements<=MaximumElements;}
        bool Enum(const FProperty* From,const void* Source,FProperty* To,void* Dest)
        {
            const FJsonObject* Row=FindOriginalProperty(From);FString EnumID;
            if(!Row||!Row->TryGetStringField(TEXT("enum_type"),EnumID))return Fail(TEXT("Enum wire field has no captured original type"));
            const auto* SourceName=CastField<FNameProperty>(From);if(!SourceName)return Fail(TEXT("Expected frozen enum FName wire value"));
            const FName Name=SourceName->GetPropertyValue(Source);const auto* Table=FindOriginalEnum(EnumID);
            if(!Table)return Fail(TEXT("Frozen enum table absent"));bool Found=false;int64 OldOrdinal=0;
            for(const auto& Value:Table->GetArrayField(TEXT("entries")))
            {const auto E=Value->AsObject();if(FName(*E->GetStringField(TEXT("name")))==Name){Found=true;OldOrdinal=FCString::Atoi64(*E->GetStringField(TEXT("numeric_value")));break;}}
            if(!Found)return Fail(TEXT("Unknown/invalid frozen enum entry: ")+Name.ToString());
            UEnum* Current=nullptr;FNumericProperty* Underlying=nullptr;
            if(auto* E=CastField<FEnumProperty>(To)){Current=E->GetEnum();Underlying=E->GetUnderlyingProperty();}
            else if(auto* B=CastField<FByteProperty>(To)){Current=B->Enum;Underlying=B;}
            if(!Current||!Underlying||Current->GetPathName()!=EnumID)return Fail(TEXT("Current enum identity changed: ")+To->GetPathName());
            const int32 CurrentIndex=Current->GetIndexByName(Name,EGetByNameFlags::CaseSensitive);
            if(CurrentIndex==INDEX_NONE||Current->GetValueByIndex(CurrentIndex)!=OldOrdinal)
                return Fail(TEXT("Old enum name/ordinal changed; refusing reinterpretation: ")+Name.ToString());
            if(!Underlying->CanHoldValue(OldOrdinal))return Fail(TEXT("Current enum underlying width cannot represent old value"));
            Underlying->SetIntPropertyValue(Dest,OldOrdinal);return true;
        }
        bool Value(const FProperty* From,const void* Source,FProperty* To,void* Dest,int32 Depth)
        {
            if(!From||!To||Depth>64)return Fail(TEXT("Conversion property/depth limit"));
            if(const auto* Row=FindOriginalProperty(From))
            {const FString Kind=Row->GetStringField(TEXT("property_class"));FString ID;Row->TryGetStringField(TEXT("enum_type"),ID);if(Kind==TEXT("EnumProperty")||(Kind==TEXT("ByteProperty")&&!ID.IsEmpty()&&ID!=TEXT("None")))return Enum(From,Source,To,Dest);}
            if(From->GetClass()!=To->GetClass())return Fail(TEXT("Current property category changed: ")+To->GetPathName());
            if(const auto* A=CastField<FArrayProperty>(From))
            {
                auto* B=CastFieldChecked<FArrayProperty>(To);FScriptArrayHelper SA(A,Source),DA(B,Dest);const int32 N=SA.Num();
                if(!Count(N))return Fail(TEXT("Converted array element budget"));DA.EmptyAndAddValues(N);Stats.ArrayElements+=N;
                for(int32 I=0;I<N;++I)if(!Value(A->Inner,SA.GetRawPtr(I),B->Inner,DA.GetRawPtr(I),Depth+1))return false;return true;
            }
            if(const auto* A=CastField<FMapProperty>(From))
            {
                auto* B=CastFieldChecked<FMapProperty>(To);FScriptMapHelper SM(A,Source),DM(B,Dest);const int32 N=SM.Num();
                if(!Count(N))return Fail(TEXT("Converted map pair budget"));DM.EmptyValues(N);Stats.MapPairs+=N;
                for(int32 I=0;I<SM.GetMaxIndex();++I)if(SM.IsValidIndex(I))
                {const int32 J=DM.AddDefaultValue_Invalid_NeedsRehash();if(!Value(A->KeyProp,SM.GetKeyPtr(I),B->KeyProp,DM.GetKeyPtr(J),Depth+1)||!Value(A->ValueProp,SM.GetValuePtr(I),B->ValueProp,DM.GetValuePtr(J),Depth+1))return false;}
                DM.Rehash();return true;
            }
            if(const auto* A=CastField<FSetProperty>(From))
            {
                auto* B=CastFieldChecked<FSetProperty>(To);FScriptSetHelper SS(A,Source),DS(B,Dest);const int32 N=SS.Num();
                if(!Count(N))return Fail(TEXT("Converted set element budget"));DS.EmptyElements(N);Stats.SetElements+=N;
                for(int32 I=0;I<SS.GetMaxIndex();++I)if(SS.IsValidIndex(I))
                {
                    const int32 J=DS.AddDefaultValue_Invalid_NeedsRehash();
                    if(!Value(A->ElementProp,SS.GetElementPtr(I),B->ElementProp,DS.GetElementPtr(J),Depth+1))return false;
                }
                DS.Rehash();return true;
            }
            if(const auto* A=CastField<FStructProperty>(From))
            {
                auto* B=CastFieldChecked<FStructProperty>(To);
                if(GetOriginalTypeID(A->Struct)!=B->Struct->GetPathName())return Fail(TEXT("Current struct identity changed: ")+B->Struct->GetPathName());
                if(IsFrozenProjectStruct(A->Struct))return Struct(A->Struct,Source,B->Struct,Dest,Depth+1);
                if(A->Struct!=B->Struct)return Fail(TEXT("Native shared struct identity mismatch"));
                B->CopySingleValue(Dest,Source);return true;
            }
            if(const auto* A=CastField<FBoolProperty>(From))
            {CastFieldChecked<FBoolProperty>(To)->SetPropertyValue(Dest,A->GetPropertyValue(Source));return true;}
            // CopySingleValue is only allowed between matching primitive/native
            // property categories. Cross-layout project structs/containers were
            // recursively handled above; C++ offsets/sizes are never guessed.
            if(const auto* A=CastField<FObjectPropertyBase>(From))
            {const auto* B=CastFieldChecked<FObjectPropertyBase>(To);if(A->PropertyClass!=B->PropertyClass)return Fail(TEXT("Object target class changed"));}
            To->CopySingleValue(Dest,Source);return true;
        }
        bool Struct(const UStruct* From,const void* Source,const UStruct* To,void* Dest,int32 Depth)
        {
            if(!From||!To||Depth>64)return Fail(TEXT("Conversion struct/depth limit"));
            TSet<FName> Copied;
            TArray<uint8> Empty;FMemoryReader Filter(Empty,true);Filter.SetWantBinaryPropertySerialization(true);Filter.ArIsSaveGame=false;
            for(FProperty* Field=From->PropertyLink;Field;Field=Field->PropertyLinkNext)
            {
                if(!Field->ShouldSerializeValue(Filter))continue;
                FProperty* Target=FindFProperty<FProperty>(To,Field->GetFName());
                if(!Target||Target->ArrayDim!=Field->ArrayDim)return Fail(TEXT("Current field removed/dimension changed: ")+Field->GetPathName());
                for(int32 I=0;I<Field->ArrayDim;++I)
                {if(!Value(Field,Field->ContainerPtrToValuePtr<void>(Source,I),Target,Target->ContainerPtrToValuePtr<void>(Dest,I),Depth+1))return false;++Stats.Properties;}
                Copied.Add(Field->GetFName());
            }
            for(FProperty* Field=To->PropertyLink;Field;Field=Field->PropertyLinkNext)if(!Copied.Contains(Field->GetFName()))++Stats.CurrentOnlyFieldsKept;
            return true;
        }
    };
}
UAftershockSaveGame* ConvertToCurrent(const UObject& Frozen,FString& Error,FConversionStats* Stats)
{
    if(Stats)*Stats=FConversionStats();
    if(!IsInGameThread()||Frozen.GetClass()!=FrozenRootClass()||!VerifyFrozenReflection(Error)){if(Error.IsEmpty())Error=TEXT("Frozen root/game-thread contract");return nullptr;}
    TStrongObjectPtr<UAftershockSaveGame> Current(NewObject<UAftershockSaveGame>(GetTransientPackage()));FConverter C;
    if(!C.Struct(Frozen.GetClass(),&Frozen,Current->GetClass(),Current.Get(),0)){Error=C.Error;return nullptr;}
    if(Current->SchemaVersion!=4){Error=TEXT("Legacy decoded schema version is not4");return nullptr;}
    // Keep4 here. Parent's explicit ValidateSnapshot V4->V5 migration initializes
    // new physical-state fields and only THEN advances SchemaVersion to5.
    if(Stats)*Stats=C.Stats;Error.Empty();return Current.Get();
}
}
