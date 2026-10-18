// AFTERSHOCK — deterministic, cosmetic city dressing. Gameplay owns every cell.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"
#include "CityPresentationSubsystem.generated.h"

class FAftershockEnvironmentDressing;
class FAftershockConformingRoads;
class UDirectionalLightComponent;
class UExponentialHeightFogComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UInstancedStaticMeshComponent;
class UPostProcessComponent;
class USkyAtmosphereComponent;
class USkyLightComponent;
class UTerrainGridSubsystem;
class UBuildingManagerSubsystem;
class UStaticMeshComponent;
class AAftershockCameraPawn;
class UMaterialInstanceDynamic;
struct FAftershockWorldClimateState;

/** Derived render slots only; no gameplay or save identity is stored here. */
struct FAftershockCityBatchSlots
{
	TMap<FIntVector,int32> IndexByKey;
	TArray<FIntVector> KeyByIndex;
};

struct FAftershockSavedHeightFog
{
	TWeakObjectPtr<UExponentialHeightFogComponent> Component;
	float Density = 0.f;
	float SecondDensity = 0.f;
	bool bVisible = true;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnAftershockCityPresentationRebuilt, int32);

/** One non-ticking host, with one HISM batch per mesh. No prop is an actor. */
UCLASS(NotBlueprintable, Transient)
class JAMPROJECT_API AAftershockCityPresentationActor final : public AActor
{
	GENERATED_BODY()

public:
	AAftershockCityPresentationActor();
	void RebuildCosmeticDistrict(const UTerrainGridSubsystem& Terrain, const UBuildingManagerSubsystem& Buildings);
	void ConfigurePresentationLighting();
	void BuildGlobeVisual(const UTerrainGridSubsystem& Terrain);
	void UpdateWorldClimate(const UTerrainGridSubsystem& Terrain, const FAftershockWorldClimateState& Climate);
	void ApplyPresentationBudget();
	bool RefreshEnvironmentDressing(const UTerrainGridSubsystem& Terrain);
	int32 GetLastUpdatedBatchCount() const { return LastUpdatedBatchCount; }
	int32 GetLastChangedInstanceCount() const { return LastChangedInstanceCount; }
	int32 GetGeographicFocusMarkerCount() const;
	int32 GetRenderedInitialRoadCount() const { return RenderedInitialRoadCount; }
	int32 GetCanonicalInitialRoadCount() const { return CanonicalInitialRoadCount; }
	int32 GetExpectedVisibleInitialRoadCount() const { return ExpectedVisibleInitialRoadCount; }
	int32 GetWindowExcludedInitialRoadCount() const { return CanonicalInitialRoadCount-ExpectedVisibleInitialRoadCount; }
	int32 GetRenderedRoadCount() const { return RenderedRoadCount; }
	FIntRect GetRenderedCellBounds() const { return RenderedCellBounds; }
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	TSharedPtr<FAftershockEnvironmentDressing> EnvironmentDressing;
	TSharedPtr<FAftershockConformingRoads> ConformingRoads;
	void CaptureHeightFogForOverview();
	void UpdateOverviewFog();
	void RestoreHeightFog();
	TArray<FAftershockSavedHeightFog> SavedHeightFog;
	TWeakObjectPtr<AAftershockCameraPawn> FogCamera;
	FTimerHandle FogTransitionTimer;
	bool bCapturedHeightFog = false;
	float LastFogScale = -1.f;
	int32 LastFogTransitionRegion = INDEX_NONE;
	int32 LastPresentationTier = INDEX_NONE;
	TArray<FAftershockCityBatchSlots> CosmeticBatchSlots;
	int32 LastUpdatedBatchCount=0,LastChangedInstanceCount=0;
	void RebuildGeographicFocusMarkers(const UTerrainGridSubsystem& Terrain);
	uint64 GeographicMarkerSignature=0;

	UPROPERTY()
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> MeshBatches;
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> WorldGlobe;
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> WorldTransition;
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> WorldAtmosphere;
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> WorldClouds;
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> GeographicFocusMarkers;
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> GlobeMaterials;
	uint64 GlobeSignature = 0;
	int32 RenderedInitialRoadCount = 0;
	int32 CanonicalInitialRoadCount = 0;
	int32 ExpectedVisibleInitialRoadCount=0,RenderedRoadCount=0;
	FIntRect RenderedCellBounds=FIntRect(0,0,0,0);

	UPROPERTY()
	TObjectPtr<UDirectionalLightComponent> FallbackSun;
	UPROPERTY()
	TObjectPtr<USkyLightComponent> FallbackSky;
	UPROPERTY()
	TObjectPtr<USkyAtmosphereComponent> FallbackAtmosphere;
	UPROPERTY()
	TObjectPtr<UExponentialHeightFogComponent> FallbackFog;
	UPROPERTY()
	TObjectPtr<UPostProcessComponent> PresentationGrade;
};

/**
 * Draws canonical FGridCell::bHasRoad records, pavement and woodland. RoadNetwork
 * owns player road construction; this layer never creates/reserves road cells.
 * Current road contracts reject river crossings, so no bridges are generated.
 * Road events refresh immediately; a one-second signature observes other canonical
 * changes. Rebuild notifications let active weather restore per-instance wind data.
 */
UCLASS()
class JAMPROJECT_API UCityPresentationSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	FOnAftershockCityPresentationRebuilt OnPresentationRebuilt;
	int32 GetPresentationRevision() const { return PresentationRevision; }
	/** Read-only refresh after a loaded camera focus is imported. */
	void RefreshVisiblePresentation() { RefreshPresentation(); }

private:
	UFUNCTION()
	void RefreshPresentation();
	UFUNCTION()
	void HandleRoadNetworkChanged();
	void PollPresentation();

	UPROPERTY()
	TObjectPtr<AAftershockCityPresentationActor> PresentationActor;
	FTimerHandle RefreshTimer;
	uint32 LastVisualSignature = 0;
	bool bHasVisualSignature = false;
	bool bRefreshingPresentation=false;
	int32 PresentationRevision = 0;
	uint64 LastObservedTerrainRevision=MAX_uint64;
	int32 LastObservedFoliageQuality=-1;
	FIntRect LastObservedBounds=FIntRect(0,0,0,0);
	FIntVector LastObservedDetailCell=FIntVector(MAX_int32,MAX_int32,MAX_int32);
};
