// AFTERSHOCK — Civil Game Jam 2026
// Arazi Grid Sistemi, Dinamik Afet ve Önleme Alt Sistemi

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Aftershock/Public/BuildingData.h"
#include "TerrainGridSubsystem.generated.h"

struct FAftershockTerrainSaveState;
struct FAftershockGeographicTerrainSample;
struct FAftershockGeographicBounds;
class UTerrainGridSubsystem;

/** Scenario substrate class, not a real parcel geotechnical investigation. */
UENUM(BlueprintType)
enum class EAftershockSoilType : uint8 { Bedrock, Alluvium };

/**
 * FGridCell — Her grid hücresinin topoğrafik, hidrolojik ve afet direnç verisi.
 */
USTRUCT(BlueprintType)
struct JAMPROJECT_API FGridCell
{
	GENERATED_BODY()

	/** Grid koordinatı (X, Y). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
	FIntPoint Coords = FIntPoint::ZeroValue;

	/** Initial oval placement mask; explicit expansion unlocks initial non-ocean land. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain")
	bool bIsPlayable = true;

	/** Metre veya cm cinsinden yükseklik. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
	float Elevation = 0.0f;
	/** Authored hydraulic cut below unchanged bank/DEM, cm; not surveyed bathymetry. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Hydraulics")
	float HydraulicExcavationCm = 0.f;
	/** 0=no authored profile; 1=explicit Model2 channel/bathymetry profile. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Hydraulics")
	int32 HydraulicProfileVersion = 0;
	/** Interpolated sparse Copernicus DEM altitude; separate from compressed game centimetres. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Geography")
	float RealElevationMetres = 0.f;
	/** True only inside a validated source DEM footprint, never fabricated outside it. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Geography")
	bool bHasRealElevationSample = false;
	/** Newly generated continuation is authored game terrain, not extended real DEM. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Geography")
	bool bAuthoredContinuation = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Ground")
	EAftershockSoilType SoilType = EAftershockSoilType::Bedrock;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Ground")
	bool bGroundSurveyed = false;
	/** Finite authored material stock. V4 loads leave stock/overlay/version0.
	 * Snow mm is water equivalent; pore-water depth is m3/planm2, never surface
	 * flood depth or the ecological SoilMoisture01 proxy. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Material") float SnowWaterEquivalentMm=0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Material") float SnowBulkDensityKgM3=300.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Material") float SnowWeakLayerStrengthPa=1000.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Material") float RegolithDepthM=0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Material") float SoilPorosity01=.35f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Material") float SoilDryBulkDensityKgM3=1500.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Material") float SoilPoreWaterDepthM=0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Material") float SoilCohesionPa=3000.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Material") float SoilFrictionAngleDegrees=32.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Material") float GroundErodedDepthM=0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Material") float GroundDepositedDepthM=0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Material") int32 MaterialScenarioVersion=0;

	/** Authored ecology overlay. Values are dimensionless gameplay state, not satellite measurements. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Ecology")
	float ForestFuel01 = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Ecology")
	float SoilMoisture01 = .65f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Ecology")
	float CropHealth01 = 1.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Ecology")
	float DroughtStress01 = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Ecology")
	bool bAgriculturalLand = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Volcano")
	bool bVolcanicVent = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Volcano")
	float LavaCoverage01 = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Volcano")
	float AshLoad01 = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Water")
	float RiverFill01 = .55f;

	/** Nehir yatağında mı. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
	bool bIsRiverbed = false;

	/** Canonical sea cells are outside playable building/navigation land. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Coast")
	bool bIsOcean = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Coast")
	bool bIsCoastal = false;

	/** Sole dam authority. Storage units are authored event-pulse units, NOT m3. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Dam")
	bool bHasDam = false;
	/** Metres above this cell's unchanged bank elevation. Authored, not surveyed. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Dam")
	float DamCrestHeightM = 2.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Dam")
	float DamGateOpen01 = 0.f;
	/** Canonical cardinal downstream neighbour; zero is an unmigrated/invalid dam axis. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Dam")
	FIntPoint DamOutflowStep = FIntPoint::ZeroValue;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Dam")
	float DamStorageCapacityUnits = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Dam")
	float DamStoredWaterUnits = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Dam")
	float DamLastRetainedUnits = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Dam")
	float DamLastOvertoppingUnits = 0.f;

	/** Nehir genişletme yatırımları ile artan taşkın kapasitesi. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain", meta=(ClampMin="0.0"))
	float RiverCapacity = 1.0f;

	/** Su akışını yavaşlatma önlemleri (bent, teraslama vb. 0.0 - 1.0). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain", meta=(ClampMin="0.0", ClampMax="1.0"))
	float WaterFlowDamping = 0.0f;

	/** Şehir içi sokak drenaj ve su tahliye kalitesi (0.0 - 1.0). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain", meta=(ClampMin="0.0", ClampMax="1.0"))
	float DrainageQuality = 0.0f;

	/** 0.0 - 1.0 arası fay hattı yakınlığı (yakınlaştıkça deprem şiddeti artar). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain", meta=(ClampMin="0.0", ClampMax="1.0"))
	float FaultLineProximity = 0.0f;

	/** 1.0 - 1.5 arası vadi/rüzgar koridoru rüzgar şiddet çarpanı. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain", meta=(ClampMin="1.0", ClampMax="1.5"))
	float ValleyWindMultiplier = 1.0f;

	/** Parsel eğimi (derece veya eğim oranı). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
	float Slope = 0.0f;

	/** Nehire olan mesafe (metre / cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
	float RiverDistance = 1000.0f;

	/** Nehire yakınlık oranı (0.0 = uzak, 1.0 = tam nehir üzerinde, FTerrainCell uyumluluğu). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain", meta=(ClampMin="0.0", ClampMax="1.0"))
	float RiverProximity = 0.0f;

	/** Başlangıç su/sel yüksekliği. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
	float InitialFloodDepth = 0.0f;

	/** Aktif simülasyondaki su/sel seviyesi. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
	float FloodDepth = 0.0f;

	/** Parsel inşaat elverişlilik skoru (0.0 - 1.0, eğim/sel/zemin uygunluğu). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ParcelSuitability = 1.0f;

	/** Hücrede aktif yangın var mı. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
	bool bBurning = false;

	/** Hücrenin yandığı tur sayısı. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
	int32 BurningTurns = 0;

	/** Player-built road occupancy; no generated or renderer-owned road network. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain|Roads")
	bool bHasRoad = false;

	/** Bridge/ramp membership; river and hydraulic fields remain unchanged. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Roads")
	int32 BridgeID = INDEX_NONE;
	/** 0=ordinary road, 1=X crossing, 2=Y crossing. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Roads")
	uint8 BridgeAxis = 0;
	/** Canonical deck/ramp surface height in world cm for this bridge cell. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Roads")
	float RoadSurfaceElevationCm = 0.f;
	/** Ground-level approach endpoints can connect to ordinary side streets. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Roads")
	bool bBridgeEndpoint = false;
	/** Preserve an existing bank road when the whole bridge is demolished. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Roads")
	bool bRoadPredatedBridge = false;

	/** Authored public evacuation assembly cell; never a residential parcel. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Emergency")
	bool bIsEmergencyAssemblyArea = false;

	/** Bu hücreyi işgal eden bina ID'si (boşsa -1). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
	int32 OccupyingBuildingID = -1;
};

/** Derived manifest only. Cells remain the sole canonical storage; no cell copy lives here. */
USTRUCT(BlueprintType)
struct JAMPROJECT_API FAftershockTerrainChunkState
{
	GENERATED_BODY()
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FIntPoint ChunkCoord=FIntPoint::ZeroValue;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 CellCount=0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) bool bContainsAuthoredContinuation=false;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnAftershockTerrainStorageChanged,uint64);

