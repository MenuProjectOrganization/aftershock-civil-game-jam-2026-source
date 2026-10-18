#include "AftershockMunicipalAuditCommandlet.h"
#include "Aftershock/Public/TransportSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

UAftershockMunicipalAuditCommandlet::UAftershockMunicipalAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}

int32 UAftershockMunicipalAuditCommandlet::Main(const FString& Params)
{
	bool Passed=true;int32 Checks=0,Failures=0;TArray<TSharedPtr<FJsonValue>> Results;
	const auto Check=[&](const TCHAR* Name,bool Value)
	{
		++Checks;Failures+=Value?0:1;Passed&=Value;auto Row=MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"),Name);Row->SetBoolField(TEXT("passed"),Value);Results.Add(MakeShared<FJsonValueObject>(Row));
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MUNICIPAL_CHECK case=%s passed=%d"),Name,Value);
	};
	UWorld* World=GEngine?UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("MunicipalFleetFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false):nullptr;
	Check(TEXT("fixture_world_created"),World!=nullptr);
	if(World)
	{
		FWorldContext& Context=GEngine->CreateNewWorldContext(EWorldType::Game);Context.SetCurrentWorld(World);
		auto* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();auto* Buildings=World->GetSubsystem<UBuildingManagerSubsystem>();
		auto* Life=World->GetSubsystem<UCityLifeSubsystem>();auto* Transport=World->GetSubsystem<UTransportSubsystem>();auto* Roads=World->GetSubsystem<URoadNetworkSubsystem>();
		Check(TEXT("canonical_subsystems_available"),Terrain&&Buildings&&Life&&Transport&&Roads);
		if(Terrain&&Buildings&&Life&&Transport&&Roads)
		{
			Terrain->GenerateProceduralTerrain(30,12,1000,8117);
			for(int32 Y=0;Y<12;++Y)for(int32 X=0;X<30;++X)
			{FGridCell Cell;Cell.Coords=FIntPoint(X,Y);Cell.bIsPlayable=true;Cell.Elevation=0.f;Cell.bHasRoad=Y==5&&X>0&&X<29;Terrain->Cells[Terrain->CoordToIndex(Cell.Coords)]=Cell;}
			UStaticMesh* Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
			const auto Place=[&](EBuildingType Type,FIntPoint Coord,int32 Residents,int32 Workers)
			{
				auto* Data=NewObject<UBuildingData>(Buildings);Data->Type=Type;Data->Residents=Residents;Data->Workers=Workers;Data->MeshIntact=Cube;
				Data->PowerDemandUnitsPerTurn=0.f;Data->CostTL=0;Data->DisplayName=FText::FromString(TEXT("Municipal native fixture"));
				int32 ID=INDEX_NONE;Check(TEXT("fixture_building_placed"),Buildings->PlaceBuilding(Coord,Data,EFoundationQuality::Medium,1,ID));return ID;
			};
			Place(EBuildingType::House,FIntPoint(3,4),120,0);
			const int32 FireA=Place(EBuildingType::FireStation,FIntPoint(6,4),0,4);
			const int32 FireB=Place(EBuildingType::FireStation,FIntPoint(9,4),0,4);
			const int32 Hospital=Place(EBuildingType::Hospital,FIntPoint(12,4),0,4);
			const int32 Police=Place(EBuildingType::PoliceStation,FIntPoint(15,4),0,4);
			const int32 Court=Place(EBuildingType::Courthouse,FIntPoint(18,4),0,4);
			const int32 Tax=Place(EBuildingType::TaxOffice,FIntPoint(22,4),0,4);
			Place(EBuildingType::House,FIntPoint(27,4),20,0);
			Life->InitializeForCity();Transport->RefreshInfrastructure();
			FAftershockCityLifeSaveState PopulationBefore;Life->CaptureSaveState(PopulationBefore);
			FAftershockTransportSaveState TrafficBefore;Transport->CaptureSaveState(TrafficBefore);
			TArray<FBuildingInstance> Records;Buildings->GetAllBuildings(Records);Transport->RefreshMunicipalFleet(Records);
			Check(TEXT("one_owned_fleet_unit_per_six_operating_staffed_service_buildings"),Transport->GetMunicipalFleetCount()==6);
			bool StaffValid=true,GaragesHidden=true;TSet<int32> Crew;
			for(const auto& Unit:Transport->MunicipalFleet)
			{
				FCityResidentRecord Resident;StaffValid&=Life->GetResident(Unit.ResidentID,Resident)&&Resident.bAlive&&Resident.WorkBuildingID==Unit.OwnerBuildingID&&!Crew.Contains(Unit.ResidentID);
				Crew.Add(Unit.ResidentID);GaragesHidden&=!Unit.bDeployed&&Unit.Vehicle.ParkedAtBuildingID==Unit.OwnerBuildingID&&Unit.Vehicle.VehicleID<=-1000;
			}
			Check(TEXT("all_service_staff_are_distinct_existing_employed_residents"),StaffValid&&Crew.Num()==6);
			Check(TEXT("idle_service_vehicles_occupy_owned_hidden_garages_not_road_centres"),GaragesHidden);
			Check(TEXT("courthouse_and_ambulance_and_police_roles_are_building_specific"),
				Transport->MunicipalFleet.ContainsByPredicate([&](const auto& U){return U.OwnerBuildingID==Court&&U.Service==EAftershockMunicipalService::CourtTransport;})&&
				Transport->MunicipalFleet.ContainsByPredicate([&](const auto& U){return U.OwnerBuildingID==Hospital&&U.Service==EAftershockMunicipalService::Ambulance;})&&
				Transport->MunicipalFleet.ContainsByPredicate([&](const auto& U){return U.OwnerBuildingID==Police&&U.Service==EAftershockMunicipalService::PolicePatrol;}));
			FAftershockCityLifeSaveState PopulationAfter;Life->CaptureSaveState(PopulationAfter);
			Check(TEXT("fleet_creation_adds_no_population_jobs_needs_or_economy_state"),Life->GetState().LivingPopulation==140&&FAftershockCityLifeSaveState::StaticStruct()->CompareScriptStruct(&PopulationBefore,&PopulationAfter,0));
			FAftershockTransportSaveState TrafficAfter;Transport->CaptureSaveState(TrafficAfter);
			Check(TEXT("derived_fleet_creation_changes_no_frozen_transport_fields"),FAftershockTransportSaveState::StaticStruct()->CompareScriptStruct(&TrafficBefore,&TrafficAfter,0)&&TrafficAfter.Vehicles.IsEmpty());
			for(FIntPoint Coord:{FIntPoint(20,6),FIntPoint(23,6)}){FGridCell Cell=Terrain->GetCell(Coord);Cell.bBurning=true;Terrain->SetCell(Coord,Cell);}
			for(auto& Unit:Transport->MunicipalFleet)Unit.CooldownSeconds=0.f;
			const auto Driver=[&](int32 Building){const auto* U=Transport->MunicipalFleet.FindByPredicate([&](const auto& Unit){return Unit.OwnerBuildingID==Building;});return U?U->ResidentID:INDEX_NONE;};
			const int32 DriverA=Driver(FireA),DriverB=Driver(FireB),CourtDriver=Driver(Court);
			Check(TEXT("emergency_staff_lookup_keeps_real_station_driver_and_excludes_court_courier"),Transport->IsMunicipalStaffResident(DriverA,FireA)&&!Transport->IsMunicipalStaffResident(CourtDriver,Court));
			Transport->MunicipalQueryFrame=MAX_uint64;Transport->MunicipalQueriesThisFrame=0;
			Check(TEXT("real_fire_cell_dispatches_existing_fire_station_employee"),Transport->TryBeginServiceDuty(DriverA,FireA));
			Check(TEXT("second_service_query_can_dispatch_without_overlap_at_another_station"),Transport->TryBeginServiceDuty(DriverB,FireB));
			const auto* CourtUnit=Transport->MunicipalFleet.FindByPredicate([&](const auto& U){return U.OwnerBuildingID==Court;});
			Check(TEXT("third_query_same_real_frame_is_rejected_without_setting_worker_cooldown"),CourtUnit&&
				!Transport->TryBeginServiceDuty(CourtDriver,Court)&&Transport->MunicipalQueriesThisFrame==2&&
				CourtUnit->CooldownSeconds==0.f);
			Check(TEXT("query_budget_is_shared_with_replanning_and_cancellation"),!Transport->TryAcquireMunicipalQueryBudget());
			FAftershockMunicipalDutyView Duty;Check(TEXT("duty_view_reserves_only_real_driver_without_passenger_trip"),Transport->GetMunicipalDuty(DriverA,Duty)&&Duty.ResidentID==DriverA&&Duty.OwnerBuildingID==FireA&&Duty.bRiding&&Transport->Trips.IsEmpty());
			auto* UnitA=Transport->MunicipalFleet.FindByPredicate([&](const auto& U){return U.OwnerBuildingID==FireA;});
			if(UnitA)
			{
				bool RouteValid=UnitA->bDeployed&&Terrain->GetCell(UnitA->IncidentCell).bBurning&&!UnitA->Vehicle.RoadPath.IsEmpty()&&UnitA->Vehicle.RoadPath.Last()==UnitA->HomeRoadCell;
				FIntPoint Previous=UnitA->HomeRoadCell;for(FIntPoint Step:UnitA->Vehicle.RoadPath){RouteValid&=FMath::Abs(Step.X-Previous.X)+FMath::Abs(Step.Y-Previous.Y)==1&&Roads->CanTraverseRoadEdge(Previous,Step);Previous=Step;}
				Check(TEXT("incident_route_and_return_are_continuous_real_safe_road_edges"),RouteValid);
				FAftershockRoadVehicle Private;Private.VehicleID=100001;Private.ResidentOwnerID=DriverA;Private.Location=UnitA->Vehicle.Location;Private.Yaw=UnitA->Vehicle.Yaw;
				Check(TEXT("ordinary_car_cannot_spawn_inside_deployed_municipal_body"),!Transport->CanMoveVehicle(Private,Private.Location,Private.Yaw));
				Transport->Vehicles.Add(Private);Check(TEXT("municipal_body_cannot_move_through_private_car"),!Transport->CanMoveVehicle(UnitA->Vehicle,UnitA->Vehicle.Location,UnitA->Vehicle.Yaw));Transport->Vehicles.Reset();
				Check(TEXT("service_body_blocks_pedestrian_admission_without_extra_npc"),!Transport->IsPedestrianSpaceClear(UnitA->Vehicle.Location,UnitA->Vehicle.Location,32.f,false));
				if(UnitA->Vehicle.RoadPath.IsValidIndex(UnitA->Vehicle.NextPathStep))
				{
					const FVector Before=UnitA->Vehicle.Location;const FIntPoint Blocked=UnitA->Vehicle.RoadPath[UnitA->Vehicle.NextPathStep];FGridCell Cell=Terrain->GetCell(Blocked);const FGridCell Original=Cell;Cell.bHasRoad=false;Terrain->SetCell(Blocked,Cell);
					UnitA->CooldownSeconds=1.f;Transport->UpdateMunicipalFleet();
					Check(TEXT("new_road_block_holds_real_vehicle_and_does_not_teleport_crew"),UnitA->Vehicle.Location.Equals(Before,.001f)&&Transport->GetMunicipalDuty(DriverA,Duty));Terrain->SetCell(Blocked,Original);
				}
			}
			// Keep one physical circuit in this movement fixture. Cancelling the
			// second via the supported building lifecycle removes its display unit.
			Buildings->ApplyDamageToBuilding(FireB,90.f);Life->RefreshCity();Transport->RefreshInfrastructure();
			Check(TEXT("collapsed_station_retires_owned_fleet_and_releases_derived_crew"),!Transport->MunicipalFleet.ContainsByPredicate([&](const auto& U){return U.OwnerBuildingID==FireB;})&&!Transport->GetMunicipalDuty(DriverB,Duty));
			for(int32 Step=0;Step<6000&&Transport->GetMunicipalDuty(DriverA,Duty);++Step)Transport->UpdateMunicipalFleet();
			UnitA=Transport->MunicipalFleet.FindByPredicate([&](const auto& U){return U.OwnerBuildingID==FireA;});
			Check(TEXT("same_vehicle_returns_on_road_and_releases_worker_at_owned_station"),UnitA&&!UnitA->bDeployed&&UnitA->Vehicle.ParkedAtBuildingID==FireA&&Terrain->WorldToGrid(UnitA->Vehicle.Location)==UnitA->HomeRoadCell&&!Transport->GetMunicipalDuty(DriverA,Duty));
			Check(TEXT("parked_municipal_vehicle_releases_previous_route_buffer_capacity"),UnitA&&!UnitA->bDeployed&&UnitA->Vehicle.RoadPath.GetAllocatedSize()==0);
			FAftershockTransportSaveState BeforeRestore;Transport->CaptureSaveState(BeforeRestore);FText Error;
			Check(TEXT("frozen_transport_restore_accepts_unchanged_canonical_snapshot"),Transport->RestoreSaveState(BeforeRestore,Error));
			FAftershockTransportSaveState AfterRestore;Transport->CaptureSaveState(AfterRestore);
			Check(TEXT("restore_clears_only_derived_fleet_and_preserves_all_frozen_values"),Transport->MunicipalFleet.IsEmpty()&&Transport->bMunicipalRebuildPending&&FAftershockTransportSaveState::StaticStruct()->CompareScriptStruct(&BeforeRestore,&AfterRestore,0));
			Buildings->GetAllBuildings(Records);Transport->RefreshMunicipalFleet(Records);
			const int32 PriorLiving=Life->GetState().LivingPopulation;const auto Dead=Life->ApplyCasualties(FireA,1,true);Transport->RefreshMunicipalFleet(Records);
			Check(TEXT("canonical_death_retires_old_driver_without_adding_or_replaying_casualties"),Dead.Contains(DriverA)&&Life->GetState().LivingPopulation==PriorLiving-1&&!Transport->MunicipalFleetByResident.Contains(DriverA));
			Buildings->RemoveBuilding(Tax);Life->RefreshCity();Transport->RefreshInfrastructure();
			Check(TEXT("demolition_retires_only_that_building_owned_fleet"),!Transport->MunicipalFleet.ContainsByPredicate([&](const auto& U){return U.OwnerBuildingID==Tax;}));
			// Real Crowd integration: isolate ONE already-canonical display
			// representative, never manufacture a service resident or bus trip.
			auto* Crowd=World->GetSubsystem<UCrowdRenderSubsystem>();Check(TEXT("crowd_integration_subsystem_available"),Crowd!=nullptr);
			if(Crowd)
			{
				Crowd->SynchronizeCanonicalPopulation();TArray<FCitizenAgent> FullRoster=Crowd->Agents;
				auto* IntegrationUnit=Transport->MunicipalFleet.FindByPredicate([&](const auto& U){return U.OwnerBuildingID==FireA;});
				const int32 IntegrationDriver=IntegrationUnit?IntegrationUnit->ResidentID:INDEX_NONE;
				const auto* ExistingAgent=FullRoster.FindByPredicate([&](const auto& A){return A.ResidentID==IntegrationDriver;});
				Check(TEXT("service_fixture_driver_is_actual_canonical_crowd_representative"),IntegrationUnit&&ExistingAgent);
				if(IntegrationUnit&&ExistingAgent)
				{
					Crowd->Agents.Reset();Crowd->Agents.Add(*ExistingAgent);auto& Agent=Crowd->Agents[0];
					Agent.Activity=EAftershockCitizenActivity::AtWork;Agent.ActivityAfterArrival=EAftershockCitizenActivity::AtWork;
					Agent.CurrentLocation=Agent.HomeLocation;Agent.TargetLocation=Agent.CurrentLocation;Agent.FinalDestinationLocation=Agent.CurrentLocation;
					Agent.OutdoorDepartureBuildingID=Agent.HomeBuildingID;Agent.OutdoorDepartureLocation=Agent.CurrentLocation;
					Agent.MoveSpeed=0.f;Agent.TransportTripID=INDEX_NONE;Agent.bInsideVehicle=false;Agent.bWaitingForOutdoorSpace=false;
					Agent.ScheduleSecondsRemaining=60.f;Agent.RouteGridCoords.Reset();Agent.NextRouteCoordIndex=0;
					IntegrationUnit->CooldownSeconds=0.f;Transport->MunicipalQueryFrame=MAX_uint64;Transport->MunicipalQueriesThisFrame=0;
					const FVector OldEmployerAnchor=Agent.CurrentLocation;Crowd->UpdateCrowdPositions();
					Check(TEXT("at_work_label_at_another_real_indoor_anchor_cannot_dispatch_remote_station_vehicle"),
						!Transport->GetMunicipalDuty(IntegrationDriver,Duty)&&Transport->MunicipalQueriesThisFrame==0&&Agent.CurrentLocation.Equals(OldEmployerAnchor,.001f));
					Agent.CurrentLocation=Agent.WorkLocation;Agent.TargetLocation=Agent.CurrentLocation;Agent.FinalDestinationLocation=Agent.CurrentLocation;
					Agent.OutdoorDepartureBuildingID=FireA;Agent.OutdoorDepartureLocation=Agent.CurrentLocation;Agent.MoveSpeed=0.f;
					IntegrationUnit->CooldownSeconds=0.f;Crowd->UpdateCrowdPositions();
					Check(TEXT("actual_worker_at_its_own_real_station_anchor_dispatches_through_crowd_callback"),Transport->GetMunicipalDuty(IntegrationDriver,Duty)&&Agent.Activity==EAftershockCitizenActivity::AtWork);
					for(int32 Step=0;Step<45;++Step)Transport->UpdateMunicipalFleet();
					FBuildingInstance Station;Buildings->GetBuildingByID(FireA,Station);const FGridCell StationOriginal=Terrain->GetCell(Station.GridCoords);
					FGridCell BurningStation=StationOriginal;BurningStation.bBurning=true;Terrain->SetCell(Station.GridCoords,BurningStation);
					const FVector FrozenWorkAnchor=Agent.CurrentLocation;const int32 LivingBeforeDuty=Life->GetState().LivingPopulation;
					Crowd->BeginEmergencyEvacuation(EDisasterType::Fire);Crowd->UpdateCrowdPositions();
					IntegrationUnit=Transport->MunicipalFleet.FindByPredicate([&](const auto& U){return U.OwnerBuildingID==FireA;});
					FTransform PersonPose;const bool BodyHidden=Crowd->CrowdHostActor&&Crowd->CrowdHostActor->CitizenISM&&Crowd->CrowdHostActor->CitizenISM->GetInstanceTransform(0,PersonPose,true)&&PersonPose.GetScale3D().IsNearlyZero();
					Check(TEXT("unsafe_station_keeps_active_crew_exclusive_hidden_without_duplicate_walker_or_shelter_slot"),
						Transport->GetMunicipalDuty(IntegrationDriver,Duty)&&IntegrationUnit&&IntegrationUnit->bCancellationRequested&&BodyHidden&&
						Agent.CurrentLocation.Equals(FrozenWorkAnchor,.001f)&&Agent.Activity==EAftershockCitizenActivity::AtWork&&Agent.MoveSpeed==0.f&&!Agent.bInsideVehicle&&Agent.TransportTripID==INDEX_NONE&&!Agent.bEmergencyPlaceReserved);
					const int32 PriorStep=IntegrationUnit?IntegrationUnit->Vehicle.NextPathStep:INDEX_NONE;
					const TArray<FIntPoint> PriorPath=IntegrationUnit?IntegrationUnit->Vehicle.RoadPath:TArray<FIntPoint>();const uint8 PriorQueries=Transport->MunicipalQueriesThisFrame;
					Crowd->UpdateCrowdPositions();
					Check(TEXT("repeated_unsafe_crowd_callback_does_not_restart_return_path_or_repeat_query"),IntegrationUnit&&IntegrationUnit->bCancellationRequested&&IntegrationUnit->Vehicle.NextPathStep==PriorStep&&IntegrationUnit->Vehicle.RoadPath==PriorPath&&Transport->MunicipalQueriesThisFrame==PriorQueries);
					for(int32 Step=0;Step<6000&&Transport->GetMunicipalDuty(IntegrationDriver,Duty);++Step)Transport->UpdateMunicipalFleet();
					Check(TEXT("cancelled_service_duty_releases_only_after_actual_road_return"),!Transport->GetMunicipalDuty(IntegrationDriver,Duty));
					Crowd->UpdateCrowdPositions();
					Check(TEXT("released_worker_at_unsafe_station_enters_civilian_evacuation_instead_of_freezing_at_work"),Agent.Activity!=EAftershockCitizenActivity::AtWork&&!Transport->GetMunicipalDuty(IntegrationDriver,Duty)&&Life->GetState().LivingPopulation==LivingBeforeDuty);
					Terrain->SetCell(Station.GridCoords,StationOriginal);Crowd->SetPanicMode(false);
				}
				Crowd->Agents=MoveTemp(FullRoster);Crowd->RebuildCitizenInstanceTransforms();Crowd->RebuildCitizenSpatialIndex();
				// Deliberately place actual employees in the final household's late
				// resident ranks: unmodified household-only sampling excludes them.
				Place(EBuildingType::House,FIntPoint(1,4),1000,0);Place(EBuildingType::House,FIntPoint(2,4),1000,0);Place(EBuildingType::House,FIntPoint(4,4),1000,0);
				Life->RefreshCity();FAftershockCityLifeSaveState LargeCity;Life->CaptureSaveState(LargeCity);
				for(auto& Resident:LargeCity.Residents)Resident.WorkBuildingID=INDEX_NONE;
				const int32 Employers[]={FireA,Hospital,Police,Court};int32 Assigned=0;
				for(int32 I=LargeCity.Residents.Num()-1;I>=0&&Assigned<16;--I)if(LargeCity.Residents[I].bAlive&&LargeCity.Residents[I].bWorkingAge)
				{LargeCity.Residents[I].WorkBuildingID=Employers[Assigned/4];++Assigned;}
				LargeCity.State.EmployedResidents=Assigned;
				for(int32 Employer:Employers)if(auto* Status=LargeCity.BuildingStates.Find(Employer))
				{Status->AssignedWorkers=4;Status->Staffing01=1.f;Status->Operation01=1.f;Status->bOperating=true;}
				Check(TEXT("large_city_fixture_reassigns_sixteen_existing_workers_without_creating_residents"),Assigned==16&&Life->RestoreSaveState(LargeCity,Error));
				Transport->ResetMunicipalPresentationAfterRestore();Transport->RefreshInfrastructure();TArray<int32> ExpectedDrivers;Transport->GetMunicipalStaffResidentIDs(ExpectedDrivers);
				FAftershockCityLifeSaveState BeforeLargeRoster;Life->CaptureSaveState(BeforeLargeRoster);Crowd->SynchronizeCanonicalPopulation();
				TSet<int32> RenderedIDs;bool UniqueCanonical=true;for(const auto& Agent:Crowd->Agents)
				{FCityResidentRecord Real;UniqueCanonical&=!RenderedIDs.Contains(Agent.ResidentID)&&Life->GetResident(Agent.ResidentID,Real)&&Real.bAlive;RenderedIDs.Add(Agent.ResidentID);}
				bool LateDriversIncluded=ExpectedDrivers.Num()==4;for(int32 ID:ExpectedDrivers)LateDriversIncluded&=ID>3000&&RenderedIDs.Contains(ID);
				Check(TEXT("large_city_keeps_late_household_canonical_service_drivers_inside_unchanged_1500_display_budget"),Life->GetState().LivingPopulation>3000&&Crowd->Agents.Num()==1500&&UniqueCanonical&&LateDriversIncluded);
				FAftershockCityLifeSaveState AfterLargeRoster;Life->CaptureSaveState(AfterLargeRoster);
				Check(TEXT("municipal_roster_priority_changes_no_canonical_population_jobs_needs_or_fiscal_state"),FAftershockCityLifeSaveState::StaticStruct()->CompareScriptStruct(&BeforeLargeRoster,&AfterLargeRoster,0));
			}
		}
		World->DestroyWorld(false);GEngine->DestroyWorldContext(World);
	}
	auto Report=MakeShared<FJsonObject>();Report->SetBoolField(TEXT("valid"),Passed);Report->SetNumberField(TEXT("passed"),Checks-Failures);Report->SetNumberField(TEXT("failed"),Failures);Report->SetNumberField(TEXT("checks"),Checks);Report->SetNumberField(TEXT("failures"),Failures);Report->SetArrayField(TEXT("results"),Results);
	Report->SetStringField(TEXT("scope"),TEXT("Native canonical-worker/derived municipal fleet fixture; no RHI or packaged visual acceptance"));
	FString Output;FParse::Value(*Params,TEXT("Output="),Output);if(Output.IsEmpty())Output=FPaths::ProjectSavedDir()/TEXT("PresentationValidation/municipal_audit.json");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Output),true);FString Json;auto Writer=TJsonWriterFactory<>::Create(&Json);FJsonSerializer::Serialize(Report,Writer);
	if(!FFileHelper::SaveStringToFile(Json,*Output))return 1;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MUNICIPAL_AUDIT result=%s checks=%d failures=%d report=%s"),Passed?TEXT("PASS"):TEXT("FAIL"),Checks,Failures,*Output);return Passed?0:1;
}
