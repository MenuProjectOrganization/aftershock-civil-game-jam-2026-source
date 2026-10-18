// AFTERSHOCK — Civil Game Jam 2026
// Optimize Bina Render ve İnşaat Yöneticisi Alt Sistemi

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/EngineTypes.h"
#include "Engine/TimerHandle.h"
#include "Aftershock/Public/BuildingData.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "BuildingManagerSubsystem.generated.h"

class UInstancedStaticMeshComponent;

struct FAftershockBuildingsSaveState;
struct FAftershockArchitecturalInput;
struct FAftershockArchitectureWork;
struct FAftershockArchitectureAudit;
class FAftershockArchitecturalRenderer;

/** A read-only access quote. The final cell is an existing, usable public road;
 * earlier cells are the paid extension. The building plot is never a road. */
USTRUCT(BlueprintType)
struct JAMPROJECT_API FAftershockBuildingRoadAccessQuote
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Aftershock|Construction") bool bRouteValid=false;
	UPROPERTY(BlueprintReadOnly, Category="Aftershock|Construction") TArray<FIntPoint> RoadPath;
	UPROPERTY(BlueprintReadOnly, Category="Aftershock|Construction") int32 NewRoadCells=0;
	UPROPERTY(BlueprintReadOnly, Category="Aftershock|Construction") int32 RoadCostTL=0;
	UPROPERTY(BlueprintReadOnly, Category="Aftershock|Construction") FText Feedback;
};

/** World-lifetime scalar counters only; no per-event history or retained records. */
struct FAftershockBuildingPresentationWork
{
	uint64 DecorationRequests=0,DecorationFlushes=0,DamagedRebuilds=0,MemorialRebuilds=0;
	uint64 DamagedBuildingsVisited=0,MemorialBuildingsVisited=0,ViewRebuilds=0;
};

/**
 * UBuildingManagerSubsystem — SIFIR TICK ve ISM tabanlı yüksek performanslı bina inşaat ve render yöneticisi.
 * Tekil AActor spawn etmez, tüm binaları UInstancedStaticMeshComponent üzerinde Custom Primitive Data ile çizer.
 */
