// AFTERSHOCK — Civil Game Jam 2026
// Bina Veri Yapıları, DataAsset ve Hafif Instance Tanımları

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "BuildingData.generated.h"

class UStaticMesh;
class UGeometryCollection;
class UTexture2D;

#ifndef AFTERSHOCK_API
#define AFTERSHOCK_API JAMPROJECT_API
#endif

/** Oyundaki bina tipleri. BUILDING_STATS.md Bölüm 1 ve Bölüm 9 ile eşlenir. */
UENUM(BlueprintType)
enum class EBuildingType : uint8
{
	House = 0        UMETA(DisplayName = "Küçük Ev"),
	Apartment = 1    UMETA(DisplayName = "Apartman"),
	Office = 2       UMETA(DisplayName = "Ofis"),
	Factory = 3      UMETA(DisplayName = "Fabrika"),
	Hospital = 4     UMETA(DisplayName = "Hastane"),
	School = 5       UMETA(DisplayName = "Okul"),
	FireStation = 6  UMETA(DisplayName = "İtfaiye İstasyonu"),
	PowerPlant = 7   UMETA(DisplayName = "Enerji Santrali"),
	GreenSpace = 8   UMETA(DisplayName = "Yeşil Alan"),
	WindTurbine = 9  UMETA(DisplayName = "Rüzgar Türbini"),
	// Geriye dönük uyumluluk takma adları
	Park = GreenSpace UMETA(Hidden),
	Fire = FireStation UMETA(Hidden),
	Shelter = 10     UMETA(DisplayName = "Sığınak"),
	Farm = 11        UMETA(DisplayName = "Tarla / Çiftlik"),
	LivestockFarm = 12 UMETA(DisplayName = "Hayvancılık"),
	Generator = 13   UMETA(DisplayName = "Jeneratör"),
	Bakery = 14      UMETA(DisplayName = "Fırın"),
	Restaurant = 15  UMETA(DisplayName = "Restoran"),
	TaxOffice = 16   UMETA(DisplayName = "Vergi Dairesi"),
	Courthouse = 17  UMETA(DisplayName = "Adliye"),
	PoliceStation = 18 UMETA(DisplayName = "Polis Merkezi"),
	BusTerminal = 19 UMETA(DisplayName = "Otobüs Terminali"),
	BusStop = 20     UMETA(DisplayName = "Otobüs Durağı"),
	WaterPlant = 21  UMETA(DisplayName = "Su Arıtma Tesisi"),
	Warehouse = 22   UMETA(DisplayName = "Depo"),
	Substation = 23  UMETA(DisplayName = "Trafo Merkezi")
};

/** Yapı malzemesi. BUILDING_STATS.md Bölüm 2. */
UENUM(BlueprintType)
enum class EMaterialType : uint8
{
	Wood      UMETA(DisplayName = "Ahşap"),
	Prefab    UMETA(DisplayName = "Prefabrik"),
	Concrete  UMETA(DisplayName = "Betonarme"),
	Steel     UMETA(DisplayName = "Çelik"),
	Brick     UMETA(DisplayName = "Tuğla")
};

// Geriye dönük kod uyumluluk alias'ı
using EMaterial = EMaterialType;

/** Afet türü. BUILDING_STATS.md & DISASTER_PHYSICS.md. */
UENUM(BlueprintType)
enum class EDisasterType : uint8
{
	Earthquake = 0 UMETA(DisplayName = "Deprem"),
	Flood = 1      UMETA(DisplayName = "Sel"),
	Wind = 2       UMETA(DisplayName = "Fırtına"),
	Fire = 3       UMETA(DisplayName = "Yangın"),
	Tsunami = 4    UMETA(DisplayName = "Tsunami"),
	Volcano = 5    UMETA(DisplayName = "Volkan"),
	Drought = 6    UMETA(DisplayName = "Kuraklık"),
	Avalanche = 7  UMETA(DisplayName = "Çığ"),
	Landslide = 8  UMETA(DisplayName = "Heyelan")
};

inline constexpr int32 AftershockDisasterTypeCount=9;

/** Temel kalitesi. BUILDING_STATS.md Bölüm 3. */
UENUM(BlueprintType)
enum class EFoundationQuality : uint8
{
	Weak    UMETA(DisplayName = "Zayıf (Maliyet x1.0, Deprem 0.0, Sel -0.1)"),
	Medium  UMETA(DisplayName = "Orta (Maliyet x1.3, Deprem +0.1, Sel 0.0)"),
	Strong  UMETA(DisplayName = "Güçlü (Maliyet x1.8, Deprem +0.3, Sel +0.2)")
};

