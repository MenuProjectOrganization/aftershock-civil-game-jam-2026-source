#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "AftershockPhysicalLifecycle.h"
#include "Aftershock/Public/AftershockSaveGame.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockDisasterDirectorSubsystem.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/AftershockRegionRules.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/CityPresentationSubsystem.h"
#include "Aftershock/Public/CoastalPresentationSubsystem.h"
#include "Aftershock/Public/DisasterPresentationSubsystem.h"
#include "Aftershock/Quiz/QuizSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/Compression.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/EngineVersion.h"
#include "Serialization/CustomVersion.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/ObjectVersion.h"
#include "UObject/UnrealType.h"
#include "UObject/StrongObjectPtr.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "Aftershock/Public/AftershockScaledChaos.h"
#include "Aftershock/Public/AftershockBuildingCatalog.h"
#include "AftershockSaveBinarySchema.h"
#include "AftershockLegacyV4Reader.h"
#include "AftershockLegacyV4Schema.h"
#include "AftershockPhysicalReplay.h"
#include "AftershockMassMovementVisual.h"
#include "AftershockHydraulicTerrain.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "UObject/StrongObjectPtr.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#else
#include <cstdio>
#endif

enum class EAftershockSaveByteFailure:uint8 {None,Compression,Directory,TempWrite,PreviousChanged,Backup,Commit};
// The worker owns only immutable byte buffers and plain metadata. It never
// captures a world/snapshot, resolves names/objects, validates UObjects or broadcasts.
struct FAftershockAsyncSaveJob
{
 FString Slot,Path,ExpectedPreviousDigest,CommittedDigest,BackupDigest;
 TArray<uint8> Payload,PreviousBytes;
 FGuid SessionID;
 bool bHadPrevious=false,bPreviousValidated=false,bSuccess=false;
 EAftershockSaveByteFailure Failure=EAftershockSaveByteFailure::None;
 double CaptureMs=0,ValidateMs=0,SerializeMs=0,PreviousRevisionMs=0,CompressMs=0,TempVerifyMs=0,BackupMs=0,CommitMs=0,WorkerMs=0;
 TAtomic<bool> bWorkerFinished{false};
 TFuture<void> Worker;
};

