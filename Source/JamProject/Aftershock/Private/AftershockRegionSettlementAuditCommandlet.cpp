#include "AftershockRegionSettlementAuditCommandlet.h"

#if WITH_EDITOR
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/AftershockRegionRules.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Class.h"

namespace
{
	struct FRegionSettlementWorld
	{
		TStrongObjectPtr<UGameInstance> Instance;
		UWorld* World=nullptr;
		~FRegionSettlementWorld()
		{
			// InitializeStandalone owns exactly one engine context. Match the
			// session/save fixture cleanup without creating a second context.
			if(Instance.IsValid())Instance->Shutdown();
			if(World)
			{
				World->DestroyWorld(false);
				if(GEngine)GEngine->DestroyWorldContext(World);
				World->RemoveFromRoot();
			}
		}
	};
	FString EscapeRegionSettlementJson(FString Value)
	{
		Value.ReplaceInline(TEXT("\\"),TEXT("\\\\"));Value.ReplaceInline(TEXT("\""),TEXT("\\\""));
		Value.ReplaceInline(TEXT("\n"),TEXT("\\n"));Value.ReplaceInline(TEXT("\r"),TEXT("\\r"));return Value;
	}
	FVector ExpectedCoastInlandDirection(EAftershockCoastEdge Edge)
	{
		switch(Edge)
		{
		case EAftershockCoastEdge::North:return FVector(0,1,0);
		case EAftershockCoastEdge::East:return FVector(-1,0,0);
		case EAftershockCoastEdge::South:return FVector(0,-1,0);
		case EAftershockCoastEdge::West:return FVector(1,0,0);
		default:return FVector::ZeroVector;
		}
	}
}

UAftershockRegionSettlementAuditCommandlet::UAftershockRegionSettlementAuditCommandlet()
{
	IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;
}