/** Hasar Eşikleri. BUILDING_STATS.md Bölüm 5 ve baseline.md. */
UENUM(BlueprintType)
enum class EDamageState : uint8
{
	Intact    = 0  UMETA(DisplayName = "Hasarsız (0)"),
	Cracked   = 1  UMETA(DisplayName = "Çatlak (1-20)"),
	Damaged   = 2  UMETA(DisplayName = "Hasarlı / Kapalı (20-50)"),
	Severe    = 3  UMETA(DisplayName = "Ağır Hasarlı (50-80)"),
	Collapsed = 4  UMETA(DisplayName = "Çöktü / Chaos (80-100)"),
	Memorial = 5 UMETA(DisplayName = "Anma Alanı (3 Tur)")
};

/** Bina hasar aşamaları. Hasar eşik yönetimi için geriye dönük uyumluluk enum'u. */
UENUM(BlueprintType)
enum class EBuildingDamageState : uint8
{
	Intact       = 0  UMETA(DisplayName = "Hasarsız (0)"),
	Cracked      = 1  UMETA(DisplayName = "Çatlak (0-20)"),
	Damaged      = 2  UMETA(DisplayName = "Hasarlı (20-50)"),
	HeavyDamage  = 3  UMETA(DisplayName = "Ağır Hasar (50-80)"),
	Collapsed    = 4  UMETA(DisplayName = "Çökme (80-100)")
};

/**
 * UBuildingData — Her bina tipi için stat ve referansları tutan DataAsset.
 */
UCLASS(BlueprintType)
class JAMPROJECT_API UBuildingData : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Basic")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Basic")
	EBuildingType Type = EBuildingType::House;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Basic", meta=(MultiLine="true"))
	FText Description;

	// -------- EKONOMİ --------
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Economy", meta=(ClampMin="0"))
	int32 CostTL = 1000;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Economy")
	int32 TaxPerTurn = 50;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Economy")
	int32 MaintenancePerTurn = 10;

	// -------- DEMOGRAFİ --------
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Demographics", meta=(ClampMin="0"))
	int32 Residents = 4;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Demographics", meta=(ClampMin="0"))
	int32 Workers = 0;

	/** Authored gameplay units. UCityLifeSubsystem owns allocation and outcomes. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="CityLife", meta=(ClampMin="0"))
	float PowerProductionUnitsPerTurn = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="CityLife", meta=(ClampMin="0"))
	float PowerDemandUnitsPerTurn = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="CityLife", meta=(ClampMin="0"))
	float FoodProductionUnitsPerTurn = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="CityLife", meta=(ClampMin="0"))
	float FoodProcessingInputUnitsPerTurn = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="CityLife", meta=(ClampMin="0"))
	int32 FoodServiceCapacity = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="CityLife", meta=(ClampMin="0"))
	int32 ServiceRadiusCells = 5;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="CityLife", meta=(ClampMin="0"))
	int32 TransitCapacity = 0;

	// -------- FİZİKSEL --------
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Physical", meta=(ClampMin="1"))
	int32 FloorsMin = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Physical", meta=(ClampMin="1"))
	int32 FloorsMax = 2;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Physical")
	EMaterialType Material = EMaterialType::Wood;

	// -------- DİRENÇ --------
	/** Afet tipi -> dayanıklılık (0.0–1.0). BUILDING_STATS.md Bölüm 2. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Resistance")
	TMap<EDisasterType, float> BaseResistance;

	// -------- ÇEVRE --------
	/** Fabrika pozitif (+CO2), yeşil alan negatif (-CO2). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Environment")
	float CO2PerTurn = 0.0f;

	// -------- MEMNUNİYET VE HİZMET --------
	/** Hastane/park gibi hizmet binaları pozitif katkıda bulunur (Hastane: +2.0). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Satisfaction")
	float SatisfactionPerTurn = 0.0f;

	/** Afet sonrası kurtarma hız çarpanı (Hastane için 2.0x, standart 1.0x). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Service", meta=(ClampMin="1.0"))
	float RescueSpeedMultiplier = 1.0f;

	// -------- GRID BOYUTU --------
	/** Grid ayak izi hücre boyutu (X, Y). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Grid", meta=(ClampMin="1"))
	FIntPoint FootprintCells = FIntPoint(1, 1);

	// -------- GÖRSEL --------
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Visual")
	TSoftObjectPtr<UStaticMesh> MeshIntact;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Visual")
	TSoftObjectPtr<UGeometryCollection> MeshFractured;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Visual")
	TSoftObjectPtr<UTexture2D> IconThumbnail;

	/** Bir hücredeki ortalama kat sayısı */
	UFUNCTION(BlueprintPure, Category="Building")
	int32 AverageFloors() const { return (FloorsMin + FloorsMax) / 2; }

	/** Afet tipine göre temel direnç değerini döndürür */
	UFUNCTION(BlueprintPure, Category="Building")
	float GetBaseResistance(EDisasterType DisasterType) const
	{
		// Coastal surge shares the canonical flood susceptibility table. This is
		// a deliberately simplified gameplay mapping, not a structural wave model.
		if (DisasterType == EDisasterType::Tsunami) DisasterType = EDisasterType::Flood;
		if (const float* Found = BaseResistance.Find(DisasterType))
		{
			return *Found;
		}
		return GetDefaultMaterialResistance(Material, DisasterType);
	}

	/** BUILDING_STATS.md Bölüm 2 tablosuna göre varsayılan malzeme direnci */
	UFUNCTION(BlueprintPure, Category="Building")
	static float GetDefaultMaterialResistance(EMaterialType InMaterial, EDisasterType DisasterType)
	{
		if (DisasterType == EDisasterType::Tsunami) DisasterType = EDisasterType::Flood;
		switch (InMaterial)
		{
		case EMaterialType::Wood:
			switch (DisasterType)
			{
			case EDisasterType::Earthquake: return 0.3f;
			case EDisasterType::Flood:      return 0.2f;
			case EDisasterType::Wind:       return 0.4f;
			case EDisasterType::Fire:       return 0.1f;
			case EDisasterType::Tsunami:    return 0.15f;
			}
			break;
		case EMaterialType::Prefab:
			switch (DisasterType)
			{
			case EDisasterType::Earthquake: return 0.5f;
			case EDisasterType::Flood:      return 0.4f;
			case EDisasterType::Wind:       return 0.5f;
			case EDisasterType::Fire:       return 0.6f;
			case EDisasterType::Tsunami:    return 0.35f;
			}
			break;
		case EMaterialType::Concrete:
			switch (DisasterType)
			{
			case EDisasterType::Earthquake: return 0.8f;
			case EDisasterType::Flood:      return 0.9f;
			case EDisasterType::Wind:       return 0.8f;
			case EDisasterType::Fire:       return 0.9f;
			case EDisasterType::Tsunami:    return 0.85f;
			}
			break;
		case EMaterialType::Steel:
			switch (DisasterType)
			{
			case EDisasterType::Earthquake: return 0.9f;
			case EDisasterType::Flood:      return 0.7f;
			case EDisasterType::Wind:       return 0.6f;
			case EDisasterType::Fire:       return 0.8f;
			case EDisasterType::Tsunami:    return 0.70f;
			}
			break;
		case EMaterialType::Brick:
			switch (DisasterType)
			{
			case EDisasterType::Earthquake: return 0.6f;
			case EDisasterType::Flood:      return 0.7f;
			case EDisasterType::Wind:       return 0.7f;
			case EDisasterType::Fire:       return 0.8f;
			case EDisasterType::Tsunami:    return 0.55f;
			}
			break;
		}
		return 0.5f;
	}
};

