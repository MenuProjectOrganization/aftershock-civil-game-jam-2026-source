#pragma once
#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "AftershockLegacyV4Types.generated.h"

class UBuildingData;

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4BuildingInstance
{
    GENERATED_BODY()
    UPROPERTY() int32 InstanceID{};
    UPROPERTY() FIntPoint GridCoords{};
    UPROPERTY() TObjectPtr<UBuildingData> BuildingData{};
    UPROPERTY() int32 CurrentFloors{};
    UPROPERTY() FName Foundation{};
    UPROPERTY() FName FoundationQuality{};
    UPROPERTY() float StructuralQuality{};
    UPROPERTY() float RoofQuality{};
    UPROPERTY() float Preparedness{};
    UPROPERTY() int32 AgeInYears{};
    UPROPERTY() bool bIsRetrofitted{};
    UPROPERTY() float CurrentDamage{};
    UPROPERTY() FName DamageState{};
    UPROPERTY() int32 ISMInstanceIndex{};
    UPROPERTY() int32 MemorialStartedTurn{};
    UPROPERTY() int32 MemorialUnlockTurn{};
    UPROPERTY() TArray<int32> MemorialCasualtyResidentIDs{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockBuildingSaveRecord
{
    GENERATED_BODY()
    UPROPERTY() FLegacyV4BuildingInstance State{};
    UPROPERTY() FName DefinitionType{};
    UPROPERTY() float Yaw{};
    UPROPERTY() FTransform WorldTransform{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockBuildingsSaveState
{
    GENERATED_BODY()
    UPROPERTY() TArray<FLegacyV4AftershockBuildingSaveRecord> Buildings{};
    UPROPERTY() int32 NextBuildingID{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockCameraSaveState
{
    GENERATED_BODY()
    UPROPERTY() FVector Location{};
    UPROPERTY() FRotator Rotation{};
    UPROPERTY() FRotator Orbit{};
    UPROPERTY() float Zoom{};
    UPROPERTY() float OverviewBlend{};
    UPROPERTY() float SavedLocalZoom{};
    UPROPERTY() float PlanetOrbitPitch{};
    UPROPERTY() FVector LocalFocus{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockCampaignProgress
{
    GENERATED_BODY()
    UPROPERTY() TSet<int32> CompletedStages{};
    UPROPERTY() int32 FurthestStage{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockCasualtyEventRecord
{
    GENERATED_BODY()
    UPROPERTY() FGuid EventID{};
    UPROPERTY() int32 Turn{};
    UPROPERTY() int32 ActualDeaths{};
    UPROPERTY() int32 ExposedSurvivors{};
    UPROPERTY() int64 DeathCostTL{};
    UPROPERTY() int64 SurvivorAidTL{};
    UPROPERTY() bool bHistoricalUnattributed{};
    UPROPERTY() bool bHasSettlementInputs{};
    UPROPERTY() bool bNeedsCasualtyEvent{};
    UPROPERTY() FName DisasterType{};
    UPROPERTY() float BaseSeverity{};
    UPROPERTY() TArray<int32> ActualDeathResidentIDs{};
    UPROPERTY() TArray<int32> ExposedSurvivorResidentIDs{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4CasualtyCitySummary
{
    GENERATED_BODY()
    UPROPERTY() FGuid SessionID{};
    UPROPERTY() int32 CampaignStage{};
    UPROPERTY() FName RegionID{};
    UPROPERTY() FName FocusID{};
    UPROPERTY() int32 Deaths{};
    UPROPERTY() int32 LivingPopulation{};
    UPROPERTY() int32 UniqueExposedSurvivors{};
    UPROPERTY() int32 RetrofitInvestments{};
    UPROPERTY() int32 FlowerTributes{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4CitizenMicroStory
{
    GENERATED_BODY()
    UPROPERTY() int32 ResidentID{};
    UPROPERTY() FText FullName{};
    UPROPERTY() int32 Age{};
    UPROPERTY() FText Occupation{};
    UPROPERTY() FText BuildingDisplayName{};
    UPROPERTY() FName BuildingDefinitionType{};
    UPROPERTY() bool bHasBuildingSnapshot{};
    UPROPERTY() FName Material{};
    UPROPERTY() FName Foundation{};
    UPROPERTY() bool bIsRetrofitted{};
    UPROPERTY() FName Outcome{};
    UPROPERTY() FName Cause{};
    UPROPERTY() FText Narrative{};
    UPROPERTY() FGuid DisasterEventID{};
    UPROPERTY() FName DisasterType{};
    UPROPERTY() int32 DisasterTurn{};
    UPROPERTY() int32 BuildingID{};
    UPROPERTY() FIntPoint BuildingCoord{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockCasualtyLedgerSaveState
{
    GENERATED_BODY()
    UPROPERTY() TArray<FLegacyV4CitizenMicroStory> Entries{};
    UPROPERTY() TArray<FLegacyV4CitizenMicroStory> SurvivalStories{};
    UPROPERTY() TArray<FLegacyV4CitizenMicroStory> BuildingWitnessStories{};
    UPROPERTY() TArray<FLegacyV4AftershockCasualtyEventRecord> Events{};
    UPROPERTY() TSet<int32> FlowerResidentIDs{};
    UPROPERTY() TSet<int32> RetrofittedBuildingIDs{};
    UPROPERTY() TSet<int32> ExposedSurvivorResidentIDs{};
    UPROPERTY() FGuid ArchiveSessionID{};
    UPROPERTY() TArray<FLegacyV4CasualtyCitySummary> ArchiveSummaries{};
    UPROPERTY() bool bHasCityKey{};
    UPROPERTY() FGuid CitySessionID{};
    UPROPERTY() int32 CityCampaignStage{};
    UPROPERTY() FName CityRegionID{};
    UPROPERTY() FName CityFocusID{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4CityBuildingStatus
{
    GENERATED_BODY()
    UPROPERTY() int32 BuildingID{};
    UPROPERTY() bool bRoadConnected{};
    UPROPERTY() bool bPowered{};
    UPROPERTY() bool bOperating{};
    UPROPERTY() int32 AssignedWorkers{};
    UPROPERTY() int32 JobCapacity{};
    UPROPERTY() float Staffing01{};
    UPROPERTY() float Operation01{};
    UPROPERTY() float PowerCoverage01{};
    UPROPERTY() float FoodOutputPerTurn{};
    UPROPERTY() FText FailureReason{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4CityLifeState
{
    GENERATED_BODY()
    UPROPERTY() int32 LivingPopulation{};
    UPROPERTY() int32 TotalDeaths{};
    UPROPERTY() int32 EmployedResidents{};
    UPROPERTY() int32 JobSlots{};
    UPROPERTY() int32 OperatingBuildings{};
    UPROPERTY() float RawFoodStock{};
    UPROPERTY() float FoodStock{};
    UPROPERTY() float WaterStock{};
    UPROPERTY() float FuelStock{};
    UPROPERTY() float LivestockCondition01{};
    UPROPERTY() float FoodProductionPerTurn{};
    UPROPERTY() float FoodDemandPerTurn{};
    UPROPERTY() float FoodCoverage01{};
    UPROPERTY() float WaterCoverage01{};
    UPROPERTY() float PowerSupply{};
    UPROPERTY() float PowerDemand{};
    UPROPERTY() float PowerCoverage01{};
    UPROPERTY() float Employment01{};
    UPROPERTY() float HealthCoverage01{};
    UPROPERTY() float SafetyCoverage01{};
    UPROPERTY() float JusticeCoverage01{};
    UPROPERTY() float TransitCoverage01{};
    UPROPERTY() float AverageHappiness01{};
    UPROPERTY() float ResidentialTaxRate{};
    UPROPERTY() float BusinessTaxRate{};
    UPROPERTY() float TaxCollectionEfficiency01{};
    UPROPERTY() int64 ProjectedTaxIncomeTL{};
    UPROPERTY() int64 ProjectedMaintenanceTL{};
    UPROPERTY() int64 LastSupplyExpenseTL{};
    UPROPERTY() int64 LastTransitIncomeTL{};
    UPROPERTY() int32 LastProcessedTurn{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4CityResidentRecord
{
    GENERATED_BODY()
    UPROPERTY() int32 ResidentID{};
    UPROPERTY() FText FullName{};
    UPROPERTY() int32 Age{};
    UPROPERTY() int32 HomeBuildingID{};
    UPROPERTY() int32 WorkBuildingID{};
    UPROPERTY() bool bAlive{};
    UPROPERTY() bool bWorkingAge{};
    UPROPERTY() bool bPrefersPrivateCar{};
    UPROPERTY() float Hunger01{};
    UPROPERTY() float Health01{};
    UPROPERTY() float Happiness01{};
    UPROPERTY() FName Emotion{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockCityLifeSaveState
{
    GENERATED_BODY()
    UPROPERTY() bool bInitialized{};
    UPROPERTY() TArray<FLegacyV4CityResidentRecord> Residents{};
    UPROPERTY() FLegacyV4CityLifeState State{};
    UPROPERTY() TMap<int32,FLegacyV4CityBuildingStatus> BuildingStates{};
    UPROPERTY() TMap<int32,int32> BuildingRoadComponents{};
    UPROPERTY() int32 NextResidentID{};
    UPROPERTY() TSet<int32> SeededHomes{};
    UPROPERTY() int32 SupplyDisruptionTurns{};
    UPROPERTY() TMap<int64,int32> LastServiceVisitTurn{};
    UPROPERTY() TSet<int32> PaidTransitTrips{};
    UPROPERTY() int64 PendingTransitRevenueTL{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4CitizenAgent
{
    GENERATED_BODY()
    UPROPERTY() int32 ResidentID{};
    UPROPERTY() int32 FoodBuildingID{};
    UPROPERTY() int32 HealthBuildingID{};
    UPROPERTY() float Hunger01{};
    UPROPERTY() float Health01{};
    UPROPERTY() float Happiness01{};
    UPROPERTY() bool bHomePowered{};
    UPROPERTY() bool bHomeOperating{};
    UPROPERTY() bool bWorkOperating{};
    UPROPERTY() bool bPrefersPrivateCar{};
    UPROPERTY() int32 TransportTripID{};
    UPROPERTY() bool bInsideVehicle{};
    UPROPERTY() bool bWaitingForOutdoorSpace{};
    UPROPERTY() int32 OutdoorDepartureBuildingID{};
    UPROPERTY() FVector OutdoorDepartureLocation{};
    UPROPERTY() FVector FoodLocation{};
    UPROPERTY() FVector HealthLocation{};
    UPROPERTY() FVector TransportFinalDestination{};
    UPROPERTY() FName TransportArrivalActivity{};
    UPROPERTY() float FoodVisitCooldown{};
    UPROPERTY() float HealthVisitCooldown{};
    UPROPERTY() FVector2D WalkVelocityCmPerSec{};
    UPROPERTY() float WalkingPhaseRadians{};
    UPROPERTY() FVector CurrentLocation{};
    UPROPERTY() FVector TargetLocation{};
    UPROPERTY() FVector FinalDestinationLocation{};
    UPROPERTY() TArray<FIntPoint> RouteGridCoords{};
    UPROPERTY() int32 NextRouteCoordIndex{};
    UPROPERTY() float MoveSpeed{};
    UPROPERTY() float FacingYaw{};
    UPROPERTY() bool bIsPanicking{};
    UPROPERTY() FName Activity{};
    UPROPERTY() FName ActivityAfterArrival{};
    UPROPERTY() FVector HomeLocation{};
    UPROPERTY() FVector WorkLocation{};
    UPROPERTY() FVector LeisureLocation{};
    UPROPERTY() FVector ShelterLocation{};
    UPROPERTY() float ScheduleSecondsRemaining{};
    UPROPERTY() int32 RandomSeed{};
    UPROPERTY() int32 RandomSequence{};
    UPROPERTY() FName CurrentActivity{};
    UPROPERTY() int32 HomeBuildingID{};
    UPROPERTY() int32 WorkBuildingID{};
    UPROPERTY() int32 LeisureBuildingID{};
    UPROPERTY() int32 EmergencyBuildingID{};
    UPROPERTY() bool bEmergencyPlaceReserved{};
    UPROPERTY() bool bEmergencyOverflow{};
    UPROPERTY() bool bEmergencyUnreachable{};
    UPROPERTY() bool bRouteReachesDestination{};
    UPROPERTY() int32 ISMIndex{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockCrowdSaveState
{
    GENERATED_BODY()
    UPROPERTY() TArray<FLegacyV4CitizenAgent> Agents{};
    UPROPERTY() int32 SimulationSeed{};
    UPROPERTY() bool bPanic{};
    UPROPERTY() FName EmergencyDisasterType{};
    UPROPERTY() float RemainingEmergencySeconds{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4DisasterReport
{
    GENERATED_BODY()
    UPROPERTY() FName DisasterType{};
    UPROPERTY() float BaseSeverity{};
    UPROPERTY() FGuid DisasterEventID{};
    UPROPERTY() TArray<int32> ActualDeathResidentIDs{};
    UPROPERTY() TArray<int32> ExposedSurvivorResidentIDs{};
    UPROPERTY() int64 TotalDeathCostTL{};
    UPROPERTY() bool bNeedsCasualtyEvent{};
    UPROPERTY() bool bWasRejected{};
    UPROPERTY() FText RejectionReason{};
    UPROPERTY() int32 TotalBuildingsAffected{};
    UPROPERTY() int32 TotalBuildingsCollapsed{};
    UPROPERTY() int32 TotalDeaths{};
    UPROPERTY() int32 TotalSurvivors{};
    UPROPERTY() int32 PreventedDeathsEstimate{};
    UPROPERTY() float TotalSatisfactionChange{};
    UPROPERTY() int64 TotalStateAidGranted{};
    UPROPERTY() float WaterSupplyStress01{};
    UPROPERTY() float CropLoss01{};
    UPROPERTY() int32 ForestCellsBurned{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockDirectorState
{
    GENERATED_BODY()
    UPROPERTY() FName Phase{};
    UPROPERTY() FName PendingType{};
    UPROPERTY() float RemainingSeconds{};
    UPROPERTY() float WarningDuration{};
    UPROPERTY() float Severity{};
    UPROPERTY() int32 LastProcessedTurn{};
    UPROPERTY() int32 LastTriggerTurn{};
    UPROPERTY() int32 RandomSeed{};
    UPROPERTY() int64 EventSequence{};
    UPROPERTY() int64 LastAppliedEventSequence{};
    UPROPERTY() FLegacyV4DisasterReport LastReport{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockDisasterVisualSnapshot
{
    GENERATED_BODY()
    UPROPERTY() int32 Revision{};
    UPROPERTY() FName Type{};
    UPROPERTY() float BaseSeverity{};
    UPROPERTY() float EffectiveIntensity{};
    UPROPERTY() float StartedAtWorldSeconds{};
    UPROPERTY() float DurationSeconds{};
    UPROPERTY() TMap<FIntPoint,float> LocalSeverity{};
    UPROPERTY() FVector DominantWindDirection{};
    UPROPERTY() bool bWindDirectionIsScenarioDefault{};
    UPROPERTY() FVector PropagationDirection{};
    UPROPERTY() float SurgePeakHeadCm{};
    UPROPERTY() TMap<FIntPoint,float> LavaSeverity{};
    UPROPERTY() TMap<FIntPoint,float> AshSeverity{};
    UPROPERTY() FIntPoint SourceCoord{};
    UPROPERTY() float DroughtStress01{};
    UPROPERTY() float CropLoss01{};
    UPROPERTY() TMap<FIntPoint,float> RiverFillBefore{};
    UPROPERTY() TMap<FIntPoint,float> RiverFillAfter{};
    UPROPERTY() int32 FloodModelVersion{};
    UPROPERTY() float RainfallPulseMm{};
    UPROPERTY() float FloodPulseDurationSeconds{};
    UPROPERTY() TMap<FIntPoint,float> WaterDepthCm{};
    UPROPERTY() TMap<FIntPoint,float> WaterSurfaceElevationCm{};
    UPROPERTY() TMap<FIntPoint,float> WaterArrivalFraction{};
    UPROPERTY() TMap<FIntPoint,float> LavaArrivalFraction{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockEconomySaveState
{
    GENERATED_BODY()
    UPROPERTY() int32 Turn{};
    UPROPERTY() int32 Year{};
    UPROPERTY() int64 Treasury{};
    UPROPERTY() float Satisfaction{};
    UPROPERTY() float CO2{};
    UPROPERTY() float ClimateRisk{};
    UPROPERTY() int32 DisasterLevel{};
    UPROPERTY() int64 PendingAid{};
    UPROPERTY() bool bGameOver{};
    UPROPERTY() int32 SurvivedDisasters{};
    UPROPERTY() TArray<FGuid> AppliedDisasterEventIDs{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockPassengerTrip
{
    GENERATED_BODY()
    UPROPERTY() int32 TripID{};
    UPROPERTY() int32 ResidentID{};
    UPROPERTY() int32 DestinationBuildingID{};
    UPROPERTY() int32 VehicleID{};
    UPROPERTY() int32 PickupStopID{};
    UPROPERTY() int32 DropoffStopID{};
    UPROPERTY() int32 PickupQueueSlot{};
    UPROPERTY() FName Mode{};
    UPROPERTY() FName State{};
    UPROPERTY() FIntPoint PickupRoadCell{};
    UPROPERTY() FIntPoint DropoffRoadCell{};
    UPROPERTY() FVector CurrentLocation{};
    UPROPERTY() FVector PickupLocation{};
    UPROPERTY() FVector DropoffLocation{};
    UPROPERTY() float TerminalObservedAtSeconds{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4QuizQuestion
{
    GENERATED_BODY()
    UPROPERTY() FText Question{};
    UPROPERTY() TArray<FText> Choices{};
    UPROPERTY() int32 CorrectIndex{};
    UPROPERTY() FText InfoNote{};
    UPROPERTY() FName Category{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockQuizSaveState
{
    GENERATED_BODY()
    UPROPERTY() FLegacyV4QuizQuestion CurrentQuestion{};
    UPROPERTY() TSet<FName> AskedRowNames{};
    UPROPERTY() int32 AnsweredCount{};
    UPROPERTY() int32 CorrectCount{};
    UPROPERTY() bool bQuestionPending{};
    UPROPERTY() int32 LastAskedTurn{};
    UPROPERTY() int32 RandomSeed{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockRoadVehicle
{
    GENERATED_BODY()
    UPROPERTY() int32 VehicleID{};
    UPROPERTY() int32 ResidentOwnerID{};
    UPROPERTY() int32 TerminalBuildingID{};
    UPROPERTY() int32 Capacity{};
    UPROPERTY() bool bBus{};
    UPROPERTY() bool bActive{};
    UPROPERTY() bool bStalled{};
    UPROPERTY() int32 ParkedAtBuildingID{};
    UPROPERTY() FVector Location{};
    UPROPERTY() float Yaw{};
    UPROPERTY() float WheelAngle{};
    UPROPERTY() float DwellSeconds{};
    UPROPERTY() FIntPoint SegmentOriginCell{};
    UPROPERTY() bool bSegmentLaneAligned{};
    UPROPERTY() bool bParking{};
    UPROPERTY() FVector ParkingGoal{};
    UPROPERTY() TArray<FIntPoint> RoadPath{};
    UPROPERTY() int32 NextPathStep{};
    UPROPERTY() TArray<int32> ServiceStopIDs{};
    UPROPERTY() int32 NextServiceStop{};
    UPROPERTY() TArray<int32> PassengerTripIDs{};
    UPROPERTY() TArray<int32> PendingAlightTripIDs{};
    UPROPERTY() float AlightReleaseSeconds{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockRubbleSaveRecord
{
    GENERATED_BODY()
    UPROPERTY() FSoftObjectPath FracturedAssetPath{};
    UPROPERTY() int32 BuildingID{};
    UPROPERTY() FIntPoint Coord{};
    UPROPERTY() FTransform ActorTransform{};
    UPROPERTY() TArray<FTransform> LocalRestTransforms{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockScenarioSaveState
{
    GENERATED_BODY()
    UPROPERTY() int32 ScenarioIndex{};
    UPROPERTY() bool bPrimaryDisasterSurvived{};
    UPROPERTY() TArray<FIntPoint> AssemblyCells{};
    UPROPERTY() int32 StarterRoadCellCount{};
    UPROPERTY() FString Name{};
    UPROPERTY() FString Briefing{};
    UPROPERTY() FName Hazard{};
    UPROPERTY() int32 Seed{};
    UPROPERTY() int32 StartYear{};
    UPROPERTY() int64 StartingBudgetTL{};
    UPROPERTY() float BaseSeverity{};
    UPROPERTY() float InitialAnomalyC{};
    UPROPERTY() int32 GeographyIndex{};
    UPROPERTY() double Latitude{};
    UPROPERTY() double Longitude{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockSessionSettings
{
    GENERATED_BODY()
    UPROPERTY() FName Mode{};
    UPROPERTY() FName RegionID{};
    UPROPERTY() FName FocusID{};
    UPROPERTY() int32 Seed{};
    UPROPERTY() float Difficulty{};
    UPROPERTY() bool bUnlimitedBudget{};
    UPROPERTY() int32 CampaignStage{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockTerrainChunkState
{
    GENERATED_BODY()
    UPROPERTY() FIntPoint ChunkCoord{};
    UPROPERTY() int32 CellCount{};
    UPROPERTY() bool bContainsAuthoredContinuation{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4GridCell
{
    GENERATED_BODY()
    UPROPERTY() FIntPoint Coords{};
    UPROPERTY() bool bIsPlayable{};
    UPROPERTY() float Elevation{};
    UPROPERTY() float RealElevationMetres{};
    UPROPERTY() bool bHasRealElevationSample{};
    UPROPERTY() bool bAuthoredContinuation{};
    UPROPERTY() FName SoilType{};
    UPROPERTY() bool bGroundSurveyed{};
    UPROPERTY() float ForestFuel01{};
    UPROPERTY() float SoilMoisture01{};
    UPROPERTY() float CropHealth01{};
    UPROPERTY() float DroughtStress01{};
    UPROPERTY() bool bAgriculturalLand{};
    UPROPERTY() bool bVolcanicVent{};
    UPROPERTY() float LavaCoverage01{};
    UPROPERTY() float AshLoad01{};
    UPROPERTY() float RiverFill01{};
    UPROPERTY() bool bIsRiverbed{};
    UPROPERTY() bool bIsOcean{};
    UPROPERTY() bool bIsCoastal{};
    UPROPERTY() bool bHasDam{};
    UPROPERTY() float DamStorageCapacityUnits{};
    UPROPERTY() float DamStoredWaterUnits{};
    UPROPERTY() float DamLastRetainedUnits{};
    UPROPERTY() float DamLastOvertoppingUnits{};
    UPROPERTY() float RiverCapacity{};
    UPROPERTY() float WaterFlowDamping{};
    UPROPERTY() float DrainageQuality{};
    UPROPERTY() float FaultLineProximity{};
    UPROPERTY() float ValleyWindMultiplier{};
    UPROPERTY() float Slope{};
    UPROPERTY() float RiverDistance{};
    UPROPERTY() float RiverProximity{};
    UPROPERTY() float InitialFloodDepth{};
    UPROPERTY() float FloodDepth{};
    UPROPERTY() float ParcelSuitability{};
    UPROPERTY() bool bBurning{};
    UPROPERTY() int32 BurningTurns{};
    UPROPERTY() bool bHasRoad{};
    UPROPERTY() int32 BridgeID{};
    UPROPERTY() uint8 BridgeAxis{};
    UPROPERTY() float RoadSurfaceElevationCm{};
    UPROPERTY() bool bBridgeEndpoint{};
    UPROPERTY() bool bRoadPredatedBridge{};
    UPROPERTY() bool bIsEmergencyAssemblyArea{};
    UPROPERTY() int32 OccupyingBuildingID{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockTerrainSaveState
{
    GENERATED_BODY()
    UPROPERTY() int32 GridWidth{};
    UPROPERTY() int32 GridHeight{};
    UPROPERTY() float CellSize{};
    UPROPERTY() FVector GridOrigin{};
    UPROPERTY() int32 TerrainSeed{};
    UPROPERTY() int32 GeographyIndex{};
    UPROPERTY() int32 GeographyFocusIndex{};
    UPROPERTY() float ActiveDroughtSeverity{};
    UPROPERTY() FIntPoint VolcanicVentCoord{};
    UPROPERTY() int32 CoastStartRow{};
    UPROPERTY() float CoastalSeaLevelCm{};
    UPROPERTY() TArray<FLegacyV4GridCell> Cells{};
    UPROPERTY() int32 TerrainStorageVersion{};
    UPROPERTY() int32 ChunkSize{};
    UPROPERTY() int32 ContinuationGeneratorVersion{};
    UPROPERTY() bool bUsesRegionalLayout{};
    UPROPERTY() bool bInitialMaskUnlocked{};
    UPROPERTY() uint64 TerrainRevision{};
    UPROPERTY() TArray<FLegacyV4AftershockTerrainChunkState> Chunks{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockTransitStop
{
    GENERATED_BODY()
    UPROPERTY() int32 BuildingID{};
    UPROPERTY() FIntPoint RoadCell{};
    UPROPERTY() int32 Capacity{};
    UPROPERTY() bool bTerminal{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockTransportStats
{
    GENERATED_BODY()
    UPROPERTY() int32 Cars{};
    UPROPERTY() int32 Buses{};
    UPROPERTY() int32 Stops{};
    UPROPERTY() int32 WaitingPassengers{};
    UPROPERTY() int32 RidingPassengers{};
    UPROPERTY() int32 CompletedTrips{};
    UPROPERTY() int32 StrandedTrips{};
    UPROPERTY() int32 InvalidRoadSteps{};
    UPROPERTY() int32 BusBoardings{};
    UPROPERTY() int32 BusArrivals{};
    UPROPERTY() int32 PrivateCarTrips{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockTransportSaveState
{
    GENERATED_BODY()
    UPROPERTY() TArray<FLegacyV4AftershockPassengerTrip> Trips{};
    UPROPERTY() TArray<FLegacyV4AftershockRoadVehicle> Vehicles{};
    UPROPERTY() TArray<FLegacyV4AftershockTransitStop> Stops{};
    UPROPERTY() bool bEmergencyActive{};
    UPROPERTY() bool bInfrastructureDirty{};
    UPROPERTY() float InfrastructureRefreshSeconds{};
    UPROPERTY() float HistoryRefreshSeconds{};
    UPROPERTY() int32 NextTripID{};
    UPROPERTY() int32 NextVehicleID{};
    UPROPERTY() float HistoryClockSeconds{};
    UPROPERTY() FLegacyV4AftershockTransportStats RetiredTripStats{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
USTRUCT()
struct FLegacyV4AftershockWorldClimateState
{
    GENERATED_BODY()
    UPROPERTY() int32 ScenarioStartYear{};
    UPROPERTY() int32 CurrentYear{};
    UPROPERTY() float InitialAnomalyC{};
    UPROPERTY() float GlobalTemperatureAnomalyC{};
    UPROPERTY() float AnnualTrendC{};
    UPROPERTY() float LocalUrbanHeatPressure{};
    UPROPERTY() float DroughtVisualPressure{};
    UPROPERTY() float IceCoverageFraction{};
    UPROPERTY() float StormVisualPressure{};
};

/** Frozen original V4 wire DTO; never use as gameplay authority. */
UCLASS()
class ULegacyV4AftershockSaveGame final:public USaveGame
{
    GENERATED_BODY()
public:
    UPROPERTY() int32 SchemaVersion{};
    UPROPERTY() FDateTime SavedAt{};
    UPROPERTY() FGuid SessionID{};
    UPROPERTY() FString MapPackage{};
    UPROPERTY() FLegacyV4AftershockSessionSettings Settings{};
    UPROPERTY() FLegacyV4AftershockCampaignProgress Campaign{};
    UPROPERTY() FLegacyV4AftershockTerrainSaveState Terrain{};
    UPROPERTY() FLegacyV4AftershockBuildingsSaveState Buildings{};
    UPROPERTY() TArray<FLegacyV4AftershockRubbleSaveRecord> Rubble{};
    UPROPERTY() FLegacyV4AftershockDisasterVisualSnapshot DisasterVisual{};
    UPROPERTY() float DisasterVisualRemainingSeconds{};
    UPROPERTY() FLegacyV4AftershockEconomySaveState Economy{};
    UPROPERTY() FLegacyV4AftershockCasualtyLedgerSaveState CasualtyLedger{};
    UPROPERTY() FLegacyV4AftershockCityLifeSaveState CityLife{};
    UPROPERTY() FLegacyV4AftershockTransportSaveState Transport{};
    UPROPERTY() FLegacyV4AftershockWorldClimateState Climate{};
    UPROPERTY() FLegacyV4AftershockScenarioSaveState Scenario{};
    UPROPERTY() FLegacyV4AftershockDirectorState Director{};
    UPROPERTY() FLegacyV4AftershockCrowdSaveState Crowd{};
    UPROPERTY() FLegacyV4AftershockQuizSaveState Quiz{};
    UPROPERTY() FLegacyV4AftershockCameraSaveState Camera{};
};
