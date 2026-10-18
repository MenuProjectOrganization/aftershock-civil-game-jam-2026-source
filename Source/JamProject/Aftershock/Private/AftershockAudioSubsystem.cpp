// AFTERSHOCK — Civil Game Jam 2026
// UAftershockAudioSubsystem implementation.

#include "Aftershock/Public/AftershockAudioSubsystem.h"
#include "Aftershock/Public/AftershockAudioPerspective.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/AftershockSettingsSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/DisasterAudioBridgeSubsystem.h"
#include "Components/AudioComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "TimerManager.h"

namespace
{
	constexpr int32 MaximumWorldAudioVoices = 16;
	float WorldVoiceGain(const FAftershockWorldAudioVoice& Voice, const FAftershockAudioPerspective& Mix, const FVector& Focus)
	{
		float Gain = Voice.BaseGain * (Voice.bHazard ? Mix.HazardGain : Mix.DetailGain);
		if (Voice.bPositional)
		{
			const float Radius = FMath::Clamp(Mix.DistanceCm*.6f, 12000.f, 60000.f);
			Gain *= 1.f-FMath::SmoothStep(Radius*.15f, Radius, static_cast<float>(FVector::DistXY(Voice.Location, Focus)));
		}
		return Gain>.001f ? FMath::Clamp(Gain, 0.f, 2.f) : 0.f;
	}
}

namespace AftershockAudioPaths
{
	// Kenney CC0 sound effects; procedurally-authored disaster sounds live in /Generated.
	static const TCHAR* AmbienceCity = TEXT("/Game/Aftershock/Audio/S_Ambience_City.S_Ambience_City");
	static const TCHAR* UIClick      = TEXT("/Game/Aftershock/Audio/S_Toggle.S_Toggle");
	static const TCHAR* Rotate       = TEXT("/Game/Aftershock/Audio/S_Rotate.S_Rotate");
	static const TCHAR* Placement[]  =
	{
		TEXT("/Game/Aftershock/Audio/S_Placement_A.S_Placement_A"),
		TEXT("/Game/Aftershock/Audio/S_Placement_B.S_Placement_B"),
		TEXT("/Game/Aftershock/Audio/S_Placement_C.S_Placement_C"),
		TEXT("/Game/Aftershock/Audio/S_Placement_D.S_Placement_D")
	};
	static const TCHAR* Removal[]    =
	{
		TEXT("/Game/Aftershock/Audio/S_Removal_A.S_Removal_A"),
		TEXT("/Game/Aftershock/Audio/S_Removal_B.S_Removal_B"),
		TEXT("/Game/Aftershock/Audio/S_Removal_C.S_Removal_C"),
		TEXT("/Game/Aftershock/Audio/S_Removal_D.S_Removal_D")
	};
	// Kenney asset files are CC0 1.0; the starter-kit sample code is MIT.
	static const TCHAR* Coin         = TEXT("/Game/Aftershock/Audio/S_Coin.S_Coin");
	static const TCHAR* DamageTaken  = TEXT("/Game/Aftershock/Audio/S_Damage.S_Damage");
	static const TCHAR* BreakCrack   = TEXT("/Game/Aftershock/Audio/S_Break.S_Break");
	static const TCHAR* Explosion    = TEXT("/Game/Aftershock/Audio/S_Explosion.S_Explosion");
	static const TCHAR* PanelSwitch  = TEXT("/Game/Aftershock/Audio/S_PanelSwitch.S_PanelSwitch");
	static const TCHAR* WarningSiren = TEXT("/Game/Aftershock/Audio/Generated/S_DisasterWarning.S_DisasterWarning");
	static const TCHAR* EarthquakeRumble = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_01_Earthquake_FaultRupture.SFX_DIS_01_Earthquake_FaultRupture");
	static const TCHAR* EarthquakeFallback = TEXT("/Game/Aftershock/Audio/Generated/S_EarthquakeRumble.S_EarthquakeRumble");
	static const TCHAR* FloodSurge = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_02_Flood_LeveeBreach.SFX_DIS_02_Flood_LeveeBreach");
	static const TCHAR* FloodFallback = TEXT("/Game/Aftershock/Audio/Generated/S_FloodSurge.S_FloodSurge");
	static const TCHAR* StormGust = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_03_Storm_DebrisImpact.SFX_DIS_03_Storm_DebrisImpact");
	static const TCHAR* StormFallback = TEXT("/Game/Aftershock/Audio/Generated/S_StormGust.S_StormGust");
	static const TCHAR* FireIgnition = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_04_Fire_ExplosiveIgnition.SFX_DIS_04_Fire_ExplosiveIgnition");
	static const TCHAR* TsunamiCoastImpact = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_05_Tsunami_CoastImpact.SFX_DIS_05_Tsunami_CoastImpact");
	static const TCHAR* VolcanoBlast = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_06_Volcano_DetonationBlast.SFX_DIS_06_Volcano_DetonationBlast");
	static const TCHAR* DroughtStress = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_07_Drought_SoilStress.SFX_DIS_07_Drought_SoilStress");
	static const TCHAR* AvalancheFlow = TEXT("/Game/Aftershock/Audio/Generated/S_Avalanche_FlowBed.S_Avalanche_FlowBed");
	static const TCHAR* LandslideDebris = TEXT("/Game/Aftershock/Audio/Generated/S_Landslide_DebrisBed.S_Landslide_DebrisBed");
	static const TCHAR* CrowdPanic = TEXT("/Game/Aftershock/Audio/ElevenLabs/Citizens/SFX_CIT_Crowd_PanicScramble.SFX_CIT_Crowd_PanicScramble");
}

void UAftershockAudioSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(this, &UAftershockAudioSubsystem::HandleWorldCleanup);
	WorldSpotAttenuation = NewObject<USoundAttenuation>(this, TEXT("AftershockWorldSpotPanning"));
	// Perspective already accounts for camera height. Keep 3D stereo panning,
	// with horizontal source distance applied once in WorldVoiceGain.
	WorldSpotAttenuation->Attenuation.bAttenuate = false;
	WorldSpotAttenuation->Attenuation.bSpatialize = true;
	WorldSpotConcurrency = NewObject<USoundConcurrency>(this, TEXT("AftershockWorldSpotConcurrency"));
	WorldSpotConcurrency->Concurrency.MaxCount = MaximumWorldAudioVoices;
	WorldSpotConcurrency->Concurrency.ResolutionRule = EMaxConcurrentResolutionRule::StopOldest;
	WorldSpotConcurrency->Concurrency.VoiceStealReleaseTime = .03f;

	// Varsayılan asset path'lerini ata (yalnız Blueprint override edilmemişse).
	if (AmbienceSound.IsNull())
	{
		AmbienceSound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(AftershockAudioPaths::AmbienceCity));
	}
	if (UIClickSound.IsNull())
	{
		UIClickSound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(AftershockAudioPaths::UIClick));
	}
	if (RotateSound.IsNull())
	{
		RotateSound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(AftershockAudioPaths::Rotate));
	}
	if (PlacementSounds.Num() == 0)
	{
		for (const TCHAR* P : AftershockAudioPaths::Placement)
		{
			PlacementSounds.Add(TSoftObjectPtr<USoundBase>(FSoftObjectPath(P)));
		}
	}
	if (RemovalSounds.Num() == 0)
	{
		for (const TCHAR* P : AftershockAudioPaths::Removal)
		{
			RemovalSounds.Add(TSoftObjectPtr<USoundBase>(FSoftObjectPath(P)));
		}
	}
	if (CoinGainSound.IsNull())
	{
		CoinGainSound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(AftershockAudioPaths::Coin));
	}
	if (DamageTakenSound.IsNull())
	{
		DamageTakenSound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(AftershockAudioPaths::DamageTaken));
	}
	if (BreakSound.IsNull())
	{
		BreakSound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(AftershockAudioPaths::BreakCrack));
	}
	if (ExplosionSound.IsNull())
	{
		ExplosionSound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(AftershockAudioPaths::Explosion));
	}
	if (PanelSwitchSound.IsNull())
	{
		PanelSwitchSound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(AftershockAudioPaths::PanelSwitch));
	}
	if (WarningSirenSound.IsNull())
	{
		WarningSirenSound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(AftershockAudioPaths::WarningSiren));
	}
}

