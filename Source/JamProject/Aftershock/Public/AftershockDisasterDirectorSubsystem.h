#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Aftershock/Public/AftershockSessionTypes.h"
#include "AftershockDisasterDirectorSubsystem.generated.h"
DECLARE_MULTICAST_DELEGATE(FOnAftershockDirectorChanged);
UCLASS()
class JAMPROJECT_API UAftershockDisasterDirectorSubsystem final:public UWorldSubsystem
{
 GENERATED_BODY()
 friend class UAftershockHUDInputAuditCommandlet;
public:
 virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
 virtual void Initialize(FSubsystemCollectionBase& Collection) override;
 virtual void OnWorldBeginPlay(UWorld& World) override;
 virtual void Deinitialize() override;
 const FAftershockDirectorState& GetState() const { return State; }
 bool RequestWarning(EDisasterType Type,float Seconds,FText& Error);
 bool TriggerPendingNow(FText& Error);
 void CaptureSaveState(FAftershockDirectorState& Out) const { Out=State; }
 bool RestoreSaveState(const FAftershockDirectorState& Saved,FText& Error);
 FOnAftershockDirectorChanged OnDirectorChanged;
private:
 UFUNCTION() void HandleEconomyUpdated(int64 Treasury,float Satisfaction,int32 Turn);
 UFUNCTION() void HandleDisasterCompleted(const FDisasterReport& Report);
 void AdvanceClock();
 FAftershockSessionSettings Settings() const;
 UPROPERTY() FAftershockDirectorState State;
 FRandomStream Random;
 FTimerHandle ClockTimer;
 bool bTriggering=false;
 bool bRestored=false;
 float SecondsSinceAutosave=0.f;
};
