#include "AftershockCrowdProfileCommandlet.h"

#if WITH_EDITOR

#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/Parse.h"

UAftershockCrowdProfileCommandlet::UAftershockCrowdProfileCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UAftershockCrowdProfileCommandlet::Main(const FString& Params)
{
	if (!GEngine)
	{
		UE_LOG(LogTemp, Error, TEXT("I9_CROWD_PROFILE_FAIL: GEngine unavailable"));
		return 1;
	}

	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		FName(TEXT("AftershockCrowdProfileWorld")),
		GetTransientPackage(),
		true,
		ERHIFeatureLevel::SM5,
		nullptr,
		false);
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("I9_CROWD_PROFILE_FAIL: temporary world creation failed"));
		return 1;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);
	UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>();
	UCrowdRenderSubsystem* Crowd = World->GetSubsystem<UCrowdRenderSubsystem>();
	if (!Terrain || !Crowd || Terrain->GridWidth != 50 || Terrain->GridHeight != 50)
	{
		UE_LOG(LogTemp, Error, TEXT("I9_CROWD_PROFILE_FAIL: grid/crowd subsystem not initialized (grid=%d x %d)"),
			Terrain ? Terrain->GridWidth : 0,
			Terrain ? Terrain->GridHeight : 0);
		World->DestroyWorld(false);
		GEngine->DestroyWorldContext(World);
		World->RemoveFromRoot();
		return 1;
	}

	int32 RequestedAgents = 1500;
	FParse::Value(*Params, TEXT("Agents="), RequestedAgents);
	RequestedAgents = FMath::Clamp(RequestedAgents, 20, 1500);
	Crowd->UpdatePopulationCount(RequestedAgents * 2, 0); // Cosmetic representatives only.
	const int32 AgentCount = Crowd->GetCurrentCrowdCount();
	if (AgentCount != RequestedAgents)
	{
		UE_LOG(LogTemp, Error, TEXT("I9_CROWD_PROFILE_FAIL: expected %d ISM agents, got %d"), RequestedAgents, AgentCount);
		World->DestroyWorld(false);
		GEngine->DestroyWorldContext(World);
		World->RemoveFromRoot();
		return 1;
	}

	AAftershockCrowdHostActor* Host = Crowd->GetOrCreateHostActor();
	if (!Host || Host->CanEverTick() || Host->CitizenISM->GetInstanceCount() != AgentCount ||
		!Host->CitizenISM->GetMaterial(0) || !Host->TerrainISM->GetMaterial(0) || !Host->RiverISM->GetMaterial(0) ||
		!Host->FloodSpraySystem || !Host->EarthquakeDustSystem || !Host->WindDebrisSystem)
	{
		UE_LOG(LogTemp, Error, TEXT("I9_CROWD_PROFILE_FAIL: host tick/ISM/material/Niagara reference audit failed"));
		World->DestroyWorld(false);
		GEngine->DestroyWorldContext(World);
		World->RemoveFromRoot();
		return 1;
	}

	Host->BuildTerrainVisual(Terrain);
	int32 ExpectedTerrainInstances = 0;
	int32 ExpectedRiverInstances = 0;
	for (const FGridCell& Cell : Terrain->Cells)
	{
		if (!Cell.bIsPlayable) continue;
		Cell.bIsRiverbed ? ++ExpectedRiverInstances : ++ExpectedTerrainInstances;
	}
	if (!Host->TerrainSurface->GetStaticMesh() || !Host->RiverSurface->GetStaticMesh() ||
		Host->RenderedLandCellCount != ExpectedTerrainInstances || Host->RenderedRiverCellCount != ExpectedRiverInstances)
	{
		UE_LOG(LogTemp, Error, TEXT("I9_CROWD_PROFILE_FAIL: continuous visual surface or canonical cell coverage is missing"));
		World->DestroyWorld(false);
		GEngine->DestroyWorldContext(World);
		World->RemoveFromRoot();
		return 1;
	}
	UE_LOG(LogTemp, Display, TEXT("I9_VISUAL_REFERENCE_AUDIT Terrain=%d River=%d CitizenMaterial=resolved TerrainMaterial=resolved WaterMaterial=resolved NiagaraSystems=3/3 ActorTick=false"),
		ExpectedTerrainInstances, ExpectedRiverInstances);

	// Regression fixtures run before warmup, outside the 600 timed callbacks.
	// They are explicit synthetic profile inputs, never normal game population.
	const auto CheckPopulation = [Crowd, Host](const TCHAR* Case, int32 Residents, int32 Workers, int32 Expected)
	{
		Crowd->UpdatePopulationCount(Residents, Workers);
		const int32 ActualAgents = Crowd->GetCurrentCrowdCount();
		const int32 ActualInstances = Host->CitizenISM->GetInstanceCount();
		const bool bPassed = ActualAgents == Expected && ActualInstances == Expected;
		UE_LOG(LogTemp, Display, TEXT("I9_CROWD_POPULATION_CHECK case=%s passed=%d residents=%d workers=%d expected=%d agents=%d instances=%d"),
			Case, bPassed, Residents, Workers, Expected, ActualAgents, ActualInstances);
		return bPassed;
	};
	bool bPopulationChecksPassed = CheckPopulation(TEXT("zero_clears_populated_instances"), 0, 0, 0);
	bPopulationChecksPassed &= CheckPopulation(TEXT("negative_sources_stay_empty"), -MAX_int32, -MAX_int32, 0);
	bPopulationChecksPassed &= CheckPopulation(TEXT("negative_residents_clamp_independently"), -MAX_int32, 200, 100);
	bPopulationChecksPassed &= CheckPopulation(TEXT("negative_workers_clamp_independently"), 200, -MAX_int32, 100);
	bPopulationChecksPassed &= CheckPopulation(TEXT("large_sum_caps_without_int32_overflow"), MAX_int32, MAX_int32, 1500);
	bPopulationChecksPassed &= CheckPopulation(TEXT("zero_removes_all_cap_instances"), 0, 0, 0);
	bPopulationChecksPassed &= CheckPopulation(TEXT("requested_population_restored"), RequestedAgents * 2, 0, RequestedAgents);
	if (!bPopulationChecksPassed)
	{
		UE_LOG(LogTemp, Error, TEXT("I9_CROWD_PROFILE_FAIL: population/ISM lifecycle regression"));
		World->DestroyWorld(false);
		GEngine->DestroyWorldContext(World);
		World->RemoveFromRoot();
		return 1;
	}

	constexpr int32 WarmupTicks = 120;
	constexpr int32 ProfileTicks = 600;
	for (int32 TickIndex = 0; TickIndex < WarmupTicks; ++TickIndex)
	{
		Crowd->UpdateCrowdPositions();
	}

	TArray<double> TickMilliseconds;
	TickMilliseconds.Reserve(ProfileTicks);
	for (int32 TickIndex = 0; TickIndex < ProfileTicks; ++TickIndex)
	{
		const double StartSeconds = FPlatformTime::Seconds();
		Crowd->UpdateCrowdPositions();
		TickMilliseconds.Add((FPlatformTime::Seconds() - StartSeconds) * 1000.0);
	}
	TickMilliseconds.Sort();
	const double TotalMilliseconds = [&TickMilliseconds]()
	{
		double Total = 0.0;
		for (const double Value : TickMilliseconds)
		{
			Total += Value;
		}
		return Total;
	}();
	const double AverageMilliseconds = TotalMilliseconds / static_cast<double>(TickMilliseconds.Num());
	const int32 P95Index = FMath::Clamp(FMath::CeilToInt(static_cast<float>(TickMilliseconds.Num()) * 0.95f) - 1, 0, TickMilliseconds.Num() - 1);
	const double P95Milliseconds = TickMilliseconds[P95Index];
	const double MaxMilliseconds = TickMilliseconds.Last();
	const double EvacuationStartSeconds = FPlatformTime::Seconds();
	Crowd->SetPanicMode(true);
	const double EvacuationSetupMilliseconds = (FPlatformTime::Seconds() - EvacuationStartSeconds) * 1000.0;
	int32 EvacuatingAgents = 0;
	for (const FCitizenAgent& Agent : Crowd->Agents)
	{
		EvacuatingAgents += Agent.Activity == EAftershockCitizenActivity::Evacuating ? 1 : 0;
	}
	// Read-only evacuation safety inspection is also outside the timed callback
	// and evacuation-setup intervals. Disconnected districts may have empty routes.
	const auto IsSafePublicCell = [Terrain](FIntPoint Coord)
	{
		if (!Terrain->IsPlayableCoord(Coord)) return false;
		const FGridCell Cell = Terrain->GetCell(Coord);
		return !Cell.bIsRiverbed && Cell.OccupyingBuildingID == INDEX_NONE;
	};
	int32 UnsafeLocations = 0;
	int32 UnsafeRouteSteps = 0;
	int32 NonAdjacentRouteSteps = 0;
	int32 RouteStepsChecked = 0;
	for (const FCitizenAgent& Agent : Crowd->Agents)
	{
		FIntPoint Previous = Terrain->WorldToGrid(Agent.CurrentLocation);
		UnsafeLocations += IsSafePublicCell(Previous) ? 0 : 1;
		UnsafeLocations += IsSafePublicCell(Terrain->WorldToGrid(Agent.TargetLocation)) ? 0 : 1;
		UnsafeLocations += IsSafePublicCell(Terrain->WorldToGrid(Agent.FinalDestinationLocation)) ? 0 : 1;
		for (const FIntPoint Step : Agent.RouteGridCoords)
		{
			++RouteStepsChecked;
			UnsafeRouteSteps += IsSafePublicCell(Step) ? 0 : 1;
			NonAdjacentRouteSteps += FMath::Abs(Step.X - Previous.X) + FMath::Abs(Step.Y - Previous.Y) == 1 ? 0 : 1;
			Previous = Step;
		}
	}
	UE_LOG(LogTemp, Display, TEXT("I9_CROWD_ROUTE_SAFETY agents=%d stepsChecked=%d unsafeLocations=%d unsafeSteps=%d nonAdjacentSteps=%d"),
		AgentCount, RouteStepsChecked, UnsafeLocations, UnsafeRouteSteps, NonAdjacentRouteSteps);
	if (UnsafeLocations != 0 || UnsafeRouteSteps != 0 || NonAdjacentRouteSteps != 0)
	{
		UE_LOG(LogTemp, Error, TEXT("I9_CROWD_PROFILE_FAIL: evacuation route leaves playable empty land or skips canonical neighbours"));
		World->DestroyWorld(false);
		GEngine->DestroyWorldContext(World);
		World->RemoveFromRoot();
		return 1;
	}
	if (EvacuatingAgents != AgentCount)
	{
		UE_LOG(LogTemp, Error, TEXT("I9_CROWD_PROFILE_FAIL: panic visual transition did not cover all agents (%d/%d)"), EvacuatingAgents, AgentCount);
		World->DestroyWorld(false);
		GEngine->DestroyWorldContext(World);
		World->RemoveFromRoot();
		return 1;
	}

	UE_LOG(LogTemp, Display, TEXT("I9_CROWD_PROFILE_RESULT Agents=%d Grid=%dx%d UpdateHz=12.5 ProfileTicks=%d AvgCpuMs=%.4f P95CpuMs=%.4f MaxCpuMs=%.4f EvacuationSetupCpuMs=%.4f Evacuating=%d"),
		AgentCount, Terrain->GridWidth, Terrain->GridHeight, ProfileTicks, AverageMilliseconds, P95Milliseconds, MaxMilliseconds, EvacuationSetupMilliseconds, EvacuatingAgents);
	UE_LOG(LogTemp, Display, TEXT("I9_CROWD_PROFILE_SCOPE: game-thread crowd update callback/schedule/ISM transform submission in -nullrhi; GPU rendering and final packaged-game cost are not measured."));

	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	World->RemoveFromRoot();
	return 0;
}

#else

UAftershockCrowdProfileCommandlet::UAftershockCrowdProfileCommandlet() {}

int32 UAftershockCrowdProfileCommandlet::Main(const FString& Params)
{
	return 1;
}

#endif
