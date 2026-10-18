// AFTERSHOCK — Civil Game Jam 2026
// Saf statik hesap fonksiyonları. DISASTER_PHYSICS.md + BUILDING_STATS.md.
// Hiçbir UObject state tutmaz, test edilmesi kolay.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Aftershock/Types/AftershockTypes.h"
#include "DisasterMath.generated.h"

class UBuildingData;

/** Bir binanın anlık durumu — hesap için snapshot. */
USTRUCT(BlueprintType)
struct FBuildingSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite) EBuildingType Type = EBuildingType::House;
	UPROPERTY(BlueprintReadWrite) EMaterialType Material = EMaterialType::Wood;
	UPROPERTY(BlueprintReadWrite) EFoundationQuality Foundation = EFoundationQuality::Medium;
	UPROPERTY(BlueprintReadWrite) int32 Floors = 1;
	UPROPERTY(BlueprintReadWrite) int32 AgeYears = 0;
	UPROPERTY(BlueprintReadWrite) bool bRetrofitted = false;
	UPROPERTY(BlueprintReadWrite) float BaseResistance = 0.3f;
	UPROPERTY(BlueprintReadWrite) int32 Residents = 0;
	UPROPERTY(BlueprintReadWrite) int32 Workers = 0;
	UPROPERTY(BlueprintReadWrite) FVector WorldLocation = FVector::ZeroVector;
	UPROPERTY(BlueprintReadWrite) FIntPoint GridCoord = FIntPoint::ZeroValue;
};

/** Şehir çapında önleme çarpanları — hesapta kullanılır. */
USTRUCT(BlueprintType)
struct FCityPreventionState
{
	GENERATED_BODY()

	/** 0.0 (hiç farkındalık yok) — 1.0 (herkes eğitimli). */
	UPROPERTY(BlueprintReadWrite, meta=(ClampMin="0.0", ClampMax="1.0"))
	float AwarenessLevel = 0.f;

	/** 0.0 (drenaj yok) — 1.0 (full drenaj). */
	UPROPERTY(BlueprintReadWrite, meta=(ClampMin="0.0", ClampMax="1.0"))
	float DrainageCapacity = 0.f;

	/** 0.0 (siren yok) — 1.0 (full erken uyarı). */
	UPROPERTY(BlueprintReadWrite, meta=(ClampMin="0.0", ClampMax="1.0"))
	float EarlyWarning = 0.f;

	/** 0.0 — 1.0, acil müdahale birimi kapasitesi. */
	UPROPERTY(BlueprintReadWrite, meta=(ClampMin="0.0", ClampMax="1.0"))
	float EmergencyCapacity = 0.f;

	/** Legacy planning metadata; does not modify an already generated hazard or damage. */
	UPROPERTY(BlueprintReadWrite, meta=(ClampMin="0.0", ClampMax="100.0"))
	float EnvironmentScore = 50.f;
};

UCLASS()
class JAMPROJECT_API UDisasterMath : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Malzeme × afet tipi → dayanıklılık (0..1). BUILDING_STATS.md Bölüm 2. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Math")
	static float MaterialResistance(EMaterialType Material, EDisasterType DisasterType);

	/** Temel kalitesinin deprem/sel bonusu. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Math")
	static float FoundationBonus(EFoundationQuality Quality, EDisasterType DisasterType);

	/** Toplam direnç skoru (0.05 — 0.98). */
	UFUNCTION(BlueprintPure, Category="Aftershock|Math")
	static float TotalResistance(const FBuildingSnapshot& Snap, EDisasterType DisasterType);

	/** Legacy scenario-planning helper only; never applied to an instantaneous hazard/damage. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Math")
	static float ClimateMultiplier(float EnvironmentScore, float MaxMultiplier = 1.5f);

	/** Legacy magnitude3..8.5 maps to authored0..100 shaking; radius uses world cm, not a GMPE. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Math")
	static float EarthquakeSeverityAt(const FDisasterEvent& Event, FVector At, float FaultLineProximity);

	/** Sel: su derinliği + drenaj → şiddet. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Math")
	static float FloodSeverityAt(float WaterDepthMeters, float DrainageCapacity);

	/** km/h->m/s, q=.5*rho*v^2, coarse height exposure; authored0..100, not structural design. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Math")
	static float WindSeverityAt(float WindSpeedKmh, float ValleyChannelFactor, int32 Floors);

	/** Yangın yanma süresi → şiddet. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Math")
	static float FireSeverityAt(int32 BurningTurns);

	/** Hasar hesabı — tüm formüllerin birleştirildiği merkezi fonksiyon. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Math")
	static FDamageResult ComputeDamage(
		const FBuildingSnapshot& Building,
		const FDisasterEvent& Disaster,
		const FCityPreventionState& City,
		const FTerrainCell& Cell,
		float RawSeverity);

	/** Ölüm katsayısı — hasar seviyesine göre. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Math")
	static float DeathCoefficient(float Damage);

	/** Memnuniyet değişimi (negatif değer = düşüş). */
	UFUNCTION(BlueprintPure, Category="Aftershock|Math")
	static float SatisfactionDeltaFromDeaths(int32 Deaths);

	/** Deprecated compatibility: deaths never generate a government reward. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Math")
	static int32 GovernmentAidFromDeaths(int32 Deaths);
	UFUNCTION(BlueprintPure,Category="Aftershock|Math") static int64 GovernmentAidFromSurvivors(int32 Survivors);
	UFUNCTION(BlueprintPure,Category="Aftershock|Math") static int64 BurialCostFromDeaths(int32 Deaths);
};