/**
 * FDisasterOutcome — Bir binanın afet simülasyonu sonrasındaki hasar ve kayıp bilançosu.
 */
USTRUCT(BlueprintType)
struct JAMPROJECT_API FDisasterOutcome
{
	GENERATED_BODY()

	/** Hesaplanan nihai toplam hasar (0 - 100). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Disaster")
	float FinalDamage = 0.0f;

	/** Hasar eşiklerine göre güncellenen durum. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Disaster")
	EDamageState NewDamageState = EDamageState::Intact;

	/** Yaşamını yitiren kişi sayısı. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Disaster")
	int32 Deaths = 0;

	/** Sağ kurtulan sakin ve çalışan sayısı. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Disaster")
	int32 Survivors = 0;

	/** Şehir memnuniyetine etki (-Deaths * 0.3). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Disaster")
	float SatisfactionChange = 0.0f;

	/** Devletten alınacak afet yardım ödeneği (Deaths * 500 TL). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Disaster")
	int32 StateAidNextTurn = 0;
};

/** Cooperative, GT-only candidate. Private staged cells cannot be substituted
 * by a caller. Live terrain remains untouched until one complete commit. */
class FAftershockTerrainChunkExpansion
{
public:
    bool IsReady() const { return bReady; }
    int32 GetPreparedCells() const { return PreparedCells.Num(); }
    int32 GetRequiredCells() const { return MissingCoords.Num(); }
private:
    friend class UTerrainGridSubsystem;
    TWeakObjectPtr<UTerrainGridSubsystem> Owner;
    uint64 ExpectedRevision=0;
    int32 OriginalCells=0,RequestedChunks=0;
    float SourceMinM=0.f,SourceMaxM=0.f;
    bool bReady=false,bCommitted=false;
    TArray<FIntPoint> MissingCoords;
    TArray<FGridCell> PreparedCells;
};

/**
 * UTerrainGridSubsystem — SIFIR TICK kuralına uygun, 2D Grid tabanlı arazi ve afet alt sistemi.
 */
UCLASS(BlueprintType)
class JAMPROJECT_API UTerrainGridSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
	friend class UDisasterSimulationSubsystem;

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	void CaptureSaveState(FAftershockTerrainSaveState& Out) const;
	bool ValidateSaveState(const FAftershockTerrainSaveState& State, FText& OutError) const;
	bool RestoreSaveState(const FAftershockTerrainSaveState& State, FText& OutError);
	static constexpr int32 ChunkSizeCells=25;
	static constexpr int32 MaximumResidentCells=65536;
	static constexpr int32 MaximumAbsGlobalCellCoord=1000000;
	static FIntPoint CellToChunk(FIntPoint GlobalCell);
	static FIntPoint CellToChunkLocal(FIntPoint GlobalCell);
	static void BuildChunkManifest(const TArray<FGridCell>& InCells,TArray<FAftershockTerrainChunkState>& Out);
	FIntPoint IndexToCoord(int32 StorageIndex) const;
	FIntRect GetResidentCellBounds() const;
	bool TryGetCell(FIntPoint GlobalCoord,FGridCell& Out) const;
	uint64 GetTerrainRevision() const { return TerrainRevision; }
	const TArray<FAftershockTerrainChunkState>& GetResidentChunks() const;
	/** Appends canonical cells; first expansion unlocks only the initial non-ocean placement mask. Existing IDs/physical state survive. */
	bool EnsureChunkGenerated(FIntPoint ChunkCoord,FText& OutError);
	/** Bounded cooperative source generation; one canonical commit, no partial append. */
	bool BeginChunkExpansion(TConstArrayView<FIntPoint> ChunkCoords,FAftershockTerrainChunkExpansion& Out,FText& OutError);
	bool ContinueChunkExpansion(FAftershockTerrainChunkExpansion& Candidate,int32 MaximumGeneratedCells,FText& OutError);
	bool CommitChunkExpansion(FAftershockTerrainChunkExpansion& Candidate,FText& OutError);
	/** Bulk restore has no callbacks; coordinator calls after every owner has imported. */
	void NotifyTerrainStorageRestored();
	FOnAftershockTerrainStorageChanged OnTerrainStorageChanged;

