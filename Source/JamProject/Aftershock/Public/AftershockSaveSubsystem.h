#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include <atomic>
#include "AftershockSaveSubsystem.generated.h"
class UAftershockSaveGame;
class AAftershockCameraPawn;
struct FAftershockAsyncSaveJob;
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnAftershockSaveCompleted,bool,bSuccess,FText,Message);
UCLASS()
class JAMPROJECT_API UAftershockSaveSubsystem final:public UGameInstanceSubsystem
{
 GENERATED_BODY()
public:
 virtual void Deinitialize() override;
 UPROPERTY(BlueprintAssignable) FOnAftershockSaveCompleted OnSaveCompleted;
 /** Thread-safe flag: true while a save job is encoding or writing. Bind HUD indicator to this. */
 std::atomic<bool> bSaveInProgress{false};
 UFUNCTION(BlueprintPure, Category="Aftershock|Save")
 bool IsSaveInProgress() const { return bSaveInProgress.load(std::memory_order_acquire); }
 UAftershockSaveGame* CaptureWorld(UWorld* World,FText& Error);
 bool RestoreWorld(UWorld* World,UAftershockSaveGame* Saved,FText& Error);
 bool SaveNow(UWorld* World,const FString& Slot,FText& Error);
 bool SaveAndExit(UWorld* World,FText& Error);
 bool QueueContinue(UWorld* World,FText& Error);
 bool RestorePendingWorld(UWorld* World, AAftershockCameraPawn* Camera,FText& Error);
 bool HasPendingLoad() const { return PendingLoad!=nullptr; }
 bool HasContinueSave() const;
 bool IsRestoring() const { return bRestoring; }
 bool IsWorldLoadFailed(const UWorld* World) const;
 /** Startup data failure shares the existing opening recovery/save-block contract. */
 void MarkWorldStartupFailure(UWorld* World,const FText& Error);
 FText GetLoadFailure(const UWorld* World) const { return IsWorldLoadFailed(World)?LoadFailure:FText::GetEmpty(); }
 void Autosave(UWorld* World);
 bool ValidateSnapshot(UAftershockSaveGame& Saved,FText& Error) const;
 bool WriteSnapshot(UAftershockSaveGame* Saved,const FString& Slot,FText& Error) const;
 UAftershockSaveGame* ReadSnapshot(const FString& Slot,FText& Error) const;
 static FString SlotPath(const FString& Slot);
private:
 friend class UAftershockSessionSaveAuditCommandlet;
 friend class UAftershockSaveCodecAuditCommandlet;
 struct FSaveStageTimings
 {
  double CaptureMs=0,ValidateMs=0,SerializeMs=0,CompressMs=0,TempVerifyMs=0,BackupMs=0,CommitMs=0;
  uint64 ValidationsBefore=0;
  bool bBackupDigestHit=false;
 };
 bool CaptureInto(UWorld* World,UAftershockSaveGame& Snapshot,FText& Error,double* OutValidationMs=nullptr);
 static bool EncodeIndexedPayload(UAftershockSaveGame& Saved,TArray<uint8>& Payload);
 static UAftershockSaveGame* DecodeIndexedPayload(const TArray<uint8>& Payload);
 static UAftershockSaveGame* ReadTypedEnvelope(const TArray<uint8>& Bytes);
 /** Caller owns a private, just-validated snapshot; never exposed to Blueprint/public callers. */
 bool WriteValidatedSnapshot(UAftershockSaveGame& Saved,const FString& Slot,FText& Error,FSaveStageTimings& Timings) const;
 bool IsValidatedDiskRevision(const FString& Path,const TArray<uint8>& Bytes) const;
 void RememberValidatedDiskRevision(const FString& Path,const TArray<uint8>& Bytes) const;
 bool BeginAutosaveWrite(UWorld* World,const FString& Slot,FText& Error);
 bool CompletePendingAutosave(bool bWait,bool bBroadcast=true);
 TSharedPtr<FAftershockAsyncSaveJob,ESPMode::ThreadSafe> ActiveAutosaveJob;
 // At most five fixed slots times main/backup/temp. Digests only, never city UObjects or payloads.
 mutable TMap<FString,FString> ValidatedDiskDigests;
 mutable uint64 SnapshotValidationCount=0,ValidatedDiskDigestHits=0,ValidatedDiskDigestMisses=0;
 UPROPERTY(Transient) TObjectPtr<UAftershockSaveGame> InternalSaveScratch;
 mutable FString ContinueFileSignature;
 mutable bool bHasContinueCache=false,bCachedHasContinue=false;
 UPROPERTY() TObjectPtr<UAftershockSaveGame> PendingLoad;
 UPROPERTY(Transient) TWeakObjectPtr<UWorld> FailedLoadWorld;
 FText LoadFailure;
 bool bRestoring=false;
 bool bRollingBack=false;
 bool bWriting=false;
 bool bShuttingDown=false;
 int32 NextAutosave=0;
};
