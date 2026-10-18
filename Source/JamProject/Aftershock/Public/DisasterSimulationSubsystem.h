// AFTERSHOCK — Civil Game Jam 2026
// Dinamik Afet Simülasyonu, Sel Yayılımı ve Chaos Yıkım Motoru Alt Sistemi

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Aftershock/Public/BuildingData.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/AftershockPhysicalHazardState.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionActor.h"
#include "DisasterSimulationSubsystem.generated.h"

class AGeometryCollectionActor;
struct FAftershockRubbleSaveRecord;
struct FAftershockPhysicalHazardRuntime;

/** Supplies instance foundation boundary conditions after OnRegister reset and before proxy creation. */
UCLASS(NotBlueprintable, Transient)
class JAMPROJECT_API UAftershockFoundationGeometryCollectionComponent final : public UGeometryCollectionComponent
{
	GENERATED_BODY()
public:
	TArray<int32> FoundationAnchorIndices;
protected:
	virtual void OnCreatePhysicsState() override;
};

UCLASS(NotBlueprintable, Transient)
class JAMPROJECT_API AAftershockFoundationGeometryCollectionActor final : public AGeometryCollectionActor
{
	GENERATED_BODY()
public:
	AAftershockFoundationGeometryCollectionActor(const FObjectInitializer& ObjectInitializer);
};

USTRUCT(BlueprintType)
struct JAMPROJECT_API FAftershockDamQuote
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) bool bCanBuild = false;
	UPROPERTY(BlueprintReadOnly) int32 CostTL = 12000;
	UPROPERTY(BlueprintReadOnly) float StorageCapacityUnits = 180.f;
	UPROPERTY(BlueprintReadOnly) FText Feedback;
};

USTRUCT(BlueprintType)
struct JAMPROJECT_API FAftershockDamStatus
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) bool bHasDam = false;
	UPROPERTY(BlueprintReadOnly) float StorageCapacityUnits = 0.f;
	UPROPERTY(BlueprintReadOnly) float StoredWaterUnits = 0.f;
	UPROPERTY(BlueprintReadOnly) float RemainingStorageUnits = 0.f;
	UPROPERTY(BlueprintReadOnly) float LastRetainedUnits = 0.f;
	UPROPERTY(BlueprintReadOnly) float LastOvertoppingUnits = 0.f;
	UPROPERTY(BlueprintReadOnly) bool bOvertopped = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAftershockDamInfrastructureChanged, FIntPoint, Coord);

/**
 * FDisasterReport — Bir afet sona erdiğinde UI ve Ekonomi sistemine aktarılan bilanço raporu.
 */
USTRUCT(BlueprintType)
struct JAMPROJECT_API FDisasterReport
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Disaster")
	EDisasterType DisasterType = EDisasterType::Earthquake;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Disaster")
	float BaseSeverity = 0.0f;
	/** Accepted canonical event identity; persisted economics consume it once. */
	UPROPERTY(BlueprintReadOnly,Category="Disaster") FGuid DisasterEventID;
	UPROPERTY(BlueprintReadOnly,Category="Disaster") TArray<int32> ActualDeathResidentIDs;
	UPROPERTY(BlueprintReadOnly,Category="Disaster") TArray<int32> ExposedSurvivorResidentIDs;
	UPROPERTY(BlueprintReadOnly,Category="Disaster") int64 TotalDeathCostTL=0;
	/** A service-needs casualty is not another hazard or progression event. */
	UPROPERTY(BlueprintReadOnly,Category="Disaster") bool bNeedsCasualtyEvent=false;
	/** Rejected events emit no snapshot/completion and cannot change economy. */
	UPROPERTY(BlueprintReadOnly, Category="Disaster")
	bool bWasRejected = false;
	/** Accepted asynchronous physical event; no final outcome has settled yet. */
	UPROPERTY(BlueprintReadOnly,Category="Disaster") bool bIsPending=false;
	UPROPERTY(BlueprintReadOnly, Category="Disaster")
	FText RejectionReason;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Disaster")
	int32 TotalBuildingsAffected = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Disaster")
	int32 TotalBuildingsCollapsed = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Disaster")
	int32 TotalDeaths = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Disaster")
	int32 TotalSurvivors = 0;

	/** Oyuncunun altyapı, temel, hastane ve retrofit yatırımları sayesinde kurtarılan tahmini insan sayısı */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Disaster")
	int32 PreventedDeathsEstimate = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Disaster")
	float TotalSatisfactionChange = 0.0f;

	/** Only unique, actually exposed survivors receive 150TL aid each. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Disaster")
	int64 TotalStateAidGranted = 0;
	/** Persistent ecology/service shock; dimensionless, never structural damage for drought. */
	UPROPERTY(BlueprintReadOnly,Category="Disaster") float WaterSupplyStress01 = 0.f;
	UPROPERTY(BlueprintReadOnly,Category="Disaster") float CropLoss01 = 0.f;
	UPROPERTY(BlueprintReadOnly,Category="Disaster") int32 ForestCellsBurned = 0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDisasterCompletedSignature, const FDisasterReport&, Report);