void UAftershockAudioSubsystem::Deinitialize()
{
	StopCityAmbience();
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
	WorldSpotConcurrency = nullptr;
	WorldSpotAttenuation = nullptr;
	Super::Deinitialize();
}

USoundBase* UAftershockAudioSubsystem::ResolveSound(const TSoftObjectPtr<USoundBase>& Soft) const
{
	if (Soft.IsNull()) return nullptr;
	if (USoundBase* Loaded = Soft.Get()) return Loaded;
	return Soft.LoadSynchronous();
}

USoundBase* UAftershockAudioSubsystem::ResolveRandomSound(const TArray<TSoftObjectPtr<USoundBase>>& SoftArray) const
{
	if (SoftArray.Num() == 0) return nullptr;
	const int32 Index = FMath::RandRange(0, SoftArray.Num() - 1);
	return ResolveSound(SoftArray[Index]);
}

void UAftershockAudioSubsystem::StartCityAmbience()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;
	EnsureAudioWorld(World);
	if (IsValid(AmbienceComponent)) { UpdateCameraMix(); return; }

	USoundBase* Sound = ResolveSound(AmbienceSound);
	if (!Sound) return;

	// Create silently; UpdateCameraMix starts only an audible live city.
	// No persisted component may carry the old world's city into a new menu.
	AmbienceComponent = UGameplayStatics::CreateSound2D(World, Sound, 1.f, 1.f, 0.f, nullptr, false, false);
	if (AmbienceComponent)
	{
		AmbienceComponent->bAutoDestroy = false;
		AmbienceComponent->bIsUISound = false;
		AmbienceComponent->SetLowPassFilterEnabled(true);
		AppliedAmbienceGain = -1.f;
		RefreshUserVolumes();
	}
}

void UAftershockAudioSubsystem::StopCityAmbience()
{
	if (UWorld* World = SoundscapeWorld.Get()) World->GetTimerManager().ClearTimer(PerspectiveTimer);
	SoundscapeWorld.Reset();
	for (const auto& Voice : WorldVoices)
		if (UAudioComponent* Component = Voice.Component.Get()) { Component->Stop(); Component->DestroyComponent(); }
	WorldVoices.Empty();
	if (IsValid(AmbienceComponent))
	{
		AmbienceComponent->Stop();
		AmbienceComponent->DestroyComponent();
	}
	AmbienceComponent = nullptr;
	AppliedAmbienceGain = -1.f;
	SmoothedLowPassHz = 20000.f;
	LastMixUpdateSeconds = 0.;
}

void UAftershockAudioSubsystem::EnsureAudioWorld(UWorld* World)
{
	if (!World || (World->WorldType!=EWorldType::Game && World->WorldType!=EWorldType::PIE)) return;
	if (SoundscapeWorld.Get()!=World)
	{
		StopCityAmbience();
		SoundscapeWorld = World;
	}
	if (!World->GetTimerManager().IsTimerActive(PerspectiveTimer))
	{
		FTimerManagerTimerParameters Parameters;
		Parameters.bLoop = true; Parameters.bMaxOncePerFrame = true;
		World->GetTimerManager().SetTimer(PerspectiveTimer, FTimerDelegate::CreateUObject(this, &UAftershockAudioSubsystem::UpdateCameraMix), .1f, Parameters);
	}
}

void UAftershockAudioSubsystem::HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	if (World && SoundscapeWorld.Get()==World) StopCityAmbience();
}

