#include "AftershockLegacyV4Reader.h"
#include "AftershockLegacyV4Schema.h"
#include "AftershockLegacyV4Converter.h"
#include "Aftershock/Public/AftershockSaveGame.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Serialization/StructuredArchiveAdapters.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/StrongObjectPtr.h"
#include "HAL/PlatformTime.h"

namespace AftershockLegacyV4
{
namespace
{
    constexpr int32 MaximumPayloadBytes=256*1024*1024,MaximumNames=16384,MaximumDictionaryBytes=4*1024*1024;
    constexpr int32 MaximumContainerEntries=MaximumPayloadBytes; // min supported wire element is one byte
    constexpr int64 MaximumElements=MaximumPayloadBytes,MaximumDTOAllocationBytes=512ll*1024ll*1024ll;
    bool ReadUTF8(FArchive& Ar,FString& Out,int32 Maximum,bool AllowEmpty=false)
    {
        int32 N=0;Ar<<N;if(Ar.IsError()||N<(AllowEmpty?0:1)||N>Maximum||N>Ar.TotalSize()-Ar.Tell())return false;
        if(!N){Out.Empty();return true;}TArray<uint8,TInlineAllocator<1024>> Bytes;Bytes.SetNumUninitialized(N);Ar.Serialize(Bytes.GetData(),N);
        if(Ar.IsError()||Bytes.Contains(0))return false;const FUTF8ToTCHAR Decode(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),N);Out=FString(Decode.Length(),Decode.Get());const FTCHARToUTF8 Encode(*Out);
        return Encode.Length()==N&&FMemory::Memcmp(Encode.Get(),Bytes.GetData(),N)==0;
    }
    class FNamesArchive final:public FObjectAndNameAsStringProxyArchive
    {
    public:
        using FObjectAndNameAsStringProxyArchive::operator<<;
        FNamesArchive(FArchive& Inner,const TArray<FName>& InNames,int64 Start,int64 End):FObjectAndNameAsStringProxyArchive(Inner,false),Names(InNames),BodyStart(Start),BodyEnd(End)
        {ArMaxSerializeSize=MaximumPayloadBytes;SetWantBinaryPropertySerialization(true);ArIsSaveGame=false;if(IsSaving())for(int32 I=0;I<Names.Num();++I)Indices.Add(Names[I],I);}
        virtual FArchive& operator<<(FName& Name) override
        {
            uint32 Index=0;
            if(IsSaving()){const int32* Found=Indices.Find(Name);if(!Found){SetError();return *this;}Index=*Found;InnerArchive<<Index;}
            else{InnerArchive<<Index;if(InnerArchive.IsError()||Index>=static_cast<uint32>(Names.Num())){SetError();Name=NAME_None;return *this;}Name=Names[Index];}
            return *this;
        }
        virtual void Serialize(void* Data,int64 Count) override
        {const int64 P=InnerArchive.Tell();if(Count<0||P<BodyStart||P>BodyEnd||Count>BodyEnd-P){SetError();return;}InnerArchive.Serialize(Data,Count);}
        virtual void Seek(int64 P) override{if(P<BodyStart||P>BodyEnd){SetError();return;}InnerArchive.Seek(P);}
    private:const TArray<FName>& Names;TMap<FName,int32> Indices;int64 BodyStart,BodyEnd;
    };
    struct FBoundedBody
    {
        FString Error;int64 Elements=0,Allocation=0,FirstContainerCountOffset=INDEX_NONE;
        bool Fail(FArchive& Ar,const FString& What){if(Error.IsEmpty())Error=What;Ar.SetError();return false;}
        bool Reserve(FArchive& Ar,int32 Count,int64 Unit)
        {
            if(Count<0||Count>MaximumContainerEntries||Unit<0||Count>MaximumElements-Elements)return Fail(Ar,TEXT("V4 container entry/element limit"));
            const int64 Bytes=static_cast<int64>(Count)*Unit;
            if(Bytes>MaximumDTOAllocationBytes-Allocation)return Fail(Ar,TEXT("V4 decoded container memory budget"));
            // Every supported scalar element consumes at least one wire byte.
            // This lower bound prevents a huge declared count from reserving
            // memory before the bounded body has even one byte per entry.
            if(Count>Ar.TotalSize()-Ar.Tell())return Fail(Ar,TEXT("V4 count exceeds remaining wire bytes"));
            Elements+=Count;Allocation+=Bytes;return true;
        }
        bool Value(FStructuredArchive::FSlot Slot,FProperty* Field,void* Data,int32 Depth)
        {
            FArchive& Ar=Slot.GetUnderlyingArchive();if(Ar.IsError()||!Field||Depth>64)return Fail(Ar,TEXT("V4 decode property/depth limit"));
            FSerializedPropertyScope PropertyScope(Ar,Field);
            if(auto* A=CastField<FArrayProperty>(Field))
            {
                FScriptArrayHelper H(A,Data);int32 N=H.Num();const int64 CountAt=Ar.Tell();auto Array=Slot.EnterArray(N);if(Ar.IsLoading()&&FirstContainerCountOffset==INDEX_NONE)FirstContainerCountOffset=CountAt;
                if(Ar.IsLoading()){if(!Reserve(Ar,N,A->Inner->GetSize()))return false;H.EmptyAndAddValues(N);}
                for(int32 I=0;I<N;++I)if(!Value(Array.EnterElement(),A->Inner,H.GetRawPtr(I),Depth+1))return false;return !Ar.IsError();
            }
            if(auto* M=CastField<FMapProperty>(Field))
            {
                auto Record=Slot.EnterRecord();int32 Removed=0;Record.EnterArray(TEXT("KeysToRemove"),Removed);
                // Original V4 native snapshots serialize full values with no
                // defaults/overridable-property state: removed count is zero.
                if(Removed!=0)return Fail(Ar,TEXT("Unsupported V4 map delta/override body"));
                FScriptMapHelper H(M,Data);int32 N=H.Num();const int64 CountAt=Ar.Tell();auto Entries=Record.EnterArray(TEXT("Entries"),N);if(Ar.IsLoading()&&FirstContainerCountOffset==INDEX_NONE)FirstContainerCountOffset=CountAt;
                if(Ar.IsLoading())
                {
                    if(!Reserve(Ar,N,static_cast<int64>(M->KeyProp->GetSize())+M->ValueProp->GetSize()+32))return false;H.EmptyValues(N);
                    for(int32 I=0;I<N;++I){const int32 J=H.AddDefaultValue_Invalid_NeedsRehash();auto E=Entries.EnterElement().EnterRecord();if(!Value(E.EnterField(TEXT("Key")),M->KeyProp,H.GetKeyPtr(J),Depth+1)||!Value(E.EnterField(TEXT("Value")),M->ValueProp,H.GetValuePtr(J),Depth+1))return false;}H.Rehash();
                }
                else for(int32 I=0;I<H.GetMaxIndex();++I)if(H.IsValidIndex(I)){auto E=Entries.EnterElement().EnterRecord();if(!Value(E.EnterField(TEXT("Key")),M->KeyProp,H.GetKeyPtr(I),Depth+1)||!Value(E.EnterField(TEXT("Value")),M->ValueProp,H.GetValuePtr(I),Depth+1))return false;}
                return !Ar.IsError();
            }
            if(auto* S=CastField<FSetProperty>(Field))
            {
                auto Record=Slot.EnterRecord();int32 Removed=0;Record.EnterArray(TEXT("ElementsToRemove"),Removed);
                if(Removed!=0)return Fail(Ar,TEXT("Unsupported V4 set delta body"));FScriptSetHelper H(S,Data);int32 N=H.Num();const int64 CountAt=Ar.Tell();auto Entries=Record.EnterArray(TEXT("Elements"),N);if(Ar.IsLoading()&&FirstContainerCountOffset==INDEX_NONE)FirstContainerCountOffset=CountAt;
                if(Ar.IsLoading())
                {if(!Reserve(Ar,N,static_cast<int64>(S->ElementProp->GetSize())+32))return false;H.EmptyElements(N);for(int32 I=0;I<N;++I){const int32 J=H.AddDefaultValue_Invalid_NeedsRehash();if(!Value(Entries.EnterElement(),S->ElementProp,H.GetElementPtr(J),Depth+1))return false;}H.Rehash();}
                else for(int32 I=0;I<H.GetMaxIndex();++I)if(H.IsValidIndex(I))if(!Value(Entries.EnterElement(),S->ElementProp,H.GetElementPtr(I),Depth+1))return false;
                return !Ar.IsError();
            }
            if(auto* S=CastField<FStructProperty>(Field);S&&IsFrozenProjectStruct(S->Struct))return Struct(Slot,S->Struct,Data,Depth+1);
            // Only verified unchanged native-core structs and scalar leaf
            // serializers remain. FString/FText/object paths retain the bounded
            // archive and original explicit native/custom-version metadata.
            Field->SerializeItem(Slot,Data,nullptr);return !Ar.IsError();
        }
        bool Struct(FStructuredArchive::FSlot Slot,UStruct* Type,void* Data,int32 Depth)
        {
            FArchive& Ar=Slot.GetUnderlyingArchive();if(Depth>64)return Fail(Ar,TEXT("V4 struct nesting limit"));auto Stream=Slot.EnterStream();
            for(FProperty* Field=Type->PropertyLink;Field;Field=Field->PropertyLinkNext)
            {
                auto PropertyStream=Stream.EnterElement().EnterStream();if(!Field->ShouldSerializeValue(Ar))continue;
                for(int32 I=0;I<Field->ArrayDim;++I)if(!Value(PropertyStream.EnterElement(),Field,Field->ContainerPtrToValuePtr<void>(Data,I),Depth+1))return false;
            }
            return !Ar.IsError();
        }
        bool Root(FArchive& Ar,UObject& Object,bool& HadGuid,FGuid& ObjectGuid)
        {
            if(Ar.UseUnversionedPropertySerialization()||Ar.ArUseCustomPropertyList||Ar.IsTransacting()||Ar.GetPortFlags()!=0||Ar.IsSaveGame())return Fail(Ar,TEXT("Unsupported V4 archive mode"));
            FStructuredArchiveFromArchive Structured(Ar);auto Record=Structured.GetSlot().EnterRecord();
            if(!Struct(Record.EnterField(TEXT("Properties")),Object.GetClass(),&Object,0))return false;
            // UObject::Serialize appends this optional lazy-object GUID after
            // script properties. Preserve bytes without altering the global
            // object-GUID annotation registry during read-only migration.
            auto GuidSlot=Record.TryEnterField(TEXT("Guid"),HadGuid);HadGuid=GuidSlot.IsSet();if(GuidSlot.IsSet())GuidSlot.GetValue()<<ObjectGuid;
            return !Ar.IsError();
        }
    };
}
UAftershockSaveGame* DecodePayload(const TArray<uint8>& Payload,FString& Error,FReadDiagnostics* Diagnostics,bool RequireExactBodyRoundtrip)
{
    if(Diagnostics)*Diagnostics=FReadDiagnostics();const double Started=FPlatformTime::Seconds();
    if(!IsInGameThread()||Payload.Num()<48||Payload.Num()>MaximumPayloadBytes){Error=TEXT("V4 payload byte/game-thread bound");return nullptr;}
    FMemoryReader Reader(Payload,true);uint32 Magic=0,Version=0,Root=0;Reader<<Magic;Reader<<Version;Reader<<Root;
    if(Reader.IsError()||Magic!=0x4146494E||Version!=2||Root!=0x4153474D){Error=TEXT("Not an original V4 indexed binary payload2");return nullptr;}
    FBlake3Hash Stored;Reader<<Stored;FPackageFileVersion Package;Reader<<Package;
    uint16 Major=0,Minor=0,Patch=0;uint32 Changelist=0;Reader<<Major;Reader<<Minor;Reader<<Patch;Reader<<Changelist;FString Branch;
    if(!ReadUTF8(Reader,Branch,1024,true)){Error=TEXT("V4 engine branch UTF8 invalid");return nullptr;}FEngineVersion Engine(Major,Minor,Patch,Changelist,Branch);
    int32 CustomCount=0;Reader<<CustomCount;if(Reader.IsError()||CustomCount<0||CustomCount>1024||static_cast<int64>(CustomCount)*20>Reader.TotalSize()-Reader.Tell()){Error=TEXT("V4 custom-version count invalid");return nullptr;}
    FCustomVersionContainer Custom;TSet<FGuid> UniqueCustom;
    for(int32 I=0;I<CustomCount;++I){FGuid Key;int32 Value;Reader<<Key;Reader<<Value;if(Reader.IsError()||UniqueCustom.Contains(Key)){Error=TEXT("V4 custom-version duplicate/truncation");return nullptr;}UniqueCustom.Add(Key);Custom.SetVersion(Key,Value,NAME_None);}
    if(!VerifyStoredWireMetadata(Stored,Package,Engine,Custom,Error))return nullptr;
    int32 NameCount=0;Reader<<NameCount;const int64 DictionaryStart=Reader.Tell();
    if(Reader.IsError()||NameCount<1||NameCount>MaximumNames||static_cast<int64>(NameCount)*5>Reader.TotalSize()-Reader.Tell()){Error=TEXT("V4 name count invalid");return nullptr;}
    TArray<FName> Names;Names.Reserve(NameCount);TSet<FName> UniqueNames;
    for(int32 I=0;I<NameCount;++I){FString Text;if(!ReadUTF8(Reader,Text,1023)||Text.Len()>=NAME_SIZE||Reader.Tell()-DictionaryStart>MaximumDictionaryBytes){Error=TEXT("V4 name dictionary invalid");return nullptr;}const FName Name(*Text);if(UniqueNames.Contains(Name)||(I==0&&!Name.IsNone())){Error=TEXT("V4 duplicate dictionary/invalid NONE slot");return nullptr;}UniqueNames.Add(Name);Names.Add(Name);}
    int32 BodyCount=0;Reader<<BodyCount;const int64 BodyStart=Reader.Tell();
    if(Reader.IsError()||BodyCount<4||BodyCount>MaximumPayloadBytes||BodyCount!=Reader.TotalSize()-BodyStart){Error=TEXT("V4 body bounds/trailing bytes invalid");return nullptr;}
    Reader.SetUEVer(Package);Reader.SetEngineVer(Engine);Reader.SetCustomVersions(Custom);
    const double HeaderFinished=FPlatformTime::Seconds();if(Diagnostics)Diagnostics->HeaderMs=(HeaderFinished-Started)*1000.;
    TStrongObjectPtr<UObject> DTO(NewObject<UObject>(GetTransientPackage(),FrozenRootClass()));FNamesArchive Ar(Reader,Names,BodyStart,BodyStart+BodyCount);FBoundedBody Body;
    bool HadGuid=false;FGuid ObjectGuid;
    if(!Body.Root(Ar,*DTO,HadGuid,ObjectGuid)||Ar.IsError()||Reader.IsError()||Reader.Tell()!=BodyStart+BodyCount){Error=Body.Error.IsEmpty()?TEXT("Frozen V4 body deserialize/end mismatch"):Body.Error;return nullptr;}
    const double DtoFinished=FPlatformTime::Seconds();
    if(Diagnostics){Diagnostics->DtoDecodeMs=(DtoFinished-HeaderFinished)*1000.;Diagnostics->BodyBytes=BodyCount;Diagnostics->Names=NameCount;Diagnostics->FirstContainerCountOffset=Body.FirstContainerCountOffset;Diagnostics->Elements=Body.Elements;Diagnostics->AllocationBudgetUsed=Body.Allocation;}
    if(RequireExactBodyRoundtrip)
    {
        TArray<uint8> Written;FMemoryWriter Writer(Written,true);Writer.SetUEVer(Package);Writer.SetEngineVer(Engine);Writer.SetCustomVersions(Custom);
        FNamesArchive AuditAr(Writer,Names,0,MaximumPayloadBytes);FBoundedBody AuditBody;
        if(!AuditBody.Root(AuditAr,*DTO,HadGuid,ObjectGuid)||AuditAr.IsError()||Writer.IsError()||Written.Num()!=BodyCount||FMemory::Memcmp(Written.GetData(),Payload.GetData()+BodyStart,BodyCount)!=0)
        {Error=TEXT("Frozen V4 exact original body byte roundtrip failed");return nullptr;}
        if(Diagnostics){Diagnostics->bExactLegacyBodyRoundtrip=true;Diagnostics->AuditRoundtripMs=(FPlatformTime::Seconds()-DtoFinished)*1000.;}
    }
    const double ConvertStarted=FPlatformTime::Seconds();auto* Result=ConvertToCurrent(*DTO,Error);if(Diagnostics)Diagnostics->ConvertMs=(FPlatformTime::Seconds()-ConvertStarted)*1000.;return Result;
}
}
