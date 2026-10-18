#include "AftershockCityScaleAuditCommandlet.h"
#if WITH_EDITOR
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Aftershock/Public/AftershockSaveGame.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

UAftershockCityScaleAuditCommandlet::UAftershockCityScaleAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}
int32 UAftershockCityScaleAuditCommandlet::Main(const FString&)
{
	int32 Passed=0,Failed=0;TArray<FString> Rows;
	const auto Check=[&](const TCHAR* Name,bool Good)
	{Good?++Passed:++Failed;Rows.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"),Name,Good?TEXT("true"):TEXT("false")));UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITY_SCALE_CHECK %s passed=%d"),Name,Good);};
	if(!GEngine)return 1;
	TStrongObjectPtr<UGameInstance> GI(NewObject<UGameInstance>(GEngine));GI->InitializeStandalone(TEXT("AftershockCityScaleFixture"));
	UWorld* W=GI->GetWorld();Check(TEXT("real_game_instance_world"),W!=nullptr);if(!W)return 1;
	auto* Session=GI->GetSubsystem<UAftershockOpeningSessionSubsystem>();auto* Saves=GI->GetSubsystem<UAftershockSaveSubsystem>();
	auto* Scenario=W->GetSubsystem<UAftershockScenarioSubsystem>();auto* Buildings=W->GetSubsystem<UBuildingManagerSubsystem>();auto* Terrain=W->GetSubsystem<UTerrainGridSubsystem>();
	auto* Life=W->GetSubsystem<UCityLifeSubsystem>();auto* Crowd=W->GetSubsystem<UCrowdRenderSubsystem>();auto* Roads=W->GetSubsystem<URoadNetworkSubsystem>();auto* Economy=W->GetSubsystem<UEconomyAndStateSubsystem>();
	FText Error;FAftershockSessionSettings Settings;
	Check(TEXT("new_campaign_uses_canonical_session"),Session->ConfigureNewSession(Settings,Error));
	const double Start=FPlatformTime::Seconds();Scenario->PrepareScenarioBeforeSettlement();Buildings->SeedInitialSettlement();Crowd->SynchronizeCanonicalPopulation();
	const double SetupSeconds=FPlatformTime::Seconds()-Start;
	// Keep all historical capacity assertions unchanged. The native runner
	// explicitly selects the authored V1 fixture with both audit flags; normal
	// V2 source/coastal windows have real land constraints and separate startup
	// counts in AftershockRealRegionSettlementAudit, not an 800-building promise.
	const bool bRealSource=Terrain->IsUsingRealGeography();
	const TCHAR* GeographyMode=bRealSource?TEXT("V2RealSource"):TEXT("V1LegacyAuthored");
	const TCHAR* Scope=bRealSource
		?TEXT("Configured V2 real-source city measured against unchanged authored-capacity regression thresholds; these thresholds are not a normal V2 startup guarantee. Actual regional capacities are audited separately. NullRHI; not GPU, UI input or photorealism proof.")
		:TEXT("Explicit authored V1 city-capacity regression with canonical buildings, population and disk save. All historical capacity assertions retained; not a claim that normal V2 real-source cities contain 800 buildings. NullRHI; not GPU, UI input or photorealism proof.");
	TArray<FBuildingInstance> Records;Buildings->GetAllBuildings(Records);
	Check(TEXT("new_city_has_125_by_125_ten_metre_cells"),Terrain->GridWidth==125&&Terrain->GridHeight==125&&FMath::IsNearlyEqual(Terrain->CellSize,1000.f));
	Check(TEXT("new_city_exceeds_eight_hundred_real_buildings"),Records.Num()>=800&&Records.Num()==Scenario->GetDesignedLots().Num());
	FBox Extent(ForceInit);TSet<int32> Types,HouseDistricts;int32 Connected=0,Power=0,Water=0,Food=0,Stops=0;
	const FIntPoint Steps[]={FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1)};
	for(const auto& B:Records)
	{
		Extent+=Terrain->GridToWorld(B.GridCoords);if(!B.BuildingData)continue;const auto Type=B.BuildingData->Type;Types.Add(static_cast<int32>(Type));
		for(FIntPoint Step:Steps)if(Roads->CanAccessRoadFromCell(B.GridCoords,B.GridCoords+Step)){++Connected;break;}
		if(Type==EBuildingType::House||Type==EBuildingType::Apartment)HouseDistricts.Add((B.GridCoords.X/25)+(B.GridCoords.Y/25)*5);
		Power+=Type==EBuildingType::PowerPlant||Type==EBuildingType::WindTurbine;Water+=Type==EBuildingType::WaterPlant;
		Food+=Type==EBuildingType::Farm||Type==EBuildingType::Bakery;Stops+=Type==EBuildingType::BusStop;
	}
	Check(TEXT("city_spans_multiple_hundred_metre_neighbourhoods"),Extent.IsValid&&Extent.GetSize().Y>=70000.f&&HouseDistricts.Num()>=10);
	Check(TEXT("every_authored_lot_has_actual_connected_road_frontage"),Connected==Records.Num()&&Roads->CountRoadCells()>500);
	Check(TEXT("city_services_repeated_with_population_not_one_token_facility"),Power>=12&&Water>=8&&Food>=12&&Stops>=8&&Types.Num()>=20);
	Check(TEXT("canonical_population_exceeds_draw_budget"),Life->GetState().LivingPopulation>1500&&Life->GetResidents().Num()==Life->GetState().LivingPopulation);
	Check(TEXT("crowd_retains_bounded_draw_population"),Crowd->GetCitizenDebugRoster().Num()==1500);
	TSet<int32> IDs;bool Actual=true;for(const auto& Agent:Crowd->GetCitizenDebugRoster()){FCityResidentRecord Person;Actual&=!IDs.Contains(Agent.ResidentID)&&Life->GetResident(Agent.ResidentID,Person)&&Person.bAlive;IDs.Add(Agent.ResidentID);}
	Check(TEXT("all_drawn_people_are_unique_actual_citizens"),Actual);
	const int32 InitialPopulation=Life->GetState().LivingPopulation;
	for(int32 Turn=0;Turn<3;++Turn)Economy->AdvanceTurn();
	Check(TEXT("large_city_three_quiet_turns_keep_people_fed_and_employed"),Life->GetState().LivingPopulation==InitialPopulation&&Life->GetState().TotalDeaths==0&&Life->GetState().EmployedResidents>0&&Life->GetState().FoodCoverage01>=.9f&&Life->GetState().WaterCoverage01>=.9f&&Life->GetState().PowerCoverage01>=.5f&&!Economy->bIsGameOver);
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITY_SCALE_SERVICES turn=%d employed=%d foodCoverage=%.4f waterCoverage=%.4f powerCoverage=%.4f treasuryTL=%lld satisfaction=%.3f"),Economy->CurrentTurn,Life->GetState().EmployedResidents,Life->GetState().FoodCoverage01,Life->GetState().WaterCoverage01,Life->GetState().PowerCoverage01,Economy->TreasuryTL,Economy->PublicSatisfaction);
	int32 Outdoor=0,PeopleOverlaps=0,VehicleIntersections=0;float MinimumDistanceCm=0;
	Crowd->GetPhysicalClearance(Outdoor,PeopleOverlaps,VehicleIntersections,MinimumDistanceCm);
	Check(TEXT("canonical_city_publishes_no_overlapping_outdoor_bodies"),PeopleOverlaps==0&&VehicleIntersections==0);
	TStrongObjectPtr<UAftershockSaveGame> Snapshot(Saves->CaptureWorld(W,Error));Check(TEXT("large_city_complete_snapshot_valid"),Snapshot.IsValid());
	int64 DiskBytes=0;
	if(Snapshot.IsValid())
	{
		// Only the reserved Audit slot is used, and its previous bytes are restored.
		const FString Path=UAftershockSaveSubsystem::SlotPath(TEXT("Audit"));TMap<FString,TArray<uint8>> Previous;
		const TArray<FString> Paths={Path,Path+TEXT(".bak"),Path+TEXT(".tmp"),Path+TEXT(".bak.tmp")};
		for(const FString& File:Paths)if(IFileManager::Get().FileExists(*File)){TArray<uint8> Bytes;if(FFileHelper::LoadFileToArray(Bytes,*File))Previous.Add(File,MoveTemp(Bytes));}
		Check(TEXT("large_city_compressed_checksum_atomic_write"),Saves->WriteSnapshot(Snapshot.Get(),TEXT("Audit"),Error));DiskBytes=IFileManager::Get().FileSize(*Path);
		TStrongObjectPtr<UAftershockSaveGame> Loaded(Saves->ReadSnapshot(TEXT("Audit"),Error));
		Check(TEXT("large_city_disk_round_trip_keeps_population_and_lots"),Loaded.IsValid()&&Loaded->CityLife.Residents.Num()==Snapshot->CityLife.Residents.Num()&&Loaded->Buildings.Buildings.Num()==Records.Num()&&Loaded->Terrain.Cells.Num()==15625);
		Check(TEXT("compressed_large_city_remains_within_disk_limit"),DiskBytes>0&&DiskBytes<64ll*1024*1024);
		const int64 BeforeMoney=Economy->TreasuryTL;Check(TEXT("large_city_bulk_restore_does_not_replay_money"),Loaded.IsValid()&&Saves->RestoreWorld(W,Loaded.Get(),Error)&&Economy->TreasuryTL==BeforeMoney&&Life->GetState().LivingPopulation==Snapshot->CityLife.State.LivingPopulation);
		for(const FString& File:Paths)if(const auto* Bytes=Previous.Find(File))FFileHelper::SaveArrayToFile(*Bytes,*File);else IFileManager::Get().Delete(*File,false,true);
	}
	const FString Folder=FPaths::ProjectSavedDir()/TEXT("PresentationValidation");IFileManager::Get().MakeDirectory(*Folder,true);
	const FString Json=FString::Printf(TEXT("{\"passed\":%d,\"failed\":%d,\"geography_mode\":\"%s\",\"buildings\":%d,\"living_population\":%d,\"draw_agents\":%d,\"road_cells\":%d,\"setup_seconds\":%.4f,\"save_bytes\":%lld,\"scope\":\"%s\",\"checks\":[%s]}"),Passed,Failed,GeographyMode,Records.Num(),Life->GetState().LivingPopulation,Crowd->GetCitizenDebugRoster().Num(),Roads->CountRoadCells(),SetupSeconds,DiskBytes,Scope,*FString::Join(Rows,TEXT(",")));
	FFileHelper::SaveStringToFile(Json,*(Folder/TEXT("city_scale_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITY_SCALE_RESULT passed=%d failed=%d geographyMode=%s buildings=%d living=%d agents=%d setupSeconds=%.4f saveBytes=%lld"),Passed,Failed,GeographyMode,Records.Num(),Life->GetState().LivingPopulation,Crowd->GetCitizenDebugRoster().Num(),SetupSeconds,DiskBytes);
	GI->Shutdown();W->DestroyWorld(false);GEngine->DestroyWorldContext(W);W->RemoveFromRoot();return Failed?1:0;
}
#else
UAftershockCityScaleAuditCommandlet::UAftershockCityScaleAuditCommandlet(){}
int32 UAftershockCityScaleAuditCommandlet::Main(const FString&){return 1;}
#endif
