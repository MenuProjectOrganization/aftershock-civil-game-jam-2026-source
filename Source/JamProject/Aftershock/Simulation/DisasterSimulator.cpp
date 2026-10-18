#include "Aftershock/Simulation/DisasterSimulator.h"
#include "Aftershock/World/GridManager.h"
#include "Aftershock/Buildings/BuildingActor.h"
#include "Aftershock/Data/BuildingData.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Engine/World.h"
#include "TimerManager.h"

UDisasterSimulator::UDisasterSimulator()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UDisasterSimulator::ScheduleDisaster(const FDisasterEvent& Event)
{
	PendingEvent = Event;
	OnDisasterWarning.Broadcast(Event);

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(OnsetTimerHandle);
		World->GetTimerManager().SetTimer(OnsetTimerHandle, this, &UDisasterSimulator::HandleOnset,
		                                   FMath::Max(0.1f, Event.WarningSeconds), false);
	}
}

void UDisasterSimulator::HandleOnset()
{
	OnDisasterOnset.Broadcast(PendingEvent);
	const FLevelOutcome Outcome = TriggerNow(PendingEvent);
	OnDisasterComplete.Broadcast(Outcome);
}

FLevelOutcome UDisasterSimulator::TriggerNow(const FDisasterEvent& Event)
{
	FLevelOutcome Outcome;
	if(!FMath::IsFinite(Event.Severity)||Event.Severity<0.f||!FMath::IsFinite(Event.DirectionDeg))return Outcome;
	// Compatibility Actor API must not create a second casualty/economy authority
	// in a canonical city. Its old magnitude/mm/kmh inputs are converted once.
	if(UWorld* World=GetWorld())if(const auto* Life=World->GetSubsystem<UCityLifeSubsystem>();Life&&Life->IsCityInitialized())
	{
		if(auto* Simulation=World->GetSubsystem<UDisasterSimulationSubsystem>())
		{
			float Severity=Event.Severity;
			if(Event.Type==EDisasterType::Earthquake)Severity=FMath::Clamp((Severity-3.f)/5.5f,0.f,1.f)*100.f;
			else if(Event.Type==EDisasterType::Flood)Severity*=.5f;
			else if(Event.Type==EDisasterType::Wind)Severity/=1.8f;
			Simulation->ScenarioWindDirection=FVector(FMath::Cos(FMath::DegreesToRadians(Event.DirectionDeg)),FMath::Sin(FMath::DegreesToRadians(Event.DirectionDeg)),0.f);
			const FDisasterReport Report=Simulation->TriggerDisaster(Event.Type,FMath::Clamp(Severity,0.f,100.f),CityState.AwarenessLevel,true);
			if(Report.bWasRejected)return Outcome;
			Outcome.EventID=Report.DisasterEventID;Outcome.BuildingsCollapsed=Report.TotalBuildingsCollapsed;
			Outcome.BuildingsDamaged=Report.TotalBuildingsAffected-Report.TotalBuildingsCollapsed;Outcome.TotalDeaths=Report.TotalDeaths;Outcome.TotalSurvivors=Report.TotalSurvivors;
			Outcome.GovernmentAid=Report.TotalStateAidGranted;Outcome.BurialCost=UDisasterMath::BurialCostFromDeaths(Report.TotalDeaths);Outcome.SatisfactionDelta=Report.TotalSatisfactionChange;
			return Outcome;
		}
		return Outcome;
	}
	if (!GridManager) return Outcome;
	Outcome.EventID=FGuid::NewGuid();

	for (ABuildingActor* Building : GridManager->AllBuildings)
	{
		if (!Building || Building->bCollapsed) continue;

		const FTerrainCell Cell = GridManager->GetCell(Building->GridCoord);
		const float RawSeverity = ComputeRawSeverity(Event, Building, Cell);

		const FBuildingSnapshot Snap = Building->MakeSnapshot();
		const FDamageResult Result = UDisasterMath::ComputeDamage(Snap, Event, CityState, Cell, RawSeverity);

		Building->ApplyDamage(Result.Damage);

		if (Result.bCollapsed) Outcome.BuildingsCollapsed++;
		else if (Result.Damage > 20.f) Outcome.BuildingsDamaged++;

		Outcome.TotalDeaths += Result.Deaths;
		Outcome.TotalSurvivors += Result.Survivors;
	}

	Outcome.GovernmentAid = UDisasterMath::GovernmentAidFromSurvivors(Outcome.TotalSurvivors);
	Outcome.BurialCost = UDisasterMath::BurialCostFromDeaths(Outcome.TotalDeaths);
	Outcome.SatisfactionDelta = UDisasterMath::SatisfactionDeltaFromDeaths(Outcome.TotalDeaths);

	Outcome.EnvironmentDelta = 0.f; // Future production/emissions belong to the canonical city turn.

	return Outcome;
}

float UDisasterSimulator::ComputeRawSeverity(const FDisasterEvent& Event, ABuildingActor* Building, const FTerrainCell& Cell) const
{
	switch (Event.Type)
	{
		case EDisasterType::Earthquake:
			return UDisasterMath::EarthquakeSeverityAt(Event, Building->GetActorLocation(), Cell.FaultLineProximity);

		case EDisasterType::Flood:
		{
			// Legacy isolated worlds have no catchment solver. Explicit mm->m
			// runoff proxy only; canonical cities route through TriggerDisaster.
			const float Depth = FMath::Max(0.f,Event.Severity)*.001f*(1.f+.5f*FMath::Clamp(Cell.RiverProximity,0.f,1.f));
			// ComputeDamage applies the legacy prevention capacity exactly once.
			return UDisasterMath::FloodSeverityAt(Depth, 0.f);
		}

		case EDisasterType::Wind:
			return UDisasterMath::WindSeverityAt(Event.Severity, Cell.ValleyChannelFactor, Building->Floors);

		case EDisasterType::Fire:
			return UDisasterMath::FireSeverityAt(Cell.BurningTurns);
	}

	return 0.f;
}
