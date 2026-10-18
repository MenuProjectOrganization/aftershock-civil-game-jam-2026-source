// AFTERSHOCK — visible weather from the canonical latest disaster snapshot.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "DisasterPresentationSubsystem.generated.h"

class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UTerrainGridSubsystem;
class UStaticMeshComponent;

/** Read-only evidence about actual generated display geometry, never hazard inputs. */
struct FAftershockVolcanoVisualStats
{
	int32 CanonicalLavaCells=0,DrawnLavaCells=0,LavaTopTriangles=0,LavaSkirtTriangles=0;
	int32 VentTriangles=0,PlumeTriangles=0,SpatterInstances=0;
	float MinimumGroundClearanceCm=0.f;
	bool bSourceResident=false,bSourceInDrawableWindow=false,bSourceContextBacked=false,bMaterialsReady=false;
	FVector SourceWorld=FVector::ZeroVector;
};

struct FAftershockVolcanoSpatter
{
	FVector Velocity=FVector::ZeroVector;
	float PhaseSeconds=0.f,LifetimeSeconds=1.f,RadiusCm=15.f;
};

struct FAftershockWeatherStreak
{
	FIntPoint Cell;
	FVector Offset = FVector::ZeroVector;
	float Phase = 0.f;
	float LocalStrength = 0.f;
	float VisualSpeed = 0.f;
	float Length = 0.f;
};

/** Bounded display seeds derived from burning canonical cells; never fire simulation state. */
struct FAftershockFireVisualSource
{
	FIntPoint Cell=FIntPoint::ZeroValue;
	FVector Anchor=FVector::ZeroVector;
	float Strength=0.f,RadiusCm=80.f,HeightCm=200.f,Phase=0.f;
	bool bBuilding=false;
};

struct FAftershockFireVisualState
{
	TArray<FAftershockFireVisualSource> Sources;
	TArray<FTransform> Transforms[4];
	FVector WindDirection=FVector::ForwardVector;
	int32 CanonicalBurningCells=0,BuildingSources=0,AuthoredMaterials=0;
	int32 BuiltTier=INDEX_NONE,BuiltEffectsQuality=INDEX_NONE;
};

/** Game-thread batch submission only; excludes GPU execution and image readback. */
struct FAftershockPresentationCpuStats
{
	double TotalMilliseconds=0.,PeakMilliseconds=0.;
	uint64 Updates=0;
};

/** Persistent terrain consequences outlive the short event animation. Read-only, bounded ISMs. */
UCLASS(NotBlueprintable, Transient)
class JAMPROJECT_API AAftershockEcologyStateActor final : public AActor
{
	GENERATED_BODY()
public:
	AAftershockEcologyStateActor();
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	void RefreshFromTerrain(const UTerrainGridSubsystem& Terrain, EDisasterType AnimatedType, bool bAnimationActive);
	void RefreshRegionalVolcanoVent(const UTerrainGridSubsystem& Terrain);
	FString DescribeCurrentVisuals() const;
	uint64 GetCanonicalCellsVisited() const { return CanonicalCellsVisited; }
	uint64 GetRebuildCount() const { return RebuildCount; }
	const FAftershockVolcanoVisualStats& GetRegionalVolcanoVisualStats() const { return RegionalVolcanoStats; }
	const FAftershockFireVisualState& GetFireVisualStateForAudit() const { return FireVisuals; }
	UInstancedStaticMeshComponent* GetFireBatchForAudit(int32 Layer) const { return FireBatches.IsValidIndex(Layer)?FireBatches[Layer].Get():nullptr; }
	FAftershockPresentationCpuStats GetFireAnimationCpuStatsForAudit() const { return FireAnimationCpuStats; }
private:
	friend class UAftershockFireVisualAuditCommandlet;
	void AnimatePersistentFire();
	UPROPERTY() TArray<TObjectPtr<UInstancedStaticMeshComponent>> FireBatches;
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> FireMaterials;
	FAftershockFireVisualState FireVisuals;
	FAftershockPresentationCpuStats FireAnimationCpuStats;
	FTimerHandle FireAnimationTimer;
	UPROPERTY() TArray<TObjectPtr<UInstancedStaticMeshComponent>> Batches;
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> RegionalVolcanoVent;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> PersistentLavaVolume;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> VentMaterial;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> PersistentLavaMaterial;
	uint32 RegionalVentSignature=0;
	bool bHasRegionalVentSignature=false;
	uint32 PersistentLavaStateSignature=0;
	bool bHasPersistentLavaStateSignature=false;
	FAftershockVolcanoVisualStats RegionalVolcanoStats;
	uint32 LastStateSignature = 0;
	bool bHasStateSignature = false;
	uint64 LastTerrainRevision=MAX_uint64,CanonicalCellsVisited=0,RebuildCount=0;
	uint32 LastFrameSignature=0;
};