	// -------- GRID PARAMETRELERİ --------
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Terrain")
	int32 GridWidth = 50;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Terrain")
	int32 GridHeight = 50;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Terrain")
	float CellSize = 500.0f; // 500cm = 5m

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Terrain")
	FVector GridOrigin = FVector::ZeroVector;

	/** Arazinin prosedürel üretim seed değeri. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Terrain")
	int32 TerrainSeed = 1337;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Geography")
	int32 GeographyIndex = INDEX_NONE;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Geography")
	int32 GeographyFocusIndex = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Geography")
	bool bUsesRegionalLayout = false;
	/** 1 authored; 2 original65 DEM; 3 detailed257 DEM. Saved source versions never change implicitly. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Geography")
	int32 ContinuationGeneratorVersion=1;
	bool IsUsingRealGeography() const { return ContinuationGeneratorVersion==2||ContinuationGeneratorVersion==3; }
	bool TrySampleGeographicSource(double Latitude,double Longitude,FAftershockGeographicTerrainSample& Out) const;
	bool TryGetGeographicSourceBounds(FAftershockGeographicBounds& Out) const;
	/** Read-only shared canonical interpolation; never loads chunks or creates cells. */
	bool TrySampleResidentElevation(float GridX,float GridY,float& OutElevationCm) const;
	/** Uses resident interpolation, otherwise the selected source version. No invented outside height. */
	bool TrySampleWorldElevation(FVector WorldLocation,float& OutElevationCm) const;
	/** Source-covered cell centres only; exclusive maximum. Does not imply they are resident. */
	FIntRect GetSupportedGeographicCellBounds() const;
	bool TryWorldToGeographic(FVector World,double& OutLatitude,double& OutLongitude) const;
	bool TryGeographicToWorld(double Latitude,double Longitude,FVector& OutWorld) const;
	bool TryGeographicToCell(double Latitude,double Longitude,FIntPoint& OutCell) const;
	/** The first successful expansion unlocks only the initial non-ocean playable mask. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Terrain")
	bool bInitialMaskUnlocked = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Ecology")
	float ActiveDroughtSeverity = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Volcano")
	FIntPoint VolcanicVentCoord = FIntPoint(INDEX_NONE, INDEX_NONE);
	/** Apply before settlement; original four layouts retain their established authored terrain. */
	bool ApplyOfflineGeography(int32 ScenarioIndex, int32 FocusIndex=0, bool bUseRegionalLayout=false,int32 SourceVersion=3);
	/** Explicit unconfigured legacy fixture only; never a failed V2 source fallback. */
	bool ApplyLegacyOfflineGeography(int32 ScenarioIndex,int32 FocusIndex=0,bool bUseRegionalLayout=false);
	/** Local authored coast orientation, used for wave presentation, not intensity. */
	FVector GetCoastInlandDirection() const;
	void AdvanceEnvironmentalTurn();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Coast")
	int32 CoastStartRow = INDEX_NONE;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Coast")
	float CoastalSeaLevelCm = 24.f;