UCLASS(BlueprintType)
class JAMPROJECT_API UBuildingManagerSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	void CaptureSaveState(FAftershockBuildingsSaveState& Out) const;
	bool RestoreSaveState(const FAftershockBuildingsSaveState& State,FText& OutError);
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Shared presentation-only soft reference mapping for authored collapse assets. */
	static TSoftObjectPtr<UGeometryCollection> GetDefaultFracturedMesh(EBuildingType Type);

	/** One shared definition per building type, reused by startup and the HUD catalog. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Catalog")
	UBuildingData* GetCatalogData(EBuildingType Type);
	/** Visual consumer only: city life owns power and operational allocation. */
	void SetBuildingOperationalVisual(int32 BuildingID, float PowerCoverage01, float Operation01);

	// -------- İNŞAAT VE MALİYET METOTLARI --------
	/**
	 * BUILDING_STATS.md kurallarına göre temel kalitesi ve ekstra kat çarpanlarıyla toplam inşaat maliyetini hesaplar.
	 */
	UFUNCTION(BlueprintPure, Category="Aftershock|Construction")
	int32 CalculateConstructionCost(const UBuildingData* Data, EFoundationQuality Foundation, int32 Floors) const;

	/**
	 * Hücrenin geçerli, boş ve nehir yatağı dışında olup olmadığını denetler.
	 */
	UFUNCTION(BlueprintPure, Category="Aftershock|Construction")
	bool CanPlaceBuildingAt(FIntPoint GridCoords, const UBuildingData* Data) const;

	/** Deterministic shortest valid frontage-to-road connection on resident land.
	 * Does not create terrain, bridges, road cells or reservations during preview. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Construction")
	FAftershockBuildingRoadAccessQuote QuoteAutomaticRoadAccess(FIntPoint GridCoords, const UBuildingData* Data) const;

	/** Player purchase: one aggregate building + access-road debit, after both
	 * canonical objects exist. Failed quotes and unaffordable purchases change neither. */
	bool PlaceBuildingWithRoadAccess(FIntPoint GridCoords, UBuildingData* Data,
		EFoundationQuality Foundation,int32 Floors,int32& OutAssignedID);

	/**
	 * Geçerli bir hücreye bina yerleştirir, ISM instance'ı oluşturur ve FBuildingInstance kaydını tutar.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Construction")
	bool PlaceBuilding(
		FIntPoint GridCoords,
		UBuildingData* Data,
		EFoundationQuality Foundation,
		int32 Floors,
		int32& OutAssignedID);

	/**
	 * BUILDING_STATS.md Bölüm 4: Binayı sismik olarak güçlendirir (1000 TL), yıpranmayı sıfırlar ve ISM verisini günceller.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Maintenance")
	bool RetrofitBuilding(int32 BuildingID, int32& OutCostTL);

	/**
	 * BUILDING_STATS.md Bölüm 5: Hasarlı binaları onarır (Damaged: 2000 TL, Severe: 5000 TL) ve görsel hasarı sıfırlar.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Maintenance")
	bool RepairBuilding(int32 BuildingID, int32& OutCostTL);

	/**
	 * Afet sonrasında binanın hasar durumunu GPU Custom Primitive Data ile günceller.
	 * Çöken (Collapsed) binaların ölçeğini ZeroVector yaparak gizler (ISM index kaymasını önler).
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Visual")
	void UpdateBuildingVisualState(int32 BuildingID);

	/**
	 * Binaya hasar uygular ve görsel durumunu (CPD) günceller.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Simulation")
	void ApplyDamageToBuilding(int32 BuildingID, float NewDamage);

	// -------- SORGULAR --------
	UFUNCTION(BlueprintPure, Category="Aftershock|Buildings")
	bool GetBuildingByID(int32 BuildingID, FBuildingInstance& OutBuilding) const;

	UFUNCTION(BlueprintPure, Category="Aftershock|Buildings")
	bool GetBuildingAtCoords(FIntPoint GridCoords, FBuildingInstance& OutBuilding) const;
	/** Click-only OBB picking against the rendered instance, independent of imported collision. */
	bool RaycastBuilding(const FVector& Origin, const FVector& Direction, float MaxDistance,
		FBuildingInstance& OutBuilding, float& OutDistance) const;
	/** Shared by placement and ghost preview. Existing saves retain their recorded transforms. */
	FTransform GetBuildingVisualTransform(FIntPoint Coord, UBuildingData* Data, int32 Floors, float Yaw = 0.f) const;
	/** Read-only descriptors: saved envelope/IDs/four canonical CPD channels remain authoritative. */
	bool GetCanonicalArchitecturalInput(int32 BuildingID,FAftershockArchitecturalInput& Out) const;
	bool GetArchitecturalPlacementInput(FIntPoint Coord,UBuildingData* Data,int32 Floors,float Yaw,FAftershockArchitecturalInput& Out) const;
	const FAftershockArchitectureWork* GetArchitecturalPresentationWork() const;
	/** Compact, read-only real CPD bindings; definition in private audit header. */
	bool ReadArchitecturalCPDAudit(FAftershockArchitectureAudit& Out) const;

	UFUNCTION(BlueprintPure, Category="Aftershock|Buildings")
	void GetAllBuildings(TArray<FBuildingInstance>& OutBuildings) const;

	UFUNCTION(BlueprintPure, Category="Aftershock|Buildings")
	int32 GetActiveBuildingsCount() const { return ActiveBuildings.Num(); }

	/** Mevcut bir binanın durumunu günceller. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Buildings")
	bool UpdateBuildingInstance(const FBuildingInstance& UpdatedBuilding);

	/** Bir binayı kayıtlardan kaldırır ve ISM görselini gizler. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Buildings")
	bool RemoveBuilding(int32 BuildingID);
	bool BeginMemorial(int32 BuildingID,int32 CurrentTurn);
	void AdvanceMemorialTurn(int32 CurrentTurn);
	void AdvanceMemorialsForTurn(int32 CurrentTurn) {AdvanceMemorialTurn(CurrentTurn);}
	int32 GetMemorialTurnsRemaining(int32 BuildingID) const;
	TArray<int32> GetMemorialCasualtyIDs(int32 BuildingID) const;
	void RebuildMemorialVisuals();
	/** Barrier for load/native fixtures; normal changes share one next-tick decoration flush. */
	void FlushPendingPresentationUpdates();
	/** Bulk restore changes membership independently of individual damage updates. */
	void InvalidateBuildingPresentation() { bBuildingPresentationDirty=true;bArchitecturalLayoutDirty=true;FreeBuildingInstanceIndices.Reset();QueueDecorationRefresh(true); }
	FAftershockBuildingPresentationWork GetPresentationWork() const { return PresentationWork; }
	/** Derived render batches only. Canonical ISM transforms/indices/custom data remain untouched. */
	void RefreshVisibleBuildingPresentation(bool bForce=false);
	bool IsBuildingInPresentationWindow(int32 BuildingID) const;
	bool GetPresentedBuildingTransform(int32 BuildingID,FTransform& OutTransform) const;
	int32 GetPresentedBuildingCount() const;
	FIntRect GetBuildingPresentationBounds() const { return BuildingPresentationBounds; }
	bool UsesDerivedBuildingPresentation() const { return bUseDerivedBuildingPresentation; }

	/** Tüm binaların yaşını 1 yıl artırır (10 turda bir çağrılır). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Buildings")
	void AgeAllBuildingsOneYear();

	/** Belirli bir bina tipinin ISM bileşenini döndürür veya dinamik olarak oluşturur. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Visual")
	UInstancedStaticMeshComponent* GetOrCreateISM(UBuildingData* Data);

	/**
	 * Normal oyun açılışında 0 bina ve 0 nüfus sorununu çözer;
	 * haritanın merkezindeki arazi hücrelerine başlangıç yerleşimini (25-30 bina) kurar
	 * ve UCrowdRenderSubsystem'i uyandırarak başlangıç vatandaşlarını sahnede üretir.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|City")
	void SeedInitialSettlement();

protected:
	/** ISM bileşenlerini barındıran tekil dünya aktörü. */
	UPROPERTY()
	TObjectPtr<AActor> ISMHostActor;

	/** Her farklı bina tipi için ISM bileşeni haritası. */
	UPROPERTY()
	TMap<TObjectPtr<UBuildingData>, TObjectPtr<UInstancedStaticMeshComponent>> BuildingISMs;
	/** Removed slots only. Existing active canonical indices are never moved. */
	TMap<TWeakObjectPtr<UBuildingData>,TArray<int32>> FreeBuildingInstanceIndices;
	UPROPERTY(Transient)
	TMap<EBuildingType, TObjectPtr<UBuildingData>> CatalogDefinitions;
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> MemorialCandles;
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> MemorialFlowers;
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> DamagedBuildingSmokeISM;
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> DamagedBuildingDebrisISM;
	void RebuildDamagedBuildingVisuals();
	void QueueDecorationRefresh(bool bMemorial=false);
	FTimerHandle DecorationRefreshTimer;
	bool bDamageDecorationsDirty=false,bMemorialDecorationsDirty=false,bFlushingDecorations=false;
	bool bDrawBuildingDecorations=true;
	FAftershockBuildingPresentationWork PresentationWork;
	bool bAdvancingMemorials=false;
	UPROPERTY(Transient) TMap<TObjectPtr<UBuildingData>,TObjectPtr<UInstancedStaticMeshComponent>> PresentationISMs;
	UPROPERTY(Transient) TMap<int32,TObjectPtr<UInstancedStaticMeshComponent>> PresentationComponentByID;
	TMap<int32,int32> PresentationIndexByID;
	TMap<TWeakObjectPtr<UInstancedStaticMeshComponent>,ECollisionResponse> CanonicalVisibilityResponses;
	FIntRect BuildingPresentationBounds=FIntRect(0,0,0,0);
	bool bUseDerivedBuildingPresentation=false;
	bool bBuildingPresentationDirty=true;
	bool bRefreshingBuildingPresentation=false;
	bool SyncPresentedBuilding(int32 BuildingID,bool bMarkRenderStateDirty=true);
	// Derived visual cache only: never serialized or consulted by hazard/economy.
	TSharedPtr<FAftershockArchitecturalRenderer> ArchitecturalRenderer;
	bool bArchitecturalLayoutDirty=true;
	void RefreshArchitecturalPresentation(bool bForce=false);
	void SyncArchitecturalBuilding(int32 BuildingID);

	/** Aktif binaların durum kayıtları (InstanceID -> FBuildingInstance). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Buildings")
	TMap<int32, FBuildingInstance> ActiveBuildings;

	/** Bir sonraki bina için benzersiz kimlik. */
	UPROPERTY()
	int32 NextBuildingID = 1;
	/** No city-life callbacks may observe a half-built access-road purchase. */
	bool bConstructionTransactionInProgress=false;

	/** ISM barındırıcı aktörü oluşturur veya döndürür. */
	AActor* GetOrCreateISMHostActor();
};
