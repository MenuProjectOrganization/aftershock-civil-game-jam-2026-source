#include "AftershockAutomaticRoadAccessAuditCommandlet.h"

#if WITH_EDITOR
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

UAftershockAutomaticRoadAccessAuditCommandlet::UAftershockAutomaticRoadAccessAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}

void UAftershockAutomaticRoadAccessAuditCommandlet::ObserveBootstrapEconomy(int64 TreasuryTL,float Satisfaction,int32 CurrentTurn)
{
	++BootstrapEconomyNotifications;
	UWorld* World=AuditWorld.Get();
	auto* Buildings=World?World->GetSubsystem<UBuildingManagerSubsystem>():nullptr;
	auto* Life=World?World->GetSubsystem<UCityLifeSubsystem>():nullptr;
	auto* Economy=World?World->GetSubsystem<UEconomyAndStateSubsystem>():nullptr;
	FBuildingInstance Building;
	bEconomyObservedPaidStructure&=Buildings&&Life&&Economy
		&&TreasuryTL==ExpectedBootstrapTreasury&&Economy->TreasuryTL==ExpectedBootstrapTreasury
		&&Buildings->GetBuildingAtCoords(BootstrapPlot,Building)
		&&Building.BuildingData==AuditHouse.Get()&&BootstrapLifeNotifications==0
		&&Life->GetResidents().IsEmpty();
	// Set the counter before attempting reentry so a broken guard cannot recurse.
	if(BootstrapEconomyNotifications==1&&Economy&&AuditHouse.IsValid())
		bEconomyReentryRejected=!Economy->BuildBuildingWithBudget(FIntPoint(14,10),AuditHouse.Get(),EFoundationQuality::Medium,1);
	else bEconomyReentryRejected=false;
}

void UAftershockAutomaticRoadAccessAuditCommandlet::ObserveBootstrapLife()
{
	++BootstrapLifeNotifications;
	UWorld* World=AuditWorld.Get();
	auto* Buildings=World?World->GetSubsystem<UBuildingManagerSubsystem>():nullptr;
	auto* Life=World?World->GetSubsystem<UCityLifeSubsystem>():nullptr;
	auto* Economy=World?World->GetSubsystem<UEconomyAndStateSubsystem>():nullptr;
	FBuildingInstance Building;
	const bool bCanonical=Buildings&&Life&&Economy&&Economy->TreasuryTL==ExpectedBootstrapTreasury
		&&Buildings->GetBuildingAtCoords(BootstrapPlot,Building)&&Building.BuildingData==AuditHouse.Get()
		&&BootstrapEconomyNotifications==1&&Life->GetState().LivingPopulation==ExpectedBootstrapPopulation
		&&Life->GetResidents().Num()==ExpectedBootstrapPopulation;
	bLifeObservedPaidCanonicalRoster&=bCanonical;
	if(bCanonical)
	{
		TSet<int32> IDs;
		for(const FCityResidentRecord& Resident:Life->GetResidents())
		{
			bLifeObservedPaidCanonicalRoster&=Resident.bAlive&&Resident.HomeBuildingID==Building.InstanceID
				&&Resident.ResidentID>0&&!IDs.Contains(Resident.ResidentID);
			IDs.Add(Resident.ResidentID);
		}
	}
	if(BootstrapLifeNotifications==1&&Economy&&AuditHouse.IsValid())
		bLifeReentryRejected=!Economy->BuildBuildingWithBudget(FIntPoint(15,10),AuditHouse.Get(),EFoundationQuality::Medium,1);
	else bLifeReentryRejected=false;
}

