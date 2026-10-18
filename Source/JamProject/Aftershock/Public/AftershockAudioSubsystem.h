// AFTERSHOCK — Civil Game Jam 2026
// Merkezi UI ses yönetimi — Kenney CC0 sesleri; örnek starter-kit kodu MIT lisanslıdır.
// Tüm UI, placement ve afet sesleri bu subsystem üzerinden tetiklenir.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "Aftershock/Public/BuildingData.h"
#include "AftershockAudioSubsystem.generated.h"

class USoundBase;
class UAudioComponent;
class USoundAttenuation;
class USoundConcurrency;

/** Bounded, weak playback handles; never one emitter per resident or vehicle. */
struct FAftershockWorldAudioVoice
{
	TWeakObjectPtr<UAudioComponent> Component;
	FVector Location = FVector::ZeroVector;
	float BaseGain = 1.f;
	float AppliedGain = -1.f;
	bool bHazard = false;
	bool bPositional = false;
};

/**
 * UAftershockAudioSubsystem — Oyun başı ambience ve spot UI/placement/removal/disaster sesleri.
 *
 * Varsayılan ses asset'leri Content/Aftershock/Audio/ altında Kenney CC0 kaynaklıdır.
 * Süreli afet katmanlarının sahibi dünya yaşam döngüsünü izleyen DisasterAudioBridgeSubsystem'dir.
 * Blueprint tarafında override edilebilir (DefaultXxx property'leri).
 */
UCLASS(BlueprintType, Config=Game)
class JAMPROJECT_API UAftershockAudioSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// -------- SES ASSET SOFT REFERANSLARI (Blueprint set edilebilir) --------

	/** Şehir arka plan sesi (loop, 2D). Default: /Game/Aftershock/Audio/S_Ambience_City */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Aftershock|Audio")
	TSoftObjectPtr<USoundBase> AmbienceSound;

	/** UI toggle/click sesi (buton tıklama). Default: /Game/Aftershock/Audio/S_Toggle */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Aftershock|Audio")
	TSoftObjectPtr<USoundBase> UIClickSound;

	/** Bina yerleştirme sesi (4 varyant random). Default: /Game/Aftershock/Audio/S_Placement_A..D */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Aftershock|Audio")
	TArray<TSoftObjectPtr<USoundBase>> PlacementSounds;

	/** Bina yıkım sesi (4 varyant random). Default: /Game/Aftershock/Audio/S_Removal_A..D */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Aftershock|Audio")
	TArray<TSoftObjectPtr<USoundBase>> RemovalSounds;

	/** Rotation sesi (R tuşu — placement ghost döndürme). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Aftershock|Audio")
	TSoftObjectPtr<USoundBase> RotateSound;

	/** Hazine kazancı sesi (vergi geliri, devlet ödeneği, quiz bonusu). Default: S_Coin */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Aftershock|Audio")
	TSoftObjectPtr<USoundBase> CoinGainSound;

	/** Bina hasar aldığında kısa feedback. Default: S_Damage */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Aftershock|Audio")
	TSoftObjectPtr<USoundBase> DamageTakenSound;

	/** Bina çatlıyor sesi (collapse öncesi, hasar >= 80). Default: S_Break */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Aftershock|Audio")
	TSoftObjectPtr<USoundBase> BreakSound;

	/** Patlama / collapse çarpması (3D positional, bina çöker). Default: S_Explosion */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Aftershock|Audio")
	TSoftObjectPtr<USoundBase> ExplosionSound;

	/** Panel/menu geçiş sesi (pause, quiz, results modal açılış). Default: S_PanelSwitch */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Aftershock|Audio")
	TSoftObjectPtr<USoundBase> PanelSwitchSound;

	/** Proje için üretilen uyarı sireni (afet hazırlık geri sayımı başında). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Aftershock|Audio")
	TSoftObjectPtr<USoundBase> WarningSirenSound;

	// -------- SES YÖNETİM API --------

	/** Ambience'ı loop'ta başlat (zaten çalıyorsa dokunma). Oyun başladığında otomatik çağrılır. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Audio")
	void StartCityAmbience();

	/** Ambience'ı durdur (level sonu, pause). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Audio")
	void StopCityAmbience();

	/** UI tıklama sesi çal. Her butonda bu çağrılır. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Audio")
	void PlayUIClick();

	/** Rastgele bir placement sesi çal (confirm placement anında). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Audio")
	void PlayPlacement();

	/** Rastgele bir removal/collapse sesi çal (bina yıkıldığında — 2D ya da 3D). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Audio")
	void PlayRemovalAtLocation(FVector WorldLocation);

	/** Rotation sesi çal (R tuşu). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Audio")
	void PlayRotate();

	/** Afet türüne göre proje için üretilen kısa 2D ses. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Audio")
	void PlayDisasterStinger(EDisasterType DisasterType);

	/** Hazine kazancı sesi (coin — vergi, state aid, quiz bonusu). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Audio")
	void PlayCoinGain();

	/** Bina hasar aldı — kısa 2D feedback. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Audio")
	void PlayDamageTaken();

	/** Bina çatlıyor (collapse öncesi, 3D konumlu). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Audio")
	void PlayBreakAtLocation(FVector WorldLocation);

	/** Patlama 3D konumlu (bina çöktüğünde Niagara ile birlikte). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Audio")
	void PlayExplosionAtLocation(FVector WorldLocation);

	/** Panel/menu açılış/kapanış sesi (pause, quiz, results). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Audio")
	void PlayPanelSwitch();

	/** Afet uyarı siren (afet hazırlık geri sayımı başında). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Audio")
	void PlayWarningSiren();

	/** Afet sırasında vatandaş panik çığlık / koşuşturma sesi. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Audio")
	void PlayCrowdPanic();

	/** Ambience ses seviyesi (0.0 - 1.0). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Audio")
	void SetAmbienceVolume(float Volume);
	/** Reapply user gains without replacing the disaster bridge's ambience duck. */
	void RefreshUserVolumes();
	/** Existing ambience and in-flight world effects follow actual camera zoom. */
	FString DescribeCameraMix() const;

protected:
	/** Loop çalan ambience audio component. */
	UPROPERTY()
	TObjectPtr<UAudioComponent> AmbienceComponent;
	UPROPERTY(Transient) TObjectPtr<USoundAttenuation> WorldSpotAttenuation;
	UPROPERTY(Transient) TObjectPtr<USoundConcurrency> WorldSpotConcurrency;
	TArray<FAftershockWorldAudioVoice> WorldVoices;
	TWeakObjectPtr<UWorld> SoundscapeWorld;
	FTimerHandle PerspectiveTimer;
	FDelegateHandle WorldCleanupHandle;
	float RuntimeAmbienceGain = .6f;
	float AppliedAmbienceGain = -1.f;
	float SmoothedLowPassHz = 20000.f;
	double LastMixUpdateSeconds = 0.;
	void EnsureAudioWorld(UWorld* World);
	void UpdateCameraMix();
	void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	void PlayWorldSound(USoundBase* Sound, float BaseGain, bool bHazard, const FVector* Location=nullptr);
	float GetUIVolume() const;

	/** TSoftObjectPtr'ı sync yükler, null güvenli döner. */
	USoundBase* ResolveSound(const TSoftObjectPtr<USoundBase>& Soft) const;

	/** TSoftObjectPtr dizisinden rastgele birini çöz, null güvenli. */
	USoundBase* ResolveRandomSound(const TArray<TSoftObjectPtr<USoundBase>>& SoftArray) const;
};