	/** Call after Generate and BEFORE settlement. South means increasing grid Y. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Coast")
	bool ConfigureCoastFromSouthernEdge(int32 InCoastStartRow);
	UFUNCTION(BlueprintPure, Category="Aftershock|Coast")
	bool IsCoastalWorld() const;

	/** Sole canonical stable append storage. Only the initial prefix is row-major. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Terrain")
	TArray<FGridCell> Cells;

	// -------- PROCEDURAL TERRAIN & UPGRADES --------
	/**
	 * Perlin Noise ile gerçekçi yükseklik haritası, kıvrımlı nehir yatağı,
	 * nehir çevresinde rüzgar koridorları ve tektonik fay hattı oluşturur.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Terrain")
	void GenerateProceduralTerrain(int32 Width, int32 Height, float InCellSize, int32 Seed);

	/**
	 * Nehir yatağını genişletme ve su akışını yavaşlatma yatırımlarını uygular.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Terrain")
	bool UpgradeRiverPrevention(FIntPoint Coords, float ExtraCapacity, float ExtraDamping);

	/**
	 * Sokak drenaj ve su tahliye kalitesini artırır.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Terrain")
	bool UpgradeCellDrainage(FIntPoint Coords, float BonusDrainage);

	/**
	 * Hücrenin parsel inşaat elverişlilik skorunu hesaplar (0.0 - 1.0).
	 */
	UFUNCTION(BlueprintPure, Category="Aftershock|Terrain")
	float CalculateParcelSuitability(FIntPoint Coords) const;

	/**
	 * BUILDING_STATS.md formüllerine ve hücre arazi çarpanlarına göre afet hasarını hesaplar.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Simulation")
	FDisasterOutcome EvaluateBuildingInDisaster(
		UPARAM(ref) FBuildingInstance& Building,
		EDisasterType DisasterType,
		float BaseDisasterSeverity,
		float CityAwarenessLevel,
		bool bIsDaytime,bool bHazardAlreadyLocalized=false);

	// -------- GRID YARDIMCI FONKSİYONLARI --------
	UFUNCTION(BlueprintPure, Category="Aftershock|Terrain")
	bool IsValidCoord(FIntPoint Coords) const;

	UFUNCTION(BlueprintPure, Category="Aftershock|Terrain")
	bool IsPlayableCoord(FIntPoint Coords) const
	{
		return IsValidCoord(Coords) && Cells.IsValidIndex(CoordToIndex(Coords)) && Cells[CoordToIndex(Coords)].bIsPlayable;
	}

	UFUNCTION(BlueprintPure, Category="Aftershock|Terrain")
	FGridCell GetCell(FIntPoint Coords) const;

	UFUNCTION(BlueprintCallable, Category="Aftershock|Terrain")
	bool SetCell(FIntPoint Coords, const FGridCell& NewCell);

	UFUNCTION(BlueprintPure, Category="Aftershock|Terrain")
	FIntPoint WorldToGrid(FVector WorldLocation) const;

	UFUNCTION(BlueprintPure, Category="Aftershock|Terrain")
	FVector GridToWorld(FIntPoint GridCoords, bool bCenter = true) const;

	int32 CoordToIndex(int32 X,int32 Y) const { return CoordToIndex(FIntPoint(X,Y)); }
	/** Resident storage ID, never X+Y*Width outside the original frame. Missing -> INDEX_NONE. */
	int32 CoordToIndex(FIntPoint Coords) const;

private:
	bool TryGenerateGeographicCell(FIntPoint Coord,FGridCell& OutCell) const;
	void EnsureStorageLookup() const;
	void RebuildStorageLookup() const;
	FGridCell GenerateContinuationCell(FIntPoint Coord,float SourceMinM,float SourceMaxM) const;
	void MarkTerrainEdited() { ++TerrainRevision; }
	uint64 TerrainRevision=0;
	mutable TMap<FIntPoint,int32> CellStorageIndices;
	mutable TArray<FAftershockTerrainChunkState> ResidentChunks;
	mutable FIntRect ResidentCellBounds=FIntRect(0,0,0,0);
	mutable int32 CachedStorageCount=INDEX_NONE;
	mutable FIntPoint CachedInitialSize=FIntPoint::ZeroValue;
};
