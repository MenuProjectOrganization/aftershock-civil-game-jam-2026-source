// AFTERSHOCK — Civil Game Jam 2026
// ISM tabanlı vatandaş görselleştirme ve timer ile çalışan kalabalık alt sistemi

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "GameFramework/Actor.h"
#include "Aftershock/Public/BuildingData.h"
#include "CrowdRenderSubsystem.generated.h"

class UInstancedStaticMeshComponent;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UTerrainGridSubsystem;
class UNiagaraSystem;
class UAftershockCrowdProfileCommandlet;
class UAftershockCitizenPlanningCommandlet;
class UAftershockTransportAuditCommandlet;
class UAftershockCrowdSpacingAuditCommandlet;
struct FAftershockCrowdSaveState;
class FAftershockIncrementalNormalRoutes;
class UCrowdRenderSubsystem;

/** Coalesces derived roster/evacuation work within one synchronous owner call.
 * Outermost exit publishes the final canonical roster and one emergency plan. */
class JAMPROJECT_API FAftershockScopedCrowdRefresh final
{
public:
	explicit FAftershockScopedCrowdRefresh(UCrowdRenderSubsystem* InOwner);
	~FAftershockScopedCrowdRefresh();
	FAftershockScopedCrowdRefresh(const FAftershockScopedCrowdRefresh&)=delete;
	FAftershockScopedCrowdRefresh& operator=(const FAftershockScopedCrowdRefresh&)=delete;
private:
	UCrowdRenderSubsystem* Owner=nullptr;
};

/** Görsel vatandaş yaşam döngüsü; afet ve can kaybı hesabı bu enum'a bağlı değildir. */
UENUM(BlueprintType)
enum class EAftershockCitizenActivity : uint8
{
	AtHome,
	Commuting,
	AtWork,
	AtLeisure,
	Evacuating,
	AtShelter,
	Rescued,
	/** Reached a public assembly/centre forecourt without a reserved visual place. */
	AwaitingShelter,
	WaitingForRoute,
	AtFoodService,
	AtHealthService,
	WalkingToTransport,
	WaitingForTransport,
	RidingTransport,
	Displaced
};

/** Cosmetic-only Niagara cues. These never feed hazard or casualty calculations. */
UENUM(BlueprintType)
enum class EAftershockCosmeticFx : uint8
{
	FloodSpray,
	EarthquakeDust,
	WindDebris
};

/** Vatandaşın mevcut gündelik aktivite durumu. */
UENUM(BlueprintType)
enum class ECitizenActivity : uint8
{
	Idle            UMETA(DisplayName = "Boşta"),
	CommutingToWork UMETA(DisplayName = "İşe Gidiyor"),
	Working         UMETA(DisplayName = "Çalışıyor"),
	CommutingHome   UMETA(DisplayName = "Eve Dönüyor"),
	Evacuating      UMETA(DisplayName = "Tahliye Oluyor"),
	Resting         UMETA(DisplayName = "Dinleniyor")
};

/**
 * FCitizenAgent — Her bir nokta insanın minimal durumunu tutan hafif struct.
 */
USTRUCT(BlueprintType)
struct JAMPROJECT_API FCitizenAgent
{
	GENERATED_BODY()

