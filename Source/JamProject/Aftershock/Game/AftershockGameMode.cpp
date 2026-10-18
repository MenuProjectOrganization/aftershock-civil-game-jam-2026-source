#include "Aftershock/Game/AftershockGameMode.h"
#include "Aftershock/Public/AftershockStartupPreparationSubsystem.h"
#include "Aftershock/Game/AftershockGameState.h"
#include "Aftershock/Game/AftershockPlayerController.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/AftershockDisasterDirectorSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/World/GridManager.h"
#include "Aftershock/Data/DisasterData.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

AAftershockGameMode::AAftershockGameMode()
{
 GameStateClass=AAftershockGameState::StaticClass();PlayerControllerClass=AAftershockPlayerController::StaticClass();DefaultPawnClass=AAftershockCameraPawn::StaticClass();
}
void AAftershockGameMode::BeginPlay()
{
 Super::BeginPlay();
 for(TActorIterator<AGridManager> It(GetWorld());It;++It){GridManager=*It;break;}
 // Compatibility reference remains null: canonical world subsystems own simulation.
 Simulator=nullptr;
 if(auto* Economy=GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>())
 {
  Economy->OnGameOver.AddUniqueDynamic(this,&AAftershockGameMode::HandleCanonicalGameOver);
  Economy->OnEconomyUpdated.AddUniqueDynamic(this,&AAftershockGameMode::HandleCanonicalEconomy);
  HandleCanonicalEconomy(Economy->TreasuryTL,Economy->PublicSatisfaction,Economy->CurrentTurn);
 }
 if(auto* Scenario=GetWorld()->GetSubsystem<UAftershockScenarioSubsystem>())Scenario->OnScenarioUpdated.AddUniqueDynamic(this,&AAftershockGameMode::HandleCanonicalScenario);
 GetWorld()->GetTimerManager().SetTimer(FlowTimer,this,&AAftershockGameMode::EvaluateGameFlow,.25f,true);
}
void AAftershockGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
 if(UWorld* World=GetWorld())
 {
  World->GetTimerManager().ClearTimer(FlowTimer);
  if(auto* Economy=World->GetSubsystem<UEconomyAndStateSubsystem>()){Economy->OnGameOver.RemoveDynamic(this,&AAftershockGameMode::HandleCanonicalGameOver);Economy->OnEconomyUpdated.RemoveDynamic(this,&AAftershockGameMode::HandleCanonicalEconomy);}
  if(auto* Scenario=World->GetSubsystem<UAftershockScenarioSubsystem>())Scenario->OnScenarioUpdated.RemoveDynamic(this,&AAftershockGameMode::HandleCanonicalScenario);
 }
 Super::EndPlay(Reason);
}
void AAftershockGameMode::BeginLevel(){EvaluateGameFlow();}
void AAftershockGameMode::BeginIntro()
{
 if(GameFlowState==EGameFlowState::Epilogue)return;GameFlowState=EGameFlowState::Intro;
 UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_NARRATIVE_FLOW state=Intro canonicalWorldSimulation=1"));
}
void AAftershockGameMode::FinishIntro()
{
 if(GameFlowState!=EGameFlowState::Intro)return;GameFlowState=EGameFlowState::Playing;
 UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_NARRATIVE_FLOW state=Playing introCompleted=1"));
}
void AAftershockGameMode::ScheduleCurrentDisaster()
{
 if(!LevelDisaster||IsNarrativeInputBlocked())return;
 FText Error;const bool Accepted=GetWorld()->GetSubsystem<UAftershockDisasterDirectorSubsystem>()->RequestWarning(LevelDisaster->Type,FMath::Max(.1f,LevelWarningSeconds),Error);
 UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_LEGACY_SCHEDULE_CANONICAL accepted=%d error=%s duplicateSimulator=0"),Accepted,*Error.ToString());
}
AAftershockGameState* AAftershockGameMode::GetAftershockState() const{return GetGameState<AAftershockGameState>();}
void AAftershockGameMode::HandleCanonicalEconomy(int64 Treasury,float Satisfaction,int32)
{
 // A read-only compatibility mirror, never another owner of money or deaths.
 if(auto* State=GetAftershockState()){State->Budget=static_cast<int32>(FMath::Clamp<int64>(Treasury,MIN_int32,MAX_int32));State->Satisfaction=Satisfaction;State->CurrentLevel=GetWorld()->GetSubsystem<UAftershockScenarioSubsystem>()->GetScenarioIndex()+1;State->TotalLevels=UAftershockScenarioSubsystem::GetScenarioCount();}
}
void AAftershockGameMode::HandleCanonicalGameOver(FText Reason){EndingReason=Reason;EvaluateGameFlow();}
void AAftershockGameMode::HandleCanonicalScenario(){EvaluateGameFlow();}
void AAftershockGameMode::EvaluateGameFlow()
{
 if(const auto* Startup=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockStartupPreparationSubsystem>():nullptr)if(Startup->IsGameplayBlocked(GetWorld()))return;
 if(GameFlowState!=EGameFlowState::Playing||!GetWorld())return;
#if !UE_BUILD_SHIPPING
 // Existing visual fixtures keep running their maps after a canonical loss.
 // The explicit narrative fixture exercises the actual production ending.
 if(FParse::Param(FCommandLine::Get(),TEXT("AftershockVisualAudit"))&&!FParse::Param(FCommandLine::Get(),TEXT("AftershockNarrativeAudit")))return;
#endif
 const auto* Saves=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>():nullptr;
 if(Saves&&Saves->IsRestoring())return;
 const auto* Economy=GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>();const auto* Scenario=GetWorld()->GetSubsystem<UAftershockScenarioSubsystem>();
 if(!Economy||!Scenario)return;
 const auto* Disaster=GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>();
 if(Disaster)
 {
	  if(Disaster->IsPhysicalEventRunning())return;
  const auto& Visual=Disaster->GetLatestVisualSnapshot();const float Duration=Visual.GetEffectiveDurationSeconds();
  if(Visual.Revision>0&&GetWorld()->GetTimeSeconds()<Visual.StartedAtWorldSeconds+Duration)return;
 }
 if(Economy->bIsGameOver){EnterEpilogue(false,EndingReason.IsEmpty()?NSLOCTEXT("Aftershock","CanonicalEndingReason","Şehir yönetimi sona erdi."):EndingReason);return;}
 const auto* Session=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockOpeningSessionSubsystem>():nullptr;
 if(Session&&Session->GetSettings().Mode==EAftershockPlayMode::Campaign&&Scenario->IsFinalScenario()&&Scenario->CanAdvanceScenario())
  EnterEpilogue(true,NSLOCTEXT("Aftershock","CampaignEndingReason","Yedi kampanya görevinin hedefleri tamamlandı."));
}
void AAftershockGameMode::EnterEpilogue(bool bSuccess,FText Reason)
{
 if(GameFlowState==EGameFlowState::Epilogue)return;
 GameFlowState=EGameFlowState::Epilogue;bCampaignSuccessful=bSuccess;EndingReason=Reason;
 UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_NARRATIVE_FLOW state=Epilogue campaignSuccess=%d canonicalGameOverOrFinalCampaignOnly=1"),bSuccess);
 if(auto* PC=Cast<AAftershockPlayerController>(GetWorld()->GetFirstPlayerController()))PC->PresentWaterGlassEpilogue(bSuccess,Reason);
}
void AAftershockGameMode::HandleDisasterComplete(const FLevelOutcome&)
{ UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_LEGACY_OUTCOME_IGNORED canonicalSubsystemOwnsEconomy=1")); }
