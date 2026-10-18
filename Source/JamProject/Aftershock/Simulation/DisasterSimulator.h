// AFTERSHOCK — Civil Game Jam 2026
// Afet tetikleyici. GameMode veya GameState altında yaşar.
// Trigger(Event) → her binaya hasar hesabı uygular, FLevelOutcome üretir.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Aftershock/Types/AftershockTypes.h"
#include "Aftershock/Simulation/DisasterMath.h"
#include "DisasterSimulator.generated.h"

class AGridManager;
class ABuildingActor;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDisasterWarning, const FDisasterEvent&, Event);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDisasterOnset, const FDisasterEvent&, Event);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDisasterComplete, const FLevelOutcome&, Outcome);

UCLASS(ClassGroup=(Aftershock), meta=(BlueprintSpawnableComponent))
class JAMPROJECT_API UDisasterSimulator : public UActorComponent
{
	GENERATED_BODY()

public:
	UDisasterSimulator();

	// -------- EVENTS --------
	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnDisasterWarning OnDisasterWarning;

	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnDisasterOnset OnDisasterOnset;

	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnDisasterComplete OnDisasterComplete;

	// -------- PUBLIC API --------
	/** Afeti planla. WarningSeconds sonra Onset olur. */
	UFUNCTION(BlueprintCallable, Category="Aftershock")
	void ScheduleDisaster(const FDisasterEvent& Event);

	/** Doğrudan tetikle (debug / instant). */
	UFUNCTION(BlueprintCallable, Category="Aftershock")
	FLevelOutcome TriggerNow(const FDisasterEvent& Event);

	UFUNCTION(BlueprintCallable, Category="Aftershock")
	void SetGridManager(AGridManager* InGridManager) { GridManager = InGridManager; }

	UFUNCTION(BlueprintCallable, Category="Aftershock")
	void SetCityState(const FCityPreventionState& InState) { CityState = InState; }

	UFUNCTION(BlueprintPure, Category="Aftershock")
	FCityPreventionState GetCityState() const { return CityState; }

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock")
	TObjectPtr<AGridManager> GridManager;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock")
	FCityPreventionState CityState;

	UPROPERTY()
	FDisasterEvent PendingEvent;

	FTimerHandle OnsetTimerHandle;

	UFUNCTION()
	void HandleOnset();

	float ComputeRawSeverity(const FDisasterEvent& Event, ABuildingActor* Building, const FTerrainCell& Cell) const;
};
