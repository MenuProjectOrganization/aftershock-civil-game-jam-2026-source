#include "AftershockNarrativeRuntimeAudit.h"
#if !UE_BUILD_SHIPPING
#include "Aftershock/Game/AftershockGameMode.h"
#include "Aftershock/Game/AftershockPlayerController.h"
#include "Aftershock/Public/AftershockMainHUDWidget.h"
#include "Aftershock/Public/AftershockStartupPreparationSubsystem.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/GameInstance.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/UI/WaterGlassEpilogueWidget.h"
#include "Aftershock/UI/AftershockNewGameTutorialWidget.h"
#include "Aftershock/UI/CasualtyLedgerPanel.h"
#include "Aftershock/Narrative/CasualtyLedgerGenerator.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "UnrealClient.h"

namespace
{
	struct FAftershockNarrativeRuntimeState
	{
		TWeakObjectPtr<UWorld> World;
		double StartedReal=0.;FDateTime StartedUtc;
		FString Directory;
		TArray<FString> Results;TArray<FString> Screenshots;
		int32 Passed=0,Failed=0;
		bool bStarted=false,bIntroChecked=false,bStepCaptured=false,bPlayingChecked=false,bDismissed=false,bEndingChecked=false;
		bool bStepProcessed=false,bSkipRequested=false,bSkipMode=false;
        int32 StepRequested=INDEX_NONE,StepProcessed=INDEX_NONE;
        double FlowStartedReal=0.,PlayingCheckedReal=0.;
        TStrongObjectPtr<UAftershockNewGameTutorialWidget> Tutorial;
        int64 TutorialTreasury=0;int32 TutorialTurn=0,TutorialLiving=0;
		FString PendingScreenshotName;
		FDelegateHandle ScreenshotProcessedHandle;
		bool bLedgerOpened=false,bLedgerCaptured=false,bDeathFixtureApplied=false,bDoubleSettlementUnchanged=false;
		bool bLedgerIdentityBound=false,bLedgerStoryBound=false,bLedgerSnapshotBound=false;
		int32 FixtureResidentID=INDEX_NONE,DeathsBefore=0,DeathsAfter=0;
		int64 TreasuryBefore=0,TreasuryAfter=0,PendingAidBefore=0,PendingAidAfter=0;
		FGuid FixtureEventID;
		FCitizenMicroStory FixtureStory;
		void Check(const TCHAR* Name,bool Value)
		{
			Value?++Passed:++Failed;Results.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"),Name,Value?TEXT("true"):TEXT("false")));
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_NARRATIVE_RUNTIME_CHECK name=%s passed=%d"),Name,Value);
		}
		bool Capture(const TCHAR* Name)
		{
			Check(TEXT("ui_screenshot_queue_available"),!FScreenshotRequest::IsScreenshotRequested());
			if(FScreenshotRequest::IsScreenshotRequested())return false;
			PendingScreenshotName=Name;Screenshots.Add(Name);FScreenshotRequest::RequestScreenshot(Directory/Name,true,false);return true;
		}
		void Finish()
		{
			FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ScreenshotProcessedHandle);
            Check(TEXT("tutorial_second_step_requested_from_actual_production_state"),bStepCaptured&&StepRequested>=1&&StepRequested<UAftershockNewGameTutorialWidget::TutorialStepCount);
            Check(TEXT("tutorial_second_step_processed_while_actual_widget_visible"),bStepProcessed&&StepProcessed>=StepRequested&&StepProcessed<UAftershockNewGameTutorialWidget::TutorialStepCount);
            Check(TEXT("tutorial_uses_requested_real_navigation_policy"),Tutorial.IsValid()&&Tutorial->HasCompletedTutorial()&&bSkipRequested==bSkipMode);
            Check(TEXT("tutorial_completes_within_real_30_second_policy"),Tutorial.IsValid()&&Tutorial->HasCompletedTutorial()&&Tutorial->GetElapsedRealSeconds()<=30.15);