int32 UAftershockAutomaticRoadAccessAuditCommandlet::Main(const FString& Params)
{
	if(!GEngine)return 1;
	UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("AftershockAutomaticAccessAudit")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	if(!World)return 1;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();
	auto* Buildings=World->GetSubsystem<UBuildingManagerSubsystem>();
	auto* Roads=World->GetSubsystem<URoadNetworkSubsystem>();
	auto* Economy=World->GetSubsystem<UEconomyAndStateSubsystem>();
	auto* Life=World->GetSubsystem<UCityLifeSubsystem>();
	int32 Passed=0,Failed=0;TArray<FString> Results;
	const auto Check=[&](const TCHAR* Name,bool Value)
	{
		Value?++Passed:++Failed;
		Results.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"),Name,Value?TEXT("true"):TEXT("false")));
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_AUTO_ACCESS_CHECK case=%s passed=%d"),Name,Value);
	};
	Check(TEXT("canonical_subsystems"),Terrain&&Buildings&&Roads&&Economy&&Life);
	if(Terrain&&Buildings&&Roads&&Economy&&Life)
	{
		Terrain->GenerateProceduralTerrain(18,14,1000.f,1337);
		const auto ResetLand=[&]()
		{
			for(int32 Y=0;Y<14;++Y)for(int32 X=0;X<18;++X)
			{
				const FIntPoint Coord(X,Y);FGridCell Cell;Cell.Coords=Coord;Cell.bIsPlayable=true;Cell.Elevation=100.f;
				// Keep committed plots for later transactions, but reset all hazard
				// fixture flags. Canonical buildings are never duplicated by reset.
				FBuildingInstance Existing;if(Buildings->GetBuildingAtCoords(Coord,Existing))Cell.OccupyingBuildingID=Existing.InstanceID;
				Cell.bHasRoad=Y==2;Terrain->SetCell(Coord,Cell);
			}
		};
		ResetLand();Economy->TreasuryTL=100000;Economy->bIsGameOver=false;
		UBuildingData* Data=NewObject<UBuildingData>(Buildings);
		Data->Type=EBuildingType::FireStation;Data->DisplayName=FText::FromString(TEXT("Audit Fire Station"));
		Data->CostTL=1000;Data->FloorsMin=1;Data->FloorsMax=2;Data->Residents=0;Data->Workers=0;
		Data->MeshIntact=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
		const FIntPoint Plot(7,7);
		const int64 OriginalMoney=Economy->TreasuryTL;const uint64 Revision=Terrain->GetTerrainRevision();
		const auto Quote=Buildings->QuoteAutomaticRoadAccess(Plot,Data);
		Check(TEXT("quote_reaches_nearest_existing_road"),Quote.bRouteValid&&Quote.NewRoadCells==4&&Quote.RoadPath.Num()==5&&Quote.RoadPath.Last()==FIntPoint(7,2));
		Check(TEXT("quote_never_writes_money_roads_or_revision"),Economy->TreasuryTL==OriginalMoney&&Roads->CountRoadCells()==18&&Terrain->GetTerrainRevision()==Revision&&Terrain->GetCell(Plot).OccupyingBuildingID==INDEX_NONE);
		const auto Again=Buildings->QuoteAutomaticRoadAccess(Plot,Data);
		Check(TEXT("same_world_quote_is_deterministic"),Again.bRouteValid&&Again.RoadPath==Quote.RoadPath&&Again.RoadCostTL==800);
		Economy->TreasuryTL=2099;const uint64 BeforeRejected=Terrain->GetTerrainRevision();
		Check(TEXT("aggregate_unaffordable_purchase_rejected"),!Economy->BuildBuildingWithBudget(Plot,Data,EFoundationQuality::Medium,1));
		Check(TEXT("unaffordable_purchase_has_no_road_building_or_charge"),Economy->TreasuryTL==2099&&Roads->CountRoadCells()==18&&Buildings->GetActiveBuildingsCount()==0&&Terrain->GetTerrainRevision()==BeforeRejected);
		Economy->TreasuryTL=2100;
		Check(TEXT("building_and_four_road_cells_commit_together"),Economy->BuildBuildingWithBudget(Plot,Data,EFoundationQuality::Medium,1));
		FBuildingInstance Built;
		Check(TEXT("exactly_one_aggregate_debit"),Economy->TreasuryTL==0&&Roads->CountRoadCells()==22&&Buildings->GetActiveBuildingsCount()==1&&Buildings->GetBuildingAtCoords(Plot,Built));
		Check(TEXT("building_plot_does_not_become_road"),!Terrain->GetCell(Plot).bHasRoad&&Terrain->GetCell(Plot).OccupyingBuildingID==Built.InstanceID);
		Check(TEXT("frontage_is_actually_accessible"),!Quote.RoadPath.IsEmpty()&&Roads->CanAccessRoadFromCell(Plot,Quote.RoadPath[0]));
		bool Contiguous=true;for(int32 I=1;I<Quote.RoadPath.Num();++I)Contiguous&=Roads->CanTraverseRoadEdge(Quote.RoadPath[I-1],Quote.RoadPath[I]);
		Check(TEXT("extension_connects_to_existing_road_without_gaps"),Contiguous);
		const int32 CountAfter=Roads->CountRoadCells();
		Check(TEXT("duplicate_construction_has_no_second_debit_or_extension"),!Economy->BuildBuildingWithBudget(Plot,Data,EFoundationQuality::Medium,1)&&Economy->TreasuryTL==0&&Roads->CountRoadCells()==CountAfter);
		const auto Direct=Buildings->QuoteAutomaticRoadAccess(FIntPoint(5,3),Data);
		Check(TEXT("adjacent_existing_road_has_zero_access_fee"),Direct.bRouteValid&&Direct.NewRoadCells==0&&Direct.RoadCostTL==0);
		// A full-width river must not be erased or silently converted to a bridge.
		ResetLand();for(int32 X=0;X<18;++X){FGridCell C=Terrain->GetCell(FIntPoint(X,5));C.bIsRiverbed=true;Terrain->SetCell(C.Coords,C);}
		const FIntPoint Isolated(12,9);const uint64 RiverRevision=Terrain->GetTerrainRevision();
		Check(TEXT("river_without_bridge_blocks_automatic_connection"),!Buildings->QuoteAutomaticRoadAccess(Isolated,Data).bRouteValid);
		Economy->TreasuryTL=100000;
		Check(TEXT("blocked_river_purchase_keeps_money_and_terrain"),!Economy->BuildBuildingWithBudget(Isolated,Data,EFoundationQuality::Medium,1)&&Economy->TreasuryTL==100000&&Terrain->GetTerrainRevision()==RiverRevision);
		ResetLand();
		// Occupancy/reservations/hazards must be routed around, never bulldozed.
		FGridCell Reserved=Terrain->GetCell(FIntPoint(12,8));Reserved.bIsEmergencyAssemblyArea=true;Terrain->SetCell(Reserved.Coords,Reserved);
		const auto Detour=Buildings->QuoteAutomaticRoadAccess(Isolated,Data);
		Check(TEXT("reserved_assembly_cell_is_not_used_by_access_road"),Detour.bRouteValid&&!Detour.RoadPath.Contains(Reserved.Coords));
		Reserved.bIsEmergencyAssemblyArea=false;Reserved.bBurning=true;Terrain->SetCell(Reserved.Coords,Reserved);
		Check(TEXT("burning_cell_is_not_used_by_access_road"),!Buildings->QuoteAutomaticRoadAccess(Isolated,Data).RoadPath.Contains(Reserved.Coords));
		Reserved.bBurning=false;Reserved.LavaCoverage01=.8f;Terrain->SetCell(Reserved.Coords,Reserved);
		Check(TEXT("lava_cell_is_not_used_by_access_road"),!Buildings->QuoteAutomaticRoadAccess(Isolated,Data).RoadPath.Contains(Reserved.Coords));
		Reserved.LavaCoverage01=0;Reserved.bIsOcean=true;Terrain->SetCell(Reserved.Coords,Reserved);
		Check(TEXT("ocean_cell_is_not_used_by_access_road"),!Buildings->QuoteAutomaticRoadAccess(Isolated,Data).RoadPath.Contains(Reserved.Coords));
		ResetLand();
		for(int32 X=0;X<18;++X){FGridCell C=Terrain->GetCell(FIntPoint(X,5));C.Elevation=10000.f;Terrain->SetCell(C.Coords,C);}
		Check(TEXT("impossible_road_grade_blocks_access"),!Buildings->QuoteAutomaticRoadAccess(Isolated,Data).bRouteValid);
		ResetLand();
		// Unlike the prior non-road wall, every reachable terminal is itself a
		// high existing road. Ordinary frontage permits it, but grade must not.
		for(int32 X=0;X<18;++X){FGridCell C=Terrain->GetCell(FIntPoint(X,2));C.Elevation=10000.f;Terrain->SetCell(C.Coords,C);}
		const FIntPoint CliffPlot(5,3),CliffRoad(5,2);
		Check(TEXT("terminal_cliff_fixture_is_frontage_but_not_a_valid_grade"),Roads->CanAccessRoadFromCell(CliffPlot,CliffRoad)&&!Roads->CanBuildAutomaticAccessEdge(CliffPlot,CliffRoad));
		Check(TEXT("existing_road_terminal_grade_is_enforced_for_adjacent_and_extended_access"),!Buildings->QuoteAutomaticRoadAccess(CliffPlot,Data).bRouteValid&&!Buildings->QuoteAutomaticRoadAccess(Isolated,Data).bRouteValid);
		const uint64 BeforeCliffRevision=Terrain->GetTerrainRevision();const int32 BeforeCliffBuildings=Buildings->GetActiveBuildingsCount();
		Check(TEXT("terminal_cliff_purchase_cannot_charge_or_mutate"),!Economy->BuildBuildingWithBudget(CliffPlot,Data,EFoundationQuality::Medium,1)&&Economy->TreasuryTL==100000&&Terrain->GetTerrainRevision()==BeforeCliffRevision&&Buildings->GetActiveBuildingsCount()==BeforeCliffBuildings&&Roads->CountRoadCells()==18);
		ResetLand();
		// A bridge interior beside a parcel is not a side entrance. The only road
		// in this enclosure is an elevated deck, so no alternative route exists.
		for(FGridCell& C:Terrain->Cells)C.bHasRoad=false;
		FGridCell Deck=Terrain->GetCell(FIntPoint(12,8));Deck.bHasRoad=true;Deck.BridgeID=77;Deck.BridgeAxis=1;Deck.RoadSurfaceElevationCm=100.f;Deck.bIsRiverbed=true;Terrain->SetCell(Deck.Coords,Deck);
		Check(TEXT("elevated_bridge_interior_is_not_building_frontage"),!Buildings->QuoteAutomaticRoadAccess(Isolated,Data).bRouteValid);
		ResetLand();for(FGridCell& C:Terrain->Cells)C.bHasRoad=false;
		Life->InitializeForCity();
		const uint64 BeforeBootstrapQuote=Terrain->GetTerrainRevision();
		const auto BootstrapQuote=Buildings->QuoteAutomaticRoadAccess(Isolated,Data);
		Check(TEXT("roadless_bootstrap_quote_is_usable_without_a_disconnected_stub"),BootstrapQuote.bRouteValid&&BootstrapQuote.RoadPath.IsEmpty()&&BootstrapQuote.NewRoadCells==0&&BootstrapQuote.RoadCostTL==0);
		Check(TEXT("roadless_bootstrap_explains_manual_first_road_without_mutation"),BootstrapQuote.Feedback.ToString().Contains(TEXT("elle"))&&Roads->CountRoadCells()==0&&Terrain->GetTerrainRevision()==BeforeBootstrapQuote&&Economy->TreasuryTL==100000);
		Check(TEXT("roadless_bootstrap_never_overrides_occupied_plot"),!Buildings->QuoteAutomaticRoadAccess(Plot,Data).bRouteValid);
		FGridCell InvalidPlot=Terrain->GetCell(Isolated);InvalidPlot.bBurning=true;Terrain->SetCell(Isolated,InvalidPlot);
		Check(TEXT("roadless_bootstrap_never_overrides_burning_plot"),!Buildings->QuoteAutomaticRoadAccess(Isolated,Data).bRouteValid&&!Economy->BuildBuildingWithBudget(Isolated,Data,EFoundationQuality::Medium,1)&&Economy->TreasuryTL==100000);
		InvalidPlot.bBurning=false;InvalidPlot.bIsOcean=true;Terrain->SetCell(Isolated,InvalidPlot);
		Check(TEXT("roadless_bootstrap_never_overrides_ocean_plot"),!Buildings->QuoteAutomaticRoadAccess(Isolated,Data).bRouteValid);
		InvalidPlot.bIsOcean=false;InvalidPlot.bIsRiverbed=true;Terrain->SetCell(Isolated,InvalidPlot);
		Check(TEXT("roadless_bootstrap_never_overrides_river_plot"),!Buildings->QuoteAutomaticRoadAccess(Isolated,Data).bRouteValid);
		InvalidPlot.bIsRiverbed=false;InvalidPlot.LavaCoverage01=.8f;Terrain->SetCell(Isolated,InvalidPlot);
		Check(TEXT("roadless_bootstrap_never_overrides_lava_plot"),!Buildings->QuoteAutomaticRoadAccess(Isolated,Data).bRouteValid);
		InvalidPlot.LavaCoverage01=0;Terrain->SetCell(Isolated,InvalidPlot);
		UBuildingData* House=NewObject<UBuildingData>(Buildings);
		House->Type=EBuildingType::House;House->DisplayName=FText::FromString(TEXT("Audit Bootstrap Home"));
		House->CostTL=1000;House->FloorsMin=1;House->FloorsMax=1;House->Residents=4;House->Workers=0;House->MeshIntact=Data->MeshIntact;
		const int32 HouseCost=Buildings->CalculateConstructionCost(House,EFoundationQuality::Medium,1);
		Economy->TreasuryTL=HouseCost-1;const uint64 BeforePoorBootstrap=Terrain->GetTerrainRevision();
		const int32 BeforeBootstrapBuildings=Buildings->GetActiveBuildingsCount();int32 RejectedID=123;
		Check(TEXT("roadless_insufficient_budget_rejected_without_any_ledger_change"),!Buildings->PlaceBuildingWithRoadAccess(Isolated,House,EFoundationQuality::Medium,1,RejectedID)&&RejectedID==INDEX_NONE&&Economy->TreasuryTL==HouseCost-1&&Terrain->GetTerrainRevision()==BeforePoorBootstrap&&Buildings->GetActiveBuildingsCount()==BeforeBootstrapBuildings&&Life->GetResidents().IsEmpty()&&Roads->CountRoadCells()==0);
		AuditWorld=World;AuditHouse=House;BootstrapPlot=Isolated;
		Economy->TreasuryTL=20000;ExpectedBootstrapTreasury=20000-HouseCost;ExpectedBootstrapPopulation=4;
		BootstrapEconomyNotifications=0;BootstrapLifeNotifications=0;
		bEconomyObservedPaidStructure=true;bLifeObservedPaidCanonicalRoster=true;bEconomyReentryRejected=true;bLifeReentryRejected=true;
		Economy->OnEconomyUpdated.AddDynamic(this,&UAftershockAutomaticRoadAccessAuditCommandlet::ObserveBootstrapEconomy);
		Life->OnCityLifeUpdated.AddDynamic(this,&UAftershockAutomaticRoadAccessAuditCommandlet::ObserveBootstrapLife);
		const bool bBootstrapBuilt=Economy->BuildBuildingWithBudget(Isolated,House,EFoundationQuality::Medium,1);
		Economy->OnEconomyUpdated.RemoveDynamic(this,&UAftershockAutomaticRoadAccessAuditCommandlet::ObserveBootstrapEconomy);
		Life->OnCityLifeUpdated.RemoveDynamic(this,&UAftershockAutomaticRoadAccessAuditCommandlet::ObserveBootstrapLife);
		FBuildingInstance BootstrapBuilt;
		Check(TEXT("roadless_paid_bootstrap_uses_normal_transaction_and_preserves_ids"),bBootstrapBuilt&&Buildings->GetBuildingAtCoords(Isolated,BootstrapBuilt)&&BootstrapBuilt.InstanceID==Built.InstanceID+1&&Buildings->GetActiveBuildingsCount()==BeforeBootstrapBuildings+1&&Economy->TreasuryTL==ExpectedBootstrapTreasury&&Roads->CountRoadCells()==0);
		Check(TEXT("bootstrap_economy_notification_observes_single_debit_and_committed_structure"),BootstrapEconomyNotifications==1&&bEconomyObservedPaidStructure);
		Check(TEXT("bootstrap_life_notification_observes_paid_canonical_population"),BootstrapLifeNotifications==1&&bLifeObservedPaidCanonicalRoster&&Life->GetState().LivingPopulation==4);
		Check(TEXT("bootstrap_economy_callback_cannot_reenter_purchase"),bEconomyReentryRejected&&BootstrapEconomyNotifications==1);
		Check(TEXT("bootstrap_life_callback_cannot_reenter_purchase"),bLifeReentryRejected&&BootstrapLifeNotifications==1);
		Check(TEXT("bootstrap_reentry_has_no_hidden_roster_or_parcel_side_effect"),Terrain->GetCell(FIntPoint(14,10)).OccupyingBuildingID==INDEX_NONE&&Terrain->GetCell(FIntPoint(15,10)).OccupyingBuildingID==INDEX_NONE&&Life->GetResidents().Num()==4&&Economy->TreasuryTL==ExpectedBootstrapTreasury);
		AuditWorld.Reset();AuditHouse.Reset();
		const int64 BeforeFree=Economy->TreasuryTL;int32 FreeID=INDEX_NONE;
		Check(TEXT("explicit_free_startup_placement_preserves_legacy_contract"),Buildings->PlaceBuilding(FIntPoint(3,9),Data,EFoundationQuality::Medium,1,FreeID)&&FreeID==BootstrapBuilt.InstanceID+1&&Economy->TreasuryTL==BeforeFree&&Roads->CountRoadCells()==0);
		ResetLand();Economy->bIsGameOver=true;
		Check(TEXT("game_over_rejects_combined_purchase"),!Economy->BuildBuildingWithBudget(Isolated,Data,EFoundationQuality::Medium,1));
	}
	const FString Directory=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("PresentationValidation"));
	IFileManager::Get().MakeDirectory(*Directory,true);
	const FString Json=FString::Printf(TEXT("{\n\"schema_version\":1,\n\"utc\":\"%s\",\n\"scope\":\"Isolated canonical road-access quotes and aggregate player construction. No GPU, user map, user save, terrain generation fidelity or animation acceptance.\",\n\"passed\":%d,\n\"failed\":%d,\n\"checks\":[%s]\n}\n"),*FDateTime::UtcNow().ToIso8601(),Passed,Failed,*FString::Join(Results,TEXT(",\n")));
	const bool Saved=FFileHelper::SaveStringToFile(Json,*FPaths::Combine(Directory,TEXT("automatic_road_access_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_AUTO_ACCESS_AUDIT result=%s passed=%d failed=%d receipt=%d"),Failed==0&&Saved?TEXT("PASS"):TEXT("FAIL"),Passed,Failed,Saved);
	World->DestroyWorld(false);GEngine->DestroyWorldContext(World);World->RemoveFromRoot();
	return Failed==0&&Saved?0:1;
}
#else
UAftershockAutomaticRoadAccessAuditCommandlet::UAftershockAutomaticRoadAccessAuditCommandlet() {}
void UAftershockAutomaticRoadAccessAuditCommandlet::ObserveBootstrapEconomy(int64 TreasuryTL,float Satisfaction,int32 CurrentTurn) {}
void UAftershockAutomaticRoadAccessAuditCommandlet::ObserveBootstrapLife() {}
int32 UAftershockAutomaticRoadAccessAuditCommandlet::Main(const FString& Params){return 1;}
#endif