int32 UAftershockRegionSettlementAuditCommandlet::Main(const FString& Params)
{
	const bool bRealSourceAudit=FParse::Param(FCommandLine::Get(),TEXT("AftershockRealRegionSettlementAudit"));
	int32 Passed=0,Failed=0,VisitedFocuses=0;
	TArray<FString> Checks,FocusMetrics;TSet<FGuid> SessionIds;
	const auto Check=[&](const TCHAR* Name,bool bPass,const FString& Detail=FString())
	{
		bPass?++Passed:++Failed;
		Checks.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s,\"detail\":\"%s\"}"),Name,bPass?TEXT("true"):TEXT("false"),*EscapeRegionSettlementJson(Detail)));
		if(bPass){UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_REGION_SETTLEMENT_PASS %s %s"),Name,*Detail);}
		else{UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_REGION_SETTLEMENT_FAIL %s %s"),Name,*Detail);}
	};
	const auto Finish=[&]()
	{
		const FString Directory=FPaths::ProjectSavedDir()/TEXT("PresentationValidation");IFileManager::Get().MakeDirectory(*Directory,true);
		const FString JSON=FString::Printf(TEXT("{\"schema_version\":1,\"created_utc\":\"%s\",\"geography_mode\":\"%s\",\"scope\":\"Fresh real GameInstance per local focus; production preparation, canonical settlement and crowd startup. Mode explicitly separates authored V1 regression from new V3 WGS84/metres/detail-source startup. V2 restoration is independently audited. No pointer UI, rendered image, hazard outcome, process restart or throughput claim.\",\"region_count\":%d,\"focus_metric_count\":%d,\"passed\":%d,\"failed\":%d,\"focus_metrics\":[%s],\"checks\":[%s]}"),*FDateTime::UtcNow().ToIso8601(),bRealSourceAudit?TEXT("V3RealSource"):TEXT("V1LegacyAuthored"),GetAftershockGeographyCount(),FocusMetrics.Num(),Passed,Failed,*FString::Join(FocusMetrics,TEXT(",")),*FString::Join(Checks,TEXT(",")));
		const FString Path=Directory/(bRealSourceAudit?TEXT("real_region_settlement_audit.json"):TEXT("region_settlement_audit.json"));
		if(!FFileHelper::SaveStringToFile(JSON,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return 2;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_REGION_SETTLEMENT_RESULT passed=%d failed=%d regions=%d focuses=%d report=%s"),Passed,Failed,GetAftershockGeographyCount(),VisitedFocuses,*Path);
		return Failed==0?0:1;
	};
	Check(TEXT("engine_available"),GEngine!=nullptr);if(!GEngine)return Finish();
	Check(TEXT("real_source_mode_cannot_use_legacy_geography_override"),!bRealSourceAudit||!FParse::Param(FCommandLine::Get(),TEXT("AftershockLegacyGeographyAudit")));if(bRealSourceAudit&&FParse::Param(FCommandLine::Get(),TEXT("AftershockLegacyGeographyAudit")))return Finish();
	Check(TEXT("twenty_regions_available"),GetAftershockGeographyCount()==20);
	const int32 InitialContexts=GEngine->GetWorldContexts().Num();
	for(int32 RegionIndex=0;RegionIndex<GetAftershockGeographyCount();++RegionIndex)
	for(int32 FocusIndex=0;FocusIndex<GetAftershockGeographyFocusCount(RegionIndex);++FocusIndex)
	{
		if(!bRealSourceAudit&&RegionIndex==5&&FocusIndex==1)
		{
			// The additive alias is real-distance V3 only: do not fabricate a new
			// compressed V1 city/vent or weaken any of the original25 regressions.
			const int32 ContextsBefore=GEngine->GetWorldContexts().Num();
			{
				FRegionSettlementWorld Rejected;
				Rejected.Instance.Reset(NewObject<UGameInstance>(GEngine));
				Rejected.Instance->InitializeStandalone(TEXT("AftershockAdditionalFocusLegacyRejection"));
				Rejected.World=Rejected.Instance->GetWorld();
				auto* Terrain=Rejected.World?Rejected.World->GetSubsystem<UTerrainGridSubsystem>():nullptr;
				Check(TEXT("v1_additive_lower_flank_rejection_fixture_created"),Terrain!=nullptr);
				if(Terrain)
				{
					Terrain->GenerateProceduralTerrain(50,50,500.f,1337);
					FAftershockTerrainSaveState Before,After;Terrain->CaptureSaveState(Before);
					const bool Applied=Terrain->ApplyLegacyOfflineGeography(5,1,true);Terrain->CaptureSaveState(After);
					Check(TEXT("v1_additive_lower_flank_is_rejected_without_any_canonical_snapshot_mutation"),!Applied
						&&FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&Before,&After,0));
				}
			}
			Check(TEXT("v1_additive_rejection_fixture_releases_its_world_context"),GEngine->GetWorldContexts().Num()==ContextsBefore);
			continue;
		}
		++VisitedFocuses;
		const auto& Site=GetAftershockGeographyFocus(RegionIndex,FocusIndex);
		const auto& Rules=GetAftershockRegionRules(RegionIndex);const auto& Local=GetAftershockLocalFocusRules(RegionIndex,FocusIndex);
		const bool bSummitInspection=bRealSourceAudit&&RegionIndex==5&&FocusIndex==0;
		const FString Key=FString::Printf(TEXT("r%02d_f%02d_"),RegionIndex,FocusIndex);
		const int32 BeforePassed=Passed,BeforeFailed=Failed,BeforeContexts=GEngine->GetWorldContexts().Num();
		int32 BuildingsCount=0,Living=0,Rendered=0,RoadsCount=0,OceanCells=0,RiverCells=0,VentCells=0;
		int32 EligibleInitialLandCells=0,LargestInitialComponent=0,ChosenComponent=0,PlannedCorridors=0,RoadBudget=0;
		float MaxDemError=0.f;FIntPoint Vent(INDEX_NONE,INDEX_NONE);
		{
			FRegionSettlementWorld Fixture;
			Fixture.Instance.Reset(NewObject<UGameInstance>(GEngine));
			Fixture.Instance->InitializeStandalone(FName(*FString::Printf(TEXT("AftershockRegionSettlement_%02d_%02d"),RegionIndex,FocusIndex)));
			Fixture.World=Fixture.Instance->GetWorld();UWorld* World=Fixture.World;
			Check(*(Key+TEXT("real_game_instance_has_single_context")),World&&World->GetGameInstance()==Fixture.Instance.Get()&&GEngine->GetWorldContexts().Num()==BeforeContexts+1);
			if(World)
			{
				auto* Session=Fixture.Instance->GetSubsystem<UAftershockOpeningSessionSubsystem>();
				auto* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();auto* Scenario=World->GetSubsystem<UAftershockScenarioSubsystem>();
				auto* Buildings=World->GetSubsystem<UBuildingManagerSubsystem>();auto* Life=World->GetSubsystem<UCityLifeSubsystem>();
				auto* Crowd=World->GetSubsystem<UCrowdRenderSubsystem>();auto* Economy=World->GetSubsystem<UEconomyAndStateSubsystem>();
				auto* Roads=World->GetSubsystem<URoadNetworkSubsystem>();auto* Disaster=World->GetSubsystem<UDisasterSimulationSubsystem>();
				const bool bOwners=Session&&Terrain&&Scenario&&Buildings&&Life&&Crowd&&Economy&&Roads&&Disaster;
				Check(*(Key+TEXT("startup_owners_exist")),bOwners);
				if(bOwners)
				{
					FAftershockSessionSettings Settings;Settings.Mode=EAftershockPlayMode::GeographicSimulation;
					Settings.RegionID=FName(Site.Id);Settings.FocusID=FName(Site.FocusId);Settings.Seed=41021+RegionIndex*97+FocusIndex*1009;
					if(RegionIndex==5&&FocusIndex==1)
					{
						FAftershockSessionSettings NewDefault=Settings;NewDefault.FocusID=NAME_None;FText DefaultError;
						Check(TEXT("new_etna_session_normalizes_missing_focus_to_explicit_saved_lower_flank_id"),Session->ConfigureNewSession(NewDefault,DefaultError)&&Session->GetSettings().FocusID==FName(TEXT("lower_flank")));
						Session->RestoreSession(NewDefault,FAftershockCampaignProgress(),FGuid::NewGuid());
						Check(TEXT("old_etna_restore_retains_none_as_legacy_summit_zero_without_new_session_normalization"),Session->GetSettings().FocusID.IsNone()
							&&UAftershockOpeningSessionSubsystem::ValidateSettings(Session->GetSettings(),DefaultError)&&GetAftershockGeographyFocus(5,0).Latitude==37.73);
					}
					FText Error;const bool bConfigured=Session->ConfigureNewSession(Settings,Error);
					Check(*(Key+TEXT("selected_region_and_focus_configure")),bConfigured,Error.ToString());
					if(bConfigured)
					{
						const FGuid SessionId=Session->GetSessionID();
						Check(*(Key+TEXT("fresh_session_identity")),SessionId.IsValid()&&!SessionIds.Contains(SessionId));SessionIds.Add(SessionId);
						const bool bPrepared=Scenario->PrepareScenarioBeforeSettlement();
						Check(*(Key+TEXT("scenario_prepares_selected_local_geography")),bPrepared&&Scenario->IsPrepared()&&Scenario->GetDefinition().GeographyIndex==RegionIndex&&Terrain->GeographyIndex==RegionIndex&&Terrain->GeographyFocusIndex==FocusIndex
							&&Scenario->GetDefinition().Seed==Settings.Seed&&Terrain->TerrainSeed==Settings.Seed&&Scenario->GetDefinition().Hazard==Rules.PrimaryHazard
							&&FMath::IsNearlyEqual(Scenario->GetDefinition().Latitude,Site.Latitude,1.e-8)&&FMath::IsNearlyEqual(Scenario->GetDefinition().Longitude,Site.Longitude,1.e-8));
						const int32 ExpectedCells=Terrain->GridWidth*Terrain->GridHeight;
						if(bRealSourceAudit)
						{
							bool bDem=ExpectedCells>0&&Terrain->ContinuationGeneratorVersion==3&&Terrain->IsUsingRealGeography()&&Terrain->Cells.Num()>=ExpectedCells;
							bool bWaterMasks=true,bMetreScale=true,bNoOval=true,bSourceChunkOnly=true;int32 ExpectedOcean=0,ExpectedRiver=0;bool bSourceForest=false;
							FIntPoint MappedVent(MAX_int32,MAX_int32);const bool bMappedVent=Local.bHasVolcanicSource&&Terrain->TryGeographicToCell(Rules.VolcanoLatitude,Rules.VolcanoLongitude,MappedVent);
							const FIntPoint SourceChunk=UTerrainGridSubsystem::CellToChunk(MappedVent);
							for(int32 I=0;I<Terrain->Cells.Num();++I)
							{
								const auto& Cell=Terrain->Cells[I];double Latitude=0.,Longitude=0.;FAftershockGeographicTerrainSample Sample;
								const FVector2D Metres((Cell.Coords.X+.5-Terrain->GridWidth*.5)*Terrain->CellSize*.01,(Cell.Coords.Y+.5-Terrain->GridHeight*.5)*Terrain->CellSize*.01);
								const bool bSample=AftershockFocusLocalMetresToLatLon(RegionIndex,FocusIndex,Metres,Latitude,Longitude)&&Terrain->TrySampleGeographicSource(Latitude,Longitude,Sample);
								if(!bSample){bDem=false;bWaterMasks=false;bMetreScale=false;continue;}
								const float Difference=FMath::Abs(Cell.RealElevationMetres-Sample.ElevationMetres);if(FMath::IsFinite(Difference))MaxDemError=FMath::Max(MaxDemError,Difference);else bDem=false;
								bDem&=Terrain->IsValidCoord(Cell.Coords)&&Cell.bHasRealElevationSample&&!Cell.bAuthoredContinuation&&FMath::IsFinite(Difference)&&Difference<=.01f;
								bMetreScale&=FMath::IsNearlyEqual(Cell.Elevation,Terrain->GridOrigin.Z+Sample.ElevationMetres*100.f,1.f);
								bNoOval&=Cell.bIsPlayable==!Sample.bOcean;
								const bool bExpectedCoastal=!Sample.bOcean&&Sample.bHasCoastlineSource&&Sample.SignedCoastDistanceMetres>=0.f&&Sample.SignedCoastDistanceMetres<=Terrain->CellSize*.03f;
								bWaterMasks&=Cell.bIsOcean==Sample.bOcean&&Cell.bIsRiverbed==(!Sample.bOcean&&Sample.bInlandWater)&&Cell.bIsCoastal==bExpectedCoastal;
								ExpectedOcean+=Sample.bOcean?1:0;ExpectedRiver+=!Sample.bOcean&&Sample.bInlandWater?1:0;
								OceanCells+=Cell.bIsOcean?1:0;RiverCells+=Cell.bIsPlayable&&Cell.bIsRiverbed&&!Cell.bIsOcean?1:0;VentCells+=Cell.bVolcanicVent?1:0;
								bSourceForest|=!Sample.bOcean&&!Sample.bInlandWater&&Sample.bMappedForest;
								if(I>=ExpectedCells)bSourceChunkOnly&=bMappedVent&&UTerrainGridSubsystem::CellToChunk(Cell.Coords)==SourceChunk;
							}
							Check(*(Key+TEXT("all_v3_runtime_cells_match_actual_wgs84_detail_source")),bDem,FString::Printf(TEXT("maxErrorMetres=%.6f"),MaxDemError));
							Check(*(Key+TEXT("v3_elevations_keep_real_metres_times_100_without_normalization")),bMetreScale);
							Check(*(Key+TEXT("v3_source_water_masks_have_no_authored_coast_or_river_substitute")),bWaterMasks&&OceanCells==ExpectedOcean&&RiverCells==ExpectedRiver);
							Check(*(Key+TEXT("v3_source_non_ocean_land_has_no_oval_mask")),bNoOval);
							const int32 ExtraCells=Terrain->Cells.Num()-ExpectedCells;
							int32 ExpectedExtra=0;const FIntRect InitialFrame(0,0,Terrain->GridWidth,Terrain->GridHeight);
							if(bMappedVent&&!InitialFrame.Contains(MappedVent))for(int32 Y=0;Y<25;++Y)for(int32 X=0;X<25;++X)
								ExpectedExtra+=InitialFrame.Contains(SourceChunk*25+FIntPoint(X,Y))?0:1;
							Check(*(Key+TEXT("v3_startup_extra_storage_is_only_one_bounded_mapped_vent_chunk")),bSourceChunkOnly&&ExtraCells==ExpectedExtra&&ExtraCells>=0&&ExtraCells<=625&&(ExtraCells==0||Local.bHasVolcanicSource));
							Vent=Terrain->VolcanicVentCoord;FText HazardError;
							Check(*(Key+TEXT("v3_tsunami_gate_uses_actual_ocean_cells")),Terrain->IsCoastalWorld()==(ExpectedOcean>0)&&Disaster->CanTriggerDisaster(EDisasterType::Tsunami,HazardError)==(ExpectedOcean>0));
							Check(*(Key+TEXT("v3_fire_gate_uses_actual_mapped_forest_fuel")),Disaster->CanTriggerDisaster(EDisasterType::Fire,HazardError)==bSourceForest);
							const bool bActualVent=bMappedVent&&Vent==MappedVent&&VentCells==1&&Terrain->IsPlayableCoord(Vent)&&Terrain->GetCell(Vent).bVolcanicVent;
							Check(*(Key+TEXT("v3_volcano_gate_uses_unmoved_actual_source_coordinate")),Local.bHasVolcanicSource?bActualVent&&Disaster->CanTriggerDisaster(EDisasterType::Volcano,HazardError):VentCells==0&&!Disaster->CanTriggerDisaster(EDisasterType::Volcano,HazardError));
						}
						else
						{
						bool bDem=Terrain->Cells.Num()==ExpectedCells&&ExpectedCells>0;
						for(const FGridCell& Cell:Terrain->Cells)
						{
							const float Expected=SampleAftershockFocusElevationMetres(RegionIndex,FocusIndex,(Cell.Coords.X+.5f)/Terrain->GridWidth,(Cell.Coords.Y+.5f)/Terrain->GridHeight);
							const float Difference=FMath::Abs(Cell.RealElevationMetres-Expected);if(FMath::IsFinite(Difference))MaxDemError=FMath::Max(MaxDemError,Difference);else bDem=false;
							bDem&=Terrain->IsValidCoord(Cell.Coords)&&FMath::IsFinite(Cell.RealElevationMetres)&&Difference<=.01f;
							OceanCells+=Cell.bIsOcean?1:0;RiverCells+=Cell.bIsPlayable&&Cell.bIsRiverbed&&!Cell.bIsOcean?1:0;VentCells+=Cell.bVolcanicVent?1:0;
						}
						Check(*(Key+TEXT("all_runtime_cells_sample_the_selected_real_dem")),bDem,FString::Printf(TEXT("maxErrorMetres=%.6f"),MaxDemError));
						Check(*(Key+TEXT("local_coast_gate_matches_actual_terrain")),Local.bHasCoast?(OceanCells>0&&Terrain->IsCoastalWorld()):(OceanCells==0&&!Terrain->IsCoastalWorld()));
						Check(*(Key+TEXT("local_coast_direction_matches_authored_edge")),!Local.bHasCoast||Terrain->GetCoastInlandDirection().Equals(ExpectedCoastInlandDirection(Local.CoastEdge),.001f));
						Check(*(Key+TEXT("local_river_gate_matches_actual_terrain")),Local.bHasRiver?RiverCells>0:RiverCells==0);
						Vent=Terrain->VolcanicVentCoord;
						Check(*(Key+TEXT("local_volcanic_source_matches_actual_terrain")),Local.bHasVolcanicSource?(VentCells==1&&Terrain->IsPlayableCoord(Vent)&&Terrain->GetCell(Vent).bVolcanicVent):(VentCells==0&&Vent==FIntPoint(INDEX_NONE,INDEX_NONE)));
						FText HazardError;
						Check(*(Key+TEXT("primary_hazard_is_eligible_in_real_startup")),IsAftershockFocusHazardAllowed(RegionIndex,FocusIndex,Rules.PrimaryHazard)&&Disaster->CanTriggerDisaster(Rules.PrimaryHazard,HazardError),HazardError.ToString());
						}
						if(bPrepared)Buildings->SeedInitialSettlement();Crowd->SynchronizeCanonicalPopulation();
						TArray<FBuildingInstance> Roster;Buildings->GetAllBuildings(Roster);BuildingsCount=Roster.Num();
						Living=Life->GetState().LivingPopulation;Rendered=Crowd->GetCurrentCrowdCount();
						Check(*(Key+TEXT("canonical_settlement_and_living_population_initialized")),Life->IsCityInitialized()&&(bSummitInspection
							?BuildingsCount==0&&Scenario->GetDesignedLots().IsEmpty()&&Living==0&&Rendered==0&&Scenario->GetAssemblyCells().IsEmpty()
							:BuildingsCount>0&&BuildingsCount<=Scenario->GetDesignedLots().Num()&&Living>0));
						bool bValidBuildings=true;TSet<int32> BuildingIDs;TSet<FIntPoint> Parcels;int32 LivingCapacity=0;
						for(const auto& Building:Roster)
						{
							const bool bInside=Terrain->IsPlayableCoord(Building.GridCoords);const FGridCell Cell=Terrain->GetCell(Building.GridCoords);
							bValidBuildings&=bInside&&Building.InstanceID>0&&!BuildingIDs.Contains(Building.InstanceID)&&!Parcels.Contains(Building.GridCoords)&&Building.BuildingData
								&&!Cell.bIsOcean&&!Cell.bIsRiverbed&&!Cell.bVolcanicVent&&!Cell.bHasRoad&&Cell.OccupyingBuildingID==Building.InstanceID;
							BuildingIDs.Add(Building.InstanceID);Parcels.Add(Building.GridCoords);
							if(Building.BuildingData&&(Building.BuildingData->Type==EBuildingType::House||Building.BuildingData->Type==EBuildingType::Apartment))LivingCapacity+=Building.BuildingData->Residents;
						}
						Check(*(Key+TEXT("initial_buildings_have_unique_safe_canonical_parcels")),bValidBuildings&&BuildingsCount<=Terrain->Cells.Num());
						bool bValidRoads=true;for(const FGridCell& Cell:Terrain->Cells)if(Cell.bHasRoad)
						{
							++RoadsCount;bValidRoads&=Terrain->IsPlayableCoord(Cell.Coords)&&!Cell.bIsOcean&&!Cell.bVolcanicVent&&Cell.OccupyingBuildingID==INDEX_NONE&&Roads->IsRoadTraversable(Cell.Coords)
								&&(!Cell.bIsRiverbed||Cell.BridgeID!=INDEX_NONE);
						}
						Check(*(Key+TEXT("startup_roads_exist_and_are_valid_public_edges")),bValidRoads&&(bSummitInspection?RoadsCount==0&&Scenario->GetStarterRoadCellCount()==0:RoadsCount>0));
						if(bRealSourceAudit)
						{
							// Independent post-placement mask traversal, ignoring building
							// occupancy: no planner-internal candidate count is trusted here.
							const auto IsInitialLand=[&](FIntPoint C)
							{
								if(C.X<0||C.Y<0||C.X>=Terrain->GridWidth||C.Y>=Terrain->GridHeight)return false;
								const auto Cell=Terrain->GetCell(C);return Cell.bIsPlayable&&!Cell.bIsOcean&&!Cell.bIsRiverbed&&!Cell.bVolcanicVent;
							};
							const FIntPoint Steps[]={FIntPoint(0,1),FIntPoint(1,0),FIntPoint(0,-1),FIntPoint(-1,0)};
							TSet<FIntPoint> Seen;
							for(int32 Y=0;Y<Terrain->GridHeight;++Y)for(int32 X=0;X<Terrain->GridWidth;++X)
							{
								const FIntPoint Start(X,Y);if(Seen.Contains(Start)||!IsInitialLand(Start))continue;
								TArray<FIntPoint> Component;Component.Add(Start);Seen.Add(Start);
								for(int32 Head=0;Head<Component.Num();++Head)for(FIntPoint Step:Steps)
								{const FIntPoint Next=Component[Head]+Step;if(!Seen.Contains(Next)&&IsInitialLand(Next)){Seen.Add(Next);Component.Add(Next);}}
								EligibleInitialLandCells+=Component.Num();LargestInitialComponent=FMath::Max(LargestInitialComponent,Component.Num());
							}
							const auto& Plan=Scenario->GetCityPlanStatistics();ChosenComponent=Plan.ChosenLandComponentCells;
							PlannedCorridors=Scenario->GetPlannedPublicCorridors().Num();RoadBudget=PlannedCorridors*2+BuildingsCount*2+128;
							Check(*(Key+TEXT("v3_chooses_largest_actual_initial_land_component_not_nearest_tiny_islet")),ChosenComponent==LargestInitialComponent
								&&Plan.LargestLandComponentCells==LargestInitialComponent&&ChosenComponent>0);
							// Authored performance/urban-space budgets, never measured road
							// density or municipal standards. A connected endpoint for EVERY
							// lot and ALL planned loops remains mandatory below.
							Check(*(Key+TEXT("v3_actual_road_coverage_is_at_most_35_percent_of_initial_dry_land")),RoadsCount*100<=EligibleInitialLandCells*35);
							Check(*(Key+TEXT("v3_actual_road_cells_fit_corridor_plus_frontage_connector_budget")),RoadsCount<=RoadBudget);
							TSet<FIntPoint> Connected;TArray<FIntPoint> Queue;
							if(!Scenario->GetAssemblyCells().IsEmpty()){Queue.Add(Scenario->GetAssemblyCells()[0]);Connected.Add(Queue[0]);}
							for(int32 Head=0;Head<Queue.Num();++Head)for(FIntPoint Step:Steps)
							{const auto Next=Queue[Head]+Step;if(!Connected.Contains(Next)&&Roads->CanTraverseRoadEdge(Queue[Head],Next)){Connected.Add(Next);Queue.Add(Next);}}
							bool AllFrontages=true,AllCorridors=true;
							for(const auto& Building:Roster)
							{bool Frontage=false;for(FIntPoint Step:Steps)Frontage|=Connected.Contains(Building.GridCoords+Step)&&Roads->CanAccessRoadFromCell(Building.GridCoords,Building.GridCoords+Step);AllFrontages&=Frontage;}
							for(FIntPoint Corridor:Scenario->GetPlannedPublicCorridors())AllCorridors&=Connected.Contains(Corridor)&&Roads->HasRoad(Corridor);
							Check(*(Key+TEXT("v3_road_budget_preserves_every_actual_lot_frontage_and_all_planned_corridor_loops")),AllFrontages&&AllCorridors&&Connected.Num()==RoadsCount);
						}
						TSet<int32> LivingIDs;bool bCanonicalPeople=true;
						for(const auto& Person:Life->GetResidents())if(Person.bAlive)
						{
							bCanonicalPeople&=Person.ResidentID>0&&!LivingIDs.Contains(Person.ResidentID)&&BuildingIDs.Contains(Person.HomeBuildingID);
							LivingIDs.Add(Person.ResidentID);
						}
						const auto Agents=Crowd->GetCitizenDebugRoster();TSet<int32> VisualIDs;bool bVisualPeople=true;
						for(const auto& Agent:Agents)
						{
							bVisualPeople&=LivingIDs.Contains(Agent.ResidentID)&&!VisualIDs.Contains(Agent.ResidentID)&&!Agent.CurrentLocation.ContainsNaN()&&Terrain->IsPlayableCoord(Terrain->WorldToGrid(Agent.CurrentLocation));
							VisualIDs.Add(Agent.ResidentID);
						}
						Check(*(Key+TEXT("crowd_is_the_bounded_canonical_living_roster")),bCanonicalPeople&&bVisualPeople&&LivingIDs.Num()==Living&&Living==LivingCapacity&&Rendered==FMath::Min(1500,Living)&&Agents.Num()==Rendered&&!Crowd->IsSyntheticRenderProfileActive());
						Check(*(Key+TEXT("settlement_does_not_override_session_focus_seed_or_budget")),Session->GetSettings().RegionID==Settings.RegionID&&Session->GetSettings().FocusID==Settings.FocusID&&Terrain->GeographyIndex==RegionIndex&&Terrain->GeographyFocusIndex==FocusIndex
							&&Terrain->TerrainSeed==Settings.Seed&&Economy->TreasuryTL==Scenario->GetDefinition().StartingBudgetTL&&Economy->CurrentTurn==1);
						const int32 BeforeBuildings=BuildingsCount,BeforePeople=Living,BeforeRoads=RoadsCount;
						Scenario->PrepareScenarioBeforeSettlement();Buildings->SeedInitialSettlement();Crowd->SynchronizeCanonicalPopulation();
						int32 AfterRoads=0;for(const auto& Cell:Terrain->Cells)AfterRoads+=Cell.bHasRoad?1:0;
						Check(*(Key+TEXT("repeat_startup_is_idempotent_without_population_or_road_duplication")),Buildings->GetActiveBuildingsCount()==BeforeBuildings&&Life->GetState().LivingPopulation==BeforePeople&&AfterRoads==BeforeRoads&&Terrain->GeographyIndex==RegionIndex&&Terrain->GeographyFocusIndex==FocusIndex);
					}
				}
			}
		}
		Check(*(Key+TEXT("world_and_engine_context_cleanup")),GEngine->GetWorldContexts().Num()==BeforeContexts);
		FocusMetrics.Add(FString::Printf(TEXT("{\"region_index\":%d,\"focus_index\":%d,\"region_id\":\"%s\",\"focus_id\":\"%s\",\"buildings\":%d,\"living_population\":%d,\"crowd_agents\":%d,\"road_cells\":%d,\"ocean_cells\":%d,\"river_cells\":%d,\"vent_cells\":%d,\"vent_x\":%d,\"vent_y\":%d,\"max_dem_error_m\":%.6f,\"initial_dry_land_cells\":%d,\"largest_initial_land_component\":%d,\"chosen_land_component\":%d,\"planned_corridor_cells\":%d,\"authored_road_budget\":%d,\"passed\":%d,\"failed\":%d}"),RegionIndex,FocusIndex,*EscapeRegionSettlementJson(Site.Id),*EscapeRegionSettlementJson(Site.FocusId),BuildingsCount,Living,Rendered,RoadsCount,OceanCells,RiverCells,VentCells,Vent.X,Vent.Y,MaxDemError,EligibleInitialLandCells,LargestInitialComponent,ChosenComponent,PlannedCorridors,RoadBudget,Passed-BeforePassed,Failed-BeforeFailed));
	}
	const int32 ExpectedFocuses=bRealSourceAudit?26:25;
	Check(bRealSourceAudit?TEXT("all_twenty_six_real_source_focuses_exercised"):TEXT("all_twenty_five_immutable_v1_focuses_exercised_and_new_alias_rejected"),
		VisitedFocuses==ExpectedFocuses&&FocusMetrics.Num()==ExpectedFocuses&&SessionIds.Num()==ExpectedFocuses);
	Check(TEXT("all_fixture_contexts_released"),GEngine->GetWorldContexts().Num()==InitialContexts);
	return Finish();
}
#else
UAftershockRegionSettlementAuditCommandlet::UAftershockRegionSettlementAuditCommandlet() {}
int32 UAftershockRegionSettlementAuditCommandlet::Main(const FString& Params) { return 1; }
#endif
