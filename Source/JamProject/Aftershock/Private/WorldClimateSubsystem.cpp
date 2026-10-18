#include "Aftershock/Public/WorldClimateSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Engine/World.h"

bool UWorldClimateSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return Super::ShouldCreateSubsystem(Outer) && World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UWorldClimateSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UEconomyAndStateSubsystem>();
	if (UEconomyAndStateSubsystem* Economy = GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>())
	{
		Economy->OnEconomyUpdated.AddUniqueDynamic(this, &UWorldClimateSubsystem::HandleEconomyUpdated);
		Economy->OnClimateUpdated.AddUniqueDynamic(this, &UWorldClimateSubsystem::HandleLocalClimateUpdated);
	}
	RefreshFromEconomy();
}

void UWorldClimateSubsystem::SetScenarioClimate(int32 StartYear, float InitialAnomalyC, float AnnualTrendC)
{
	State.ScenarioStartYear = FMath::Clamp(StartYear, 1900, 2300);
	State.InitialAnomalyC = FMath::Clamp(FMath::IsFinite(InitialAnomalyC) ? InitialAnomalyC : 1.4f, -1.f, 8.f);
	State.AnnualTrendC = FMath::Clamp(FMath::IsFinite(AnnualTrendC) ? AnnualTrendC : 0.025f, -0.1f, 0.15f);
	RefreshFromEconomy();
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_WORLD_CLIMATE scenarioModel=1 startYear=%d initialAnomalyC=%.3f trendCPerYear=%.3f cityCO2IsGlobalDriver=0 gameplayRiskChanged=0"),
		State.ScenarioStartYear, State.InitialAnomalyC, State.AnnualTrendC);
}

void UWorldClimateSubsystem::RefreshFromEconomy()
{
	const UEconomyAndStateSubsystem* Economy = GetWorld() ? GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	State.CurrentYear = Economy ? Economy->CurrentYear : State.ScenarioStartYear;
	const int32 ElapsedYears = FMath::Max(0, State.CurrentYear - State.ScenarioStartYear);
	State.GlobalTemperatureAnomalyC = FMath::Clamp(State.InitialAnomalyC + ElapsedYears * State.AnnualTrendC, -1.f, 8.f);
	const float LocalPollution = Economy && FMath::IsFinite(Economy->CityCO2Level) ? FMath::Max(0.f, Economy->CityCO2Level) : 0.f;
	State.LocalUrbanHeatPressure = LocalPollution / (100.f + LocalPollution);
	// These bounded art factors describe one fictional region. They are neither
	// probabilities nor a global prediction that every location becomes drier.
	State.DroughtVisualPressure = FMath::Clamp((State.GlobalTemperatureAnomalyC - 1.f) / 3.f, 0.f, 1.f);
	State.IceCoverageFraction = FMath::Clamp(1.f - FMath::Max(0.f, State.GlobalTemperatureAnomalyC) * 0.14f, 0.1f, 1.f);
	State.StormVisualPressure = FMath::Clamp((State.GlobalTemperatureAnomalyC - 1.f) / 4.f, 0.f, 1.f);
	OnWorldClimateUpdated.Broadcast(State);
}

void UWorldClimateSubsystem::HandleEconomyUpdated(int64 TreasuryTL, float Satisfaction, int32 CurrentTurn)
{
	RefreshFromEconomy();
}

void UWorldClimateSubsystem::HandleLocalClimateUpdated(float CO2Level, float ClimateRiskMultiplier)
{
	RefreshFromEconomy();
}

void UWorldClimateSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		if (UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>())
		{
			Economy->OnEconomyUpdated.RemoveDynamic(this, &UWorldClimateSubsystem::HandleEconomyUpdated);
			Economy->OnClimateUpdated.RemoveDynamic(this, &UWorldClimateSubsystem::HandleLocalClimateUpdated);
		}
	}
	OnWorldClimateUpdated.Clear();
	Super::Deinitialize();
}
