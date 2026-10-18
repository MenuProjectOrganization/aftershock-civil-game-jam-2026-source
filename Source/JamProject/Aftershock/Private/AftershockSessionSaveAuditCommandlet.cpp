#include "AftershockSessionSaveAuditCommandlet.h"
#include "AftershockSaveBinarySchema.h"
#include "AftershockHydraulicTerrain.h"
#include "AftershockMassMovementVisual.h"
#include "AftershockPhysicalReplay.h"
#if WITH_EDITOR
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Aftershock/Public/AftershockSaveGame.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockDisasterDirectorSubsystem.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Quiz/QuizSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Misc/Compression.h"
#include "Misc/SecureHash.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/StrongObjectPtr.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/TextBlock.h"
#include "UObject/UnrealType.h"
#include "UObject/ObjectVersion.h"
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/ISlateNullRendererModule.h"
#include "Modules/ModuleManager.h"
#include "Null/NullApplication.h"
#include <limits>

namespace
{
 struct FScopedSessionAuditSlate
 {
  bool bOwnsSlate=false;
  FScopedSessionAuditSlate()
  {
   if(FSlateApplication::IsInitialized())return;
   if(auto* NullRenderer=FModuleManager::LoadModulePtr<ISlateNullRendererModule>(TEXT("SlateNullRenderer")))
   {
    const TSharedRef<GenericApplication> HeadlessApplication=MakeShareable(FNullApplication::CreateNullApplication());
    FSlateApplication::InitializeAsStandaloneApplication(NullRenderer->CreateSlateNullRenderer(),HeadlessApplication);
    bOwnsSlate=FSlateApplication::IsInitialized();
   }
  }
  ~FScopedSessionAuditSlate()
  {
   if(bOwnsSlate&&FSlateApplication::IsInitialized())FSlateApplication::Shutdown();
  }
 };
}