/** Bounded event ISM batches, one actor; no rain-drop/wind-streak actor or tick. */
UCLASS(NotBlueprintable, Transient)
class JAMPROJECT_API AAftershockDisasterPresentationActor final : public AActor
{
	GENERATED_BODY()

public:
	AAftershockDisasterPresentationActor();
	void BeginSnapshot(const FAftershockDisasterVisualSnapshot& Snapshot, const UTerrainGridSubsystem& Terrain);
	void AnimateSnapshot(const FAftershockDisasterVisualSnapshot& Snapshot, const UTerrainGridSubsystem& Terrain, float AgeSeconds);
	void ClearPresentation();
	void RebindFoliageWind(const FAftershockDisasterVisualSnapshot& Snapshot, const UTerrainGridSubsystem& Terrain, float AgeSeconds);
	/** Rebuild clipped geometry at the SAME event age when the camera changes chunk window. */
	void RefreshVisibleWindow(const FAftershockDisasterVisualSnapshot& Snapshot,const UTerrainGridSubsystem& Terrain,float AgeSeconds);
	/** Installed-ground draw mask, independent of the full resident hazard fields. */
	FIntRect GetBuiltDrawableCellBounds() const { return BuiltVisibleCellBounds; }
	FString DescribeCurrentVisuals() const;
	float GetDisplayedFloodDepthCm(FIntPoint Coord) const;
	uint64 GetRetainedTransientMeshBytes() const;
	const FAftershockVolcanoVisualStats& GetVolcanoVisualStats() const { return VolcanoStats; }
	const FAftershockFireVisualState& GetFireVisualStateForAudit() const { return FireVisuals; }
	UInstancedStaticMeshComponent* GetFireBatchForAudit(int32 Layer) const { return FireBatches.IsValidIndex(Layer)?FireBatches[Layer].Get():nullptr; }

private:
	friend class UAftershockGeographicHazardsAuditCommandlet;
	friend class UAftershockFireVisualAuditCommandlet;
	void BindFoliageWind(const FAftershockDisasterVisualSnapshot& Snapshot, const UTerrainGridSubsystem& Terrain);
	void BuildVolcanoPresentation(const FAftershockDisasterVisualSnapshot& Snapshot,const UTerrainGridSubsystem& Terrain);
	void AnimateVolcanoPresentation(const FAftershockDisasterVisualSnapshot& Snapshot,float AgeSeconds,float Envelope);
	UPROPERTY() TArray<TObjectPtr<UInstancedStaticMeshComponent>> FireBatches;
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> FireMaterials;
	FAftershockFireVisualState FireVisuals;

	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> FloodSurfaceISM;
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> WindStreakISM;
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> RainStreakISM;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> RiverStageISM;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> LavaSurfaceISM;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> AshCloudISM;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> DroughtSurfaceISM;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> QuakeEpicenterRingISM;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> QuakeFissureISM;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> DisasterEpicenterBeaconISM;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> LavaMaterial;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> AshMaterial;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> DroughtMaterial;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> VolcanoLavaVolume;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> VolcanoPlume;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> VolcanoSpatterISM;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> VolcanoLavaMaterial;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> VolcanoPlumeMaterial;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> VolcanoSpatterMaterial;
	TArray<FAftershockVolcanoSpatter> VolcanoSpatter;
	TArray<FTransform> VolcanoSpatterTransforms;
	FAftershockVolcanoVisualStats VolcanoStats;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> TsunamiSurface;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> TsunamiMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> FloodMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> WindMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> RainMaterial;
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> FoliageWindMaterials;
	UPROPERTY() TObjectPtr<UMaterialInterface> RiverWaterMaterial;
	int32 FoliageBoundSnapshotRevision=INDEX_NONE,FoliageBoundCityRevision=INDEX_NONE;
	FIntRect FoliageBoundCellBounds=FIntRect(0,0,0,0);