namespace
{
 constexpr uint32 SaveMagic=0x41465348; // AFSH envelope; typed payload has its own schema.
 constexpr int32 EnvelopeVersion=3;
 constexpr int32 MaximumSaveBytes=64*1024*1024;
 constexpr int32 MaximumUncompressedSaveBytes=256*1024*1024;
 void ReleaseSnapshotPayload(UAftershockSaveGame& S)
 {
  S.Terrain={};S.Buildings={};S.Rubble.Empty();S.DisasterVisual={};S.Physics={};S.Economy={};S.CasualtyLedger={};
  S.CityLife={};S.Transport={};S.Scenario={};S.Director={};S.Crowd={};S.Quiz={};S.Campaign={};
 }
 struct FFinitePropertyPlan
 {
  enum class EKind:uint8 { Number,Struct,Array,Map,Set };
  const FProperty* Property=nullptr;
  const FNumericProperty* Number=nullptr;
  EKind Kind=EKind::Number;
  TArray<FFinitePropertyPlan> Children;
  bool Compile(const FProperty* InProperty)
  {
   Property=InProperty;
   if(const auto* Numeric=CastField<FNumericProperty>(Property))
   {Number=Numeric;return Number->IsFloatingPoint();}
   const auto Add=[this](const FProperty* Child)
   {FFinitePropertyPlan Plan;if(Plan.Compile(Child))Children.Add(MoveTemp(Plan));};
   if(const auto* Struct=CastField<FStructProperty>(Property))
   {Kind=EKind::Struct;for(TFieldIterator<FProperty> P(Struct->Struct);P;++P)Add(*P);}
   else if(const auto* Array=CastField<FArrayProperty>(Property)){Kind=EKind::Array;Add(Array->Inner);}
   else if(const auto* Map=CastField<FMapProperty>(Property)){Kind=EKind::Map;Add(Map->KeyProp);Add(Map->ValueProp);}
   else if(const auto* Set=CastField<FSetProperty>(Property)){Kind=EKind::Set;Add(Set->ElementProp);}
   return !Children.IsEmpty();
  }
  bool Evaluate(const void* Value) const
  {
   switch(Kind)
   {
    case EKind::Number:return FMath::IsFinite(Number->GetFloatingPointPropertyValue(Value));
    case EKind::Struct:
     for(const auto& Child:Children)if(!Child.Evaluate(Child.Property->ContainerPtrToValuePtr<void>(Value)))return false;
     break;
    case EKind::Array:
    {FScriptArrayHelper H(static_cast<const FArrayProperty*>(Property),Value);for(int32 I=0;I<H.Num();++I)if(!Children[0].Evaluate(H.GetRawPtr(I)))return false;break;}
    case EKind::Map:
    {const auto* Map=static_cast<const FMapProperty*>(Property);FScriptMapHelper H(Map,Value);for(int32 I=0;I<H.GetMaxIndex();++I)if(H.IsValidIndex(I))for(const auto& Child:Children)if(!Child.Evaluate(Child.Property==Map->KeyProp?H.GetKeyPtr(I):H.GetValuePtr(I)))return false;break;}
    case EKind::Set:
    {FScriptSetHelper H(static_cast<const FSetProperty*>(Property),Value);for(int32 I=0;I<H.GetMaxIndex();++I)if(H.IsValidIndex(I)&&!Children[0].Evaluate(H.GetElementPtr(I)))return false;break;}
   }
   return true;
  }
 };
 bool ValidSlot(const FString& Slot)
 {return Slot==TEXT("Manual")||Slot==TEXT("Auto0")||Slot==TEXT("Auto1")||Slot==TEXT("Auto2")||Slot==TEXT("Audit");}
 bool Decode(const TArray<uint8>& Bytes,TArray<uint8>& Payload,int32* OutVersion=nullptr)
 {
  constexpr int32 HeaderBytes=sizeof(uint32)+sizeof(int32)*2+FSHA1::DigestSize;
  if(Bytes.Num()<HeaderBytes||Bytes.Num()>MaximumSaveBytes)return false;
  FMemoryReader Reader(Bytes,true);uint32 Magic=0;int32 Version=0,Count=0;uint8 Digest[FSHA1::DigestSize];
  Reader<<Magic;Reader<<Version;Reader<<Count;
  if(Reader.IsError()||Magic!=SaveMagic||(Version!=1&&Version!=2&&Version!=EnvelopeVersion)||Count<1)return false;
  int32 UncompressedSize=Count;if(Version>=2)Reader<<UncompressedSize;
  Reader.Serialize(Digest,sizeof(Digest));
  const int32 ActualHeaderBytes=HeaderBytes+(Version>=2?sizeof(int32):0);
  if(Reader.IsError()||Count!=Bytes.Num()-ActualHeaderBytes||UncompressedSize<1||UncompressedSize>MaximumUncompressedSaveBytes)return false;
  Payload.SetNumUninitialized(UncompressedSize);
  if(Version==1)Reader.Serialize(Payload.GetData(),Count);
  else if(!FCompression::UncompressMemory(NAME_Zlib,Payload.GetData(),static_cast<int64>(UncompressedSize),Bytes.GetData()+ActualHeaderBytes,static_cast<int64>(Count)))return false;
  uint8 Actual[FSHA1::DigestSize];FSHA1::HashBuffer(Payload.GetData(),Payload.Num(),Actual);
  const bool Valid=!Reader.IsError()&&FMemory::Memcmp(Digest,Actual,sizeof(Digest))==0;
  if(Valid&&OutVersion)*OutVersion=Version;return Valid;
 }
 bool ReadBytes(const FString& Path,TArray<uint8>& Bytes)
 {
  const int64 Size=IFileManager::Get().FileSize(*Path);
  return Size>0&&Size<=MaximumSaveBytes&&FFileHelper::LoadFileToArray(Bytes,*Path);
 }
 bool ReplaceSameDirectory(const FString& Source,const FString& Destination)
 {
  if(FPaths::GetPath(Source)!=FPaths::GetPath(Destination))return false;
#if PLATFORM_WINDOWS
  // IFileManager::Move deletes the destination first on Windows. The native
  // same-volume rename preserves the previous slot until replacement commits.
  return ::MoveFileExW(*Source,*Destination,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
  return std::rename(TCHAR_TO_UTF8(*Source),TCHAR_TO_UTF8(*Destination))==0;
#endif
 }
 bool WriteFlushed(const FString& Path,const TArray<uint8>& Bytes)
 {
  double OpenMs=0,WriteMs=0,FlushMs=0,CloseMs=0,ReadMs=0,DecodeMs=0;bool Success=false;
  ON_SCOPE_EXIT{UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SAVE_BYTE_IO openMs=%.3f writeMs=%.3f flushMs=%.3f closeMs=%.3f readMs=%.3f decodeHashMs=%.3f bytes=%d success=%d"),OpenMs,WriteMs,FlushMs,CloseMs,ReadMs,DecodeMs,Bytes.Num(),Success);};
  IPlatformFile& Files=FPlatformFileManager::Get().GetPlatformFile();double Started=FPlatformTime::Seconds();
  TUniquePtr<IFileHandle> File(Files.OpenWrite(*Path,false,false));OpenMs=(FPlatformTime::Seconds()-Started)*1000.;if(!File)return false;
  Started=FPlatformTime::Seconds();const bool Wrote=File->Write(Bytes.GetData(),Bytes.Num());WriteMs=(FPlatformTime::Seconds()-Started)*1000.;if(!Wrote)return false;
  Started=FPlatformTime::Seconds();const bool Flushed=File->Flush(true);FlushMs=(FPlatformTime::Seconds()-Started)*1000.;if(!Flushed)return false;
  Started=FPlatformTime::Seconds();File.Reset();CloseMs=(FPlatformTime::Seconds()-Started)*1000.;
  TArray<uint8> Verified,Payload;Started=FPlatformTime::Seconds();const bool Read=ReadBytes(Path,Verified);ReadMs=(FPlatformTime::Seconds()-Started)*1000.;if(!Read)return false;
  Started=FPlatformTime::Seconds();Success=Verified==Bytes&&Decode(Verified,Payload);DecodeMs=(FPlatformTime::Seconds()-Started)*1000.;return Success;
 }
 constexpr uint32 IndexedPayloadMagic=0x4146494E; // AFIN, original AFTERSHOCK tagged payload.
 constexpr uint32 IndexedPayloadVersion=AftershockSaveBinarySchema::PayloadVersion,IndexedRootClass=0x4153474D; // fixed UAftershockSaveGame only.
 constexpr int32 MaximumIndexedNames=16384,MaximumIndexedNameBytes=1023;
 constexpr int32 MaximumDictionaryBytes=4*1024*1024,MaximumCustomVersions=1024;
 bool WriteBoundedUtf8(FArchive& Ar,const FString& Text,int32 Maximum)
 {
  const FTCHARToUTF8 Utf8(*Text);int32 Count=Utf8.Length();if(Count<0||Count>Maximum)return false;
  Ar<<Count;if(Count)Ar.Serialize(const_cast<ANSICHAR*>(Utf8.Get()),Count);return !Ar.IsError();
 }
 bool ReadBoundedUtf8(FArchive& Ar,FString& Text,int32 Maximum,bool bAllowEmpty=false)
 {
  int32 Count=0;Ar<<Count;
  if(Ar.IsError()||Count<(bAllowEmpty?0:1)||Count>Maximum||Count>Ar.TotalSize()-Ar.Tell())return false;
  if(!Count){Text.Empty();return true;}
  TArray<uint8,TInlineAllocator<1024>> Bytes;Bytes.SetNumUninitialized(Count);Ar.Serialize(Bytes.GetData(),Count);
  if(Ar.IsError()||Bytes.Contains(0))return false;
  const FUTF8ToTCHAR Decoded(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),Count);
  Text=FString(Decoded.Length(),Decoded.Get());const FTCHARToUTF8 Roundtrip(*Text);
  // Reject malformed/overlong UTF8 rather than silently replacing bad bytes.
  return Roundtrip.Length()==Count&&FMemory::Memcmp(Roundtrip.Get(),Bytes.GetData(),Count)==0;
 }
 class FIndexedNameSaveArchive final:public FObjectAndNameAsStringProxyArchive
 {
 public:
  using FObjectAndNameAsStringProxyArchive::operator<<;
  FIndexedNameSaveArchive(FArchive& Inner,TArray<FName>& InNames,bool bLoading,int64 InBodyStart=0,int64 InBodyEnd=MaximumUncompressedSaveBytes)
   :FObjectAndNameAsStringProxyArchive(Inner,false),Names(InNames),BodyStart(InBodyStart),BodyEnd(InBodyEnd)
  {
   ArMaxSerializeSize=MaximumUncompressedSaveBytes;
   if(!bLoading)for(int32 I=0;I<Names.Num();++I)Indices.Add(Names[I],I);
  }
  virtual FArchive& operator<<(FName& Name) override
  {
   uint32 Index=0;
   if(IsSaving())
   {
    if(const int32* Existing=Indices.Find(Name))Index=static_cast<uint32>(*Existing);
    else
    {
     if(Names.Num()>=MaximumIndexedNames){SetError();return *this;}
     const FString Text=Name.ToString();const FTCHARToUTF8 Utf8(*Text);
     if(Text.Len()>=NAME_SIZE||Utf8.Length()<1||Utf8.Length()>MaximumIndexedNameBytes){SetError();return *this;}
     Index=Names.Num();Names.Add(Name);Indices.Add(Name,Index);
    }
    InnerArchive<<Index;
   }
   else
   {
    InnerArchive<<Index;
    if(InnerArchive.IsError()||Index>=static_cast<uint32>(Names.Num())){SetError();Name=NAME_None;return *this;}
    Name=Names[Index];
   }
   return *this;
  }
  virtual void Serialize(void* Data,int64 Count) override
  {
   const int64 Position=InnerArchive.Tell();
   if(Count<0||Position<BodyStart||Position>BodyEnd||Count>BodyEnd-Position){SetError();return;}
   InnerArchive.Serialize(Data,Count);
  }
  virtual void Seek(int64 Position) override
  {
   if(Position<BodyStart||Position>BodyEnd){SetError();return;}InnerArchive.Seek(Position);
  }
 private:
  TArray<FName>& Names;
  TMap<FName,int32> Indices;
  int64 BodyStart,BodyEnd;
 };
 FString EnvelopeDigest(const TArray<uint8>& Bytes)
 {uint8 Digest[FSHA1::DigestSize];FSHA1::HashBuffer(Bytes.GetData(),Bytes.Num(),Digest);return BytesToHex(Digest,sizeof(Digest));}
 bool IsFixedSaveCandidate(const FString& Path)
 {
  for(const TCHAR* Slot:{TEXT("Manual"),TEXT("Auto0"),TEXT("Auto1"),TEXT("Auto2"),TEXT("Audit")})
  {const FString Base=UAftershockSaveSubsystem::SlotPath(Slot);if(Path==Base||Path==Base+TEXT(".bak")||Path==Base+TEXT(".tmp"))return true;}
  return false;
 }

 void RunAutosaveByteWorker(FAftershockAsyncSaveJob& Job)
 {
  const double WorkerStarted=FPlatformTime::Seconds();ON_SCOPE_EXIT{Job.Payload.Empty();Job.PreviousBytes.Empty();Job.WorkerMs=(FPlatformTime::Seconds()-WorkerStarted)*1000.;};
  const auto PreviousUnchanged=[&Job]()
  {
   IPlatformFile& Files=FPlatformFileManager::Get().GetPlatformFile();
   if(!Job.bHadPrevious)return !Files.FileExists(*Job.Path);
   TArray<uint8> Current;return ReadBytes(Job.Path,Current)&&EnvelopeDigest(Current)==Job.ExpectedPreviousDigest;
  };
  // NAME_Zlib is an existing engine constant; no name construction/reflection,
  // FText, UObject serialization or asset access occurs in this byte-only path.
  double Started=FPlatformTime::Seconds();int32 Size=FCompression::CompressMemoryBound(NAME_Zlib,Job.Payload.Num());
  TArray<uint8> Compressed;Compressed.SetNumUninitialized(Size);
  if(!FCompression::CompressMemory(NAME_Zlib,Compressed.GetData(),Size,Job.Payload.GetData(),Job.Payload.Num(),COMPRESS_BiasSpeed)||Size>MaximumSaveBytes-64)
  {Job.Failure=EAftershockSaveByteFailure::Compression;return;}
  Compressed.SetNum(Size,EAllowShrinking::No);TArray<uint8> Envelope;FMemoryWriter Writer(Envelope,true);
  uint32 Magic=SaveMagic;int32 Version=EnvelopeVersion,UncompressedSize=Job.Payload.Num();uint8 Digest[FSHA1::DigestSize];
  FSHA1::HashBuffer(Job.Payload.GetData(),Job.Payload.Num(),Digest);Writer<<Magic;Writer<<Version;Writer<<Size;Writer<<UncompressedSize;
  Writer.Serialize(Digest,sizeof(Digest));Writer.Serialize(Compressed.GetData(),Compressed.Num());Writer.Close();
  if(Writer.IsError()||Envelope.Num()>MaximumSaveBytes){Job.Failure=EAftershockSaveByteFailure::Compression;return;}
  Job.CompressMs=(FPlatformTime::Seconds()-Started)*1000.;Job.Payload.Empty();Compressed.Empty();
  IPlatformFile& Files=FPlatformFileManager::Get().GetPlatformFile();const FString Temp=Job.Path+TEXT(".tmp"),Backup=Job.Path+TEXT(".bak");
  if(!Files.CreateDirectoryTree(*FPaths::GetPath(Job.Path))){Job.Failure=EAftershockSaveByteFailure::Directory;return;}
  if(!PreviousUnchanged()){Job.Failure=EAftershockSaveByteFailure::PreviousChanged;return;}
  Started=FPlatformTime::Seconds();const bool Wrote=WriteFlushed(Temp,Envelope);Job.TempVerifyMs=(FPlatformTime::Seconds()-Started)*1000.;
  if(!Wrote){Job.Failure=EAftershockSaveByteFailure::TempWrite;return;}
  if(!PreviousUnchanged()){Job.Failure=EAftershockSaveByteFailure::PreviousChanged;return;}
  Started=FPlatformTime::Seconds();
  if(Job.bPreviousValidated)
  {
   const FString BackupTemp=Backup+TEXT(".tmp");
   if(!WriteFlushed(BackupTemp,Job.PreviousBytes)){Job.Failure=EAftershockSaveByteFailure::Backup;return;}
   // Recheck after a potentially slow durable flush, before promoting any backup.
   if(!PreviousUnchanged()){Job.Failure=EAftershockSaveByteFailure::PreviousChanged;return;}
   if(!ReplaceSameDirectory(BackupTemp,Backup)){Job.Failure=EAftershockSaveByteFailure::Backup;return;}
   Job.BackupDigest=EnvelopeDigest(Job.PreviousBytes);
  }
  Job.BackupMs=(FPlatformTime::Seconds()-Started)*1000.;Started=FPlatformTime::Seconds();
  if(!PreviousUnchanged()){Job.Failure=EAftershockSaveByteFailure::PreviousChanged;return;}
  if(!ReplaceSameDirectory(Temp,Job.Path)){Job.Failure=EAftershockSaveByteFailure::Commit;return;}
  Job.CommittedDigest=EnvelopeDigest(Envelope);Job.CommitMs=(FPlatformTime::Seconds()-Started)*1000.;Job.bSuccess=true;
 }
}

static bool EncodeIndexedPayloadVersion(UAftershockSaveGame& Saved,TArray<uint8>& Payload,bool bBinary)
{
 double SchemaMs=0,BodyMs=0,DictionaryMs=0;int32 BodyBytes=0,NameCountForLog=0;bool Succeeded=false;
 ON_SCOPE_EXIT{UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SAVE_ENCODE_STAGES binary=%d schemaBindMs=%.3f bodySerializeMs=%.3f dictionaryAssemblyMs=%.3f names=%d bodyBytes=%d payloadBytes=%d success=%d"),bBinary,SchemaMs,BodyMs,DictionaryMs,NameCountForLog,BodyBytes,Payload.Num(),Succeeded);};
 Payload.Reset();if(!IsInGameThread()||Saved.GetClass()!=UAftershockSaveGame::StaticClass())return false;
 double StageStarted=FPlatformTime::Seconds();
 AftershockSaveBinarySchema::FSchema Schema;
 if(bBinary&&!AftershockSaveBinarySchema::Build(Saved.GetClass(),Schema))return false;
 FPackageFileVersion PackageVersion=GPackageFileUEVersion;FEngineVersion EngineVersion=FEngineVersion::Current();
 const FCustomVersionContainer CustomVersions=FCurrentCustomVersions::GetAll();
 if(CustomVersions.GetAllVersions().Num()>MaximumCustomVersions)return false;
 FBlake3Hash WireFingerprint;
 if(bBinary&&!AftershockSaveBinarySchema::BindMetadata(Schema,PackageVersion,EngineVersion,CustomVersions,WireFingerprint))return false;
 SchemaMs=(FPlatformTime::Seconds()-StageStarted)*1000.;StageStarted=FPlatformTime::Seconds();
 TArray<uint8> Body;FMemoryWriter BodyWriter(Body,true);BodyWriter.SetUEVer(PackageVersion);BodyWriter.SetEngineVer(EngineVersion);BodyWriter.SetCustomVersions(CustomVersions);
 TArray<FName> Names;Names.Add(NAME_None);FIndexedNameSaveArchive ObjectArchive(BodyWriter,Names,false);
 ObjectArchive.SetWantBinaryPropertySerialization(bBinary);ObjectArchive.ArIsSaveGame=false;
 Saved.Serialize(ObjectArchive);
 BodyMs=(FPlatformTime::Seconds()-StageStarted)*1000.;BodyBytes=Body.Num();NameCountForLog=Names.Num();StageStarted=FPlatformTime::Seconds();
 if(ObjectArchive.IsError()||BodyWriter.IsError()||Body.IsEmpty()||Body.Num()>MaximumUncompressedSaveBytes)return false;
 // Version1 remains tagged and can skip unknown fields. Version2 removes tags
 // only after an exact runtime wire-schema fingerprint has been established.
 FMemoryWriter Writer(Payload,true);uint32 Magic=IndexedPayloadMagic,Version=bBinary?IndexedPayloadVersion:1,Root=IndexedRootClass;
 Writer<<Magic;Writer<<Version;Writer<<Root;if(bBinary)Writer<<WireFingerprint;Writer<<PackageVersion;
 uint16 Major=EngineVersion.GetMajor(),Minor=EngineVersion.GetMinor(),Patch=EngineVersion.GetPatch();uint32 Changelist=EngineVersion.GetChangelist();
 Writer<<Major;Writer<<Minor;Writer<<Patch;Writer<<Changelist;
 if(!WriteBoundedUtf8(Writer,EngineVersion.GetBranchDescriptor(),1024))return false;
 int32 CustomCount=CustomVersions.GetAllVersions().Num();Writer<<CustomCount;
 for(const auto& Custom:CustomVersions.GetAllVersions()){FGuid Key=Custom.Key;int32 Value=Custom.Version;Writer<<Key;Writer<<Value;}
 int32 NameCount=Names.Num();Writer<<NameCount;const int64 DictionaryStart=Writer.Tell();
 for(FName Name:Names)if(!WriteBoundedUtf8(Writer,Name.ToString(),MaximumIndexedNameBytes)||Writer.Tell()-DictionaryStart>MaximumDictionaryBytes)return false;
 int32 BodyCount=Body.Num();Writer<<BodyCount;Writer.Serialize(Body.GetData(),BodyCount);Writer.Close();
 DictionaryMs=(FPlatformTime::Seconds()-StageStarted)*1000.;Succeeded=!Writer.IsError()&&Payload.Num()<=MaximumUncompressedSaveBytes;return Succeeded;
}
bool UAftershockSaveSubsystem::EncodeIndexedPayload(UAftershockSaveGame& Saved,TArray<uint8>& Payload)
{return EncodeIndexedPayloadVersion(Saved,Payload,true);}
bool AftershockSaveBinarySchema::EncodeLegacyTaggedFixture(UAftershockSaveGame& Saved,TArray<uint8>& Payload)
{return EncodeIndexedPayloadVersion(Saved,Payload,false);}
UAftershockSaveGame* UAftershockSaveSubsystem::DecodeIndexedPayload(const TArray<uint8>& Payload)
{
 if(!IsInGameThread()||Payload.Num()<48||Payload.Num()>MaximumUncompressedSaveBytes)return nullptr;
 FMemoryReader Reader(Payload,true);uint32 Magic=0,Version=0,Root=0;Reader<<Magic;Reader<<Version;Reader<<Root;
 if(Reader.IsError()||Magic!=IndexedPayloadMagic||(Version!=1&&Version!=IndexedPayloadVersion)||Root!=IndexedRootClass)return nullptr;
 const bool bBinary=Version==IndexedPayloadVersion;
 FBlake3Hash Stored;if(bBinary)Reader<<Stored;
 FPackageFileVersion PackageVersion;Reader<<PackageVersion;
 uint16 Major=0,Minor=0,Patch=0;uint32 Changelist=0;Reader<<Major;Reader<<Minor;Reader<<Patch;Reader<<Changelist;FString Branch;
 if(Reader.IsError()||!GPackageFileUEVersion.IsCompatible(PackageVersion)||!PackageVersion.IsCompatible(GOldestLoadablePackageFileUEVersion)
  ||Major!=FEngineVersion::Current().GetMajor()||Minor>FEngineVersion::Current().GetMinor()||!ReadBoundedUtf8(Reader,Branch,1024,true))return nullptr;
 if(bBinary&&(!(PackageVersion==GPackageFileUEVersion)||Major!=FEngineVersion::Current().GetMajor()
  ||Minor!=FEngineVersion::Current().GetMinor()||Patch!=FEngineVersion::Current().GetPatch()))return nullptr;
 FEngineVersion EngineVersion(Major,Minor,Patch,Changelist,Branch);int32 CustomCount=0;Reader<<CustomCount;
 if(Reader.IsError()||CustomCount<0||CustomCount>MaximumCustomVersions||static_cast<int64>(CustomCount)*20>Reader.TotalSize()-Reader.Tell())return nullptr;
 FCustomVersionContainer CustomVersions;TSet<FGuid> CustomKeys;
 for(int32 I=0;I<CustomCount;++I){FGuid Key;int32 Value=0;Reader<<Key;Reader<<Value;if(Reader.IsError()||CustomKeys.Contains(Key))return nullptr;CustomKeys.Add(Key);CustomVersions.SetVersion(Key,Value,NAME_None);}
 if(bBinary)
 {
  AftershockSaveBinarySchema::FSchema Expected;FBlake3Hash ExpectedWire;
  if(!AftershockSaveBinarySchema::Build(UAftershockSaveGame::StaticClass(),Expected)
   ||!AftershockSaveBinarySchema::BindMetadata(Expected,PackageVersion,EngineVersion,CustomVersions,ExpectedWire))return nullptr;
  if(Stored!=ExpectedWire)
  {
   // A different current schema never authorizes current SerializeBin on an
   // older positional body. Only the exact captured V4 binding may enter the
   // frozen reflected DTO reader; every unknown binding remains rejected.
   FString LegacyError;
   if(!AftershockLegacyV4::VerifyStoredWireMetadata(Stored,PackageVersion,EngineVersion,CustomVersions,LegacyError))return nullptr;
   auto* Legacy=AftershockLegacyV4::DecodePayload(Payload,LegacyError);
   if(Legacy){UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SAVE_FROZEN_V4_DECODE schema=4 exactBinding=1 currentBinaryDeserializer=0 migrationDeferredUntilValidation=1"));}
   else{UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_SAVE_FROZEN_V4_REJECT %s"),*LegacyError);}
   return Legacy;
  }
 }
 int32 NameCount=0;Reader<<NameCount;const int64 DictionaryStart=Reader.Tell();
 if(Reader.IsError()||NameCount<1||NameCount>MaximumIndexedNames||static_cast<int64>(NameCount)*5>Reader.TotalSize()-Reader.Tell())return nullptr;
 TArray<FName> Names;Names.Reserve(NameCount);TSet<FName> UniqueNames;
 for(int32 I=0;I<NameCount;++I)
 {
  FString Text;if(!ReadBoundedUtf8(Reader,Text,MaximumIndexedNameBytes)||Text.Len()>=NAME_SIZE||Reader.Tell()-DictionaryStart>MaximumDictionaryBytes)return nullptr;
  const FName Name(*Text);if(UniqueNames.Contains(Name)||(I==0&&!Name.IsNone()))return nullptr;UniqueNames.Add(Name);Names.Add(Name);
 }
 int32 BodyCount=0;Reader<<BodyCount;const int64 BodyStart=Reader.Tell();
 if(Reader.IsError()||BodyCount<4||BodyCount>MaximumUncompressedSaveBytes||BodyCount!=Reader.TotalSize()-BodyStart)return nullptr;
 Reader.SetUEVer(PackageVersion);Reader.SetEngineVer(EngineVersion);Reader.SetCustomVersions(CustomVersions);
 auto* Saved=NewObject<UAftershockSaveGame>(GetTransientPackage());FIndexedNameSaveArchive ObjectArchive(Reader,Names,true,BodyStart,BodyStart+BodyCount);
 ObjectArchive.SetWantBinaryPropertySerialization(bBinary);ObjectArchive.ArIsSaveGame=false;
 if(bBinary)AftershockSaveBinarySchema::RecordBinaryDeserialize();
 Saved->Serialize(ObjectArchive);
 if(ObjectArchive.IsError()||Reader.IsError()||Reader.Tell()!=BodyStart+BodyCount){ReleaseSnapshotPayload(*Saved);return nullptr;}
 return Saved;
}
UAftershockSaveGame* UAftershockSaveSubsystem::ReadTypedEnvelope(const TArray<uint8>& Bytes)
{
 TArray<uint8> Payload;int32 Version=0;if(!Decode(Bytes,Payload,&Version))return nullptr;
 return Version==3?DecodeIndexedPayload(Payload):Cast<UAftershockSaveGame>(UGameplayStatics::LoadGameFromMemory(Payload));
}

FString UAftershockSaveSubsystem::SlotPath(const FString& Slot)
{return ValidSlot(Slot)?FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("SaveGames/Aftershock"),Slot+TEXT(".sav")):FString();}
bool UAftershockSaveSubsystem::IsWorldLoadFailed(const UWorld* World) const
{return World&&FailedLoadWorld.Get()==World;}
void UAftershockSaveSubsystem::MarkWorldStartupFailure(UWorld* World,const FText& Error)
{
 if(!World)return;FailedLoadWorld=World;PendingLoad=nullptr;
 LoadFailure=Error.IsEmpty()?NSLOCTEXT("Aftershock","StartupFailureUnknown","Şehir verileri yüklenemedi. Yeni Oyun menüsünden yeniden deneyin."):Error;
}

bool UAftershockSaveSubsystem::ValidateSnapshot(UAftershockSaveGame& S,FText& Error) const
{
 ++SnapshotValidationCount;
 Error=FText::GetEmpty();
 const auto Fail=[&](const TCHAR* Message){Error=FText::FromString(Message);return false;};
 if(S.SchemaVersion<1||S.SchemaVersion>UAftershockSaveGame::CurrentVersion)return Fail(TEXT("Kayıt sürümü desteklenmiyor; dosya korunuyor."));
 if(S.SchemaVersion<3)
 {
  // Legacy versions described exactly one dense initial frame. Never infer
  // missing expansion data, reorder cells, or truncate an incorrectly labelled
  // expanded snapshot. Validate a copy before committing even the migration.
  const auto& Old=S.Terrain;const int64 InitialCount=static_cast<int64>(Old.GridWidth)*Old.GridHeight;
  if(Old.GridWidth<2||Old.GridHeight<2||Old.GridWidth>256||Old.GridHeight>256||InitialCount!=Old.Cells.Num()
   ||Old.TerrainStorageVersion!=1||!Old.Chunks.IsEmpty()||Old.ChunkSize!=UTerrainGridSubsystem::ChunkSizeCells||Old.ContinuationGeneratorVersion!=1||Old.TerrainRevision!=0||Old.bInitialMaskUnlocked)
   return Fail(TEXT("Eski kayıt yoğun başlangıç arazisiyle uyuşmuyor; genişleme verisi kayıpsız taşınamıyor."));
  for(int32 I=0;I<Old.Cells.Num();++I)if(Old.Cells[I].Coords!=FIntPoint(I%Old.GridWidth,I/Old.GridWidth))
   return Fail(TEXT("Eski kayıttaki hücre sırası/koordinatları kayıpsız taşınamıyor."));
  TStrongObjectPtr<UAftershockSaveGame> Migrated(DuplicateObject<UAftershockSaveGame>(&S,GetTransientPackage()));
  if(!Migrated.IsValid())return Fail(TEXT("Kayıt sürüm yükseltme kopyası oluşturulamadı."));
  ON_SCOPE_EXIT{ReleaseSnapshotPayload(*Migrated);};
  if(S.SchemaVersion==1){Migrated->Settings.FocusID=NAME_None;Migrated->Terrain.GeographyFocusIndex=0;}
  Migrated->Terrain.TerrainStorageVersion=2;Migrated->Terrain.ChunkSize=25;Migrated->Terrain.ContinuationGeneratorVersion=1;
  Migrated->Terrain.bUsesRegionalLayout=Migrated->Settings.Mode!=EAftershockPlayMode::Campaign;
  if(IsAftershockGeographyFocusReady(Migrated->Terrain.GeographyIndex,Migrated->Terrain.GeographyFocusIndex))
   for(auto& Cell:Migrated->Terrain.Cells)Cell.bHasRealElevationSample=true;
  UTerrainGridSubsystem::BuildChunkManifest(Migrated->Terrain.Cells,Migrated->Terrain.Chunks);
  // Finish terrain migration first; the copied schema3 path below then imports
  // historical identities without replaying money or inventing old causes.
  Migrated->SchemaVersion=3;
  if(!ValidateSnapshot(*Migrated,Error))return false;
  S.Settings=Migrated->Settings;S.Terrain=MoveTemp(Migrated->Terrain);S.CityLife=MoveTemp(Migrated->CityLife);S.CasualtyLedger=MoveTemp(Migrated->CasualtyLedger);S.Economy.AppliedDisasterEventIDs=MoveTemp(Migrated->Economy.AppliedDisasterEventIDs);S.Physics=MoveTemp(Migrated->Physics);S.Crowd=MoveTemp(Migrated->Crowd);S.SchemaVersion=Migrated->SchemaVersion;
  return true;
 }
 if(S.SchemaVersion<4)
 {
  TStrongObjectPtr<UAftershockSaveGame> Migrated(DuplicateObject<UAftershockSaveGame>(&S,GetTransientPackage()));
  if(!Migrated.IsValid())return Fail(TEXT("Kayıt sürüm yükseltme kopyası oluşturulamadı."));
  ON_SCOPE_EXIT{ReleaseSnapshotPayload(*Migrated);};
  for(auto& Person:Migrated->CityLife.Residents)
  {
   if(Person.FullName.IsEmpty()||Person.Age<1)
   {FText StableName;int32 StableAge=0;UCasualtyLedgerGenerator::GetStableDemographics(Person.ResidentID,Person.bWorkingAge,StableName,StableAge);if(Person.FullName.IsEmpty())Person.FullName=StableName;if(Person.Age<1)Person.Age=StableAge;}
  }
  UCasualtyLedgerGenerator::BuildLegacyLedgerState(Migrated->CityLife.Residents,Migrated->SessionID,Migrated->Economy.Turn,Migrated->CasualtyLedger);
  Migrated->Economy.AppliedDisasterEventIDs.Reset();for(const auto& Event:Migrated->CasualtyLedger.Events)Migrated->Economy.AppliedDisasterEventIDs.Add(Event.EventID);
  Migrated->SchemaVersion=4;if(!ValidateSnapshot(*Migrated,Error))return false;
  S.CityLife=MoveTemp(Migrated->CityLife);S.CasualtyLedger=MoveTemp(Migrated->CasualtyLedger);S.Economy.AppliedDisasterEventIDs=MoveTemp(Migrated->Economy.AppliedDisasterEventIDs);S.Terrain=MoveTemp(Migrated->Terrain);S.Physics=MoveTemp(Migrated->Physics);S.Crowd=MoveTemp(Migrated->Crowd);S.SchemaVersion=Migrated->SchemaVersion;return true;
 }
 if(S.SchemaVersion<5)
 {
  // A current physical snapshot cannot be made legacy by relabelling its
  // version. Reject rather than erase valid new inventory under migration.
  const FAftershockPhysicalHazardSaveState EmptyPhysics;
  if(!FAftershockPhysicalHazardSaveState::StaticStruct()->CompareScriptStruct(&S.Physics,&EmptyPhysics,0))
   return Fail(TEXT("Eski kayıt sürümü yeni fizik alanları taşıyor; fizik verisi sessizce silinemez."));
  for(const auto& Cell:S.Terrain.Cells)
   if(Cell.SnowWaterEquivalentMm!=0.f||Cell.RegolithDepthM!=0.f||Cell.SoilPoreWaterDepthM!=0.f
    ||Cell.GroundErodedDepthM!=0.f||Cell.GroundDepositedDepthM!=0.f||Cell.MaterialScenarioVersion!=0)
    return Fail(TEXT("Eski kayıt sürümü yeni fizik stokları taşıyor; stoklar sessizce sıfırlanamaz."));
  // V4 had no conserved hydraulic/mass inventory. Migrate a transient copy;
  // never synthesize snow/soil water from legacy visual moisture or river fill.
  TStrongObjectPtr<UAftershockSaveGame> Migrated(DuplicateObject<UAftershockSaveGame>(&S,GetTransientPackage()));
  if(!Migrated.IsValid())return Fail(TEXT("Fizik kayıt sürümü için güvenli geçiş kopyası oluşturulamadı."));
  ON_SCOPE_EXIT{ReleaseSnapshotPayload(*Migrated);};
  Migrated->Physics={};Migrated->Physics.ModelVersion=0;Migrated->Physics.bNeedsLegacyWaterSeed=true;
  for(auto& Cell:Migrated->Terrain.Cells)
  {
   Cell.SnowWaterEquivalentMm=0.f;Cell.RegolithDepthM=0.f;Cell.SoilPoreWaterDepthM=0.f;
   Cell.GroundErodedDepthM=0.f;Cell.GroundDepositedDepthM=0.f;Cell.MaterialScenarioVersion=0;
  }
  Migrated->SchemaVersion=5;
  if(!ValidateSnapshot(*Migrated,Error))return false;
  S.Terrain=MoveTemp(Migrated->Terrain);S.Physics=MoveTemp(Migrated->Physics);S.Crowd=MoveTemp(Migrated->Crowd);S.SchemaVersion=5;
  return true;
 }
 if(!UAftershockOpeningSessionSubsystem::ValidateSettings(S.Settings,Error))return false;
 // Compile reflection dispatch once for this validation call, not once per
 // resident/cell/waypoint. Float-free ID/coordinate branches are omitted here
 // only; all semantic identity, topology and bounds checks below still run.
 // No static FProperty pointers survive a call or a hot reload.
 TArray<FFinitePropertyPlan> FinitePlans;
 for(TFieldIterator<FProperty> Property(S.GetClass());Property;++Property)
 {FFinitePropertyPlan Plan;if(Plan.Compile(*Property))FinitePlans.Add(MoveTemp(Plan));}
 for(const auto& Plan:FinitePlans)if(!Plan.Evaluate(Plan.Property->ContainerPtrToValuePtr<void>(&S)))return Fail(TEXT("Kayıtta NaN veya sonsuz sayı var."));
 if(S.MapPackage!=TEXT("/Game/Aftershock/Levels/L_Level_01_Deprem"))return Fail(TEXT("Kayıttaki harita bu sürümde desteklenmiyor."));
 const auto& T=S.Terrain;const int64 InitialCount=static_cast<int64>(T.GridWidth)*T.GridHeight;
 if(T.GridWidth<2||T.GridHeight<2||T.GridWidth>256||T.GridHeight>256||InitialCount>T.Cells.Num()||T.Cells.Num()>UTerrainGridSubsystem::MaximumResidentCells
  ||T.TerrainStorageVersion!=2||T.ChunkSize!=UTerrainGridSubsystem::ChunkSizeCells||(T.ContinuationGeneratorVersion<1||T.ContinuationGeneratorVersion>3)||T.Chunks.IsEmpty()
  ||!FMath::IsFinite(T.CellSize)||T.CellSize<100||T.CellSize>5000||T.GridOrigin.ContainsNaN())return Fail(TEXT("Arazi boyutları veya chunk depolama sürümü geçersiz."));
 const int32 Region=FindAftershockGeographyIndex(S.Settings.RegionID),Focus=S.Settings.FocusID.IsNone()?0:FindAftershockGeographyFocusIndex(Region,S.Settings.FocusID);
 if(T.GeographyIndex!=Region||T.GeographyFocusIndex!=Focus||!IsAftershockGeographyFocusReady(Region,Focus))return Fail(TEXT("Kayıt coğrafyası ve oturum bölgesi uyuşmuyor."));
 if(T.bUsesRegionalLayout!=(S.Settings.Mode!=EAftershockPlayMode::Campaign))return Fail(TEXT("Kayıt arazi devam düzeni oyun moduyla uyuşmuyor."));
 // Coordinates are global and may be negative. Array order remains a stable
 // storage index; only the original prefix retains the old row-major contract.
 TMap<FIntPoint,int32> SavedCellIndices;SavedCellIndices.Reserve(T.Cells.Num());
 bool HasAuthoredMaterial=false;
 for(int32 I=0;I<T.Cells.Num();++I)
 {
  const auto& C=T.Cells[I];if((I<InitialCount&&C.Coords!=FIntPoint(I%T.GridWidth,I/T.GridWidth))||SavedCellIndices.Contains(C.Coords)
   ||static_cast<int64>(C.Coords.X)<-UTerrainGridSubsystem::MaximumAbsGlobalCellCoord||static_cast<int64>(C.Coords.X)>UTerrainGridSubsystem::MaximumAbsGlobalCellCoord||static_cast<int64>(C.Coords.Y)<-UTerrainGridSubsystem::MaximumAbsGlobalCellCoord||static_cast<int64>(C.Coords.Y)>UTerrainGridSubsystem::MaximumAbsGlobalCellCoord
   ||!FMath::IsFinite(C.Elevation)||!FMath::IsFinite(C.FloodDepth)||!FMath::IsFinite(C.DrainageQuality)||!FMath::IsFinite(C.RiverFill01))return Fail(TEXT("Arazi hücresi veya kalıcı global koordinatı geçersiz."));
  if(C.SnowWaterEquivalentMm<0.f||C.SnowBulkDensityKgM3<=0.f||C.SnowWeakLayerStrengthPa<0.f
   ||C.RegolithDepthM<0.f||C.SoilPorosity01<0.f||C.SoilPorosity01>1.f||C.SoilDryBulkDensityKgM3<=0.f
   ||C.SoilPoreWaterDepthM<0.f||static_cast<double>(C.SoilPoreWaterDepthM)>static_cast<double>(C.RegolithDepthM)*C.SoilPorosity01+1.e-6
   ||C.SoilCohesionPa<0.f||C.SoilFrictionAngleDegrees<0.f||C.SoilFrictionAngleDegrees>=90.f
   ||C.GroundErodedDepthM<0.f||C.GroundDepositedDepthM<0.f||C.MaterialScenarioVersion<0||C.MaterialScenarioVersion>1
   ||(C.MaterialScenarioVersion==0&&(C.SnowWaterEquivalentMm!=0.f||C.RegolithDepthM!=0.f||C.SoilPoreWaterDepthM!=0.f
    ||C.GroundErodedDepthM!=0.f||C.GroundDepositedDepthM!=0.f)))
   return Fail(TEXT("Arazi fizik stokları, doygunluk kapasitesi veya malzeme sürümü geçersiz."));
  HasAuthoredMaterial|=C.MaterialScenarioVersion==1;
  SavedCellIndices.Add(C.Coords,I);
 }
 const auto Inside=[&SavedCellIndices](FIntPoint C){return SavedCellIndices.Contains(C);};
 const auto LocationCell=[&T,&Inside](FVector Location,FIntPoint& Coord)
 {
  const double X=(Location.X-T.GridOrigin.X)/T.CellSize,Y=(Location.Y-T.GridOrigin.Y)/T.CellSize;
  if(!FMath::IsFinite(X)||!FMath::IsFinite(Y)||X<MIN_int32||X>=MAX_int32||Y<MIN_int32||Y>=MAX_int32)return false;
  Coord=FIntPoint(FMath::FloorToInt(X),FMath::FloorToInt(Y));return Inside(Coord);
 };
 if(S.Buildings.NextBuildingID<1||S.Buildings.Buildings.Num()>T.Cells.Num())return Fail(TEXT("Bina sayısı geçersiz."));
 TSet<int32> BuildingIDs;TSet<FIntPoint> Parcels;
 TMap<int32,const FAftershockBuildingSaveRecord*> BuildingsByID;BuildingsByID.Reserve(S.Buildings.Buildings.Num());
 for(const auto& B:S.Buildings.Buildings)
 {
  const auto& R=B.State;if(R.InstanceID<1||R.InstanceID>=S.Buildings.NextBuildingID||BuildingIDs.Contains(R.InstanceID)||Parcels.Contains(R.GridCoords)||!Inside(R.GridCoords)||static_cast<uint8>(B.DefinitionType)>23||static_cast<uint8>(R.DamageState)>static_cast<uint8>(EDamageState::Memorial)||static_cast<uint8>(R.Foundation)>2||R.CurrentFloors<1||R.CurrentFloors>200||!FMath::IsFinite(R.CurrentDamage)||B.WorldTransform.ContainsNaN())return Fail(TEXT("Bina kaydı geçersiz."));
  const auto& C=T.Cells[SavedCellIndices.FindChecked(R.GridCoords)];if(!C.bIsPlayable||C.OccupyingBuildingID!=R.InstanceID)return Fail(TEXT("Bina ve arazi işgali uyuşmuyor."));BuildingIDs.Add(R.InstanceID);Parcels.Add(R.GridCoords);BuildingsByID.Add(R.InstanceID,&B);
 }
 // Pure validation of the copied snapshot works before a level exists, too.
 // Never interpret saved coordinates through the current world's lookup table.
 if(!GetDefault<UTerrainGridSubsystem>()->ValidateSaveState(T,Error))return false;
 for(const auto& C:T.Cells)if(C.OccupyingBuildingID!=INDEX_NONE&&!BuildingIDs.Contains(C.OccupyingBuildingID))return Fail(TEXT("Arazi hücresinin bina kimliği kayıp."));
 if(S.Rubble.Num()>4096||S.DisasterVisual.Revision<0||S.DisasterVisualRemainingSeconds<0.f||S.DisasterVisualRemainingSeconds>3600.f||S.DisasterVisual.DurationSeconds<0.f||S.DisasterVisual.DurationSeconds>3600.f||static_cast<uint8>(S.DisasterVisual.Type)>static_cast<uint8>(EDisasterType::Landslide)||S.DisasterVisual.DroughtStress01<0||S.DisasterVisual.DroughtStress01>1||S.DisasterVisual.CropLoss01<0||S.DisasterVisual.CropLoss01>1||S.DisasterVisualRemainingSeconds>S.DisasterVisual.GetEffectiveDurationSeconds())return Fail(TEXT("Enkaz veya afet görsel süresi geçersiz."));
 TSet<int32> RubbleIDs;
 TArray<TStrongObjectPtr<UBuildingData>> ValidationDefinitions;ValidationDefinitions.SetNum(24);
 const auto DefinitionFor=[&](EBuildingType Type)->UBuildingData*
 {
  auto& Definition=ValidationDefinitions[static_cast<int32>(Type)];
  if(!Definition.IsValid())
  {
   if(const auto* W=GetGameInstance()?GetGameInstance()->GetWorld():nullptr)if(auto* Manager=W->GetSubsystem<UBuildingManagerSubsystem>())Definition.Reset(Manager->GetCatalogData(Type));
   if(!Definition.IsValid())Definition.Reset(FAftershockBuildingCatalog::CreateData(GetTransientPackage(),Type));
  }
  return Definition.Get();
 };
 for(const auto& Rubble:S.Rubble)
 {
  const auto* Building=BuildingsByID.FindRef(Rubble.BuildingID);
  if(!Building||RubbleIDs.Contains(Rubble.BuildingID)||Building->State.DamageState!=EDamageState::Collapsed||Building->State.GridCoords!=Rubble.Coord||!Rubble.ActorTransform.IsValid()||Rubble.LocalRestTransforms.IsEmpty()||Rubble.LocalRestTransforms.Num()>4096)return Fail(TEXT("Enkaz parça kaydı geçersiz."));
  for(const auto& Transform:Rubble.LocalRestTransforms)if(!Transform.IsValid())return Fail(TEXT("Enkaz parça pozu geçersiz."));RubbleIDs.Add(Rubble.BuildingID);
  FBuildingInstance CanonicalBuilding=Building->State;CanonicalBuilding.BuildingData=DefinitionFor(Building->DefinitionType);
  if(!AftershockScaledChaos::IsValidSavedFracturedAssetReference(CanonicalBuilding,Rubble.FracturedAssetPath,Rubble.ActorTransform,Error))return false;
  const auto* Asset=AftershockScaledChaos::ResolveSavedFracturedAsset(CanonicalBuilding,Rubble.FracturedAssetPath,Error);
  const auto Collection=Asset?Asset->GetGeometryCollection():nullptr;
  if(!Collection||Collection->Transform.Num()!=Rubble.LocalRestTransforms.Num())return Fail(TEXT("Enkaz kaydı bu sürümün modeliyle uyuşmuyor."));
 }
 const auto& Visual=S.DisasterVisual;
 for(const auto* Map:{&Visual.LocalSeverity,&Visual.LavaSeverity,&Visual.AshSeverity,&Visual.RiverFillBefore,&Visual.RiverFillAfter,&Visual.WaterDepthCm,&Visual.WaterSurfaceElevationCm,&Visual.WaterArrivalFraction,&Visual.LavaArrivalFraction})
  for(const auto& Pair:*Map)if(!Inside(Pair.Key)||!FMath::IsFinite(Pair.Value))return Fail(TEXT("Afet görsel hücresi veya değeri geçersiz."));
 if(Visual.FloodModelVersion<0||Visual.FloodModelVersion>2||Visual.RainfallPulseMm<0.f||Visual.RainfallPulseMm>200.f||Visual.FloodPulseDurationSeconds<0.f||Visual.FloodPulseDurationSeconds>3600.f)return Fail(TEXT("Fiziksel sel sürümü veya yağış darbesi geçersiz."));
 if(Visual.FloodModelVersion==0)
 {
  if(Visual.RainfallPulseMm!=0.f||Visual.FloodPulseDurationSeconds!=0.f||!Visual.WaterDepthCm.IsEmpty()||!Visual.WaterSurfaceElevationCm.IsEmpty()||!Visual.WaterArrivalFraction.IsEmpty())return Fail(TEXT("Eski sel modeli fiziksel alanlar taşıyamaz."));
 }
 else if(Visual.FloodModelVersion==1)
 {
  if(Visual.Type!=EDisasterType::Flood||Visual.FloodPulseDurationSeconds<=0.f||Visual.WaterDepthCm.Num()>UTerrainGridSubsystem::MaximumResidentCells||Visual.WaterDepthCm.Num()!=Visual.WaterSurfaceElevationCm.Num()||Visual.WaterDepthCm.Num()!=Visual.WaterArrivalFraction.Num())return Fail(TEXT("Fiziksel su alanları uyuşmuyor."));
  for(const auto& Pair:Visual.WaterDepthCm)
  {
   const float* Head=Visual.WaterSurfaceElevationCm.Find(Pair.Key);const float* Arrival=Visual.WaterArrivalFraction.Find(Pair.Key);
   if(Pair.Value<0.f||!Head||!Arrival||*Arrival<0.f||*Arrival>1.f)return Fail(TEXT("Fiziksel su derinliği veya varış zamanı geçersiz."));
  }
 }
 if(Visual.FloodModelVersion==2&&((Visual.Type!=EDisasterType::Flood&&Visual.Type!=EDisasterType::Tsunami)
  ||S.Physics.ModelVersion!=2||!Visual.WaterDepthCm.IsEmpty()||!Visual.WaterSurfaceElevationCm.IsEmpty()||!Visual.WaterArrivalFraction.IsEmpty()))
  return Fail(TEXT("Sürekli su görsel modeli yetkili fizik sınır durumuyla uyuşmuyor veya ikinci su haritası taşıyor."));
 if(Visual.LavaArrivalFraction.Num()>UTerrainGridSubsystem::MaximumResidentCells||(!Visual.LavaArrivalFraction.IsEmpty()&&(Visual.Type!=EDisasterType::Volcano||Visual.LavaArrivalFraction.Num()!=Visual.LavaSeverity.Num())))return Fail(TEXT("Lav varış alanları uyuşmuyor."));
 for(const auto& Pair:Visual.LavaArrivalFraction)if(!Visual.LavaSeverity.Contains(Pair.Key)||Pair.Value<0.f||Pair.Value>1.f)return Fail(TEXT("Lav varış zamanı geçersiz."));
 const auto& E=S.Economy;if(E.Turn<1||E.Year<1900||E.Year>10000||!FMath::IsFinite(E.Satisfaction)||E.Satisfaction<0||E.Satisfaction>100||!FMath::IsFinite(E.CO2)||!FMath::IsFinite(E.ClimateRisk))return Fail(TEXT("Ekonomi kaydı geçersiz."));
 const auto& L=S.CityLife;if(!L.bInitialized||L.NextResidentID<1||L.Residents.Num()>100000||L.PendingTransitRevenueTL<0)return Fail(TEXT("Nüfus kaydı hazır değil."));
 const auto FindBuilding=[&](int32 ID){return BuildingsByID.FindRef(ID);};
 for(int32 ID:L.SeededHomes)if(ID<1||ID>=S.Buildings.NextBuildingID)return Fail(TEXT("Konut nüfus tohumlama kimliği geçersiz."));
 for(const auto& B:S.Buildings.Buildings)if((B.DefinitionType==EBuildingType::House||B.DefinitionType==EBuildingType::Apartment)&&!L.SeededHomes.Contains(B.State.InstanceID))return Fail(TEXT("Mevcut konutun nüfus tohumlama kaydı eksik."));
 TMap<int32,const FCityResidentRecord*> ResidentsByID;ResidentsByID.Reserve(L.Residents.Num());
 TSet<int32> Residents,Living;for(const auto& R:L.Residents)
 {
  if(R.ResidentID<1||R.ResidentID>=L.NextResidentID||Residents.Contains(R.ResidentID)||!FMath::IsFinite(R.Health01)||!FMath::IsFinite(R.Hunger01)||!FMath::IsFinite(R.Happiness01))return Fail(TEXT("Vatandaş kaydı geçersiz."));
  const auto* Home=FindBuilding(R.HomeBuildingID);
  if(!L.SeededHomes.Contains(R.HomeBuildingID)||(Home&&Home->DefinitionType!=EBuildingType::House&&Home->DefinitionType!=EBuildingType::Apartment)||R.Health01<0||R.Health01>1||R.Hunger01<0||R.Hunger01>1||R.Happiness01<0||R.Happiness01>1||static_cast<uint8>(R.Emotion)>static_cast<uint8>(ECityResidentEmotion::Emergency))return Fail(TEXT("Vatandaş konutu veya ihtiyaç kaydı geçersiz."));
  if(R.WorkBuildingID!=INDEX_NONE&&!BuildingIDs.Contains(R.WorkBuildingID))return Fail(TEXT("Vatandaşın iş kaydı eksik."));
  Residents.Add(R.ResidentID);ResidentsByID.Add(R.ResidentID,&R);if(R.bAlive)Living.Add(R.ResidentID);
 }
 if(Living.Num()!=L.State.LivingPopulation||!FMath::IsFinite(L.State.FoodStock)||!FMath::IsFinite(L.State.WaterStock))return Fail(TEXT("Nüfus veya stok toplamı uyuşmuyor."));
 if(L.State.TotalDeaths!=L.Residents.Num()-Living.Num()||L.State.RawFoodStock<0||L.State.FoodStock<0||L.State.WaterStock<0||L.State.FuelStock<0||L.State.LastProcessedTurn<1||L.State.LastProcessedTurn>E.Turn||L.State.ResidentialTaxRate<0||L.State.ResidentialTaxRate>.3f||L.State.BusinessTaxRate<0||L.State.BusinessTaxRate>.3f)return Fail(TEXT("Nüfus, vergi veya stok defteri tutarsız."));
 if(!GetDefault<UCasualtyLedgerGenerator>()->ValidateSaveState(S.CasualtyLedger,Error))return false;
 if(S.CasualtyLedger.ArchiveSessionID.IsValid()&&S.CasualtyLedger.ArchiveSessionID!=S.SessionID)return Fail(TEXT("Anma arşivi başka bir oyun oturumuna ait."));
 TSet<int32> LedgerDeaths;TSet<FGuid> LedgerEvents,FiscalEvents;
 TMap<int32,const FCitizenMicroStory*> DeathsByID;DeathsByID.Reserve(S.CasualtyLedger.Entries.Num());
 for(const auto& Event:S.CasualtyLedger.Events){if(Event.Turn>E.Turn)return Fail(TEXT("Kayıp defterinde gelecek tur olayı var."));LedgerEvents.Add(Event.EventID);}
 for(const auto& Row:S.CasualtyLedger.Entries)
 {if(!Residents.Contains(Row.ResidentID)||Living.Contains(Row.ResidentID))return Fail(TEXT("Kayıplar defteri gerçek nüfusla uyuşmuyor."));LedgerDeaths.Add(Row.ResidentID);DeathsByID.Add(Row.ResidentID,&Row);}
 if(LedgerDeaths.Num()!=L.State.TotalDeaths)return Fail(TEXT("Kayıplar defterinde gerçek can kaybı kayıtları eksik."));
 for(int32 ID:S.CasualtyLedger.ExposedSurvivorResidentIDs)if(!Living.Contains(ID))return Fail(TEXT("Hayatta kalan kaydı canlı nüfusta yok."));
 for(const auto& Witness:S.CasualtyLedger.BuildingWitnessStories)if(!Residents.Contains(Witness.ResidentID))return Fail(TEXT("Bina tanıklığı gerçek vatandaş defterinde bulunmuyor."));
 if(S.CasualtyLedger.bHasCityKey)
 {
  const auto& Ledger=S.CasualtyLedger;const auto* Geography=TryGetAftershockGeographyFocus(T.GeographyIndex,T.GeographyFocusIndex);
  const int32 Stage=S.Scenario.ScenarioIndex;
  if(!Geography||Ledger.CitySessionID!=S.SessionID||Ledger.CityCampaignStage!=Stage||Ledger.CityRegionID!=FName(Geography->Id)||Ledger.CityFocusID!=FName(Geography->FocusId))return Fail(TEXT("Anma arşivinin şehir kimliği kayıtlı dünyayla uyuşmuyor."));
  const auto* Summary=Ledger.ArchiveSummaries.FindByPredicate([&Ledger](const auto& Row){return Row.SessionID==Ledger.CitySessionID&&Row.CampaignStage==Ledger.CityCampaignStage&&Row.RegionID==Ledger.CityRegionID&&Row.FocusID==Ledger.CityFocusID;});
  if(!Summary||Summary->Deaths!=L.State.TotalDeaths||Summary->LivingPopulation!=Living.Num()||Summary->UniqueExposedSurvivors!=Ledger.ExposedSurvivorResidentIDs.Num()||Summary->RetrofitInvestments!=Ledger.RetrofittedBuildingIDs.Num()||Summary->FlowerTributes!=Ledger.FlowerResidentIDs.Num())return Fail(TEXT("Anma arşivinin güncel şehir toplamları nüfus ve karar defteriyle uyuşmuyor."));
 }
 for(const FGuid& ID:E.AppliedDisasterEventIDs)
 {if(!ID.IsValid()||FiscalEvents.Contains(ID)||!LedgerEvents.Contains(ID))return Fail(TEXT("Afet mali işlemi kayıp defteriyle eşleşmiyor."));FiscalEvents.Add(ID);}
 if(FiscalEvents.Num()!=LedgerEvents.Num())return Fail(TEXT("Afet mali işlem defteri eksik; yükleme ikinci ödeme yapamaz."));
 // The physical owner is an additional persisted frontier, never a substitute
 // resident/building/fiscal authority. All references below resolve only in
 // THIS snapshot; no live world or asset pointer can legitimize saved IDs.
 const auto& Physics=S.Physics;const auto& Episode=Physics.Episode;const auto& WaterBudget=Physics.WaterBudget;
 const FString& MaterialReceipt=Physics.MaterialScenarioReceiptJSON;
 if(MaterialReceipt.Len()>256*1024||(HasAuthoredMaterial&&MaterialReceipt.IsEmpty())
  ||(Physics.bNeedsLegacyWaterSeed&&!MaterialReceipt.IsEmpty()))
  return Fail(TEXT("Malzeme/hava geçmişi kaydı eksik, boyut sınırını aşıyor veya eski kayda sonradan atfedilmiş."));
 if(!MaterialReceipt.IsEmpty())
 {
  const FTCHARToUTF8 ReceiptUTF8(*MaterialReceipt);
  if(ReceiptUTF8.Length()>256*1024)return Fail(TEXT("Malzeme geçmişi UTF-8 boyut sınırını aşıyor."));
  int32 Depth=0;bool InString=false,Escaped=false;
  for(TCHAR Character:MaterialReceipt)
  {
   if(Character==0)return Fail(TEXT("Malzeme geçmişinde gömülü null karakteri var."));
   if(InString){if(Escaped)Escaped=false;else if(Character==TEXT('\\'))Escaped=true;else if(Character==TEXT('"'))InString=false;continue;}
   if(Character==TEXT('"'))InString=true;
   else if(Character==TEXT('{')||Character==TEXT('[')){if(++Depth>32)return Fail(TEXT("Malzeme geçmişinin JSON derinliği geçersiz."));}
   else if(Character==TEXT('}')||Character==TEXT(']')){if(--Depth<0)return Fail(TEXT("Malzeme geçmişinin JSON yapısı geçersiz."));}
  }
  TSharedPtr<FJsonObject> ParsedReceipt;
  if(InString||Depth!=0||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MaterialReceipt),ParsedReceipt)||!ParsedReceipt.IsValid())
   return Fail(TEXT("Malzeme/hava geçmişi geçerli bir JSON nesnesi değil."));
 }

 if((Physics.ModelVersion!=0&&Physics.ModelVersion!=2)||Physics.WaterCells.Num()>T.Cells.Num()
  ||Episode.MassReplay.Num()>8*1024*1024||Episode.BeforeResidents.Num()>L.Residents.Num()
  ||Episode.BeforeBuildings.Num()>T.Cells.Num()||Episode.Exposures.Num()>Episode.BeforeBuildings.Num()
  ||Episode.ActualDeathResidentIDs.Num()>L.Residents.Num()||Episode.ExposedResidentIDs.Num()>L.Residents.Num())
  return Fail(TEXT("Fizik modeli veya sınırlı fizik kayıt boyutları geçersiz."));
 for(double Value:{Physics.CapturedCellAreaM2,Physics.PhysicalSeconds,Physics.PendingPhysicalSeconds,Physics.PendingAcceptedSeconds,
  WaterBudget.InitialSurfaceM3,WaterBudget.InitialStorageM3,WaterBudget.RainM3,WaterBudget.ExternalInflowM3,
  WaterBudget.BoundaryInflowM3,WaterBudget.BoundaryOutflowM3,WaterBudget.InfiltrationM3,
  WaterBudget.ExternalDrainageM3,WaterBudget.EvaporationM3,WaterBudget.StorageExternalOutflowM3,
  WaterBudget.FinalSurfaceM3,WaterBudget.FinalStorageM3,Episode.StartPhysicalSeconds,Episode.SourceDurationSeconds,Episode.SourceElapsedSeconds,
  Episode.RainMPerSecond,Episode.PlaybackSeconds,Episode.ReleasedMixtureWaterM3,Episode.TransferredMixtureWaterM3})
  if(!FMath::IsFinite(Value)||Value<0.)return Fail(TEXT("Fizik saatleri, su miktarı veya kaynak programı geçersiz."));
 if(Physics.PendingAcceptedSeconds>Physics.PendingPhysicalSeconds
  ||Physics.PendingForcingCells.Num()>65536||Physics.PendingForcingBoundaries.Num()>4*65536
  ||(Physics.PendingAcceptedSeconds==0.&&(!Physics.PendingForcingCells.IsEmpty()||!Physics.PendingForcingBoundaries.IsEmpty()))
  ||(Physics.PendingAcceptedSeconds>0.&&(Physics.ModelVersion!=2||!Physics.bWaterInitialized)))
  return Fail(TEXT("Kabul edilmiş su zaman dilimi veya tam kaynak paketi sınırları geçersiz."));
 if(!FMath::IsFinite(Physics.PhysicalSeconds+Physics.PendingPhysicalSeconds)||!FMath::IsFinite(WaterBudget.NumericalVolumeCorrectionM3)||!FMath::IsFinite(WaterBudget.ResidualM3)
  ||!FMath::IsFinite(Episode.SimulationTimeScale)||Episode.SimulationTimeScale<=0.
  ||static_cast<uint8>(Episode.Type)>static_cast<uint8>(EDisasterType::Landslide)
  ||Episode.BaseSeverity<0.f||Episode.CityAwareness01<0.f||Episode.CityAwareness01>1.f
  ||Episode.CollapsedBuildings<0||Episode.CollapsedBuildings>Episode.BeforeBuildings.Num()
  ||Episode.PreventedDeathsEstimate<0||Episode.PreventedDeathsEstimate>Episode.BeforeResidents.Num()
  ||Episode.SourceElapsedSeconds>Episode.SourceDurationSeconds+1.e-8
  ||Episode.TransferredMixtureWaterM3>Episode.ReleasedMixtureWaterM3+1.e-8)
  return Fail(TEXT("Fizik bölümünün türü, ilerlemesi veya korunmuş kütlesi geçersiz."));
 const bool EpisodeHasState=Episode.bActive||Episode.bOutcomeCommitted||Episode.EventID.IsValid()
  ||!Episode.BeforeResidents.IsEmpty()||!Episode.BeforeBuildings.IsEmpty()||!Episode.Exposures.IsEmpty()
  ||!Episode.ActualDeathResidentIDs.IsEmpty()||!Episode.ExposedResidentIDs.IsEmpty()||!Episode.MassReplay.IsEmpty();
 if(!EpisodeHasState&&(Episode.BaseSeverity!=0.f||Episode.CityAwareness01!=0.f||Episode.StartPhysicalSeconds!=0.||Episode.SourceDurationSeconds!=0.
  ||Episode.SourceElapsedSeconds!=0.||Episode.RainMPerSecond!=0.||Episode.PlaybackSeconds!=0.
  ||Episode.CollapsedBuildings!=0||Episode.PreventedDeathsEstimate!=0
  ||Episode.ReleasedMixtureWaterM3!=0.||Episode.TransferredMixtureWaterM3!=0.))
  return Fail(TEXT("Olay kimliği olmayan fizik kaynak programı veya sonuç alanı var."));
 if(Physics.ModelVersion==0)
 {
  if(Physics.bWaterInitialized||!Physics.WaterCells.IsEmpty()||Physics.CapturedCellAreaM2!=0.
   ||Physics.PendingAcceptedSeconds!=0.||!Physics.PendingForcingCells.IsEmpty()||!Physics.PendingForcingBoundaries.IsEmpty()
   ||Physics.PhysicalSeconds!=0.||Physics.PendingPhysicalSeconds!=0.||Physics.PublishedStep!=0||Physics.DomainRevision!=0
   ||WaterBudget.InitialSurfaceM3!=0.||WaterBudget.InitialStorageM3!=0.||WaterBudget.RainM3!=0.||WaterBudget.ExternalInflowM3!=0.
   ||WaterBudget.BoundaryInflowM3!=0.||WaterBudget.BoundaryOutflowM3!=0.||WaterBudget.InfiltrationM3!=0.
   ||WaterBudget.ExternalDrainageM3!=0.||WaterBudget.EvaporationM3!=0.||WaterBudget.StorageExternalOutflowM3!=0.
   ||WaterBudget.FinalSurfaceM3!=0.||WaterBudget.FinalStorageM3!=0.||WaterBudget.NumericalVolumeCorrectionM3!=0.
   ||WaterBudget.ResidualM3!=0.||WaterBudget.AcceptedSteps!=0||WaterBudget.FaceEvaluations!=0||EpisodeHasState||Episode.StartPhysicalSeconds!=0.||Episode.SourceDurationSeconds!=0.
   ||Episode.SourceElapsedSeconds!=0.||Episode.RainMPerSecond!=0.||Episode.PlaybackSeconds!=0.
   ||Episode.ReleasedMixtureWaterM3!=0.||Episode.TransferredMixtureWaterM3!=0.)
   return Fail(TEXT("Başlatılmamış/eski fizik modeli kalıcı su veya etkin olay verisi taşıyamaz."));
 }
 else
 {
  const double ExpectedArea=FMath::Square(static_cast<double>(T.CellSize)*.01);
  if(Physics.bNeedsLegacyWaterSeed||!Physics.bWaterInitialized||Physics.WaterCells.IsEmpty()
   ||Physics.CapturedCellAreaM2<=0.||!FMath::IsNearlyEqual(Physics.CapturedCellAreaM2,ExpectedArea,ExpectedArea*1.e-8)
   ||Physics.PublishedStep!=WaterBudget.AcceptedSteps||WaterBudget.InitialStorageM3!=0.
   ||WaterBudget.FinalStorageM3!=0.||WaterBudget.StorageExternalOutflowM3!=0.)
   return Fail(TEXT("SI su sınır durumu, hücre alanı veya desteklenmeyen 0D depo verisi geçersiz."));
  TSet<FIntPoint> WaterCoords;WaterCoords.Reserve(Physics.WaterCells.Num());
  double TotalWaterM3=0.,TotalInfiltrationM3=0.;
  const bool FrozenMassBed=Episode.bActive&&(Episode.Type==EDisasterType::Avalanche||Episode.Type==EDisasterType::Landslide);
  for(const auto& Cell:Physics.WaterCells)
  {
   const int32* SavedCellIndex=SavedCellIndices.Find(Cell.Coord);
   if(!SavedCellIndex||WaterCoords.Contains(Cell.Coord)||!FMath::IsFinite(Cell.BedElevationCm)||FMath::Abs(Cell.BedElevationCm)>1.e9
    ||!FMath::IsFinite(Cell.VolumeM3)||Cell.VolumeM3<0.
    ||!FMath::IsFinite(Cell.DischargeXM2PerSecond)||!FMath::IsFinite(Cell.DischargeYM2PerSecond)
    ||!FMath::IsFinite(Cell.CumulativeInfiltrationM3)||Cell.CumulativeInfiltrationM3<0.
    ||(Cell.VolumeM3==0.&&(Cell.DischargeXM2PerSecond!=0.||Cell.DischargeYM2PerSecond!=0.)))
    return Fail(TEXT("Kalıcı su hücresinin koordinatı, hacmi veya akışı geçersiz."));
   if(!FrozenMassBed)
   {
    double ExpectedBedCm=0.;FText BedError;
    if(!TryGetAftershockHydraulicBedCm(T.Cells[*SavedCellIndex],true,ExpectedBedCm,BedError)
     ||!FMath::IsNearlyEqual(Cell.BedElevationCm,ExpectedBedCm,1.e-3))
     return Fail(TEXT("Kalıcı su yatağı mevcut kanonik hidrolik yatakla uyuşmuyor."));
   }
   // During accepted/pending mass motion the surface solver retains its last
   // committed bed. Mineral overlays can advance above it; serializing that
   // exact frozen bed prevents reinterpretation of the saved water column.
   WaterCoords.Add(Cell.Coord);TotalWaterM3+=Cell.VolumeM3;TotalInfiltrationM3+=Cell.CumulativeInfiltrationM3;
  }
  if(Physics.PendingAcceptedSeconds>0.)
  {
   if(Physics.PendingForcingCells.Num()!=Physics.WaterCells.Num()
    ||Physics.PendingForcingBoundaries.Num()>Physics.WaterCells.Num()*4)
    return Fail(TEXT("Kabul edilmiş su kaynağı her kalıcı su hücresini tam bir kez kapsamalı."));
   TSet<FIntPoint> ForcedCoords;ForcedCoords.Reserve(Physics.PendingForcingCells.Num());
   for(const auto& Force:Physics.PendingForcingCells)
   {
    if(!WaterCoords.Contains(Force.Coord)||ForcedCoords.Contains(Force.Coord)
     ||!FMath::IsFinite(Force.InflowVelocityXMps)||!FMath::IsFinite(Force.InflowVelocityYMps))
     return Fail(TEXT("Kabul edilmiş su kaynak hücresi veya hızı geçersiz."));
    for(double Value:{Force.RainMPerSecond,Force.InfiltrationCapacityMPerSecond,Force.ExternalDrainCapacityM3PerSecond,
     Force.EvaporationMPerSecond,Force.ExternalInflowM3PerSecond})
     if(!FMath::IsFinite(Value)||Value<0.)return Fail(TEXT("Kabul edilmiş su kaynağı negatif veya sonlu değil."));
    const double Interval=Physics.PendingAcceptedSeconds,Area=Physics.CapturedCellAreaM2;
    if(!FMath::IsFinite(Force.RainMPerSecond*Area*Interval)||!FMath::IsFinite(Force.InfiltrationCapacityMPerSecond*Area*Interval)
     ||!FMath::IsFinite(Force.EvaporationMPerSecond*Area*Interval)||!FMath::IsFinite(Force.ExternalDrainCapacityM3PerSecond*Interval)
     ||!FMath::IsFinite(Force.ExternalInflowM3PerSecond*Interval)
     ||!FMath::IsFinite(Force.ExternalInflowM3PerSecond*Force.InflowVelocityXMps)
     ||!FMath::IsFinite(Force.ExternalInflowM3PerSecond*Force.InflowVelocityYMps))
     return Fail(TEXT("Kabul edilmiş su paketi zaman/hacim veya momentum hesabında taşıyor."));
    ForcedCoords.Add(Force.Coord);
   }
   // Only bounded scalar/uniqueness validation lives here. The runtime
   // owner MUST bind this sparse stage set to the exact immutable Domain
   // BEFORE initialization/publication. No private face-ID codec is copied.
   TSet<int64> ForcedBoundaryIDs;ForcedBoundaryIDs.Reserve(Physics.PendingForcingBoundaries.Num());
   for(const auto& Boundary:Physics.PendingForcingBoundaries)
   {
    if(ForcedBoundaryIDs.Contains(Boundary.FaceStableID)
     ||!FMath::IsFinite(Boundary.HeadM)||!FMath::IsFinite(Boundary.VelocityXMps)||!FMath::IsFinite(Boundary.VelocityYMps))
     return Fail(TEXT("Kabul edilmiş su stage kimliği, yüksekliği veya hızı geçersiz."));
    ForcedBoundaryIDs.Add(Boundary.FaceStableID);
   }
  }
  const double VolumeScale=FMath::Max(1.,FMath::Max(TotalWaterM3,WaterBudget.InitialSurfaceM3+WaterBudget.RainM3
   +WaterBudget.ExternalInflowM3+WaterBudget.BoundaryInflowM3+FMath::Abs(WaterBudget.NumericalVolumeCorrectionM3)));
  const double Tolerance=VolumeScale*1.e-7;
  const double Residual=WaterBudget.InitialSurfaceM3+WaterBudget.RainM3+WaterBudget.ExternalInflowM3
   +WaterBudget.BoundaryInflowM3+WaterBudget.NumericalVolumeCorrectionM3-WaterBudget.BoundaryOutflowM3
   -WaterBudget.InfiltrationM3-WaterBudget.ExternalDrainageM3-WaterBudget.EvaporationM3-WaterBudget.FinalSurfaceM3;
  if(!FMath::IsFinite(TotalWaterM3)||!FMath::IsFinite(TotalInfiltrationM3)||!FMath::IsFinite(VolumeScale)
   ||FMath::Abs(TotalWaterM3-WaterBudget.FinalSurfaceM3)>Tolerance
   ||FMath::Abs(TotalInfiltrationM3-WaterBudget.InfiltrationM3)>Tolerance
   ||!FMath::IsFinite(Residual)||FMath::Abs(Residual)>Tolerance||FMath::Abs(Residual-WaterBudget.ResidualM3)>Tolerance)
   return Fail(TEXT("Kalıcı su ve hücresel sızma miktarı korunum defteriyle uyuşmuyor."));
 }
 if(EpisodeHasState)
 {
  const bool IsMass=Episode.Type==EDisasterType::Avalanche||Episode.Type==EDisasterType::Landslide;
  if(!Episode.EventID.IsValid()||(!Episode.bActive&&!Episode.bOutcomeCommitted)
   ||(Episode.Type!=EDisasterType::Flood&&Episode.Type!=EDisasterType::Tsunami&&!IsMass)
   ||(Episode.bOutcomeCommitted&&(!LedgerEvents.Contains(Episode.EventID)||!FiscalEvents.Contains(Episode.EventID)))
   ||(!Episode.bOutcomeCommitted&&(!Episode.ActualDeathResidentIDs.IsEmpty()||LedgerEvents.Contains(Episode.EventID)||FiscalEvents.Contains(Episode.EventID)))
   ||(IsMass&&Episode.bActive&&Episode.MassReplay.IsEmpty())
   ||(!IsMass&&(!Episode.MassReplay.IsEmpty()||Episode.ReleasedMixtureWaterM3!=0.||Episode.TransferredMixtureWaterM3!=0.)))
   return Fail(TEXT("Fizik olay kimliği, tek seferlik sonuç işlemi veya afet türü tutarsız."));
  // Validate the same owner source-boundary arithmetic before any restore
  // import. Finite operands can still produce an infinite end or a positive
  // duration that rounds back to Start. This is observational only: it never
  // changes clock/debt/accepted controls or normalizes the saved source.
  AftershockPhysicalLifecycle::ESourceControlPhase SavedSourcePhase;
  double SavedBoundaryGap=0.,SavedSourceElapsed=0.,SavedSourceTolerance=0.;FString SourceBoundaryFailure;
  if(!AftershockPhysicalLifecycle::ClassifySourceControls(Physics.PhysicalSeconds,
   Episode.StartPhysicalSeconds,Episode.SourceDurationSeconds,SavedSourcePhase,
   SavedBoundaryGap,SavedSourceElapsed,SavedSourceTolerance,SourceBoundaryFailure))
   return Fail(*(FString(TEXT("Fizik olayının kaynak zaman sınırı gösterilemiyor: "))+SourceBoundaryFailure));
  TMap<int32,const FCityResidentRecord*> BeforeResidentsByID;BeforeResidentsByID.Reserve(Episode.BeforeResidents.Num());
  for(const auto& Person:Episode.BeforeResidents)
  {
   if(!Residents.Contains(Person.ResidentID)||BeforeResidentsByID.Contains(Person.ResidentID)
    ||Person.Health01<0.f||Person.Health01>1.f||Person.Hunger01<0.f||Person.Hunger01>1.f
    ||Person.Happiness01<0.f||Person.Happiness01>1.f||Person.Age<1||Person.Age>120||Person.FullName.IsEmpty()
    ||static_cast<uint8>(Person.Emotion)>static_cast<uint8>(ECityResidentEmotion::Emergency)
    ||!L.SeededHomes.Contains(Person.HomeBuildingID))return Fail(TEXT("Fizik olayının nüfus başlangıcı gerçek vatandaş kimlikleriyle uyuşmuyor."));
   BeforeResidentsByID.Add(Person.ResidentID,&Person);
  }
  TMap<int32,const FAftershockPhysicalBuildingBaseline*> BeforeBuildingsByID;BeforeBuildingsByID.Reserve(Episode.BeforeBuildings.Num());
  for(const auto& Baseline:Episode.BeforeBuildings)
  {
   const auto& B=Baseline.State;const auto* Current=FindBuilding(B.InstanceID);
   if(B.InstanceID<1||B.InstanceID>=S.Buildings.NextBuildingID||BeforeBuildingsByID.Contains(B.InstanceID)
    ||!Inside(B.GridCoords)||B.BuildingData!=nullptr||B.ISMInstanceIndex!=INDEX_NONE
    ||static_cast<uint8>(Baseline.DefinitionType)>23||static_cast<uint8>(B.DamageState)>static_cast<uint8>(EDamageState::Memorial)
    ||static_cast<uint8>(B.Foundation)>2||static_cast<uint8>(B.FoundationQuality)>2||B.CurrentFloors<1||B.CurrentFloors>200
    ||B.CurrentDamage<0.f||B.CurrentDamage>100.f||B.AgeInYears<0||B.StructuralQuality<.5f||B.StructuralQuality>2.f
    ||B.RoofQuality<.5f||B.RoofQuality>2.f||B.Preparedness<0.f||B.Preparedness>1.f
    ||(!Episode.bOutcomeCommitted&&!Current)
    ||(Current&&(Current->DefinitionType!=Baseline.DefinitionType||Current->State.GridCoords!=B.GridCoords)))
    return Fail(TEXT("Fizik olayının bina başlangıcı kimlik sahibiyle uyuşmuyor veya sabit asset pointer taşıyor."));
   BeforeBuildingsByID.Add(B.InstanceID,&Baseline);
  }
  TSet<int32> ExposureBuildingIDs;int64 AssignedDeaths=0;int32 CountedCollapses=0;
  for(const auto& Exposure:Episode.Exposures)
  {
   if(!BeforeBuildingsByID.Contains(Exposure.BuildingID)||ExposureBuildingIDs.Contains(Exposure.BuildingID)
    ||(!Episode.bOutcomeCommitted&&!BuildingIDs.Contains(Exposure.BuildingID))
    ||Exposure.PeakDepthM<0.||Exposure.PeakSpeedMps<0.||Exposure.WetSeconds<0.
    ||Exposure.PeakPressurePa<0.||Exposure.ImpulseNs<0.||Exposure.AppliedDamageDemand<0.f||Exposure.AppliedDamageDemand>100.f
    ||Exposure.AssignedDeaths<0||Exposure.AssignedDeaths>Episode.BeforeResidents.Num())
    return Fail(TEXT("Fizik maruziyeti bina kimliğine veya fiziksel sınırlara uymuyor."));
   ExposureBuildingIDs.Add(Exposure.BuildingID);AssignedDeaths+=Exposure.AssignedDeaths;
   CountedCollapses+=Exposure.bCollapseCounted?1:0;
  }
  if(CountedCollapses!=Episode.CollapsedBuildings||AssignedDeaths<Episode.ActualDeathResidentIDs.Num())
   return Fail(TEXT("Fizik olayının yeni yıkım veya gerçek kayıp toplamı tutarsız."));
  TSet<int32> ActualEpisodeDeaths,ExposedEpisodeResidents;
  for(int32 ID:Episode.ActualDeathResidentIDs)
  {
   const auto* Before=BeforeResidentsByID.FindRef(ID);const auto* Death=DeathsByID.FindRef(ID);
   if(!Before||!Before->bAlive||Living.Contains(ID)||ActualEpisodeDeaths.Contains(ID)
    ||!Death||Death->DisasterEventID!=Episode.EventID)return Fail(TEXT("Fizik olayı kayıp kimliği gerçek ve tek seferlik sonuç defterine uymuyor."));
   ActualEpisodeDeaths.Add(ID);
  }
  for(int32 ID:Episode.ExposedResidentIDs)
  {
   const auto* Before=BeforeResidentsByID.FindRef(ID);
   if(!Before||!Before->bAlive||ExposedEpisodeResidents.Contains(ID))return Fail(TEXT("Fizik maruziyetinde gerçek vatandaş kimliği eksik veya yinelenmiş."));
   ExposedEpisodeResidents.Add(ID);
  }
  if(!Episode.MassReplay.IsEmpty())
  {
   // One bounded deep decode, not ValidateBytes followed by a second copy.
   // A self-consistent replay must also belong to THIS saved event/map frame.
   FAftershockMassVisualEvent CheckedReplay;
   if(!DecodeAftershockMassReplay(Episode.MassReplay,CheckedReplay,Error))return false;
   const FVector2D ExpectedOrigin=FVector2D(T.GridOrigin)*.01;
   if(CheckedReplay.EventID!=Episode.EventID||CheckedReplay.Result.HazardTypeTag!=static_cast<int32>(Episode.Type)
    ||!FMath::IsNearlyEqual(CheckedReplay.CellSizeM,static_cast<double>(T.CellSize)*.01,1.e-8)
    ||!CheckedReplay.GridOriginM.Equals(ExpectedOrigin,1.e-8)||!CheckedReplay.WorldOffsetCm.IsNearlyZero(1.e-6))
    return Fail(TEXT("Afet hareket kaydının olay kimliği, türü veya kanonik harita koordinat çerçevesi uyuşmuyor."));
   // Support is an immutable subset: later resident expansion is permitted.
   // Do not compare current elevation/revision for equality: replay can already
   // have applied valid progressive mineral erosion/deposition to this world.
   for(const auto& SourceCell:CheckedReplay.SupportCells)if(!SavedCellIndices.Contains(SourceCell.Coord))
    return Fail(TEXT("Afet hareket kaydının donmuş kaynak hücresi bu kanonik haritada yok."));
  }
 }
 for(const auto& Building:S.Buildings.Buildings)
 {
  const auto& R=Building.State;
  if(R.DamageState==EDamageState::Memorial)
  {
   const bool WaitingForGarageExit=S.Transport.Vehicles.ContainsByPredicate([&R](const auto& Car){return Car.bActive&&Car.ParkedAtBuildingID==R.InstanceID;});
   if(R.CurrentDamage<80.f||R.MemorialStartedTurn<1||R.MemorialUnlockTurn-R.MemorialStartedTurn!=3||R.MemorialStartedTurn>E.Turn||(R.MemorialUnlockTurn<=E.Turn&&!WaitingForGarageExit)||!Building.WorldTransform.GetScale3D().IsNearlyZero())return Fail(TEXT("Hafıza mekânının parsel süresi veya görünümü geçersiz."));
   TSet<int32> SiteIDs;for(int32 ID:R.MemorialCasualtyResidentIDs)
   {
    const auto* Row=DeathsByID.FindRef(ID);
    if(!Row||Row->BuildingID!=R.InstanceID||SiteIDs.Contains(ID))return Fail(TEXT("Hafıza mekânı gerçek kayıp isimleriyle eşleşmiyor."));SiteIDs.Add(ID);
   }
  }
  else if(R.MemorialStartedTurn!=0||R.MemorialUnlockTurn!=0||!R.MemorialCasualtyResidentIDs.IsEmpty())return Fail(TEXT("Hafıza mekânı alanları yapı durumuyla uyuşmuyor."));
 }
 if(L.BuildingStates.Num()!=S.Buildings.Buildings.Num())return Fail(TEXT("İşletme durum defterinde eksik kayıt var."));
 for(const auto& Pair:L.BuildingStates)if(!BuildingIDs.Contains(Pair.Key)||Pair.Value.BuildingID!=Pair.Key||Pair.Value.AssignedWorkers<0||Pair.Value.JobCapacity<0||Pair.Value.Staffing01<0||Pair.Value.Staffing01>1||Pair.Value.Operation01<0||Pair.Value.Operation01>1)return Fail(TEXT("İşletme durum kimliği geçersiz."));
 for(const auto& Pair:L.BuildingRoadComponents)if(!BuildingIDs.Contains(Pair.Key)||Pair.Value<0)return Fail(TEXT("İşletme yol bileşeni geçersiz."));
 for(const auto& Pair:L.LastServiceVisitTurn)if(!Residents.Contains(static_cast<int32>(Pair.Key>>8))||(Pair.Key&255)>23||Pair.Value<0||Pair.Value>L.State.LastProcessedTurn)return Fail(TEXT("Hizmet ziyaret defteri geçersiz."));
 TSet<int32> Rendered;for(auto& A:S.Crowd.Agents)
 {
  FIntPoint At;
  if(!UCrowdRenderSubsystem::NormalizeDeferredDepartureAnchor(A,[&](int32 ID,const FVector& Location)
   {const auto* B=FindBuilding(ID);FIntPoint Source;return B&&B->DefinitionType!=EBuildingType::GreenSpace&&B->DefinitionType!=EBuildingType::BusStop&&LocationCell(Location,Source)&&FMath::Abs(Source.X-B->State.GridCoords.X)+FMath::Abs(Source.Y-B->State.GridCoords.Y)<=3;},Error))return false;
  if(A.WalkingPhaseRadians<0.f||A.WalkingPhaseRadians>2.f*PI)return Fail(TEXT("Vatandaş yürüyüş fazı geçersiz."));
  if(!Living.Contains(A.ResidentID)||Rendered.Contains(A.ResidentID)||!LocationCell(A.CurrentLocation,At)||A.MoveSpeed<0||A.MoveSpeed>1000||A.ScheduleSecondsRemaining<0||A.ScheduleSecondsRemaining>3600||A.FoodVisitCooldown<0||A.FoodVisitCooldown>3600||A.HealthVisitCooldown<0||A.HealthVisitCooldown>3600||A.Hunger01<0||A.Hunger01>1||A.Health01<0||A.Health01>1||A.Happiness01<0||A.Happiness01>1||static_cast<uint8>(A.Activity)>static_cast<uint8>(EAftershockCitizenActivity::Displaced)||static_cast<uint8>(A.ActivityAfterArrival)>static_cast<uint8>(EAftershockCitizenActivity::Displaced)||static_cast<uint8>(A.TransportArrivalActivity)>static_cast<uint8>(EAftershockCitizenActivity::Displaced)||A.NextRouteCoordIndex<0||A.NextRouteCoordIndex>A.RouteGridCoords.Num()||A.RouteGridCoords.Num()>T.Cells.Num()+1||(A.bInsideVehicle&&A.TransportTripID==INDEX_NONE))return Fail(TEXT("Vatandaş görsel kaydı geçersiz."));
  for(FIntPoint C:A.RouteGridCoords)if(!Inside(C))return Fail(TEXT("Vatandaş rotası harita dışında."));Rendered.Add(A.ResidentID);
  const auto* Resident=ResidentsByID.FindRef(A.ResidentID);
  if(!Resident||A.HomeBuildingID!=Resident->HomeBuildingID||A.WorkBuildingID!=Resident->WorkBuildingID)return Fail(TEXT("Görsel vatandaşın konut/iş kimliği nüfus kaydıyla uyuşmuyor."));
  const auto ValidService=[&](int32 ID,EBuildingType First,EBuildingType Second){const auto* B=FindBuilding(ID);return ID==INDEX_NONE||(B&&(B->DefinitionType==First||B->DefinitionType==Second));};
  if(!ValidService(A.FoodBuildingID,EBuildingType::Restaurant,EBuildingType::Bakery)||!ValidService(A.HealthBuildingID,EBuildingType::Hospital,EBuildingType::Hospital)||!ValidService(A.LeisureBuildingID,EBuildingType::GreenSpace,EBuildingType::GreenSpace)||(A.EmergencyBuildingID!=INDEX_NONE&&!BuildingIDs.Contains(A.EmergencyBuildingID)))return Fail(TEXT("Vatandaş hizmet hedefi kayıp veya yanlış türde."));
 }
 if(S.Crowd.Agents.Num()>1500||S.Crowd.Agents.Num()!=FMath::Min(1500,Living.Num()))return Fail(TEXT("Görsel nüfus tek nüfus kaynağıyla uyuşmuyor."));
 if(S.Crowd.RemainingEmergencySeconds<0||S.Crowd.RemainingEmergencySeconds>3600||static_cast<uint8>(S.Crowd.EmergencyDisasterType)>static_cast<uint8>(EDisasterType::Landslide))return Fail(TEXT("Vatandaş tahliye saati geçersiz."));
 const auto& V=S.Transport;if(V.NextTripID<1||V.NextVehicleID<1||V.Trips.Num()>105000||V.Vehicles.Num()>2500||!FMath::IsFinite(V.HistoryClockSeconds))return Fail(TEXT("Ulaşım kaydı geçersiz."));
 TMap<int32,const FAftershockPassengerTrip*> TripsByID;TripsByID.Reserve(V.Trips.Num());
 TMap<int32,const FAftershockRoadVehicle*> VehiclesByID;VehiclesByID.Reserve(V.Vehicles.Num());
 TMap<int32,const FAftershockTransitStop*> StopsByID;StopsByID.Reserve(V.Stops.Num());
 TSet<int32> Trips,Vehicles,ActiveTravelers;for(const auto& P:V.Trips)
 {
  FIntPoint At;
  if(P.TripID<1||P.TripID>=V.NextTripID||Trips.Contains(P.TripID)||!Residents.Contains(P.ResidentID)||!LocationCell(P.CurrentLocation,At)||static_cast<uint8>(P.Mode)>2||static_cast<uint8>(P.State)>5)return Fail(TEXT("Yolculuk kimlikleri veya global konumu geçersiz."));Trips.Add(P.TripID);TripsByID.Add(P.TripID,&P);
  const bool Active=P.State==EAftershockPassengerState::WalkingToPickup||P.State==EAftershockPassengerState::Waiting||P.State==EAftershockPassengerState::Riding;
  if(Active){if(ActiveTravelers.Contains(P.ResidentID))return Fail(TEXT("Aynı vatandaşın iki etkin yolculuğu var."));ActiveTravelers.Add(P.ResidentID);}
  if(Active&&P.Mode==EAftershockTravelMode::PrivateCar&&(!Inside(P.PickupRoadCell)||!Inside(P.DropoffRoadCell)))return Fail(TEXT("Etkin özel araç yolculuğunun global durak hücreleri kayıp."));
 }
 for(const auto& Car:V.Vehicles)
 {FIntPoint At;if(Car.VehicleID<1||Car.VehicleID>=V.NextVehicleID||Vehicles.Contains(Car.VehicleID)||!LocationCell(Car.Location,At)||Car.NextPathStep<0||Car.NextPathStep>Car.RoadPath.Num()||Car.RoadPath.Num()>T.Cells.Num()+1||Car.PassengerTripIDs.Num()>Car.Capacity)return Fail(TEXT("Araç kaydı veya global konumu geçersiz."));Vehicles.Add(Car.VehicleID);VehiclesByID.Add(Car.VehicleID,&Car);for(int32 ID:Car.PassengerTripIDs)if(!Trips.Contains(ID))return Fail(TEXT("Araç yolcusu kayıp."));for(FIntPoint C:Car.RoadPath)if(!Inside(C))return Fail(TEXT("Araç rotası harita dışında."));}
 if(V.HistoryClockSeconds<0||V.HistoryRefreshSeconds<0||V.HistoryRefreshSeconds>5.f||V.InfrastructureRefreshSeconds<0||V.Stops.Num()>T.Cells.Num())return Fail(TEXT("Ulaşım yenileme saati geçersiz."));
 TSet<int32> StopIDs;
 for(const auto& Stop:V.Stops)
 {
  const auto* B=FindBuilding(Stop.BuildingID);
  if(Stop.BuildingID<1||Stop.BuildingID>=S.Buildings.NextBuildingID||StopIDs.Contains(Stop.BuildingID)||!Inside(Stop.RoadCell)||Stop.Capacity<0||Stop.Capacity>40||(!B&&!V.bInfrastructureDirty)||(B&&B->DefinitionType!=(Stop.bTerminal?EBuildingType::BusTerminal:EBuildingType::BusStop)))return Fail(TEXT("Ulaşım durağı kimliği geçersiz."));StopIDs.Add(Stop.BuildingID);StopsByID.Add(Stop.BuildingID,&Stop);
 }
 const auto FindTrip=[&](int32 ID){return TripsByID.FindRef(ID);};
 const auto FindVehicle=[&](int32 ID){return VehiclesByID.FindRef(ID);};
 for(const auto& P:V.Trips)
 {
  const bool Active=P.State==EAftershockPassengerState::WalkingToPickup||P.State==EAftershockPassengerState::Waiting||P.State==EAftershockPassengerState::Riding;
  if(Active&&P.Mode==EAftershockTravelMode::Bus)
  {
   const auto* Pickup=StopsByID.FindRef(P.PickupStopID);
   const auto* Dropoff=StopsByID.FindRef(P.DropoffStopID);
   if(!Pickup||!Dropoff||Pickup->bTerminal||Dropoff->bTerminal||Pickup->BuildingID==Dropoff->BuildingID||Pickup->RoadCell!=P.PickupRoadCell||Dropoff->RoadCell!=P.DropoffRoadCell)return Fail(TEXT("Etkin otobüs yolculuğunun durak eşleşmesi geçersiz."));
  }
 }
 TSet<int32> AssignedPassengers;
 TMap<int32,int32> SavedGarageUse;
 for(const auto& Car:V.Vehicles)
 {
  const bool Serving=Car.bActive&&!Car.bStalled;
  if(Car.Capacity<1||Car.Capacity>40||Car.NextServiceStop<0||(Serving&&((!Car.ServiceStopIDs.IsEmpty()&&Car.NextServiceStop>=Car.ServiceStopIDs.Num())||(Car.ServiceStopIDs.IsEmpty()&&Car.NextServiceStop!=0))))return Fail(TEXT("Araç servis kapasitesi veya indeks geçersiz."));
  TSet<int32> ServiceIDs;for(int32 ID:Car.ServiceStopIDs){if(ID<1||ID>=S.Buildings.NextBuildingID||ServiceIDs.Contains(ID)||(Serving&&!V.bInfrastructureDirty&&!StopIDs.Contains(ID)))return Fail(TEXT("Araç durak listesi geçersiz."));ServiceIDs.Add(ID);}
  if(Car.bBus&&Car.bActive&&!Car.bStalled&&!StopIDs.Contains(Car.TerminalBuildingID))return Fail(TEXT("Aktif otobüs terminali eksik."));
  if(!Car.bBus&&!Residents.Contains(Car.ResidentOwnerID))return Fail(TEXT("Özel aracın gerçek sahibi eksik."));
  if(Car.ParkedAtBuildingID!=INDEX_NONE)
  {
   const auto* Garage=FindBuilding(Car.ParkedAtBuildingID);
   if(!Garage||Car.bBus||!Car.bActive||!Car.PassengerTripIDs.IsEmpty()||!Car.PendingAlightTripIDs.IsEmpty())return Fail(TEXT("Kapalı otopark kaydı gerçek yapı ve özel araçla eşleşmiyor."));
   FBuildingInstance GarageState=Garage->State;GarageState.BuildingData=DefinitionFor(Garage->DefinitionType);
   const int32 Capacity=UTransportSubsystem::GetOffstreetParkingCapacity(GarageState);
   if(Capacity<1||++SavedGarageUse.FindOrAdd(Car.ParkedAtBuildingID)>Capacity)return Fail(TEXT("Kapalı otopark kapasitesi aşıldı."));
  }
  if(Car.AlightReleaseSeconds<0.f||Car.AlightReleaseSeconds>.5f||(!Car.PendingAlightTripIDs.IsEmpty()&&(!Car.bBus||!Car.bActive||Car.bStalled||!Car.ServiceStopIDs.IsValidIndex(Car.NextServiceStop)))||(Car.PendingAlightTripIDs.IsEmpty()&&Car.AlightReleaseSeconds!=0.f))return Fail(TEXT("Araç iniş kuyruğu veya saati geçersiz."));
  TSet<int32> PendingAlight;
  for(int32 ID:Car.PendingAlightTripIDs)
  {
   const auto* P=FindTrip(ID);
   if(PendingAlight.Contains(ID)||!Car.PassengerTripIDs.Contains(ID)||!P||P->State!=EAftershockPassengerState::Riding||P->VehicleID!=Car.VehicleID||P->DropoffStopID!=Car.ServiceStopIDs[Car.NextServiceStop])return Fail(TEXT("İniş kuyruğu mevcut otobüs durağı ve gerçek yolcusuyla eşleşmiyor."));
   PendingAlight.Add(ID);
  }
  for(int32 ID:Car.PassengerTripIDs)
  {
   const auto* P=FindTrip(ID);
   const bool StateValid=P&&(P->State==EAftershockPassengerState::Riding||(!Car.bBus&&P->State==EAftershockPassengerState::WalkingToPickup));
   if(!StateValid||AssignedPassengers.Contains(ID)||P->VehicleID!=Car.VehicleID||(Car.bBus?P->Mode!=EAftershockTravelMode::Bus:(P->Mode!=EAftershockTravelMode::PrivateCar||P->ResidentID!=Car.ResidentOwnerID)))return Fail(TEXT("Araç/yolcu eşleşmesi geçersiz veya yinelenmiş."));AssignedPassengers.Add(ID);
  }
 }
 for(const auto& P:V.Trips)if(P.State==EAftershockPassengerState::Riding)
 {const auto* Car=FindVehicle(P.VehicleID);if(!Car||!Car->bActive||!Car->PassengerTripIDs.Contains(P.TripID))return Fail(TEXT("Araçta görünen yolcunun aracı/yolcu kaydı eksik."));}
 for(const auto& A:S.Crowd.Agents)if(A.TransportTripID!=INDEX_NONE)
 {
  const auto* P=FindTrip(A.TransportTripID);if(!P||P->ResidentID!=A.ResidentID)return Fail(TEXT("Vatandaş yolculuk kimliğiyle eşleşmiyor."));
  if(A.bInsideVehicle)
  {
   const auto* Car=FindVehicle(P->VehicleID);
   // Transport and crowd consume separate 80ms timers. An arrived/cancelled
   // passenger can legitimately remain hidden in the previous visual state
   // until the next crowd callback consumes this known terminal handoff.
   const bool Terminal=P->State==EAftershockPassengerState::Arrived||P->State==EAftershockPassengerState::Cancelled||P->State==EAftershockPassengerState::Stranded;
   const bool VehicleMatches=Car&&(Car->bBus?P->Mode==EAftershockTravelMode::Bus:(P->Mode==EAftershockTravelMode::PrivateCar&&Car->ResidentOwnerID==P->ResidentID));
   if(!VehicleMatches||(!Terminal&&(P->State!=EAftershockPassengerState::Riding||!Car->bActive||!Car->PassengerTripIDs.Contains(P->TripID))))return Fail(TEXT("Görsel yolcu gerçekte bir araçta veya sonlanmış aktarımda değil."));
  }
 }
 for(int32 ID:L.PaidTransitTrips)if(ID<1||ID>=V.NextTripID)return Fail(TEXT("Ücret idempotency kimliği geçersiz."));
 if(static_cast<uint8>(S.Director.Phase)>2||static_cast<uint8>(S.Director.PendingType)>static_cast<uint8>(EDisasterType::Landslide)||!FMath::IsFinite(S.Director.RemainingSeconds)||S.Director.RemainingSeconds<0||S.Director.LastAppliedEventSequence>S.Director.EventSequence)return Fail(TEXT("Afet akışı geçersiz."));
 if(S.Director.EventSequence<0||S.Director.LastAppliedEventSequence<0||S.Director.LastProcessedTurn<1||S.Director.LastProcessedTurn>E.Turn||S.Director.LastTriggerTurn<0||S.Director.LastTriggerTurn>E.Turn||S.Director.RemainingSeconds>3600||S.Director.WarningDuration<1||S.Director.WarningDuration>3600||(S.Director.Phase==EAftershockDisasterPhase::Warning&&S.Director.LastAppliedEventSequence>=S.Director.EventSequence))return Fail(TEXT("Afet akışı olay kimliği veya turu geçersiz."));
 if(S.Director.Severity<0||S.Director.Severity>95||(S.Director.Phase==EAftershockDisasterPhase::Ready&&S.Director.RemainingSeconds!=0)||(S.Director.Phase==EAftershockDisasterPhase::Warning&&(S.Director.RemainingSeconds>S.Director.WarningDuration||!IsAftershockFocusHazardAllowed(Region,Focus,S.Director.PendingType)))||(S.Director.Phase==EAftershockDisasterPhase::Aftermath&&(S.Director.EventSequence!=S.Director.LastAppliedEventSequence||S.Director.LastReport.DisasterType!=S.Director.PendingType)))return Fail(TEXT("Afet evresi ve uyarı coğrafyası tutarsız."));
 if(static_cast<uint8>(S.Scenario.Hazard)>static_cast<uint8>(EDisasterType::Landslide)||static_cast<uint8>(S.Director.LastReport.DisasterType)>static_cast<uint8>(EDisasterType::Landslide)||S.Scenario.ScenarioIndex<0||S.Scenario.ScenarioIndex>=7||S.Quiz.CorrectCount<0||S.Quiz.AnsweredCount<S.Quiz.CorrectCount||S.Camera.Location.ContainsNaN()||S.Camera.Rotation.ContainsNaN()||S.Camera.LocalFocus.ContainsNaN()||S.Camera.Orbit.ContainsNaN()||!FMath::IsFinite(S.Camera.Zoom)||S.Camera.Zoom<500)return Fail(TEXT("Oturum/kamera kaydı geçersiz."));
 const float MaximumCameraZoom=T.ContinuationGeneratorVersion>=2?637100000.f*6.f:1.e9f;
 if(S.Camera.SavedLocalZoom<500||S.Camera.SavedLocalZoom>1.e9f||S.Camera.Zoom>MaximumCameraZoom||S.Camera.PlanetOrbitPitch< -85||S.Camera.PlanetOrbitPitch>70||S.Camera.OverviewBlend<0||S.Camera.OverviewBlend>1)return Fail(TEXT("Kamera küre/şehir dönüş kaydı geçersiz."));
 FIntPoint CameraFocusCell;bool CameraFocusSupported=LocationCell(S.Camera.LocalFocus,CameraFocusCell);
 if(T.ContinuationGeneratorVersion>=2)
 {
  const FVector2D Metres((S.Camera.LocalFocus.X-T.GridOrigin.X-T.GridWidth*T.CellSize*.5)*.01,(S.Camera.LocalFocus.Y-T.GridOrigin.Y-T.GridHeight*T.CellSize*.5)*.01);
  double Latitude=0.,Longitude=0.;FAftershockGeographicTerrainSample Source;
  CameraFocusSupported=AftershockFocusLocalMetresToLatLon(Region,Focus,Metres,Latitude,Longitude)&&(T.ContinuationGeneratorVersion==3?TrySampleAftershockFocusDetailGeodata(Region,Focus,Latitude,Longitude,Source):TrySampleAftershockFocusGeodata(Region,Focus,Latitude,Longitude,Source));
 }
 if(!CameraFocusSupported)return Fail(TEXT("Kamera odağı desteklenen arazi veri sınırının dışında."));
 if(!FMath::IsFinite(S.Climate.InitialAnomalyC)||!FMath::IsFinite(S.Climate.GlobalTemperatureAnomalyC)||!FMath::IsFinite(S.Climate.AnnualTrendC))return Fail(TEXT("İklim kaydı geçersiz."));
 if(S.Quiz.bQuestionPending&&!S.Quiz.CurrentQuestion.Choices.IsValidIndex(S.Quiz.CurrentQuestion.CorrectIndex))return Fail(TEXT("Bekleyen eğitim sorusu geçersiz."));
 if(S.Campaign.FurthestStage<0||S.Campaign.FurthestStage>7)return Fail(TEXT("Kampanya ilerlemesi geçersiz."));for(int32 Stage:S.Campaign.CompletedStages)if(Stage<0||Stage>=7)return Fail(TEXT("Kampanya aşaması geçersiz."));
 return true;
}

UAftershockSaveGame* UAftershockSaveSubsystem::CaptureWorld(UWorld* W,FText& Error)
{
 if(bShuttingDown){Error=NSLOCTEXT("Aftershock","CaptureDuringShutdown","Oyun kapanırken yeni kayıt alınamaz.");return nullptr;}
 auto* S=NewObject<UAftershockSaveGame>(this);if(CaptureInto(W,*S,Error))return S;
 ReleaseSnapshotPayload(*S);return nullptr;
}

void UAftershockSaveSubsystem::Deinitialize()
{
 check(IsInGameThread());bShuttingDown=true;
 // Worker completion does not depend on a game-thread callback. Waiting here
 // cannot deadlock on UObject access or a GT continuation during world shutdown.
 CompletePendingAutosave(true,false);Super::Deinitialize();
}

bool UAftershockSaveSubsystem::BeginAutosaveWrite(UWorld* W,const FString& Slot,FText& Error)
{
 check(IsInGameThread());
 if(bShuttingDown||bWriting||bRestoring||ActiveAutosaveJob.IsValid())
 {Error=NSLOCTEXT("Aftershock","SaveAlreadyInProgress","Kayıt/yükleme zaten sürüyor; tamamlanmasını bekleyin.");return false;}
 if(!W||IsWorldLoadFailed(W)||SlotPath(Slot).IsEmpty()||(Slot==TEXT("Audit")&&!IsRunningCommandlet())||Slot==TEXT("Manual"))
 {Error=NSLOCTEXT("Aftershock","AutosaveNotAvailable","Bu şehir için otomatik kayıt başlatılamadı.");return false;}
 bWriting=true;bSaveInProgress.store(true,std::memory_order_release);bool Dispatched=false;ON_SCOPE_EXIT{if(!Dispatched){bWriting=false;bSaveInProgress.store(false,std::memory_order_release);}};
 if(!InternalSaveScratch)InternalSaveScratch=NewObject<UAftershockSaveGame>(this);
 ON_SCOPE_EXIT{ReleaseSnapshotPayload(*InternalSaveScratch);};
 auto Job=MakeShared<FAftershockAsyncSaveJob,ESPMode::ThreadSafe>();Job->Slot=Slot;Job->Path=SlotPath(Slot);
 const double CaptureStarted=FPlatformTime::Seconds();
 if(!CaptureInto(W,*InternalSaveScratch,Error,&Job->ValidateMs))return false;
 Job->CaptureMs=FMath::Max(0.,(FPlatformTime::Seconds()-CaptureStarted)*1000.-Job->ValidateMs);Job->SessionID=InternalSaveScratch->SessionID;
 double Started=FPlatformTime::Seconds();
 if(!EncodeIndexedPayload(*InternalSaveScratch,Job->Payload)||Job->Payload.IsEmpty()||Job->Payload.Num()>MaximumUncompressedSaveBytes)
 {Error=NSLOCTEXT("Aftershock","AutosaveEncodeFailed","Şehir serileştirilemedi; önceki kayıt korundu.");return false;}
 Job->SerializeMs=(FPlatformTime::Seconds()-Started)*1000.;
 // Validate an existing revision on the GT exactly as the synchronous writer
 // does. The worker may promote only these exact bytes, never an unchecked file.
 Started=FPlatformTime::Seconds();
 IPlatformFile& Files=FPlatformFileManager::Get().GetPlatformFile();Job->bHadPrevious=Files.FileExists(*Job->Path);
 if(Job->bHadPrevious)
 {
  if(!ReadBytes(Job->Path,Job->PreviousBytes))
  {Error=NSLOCTEXT("Aftershock","AutosavePreviousUnreadable","Önceki kayıt güvenle okunamadı; dosyalar korunuyor.");return false;}
  Job->ExpectedPreviousDigest=EnvelopeDigest(Job->PreviousBytes);Job->bPreviousValidated=IsValidatedDiskRevision(Job->Path,Job->PreviousBytes);
  if(!Job->bPreviousValidated)if(auto* Previous=ReadTypedEnvelope(Job->PreviousBytes))
  {
   TStrongObjectPtr<UAftershockSaveGame> KeepPrevious(Previous);ON_SCOPE_EXIT{ReleaseSnapshotPayload(*Previous);};
   FText OldError;Job->bPreviousValidated=ValidateSnapshot(*Previous,OldError);
   if(Job->bPreviousValidated)RememberValidatedDiskRevision(Job->Path,Job->PreviousBytes);
  }
 }
 Job->PreviousRevisionMs=(FPlatformTime::Seconds()-Started)*1000.;
 ActiveAutosaveJob=Job;Dispatched=true;
 const TWeakObjectPtr<UAftershockSaveSubsystem> WeakThis(this);
 Job->Worker=Async(EAsyncExecution::ThreadPool,[Job,WeakThis]()
 {
  RunAutosaveByteWorker(*Job);Job->bWorkerFinished.Store(true);
  AsyncTask(ENamedThreads::GameThread,[Job,WeakThis]()
  {
   if(auto* Self=WeakThis.Get())if(Self->ActiveAutosaveJob==Job&&!Self->bShuttingDown)Self->CompletePendingAutosave(false);
  });
 });
 UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_AUTOSAVE_QUEUED slot=%s captureMs=%.3f validateMs=%.3f serializeMs=%.3f previousRevisionReadValidateMs=%.3f gameThreadPreparationMs=%.3f singleByteWorker=1 sourceSnapshotReleasedAfterReturn=1"),*Slot,Job->CaptureMs,Job->ValidateMs,Job->SerializeMs,Job->PreviousRevisionMs,(FPlatformTime::Seconds()-CaptureStarted)*1000.);
 return true;
}

bool UAftershockSaveSubsystem::CompletePendingAutosave(bool bWait,bool bBroadcast)
{
 check(IsInGameThread());auto Job=ActiveAutosaveJob;if(!Job)return false;
 if(bWait&&Job->Worker.IsValid())Job->Worker.Wait();
 if(!Job->bWorkerFinished.Load())return false;
 const auto* CurrentSession=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockOpeningSessionSubsystem>():nullptr;
 const bool SameSession=CurrentSession&&CurrentSession->HasLiveSession()&&CurrentSession->GetSessionID()==Job->SessionID;
 // Only this GT path touches subsystem digests, rotation, delegates or UI text.
 if(!Job->BackupDigest.IsEmpty())ValidatedDiskDigests.Add(Job->Path+TEXT(".bak"),Job->BackupDigest);
 if(Job->bSuccess)
 {
  ValidatedDiskDigests.Add(Job->Path,Job->CommittedDigest);ValidatedDiskDigests.Remove(Job->Path+TEXT(".tmp"));
  if(SameSession&&Job->Slot.StartsWith(TEXT("Auto")))NextAutosave=(NextAutosave+1)%3;
 }
 bHasContinueCache=false;ActiveAutosaveJob.Reset();bSaveInProgress.store(false,std::memory_order_release);bWriting=false;
 FText Message=NSLOCTEXT("Aftershock","AutosaveFinished","Şehir güvenle kaydedildi.");
 if(!Job->bSuccess)
 {
  Message=Job->Failure==EAftershockSaveByteFailure::PreviousChanged
   ?NSLOCTEXT("Aftershock","AutosaveDiskChanged","Kayıt dosyası işlem sırasında değişti; yeni kayıt uygulanmadı.")
   :NSLOCTEXT("Aftershock","AutosaveWriteFailed","Otomatik kayıt tamamlanamadı; önceki kayıt ve yedek korundu.");
 }
 UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_AUTOSAVE_COMPLETED slot=%s captureMs=%.3f validateMs=%.3f serializeMs=%.3f previousRevisionReadValidateMs=%.3f compressMs=%.3f tempVerifyMs=%.3f backupMs=%.3f commitMs=%.3f workerMs=%.3f failure=%d success=%d gameThreadSerializeRemains=1"),*Job->Slot,Job->CaptureMs,Job->ValidateMs,Job->SerializeMs,Job->PreviousRevisionMs,Job->CompressMs,Job->TempVerifyMs,Job->BackupMs,Job->CommitMs,Job->WorkerMs,static_cast<int32>(Job->Failure),Job->bSuccess);
 if(bBroadcast&&!bShuttingDown&&SameSession)OnSaveCompleted.Broadcast(Job->bSuccess,Message);return Job->bSuccess;
}

bool UAftershockSaveSubsystem::CaptureInto(UWorld* W,UAftershockSaveGame& Snapshot,FText& Error,double* OutValidationMs)
{
 if(OutValidationMs)*OutValidationMs=0;
 if(!W||!W->GetGameInstance()||bRestoring||bShuttingDown){Error=FText::FromString(TEXT("Oyun yüklenirken veya kapanırken kayıt alınamaz."));return false;}
 auto* Session=W->GetGameInstance()->GetSubsystem<UAftershockOpeningSessionSubsystem>();auto* Crowd=W->GetSubsystem<UCrowdRenderSubsystem>();
 if(!Session||!Session->HasLiveSession()||!Crowd||Crowd->IsSyntheticRenderProfileActive()){Error=FText::FromString(TEXT("Yalnız başlamış gerçek şehir kaydedilebilir."));return false;}
 if(!Crowd->FlushScopedRefreshForSnapshot()){Error=FText::FromString(TEXT("Vatandaşlar güncellenirken kayıt alınamaz. Güncelleme tamamlandıktan sonra yeniden deneyin."));return false;}
 auto* S=&Snapshot;S->SchemaVersion=UAftershockSaveGame::CurrentVersion;S->MapPackage=TEXT("/Game/Aftershock/Levels/L_Level_01_Deprem");
 S->SavedAt=FDateTime::UtcNow();S->SessionID=Session->GetSessionID();S->Settings=Session->GetSettings();S->Campaign=Session->GetCampaignProgress();S->Camera={};
 if(!Session->HasConfiguredSession())
 {const auto* Scenario=W->GetSubsystem<UAftershockScenarioSubsystem>();S->Settings.CampaignStage=Scenario->GetScenarioIndex();S->Settings.RegionID=FName(GetAftershockGeography(Scenario->GetDefinition().GeographyIndex).Id);S->Settings.Seed=Scenario->GetDefinition().Seed;}
 W->GetSubsystem<UTerrainGridSubsystem>()->CaptureSaveState(S->Terrain);W->GetSubsystem<UBuildingManagerSubsystem>()->CaptureSaveState(S->Buildings);
 W->GetSubsystem<UDisasterSimulationSubsystem>()->CapturePhysicalSaveState(S->Physics);
 W->GetSubsystem<UDisasterSimulationSubsystem>()->CaptureRubbleSaveState(S->Rubble);S->DisasterVisual=W->GetSubsystem<UDisasterSimulationSubsystem>()->GetLatestVisualSnapshot();
 S->DisasterVisualRemainingSeconds=S->DisasterVisual.Revision>0?FMath::Max(0.f,S->DisasterVisual.GetEffectiveDurationSeconds()-(W->GetTimeSeconds()-S->DisasterVisual.StartedAtWorldSeconds)):0.f;
 W->GetSubsystem<UEconomyAndStateSubsystem>()->CaptureSaveState(S->Economy);W->GetSubsystem<UCityLifeSubsystem>()->CaptureSaveState(S->CityLife);
 W->GetSubsystem<UCasualtyLedgerGenerator>()->CaptureSaveState(S->CasualtyLedger);
 W->GetSubsystem<UTransportSubsystem>()->CaptureSaveState(S->Transport);W->GetSubsystem<UWorldClimateSubsystem>()->CaptureSaveState(S->Climate);
 W->GetSubsystem<UAftershockScenarioSubsystem>()->CaptureSaveState(S->Scenario);W->GetSubsystem<UAftershockDisasterDirectorSubsystem>()->CaptureSaveState(S->Director);
 Crowd->CaptureSaveState(S->Crowd);W->GetGameInstance()->GetSubsystem<UQuizSubsystem>()->CaptureSaveState(S->Quiz);
 for(TActorIterator<AAftershockCameraPawn> It(W);It;++It){It->CaptureSaveState(S->Camera);break;}
 const double ValidationStarted=FPlatformTime::Seconds();const bool Valid=ValidateSnapshot(*S,Error);
 if(OutValidationMs)*OutValidationMs=(FPlatformTime::Seconds()-ValidationStarted)*1000.;return Valid;
}

bool UAftershockSaveSubsystem::RestoreWorld(UWorld* W,UAftershockSaveGame* S,FText& Error)
{
 if(bWriting||bShuttingDown){Error=NSLOCTEXT("Aftershock","RestoreDuringSave","Kayıt tamamlanmadan şehir yüklenemez.");return false;}
 if(!W||!W->GetGameInstance()||!S||bRestoring||!ValidateSnapshot(*S,Error))return false;
 // Visual-space migration can reject a physically full legacy street after
 // pure snapshot validation. Preserve a live city before importing any owner.
 TStrongObjectPtr<UAftershockSaveGame> Previous;
 if(!bRollingBack&&W->GetSubsystem<UCityLifeSubsystem>()->IsCityInitialized())
 {
  FText BackupError;Previous.Reset(CaptureWorld(W,BackupError));
  if(!Previous.IsValid()){Error=FText::FromString(TEXT("Mevcut şehir güvenli geri dönüş için kayda alınamadı; yükleme başlamadı."));return false;}
 }
 TGuardValue<bool> Guard(bRestoring,true);
 bool bCommitted=false;
 ON_SCOPE_EXIT
 {
  if(!bCommitted&&Previous.IsValid()&&!bRollingBack)
  {
   const FText OriginalError=Error;FText RollbackError;TGuardValue<bool> RollbackGuard(bRollingBack,true);bRestoring=false;
   const bool Restored=RestoreWorld(W,Previous.Get(),RollbackError);Error=OriginalError;
   if(Restored){UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_LOAD_ROLLBACK restored=1 previousCityPreserved=1"));}
   else{UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_LOAD_ROLLBACK restored=0 previousCityPreserved=0"));}
  }
 };
 W->GetGameInstance()->GetSubsystem<UAftershockOpeningSessionSubsystem>()->RestoreSession(S->Settings,S->Campaign,S->SessionID);
 // Never seed buildings or replay TriggerDisaster: canonical casualties, aid
 // and fares are already committed in this complete owner snapshot.
 if(!W->GetSubsystem<UTerrainGridSubsystem>()->RestoreSaveState(S->Terrain,Error)
  ||!W->GetSubsystem<UBuildingManagerSubsystem>()->RestoreSaveState(S->Buildings,Error)
  ||!W->GetSubsystem<UCityLifeSubsystem>()->RestoreSaveState(S->CityLife,Error)
  ||!W->GetSubsystem<UEconomyAndStateSubsystem>()->RestoreSaveState(S->Economy,Error)
  ||!W->GetSubsystem<UCasualtyLedgerGenerator>()->RestoreSaveState(S->CasualtyLedger,Error)
  ||!W->GetSubsystem<UWorldClimateSubsystem>()->RestoreSaveState(S->Climate,Error)
  ||!W->GetSubsystem<UAftershockScenarioSubsystem>()->RestoreSaveState(S->Scenario,Error)
  ||!W->GetSubsystem<UAftershockDisasterDirectorSubsystem>()->RestoreSaveState(S->Director,Error)
  ||!W->GetGameInstance()->GetSubsystem<UQuizSubsystem>()->RestoreSaveState(S->Quiz,Error))return false;
 // Physical frontier must exist before visual restore or any crowd safety
 // readback. The owner validates its pure replay before mutating physical state.
 if(!W->GetSubsystem<UDisasterSimulationSubsystem>()->RestorePhysicalSaveState(S->Physics,Error))return false;
 // A loaded focus can share the same seed as the bootstrap terrain. Explicitly
 // rebuild the existing host from imported cells rather than trusting a seed cache.
 for(TActorIterator<AAftershockCrowdHostActor> It(W);It;++It)It->BuildTerrainVisual(W->GetSubsystem<UTerrainGridSubsystem>());
 if(!W->GetSubsystem<UDisasterSimulationSubsystem>()->RestoreRubbleSaveState(S->Rubble,Error))return false;
 if(!W->GetSubsystem<UDisasterSimulationSubsystem>()->RestoreVisualSaveState(S->DisasterVisual,S->DisasterVisualRemainingSeconds)){Error=FText::FromString(TEXT("Afet görsel kaydı geçersiz."));return false;}
 // Exactly one terrain-storage publication rebuilds native road connectivity
 // after canonical owners import. Final transport/crowd imports below replace
 // any derived caches without replaying construction, fares or population.
 W->GetSubsystem<UTerrainGridSubsystem>()->NotifyTerrainStorageRestored();
 if(!W->GetSubsystem<UTransportSubsystem>()->RestoreSaveState(S->Transport,Error))return false;
 auto* Crowd=W->GetSubsystem<UCrowdRenderSubsystem>();Crowd->EnsureTerrainVisualBuilt();
 if(!Crowd->RestoreSaveState(S->Crowd,Error))return false;
 for(TActorIterator<AAftershockCameraPawn> It(W);It;++It){It->InitializeCameraForGrid(S->Terrain.GridWidth,S->Terrain.GridHeight,S->Terrain.CellSize);if(!It->RestoreSaveState(S->Camera,Error))return false;break;}
 // Imported camera focus owns the final render window. These calls refresh
 // derived geometry from restored state, without replaying gameplay events.
 Crowd->EnsureTerrainVisualBuilt();
 if(auto* City=W->GetSubsystem<UCityPresentationSubsystem>())City->RefreshVisiblePresentation();
 if(auto* Coast=W->GetSubsystem<UCoastalPresentationSubsystem>())Coast->RefreshVisiblePresentation();
 if(auto* Weather=W->GetSubsystem<UDisasterPresentationSubsystem>())Weather->RefreshVisiblePresentation();
 W->GetSubsystem<UBuildingManagerSubsystem>()->RebuildMemorialVisuals();
 W->GetSubsystem<UAftershockDisasterDirectorSubsystem>()->OnDirectorChanged.Broadcast();W->GetSubsystem<UAftershockScenarioSubsystem>()->OnScenarioUpdated.Broadcast();
 if(S->Quiz.bQuestionPending)W->GetGameInstance()->GetSubsystem<UQuizSubsystem>()->OnQuizRequested.Broadcast(S->Quiz.CurrentQuestion);
 UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SAVE_RESTORED session=%s turn=%d residents=%d dead=%d vehicles=%d pendingFare=%lld phase=%d eventSequence=%lld treasury=%lld mode=%d region=%s focus=%s deathsReplayed=0"),*S->SessionID.ToString(),S->Economy.Turn,S->CityLife.State.LivingPopulation,S->CityLife.Residents.Num()-S->CityLife.State.LivingPopulation,S->Transport.Vehicles.Num(),S->CityLife.PendingTransitRevenueTL,static_cast<int32>(S->Director.Phase),S->Director.LastAppliedEventSequence,S->Economy.Treasury,static_cast<int32>(S->Settings.Mode),*S->Settings.RegionID.ToString(),*S->Settings.FocusID.ToString());
 bCommitted=true;
 return true;
}

bool UAftershockSaveSubsystem::WriteSnapshot(UAftershockSaveGame* S,const FString& Slot,FText& Error) const
{
 if(bWriting||bRestoring||bShuttingDown){Error=NSLOCTEXT("Aftershock","WriteDuringSave","Kayıt/yükleme zaten sürüyor.");return false;}
 // Public snapshots are mutable. A previous successful validation/read must
 // never let a caller bypass validation after changing any typed field.
 if(!S||SlotPath(Slot).IsEmpty())return false;
 FSaveStageTimings Timings;Timings.ValidationsBefore=SnapshotValidationCount;const double Started=FPlatformTime::Seconds();
 const bool Valid=ValidateSnapshot(*S,Error);Timings.ValidateMs=(FPlatformTime::Seconds()-Started)*1000.;
 return Valid&&WriteValidatedSnapshot(*S,Slot,Error,Timings);
}
bool UAftershockSaveSubsystem::IsValidatedDiskRevision(const FString& Path,const TArray<uint8>& Bytes) const
{
 // The actual complete envelope is hashed even when file size/mtime matches.
 // This cache only omits repeat typed deserialization/semantic validation of
 // identical, previously fully validated bytes; public reads still validate.
 const FString* Known=ValidatedDiskDigests.Find(Path);
 const bool Hit=Known&&*Known==EnvelopeDigest(Bytes);
 if(Hit)++ValidatedDiskDigestHits;else ++ValidatedDiskDigestMisses;return Hit;
}
void UAftershockSaveSubsystem::RememberValidatedDiskRevision(const FString& Path,const TArray<uint8>& Bytes) const
{
 if(!IsFixedSaveCandidate(Path)||Bytes.IsEmpty())return;
 ValidatedDiskDigests.Add(Path,EnvelopeDigest(Bytes));checkSlow(ValidatedDiskDigests.Num()<=15);
}
bool UAftershockSaveSubsystem::WriteValidatedSnapshot(UAftershockSaveGame& Snapshot,const FString& Slot,FText& Error,FSaveStageTimings& Timings) const
{
 auto* S=&Snapshot;const FString Path=SlotPath(Slot);if(Path.IsEmpty())return false;
 bool Written=false;
 ON_SCOPE_EXIT
 {
  UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SAVE_STAGES slot=%s captureMs=%.3f validateMs=%.3f serializeMs=%.3f compressMs=%.3f tempVerifyMs=%.3f backupMs=%.3f commitMs=%.3f snapshotValidations=%llu backupDigestHit=%d digestEntries=%d success=%d"),*Slot,Timings.CaptureMs,Timings.ValidateMs,Timings.SerializeMs,Timings.CompressMs,Timings.TempVerifyMs,Timings.BackupMs,Timings.CommitMs,SnapshotValidationCount-Timings.ValidationsBefore,Timings.bBackupDigestHit,ValidatedDiskDigests.Num(),Written);
 };
 double StageStarted=FPlatformTime::Seconds();
 TArray<uint8> Payload;if(!EncodeIndexedPayload(*S,Payload)||Payload.IsEmpty()||Payload.Num()>MaximumUncompressedSaveBytes){Error=FText::FromString(TEXT("Şehir kaydı güvenli serileştirme boyutunu aşıyor; eski kayıt korundu."));return false;}
 Timings.SerializeMs=(FPlatformTime::Seconds()-StageStarted)*1000.;StageStarted=FPlatformTime::Seconds();
 int32 Size=FCompression::CompressMemoryBound(NAME_Zlib,Payload.Num());TArray<uint8> Compressed;Compressed.SetNumUninitialized(Size);
 if(!FCompression::CompressMemory(NAME_Zlib,Compressed.GetData(),Size,Payload.GetData(),Payload.Num(),COMPRESS_BiasSpeed)||Size>MaximumSaveBytes-64)
 {Error=FText::FromString(TEXT("Şehir kaydı sıkıştırılamadı veya disk sınırını aştı; eski kayıt korundu."));return false;}
 Compressed.SetNum(Size,EAllowShrinking::No);
 TArray<uint8> Envelope;FMemoryWriter Writer(Envelope,true);uint32 Magic=SaveMagic;int32 Version=EnvelopeVersion,UncompressedSize=Payload.Num();uint8 Digest[FSHA1::DigestSize];FSHA1::HashBuffer(Payload.GetData(),Payload.Num(),Digest);
 Writer<<Magic;Writer<<Version;Writer<<Size;Writer<<UncompressedSize;Writer.Serialize(Digest,sizeof(Digest));Writer.Serialize(Compressed.GetData(),Compressed.Num());Writer.Close();
 Timings.CompressMs=(FPlatformTime::Seconds()-StageStarted)*1000.;
 // Envelope owns these bytes now. Do not retain another uncompressed city and
 // compressed copy while verifying disk output and reading the previous slot.
 Payload.Empty();Compressed.Empty();
 IPlatformFile& Files=FPlatformFileManager::Get().GetPlatformFile();Files.CreateDirectoryTree(*FPaths::GetPath(Path));const FString Temp=Path+TEXT(".tmp"),Backup=Path+TEXT(".bak");
 StageStarted=FPlatformTime::Seconds();const bool TempWritten=WriteFlushed(Temp,Envelope);Timings.TempVerifyMs=(FPlatformTime::Seconds()-StageStarted)*1000.;
 if(!TempWritten){Error=FText::FromString(TEXT("Yeni kayıt diske yazılamadı/doğrulanamadı; önceki kayıt korundu."));return false;}
 // Preserve only a validated old slot. A corrupt main can never overwrite its good backup.
 StageStarted=FPlatformTime::Seconds();TArray<uint8> OldBytes;
 if(ReadBytes(Path,OldBytes))
 {
  Timings.bBackupDigestHit=IsValidatedDiskRevision(Path,OldBytes);bool ValidPrevious=Timings.bBackupDigestHit;
  if(!ValidPrevious)if(auto* Previous=ReadTypedEnvelope(OldBytes))
  {
   TStrongObjectPtr<UAftershockSaveGame> KeepPrevious(Previous);ON_SCOPE_EXIT{ReleaseSnapshotPayload(*Previous);};
   FText OldError;ValidPrevious=ValidateSnapshot(*Previous,OldError);
   if(ValidPrevious)RememberValidatedDiskRevision(Path,OldBytes);
  }
  if(ValidPrevious)
  {
   const FString BackupTemp=Backup+TEXT(".tmp");if(!WriteFlushed(BackupTemp,OldBytes)||!ReplaceSameDirectory(BackupTemp,Backup)){Error=FText::FromString(TEXT("Kayıt yedeği atomik olarak oluşturulamadı; önceki kayıt korundu."));return false;}
   RememberValidatedDiskRevision(Backup,OldBytes);
  }
 }
 Timings.BackupMs=(FPlatformTime::Seconds()-StageStarted)*1000.;OldBytes.Empty();StageStarted=FPlatformTime::Seconds();
 const bool Replaced=ReplaceSameDirectory(Temp,Path);
 if(!Replaced){Error=FText::FromString(TEXT("Kayıt atomik olarak tamamlanamadı; yedek korundu."));return false;}
 RememberValidatedDiskRevision(Path,Envelope);ValidatedDiskDigests.Remove(Temp);Written=true;
 Timings.CommitMs=(FPlatformTime::Seconds()-StageStarted)*1000.;
 bHasContinueCache=false;
 UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SAVE_WRITTEN slot=%s bytes=%d schema=%d checksum=sha1 atomicReplace=1 session=%s turn=%d residents=%d dead=%d vehicles=%d pendingFare=%lld phase=%d eventSequence=%lld treasury=%lld mode=%d region=%s focus=%s"),*Slot,Envelope.Num(),S->SchemaVersion,*S->SessionID.ToString(),S->Economy.Turn,S->CityLife.State.LivingPopulation,S->CityLife.Residents.Num()-S->CityLife.State.LivingPopulation,S->Transport.Vehicles.Num(),S->CityLife.PendingTransitRevenueTL,static_cast<int32>(S->Director.Phase),S->Director.LastAppliedEventSequence,S->Economy.Treasury,static_cast<int32>(S->Settings.Mode),*S->Settings.RegionID.ToString(),*S->Settings.FocusID.ToString());return true;
}
UAftershockSaveGame* UAftershockSaveSubsystem::ReadSnapshot(const FString& Slot,FText& Error) const
{
 if(bWriting||bShuttingDown){Error=NSLOCTEXT("Aftershock","ReadDuringSave","Kayıt tamamlanmadan şehir yüklenemez.");return nullptr;}
 const FString Path=SlotPath(Slot);if(Path.IsEmpty()){Error=FText::FromString(TEXT("Geçersiz kayıt yuvası."));return nullptr;}
 for(const FString& Candidate:{Path,Path+TEXT(".bak"),Path+TEXT(".tmp")})
 {TArray<uint8> Bytes;if(ReadBytes(Candidate,Bytes))if(auto* S=ReadTypedEnvelope(Bytes))
  {if(ValidateSnapshot(*S,Error)){RememberValidatedDiskRevision(Candidate,Bytes);return S;}ReleaseSnapshotPayload(*S);}}
 Error=FText::FromString(TEXT("Geçerli kayıt bulunamadı; bozuk dosyalar değiştirilmedi."));return nullptr;
}
bool UAftershockSaveSubsystem::SaveNow(UWorld* W,const FString& Slot,FText& Error)
{
 if(IsWorldLoadFailed(W)){Error=FText::Format(NSLOCTEXT("Aftershock","LoadFailureCannotSave","Yüklenemeyen şehir kaydedilemez. Önce Yeni Oyun veya Devam Et ile sağlam bir şehir açın.\n{0}"),LoadFailure);return false;}
 if(bWriting||bRestoring||bShuttingDown){Error=FText::FromString(TEXT("Kayıt/yükleme zaten sürüyor; tamamlanmasını bekleyin."));return false;}
 TGuardValue<bool> Guard(bWriting,true);
 bSaveInProgress.store(true,std::memory_order_release);ON_SCOPE_EXIT{bSaveInProgress.store(false,std::memory_order_release);};
 if(!InternalSaveScratch)InternalSaveScratch=NewObject<UAftershockSaveGame>(this);
 ON_SCOPE_EXIT{ReleaseSnapshotPayload(*InternalSaveScratch);};
 FSaveStageTimings Timings;Timings.ValidationsBefore=SnapshotValidationCount;
 const double Started=FPlatformTime::Seconds();const bool Captured=CaptureInto(W,*InternalSaveScratch,Error,&Timings.ValidateMs);
 Timings.CaptureMs=FMath::Max(0.,(FPlatformTime::Seconds()-Started)*1000.-Timings.ValidateMs);
 // No delegate or timer can mutate this private scratch between capture's
 // validation and serialization on the same game-thread call stack.
 const bool Good=Captured&&WriteValidatedSnapshot(*InternalSaveScratch,Slot,Error,Timings);
 UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SAVE_CPU slot=%s elapsedMs=%.3f synchronous=1 privateScratchObjects=1 payloadReleasedAfterReturn=1 success=%d"),*Slot,(FPlatformTime::Seconds()-Started)*1000.,Good);
 if(Good)Error=FText::FromString(TEXT("Şehir güvenle kaydedildi."));OnSaveCompleted.Broadcast(Good,Error);return Good;
}
bool UAftershockSaveSubsystem::SaveAndExit(UWorld* W,FText& Error)
{if(!SaveNow(W,TEXT("Manual"),Error))return false;UKismetSystemLibrary::QuitGame(W,W?W->GetFirstPlayerController():nullptr,EQuitPreference::Quit,false);return true;}
bool UAftershockSaveSubsystem::HasContinueSave() const
{
 FString Signature;
 for(const TCHAR* Slot:{TEXT("Manual"),TEXT("Auto0"),TEXT("Auto1"),TEXT("Auto2")})
 {const FString Path=SlotPath(Slot);for(const FString& Candidate:{Path,Path+TEXT(".bak"),Path+TEXT(".tmp")})Signature+=FString::Printf(TEXT("%lld:%lld;"),IFileManager::Get().FileSize(*Candidate),IFileManager::Get().GetTimeStamp(*Candidate).GetTicks());}
 if(bHasContinueCache&&ContinueFileSignature==Signature)return bCachedHasContinue;
 FText Error;bool Found=false;
 for(const TCHAR* Slot:{TEXT("Manual"),TEXT("Auto0"),TEXT("Auto1"),TEXT("Auto2")})if(auto* S=ReadSnapshot(Slot,Error)){ReleaseSnapshotPayload(*S);Found=true;break;}
 ContinueFileSignature=MoveTemp(Signature);bHasContinueCache=true;bCachedHasContinue=Found;return Found;
}
bool UAftershockSaveSubsystem::QueueContinue(UWorld* W,FText& Error)
{
 if(bWriting||bRestoring||bShuttingDown){Error=NSLOCTEXT("Aftershock","ContinueDuringSave","Kayıt/yükleme zaten sürüyor; tamamlanmasını bekleyin.");return false;}
 if(!W)return false;TStrongObjectPtr<UAftershockSaveGame> KeepLatest;UAftershockSaveGame* Latest=nullptr;
 for(const TCHAR* Slot:{TEXT("Manual"),TEXT("Auto0"),TEXT("Auto1"),TEXT("Auto2")})if(auto* S=ReadSnapshot(Slot,Error))
 {if(!Latest||S->SavedAt>Latest->SavedAt){if(Latest)ReleaseSnapshotPayload(*Latest);KeepLatest.Reset(S);Latest=S;}else ReleaseSnapshotPayload(*S);}
 if(!Latest){Error=FText::FromString(TEXT("Devam edilebilecek sağlam bir kayıt yok."));return false;}
 PendingLoad=Latest;GetGameInstance()->GetSubsystem<UAftershockOpeningSessionSubsystem>()->RestoreSession(Latest->Settings,Latest->Campaign,Latest->SessionID);
 UGameplayStatics::OpenLevel(W,FName(*Latest->MapPackage));return true;
}
bool UAftershockSaveSubsystem::RestorePendingWorld(UWorld* W,AAftershockCameraPawn* Camera,FText& Error)
{
 if(bWriting||bShuttingDown){Error=NSLOCTEXT("Aftershock","PendingDuringSave","Kayıt tamamlanmadan şehir yüklenemez.");return false;}
 if(!PendingLoad)return false;
 UAftershockSaveGame* Saved=PendingLoad;const bool Good=RestoreWorld(W,Saved,Error);PendingLoad=nullptr;
 if(Good){FailedLoadWorld.Reset();LoadFailure=FText::GetEmpty();}
 else if(W){FailedLoadWorld=W;LoadFailure=Error.IsEmpty()?NSLOCTEXT("Aftershock","LoadFailureUnknown","Şehir yüklenemedi. Kayıt dosyaları korunuyor."):Error;Error=LoadFailure;}
 return Good;
}
void UAftershockSaveSubsystem::Autosave(UWorld* W)
{
 // Render fixtures must never replace a player's manual/autosave history.
 if(!W||IsRunningCommandlet()||IsWorldLoadFailed(W)||bRestoring||bWriting||bShuttingDown||FParse::Param(FCommandLine::Get(),TEXT("AftershockVisualAudit")))return;
 FText Error;if(!BeginAutosaveWrite(W,FString::Printf(TEXT("Auto%d"),NextAutosave%3),Error)){UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_AUTOSAVE_FAILED %s"),*Error.ToString());}
}

