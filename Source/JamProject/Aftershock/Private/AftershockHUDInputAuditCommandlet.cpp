#include "AftershockHUDInputAuditCommandlet.h"

#if WITH_EDITOR
#include "Aftershock/Public/AftershockMainHUDWidget.h"
#include "Aftershock/UI/AftershockNewGameTutorialWidget.h"
#include "Aftershock/Public/AftershockDisasterDirectorSubsystem.h"
#include "Aftershock/Public/AftershockBuildingCatalog.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Buildings/BuildingActor.h"
#include "Aftershock/World/GridManager.h"
#include "Aftershock/Game/AftershockPlayerController.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HorizontalBox.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	struct FScopedHUDAuditWorld
	{
		UWorld* World = nullptr;
		~FScopedHUDAuditWorld()
		{
			if (!World) return;
			World->DestroyWorld(false);
			if (GEngine) GEngine->DestroyWorldContext(World);
			World->RemoveFromRoot();
		}
	};
	FString EscapeHUDJson(FString Value)
	{
		Value.ReplaceInline(TEXT("\\"), TEXT("\\\\")); Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Value.ReplaceInline(TEXT("\n"), TEXT("\\n")); Value.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		return Value;
	}
}

UAftershockHUDInputAuditCommandlet::UAftershockHUDInputAuditCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UAftershockHUDInputAuditCommandlet::Main(const FString& Params)
{
	int32 Passed = 0, Failed = 0;
	TArray<FString> Results;
	const auto Check = [&](const TCHAR* Name, bool Success, FString Detail)
	{
		Success ? ++Passed : ++Failed;
		Results.Add(FString::Printf(TEXT("    {\"name\":\"%s\",\"passed\":%s,\"detail\":\"%s\"}"), Name, Success ? TEXT("true") : TEXT("false"), *EscapeHUDJson(Detail)));
		if (Success)
		{
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_HUD_INPUT_AUDIT_PASS %s: %s"), Name, *Detail);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_HUD_INPUT_AUDIT_FAIL %s: %s"), Name, *Detail);
		}
	};
	const auto Finish = [&]()
	{
		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PresentationValidation"));
		IFileManager::Get().MakeDirectory(*Directory, true);
		const FString Path = FPaths::Combine(Directory, TEXT("hud_input_audit.json"));
		const FString Json = FString::Printf(TEXT("{\n  \"schema_version\":1,\n  \"utc\":\"%s\",\n  \"scope\":\"Native UMG widget tree, real OnClicked delegates, tax transactions, catalog reachability, modal locks and preparation timer in isolated game world. Does not prove OS pointer hit testing, rendered layout, camera placement, quiz rewards, GPU or packaged execution.\",\n  \"passed\":%d,\n  \"failed\":%d,\n  \"checks\":[\n%s\n  ]\n}\n"), *FDateTime::UtcNow().ToIso8601(), Passed, Failed, *FString::Join(Results, TEXT(",\n")));
		const bool Saved = FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_HUD_INPUT_AUDIT_RESULT passed=%d failed=%d report=%s"), Passed, Failed, *Path);
		return Saved ? (Failed == 0 ? 0 : 1) : 2;
	};
	if (!GEngine) { Check(TEXT("engine_available"), false, TEXT("Editor engine missing")); return Finish(); }
    using FTutorial=UAftershockNewGameTutorialWidget;
    Check(TEXT("tutorial_six_steps_finish_with_fade_within_30_real_seconds"),FTutorial::TutorialStepCount==6&&FTutorial::TutorialStepCount*FTutorial::SecondsPerStep+FTutorial::ClosingSeconds<=FTutorial::MaximumRealDurationSeconds,TEXT("Actual production constants; native policy only, rendered timing requires RHI."));
    Check(TEXT("tutorial_deadline_has_exact_30_second_boundary"),!FTutorial::HasReachedDeadline(29.999)&&FTutorial::HasReachedDeadline(30.)&&FTutorial::HasReachedDeadline(31.),TEXT("Same predicate used by the unpaused core ticker, independent of world pause."));
    Check(TEXT("tutorial_negative_clock_cannot_finish_or_advance"),!FTutorial::HasReachedDeadline(-1.)&&FTutorial::ComputeElapsedStepCount(-1.)==0,TEXT("Invalid negative elapsed time cannot skip presentation."));
    Check(TEXT("tutorial_step_advance_uses_real_4_75_second_boundary"),FTutorial::ComputeElapsedStepCount(4.749)==0&&FTutorial::ComputeElapsedStepCount(4.75)==1,TEXT("Same catch-up function used in production ticker."));
    Check(TEXT("tutorial_late_frame_catches_up_three_steps"),FTutorial::ComputeElapsedStepCount(14.25)==3,TEXT("No world timer and no accumulated float delta."));
    Check(TEXT("tutorial_long_frame_is_bounded_without_integer_overflow"),FTutorial::ComputeElapsedStepCount(28.5)==6&&FTutorial::ComputeElapsedStepCount(1.e300)==6,TEXT("At most six steps; back navigation never resets the absolute 30-second start clock."));
	FScopedHUDAuditWorld Scoped;
	Scoped.World = UWorld::CreateWorld(EWorldType::Game, false, FName(TEXT("AftershockHUDInputAuditWorld")), GetTransientPackage(), true, ERHIFeatureLevel::SM5, nullptr, false);
	Check(TEXT("isolated_world_created"), Scoped.World != nullptr, TEXT("User gameplay world and map files are not modified."));
	if (!Scoped.World) return Finish();
	UWorld* World = Scoped.World;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	World->URL.AddOption(TEXT("AftershockScenario=0"));
	UAftershockScenarioSubsystem* Scenario = World->GetSubsystem<UAftershockScenarioSubsystem>();
	UBuildingManagerSubsystem* Buildings = World->GetSubsystem<UBuildingManagerSubsystem>();
	UCityLifeSubsystem* Life = World->GetSubsystem<UCityLifeSubsystem>();
	UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>();
	Check(TEXT("canonical_city_subsystems"), Scenario && Buildings && Life && Economy, TEXT("HUD transactions use the same world's canonical city systems."));
	if (!Scenario || !Buildings || !Life || !Economy) return Finish();
	Scenario->PrepareScenarioBeforeSettlement();
	Buildings->SeedInitialSettlement();
	Check(TEXT("real_startup_initializes_designed_roads_and_city_life"), Scenario->IsPrepared() && Life->IsCityInitialized() && Scenario->GetStarterRoadCellCount() > 0,
		FString::Printf(TEXT("scenario=%d initializedLife=%d starterRoadCells=%d"), Scenario->GetScenarioIndex(), Life->IsCityInitialized(), Scenario->GetStarterRoadCellCount()));
	Economy->TreasuryTL = 200000;
	TStrongObjectPtr<UAftershockMainHUDWidget> HUD(CreateWidget<UAftershockMainHUDWidget>(World));
	Check(TEXT("native_hud_created"), HUD.IsValid(), TEXT("Actual native production HUD class; no mirrored test implementation."));
	if (!HUD.IsValid()) return Finish();
	HUD->NativeConstruct();
	int32 ButtonCount = 0, UnboundButtons = 0;
	HUD->WidgetTree->ForEachWidget([&](UWidget* Widget)
	{
		if (const UButton* Button = Cast<UButton>(Widget)) { ++ButtonCount; if (!Button->OnClicked.IsBound()) ++UnboundButtons; }
	});
	Check(TEXT("all_reachable_buttons_have_click_delegates"), ButtonCount >= 45 && UnboundButtons == 0, FString::Printf(TEXT("buttons=%d unbound=%d"), ButtonCount, UnboundButtons));
	bool EveryPoolBound=HUD->CatalogItemButtonsPool.Num()==6,EmptyPoolSafe=true;int32 EmptyPoolCount=0;
	const int32 CategoryBeforeEmpty=HUD->CatalogCategory,PageBeforeEmpty=HUD->CatalogPage,ToolBeforeEmpty=HUD->ActiveMapTool;
	const int64 MoneyBeforeEmpty=Economy->TreasuryTL;const int32 BuildingsBeforeEmpty=Buildings->GetActiveBuildingsCount();
	for(UAftershockCatalogActionButton* Button:HUD->CatalogItemButtonsPool)
	{
		EveryPoolBound&=Button&&Button->OnClicked.IsBound();
		if(Button&&Button->GetVisibility()==ESlateVisibility::Collapsed)
		{
			++EmptyPoolCount;EmptyPoolSafe&=!Button->GetIsEnabled()&&Button->Action==INDEX_NONE&&Button->Value==INDEX_NONE;
			Button->OnClicked.Broadcast(); // Deliberate stale delegate dispatch.
		}
	}
	Check(TEXT("all_six_pooled_catalog_controls_have_lifetime_dispatch_bindings"),EveryPoolBound,TEXT("The empty slots also own real native delegates; the original unbound-control audit remains strict."));
	Check(TEXT("empty_pooled_catalog_slots_are_disabled_collapsed_and_have_no_stale_action"),EmptyPoolCount>0&&EmptyPoolSafe,TEXT("Unused first-page slots have no previous building/type action."));
	Check(TEXT("empty_catalog_delegate_dispatch_cannot_change_tools_category_or_city"),HUD->CatalogCategory==CategoryBeforeEmpty&&HUD->CatalogPage==PageBeforeEmpty&&HUD->ActiveMapTool==ToolBeforeEmpty&&Economy->TreasuryTL==MoneyBeforeEmpty&&Buildings->GetActiveBuildingsCount()==BuildingsBeforeEmpty,TEXT("Real broadcast on every unused pooled control is inert."));
	// Reconstruct the actual production layout on the same HUD object. The
	// lifetime of its six pooled controls outlasts an individual row subtree.
	const TArray<TObjectPtr<UAftershockCatalogActionButton>> OriginalCatalogPool = HUD->CatalogItemButtonsPool;
	const UHorizontalBox* RetiredCatalogRow = HUD->CatalogItemsRow;
	HUD->SetupCleanHUDLayout();
	bool SamePool = OriginalCatalogPool.Num() == 6 && HUD->CatalogItemButtonsPool.Num() == 6;
	bool ReattachedPool = HUD->CatalogItemsRow && HUD->CatalogItemsRow != RetiredCatalogRow && HUD->CatalogItemsRow->GetChildrenCount() == 6;
	for (int32 Slot = 0; Slot < OriginalCatalogPool.Num(); ++Slot)
	{
		const UAftershockCatalogActionButton* Button = OriginalCatalogPool[Slot];
		SamePool &= HUD->CatalogItemButtonsPool.IsValidIndex(Slot) && HUD->CatalogItemButtonsPool[Slot] == Button;
		ReattachedPool &= Button && Button->GetParent() == HUD->CatalogItemsRow;
	}
	Check(TEXT("catalog_layout_rebuild_reuses_exact_six_control_objects"), SamePool, TEXT("No new pooled item widgets are allocated when the production row is replaced."));
	Check(TEXT("catalog_layout_rebuild_attaches_pool_to_live_row"), ReattachedPool, TEXT("All six pooled controls belong to the new catalog row rather than the retired widget subtree."));
	Check(TEXT("first_normal_legacy_unset_frame_limit_defaults_to_sixty"),AAftershockPlayerController::ResolveStartupFrameRateLimit(0.f,false,0)==60.f,TEXT("Pure policy only; this commandlet never writes the player's graphics settings."));
	Check(TEXT("existing_positive_frame_limit_and_vsync_preferences_are_preserved"),AAftershockPlayerController::ResolveStartupFrameRateLimit(144.f,false,0)==144.f&&AAftershockPlayerController::ResolveStartupFrameRateLimit(0.f,true,0)==0.f,TEXT("Custom positive caps and a VSync-enabled legacy preference are not replaced by the default."));
	Check(TEXT("explicit_unlimited_frame_preference_is_not_defaulted_on_later_startup"),AAftershockPlayerController::ResolveStartupFrameRateLimit(0.f,false,1)==0.f,TEXT("The versioned explicit zero is unlimited on every later normal launch."));
	TSet<int32> FrameChoices;bool FrameButtonsBound=HUD->FramePacingButtons.Num()==4;
	for(const UAftershockCatalogActionButton* Button:HUD->FramePacingButtons){FrameButtonsBound&=Button&&Button->Action==5&&Button->OnClicked.IsBound();if(Button)FrameChoices.Add(Button->Value);}
	Check(TEXT("pause_menu_exposes_bound_sixty_ninety_one_twenty_and_unlimited_choices"),FrameButtonsBound&&FrameChoices.Num()==4&&FrameChoices.Contains(0)&&FrameChoices.Contains(60)&&FrameChoices.Contains(90)&&FrameChoices.Contains(120),TEXT("The real production pause tree contains all four frame-pacing choices."));
	const auto* UserSettings=UGameUserSettings::GetGameUserSettings();const float OriginalUserCap=UserSettings?UserSettings->GetFrameRateLimit():0.f;const bool OriginalUserVSync=UserSettings&&UserSettings->IsVSyncEnabled();const float OriginalEngineCap=GEngine->GetMaxFPS();
	for(UAftershockCatalogActionButton* Button:HUD->FramePacingButtons)if(Button)Button->OnClicked.Broadcast();
	Check(TEXT("headless_frame_choice_callbacks_never_mutate_user_settings_or_engine_cap"),(!UserSettings||(UserSettings->GetFrameRateLimit()==OriginalUserCap&&UserSettings->IsVSyncEnabled()==OriginalUserVSync))&&GEngine->GetMaxFPS()==OriginalEngineCap,TEXT("No owning normal player and commandlet context make all persistence/action paths reject; pure policy coverage does not force a real INI value."));
	TSet<int32> ReachableTypes;
	for (int32 Category = 0; Category < 5; ++Category)
	{
		HUD->HandleCatalogAction(0, Category);
		for (int32 Page = 0; Page < 4; ++Page)
		{
			for (const UAftershockCatalogActionButton* Button : HUD->CatalogItemButtons) ReachableTypes.Add(Button->Value);
			HUD->HandleCatalogAction(2, 1);
		}
	}
	Check(TEXT("all_24_catalog_definitions_reachable"), ReachableTypes.Num() == 24, FString::Printf(TEXT("uniqueTypes=%d across five categories and real page handlers"), ReachableTypes.Num()));
	HUD->HandleCatalogAction(0, 0); HUD->HandleCatalogAction(2, -100);
	Check(TEXT("catalog_page_lower_bound"), HUD->CatalogPage == 0, TEXT("Previous page never moves below page zero."));
	Check(TEXT("four_tax_buttons_bound"), HUD->TaxActionButtons.Num() == 4, TEXT("Residential minus/plus and business minus/plus remain referenced by the widget tree."));
	if (HUD->TaxActionButtons.Num() == 4)
	{
		Life->SetTaxRates(.10f, .12f);
		const int64 BudgetBeforeTax = Economy->TreasuryTL;
		HUD->TaxActionButtons[1]->OnClicked.Broadcast();
		Check(TEXT("residential_plus_real_click_changes_rate"), FMath::IsNearlyEqual(Life->GetState().ResidentialTaxRate, .12f), HUD->TextTaxRates->GetText().ToString());
		HUD->TaxActionButtons[0]->OnClicked.Broadcast();
		HUD->TaxActionButtons[3]->OnClicked.Broadcast();
		Check(TEXT("other_tax_buttons_dispatch_correct_actions"), FMath::IsNearlyEqual(Life->GetState().ResidentialTaxRate, .10f) && FMath::IsNearlyEqual(Life->GetState().BusinessTaxRate, .14f), HUD->TextTaxRates->GetText().ToString());
		HUD->TaxActionButtons[2]->OnClicked.Broadcast();
		Check(TEXT("tax_change_does_not_spend_or_award_cash"), Economy->TreasuryTL == BudgetBeforeTax, TEXT("Tax changes affect projected collections, not an immediate cash transfer."));
		Life->SetTaxRates(0.f, .30f); HUD->RefreshActionAvailability();
		Check(TEXT("tax_limits_disable_impossible_actions"), !HUD->TaxActionButtons[0]->GetIsEnabled() && HUD->TaxActionButtons[1]->GetIsEnabled() && HUD->TaxActionButtons[2]->GetIsEnabled() && !HUD->TaxActionButtons[3]->GetIsEnabled(), TEXT("Zero percent minus and thirty percent plus cannot silently no-op."));
		Life->SetTaxRates(.10f, .12f);
		HUD->bDisasterInFlight = true; HUD->RefreshActionAvailability();
		const bool Locked = !HUD->TaxActionButtons[1]->GetIsEnabled();
		HUD->TaxActionButtons[1]->OnClicked.Broadcast();
		Check(TEXT("active_disaster_locks_tax_controls"), Locked && FMath::IsNearlyEqual(Life->GetState().ResidentialTaxRate, .10f), TEXT("Visual state and actual transaction guard agree while the canonical disaster is in flight."));
		HUD->bDisasterInFlight = false;
	}
	const TArray<TPair<UButton*, int32>> Tools = {{HUD->BtnBuildRoad,7},{HUD->BtnRemoveRoad,8},{HUD->BtnInvestRiver,1},{HUD->BtnInvestDamping,2},{HUD->BtnInvestDrainage,3},{HUD->BtnRetrofit,4},{HUD->BtnRepair,5},{HUD->BtnClearRubble,6},{HUD->BtnInvestDam,9},{HUD->BtnGroundSurvey,10}};
	for (const auto& Tool : Tools)
	{
		Tool.Key->OnClicked.Broadcast();
		Check(*FString::Printf(TEXT("map_tool_click_%d"), Tool.Value), HUD->ActiveMapTool == Tool.Value, TEXT("Actual button selects its own tool without modifying the map or charging funds."));
		HUD->HandleEscapeAction();
		Check(*FString::Printf(TEXT("map_tool_escape_%d"), Tool.Value), HUD->ActiveMapTool == 0 && !HUD->bRoadStartSelected, TEXT("Escape clears pending tool and road transaction state."));
	}
	HUD->BtnPause->OnClicked.Broadcast();
	const int32 TurnBeforePause = Economy->CurrentTurn;
	HUD->BtnNextTurn->OnClicked.Broadcast();
	Check(TEXT("pause_modal_blocks_city_transactions"), HUD->bIsGamePaused && HUD->HasModalInputBlock() && Economy->CurrentTurn == TurnBeforePause, TEXT("Direct delegate dispatch also obeys the modal guard."));
	HUD->BtnResume->OnClicked.Broadcast();
	Check(TEXT("resume_clears_pause_modal"), !HUD->bIsGamePaused && !HUD->HasModalInputBlock(), TEXT("Resume restores city order eligibility."));
	HUD->SetOpeningMenuActive(true); HUD->HandleCatalogAction(3, 1);
	Check(TEXT("opening_menu_blocks_city_transactions"), HUD->HasModalInputBlock() && FMath::IsNearlyEqual(Life->GetState().ResidentialTaxRate, .10f), TEXT("Opening menu cannot leak a tax click into the live city."));
	HUD->SetOpeningMenuActive(false);
	HUD->BtnOpenQuiz->OnClicked.Broadcast();
	Check(TEXT("missing_quiz_service_does_not_leave_empty_modal"), !HUD->PanelQuizModal->IsVisible() && HUD->LastQuizTurn == INDEX_NONE && !HUD->bAwaitingQuizAnswer,
		TEXT("An isolated world without GameInstance cannot load a quiz; the production HUD closes the empty modal and allows retry."));
	HUD->BtnTriggerEarthquake->OnClicked.Broadcast();
	Check(TEXT("disaster_button_arms_preparation_timer"), HUD->bIsWarningActive && World->GetSubsystem<UAftershockDisasterDirectorSubsystem>()->GetState().Phase==EAftershockDisasterPhase::Warning, TEXT("The real disaster button arms the canonical director; UMG no longer owns a second clock."));
	const int32 PreparationTurn = Economy->CurrentTurn;
	HUD->AdvanceTurnFromUI();
	Check(TEXT("preparation_does_not_allow_turn_skip"), Economy->CurrentTurn == PreparationTurn, TEXT("A pending disaster cannot be skipped through Next Turn."));
	World->GetTimerManager().ClearTimer(HUD->WarningTimerHandle); HUD->bIsWarningActive = false;
	FAftershockDirectorState ClearDirector;FText ClearDirectorError;World->GetSubsystem<UAftershockDisasterDirectorSubsystem>()->RestoreSaveState(ClearDirector,ClearDirectorError);
	HUD->PanelDisasterWarning->SetVisibility(ESlateVisibility::Collapsed);
	UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>();
	URoadNetworkSubsystem* Roads = World->GetSubsystem<URoadNetworkSubsystem>();
	AGridManager* Grid = World->SpawnActor<AGridManager>();
	UBuildingData* House = Buildings->GetCatalogData(EBuildingType::House);
	Check(TEXT("legacy_placement_fixture_available"), Terrain && Roads && Grid && House, TEXT("Real Blueprint compatibility entry point shares the same world, road owner and catalog."));
	if (Terrain && Roads && Grid && House)
	{
		// This block isolates legacy handle/payment/rotation behavior. A free
		// parcel alone no longer authorizes construction: the real access owner
		// must accept its frontage and grade. Choose an already served lot so the
		// existing exact building-only debit assertions retain their coverage.
		const auto FindDirectlyServedLot = [&](UBuildingData* Data)
		{
			if (!Data) return FIntPoint(INDEX_NONE, INDEX_NONE);
			const FIntPoint Offsets[] = {FIntPoint(1,0),FIntPoint(0,1),FIntPoint(-1,0),FIntPoint(0,-1)};
			for (const FGridCell& Candidate : Terrain->Cells)
			{
				if (!Buildings->CanPlaceBuildingAt(Candidate.Coords, Data)) continue;
				bool bDirectAccess = false;
				for (FIntPoint Offset : Offsets)
					bDirectAccess |= Roads->CanAccessRoadFromCell(Candidate.Coords, Candidate.Coords+Offset)
						&& Roads->CanBuildAutomaticAccessEdge(Candidate.Coords, Candidate.Coords+Offset);
				if (!bDirectAccess) continue; // Avoid a whole-world search per unrelated free parcel.
				const auto Quote = Buildings->QuoteAutomaticRoadAccess(Candidate.Coords, Data);
				if (Quote.bRouteValid && Quote.RoadPath.Num()==1 && Quote.NewRoadCells==0 && Quote.RoadCostTL==0)
					return Candidate.Coords;
			}
			return FIntPoint(INDEX_NONE, INDEX_NONE);
		};
		const FIntPoint Free = FindDirectlyServedLot(House);
		FIntPoint Road(INDEX_NONE, INDEX_NONE);
		for (const FGridCell& Cell : Terrain->Cells)
		{
			if (Road.X == INDEX_NONE && Cell.bHasRoad) Road = Cell.Coords;
		}
		Check(TEXT("legacy_required_free_lot_fixture_available"), Free.X != INDEX_NONE,
			TEXT("A missing safe, directly served free lot is a test failure; legacy transaction regressions cannot silently be skipped."));
		const auto FreeAccess = Buildings->QuoteAutomaticRoadAccess(Free, House);
		Check(TEXT("legacy_lot_has_actual_zero_fee_existing_road_frontage"), Free.X!=INDEX_NONE && FreeAccess.bRouteValid
			&& FreeAccess.RoadPath.Num()==1 && FreeAccess.NewRoadCells==0 && FreeAccess.RoadCostTL==0,
			TEXT("The production access quote accepts this original terrain/road layout; no free road, flattened terrain or bypassed purchase gate is fabricated."));
		const int64 Before = Economy->TreasuryTL;
		const int32 Count = Buildings->GetActiveBuildingsCount();
		Check(TEXT("legacy_road_placement_rejected_without_charge"), Road.X != INDEX_NONE && !Grid->IsCoordFree(Road) && Grid->PlaceBuilding(House, Road, ABuildingActor::StaticClass()) == nullptr && Economy->TreasuryTL == Before && Buildings->GetActiveBuildingsCount() == Count, TEXT("Historical prepayment-before-rejection bug must not create an actor or charge the treasury."));
		if (Free.X != INDEX_NONE)
		{
			FGridCell Cell = Terrain->GetCell(Free); Cell.bIsEmergencyAssemblyArea = true; Terrain->SetCell(Free, Cell);
			Check(TEXT("legacy_reserved_assembly_placement_rejected"), !Grid->IsCoordFree(Free) && Grid->PlaceBuilding(House, Free, ABuildingActor::StaticClass()) == nullptr && Economy->TreasuryTL == Before, TEXT("A legacy Blueprint cannot place or pay for a building in an emergency assembly area."));
			Cell.bIsEmergencyAssemblyArea = false; Cell.bVolcanicVent = true; Terrain->SetCell(Free, Cell);
			Check(TEXT("legacy_volcanic_vent_placement_rejected"), !Grid->IsCoordFree(Free) && Grid->PlaceBuilding(House, Free, ABuildingActor::StaticClass()) == nullptr && Economy->TreasuryTL == Before, TEXT("Mapped active vent remains unavailable through the compatibility API."));
			Cell.bVolcanicVent = false; Terrain->SetCell(Free, Cell);
			Economy->TreasuryTL = 0;
			Check(TEXT("legacy_unaffordable_placement_rejected"), Grid->PlaceBuilding(House, Free, ABuildingActor::StaticClass()) == nullptr && Economy->TreasuryTL == 0 && Buildings->GetActiveBuildingsCount() == Count, TEXT("Insufficient funds never create either a canonical lot or a compatibility actor."));
			Economy->TreasuryTL = Before;
			Check(TEXT("legacy_missing_actor_class_does_not_charge"), Grid->PlaceBuilding(House, Free, nullptr) == nullptr && Economy->TreasuryTL == Before, TEXT("Required Blueprint return handle must exist before a construction purchase."));
			ABuildingActor* Handle = Grid->PlaceBuilding(House, Free, ABuildingActor::StaticClass());
			FBuildingInstance Record;
			const int32 Expected = Buildings->CalculateConstructionCost(House, EFoundationQuality::Medium, House->AverageFloors());
			Check(TEXT("legacy_success_uses_one_canonical_purchase"), Handle && Buildings->GetBuildingAtCoords(Free, Record) && Handle->BuildingID == Record.InstanceID && Before - Economy->TreasuryTL == Expected && Buildings->GetActiveBuildingsCount() == Count + 1, FString::Printf(TEXT("expectedCharge=%d actualCharge=%lld"), Expected, Before - Economy->TreasuryTL));
			Check(TEXT("legacy_actor_handle_does_not_duplicate_rendering"), Handle && Handle->IsHidden() && !Handle->GetActorEnableCollision() && !Handle->IsActorTickEnabled(), TEXT("Compatibility actor is a hidden, noncolliding, unticked handle; the canonical ISM supplies the building visual."));
			const int64 After = Economy->TreasuryTL;
			Check(TEXT("legacy_duplicate_placement_is_free_rejection"), Grid->PlaceBuilding(House, Free, ABuildingActor::StaticClass()) == nullptr && Economy->TreasuryTL == After && Buildings->GetActiveBuildingsCount() == Count + 1, TEXT("A second placement on the same lot cannot double charge or duplicate its building."));
			const int32 OldHandleID = Handle ? Handle->BuildingID : INDEX_NONE;
			const bool RemovedOldCanonical = OldHandleID != INDEX_NONE && Buildings->RemoveBuilding(OldHandleID);
			const bool ReplacedThroughNativePurchase = Economy->BuildBuildingWithBudget(Free, House, EFoundationQuality::Medium, House->FloorsMin);
			FBuildingInstance Replacement;
			const bool ReplacementExists = Buildings->GetBuildingAtCoords(Free, Replacement);
			const int64 BeforeStaleDemolition = Economy->TreasuryTL;
			const int32 BeforeStaleCount = Buildings->GetActiveBuildingsCount();
			const bool StaleDemolishResult = Grid->DemolishBuilding(Free);
			FBuildingInstance AfterStaleDemolition;
			Check(TEXT("legacy_stale_handle_cannot_clear_replacement_building"), RemovedOldCanonical && ReplacedThroughNativePurchase && ReplacementExists
				&& Replacement.InstanceID != OldHandleID && !StaleDemolishResult
				&& Terrain->GetCell(Free).OccupyingBuildingID == Replacement.InstanceID
				&& Buildings->GetBuildingByID(Replacement.InstanceID, AfterStaleDemolition)
				&& AfterStaleDemolition.GridCoords == Free && Buildings->GetActiveBuildingsCount() == BeforeStaleCount
				&& Economy->TreasuryTL == BeforeStaleDemolition && Grid->GetBuildingAt(Free) == nullptr,
				TEXT("An old Blueprint handle is discarded without clearing, destroying or charging for the newer canonical building on its former lot."));
			const int64 BeforeOptions = Economy->TreasuryTL;
			UBuildingData* OptionsData = Buildings->GetCatalogData(EBuildingType::Apartment);
			const FIntPoint SelectedOptionsCell = FindDirectlyServedLot(OptionsData);
			const auto OptionsAccess = Buildings->QuoteAutomaticRoadAccess(SelectedOptionsCell, OptionsData);
			Check(TEXT("legacy_selected_options_lot_has_actual_zero_fee_existing_road_frontage"), SelectedOptionsCell.X!=INDEX_NONE
				&& OptionsAccess.bRouteValid && OptionsAccess.RoadPath.Num()==1 && OptionsAccess.NewRoadCells==0 && OptionsAccess.RoadCostTL==0,
				TEXT("Apartment preview/purchase uses its own catalog data and the real grade-safe existing road, preserving the exact selected-building debit assertion."));
			const int32 SelectedFloors = OptionsData ? OptionsData->FloorsMax : 1;
			const int32 PreviewQuote = OptionsData ? Buildings->CalculateConstructionCost(OptionsData, EFoundationQuality::Strong, SelectedFloors) : 0;
			ABuildingActor* SelectedHandle = Grid->PlaceBuildingWithOptions(OptionsData, SelectedOptionsCell, ABuildingActor::StaticClass(), EFoundationQuality::Strong, SelectedFloors, 90.f);
			FBuildingInstance SelectedRecord;
			Check(TEXT("legacy_selected_preview_options_match_purchase"), SelectedHandle && Buildings->GetBuildingAtCoords(SelectedOptionsCell, SelectedRecord)
				&& SelectedRecord.Foundation == EFoundationQuality::Strong && SelectedRecord.CurrentFloors == SelectedFloors
				&& SelectedHandle->FoundationQuality == EFoundationQuality::Strong && SelectedHandle->CurrentFloors == SelectedFloors
				&& BeforeOptions - Economy->TreasuryTL == PreviewQuote,
				FString::Printf(TEXT("previewQuote=%d actualCharge=%lld selectedFloors=%d"), PreviewQuote, BeforeOptions - Economy->TreasuryTL, SelectedFloors));
			UInstancedStaticMeshComponent* SelectedISM = OptionsData ? Buildings->GetOrCreateISM(OptionsData) : nullptr;
			FTransform SelectedTransform;
			Check(TEXT("legacy_selected_preview_rotation_matches_canonical_ism"), SelectedHandle && SelectedISM
				&& SelectedISM->GetInstanceTransform(SelectedRecord.ISMInstanceIndex, SelectedTransform, true)
				&& FMath::IsNearlyEqual(FRotator::NormalizeAxis(SelectedTransform.Rotator().Yaw), 90.f, .01f)
				&& FMath::IsNearlyEqual(FRotator::NormalizeAxis(SelectedHandle->GetActorRotation().Yaw), 90.f, .01f),
				TEXT("R rotation applies to both the one canonical ISM instance and its hidden Blueprint handle."));
			const int64 BeforeValidDemolition = Economy->TreasuryTL;
			const int32 BeforeValidDemolitionCount = Buildings->GetActiveBuildingsCount();
			const int32 SelectedID = SelectedHandle ? SelectedHandle->BuildingID : INDEX_NONE;
			const bool ValidDemolishResult = Grid->DemolishBuilding(SelectedOptionsCell);
			FBuildingInstance RemovedRecord;
			Check(TEXT("legacy_valid_handle_removes_only_its_own_canonical_building"), SelectedID != INDEX_NONE && ValidDemolishResult
				&& !Buildings->GetBuildingByID(SelectedID, RemovedRecord) && Terrain->GetCell(SelectedOptionsCell).OccupyingBuildingID == INDEX_NONE
				&& Buildings->GetActiveBuildingsCount() == BeforeValidDemolitionCount - 1 && Economy->TreasuryTL == BeforeValidDemolition,
				TEXT("A valid legacy handle still removes its own building and cell through canonical removal without a refund or extra charge."));
		}
	}
	Check(TEXT("material_hazard_buttons_have_real_native_bindings"),HUD->BtnTriggerAvalanche&&HUD->BtnTriggerLandslide&&HUD->BtnTriggerAvalanche->OnClicked.IsBound()&&HUD->BtnTriggerLandslide->OnClicked.IsBound(),TEXT("Original seven button identities remain; new snow/soil controls dispatch their own canonical types."));
	UAftershockDisasterDirectorSubsystem* Director=World->GetSubsystem<UAftershockDisasterDirectorSubsystem>();
	FDisasterReport Pending;Pending.DisasterType=EDisasterType::Earthquake;Pending.bIsPending=true;Pending.DisasterEventID=FGuid::NewGuid();
	const int64 PendingTreasury=Economy->TreasuryTL;
	HUD->HandleDisasterCompleted(Pending);Director->HandleDisasterCompleted(Pending);
	Check(TEXT("pending_physical_report_is_not_a_final_hud_or_director_result"),!HUD->PanelDisasterResults->IsVisible()&&!World->GetTimerManager().TimerExists(HUD->ResultsRevealTimer)&&Director->GetState().Phase==EAftershockDisasterPhase::Ready&&Economy->TreasuryTL==PendingTreasury,TEXT("Direct production callback guards only; no simulated job, damage or financial outcome is claimed."));
	FAftershockDirectorState Awaiting;Awaiting.Phase=EAftershockDisasterPhase::Warning;Awaiting.RemainingSeconds=0.f;Awaiting.EventSequence=1;
	FText PhysicalFlowError;
	const bool AwaitingRestored=Director->RestoreSaveState(Awaiting,PhysicalFlowError);
	FDisasterReport Aborted=Pending;Aborted.bIsPending=false;Aborted.bWasRejected=true;Aborted.RejectionReason=FText::FromString(TEXT("Explicit native failed-worker callback fixture"));
	Director->HandleDisasterCompleted(Aborted);HUD->HandleDisasterCompleted(Aborted);
	Check(TEXT("failed_physical_callback_aborts_without_settling_final_report"),AwaitingRestored&&Director->GetState().Phase==EAftershockDisasterPhase::Ready&&Director->GetState().EventSequence==1&&Director->GetState().LastAppliedEventSequence==0&&!Director->GetState().LastReport.DisasterEventID.IsValid()&&!HUD->PanelDisasterResults->IsVisible()&&Economy->TreasuryTL==PendingTreasury,TEXT("A requested ordinal can be aborted; no final LastReport, award, casualty or result modal is manufactured."));
	const bool SecondAwaitingRestored=Director->RestoreSaveState(Awaiting,PhysicalFlowError);
	FDisasterReport Finished=Pending;Finished.bIsPending=false;Director->HandleDisasterCompleted(Finished);
	const int64 SettledSequence=Director->GetState().EventSequence;Director->HandleDisasterCompleted(Finished);
	Check(TEXT("async_completion_and_duplicate_keep_one_requested_ordinal"),SecondAwaitingRestored&&SettledSequence==1&&Director->GetState().EventSequence==1&&Director->GetState().LastAppliedEventSequence==1&&Director->GetState().LastReport.DisasterEventID==Finished.DisasterEventID,TEXT("Production Director callback is invoked in isolation; no global completion delegate or economy subscriber is forged."));
	FAftershockDirectorState DirectorReset;Director->RestoreSaveState(DirectorReset,PhysicalFlowError);
	World->GetTimerManager().ClearTimer(HUD->ResultsRevealTimer);HUD->PendingVisualRevision=INDEX_NONE;HUD->bDisasterInFlight=false;HUD->bIsWarningActive=false;HUD->RefreshDirectorUI();
	const FDisasterReport Quake=World->GetSubsystem<UDisasterSimulationSubsystem>()->TriggerDisaster(EDisasterType::Earthquake,0.f,1.f,true);
	Check(TEXT("instant_quake_result_waits_for_ten_second_presentation_clock"),!Quake.bWasRejected&&HUD->bDisasterInFlight&&World->GetTimerManager().TimerExists(HUD->ResultsRevealTimer)&&!HUD->PanelDisasterResults->IsVisible(),TEXT("The real canonical quake has zero weather duration but ten seconds of presentation; UMG cannot reveal its result immediately."));
	Economy->bIsGameOver=true;HUD->HandleGameOver(FText::FromString(TEXT("quake game-over presentation fixture")));
	Check(TEXT("quake_game_over_modal_waits_for_the_same_presentation_clock"),!HUD->PanelGameOver->IsVisible(),TEXT("Game-over accounting is immediate; its modal waits for the same actual quake presentation interval."));Economy->bIsGameOver=false;
	HUD->NativeDestruct();
	Check(TEXT("native_destruct_clears_widget_timers"), !World->GetTimerManager().TimerExists(HUD->BuildHintTimerHandle) && !World->GetTimerManager().TimerExists(HUD->WarningTimerHandle), TEXT("Reloaded HUDs cannot leave timer callbacks referencing removed controls."));
	return Finish();
}
#else
UAftershockHUDInputAuditCommandlet::UAftershockHUDInputAuditCommandlet() {}
int32 UAftershockHUDInputAuditCommandlet::Main(const FString& Params) { return 1; }
#endif