void UAftershockAudioSubsystem::UpdateCameraMix()
{
	UWorld* World = SoundscapeWorld.Get();
	if (!World) return;
	const FAftershockAudioPerspective Mix = GetAftershockAudioPerspective(World);
	const double Now = FPlatformTime::Seconds();
	const float Delta = LastMixUpdateSeconds>0. ? FMath::Clamp(static_cast<float>(Now-LastMixUpdateSeconds), 0.f, .25f) : .1f;
	LastMixUpdateSeconds = Now;
	SmoothedLowPassHz = FMath::FInterpTo(SmoothedLowPassHz, Mix.LowPassHz, Delta, 7.f);
	const auto* Preferences = GetGameInstance() ? GetGameInstance()->GetSubsystem<UAftershockSettingsSubsystem>() : nullptr;
	const float UserGain = Preferences ? Preferences->GetPreferences().CityAmbienceVolume : 1.f;
	if (IsValid(AmbienceComponent))
	{
		const float RawTarget = RuntimeAmbienceGain*UserGain*Mix.CityGain;
		const float Target = RawTarget>.001f ? RawTarget : 0.f;
		AmbienceComponent->SetLowPassFilterFrequency(SmoothedLowPassHz);
		if (Target>.001f && !AmbienceComponent->IsPlaying())
		{
			AmbienceComponent->FadeIn(.2f, Target);
			AppliedAmbienceGain = Target;
		}
		else if (AmbienceComponent->IsPlaying() && !FMath::IsNearlyEqual(Target, AppliedAmbienceGain, .0001f))
		{
			// AdjustVolume(0) releases the playing voice; the same retained
			// component is faded back in on zoom-in, without reloading an asset.
			AmbienceComponent->AdjustVolume(.2f, Target);
			AppliedAmbienceGain = Target;
		}
	}
	const auto* PC = World->GetFirstPlayerController();
	const auto* Camera = PC ? Cast<AAftershockCameraPawn>(PC->GetPawn()) : nullptr;
	const FVector Focus = Camera ? Camera->GetLocalMapFocus() : FVector::ZeroVector;
	for (int32 Index=WorldVoices.Num()-1; Index>=0; --Index)
	{
		auto& Voice = WorldVoices[Index]; UAudioComponent* Component = Voice.Component.Get();
		if (!Component || !Component->IsPlaying()) { WorldVoices.RemoveAtSwap(Index); continue; }
		Component->SetLowPassFilterFrequency(SmoothedLowPassHz);
		const float Target = WorldVoiceGain(Voice, Mix, Focus);
		if (!FMath::IsNearlyEqual(Target, Voice.AppliedGain, .0001f))
		{
			Component->AdjustVolume(.15f, Target);
			Voice.AppliedGain = Target;
		}
	}
}

void UAftershockAudioSubsystem::PlayWorldSound(USoundBase* Sound, float BaseGain, bool bHazard, const FVector* Location)
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World || !Sound || (Location && Location->ContainsNaN())) return;
	EnsureAudioWorld(World);
	if (SoundscapeWorld.Get()!=World) return;
	WorldVoices.RemoveAllSwap([](const FAftershockWorldAudioVoice& Voice){return !Voice.Component.IsValid() || !Voice.Component->IsPlaying();});
	if (WorldVoices.Num()>=MaximumWorldAudioVoices) return;
	const FAftershockAudioPerspective Mix = GetAftershockAudioPerspective(World);
	const auto* PC = World->GetFirstPlayerController();
	const auto* Camera = PC ? Cast<AAftershockCameraPawn>(PC->GetPawn()) : nullptr;
	FAftershockWorldAudioVoice Voice;
	Voice.BaseGain=BaseGain; Voice.bHazard=bHazard; Voice.bPositional=Location!=nullptr;
	if (Location) Voice.Location=*Location;
	const float Gain=WorldVoiceGain(Voice, Mix, Camera ? Camera->GetLocalMapFocus() : FVector::ZeroVector);
	if (Gain<=.001f) return; // A one-shot outside listening range is not queued for later replay.
	UAudioComponent* Component=UGameplayStatics::CreateSound2D(World, Sound, 1.f, 1.f, 0.f, WorldSpotConcurrency, false, true);
	if (!Component) return;
	Component->bIsUISound=false;
	if (Location)
	{
		Component->bAllowSpatialization=true;
		Component->SetAttenuationSettings(WorldSpotAttenuation);
		Component->SetWorldLocation(*Location);
	}
	Component->SetLowPassFilterEnabled(true);
	Component->SetLowPassFilterFrequency(Mix.LowPassHz);
	Component->FadeIn(.015f, Gain);
	Voice.Component=Component; Voice.AppliedGain=Gain; WorldVoices.Add(Voice);
}