/** Exact canonical hazard inputs/results. Physical units exist only in versioned V3 flood fields. */
USTRUCT(BlueprintType)
struct JAMPROJECT_API FAftershockDisasterVisualSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Disaster|Presentation")
	int32 Revision = 0;
	UPROPERTY(BlueprintReadOnly, Category="Disaster|Presentation")
	EDisasterType Type = EDisasterType::Earthquake;
	UPROPERTY(BlueprintReadOnly, Category="Disaster|Presentation")
	float BaseSeverity = 0.f;
	/** Post city-prevention input, still dimensionless gameplay intensity. */
	UPROPERTY(BlueprintReadOnly, Category="Disaster|Presentation")
	float EffectiveIntensity = 0.f;
	UPROPERTY(BlueprintReadOnly, Category="Disaster|Presentation")
	float StartedAtWorldSeconds = 0.f;
	UPROPERTY(BlueprintReadOnly, Category="Disaster|Presentation")
	float DurationSeconds = 24.f;
	/** Authored event clock; pre-duration quake saves retain their ten-second bed. */
	float GetEffectiveDurationSeconds() const
	{
		return Type==EDisasterType::Earthquake?FMath::Max(10.f,DurationSeconds):DurationSeconds;
	}
	/** Exact canonical localized gameplay severity; never reconstruct V3 water depth from this value. */
	UPROPERTY(BlueprintReadOnly, Category="Disaster|Presentation")
	TMap<FIntPoint, float> LocalSeverity;
	/** Scenario wind input shared by wind exposure, wildfire spread, ash and their visuals. */
	UPROPERTY(BlueprintReadOnly, Category="Disaster|Presentation")
	FVector DominantWindDirection = FVector(1.f, 0.35f, 0.f);
	UPROPERTY(BlueprintReadOnly, Category="Disaster|Presentation")
	bool bWindDirectionIsScenarioDefault = true;
	/** Scenario shoreline direction; older south-coast scenarios travel toward -Y. */
	UPROPERTY(BlueprintReadOnly, Category="Disaster|Presentation")
	FVector PropagationDirection = FVector::ZeroVector;
	/** Authored run-up calibration from the canonical tsunami scenario, not a CFD prediction. */
	UPROPERTY(BlueprintReadOnly, Category="Disaster|Presentation")
	float SurgePeakHeadCm = 0.f;
	UPROPERTY(BlueprintReadOnly,Category="Disaster|Presentation") TMap<FIntPoint,float> LavaSeverity;
	UPROPERTY(BlueprintReadOnly,Category="Disaster|Presentation") TMap<FIntPoint,float> AshSeverity;
	UPROPERTY(BlueprintReadOnly,Category="Disaster|Presentation") FIntPoint SourceCoord = FIntPoint(INDEX_NONE,INDEX_NONE);
	UPROPERTY(BlueprintReadOnly,Category="Disaster|Presentation") float DroughtStress01 = 0.f;
	UPROPERTY(BlueprintReadOnly,Category="Disaster|Presentation") float CropLoss01 = 0.f;
	/** Same canonical pre/post river occupancy of capacity; dimensionless, never measured depth. */
	UPROPERTY(BlueprintReadOnly,Category="Disaster|Presentation") TMap<FIntPoint,float> RiverFillBefore;
	UPROPERTY(BlueprintReadOnly,Category="Disaster|Presentation") TMap<FIntPoint,float> RiverFillAfter;
	/** Additive V3 event-pulse hydrology. Zero retains legacy severity reconstruction. */
	UPROPERTY(BlueprintReadOnly,Category="Disaster|Presentation") int32 FloodModelVersion=0;
	UPROPERTY(BlueprintReadOnly,Category="Disaster|Presentation") float RainfallPulseMm=0.f;
	UPROPERTY(BlueprintReadOnly,Category="Disaster|Presentation") float FloodPulseDurationSeconds=0.f;
	/** Physical column above the authored hydraulic BED, not necessarily above DEM. */
	UPROPERTY(BlueprintReadOnly,Category="Disaster|Presentation") TMap<FIntPoint,float> WaterDepthCm;
	/** Absolute hydraulic head in world cm; river heads can be below the DEM bank. */
	UPROPERTY(BlueprintReadOnly,Category="Disaster|Presentation") TMap<FIntPoint,float> WaterSurfaceElevationCm;
	UPROPERTY(BlueprintReadOnly,Category="Disaster|Presentation") TMap<FIntPoint,float> WaterArrivalFraction;
	UPROPERTY(BlueprintReadOnly,Category="Disaster|Presentation") TMap<FIntPoint,float> LavaArrivalFraction;
};

