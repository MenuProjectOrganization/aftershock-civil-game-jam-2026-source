#include "AftershockCitizenPlanningCommandlet.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"

UAftershockCitizenPlanningCommandlet::UAftershockCitizenPlanningCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UAftershockCitizenPlanningCommandlet::Main(const FString& Params)
{
	if (!GEngine) return 1;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, FName(TEXT("CitizenPlanningFixture")),
		GetTransientPackage(), true, ERHIFeatureLevel::SM5, nullptr, false);
	if (!World) return 1;
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>();
	UBuildingManagerSubsystem* Buildings = World->GetSubsystem<UBuildingManagerSubsystem>();
	UCrowdRenderSubsystem* Crowd = World->GetSubsystem<UCrowdRenderSubsystem>();
	UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>();
	bool Passed = Terrain && Buildings && Crowd && Economy;
	const auto Check = [&Passed](const TCHAR* Name, bool Condition)
	{
		Passed &= Condition;
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CITIZEN_CHECK case=%s passed=%d"), Name, Condition);
	};
	if (Passed)
	{
		// A deliberately disconnected river district proves that an unreachable
		// centre is reported instead of crossing the river or inventing a bridge.
		Terrain->GenerateProceduralTerrain(16,10,500.f,1337);
		for (int32 Y = 0; Y < 10; ++Y)
		{
			for (int32 X = 0; X < 16; ++X)
			{
				FGridCell Cell;
				Cell.Coords = FIntPoint(X, Y); Cell.Elevation = static_cast<float>(Y * 3);
				Cell.bIsPlayable = true; Cell.bIsRiverbed = X == 7;
				Cell.bHasRoad = Y == 4 && X != 7;
				Terrain->Cells[Terrain->CoordToIndex(Cell.Coords)] = Cell;
			}
		}
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		const auto Place = [Buildings, Cube, &Check](const TCHAR* Name, EBuildingType Type, FIntPoint Coord, int32 Residents, int32 Workers)
		{
			UBuildingData* Data = NewObject<UBuildingData>(Buildings);
			Data->DisplayName = FText::FromString(Name); Data->Type = Type;
			Data->Residents = Residents; Data->Workers = Workers; Data->MeshIntact = Cube;
			int32 ID = INDEX_NONE;
			Check(Name, Buildings->PlaceBuilding(Coord, Data, EFoundationQuality::Medium, 1, ID));
			return ID;
		};
		const int32 HomeLeft = Place(TEXT("home_left"), EBuildingType::House, FIntPoint(2, 2), 20, 0);
		const int32 HomeRight = Place(TEXT("home_right"), EBuildingType::Apartment, FIntPoint(12, 2), 20, 0);
		Place(TEXT("office"), EBuildingType::Office, FIntPoint(5, 2), 0, 8);
		const int32 School = Place(TEXT("school"), EBuildingType::School, FIntPoint(2, 6), 0, 4);
		const int32 Hospital = Place(TEXT("hospital"), EBuildingType::Hospital, FIntPoint(4, 6), 0, 2);
		const int32 Fire = Place(TEXT("fire_station"), EBuildingType::FireStation, FIntPoint(5, 8), 0, 2);
		Place(TEXT("public_park"), EBuildingType::GreenSpace, FIntPoint(3, 8), 0, 0);
		const int64 InitialTreasury = Economy->TreasuryTL;
		const float InitialSatisfaction = Economy->PublicSatisfaction;
		Crowd->SynchronizeCanonicalPopulation();
		Check(TEXT("canonical_40_residents_16_workers_half_representatives"), Crowd->GetCurrentCrowdCount() == 28);
		bool ValidIdentities = true;
		TArray<TPair<int32, int32>> Identities;
		for (const FCitizenAgent& Agent : Crowd->Agents)
		{
			ValidIdentities &= (Agent.HomeBuildingID == HomeLeft || Agent.HomeBuildingID == HomeRight) && Agent.WorkBuildingID != INDEX_NONE;
			Identities.Emplace(Agent.HomeBuildingID, Agent.WorkBuildingID);
		}
		Check(TEXT("actual_home_and_work_building_ids"), ValidIdentities);
		int32 RoadSteps = 0, WorkArrivals = 0, WaitingForRoute = 0;
		for (int32 Tick = 0; Tick < 300; ++Tick)
		{
			Crowd->UpdateCrowdPositions();
			for (const FCitizenAgent& Agent : Crowd->Agents)
			{
				for (const FIntPoint Coord : Agent.RouteGridCoords) RoadSteps += Terrain->GetCell(Coord).bHasRoad ? 1 : 0;
				WorkArrivals += Agent.Activity == EAftershockCitizenActivity::AtWork ? 1 : 0;
				WaitingForRoute += Agent.Activity == EAftershockCitizenActivity::WaitingForRoute ? 1 : 0;
			}
		}
		Check(TEXT("normal_schedule_walks_player_roads_and_reaches_work"), RoadSteps > 0 && WorkArrivals > 0);
		Check(TEXT("disconnected_jobs_do_not_fake_work_arrival"), WaitingForRoute > 0);
		bool StableIDs = Crowd->Agents.Num() == Identities.Num();
		for (int32 A = 0; A < Crowd->Agents.Num(); ++A)
			StableIDs &= Crowd->Agents[A].HomeBuildingID == Identities[A].Key && Crowd->Agents[A].WorkBuildingID == Identities[A].Value;
		Check(TEXT("home_work_assignments_persist"), StableIDs);
		Crowd->BeginEmergencyEvacuation(EDisasterType::Flood);
		const FAftershockCitizenPlanningStats First = Crowd->GetCitizenPlanningStats();
		Check(TEXT("only_real_intact_service_centres_eligible"), First.EligibleCentres == 3);
		Check(TEXT("explicit_visual_quota_never_overbooked"), First.VisualPlaceQuota == 4 && First.Reserved == 4);
		Check(TEXT("full_centres_have_distinct_overflow"), First.Overflow > 0);
		Check(TEXT("river_isolated_district_reports_unreachable_centre"), First.CentreUnreachable > 0);
		const TArray<FCitizenAgent> Before = Crowd->GetCitizenDebugRoster();
		Crowd->BeginEmergencyEvacuation(EDisasterType::Flood);
		bool Deterministic = Before.Num() == Crowd->Agents.Num();
		for (int32 A = 0; A < Crowd->Agents.Num(); ++A)
		{
			const FCitizenAgent& Agent = Crowd->Agents[A];
			Deterministic &= Agent.EmergencyBuildingID == Before[A].EmergencyBuildingID
				&& Agent.bEmergencyPlaceReserved == Before[A].bEmergencyPlaceReserved
				&& Agent.bEmergencyOverflow == Before[A].bEmergencyOverflow
				&& Agent.RouteGridCoords == Before[A].RouteGridCoords;
		}
		Check(TEXT("repeat_warning_keeps_deterministic_assignments_and_routes"), Deterministic);
		const auto RoutesValid = [Terrain, Crowd]()
		{
			for (const FCitizenAgent& Agent : Crowd->Agents)
			{
				FIntPoint Previous = Terrain->WorldToGrid(Agent.CurrentLocation);
				for (int32 Step = Agent.NextRouteCoordIndex; Step < Agent.RouteGridCoords.Num(); ++Step)
				{
					const FIntPoint Coord = Agent.RouteGridCoords[Step];
					if (!Terrain->IsPlayableCoord(Coord)) return false;
					const FGridCell Cell = Terrain->GetCell(Coord);
					if (Cell.bIsRiverbed || Cell.bBurning || Cell.OccupyingBuildingID != INDEX_NONE) return false;
					const int32 Distance = FMath::Abs(Coord.X - Previous.X) + FMath::Abs(Coord.Y - Previous.Y);
					// Mid-step feet can already be inside the next canonical cell.
					if (Distance > 1) return false;
					Previous = Coord;
				}
			}
			return true;
		};
		Check(TEXT("warning_paths_use_valid_adjacent_public_land"), RoutesValid());
		// Force one ordinary road cell to burn; the existing grid remains the authority.
		Terrain->Cells[Terrain->CoordToIndex(FIntPoint(3, 4))].bBurning = true;
		Crowd->BeginEmergencyEvacuation(EDisasterType::Fire);
		Check(TEXT("burning_road_is_not_a_through_route"), RoutesValid());
		FBuildingInstance Closed;
		Buildings->GetBuildingByID(Hospital, Closed); Closed.DamageState = EDamageState::Collapsed; Closed.CurrentDamage = 100.f;
		Buildings->UpdateBuildingInstance(Closed);
		Crowd->BeginEmergencyEvacuation(EDisasterType::Earthquake);
		bool NoClosedCentre = true;
		for (const FCitizenAgent& Agent : Crowd->Agents) NoClosedCentre &= Agent.EmergencyBuildingID != Hospital;
		Check(TEXT("collapsed_hospital_removed_from_assignments"), NoClosedCentre && Crowd->GetCitizenPlanningStats().VisualPlaceQuota == 3);
		TMap<int32,FVector> ReservedGoals;for(int32 I=0;I<Crowd->Agents.Num();++I)if(Crowd->Agents[I].bEmergencyPlaceReserved)ReservedGoals.Add(I,Crowd->Agents[I].FinalDestinationLocation);
		for (int32 Tick = 0; Tick < 600; ++Tick) Crowd->UpdateCrowdPositions();
		const FAftershockCitizenPlanningStats Arrival = Crowd->GetCitizenPlanningStats();
		if(Arrival.ArrivedAtCentre!=Arrival.Reserved)for(const auto& Agent:Crowd->Agents)if(Agent.bEmergencyPlaceReserved)
		{
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITIZEN_PENDING_CENTRE building=%d activity=%d at=%s target=%s final=%s distanceCm=%.2f next=%d/%d routeReaches=%d waitingDeparture=%d"),Agent.EmergencyBuildingID,static_cast<int32>(Agent.Activity),*Agent.CurrentLocation.ToCompactString(),*Agent.TargetLocation.ToCompactString(),*Agent.FinalDestinationLocation.ToCompactString(),FVector::Dist(Agent.CurrentLocation,Agent.FinalDestinationLocation),Agent.NextRouteCoordIndex,Agent.RouteGridCoords.Num(),Agent.bRouteReachesDestination,Agent.bWaitingForOutdoorSpace);
			if(Agent.Activity==EAftershockCitizenActivity::Evacuating)for(int32 I=0;I<Crowd->Agents.Num();++I)
			{
				const auto& Other=Crowd->Agents[I];if(&Other==&Agent||Other.bInsideVehicle||FVector::Dist2D(Other.CurrentLocation,Agent.FinalDestinationLocation)>160.f)continue;
				UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITIZEN_DOOR_BODY index=%d activity=%d reserved=%d overflow=%d moving=%.2f centre=%d at=%s toDoorCm=%.2f"),I,static_cast<int32>(Other.Activity),Other.bEmergencyPlaceReserved,Other.bEmergencyOverflow,Other.MoveSpeed,Other.EmergencyBuildingID,*Other.CurrentLocation.ToCompactString(),FVector::Dist2D(Other.CurrentLocation,Agent.FinalDestinationLocation));
			}
		}
		Check(TEXT("representatives_reach_reserved_centres"), Arrival.ArrivedAtCentre == Arrival.Reserved && Arrival.Reserved > 0);
		bool ExactReservedGoals=true;for(const auto& Pair:ReservedGoals)ExactReservedGoals&=Crowd->Agents.IsValidIndex(Pair.Key)&&Crowd->Agents[Pair.Key].bEmergencyPlaceReserved&&Crowd->Agents[Pair.Key].FinalDestinationLocation.Equals(Pair.Value,.01f);
		Check(TEXT("reserved_centre_identity_and_exact_goal_survive_forecourt_yielding"),ExactReservedGoals&&ReservedGoals.Num()==Arrival.Reserved);
		Check(TEXT("arrived_real_reserved_centre_occupants_do_not_block_public_doorway"),Arrival.ArrivedAtCentre>0&&Crowd->GetCrowdSpacingStats().IndoorAgents>=Arrival.ArrivedAtCentre);
		Check(TEXT("overflow_waits_without_false_admission"), Arrival.WaitingAtAssembly > 0 && Arrival.UnsafeRemainingRouteSteps == 0);
		Buildings->RemoveBuilding(School); Buildings->RemoveBuilding(Hospital); Buildings->RemoveBuilding(Fire);
		Crowd->BeginEmergencyEvacuation(EDisasterType::Earthquake);
		const FAftershockCitizenPlanningStats Missing = Crowd->GetCitizenPlanningStats();
		Check(TEXT("no_centres_use_public_assembly_not_fake_shelter"), Missing.Reserved == 0 && Missing.CentreUnreachable == Crowd->Agents.Num() && RoutesValid());
		Check(TEXT("navigation_never_changes_economy_or_satisfaction"), Economy->TreasuryTL == InitialTreasury && Economy->PublicSatisfaction == InitialSatisfaction);
		Check(TEXT("single_zero_tick_ism_owner"), !Crowd->GetOrCreateHostActor()->CanEverTick()
			&& Crowd->GetOrCreateHostActor()->CitizenISM->GetInstanceCount() == Crowd->GetCurrentCrowdCount());
		Crowd->AuditCitizenPlanning();
	}
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CITIZEN_PLANNING_RESULT passed=%d fixtureOnly=1 economyWrites=0 casualtyWrites=0"), Passed);
	World->DestroyWorld(false); GEngine->DestroyWorldContext(World); World->RemoveFromRoot();
	return Passed ? 0 : 1;
}
