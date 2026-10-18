#pragma once
#include "CoreMinimal.h"
#include "Aftershock/Public/AftershockSessionTypes.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/TransportSubsystem.h"
#include "Aftershock/Public/WorldClimateSubsystem.h"
#include "Aftershock/Types/AftershockTypes.h"
#include "Aftershock/Narrative/CasualtyLedgerGenerator.h"
#include "AftershockSaveTypes.generated.h"

USTRUCT()
struct FAftershockTerrainSaveState
{
 GENERATED_BODY()
 UPROPERTY() int32 GridWidth=50;
 UPROPERTY() int32 GridHeight=50;
 UPROPERTY() float CellSize=500.f;
 UPROPERTY() FVector GridOrigin=FVector::ZeroVector;
 UPROPERTY() int32 TerrainSeed=1337;
 UPROPERTY() int32 GeographyIndex=0;
 UPROPERTY() int32 GeographyFocusIndex=0;
 UPROPERTY() float ActiveDroughtSeverity=0.f;
 UPROPERTY() FIntPoint VolcanicVentCoord=FIntPoint(INDEX_NONE,INDEX_NONE);
 UPROPERTY() int32 CoastStartRow=INDEX_NONE;
 UPROPERTY() float CoastalSeaLevelCm=24.f;
 UPROPERTY() TArray<FGridCell> Cells;
 /** 1=legacy dense initial frame, 2=stable append cells plus derived chunk manifest. */
 UPROPERTY() int32 TerrainStorageVersion=1;
 UPROPERTY() int32 ChunkSize=25;
 UPROPERTY() int32 ContinuationGeneratorVersion=1;
 UPROPERTY() bool bUsesRegionalLayout=false;
 UPROPERTY() bool bInitialMaskUnlocked=false;
 UPROPERTY() uint64 TerrainRevision=0;
 UPROPERTY() TArray<FAftershockTerrainChunkState> Chunks;
};

USTRUCT()
struct FAftershockCrowdSaveState
{
 GENERATED_BODY()
 UPROPERTY() TArray<FCitizenAgent> Agents;
 UPROPERTY() int32 SimulationSeed=1337;
 UPROPERTY() bool bPanic=false;
 UPROPERTY() EDisasterType EmergencyDisasterType=EDisasterType::Earthquake;
 UPROPERTY() float RemainingEmergencySeconds=0.f;
};

USTRUCT()
struct FAftershockRubbleSaveRecord
{
 GENERATED_BODY()
 /** Empty only for legacy canonical collections; explicit paths use a strict building/floor allowlist. */
 UPROPERTY() FSoftObjectPath FracturedAssetPath;
 UPROPERTY() int32 BuildingID=INDEX_NONE;
 UPROPERTY() FIntPoint Coord=FIntPoint::ZeroValue;
 UPROPERTY() FTransform ActorTransform=FTransform::Identity;
 UPROPERTY() TArray<FTransform> LocalRestTransforms;
};