			for(const FString& Image:Screenshots)Check(TEXT("requested_ui_png_exists_and_is_fresh"),IFileManager::Get().FileSize(*(Directory/Image))>1024&&IFileManager::Get().GetTimeStamp(*(Directory/Image))>=StartedUtc);
			Check(TEXT("four_actual_tutorial_and_ending_ui_frames_requested"),Screenshots.Contains(TEXT("narrative-tutorial.png"))&&Screenshots.Contains(TEXT("narrative-tutorial-step.png"))&&Screenshots.Contains(TEXT("narrative-playing.png"))&&Screenshots.Contains(TEXT("narrative-epilogue.png")));
			Check(TEXT("fifth_actual_named_casualty_ledger_frame_requested"),bLedgerCaptured&&Screenshots.Num()==5&&Screenshots.Contains(TEXT("narrative-ledger.png")));
			const FString Json=FString::Printf(TEXT("{\"schema_version\":4,\"scope\":\"Explicit real RHI world fixture, production new-game tutorial, real-clock autoplay or explicit skip, visible paused city and consumed travel marker, one canonical resident's explicit induced casualty BEFORE satisfaction-zero dismissal, actual epilogue and named ledger UMG bindings with five fresh UI PNGs. No hazard or evacuation outcome, OS input, complete-campaign playthrough, package, Mac or performance claim. Tutorial keys use current preferences; no legacy intro or camera-reveal claim. This explicit fixture is not an OS-input or cold-travel test.\",\"game_over_fixture_only\":true,\"explicit_induced_resident_death_fixture\":true,\"resident_death_fixture_applied\":%s,\"ledger_resident_id\":%d,\"ledger_event_id\":\"%s\",\"ledger_deaths_before\":%d,\"ledger_deaths_after\":%d,\"ledger_treasury_before\":%lld,\"ledger_treasury_after\":%lld,\"ledger_pending_aid_before\":%lld,\"ledger_pending_aid_after\":%lld,\"ledger_double_settlement_unchanged\":%s,\"ledger_widget_identity_bound\":%s,\"ledger_widget_story_bound\":%s,\"ledger_widget_snapshot_bound\":%s,\"ledger_frame_captured\":%s,\"tutorial_step_requested\":%d,\"tutorial_step_processed\":%d,\"tutorial_step_frame_processed\":%s,\"tutorial_skip_mode\":%s,\"tutorial_skip_requested\":%s,\"tutorial_completed_elapsed_real_seconds\":%.6f,\"tutorial_maximum_real_policy_seconds\":30,\"completion_observation_tolerance_seconds\":0.15,\"passed\":%d,\"failed\":%d,\"valid\":%s,\"checks\":[%s]}"),bDeathFixtureApplied?TEXT("true"):TEXT("false"),FixtureResidentID,*FixtureEventID.ToString(),DeathsBefore,DeathsAfter,TreasuryBefore,TreasuryAfter,PendingAidBefore,PendingAidAfter,bDoubleSettlementUnchanged?TEXT("true"):TEXT("false"),bLedgerIdentityBound?TEXT("true"):TEXT("false"),bLedgerStoryBound?TEXT("true"):TEXT("false"),bLedgerSnapshotBound?TEXT("true"):TEXT("false"),bLedgerCaptured?TEXT("true"):TEXT("false"),StepRequested,StepProcessed,bStepProcessed?TEXT("true"):TEXT("false"),bSkipMode?TEXT("true"):TEXT("false"),bSkipRequested?TEXT("true"):TEXT("false"),Tutorial.IsValid()?Tutorial->GetElapsedRealSeconds():-1.,Passed,Failed,Failed==0?TEXT("true"):TEXT("false"),*FString::Join(Results,TEXT(",")));
			const bool Written=FFileHelper::SaveStringToFile(Json,*(Directory/TEXT("narrative_runtime_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_NARRATIVE_RUNTIME_RESULT valid=%d passed=%d failed=%d screenshots=%d realClock=1 canonicalDismissal=1 osInputClaim=0"),Written&&Failed==0,Passed,Failed,Screenshots.Num());
		}
	};
}
void BeginAftershockNarrativeRuntimeAudit(UWorld& World)
{
	if(!FParse::Param(FCommandLine::Get(),TEXT("AftershockNarrativeAudit")))return;
	const auto State=MakeShared<FAftershockNarrativeRuntimeState>();State->World=&World;State->StartedReal=FPlatformTime::Seconds();State->StartedUtc=FDateTime::UtcNow();State->Directory=FPaths::ProjectSavedDir()/TEXT("PresentationValidation/Narrative");IFileManager::Get().MakeDirectory(*State->Directory,true);
	State->ScreenshotProcessedHandle=FScreenshotRequest::OnScreenshotRequestProcessed().AddLambda([State]()
	{
		if(State->PendingScreenshotName==TEXT("narrative-tutorial-step.png"))
		{
			const auto* W=State->World.Get();const auto* PC=W?Cast<AAftershockPlayerController>(W->GetFirstPlayerController()):nullptr;
			const auto* Tutorial=PC?PC->GetNewGameTutorial():nullptr;
            State->StepProcessed=Tutorial?Tutorial->GetCurrentStep():INDEX_NONE;
            State->bStepProcessed=PC&&PC->IsNewGameTutorialVisible()&&Tutorial;
            UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_TUTORIAL_CHECKPOINT requestedStep=%d processedStep=%d actualVisibleTutorial=%d clockOverride=0"),State->StepRequested,State->StepProcessed,State->bStepProcessed);
		}
		State->PendingScreenshotName.Reset();
	});
	State->bSkipMode=FParse::Param(FCommandLine::Get(),TEXT("AftershockTutorialSkipAudit"));
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_NARRATIVE_RUNTIME_BEGIN explicitFixture=1 tutorialSkip=%d tutorialClockUnmodified=1 uiScreenshots=1"),State->bSkipMode);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([State](float)
	{
		const double Elapsed=FPlatformTime::Seconds()-State->StartedReal;UWorld* ActiveWorld=State->World.Get();
		if(!ActiveWorld){State->Check(TEXT("fixture_world_survives_until_delivery"),false);State->Finish();FPlatformMisc::RequestExit(false);return false;}
		auto* PC=Cast<AAftershockPlayerController>(ActiveWorld->GetFirstPlayerController());auto* Mode=ActiveWorld->GetAuthGameMode<AAftershockGameMode>();auto* HUD=PC?PC->GetMainHUD():nullptr;
        const auto* Startup=ActiveWorld->GetGameInstance()?ActiveWorld->GetGameInstance()->GetSubsystem<UAftershockStartupPreparationSubsystem>():nullptr;
        if(!State->bStarted&&Elapsed>=.3&&PC&&Mode&&(!Startup||!Startup->IsGameplayBlocked(ActiveWorld)))
        {
            State->bStarted=true;State->FlowStartedReal=FPlatformTime::Seconds();State->Check(TEXT("production_controller_mode_and_hud_exist"),HUD!=nullptr);
            // Explicit fixture supplies the same transient marker as New Game.
            // No session/world reset and no forced tutorial clock or step.
            ActiveWorld->URL.AddOption(TEXT("AftershockIntro=1"));PC->PresentNewGameTutorial();State->Tutorial.Reset(PC->GetNewGameTutorial());
            if(auto* Economy=ActiveWorld->GetSubsystem<UEconomyAndStateSubsystem>()){State->TutorialTreasury=Economy->TreasuryTL;State->TutorialTurn=Economy->CurrentTurn;}
            if(auto* Life=ActiveWorld->GetSubsystem<UCityLifeSubsystem>())State->TutorialLiving=Life->GetState().LivingPopulation;
        }
        const double FlowElapsed=State->bStarted?FPlatformTime::Seconds()-State->FlowStartedReal:0.;
        if(!State->bIntroChecked&&State->bStarted&&FlowElapsed>=2.6)
        {
            State->bIntroChecked=true;State->Check(TEXT("tutorial_is_actual_visible_widget_and_canonical_intro_flow"),PC&&Mode&&PC->IsNewGameTutorialVisible()&&Mode->GetGameFlowState()==EGameFlowState::Intro);
            State->Check(TEXT("tutorial_pauses_city_and_locks_visible_dashboard"),ActiveWorld->IsPaused()&&HUD&&!HUD->CanIssueCityOrder()&&HUD->IsVisible()&&HUD->GetVisibility()==ESlateVisibility::HitTestInvisible);
            State->Check(TEXT("tutorial_consumes_newgame_marker_without_config_writes"),!ActiveWorld->URL.HasOption(TEXT("AftershockIntro")));
            if(auto* CameraPawn=PC?Cast<AAftershockCameraPawn>(PC->GetPawn()):nullptr){FAftershockCameraSaveState Before,After;CameraPawn->CaptureSaveState(Before);CameraPawn->ZoomCamera(1.f);CameraPawn->MoveCamera({1,1});CameraPawn->RotateCamera(1.f);CameraPawn->CaptureSaveState(After);State->Check(TEXT("tutorial_blocks_public_pan_zoom_and_rotation"),Before.LocalFocus.Equals(After.LocalFocus)&&FMath::IsNearlyEqual(Before.Zoom,After.Zoom)&&Before.Orbit.Equals(After.Orbit));}
            else State->Check(TEXT("tutorial_blocks_public_pan_zoom_and_rotation"),false);
            State->Capture(TEXT("narrative-tutorial.png"));
        }
        if(!State->bStepCaptured&&PC&&PC->IsNewGameTutorialVisible()&&!FScreenshotRequest::IsScreenshotRequested())
        {
            const auto* Tutorial=PC->GetNewGameTutorial();
            if(Tutorial&&Tutorial->GetCurrentStep()>=1){State->StepRequested=Tutorial->GetCurrentStep();State->bStepCaptured=State->Capture(TEXT("narrative-tutorial-step.png"));}
        }
        if(State->bSkipMode&&!State->bSkipRequested&&FlowElapsed>=6.&&State->bStepProcessed&&PC&&PC->GetNewGameTutorial())
        {State->bSkipRequested=true;PC->GetNewGameTutorial()->SkipTutorial();}
        if(!State->bPlayingChecked&&State->bStarted&&FlowElapsed>=(State->bSkipMode?8.:31.))
        {
            State->bPlayingChecked=true;State->PlayingCheckedReal=FPlatformTime::Seconds();
            State->Check(TEXT("tutorial_returns_to_playing_using_real_autoplay_or_skip"),PC&&Mode&&Mode->GetGameFlowState()==EGameFlowState::Playing&&!PC->IsNewGameTutorialVisible());
            State->Check(TEXT("normal_hud_and_unpaused_city_return_after_tutorial"),HUD&&HUD->IsVisible()&&HUD->CanIssueCityOrder()&&!ActiveWorld->IsPaused());
            const auto* Economy=ActiveWorld->GetSubsystem<UEconomyAndStateSubsystem>();const auto* Life=ActiveWorld->GetSubsystem<UCityLifeSubsystem>();
            State->Check(TEXT("paused_tutorial_keeps_canonical_turn_treasury_and_living_count"),Economy&&Life&&Economy->TreasuryTL==State->TutorialTreasury&&Economy->CurrentTurn==State->TutorialTurn&&Life->GetState().LivingPopulation==State->TutorialLiving);
            if(PC){PC->PresentNewGameTutorial();State->Check(TEXT("consumed_newgame_marker_cannot_replay_tutorial"),!PC->IsNewGameTutorialVisible());PC->ShowOpeningMenu();PC->ContinueFromOpening();State->Check(TEXT("actual_live_continue_returns_without_tutorial_or_city_reset"),!PC->IsOpeningMenuVisible()&&!PC->IsNewGameTutorialVisible()&&!ActiveWorld->IsPaused()&&Economy&&Economy->CurrentTurn==State->TutorialTurn);}
            State->Capture(TEXT("narrative-playing.png"));
        }
        const double AfterPlaying=State->bPlayingChecked?FPlatformTime::Seconds()-State->PlayingCheckedReal:-1.;
		if(!State->bDismissed&&AfterPlaying>=1.5)
		{
			State->bDismissed=true;auto* Economy=ActiveWorld->GetSubsystem<UEconomyAndStateSubsystem>();State->Check(TEXT("canonical_economy_available_for_explicit_dismissal_fixture"),Economy!=nullptr);
			auto* Life=ActiveWorld->GetSubsystem<UCityLifeSubsystem>();auto* Buildings=ActiveWorld->GetSubsystem<UBuildingManagerSubsystem>();auto* Ledger=ActiveWorld->GetSubsystem<UCasualtyLedgerGenerator>();
			if(Economy&&Life&&Buildings&&Ledger&&!Economy->bIsGameOver)
			{
				const auto Before=Life->GetResidents();TArray<FBuildingInstance> BeforeBuildings;Buildings->GetAllBuildings(BeforeBuildings);int32 HomeID=INDEX_NONE;
				for(const auto& Person:Before)if(Person.bAlive){const auto* Home=BeforeBuildings.FindByPredicate([&](const auto& B){return B.InstanceID==Person.HomeBuildingID&&B.BuildingData&&B.DamageState<EDamageState::Collapsed;});if(Home){HomeID=Home->InstanceID;break;}}
				State->DeathsBefore=Life->GetState().TotalDeaths;State->TreasuryBefore=Economy->TreasuryTL;State->PendingAidBefore=Economy->PendingStateAidTL;
				const TArray<int32> DeathIDs=HomeID!=INDEX_NONE?Life->ApplyCasualties(HomeID,1,false):TArray<int32>();
				FDisasterReport Report;Report.DisasterEventID=FGuid::NewGuid();Report.DisasterType=EDisasterType::Earthquake;Report.BaseSeverity=0.f;Report.ActualDeathResidentIDs=DeathIDs;Report.TotalDeaths=DeathIDs.Num();Report.TotalDeathCostTL=static_cast<int64>(DeathIDs.Num())*500;
				const bool Recorded=DeathIDs.Num()==1&&Ledger->RecordDisaster(Report.DisasterEventID,Report.DisasterType,Economy->CurrentTurn,false,Before,BeforeBuildings,DeathIDs,{},false,Report.BaseSeverity);
				if(Recorded)
				{
					Economy->HandleDisasterCompleted(Report);Life->RefreshCity();State->FixtureResidentID=DeathIDs[0];State->FixtureEventID=Report.DisasterEventID;
					State->DeathsAfter=Life->GetState().TotalDeaths;State->TreasuryAfter=Economy->TreasuryTL;State->PendingAidAfter=Economy->PendingStateAidTL;
					const auto* Row=Ledger->GetEntries().FindByPredicate([&](const auto& Entry){return Entry.ResidentID==State->FixtureResidentID&&Entry.DisasterEventID==State->FixtureEventID;});FCityResidentRecord Actual;
					TArray<FGuid> Applied;Economy->CaptureAppliedDisasterEvents(Applied);
					State->bDeathFixtureApplied=Row&&Life->GetResident(State->FixtureResidentID,Actual)&&!Actual.bAlive&&State->DeathsAfter==State->DeathsBefore+1&&State->TreasuryAfter==State->TreasuryBefore-500&&State->PendingAidAfter==State->PendingAidBefore&&Applied.Contains(State->FixtureEventID);
					if(Row)State->FixtureStory=*Row;
					Economy->HandleDisasterCompleted(Report);State->bDoubleSettlementUnchanged=Economy->TreasuryTL==State->TreasuryAfter&&Economy->PendingStateAidTL==State->PendingAidAfter&&Life->GetState().TotalDeaths==State->DeathsAfter;
				}
				UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_NARRATIVE_INDUCED_RESIDENT_CASUALTY fixtureOnly=1 resident=%d event=%s beforeDeaths=%d afterDeaths=%d treasuryBefore=%lld treasuryAfter=%lld aidBefore=%lld aidAfter=%lld actualIDs=1 forcedBeforeGameOver=1 hazardFormulaCalled=0 evacuationClaim=0"),State->FixtureResidentID,*State->FixtureEventID.ToString(),State->DeathsBefore,State->DeathsAfter,State->TreasuryBefore,State->TreasuryAfter,State->PendingAidBefore,State->PendingAidAfter);
			}
			State->Check(TEXT("explicit_one_actual_resident_casualty_settles_500_before_game_over"),State->bDeathFixtureApplied);
			State->Check(TEXT("induced_actual_casualty_cannot_settle_twice"),State->bDoubleSettlementUnchanged);
			if(Economy){Economy->PublicSatisfaction=0.f;Economy->TriggerGameOver(EGameOverReason::DismissedByPublic);State->Check(TEXT("real_economy_commits_satisfaction_zero_game_over"),Economy->bIsGameOver&&Economy->PublicSatisfaction==0.f);}
		}
		if(!State->bEndingChecked&&AfterPlaying>=7.5)
		{
			State->bEndingChecked=true;State->Check(TEXT("canonical_game_over_opens_actual_epilogue"),PC&&Mode&&Mode->GetGameFlowState()==EGameFlowState::Epilogue&&PC->IsWaterGlassEpilogueVisible());
			State->Check(TEXT("epilogue_hides_legacy_end_headers_and_city_hud"),HUD&&!HUD->IsVisible()&&!HUD->IsLegacyEndingPanelVisible()&&!HUD->CanIssueCityOrder());
			auto* Ledger=ActiveWorld->GetSubsystem<UCasualtyLedgerGenerator>();auto* Ending=PC?PC->GetWaterGlassEpilogue():nullptr;
			if(Ledger&&Ending){const auto Totals=Ledger->GetCampaignTotals();State->Check(TEXT("epilogue_counts_match_unique_canonical_ledger"),Ending->GetDisplayedDeaths()==Totals.Deaths&&Ending->GetDisplayedSurvivors()==Totals.UniqueExposedSurvivors);}
			else State->Check(TEXT("epilogue_counts_match_unique_canonical_ledger"),false);
			State->Capture(TEXT("narrative-epilogue.png"));
		}
		if(!State->bLedgerOpened&&AfterPlaying>=9.&&State->bEndingChecked&&!FScreenshotRequest::IsScreenshotRequested())
		{
			State->bLedgerOpened=true;auto* Ending=PC?PC->GetWaterGlassEpilogue():nullptr;
			State->Check(TEXT("actual_epilogue_button_handler_opens_named_ledger_overlay"),Ending&&PC->IsWaterGlassEpilogueVisible()&&Ending->OpenCasualtyLedger());
		}
		if(!State->bLedgerCaptured&&AfterPlaying>=9.7&&State->bLedgerOpened&&!FScreenshotRequest::IsScreenshotRequested())
		{
			auto* Ending=PC?PC->GetWaterGlassEpilogue():nullptr;auto* Panel=Ending?Ending->GetCasualtyLedgerPanel():nullptr;
			bool NameAge=false,Job=false,Story=false,Snapshot=false;
			if(Ending&&Ending->IsCasualtyLedgerVisible()&&Panel&&Panel->WidgetTree&&State->bDeathFixtureApplied)
			{
				TArray<UWidget*> Widgets;Panel->WidgetTree->GetAllWidgets(Widgets);const auto& Entry=State->FixtureStory;
				for(const UWidget* Widget:Widgets)if(Widget)
				{
					if(const auto* Text=Cast<UTextBlock>(Widget))
					{
						const FString Copy=Text->GetText().ToString();NameAge|=Copy.Contains(Entry.FullName.ToString())&&Copy.Contains(FText::AsNumber(Entry.Age).ToString())&&Copy.Contains(TEXT("yaşında"));Job|=Text->GetText().EqualTo(Entry.Occupation);Story|=Text->GetText().EqualTo(Entry.Narrative);
					}
					const FString Tooltip=Widget->GetToolTipText().ToString();Snapshot|=Entry.bHasBuildingSnapshot&&Tooltip.Contains(FString::Printf(TEXT("Vatandaş #%d |"),Entry.ResidentID))&&Tooltip.Contains(FString::Printf(TEXT("Yapı #%d | Parsel %d,%d"),Entry.BuildingID,Entry.BuildingCoord.X,Entry.BuildingCoord.Y));
				}
			}
			State->bLedgerIdentityBound=NameAge&&Job;State->bLedgerStoryBound=Story;State->bLedgerSnapshotBound=Snapshot;
			State->Check(TEXT("rendered_ledger_name_age_and_occupation_match_actual_resident_row"),State->bLedgerIdentityBound);
			State->Check(TEXT("rendered_ledger_story_and_snapshot_tooltip_match_actual_event"),Story&&Snapshot);
			State->Check(TEXT("named_ledger_is_laid_out_on_actual_visible_epilogue"),Ending&&Ending->IsCasualtyLedgerVisible()&&Panel&&Panel->GetCachedGeometry().GetLocalSize().X>0&&Panel->GetCachedGeometry().GetLocalSize().Y>0);
			State->bLedgerCaptured=State->Capture(TEXT("narrative-ledger.png"));
		}
		if(AfterPlaying>=11.5||Elapsed>=150.){State->Check(TEXT("tutorial_and_ending_fixture_complete_before_deadline"),State->bPlayingChecked&&State->bEndingChecked&&State->bLedgerCaptured);State->Finish();FPlatformMisc::RequestExit(false);return false;}return true;
	}),.05f);
}
#else
void BeginAftershockNarrativeRuntimeAudit(UWorld&){}
#endif
