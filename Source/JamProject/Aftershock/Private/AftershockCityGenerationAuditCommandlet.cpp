#include "AftershockCityGenerationAuditCommandlet.h"

#if WITH_EDITOR
#include "AftershockSettlementPlanner.h"
#include "AftershockSettlementMorphology.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	struct FGenerationAuditWorld
	{
		UWorld* World=nullptr;
		~FGenerationAuditWorld(){if(World){World->DestroyWorld(false);if(GEngine)GEngine->DestroyWorldContext(World);World->RemoveFromRoot();}}
	};
	bool EqualLots(const TArray<FAftershockScenarioLot>& A,const TArray<FAftershockScenarioLot>& B)
	{
		if(A.Num()!=B.Num())return false;
		for(int32 I=0;I<A.Num();++I)if(A[I].Coord!=B[I].Coord||A[I].Type!=B[I].Type||A[I].Floors!=B[I].Floors||A[I].Foundation!=B[I].Foundation)return false;
		return true;
	}
	TArray<FAftershockScenarioLot> EssentialFixture(UBuildingManagerSubsystem& Buildings)
	{
		TArray<EBuildingType> Types={EBuildingType::Hospital,EBuildingType::School,EBuildingType::FireStation};
		for(int32 I=0;I<12;++I)Types.Add(EBuildingType::House);
		for(int32 I=0;I<8;++I)Types.Add(EBuildingType::Apartment);
		for(int32 I=0;I<3;++I)Types.Add(EBuildingType::Office);
		Types.Append({EBuildingType::Factory,EBuildingType::GreenSpace,EBuildingType::Farm,EBuildingType::LivestockFarm,
			EBuildingType::Bakery,EBuildingType::Restaurant,EBuildingType::PowerPlant,EBuildingType::Generator,
			EBuildingType::TaxOffice,EBuildingType::Courthouse,EBuildingType::PoliceStation,EBuildingType::BusTerminal,
			EBuildingType::BusStop,EBuildingType::BusStop,EBuildingType::Shelter,EBuildingType::WaterPlant,EBuildingType::Warehouse,EBuildingType::Substation});
		TArray<FAftershockScenarioLot> Result;
		for(int32 I=0;I<Types.Num();++I)
		{
			const UBuildingData* D=Buildings.GetCatalogData(Types[I]);if(!D)continue;
			auto& L=Result.AddDefaulted_GetRef();L.Coord=FIntPoint(12+(I%8)*2,18+(I/8)*2);L.Type=Types[I];
			L.Floors=D->FloorsMin;L.Foundation=I<3?EFoundationQuality::Strong:EFoundationQuality::Medium;
		}
		return Result;
	}
}

UAftershockCityGenerationAuditCommandlet::UAftershockCityGenerationAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}