	/** Stable identity from CityLife; INDEX_NONE only in explicit legacy fixtures. */
	UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Crowd|Life") int32 ResidentID=INDEX_NONE;
	UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Crowd|Life") int32 FoodBuildingID=INDEX_NONE;
	UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Crowd|Life") int32 HealthBuildingID=INDEX_NONE;
	UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Crowd|Life") float Hunger01=0.f;
	UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Crowd|Life") float Health01=1.f;
	UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Crowd|Life") float Happiness01=.75f;
	UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Crowd|Life") bool bHomePowered=true;
	UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Crowd|Life") bool bHomeOperating=true;
	UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Crowd|Life") bool bWorkOperating=true;
	UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Crowd|Life") bool bPrefersPrivateCar=false;
	UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Crowd|Transport") int32 TransportTripID=INDEX_NONE;
	UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Crowd|Transport") bool bInsideVehicle=false;
	/** Still inside the actual departure building until an outdoor body slot is free. */
	UPROPERTY() bool bWaitingForOutdoorSpace=false;
	/** Actual indoor departure, independent of subsequently rebound service/shelter targets. */
	UPROPERTY() int32 OutdoorDepartureBuildingID=INDEX_NONE;
	UPROPERTY() FVector OutdoorDepartureLocation=FVector::ZeroVector;
	UPROPERTY() FVector FoodLocation=FVector::ZeroVector;
	UPROPERTY() FVector HealthLocation=FVector::ZeroVector;
	UPROPERTY() FVector TransportFinalDestination=FVector::ZeroVector;
	UPROPERTY() EAftershockCitizenActivity TransportArrivalActivity=EAftershockCitizenActivity::AtHome;
	UPROPERTY() float FoodVisitCooldown=0.f;
	UPROPERTY() float HealthVisitCooldown=0.f;
	/** Measured visual displacement; never writes the canonical needs/health ledger. */
	UPROPERTY() FVector2D WalkVelocityCmPerSec=FVector2D::ZeroVector;
	/** Distance-integrated shared-shader gait phase; persisted without a wall-clock epoch. */
	UPROPERTY() float WalkingPhaseRadians=0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowd")
	FVector CurrentLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowd")
	FVector TargetLocation = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Schedule")
	FVector FinalDestinationLocation = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Schedule")
	TArray<FIntPoint> RouteGridCoords;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Schedule")
	int32 NextRouteCoordIndex = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowd")
	float MoveSpeed = 200.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd")
	float FacingYaw = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowd")
	bool bIsPanicking = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Schedule")
	EAftershockCitizenActivity Activity = EAftershockCitizenActivity::AtHome;

	/** Commuting/Evacuating tamamlandığında girilecek görsel state. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Schedule")
	EAftershockCitizenActivity ActivityAfterArrival = EAftershockCitizenActivity::AtHome;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Schedule")
	FVector HomeLocation = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Schedule")
	FVector WorkLocation = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Schedule")
	FVector LeisureLocation = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Schedule")
	FVector ShelterLocation = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Schedule")
	float ScheduleSecondsRemaining = 0.0f;

	/** Stable per-agent stream state: identical seed and population order reproduce schedules. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Schedule")
	int32 RandomSeed = 1337;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Schedule")
	int32 RandomSequence = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowd")
	ECitizenActivity CurrentActivity = ECitizenActivity::Idle;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowd")
	int32 HomeBuildingID = -1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowd")
	int32 WorkBuildingID = -1;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Schedule")
	int32 LeisureBuildingID = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Evacuation")
	int32 EmergencyBuildingID = INDEX_NONE;

	/** Presentation quota only, never shelter beds, actual population or saved lives. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Evacuation")
	bool bEmergencyPlaceReserved = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Evacuation")
	bool bEmergencyOverflow = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Evacuation")
	bool bEmergencyUnreachable = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd|Schedule")
	bool bRouteReachesDestination = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowd")
	int32 ISMIndex = -1;
};

/** Read-only counters for representative agents; deliberately not a rescue report. */
USTRUCT(BlueprintType)
struct JAMPROJECT_API FAftershockCitizenPlanningStats
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) int32 Representatives = 0;
	UPROPERTY(BlueprintReadOnly) int32 EligibleCentres = 0;
	UPROPERTY(BlueprintReadOnly) int32 VisualPlaceQuota = 0;
	UPROPERTY(BlueprintReadOnly) int32 Reserved = 0;
	UPROPERTY(BlueprintReadOnly) int32 ArrivedAtCentre = 0;
	UPROPERTY(BlueprintReadOnly) int32 MovingToSafety = 0;
	UPROPERTY(BlueprintReadOnly) int32 Overflow = 0;
	UPROPERTY(BlueprintReadOnly) int32 CentreUnreachable = 0;
	UPROPERTY(BlueprintReadOnly) int32 WaitingAtAssembly = 0;
	UPROPERTY(BlueprintReadOnly) int32 UnsafeRemainingRouteSteps = 0;
};