UAftershockSessionSaveAuditCommandlet::UAftershockSessionSaveAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}
int32 UAftershockSessionSaveAuditCommandlet::Main(const FString& Params)
{
 int32 Passed=0,Failed=0;TArray<FString> Results;
 const auto Check=[&](const TCHAR* Name,bool Good)
 {Good?++Passed:++Failed;Results.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"),Name,Good?TEXT("true"):TEXT("false")));UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SESSION_SAVE_CHECK %s passed=%d"),Name,Good);};
 const auto Finish=[&]()
 {const FString Dir=FPaths::ProjectSavedDir()/TEXT("PresentationValidation");IFileManager::Get().MakeDirectory(*Dir,true);const FString Json=FString::Printf(TEXT("{\"schema_version\":1,\"scope\":\"Isolated real GameInstance/world typed save, disk checksum and backup, owner restore and mode contracts. Does not prove rendered UI or process restart.\",\"passed\":%d,\"failed\":%d,\"checks\":[%s]}"),Passed,Failed,*FString::Join(Results,TEXT(",")));FFileHelper::SaveStringToFile(Json,*(Dir/TEXT("session_save_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SESSION_SAVE_RESULT passed=%d failed=%d"),Passed,Failed);return Failed?1:0;};
 if(!GEngine){Check(TEXT("engine"),false);return Finish();}
 TStrongObjectPtr<UGameInstance> GI(NewObject<UGameInstance>(GEngine));GI->InitializeStandalone(TEXT("AftershockSessionSaveFixture"));
 UWorld* W=GI->GetWorld();Check(TEXT("real_game_instance_and_world"),W!=nullptr);if(!W)return Finish();
 auto* Session=GI->GetSubsystem<UAftershockOpeningSessionSubsystem>();auto* Saves=GI->GetSubsystem<UAftershockSaveSubsystem>();auto* Quiz=GI->GetSubsystem<UQuizSubsystem>();
 auto* T=W->GetSubsystem<UTerrainGridSubsystem>();auto* B=W->GetSubsystem<UBuildingManagerSubsystem>();auto* Life=W->GetSubsystem<UCityLifeSubsystem>();auto* Crowd=W->GetSubsystem<UCrowdRenderSubsystem>();auto* R=W->GetSubsystem<UTransportSubsystem>();auto* E=W->GetSubsystem<UEconomyAndStateSubsystem>();auto* D=W->GetSubsystem<UAftershockDisasterDirectorSubsystem>();auto* Scenario=W->GetSubsystem<UAftershockScenarioSubsystem>();
 FText Error;FAftershockSessionSettings Settings;
 {
  // Commandlets have no Slate application. Own only the headless instance made here;
  // an existing editor/RHI application's renderer and lifecycle remain untouched.
  FScopedSessionAuditSlate AuditSlate;
  // Build the real opening Slate tree without a window or player controller.
  // A valid confirmation must reach the explicit missing-player dispatch error;
  // this fixture does not claim level travel, OS hit testing or a live city start.
  TStrongObjectPtr<UAftershockOpeningWidget> Opening(FSlateApplication::IsInitialized()?CreateWidget<UAftershockOpeningWidget>(GI.Get()):nullptr);
  if(Opening.IsValid())(void)Opening->TakeWidget();
  const auto OpeningField=[&](const TCHAR* Name)->UObject*
  {
   const FObjectPropertyBase* Property=Opening.IsValid()?FindFProperty<FObjectPropertyBase>(Opening->GetClass(),Name):nullptr;
   return Property?Property->GetObjectPropertyValue_InContainer(Opening.Get()):nullptr;
  };
  auto* Mode=Cast<UComboBoxString>(OpeningField(TEXT("ModeChoice")));
  auto* Seed=Cast<UEditableTextBox>(OpeningField(TEXT("SeedEntry")));
  auto* Status=Cast<UTextBlock>(OpeningField(TEXT("SessionStatus")));
  auto* Confirm=Cast<UButton>(OpeningField(TEXT("ConfirmNewGame")));
  const bool HasOpeningFixture=Opening.IsValid()&&!Opening->GetOwningPlayer()&&Mode&&Seed&&Status&&Confirm&&Confirm->OnClicked.IsBound();
  Check(TEXT("opening_seed_fixture_uses_real_widgets_without_level_travel"),HasOpeningFixture);
  if(HasOpeningFixture)
  {
   const FString InvalidSeedError=TEXT("Tohum geçerli bir tam sayı olmalı.");
   const FText MissingPlayerError=NSLOCTEXT("Aftershock","OpeningPlayerUnavailable","Şehir başlatılamadı: oyuncu oturumu hazır değil. Menüye dönüp yeniden dene.");
   const auto ClickConfirm=[&]() { Status->SetText(FText::GetEmpty());Confirm->OnClicked.Broadcast(); };
   Mode->SetSelectedIndex(2);Seed->SetText(FText::FromString(TEXT("abc")));ClickConfirm();
   Check(TEXT("sandbox_invalid_seed_rejected_by_real_opening_button"),Seed->GetIsEnabled()&&Status->GetText().ToString()==InvalidSeedError);
   Mode->SetSelectedIndex(0);
   Check(TEXT("campaign_switch_locks_and_displays_canonical_default_seed"),!Seed->GetIsEnabled()&&Seed->GetText().ToString()==FString::FromInt(FAftershockSessionSettings().Seed));
   // Even stale serialized/editor text in the locked field cannot block Campaign.
   Seed->SetText(FText::FromString(TEXT("abc")));ClickConfirm();
   Check(TEXT("campaign_locked_invalid_seed_reaches_player_dispatch_with_default"),!Seed->GetIsEnabled()&&Status->GetText().EqualTo(MissingPlayerError)&&UAftershockOpeningSessionSubsystem::ValidateSettings(FAftershockSessionSettings(),Error));
   Mode->SetSelectedIndex(1);Seed->SetText(FText::GetEmpty());ClickConfirm();
   Check(TEXT("geographic_invalid_seed_still_rejected_after_campaign"),Seed->GetIsEnabled()&&Status->GetText().ToString()==InvalidSeedError);
   Mode->SetSelectedIndex(2);Seed->SetText(FText::FromString(TEXT("abc")));ClickConfirm();
   Check(TEXT("sandbox_invalid_seed_still_rejected_after_campaign"),Seed->GetIsEnabled()&&Status->GetText().ToString()==InvalidSeedError);
   Seed->SetText(FText::FromString(TEXT("42")));ClickConfirm();
   Check(TEXT("sandbox_valid_seed_still_reaches_player_dispatch"),Status->GetText().EqualTo(MissingPlayerError));
  }
  if(Opening.IsValid())Opening->ReleaseSlateResources(true);
 }
 Check(TEXT("configure_campaign"),Session->ConfigureNewSession(Settings,Error));Scenario->PrepareScenarioBeforeSettlement();B->SeedInitialSettlement();Crowd->SynchronizeCanonicalPopulation();W->GetSubsystem<URoadNetworkSubsystem>()->NotifySaveRestored();
 auto* Camera=W->SpawnActor<AAftershockCameraPawn>();
 FAftershockCameraSaveState CameraFixture;if(Camera){Camera->CaptureSaveState(CameraFixture);CameraFixture.Zoom=30000;CameraFixture.OverviewBlend=.7f;CameraFixture.SavedLocalZoom=7200;CameraFixture.PlanetOrbitPitch=33;Check(TEXT("camera_globe_fixture_import"),Camera->RestoreSaveState(CameraFixture,Error));}
 Check(TEXT("canonical_living_roster_initialized"),Life->IsCityInitialized()&&Life->GetState().LivingPopulation>0&&Crowd->Agents.Num()==FMath::Min(1500,Life->GetState().LivingPopulation));
 FAftershockSessionSettings Invalid=Settings;Invalid.RegionID=TEXT("unknown-region");Check(TEXT("unknown_region_never_falls_back"),!Session->ValidateSettings(Invalid,Error));Invalid=Settings;Invalid.FocusID=TEXT("unknown-focus");Check(TEXT("unknown_focus_never_falls_back"),!Session->ValidateSettings(Invalid,Error));Invalid=Settings;Invalid.Mode=static_cast<EAftershockPlayMode>(99);Check(TEXT("unknown_mode_rejected"),!Session->ValidateSettings(Invalid,Error));
 Invalid=Settings;Invalid.Mode=EAftershockPlayMode::GeographicSimulation;Invalid.RegionID=TEXT("cyprus");Invalid.FocusID=TEXT("lefke");Check(TEXT("twenty_region_local_focus_is_selectable"),GetAftershockGeographyCount()==20&&Session->ValidateSettings(Invalid,Error));
 E->ClimateRiskMultiplier=1;const float QuakeBase=Scenario->GetScenarioSeverityForHazard(EDisasterType::Earthquake),FloodBase=Scenario->GetScenarioSeverityForHazard(EDisasterType::Flood);E->ClimateRiskMultiplier=2;
 Check(TEXT("requested_tectonic_hazards_ignore_weather_risk"),Scenario->GetScenarioSeverityForHazard(EDisasterType::Earthquake)==QuakeBase&&Scenario->GetScenarioSeverityForHazard(EDisasterType::Tsunami)==QuakeBase&&Scenario->GetScenarioSeverityForHazard(EDisasterType::Volcano)==QuakeBase);
 Check(TEXT("requested_weather_hazard_uses_weather_risk_even_in_quake_primary_city"),Scenario->GetScenarioSeverityForHazard(EDisasterType::Flood)>FloodBase);E->ClimateRiskMultiplier=1;
 FAftershockCityLifeSaveState LS;Life->CaptureSaveState(LS);const int32 DeadID=LS.Residents[0].ResidentID;LS.Residents[0].bAlive=false;LS.Residents[0].Health01=0.f;LS.Residents[0].WorkBuildingID=INDEX_NONE;--LS.State.LivingPopulation;++LS.State.TotalDeaths;LS.State.FoodStock=123.25f;LS.State.RawFoodStock=77.5f;LS.State.WaterStock=66.75f;LS.PaidTransitTrips.Add(17);LS.PendingTransitRevenueTL=35;
 Check(TEXT("fixture_bulk_life_import"),Life->RestoreSaveState(LS,Error));Crowd->SynchronizeCanonicalPopulation();
 // This fixture deliberately imports a historical death, without a current
 // disaster. Preserve that unknown history instead of inventing a cause/charge.
 FAftershockCasualtyLedgerSaveState HistoricalLedger;UCasualtyLedgerGenerator::BuildLegacyLedgerState(LS.Residents,Session->GetSessionID(),E->CurrentTurn,HistoricalLedger);
 TArray<FGuid> HistoricalEvents;for(const auto& Event:HistoricalLedger.Events)HistoricalEvents.Add(Event.EventID);
 Check(TEXT("fixture_imports_explicit_unknown_history_without_replaying_cost"),W->GetSubsystem<UCasualtyLedgerGenerator>()->RestoreSaveState(HistoricalLedger,Error)&&E->RestoreAppliedDisasterEvents(HistoricalEvents,Error));
 FAftershockTransportSaveState TS;R->CaptureSaveState(TS);TS.NextTripID=FMath::Max(42,TS.NextTripID);Check(TEXT("fixture_preserves_monotonic_trip_id"),R->RestoreSaveState(TS,Error));
 FAftershockBuildingsSaveState BS;B->CaptureSaveState(BS);BS.Buildings[0].WorldTransform.SetRotation(FRotator(0,90,0).Quaternion());BS.Buildings[0].Yaw=90;Check(TEXT("fixture_building_yaw_import"),B->RestoreSaveState(BS,Error));
 FAftershockDirectorState DS;DS.Phase=EAftershockDisasterPhase::Warning;DS.RemainingSeconds=8.5f;DS.WarningDuration=25;DS.RandomSeed=987654321;DS.EventSequence=3;DS.LastAppliedEventSequence=2;Check(TEXT("fixture_director_warning_import"),D->RestoreSaveState(DS,Error));
 Quiz->RequestRandomQuestion(EDisasterType::Earthquake);const int32 Answer=Quiz->GetCurrentQuestion().CorrectIndex;Check(TEXT("quiz_first_answer_commits"),Quiz->SubmitAnswer(Answer));const int32 Answered=Quiz->GetAnsweredCount();const int64 QuizBalance=E->TreasuryTL;Check(TEXT("quiz_duplicate_answer_has_no_reward"),!Quiz->SubmitAnswer(Answer)&&Quiz->GetAnsweredCount()==Answered&&E->TreasuryTL==QuizBalance);
 TStrongObjectPtr<UAftershockSaveGame> Saved(Saves->CaptureWorld(W,Error));Check(TEXT("complete_world_capture_valid"),Saved.IsValid());
 if(Saved.IsValid())
 {
  {
   // The Director's autosave captures synchronously from a completion delegate.
   // Exercise that SAME shared CaptureInto entry inside nested presentation
   // batches, after an explicitly labelled canonical death/import fixture.
   auto* Ledger=W->GetSubsystem<UCasualtyLedgerGenerator>();
   const auto BeforeWork=Crowd->GetRosterRefreshWork();TArray<int32> NewDeaths;
   bool Captured=false,DepthPreserved=false,CheckpointFlushed=false,LaterDeferred=false,InnerDeferred=false,IDsMatch=false;
   {
    FAftershockScopedCrowdRefresh Outer(Crowd);
    {
     FAftershockScopedCrowdRefresh Inner(Crowd);
     int32 LivingHome=INDEX_NONE;for(const auto& Person:Life->GetResidents())if(Person.bAlive){LivingHome=Person.HomeBuildingID;break;}
     NewDeaths=Life->ApplyCasualties(LivingHome,1,false);
     FAftershockCasualtyLedgerSaveState ImportedHistory;
     UCasualtyLedgerGenerator::BuildLegacyLedgerState(Life->GetResidents(),Session->GetSessionID(),E->CurrentTurn,ImportedHistory);
     TArray<FGuid> ImportedEvents;ImportedEvents.Reserve(ImportedHistory.Events.Num());
     for(const auto& Event:ImportedHistory.Events)ImportedEvents.Add(Event.EventID);
     const bool HistoryImported=Ledger->RestoreSaveState(ImportedHistory,Error)&&E->RestoreAppliedDisasterEvents(ImportedEvents,Error);
     Life->RefreshCity();Crowd->SynchronizeCanonicalPopulation();Crowd->BeginEmergencyEvacuation(EDisasterType::Earthquake);
     TStrongObjectPtr<UAftershockSaveGame> DuringBatch(NewObject<UAftershockSaveGame>(Saves));
     Captured=HistoryImported&&NewDeaths.Num()==1&&Saves->CaptureInto(W,*DuringBatch.Get(),Error);
     DepthPreserved=Crowd->ScopedCrowdRefreshDepth==2;
     const auto Checkpoint=Crowd->GetRosterRefreshWork();
     CheckpointFlushed=Checkpoint.Synchronizations==BeforeWork.Synchronizations+1&&Checkpoint.EmergencyPlans==BeforeWork.EmergencyPlans+1;
     if(Captured)
     {
      TSet<int32> Alive;for(const auto& Person:DuringBatch->CityLife.Residents)if(Person.bAlive)Alive.Add(Person.ResidentID);
      TSet<int32> Drawn;IDsMatch=DuringBatch->Crowd.Agents.Num()==FMath::Min(1500,Alive.Num());
      for(const auto& Agent:DuringBatch->Crowd.Agents){IDsMatch&=Alive.Contains(Agent.ResidentID)&&!Drawn.Contains(Agent.ResidentID)&&!NewDeaths.Contains(Agent.ResidentID);Drawn.Add(Agent.ResidentID);}
      IDsMatch&=DuringBatch->CityLife.State.LivingPopulation==Saved->CityLife.State.LivingPopulation-1;
     }
     Life->RefreshCity();Crowd->SynchronizeCanonicalPopulation();Crowd->ReplanEmergencyEvacuation();
     const auto AfterLaterNotice=Crowd->GetRosterRefreshWork();
     LaterDeferred=AfterLaterNotice.Synchronizations==Checkpoint.Synchronizations&&AfterLaterNotice.EmergencyPlans==Checkpoint.EmergencyPlans;
    }
    const auto AfterInner=Crowd->GetRosterRefreshWork();
    InnerDeferred=Crowd->ScopedCrowdRefreshDepth==1&&AfterInner.Synchronizations==BeforeWork.Synchronizations+1&&AfterInner.EmergencyPlans==BeforeWork.EmergencyPlans+1;
   }
   const auto Final=Crowd->GetRosterRefreshWork();
   Check(TEXT("save_inside_presentation_batch_uses_real_shared_capture_and_validation"),Captured);
   Check(TEXT("save_barrier_flushes_one_current_canonical_roster_and_emergency_plan"),CheckpointFlushed);
   Check(TEXT("save_barrier_snapshot_excludes_new_death_and_contains_only_final_living_ids"),Captured&&IDsMatch);
   Check(TEXT("save_barrier_preserves_nested_batch_depth_and_later_coalescing"),DepthPreserved&&LaterDeferred&&InnerDeferred);
   Check(TEXT("post_checkpoint_outer_exit_has_one_bounded_additional_flush"),Final.Synchronizations==BeforeWork.Synchronizations+2&&Final.EmergencyPlans==BeforeWork.EmergencyPlans+2);
   Check(TEXT("save_barrier_fixture_restores_original_complete_world"),Saves->RestoreWorld(W,Saved.Get(),Error));
  }
  // Isolated V5 integrity fixtures. No gameplay event, reward or user file is
  // replayed. The current world snapshot remains unchanged throughout.
  if(FParse::Param(*Params,TEXT("AuditPhysicalSaveV5")))
  {
   const auto ClearNewInventory=[](UAftershockSaveGame& Snapshot)
   {
    Snapshot.Physics={};
    for(auto& Cell:Snapshot.Terrain.Cells)
    {Cell.SnowWaterEquivalentMm=0.f;Cell.RegolithDepthM=0.f;Cell.SoilPoreWaterDepthM=0.f;Cell.GroundErodedDepthM=0.f;Cell.GroundDepositedDepthM=0.f;Cell.MaterialScenarioVersion=0;}
   };
   const auto SameExceptVersionAndPhysics=[](const UAftershockSaveGame& A,const UAftershockSaveGame& B)
   {
    for(TFieldIterator<FProperty> Field(A.GetClass());Field;++Field)
     if(Field->GetFName()!=GET_MEMBER_NAME_CHECKED(UAftershockSaveGame,SchemaVersion)&&Field->GetFName()!=GET_MEMBER_NAME_CHECKED(UAftershockSaveGame,Physics))
      for(int32 I=0;I<Field->ArrayDim;++I)if(!Field->Identical_InContainer(&A,&B,I,0))return false;
    return true;
   };
   TStrongObjectPtr<UAftershockSaveGame> Legacy(DuplicateObject<UAftershockSaveGame>(Saved.Get(),GetTransientPackage()));
   ClearNewInventory(*Legacy);Legacy->SchemaVersion=4;
   TStrongObjectPtr<UAftershockSaveGame> BeforeLegacy(DuplicateObject<UAftershockSaveGame>(Legacy.Get(),GetTransientPackage()));
   const bool Migrated=Saves->ValidateSnapshot(*Legacy,Error);
   Check(TEXT("v5_real_v4_semantic_fixture_migrates_only_after_validation"),Migrated&&Legacy->SchemaVersion==5
    &&Legacy->Physics.ModelVersion==0&&Legacy->Physics.bNeedsLegacyWaterSeed&&!Legacy->Physics.bWaterInitialized);
   Check(TEXT("v5_migration_preserves_all_previous_owner_records"),Migrated&&SameExceptVersionAndPhysics(*BeforeLegacy,*Legacy));
   TStrongObjectPtr<UAftershockSaveGame> InvalidLegacy(DuplicateObject<UAftershockSaveGame>(BeforeLegacy.Get(),GetTransientPackage()));
   InvalidLegacy->Economy.Satisfaction=-1.f;
   const bool RejectedLegacy=!Saves->ValidateSnapshot(*InvalidLegacy,Error);
   Check(TEXT("v5_failed_migration_keeps_original_schema_and_default_physics"),RejectedLegacy&&InvalidLegacy->SchemaVersion==4
    &&!InvalidLegacy->Physics.bNeedsLegacyWaterSeed&&InvalidLegacy->Physics.ModelVersion==0&&InvalidLegacy->Economy.Satisfaction==-1.f);
   TStrongObjectPtr<UAftershockSaveGame> Physical(DuplicateObject<UAftershockSaveGame>(Saved.Get(),GetTransientPackage()));
   auto& P=Physical->Physics;P={};P.MaterialScenarioReceiptJSON=Saved->Physics.MaterialScenarioReceiptJSON;P.ModelVersion=2;P.bWaterInitialized=true;
   P.CapturedCellAreaM2=FMath::Square(static_cast<double>(Physical->Terrain.CellSize)*.01);
   P.PhysicalSeconds=4.25;P.PendingPhysicalSeconds=.125;P.PublishedStep=8;P.DomainRevision=Physical->Terrain.TerrainRevision;
   P.WaterBudget.InitialSurfaceM3=3.;P.WaterBudget.FinalSurfaceM3=3.;P.WaterBudget.AcceptedSteps=8;
   bool PhysicalBedsReady=false;
   if(Physical->Terrain.Cells.Num()>=2)
   {
    P.WaterCells.Reserve(2); // Keep both fixture references valid during the second append.
    auto& A=P.WaterCells.AddDefaulted_GetRef();A.Coord=Physical->Terrain.Cells[0].Coords;A.VolumeM3=1.25;A.DischargeXM2PerSecond=.05;
    auto& Other=P.WaterCells.AddDefaulted_GetRef();Other.Coord=Physical->Terrain.Cells[1].Coords;Other.VolumeM3=1.75;
    PhysicalBedsReady=TryGetAftershockHydraulicBedCm(Physical->Terrain.Cells[0],true,A.BedElevationCm,Error)
     &&TryGetAftershockHydraulicBedCm(Physical->Terrain.Cells[1],true,Other.BedElevationCm,Error);
   }
   const bool PhysicalValid=PhysicalBedsReady&&Saves->ValidateSnapshot(*Physical,Error);
   Check(TEXT("v5_completed_frontier_plus_unadvanced_time_passes_validation"),PhysicalValid&&Physical->SchemaVersion==UAftershockSaveGame::CurrentVersion);
   TArray<uint8> PhysicalBytes;const bool Encoded=UAftershockSaveSubsystem::EncodeIndexedPayload(*Physical,PhysicalBytes);
   TStrongObjectPtr<UAftershockSaveGame> PhysicalRead(Encoded?UAftershockSaveSubsystem::DecodeIndexedPayload(PhysicalBytes):nullptr);
   Check(TEXT("v5_binary_codec_preserves_full_physical_frontier"),Encoded&&PhysicalRead.IsValid()
    &&Saves->ValidateSnapshot(*PhysicalRead,Error)&&FAftershockPhysicalHazardSaveState::StaticStruct()->CompareScriptStruct(&PhysicalRead->Physics,&P,0));
   // Actual public RestoreWorld rejection on an otherwise valid active-water
   // snapshot. No disk/user slots and no world ticks/async jobs are invoked.
   // Full before/after captures compare EVERY canonical owner, except the
   // intentionally wall-clock SavedAt capture timestamp.
   {
    const auto SameCanonical=[](const UAftershockSaveGame& A,const UAftershockSaveGame& Other)
    {
     for(FProperty* Field=A.GetClass()->PropertyLink;Field;Field=Field->PropertyLinkNext)
      if(Field->GetFName()!=GET_MEMBER_NAME_CHECKED(UAftershockSaveGame,SavedAt))
       for(int32 I=0;I<Field->ArrayDim;++I)if(!Field->Identical_InContainer(&A,&Other,I,0))return false;
     return true;
    };
    TStrongObjectPtr<UAftershockSaveGame> EpisodeFixture(DuplicateObject<UAftershockSaveGame>(Physical.Get(),GetTransientPackage()));
    auto& PhaseEpisode=EpisodeFixture->Physics.Episode;
    PhaseEpisode.bActive=true;PhaseEpisode.EventID=FGuid::NewGuid();PhaseEpisode.Type=EDisasterType::Flood;
    PhaseEpisode.StartPhysicalSeconds=EpisodeFixture->Physics.PhysicalSeconds;PhaseEpisode.SourceDurationSeconds=10.;
    const bool EpisodeValid=PhysicalValid&&Saves->ValidateSnapshot(*EpisodeFixture,Error);
    Check(TEXT("v5_representable_active_source_boundary_remains_valid"),EpisodeValid);
    const auto RejectBoundaryWithoutMutation=[&](const TCHAR* Name,double Start,double Duration)
    {
     TStrongObjectPtr<UAftershockSaveGame> Corrupt(DuplicateObject<UAftershockSaveGame>(EpisodeFixture.Get(),GetTransientPackage()));
     Corrupt->Physics.Episode.StartPhysicalSeconds=Start;Corrupt->Physics.Episode.SourceDurationSeconds=Duration;
     TStrongObjectPtr<UAftershockSaveGame> InputBefore(DuplicateObject<UAftershockSaveGame>(Corrupt.Get(),GetTransientPackage()));
     TStrongObjectPtr<UAftershockSaveGame> WorldBefore(Saves->CaptureWorld(W,Error));
     const bool PreflightRejected=!Saves->ValidateSnapshot(*Corrupt,Error);
     const bool PrecisePreflight=Error.ToString().Contains(TEXT("unrepresentable"));
     const bool RestoreRejected=!Saves->RestoreWorld(W,Corrupt.Get(),Error);
     TStrongObjectPtr<UAftershockSaveGame> WorldAfter(Saves->CaptureWorld(W,Error));
     Check(Name,EpisodeValid&&PreflightRejected&&PrecisePreflight&&RestoreRejected
      &&WorldBefore.IsValid()&&WorldAfter.IsValid()&&SameCanonical(*WorldBefore,*WorldAfter)
      &&SameCanonical(*InputBefore,*Corrupt)&&InputBefore->SavedAt==Corrupt->SavedAt);
    };
    RejectBoundaryWithoutMutation(TEXT("v5_source_end_overflow_rejected_before_restore_without_any_owner_or_input_mutation"),1.e308,1.e308);
    RejectBoundaryWithoutMutation(TEXT("v5_positive_source_duration_lost_to_roundoff_rejected_before_restore_without_mutation"),1.e20,.1);
    auto& LargeEpoch=EpisodeFixture->Physics;LargeEpoch.PhysicalSeconds=1.e8;
    LargeEpoch.Episode.StartPhysicalSeconds=1.e8;LargeEpoch.Episode.SourceDurationSeconds=10.;
    Check(TEXT("v5_large_but_representable_source_epoch_remains_valid"),EpisodeValid&&Saves->ValidateSnapshot(*EpisodeFixture,Error));
   }
   TStrongObjectPtr<UAftershockSaveGame> Pending(DuplicateObject<UAftershockSaveGame>(Physical.Get(),GetTransientPackage()));
   Pending->Physics.PendingAcceptedSeconds=.0625;
   for(int32 I=Pending->Physics.WaterCells.Num()-1;I>=0;--I)
   {
    auto& C=Pending->Physics.PendingForcingCells.AddDefaulted_GetRef();C.Coord=Pending->Physics.WaterCells[I].Coord;
    C.RainMPerSecond=.0000123;C.InfiltrationCapacityMPerSecond=.0000004;C.ExternalDrainCapacityM3PerSecond=.03;
    C.EvaporationMPerSecond=.0000001;C.ExternalInflowM3PerSecond=.012;C.InflowVelocityXMps=-.35;C.InflowVelocityYMps=.27;
   }
   // Key/scalar fixture only: runtime checks exact StageBoundary membership
   // against its immutable Domain before launching any initialization.
   Check(TEXT("v5_pending_forcing_scalar_validation_accepts_full_keyed_packet"),Saves->ValidateSnapshot(*Pending,Error));
   TArray<uint8> PendingBytes;const bool PendingEncoded=UAftershockSaveSubsystem::EncodeIndexedPayload(*Pending,PendingBytes);
   TStrongObjectPtr<UAftershockSaveGame> PendingRead(PendingEncoded?UAftershockSaveSubsystem::DecodeIndexedPayload(PendingBytes):nullptr);
   Check(TEXT("v5_pending_forcing_binary_roundtrip_keeps_all_cell_controls"),PendingEncoded&&PendingRead.IsValid()
    &&Saves->ValidateSnapshot(*PendingRead,Error)&&FAftershockPhysicalHazardSaveState::StaticStruct()->CompareScriptStruct(&PendingRead->Physics,&Pending->Physics,0));
   TStrongObjectPtr<UAftershockSaveGame> BoundaryWire(DuplicateObject<UAftershockSaveGame>(Pending.Get(),GetTransientPackage()));
   auto& WB=BoundaryWire->Physics.PendingForcingBoundaries.AddDefaulted_GetRef();WB.FaceStableID=17;WB.HeadM=-1.25;WB.VelocityXMps=-.0125;WB.VelocityYMps=.035;
   TArray<uint8> BoundaryBytes;const bool BoundaryEncoded=UAftershockSaveSubsystem::EncodeIndexedPayload(*BoundaryWire,BoundaryBytes);
   TStrongObjectPtr<UAftershockSaveGame> BoundaryRead(BoundaryEncoded?UAftershockSaveSubsystem::DecodeIndexedPayload(BoundaryBytes):nullptr);
   Check(TEXT("v5_wire_preserves_signed_boundary_bits_not_stage_membership"),BoundaryEncoded&&BoundaryRead.IsValid()
    &&FAftershockPhysicalHazardSaveState::StaticStruct()->CompareScriptStruct(&BoundaryRead->Physics,&BoundaryWire->Physics,0));
   const auto RejectPending=[&](const TCHAR* Name,auto&& Mutate)
   {
    TStrongObjectPtr<UAftershockSaveGame> Copy(DuplicateObject<UAftershockSaveGame>(Pending.Get(),GetTransientPackage()));
    Mutate(Copy->Physics);Check(Name,!Saves->ValidateSnapshot(*Copy,Error)&&Copy->SchemaVersion==5);
   };
   RejectPending(TEXT("v5_rejects_negative_accepted_quantum"),[](auto& F){F.PendingAcceptedSeconds=-.01;});
   RejectPending(TEXT("v5_rejects_accepted_quantum_exceeding_total_debt"),[](auto& F){F.PendingAcceptedSeconds=F.PendingPhysicalSeconds+.01;});
   RejectPending(TEXT("v5_rejects_nonfinite_accepted_quantum"),[](auto& F){F.PendingAcceptedSeconds=std::numeric_limits<double>::infinity();});
   RejectPending(TEXT("v5_rejects_source_arrays_without_accepted_time"),[](auto& F){F.PendingAcceptedSeconds=0.;});
   RejectPending(TEXT("v5_rejects_incomplete_accepted_cell_controls"),[](auto& F){F.PendingForcingCells.Pop();});
   RejectPending(TEXT("v5_rejects_duplicate_accepted_cell_controls"),[](auto& F){F.PendingForcingCells[1].Coord=F.PendingForcingCells[0].Coord;});
   RejectPending(TEXT("v5_rejects_accepted_controls_outside_saved_water"),[](auto& F){F.PendingForcingCells[0].Coord=FIntPoint(MAX_int32,MAX_int32);});
   RejectPending(TEXT("v5_rejects_negative_accepted_rain"),[](auto& F){F.PendingForcingCells[0].RainMPerSecond=-1.;});
   RejectPending(TEXT("v5_rejects_negative_accepted_infiltration"),[](auto& F){F.PendingForcingCells[0].InfiltrationCapacityMPerSecond=-1.;});
   RejectPending(TEXT("v5_rejects_negative_accepted_drain"),[](auto& F){F.PendingForcingCells[0].ExternalDrainCapacityM3PerSecond=-1.;});
   RejectPending(TEXT("v5_rejects_negative_accepted_evaporation"),[](auto& F){F.PendingForcingCells[0].EvaporationMPerSecond=-1.;});
   RejectPending(TEXT("v5_rejects_negative_accepted_external_inflow"),[](auto& F){F.PendingForcingCells[0].ExternalInflowM3PerSecond=-1.;});
   RejectPending(TEXT("v5_rejects_nonfinite_accepted_velocity"),[](auto& F){F.PendingForcingCells[0].InflowVelocityXMps=std::numeric_limits<double>::infinity();});
   RejectPending(TEXT("v5_rejects_nonfinite_accepted_stage"),[](auto& F){auto& Boundary=F.PendingForcingBoundaries.AddDefaulted_GetRef();Boundary.HeadM=std::numeric_limits<double>::infinity();});
   RejectPending(TEXT("v5_rejects_duplicate_accepted_stage_id"),[](auto& F){F.PendingForcingBoundaries.AddDefaulted(2);F.PendingForcingBoundaries[0].FaceStableID=42;F.PendingForcingBoundaries[1].FaceStableID=42;});
   const auto RejectPhysical=[&](const TCHAR* Name,auto&& Mutate)
   {
    TStrongObjectPtr<UAftershockSaveGame> Copy(DuplicateObject<UAftershockSaveGame>(Physical.Get(),GetTransientPackage()));
    Mutate(Copy->Physics);Check(Name,!Saves->ValidateSnapshot(*Copy,Error)&&Copy->SchemaVersion==5);
   };
   RejectPhysical(TEXT("v5_rejects_negative_water_volume"),[](auto& F){F.WaterCells[0].VolumeM3=-.1;});
   RejectPhysical(TEXT("v5_rejects_duplicate_water_coordinates"),[](auto& F){F.WaterCells[1].Coord=F.WaterCells[0].Coord;});
   RejectPhysical(TEXT("v5_rejects_water_outside_saved_global_cells"),[](auto& F){F.WaterCells[0].Coord=FIntPoint(MAX_int32,MAX_int32);});
   RejectPhysical(TEXT("v5_rejects_nonfinite_discharge"),[](auto& F){F.WaterCells[0].DischargeYM2PerSecond=std::numeric_limits<double>::infinity();});
   RejectPhysical(TEXT("v5_rejects_negative_physical_clock"),[](auto& F){F.PhysicalSeconds=-1.;});
   RejectPhysical(TEXT("v5_rejects_step_stamp_ledger_mismatch"),[](auto& F){++F.PublishedStep;});
   RejectPhysical(TEXT("v5_rejects_oversized_material_history_json"),[](auto& F){F.MaterialScenarioReceiptJSON=FString::ChrN(256*1024+1,TEXT('x'));});
   RejectPhysical(TEXT("v5_rejects_malformed_material_history_json"),[](auto& F){F.MaterialScenarioReceiptJSON=TEXT("{invalid");});
   RejectPhysical(TEXT("v5_rejects_negative_pending_physical_time"),[](auto& F){F.PendingPhysicalSeconds=-1.;});
   RejectPhysical(TEXT("v5_rejects_unmatched_physical_cell_area"),[](auto& F){F.CapturedCellAreaM2*=2.;});
   RejectPhysical(TEXT("v5_rejects_water_ledger_volume_mismatch"),[](auto& F){F.WaterBudget.FinalSurfaceM3+=1.;});
   RejectPhysical(TEXT("v5_rejects_uncaptured_or_invented_0d_storage"),[](auto& F){F.WaterBudget.InitialStorageM3=1.;});
   RejectPhysical(TEXT("v5_rejects_cell_infiltration_not_in_cumulative_ledger"),[](auto& F){F.WaterCells[0].CumulativeInfiltrationM3=1.;});
   RejectPhysical(TEXT("v5_rejects_mass_replay_over_eight_mib_without_truncating"),[](auto& F){F.Episode.MassReplay.SetNumZeroed(8*1024*1024+1);});
   RejectPhysical(TEXT("v5_rejects_active_episode_without_stable_event_id"),[](auto& F){F.Episode.bActive=true;F.Episode.SourceDurationSeconds=1.;});
   RejectPhysical(TEXT("v5_rejects_unknown_mass_replay_magic_or_version"),[](auto& F)
   {F.Episode.bActive=true;F.Episode.EventID=FGuid::NewGuid();F.Episode.Type=EDisasterType::Avalanche;F.Episode.MassReplay={1,2,3,4};});
   RejectPhysical(TEXT("v5_rejects_hard_asset_pointer_in_historical_building_baseline"),[&](auto& F)
   {
    F.Episode.bActive=true;F.Episode.EventID=FGuid::NewGuid();
    auto& Baseline=F.Episode.BeforeBuildings.AddDefaulted_GetRef();Baseline.State=Saved->Buildings.Buildings[0].State;
    Baseline.DefinitionType=Saved->Buildings.Buildings[0].DefinitionType;Baseline.State.BuildingData=B->GetCatalogData(Baseline.DefinitionType);
   });
   // Synthetic codec/spatial-binding fixture from an ACTUAL small kernel
   // solve. Flat authored snow is deliberately NOT this city's real avalanche
   // source, does not import into the world and is not runout/gameplay proof.
   // Negative cases use public RestoreWorld and compare all canonical owners.
   {
    FAftershockMassInput BindingInput;BindingInput.Material=EAftershockMassMaterial::Snow;BindingInput.HazardTypeTag=7;
    BindingInput.TerrainRevision=Physical->Terrain.TerrainRevision;BindingInput.CellSizeM=Physical->Terrain.CellSize*.01;
    BindingInput.GridOriginM=FVector2D(Physical->Terrain.GridOrigin)*.01;BindingInput.Settings.MaxCells=81;
    BindingInput.Settings.MaximumSeconds=5.;BindingInput.Settings.SnowFriction.Mu=2.;
    bool HasRelease=false;FIntPoint ReleaseCoord=FIntPoint::ZeroValue;
    TMap<FIntPoint,const FGridCell*> MapCells;for(const auto& Cell:Physical->Terrain.Cells)MapCells.Add(Cell.Coords,&Cell);
    for(int32 Y=0;Y<9;++Y)for(int32 X=0;X<9;++X)
    {
     const auto* Source=MapCells.FindRef(FIntPoint(X,Y));if(!Source)continue;
     FAftershockMassCell Cell;Cell.Coord=Source->Coords;Cell.bLand=!Source->bIsOcean;
     Cell.GroundElevationM=0.;Cell.SnowWaterEquivalentMm=Cell.bLand?180.:0.;BindingInput.Cells.Add(Cell);
    }
    for(int32 Y=2;Y<7&&!HasRelease;++Y)for(int32 X=2;X<7&&!HasRelease;++X)
    {
     bool AllLand=true;for(int32 DY=-1;DY<=1;++DY)for(int32 DX=-1;DX<=1;++DX)
     {const auto* Cell=MapCells.FindRef(FIntPoint(X+DX,Y+DY));AllLand&=Cell&&!Cell->bIsOcean;}
     if(AllLand){ReleaseCoord=FIntPoint(X,Y);HasRelease=true;}
    }
    FAftershockMassRelease Release;Release.Coord=ReleaseCoord;Release.bForceRelease=true;BindingInput.Release.Add(Release);
    const auto MakeBindingReplay=[](const FAftershockMassInput& Input)
    {
     FAftershockMassVisualEvent Event;Event.EventID=FGuid::NewGuid();Event.CellSizeM=Input.CellSizeM;Event.GridOriginM=Input.GridOriginM;
     Event.SupportCells=Input.Cells;Event.Obstacles=Input.Obstacles;Event.Result=FAftershockMassMovementSolver::Solve(Input);return Event;
    };
    const auto BindingReplay=MakeBindingReplay(BindingInput);TArray<uint8> ReplayBytes;
    const bool ReplayReady=HasRelease&&BindingInput.Cells.Num()==81&&BindingReplay.Result.bAccepted
     &&EncodeAftershockMassReplay(BindingReplay,ReplayBytes,Error);
    Check(TEXT("v5_replay_binding_fixture_has_actual_bounded_kernel_and_codec_output"),ReplayReady);
    TStrongObjectPtr<UAftershockSaveGame> BoundReplay(DuplicateObject<UAftershockSaveGame>(Physical.Get(),GetTransientPackage()));
    auto& BoundEpisode=BoundReplay->Physics.Episode;BoundEpisode.bActive=true;BoundEpisode.EventID=BindingReplay.EventID;
    BoundEpisode.Type=EDisasterType::Avalanche;BoundEpisode.StartPhysicalSeconds=BoundReplay->Physics.PhysicalSeconds;
    BoundEpisode.SourceDurationSeconds=BindingReplay.Result.SimulatedSeconds;BoundEpisode.MassReplay=ReplayBytes;
    const bool BoundValid=ReplayReady&&Saves->ValidateSnapshot(*BoundReplay,Error);
    Check(TEXT("v5_replay_same_event_type_origin_cellsize_and_resident_support_passes_preflight"),BoundValid);
    const auto SameCapturedOwners=[](const UAftershockSaveGame& A,const UAftershockSaveGame& Other)
    {
     for(FProperty* Field=A.GetClass()->PropertyLink;Field;Field=Field->PropertyLinkNext)
      if(Field->GetFName()!=GET_MEMBER_NAME_CHECKED(UAftershockSaveGame,SavedAt))
       for(int32 I=0;I<Field->ArrayDim;++I)if(!Field->Identical_InContainer(&A,&Other,I,0))return false;
     return true;
    };
    const auto RejectBinding=[&](const TCHAR* Name,auto&& Mutate)
    {
     TStrongObjectPtr<UAftershockSaveGame> Corrupt(DuplicateObject<UAftershockSaveGame>(BoundReplay.Get(),GetTransientPackage()));
     auto ChangedReplay=BindingReplay;Mutate(Corrupt->Physics.Episode,ChangedReplay);TArray<uint8> ChangedBytes;
     // Every mutation must still pass the independent bounded/deep replay
     // codec. A generic malformed-body failure cannot impersonate this guard.
     const bool DeepCodecValid=EncodeAftershockMassReplay(ChangedReplay,ChangedBytes,Error);
     Corrupt->Physics.Episode.MassReplay=MoveTemp(ChangedBytes);
     TStrongObjectPtr<UAftershockSaveGame> InputBefore(DuplicateObject<UAftershockSaveGame>(Corrupt.Get(),GetTransientPackage()));
     TStrongObjectPtr<UAftershockSaveGame> WorldBefore(Saves->CaptureWorld(W,Error));
     const bool PreflightRejected=!Saves->ValidateSnapshot(*Corrupt,Error);
     const bool BindingReason=Error.ToString().Contains(TEXT("hareket kaydının"));
     const bool RestoreRejected=!Saves->RestoreWorld(W,Corrupt.Get(),Error);
     TStrongObjectPtr<UAftershockSaveGame> WorldAfter(Saves->CaptureWorld(W,Error));
     Check(Name,BoundValid&&DeepCodecValid&&PreflightRejected&&BindingReason&&RestoreRejected
      &&WorldBefore.IsValid()&&WorldAfter.IsValid()&&SameCapturedOwners(*WorldBefore,*WorldAfter)
      &&SameCapturedOwners(*InputBefore,*Corrupt)&&InputBefore->SavedAt==Corrupt->SavedAt);
    };
    RejectBinding(TEXT("v5_valid_replay_wrong_event_id_rejected_before_restore_without_mutation"),[](auto& Episode,auto&){Episode.EventID=FGuid::NewGuid();});
    RejectBinding(TEXT("v5_valid_replay_wrong_hazard_type_rejected_before_restore_without_mutation"),[](auto& Episode,auto&){Episode.Type=EDisasterType::Landslide;});
    RejectBinding(TEXT("v5_valid_replay_shifted_world_offset_rejected_before_restore_without_mutation"),[](auto&,auto& Replay){Replay.WorldOffsetCm.X=100.;});
    RejectBinding(TEXT("v5_valid_replay_shifted_grid_origin_rejected_before_restore_without_mutation"),[](auto&,auto& Replay)
    {
     Replay.GridOriginM.X+=1.;for(auto& Frame:Replay.Result.Frames)for(auto& Parcel:Frame.Parcels)Parcel.PositionM.X+=1.;
     for(auto& Obstacle:Replay.Obstacles)Obstacle.CenterM.X+=1.;
    });
    RejectBinding(TEXT("v5_valid_replay_nonresident_frozen_source_rejected_before_restore_without_mutation"),[](auto&,auto& Replay)
    {
     const int32 Shift=10000;const double DX=Shift*Replay.CellSizeM;
     for(auto& Cell:Replay.SupportCells)Cell.Coord.X+=Shift;for(auto& Cell:Replay.Result.Cells)Cell.Coord.X+=Shift;
     for(auto& Frame:Replay.Result.Frames){for(auto& Cell:Frame.Cells)Cell.Coord.X+=Shift;for(auto& Parcel:Frame.Parcels)Parcel.PositionM.X+=DX;}
     for(auto& Obstacle:Replay.Obstacles)Obstacle.CenterM.X+=DX;
    });
    RejectBinding(TEXT("v5_valid_replay_different_cell_size_rejected_before_restore_without_mutation"),[&](auto&,auto& Replay)
    {
     auto OtherScale=BindingInput;OtherScale.CellSizeM*=2.;const FGuid OriginalEvent=Replay.EventID;
     Replay=MakeBindingReplay(OtherScale);Replay.EventID=OriginalEvent;
    });
   }
   TStrongObjectPtr<UAftershockSaveGame> MissingReceipt(DuplicateObject<UAftershockSaveGame>(Physical.Get(),GetTransientPackage()));
   MissingReceipt->Terrain.Cells[0].MaterialScenarioVersion=1;MissingReceipt->Physics.MaterialScenarioReceiptJSON.Empty();
   Check(TEXT("v5_authored_material_version_requires_history_receipt"),!Saves->ValidateSnapshot(*MissingReceipt,Error));
   TStrongObjectPtr<UAftershockSaveGame> Relabelled(DuplicateObject<UAftershockSaveGame>(Physical.Get(),GetTransientPackage()));
   Relabelled->SchemaVersion=4;
   Check(TEXT("v5_relabelled_physical_save_is_rejected_without_erasing_frontier"),!Saves->ValidateSnapshot(*Relabelled,Error)
    &&Relabelled->SchemaVersion==4&&Relabelled->Physics.ModelVersion==2&&Relabelled->Physics.WaterCells.Num()==2);
   TStrongObjectPtr<UAftershockSaveGame> InventoryRelabel(DuplicateObject<UAftershockSaveGame>(BeforeLegacy.Get(),GetTransientPackage()));
   InventoryRelabel->Terrain.Cells[0].SnowWaterEquivalentMm=1.f;
   Check(TEXT("v5_relabelled_mass_stock_is_rejected_without_erasing_inventory"),!Saves->ValidateSnapshot(*InventoryRelabel,Error)
    &&InventoryRelabel->SchemaVersion==4&&InventoryRelabel->Terrain.Cells[0].SnowWaterEquivalentMm==1.f);
  }
  {
   TArray<uint8> Indexed;const bool Encoded=AftershockSaveBinarySchema::EncodeLegacyTaggedFixture(*Saved,Indexed);
   TStrongObjectPtr<UAftershockSaveGame> IndexedRead(Encoded?Saves->DecodeIndexedPayload(Indexed):nullptr);
   Check(TEXT("indexed_name_tagged_payload_roundtrip_passes_full_snapshot_validation"),Encoded&&IndexedRead.IsValid()&&Saves->ValidateSnapshot(*IndexedRead,Error));
   Check(TEXT("indexed_name_roundtrip_preserves_canonical_terrain_residents_and_casualty_ledgers"),IndexedRead.IsValid()
    &&IndexedRead->SessionID==Saved->SessionID&&IndexedRead->SchemaVersion==Saved->SchemaVersion&&IndexedRead->Economy.Treasury==Saved->Economy.Treasury
    &&FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&IndexedRead->Terrain,&Saved->Terrain,0)
    &&FAftershockCityLifeSaveState::StaticStruct()->CompareScriptStruct(&IndexedRead->CityLife,&Saved->CityLife,0)
    &&FAftershockCasualtyLedgerSaveState::StaticStruct()->CompareScriptStruct(&IndexedRead->CasualtyLedger,&Saved->CasualtyLedger,0)
    &&FAftershockTransportSaveState::StaticStruct()->CompareScriptStruct(&IndexedRead->Transport,&Saved->Transport,0));
   struct FNameLocation{int32 LengthAt=0,TextAt=0,Bytes=0;FString Text;};
   const auto ReadLayout=[](const TArray<uint8>& Bytes,TArray<FNameLocation>& NameLocations,int32& NameCountAt,int32& BodyAt)
   {
    bool LayoutValid=true;FMemoryReader Layout(Bytes,true);uint32 Magic=0,Version=0,Root=0,Changelist=0;FPackageFileVersion Package;uint16 Major=0,Minor=0,Patch=0;
    Layout<<Magic;Layout<<Version;Layout<<Root;Layout<<Package;Layout<<Major;Layout<<Minor;Layout<<Patch;Layout<<Changelist;
    int32 BranchBytes=0;Layout<<BranchBytes;LayoutValid=!Layout.IsError()&&BranchBytes>=0&&BranchBytes<=1024;
    if(LayoutValid){Layout.Seek(Layout.Tell()+BranchBytes);int32 CustomCount=0;Layout<<CustomCount;LayoutValid=CustomCount>=0&&CustomCount<=1024&&!Layout.IsError();if(LayoutValid)Layout.Seek(Layout.Tell()+static_cast<int64>(CustomCount)*20);}
    if(LayoutValid)
    {
     NameCountAt=Layout.Tell();int32 NameCount=0;Layout<<NameCount;LayoutValid=NameCount>0&&NameCount<=16384&&!Layout.IsError();
     for(int32 I=0;I<NameCount&&LayoutValid;++I)
     {
      FNameLocation At;At.LengthAt=Layout.Tell();Layout<<At.Bytes;At.TextAt=Layout.Tell();LayoutValid=!Layout.IsError()&&At.Bytes>0&&At.Bytes<=1023&&At.Bytes<=Layout.TotalSize()-Layout.Tell();
      if(LayoutValid){const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()+At.TextAt),At.Bytes);At.Text=FString(Text.Length(),Text.Get());NameLocations.Add(At);Layout.Seek(Layout.Tell()+At.Bytes);}
     }
     if(LayoutValid){int32 BodyBytes=0;Layout<<BodyBytes;BodyAt=Layout.Tell();LayoutValid=!Layout.IsError()&&BodyBytes==Layout.TotalSize()-BodyAt&&BodyBytes>=4;}
    }
    return LayoutValid;
   };
   TArray<FNameLocation> NameLocations;int32 NameCountAt=INDEX_NONE,BodyAt=INDEX_NONE;
   const bool LayoutValid=Encoded&&ReadLayout(Indexed,NameLocations,NameCountAt,BodyAt);
   Check(TEXT("indexed_codec_has_bounded_dictionary_prelude_and_exact_tagged_body_extent"),LayoutValid&&!NameLocations.IsEmpty()&&NameLocations[0].Text==TEXT("None")&&NameCountAt>=0&&BodyAt>=0);
   if(LayoutValid&&!NameLocations.IsEmpty()&&BodyAt>=0)
   {
    TArray<uint8> Mutated=Indexed;const uint32 BadReference=MAX_uint32;FMemory::Memcpy(Mutated.GetData()+BodyAt,&BadReference,sizeof(BadReference));
    Check(TEXT("indexed_codec_rejects_out_of_dictionary_name_reference"),Saves->DecodeIndexedPayload(Mutated)==nullptr);
    Mutated=Indexed;const int32 OversizedName=1024;FMemory::Memcpy(Mutated.GetData()+NameLocations[0].LengthAt,&OversizedName,sizeof(OversizedName));
    Check(TEXT("indexed_codec_rejects_oversized_name_before_name_construction"),Saves->DecodeIndexedPayload(Mutated)==nullptr);
    Mutated=Indexed;const int32 OversizedCount=16385;FMemory::Memcpy(Mutated.GetData()+NameCountAt,&OversizedCount,sizeof(OversizedCount));
    Check(TEXT("indexed_codec_rejects_dictionary_entry_count_over_bound"),Saves->DecodeIndexedPayload(Mutated)==nullptr);
    Mutated=Indexed;Mutated[NameLocations[0].TextAt]='X';
    Check(TEXT("indexed_codec_rejects_tampered_none_dictionary_anchor"),Saves->DecodeIndexedPayload(Mutated)==nullptr);
    Mutated=Indexed;Mutated.SetNum(Mutated.Num()-1,EAllowShrinking::No);
    Check(TEXT("indexed_codec_rejects_truncated_tagged_body_before_deserialization"),Saves->DecodeIndexedPayload(Mutated)==nullptr);
    Mutated=Indexed;Mutated.Add(0);
    Check(TEXT("indexed_codec_rejects_trailing_payload_bytes"),Saves->DecodeIndexedPayload(Mutated)==nullptr);
    Mutated=Indexed;const uint32 WrongRoot=0;FMemory::Memcpy(Mutated.GetData()+sizeof(uint32)*2,&WrongRoot,sizeof(WrongRoot));
    Check(TEXT("indexed_codec_rejects_any_root_other_than_fixed_savegame_class"),Saves->DecodeIndexedPayload(Mutated)==nullptr);
    Mutated=Indexed;int32 UE4=0;FMemory::Memcpy(&UE4,Mutated.GetData()+sizeof(uint32)*3,sizeof(UE4));++UE4;FMemory::Memcpy(Mutated.GetData()+sizeof(uint32)*3,&UE4,sizeof(UE4));
    Check(TEXT("indexed_codec_rejects_future_ue4_component_even_with_current_ue5_component"),Saves->DecodeIndexedPayload(Mutated)==nullptr);
    // A default-valued SchemaVersion4 tag may be delta-skipped entirely. Force
    // a nondefault tag in this separate payload, then make only that tag name
    // unknown. The reader must use its current default4 and keep every other
    // canonical owner field; the ordinary current4 roundtrip above is intact.
    TStrongObjectPtr<UAftershockSaveGame> FutureSource(DuplicateObject<UAftershockSaveGame>(Saved.Get(),GI.Get()));FutureSource->SchemaVersion=3;
    TArray<uint8> FuturePayload;TArray<FNameLocation> FutureNames;int32 FutureNameCountAt=INDEX_NONE,FutureBodyAt=INDEX_NONE;
    const bool FutureLayout=AftershockSaveBinarySchema::EncodeLegacyTaggedFixture(*FutureSource,FuturePayload)&&ReadLayout(FuturePayload,FutureNames,FutureNameCountAt,FutureBodyAt);
    bool SchemaFound=false,LengthMatches=false,FutureDecoded=false,FutureValidated=false,UnknownSkipped=false;FText FutureError;
    for(const auto& Name:FutureNames)if(FName(*Name.Text)==GET_MEMBER_NAME_CHECKED(UAftershockSaveGame,SchemaVersion))
    {
     SchemaFound=true;const FTCHARToUTF8 Future(TEXT("FutureSchemaX"));LengthMatches=Future.Length()==Name.Bytes;Mutated=FuturePayload;
     if(LengthMatches)
     {
      FMemory::Memcpy(Mutated.GetData()+Name.TextAt,Future.Get(),Name.Bytes);TStrongObjectPtr<UAftershockSaveGame> FutureRead(Saves->DecodeIndexedPayload(Mutated));FutureDecoded=FutureRead.IsValid();FutureValidated=FutureDecoded&&Saves->ValidateSnapshot(*FutureRead,FutureError);
      UnknownSkipped=FutureValidated&&FutureRead->SchemaVersion==UAftershockSaveGame::CurrentVersion&&FutureRead->CityLife.State.TotalDeaths==Saved->CityLife.State.TotalDeaths&&FutureRead->CityLife.Residents.Num()==Saved->CityLife.Residents.Num()&&FutureRead->Economy.Treasury==Saved->Economy.Treasury;
     }
     break;
    }
    Check(TEXT("unknown_tag_fixture_dictionary_schema_entry_found"),FutureLayout&&SchemaFound);
    Check(TEXT("unknown_tag_fixture_mutation_preserves_name_and_body_lengths"),LengthMatches&&Mutated.Num()==FuturePayload.Num());
    Check(TEXT("unknown_tag_fixture_decodes_after_real_property_skip"),FutureDecoded);
    Check(TEXT("unknown_tag_fixture_passes_full_semantic_validation"),FutureValidated);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_INDEXED_UNKNOWN_TAG forcedSourceSchema=3 dictionaryFound=%d equalNameLength=%d decoded=%d validated=%d preserved=%d error=%s"),SchemaFound,LengthMatches,FutureDecoded,FutureValidated,UnknownSkipped,*FutureError.ToString());
    Check(TEXT("dictionary_prelude_allows_unknown_tagged_field_skip_without_breaking_later_names"),UnknownSkipped);
   }
  }
  {
   TStrongObjectPtr<UAftershockSaveGame> LongQuake(DuplicateObject<UAftershockSaveGame>(Saved.Get(),GI.Get()));
   LongQuake->DisasterVisual.Type=EDisasterType::Earthquake;LongQuake->DisasterVisual.Revision=1;LongQuake->DisasterVisual.DurationSeconds=14.f;
   LongQuake->DisasterVisual.StartedAtWorldSeconds=W->GetTimeSeconds()-2.f;LongQuake->DisasterVisualRemainingSeconds=12.f;
   Check(TEXT("fourteen_second_quake_can_save_twelve_seconds_remaining"),Saves->ValidateSnapshot(*LongQuake,Error));
   LongQuake->DisasterVisualRemainingSeconds=15.f;Check(TEXT("fourteen_second_quake_rejects_fifteen_seconds_remaining"),!Saves->ValidateSnapshot(*LongQuake,Error));LongQuake->DisasterVisualRemainingSeconds=12.f;
   const bool LongRestored=Saves->RestoreWorld(W,LongQuake.Get(),Error);
   TStrongObjectPtr<UAftershockSaveGame> LongCaptured(LongRestored?Saves->CaptureWorld(W,Error):nullptr);
   Check(TEXT("fourteen_second_quake_capture_and_owner_roundtrip_keep_actual_remaining_clock"),LongRestored&&LongCaptured.IsValid()
    &&LongCaptured->DisasterVisual.DurationSeconds==14.f&&FMath::IsNearlyEqual(LongCaptured->DisasterVisualRemainingSeconds,12.f,.001f)
    &&W->GetSubsystem<UDisasterSimulationSubsystem>()->GetLatestVisualSnapshot().GetEffectiveDurationSeconds()==14.f);
   Check(TEXT("quake_clock_fixture_restores_original_world_without_replaying_damage"),Saves->RestoreWorld(W,Saved.Get(),Error)&&Life->GetState().TotalDeaths==Saved->CityLife.State.TotalDeaths&&E->TreasuryTL==Saved->Economy.Treasury);
   LongQuake->DisasterVisual.DurationSeconds=0.f;LongQuake->DisasterVisualRemainingSeconds=10.f;
   const bool TenAccepted=Saves->ValidateSnapshot(*LongQuake,Error);LongQuake->DisasterVisualRemainingSeconds=11.f;
   Check(TEXT("legacy_zero_duration_quake_keeps_exact_ten_second_fallback_bound"),TenAccepted&&!Saves->ValidateSnapshot(*LongQuake,Error));
  }
  {
   TStrongObjectPtr<UAftershockSaveGame> FiniteCase(DuplicateObject<UAftershockSaveGame>(Saved.Get(),GI.Get()));
   const float OriginalRemaining=FiniteCase->DisasterVisualRemainingSeconds;FiniteCase->DisasterVisualRemainingSeconds=std::numeric_limits<float>::quiet_NaN();
   Check(TEXT("finite_schema_plan_rejects_scalar_float_nan"),!Saves->ValidateSnapshot(*FiniteCase,Error));FiniteCase->DisasterVisualRemainingSeconds=OriginalRemaining;
   const double OriginalLatitude=FiniteCase->Scenario.Latitude;FiniteCase->Scenario.Latitude=std::numeric_limits<double>::infinity();
   Check(TEXT("finite_schema_plan_rejects_nested_double_infinity"),!Saves->ValidateSnapshot(*FiniteCase,Error));FiniteCase->Scenario.Latitude=OriginalLatitude;
   const float OriginalElevation=FiniteCase->Terrain.Cells[0].Elevation;FiniteCase->Terrain.Cells[0].Elevation=std::numeric_limits<float>::quiet_NaN();
   Check(TEXT("finite_schema_plan_rejects_float_in_struct_array"),!Saves->ValidateSnapshot(*FiniteCase,Error));FiniteCase->Terrain.Cells[0].Elevation=OriginalElevation;
   FiniteCase->DisasterVisual.LocalSeverity.Add(FiniteCase->Terrain.Cells[0].Coords,std::numeric_limits<float>::quiet_NaN());
   Check(TEXT("finite_schema_plan_rejects_map_float_value_nan"),!Saves->ValidateSnapshot(*FiniteCase,Error));FiniteCase->DisasterVisual=Saved->DisasterVisual;
   if(!FiniteCase->CityLife.BuildingStates.IsEmpty())
   {
    auto It=FiniteCase->CityLife.BuildingStates.CreateIterator();const float Operation=It.Value().Operation01;It.Value().Operation01=std::numeric_limits<float>::infinity();
    Check(TEXT("finite_schema_plan_rejects_nested_struct_map_value_infinity"),!Saves->ValidateSnapshot(*FiniteCase,Error));It.Value().Operation01=Operation;
   }
   if(!FiniteCase->Crowd.Agents.IsEmpty())
   {
    const FVector OriginalLocation=FiniteCase->Crowd.Agents[0].CurrentLocation;FiniteCase->Crowd.Agents[0].CurrentLocation.X=std::numeric_limits<double>::quiet_NaN();
    Check(TEXT("finite_schema_plan_rejects_double_vector_inside_agent_array"),!Saves->ValidateSnapshot(*FiniteCase,Error));FiniteCase->Crowd.Agents[0].CurrentLocation=OriginalLocation;
    FiniteCase->Crowd.Agents[0].RouteGridCoords.Add(FIntPoint(MAX_int32,MAX_int32));
    Check(TEXT("float_free_route_array_still_receives_semantic_coordinate_validation"),!Saves->ValidateSnapshot(*FiniteCase,Error));FiniteCase->Crowd=Saved->Crowd;
   }
   // The actual save schema has only integer/FName sets, hence no floating set
   // sample can be constructed. Their semantic identity checks remain active.
   FiniteCase->CityLife.SeededHomes.Add(MAX_int32);
   Check(TEXT("float_free_set_still_receives_semantic_identity_validation"),!Saves->ValidateSnapshot(*FiniteCase,Error));
  }
  TStrongObjectPtr<UAftershockSaveGame> InvalidSaved(DuplicateObject<UAftershockSaveGame>(Saved.Get(),GI.Get()));
  const int32 HomeID=InvalidSaved->CityLife.Residents[0].HomeBuildingID;InvalidSaved->CityLife.SeededHomes.Remove(HomeID);
  const int32 BeforeInvalidPopulation=Life->GetState().LivingPopulation;const int64 BeforeInvalidMoney=E->TreasuryTL;
  Check(TEXT("missing_seeded_home_rejected_before_resident_reseeding"),!Saves->RestoreWorld(W,InvalidSaved.Get(),Error)&&Life->GetState().LivingPopulation==BeforeInvalidPopulation&&E->TreasuryTL==BeforeInvalidMoney);
  InvalidSaved->CityLife.SeededHomes=Saved->CityLife.SeededHomes;InvalidSaved->DisasterVisual.Revision=-1;Check(TEXT("negative_visual_revision_rejected_before_mutation"),!Saves->RestoreWorld(W,InvalidSaved.Get(),Error)&&E->TreasuryTL==BeforeInvalidMoney);
  InvalidSaved->DisasterVisual=Saved->DisasterVisual;InvalidSaved->DisasterVisual.Type=EDisasterType::Earthquake;InvalidSaved->DisasterVisual.DurationSeconds=0.f;InvalidSaved->DisasterVisualRemainingSeconds=11;Check(TEXT("visual_remaining_cannot_exceed_effective_quake_duration"),!Saves->ValidateSnapshot(*InvalidSaved,Error));InvalidSaved->DisasterVisual=Saved->DisasterVisual;InvalidSaved->DisasterVisualRemainingSeconds=Saved->DisasterVisualRemainingSeconds;
  InvalidSaved->DisasterVisual.DroughtStress01=1.01f;Check(TEXT("visual_drought_bounds_preflight"),!Saves->ValidateSnapshot(*InvalidSaved,Error));InvalidSaved->DisasterVisual=Saved->DisasterVisual;
  if(!InvalidSaved->Crowd.Agents.IsEmpty())
  {
   InvalidSaved->Crowd.Agents[0].WalkingPhaseRadians=7.f;
   Check(TEXT("walking_phase_out_of_cycle_rejected_before_owner_import"),!Saves->ValidateSnapshot(*InvalidSaved,Error));InvalidSaved->Crowd=Saved->Crowd;
   InvalidSaved->Crowd.Agents[0].WalkingPhaseRadians=2.f*PI+.0001f;
   Check(TEXT("walking_phase_exact_restore_boundary_rejected_before_owner_import"),!Saves->ValidateSnapshot(*InvalidSaved,Error));InvalidSaved->Crowd=Saved->Crowd;
   InvalidSaved->Crowd.Agents[0].bInsideVehicle=true;InvalidSaved->Crowd.Agents[0].TransportTripID=999999;
   Check(TEXT("inside_vehicle_unknown_trip_rejected"),!Saves->ValidateSnapshot(*InvalidSaved,Error));InvalidSaved->Crowd=Saved->Crowd;
  }
  TArray<FAftershockTransitStop> PassengerStops;for(const auto& Stop:Saved->Transport.Stops)if(!Stop.bTerminal)PassengerStops.Add(Stop);
  if(PassengerStops.Num()>=2)
  {
   FAftershockPassengerTrip Pending;Pending.TripID=InvalidSaved->Transport.NextTripID++;Pending.ResidentID=Saved->Crowd.Agents[0].ResidentID;Pending.Mode=EAftershockTravelMode::Bus;Pending.State=EAftershockPassengerState::Riding;
   Pending.VehicleID=InvalidSaved->Transport.NextVehicleID++;Pending.PickupStopID=PassengerStops[0].BuildingID;Pending.DropoffStopID=PassengerStops[1].BuildingID;Pending.PickupQueueSlot=0;Pending.PickupRoadCell=PassengerStops[0].RoadCell;Pending.DropoffRoadCell=PassengerStops[1].RoadCell;
   Pending.CurrentLocation=T->GridToWorld(Pending.DropoffRoadCell);Pending.PickupLocation=T->GridToWorld(Pending.PickupRoadCell);Pending.DropoffLocation=Pending.CurrentLocation;
   FAftershockRoadVehicle Bus;Bus.VehicleID=Pending.VehicleID;Bus.bBus=true;Bus.Capacity=20;Bus.Location=Pending.CurrentLocation;Bus.PassengerTripIDs.Add(Pending.TripID);Bus.PendingAlightTripIDs.Add(Pending.TripID);Bus.AlightReleaseSeconds=.24f;Bus.ServiceStopIDs.Add(Pending.DropoffStopID);
   for(const auto& Stop:Saved->Transport.Stops)if(Stop.bTerminal){Bus.TerminalBuildingID=Stop.BuildingID;break;}
   InvalidSaved->Transport.Trips.Add(Pending);InvalidSaved->Transport.Vehicles.Add(Bus);
   Check(TEXT("pending_bus_alighting_retains_real_riding_membership_on_save"),Saves->ValidateSnapshot(*InvalidSaved,Error));
   InvalidSaved->Transport.Vehicles.Last().PendingAlightTripIDs.Add(Pending.TripID);Check(TEXT("duplicate_pending_alighting_trip_rejected"),!Saves->ValidateSnapshot(*InvalidSaved,Error));InvalidSaved->Transport.Vehicles.Last().PendingAlightTripIDs.Pop();
   InvalidSaved->Transport.Vehicles.Last().AlightReleaseSeconds=.6f;Check(TEXT("invalid_alighting_release_clock_rejected"),!Saves->ValidateSnapshot(*InvalidSaved,Error));InvalidSaved->Transport.Vehicles.Last().AlightReleaseSeconds=.24f;
   InvalidSaved->Transport.Vehicles.Last().ServiceStopIDs[0]=Pending.PickupStopID;Check(TEXT("alighting_at_wrong_bus_stop_rejected"),!Saves->ValidateSnapshot(*InvalidSaved,Error));
   InvalidSaved->Transport=Saved->Transport;InvalidSaved->Crowd=Saved->Crowd;
  }
  FAftershockPassengerTrip MissingPassenger;MissingPassenger.TripID=41;MissingPassenger.ResidentID=Saved->Crowd.Agents[0].ResidentID;MissingPassenger.Mode=EAftershockTravelMode::Bus;MissingPassenger.State=EAftershockPassengerState::Riding;MissingPassenger.VehicleID=Saved->Transport.Vehicles.IsEmpty()?1:Saved->Transport.Vehicles[0].VehicleID;
  if(PassengerStops.Num()>=2){MissingPassenger.PickupStopID=PassengerStops[0].BuildingID;MissingPassenger.DropoffStopID=PassengerStops[1].BuildingID;MissingPassenger.PickupRoadCell=PassengerStops[0].RoadCell;MissingPassenger.DropoffRoadCell=PassengerStops[1].RoadCell;MissingPassenger.PickupQueueSlot=0;MissingPassenger.PickupLocation=T->GridToWorld(MissingPassenger.PickupRoadCell);MissingPassenger.DropoffLocation=T->GridToWorld(MissingPassenger.DropoffRoadCell);MissingPassenger.CurrentLocation=MissingPassenger.DropoffLocation;}
  InvalidSaved->Transport.Trips.Add(MissingPassenger);Check(TEXT("riding_trip_without_vehicle_passenger_membership_rejected"),!Saves->ValidateSnapshot(*InvalidSaved,Error));
  InvalidSaved->Transport=Saved->Transport;MissingPassenger.State=EAftershockPassengerState::Arrived;
  // Construct a real, known bus record for the deliberate between-timers
  // handoff fixture. An invented VehicleID with no vehicle is correctly invalid.
  FAftershockRoadVehicle HandoffBus;HandoffBus.VehicleID=InvalidSaved->Transport.NextVehicleID++;HandoffBus.bBus=true;HandoffBus.Capacity=20;HandoffBus.Location=MissingPassenger.DropoffLocation;
  for(const auto& Stop:Saved->Transport.Stops)if(Stop.bTerminal){HandoffBus.TerminalBuildingID=Stop.BuildingID;break;}
  HandoffBus.ServiceStopIDs.Add(MissingPassenger.DropoffStopID);MissingPassenger.VehicleID=HandoffBus.VehicleID;
  InvalidSaved->Transport.Vehicles.Add(HandoffBus);InvalidSaved->Transport.Trips.Add(MissingPassenger);
  auto& HandoffAgent=InvalidSaved->Crowd.Agents[0];HandoffAgent.bInsideVehicle=true;HandoffAgent.TransportTripID=MissingPassenger.TripID;HandoffAgent.CurrentLocation=MissingPassenger.CurrentLocation;HandoffAgent.bWaitingForOutdoorSpace=false;HandoffAgent.OutdoorDepartureBuildingID=INDEX_NONE;HandoffAgent.OutdoorDepartureLocation=FVector::ZeroVector;
  Check(TEXT("known_terminal_passenger_handoff_between_timers_is_saveable"),Saves->ValidateSnapshot(*InvalidSaved,Error));
  InvalidSaved->Crowd=Saved->Crowd;InvalidSaved->Transport=Saved->Transport;MissingPassenger.State=EAftershockPassengerState::Waiting;MissingPassenger.VehicleID=INDEX_NONE;MissingPassenger.CurrentLocation=MissingPassenger.PickupLocation;InvalidSaved->Transport.Trips.Add(MissingPassenger);
  Check(TEXT("valid_single_waiting_bus_trip_is_saveable"),Saves->ValidateSnapshot(*InvalidSaved,Error));
  auto DuplicateTrip=MissingPassenger;DuplicateTrip.TripID=42;DuplicateTrip.PickupQueueSlot=1;InvalidSaved->Transport.NextTripID=43;InvalidSaved->Transport.Trips.Add(DuplicateTrip);
  Check(TEXT("same_resident_cannot_restore_two_active_trips"),!Saves->ValidateSnapshot(*InvalidSaved,Error));
  InvalidSaved->Transport.Trips.Pop();InvalidSaved->Transport.Trips[0].PickupStopID=999999;Check(TEXT("active_bus_trip_requires_known_pickup_and_dropoff"),!Saves->ValidateSnapshot(*InvalidSaved,Error));
  Check(TEXT("dead_resident_remains_in_roster"),Saved->CityLife.Residents.ContainsByPredicate([&](const auto& X){return X.ResidentID==DeadID&&!X.bAlive;})&&!Saved->Crowd.Agents.ContainsByPredicate([&](const auto& X){return X.ResidentID==DeadID;}));
  Check(TEXT("seeded_homes_and_pending_fare_persist"),Saved->CityLife.SeededHomes.Num()==LS.SeededHomes.Num()&&Saved->CityLife.PaidTransitTrips.Contains(17)&&Saved->CityLife.PendingTransitRevenueTL==35);
  const FString Path=UAftershockSaveSubsystem::SlotPath(TEXT("Audit"));
  // Only the dedicated, fixed Audit slot is touched; manual/autosave slots are never deleted.
  for(const FString& File:{Path,Path+TEXT(".bak"),Path+TEXT(".tmp")})IFileManager::Get().Delete(*File,false,true);
  {
   // Exercise production SaveNow, not a caller-owned CaptureWorld object. The
   // subsystem may reuse exactly one private scratch, but public reads may not.
   const auto ScratchPayloadEmpty=[&]()
   {
    const auto* Scratch=Saves->InternalSaveScratch.Get();
    return Scratch&&Scratch->Terrain.Cells.IsEmpty()&&Scratch->Terrain.Cells.GetAllocatedSize()==0
     &&Scratch->Terrain.Chunks.IsEmpty()&&Scratch->Buildings.Buildings.IsEmpty()&&Scratch->Rubble.IsEmpty()
     &&Scratch->CityLife.Residents.IsEmpty()&&Scratch->CityLife.Residents.GetAllocatedSize()==0
     &&Scratch->CityLife.BuildingStates.IsEmpty()&&Scratch->CityLife.BuildingRoadComponents.IsEmpty()
     &&Scratch->CityLife.SeededHomes.IsEmpty()&&Scratch->CityLife.LastServiceVisitTurn.IsEmpty()&&Scratch->CityLife.PaidTransitTrips.IsEmpty()
     &&Scratch->Crowd.Agents.IsEmpty()&&Scratch->Crowd.Agents.GetAllocatedSize()==0
     &&Scratch->Transport.Trips.IsEmpty()&&Scratch->Transport.Vehicles.IsEmpty()&&Scratch->Transport.Stops.IsEmpty()
     &&Scratch->Economy.AppliedDisasterEventIDs.IsEmpty()&&Scratch->DisasterVisual.LocalSeverity.IsEmpty()
     &&Scratch->DisasterVisual.LavaSeverity.IsEmpty()&&Scratch->DisasterVisual.AshSeverity.IsEmpty()
     &&Scratch->DisasterVisual.RiverFillBefore.IsEmpty()&&Scratch->DisasterVisual.RiverFillAfter.IsEmpty()
     &&Scratch->CasualtyLedger.Entries.IsEmpty()&&Scratch->CasualtyLedger.SurvivalStories.IsEmpty()
     &&Scratch->CasualtyLedger.BuildingWitnessStories.IsEmpty()&&Scratch->CasualtyLedger.Events.IsEmpty()
     &&Scratch->CasualtyLedger.FlowerResidentIDs.IsEmpty()&&Scratch->CasualtyLedger.RetrofittedBuildingIDs.IsEmpty()
     &&Scratch->CasualtyLedger.ExposedSurvivorResidentIDs.IsEmpty()&&Scratch->CasualtyLedger.ArchiveSummaries.IsEmpty()
     &&Scratch->Scenario.AssemblyCells.IsEmpty()&&Scratch->Quiz.AskedRowNames.IsEmpty()&&Scratch->Campaign.CompletedStages.IsEmpty();
   };
   const uint64 BeforeFirstValidation=Saves->SnapshotValidationCount;
   const bool FirstSave=Saves->SaveNow(W,TEXT("Audit"),Error);UAftershockSaveGame* FirstScratch=Saves->InternalSaveScratch.Get();
   Check(TEXT("first_save_now_uses_private_scratch_and_releases_heavy_payload_arrays"),FirstSave&&FirstScratch&&FirstScratch!=Saved.Get()&&ScratchPayloadEmpty());
   Check(TEXT("private_save_now_validates_immutable_capture_exactly_once"),FirstSave&&Saves->SnapshotValidationCount==BeforeFirstValidation+1);
   const uint64 BeforeSecondValidation=Saves->SnapshotValidationCount,BeforeSecondDigestHit=Saves->ValidatedDiskDigestHits;
   const bool SecondSave=Saves->SaveNow(W,TEXT("Audit"),Error);
   Check(TEXT("second_save_now_reuses_identical_private_scratch_without_retaining_world_arrays"),SecondSave&&FirstScratch&&Saves->InternalSaveScratch.Get()==FirstScratch&&ScratchPayloadEmpty());
   Check(TEXT("second_private_save_validates_once_and_reuses_exact_validated_backup_revision"),SecondSave&&Saves->SnapshotValidationCount==BeforeSecondValidation+1&&Saves->ValidatedDiskDigestHits==BeforeSecondDigestHit+1);
   // Opt-in commandlet access to the same byte worker, restricted to Audit.
   // Production Autosave intentionally refuses to touch user slots in fixtures.
   const int64 BeforeAsyncTreasury=E->TreasuryTL;const int32 BeforeAsyncPopulation=Life->GetState().LivingPopulation;
   const int32 BeforeAsyncRotation=Saves->NextAutosave;
   const bool AsyncQueued=Saves->BeginAutosaveWrite(W,TEXT("Audit"),Error);
   Check(TEXT("async_autosave_queues_one_byte_job_and_releases_gt_snapshot"),AsyncQueued&&Saves->bWriting&&Saves->ActiveAutosaveJob.IsValid()&&ScratchPayloadEmpty());
   const auto QueuedJob=Saves->ActiveAutosaveJob;
   Check(TEXT("async_autosave_coalesces_second_request_without_replacing_job"),AsyncQueued&&!Saves->BeginAutosaveWrite(W,TEXT("Audit"),Error)&&Saves->ActiveAutosaveJob==QueuedJob);
   Check(TEXT("async_pending_manual_save_and_save_exit_reject_without_quitting"),AsyncQueued&&!Saves->SaveNow(W,TEXT("Audit"),Error)&&!Saves->SaveAndExit(W,Error));
   Check(TEXT("async_pending_blocks_continue_and_pending_restore"),AsyncQueued&&!Saves->QueueContinue(W,Error)&&!Saves->RestorePendingWorld(W,Camera,Error)&&!Saves->HasPendingLoad());
   Check(TEXT("async_pending_blocks_public_write_and_read"),AsyncQueued&&!Saves->WriteSnapshot(Saved.Get(),TEXT("Audit"),Error)&&Saves->ReadSnapshot(TEXT("Audit"),Error)==nullptr);
   Check(TEXT("async_pending_restore_cannot_mutate_live_city"),AsyncQueued&&!Saves->RestoreWorld(W,Saved.Get(),Error)&&E->TreasuryTL==BeforeAsyncTreasury&&Life->GetState().LivingPopulation==BeforeAsyncPopulation);
   const bool AsyncCommitted=AsyncQueued&&Saves->CompletePendingAutosave(true,false);
   Check(TEXT("async_byte_job_completes_without_gt_dependency_and_releases_lock"),AsyncCommitted&&!Saves->bWriting&&!Saves->ActiveAutosaveJob.IsValid()&&ScratchPayloadEmpty());
   TStrongObjectPtr<UAftershockSaveGame> AsyncDisk(AsyncCommitted?Saves->ReadSnapshot(TEXT("Audit"),Error):nullptr);
   Check(TEXT("async_committed_audit_envelope_decodes_same_canonical_city"),AsyncDisk.IsValid()&&AsyncDisk->SessionID==Session->GetSessionID()&&AsyncDisk->Economy.Treasury==BeforeAsyncTreasury&&AsyncDisk->CityLife.State.LivingPopulation==BeforeAsyncPopulation);
   Check(TEXT("async_completion_is_idempotent_and_audit_slot_does_not_rotate_user_autosaves"),AsyncCommitted&&!Saves->CompletePendingAutosave(true,false)&&Saves->NextAutosave==BeforeAsyncRotation);
   TStrongObjectPtr<UAftershockSaveGame> ReadOne(Saves->ReadSnapshot(TEXT("Audit"),Error)),ReadTwo(Saves->ReadSnapshot(TEXT("Audit"),Error));
   const bool IndependentObjects=ReadOne.IsValid()&&ReadTwo.IsValid()&&ReadOne.Get()!=ReadTwo.Get()&&ReadOne.Get()!=FirstScratch&&ReadTwo.Get()!=FirstScratch;
   Check(TEXT("public_snapshot_reads_return_distinct_objects_separate_from_save_scratch"),IndependentObjects);
   bool IndependentPayload=false;
   if(IndependentObjects&&!ReadOne->Terrain.Cells.IsEmpty()&&!ReadTwo->Terrain.Cells.IsEmpty()&&!ReadOne->CityLife.Residents.IsEmpty()&&!ReadTwo->CityLife.Residents.IsEmpty())
   {
    const int64 OtherBalance=ReadTwo->Economy.Treasury;const float OtherElevation=ReadTwo->Terrain.Cells[0].Elevation,OtherHealth=ReadTwo->CityLife.Residents[0].Health01;
    ReadOne->Economy.Treasury+=19;ReadOne->Terrain.Cells[0].Elevation+=3.f;ReadOne->CityLife.Residents[0].Health01=.375f;
    IndependentPayload=ReadOne->Terrain.Cells.GetData()!=ReadTwo->Terrain.Cells.GetData()&&ReadOne->CityLife.Residents.GetData()!=ReadTwo->CityLife.Residents.GetData()
     &&ReadTwo->Economy.Treasury==OtherBalance&&ReadTwo->Terrain.Cells[0].Elevation==OtherElevation&&ReadTwo->CityLife.Residents[0].Health01==OtherHealth;
    TStrongObjectPtr<UAftershockSaveGame> ReadAgain(Saves->ReadSnapshot(TEXT("Audit"),Error));
    IndependentPayload&=ReadAgain.IsValid()&&ReadAgain->Economy.Treasury==OtherBalance&&ReadAgain->Terrain.Cells[0].Elevation==OtherElevation&&ReadAgain->CityLife.Residents[0].Health01==OtherHealth&&ScratchPayloadEmpty();
   }
   Check(TEXT("mutating_one_public_snapshot_cannot_change_another_snapshot_or_disk"),IndependentPayload);
   if(ReadTwo.IsValid())
   {
    TArray<uint8> MainBefore,MainAfter;const bool HadMain=FFileHelper::LoadFileToArray(MainBefore,*Path);
    TStrongObjectPtr<UAftershockSaveGame> PublicMutation(DuplicateObject<UAftershockSaveGame>(ReadTwo.Get(),GI.Get()));
    const uint64 BeforePublicValidation=Saves->SnapshotValidationCount;
    if(!PublicMutation->CityLife.Residents.IsEmpty())PublicMutation->CityLife.Residents[0].Health01=-1.f;
    const bool Rejected=!Saves->WriteSnapshot(PublicMutation.Get(),TEXT("Audit"),Error);
    const bool HaveMainAfter=FFileHelper::LoadFileToArray(MainAfter,*Path);
    Check(TEXT("public_write_always_revalidates_mutated_snapshot_without_touching_disk"),HadMain&&HaveMainAfter&&Rejected&&MainBefore==MainAfter&&Saves->SnapshotValidationCount==BeforePublicValidation+1);

    // Keep byte count and timestamp identical. Only the actual envelope digest
    // can detect this external edit; a metadata-only validation cache is unsafe.
    TArray<uint8> BackupBefore,BackupAfter;const bool HadBackup=FFileHelper::LoadFileToArray(BackupBefore,*(Path+TEXT(".bak")));
    const FDateTime OriginalTimestamp=IFileManager::Get().GetTimeStamp(*Path);TArray<uint8> ExternalBytes=MainBefore;
    constexpr int32 EnvelopeDigestOffset=sizeof(uint32)+sizeof(int32)*3;
    if(ExternalBytes.IsValidIndex(EnvelopeDigestOffset))ExternalBytes[EnvelopeDigestOffset]^=0x35;
    const bool ExternalWritten=FFileHelper::SaveArrayToFile(ExternalBytes,*Path);IFileManager::Get().SetTimeStamp(*Path,OriginalTimestamp);
    const uint64 BeforeDigestMiss=Saves->ValidatedDiskDigestMisses;
    const bool ReplacedCorrupt=Saves->WriteSnapshot(ReadTwo.Get(),TEXT("Audit"),Error);
    const bool HaveBackupAfter=FFileHelper::LoadFileToArray(BackupAfter,*(Path+TEXT(".bak")));
    Check(TEXT("same_size_timestamp_external_revision_misses_digest_and_preserves_good_backup"),HadMain&&HadBackup&&ExternalWritten&&ExternalBytes.Num()==MainBefore.Num()&&ReplacedCorrupt&&Saves->ValidatedDiskDigestMisses==BeforeDigestMiss+1&&HaveBackupAfter&&BackupBefore==BackupAfter);
    TStrongObjectPtr<UAftershockSaveGame> AfterExternal(Saves->ReadSnapshot(TEXT("Audit"),Error));
    Check(TEXT("digest_miss_uses_full_disk_decode_before_promoting_backup"),AfterExternal.IsValid()&&AfterExternal->Economy.Treasury==ReadTwo->Economy.Treasury&&ScratchPayloadEmpty());

    // Metadata-only cache admission is restricted to exactly15 fixed paths.
    // Reuse already fully validated bytes without writing any player slot.
    TArray<uint8> ValidEnvelope;const bool HaveEnvelope=FFileHelper::LoadFileToArray(ValidEnvelope,*Path);
    if(HaveEnvelope)for(const TCHAR* Slot:{TEXT("Manual"),TEXT("Auto0"),TEXT("Auto1"),TEXT("Auto2"),TEXT("Audit")})
    {const FString Candidate=UAftershockSaveSubsystem::SlotPath(Slot);for(const FString& Suffix:{FString(),FString(TEXT(".bak")),FString(TEXT(".tmp"))})Saves->RememberValidatedDiskRevision(Candidate+Suffix,ValidEnvelope);}
    const int32 FixedEntries=Saves->ValidatedDiskDigests.Num();
    for(int32 I=0;I<64;++I)Saves->RememberValidatedDiskRevision(Path+FString::Printf(TEXT(".untrusted%d"),I),ValidEnvelope);
    Check(TEXT("disk_validation_cache_is_fifteen_fixed_digest_entries_without_snapshot_payloads"),HaveEnvelope&&FixedEntries==15&&Saves->ValidatedDiskDigests.Num()==15&&ScratchPayloadEmpty());
   }
  }
  // Restart the existing checksum/backup fixture from its original empty Audit
  // slot, so scratch-reuse coverage does not change the backup under test.
  for(const FString& File:{Path,Path+TEXT(".bak"),Path+TEXT(".tmp")})IFileManager::Get().Delete(*File,false,true);
  {
   // Actual historic envelope1/raw and envelope2/zlib files retain the engine
   // save-game payload reader. Only the dedicated Audit slot is touched.
   TArray<uint8> LegacyPayload;const bool LegacyEncoded=UGameplayStatics::SaveGameToMemory(Saved.Get(),LegacyPayload);
   for(int32 LegacyVersion:{1,2})
   {
    TArray<uint8> Body=LegacyPayload;bool Good=LegacyEncoded&&!Body.IsEmpty();
    if(LegacyVersion==2&&Good)
    {
     int32 Size=FCompression::CompressMemoryBound(NAME_Zlib,LegacyPayload.Num());Body.SetNumUninitialized(Size);
     Good=FCompression::CompressMemory(NAME_Zlib,Body.GetData(),Size,LegacyPayload.GetData(),LegacyPayload.Num(),COMPRESS_BiasSpeed);Body.SetNum(Size,EAllowShrinking::No);
    }
    TArray<uint8> Envelope;FMemoryWriter Writer(Envelope,true);uint32 Magic=0x41465348;int32 Version=LegacyVersion,Count=Body.Num(),Uncompressed=LegacyPayload.Num();uint8 Digest[FSHA1::DigestSize];FSHA1::HashBuffer(LegacyPayload.GetData(),LegacyPayload.Num(),Digest);
    Writer<<Magic;Writer<<Version;Writer<<Count;if(Version==2)Writer<<Uncompressed;Writer.Serialize(Digest,sizeof(Digest));if(Count)Writer.Serialize(Body.GetData(),Count);Writer.Close();
    Good&=FFileHelper::SaveArrayToFile(Envelope,*Path);
    TStrongObjectPtr<UAftershockSaveGame> LegacyRead(Good?Saves->ReadSnapshot(TEXT("Audit"),Error):nullptr);
    Check(*FString::Printf(TEXT("historic_envelope_%d_engine_payload_remains_readable_and_semantically_valid"),LegacyVersion),Good&&LegacyRead.IsValid()&&LegacyRead->SessionID==Saved->SessionID&&LegacyRead->CityLife.State.TotalDeaths==Saved->CityLife.State.TotalDeaths&&LegacyRead->CityLife.Residents.Num()==Saved->CityLife.Residents.Num()&&LegacyRead->Economy.Treasury==Saved->Economy.Treasury);
    for(const FString& File:{Path,Path+TEXT(".bak"),Path+TEXT(".tmp")})IFileManager::Get().Delete(*File,false,true);
   }
  }
  Check(TEXT("typed_checksum_disk_write"),Saves->WriteSnapshot(Saved.Get(),TEXT("Audit"),Error));
  {
   TArray<uint8> VersionBytes,TypedPayload;uint32 Magic=0,PayloadMagic=0,PayloadVersion=0,PayloadRoot=0;int32 Version=0;
   bool ActualCodec=false;
   if(FFileHelper::LoadFileToArray(VersionBytes,*Path))
   {
    FMemoryReader Reader(VersionBytes,true);int32 Count=0,Uncompressed=0;uint8 Digest[FSHA1::DigestSize];
    Reader<<Magic;Reader<<Version;Reader<<Count;Reader<<Uncompressed;Reader.Serialize(Digest,FSHA1::DigestSize);
    if(!Reader.IsError()&&Version==3&&Count>0&&Count==Reader.TotalSize()-Reader.Tell()&&Uncompressed>=12&&Uncompressed<=256*1024*1024)
    {
     TypedPayload.SetNumUninitialized(Uncompressed);
     if(FCompression::UncompressMemory(NAME_Zlib,TypedPayload.GetData(),static_cast<int64>(Uncompressed),VersionBytes.GetData()+Reader.Tell(),static_cast<int64>(Count)))
     {
      FMemoryReader PayloadReader(TypedPayload,true);PayloadReader<<PayloadMagic;PayloadReader<<PayloadVersion;PayloadReader<<PayloadRoot;
      TStrongObjectPtr<UAftershockSaveGame> ActualRead(Saves->ReadTypedEnvelope(VersionBytes));
      ActualCodec=!PayloadReader.IsError()&&PayloadMagic==0x4146494E&&PayloadVersion==2&&PayloadRoot==0x4153474D&&ActualRead.IsValid()
       &&ActualRead->SessionID==Saved->SessionID&&ActualRead->Economy.Treasury==Saved->Economy.Treasury&&ActualRead->CityLife.Residents.Num()==Saved->CityLife.Residents.Num();
     }
    }
   }
   Check(TEXT("new_writes_use_envelope_three_indexed_binary_payload_two"),Magic==0x41465348&&Version==3&&ActualCodec);
  }
  const int64 OriginalBalance=Saved->Economy.Treasury;Saved->Economy.Treasury+=7;Check(TEXT("second_write_preserves_valid_backup"),Saves->WriteSnapshot(Saved.Get(),TEXT("Audit"),Error));
  // Corrupt the Envelope2 digest deliberately, keeping the compressed stream
  // decodable so this checksum test does not produce an expected engine Error.
  TArray<uint8> Corrupt;FFileHelper::LoadFileToArray(Corrupt,*Path);constexpr int32 DigestOffset=sizeof(uint32)+sizeof(int32)*3;if(Corrupt.IsValidIndex(DigestOffset))Corrupt[DigestOffset]^=0x55;FFileHelper::SaveArrayToFile(Corrupt,*Path);
  TStrongObjectPtr<UAftershockSaveGame> Recovered(Saves->ReadSnapshot(TEXT("Audit"),Error));Check(TEXT("checksum_failure_uses_last_valid_backup"),Recovered.IsValid()&&Recovered->Economy.Treasury==OriginalBalance);
  if(Recovered.IsValid())
  {
   E->TreasuryTL=1;FAftershockCityLifeSaveState Changed;Life->CaptureSaveState(Changed);Changed.State.FoodStock=1;Life->RestoreSaveState(Changed,Error);
   Check(TEXT("complete_bulk_owner_restore"),Saves->RestoreWorld(W,Recovered.Get(),Error));
   FAftershockCityLifeSaveState LoadedLife;Life->CaptureSaveState(LoadedLife);FAftershockDirectorState LoadedDirector;D->CaptureSaveState(LoadedDirector);FAftershockBuildingsSaveState LoadedBuildings;B->CaptureSaveState(LoadedBuildings);
   Check(TEXT("stocks_and_money_restored_exactly"),E->TreasuryTL==OriginalBalance&&LoadedLife.State.FoodStock==123.25f&&LoadedLife.State.RawFoodStock==77.5f&&LoadedLife.State.WaterStock==66.75f);
   Check(TEXT("fare_cannot_charge_again_after_restore"),LoadedLife.PendingTransitRevenueTL==35&&!Life->RecordTransitFare(2,17));
   Check(TEXT("dead_resident_and_seeded_homes_not_regenerated"),LoadedLife.Residents.Num()==LS.Residents.Num()&&LoadedLife.State.LivingPopulation==LS.State.LivingPopulation&&LoadedLife.SeededHomes.Num()==LS.SeededHomes.Num());
   bool OperationalVisuals=true;for(const auto& Pair:LoadedLife.BuildingStates)
   {FBuildingInstance Building;if(!B->GetBuildingByID(Pair.Key,Building)){OperationalVisuals=false;continue;}const auto* ISM=B->GetOrCreateISM(Building.BuildingData);const int32 Index=ISM?Building.ISMInstanceIndex*ISM->NumCustomDataFloats+2:INDEX_NONE;OperationalVisuals&=ISM&&ISM->PerInstanceSMCustomData.IsValidIndex(Index+1)&&FMath::IsNearlyEqual(ISM->PerInstanceSMCustomData[Index],Pair.Value.PowerCoverage01)&&FMath::IsNearlyEqual(ISM->PerInstanceSMCustomData[Index+1],Pair.Value.Operation01);}
   Check(TEXT("loaded_power_and_operation_restore_actual_ism_custom_data"),OperationalVisuals);
   Check(TEXT("building_yaw_and_camera_typed_state_survive"),!LoadedBuildings.Buildings.IsEmpty()&&FMath::IsNearlyEqual(LoadedBuildings.Buildings[0].WorldTransform.Rotator().Yaw,90.f,.01f));
   FAftershockCameraSaveState LoadedCamera;if(Camera)Camera->CaptureSaveState(LoadedCamera);Check(TEXT("globe_pitch_and_return_to_city_zoom_persist"),Camera&&LoadedCamera.SavedLocalZoom==7200&&LoadedCamera.PlanetOrbitPitch==33&&LoadedCamera.OverviewBlend==.7f);
   FRandomStream Expected(987654321),Actual(LoadedDirector.RandomSeed);Check(TEXT("director_rng_and_warning_do_not_restart"),LoadedDirector.RemainingSeconds==8.5f&&LoadedDirector.Phase==EAftershockDisasterPhase::Warning&&Expected.GetUnsignedInt()==Actual.GetUnsignedInt()&&LoadedDirector.EventSequence==3&&LoadedDirector.LastAppliedEventSequence==2);
   const int64 Before=E->TreasuryTL;Recovered->Terrain.Cells[0].Elevation=std::numeric_limits<float>::quiet_NaN();Check(TEXT("invalid_load_rejected_without_money_mutation"),!Saves->RestoreWorld(W,Recovered.Get(),Error)&&E->TreasuryTL==Before);
   // Exercise the same pending-owner path as cold Continue without level travel.
   // The fixture's corruption is explicit; no manual/autosave slot is modified.
   Saves->PendingLoad=Recovered.Get();
   Check(TEXT("failed_pending_load_marks_world_and_clears_pending_without_reseed"),!Saves->RestorePendingWorld(W,Camera,Error)&&Saves->IsWorldLoadFailed(W)&&!Saves->HasPendingLoad()&&!Saves->GetLoadFailure(W).IsEmpty()&&E->TreasuryTL==Before&&Life->GetState().TotalDeaths==Saved->CityLife.State.TotalDeaths);
   TArray<uint8> BlockedMainBefore,BlockedBackupBefore,BlockedMainAfter,BlockedBackupAfter;
   const bool ReadBlockedBefore=FFileHelper::LoadFileToArray(BlockedMainBefore,*Path)&&FFileHelper::LoadFileToArray(BlockedBackupBefore,*(Path+TEXT(".bak")));
   Check(TEXT("failed_load_world_cannot_overwrite_save"),!Saves->SaveNow(W,TEXT("Audit"),Error)&&E->TreasuryTL==Before);
   const bool ReadBlockedAfter=FFileHelper::LoadFileToArray(BlockedMainAfter,*Path)&&FFileHelper::LoadFileToArray(BlockedBackupAfter,*(Path+TEXT(".bak")));
   Check(TEXT("failed_world_save_preserves_main_and_backup_bytes_exactly"),ReadBlockedBefore&&ReadBlockedAfter&&!BlockedMainBefore.IsEmpty()&&!BlockedBackupBefore.IsEmpty()&&BlockedMainBefore==BlockedMainAfter&&BlockedBackupBefore==BlockedBackupAfter);
   const int32 AutoIndex=Saves->NextAutosave;Saves->Autosave(W);
   Check(TEXT("failed_load_world_does_not_rotate_autosave"),Saves->NextAutosave==AutoIndex);
   UWorld* OtherWorld=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("AftershockRecoveryGuardFixture")));
   Check(TEXT("failed_load_guard_does_not_poison_new_world"),OtherWorld&&!Saves->IsWorldLoadFailed(OtherWorld));
   if(OtherWorld){OtherWorld->DestroyWorld(false);OtherWorld->RemoveFromRoot();}
   Saves->PendingLoad=Saved.Get();
   Check(TEXT("successful_pending_retry_clears_failure_and_restores_exact_state"),Saves->RestorePendingWorld(W,Camera,Error)&&!Saves->IsWorldLoadFailed(W)&&!Saves->HasPendingLoad()&&Saves->GetLoadFailure(W).IsEmpty()&&E->TreasuryTL==Saved->Economy.Treasury&&Life->GetState().TotalDeaths==Saved->CityLife.State.TotalDeaths);
  }
  // A new configured V3 source world cannot be relabelled as a pre-source V1 save. Keep
  // the migration guard strict and exercise genuine V1 terrain separately.
  TStrongObjectPtr<UAftershockSaveGame> FakeLegacy(DuplicateObject<UAftershockSaveGame>(Saved.Get(),GI.Get()));FakeLegacy->SchemaVersion=1;
  FakeLegacy->Terrain.TerrainStorageVersion=1;FakeLegacy->Terrain.TerrainRevision=0;FakeLegacy->Terrain.Chunks.Reset();FakeLegacy->Terrain.bInitialMaskUnlocked=false;
  Check(TEXT("real_v3_source_cannot_be_silently_relabelled_as_schema_one"),Saved->Terrain.ContinuationGeneratorVersion==3&&!Saves->ValidateSnapshot(*FakeLegacy,Error)&&FakeLegacy->SchemaVersion==1&&FakeLegacy->Terrain.ContinuationGeneratorVersion==3);
  FVector SupportedFocus=FVector::ZeroVector;bool FoundNonResident=false;
  for(const FIntPoint Cell:{FIntPoint(T->GridWidth+2,T->GridHeight/2),FIntPoint(-3,T->GridHeight/2),FIntPoint(T->GridWidth/2,T->GridHeight+2),FIntPoint(T->GridWidth/2,-3)})
  {
   const FVector Candidate=T->GridToWorld(Cell);double Latitude=0.,Longitude=0.;
   if(!T->IsValidCoord(Cell)&&T->TryWorldToGeographic(Candidate,Latitude,Longitude)&&T->TryGeographicToWorld(Latitude,Longitude,SupportedFocus)){FoundNonResident=true;break;}
  }
  FAftershockCameraSaveState HighCamera;if(Camera)Camera->CaptureSaveState(HighCamera);HighCamera.Zoom=1.40162e9f;HighCamera.OverviewBlend=1.f;HighCamera.LocalFocus=SupportedFocus;
  const int32 ResidentCellCount=T->Cells.Num();const bool CameraImported=Camera&&FoundNonResident&&Camera->RestoreSaveState(HighCamera,Error);
  TStrongObjectPtr<UAftershockSaveGame> HighSourceCamera(CameraImported?Saves->CaptureWorld(W,Error):nullptr);
  Check(TEXT("v3_globe_zoom_and_supported_nonresident_focus_capture_without_generating_cells"),FoundNonResident&&CameraImported&&HighSourceCamera.IsValid()&&HighSourceCamera->Terrain.ContinuationGeneratorVersion==3&&HighSourceCamera->Camera.Zoom==HighCamera.Zoom&&HighSourceCamera->Camera.LocalFocus.Equals(SupportedFocus,.1)&&T->Cells.Num()==ResidentCellCount);
  const bool HighSourceRestored=HighSourceCamera.IsValid()&&Saves->ValidateSnapshot(*HighSourceCamera,Error)&&Saves->RestoreWorld(W,HighSourceCamera.Get(),Error);
  FAftershockCameraSaveState RestoredSourceCamera;if(Camera)Camera->CaptureSaveState(RestoredSourceCamera);
  Check(TEXT("v3_source_supported_nonresident_camera_roundtrip_preserves_zoom_focus_and_resident_storage"),HighSourceRestored&&RestoredSourceCamera.Zoom==HighCamera.Zoom&&RestoredSourceCamera.LocalFocus.Equals(SupportedFocus,.1)&&T->Cells.Num()==ResidentCellCount&&!T->IsValidCoord(T->WorldToGrid(SupportedFocus)));
  Check(TEXT("path_traversal_slot_rejected"),!Saves->WriteSnapshot(Saved.Get(),TEXT("../Manual"),Error)&&UAftershockSaveSubsystem::SlotPath(TEXT("../Manual")).IsEmpty());
  for(const FString& File:{Path,Path+TEXT(".bak"),Path+TEXT(".tmp")})IFileManager::Get().Delete(*File,false,true);
 }
 // Explicit unconfigured startup retains the authored V1 geography contract;
 // migration tests must not manufacture V1 by erasing V2 source metadata.
 {
  TStrongObjectPtr<UGameInstance> LegacyGI(NewObject<UGameInstance>(GEngine));LegacyGI->InitializeStandalone(TEXT("AftershockLegacySaveFixture"));UWorld* LW=LegacyGI->GetWorld();
  Check(TEXT("separate_authored_v1_save_world_created"),LW!=nullptr);
  if(LW)
  {
   auto* LegacyScenario=LW->GetSubsystem<UAftershockScenarioSubsystem>();auto* LegacySession=LegacyGI->GetSubsystem<UAftershockOpeningSessionSubsystem>();auto* LegacySaves=LegacyGI->GetSubsystem<UAftershockSaveSubsystem>();
   const bool Prepared=LegacyScenario->PrepareScenarioBeforeSettlement();if(Prepared)LW->GetSubsystem<UBuildingManagerSubsystem>()->SeedInitialSettlement();LegacySession->MarkSessionStarted();
   auto* LegacyCamera=LW->SpawnActor<AAftershockCameraPawn>();auto* LegacyTerrain=LW->GetSubsystem<UTerrainGridSubsystem>();if(LegacyCamera)LegacyCamera->InitializeCameraForGrid(LegacyTerrain->GridWidth,LegacyTerrain->GridHeight,LegacyTerrain->CellSize);
   TStrongObjectPtr<UAftershockSaveGame> Legacy(Prepared?LegacySaves->CaptureWorld(LW,Error):nullptr);
   Check(TEXT("legacy_migration_fixture_captures_actual_authored_v1_cells"),Legacy.IsValid()&&Legacy->Terrain.ContinuationGeneratorVersion==1&&!LegacySession->HasConfiguredSession());
   if(Legacy.IsValid())
   {
    const float LegacyZoom=Legacy->Camera.Zoom;Legacy->Camera.Zoom=1.40162e9f;
    Check(TEXT("legacy_v1_camera_still_rejects_zoom_above_one_billion"),!LegacySaves->ValidateSnapshot(*Legacy,Error));Legacy->Camera.Zoom=LegacyZoom;
    const TArray<FGridCell> OriginalCells=Legacy->Terrain.Cells;const int32 OriginalResidents=Legacy->CityLife.Residents.Num();
    // Explicit old-wire fixture: V1 had none of these V5 physical fields.
    Legacy->Physics={};
    for(auto& Cell:Legacy->Terrain.Cells)
    {Cell.SnowWaterEquivalentMm=0.f;Cell.RegolithDepthM=0.f;Cell.SoilPoreWaterDepthM=0.f;Cell.GroundErodedDepthM=0.f;Cell.GroundDepositedDepthM=0.f;Cell.MaterialScenarioVersion=0;}
    Legacy->SchemaVersion=1;Legacy->Settings.FocusID=TEXT("will-migrate");Legacy->Terrain.TerrainStorageVersion=1;Legacy->Terrain.TerrainRevision=0;Legacy->Terrain.Chunks.Reset();Legacy->Terrain.bUsesRegionalLayout=false;Legacy->Terrain.bInitialMaskUnlocked=false;
    for(auto& Cell:Legacy->Terrain.Cells){Cell.bAuthoredContinuation=false;Cell.bHasRealElevationSample=false;}
    const bool Migrated=LegacySaves->ValidateSnapshot(*Legacy,Error);bool CoordinatesAndHeights=Legacy->Terrain.Cells.Num()==OriginalCells.Num();
    if(CoordinatesAndHeights)for(int32 I=0;I<OriginalCells.Num();++I)CoordinatesAndHeights&=Legacy->Terrain.Cells[I].Coords==OriginalCells[I].Coords&&Legacy->Terrain.Cells[I].Elevation==OriginalCells[I].Elevation;
    Check(TEXT("version_one_migrates_default_focus"),Migrated&&Legacy->SchemaVersion==UAftershockSaveGame::CurrentVersion&&Legacy->Settings.FocusID.IsNone()&&Legacy->Terrain.ContinuationGeneratorVersion==1&&Legacy->Terrain.TerrainStorageVersion==2&&!Legacy->Terrain.Chunks.IsEmpty()&&CoordinatesAndHeights&&Legacy->CityLife.Residents.Num()==OriginalResidents);
   }
   LegacyGI->Shutdown();LW->DestroyWorld(false);GEngine->DestroyWorldContext(LW);LW->RemoveFromRoot();
  }
 }
 {
  TStrongObjectPtr<UGameInstance> EtnaGI(NewObject<UGameInstance>(GEngine));EtnaGI->InitializeStandalone(TEXT("AftershockEtnaSourceStartupFixture"));UWorld* EW=EtnaGI->GetWorld();
  Check(TEXT("separate_real_etna_startup_world_created"),EW!=nullptr);
  if(EW)
  {
   auto* EtnaSession=EtnaGI->GetSubsystem<UAftershockOpeningSessionSubsystem>();auto* EtnaTerrain=EW->GetSubsystem<UTerrainGridSubsystem>();auto* EtnaScenario=EW->GetSubsystem<UAftershockScenarioSubsystem>();
   FAftershockSessionSettings EtnaSettings;EtnaSettings.CampaignStage=5;EtnaSettings.RegionID=TEXT("etna");
   const bool EtnaConfigured=EtnaSession->ConfigureNewSession(EtnaSettings,Error);const bool EtnaPrepared=EtnaConfigured&&EtnaScenario->PrepareScenarioBeforeSettlement();
   const auto& RegionRules=GetAftershockRegionRules(5);FIntPoint ActualVent(MAX_int32,MAX_int32);
   const bool Mapped=EtnaTerrain->TryGeographicToCell(RegionRules.VolcanoLatitude,RegionRules.VolcanoLongitude,ActualVent);
   const FIntPoint VentChunk=UTerrainGridSubsystem::CellToChunk(ActualVent);const int32 InitialCount=EtnaTerrain->GridWidth*EtnaTerrain->GridHeight;bool OnlySourceChunk=true;int32 ExtraSourceCells=0;
   for(int32 I=InitialCount;I<EtnaTerrain->Cells.Num();++I)
   {
    const auto& Cell=EtnaTerrain->Cells[I];++ExtraSourceCells;
    OnlySourceChunk&=UTerrainGridSubsystem::CellToChunk(Cell.Coords)==VentChunk&&Cell.bHasRealElevationSample&&!Cell.bAuthoredContinuation&&FMath::IsNearlyEqual(Cell.Elevation,EtnaTerrain->GridOrigin.Z+Cell.RealElevationMetres*100.f,1.f);
   }
   Check(TEXT("real_etna_prepare_loads_only_mapped_625_cell_vent_chunk_before_population_seed"),EtnaPrepared&&EtnaTerrain->IsUsingRealGeography()&&Mapped&&EtnaTerrain->VolcanicVentCoord==ActualVent&&ExtraSourceCells==625&&OnlySourceChunk&&EW->GetSubsystem<UBuildingManagerSubsystem>()->GetActiveBuildingsCount()==0&&!EW->GetSubsystem<UCityLifeSubsystem>()->IsCityInitialized()&&EW->GetSubsystem<UCrowdRenderSubsystem>()->GetCurrentCrowdCount()==0);
   Check(TEXT("real_etna_mapped_vent_is_resident_and_volcano_trigger_gate_is_usable"),EtnaPrepared&&EtnaTerrain->IsPlayableCoord(ActualVent)&&EtnaTerrain->GetCell(ActualVent).bVolcanicVent&&EW->GetSubsystem<UDisasterSimulationSubsystem>()->CanTriggerDisaster(EDisasterType::Volcano,Error));
   EtnaGI->Shutdown();EW->DestroyWorld(false);GEngine->DestroyWorldContext(EW);EW->RemoveFromRoot();
  }
 }
 Settings.Mode=EAftershockPlayMode::Sandbox;Settings.bUnlimitedBudget=true;Session->ConfigureNewSession(Settings,Error);E->TreasuryTL=0;Check(TEXT("sandbox_unlimited_investment_keeps_real_balance"),E->CanAfford(5000)&&E->TrySpendMoney(5000)&&E->TreasuryTL==0);
 Settings.Mode=EAftershockPlayMode::GeographicSimulation;Session->ConfigureNewSession(Settings,Error);Check(TEXT("unlimited_flag_has_no_effect_outside_sandbox"),!E->CanAfford(5000)&&!E->TrySpendMoney(5000));
 Settings={};Session->ConfigureNewSession(Settings,Error);Check(TEXT("campaign_cannot_skip_unfinished_stage"),!Session->AdvanceCampaignStage(1,Error));Session->CompleteCampaignStage(0);Check(TEXT("campaign_advance_retains_session_progress"),Session->AdvanceCampaignStage(1,Error)&&Session->GetSettings().RegionID==FName(TEXT("dereyolu"))&&Session->GetCampaignProgress().CompletedStages.Contains(0));
 GI->Shutdown();W->DestroyWorld(false);GEngine->DestroyWorldContext(W);W->RemoveFromRoot();return Finish();
}
#else
UAftershockSessionSaveAuditCommandlet::UAftershockSessionSaveAuditCommandlet(){}
int32 UAftershockSessionSaveAuditCommandlet::Main(const FString&){return 1;}
#endif
