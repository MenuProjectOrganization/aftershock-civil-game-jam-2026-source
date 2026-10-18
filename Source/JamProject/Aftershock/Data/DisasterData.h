// AFTERSHOCK — Civil Game Jam 2026
// Afet tipleri için DataAsset. Her level bir UDisasterData referansı tutar.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Aftershock/Types/AftershockTypes.h"
#include "DisasterData.generated.h"

class USoundBase;
class UNiagaraSystem;

UCLASS(BlueprintType)
class JAMPROJECT_API UDisasterData : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Basic")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Basic", meta=(MultiLine="true"))
	FText Description;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Basic")
	EDisasterType Type = EDisasterType::Earthquake;

	// -------- ŞİDDET ARALIKLARI --------
	/** Deprem: Richter / Sel: mm yağış / Rüzgar: km/h / Yangın: dryness 0-1 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Severity")
	float SeverityMin = 4.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Severity")
	float SeverityMax = 7.f;

	/** Etki yarıçapı base (metrik). Deprem için Magnitude x bu çarpan. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Severity")
	float RadiusBase = 150.f;

	// -------- UYARI --------
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Warning")
	float WarningSecondsMin = 180.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Warning")
	float WarningSecondsMax = 300.f;

	// -------- İKLİM --------
	/** Çevre skoruna göre afet şiddet çarpanı (CO2 çok → afet sert). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Climate", meta=(ClampMin="1.0", ClampMax="3.0"))
	float ClimateMultiplierMax = 1.5f;

	// -------- GÖRSEL / SES --------
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="FX")
	TSoftObjectPtr<UNiagaraSystem> OnsetFX;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="FX")
	TSoftObjectPtr<USoundBase> WarningSound;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="FX")
	TSoftObjectPtr<USoundBase> OnsetSound;

	// -------- TAHMİNİ KAYIP (UI için) --------
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="UI")
	FLinearColor WarningColor = FLinearColor(1.f, 0.3f, 0.1f);
};
