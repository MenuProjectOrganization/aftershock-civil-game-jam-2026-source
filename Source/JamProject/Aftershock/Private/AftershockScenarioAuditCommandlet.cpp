#include "AftershockScenarioAuditCommandlet.h"

#if WITH_EDITOR
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/WorldClimateSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "AftershockTerrainSurface.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace
{
	struct FScenarioCheck { FString Name; bool Passed; FString Detail; };
	struct FScopedScenarioWorld
	{
		UWorld* World = nullptr;
		~FScopedScenarioWorld()
		{
			if (!World) return;
			World->DestroyWorld(false);
			if (GEngine) GEngine->DestroyWorldContext(World);
			World->RemoveFromRoot();
		}
	};
	FString JsonEscape(FString Value)
	{
		Value.ReplaceInline(TEXT("\\"), TEXT("\\\\")); Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Value.ReplaceInline(TEXT("\n"), TEXT("\\n")); Value.ReplaceInline(TEXT("\r"), TEXT("\\r")); Value.ReplaceInline(TEXT("\t"), TEXT("\\t"));
		return Value;
	}

	FString ProbeFloodDisplay(UWorld& World, UTerrainGridSubsystem& Terrain, UBuildingManagerSubsystem& Buildings,
		URoadNetworkSubsystem& Roads, UDisasterSimulationSubsystem& Simulation, const UAftershockScenarioSubsystem& Scenario,
		int32& OutWetHomes, int32& OutWetCentres, bool& bOutBuilt)
	{
		const float Rainfall = Scenario.GetDefinition().BaseSeverity;
		const TMap<FIntPoint, float> Severity = Simulation.SimulateFloodMap(Rainfall);
		FActorSpawnParameters Spawn;
		Spawn.ObjectFlags |= RF_Transient;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* ProbeOwner = World.SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/Aftershock/Materials/M_DisasterFloodVolume.M_DisasterFloodVolume"));
		UStaticMeshComponent* Surface = ProbeOwner ? NewObject<UStaticMeshComponent>(ProbeOwner, NAME_None, RF_Transient) : nullptr;
		if (Surface)
		{
			ProbeOwner->AddInstanceComponent(Surface);
			Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Surface->SetMaterial(0, Material);
		}
		FAftershockFloodSurfaceStats Stats;
		TMap<FIntPoint, float> Depths;
		const bool bBuilt = Surface && Material && BuildAftershockFloodSurface(ProbeOwner, &Terrain, Severity, Surface, &Stats, &Depths);
		TArray<FBuildingInstance> Records;
		Buildings.GetAllBuildings(Records);
		FBox CityBounds(ForceInit);
		UBuildingData* HouseData = nullptr;
		for (const FBuildingInstance& B : Records)
		{
			CityBounds += Terrain.GridToWorld(B.GridCoords);
			if (B.BuildingData && B.BuildingData->Type == EBuildingType::House) HouseData = B.BuildingData;
		}
		const FVector CityCentre = CityBounds.IsValid ? CityBounds.GetCenter() : FVector::ZeroVector;
		const FIntPoint AssemblyRoot = Scenario.GetAssemblyCells().IsEmpty() ? FIntPoint(INDEX_NONE, INDEX_NONE) : Scenario.GetAssemblyCells()[0];
		const FIntPoint Directions[] = { FIntPoint(1,0), FIntPoint(-1,0), FIntPoint(0,1), FIntPoint(0,-1) };
		const auto RouteCostToFrontage = [&Roads, AssemblyRoot, &Directions](FIntPoint Coord, int32& OutLength)
		{
			int32 BestCost = MAX_int32;
			OutLength = INDEX_NONE;
			for (FIntPoint Direction : Directions)
			{
				const FAftershockRoadQuote Quote = Roads.QuoteRoad(AssemblyRoot, Coord + Direction);
				// For an empty candidate, do not propose an access route that would
				// itself be occupied by the house after the possible relocation.
				if (Quote.bRouteValid && !Quote.Path.Contains(Coord) && Quote.CostTL < BestCost) { BestCost = Quote.CostTL; OutLength = Quote.Path.Num(); }
			}
			return BestCost == MAX_int32 ? INDEX_NONE : BestCost;
		};
		int32 WetHomes = 0, TotalHomes = 0, WetCentres = 0, TotalCentres = 0;
		FString BuildingJson = TEXT("[");
		for (int32 I = 0; I < Records.Num(); ++I)
		{
			const FBuildingInstance& B = Records[I];
			const EBuildingType Type = B.BuildingData ? B.BuildingData->Type : EBuildingType::House;
			const bool bHome = Type == EBuildingType::House || Type == EBuildingType::Apartment;
			const bool bCentre = Type == EBuildingType::Hospital || Type == EBuildingType::School || Type == EBuildingType::FireStation || Type == EBuildingType::Shelter;
			const float Depth = Depths.FindRef(B.GridCoords);
			if (bHome) { ++TotalHomes; if (Depth > 12.f) ++WetHomes; }
			if (bCentre) { ++TotalCentres; if (Depth > 12.f) ++WetCentres; }
			const FGridCell Cell = Terrain.GetCell(B.GridCoords);
			int32 Length = INDEX_NONE;
			const int32 Cost = RouteCostToFrontage(B.GridCoords, Length);
			BuildingJson += FString::Printf(TEXT("%s{\"id\":%d,\"type\":%d,\"coord\":[%d,%d],\"display_depth_cm\":%.3f,\"elevation_cm\":%.3f,\"river_distance_cm\":%.3f,\"frontage_road_quote_tl\":%d,\"frontage_path_cells\":%d}"),
				I ? TEXT(",") : TEXT(""), B.InstanceID, static_cast<int32>(Type), B.GridCoords.X, B.GridCoords.Y, Depth, Cell.Elevation, Cell.RiverDistance, Cost, Length);
		}
		BuildingJson += TEXT("]");
		TArray<FIntPoint> Candidates;
		for (const FGridCell& Cell : Terrain.Cells)
		{
			if (Cell.bIsPlayable && !Cell.bIsRiverbed && Cell.OccupyingBuildingID == INDEX_NONE && Depths.FindRef(Cell.Coords) > 12.f)
				Candidates.Add(Cell.Coords);
		}
		Candidates.Sort([&Terrain, CityCentre](FIntPoint A, FIntPoint B)
		{
			const double DA = FVector::DistSquared2D(Terrain.GridToWorld(A), CityCentre), DB = FVector::DistSquared2D(Terrain.GridToWorld(B), CityCentre);
			if (!FMath::IsNearlyEqual(DA, DB)) return DA < DB;
			return A.Y == B.Y ? A.X < B.X : A.Y < B.Y;
		});
		const FIntPoint LotParity = Scenario.GetDesignedLots().IsEmpty() ? FIntPoint::ZeroValue : Scenario.GetDesignedLots()[0].Coord;
		FString CandidateJson = TEXT("[");
		int32 ReachableWetCandidates = 0;
		for (int32 I = 0; I < Candidates.Num(); ++I)
		{
			const FIntPoint Coord = Candidates[I];
			const FGridCell Cell = Terrain.GetCell(Coord);
			const bool bCanPlace = HouseData && Buildings.CanPlaceBuildingAt(Coord, HouseData);
			const bool bParity = Coord.X % 2 == LotParity.X % 2 && Coord.Y % 2 == LotParity.Y % 2;
			bool bRing = true;
			for (int32 DY = -1; DY <= 1 && bRing; ++DY)
				for (int32 DX = -1; DX <= 1; ++DX)
				{
					const FIntPoint Neighbor = Coord + FIntPoint(DX, DY);
					if (!Terrain.IsPlayableCoord(Neighbor) || Terrain.GetCell(Neighbor).bIsRiverbed
						|| ((DX || DY) && Terrain.GetCell(Neighbor).OccupyingBuildingID != INDEX_NONE)) { bRing = false; break; }
				}
			int32 Length = INDEX_NONE;
			const int32 Cost = RouteCostToFrontage(Coord, Length);
			const double CityDistance = FVector::Dist2D(Terrain.GridToWorld(Coord), CityCentre);
			if (bCanPlace && Cost != INDEX_NONE) ++ReachableWetCandidates;
			CandidateJson += FString::Printf(TEXT("%s{\"coord\":[%d,%d],\"display_depth_cm\":%.3f,\"elevation_cm\":%.3f,\"river_distance_cm\":%.3f,\"city_distance_cm\":%.3f,\"can_place_house\":%s,\"matches_lot_parity\":%s,\"public_land_ring\":%s,\"frontage_road_quote_tl\":%d,\"frontage_path_cells\":%d}"),
				I ? TEXT(",") : TEXT(""), Coord.X, Coord.Y, Depths.FindRef(Coord), Cell.Elevation, Cell.RiverDistance, CityDistance,
				bCanPlace ? TEXT("true") : TEXT("false"), bParity ? TEXT("true") : TEXT("false"), bRing ? TEXT("true") : TEXT("false"), Cost, Length);
			if (I < 48) UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_SCENARIO_FLOOD_CANDIDATE coord=(%d,%d) displayDepthCm=%.2f elevationCm=%.2f riverDistanceCm=%.1f cityDistanceCm=%.1f canPlaceHouse=%d lotParity=%d publicRing=%d frontageQuoteTL=%d pathCells=%d"),
				Coord.X, Coord.Y, Depths.FindRef(Coord), Cell.Elevation, Cell.RiverDistance, CityDistance, bCanPlace, bParity, bRing, Cost, Length);
		}
		CandidateJson += TEXT("]");
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_SCENARIO_FLOOD_DISPLAY_PROBE built=%d rainfallInput=%.2f canonicalPositiveCells=%d visibleLandCells=%d wetHomes=%d/%d wetCentres=%d/%d wetEmptyLand=%d reachableWetHouseCandidates=%d maxDisplayDepthCm=%.2f stage=BeforeTurnsAndInvestments damageApplied=0"),
			bBuilt, Rainfall, Stats.CanonicalPositiveCells, Stats.ReconstructedLandCells, WetHomes, TotalHomes, WetCentres, TotalCentres, Candidates.Num(), ReachableWetCandidates, Stats.MaximumDisplayDepthCm);
		if (Surface) { Surface->SetStaticMesh(nullptr); Surface->DestroyComponent(); }
		if (ProbeOwner) ProbeOwner->Destroy();
		OutWetHomes = WetHomes; OutWetCentres = WetCentres; bOutBuilt = bBuilt;
		return FString::Printf(TEXT("{\"built\":%s,\"input_scope\":\"SimulateFloodMap(definition.BaseSeverity), before turns and investments; no TriggerDisaster, prevention multiplier or gameplay damage applied\",\"rainfall_gameplay_intensity\":%.3f,\"wet_threshold_display_cm\":12,\"canonical_positive_cells\":%d,\"visible_land_cells\":%d,\"maximum_display_depth_cm\":%.3f,\"wet_homes\":%d,\"total_homes\":%d,\"wet_emergency_centres\":%d,\"total_emergency_centres\":%d,\"reachable_wet_house_candidates\":%d,\"buildings\":%s,\"wet_empty_land_candidates\":%s}"),
			bBuilt ? TEXT("true") : TEXT("false"), Rainfall, Stats.CanonicalPositiveCells, Stats.ReconstructedLandCells, Stats.MaximumDisplayDepthCm,
			WetHomes, TotalHomes, WetCentres, TotalCentres, ReachableWetCandidates, *BuildingJson, *CandidateJson);
	}
}