/**
 * FBuildingInstance — Sahnedeki her bir binanın durumunu tutan hafif, zero-tick uyumlu yapı.
 */
USTRUCT(BlueprintType)
struct JAMPROJECT_API FBuildingInstance
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building")
	int32 InstanceID = -1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building")
	FIntPoint GridCoords = FIntPoint::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building")
	TObjectPtr<UBuildingData> BuildingData = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building", meta=(ClampMin="1"))
	int32 CurrentFloors = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building")
	EFoundationQuality Foundation = EFoundationQuality::Medium;

	/** Temel kalitesi (Foundation ile eşdeğer takma ad / oyuncu tercihi). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building")
	EFoundationQuality FoundationQuality = EFoundationQuality::Medium;

	/** Yapısal inşaat işçiliği kalitesi (0.5 - 1.5, default 1.0). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building", meta=(ClampMin="0.5", ClampMax="2.0"))
	float StructuralQuality = 1.0f;

	/** Çatı sistemi kalitesi ve rüzgar/su izolasyonu (0.5 - 1.5, default 1.0). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building", meta=(ClampMin="0.5", ClampMax="2.0"))
	float RoofQuality = 1.0f;

	/** Bina afet hazırlık ve bilinç düzeyi (0.0 - 1.0). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building", meta=(ClampMin="0.0", ClampMax="1.0"))
	float Preparedness = 0.0f;

	/**
	 * NOT: Drenaj kalitesi (DrainageQuality) bina bazında mükerrer olarak tutulmaz;
	 * tekil ve yetkili (authoritative) veri sahibi UTerrainGridSubsystem / FGridCell'dir.
	 */

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building", meta=(ClampMin="0"))
	int32 AgeInYears = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building")
	bool bIsRetrofitted = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building", meta=(ClampMin="0.0", ClampMax="100.0"))
	float CurrentDamage = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building")
	EDamageState DamageState = EDamageState::Intact;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Building")
	int32 ISMInstanceIndex = -1;
	/** Real economy turns; the parcel stays occupied until this turn. */
	UPROPERTY(BlueprintReadOnly,Category="Building|Memorial") int32 MemorialStartedTurn=0;
	UPROPERTY(BlueprintReadOnly,Category="Building|Memorial") int32 MemorialUnlockTurn=0;
	UPROPERTY(BlueprintReadOnly,Category="Building|Memorial") TArray<int32> MemorialCasualtyResidentIDs;
};