/** Bounded neighbourhood diagnostics; not a crowd safety or casualty model. */
USTRUCT(BlueprintType)
struct JAMPROJECT_API FAftershockCrowdSpacingStats
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) int32 OutdoorAgents=0;
	UPROPERTY(BlueprintReadOnly) int32 IndoorAgents=0;
	UPROPERTY(BlueprintReadOnly) int32 VehiclePassengers=0;
	UPROPERTY(BlueprintReadOnly) int32 NeighbourChecks=0;
	UPROPERTY(BlueprintReadOnly) int32 AcceptedCorrections=0;
	UPROPERTY(BlueprintReadOnly) int32 RejectedUnsafeCorrections=0;
	UPROPERTY(BlueprintReadOnly) int32 ObservedClosePairs=0;
	UPROPERTY(BlueprintReadOnly) int32 MaxBucketOccupancy=0;
	UPROPERTY(BlueprintReadOnly) int32 AdmissionChecks=0;
	UPROPERTY(BlueprintReadOnly) int32 BlockedMoves=0;
	UPROPERTY(BlueprintReadOnly) int32 DeferredEmergences=0;
};

/** Derived navigation caches, not additional building/population authorities. */
struct FAftershockCitizenDestination
{
	int32 BuildingID = INDEX_NONE;
	FIntPoint BuildingCoord = FIntPoint(MAX_int32,MAX_int32);
	EBuildingType Type = EBuildingType::House;
	int32 Weight = 0;
	TArray<FIntPoint> Frontages;
};

/** Fixed-size counters; ephemeral lookup records never survive a roster refresh. */
struct FAftershockCrowdRosterWork
{
	uint64 Synchronizations=0,ServiceIndexBuilds=0,ServiceBuildingsVisited=0;
	uint64 EmergencyPlans=0;
	uint64 ServiceQueries=0,ServiceCacheHits=0,ServiceCandidatesVisited=0,FrontageLookups=0,FrontagesIndexed=0;
};

struct FAftershockCitizenRouteTree
{
	TArray<int32> Next;
	TArray<int32> Cost;
};

/**
 * AAftershockCrowdHostActor — Nokta insan ISM bileşenini barındıran tekil dünya aktörü (Zero-Tick).
 */
UCLASS()
class JAMPROJECT_API AAftershockCrowdHostActor : public AActor
{
	GENERATED_BODY()

public:
	AAftershockCrowdHostActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd")
	TObjectPtr<USceneComponent> RootComp;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowd")
	TObjectPtr<UInstancedStaticMeshComponent> CitizenISM;

	/** Sparse shared-atlas billboards, no per-person widget or material instance. */
	UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Crowd") TObjectPtr<UInstancedStaticMeshComponent> EmotionISM;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Terrain")
	TObjectPtr<UInstancedStaticMeshComponent> TerrainISM;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Terrain")
	TObjectPtr<UInstancedStaticMeshComponent> RiverISM;

	/** Continuous presentation surfaces; canonical grid values remain unchanged. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Terrain")
	TObjectPtr<UStaticMeshComponent> TerrainSurface;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Terrain")
	TObjectPtr<UStaticMeshComponent> RiverSurface;

	int32 RenderedLandCellCount = 0;
	int32 RenderedRiverCellCount = 0;
	/** Canonical terrain revision used by the existing presentation surface. */
	uint64 BuiltTerrainRevision = MAX_uint64;
	/** Physical mode changes independently of the canonical cell revision. */
	bool bBuiltHydraulicModel2=false;
	FIntRect BuiltVisibleCellBounds=FIntRect(0,0,0,0);

	/** Hard asset references keep these presentation systems discoverable by the cooker. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Visual FX")
	TObjectPtr<UNiagaraSystem> FloodSpraySystem;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Visual FX")
	TObjectPtr<UNiagaraSystem> EarthquakeDustSystem;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Visual FX")
	TObjectPtr<UNiagaraSystem> WindDebrisSystem;

	/** Build a presentation-only surface from the canonical grid; no terrain values are written. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Terrain")
	void BuildTerrainVisual(const UTerrainGridSubsystem* Terrain);

	bool SpawnCosmeticFx(EAftershockCosmeticFx Effect, const FVector& WorldLocation, float Scale);
};

/**
 * UCrowdRenderSubsystem — Nokta vatandaşları tek ISM ve 0.08 sn timer ile yöneten görsel katman.
 * Agent sayısı 1500 ile sınırlandırılır; hareket/schedule CPU maliyeti ölçülmelidir.
 */
UCLASS(BlueprintType)
class JAMPROJECT_API UCrowdRenderSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	// -------- NÜFUS VE KALABALIK API --------
	/**
	 * Live CityLife: one agent per living ResidentID, max1500. The old half-capacity
	 * input is retained only while CityLife is uninitialized in legacy fixtures.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Crowd")
	void UpdatePopulationCount(int32 TotalResidents, int32 TotalWorkers);

#if !UE_BUILD_SHIPPING
	/** Explicit command-line-only render fixture. Never creates CityLife residents. */
	bool BeginSyntheticRenderProfile(int32 RequestedAgents);