UAftershockScenarioAuditCommandlet::UAftershockScenarioAuditCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UAftershockScenarioAuditCommandlet::Main(const FString& Params)
{
	int32 RequestedIndex = 0;
	FParse::Value(FCommandLine::Get(), TEXT("AftershockScenario="), RequestedIndex);
	RequestedIndex = FMath::Clamp(RequestedIndex, 0, UAftershockScenarioSubsystem::GetScenarioCount() - 1);
	TArray<FScenarioCheck> Checks;
	FString FloodDisplayProbeJson = TEXT("null");
	int32 Failed = 0;
	const auto Check = [&](const TCHAR* Name, bool Passed, const FString& Detail)
	{
		Checks.Add({ Name, Passed, Detail }); Failed += Passed ? 0 : 1;
		if (Passed) { UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_SCENARIO_AUDIT_PASS %s: %s"), Name, *Detail); }
		else { UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_SCENARIO_AUDIT_FAIL %s: %s"), Name, *Detail); }
	};
	const auto Finish = [&]()
	{
		const FString Directory = FPaths::ProjectSavedDir() / TEXT("PresentationValidation");
		IFileManager::Get().MakeDirectory(*Directory, true);
		const FString Path = Directory / FString::Printf(TEXT("scenario_audit_%d.json"), RequestedIndex);
		FString Json = FString::Printf(TEXT("{\n\"schema_version\":1,\n\"utc\":\"%s\",\n\"requested_scenario\":%d,\n\"scope\":\"Isolated canonical startup, transaction, objective and climate APIs. Zero-intensity disaster events exercise progression only; no UI, rendered scene, GPU, nonzero hazard accuracy, cinematic or packaged build claim.\",\n\"passed\":%d,\n\"failed\":%d,\n\"checks\":[\n"),
			*FDateTime::UtcNow().ToIso8601(), RequestedIndex, Checks.Num() - Failed, Failed);
		for (int32 I = 0; I < Checks.Num(); ++I)
		{
			const FScenarioCheck& Result = Checks[I];
			Json += FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s,\"detail\":\"%s\"}%s\n"),
				*JsonEscape(Result.Name), Result.Passed ? TEXT("true") : TEXT("false"), *JsonEscape(Result.Detail), I + 1 < Checks.Num() ? TEXT(",") : TEXT(""));
		}
		Json += TEXT("],\n\"flood_display_probe\":") + FloodDisplayProbeJson + TEXT("\n}\n");
		if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return 2;
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_SCENARIO_AUDIT_RESULT index=%d passed=%d failed=%d report=%s"), RequestedIndex, Checks.Num() - Failed, Failed, *Path);
		return Failed == 0 ? 0 : 1;
	};
	Check(TEXT("engine_initialized"), GEngine != nullptr, TEXT("This commandlet needs the editor engine, without saving or opening a gameplay map."));
	if (!GEngine) return Finish();
	FScopedScenarioWorld Fixture;
	Fixture.World = UWorld::CreateWorld(EWorldType::Game, false, FName(TEXT("AftershockScenarioFixture")), GetTransientPackage(), true, ERHIFeatureLevel::SM5, nullptr, false);
	Check(TEXT("isolated_world_created"), Fixture.World != nullptr, TEXT("The fixture has not begun gameplay; Prepare and seed are called explicitly in the same order as startup."));
	if (!Fixture.World) return Finish();
	UWorld* World = Fixture.World;
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game); Context.SetCurrentWorld(World);
	UAftershockScenarioSubsystem* Scenario = World->GetSubsystem<UAftershockScenarioSubsystem>();
	UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>();
	UBuildingManagerSubsystem* Buildings = World->GetSubsystem<UBuildingManagerSubsystem>();
	UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>();
	UWorldClimateSubsystem* Climate = World->GetSubsystem<UWorldClimateSubsystem>();
	UCrowdRenderSubsystem* Crowd = World->GetSubsystem<UCrowdRenderSubsystem>();
	URoadNetworkSubsystem* Roads = World->GetSubsystem<URoadNetworkSubsystem>();
	UDisasterSimulationSubsystem* Simulation = World->GetSubsystem<UDisasterSimulationSubsystem>();
	Check(TEXT("same_world_canonical_subsystems"), Scenario && Terrain && Buildings && Economy && Climate && Crowd && Roads && Simulation,
		TEXT("Scenario, terrain, buildings, economy, climate, crowd, roads and simulation must all initialize."));
	if (!Scenario || !Terrain || !Buildings || !Economy || !Climate || !Crowd || !Roads || !Simulation) return Finish();
	Check(TEXT("prepare_boundary_precedes_seed"), !Scenario->IsPrepared() && Buildings->GetActiveBuildingsCount() == 0 && Roads->CountRoadCells() == 0,
		TEXT("No authored buildings, auto-roads or already-prepared scenario may precede the explicit startup boundary."));
	Scenario->PrepareScenarioBeforeSettlement();
	const FAftershockScenarioDefinition Definition = Scenario->GetDefinition();
	const int32 ExpectedSeeds[] = {1337,7447,9851,20261,43881,55153,67319}, ExpectedYears[] = {2026,2035,2045,2055,2060,2065,2070};
	const int64 ExpectedBudgets[] = {50000,60000,70000,90000,110000,130000,150000};
	const EDisasterType ExpectedTypes[] = {EDisasterType::Earthquake,EDisasterType::Flood,EDisasterType::Wind,EDisasterType::Tsunami,EDisasterType::Fire,EDisasterType::Volcano,EDisasterType::Drought};
	Check(TEXT("command_line_selects_requested_scenario"), Scenario->IsPrepared() && Scenario->GetScenarioIndex() == RequestedIndex
		&& Definition.Seed == ExpectedSeeds[RequestedIndex] && Definition.StartYear == ExpectedYears[RequestedIndex] && Definition.Hazard == ExpectedTypes[RequestedIndex],
		FString::Printf(TEXT("launch=-AftershockScenario=%d actualIndex=%d seed=%d year=%d hazard=%d"), RequestedIndex, Scenario->GetScenarioIndex(), Definition.Seed, Definition.StartYear, static_cast<int32>(Definition.Hazard)));
	Check(TEXT("scenario_canonical_seed_budget_year"), Terrain->TerrainSeed == ExpectedSeeds[RequestedIndex] && Economy->TreasuryTL == ExpectedBudgets[RequestedIndex]
		&& Economy->CurrentYear == ExpectedYears[RequestedIndex] && Climate->GetState().CurrentYear == Economy->CurrentYear,
		TEXT("The selected definition prepares the actual terrain/economy/year, before any building placement."));
	TSet<FIntPoint> LotCoords;
	bool LotsValid = Scenario->GetDesignedLots().Num() == 44;
	for (const FAftershockScenarioLot& Lot : Scenario->GetDesignedLots())
	{
		LotsValid &= Terrain->IsPlayableCoord(Lot.Coord) && !Terrain->GetCell(Lot.Coord).bIsRiverbed && !LotCoords.Contains(Lot.Coord);
		LotCoords.Add(Lot.Coord);
	}
	Check(TEXT("forty_four_unique_authored_land_lots"), LotsValid, FString::Printf(TEXT("lots=%d unique=%d"), Scenario->GetDesignedLots().Num(), LotCoords.Num()));
	bool AssemblyValid = Scenario->GetAssemblyCells().Num() == 3;
	TSet<FIntPoint> AssemblyCoords;
	for (FIntPoint Coord : Scenario->GetAssemblyCells())
	{
		AssemblyValid &= Terrain->IsPlayableCoord(Coord) && !Terrain->GetCell(Coord).bIsRiverbed && !LotCoords.Contains(Coord)
			&& Terrain->GetCell(Coord).bIsEmergencyAssemblyArea && !AssemblyCoords.Contains(Coord);
		AssemblyCoords.Add(Coord);
	}
	Check(TEXT("three_canonical_open_assembly_cells"), AssemblyValid, TEXT("Distinct, playable, non-river assembly flags belong to the same canonical cells and do not overlap building lots."));
	const int64 PreparedBudget = Economy->TreasuryTL;
	Buildings->SeedInitialSettlement();
	TArray<FBuildingInstance> Records; Buildings->GetAllBuildings(Records);
	bool RecordsMatch = Records.Num() == Scenario->GetDesignedLots().Num();
	int64 Population = 0;
	UBuildingData* ParkData = nullptr;
	int32 RetrofitID = INDEX_NONE;
	for (const FAftershockScenarioLot& Lot : Scenario->GetDesignedLots())
	{
		FBuildingInstance B;
		const bool Found = Buildings->GetBuildingAtCoords(Lot.Coord, B);
		RecordsMatch &= Found && B.BuildingData && B.BuildingData->Type == Lot.Type && B.Foundation == Lot.Foundation && B.CurrentFloors == Lot.Floors;
		if (Found && B.BuildingData)
		{
			Population += FMath::Max(0,B.BuildingData->Residents);
			if (B.BuildingData->Type == EBuildingType::GreenSpace) ParkData = B.BuildingData;
			if (B.BuildingData->Type == EBuildingType::House && RetrofitID == INDEX_NONE) RetrofitID = B.InstanceID;
		}
	}
	Check(TEXT("authored_seed_matches_canonical_buildings_without_payment"), RecordsMatch && Economy->TreasuryTL == PreparedBudget,
		FString::Printf(TEXT("records=%d lots=%d treasuryBefore=%lld after=%lld"), Records.Num(), Scenario->GetDesignedLots().Num(), PreparedBudget, Economy->TreasuryTL));
	const int32 InitialRoads = Roads->CountRoadCells();
	Check(TEXT("scenario_starter_streets_match_contract"), InitialRoads>=8 && InitialRoads==Scenario->GetStarterRoadCellCount(),
		FString::Printf(TEXT("Every authored city has free connected starter services; player extensions are paid. initialRoads=%d treasuryUnchanged=%d"), InitialRoads, Economy->TreasuryTL == PreparedBudget));
	{
		TSet<FIntPoint> Connected;
		TArray<FIntPoint> Queue;
		const FIntPoint Steps[] = { FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1) };
		if (!Scenario->GetAssemblyCells().IsEmpty()) { Queue.Add(Scenario->GetAssemblyCells()[0]); Connected.Add(Queue[0]); }
		for (int32 Head = 0; Head < Queue.Num(); ++Head) for (FIntPoint Step : Steps)
		{
			const FIntPoint C = Queue[Head] + Step;
			if (Roads->HasRoad(C) && !Connected.Contains(C)) { Connected.Add(C); Queue.Add(C); }
		}
		bool AllFrontages = Connected.Num() == InitialRoads;
		for (const FBuildingInstance& B : Records)
		{
			bool HasFrontage = false;
			for (FIntPoint Step : Steps) HasFrontage |= Connected.Contains(B.GridCoords + Step);
			AllFrontages &= HasFrontage;
		}
		for (FIntPoint C : Scenario->GetAssemblyCells()) AllFrontages &= Connected.Contains(C);
		Check(TEXT("starter_streets_connect_all_authored_frontages_and_centres"), AllFrontages,
			FString::Printf(TEXT("connectedCells=%d totalRoadCells=%d buildingFrontages=%d assemblyAreas=3"), Connected.Num(), InitialRoads, Records.Num()));
	}
	Check(TEXT("coastal_unlock_matches_scenario"), Terrain->IsCoastalWorld() == (RequestedIndex == 3), TEXT("Only scenario3 owns canonical sea cells; tsunami must not be available in inland cities."));
	UCityLifeSubsystem* Life=World->GetSubsystem<UCityLifeSubsystem>();
    Crowd->SynchronizeCanonicalPopulation();
    Check(TEXT("population_comes_only_from_living_resident_ledger"),Life && Life->IsCityInitialized() && Life->GetState().LivingPopulation==Population && Crowd->GetCurrentCrowdCount()==FMath::Min<int64>(1500,Population),
        FString::Printf(TEXT("residentialCapacity=%lld living=%d actualAgents=%d workersNeverAdded=1"),Population,Life ? Life->GetState().LivingPopulation : -1,Crowd->GetCurrentCrowdCount()));
    const auto& Geo=GetAftershockGeography(RequestedIndex);
    const FVector Anchor=AftershockLatLonToGlobeNormal(Geo.Latitude,Geo.Longitude,RequestedIndex);
    Check(TEXT("offline_geography_and_globe_share_real_anchor"),Terrain->GeographyIndex==RequestedIndex && FMath::IsNearlyEqual(Definition.Latitude,Geo.Latitude) && FMath::IsNearlyEqual(Definition.Longitude,Geo.Longitude) && Anchor.Equals(FVector::UpVector,.0001),
        FString::Printf(TEXT("lat=%.6f lon=%.6f canonicalDEMCentreM=%.2f offline81samples=1 compressedNot1to1=1"),Geo.Latitude,Geo.Longitude,Terrain->GetCell(FIntPoint(25,25)).RealElevationMetres));
    FText VolcanoReason;
    Check(TEXT("volcano_requires_actual_mapped_source"),Simulation->CanTriggerDisaster(EDisasterType::Volcano,VolcanoReason)==(RequestedIndex==5),TEXT("No hot-weather or cityCO2 eruption trigger; only Etna mapped vent."));
	Buildings->SeedInitialSettlement(); Scenario->PrepareScenarioBeforeSettlement();
	Check(TEXT("prepare_and_seed_are_idempotent"), Buildings->GetActiveBuildingsCount() == Records.Num() && Economy->TreasuryTL == PreparedBudget && Roads->CountRoadCells() == InitialRoads,
		TEXT("Repeating the public startup calls must not duplicate lots, population, roads or construction charges."));
	Check(TEXT("initial_objectives_block_progression"), !Scenario->CanAdvanceScenario(), *Scenario->GetProgressText().ToString());
	if (RequestedIndex == 1)
	{
		int32 WetHomes = 0, WetCentres = 0;
		bool bDisplayBuilt = false;
		FloodDisplayProbeJson = ProbeFloodDisplay(*World, *Terrain, *Buildings, *Roads, *Simulation, *Scenario, WetHomes, WetCentres, bDisplayBuilt);
		Check(TEXT("flood_scenario_has_visible_riverside_homes"), bDisplayBuilt && WetHomes >= 2,
			FString::Printf(TEXT("built=%d wetHomes=%d; real input65 display reconstruction before investments, peak display depth>12cm; no damage/physical-depth claim"), bDisplayBuilt, WetHomes));
		Check(TEXT("flood_scenario_keeps_emergency_centres_dry"), bDisplayBuilt && WetCentres == 0,
			FString::Printf(TEXT("built=%d wetCentres=%d; authored hospital, school and fire station stay above the same input65 display flood"), bDisplayBuilt, WetCentres));
	}

	const int32 InitialYear = Economy->CurrentYear;
	const float InitialAnomaly = Climate->GetState().GlobalTemperatureAnomalyC;
	for (int32 Turn = 0; Turn < 9; ++Turn) Economy->AdvanceTurn();
	Check(TEXT("real_economy_year_advances_annual_scenario"), Economy->CurrentYear == InitialYear + 1 && Climate->GetState().CurrentYear == Economy->CurrentYear
		&& FMath::IsNearlyEqual(Climate->GetState().GlobalTemperatureAnomalyC - InitialAnomaly, .025f, .0001f),
		FString::Printf(TEXT("yearBefore=%d yearAfter=%d anomalyBefore=%.4f after=%.4f; nine real turns cross turn10"), InitialYear, Economy->CurrentYear, InitialAnomaly, Climate->GetState().GlobalTemperatureAnomalyC));
	const float OriginalCO2 = Economy->CityCO2Level;
	const float GlobalBeforeCO2Probe = Climate->GetState().GlobalTemperatureAnomalyC;
	const float HeatBeforeProbe = Climate->GetState().LocalUrbanHeatPressure;
	Economy->CityCO2Level += 100.f; Climate->RefreshFromEconomy();
	Check(TEXT("co2_only_probe_changes_local_heat_not_global_temperature"), FMath::IsNearlyEqual(GlobalBeforeCO2Probe, Climate->GetState().GlobalTemperatureAnomalyC)
		&& Climate->GetState().LocalUrbanHeatPressure > HeatBeforeProbe,
		TEXT("Explicit isolated input probe adds100 dimensionless cityCO2, refreshes the real climate API, then restores the input; no degree/ppm conversion or hazard test."));
	Economy->CityCO2Level = OriginalCO2; Climate->RefreshFromEconomy();

	bool RoadTransactions = AssemblyValid;
	if (AssemblyValid)
	{
		for (int32 I = 1; I < Scenario->GetAssemblyCells().Num(); ++I)
		{
			FAftershockRoadQuote Quote;
			const int64 Before = Economy->TreasuryTL;
			const FAftershockRoadQuote Existing = Roads->QuoteRoad(Scenario->GetAssemblyCells()[0], Scenario->GetAssemblyCells()[I]);
			if (Existing.bRouteValid && Existing.NewRoadCells == 0) continue;
			const bool Built = Roads->BuildRoad(Scenario->GetAssemblyCells()[0], Scenario->GetAssemblyCells()[I], Quote);
			RoadTransactions &= Built && Before - Economy->TreasuryTL == Quote.CostTL;
		}
		// Compact authored frontages may connect with fewer than the objective's
		// eight cells. Extend the SAME root-connected network with real paid routes.
		// Nine are intentional: the later root-removal probe must leave >=8 raw
		// road cells, proving connectivity fails independently of the count gate.
		const FIntPoint AssemblyRoot = Scenario->GetAssemblyCells()[0];
		TArray<FIntPoint> ExtensionCandidates;
		for (const FGridCell& Cell : Terrain->Cells)
		{
			if (Cell.bIsPlayable && !Cell.bIsRiverbed && Cell.OccupyingBuildingID == INDEX_NONE && !Cell.bHasRoad) ExtensionCandidates.Add(Cell.Coords);
		}
		ExtensionCandidates.Sort([AssemblyRoot](FIntPoint A, FIntPoint B)
		{
			const int32 DA = FMath::Abs(A.X - AssemblyRoot.X) + FMath::Abs(A.Y - AssemblyRoot.Y);
			const int32 DB = FMath::Abs(B.X - AssemblyRoot.X) + FMath::Abs(B.Y - AssemblyRoot.Y);
			if (DA != DB) return DA < DB;
			return A.Y == B.Y ? A.X < B.X : A.Y < B.Y;
		});
		for (FIntPoint End : ExtensionCandidates)
		{
			if (Roads->CountRoadCells() >= 9) break;
			const FAftershockRoadQuote Preview = Roads->QuoteRoad(AssemblyRoot, End);
			if (!Preview.bRouteValid || Preview.NewRoadCells == 0) continue;
			FAftershockRoadQuote Actual;
			const int64 Before = Economy->TreasuryTL;
			const bool Built = Roads->BuildRoad(AssemblyRoot, End, Actual);
			RoadTransactions &= Built && Before - Economy->TreasuryTL == Actual.CostTL;
			if (!Built) break;
		}
	}
	Check(TEXT("canonical_roads_connect_assembly_frontages"), RoadTransactions && Roads->CountRoadCells() >= 9,
		FString::Printf(TEXT("roadCells=%d; scenario0 keeps its free authored network; missing links/extensions use actual BuildRoad with exact charges, existing streets do not charge again"), Roads->CountRoadCells()));
	bool PreparationSucceeded = false;
	if (RequestedIndex == 0) PreparationSucceeded = RetrofitID != INDEX_NONE && Economy->RetrofitWithBudget(RetrofitID);
	else if (RequestedIndex == 1)
	{
		int32 Upgraded = 0;
		for (const FGridCell Cell : Terrain->Cells)
		{
			if (Upgraded >= 6) break;
			if (!Cell.bIsPlayable || Cell.bIsRiverbed || Cell.DrainageQuality >= .2f) continue;
			int32 ActualCost = 0;
			if (!Economy->TrySpendMoney(400)) break;
			if (Simulation->InvestInStreetDrainage(Cell.Coords, 0, ActualCost) && ActualCost == 400) ++Upgraded;
		}
		PreparationSucceeded = Upgraded == 6;
	}
	else if (RequestedIndex == 2 && ParkData)
	{
		int32 Added = 0;
		for (const FGridCell Cell : Terrain->Cells)
		{
			if (Added >= 2) break;
			if (Buildings->CanPlaceBuildingAt(Cell.Coords, ParkData) && Economy->BuildBuildingWithBudget(Cell.Coords, ParkData, EFoundationQuality::Medium, 1)) ++Added;
		}
		PreparationSucceeded = Added == 2;
	}
	else if (RequestedIndex == 3)
	{
		PreparationSucceeded = AssemblyValid;
		for (FIntPoint Assembly : Scenario->GetAssemblyCells()) PreparationSucceeded &= Roads->HasRoad(Assembly);
	}
    if (RequestedIndex==4 || RequestedIndex==6)
    {
        UBuildingData* Data=Buildings->GetCatalogData(RequestedIndex==4 ? EBuildingType::FireStation : EBuildingType::Farm);
        const FIntPoint Steps[]={FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1)};
		if (RequestedIndex==6 && Data)
		{
			bool bPlacedIsolatedFarm = false,bRemovedRealAccess=false,bFarmHasNoFrontage=false,bPaymentPreserved=false;
			for (const FGridCell& Cell : Terrain->Cells)
			{
				if (!Buildings->CanPlaceBuildingAt(Cell.Coords,Data)) continue;
				bool bTouchesRoad = false;
				for (FIntPoint Step : Steps) bTouchesRoad |= Roads->HasRoad(Cell.Coords+Step);
				if (bTouchesRoad) continue;
				const auto Access=Buildings->QuoteAutomaticRoadAccess(Cell.Coords,Data);
				if(!Access.bRouteValid||Access.NewRoadCells<1)continue;
				const int64 BeforePurchase=Economy->TreasuryTL;
				const int64 ExpectedCost=static_cast<int64>(Buildings->CalculateConstructionCost(Data,EFoundationQuality::Medium,1))+Access.RoadCostTL;
				const int32 PopulationBefore=Life?Life->GetState().LivingPopulation:0;
				bPlacedIsolatedFarm = Economy->BuildBuildingWithBudget(Cell.Coords,Data,EFoundationQuality::Medium,1);
				if (!bPlacedIsolatedFarm) continue;
				const int64 AfterPurchase=Economy->TreasuryTL;
				// A paid farm now correctly purchases its access road too. Create
				// the isolation fixture through real road removal AFTER that debit.
				// No frontage existed beforehand, so these are new access cells,
				// never pre-existing city roads or fabricated disconnected stubs.
				int32 Removed=0;bool bAllRemoved=true;
				for(FIntPoint Step:Steps)if(Roads->HasRoad(Cell.Coords+Step))
				{const bool Good=Roads->RemoveRoadAt(Cell.Coords+Step);bAllRemoved&=Good;Removed+=Good?1:0;}
				bRemovedRealAccess=bAllRemoved&&Removed>0;
				bFarmHasNoFrontage=true;for(FIntPoint Step:Steps)bFarmHasNoFrontage&=!Roads->HasRoad(Cell.Coords+Step);
				FBuildingInstance Isolated;
				bPlacedIsolatedFarm=Buildings->GetBuildingAtCoords(Cell.Coords,Isolated)&&Isolated.BuildingData&&Isolated.BuildingData->Type==EBuildingType::Farm;
				bPaymentPreserved=BeforePurchase-AfterPurchase==ExpectedCost&&Economy->TreasuryTL==AfterPurchase
					&&(!Life||Life->GetState().LivingPopulation==PopulationBefore);
				break;
			}
			Check(TEXT("isolated_farm_fixture_keeps_full_access_purchase_without_refund_or_new_population"),bPlacedIsolatedFarm&&bRemovedRealAccess&&bPaymentPreserved,
				TEXT("The real farm plus quoted access road are paid exactly once. Canonical frontage removal gives no refund and creates no people."));
			Check(TEXT("isolated_farm_cannot_satisfy_food_delivery_objective"),bPlacedIsolatedFarm&&bRemovedRealAccess&&bFarmHasNoFrontage&&Scenario->GetProgressText().ToString().Contains(TEXT("bağlı tarla: 1/2")),
				TEXT("The paid second farm remains canonical after its newly purchased frontage is removed through the road owner. Only the original city-connected field counts until another connected field is built."));
		}
        for (const FGridCell Cell:Terrain->Cells)
        {
            if (!Data || !Buildings->CanPlaceBuildingAt(Cell.Coords,Data)) continue;
            bool bHasFrontage=false; for (FIntPoint Step:Steps) bHasFrontage|=Roads->HasRoad(Cell.Coords+Step);
            if (bHasFrontage && Economy->BuildBuildingWithBudget(Cell.Coords,Data,EFoundationQuality::Medium,1)) { PreparationSucceeded=true; break; }
        }
        if (Life) Life->RefreshCity();
    }
    if (RequestedIndex==5)
    {
        int32 Surveyed=0;
        for (const FGridCell Cell:Terrain->Cells)
        {
            if (Surveyed>=3) break;
            int32 Cost=0; if (Simulation->SurveyGround(Cell.Coords,Cost) && Cost==500) ++Surveyed;
        }
        PreparationSucceeded=Surveyed==3;
    }
	Check(TEXT("scenario_preparedness_uses_real_public_transactions"), PreparationSucceeded, TEXT("Scenario0 paid retrofit; scenario1 six paid400TL radius0 drainage calls; scenario2 two paid parks; scenario3 actual connected evacuation assembly roads, never dams/drainage as tsunami protection."));
	Check(TEXT("all_investments_still_require_primary_disaster"), !Scenario->CanAdvanceScenario(), *Scenario->GetProgressText().ToString());
	const EDisasterType WrongHazard = Definition.Hazard == EDisasterType::Earthquake ? EDisasterType::Wind : EDisasterType::Earthquake;
	const FDisasterReport WrongReport = Simulation->TriggerDisaster(WrongHazard, 0.f, 1.f, true);
	Check(TEXT("different_hazard_does_not_unlock_primary_objective"), WrongReport.DisasterType == WrongHazard && !Scenario->CanAdvanceScenario(),
		TEXT("Real zero-intensity wrong-hazard event. This tests event/type gating, never nonzero hazard accuracy."));
	const FDisasterReport PrimaryReport = Simulation->TriggerDisaster(Definition.Hazard, 0.f, 1.f, true);
	Check(TEXT("primary_event_and_objectives_unlock_progression"), PrimaryReport.DisasterType == Definition.Hazard && PrimaryReport.TotalDeaths == 0 && Scenario->CanAdvanceScenario(),
		TEXT("Real zero-intensity primary-hazard event after the real investments; this proves the objective/API gate only."));
	if (AssemblyValid)
	{
		const FIntPoint AssemblyRoot = Scenario->GetAssemblyCells()[0];
		const int32 RoadsBeforeCut = Roads->CountRoadCells();
		const int64 BudgetBeforeCut = Economy->TreasuryTL;
		const bool RootExisted = Roads->HasRoad(AssemblyRoot);
		const bool Removed = Roads->RemoveRoadAt(AssemblyRoot);
		Check(TEXT("disconnected_assembly_root_blocks_progression"), RootExisted && Removed && !Roads->HasRoad(AssemblyRoot)
			&& Roads->CountRoadCells() == RoadsBeforeCut - 1 && Roads->CountRoadCells() >= 8
			&& Economy->TreasuryTL == BudgetBeforeCut && !Scenario->CanAdvanceScenario(),
			FString::Printf(TEXT("Removed actual assembly-root road %s; remainingRoads=%d. The still-sufficient remote road count cannot substitute for the required connected component."),
				*AssemblyRoot.ToString(), Roads->CountRoadCells()));
		FAftershockRoadQuote RestoreQuote;
		const int64 BudgetBeforeRestore = Economy->TreasuryTL;
		const bool Restored = Roads->BuildRoad(AssemblyRoot, AssemblyRoot, RestoreQuote);
		Check(TEXT("paid_root_road_restoration_reconnects_objectives"), Restored && Roads->HasRoad(AssemblyRoot)
			&& Roads->CountRoadCells() == RoadsBeforeCut && RestoreQuote.NewRoadCells == 1 && RestoreQuote.CostTL == 200
			&& BudgetBeforeRestore - Economy->TreasuryTL == 200 && Scenario->CanAdvanceScenario(),
			TEXT("Real one-cell BuildRoad transaction restores the assembly root, all original road cells and connected-centre eligibility for exactly200TL."));
	}
	const float Satisfaction = Economy->PublicSatisfaction;
	Economy->PublicSatisfaction = 24.f;
	Check(TEXT("positive_satisfaction_keeps_completed_city_eligible"), Scenario->CanAdvanceScenario(), TEXT("Explicit satisfaction24 gate probe; the canonical dismissal threshold is now zero."));
	Economy->PublicSatisfaction = 0.f;
	Check(TEXT("dismissal_threshold_blocks_progression"), !Scenario->CanAdvanceScenario(), TEXT("Explicit isolated satisfaction0 input probe; restored immediately, no casualty assertion."));
	Economy->PublicSatisfaction = Satisfaction;
	const bool WasGameOver = Economy->bIsGameOver; Economy->bIsGameOver = true;
	Check(TEXT("terminal_game_state_blocks_progression"), !Scenario->CanAdvanceScenario(), TEXT("Explicit isolated terminal-state input probe; restored immediately."));
	Economy->bIsGameOver = WasGameOver;
	Check(TEXT("objective_probe_restores_valid_state"), Scenario->CanAdvanceScenario(), TEXT("Gate returns to its valid completed state; OpenLevel is deliberately not called by this commandlet."));
	return Finish();
}
#else
UAftershockScenarioAuditCommandlet::UAftershockScenarioAuditCommandlet() {}
int32 UAftershockScenarioAuditCommandlet::Main(const FString& Params) { return 1; }
#endif