/** Transient numerical accounting only; never serialized or treated as a forecast. */
struct FAftershockFloodBudgetStats
{
	double RainfallM3=0.,InitialRiverStorageM3=0.,InfiltrationM3=0.,DrainageM3=0.,OutletM3=0.,DamRetainedM3=0.,FinalStorageM3=0.,MassResidualM3=0.;
	int32 ActiveCells=0,Iterations=0;
	int64 EdgeVisits=0;
	bool bCommittedStorage=false;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnAftershockDisasterVisualSnapshot, const FAftershockDisasterVisualSnapshot&);

/**
 * UDisasterSimulationSubsystem — Altyapı yatırımları, dinamik sel simülasyonu ve Chaos yıkım motoru yöneticisi.
 */
UCLASS(BlueprintType)
class JAMPROJECT_API UDisasterSimulationSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
	friend struct FAftershockPhysicalHazardAuditDriver;

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& World) override;
	bool IsHydraulicModel2Active() const { return PhysicalState.ModelVersion==2; }
	bool IsPhysicalEventRunning() const { return PhysicalState.Episode.bActive; }
	/** A stopped worker is an error, never a successfully completed disaster. */
	bool GetPhysicalRuntimeFailure(FText& Reason) const;
	/** Read-only accepted clock; pending mass has an empty replay until solved. */
	const FAftershockPhysicalEpisodeSaveState& GetPhysicalEpisodeState() const { return PhysicalState.Episode; }
	bool TryGetPhysicalAudioProgress(double& PlaybackSeconds,double& SourceSeconds,double& SourceDurationSeconds,float& MovingMass01) const;
	double GetPublishedHydraulicSeconds() const { return PhysicalState.PhysicalSeconds; }
	void CapturePhysicalSaveState(FAftershockPhysicalHazardSaveState& Out) const;
	bool RestorePhysicalSaveState(const FAftershockPhysicalHazardSaveState& Saved,FText& Error);
	/** Canonical water sample; independent of camera, actor visibility and quality. */
	bool TryGetPhysicalWaterSample(FIntPoint Coord,double& HeadCm,double& DepthCm,FVector2D& VelocityCmPerSecond) const;
	float GetPhysicalHazardWeight(EDisasterType Type) const;

	// -------- DELEGATE --------
	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnDisasterCompletedSignature OnDisasterCompleted;

	/** Native render event emitted before the gameplay completion UI event. */
	FOnAftershockDisasterVisualSnapshot OnDisasterVisualSnapshotChanged;
	UPROPERTY(BlueprintAssignable, Category="Aftershock|Infrastructure")
	FOnAftershockDamInfrastructureChanged OnDamInfrastructureChanged;
	const FAftershockDisasterVisualSnapshot& GetLatestVisualSnapshot() const { return LatestVisualSnapshot; }
	void CaptureRubbleSaveState(TArray<FAftershockRubbleSaveRecord>& OutState) const;
	bool RestoreRubbleSaveState(const TArray<FAftershockRubbleSaveRecord>& State, FText& OutError);
	bool RestoreVisualSaveState(const FAftershockDisasterVisualSnapshot& State, float RemainingSeconds);

	/** Authored wind direction shared by hazard fields and presentation; not a weather forecast. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Presentation")
	FVector ScenarioWindDirection = FVector(1.f, 0.35f, 0.f);

	// -------- ANA AFET SİMÜLASYONU --------
	/**
	 * Afeti tüm şehir binaları, arazi verileri ve altyapı önlemleri üzerinden simüle eder.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Disaster")
	FDisasterReport TriggerDisaster(
		EDisasterType Type,
		float BaseSeverity,
		float CityAwarenessLevel,
		bool bIsDaytime);

	/**
	 * Nehir taşması ve sokak drenaj kapasitelerine göre hidrolik sel yayılımını hesaplar.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Disaster")
	TMap<FIntPoint, float> SimulateFloodMap(float RainfallIntensity);
	const FAftershockFloodBudgetStats& GetLastPhysicalFloodBudget() const { return LastFloodBudget; }
	/** Direction/exposure-aware authored gust-pressure field, not a weather forecast. */
	UFUNCTION(BlueprintPure,Category="Aftershock|Disaster")
	TMap<FIntPoint,float> SimulateWindMap(float BaseSeverity) const;

	/** Coast-connected gameplay field; dimensionless severity, authored run-up envelope. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Disaster")
	TMap<FIntPoint, float> SimulateTsunamiMap(float BaseSeverity) const;
	UFUNCTION(BlueprintPure, Category="Aftershock|Disaster")
	bool CanTriggerDisaster(EDisasterType Type, FText& OutReason) const;
	UFUNCTION(BlueprintPure,Category="Aftershock|Disaster")
	TMap<FIntPoint,float> SimulateForestFireMap(float BaseSeverity) const;
	UFUNCTION(BlueprintCallable,Category="Aftershock|Disaster")
	TMap<FIntPoint,float> SimulateVolcanoMap(float BaseSeverity,TMap<FIntPoint,float>& OutLava,TMap<FIntPoint,float>& OutAsh) const;
	UFUNCTION(BlueprintPure,Category="Aftershock|Disaster")
	TMap<FIntPoint,float> SimulateDroughtMap(float BaseSeverity) const;
	/** One atomic500TL authored substrate investigation. Not real geotechnical advice. */
	UFUNCTION(BlueprintCallable,Category="Aftershock|Infrastructure")
	bool SurveyGround(FIntPoint Coords,int32& OutCostTL);

	UFUNCTION(BlueprintPure, Category="Aftershock|Infrastructure")
	FAftershockDamQuote QuoteDam(FIntPoint Coords) const;
	UFUNCTION(BlueprintPure, Category="Aftershock|Infrastructure")
	bool CanBuildDam(FIntPoint Coords, FText& OutReason) const;
	/** Atomic purchase: this method already charges the economy. Never charge twice. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Infrastructure")
	bool InvestInDam(FIntPoint Coords, int32& OutCostTL);
	UFUNCTION(BlueprintPure, Category="Aftershock|Infrastructure")
	FAftershockDamStatus GetDamStatus(FIntPoint Coords) const;
	UFUNCTION(BlueprintPure, Category="Aftershock|Infrastructure")
	int32 GetDamBuildCostTL() const { return 12000; }

	// -------- CHAOS YIKIM VE ENKAZ YÖNETİMİ --------
	/**
	 * 80-100 hasar alarak çöken binalar için Chaos Destruction aktörü spawn eder.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Destruction")
	void SpawnChaosDestructionForBuilding(const FBuildingInstance& Building);

	/**
	 * BUILDING_STATS.md Bölüm 5: Çöken binanın enkazını temizler (3000 TL) ve hücreyi yeni inşaata açar.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Destruction")
	bool ClearCollapsedRubble(FIntPoint GridCoords, int32& OutCostTL);

	// -------- ALTYAPI YATIRIM KÖPRÜLERİ --------
	/** Nehir yatağını genişletir (Maliyet: 2500 TL). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Infrastructure")
	bool InvestInRiverExpansion(FIntPoint Coords, int32& OutCostTL);

	/** Su akışını yavaşlatıcı set/bent inşa eder (Maliyet: 2000 TL). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Infrastructure")
	bool InvestInWaterDamping(FIntPoint Coords, int32& OutCostTL);

	/** Hücre ve çevresindeki sokakların drenajını temizler/geliştirir. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Infrastructure")
	bool InvestInStreetDrainage(FIntPoint Coords, int32 Radius, int32& OutCostTL);

	UFUNCTION(BlueprintPure, Category="Aftershock|Destruction")
	bool HasCollapsedRubbleAt(FIntPoint GridCoords) const { return CollapsedRubbleActors.Contains(GridCoords); }
	/** Derived draw mask only. Never changes saved poses, collision or solver state. */
	void RefreshVisibleRubblePresentation(bool bForce=false);

	/** Equilibrium flow velocity proxies for flood presentation */
	TMap<FIntPoint, float> ComputedVelX, ComputedVelY;