#endif

	/**
	 * Afet durumunda tüm insanları panik moduna geçirir veya sakinleştirir.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Crowd")
	void SetPanicMode(bool bPanic);

	/** Warning and actual disaster use the same real-centre evacuation planner. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Crowd")
	void BeginEmergencyEvacuation(EDisasterType Type);

	/** Legacy actor compatibility entry point; only canonical building records are read. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Crowd")
	void SynchronizeCanonicalPopulation();

	UFUNCTION(BlueprintPure, Category="Aftershock|Crowd|Debug")
	FAftershockCitizenPlanningStats GetCitizenPlanningStats() const;

	/** Copies the representative roster for deterministic diagnostics; never modifies simulation. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Crowd|Debug")
	TArray<FCitizenAgent> GetCitizenDebugRoster() const { return Agents; }

	UFUNCTION(BlueprintCallable, Category="Aftershock|Crowd|Debug")
	void AuditCitizenPlanning() const;

	/** Spawn one of the project Niagara cues at a world location; visual presentation only. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Visual FX")
	bool SpawnCosmeticFx(EAftershockCosmeticFx Effect, FVector WorldLocation, float Scale = 1.0f);

	UFUNCTION(BlueprintPure, Category="Aftershock|Crowd")
	int32 GetCurrentCrowdCount() const { return Agents.Num(); }

	UFUNCTION(BlueprintPure, Category="Aftershock|Crowd")
	bool IsPanicModeActive() const { return bIsPanicModeActive; }
	bool IsSyntheticRenderProfileActive() const { return bSyntheticRenderProfileActive; }
	FAftershockCrowdSpacingStats GetCrowdSpacingStats() const { return SpacingStats; }
	/** Derived path fields are bounded; these counters never alter residents or movement. */
	static constexpr int32 MaximumNormalRouteTrees = 256;
	static constexpr uint64 MaximumNormalRouteCacheBytes = 32ull * 1024ull * 1024ull;
	int32 GetNormalRouteCacheEntryCount() const;
	uint64 GetNormalRouteCacheAllocatedBytes() const;
	uint64 GetNormalRouteTreeBuildCount() const { return NormalRouteTreeBuildCount; }
	FAftershockCrowdRosterWork GetRosterRefreshWork() const { return RosterRefreshWork; }
	/** Save-owner barrier: drain current deferred presentation before any owner
	 * DTO is read, preserving nested batch depth for later synchronous notices. */
	bool FlushScopedRefreshForSnapshot();
	void CaptureSaveState(FAftershockCrowdSaveState& OutState) const;
	bool RestoreSaveState(const FAftershockCrowdSaveState& State, FText& OutError);

	/** Development evidence: real mesh/instance bounds, safe cell coverage and projection candidates. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Crowd|Debug")
	void AuditCrowdVisibility() const;
	/** Exhaustive diagnostic only; not called by the crowd update timer. */
	void GetPhysicalClearance(int32& Outdoor,int32& CitizenOverlaps,int32& VehicleIntersections,float& MinimumDistanceCm) const;

	/** 50x50 arazi ve nehir ISM görselinin oluşturulduğunu garanti eder */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Terrain")
	void EnsureTerrainVisualBuilt();
	/** Changes only the installed mesh's render LOD; no geometry build or simulation mutation. */
	void RefreshTerrainPresentationBudget();
	/** Read-only installed ground coverage; never builds terrain or changes simulation. */
	bool TryGetInstalledTerrainVisualBounds(FIntRect& OutBounds) const;
	/** Shared physical admission queries. They never create or remove CityLife residents. */
	bool CanAdmitCitizenAt(const FVector& Location,int32 IgnoredResidentID=INDEX_NONE) const;
	bool IsVehicleSpaceClear(const FVector& From,float FromYaw,const FVector& To,float ToYaw,
		const FVector& Dimensions,int32 IgnoredResidentID=INDEX_NONE,float CrossingLookaheadCm=0.f) const;
	/** Covered-load repair on a candidate copy; no live agent/ISM mutation on failure. */
	bool ReconcileCitizenPlacements(TArray<FCitizenAgent>& InOutAgents,FText& Error,int32& OutMoved,float& OutMaxDistanceCm) const;
	static bool NormalizeDeferredDepartureAnchor(FCitizenAgent& Agent,TFunctionRef<bool(int32,const FVector&)> IsKnownIndoorAnchor,FText& Error);

