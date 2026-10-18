#include "Aftershock/Public/AftershockDisasterDirectorSubsystem.h"
#include "Aftershock/Public/AftershockStartupPreparationSubsystem.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/AftershockRegionRules.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "AftershockPhysicalLifecycle.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "TimerManager.h"
namespace AftershockDirectorHazardPolicy
{
 float EligibleWeight(const UDisasterSimulationSubsystem* Sim,int32 Region,int32 Focus,EDisasterType Type)
 {
  if(!Sim||!IsAftershockFocusHazardAllowed(Region,Focus,Type))return 0.f;
  FText Reason;if(!Sim->CanTriggerDisaster(Type,Reason))return 0.f;
  const float Physical=Sim->GetPhysicalHazardWeight(Type),Regional=GetAftershockHazardWeight(Region,Type,Focus);
  if(!FMath::IsFinite(Physical)||Physical<=0.f||!FMath::IsFinite(Regional)||Regional<=0.f)return 0.f;
  return Regional*FMath::Clamp(Physical,0.f,1.f); // Old seven have Physical==1, preserving the original weights.
 }
}
bool UAftershockDisasterDirectorSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{const auto* W=Cast<UWorld>(Outer);return W&&(W->WorldType==EWorldType::Game||W->WorldType==EWorldType::PIE);}
void UAftershockDisasterDirectorSubsystem::Initialize(FSubsystemCollectionBase& C)
{
 Super::Initialize(C);C.InitializeDependency<UEconomyAndStateSubsystem>();C.InitializeDependency<UAftershockScenarioSubsystem>();
 GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>()->OnEconomyUpdated.AddUniqueDynamic(this,&UAftershockDisasterDirectorSubsystem::HandleEconomyUpdated);
 GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>()->OnDisasterCompleted.AddUniqueDynamic(this,&UAftershockDisasterDirectorSubsystem::HandleDisasterCompleted);
}
FAftershockSessionSettings UAftershockDisasterDirectorSubsystem::Settings() const
{
 if(const UGameInstance* GI=GetWorld()->GetGameInstance())if(const auto* S=GI->GetSubsystem<UAftershockOpeningSessionSubsystem>())if(S->HasConfiguredSession())return S->GetSettings();
 FAftershockSessionSettings Result;if(const auto* Scenario=GetWorld()->GetSubsystem<UAftershockScenarioSubsystem>())
 {Result.CampaignStage=Scenario->GetScenarioIndex();Result.RegionID=FName(GetAftershockGeography(Scenario->GetDefinition().GeographyIndex).Id);Result.Seed=Scenario->GetDefinition().Seed;}return Result;
}
void UAftershockDisasterDirectorSubsystem::OnWorldBeginPlay(UWorld& W)
{
 Super::OnWorldBeginPlay(W);if(!bRestored){Random.Initialize(Settings().Seed);State.RandomSeed=Random.GetCurrentSeed();}
 W.GetTimerManager().SetTimer(ClockTimer,this,&UAftershockDisasterDirectorSubsystem::AdvanceClock,.25f,true);
}
bool UAftershockDisasterDirectorSubsystem::RequestWarning(EDisasterType Type,float Seconds,FText& Error)
{
 auto* Economy=GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>();auto* Sim=GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>();
 if(!FMath::IsFinite(Seconds)||Seconds<1.f||Seconds>3600.f){Error=FText::FromString(TEXT("Hazırlık süresi geçersiz."));return false;}
 if(State.Phase!=EAftershockDisasterPhase::Ready||Economy->bIsGameOver||Sim->IsPhysicalEventRunning()){Error=FText::FromString(TEXT("Devam eden olay tamamlanmalı."));return false;}
 const auto S=Settings();const int32 R=FindAftershockGeographyIndex(S.RegionID);const int32 F=S.FocusID.IsNone()?0:FindAftershockGeographyFocusIndex(R,S.FocusID);
 const float PhysicalWeight=Sim->GetPhysicalHazardWeight(Type);
 if(!IsAftershockFocusHazardAllowed(R,F,Type)||!Sim->CanTriggerDisaster(Type,Error)||!FMath::IsFinite(PhysicalWeight)||PhysicalWeight<=0.f)
 {if(Error.IsEmpty())Error=FText::FromString(TEXT("Bu afet seçilen coğrafyada uygun değil."));return false;}
 State.PendingType=Type;State.WarningDuration=FMath::Max(1.f,Seconds);State.RemainingSeconds=State.WarningDuration;State.Phase=EAftershockDisasterPhase::Warning;
 const auto* Scenario=GetWorld()->GetSubsystem<UAftershockScenarioSubsystem>();State.Severity=(Scenario?Scenario->GetScenarioSeverityForHazard(Type):42.f)*S.Difficulty;
 State.Severity=FMath::Clamp(State.Severity,0.f,95.f);++State.EventSequence;
 if(auto* Crowd=GetWorld()->GetSubsystem<UCrowdRenderSubsystem>())Crowd->BeginEmergencyEvacuation(Type);
 OnDirectorChanged.Broadcast();return true;
}
bool UAftershockDisasterDirectorSubsystem::TriggerPendingNow(FText& Error)
{
 if(State.Phase!=EAftershockDisasterPhase::Warning){Error=FText::FromString(TEXT("Hazır bir afet uyarısı yok."));return false;}
 auto* Sim=GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>();if(!Sim->CanTriggerDisaster(State.PendingType,Error))return false;
 TGuardValue<bool> Guard(bTriggering,true);const auto Report=Sim->TriggerDisaster(State.PendingType,State.Severity,.25f,true);
 if(Report.bWasRejected){State.Phase=EAftershockDisasterPhase::Ready;State.RemainingSeconds=0.f;Error=Report.RejectionReason;return false;}
 if(Report.bIsPending)
 {
  // Keep the one requested ordinal pending; no LastReport/final money/deaths.
  State.RemainingSeconds=0.f;OnDirectorChanged.Broadcast();
 }
 return true;
}
void UAftershockDisasterDirectorSubsystem::HandleDisasterCompleted(const FDisasterReport& Report)
{
 // This handler observes an already committed canonical result. Restore never
 // rebroadcasts completion, so aid/deaths/fares cannot be applied a second time.
 if(Report.bIsPending||Report.bNeedsCasualtyEvent)return;
 if(Report.bWasRejected)
 {
  if(State.Phase==EAftershockDisasterPhase::Warning&&State.PendingType==Report.DisasterType&&State.EventSequence>State.LastAppliedEventSequence)
  {State.Phase=EAftershockDisasterPhase::Ready;State.RemainingSeconds=0.f;OnDirectorChanged.Broadcast();}
  return; // Failed work is not a settled report and never increments/applies an event.
 }
 if(Report.DisasterEventID.IsValid()&&State.LastReport.DisasterEventID==Report.DisasterEventID)return;
 const bool ExpectedPending=State.Phase==EAftershockDisasterPhase::Warning&&State.PendingType==Report.DisasterType&&State.EventSequence>State.LastAppliedEventSequence;
 if(!bTriggering&&!ExpectedPending)++State.EventSequence;
 State.LastAppliedEventSequence=State.EventSequence;State.LastReport=Report;State.Phase=EAftershockDisasterPhase::Aftermath;
 State.PendingType=Report.DisasterType;State.Severity=Report.BaseSeverity;
 State.LastTriggerTurn=GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>()->CurrentTurn;
 const auto& V=GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>()->GetLatestVisualSnapshot();
 // Async water/mass can settle after its visual timeline has already elapsed.
 // Retain only the original timeline's remainder; never add a second full event.
 State.RemainingSeconds=AftershockPhysicalLifecycle::RemainingVisualSeconds(
  GetWorld()->GetTimeSeconds(),V.StartedAtWorldSeconds,V.GetEffectiveDurationSeconds());
 OnDirectorChanged.Broadcast();
}
void UAftershockDisasterDirectorSubsystem::AdvanceClock()
{
 if(const auto* Startup=GetWorld()->GetGameInstance()?GetWorld()->GetGameInstance()->GetSubsystem<UAftershockStartupPreparationSubsystem>():nullptr)if(Startup->IsGameplayBlocked(GetWorld()))return;
 SecondsSinceAutosave+=.25f;
 if(SecondsSinceAutosave>=120.f){SecondsSinceAutosave=0.f;if(auto* GI=GetWorld()->GetGameInstance())if(GI->GetSubsystem<UAftershockOpeningSessionSubsystem>()->HasLiveSession())GI->GetSubsystem<UAftershockSaveSubsystem>()->Autosave(GetWorld());}
 if(GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>()->IsPhysicalEventRunning())
 {
  // The canonical worker owns completion time. A zero warning countdown must
  // not retry TriggerDisaster every .25s, reset Ready or invent a final report.
  if(State.Phase==EAftershockDisasterPhase::Warning)State.RemainingSeconds=0.f;
  OnDirectorChanged.Broadcast();return;
 }
 if(State.Phase==EAftershockDisasterPhase::Ready)return;
 State.RemainingSeconds=FMath::Max(0.f,State.RemainingSeconds-.25f);
 if(State.RemainingSeconds<=0.f)
 {
  if(State.Phase==EAftershockDisasterPhase::Warning){FText Error;if(!TriggerPendingNow(Error)){State.Phase=EAftershockDisasterPhase::Ready;UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_DIRECTOR_REJECTED %s"),*Error.ToString());}}
  else {State.Phase=EAftershockDisasterPhase::Ready;if(auto* GI=GetWorld()->GetGameInstance())GI->GetSubsystem<UAftershockSaveSubsystem>()->Autosave(GetWorld());}
 }
 OnDirectorChanged.Broadcast();
}
void UAftershockDisasterDirectorSubsystem::HandleEconomyUpdated(int64 Treasury,float Satisfaction,int32 Turn)
{
 if(const auto* Startup=GetWorld()->GetGameInstance()?GetWorld()->GetGameInstance()->GetSubsystem<UAftershockStartupPreparationSubsystem>():nullptr)if(Startup->IsGameplayBlocked(GetWorld()))return;
 if(const auto* GI=GetWorld()->GetGameInstance())if(GI->GetSubsystem<UAftershockSaveSubsystem>()->IsRestoring())return;
 if(Turn<=State.LastProcessedTurn)return;State.LastProcessedTurn=Turn;
 const auto S=Settings();if(S.Mode!=EAftershockPlayMode::Sandbox&&State.Phase==EAftershockDisasterPhase::Ready&&Turn>=3&&Turn-State.LastTriggerTurn>=3)
 {
  EDisasterType Type=GetWorld()->GetSubsystem<UAftershockScenarioSubsystem>()->GetDefinition().Hazard;
  if(S.Mode==EAftershockPlayMode::GeographicSimulation)
  {
   const int32 R=FindAftershockGeographyIndex(S.RegionID),F=S.FocusID.IsNone()?0:FindAftershockGeographyFocusIndex(R,S.FocusID);
   constexpr int32 Count=static_cast<int32>(EDisasterType::Landslide)+1;
   float Weights[Count]={};float Total=0.f;const auto* Sim=GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>();
   for(int32 I=0;I<Count;++I){Weights[I]=AftershockDirectorHazardPolicy::EligibleWeight(Sim,R,F,static_cast<EDisasterType>(I));Total+=Weights[I];}
   if(Total>0.f)
   {
    float Pick=Random.FRand()*Total; // Exactly one draw, same ordinal and 0..6 ordering when new weights are zero.
    for(int32 I=0;I<Count;++I){if(Weights[I]<=0.f)continue;Pick-=Weights[I];if(Pick<=0.f){Type=static_cast<EDisasterType>(I);break;}}
    State.RandomSeed=Random.GetCurrentSeed();
   }
  }
  FText Error;RequestWarning(Type,25.f,Error);
 }
 if(auto* GI=GetWorld()->GetGameInstance())GI->GetSubsystem<UAftershockSaveSubsystem>()->Autosave(GetWorld());
}
bool UAftershockDisasterDirectorSubsystem::RestoreSaveState(const FAftershockDirectorState& S,FText& Error)
{
 const auto SettingsValue=Settings();const int32 R=FindAftershockGeographyIndex(SettingsValue.RegionID),F=SettingsValue.FocusID.IsNone()?0:FindAftershockGeographyFocusIndex(R,SettingsValue.FocusID);
 if(static_cast<uint8>(S.Phase)>2||static_cast<uint8>(S.PendingType)>static_cast<uint8>(EDisasterType::Landslide)||S.LastReport.bIsPending||S.LastReport.bWasRejected||S.RemainingSeconds<0||!FMath::IsFinite(S.RemainingSeconds)||S.LastAppliedEventSequence>S.EventSequence||S.EventSequence<0||S.LastAppliedEventSequence<0||!FMath::IsFinite(S.Severity)||S.Severity<0||S.Severity>95||!FMath::IsFinite(S.WarningDuration)||S.WarningDuration<1||S.WarningDuration>3600
  ||(S.Phase==EAftershockDisasterPhase::Ready&&S.RemainingSeconds!=0)
  ||(S.Phase==EAftershockDisasterPhase::Warning&&(S.RemainingSeconds>S.WarningDuration||S.EventSequence<=S.LastAppliedEventSequence||!IsAftershockFocusHazardAllowed(R,F,S.PendingType)))
  ||(S.Phase==EAftershockDisasterPhase::Aftermath&&(S.EventSequence!=S.LastAppliedEventSequence||S.LastReport.DisasterType!=S.PendingType)))
 {Error=FText::FromString(TEXT("Afet akışı kaydı geçersiz."));return false;}
 State=S;Random.Initialize(S.RandomSeed);bRestored=true;return true;
}
void UAftershockDisasterDirectorSubsystem::Deinitialize()
{
 if(GetWorld()){GetWorld()->GetTimerManager().ClearTimer(ClockTimer);GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>()->OnEconomyUpdated.RemoveDynamic(this,&UAftershockDisasterDirectorSubsystem::HandleEconomyUpdated);GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>()->OnDisasterCompleted.RemoveDynamic(this,&UAftershockDisasterDirectorSubsystem::HandleDisasterCompleted);}
 Super::Deinitialize();
}