USTRUCT()
struct FAftershockBuildingSaveRecord
{
 GENERATED_BODY()
 UPROPERTY() FBuildingInstance State;
 UPROPERTY() EBuildingType DefinitionType=EBuildingType::House;
 UPROPERTY() float Yaw=0.f;
 UPROPERTY() FTransform WorldTransform=FTransform::Identity;
};
USTRUCT()
struct FAftershockBuildingsSaveState
{
 GENERATED_BODY()
 UPROPERTY() TArray<FAftershockBuildingSaveRecord> Buildings;
 UPROPERTY() int32 NextBuildingID=1;
};
USTRUCT()
struct FAftershockEconomySaveState
{
 GENERATED_BODY()
 UPROPERTY() int32 Turn=1;
 UPROPERTY() int32 Year=2026;
 UPROPERTY() int64 Treasury=50000;
 UPROPERTY() float Satisfaction=100.f;
 UPROPERTY() float CO2=0.f;
 UPROPERTY() float ClimateRisk=1.f;
 UPROPERTY() int32 DisasterLevel=1;
 UPROPERTY() int64 PendingAid=0;
 UPROPERTY() bool bGameOver=false;
 UPROPERTY() int32 SurvivedDisasters=0;
 UPROPERTY() TArray<FGuid> AppliedDisasterEventIDs;
};
USTRUCT()
struct FAftershockCityLifeSaveState
{
 GENERATED_BODY()
 UPROPERTY() bool bInitialized=false;
 UPROPERTY() TArray<FCityResidentRecord> Residents;
 UPROPERTY() FCityLifeState State;
 UPROPERTY() TMap<int32,FCityBuildingStatus> BuildingStates;
 UPROPERTY() TMap<int32,int32> BuildingRoadComponents;
 UPROPERTY() int32 NextResidentID=1;
 UPROPERTY() TSet<int32> SeededHomes;
 UPROPERTY() int32 SupplyDisruptionTurns=0;
 UPROPERTY() TMap<int64,int32> LastServiceVisitTurn;
 UPROPERTY() TSet<int32> PaidTransitTrips;
 UPROPERTY() int64 PendingTransitRevenueTL=0;
};
USTRUCT()
struct FAftershockTransportSaveState
{
 GENERATED_BODY()
 UPROPERTY() TArray<FAftershockPassengerTrip> Trips;
 UPROPERTY() TArray<FAftershockRoadVehicle> Vehicles;
 UPROPERTY() TArray<FAftershockTransitStop> Stops;
 UPROPERTY() bool bEmergencyActive=false;
 UPROPERTY() bool bInfrastructureDirty=true;
 UPROPERTY() float InfrastructureRefreshSeconds=0.f;
 UPROPERTY() float HistoryRefreshSeconds=0.f;
 UPROPERTY() int32 NextTripID=1;
 UPROPERTY() int32 NextVehicleID=1;
 UPROPERTY() float HistoryClockSeconds=0.f;
 UPROPERTY() FAftershockTransportStats RetiredTripStats;
};
USTRUCT()
struct FAftershockScenarioSaveState
{
 GENERATED_BODY()
 UPROPERTY() int32 ScenarioIndex=0;
 UPROPERTY() bool bPrimaryDisasterSurvived=false;
 UPROPERTY() TArray<FIntPoint> AssemblyCells;
 UPROPERTY() int32 StarterRoadCellCount=0;
 UPROPERTY() FString Name;
 UPROPERTY() FString Briefing;
 UPROPERTY() EDisasterType Hazard=EDisasterType::Earthquake;
 UPROPERTY() int32 Seed=1337;
 UPROPERTY() int32 StartYear=2026;
 UPROPERTY() int64 StartingBudgetTL=50000;
 UPROPERTY() float BaseSeverity=42.f;
 UPROPERTY() float InitialAnomalyC=1.4f;
 UPROPERTY() int32 GeographyIndex=0;
 UPROPERTY() double Latitude=0;
 UPROPERTY() double Longitude=0;
};
USTRUCT()
struct FAftershockQuizSaveState
{
 GENERATED_BODY()
 UPROPERTY() FQuizQuestion CurrentQuestion;
 UPROPERTY() TSet<FName> AskedRowNames;
 UPROPERTY() int32 AnsweredCount=0;
 UPROPERTY() int32 CorrectCount=0;
 UPROPERTY() bool bQuestionPending=false;
 UPROPERTY() int32 LastAskedTurn=INDEX_NONE;
 UPROPERTY() int32 RandomSeed=9182;
};
USTRUCT()
struct FAftershockCameraSaveState
{
 GENERATED_BODY()
 UPROPERTY() FVector Location=FVector::ZeroVector;
 UPROPERTY() FRotator Rotation=FRotator::ZeroRotator;
 UPROPERTY() FRotator Orbit=FRotator(-48,-45,0);
 UPROPERTY() float Zoom=8500.f;
 UPROPERTY() float OverviewBlend=0.f;
 UPROPERTY() float SavedLocalZoom=8500.f;
 UPROPERTY() float PlanetOrbitPitch=-48.f;
 UPROPERTY() FVector LocalFocus=FVector::ZeroVector;
};