protected:
	/** Profile harness access only; normal runtime scheduling remains timer-driven. */
	friend class UAftershockCrowdProfileCommandlet;
	friend class UAftershockCitizenPlanningCommandlet;
	friend class UAftershockTransportAuditCommandlet;
	friend class UTransportSubsystem;
	friend class UAftershockMunicipalAuditCommandlet;
	friend class UAftershockCrowdSpacingAuditCommandlet;
	friend class UAftershockSessionSaveAuditCommandlet;
	friend class UAftershockPresentationPerformanceAuditCommandlet;
	void UpdateCrowdPositions();
	void ResolveCrowdSpacing(const UTerrainGridSubsystem& Terrain);
	void RebuildCitizenInstanceTransforms();

	FVector PickValidGroundLocation(FCitizenAgent& Agent) const;
	FVector MakeVisibleCellLocation(const UTerrainGridSubsystem& Terrain, FIntPoint Coord, const FCitizenAgent& Agent) const;
	void StartCommute(FCitizenAgent& Agent, const FVector& Destination, EAftershockCitizenActivity ArrivalActivity,bool bAllowTransport=true);
	void BuildSafeRoute(FCitizenAgent& Agent, const FVector& Destination);
	void HandleAgentArrival(FCitizenAgent& Agent);
	void AdvanceAgentSchedule(FCitizenAgent& Agent);
	void SetAgentActivity(FCitizenAgent& Agent, EAftershockCitizenActivity NewActivity);
	void SetCitizenInstanceColor(const FCitizenAgent& Agent, bool bMarkRenderStateDirty = false);
	bool CanMoveCitizenSegment(const UTerrainGridSubsystem& Terrain,const FVector& From,const FVector& To) const;
	void RebuildCitizenSpatialIndex() const;
	void UpdateCitizenSpatialEntry(int32 AgentIndex) const;
	bool IsCitizenSpaceClear(const FVector& From,const FVector& To,int32 IgnoredAgentIndex,bool bAllowExistingOverlap=false) const;
	bool TryAdmitOutdoorDeparture(FCitizenAgent& Agent);
	void RebuildDestinationCaches();
	void AssignScheduleDestinations(FCitizenAgent& Agent, int32 AgentIndex);
	void ReplanEmergencyEvacuation();
    /** Fresh-list mode is ONLY for the synchronous destination rebuild;
     * independent hazard polls must recheck live public walkability. No latent cache. */
    bool RefreshRoutingSafety(bool bWalkingListJustRebuilt=false);
	void ResetNormalRouteCache();
    bool UsesIncrementalNormalRouting() const;
    bool HasPendingNormalRoute(const FCitizenAgent& Agent) const;
	void TrimNormalRouteCache(uint64 IncomingBytes);
	bool ApplyRouteTree(FCitizenAgent& Agent, const FAftershockCitizenRouteTree& Tree);
	void HandleDisasterVisualSnapshot(const struct FAftershockDisasterVisualSnapshot& Snapshot);
	void HandleTerrainStorageChanged(uint64 Revision);
	UFUNCTION() void HandleCityLifeUpdated();
	void SynchronizeCityLifeRoster();
	bool UpdatePassengerState(FCitizenAgent& Agent);
	void CancelAgentTransport(FCitizenAgent& Agent);
	void UpdateEmotionBillboards();
	int32 DestinationBuildingFor(const FCitizenAgent& Agent,EAftershockCitizenActivity Arrival) const;

	UFUNCTION()
	void HandleDisasterCompleted(const struct FDisasterReport& Report);
	void ApplyDisasterCompletedVisuals(const struct FDisasterReport& Report);

	UFUNCTION()
	void HandleEconomyUpdated(int64 InTreasuryTL, float InSatisfaction, int32 InCurrentTurn);

	UFUNCTION()
	void HandleRoadNetworkChanged();

	UPROPERTY()
	TObjectPtr<AAftershockCrowdHostActor> CrowdHostActor;

	UPROPERTY()
	TArray<FCitizenAgent> Agents;

	/** Cached valid destinations from the canonical grid/building records. */
	TArray<FIntPoint> WalkableCellCoords;
	TSet<int32> WalkableCellIndices;
	/** Per callback door throughput, derived scratch data rather than another population queue. */
	TMap<int32,int32> DeparturesThisUpdate;
	TArray<FIntPoint> RoadCellCoords;
	TSet<int32> RoadCellIndices;
	TArray<FIntPoint> CityVergeCellCoords;
	TArray<FVector> HomeLocations;
	TArray<FVector> WorkLocations;
	TArray<FVector> LeisureLocations;
	TArray<FVector> ShelterLocations;
	TArray<FAftershockCitizenDestination> HomeRecords;
	TArray<FAftershockCitizenDestination> WorkRecords;
	TArray<FAftershockCitizenDestination> LeisureRecords;
	TArray<FAftershockCitizenDestination> EmergencyCentres;
	TArray<FAftershockCitizenDestination> AllBuildingFrontages;
	TArray<FIntPoint> AssemblyCoords;
	/** MAX_int32 = blocked; road 10, public land 30, shallow visible-water penalty. */
	TArray<int32> RoutingCellCosts;
	TMap<int32, FAftershockCitizenRouteTree> NormalRouteTrees;
    TSharedPtr<FAftershockIncrementalNormalRoutes> IncrementalNormalRoutes;
	TMap<int32, uint64> NormalRouteTreeUseSerial;
	uint64 NormalRouteCacheSerial = 0;
	uint64 NormalRouteTreeBuildCount = 0;
	uint32 RoutingTopologySignature = 0;
	TArray<FTransform> UpdatedTransforms;
	mutable FAftershockCrowdSpacingStats SpacingStats;
	FAftershockCrowdRosterWork RosterRefreshWork;
	friend class FAftershockScopedCrowdRefresh;
	void BeginScopedCrowdRefresh();
	void EndScopedCrowdRefresh();
	int32 ScopedCrowdRefreshDepth=0;
	bool bScopedRosterRefreshPending=false,bScopedEmergencyPlanPending=false;
	bool bDeferEmergencyPlanDuringRosterFlush=false,bScopedPlanningAuditPending=false;
	TMap<FIntPoint,int32> SpacingBucketHeads;
	TArray<int32> SpacingNextIndices;
	/** Scratch only: authoritative predicates are memoized for one synchronous update. */
	TArray<int8> SpacingWalkabilityCache;
	TMap<uint64,bool> SpacingWalkingEdgeCache;
	TArray<float> SpacingRemainingCorrection;
	TMap<FIntPoint,int32> SpacingBucketCounts;
	TArray<FTransform> EmotionPooledTransforms;
	TArray<FVector> PreMovementPositions;
	/** Derived index, rebuilt after import/roster replacement and updated per accepted move. */
	mutable TMap<FIntPoint,TArray<int32>> CitizenSpatialBuckets;
	mutable TArray<FIntPoint> CitizenSpatialKeys;
	mutable TArray<uint8> CitizenSpatialIncluded;
	mutable bool bCitizenSpatialDirty=true;

	FTimerHandle CrowdTimerHandle;
	FTimerHandle DisasterVisualCompletionHandle;

	bool bIsPanicModeActive = false;
	EDisasterType EmergencyDisasterType = EDisasterType::Earthquake;
	float EmergencyStartedAtWorldSeconds = 0.f;
	float RoutingRefreshSeconds = 0.f;
	int32 ActiveEligibleCentres = 0;
	int32 ActiveVisualPlaceQuota = 0;
	FDelegateHandle DisasterSnapshotDelegateHandle;
	FDelegateHandle TerrainStorageDelegateHandle;
	int32 SimulationSeed = 1337;
	bool bUpdatingAgents=false;
	bool bCityLifeSyncPending=false;
    // Derived successful-sync watermarks, not a second population/death ledger.
    // Full canonical counters keep births/deaths immediate even beyond draw cap.
    int32 LastSynchronizedLivingPopulation=INDEX_NONE;
    int32 LastSynchronizedTotalDeaths=INDEX_NONE;
	bool bRefreshingTerrainDraws=false;
	bool bSyntheticRenderProfileActive=false;
	int32 SyntheticProfileSamples=0;
	double SyntheticProfileTotalMilliseconds=0.;
	double SyntheticProfileMaxMilliseconds=0.;

	/** ISM barındırıcı aktörü başlatır */
	AAftershockCrowdHostActor* GetOrCreateHostActor();
};
