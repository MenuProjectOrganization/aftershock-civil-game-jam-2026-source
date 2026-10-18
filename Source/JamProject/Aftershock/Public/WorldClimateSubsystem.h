#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WorldClimateSubsystem.generated.h"

/** Annual design scenario, not a weather forecast or an observed temperature. */
USTRUCT(BlueprintType)
struct FAftershockWorldClimateState
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) int32 ScenarioStartYear = 2026;
	UPROPERTY(BlueprintReadOnly) int32 CurrentYear = 2026;
	UPROPERTY(BlueprintReadOnly) float InitialAnomalyC = 1.4f;
	UPROPERTY(BlueprintReadOnly) float GlobalTemperatureAnomalyC = 1.4f;
	UPROPERTY(BlueprintReadOnly) float AnnualTrendC = 0.025f;
	/** Dimensionless city pollution pressure; never interpreted as ppm or global degrees. */
	UPROPERTY(BlueprintReadOnly) float LocalUrbanHeatPressure = 0.f;
	/** Artistic regional response factors. These do not modify disaster physics. */
	UPROPERTY(BlueprintReadOnly) float DroughtVisualPressure = 0.f;
	UPROPERTY(BlueprintReadOnly) float IceCoverageFraction = 1.f;
	UPROPERTY(BlueprintReadOnly) float StormVisualPressure = 0.f;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnAftershockWorldClimateUpdated, const FAftershockWorldClimateState&);

/** Single annual scenario temperature authority; economy retains its existing risk contract. */
UCLASS(BlueprintType)
class JAMPROJECT_API UWorldClimateSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	void CaptureSaveState(FAftershockWorldClimateState& Out) const { Out=State; }
	bool RestoreSaveState(const FAftershockWorldClimateState& Saved,FText& OutError);
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	UFUNCTION(BlueprintCallable, Category="Aftershock|Scenario Climate")
	void SetScenarioClimate(int32 StartYear, float InitialAnomalyC, float AnnualTrendC);
	UFUNCTION(BlueprintCallable, Category="Aftershock|Scenario Climate")
	void RefreshFromEconomy();
	const FAftershockWorldClimateState& GetState() const { return State; }
	UFUNCTION(BlueprintPure, Category="Aftershock|Scenario Climate")
	FAftershockWorldClimateState GetScenarioClimateState() const { return State; }
	FOnAftershockWorldClimateUpdated OnWorldClimateUpdated;

private:
	UFUNCTION() void HandleEconomyUpdated(int64 TreasuryTL, float Satisfaction, int32 CurrentTurn);
	UFUNCTION() void HandleLocalClimateUpdated(float CO2Level, float ClimateRiskMultiplier);
	UPROPERTY() FAftershockWorldClimateState State;
};