int32 UAftershockCityGenerationAuditCommandlet::Main(const FString& Params)
{
	int32 Passed=0,Failed=0;TArray<FString> Checks;TArray<FString> Metrics;
	const auto Check=[&](const TCHAR* Name,bool bPass)
	{
		bPass?++Passed:++Failed;
		Checks.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"),Name,bPass?TEXT("true"):TEXT("false")));
		if(bPass){UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITY_GENERATION_AUDIT_PASS %s"),Name);}
		else{UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_CITY_GENERATION_AUDIT_FAIL %s"),Name);}
	};
	const auto Finish=[&]()
	{
		const FString Folder=FPaths::ProjectSavedDir()/TEXT("PresentationValidation");IFileManager::Get().MakeDirectory(*Folder,true);
		const FString Path=Folder/TEXT("city_generation_audit.json");
		const FString JSON=FString::Printf(TEXT("{\"schema_version\":1,\"scope\":\"Actual bounded production planner on explicit synthetic canonical terrain; repeat/seed/region variation, safety and work counters. Actual building and resident save-owner roundtrip on a bounded96-record subset. No real-source geography, complete production startup-road integration, rendered image, GPU timing, disk restart or whole-game performance claim.\",\"passed\":%d,\"failed\":%d,\"metrics\":[%s],\"checks\":[%s]}"),Passed,Failed,*FString::Join(Metrics,TEXT(",")),*FString::Join(Checks,TEXT(",")));
		if(!FFileHelper::SaveStringToFile(JSON,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return 2;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITY_GENERATION_AUDIT_RESULT passed=%d failed=%d report=%s"),Passed,Failed,*Path);
		return Failed==0?0:1;
	};
	Check(TEXT("engine_initialized"),GEngine!=nullptr);if(!GEngine)return Finish();
	FGenerationAuditWorld Fixture;
	Fixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("CityGenerationFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("isolated_game_world_created"),Fixture.World!=nullptr);if(!Fixture.World)return Finish();
	UWorld* World=Fixture.World;GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();auto* Buildings=World->GetSubsystem<UBuildingManagerSubsystem>();
	auto* Life=World->GetSubsystem<UCityLifeSubsystem>();auto* Economy=World->GetSubsystem<UEconomyAndStateSubsystem>();
	Check(TEXT("canonical_owners_ready"),Terrain&&Buildings&&Life&&Economy);if(!Terrain||!Buildings||!Life||!Economy)return Finish();
	Terrain->GenerateProceduralTerrain(125,125,1000.f,99173);
	TSet<FIntPoint> Land;
	for(FGridCell& C:Terrain->Cells)
	{
		// Synthetic fixture, deliberately not claimed to be sampled geography.
		C.bIsPlayable=true;C.bIsOcean=false;C.bIsRiverbed=C.Coords.X==64;C.bVolcanicVent=C.Coords==FIntPoint(41,40);
		C.Elevation=100.f+C.Coords.Y*2.f;C.Slope=C.Coords.X<22?3.f:8.f;
		C.ForestFuel01=.15f;C.bAgriculturalLand=C.Coords.X<18;C.ParcelSuitability=.85f;
		C.OccupyingBuildingID=INDEX_NONE;C.bHasRoad=false;C.bIsEmergencyAssemblyArea=false;
		if(C.Coords.X<64&&!C.bVolcanicVent)Land.Add(C.Coords);
	}
	FGridCell Occupied=Terrain->GetCell(FIntPoint(33,48));Occupied.OccupyingBuildingID=999;Terrain->SetCell(Occupied.Coords,Occupied);
	FGridCell Road=Terrain->GetCell(FIntPoint(35,50));Road.bHasRoad=true;Terrain->SetCell(Road.Coords,Road);
	FGridCell Cliff=Terrain->GetCell(FIntPoint(31,46));Cliff.Slope=48.f;Cliff.ParcelSuitability=0.f;Terrain->SetCell(Cliff.Coords,Cliff);
	const auto Essential=EssentialFixture(*Buildings);Check(TEXT("essential_fixture_has_44_canonical_lots"),Essential.Num()==44);
	const int64 Money=Economy->TreasuryTL;const int32 BeforeStructures=Buildings->GetActiveBuildingsCount();
	TArray<FAftershockScenarioLot> First=Essential,Repeat=Essential,OtherSeed=Essential,OtherRegion=Essential;
	TArray<FIntPoint> Streets,RepeatStreets,OtherStreets,RegionStreets;
	FAftershockCityPlanStatistics A,B,C,D;
	ExpandAftershockSettlementPlan(*Terrain,*Buildings,Land,FIntPoint(30,62),99173,0,0,First,Streets,A);
	ExpandAftershockSettlementPlan(*Terrain,*Buildings,Land,FIntPoint(30,62),99173,0,0,Repeat,RepeatStreets,B);
	ExpandAftershockSettlementPlan(*Terrain,*Buildings,Land,FIntPoint(30,62),99174,0,0,OtherSeed,OtherStreets,C);
	ExpandAftershockSettlementPlan(*Terrain,*Buildings,Land,FIntPoint(30,62),99173,6,0,OtherRegion,RegionStreets,D);
	Check(TEXT("same_seed_has_exact_ordered_lots_floors_foundations_and_corridors"),EqualLots(First,Repeat)&&Streets==RepeatStreets&&A.LayoutHash==B.LayoutHash);
	Check(TEXT("changing_seed_changes_actual_building_plan"),!EqualLots(First,OtherSeed)&&A.LayoutHash!=C.LayoutHash);
	Check(TEXT("region_biome_changes_actual_block_plan"),A.LayoutHash!=D.LayoutHash&&Streets!=RegionStreets);
	bool EssentialSame=First.Num()>=Essential.Num();for(int32 I=0;I<Essential.Num()&&EssentialSame;++I)
		EssentialSame=First[I].Coord==Essential[I].Coord&&First[I].Type==Essential[I].Type&&First[I].Floors==Essential[I].Floors&&First[I].Foundation==Essential[I].Foundation;
	Check(TEXT("essential_service_plan_preserved_verbatim"),EssentialSame);
	Check(TEXT("planner_produces_real_bounded_diverse_lots"),First.Num()>150&&First.Num()<=1000&&A.DistrictUseTypes>=3&&A.ActiveBlocks>5);
	TSet<FIntPoint> Used;bool Safe=true,FloorBounds=true;TSet<uint8> NewTypes;TSet<int32> FloorBands,Parities;
	for(int32 I=0;I<First.Num();++I)
	{
		const auto& L=First[I];const FGridCell Cell=Terrain->GetCell(L.Coord);const auto* Data=Buildings->GetCatalogData(L.Type);
		Safe&=Land.Contains(L.Coord)&&!Cell.bIsRiverbed&&!Cell.bIsOcean&&!Cell.bVolcanicVent&&!Cell.bHasRoad&&Cell.OccupyingBuildingID==INDEX_NONE&&!Used.Contains(L.Coord);
		FloorBounds&=Data&&L.Floors>=Data->FloorsMin&&L.Floors<=Data->FloorsMax;
		if(I>=44){Safe&=Cell.Slope<=32.f;NewTypes.Add(static_cast<uint8>(L.Type));FloorBands.Add(L.Floors);Parities.Add((L.Coord.X&1)+2*(L.Coord.Y&1));}
		Used.Add(L.Coord);
	}
	Check(TEXT("all_planned_buildings_use_unique_canonical_safe_land"),Safe);
	Check(TEXT("catalogue_floor_limits_are_the_single_source"),FloorBounds);
	Check(TEXT("new_neighbourhoods_have_multiple_uses_heights_and_parity_phases"),NewTypes.Num()>=10&&FloorBands.Num()>=5&&Parities.Num()>=3);
	bool PublicRing=true;for(const auto& L:First)for(int32 Y=-1;Y<=1;++Y)for(int32 X=-1;X<=1;++X)
		if(X||Y)PublicRing&=Land.Contains(L.Coord+FIntPoint(X,Y))&&!Used.Contains(L.Coord+FIntPoint(X,Y));
	Check(TEXT("all_buildings_retain_free_eight_neighbour_land_access"),PublicRing);
	bool StreetSafe=!Streets.IsEmpty();TSet<FIntPoint> StreetSet;
	for(FIntPoint S:Streets){const FGridCell Cell=Terrain->GetCell(S);StreetSafe&=Land.Contains(S)&&!Used.Contains(S)&&!Cell.bHasRoad&&Cell.OccupyingBuildingID==INDEX_NONE&&!Cell.bVolcanicVent&&!Cell.bIsRiverbed;StreetSet.Add(S);}
	Check(TEXT("planned_public_corridors_do_not_cross_water_or_buildings"),StreetSafe&&StreetSet.Num()==Streets.Num());
	int32 Vertices=StreetSet.Num(),Edges=0,Components=0;TSet<FIntPoint> Seen;
	const FIntPoint Steps[]={FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1)};
	for(FIntPoint S:Streets)
	{
		Edges+=StreetSet.Contains(S+FIntPoint(1,0))?1:0;Edges+=StreetSet.Contains(S+FIntPoint(0,1))?1:0;
		if(Seen.Contains(S))continue;++Components;TArray<FIntPoint> Queue;Queue.Add(S);Seen.Add(S);
		for(int32 Head=0;Head<Queue.Num();++Head)for(FIntPoint Step:Steps)if(StreetSet.Contains(Queue[Head]+Step)&&!Seen.Contains(Queue[Head]+Step)){Seen.Add(Queue[Head]+Step);Queue.Add(Queue[Head]+Step);}
	}
	Check(TEXT("block_public_corridors_include_actual_loops"),Edges-Vertices+Components>0);
	Check(TEXT("planning_work_is_bounded_by_cells_and_local_footprints"),A.TerrainCellsVisited<=Terrain->GridWidth*Terrain->GridHeight*3&&A.FootprintChecks<=Terrain->GridWidth*Terrain->GridHeight*6);
	Check(TEXT("pure_plan_never_spends_or_spawns_people_or_structures"),Economy->TreasuryTL==Money&&Buildings->GetActiveBuildingsCount()==BeforeStructures&&!Life->IsCityInitialized());
	Check(TEXT("existing_occupied_road_and_steep_fixture_cells_unchanged"),Terrain->GetCell(Occupied.Coords).OccupyingBuildingID==999&&Terrain->GetCell(Road.Coords).bHasRoad&&Terrain->GetCell(Cliff.Coords).Slope==48.f);
	// Identical synthetic physical land makes this a controlled profile test,
	// not a claim these are the surveyed building counts of the real places.
	const auto ProfileCore=[&](int32 Region,int32 Focus)
	{
		auto Core=Essential;const auto P=GetAftershockSettlementMorphology(Region,Focus);
		for(int32 I=0;I<Core.Num();++I)
		{
			auto& L=Core[I];L.Type=ResolveAftershockMorphologyBuildingType(P,L.Type,static_cast<uint32>(I+1),true);
			const auto* Data=Buildings->GetCatalogData(L.Type);
			if(Data)L.Floors=FMath::Clamp(L.Floors,Data->FloorsMin,FMath::Max(Data->FloorsMin,FMath::Min(Data->FloorsMax,GetAftershockMorphologyFloorCap(P,L.Type))));
		}
		return Core;
	};
	auto Village=ProfileCore(8,0),Miami=ProfileCore(7,0),KeyWest=ProfileCore(7,3),VillageRepeat=ProfileCore(8,0);
	TArray<FIntPoint> VillageStreets,MiamiStreets,KeyWestStreets,VillageRepeatStreets;
	FAftershockCityPlanStatistics V,M,K,VR;
	ExpandAftershockSettlementPlan(*Terrain,*Buildings,Land,FIntPoint(30,62),99173,8,0,Village,VillageStreets,V);
	ExpandAftershockSettlementPlan(*Terrain,*Buildings,Land,FIntPoint(30,62),99173,7,0,Miami,MiamiStreets,M);
	ExpandAftershockSettlementPlan(*Terrain,*Buildings,Land,FIntPoint(30,62),99173,7,3,KeyWest,KeyWestStreets,K);
	ExpandAftershockSettlementPlan(*Terrain,*Buildings,Land,FIntPoint(30,62),99173,8,0,VillageRepeat,VillageRepeatStreets,VR);
	bool VillageLowRise=true,KeyWestLowRise=true,ProfileCatalogue=true;
	for(const auto& L:Village)VillageLowRise&=L.Floors<=3&&L.Type!=EBuildingType::Apartment&&L.Type!=EBuildingType::Office&&L.Type!=EBuildingType::Factory;
	for(const auto& L:KeyWest)KeyWestLowRise&=L.Floors<=3&&L.Type!=EBuildingType::Apartment&&L.Type!=EBuildingType::Office;
	for(const auto& L:Village)
	{const auto* Data=Buildings->GetCatalogData(L.Type);ProfileCatalogue&=Data&&L.Floors>=Data->FloorsMin&&L.Floors<=Data->FloorsMax;}
	Check(TEXT("kalkanli_core_and_fringe_are_lowrise_without_apartment_or_office_substitution_cheat"),VillageLowRise&&ProfileCatalogue&&Village.Num()<=140&&Village.Num()>=44);
	Check(TEXT("miami_actual_generated_density_exceeds_kalkanli_on_identical_physical_land"),Miami.Num()>Village.Num()*2&&M.ProfileMaximumLots>V.ProfileMaximumLots);
	Check(TEXT("key_west_core_and_fringe_never_inherit_miami_tower_profile"),KeyWestLowRise&&KeyWest.Num()<=390&&K.MorphologyId!=M.MorphologyId);
	Check(TEXT("focus_specific_profile_is_exactly_deterministic"),EqualLots(Village,VillageRepeat)&&VillageStreets==VillageRepeatStreets&&V.LayoutHash==VR.LayoutHash);
	Check(TEXT("cyprus_three_focuses_have_distinct_density_and_mix_policies"),FString(GetAftershockSettlementMorphology(8,0).Id)!=GetAftershockSettlementMorphology(8,1).Id
		&&GetAftershockSettlementMorphology(8,0).ApartmentFloorCap==0&&GetAftershockSettlementMorphology(8,1).bIndustrial
		&&GetAftershockSettlementMorphology(8,2).ApartmentFloorCap==4);
	for(auto& Cell:Terrain->Cells){Cell.bHasRealElevationSample=true;Cell.RealElevationMetres=3000.f;}
	TArray<FAftershockScenarioLot> Summit;TArray<FIntPoint> SummitStreets;FAftershockCityPlanStatistics S;
	ExpandAftershockSettlementPlan(*Terrain,*Buildings,Land,FIntPoint(30,62),99173,5,0,Summit,SummitStreets,S);
	Check(TEXT("source_high_elevation_etna_has_no_invented_summit_city_or_streets"),Summit.IsEmpty()&&SummitStreets.IsEmpty()
		&&!IsAftershockMorphologyCellSuitable(GetAftershockSettlementMorphology(5,0),Terrain->GetCell(FIntPoint(30,62))));
	for(auto& Cell:Terrain->Cells){Cell.bHasRealElevationSample=false;Cell.RealElevationMetres=0.f;}
	Metrics.Add(FString::Printf(TEXT("{\"controlled_morphology_fixture\":true,\"kalkanli_lots\":%d,\"miami_lots\":%d,\"key_west_lots\":%d,\"etna_summit_lots\":%d,\"kalkanli_cap\":%d,\"miami_cap\":%d,\"key_west_cap\":%d,\"is_real_world_census\":false}"),Village.Num(),Miami.Num(),KeyWest.Num(),Summit.Num(),V.ProfileMaximumLots,M.ProfileMaximumLots,K.ProfileMaximumLots));
	// Remove explicit blockers only from the synthetic fixture before exercising
	// real placement/save owners. Planner output is not rerolled or regenerated.
	Occupied.OccupyingBuildingID=INDEX_NONE;Terrain->SetCell(Occupied.Coords,Occupied);Road.bHasRoad=false;Terrain->SetCell(Road.Coords,Road);
	int32 Placed=0;const int32 PlacementCount=FMath::Min(96,First.Num());
	for(int32 I=0;I<PlacementCount;++I)
	{
		int32 ID=INDEX_NONE;const auto& L=First[I];
		if(Buildings->PlaceBuilding(L.Coord,Buildings->GetCatalogData(L.Type),L.Foundation,L.Floors,ID))++Placed;
	}
	Check(TEXT("bounded_plan_subset_places_through_real_canonical_manager"),Placed==PlacementCount);
	Life->InitializeForCity();FAftershockBuildingsSaveState Saved;FAftershockCityLifeSaveState SavedLife;
	Buildings->CaptureSaveState(Saved);Life->CaptureSaveState(SavedLife);
	FText Error;const bool Restored=Buildings->RestoreSaveState(Saved,Error)&&Life->RestoreSaveState(SavedLife,Error);
	FAftershockBuildingsSaveState After;FAftershockCityLifeSaveState AfterLife;Buildings->CaptureSaveState(After);Life->CaptureSaveState(AfterLife);
	bool RecordsSame=Restored&&Saved.Buildings.Num()==After.Buildings.Num()&&Saved.NextBuildingID==After.NextBuildingID;
	for(int32 I=0;I<Saved.Buildings.Num()&&RecordsSame;++I)
	{
		const auto& X=Saved.Buildings[I];const auto& Y=After.Buildings[I];
		RecordsSame=X.State.InstanceID==Y.State.InstanceID&&X.State.GridCoords==Y.State.GridCoords&&X.DefinitionType==Y.DefinitionType&&X.State.CurrentFloors==Y.State.CurrentFloors&&X.State.Foundation==Y.State.Foundation&&X.WorldTransform.Equals(Y.WorldTransform,0.f);
	}
	bool ResidentsSame=SavedLife.State.LivingPopulation==AfterLife.State.LivingPopulation&&SavedLife.Residents.Num()==AfterLife.Residents.Num();
	for(int32 I=0;I<SavedLife.Residents.Num()&&ResidentsSame;++I)
		ResidentsSame=SavedLife.Residents[I].ResidentID==AfterLife.Residents[I].ResidentID&&SavedLife.Residents[I].HomeBuildingID==AfterLife.Residents[I].HomeBuildingID&&SavedLife.Residents[I].bAlive==AfterLife.Residents[I].bAlive;
	Check(TEXT("save_owner_restores_actual_building_records_without_plan_reroll"),RecordsSame);
	Check(TEXT("save_owner_preserves_actual_resident_ids_without_job_population"),Restored&&ResidentsSame);
	Metrics.Add(FString::Printf(TEXT("{\"seed\":%d,\"lots\":%d,\"blocks\":%d,\"active_blocks\":%d,\"use_types\":%d,\"corridor_cells\":%d,\"new_building_types\":%d,\"floor_bands\":%d,\"parity_phases\":%d,\"work_cell_visits\":%d,\"footprint_checks\":%d,\"layout_hash\":%u,\"planner_cpu_ms\":%.3f,\"restored_subset_buildings\":%d,\"restored_living\":%d}"),A.Seed,First.Num(),A.Blocks,A.ActiveBlocks,A.DistrictUseTypes,Streets.Num(),NewTypes.Num(),FloorBands.Num(),Parities.Num(),A.TerrainCellsVisited,A.FootprintChecks,A.LayoutHash,A.PlannerCpuMilliseconds,After.Buildings.Num(),AfterLife.State.LivingPopulation));
	return Finish();
}
#else
UAftershockCityGenerationAuditCommandlet::UAftershockCityGenerationAuditCommandlet(){}
int32 UAftershockCityGenerationAuditCommandlet::Main(const FString& Params){return 1;}
#endif