FString UAftershockAudioSubsystem::DescribeCameraMix() const
{
	const auto Mix=GetAftershockAudioPerspective(SoundscapeWorld.Get());
	return FString::Printf(TEXT("listener=%d distanceCm=%.1f overviewBlend=%.3f cityGain=%.4f detailGain=%.4f hazardGain=%.4f lowPassHz=%.1f ambienceTarget=%.4f worldSpots=%d maxWorldSpots=%d worldAudioIsUI=0 timerSeconds=.1"),
		Mix.bHasListener, Mix.DistanceCm, Mix.OverviewBlend, Mix.CityGain, Mix.DetailGain, Mix.HazardGain, SmoothedLowPassHz,
		FMath::Max(0.f,AppliedAmbienceGain), WorldVoices.Num(), MaximumWorldAudioVoices);
}

void UAftershockAudioSubsystem::PlayUIClick()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;
	if (USoundBase* S = ResolveSound(UIClickSound))
	{
		UGameplayStatics::PlaySound2D(World, S, 0.9f*GetUIVolume());
	}
}

void UAftershockAudioSubsystem::PlayPlacement()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;
	if (USoundBase* S = ResolveRandomSound(PlacementSounds))
	{
		UGameplayStatics::PlaySound2D(World, S, 0.85f);
	}
}

void UAftershockAudioSubsystem::PlayRemovalAtLocation(FVector WorldLocation)
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;
	if (USoundBase* S = ResolveRandomSound(RemovalSounds))
	{
		PlayWorldSound(S, 1.1f, false, &WorldLocation);
	}
}

void UAftershockAudioSubsystem::PlayRotate()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;
	if (USoundBase* S = ResolveSound(RotateSound))
	{
		UGameplayStatics::PlaySound2D(World, S, 0.7f);
	}
}

void UAftershockAudioSubsystem::PlayDisasterStinger(EDisasterType DisasterType)
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;

	const TCHAR* DisasterSoundPath = nullptr;
	const TCHAR* FallbackSoundPath = nullptr;
	switch (DisasterType)
	{
	case EDisasterType::Earthquake:
		DisasterSoundPath = AftershockAudioPaths::EarthquakeRumble;
		FallbackSoundPath = AftershockAudioPaths::EarthquakeFallback;
		break;
	case EDisasterType::Flood:
		DisasterSoundPath = AftershockAudioPaths::FloodSurge;
		FallbackSoundPath = AftershockAudioPaths::FloodFallback;
		break;
	case EDisasterType::Wind:
		DisasterSoundPath = AftershockAudioPaths::StormGust;
		FallbackSoundPath = AftershockAudioPaths::StormFallback;
		break;
	case EDisasterType::Fire:
		DisasterSoundPath = AftershockAudioPaths::FireIgnition;
		break;
	case EDisasterType::Tsunami:
		DisasterSoundPath = AftershockAudioPaths::TsunamiCoastImpact;
		FallbackSoundPath = AftershockAudioPaths::FloodFallback;
		break;
	case EDisasterType::Volcano:
		DisasterSoundPath = AftershockAudioPaths::VolcanoBlast;
		FallbackSoundPath = AftershockAudioPaths::Explosion;
		break;
	case EDisasterType::Drought:
		DisasterSoundPath = AftershockAudioPaths::DroughtStress;
		break;
	case EDisasterType::Avalanche:
		DisasterSoundPath = AftershockAudioPaths::AvalancheFlow;
		break;
	case EDisasterType::Landslide:
		DisasterSoundPath = AftershockAudioPaths::LandslideDebris;
		break;
	default:
		DisasterSoundPath = AftershockAudioPaths::WarningSiren;
		break;
	}

	if (DisasterType == EDisasterType::Avalanche || DisasterType == EDisasterType::Landslide)
	{
		// These assets are looping motion beds, not one-shot stingers. Their
		// world-owned bridge starts them only after a mass replay is accepted,
		// tracks actual moving mass, and destroys them on travel. Playing the
		// looping SoundWave through PlaySound2D would have no owner to stop it.
		const auto* Simulation = World->GetSubsystem<UDisasterSimulationSubsystem>();
		double Playback = 0., SourceElapsed = 0., SourceDuration = 0.; float Moving = 0.f;
		const bool bAcceptedMotion = Simulation && Simulation->GetPhysicalEpisodeState().Type == DisasterType
			&& Simulation->TryGetPhysicalAudioProgress(Playback, SourceElapsed, SourceDuration, Moving) && Moving > 0.f;
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_MASS_STINGER_ROUTED type=%d primary=%s acceptedMotion=%d worldBridgePresent=%d oneShotLoopSpawned=0 unrelatedFallback=0"),
			static_cast<int32>(DisasterType), DisasterSoundPath, bAcceptedMotion, World->GetSubsystem<UDisasterAudioBridgeSubsystem>() != nullptr);
		return;
	}

	USoundBase* S = ResolveSound(TSoftObjectPtr<USoundBase>{FSoftObjectPath(FString(DisasterSoundPath))});
	if (!S && FallbackSoundPath)
	{
		S = ResolveSound(TSoftObjectPtr<USoundBase>{FSoftObjectPath(FString(FallbackSoundPath))});
	}
	if (!S)
	{
		S = ResolveSound(TSoftObjectPtr<USoundBase>{FSoftObjectPath(FString(AftershockAudioPaths::WarningSiren))});
	}

	if (S)
	{
		PlayWorldSound(S, 1.f, true);
	}
	PlayCrowdPanic();
}

