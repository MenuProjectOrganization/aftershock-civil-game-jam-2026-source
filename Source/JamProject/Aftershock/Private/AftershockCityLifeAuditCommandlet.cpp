#include "AftershockCityLifeAuditCommandlet.h"

#if WITH_EDITOR
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Narrative/CasualtyLedgerGenerator.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include <limits>

namespace
{
	struct FLifeAuditCheck { FString Name; bool Passed; FString Detail; };
	struct FScopedLifeAuditWorld
	{
		UWorld* World = nullptr;
		~FScopedLifeAuditWorld()
		{
			if (!World) return;
			World->DestroyWorld(false);
			if (GEngine) GEngine->DestroyWorldContext(World);
			World->RemoveFromRoot();
		}
	};
	FString LifeJsonEscape(FString Value)
	{
		Value.ReplaceInline(TEXT("\\"),TEXT("\\\\")); Value.ReplaceInline(TEXT("\""),TEXT("\\\""));
		Value.ReplaceInline(TEXT("\n"),TEXT("\\n")); Value.ReplaceInline(TEXT("\r"),TEXT("\\r")); Value.ReplaceInline(TEXT("\t"),TEXT("\\t")); return Value;
	}
	// Independent reachability assertion over actual canonical road cells. This
	// checks the public job assignments; it does not create a second live network.
	TMap<FIntPoint,int32> AuditRoadComponents(const UTerrainGridSubsystem& Terrain)
	{
		TMap<FIntPoint,int32> Result; int32 Component = 0;
		const FIntPoint Steps[] = { FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1) };
		for (const FGridCell& Cell : Terrain.Cells)
		{
			if (!Cell.bIsPlayable || !Cell.bHasRoad || Cell.bIsRiverbed || Cell.bBurning || Cell.LavaCoverage01>.1f || Result.Contains(Cell.Coords)) continue;
			TArray<FIntPoint> Queue; Queue.Add(Cell.Coords); Result.Add(Cell.Coords,Component);
			for (int32 Head=0; Head<Queue.Num(); ++Head) for (FIntPoint Step : Steps)
			{
				const FIntPoint Coord=Queue[Head]+Step; if (!Terrain.IsPlayableCoord(Coord) || Result.Contains(Coord)) continue;
				const FGridCell Next=Terrain.GetCell(Coord);
				if (!Next.bHasRoad || Next.bIsRiverbed || Next.bBurning || Next.LavaCoverage01>.1f) continue;
				Result.Add(Coord,Component); Queue.Add(Coord);
			}
			++Component;
		}
		return Result;
	}
}
#endif

UAftershockCityLifeAuditCommandlet::UAftershockCityLifeAuditCommandlet()
{
	IsClient=false; IsServer=false; IsEditor=true; LogToConsole=true;
}

int32 UAftershockCityLifeAuditCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	TArray<FLifeAuditCheck> Checks; int32 Failed=0;
	const auto Check = [&](const TCHAR* Name, bool Passed, const FString& Detail)
	{
		Checks.Add({Name,Passed,Detail}); Failed+=Passed?0:1;
		if (Passed) { UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITY_LIFE_AUDIT_PASS %s: %s"),Name,*Detail); }
		else { UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_CITY_LIFE_AUDIT_FAIL %s: %s"),Name,*Detail); }
	};
	const auto Finish = [&]()
	{
		const FString Directory=FPaths::ProjectSavedDir()/TEXT("PresentationValidation"); IFileManager::Get().MakeDirectory(*Directory,true);
		const FString Path=Directory/TEXT("city_life_audit.json");
		FString Json=FString::Printf(TEXT("{\n\"schema_version\":1,\n\"utc\":\"%s\",\n\"scope\":\"Real scenario0 startup and canonical CityLife/Economy/Building/Road APIs in an isolated Game world. Controlled damage, service-arrival and road-disconnection fixtures are explicit. No UI, vehicle-boarding, rendering, performance or scientific model claim.\",\n\"passed\":%d,\n\"failed\":%d,\n\"checks\":[\n"),*FDateTime::UtcNow().ToIso8601(),Checks.Num()-Failed,Failed);
		for (int32 I=0;I<Checks.Num();++I)
		{
			const FLifeAuditCheck& C=Checks[I]; Json+=FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s,\"detail\":\"%s\"}%s\n"),*LifeJsonEscape(C.Name),C.Passed?TEXT("true"):TEXT("false"),*LifeJsonEscape(C.Detail),I+1<Checks.Num()?TEXT(","):TEXT(""));
		}
		Json+=TEXT("]\n}\n"); if (!FFileHelper::SaveStringToFile(Json,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return 2;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITY_LIFE_AUDIT_RESULT passed=%d failed=%d report=%s"),Checks.Num()-Failed,Failed,*Path); return Failed==0?0:1;
	};
	Check(TEXT("engine_initialized"),GEngine!=nullptr,TEXT("No gameplay map, persistent asset or user world is changed.")); if (!GEngine) return Finish();
	FScopedLifeAuditWorld Fixture;
	Fixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("AftershockCityLifeFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("isolated_game_world_created"),Fixture.World!=nullptr,TEXT("World is destroyed when the opt-in commandlet ends.")); if (!Fixture.World) return Finish();
	UWorld* World=Fixture.World; FWorldContext& Context=GEngine->CreateNewWorldContext(EWorldType::Game); Context.SetCurrentWorld(World);
	World->URL.AddOption(TEXT("AftershockScenario=0"));
	UAftershockScenarioSubsystem* Scenario=World->GetSubsystem<UAftershockScenarioSubsystem>();
	UBuildingManagerSubsystem* Buildings=World->GetSubsystem<UBuildingManagerSubsystem>();
	UTerrainGridSubsystem* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();
	UEconomyAndStateSubsystem* Economy=World->GetSubsystem<UEconomyAndStateSubsystem>();
	URoadNetworkSubsystem* Roads=World->GetSubsystem<URoadNetworkSubsystem>();
	UCityLifeSubsystem* Life=World->GetSubsystem<UCityLifeSubsystem>();
	Check(TEXT("canonical_subsystems_ready"),Scenario&&Buildings&&Terrain&&Economy&&Roads&&Life,TEXT("Tests read/write the actual public subsystem APIs."));
	if (!Scenario||!Buildings||!Terrain||!Economy||!Roads||!Life) return Finish();
	Scenario->PrepareScenarioBeforeSettlement(); Buildings->SeedInitialSettlement();
	Check(TEXT("real_startup_initializes_roads_then_life"),Scenario->IsPrepared()&&Scenario->GetScenarioIndex()==0&&Buildings->GetActiveBuildingsCount()==Scenario->GetDesignedLots().Num()&&Roads->CountRoadCells()>0&&Life->IsCityInitialized(),
		FString::Printf(TEXT("lots=%d records=%d roads=%d initialized=%d"),Scenario->GetDesignedLots().Num(),Buildings->GetActiveBuildingsCount(),Roads->CountRoadCells(),Life->IsCityInitialized()));
	{
		// Explicit isolated owner-import fixture. Restore the complete original
		// city afterward; these deaths never become a fiscal or narrative event.
		FAftershockCityLifeSaveState Original;Life->CaptureSaveState(Original);FText Error;
		int32 HomeA=INDEX_NONE,HomeB=INDEX_NONE,WorkA=INDEX_NONE,WorkB=INDEX_NONE;
		for(const auto& Resident:Original.Residents)
		{
			if(HomeA==INDEX_NONE)HomeA=Resident.HomeBuildingID;
			else if(Resident.HomeBuildingID!=HomeA&&HomeB==INDEX_NONE)HomeB=Resident.HomeBuildingID;
			if(Resident.WorkBuildingID!=INDEX_NONE)
			{
				if(WorkA==INDEX_NONE)WorkA=Resident.WorkBuildingID;
				else if(Resident.WorkBuildingID!=WorkA&&WorkB==INDEX_NONE)WorkB=Resident.WorkBuildingID;
			}
		}
		const bool Ready=Original.Residents.Num()>=6&&HomeA!=INDEX_NONE&&HomeB!=INDEX_NONE&&WorkA!=INDEX_NONE&&WorkB!=INDEX_NONE;
		Check(TEXT("occupancy_parity_fixture_has_real_distinct_homes_and_workplaces"),Ready,TEXT("Uses actual scenario building IDs and canonical resident identities, with explicitly authored six-person occupancy."));
		if(Ready)
		{
			auto FixtureState=Original;FixtureState.Residents.Reset();
			const int32 OriginalOrder[]={5,1,4,0,3,2};
			const int32 Homes[]={HomeA,HomeA,HomeB,HomeA,HomeB,HomeB};
			const int32 Workplaces[]={WorkA,WorkA,INDEX_NONE,INDEX_NONE,WorkA,WorkB};
			for(int32 I=0;I<6;++I)
			{
				auto Person=Original.Residents[OriginalOrder[I]];Person.HomeBuildingID=Homes[I];Person.WorkBuildingID=Workplaces[I];
				Person.bWorkingAge=true;Person.Age=30;Person.bAlive=I!=3;FixtureState.Residents.Add(Person);
			}
			FixtureState.State.LivingPopulation=5;FixtureState.State.TotalDeaths=1;
			const auto Scan=[&](int32 BuildingID,bool Day)
			{
				int32 Count=0;for(const auto& Person:Life->GetResidents())if(Person.bAlive)
					Count+=(Day&&Person.WorkBuildingID!=INDEX_NONE?Person.WorkBuildingID:Person.HomeBuildingID)==BuildingID?1:0;
				return Count;
			};
			const auto CountsAgree=[&]()
			{
				const int32 ComparedBuildingIDs[]={HomeA,HomeB,WorkA,WorkB,INDEX_NONE,MAX_int32};
				bool Same=true;
				for(const int32 ID:ComparedBuildingIDs)
				{
					for(bool Day:{false,true})
					{
						Same&=Life->GetLivingOccupants(ID,Day)==Scan(ID,Day);
					}
				}
				return Same;
			};
			const auto Exercise=[&]()
			{
				TArray<int32> Trace;
				const auto Add=[&](int32 ID,int32 Requested,bool Day){Trace.Append(Life->ApplyCasualties(ID,Requested,Day));Trace.Add(INDEX_NONE);};
				Add(HomeA,1,false);Add(WorkA,1,true);Add(HomeA,100,false);Add(WorkA,2,true);
				Add(HomeB,2,true);Add(HomeB,100,false);Add(INDEX_NONE,100,true);Add(MAX_int32,1,false);
				return Trace;
			};
			bool Imported=Life->RestoreSaveState(FixtureState,Error);
			const TArray<int32> LinearTrace=Exercise();FAftershockCityLifeSaveState Linear;Life->CaptureSaveState(Linear);
			Check(TEXT("unscoped_occupancy_calls_retain_no_derived_cache"),Imported&&!Life->HasScopedOccupancyLookup()&&Life->GetScopedOccupancyLookupResidentCount()==0,TEXT("The original linear API remains active outside an explicit synchronous batch."));
			Imported=Life->RestoreSaveState(FixtureState,Error);bool FastCounts=false,SameIDs=false,SameState=false,Nested=false,RestoreInvalidation=false,SyncInvalidation=false;
			{
				FAftershockScopedOccupancyBatch Outer(Life);FastCounts=Imported&&CountsAgree();
				{
					FAftershockScopedOccupancyBatch Inner(Life);Nested=CountsAgree()&&Life->HasScopedOccupancyLookup();
				}
				Nested&=Life->HasScopedOccupancyLookup()&&Life->GetScopedOccupancyLookupResidentCount()==6;
				const auto FastTrace=Exercise();FAftershockCityLifeSaveState Fast;Life->CaptureSaveState(Fast);
				SameIDs=FastTrace==LinearTrace;SameState=FAftershockCityLifeSaveState::StaticStruct()->CompareScriptStruct(&Linear,&Fast,0);
				FastCounts&=CountsAgree()&&Life->GetState().LivingPopulation==0;
				auto Reassigned=FixtureState;Reassigned.Residents[0].WorkBuildingID=WorkB;
				Reassigned.Residents[1].bAlive=false;Reassigned.Residents[1].WorkBuildingID=INDEX_NONE;
				Reassigned.State.LivingPopulation=4;Reassigned.State.TotalDeaths=2;
				RestoreInvalidation=Life->RestoreSaveState(Reassigned,Error)&&!Life->HasScopedOccupancyLookup()
					&&CountsAgree()&&Life->GetLivingOccupants(WorkA,true)==1;
				Life->RefreshCity();SyncInvalidation=!Life->HasScopedOccupancyLookup()&&CountsAgree();
			}
			Check(TEXT("scoped_occupancy_matches_canonical_day_night_and_post_death_counts"),FastCounts,TEXT("Independent linear counts include home fallback, employed workers, prior deceased records and depleted buildings."));
			Check(TEXT("scoped_casualties_preserve_exact_array_order_ids_and_repeated_cross_mode_calls"),SameIDs,TEXT("Intentionally shuffled resident IDs; day/night casualties remove the same canonical identities exactly once."));
			Check(TEXT("scoped_casualties_preserve_entire_original_owner_save_state"),SameState,TEXT("Full reflected CityLife save comparison, including resident attributes, deaths, living population and services."));
			Check(TEXT("nested_occupancy_batch_keeps_outer_lookup_alive"),Nested,TEXT("Inner scope exit cannot clear the enclosing event's derived index."));
			Check(TEXT("same_size_restore_invalidates_reassigned_and_deceased_occupancy"),RestoreInvalidation,TEXT("Restoring six records with changed jobs/alive states cannot reuse same-size stale indices or counts."));
			Check(TEXT("normal_job_sync_invalidates_occupancy_inside_active_batch"),SyncInvalidation,TEXT("The real RefreshCity/SyncResidentsAndJobs path invalidates before changing jobs; later queries match live records."));
			Check(TEXT("outer_batch_exit_releases_every_derived_occupancy_entry"),!Life->HasScopedOccupancyLookup()&&Life->GetScopedOccupancyLookupResidentCount()==0,TEXT("No duplicate population or per-event index is retained outside its scope."));
			Check(TEXT("occupancy_parity_fixture_restores_original_city"),Life->RestoreSaveState(Original,Error),TEXT("Subsequent audit checks continue on the original scenario0 roster and service state."));
		}
	}
	TArray<FBuildingInstance> Records; Buildings->GetAllBuildings(Records);
	TMap<int32,FBuildingInstance> ByID; int32 HousingCapacity=0,JobCapacity=0; TArray<int32> PowerIDs,FoodWaterIDs; int32 HomeID=INDEX_NONE,HospitalID=INDEX_NONE;
	for (const FBuildingInstance& B:Records) if (B.BuildingData)
	{
		ByID.Add(B.InstanceID,B); JobCapacity+=B.BuildingData->Workers;
		if (B.BuildingData->Type==EBuildingType::House||B.BuildingData->Type==EBuildingType::Apartment) {HousingCapacity+=B.BuildingData->Residents; if (HomeID==INDEX_NONE) HomeID=B.InstanceID;}
		if (B.BuildingData->PowerProductionUnitsPerTurn>0) PowerIDs.Add(B.InstanceID);
		if (B.BuildingData->Type==EBuildingType::Farm||B.BuildingData->Type==EBuildingType::LivestockFarm||B.BuildingData->Type==EBuildingType::WaterPlant) FoodWaterIDs.Add(B.InstanceID);
		if (B.BuildingData->Type==EBuildingType::Hospital) HospitalID=B.InstanceID;
	}
	Check(TEXT("residents_are328_not_residents_plus_jobs"),HousingCapacity==328&&Life->GetState().LivingPopulation==HousingCapacity&&Life->GetResidents().Num()==HousingCapacity,
		FString::Printf(TEXT("housing=%d living=%d ledger=%d jobCapacity=%d"),HousingCapacity,Life->GetState().LivingPopulation,Life->GetResidents().Num(),JobCapacity));
	TSet<int32> OriginalIDs; bool Unique=true;
	for (const FCityResidentRecord& R:Life->GetResidents()) {Unique&=R.ResidentID>0&&!OriginalIDs.Contains(R.ResidentID)&&ByID.Contains(R.HomeBuildingID); OriginalIDs.Add(R.ResidentID);}
	Check(TEXT("stable_resident_ids_unique_and_valid_homes"),Unique&&OriginalIDs.Num()==HousingCapacity,TEXT("Each resident is a single ledger record with one real home BuildingID."));
	Life->InitializeForCity(); Life->RefreshCity(); Buildings->SeedInitialSettlement(); bool SameIDs=Life->GetResidents().Num()==OriginalIDs.Num();
	for (const FCityResidentRecord& R:Life->GetResidents()) SameIDs&=OriginalIDs.Contains(R.ResidentID);
	Check(TEXT("repeat_initialize_refresh_seed_does_not_spawn_people"),SameIDs&&Life->GetState().LivingPopulation==HousingCapacity,TEXT("Public initialization is idempotent; jobs do not create residents."));
	const TMap<FIntPoint,int32> Components=AuditRoadComponents(*Terrain);
	const FIntPoint Steps[]={FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1)};
	const auto ComponentFor = [&](int32 ID)->int32
	{
		const FBuildingInstance* B=ByID.Find(ID); if (!B) return INDEX_NONE;
		for (FIntPoint Step:Steps) if (const int32* Component=Components.Find(B->GridCoords+Step)) return *Component;
		return INDEX_NONE;
	};
	bool JobsValid=true; int32 Assigned=0; TMap<int32,int32> Workers;
	for (const FCityResidentRecord& R:Life->GetResidents()) if (R.WorkBuildingID!=INDEX_NONE)
	{
		const FBuildingInstance* Work=ByID.Find(R.WorkBuildingID); const int32 HC=ComponentFor(R.HomeBuildingID),WC=ComponentFor(R.WorkBuildingID);
		JobsValid&=R.bAlive&&R.bWorkingAge&&Work&&Work->BuildingData&&HC!=INDEX_NONE&&HC==WC; ++Assigned; ++Workers.FindOrAdd(R.WorkBuildingID);
	}
	for (const auto& Pair:Workers) JobsValid&=ByID.Contains(Pair.Key)&&ByID[Pair.Key].BuildingData&&Pair.Value<=ByID[Pair.Key].BuildingData->Workers;
	Check(TEXT("jobs_use_living_workers_in_same_actual_road_component"),JobsValid&&Assigned>0&&Assigned==Life->GetState().EmployedResidents,TEXT("Independent canonical-road reachability and actual job-capacity checks; no new network is authored."));
	Check(TEXT("startup_power_supply_covers_connected_demand"),Life->GetState().PowerDemand>0&&Life->GetState().PowerCoverage01>.95f&&Life->GetState().PowerSupply>=Life->GetState().PowerDemand,
		FString::Printf(TEXT("supply=%.2f demand=%.2f coverage=%.3f"),Life->GetState().PowerSupply,Life->GetState().PowerDemand,Life->GetState().PowerCoverage01));
	bool WaterOperating=false,FoodOperating=false,BakeryOperating=false;
	for (const FBuildingInstance& B:Records) if (B.BuildingData)
	{
		if (B.BuildingData->Type==EBuildingType::WaterPlant) WaterOperating|=Life->GetBuildingOperation01(B.InstanceID)>.5f;
		if (B.BuildingData->Type==EBuildingType::Farm) FoodOperating|=Life->GetBuildingOperation01(B.InstanceID)>.5f;
		if (B.BuildingData->Type==EBuildingType::Bakery) BakeryOperating|=Life->GetBuildingOperation01(B.InstanceID)>.5f;
	}
	Check(TEXT("startup_water_and_food_chain_operate"),WaterOperating&&FoodOperating&&BakeryOperating,TEXT("Real connected, staffed and powered water plant, farm and bakery exist."));
	for (int32 I=0;I<2;++I)
	{
		Economy->PendingStateAidTL=0; const int64 Before=Economy->TreasuryTL; const int32 BeforeTurn=Economy->CurrentTurn; Economy->AdvanceTurn(); const FCityLifeState& S=Life->GetState();
		const int64 Expected=S.ProjectedTaxIncomeTL+S.LastTransitIncomeTL-S.ProjectedMaintenanceTL-S.LastSupplyExpenseTL;
		Check(I==0?TEXT("first_real_turn_money_matches_canonical_ledger"):TEXT("second_real_turn_money_matches_canonical_ledger"),Economy->CurrentTurn==BeforeTurn+1&&Economy->TreasuryTL-Before==Expected&&S.LastProcessedTurn==Economy->CurrentTurn,
			FString::Printf(TEXT("turn=%d delta=%lld tax=%lld fare=%lld maintenance=%lld paidSupply=%lld expected=%lld"),Economy->CurrentTurn,Economy->TreasuryTL-Before,S.ProjectedTaxIncomeTL,S.LastTransitIncomeTL,S.ProjectedMaintenanceTL,S.LastSupplyExpenseTL,Expected));
	}
	const FCityLifeState BeforeDuplicate=Life->GetState(); const int64 CashBeforeDuplicate=Economy->TreasuryTL; Life->AdvanceCityTurn(BeforeDuplicate.LastProcessedTurn); Life->AdvanceCityTurn(BeforeDuplicate.LastProcessedTurn-1);
	Check(TEXT("duplicate_or_old_life_turn_is_noop"),Economy->TreasuryTL==CashBeforeDuplicate&&Life->GetState().FoodStock==BeforeDuplicate.FoodStock&&Life->GetState().WaterStock==BeforeDuplicate.WaterStock&&Life->GetState().TotalDeaths==BeforeDuplicate.TotalDeaths,TEXT("Replaying an already processed or older turn cannot consume stocks, charge or kill twice."));
	Check(TEXT("zero_tax_rates_remove_only_tax_income"),Life->SetTaxRates(0.f,0.f)&&Life->GetState().ProjectedTaxIncomeTL==0,TEXT("Real public tax policy changes projected income; no finance formula is duplicated here."));
	Check(TEXT("tax_rates_clamp_to_thirty_percent"),Life->SetTaxRates(10.f,10.f)&&FMath::IsNearlyEqual(Life->GetState().ResidentialTaxRate,.30f)&&FMath::IsNearlyEqual(Life->GetState().BusinessTaxRate,.30f),TEXT("Out-of-range finite requests are clamped by the canonical policy API."));
	const int64 HighTax=Life->GetState().ProjectedTaxIncomeTL; const float NaN=std::numeric_limits<float>::quiet_NaN();
	Check(TEXT("nonfinite_tax_policy_is_rejected_without_mutation"),!Life->SetTaxRates(NaN,.1f)&&Life->GetState().ProjectedTaxIncomeTL==HighTax&&FMath::IsNearlyEqual(Life->GetState().ResidentialTaxRate,.30f),TEXT("NaN does not contaminate taxes or the current policy."));
	Life->SetTaxRates(.10f,.12f);
	const int32 ResidentID=Life->GetResidents().IsEmpty()?INDEX_NONE:Life->GetResidents()[0].ResidentID;
	int32 ServiceID=Life->FindServiceDestination(ResidentID,EBuildingType::Bakery); if (ServiceID==INDEX_NONE) ServiceID=Life->FindServiceDestination(ResidentID,EBuildingType::Hospital);
	FBuildingInstance Service; const bool HasService=Buildings->GetBuildingByID(ServiceID,Service);
	const FVector Arrival=HasService?Terrain->GridToWorld(Service.GridCoords):FVector::ZeroVector;
	Check(TEXT("remote_or_invalid_service_arrival_does_not_apply_benefit"),HasService&&!Life->ReportServiceVisit(ResidentID,ServiceID,Arrival+FVector(Terrain->CellSize*10,0,0))&&!Life->ReportServiceVisit(INDEX_NONE,ServiceID,Arrival),TEXT("Actual distance and living identity validation reject remote or invalid arrivals."));
	const bool Visit=HasService&&Life->ReportServiceVisit(ResidentID,ServiceID,Arrival); const float StockAfterVisit=Life->GetState().FoodStock;
	Check(TEXT("actual_service_visit_applies_once_per_turn"),Visit&&!Life->ReportServiceVisit(ResidentID,ServiceID,Arrival)&&Life->GetState().FoodStock==StockAfterVisit,TEXT("A valid on-site canonical service visit succeeds; same resident/type/turn repeat is rejected."));
	// This deliberately tests the fare ledger API, not boarding or rendered vehicles.
	const int64 CashBeforeFare=Economy->TreasuryTL;
	Check(TEXT("fare_ledger_rejects_duplicate_trip_and_invalid_identity"),Life->RecordTransitFare(ResidentID,1000001)&&!Life->RecordTransitFare(ResidentID,1000001)&&!Life->RecordTransitFare(INDEX_NONE,1000002)&&!Life->RecordTransitFare(ResidentID,0)&&Economy->TreasuryTL==CashBeforeFare,TEXT("Explicit isolated positive TripID fixture; fare is pending until the economy turn, and cannot be recorded twice. Boarding is outside this audit."));
	Economy->PendingStateAidTL=0; Economy->AdvanceTurn();
	Check(TEXT("pending_fare_is_transferred_exactly_once_on_turn"),Life->GetState().LastTransitIncomeTL==5&&Economy->TreasuryTL-CashBeforeFare==Life->GetState().ProjectedTaxIncomeTL+5-Life->GetState().ProjectedMaintenanceTL-Life->GetState().LastSupplyExpenseTL,TEXT("One accepted fare contributes5TL on the real economy turn and has no immediate treasury mutation."));
	for (int32 ID:PowerIDs) Buildings->ApplyDamageToBuilding(ID,100.f); Life->RefreshCity();
	Check(TEXT("all_power_producers_collapsed_close_hospital"),!PowerIDs.IsEmpty()&&HospitalID!=INDEX_NONE&&Life->GetState().PowerSupply<=.001f&&Life->GetState().PowerCoverage01<=.001f&&Life->GetBuildingOperation01(HospitalID)<=.001f,TEXT("Actual controlled100damage fixture collapses all current electricity producers, then canonical services lose power."));
	bool RepairSucceeded=true;
	for (int32 ID:PowerIDs) {Buildings->ApplyDamageToBuilding(ID,75.f); RepairSucceeded&=Economy->RepairWithBudget(ID);}
	Life->RefreshCity();
	Check(TEXT("real_paid_repair_of_severe_power_fixture_restores_service"),RepairSucceeded&&Life->GetState().PowerCoverage01>.95f&&Life->GetBuildingOperation01(HospitalID)>.1f,TEXT("Explicit controlled fixture changes damage100→75 to allow the actual severe-building repair API; no claim that collapsed structures are directly repairable."));
	const int32 Occupants=Life->GetLivingOccupants(HomeID,false),PopulationBeforeDeaths=Life->GetState().LivingPopulation,DeathsBefore=Life->GetState().TotalDeaths;
	const int32 Killed=Life->CommitFatalities(HomeID,MAX_int32,false); const int32 Repeated=Life->CommitFatalities(HomeID,MAX_int32,false); Life->RefreshCity();
	Check(TEXT("fatalities_consume_same_residents_once"),HomeID!=INDEX_NONE&&Occupants>0&&Killed==Occupants&&Repeated==0&&Life->GetState().LivingPopulation==PopulationBeforeDeaths-Killed&&Life->GetState().TotalDeaths==DeathsBefore+Killed,TEXT("A second full-capacity fatality request cannot count the already dead people again."));
	TSet<int32> DeadIDs; for (const FCityResidentRecord& R:Life->GetResidents()) if (!R.bAlive) DeadIDs.Add(R.ResidentID);
	Buildings->ApplyDamageToBuilding(HomeID,30.f); const bool HomeRepaired=Economy->RepairWithBudget(HomeID); Life->InitializeForCity(); Life->RefreshCity();
	bool DeadStayDead=true; for (int32 ID:DeadIDs) {FCityResidentRecord R; DeadStayDead&=Life->GetResident(ID,R)&&!R.bAlive;}
	Check(TEXT("repair_and_refresh_never_respawn_dead_residents"),HomeRepaired&&DeadStayDead&&Life->GetResidents().Num()==HousingCapacity&&Life->GetState().LivingPopulation==PopulationBeforeDeaths-Killed,TEXT("Repair restores the structure and keeps persistent dead ledger entries; no synthetic replacement population is added."));
	TSet<FIntPoint> RoadCuts;
	for (int32 ID:FoodWaterIDs) if (const FBuildingInstance* B=ByID.Find(ID)) for (FIntPoint Step:Steps) if (Roads->HasRoad(B->GridCoords+Step)) RoadCuts.Add(B->GridCoords+Step);
	bool CutsApplied=!RoadCuts.IsEmpty(); for (FIntPoint Coord:RoadCuts) CutsApplied&=Roads->RemoveRoadAt(Coord); Life->RefreshCity();
	bool Disconnected=true; for (int32 ID:FoodWaterIDs) Disconnected&=Life->GetBuildingOperation01(ID)<=.001f;
	Check(TEXT("real_road_removal_disconnects_food_and_water_producers"),CutsApplied&&Disconnected,TEXT("Only actual RemoveRoadAt transactions disconnect every farm/livestock/water-plant frontage; no operation flag or stock is overwritten."));
	float HungerBefore=0,HealthBefore=0; for (const FCityResidentRecord& R:Life->GetResidents()) if (R.bAlive) {HungerBefore+=R.Hunger01; HealthBefore+=R.Health01;}
	Economy->PublicSatisfaction=100.f; const int32 ShortageStartTurn=Economy->CurrentTurn;
	// Observe real deterioration before the existing needs-death threshold.
	// Continue into that threshold afterward; no need, stock, pending grant or
	// game-over state is reset between these ten ordinary turn requests.
	for (int32 I=0;I<8;++I)Economy->AdvanceTurn();
	float HungerAfter=0,HealthAfter=0; for (const FCityResidentRecord& R:Life->GetResidents()) if (R.bAlive) {HungerAfter+=R.Hunger01; HealthAfter+=R.Health01;}
	Check(TEXT("finite_reserves_eventually_expose_food_and_water_shortage"),Life->GetState().FoodCoverage01<.95f&&Life->GetState().WaterCoverage01<.95f,TEXT("Eight real economy turns consume finite supplies after the explicit producer disconnection; no infinite reserve or free import assertion."));
	Check(TEXT("shortage_changes_actual_resident_hunger_and_health"),Economy->CurrentTurn==ShortageStartTurn+8&&!Economy->bIsGameOver&&Life->GetState().LivingPopulation>0&&HungerAfter>HungerBefore&&HealthAfter<HealthBefore,FString::Printf(TEXT("pre-fatal aggregate hunger %.3f→%.3f health %.3f→%.3f living=%d"),HungerBefore,HungerAfter,HealthBefore,HealthAfter,Life->GetState().LivingPopulation));
	auto* Ledger=World->GetSubsystem<UCasualtyLedgerGenerator>();FAftershockCasualtyLedgerSaveState BeforeNeedsLedger,AfterNeedsLedger;Ledger->CaptureSaveState(BeforeNeedsLedger);
	const int32 LivingBeforeNeedsDeath=Life->GetState().LivingPopulation,DeathsBeforeNeeds=Life->GetState().TotalDeaths;const int64 BalanceBeforeNeeds=Economy->TreasuryTL,GrantBeforeNeeds=Economy->PendingStateAidTL;
	Economy->AdvanceTurn();Ledger->CaptureSaveState(AfterNeedsLedger);
	int32 RecordedNeedsDeaths=0;int64 RecordedDeathCost=0,RecordedSurvivorAid=0;TSet<FGuid> ExistingEvents;for(const auto& Event:BeforeNeedsLedger.Events)ExistingEvents.Add(Event.EventID);
	for(const auto& Event:AfterNeedsLedger.Events)if(!ExistingEvents.Contains(Event.EventID)){RecordedNeedsDeaths+=Event.ActualDeaths;RecordedDeathCost+=Event.DeathCostTL;RecordedSurvivorAid+=Event.SurvivorAidTL;}
	const int32 ActualNeedsDeaths=Life->GetState().TotalDeaths-DeathsBeforeNeeds;
	Check(TEXT("sustained_shortage_records_actual_needs_deaths_cost_and_no_survivor_grant"),ActualNeedsDeaths==LivingBeforeNeedsDeath&&ActualNeedsDeaths>0&&Life->GetState().LivingPopulation==0&&RecordedNeedsDeaths==ActualNeedsDeaths&&RecordedDeathCost==static_cast<int64>(ActualNeedsDeaths)*500&&RecordedSurvivorAid==0&&Economy->PendingStateAidTL==GrantBeforeNeeds&&BalanceBeforeNeeds-Economy->TreasuryTL==RecordedDeathCost+Life->GetState().LastSupplyExpenseTL,TEXT("The existing ninth shortage turn kills canonical residents and settles their ledger cost once; deaths create no aid and dismissal finalizes before turn tax/maintenance."));
	const int32 DismissalTurn=Economy->CurrentTurn;const int64 DismissalBalance=Economy->TreasuryTL;Economy->AdvanceTurn();FAftershockCasualtyLedgerSaveState AfterBlockedTurn;Ledger->CaptureSaveState(AfterBlockedTurn);
	Check(TEXT("ten_turn_shortage_requests_preserve_needs_death_and_gameover_guard"),Economy->bIsGameOver&&DismissalTurn==ShortageStartTurn+9&&Economy->CurrentTurn==DismissalTurn&&Economy->TreasuryTL==DismissalBalance&&Life->GetState().TotalDeaths==DeathsBeforeNeeds+ActualNeedsDeaths&&AfterBlockedTurn.Events.Num()==AfterNeedsLedger.Events.Num(),TEXT("The tenth request is correctly rejected after real public dismissal. No game-over bypass, repeated death cost or hidden fixture state reset."));
	return Finish();
#else
	return 1;
#endif
}