	TArray<FIntPoint> FloodCells;
	TMap<FIntPoint, float> VisibleFloodPeakDepthCm;
	TMap<FIntPoint,float> VisibleFloodArrival;
	bool bPhysicalFloodArrival=false;
	float CurrentFloodArrivalProgress=1.f;
	float CurrentFloodEnvelope = 0.f;
	TMap<FIntPoint,float> TsunamiPeakDepthCm;
	TMap<FIntPoint,float> TsunamiArrivalFraction;
	bool bTsunamiActive = false;
	float CurrentTsunamiProgress = 0.f;
	float CurrentTsunamiDrain = 0.f;
	int32 TsunamiOceanCells = 0;
	int32 TsunamiLandCells = 0;
	int32 VisibleFloodLandCells = 0;
	int32 FloodVolumeFrontTriangles = 0;
	TArray<FAftershockWeatherStreak> WindStreaks;
	TArray<FAftershockWeatherStreak> RainStreaks;
	TArray<FTransform> WindTransforms;
	TArray<FTransform> RainTransforms;
	TArray<FIntPoint> RiverStageCells;
	TArray<float> RiverStageHeadroomCm;
	TArray<FTransform> RiverStageTransforms;
	float CurrentRiverStageProgress=0.f;
	FIntRect BuiltVisibleCellBounds=FIntRect(0,0,0,0);
};

/**
 * Reads canonical physical columns/arrival for versioned physical snapshots.
 * Legacy severity-only snapshots retain the old art-calibrated reconstruction;
 * neither rendering path writes a hazard input or terrain height.
 * Wind direction is explicit scenario metadata until the shared solver owns one.
 */
UCLASS()
class JAMPROJECT_API UDisasterPresentationSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	UFUNCTION(BlueprintPure, Category="Aftershock|Presentation")
	FString DescribeCurrentVisuals() const;
	/** Current display inundation for all resident cells; independent of the clipped render window. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Presentation")
	float GetDisplayedFloodDepthCm(FIntPoint Coord) const;
	/** Read-only derived meshes/ecology at the existing snapshot age; no hazard replay. */
	void RefreshVisiblePresentation();
	/** Existing terrain/camera publications also update the passive real-source vent. */
	void RefreshRegionalVolcanoPresentation();
	FAftershockPresentationCpuStats GetAnimationCpuStatsForAudit() const { return {TotalAnimationCpuMilliseconds,PeakAnimationCpuMilliseconds,static_cast<uint64>(AnimationUpdateCount)}; }
	double GetLastHandoffCpuMillisecondsForAudit() const { return LastHandoffCpuMilliseconds; }

private:
	friend class UAftershockFireVisualAuditCommandlet;
	/** New accepted events only; snapshot restore/window rebuilding never broadcasts completion. */
	UFUNCTION() void HandleDisasterCompleted(const FDisasterReport& Report);
	void HandleSnapshot(const FAftershockDisasterVisualSnapshot& Snapshot);
	void HandleCityPresentationRebuilt(int32 CityRevision);
	void UpdatePresentation();
	void UpdatePersistentEcology();
	void ReleaseCompletedSnapshotFields();
	UPROPERTY()
	TObjectPtr<AAftershockDisasterPresentationActor> PresentationActor;
	UPROPERTY() TObjectPtr<AAftershockEcologyStateActor> EcologyStateActor;
	UPROPERTY(Transient)
	FAftershockDisasterVisualSnapshot ActiveSnapshot;
	FTimerHandle AnimationTimer;
	FTimerHandle EcologyStateTimer;
	FDelegateHandle SnapshotDelegateHandle;
	FDelegateHandle CityRebuiltDelegateHandle;
	bool bLoggedPeakState = false;
	double TotalAnimationCpuMilliseconds = 0.0;
	double PeakAnimationCpuMilliseconds=0.,LastHandoffCpuMilliseconds=0.;
	int32 AnimationUpdateCount = 0;
	int32 ActiveCanonicalCellCount=0;
};
