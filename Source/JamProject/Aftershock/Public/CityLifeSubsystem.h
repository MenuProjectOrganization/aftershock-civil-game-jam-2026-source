#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Aftershock/Public/BuildingData.h"
#include "CityLifeSubsystem.generated.h"

UENUM(BlueprintType)
enum class ECityResidentEmotion : uint8 { Neutral, Happy, Sad, Hungry, Sick, Emergency };

struct FAftershockCityLifeSaveState;
class UCityLifeSubsystem;

/** Synchronous derived occupancy acceleration. Nested scopes share one index;
 * the outermost destructor releases it. Never a saved resident authority. */
class JAMPROJECT_API FAftershockScopedOccupancyBatch final
{
public:
	explicit FAftershockScopedOccupancyBatch(UCityLifeSubsystem* InOwner);
	~FAftershockScopedOccupancyBatch();
	FAftershockScopedOccupancyBatch(const FAftershockScopedOccupancyBatch&)=delete;
	FAftershockScopedOccupancyBatch& operator=(const FAftershockScopedOccupancyBatch&)=delete;
private:
	UCityLifeSubsystem* Owner=nullptr;
};

/** One real resident, never one extra person per job slot. IDs survive jobs/repairs/disasters. */
USTRUCT(BlueprintType)
struct FCityResidentRecord
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) int32 ResidentID = INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) FText FullName;
	UPROPERTY(BlueprintReadOnly) int32 Age=0;
	UPROPERTY(BlueprintReadOnly) int32 HomeBuildingID = INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) int32 WorkBuildingID = INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) bool bAlive = true;
	UPROPERTY(BlueprintReadOnly) bool bWorkingAge = true;
	UPROPERTY(BlueprintReadOnly) bool bPrefersPrivateCar = false;
	UPROPERTY(BlueprintReadOnly) float Hunger01 = 0.f;
	UPROPERTY(BlueprintReadOnly) float Health01 = 1.f;
	UPROPERTY(BlueprintReadOnly) float Happiness01 = .75f;
	UPROPERTY(BlueprintReadOnly) ECityResidentEmotion Emotion = ECityResidentEmotion::Neutral;
};

USTRUCT(BlueprintType)
struct FCityBuildingStatus
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) int32 BuildingID = INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) bool bRoadConnected = false;
	UPROPERTY(BlueprintReadOnly) bool bPowered = false;
	UPROPERTY(BlueprintReadOnly) bool bOperating = false;
	UPROPERTY(BlueprintReadOnly) int32 AssignedWorkers = 0;
	UPROPERTY(BlueprintReadOnly) int32 JobCapacity = 0;
	UPROPERTY(BlueprintReadOnly) float Staffing01 = 0.f;
	UPROPERTY(BlueprintReadOnly) float Operation01 = 0.f;
	UPROPERTY(BlueprintReadOnly) float PowerCoverage01 = 0.f;
	UPROPERTY(BlueprintReadOnly) float FoodOutputPerTurn = 0.f;
	UPROPERTY(BlueprintReadOnly) FText FailureReason;
};

USTRUCT(BlueprintType)
struct FCityLifeState
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) int32 LivingPopulation = 0;
	UPROPERTY(BlueprintReadOnly) int32 TotalDeaths = 0;
	UPROPERTY(BlueprintReadOnly) int32 EmployedResidents = 0;
	UPROPERTY(BlueprintReadOnly) int32 JobSlots = 0;
	UPROPERTY(BlueprintReadOnly) int32 OperatingBuildings = 0;
	UPROPERTY(BlueprintReadOnly) float RawFoodStock = 0.f;
	UPROPERTY(BlueprintReadOnly) float FoodStock = 0.f;
	UPROPERTY(BlueprintReadOnly) float WaterStock = 0.f;
	UPROPERTY(BlueprintReadOnly) float FuelStock = 0.f;
	UPROPERTY(BlueprintReadOnly) float LivestockCondition01 = 1.f;
	UPROPERTY(BlueprintReadOnly) float FoodProductionPerTurn = 0.f;
	UPROPERTY(BlueprintReadOnly) float FoodDemandPerTurn = 0.f;
	UPROPERTY(BlueprintReadOnly) float FoodCoverage01 = 1.f;
	UPROPERTY(BlueprintReadOnly) float WaterCoverage01 = 1.f;
	UPROPERTY(BlueprintReadOnly) float PowerSupply = 0.f;
	UPROPERTY(BlueprintReadOnly) float PowerDemand = 0.f;
	UPROPERTY(BlueprintReadOnly) float PowerCoverage01 = 0.f;
	UPROPERTY(BlueprintReadOnly) float Employment01 = 0.f;
	UPROPERTY(BlueprintReadOnly) float HealthCoverage01 = 0.f;
	UPROPERTY(BlueprintReadOnly) float SafetyCoverage01 = 0.f;
	UPROPERTY(BlueprintReadOnly) float JusticeCoverage01 = 0.f;
	UPROPERTY(BlueprintReadOnly) float TransitCoverage01 = 0.f;
	UPROPERTY(BlueprintReadOnly) float AverageHappiness01 = .75f;
	UPROPERTY(BlueprintReadOnly) float ResidentialTaxRate = .10f;
	UPROPERTY(BlueprintReadOnly) float BusinessTaxRate = .12f;
	UPROPERTY(BlueprintReadOnly) float TaxCollectionEfficiency01 = .70f;
	UPROPERTY(BlueprintReadOnly) int64 ProjectedTaxIncomeTL = 0;
	UPROPERTY(BlueprintReadOnly) int64 ProjectedMaintenanceTL = 0;
	UPROPERTY(BlueprintReadOnly) int64 LastSupplyExpenseTL = 0;
	UPROPERTY(BlueprintReadOnly) int64 LastTransitIncomeTL = 0;
	UPROPERTY(BlueprintReadOnly) int32 LastProcessedTurn = 1;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnCityLifeUpdated);

