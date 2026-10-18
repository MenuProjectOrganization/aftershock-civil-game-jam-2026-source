// AFTERSHOCK — Civil Game Jam 2026
// Core enum ve struct tanımları. Tüm Aftershock modülü bu header'ı include eder.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "Aftershock/Public/BuildingData.h"
#include "AftershockTypes.generated.h"

/** Önleme yatırım kategorisi. */
UENUM(BlueprintType)
enum class EPreventionType : uint8
{
	SeismicRetrofit    UMETA(DisplayName = "Sismik Güçlendirme"),
	DrainageSystem     UMETA(DisplayName = "Drenaj"),
	Breakwater         UMETA(DisplayName = "Dalgakıran"),
	WindShutter        UMETA(DisplayName = "Rüzgar Panjuru"),
	FirebreakZone      UMETA(DisplayName = "Yangın Emniyet Şeridi"),
	TsunamiSiren       UMETA(DisplayName = "Tsunami Siren"),
	AwarenessCampaign  UMETA(DisplayName = "Farkındalık / Tatbikat"),
	EmergencyUnit      UMETA(DisplayName = "Acil Durum Birimi")
};

/** Terrain cell özelliği — her grid hücresinin topoğrafik ve risk verisi. */
USTRUCT(BlueprintType)
struct FTerrainCell
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FIntPoint GridCoord = FIntPoint::ZeroValue;

	/** Metre cinsinden yükseklik. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float Elevation = 0.f;

	/** 0.0 (nehir yok) — 1.0 (hücre tam nehir yatağında). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0.0", ClampMax="1.0"))
	float RiverProximity = 0.f;

	/** 1.0 (vadi etkisi yok) — 2.5 (dar vadi, rüzgar hızlanır). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="1.0", ClampMax="3.0"))
	float ValleyChannelFactor = 1.f;

	/** 0.0 — 1.0, fay hattına yakınlık. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0.0", ClampMax="1.0"))
	float FaultLineProximity = 0.f;

	/** Hücrede su var mı (sel durumunda doldurulur). */
	UPROPERTY(BlueprintReadWrite)
	float FloodDepth = 0.f;

	/** Hücre yanıyor mu (yangın yayılımı için). */
	UPROPERTY(BlueprintReadWrite)
	bool bBurning = false;

	UPROPERTY(BlueprintReadWrite)
	int32 BurningTurns = 0;
};

/** Bir afet olayı için parametre paketi. UDisasterSimulator::Trigger() alır. */
USTRUCT(BlueprintType)
struct FDisasterEvent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	EDisasterType Type = EDisasterType::Earthquake;

	/** Deprem: Richter 4.0–8.5. Sel: yağış mm. Rüzgar: km/h. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float Severity = 5.f;

	/** Afet merkezi (deprem/yangın/tsunami için). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector Epicenter = FVector::ZeroVector;

	/** Rüzgar yönü (derece, 0–360). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float DirectionDeg = 0.f;

	/** Etki yarıçapı (deprem için metrik; sel/yangın için başlangıç). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float AffectedRadius = 500.f;

	/** Afet öncesi oyuncuya verilecek uyarı süresi (sn). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float WarningSeconds = 240.f;
};

/** Bir binaya uygulanan hasar sonucu. */
USTRUCT(BlueprintType)
struct FDamageResult
{
	GENERATED_BODY()

	/** 0–100, binaya uygulanacak hasar. */
	UPROPERTY(BlueprintReadWrite)
	float Damage = 0.f;

	/** Bu binada beklenen ölüm sayısı. */
	UPROPERTY(BlueprintReadWrite)
	int32 Deaths = 0;

	/** Kurtarılan sakin + çalışan sayısı. */
	UPROPERTY(BlueprintReadWrite)
	int32 Survivors = 0;

	/** Bu bina çökmeli mi (Chaos Destruction tetiklenir). */
	UPROPERTY(BlueprintReadWrite)
	bool bCollapsed = false;
};

/** Level sonu bilanço. UI'da gösterilir. */
USTRUCT(BlueprintType)
struct FLevelOutcome
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite) FGuid EventID;
	UPROPERTY(BlueprintReadWrite) int64 BurialCost=0;

	UPROPERTY(BlueprintReadWrite)
	int32 BuildingsCollapsed = 0;

	UPROPERTY(BlueprintReadWrite)
	int32 BuildingsDamaged = 0;

	UPROPERTY(BlueprintReadWrite)
	int32 TotalDeaths = 0;

	UPROPERTY(BlueprintReadWrite)
	int32 TotalSurvivors = 0;

	UPROPERTY(BlueprintReadWrite)
	int64 GovernmentAid = 0;

	UPROPERTY(BlueprintReadWrite)
	float SatisfactionDelta = 0.f;

	UPROPERTY(BlueprintReadWrite)
	float EnvironmentDelta = 0.f;
};

/** Quiz sorusu DataTable kaydı. */
USTRUCT(BlueprintType)
struct FQuizQuestion : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FText Question;

	/** Her zaman 4 şık. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	TArray<FText> Choices;

	/** 0–3, doğru şıkkın indeksi. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(ClampMin="0", ClampMax="3"))
	int32 CorrectIndex = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(MultiLine="true"))
	FText InfoNote;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	EDisasterType Category = EDisasterType::Earthquake;
};
