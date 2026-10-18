#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Aftershock/Public/AftershockAudioPerspective.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "DisasterAudioBridgeSubsystem.generated.h"

class UAudioComponent;
class USoundConcurrency;

/** World-owned audio follows the canonical presentation snapshot, including pause and travel. */
UCLASS()
class JAMPROJECT_API UDisasterAudioBridgeSubsystem final : public UWorldSubsystem
{

	GENERATED_BODY()
public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	FString DescribeSoundscape() const;
private:
	void HandleSnapshot(const FAftershockDisasterVisualSnapshot& Snapshot);
	void UpdateSoundscape();
	/** Bounded listener-area samples from registered canonical cells; never changes hazards. */
	void UpdateLocalHazardSamples();
	void StopSoundscape();
	UAudioComponent* CreateLayer(const TCHAR* AssetPath);
	UPROPERTY(Transient) TObjectPtr<UAudioComponent> PrimaryLayer;
	UPROPERTY(Transient) TObjectPtr<UAudioComponent> WaterLayer;
	/** Two world-owned beds share a hard voice limit, independent of UI sounds. */
	UPROPERTY(Transient) TObjectPtr<USoundConcurrency> LoopConcurrency;
	UPROPERTY(Transient) FAftershockDisasterVisualSnapshot ActiveSnapshot;
	FTimerHandle SoundscapeTimer;
	FDelegateHandle SnapshotHandle;
	float AudioDuration = 0.f;
	float WaterCoverage = 0.f;
	float BurningCoverage = 0.f;
	float BurningFuelRatio = 0.f;
	/** Retained diagnostic name: shared DetailGain, never a second zoom multiplier. */
	float LocalDetailGain = 0.f;
	FAftershockAudioPerspective AudioPerspective;
	float PrimaryTargetGain = 0.f;
	float WaterTargetGain = 0.f;
	float LastPrimaryAppliedTarget = -1.f;
	float LastWaterAppliedTarget = -1.f;
	float AppliedLowPassHz = 20000.f;
	float CityDuckGain = .6f;
	int32 SpatialSampleChecks = 0;
	int32 SpatialLandSamples = 0;
	FIntPoint AudioFocusCell = FIntPoint::ZeroValue;
	bool bLoggedPeak = false;
};
