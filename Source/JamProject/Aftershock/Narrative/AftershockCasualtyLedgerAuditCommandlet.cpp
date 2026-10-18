#include "Aftershock/Narrative/AftershockCasualtyLedgerAuditCommandlet.h"
#if WITH_EDITOR
#include "Aftershock/Narrative/CasualtyLedgerGenerator.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include <limits>

namespace
{
	struct FScopedLedgerWorld
	{
		UWorld* World=nullptr;
		~FScopedLedgerWorld(){if(World){World->DestroyWorld(false);GEngine->DestroyWorldContext(World);World->RemoveFromRoot();}}
		bool Create(const TCHAR* Name){World=UWorld::CreateWorld(EWorldType::Game,false,FName(Name),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);if(World)GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);return World!=nullptr;}
	};
}
#endif
UAftershockCasualtyLedgerAuditCommandlet::UAftershockCasualtyLedgerAuditCommandlet(){IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}
int32 UAftershockCasualtyLedgerAuditCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	int32 Passed=0,Failed=0;TArray<FString> Rows;
	const auto Check=[&](const TCHAR* Name,bool Good){Good?++Passed:++Failed;Rows.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"),Name,Good?TEXT("true"):TEXT("false")));if(Good){UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CASUALTY_AUDIT_PASS %s"),Name);}else{UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_CASUALTY_AUDIT_FAIL %s"),Name);}};
	if(!GEngine)return 2;FScopedLedgerWorld Scope;if(!Scope.Create(TEXT("AftershockCasualtyLedgerAudit")))return 2;UWorld* W=Scope.World;
	auto* T=W->GetSubsystem<UTerrainGridSubsystem>();auto* M=W->GetSubsystem<UBuildingManagerSubsystem>();auto* L=W->GetSubsystem<UCityLifeSubsystem>();auto* E=W->GetSubsystem<UEconomyAndStateSubsystem>();auto* D=W->GetSubsystem<UDisasterSimulationSubsystem>();auto* N=W->GetSubsystem<UCasualtyLedgerGenerator>();
	if(!T||!M||!L||!E||!D||!N)return 2;
	T->GenerateProceduralTerrain(20,20,500.f,1505);for(auto& C:T->Cells){C.bIsPlayable=true;C.bIsRiverbed=false;C.bIsOcean=false;C.Elevation=0;C.DrainageQuality=0;}
	E->ResetState(1000000);int32 House1=INDEX_NONE,House2=INDEX_NONE;
	const bool Built=M->PlaceBuilding(FIntPoint(4,4),M->GetCatalogData(EBuildingType::House),EFoundationQuality::Weak,1,House1)
		&&M->PlaceBuilding(FIntPoint(6,4),M->GetCatalogData(EBuildingType::House),EFoundationQuality::Strong,1,House2);
	L->InitializeForCity();Check(TEXT("fixture_has_only_canonical_real_residents"),Built&&L->GetState().LivingPopulation==8);
	FResidentIdentityCard CardA,CardB;const int32 FirstID=L->GetResidents().IsEmpty()?INDEX_NONE:L->GetResidents()[0].ResidentID;
	Check(TEXT("resident_identity_name_age_stable"),N->GetResidentIdentityCard(FirstID,CardA)&&N->GetResidentIdentityCard(FirstID,CardB)&&CardA.FullName.EqualTo(CardB.FullName)&&CardA.Age==CardB.Age&&CardA.Age>0);
	const int64 Cash=E->TreasuryTL,Aid=E->PendingStateAidTL;const auto R=D->TriggerDisaster(EDisasterType::Earthquake,2000.f,0.f,false);
	Check(TEXT("real_disaster_assigns_real_death_ids"),!R.bWasRejected&&R.DisasterEventID.IsValid()&&R.TotalDeaths>0&&R.TotalDeaths==R.ActualDeathResidentIDs.Num()&&N->GetEntries().Num()==R.TotalDeaths);
	bool AllReal=true;for(const auto& Row:N->GetEntries()){FCityResidentRecord Person;AllReal&=L->GetResident(Row.ResidentID,Person)&&!Person.bAlive&&!Row.FullName.IsEmpty()&&Row.Age>0&&!Row.Narrative.ToString().Contains(TEXT("bütçe"));}
	Check(TEXT("every_actual_death_has_factual_named_row"),AllReal);
	Check(TEXT("death_is_exact_500_cost_survivor_exact_150_aid"),Cash-E->TreasuryTL==static_cast<int64>(R.TotalDeaths)*500&&E->PendingStateAidTL-Aid==static_cast<int64>(R.ExposedSurvivorResidentIDs.Num())*150);
	const int64 SettledCash=E->TreasuryTL,SettledAid=E->PendingStateAidTL;E->HandleDisasterCompleted(R);
	Check(TEXT("same_event_cannot_charge_or_grant_twice"),E->TreasuryTL==SettledCash&&E->PendingStateAidTL==SettledAid);
	TArray<FGuid> Applied;E->CaptureAppliedDisasterEvents(Applied);FText Error;Check(TEXT("persisted_event_ids_restore"),E->RestoreAppliedDisasterEvents(Applied,Error));E->HandleDisasterCompleted(R);
	Check(TEXT("reload_does_not_reapply_fiscal_event"),E->TreasuryTL==SettledCash&&E->PendingStateAidTL==SettledAid);
	if(!N->GetEntries().IsEmpty()){const int32 ID=N->GetEntries()[0].ResidentID;Check(TEXT("real_person_flower_once"),N->PlaceFlower(ID)&&!N->PlaceFlower(ID)&&N->HasFlowerTribute(ID)&&N->GetFlowerTributeCount()==1);}
	FAftershockCasualtyLedgerSaveState Ledger;N->CaptureSaveState(Ledger);Check(TEXT("ledger_snapshot_restores_without_adding_rows"),N->RestoreSaveState(Ledger,Error)&&N->GetEntries().Num()==R.TotalDeaths&&N->GetFlowerTributeCount()==1);
	if(!Ledger.Entries.IsEmpty()){auto Invalid=Ledger;const auto Copy=Invalid.Entries[0];Invalid.Entries.Add(Copy);Check(TEXT("duplicate_resident_rejected_before_ledger_mutation"),!N->RestoreSaveState(Invalid,Error)&&N->GetEntries().Num()==Ledger.Entries.Num());}
	FAftershockCasualtyLedgerSaveState LegacyA,LegacyB;UCasualtyLedgerGenerator::BuildLegacyLedgerState(L->GetResidents(),FGuid(1,2,3,4),E->CurrentTurn,LegacyA);UCasualtyLedgerGenerator::BuildLegacyLedgerState(L->GetResidents(),FGuid(1,2,3,4),E->CurrentTurn,LegacyB);
	Check(TEXT("historical_legacy_rows_truthful_no_retroactive_cost"),N->ValidateSaveState(LegacyA,Error)&&LegacyA.Entries.Num()==L->GetState().TotalDeaths&&LegacyA.Events.Num()==1&&LegacyA.Events[0].bHistoricalUnattributed&&LegacyA.Events[0].DeathCostTL==0&&LegacyA.Events[0].SurvivorAidTL==0&&LegacyA.Events[0].EventID==LegacyB.Events[0].EventID);
	TArray<FBuildingInstance> Buildings;M->GetAllBuildings(Buildings);const auto* Collapsed=Buildings.FindByPredicate([](const auto& B){return B.DamageState==EDamageState::Collapsed;});
	if(Collapsed)
	{
		const int32 ID=Collapsed->InstanceID;const FIntPoint Coord=Collapsed->GridCoords;const int32 Living=L->GetState().LivingPopulation;const int64 Before=E->TreasuryTL;
		const bool Cleared=E->ClearRubbleWithBudget(Coord);FBuildingInstance Memorial;const bool Found=M->GetBuildingByID(ID,Memorial);
		Check(TEXT("clear_3000_creates_occupied_three_turn_memorial"),Cleared&&Before-E->TreasuryTL==3000&&Found&&Memorial.DamageState==EDamageState::Memorial&&T->GetCell(Coord).OccupyingBuildingID==ID&&M->GetMemorialTurnsRemaining(ID)==3);
		Check(TEXT("memorial_blocks_rebuild_without_deleting_survivors"),!M->CanPlaceBuildingAt(Coord,M->GetCatalogData(EBuildingType::House))&&!M->RemoveBuilding(ID)&&L->GetState().LivingPopulation==Living&&L->GetBuildingOperation01(ID)==0);
		const int64 After=E->TreasuryTL;Check(TEXT("second_clear_is_noop"),!E->ClearRubbleWithBudget(Coord)&&E->TreasuryTL==After);
		E->AdvanceTurn();E->AdvanceTurn();Check(TEXT("memorial_still_occupied_after_two_real_turns"),M->GetBuildingByID(ID,Memorial)&&M->GetMemorialTurnsRemaining(ID)==1&&T->GetCell(Coord).OccupyingBuildingID==ID);
		E->AdvanceTurn();Check(TEXT("third_real_turn_releases_memorial_parcel"),!M->GetBuildingByID(ID,Memorial)&&T->GetCell(Coord).OccupyingBuildingID==INDEX_NONE&&L->GetState().LivingPopulation==Living);
	}
	else Check(TEXT("memorial_fixture_available"),false);
	UGameInstance* GI=NewObject<UGameInstance>();auto* Archive=NewObject<UCasualtyLedgerArchiveSubsystem>(GI);
	FCasualtyCitySummary City;City.SessionID=FGuid(11,22,33,44);City.RegionID=FName(GetAftershockGeographyFocus(0,0).Id);City.FocusID=FName(GetAftershockGeographyFocus(0,0).FocusId);City.Deaths=3;City.UniqueExposedSurvivors=5;City.LivingPopulation=8;City.FlowerTributes=2;City.RetrofitInvestments=4;
	Archive->ReplaceCitySnapshot(City);Archive->ReplaceCitySnapshot(City);Check(TEXT("campaign_archive_replace_same_city_is_idempotent"),Archive->GetTotals().Cities==1&&Archive->GetTotals().Deaths==3);
	City.CampaignStage=1;City.RegionID=FName(GetAftershockGeographyFocus(1,0).Id);City.FocusID=FName(GetAftershockGeographyFocus(1,0).FocusId);City.Deaths=2;Archive->ReplaceCitySnapshot(City);Check(TEXT("campaign_archive_accumulates_distinct_cities"),Archive->GetTotals().Cities==2&&Archive->GetTotals().Deaths==5&&Archive->GetTotals().UniqueExposedSurvivors==10);
	FGuid ArchiveSession;TArray<FCasualtyCitySummary> Summaries;Archive->CaptureArchive(ArchiveSession,Summaries);Check(TEXT("campaign_archive_restore_not_additive"),Archive->RestoreArchive(ArchiveSession,Summaries,Error)&&Archive->GetTotals().Deaths==5&&Archive->GetTotals().Cities==2);
	{auto Invalid=Summaries;Invalid[0].CampaignStage=7;Check(TEXT("archive_unknown_stage_rejected"),!Archive->RestoreArchive(ArchiveSession,Invalid,Error)&&Archive->GetTotals().Cities==2);Invalid=Summaries;Invalid[0].RegionID=TEXT("invented_region");Check(TEXT("archive_unknown_region_rejected"),!Archive->RestoreArchive(ArchiveSession,Invalid,Error));Invalid=Summaries;Invalid[0].FocusID=NAME_None;Check(TEXT("archive_unspecified_focus_rejected"),!Archive->RestoreArchive(ArchiveSession,Invalid,Error));}
	{
		TStrongObjectPtr<UGameInstance> TravelGI(NewObject<UGameInstance>(GEngine));TravelGI->InitializeStandalone(TEXT("AftershockLedgerTravelFixture"));
		UWorld* TravelWorld=TravelGI->GetWorld();auto* Session=TravelGI->GetSubsystem<UAftershockOpeningSessionSubsystem>();
		if(TravelWorld&&Session)
		{
			FAftershockSessionSettings Settings;Settings.CampaignStage=0;Session->ConfigureNewSession(Settings,Error);Session->MarkSessionStarted();
			TravelWorld->GetSubsystem<UCityLifeSubsystem>()->InitializeForCity();auto* TravelLedger=TravelWorld->GetSubsystem<UCasualtyLedgerGenerator>();TravelLedger->SnapshotCurrentCity();
			FAftershockCasualtyLedgerSaveState Before;TravelLedger->CaptureSaveState(Before);Settings.CampaignStage=1;
			Session->RestoreSession(Settings,Session->GetCampaignProgress(),Session->GetSessionID());TravelLedger->SnapshotCurrentCity();FAftershockCasualtyLedgerSaveState After;TravelLedger->CaptureSaveState(After);
			Check(TEXT("advanced_gi_stage_cannot_rekey_current_world_archive"),Before.bHasCityKey&&After.CityCampaignStage==Before.CityCampaignStage&&After.CitySessionID==Before.CitySessionID&&After.ArchiveSummaries.Num()==1&&After.ArchiveSummaries[0].CampaignStage==Before.CityCampaignStage);
			Settings.CampaignStage=0;Session->ConfigureNewSession(Settings,Error);Session->MarkSessionStarted();TravelLedger->SnapshotCurrentCity();FAftershockCasualtyLedgerSaveState NewSession;TravelLedger->CaptureSaveState(NewSession);
			Check(TEXT("new_session_id_cannot_archive_old_world_ledger"),NewSession.CitySessionID==Before.CitySessionID&&NewSession.CitySessionID!=Session->GetSessionID());
		}
		else Check(TEXT("travel_key_fixture_game_instance"),false);
		TravelGI->Shutdown();
		if(TravelWorld){TravelWorld->DestroyWorld(false);GEngine->DestroyWorldContext(TravelWorld);TravelWorld->RemoveFromRoot();}
	}
	const auto Risk=[](float C,int32 P,int32 B,int32 A){return UEconomyAndStateSubsystem::CalculateLocalClimateRisk(C,P,B,A);};
	Check(TEXT("local_climate_index_is_bounded_and_monotonic"),Risk(0,100,10,1000)>=1&&Risk(MAX_flt,100,10000,1000)<=2&&Risk(200,100,10,1000)>Risk(100,100,10,1000)&&Risk(100,100,20,1000)>Risk(100,100,10,1000));
	Check(TEXT("city_scale_does_not_multiply_climate_pressure"),FMath::IsNearlyEqual(Risk(100,100,10,1000),Risk(1000,1000,100,10000),.0001f));
	{
		int32 NewHome=INDEX_NONE;const bool Placed=M->PlaceBuilding(FIntPoint(10,10),M->GetCatalogData(EBuildingType::House),EFoundationQuality::Medium,1,NewHome);L->RefreshCity();
		const auto Before=L->GetResidents();TArray<FBuildingInstance> EventBuildings;M->GetAllBuildings(EventBuildings);const auto NewDeaths=L->ApplyCasualties(NewHome,1,false);TArray<int32> Survivors;
		for(const auto& Person:L->GetResidents())if(Person.bAlive&&Person.HomeBuildingID==NewHome)Survivors.Add(Person.ResidentID);
		FDisasterReport Real;Real.DisasterEventID=FGuid::NewGuid();Real.DisasterType=EDisasterType::Flood;Real.BaseSeverity=37.f;Real.ActualDeathResidentIDs=NewDeaths;Real.ExposedSurvivorResidentIDs=Survivors;Real.TotalDeaths=NewDeaths.Num();Real.TotalSurvivors=Survivors.Num();Real.TotalDeathCostTL=static_cast<int64>(Real.TotalDeaths)*500;Real.TotalStateAidGranted=static_cast<int64>(Real.TotalSurvivors)*150;
		const bool Recorded=N->RecordDisaster(Real.DisasterEventID,Real.DisasterType,E->CurrentTurn,false,Before,EventBuildings,NewDeaths,Survivors,false,Real.BaseSeverity);
		if(Placed&&Recorded&&!NewDeaths.IsEmpty()&&!R.ActualDeathResidentIDs.IsEmpty())
		{
			FDisasterReport Forged=Real;Forged.ActualDeathResidentIDs[0]=R.ActualDeathResidentIDs[0];const int64 BeforeCash=E->TreasuryTL,BeforeAid=E->PendingStateAidTL;
			E->HandleDisasterCompleted(Forged);TArray<FGuid> Processed;E->CaptureAppliedDisasterEvents(Processed);
			Check(TEXT("forged_first_report_rejected_without_consuming_guid"),E->TreasuryTL==BeforeCash&&E->PendingStateAidTL==BeforeAid&&!Processed.Contains(Real.DisasterEventID));
			FAftershockEconomySaveState BeforeEconomy;E->CaptureSaveState(BeforeEconomy);const float BeforeFood=L->GetState().FoodStock;
			const auto RejectsInputs=[&](const TCHAR* Name,const FDisasterReport& Bad)
			{
				E->HandleDisasterCompleted(Bad);TArray<FGuid> IDs;E->CaptureAppliedDisasterEvents(IDs);FAftershockEconomySaveState AfterEconomy;E->CaptureSaveState(AfterEconomy);
				Check(Name,E->TreasuryTL==BeforeCash&&E->PendingStateAidTL==BeforeAid&&!IDs.Contains(Real.DisasterEventID)&&AfterEconomy.SurvivedDisasters==BeforeEconomy.SurvivedDisasters&&L->GetState().FoodStock==BeforeFood);
			};
			Forged=Real;Forged.bNeedsCasualtyEvent=true;RejectsInputs(TEXT("forged_needs_flag_rejected_before_guid_and_shock"),Forged);
			Forged=Real;Forged.DisasterType=EDisasterType::Earthquake;RejectsInputs(TEXT("forged_hazard_type_rejected_before_guid_and_shock"),Forged);
			Forged=Real;Forged.BaseSeverity=std::numeric_limits<float>::quiet_NaN();RejectsInputs(TEXT("nan_settlement_severity_rejected_before_guid_and_shock"),Forged);
			Forged=Real;Forged.BaseSeverity=-1.f;RejectsInputs(TEXT("negative_settlement_severity_rejected_before_guid_and_shock"),Forged);
			Forged=Real;Forged.BaseSeverity+=1.f;RejectsInputs(TEXT("different_finite_severity_rejected_before_guid_and_shock"),Forged);
			FAftershockCasualtyLedgerSaveState Exact;N->CaptureSaveState(Exact);auto EarlierSchemaFour=Exact;
			if(auto* OldEvent=EarlierSchemaFour.Events.FindByPredicate([&](const auto& Event){return Event.EventID==Real.DisasterEventID;}))
			{
				OldEvent->bHasSettlementInputs=false;OldEvent->bNeedsCasualtyEvent=false;OldEvent->DisasterType=EDisasterType::Earthquake;OldEvent->BaseSeverity=0.f;
				Check(TEXT("earlier_schema_four_events_remain_readable_but_not_replayable"),N->RestoreSaveState(EarlierSchemaFour,Error)&&!N->ValidateDisasterSettlement(Real)&&N->RestoreSaveState(Exact,Error));
			}
			E->HandleDisasterCompleted(Real);Check(TEXT("correct_report_after_forgery_still_settles_once"),E->TreasuryTL==BeforeCash-Real.TotalDeathCostTL&&E->PendingStateAidTL==BeforeAid+Real.TotalStateAidGranted);
		}
		else Check(TEXT("forged_settlement_fixture_has_distinct_real_dead_ids"),false);
	}
	{
		FScopedLedgerWorld WitnessScope;Check(TEXT("building_witness_fixture_world"),WitnessScope.Create(TEXT("AftershockBuildingWitnessAudit")));
		if(WitnessScope.World)
		{
			auto* WT=WitnessScope.World->GetSubsystem<UTerrainGridSubsystem>();auto* WM=WitnessScope.World->GetSubsystem<UBuildingManagerSubsystem>();auto* WL=WitnessScope.World->GetSubsystem<UCityLifeSubsystem>();auto* WN=WitnessScope.World->GetSubsystem<UCasualtyLedgerGenerator>();
			WT->GenerateProceduralTerrain(36,10,500.f,1507);for(auto& Cell:WT->Cells){Cell.bIsPlayable=true;Cell.bIsOcean=false;Cell.bIsRiverbed=false;Cell.Elevation=0;}
			TArray<int32> Homes;for(int32 I=0;I<16;++I){int32 ID=INDEX_NONE;if(WM->PlaceBuilding(FIntPoint(2+I*2,4),WM->GetCatalogData(EBuildingType::House),EFoundationQuality::Strong,1,ID))Homes.Add(ID);}
			WL->InitializeForCity();const auto Before=WL->GetResidents();TArray<FBuildingInstance> EventBuildings;WM->GetAllBuildings(EventBuildings);TArray<int32> Exposed;for(const auto& Person:Before)if(Person.bAlive)Exposed.Add(Person.ResidentID);
			const FGuid EventID=FGuid::NewGuid();const bool Recorded=WN->RecordDisaster(EventID,EDisasterType::Earthquake,1,false,Before,EventBuildings,{},Exposed);
			FAftershockCasualtyLedgerSaveState Saved;WN->CaptureSaveState(Saved);bool EveryBuilding=Homes.Num()==16;
			for(int32 ID:Homes){const auto Stories=WN->GetBuildingNarrativePreview(ID);EveryBuilding&=Stories.Num()==1&&Stories[0].BuildingID==ID&&Exposed.Contains(Stories[0].ResidentID)&&Stories[0].Outcome==ECitizenMicroStoryOutcome::Survived&&Stories[0].DisasterEventID==EventID;}
			Check(TEXT("every_affected_residential_building_has_real_witness_beyond_global_twelve"),Recorded&&Saved.SurvivalStories.Num()==12&&Saved.BuildingWitnessStories.Num()==16&&EveryBuilding);
			Check(TEXT("building_witness_round_trip_preserves_all_sixteen"),WN->ValidateSaveState(Saved,Error)&&WN->RestoreSaveState(Saved,Error)&&!Homes.IsEmpty()&&WN->GetBuildingNarrativePreview(Homes.Last()).Num()==1);
			if(!Saved.BuildingWitnessStories.IsEmpty())
			{
				auto Bad=Saved;Bad.BuildingWitnessStories[0].ResidentID=MAX_int32;Check(TEXT("witness_not_in_event_survivors_rejected"),!WN->RestoreSaveState(Bad,Error));
				Bad=Saved;const auto Duplicate=Bad.BuildingWitnessStories[0];Bad.BuildingWitnessStories.Add(Duplicate);Check(TEXT("duplicate_building_witness_rejected"),!WN->RestoreSaveState(Bad,Error));
				Bad=Saved;Bad.BuildingWitnessStories[0].BuildingDefinitionType=static_cast<EBuildingType>(255);Check(TEXT("unknown_witness_building_definition_rejected"),!WN->RestoreSaveState(Bad,Error));
				Bad=Saved;Bad.BuildingWitnessStories[0].DisasterEventID=FGuid::NewGuid();Check(TEXT("unknown_witness_event_rejected"),!WN->RestoreSaveState(Bad,Error));
				Bad=Saved;Bad.BuildingWitnessStories.Reset();Check(TEXT("older_schema_four_without_witnesses_is_not_fabricated"),WN->ValidateSaveState(Bad,Error));
			}
			const auto BeforeDeath=WL->GetResidents();const int32 LatestHome=Homes.IsEmpty()?INDEX_NONE:Homes.Last();const auto DeathIDs=WL->ApplyCasualties(LatestHome,1,false);const FGuid Later=FGuid::NewGuid();
			const bool LaterRecorded=WN->RecordDisaster(Later,EDisasterType::Earthquake,2,false,BeforeDeath,EventBuildings,DeathIDs,{});
			WN->CaptureSaveState(Saved);const auto Newest=WN->GetBuildingNarrativePreview(LatestHome,1);
			Check(TEXT("later_death_keeps_truthful_living_at_prior_event_witness"),LaterRecorded&&!DeathIDs.IsEmpty()&&WN->ValidateSaveState(Saved,Error)&&Saved.BuildingWitnessStories.Num()==16&&Newest.Num()==1&&Newest[0].Outcome==ECitizenMicroStoryOutcome::Deceased&&Newest[0].DisasterEventID==Later);
		}
	}
	{
		FScopedLedgerWorld WorkScope;Check(TEXT("historical_workplace_fixture_world"),WorkScope.Create(TEXT("AftershockHistoricalWorkplaceAudit")));
		if(WorkScope.World)
		{
			auto* WT=WorkScope.World->GetSubsystem<UTerrainGridSubsystem>();auto* WM=WorkScope.World->GetSubsystem<UBuildingManagerSubsystem>();auto* WL=WorkScope.World->GetSubsystem<UCityLifeSubsystem>();auto* WN=WorkScope.World->GetSubsystem<UCasualtyLedgerGenerator>();
			WT->GenerateProceduralTerrain(12,12,1000.f,1527);for(auto& Cell:WT->Cells){Cell.bIsPlayable=true;Cell.bIsOcean=false;Cell.bIsRiverbed=false;Cell.Elevation=0;Cell.bHasRoad=Cell.Coords.Y==3;}
			int32 HomeID=INDEX_NONE,WorkID=INDEX_NONE;
			const bool Placed=WM->PlaceBuilding(FIntPoint(4,4),WM->GetCatalogData(EBuildingType::House),EFoundationQuality::Strong,1,HomeID)&&WM->PlaceBuilding(FIntPoint(6,4),WM->GetCatalogData(EBuildingType::Factory),EFoundationQuality::Strong,1,WorkID);
			WL->InitializeForCity();const auto Before=WL->GetResidents();const auto* Worker=Before.FindByPredicate([&](const auto& Person){return Person.bAlive&&Person.WorkBuildingID==WorkID;});
			Check(TEXT("historical_workplace_fixture_has_canonical_employee"),Placed&&Worker);
			if(Worker)
			{
				TArray<FBuildingInstance> Historical;WM->GetAllBuildings(Historical);FBuildingInstance Work;WM->GetBuildingByID(WorkID,Work);
				const FString Expected=Work.BuildingData->DisplayName.ToString()+TEXT(" çalışanı");
				// Preserve the old first-match behavior even for a duplicate-ID input;
				// the actual canonical building registry itself always has unique IDs.
				FBuildingInstance Duplicate=Work;Duplicate.BuildingData=NewObject<UBuildingData>(WM);Duplicate.BuildingData->DisplayName=FText::FromString(TEXT("Later duplicate workplace"));Historical.Add(Duplicate);
				const bool Removed=WM->RemoveBuilding(WorkID);const TArray<int32> Survived={Worker->ResidentID};
				const bool Recorded=WN->RecordDisaster(FGuid::NewGuid(),EDisasterType::Earthquake,1,false,Before,Historical,{},Survived);
				const auto Stories=WN->GetBuildingNarrativePreview(HomeID,1);
				Check(TEXT("story_workplace_uses_first_historical_record_after_live_employer_removed"),Removed&&Recorded&&Stories.Num()==1&&Stories[0].Occupation.ToString()==Expected);
			}
		}
	}
	{
		FScopedLedgerWorld NeedsScope;Check(TEXT("needs_dismissal_fixture_world"),NeedsScope.Create(TEXT("AftershockNeedsDismissalAudit")));
		if(NeedsScope.World)
		{
			auto* NT=NeedsScope.World->GetSubsystem<UTerrainGridSubsystem>();auto* NM=NeedsScope.World->GetSubsystem<UBuildingManagerSubsystem>();auto* NL=NeedsScope.World->GetSubsystem<UCityLifeSubsystem>();auto* NE=NeedsScope.World->GetSubsystem<UEconomyAndStateSubsystem>();auto* NN=NeedsScope.World->GetSubsystem<UCasualtyLedgerGenerator>();
			NT->GenerateProceduralTerrain(12,12,500.f,1508);for(auto& Cell:NT->Cells){Cell.bIsPlayable=true;Cell.bIsOcean=false;Cell.bIsRiverbed=false;Cell.Elevation=0;}
			NE->ResetState(1000000);int32 CriticalHome=INDEX_NONE,HealthyHome=INDEX_NONE;
			const bool Placed=NM->PlaceBuilding(FIntPoint(4,4),NM->GetCatalogData(EBuildingType::House),EFoundationQuality::Strong,1,CriticalHome)&&NM->PlaceBuilding(FIntPoint(6,4),NM->GetCatalogData(EBuildingType::House),EFoundationQuality::Strong,1,HealthyHome);
			NL->InitializeForCity();FAftershockCityLifeSaveState Critical;NL->CaptureSaveState(Critical);Critical.State.FoodStock=0;Critical.State.RawFoodStock=0;Critical.State.WaterStock=0;
			for(auto& Person:Critical.Residents){Person.Hunger01=Person.HomeBuildingID==CriticalHome?1.f:0.f;Person.Health01=Person.HomeBuildingID==CriticalHome?0.f:1.f;}
			const bool Ready=Placed&&Critical.Residents.Num()==8&&NL->RestoreSaveState(Critical,Error);NE->PublicSatisfaction=1.f;NE->PendingStateAidTL=12345;
			const int64 BeforeCash=NE->TreasuryTL;NE->AdvanceTurn();
			Check(TEXT("actual_needs_deaths_dismiss_at_zero_with_exact_expense"),Ready&&NL->GetState().TotalDeaths==4&&NL->GetState().LivingPopulation==4&&NE->bIsGameOver&&NE->PublicSatisfaction==0.f&&NE->TreasuryTL==BeforeCash-2000&&NN->GetEntries().Num()==4);
			Check(TEXT("needs_dismissal_skips_post_finalization_tax_recovery_and_aid"),NE->CurrentTurn==2&&NE->PendingStateAidTL==12345&&NE->TreasuryTL==BeforeCash-2000&&NE->PublicSatisfaction==0.f&&NL->GetState().AverageHappiness01>0.f);
			const int64 EndCash=NE->TreasuryTL;NE->AdvanceTurn();Check(TEXT("dismissed_needs_turn_cannot_advance_or_charge_again"),NE->CurrentTurn==2&&NE->TreasuryTL==EndCash&&NE->PendingStateAidTL==12345&&NN->GetEntries().Num()==4);
		}
	}
	if(FParse::Param(*Params,TEXT("AftershockLargeCityAudit")))
	{
		FScopedLedgerWorld Large;Check(TEXT("large_city_fixture_world"),Large.Create(TEXT("AftershockLargeCityLedgerAudit")));
		if(Large.World)
		{
			auto* LT=Large.World->GetSubsystem<UTerrainGridSubsystem>();auto* LM=Large.World->GetSubsystem<UBuildingManagerSubsystem>();auto* LL=Large.World->GetSubsystem<UCityLifeSubsystem>();auto* LE=Large.World->GetSubsystem<UEconomyAndStateSubsystem>();auto* LR=Large.World->GetSubsystem<URoadNetworkSubsystem>();
			LT->GenerateProceduralTerrain(80,80,1000.f,1506);for(auto& C:LT->Cells){C.bIsPlayable=true;C.bIsOcean=false;C.bIsRiverbed=false;C.Elevation=0;C.bHasRoad=(C.Coords.X%2!=0||C.Coords.Y%2!=0);}
			LE->ResetState(50000000);int32 Next=0,Made=0;
			const auto Add=[&](EBuildingType Type,int32 Count){for(int32 I=0;I<Count;++I){const FIntPoint Coord(2+(Next%38)*2,2+(Next/38)*2);++Next;int32 ID;auto* Data=LM->GetCatalogData(Type);Made+=LM->PlaceBuilding(Coord,Data,EFoundationQuality::Medium,Data->FloorsMin,ID)?1:0;}};
			Add(EBuildingType::Apartment,400);Add(EBuildingType::Office,150);Add(EBuildingType::PowerPlant,40);Add(EBuildingType::Warehouse,20);Add(EBuildingType::Substation,1);LR->NotifySaveRestored();
			const double Started=FPlatformTime::Seconds();LL->InitializeForCity();const double InitialMs=(FPlatformTime::Seconds()-Started)*1000.;
			const double RefreshStarted=FPlatformTime::Seconds();LL->RefreshCity();const double RefreshMs=(FPlatformTime::Seconds()-RefreshStarted)*1000.;
			Check(TEXT("large_city_14k_canonical_people_indexed_jobs"),Made==611&&LL->GetState().LivingPopulation==14000&&LL->GetState().EmployedResidents>5000);
			float ActualDemand=0;TArray<FBuildingInstance> LargeBuildings;LM->GetAllBuildings(LargeBuildings);for(const auto& B:LargeBuildings)if(B.BuildingData->Type==EBuildingType::PowerPlant)ActualDemand+=4.f*LL->GetBuildingOperation01(B.InstanceID);
			Check(TEXT("large_city_initial_fuel_covers_four_actual_demand_turns"),ActualDemand>22.5f&&FMath::IsNearlyEqual(LL->GetState().FuelStock,4.f*ActualDemand,.05f));
			FAftershockCityLifeSaveState SavedLife;LL->CaptureSaveState(SavedLife);SavedLife.State.FuelStock=20;LL->RestoreSaveState(SavedLife,Error);const int64 BeforeSupply=LE->TreasuryTL;
			LL->AdvanceCityTurn(2);const int64 SupplyCost=BeforeSupply-LE->TreasuryTL;
			Check(TEXT("large_city_paid_fuel_batches_scale_with_real_warehouses"),SupplyCost>600&&SupplyCost<=20*600&&SupplyCost%600==0&&LL->GetState().LastSupplyExpenseTL==SupplyCost&&LL->GetState().FuelStock>0);
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITY_LIFE_CPU_PROFILE population=%d buildings=%d employed=%d initializationMs=%.3f refreshMs=%.3f actualFuelDemand=%.2f paidFuelExpense=%lld scope=NativeScriptedSubsystemCPU_NotGPUOrFrameTime"),LL->GetState().LivingPopulation,Made,LL->GetState().EmployedResidents,InitialMs,RefreshMs,ActualDemand,SupplyCost);
		}
	}
	const FString Dir=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("PresentationValidation"));IFileManager::Get().MakeDirectory(*Dir,true);
	const FString Json=FString::Printf(TEXT("{\"schema_version\":1,\"utc\":\"%s\",\"scope\":\"Canonical IDs, actual casualties, idempotent fiscal event and memorial turns; native scripted fixture, no UI or GPU proof.\",\"passed\":%d,\"failed\":%d,\"checks\":[%s]}"),*FDateTime::UtcNow().ToIso8601(),Passed,Failed,*FString::Join(Rows,TEXT(",")));
	if(!FFileHelper::SaveStringToFile(Json,*FPaths::Combine(Dir,TEXT("casualty_ledger_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return 2;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CASUALTY_AUDIT_RESULT passed=%d failed=%d"),Passed,Failed);return Failed?1:0;
#else
	return 1;
#endif
}