protected:
	void AdvancePhysicalHazards();
	FDisasterReport BeginPhysicalDisaster(EDisasterType Type,float Severity,float Awareness,bool bDaytime);
	void ApplyPhysicalExposure();
	void CompletePhysicalEpisode();
	void ShutdownPhysicalRuntime();
	bool RebuildPhysicalWaterDomain(FText& Error);
	void PublishPhysicalWater();
	void InitializePhysicalMaterials();
	bool StagePhysicalMassJob(FText& Error);
	UPROPERTY(Transient) FAftershockPhysicalHazardSaveState PhysicalState;
	TSharedPtr<FAftershockPhysicalHazardRuntime> PhysicalRuntime;
	FTimerHandle PhysicalTimer;
	TMap<FIntPoint, float> BuildFloodMapWithDams(float RainfallIntensity, bool bCommitDamStorage);
	TMap<FIntPoint,float> BuildPhysicalFloodMap(float RainfallIntensity,bool bCommitStorage);
	TMap<FIntPoint,float> BuildVolcanoField(float BaseSeverity,TMap<FIntPoint,float>& OutLava,TMap<FIntPoint,float>& OutAsh,TMap<FIntPoint,float>& OutArrival) const;
	FAftershockFloodBudgetStats LastFloodBudget;
	TMap<FIntPoint,float> ComputedWaterDepthCm,ComputedWaterHeadCm,ComputedWaterArrival,ComputedInfiltrationCm;
	TMap<FIntPoint, float> BuildDamRetentionMap(float RainfallIntensity, bool bCommitDamStorage);
	bool bDamTransactionInProgress = false;
	UPROPERTY(Transient)
	FAftershockDisasterVisualSnapshot LatestVisualSnapshot;

	/** 4 saniye sonra fiziği kapatıp enkazı donduran fonksiyon */
	void FreezeChaosActor(TWeakObjectPtr<AActor> ChaosActorPtr);

	/** Oyuncunun önlemleri olmasaydı (Weak temel, 0 retrofit, 0 drenaj) gerçekleşecek teorik ölümü hesaplar */
	int32 CalculateTheoreticalZeroPreventionDeaths(
		const FBuildingInstance& Building,
		EDisasterType DisasterType,
		float LocalSeverity,
		float CityAwarenessLevel,
		bool bIsDaytime) const;

	/** Aktif fizik simülasyonu yapan Chaos aktörleri (Max 15) */
	UPROPERTY()
	TArray<TWeakObjectPtr<AActor>> ActiveChaosActors;

	/** Koordinat bazlı dondurulmuş/aktif enkaz aktörleri */
	UPROPERTY()
	TMap<FIntPoint, TObjectPtr<AActor>> CollapsedRubbleActors;
	FIntRect RubbleDrawBounds=FIntRect(0,0,0,0);
	bool bHasRubbleDrawBounds=false;
};