/** Caller-owned, single-refresh read index. Never stored, serialized or an authority. */
struct FAftershockCityServiceLookup
{
	struct FCandidate { int32 BuildingID=INDEX_NONE;FIntPoint Coord=FIntPoint::ZeroValue; };
	TMap<int32,FIntPoint> BuildingCoords;
	TMap<int32,int32> RoadComponents;
	TMap<FIntPoint,TArray<FCandidate>> OperatingCandidates;
	TMap<FIntVector,int32> DestinationsByHomeTypeComponent;
	uint64 BuildingsVisited=0,Queries=0,CacheHits=0,CandidatesVisited=0;
};

/** Canonical resident ledger + connected services. Economy owns money and satisfaction;
 * terrain owns hazards, buildings own structures. No timer, no actor or separate hazards. */
UCLASS()
class JAMPROJECT_API UCityLifeSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	void CaptureSaveState(FAftershockCityLifeSaveState& Out) const;
	bool RestoreSaveState(const FAftershockCityLifeSaveState& State,FText& OutError);
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	void InitializeForCity();
	bool IsCityInitialized() const { return bInitializedForCity; }
	void RefreshCity();
	void AdvanceCityTurn(int32 Turn);
	void ApplyDisasterShock(EDisasterType Type, float Severity);
	int32 GetLivingOccupants(int32 BuildingID, bool bDaytime) const;
	/** Read-only native audit visibility; no cache survives an outer batch exit. */
	bool HasScopedOccupancyLookup() const {return OccupancyBatchDepth>0&&bScopedOccupancyLookupValid;}
	int32 GetScopedOccupancyLookupResidentCount() const {return FMath::Max(0,ScopedOccupancyResidentCount);}
	int32 CommitFatalities(int32 BuildingID, int32 RequestedDeaths, bool bDaytime);
	/** Returns only canonical IDs that actually changed alive -> deceased. */
	TArray<int32> ApplyCasualties(int32 BuildingID,int32 RequestedDeaths,bool bDaytime);
	const TArray<FCityResidentRecord>& GetResidents() const { return Residents; }
	/** Existing employed residents only, sorted by stable ID. Never creates staff/population. */
	void GetBuildingStaffResidentIDs(int32 BuildingID,TArray<int32>& OutResidentIDs,int32 Maximum=8) const;
	bool GetResident(int32 ResidentID, FCityResidentRecord& OutResident) const;
	const FCityLifeState& GetState() const { return State; }
	int64 GetPendingTransitRevenueTL() const { return PendingTransitRevenueTL; }
	FCityBuildingStatus GetBuildingStatus(int32 BuildingID) const;
	float GetBuildingOperation01(int32 BuildingID) const;
	int32 FindServiceDestination(int32 ResidentID, EBuildingType Type) const;
	/** Reuses exactly the normal nearest-service rules within one synchronous refresh. */
	void BuildServiceDestinationLookup(FAftershockCityServiceLookup& OutLookup) const;
	int32 FindServiceDestination(int32 ResidentID,EBuildingType Type,FAftershockCityServiceLookup& Lookup) const;
	bool ReportServiceVisit(int32 ResidentID, int32 BuildingID, FVector ArrivalLocation);
	bool RecordTransitFare(int32 ResidentID, int32 TripID);
	bool SetTaxRates(float ResidentialRate, float BusinessRate);
	FText DescribeCity() const;
	FText DescribeBuilding(int32 BuildingID) const;
	UPROPERTY(BlueprintAssignable) FOnCityLifeUpdated OnCityLifeUpdated;
private:
	friend class FAftershockScopedOccupancyBatch;
	struct FScopedBuildingOccupants {TArray<int32> Indices;int32 Living=0;int32 NextCandidate=0;};
	void BeginOccupancyBatch();
	void EndOccupancyBatch();
	void InvalidateScopedOccupancyLookup();
	void EnsureScopedOccupancyLookup() const;
	int32 OccupancyBatchDepth=0;
	mutable bool bScopedOccupancyLookupValid=false;
	mutable int32 ScopedOccupancyResidentCount=INDEX_NONE;
	mutable TMap<int32,FScopedBuildingOccupants> ScopedDayOccupants,ScopedNightOccupants;
	mutable TMap<int32,int32> ResidentIndexLookup;
	mutable int32 IndexedResidentCount=INDEX_NONE;
	void SyncResidentsAndJobs();
	void RecomputeServices();
	void UpdateResidentNeeds(float FoodCoverage, float WaterCoverage);
	TMap<int32, int32> BuildRoadComponents() const;
	bool bInitializedForCity = false;
	bool bRefreshing = false;
	bool bAdvancing = false;
	int32 NextResidentID = 1;
	TSet<int32> SeededHomes;
	UPROPERTY() TArray<FCityResidentRecord> Residents;
	UPROPERTY() TMap<int32, FCityBuildingStatus> BuildingStates;
	UPROPERTY() FCityLifeState State;
	TMap<int32, int32> BuildingRoadComponents;
	int32 SupplyDisruptionTurns = 0;
	TMap<int64, int32> LastServiceVisitTurn;
	TSet<int32> PaidTransitTrips;
	int64 PendingTransitRevenueTL = 0;
};
