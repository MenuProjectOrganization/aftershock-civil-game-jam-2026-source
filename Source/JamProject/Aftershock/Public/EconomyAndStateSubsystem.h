// AFTERSHOCK — Civil Game Jam 2026
// Ekonomi, Vergi, İklim/CO2 Döngüsü, Dinamik Ödüller ve Oyun Durumu Alt Sistemi

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Aftershock/Public/BuildingData.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "EconomyAndStateSubsystem.generated.h"

struct FAftershockEconomySaveState;

/**
 * Oyunun sona erme (Game Over) sebepleri.
 */
UENUM(BlueprintType)
enum class EGameOverReason : uint8
{
	DismissedByPublic UMETA(DisplayName = "Halk Tarafından Görevden Alındı (Memnuniyet Sıfır)"),
	Bankruptcy        UMETA(DisplayName = "İflas (Hazine İflası)")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnEconomyUpdatedSignature, int64, TreasuryTL, float, Satisfaction, int32, CurrentTurn);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnClimateUpdatedSignature, float, CO2Level, float, ClimateRiskMultiplier);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameOverSignature, FText, ReasonMessage);

/**
 * UEconomyAndStateSubsystem — Şehrin hazine bütçesini, vergi döngüsünü, CO2/iklim riskini ve yöneticilik durumunu yönetir.
 */
UCLASS(BlueprintType)
class JAMPROJECT_API UEconomyAndStateSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	bool CanAfford(int64 Amount) const;
	void CaptureSaveState(FAftershockEconomySaveState& Out) const;
	bool RestoreSaveState(const FAftershockEconomySaveState& State,FText& OutError);
	void CaptureAppliedDisasterEvents(TArray<FGuid>& Out) const;
	bool RestoreAppliedDisasterEvents(const TArray<FGuid>& Events,FText& OutError);
	/** Bounded dimensionless local pressure, not tonnes, temperature or hazard physics. */
	static float CalculateLocalClimateRisk(float AccumulatedCO2,int32 LivingPopulation,int32 BuiltLots,int32 AvailableLandCells);
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// -------- EVENT DISPATCHER'LAR --------
	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnEconomyUpdatedSignature OnEconomyUpdated;

	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnClimateUpdatedSignature OnClimateUpdated;

	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnGameOverSignature OnGameOver;

	// -------- METRİKLER VE OYUN DURUMU --------
	/** Mevcut tur sayısı (1'den başlar). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Economy")
	int32 CurrentTurn = 1;

	/** Mevcut simülasyon yılı (2026'dan başlar, her 10 turda +1 yıl). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Economy")
	int32 CurrentYear = 2026;

	/** Şehir hazinesindeki bütçe (Başlangıç: 50.000 TL). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Economy")
	int64 TreasuryTL = 50000;

	/** Halk memnuniyeti (0 - 100; sıfıra ulaştığında görev sona erer). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Economy")
	float PublicSatisfaction = 100.0f;

	/** Şehirdeki toplam CO2 / kirlilik birikimi. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Climate")
	float CityCO2Level = 0.0f;

	/** İklim ve şehirleşme riski çarpanı (Taban 1.0; afet şiddetini çarpar). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Climate")
	float ClimateRiskMultiplier = 1.0f;

	/** Afet zorluk seviyesi (1'den başlar). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Progression")
	int32 CurrentDisasterLevel = 1;

	/** Bir sonraki tur hazineye aktarılacak devlet afet ödeneği. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Economy")
	int64 PendingStateAidTL = 0;

	/** Oyun bitti mi bayrağı. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|State")
	bool bIsGameOver = false;

	// -------- TUR YÖNETİMİ VE SİMÜLASYON --------
	/**
	 * Bir sonraki tura geçer: Vergi gelirlerini toplar, bakım giderlerini düşer,
	 * iklim riskini günceller ve memnuniyeti kontrol eder.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Economy")
	void AdvanceTurn();

	/**
	 * Afet tamamlandığında devlet ödeneğini, can kaybı memnuniyet düşüşünü ve önleme ödülünü işler.
	 */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Disaster")
	void HandleDisasterCompleted(const FDisasterReport& Report);

	// -------- BÜTÇE KONTROL VE HARCAMA ONAY SİSTEMİ --------
	/** Şehirdeki tüm binalara göre tur başına tahmini vergi geliri ve bakım giderini hesaplar. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Economy")
	void CalculateProjectedFinances(int64& OutTaxIncome, int64& OutMaintenance) const;

	/** Belirli bir tutarı hazineden harcamayı dener. Yeterli bütçe varsa true döner. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Economy")
	bool TrySpendMoney(int64 Amount);

	/** Bütçe kontrolü yaparak yeni bir bina inşa eder. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Construction")
	bool BuildBuildingWithBudget(
		FIntPoint GridCoords,
		UBuildingData* Data,
		EFoundationQuality Foundation,
		int32 Floors);

	/** 1.000 TL bütçe kontrolüyle binayı sismik olarak güçlendirir (Retrofit). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Maintenance")
	bool RetrofitWithBudget(int32 BuildingID);

	/** Hasar derecesine göre (2.000 veya 5.000 TL) bütçe kontrolüyle binayı onarır. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Maintenance")
	bool RepairWithBudget(int32 BuildingID);

	/** 3.000 TL bütçe kontrolüyle çöken binanın enkazını temizler. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Destruction")
	bool ClearRubbleWithBudget(FIntPoint GridCoords);

	// -------- İLERLEME VE OYUN BİTİŞİ --------
	/** Afet seviyesini bir üst kademeye taşır. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Progression")
	void AdvanceDisasterLevel();

	/** Mevcut iklim riski ve seviyeye göre bir sonraki afetin taban şiddetini hesaplar. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Progression")
	float GetNextDisasterBaseSeverity() const;

	/** Oyunu sonlandırır ve bitiş sebebini bildirir. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|State")
	void TriggerGameOver(EGameOverReason Reason);

	/** Oyunu başlangıç ayarlarına sıfırlar. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|State")
	void ResetState(int64 StartingBudget = 50000);

protected:
	UPROPERTY() TSet<FGuid> AppliedDisasterEventIDs;
	bool bAdvancingTurn=false;
	/** Başarıyla atlatılan afet sayısı. */
	UPROPERTY()
	int32 SuccessfulDisastersSurvived = 0;
};