void UAftershockAudioSubsystem::PlayCrowdPanic()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;

	USoundBase* S = ResolveSound(TSoftObjectPtr<USoundBase>{FSoftObjectPath(FString(AftershockAudioPaths::CrowdPanic))});
	if (S)
	{
		PlayWorldSound(S, .85f, false);
	}
}

void UAftershockAudioSubsystem::SetAmbienceVolume(float Volume)
{
	RuntimeAmbienceGain=FMath::IsFinite(Volume) ? FMath::Clamp(Volume,0.f,1.f) : 0.f;
	RefreshUserVolumes();
}

void UAftershockAudioSubsystem::RefreshUserVolumes()
{
	UpdateCameraMix();
}

float UAftershockAudioSubsystem::GetUIVolume() const
{
	const auto* Preferences=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSettingsSubsystem>():nullptr;
	return Preferences?Preferences->GetPreferences().UIVolume:1.f;
}

void UAftershockAudioSubsystem::PlayCoinGain()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;
	if (USoundBase* S = ResolveSound(CoinGainSound))
	{
		UGameplayStatics::PlaySound2D(World, S, 0.8f);
	}
}

void UAftershockAudioSubsystem::PlayDamageTaken()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;
	if (USoundBase* S = ResolveSound(DamageTakenSound))
	{
		UGameplayStatics::PlaySound2D(World, S, 0.75f);
	}
}

void UAftershockAudioSubsystem::PlayBreakAtLocation(FVector WorldLocation)
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;
	if (USoundBase* S = ResolveSound(BreakSound))
	{
		PlayWorldSound(S, 1.f, false, &WorldLocation);
	}
}

void UAftershockAudioSubsystem::PlayExplosionAtLocation(FVector WorldLocation)
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;
	if (USoundBase* S = ResolveSound(ExplosionSound))
	{
		PlayWorldSound(S, 1.3f, true, &WorldLocation);
	}
}

void UAftershockAudioSubsystem::PlayPanelSwitch()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;
	if (USoundBase* S = ResolveSound(PanelSwitchSound))
	{
		UGameplayStatics::PlaySound2D(World, S, 0.65f*GetUIVolume());
	}
}

void UAftershockAudioSubsystem::PlayWarningSiren()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;
	if (USoundBase* S = ResolveSound(WarningSirenSound))
	{
		PlayWorldSound(S, 1.1f, true);
	}
}
