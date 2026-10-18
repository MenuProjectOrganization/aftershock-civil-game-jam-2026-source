#include "Aftershock/Game/AftershockGameState.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Engine/World.h"

AAftershockGameState::AAftershockGameState()
{
}

bool AAftershockGameState::TrySpend(int32 Amount)
{
	if (Amount <= 0) return true;
	if (Budget < Amount) return false;
	Budget -= Amount;
	OnBudgetChanged.Broadcast(Budget);
	return true;
}

void AAftershockGameState::AddIncome(int32 Amount)
{
	Budget += Amount;
	OnBudgetChanged.Broadcast(Budget);
}

void AAftershockGameState::AddSatisfaction(float Delta)
{
	Satisfaction = FMath::Clamp(Satisfaction + Delta, 0.f, 100.f);
	OnSatisfactionChanged.Broadcast(Satisfaction);
}

void AAftershockGameState::AddEnvironment(float Delta)
{
	Environment = FMath::Clamp(Environment + Delta, 0.f, 100.f);
	CityPrevention.EnvironmentScore = Environment;
	OnEnvironmentChanged.Broadcast(Environment);
}

void AAftershockGameState::EndTurn(int32 TurnIncome, int32 TurnExpense, float TurnCO2)
{
	IncomePerTurn = TurnIncome;
	ExpensePerTurn = TurnExpense;

	Budget += TurnIncome;
	Budget -= TurnExpense;
	OnBudgetChanged.Broadcast(Budget);

	// CO2 → çevre
	AddEnvironment(-TurnCO2 * 0.1f);

	if (Budget < 0) NegativeBudgetStreak++;
	else NegativeBudgetStreak = 0;

	CheckGameOver();
}

void AAftershockGameState::ApplyLevelOutcome(const FLevelOutcome& Outcome)
{
	if(const auto* Economy=GetWorld()?GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>():nullptr)
	{
		// This compatibility actor is a read-only mirror in canonical worlds.
		// Real resident IDs and exactly-once settlement belong to the subsystem.
		Budget=static_cast<int32>(FMath::Clamp<int64>(Economy->TreasuryTL,MIN_int32,MAX_int32));Satisfaction=Economy->PublicSatisfaction;
		OnBudgetChanged.Broadcast(Budget);OnSatisfactionChanged.Broadcast(Satisfaction);return;
	}
	if(!Outcome.EventID.IsValid()||AppliedLegacyOutcomeIDs.Contains(Outcome.EventID))return;
	AppliedLegacyOutcomeIDs.Add(Outcome.EventID);
	const int64 Net=static_cast<int64>(FMath::Max(0,Outcome.TotalSurvivors))*150-static_cast<int64>(FMath::Max(0,Outcome.TotalDeaths))*500;
	Budget=static_cast<int32>(FMath::Clamp<int64>(static_cast<int64>(Budget)+Net,MIN_int32,MAX_int32));OnBudgetChanged.Broadcast(Budget);
	AddSatisfaction(Outcome.SatisfactionDelta);
	AddEnvironment(Outcome.EnvironmentDelta);
	CheckGameOver();
}

void AAftershockGameState::AdvanceLevel()
{
	if (IsFinalLevel()) return;
	CurrentLevel++;
	OnLevelAdvance.Broadcast();
}

bool AAftershockGameState::InvestInPrevention(EPreventionType Type, int32 CostTL)
{
	if (!TrySpend(CostTL)) return false;

	switch (Type)
	{
		case EPreventionType::SeismicRetrofit:
			// Bu şehir çapında bir etki değil — tek binaya uygulanır (ApplyRetrofit() via UI)
			break;
		case EPreventionType::DrainageSystem:
			CityPrevention.DrainageCapacity = FMath::Min(1.f, CityPrevention.DrainageCapacity + 0.25f);
			break;
		case EPreventionType::Breakwater:
			CityPrevention.EarlyWarning = FMath::Min(1.f, CityPrevention.EarlyWarning + 0.2f);
			break;
		case EPreventionType::WindShutter:
			// Tek bina — UI'dan apply
			break;
		case EPreventionType::FirebreakZone:
			CityPrevention.EmergencyCapacity = FMath::Min(1.f, CityPrevention.EmergencyCapacity + 0.15f);
			break;
		case EPreventionType::TsunamiSiren:
			CityPrevention.EarlyWarning = FMath::Min(1.f, CityPrevention.EarlyWarning + 0.4f);
			break;
		case EPreventionType::AwarenessCampaign:
			CityPrevention.AwarenessLevel = FMath::Min(1.f, CityPrevention.AwarenessLevel + 0.2f);
			break;
		case EPreventionType::EmergencyUnit:
			CityPrevention.EmergencyCapacity = FMath::Min(1.f, CityPrevention.EmergencyCapacity + 0.3f);
			break;
	}
	return true;
}

void AAftershockGameState::CheckGameOver()
{
	if (Satisfaction <= SatisfactionGameOverThreshold)
	{
		OnGameOver.Broadcast();
		return;
	}
	if (NegativeBudgetStreak >= NegativeBudgetTurnsAllowed)
	{
		OnGameOver.Broadcast();
	}
}
