#pragma once
#include "CoreMinimal.h"
#include "Aftershock/Public/BuildingData.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "AftershockPhysicalHazardState.generated.h"

/** Saved completed hydraulic frontier. Geometry remains owned by Terrain. */
USTRUCT()
struct FAftershockHydraulicCellSaveRecord
{
    GENERATED_BODY()
    UPROPERTY() FIntPoint Coord=FIntPoint::ZeroValue;
    /** Exact committed SWE bed; may be frozen while finite mass moves above it. */
    UPROPERTY() double BedElevationCm=0.;
    UPROPERTY() double VolumeM3=0.;
    UPROPERTY() double DischargeXM2PerSecond=0.;
    UPROPERTY() double DischargeYM2PerSecond=0.;
    UPROPERTY() double CumulativeInfiltrationM3=0.;
};

/** Exact immutable controls of the ONE accepted, unfinished SI water quantum. */
USTRUCT()
struct FAftershockHydraulicForcingCellSaveRecord
{
    GENERATED_BODY()
    UPROPERTY() FIntPoint Coord=FIntPoint::ZeroValue;
    UPROPERTY() double RainMPerSecond=0.;
    UPROPERTY() double InfiltrationCapacityMPerSecond=0.;
    UPROPERTY() double ExternalDrainCapacityM3PerSecond=0.;
    UPROPERTY() double EvaporationMPerSecond=0.;
    UPROPERTY() double ExternalInflowM3PerSecond=0.;
    UPROPERTY() double InflowVelocityXMps=0.;
    UPROPERTY() double InflowVelocityYMps=0.;
};

/** Only actual StageBoundary controls; absent walls/interior faces are NOT an inflow. */
USTRUCT()
struct FAftershockHydraulicForcingBoundarySaveRecord
{
    GENERATED_BODY()
    UPROPERTY() int64 FaceStableID=0;
    UPROPERTY() double HeadM=0.;
    UPROPERTY() double VelocityXMps=0.;
    UPROPERTY() double VelocityYMps=0.;
};

USTRUCT()
struct FAftershockHydraulicBudgetSaveRecord
{
    GENERATED_BODY()
    UPROPERTY() double InitialSurfaceM3=0.;
    UPROPERTY() double InitialStorageM3=0.;
    UPROPERTY() double RainM3=0.;
    UPROPERTY() double ExternalInflowM3=0.;
    UPROPERTY() double BoundaryInflowM3=0.;
    UPROPERTY() double BoundaryOutflowM3=0.;
    UPROPERTY() double InfiltrationM3=0.;
    UPROPERTY() double ExternalDrainageM3=0.;
    UPROPERTY() double EvaporationM3=0.;
    UPROPERTY() double StorageExternalOutflowM3=0.;
    UPROPERTY() double FinalSurfaceM3=0.;
    UPROPERTY() double FinalStorageM3=0.;
    UPROPERTY() double NumericalVolumeCorrectionM3=0.;
    UPROPERTY() double ResidualM3=0.;
    UPROPERTY() uint64 AcceptedSteps=0;
    UPROPERTY() uint64 FaceEvaluations=0;
};

/** Historical instance data has no hard asset pointer in a save. */
USTRUCT()
struct FAftershockPhysicalBuildingBaseline
{
    GENERATED_BODY()
    UPROPERTY() FBuildingInstance State;
    UPROPERTY() EBuildingType DefinitionType=EBuildingType::House;
};

USTRUCT()
struct FAftershockPhysicalBuildingExposure
{
    GENERATED_BODY()
    UPROPERTY() int32 BuildingID=INDEX_NONE;
    UPROPERTY() double PeakDepthM=0.;
    UPROPERTY() double PeakSpeedMps=0.;
    UPROPERTY() double WetSeconds=0.;
    UPROPERTY() double PeakPressurePa=0.;
    UPROPERTY() double ImpulseNs=0.;
    UPROPERTY() float AppliedDamageDemand=0.f;
    UPROPERTY() int32 AssignedDeaths=0;
    UPROPERTY() bool bCollapseCounted=false;
};

/** One accepted episode settles its resident IDs and fiscal result once. */
USTRUCT()
struct FAftershockPhysicalEpisodeSaveState
{
    GENERATED_BODY()
    UPROPERTY() bool bActive=false;
    UPROPERTY() bool bOutcomeCommitted=false;
    UPROPERTY() FGuid EventID;
    UPROPERTY() EDisasterType Type=EDisasterType::Flood;
    UPROPERTY() float BaseSeverity=0.f;
    UPROPERTY() float CityAwareness01=0.f;
    UPROPERTY() bool bDaytime=true;
    UPROPERTY() double SourceDurationSeconds=0.;
    UPROPERTY() double StartPhysicalSeconds=0.;
    UPROPERTY() double SourceElapsedSeconds=0.;
    UPROPERTY() double RainMPerSecond=0.;
    UPROPERTY() double SimulationTimeScale=1.;
    UPROPERTY() double PlaybackSeconds=0.;
    UPROPERTY() int32 CollapsedBuildings=0;
    UPROPERTY() int32 PreventedDeathsEstimate=0;
    UPROPERTY() TArray<FCityResidentRecord> BeforeResidents;
    UPROPERTY() TArray<FAftershockPhysicalBuildingBaseline> BeforeBuildings;
    UPROPERTY() TArray<FAftershockPhysicalBuildingExposure> Exposures;
    UPROPERTY() TArray<int32> ActualDeathResidentIDs;
    UPROPERTY() TArray<int32> ExposedResidentIDs;
    /** Bounded, explicitly versioned pure mass solver replay, no UObject paths. */
    UPROPERTY() TArray<uint8> MassReplay;
    UPROPERTY() double ReleasedMixtureWaterM3=0.;
    UPROPERTY() double TransferredMixtureWaterM3=0.;
};

USTRUCT()
struct FAftershockPhysicalHazardSaveState
{
    GENERATED_BODY()
    /** 0=no persistent hydrology yet; 2=SI shallow-water owner. */
    UPROPERTY() int32 ModelVersion=0;
    UPROPERTY() bool bNeedsLegacyWaterSeed=false;
    UPROPERTY() bool bWaterInitialized=false;
    UPROPERTY() double CapturedCellAreaM2=0.;
    UPROPERTY() double PhysicalSeconds=0.;
    UPROPERTY() double PendingPhysicalSeconds=0.;
    /** Accepted packet remainder INCLUDED in PendingPhysicalSeconds, never added twice. */
    UPROPERTY() double PendingAcceptedSeconds=0.;
    UPROPERTY() TArray<FAftershockHydraulicForcingCellSaveRecord> PendingForcingCells;
    UPROPERTY() TArray<FAftershockHydraulicForcingBoundarySaveRecord> PendingForcingBoundaries;
    UPROPERTY() uint64 PublishedStep=0;
    UPROPERTY() uint64 DomainRevision=0;
    UPROPERTY() TArray<FAftershockHydraulicCellSaveRecord> WaterCells;
    UPROPERTY() FAftershockHydraulicBudgetSaveRecord WaterBudget;
    UPROPERTY() FAftershockPhysicalEpisodeSaveState Episode;
    /** Original authored weather/material assumptions; UTF-8 JSON <=256 KiB. Empty on legacy migration. */
    UPROPERTY() FString MaterialScenarioReceiptJSON;
};
