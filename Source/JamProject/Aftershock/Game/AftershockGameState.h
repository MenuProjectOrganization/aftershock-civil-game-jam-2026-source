// AFTERSHOCK — Civil Game Jam 2026
// Bütçe, memnuniyet, çevre skorlarının merkezi.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "Aftershock/Types/AftershockTypes.h"
#include "Aftershock/Simulation/DisasterMath.h"
#include "AftershockGameState.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnBudgetChanged, int32, NewBudget);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSatisfactionChanged, float, NewSatisfaction);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEnvironmentChanged, float, NewEnvironment);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnGameOver);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnLevelAdvance);

UCLASS(BlueprintType)
class JAMPROJECT_API AAftershockGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	AAftershockGameState();

	// -------- EKONOMİ --------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Economy")
	int32 Budget = 10000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Economy")
	int32 IncomePerTurn = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Economy")
	int32 ExpensePerTurn = 0;

	// -------- MEMNUNİYET --------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Satisfaction", meta=(ClampMin="0.0", ClampMax="100.0"))
	float Satisfaction = 70.f;

	// -------- ÇEVRE --------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Environment", meta=(ClampMin="0.0", ClampMax="100.0"))
	float Environment = 50.f;

	// -------- CITY STATE (önleme) --------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|City")
	FCityPreventionState CityPrevention;

	// -------- LEVEL --------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Level")
	int32 CurrentLevel = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Level")
	int32 TotalLevels = 5;

	// -------- GAME OVER EŞİKLERİ --------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Rules")
	float SatisfactionGameOverThreshold = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Rules")
	int32 NegativeBudgetTurnsAllowed = 3;

	// -------- EVENTS --------
	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnBudgetChanged OnBudgetChanged;

	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnSatisfactionChanged OnSatisfactionChanged;

	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnEnvironmentChanged OnEnvironmentChanged;

	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnGameOver OnGameOver;

	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnLevelAdvance OnLevelAdvance;

	// -------- API --------
	UFUNCTION(BlueprintCallable, Category="Aftershock|Economy")
	bool TrySpend(int32 Amount);

	UFUNCTION(BlueprintCallable, Category="Aftershock|Economy")
	void AddIncome(int32 Amount);

	UFUNCTION(BlueprintCallable, Category="Aftershock|Satisfaction")
	void AddSatisfaction(float Delta);

	UFUNCTION(BlueprintCallable, Category="Aftershock|Environment")
	void AddEnvironment(float Delta);

	/** Tur sonu — gelir/gider işler, bütçeyi günceller, Game Over kontrolü yapar. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Turn")
	void EndTurn(int32 TurnIncome, int32 TurnExpense, float TurnCO2);

	/** Level sonu — outcome uygular, Game Over kontrol, sonraki level'a geç. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Level")
	void ApplyLevelOutcome(const FLevelOutcome& Outcome);
	UPROPERTY() TSet<FGuid> AppliedLegacyOutcomeIDs;

	UFUNCTION(BlueprintCallable, Category="Aftershock|Level")
	void AdvanceLevel();

	UFUNCTION(BlueprintPure, Category="Aftershock|Level")
	bool IsFinalLevel() const { return CurrentLevel >= TotalLevels; }

	/** Yatırım yardımcı: prevention bir kategoride artır. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|City")
	bool InvestInPrevention(EPreventionType Type, int32 CostTL);

protected:
	int32 NegativeBudgetStreak = 0;

	void CheckGameOver();
};
