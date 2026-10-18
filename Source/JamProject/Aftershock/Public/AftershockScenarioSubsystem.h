#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Aftershock/Public/BuildingData.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "AftershockScenarioSubsystem.generated.h"

class AActor;

struct FAftershockScenarioSaveState;

/** Authored neighbourhood lots; buildings and population still belong to their existing managers. */
struct FAftershockScenarioLot
{
	FIntPoint Coord = FIntPoint::ZeroValue;
	EBuildingType Type = EBuildingType::House;
	EFoundationQuality Foundation = EFoundationQuality::Medium;
	int32 Floors = 1;
};

/** Derived startup diagnostics. Canonical buildings/roads remain the saved authority. */
struct FAftershockCityPlanStatistics
{
	int32 GeneratorVersion=1;
	int32 Seed=0;
	int32 Blocks=0;
	int32 ActiveBlocks=0;
	int32 DistrictUseTypes=0;
	int32 CandidateSlots=0;
	int32 SteepSlotsRejected=0;
	int32 TerrainCellsVisited=0;
	int32 FootprintChecks=0;
	int32 PlannedStreetCells=0;
	int32 ActualLots=0;
	int32 ResidentialLots=0;
	int32 WorkLots=0;
	int32 FoodLots=0;
	int32 OpenSpaceLots=0;
	uint32 LayoutHash=0;
	FString MorphologyId;
	int32 ProfileMaximumLots=0;
	int32 ChosenLandComponentCells=0;
	int32 LargestLandComponentCells=0;
	int32 LandComponentsConsidered=0;
	double PlannerCpuMilliseconds=0.;
};

struct FAftershockScenarioDefinition
{
	FString Name;
	FString Briefing;
	EDisasterType Hazard = EDisasterType::Earthquake;
	int32 Seed = 1337;
	int32 StartYear = 2026;
	int64 StartingBudgetTL = 50000;
	float BaseSeverity = 42.f;
	float InitialAnomalyC = 1.4f;
	int32 GeographyIndex = 0;
	double Latitude = 41.041;
	double Longitude = 28.775;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnAftershockScenarioUpdated);

/** Authored first-playable scenarios, sharing the existing level and canonical grid. */
UCLASS()
class JAMPROJECT_API UAftershockScenarioSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	void CaptureSaveState(FAftershockScenarioSaveState& Out) const;
	bool RestoreSaveState(const FAftershockScenarioSaveState& State,FText& OutError);
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// Explicit startup boundary: camera calls this before terrain visuals and settlement seeding.
	bool PrepareScenarioBeforeSettlement();
	void NotifySettlementSeeded();
	bool IsPrepared() const { return bPrepared; }
	int32 GetScenarioIndex() const { return ScenarioIndex; }
	static constexpr int32 GetScenarioCount() { return 7; }
	bool IsFinalScenario() const { return ScenarioIndex >= GetScenarioCount() - 1; }
	int32 GetStarterRoadCellCount() const { return StarterRoadCellCount; }
	const FAftershockScenarioDefinition& GetDefinition() const { return Definition; }
	const TArray<FAftershockScenarioLot>& GetDesignedLots() const { return DesignedLots; }
	const TArray<FIntPoint>& GetAssemblyCells() const { return AssemblyCells; }
	const FAftershockCityPlanStatistics& GetCityPlanStatistics() const { return CityPlanStatistics; }
	const TArray<FIntPoint>& GetPlannedPublicCorridors() const { return PlannedPublicCorridors; }
	FText GetBriefingText() const;
	FText GetProgressText() const;
	bool CanAdvanceScenario() const;
	bool AdvanceToNextScenario();
	float GetScenarioBaseSeverity() const;
	float GetScenarioSeverityForHazard(EDisasterType Type) const;

	UPROPERTY(BlueprintAssignable)
	FOnAftershockScenarioUpdated OnScenarioUpdated;

private:
	UFUNCTION() void HandleEconomyChanged(int64 TreasuryTL, float Satisfaction, int32 Turn);
	UFUNCTION() void HandleDisasterCompleted(const FDisasterReport& Report);
	void BuildDesignedLots();
	/** Immutable0152f4d authored V1/V2 fixture path; new real-source V3 uses the morphology planner. */
	void BuildLegacyDesignedLots();
	void BuildAssemblyMarkers();
	void SeedStarterStreets();
	void GetObjectiveCounts(int32& Roads, int32& Preparedness, int32& Centres) const;
	int32 ScenarioIndex = 0;
	bool bPrepared = false;
	bool bPrimaryDisasterSurvived = false;
	bool bStarterRoadsInitialized = false;
	int32 StarterRoadCellCount = 0;
	FAftershockScenarioDefinition Definition;
	TArray<FAftershockScenarioLot> DesignedLots;
	TArray<FIntPoint> AssemblyCells;
	/** Transient construction plan only; restore uses saved actual road/cell/building records. */
	TArray<FIntPoint> PlannedPublicCorridors;
	FAftershockCityPlanStatistics CityPlanStatistics;
	UPROPERTY(Transient) TObjectPtr<AActor> AssemblyMarkerHost;
};
