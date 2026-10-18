#include "Aftershock/Public/DisasterAudioBridgeSubsystem.h"
#include "Aftershock/Public/AftershockAudioSubsystem.h"
#include "Aftershock/Public/DisasterPresentationSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Camera/CameraTypes.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/AudioComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundConcurrency.h"
#include "TimerManager.h"

namespace
{
void ApplyDisasterLayerPerspective(UAudioComponent* Component, float Gain, float LowPassHz, float& LastAppliedGain)
{
	if (!IsValid(Component)) return;
	Gain = Gain <= .001f ? 0.f : Gain;
	Component->SetLowPassFilterEnabled(true);
	Component->SetLowPassFilterFrequency(LowPassHz);
	if (Gain > 0.f && !Component->IsPlaying())
	{
		// AdjustVolume(..., 0) eventually stops its voice in UE. Reuse the same
		// bounded component on zoom-in; the event envelope/physical clock is NOT
		// restarted, and no asset is loaded by this heartbeat.
		Component->FadeIn(.15f, Gain);
		LastAppliedGain = Gain;
	}
	else if (Component->IsPlaying() && (FMath::Abs(Gain - LastAppliedGain) > 1.e-4f
		|| (Gain <= 0.f && LastAppliedGain > 0.f)))
	{
		// Reissuing an unchanged zero target keeps extending the fade deadline.
		// Retarget only actual changes, so silent voices can finish and release.
		Component->AdjustVolume(.15f, Gain);
		LastAppliedGain = Gain;
	}
}
}

bool UDisasterAudioBridgeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return Super::ShouldCreateSubsystem(Outer) && World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UDisasterAudioBridgeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UDisasterSimulationSubsystem>();
	Collection.InitializeDependency<UDisasterPresentationSubsystem>();
	LoopConcurrency = NewObject<USoundConcurrency>(this, TEXT("AftershockDisasterLoopConcurrency"));
	LoopConcurrency->Concurrency.MaxCount = 2;
	LoopConcurrency->Concurrency.ResolutionRule = EMaxConcurrentResolutionRule::StopOldest;
	LoopConcurrency->Concurrency.VoiceStealReleaseTime = .05f;
}

void UDisasterAudioBridgeSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (UDisasterSimulationSubsystem* Simulation = InWorld.GetSubsystem<UDisasterSimulationSubsystem>())
	{
		SnapshotHandle = Simulation->OnDisasterVisualSnapshotChanged.AddUObject(this, &UDisasterAudioBridgeSubsystem::HandleSnapshot);
		if (Simulation->GetLatestVisualSnapshot().Revision > 0) HandleSnapshot(Simulation->GetLatestVisualSnapshot());
	}
}

UAudioComponent* UDisasterAudioBridgeSubsystem::CreateLayer(const TCHAR* AssetPath)
{
	USoundBase* Sound = LoadObject<USoundBase>(nullptr, AssetPath);
	if (!Sound) { UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_DISASTER_AUDIO missing=%s"), AssetPath); return nullptr; }
	UAudioComponent* Component = UGameplayStatics::CreateSound2D(GetWorld(), Sound, 1.f, 1.f, 0.f, LoopConcurrency, false, false);
	if (Component)
	{
		Component->bIsUISound = false; // A disaster freezes with the world; UI clicks may continue.
		Component->bAutoDestroy = false;
		// Keep base volume one but leave the component unstarted. The first
		// audible target starts a fade-in; globe/no-listener needs no silent voice.
	}
	return Component;
}

void UDisasterAudioBridgeSubsystem::HandleSnapshot(const FAftershockDisasterVisualSnapshot& Snapshot)
{
	StopSoundscape();
	ActiveSnapshot = Snapshot;
	UWorld* World = GetWorld();
	if (!World || Snapshot.Revision <= 0) return;
	const auto* Simulation = World->GetSubsystem<UDisasterSimulationSubsystem>();
	const bool bMass = Snapshot.Type == EDisasterType::Avalanche || Snapshot.Type == EDisasterType::Landslide;
	const bool bPersistentWater = Simulation && Simulation->IsHydraulicModel2Active();
	AudioDuration = Snapshot.GetEffectiveDurationSeconds();
	const bool bExpiredLegacy = !bMass && Snapshot.FloodModelVersion != 2
		&& World->GetTimeSeconds() - Snapshot.StartedAtWorldSeconds >= AudioDuration;
	if (bExpiredLegacy && !bPersistentWater) { AudioDuration = 0.f; return; }

	const TCHAR* PrimaryPath = nullptr;
	switch (Snapshot.Type)
	{
	case EDisasterType::Flood:
		PrimaryPath = TEXT("/Game/Aftershock/Audio/Generated/S_Flood_RainBed.S_Flood_RainBed");
		break;
	case EDisasterType::Wind: PrimaryPath = TEXT("/Game/Aftershock/Audio/Generated/S_Storm_WindBed.S_Storm_WindBed"); break;
	case EDisasterType::Tsunami:
		PrimaryPath = TEXT("/Game/Aftershock/Audio/Generated/S_Tsunami_SurgeBed.S_Tsunami_SurgeBed");
		break;
	case EDisasterType::Earthquake: PrimaryPath = TEXT("/Game/Aftershock/Audio/Generated/S_Earthquake_RumbleBed.S_Earthquake_RumbleBed"); break;
	case EDisasterType::Fire: PrimaryPath = TEXT("/Game/Aftershock/Audio/Generated/S_ForestFire_CrackleBed.S_ForestFire_CrackleBed"); break;
	case EDisasterType::Volcano: PrimaryPath = TEXT("/Game/Aftershock/Audio/Generated/S_Volcano_RoarBed.S_Volcano_RoarBed"); break;
	case EDisasterType::Drought: PrimaryPath = TEXT("/Game/Aftershock/Audio/Generated/S_Drought_DryBed.S_Drought_DryBed"); break;
	case EDisasterType::Avalanche: PrimaryPath = TEXT("/Game/Aftershock/Audio/Generated/S_Avalanche_FlowBed.S_Avalanche_FlowBed"); break;
	case EDisasterType::Landslide: PrimaryPath = TEXT("/Game/Aftershock/Audio/Generated/S_Landslide_DebrisBed.S_Landslide_DebrisBed"); break;
	default: AudioDuration = 0.f; return;
	}

	// SI water is persistent world state. A new warning/event may replace the
	// primary bed, but never converts remaining water back to a 24-second effect.
	if (bPersistentWater || Snapshot.Type == EDisasterType::Flood)
		WaterLayer = CreateLayer(TEXT("/Game/Aftershock/Audio/Generated/S_Flood_WaterBed.S_Flood_WaterBed"));
	else if (Snapshot.Type == EDisasterType::Tsunami)
		WaterLayer = CreateLayer(TEXT("/Game/Aftershock/Audio/Generated/S_Earthquake_RumbleBed.S_Earthquake_RumbleBed"));

	// A queued numerical mass job is NOT an accepted moving avalanche/landslide.
	// Load/start its unique primary once, later, when canonical replay motion is
	// actually published. The timer does not load any assets while it waits.
	if (!bMass && !bExpiredLegacy) PrimaryLayer = CreateLayer(PrimaryPath);
	if (!bMass && !PrimaryLayer && !WaterLayer)
	{
		StopSoundscape();
		if (UGameInstance* Instance = World->GetGameInstance())
			if (UAftershockAudioSubsystem* Audio = Instance->GetSubsystem<UAftershockAudioSubsystem>()) Audio->PlayDisasterStinger(Snapshot.Type);
		UE_LOG(LogTemp, Warning, TEXT("AFTERSHOCK_DISASTER_AUDIO_FALLBACK revision=%d cityAmbienceRestored=1"), Snapshot.Revision);
		return;
	}
	bLoggedPeak = false;
	FTimerManagerTimerParameters TimerParameters;
	TimerParameters.bLoop = true;
	TimerParameters.bMaxOncePerFrame = true;
	World->GetTimerManager().SetTimer(SoundscapeTimer, this, &UDisasterAudioBridgeSubsystem::UpdateSoundscape, .1f, TimerParameters);
	UpdateSoundscape();
	if (AudioDuration <= 0.f) return;
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_DISASTER_AUDIO_ARM revision=%d type=%d snapshotDuration=%.2f primary=%s waterLayer=%d massWaitsForAcceptedMotion=%d persistentWater=%d pauseWithWorld=1"),
		Snapshot.Revision, static_cast<int32>(Snapshot.Type), AudioDuration, PrimaryPath, WaterLayer != nullptr, bMass, bPersistentWater);
}

void UDisasterAudioBridgeSubsystem::UpdateSoundscape()
{
	UWorld* World = GetWorld();
	if (!World || AudioDuration <= 0.f) return;
	const auto* Simulation = World->GetSubsystem<UDisasterSimulationSubsystem>();
	const bool bMass = ActiveSnapshot.Type == EDisasterType::Avalanche || ActiveSnapshot.Type == EDisasterType::Landslide;
	const bool bPhysicalWater = ActiveSnapshot.FloodModelVersion == 2
		&& (ActiveSnapshot.Type == EDisasterType::Flood || ActiveSnapshot.Type == EDisasterType::Tsunami);
	const bool bPersistentWater = Simulation && Simulation->IsHydraulicModel2Active();
	const float Age = FMath::Max(0.f, World->GetTimeSeconds() - ActiveSnapshot.StartedAtWorldSeconds);
	if (!bMass && !bPhysicalWater && Age >= AudioDuration && !bPersistentWater) { StopSoundscape(); return; }

	double Playback = 0., SourceElapsed = 0., SourceDuration = 0.;
	float MovingMass01 = 0.f;
	const bool bMatchingEpisode = Simulation && Simulation->GetPhysicalEpisodeState().Type == ActiveSnapshot.Type;
	const bool bAcceptedProgress = bMatchingEpisode && Simulation->TryGetPhysicalAudioProgress(Playback, SourceElapsed, SourceDuration, MovingMass01);
	AudioPerspective = GetAftershockAudioPerspective(World);
	LocalDetailGain = AudioPerspective.bHasListener ? FMath::Clamp(AudioPerspective.DetailGain, 0.f, 1.f) : 0.f;
	const float HazardPerspectiveGain = AudioPerspective.bHasListener ? FMath::Clamp(AudioPerspective.HazardGain, 0.f, 1.f) : 0.f;
	const float TargetLowPassHz = FMath::IsFinite(AudioPerspective.LowPassHz) ? FMath::Clamp(AudioPerspective.LowPassHz, 200.f, 20000.f) : 20000.f;
	AppliedLowPassHz = FMath::FInterpTo(AppliedLowPassHz, TargetLowPassHz, .1f, 8.f);
	UpdateLocalHazardSamples();
	float PrimaryGain = 0.f;
	if (bMass)
	{
		if (bAcceptedProgress && MovingMass01 > 0.f && !PrimaryLayer)
		{
			const TCHAR* Path = ActiveSnapshot.Type == EDisasterType::Avalanche
				? TEXT("/Game/Aftershock/Audio/Generated/S_Avalanche_FlowBed.S_Avalanche_FlowBed")
				: TEXT("/Game/Aftershock/Audio/Generated/S_Landslide_DebrisBed.S_Landslide_DebrisBed");
			PrimaryLayer = CreateLayer(Path);
			if (!PrimaryLayer)
			{
				// Explicit asset failure stops this attempt, rather than LoadObject
				// on every heartbeat or substitute an unrelated earthquake sound.
				StopSoundscape();
				return;
			}
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_MASS_AUDIO_ACCEPTED revision=%d type=%d playback=%.3f movingFraction=%.6f primary=%s pendingWorkerAudio=0"),
				ActiveSnapshot.Revision, static_cast<int32>(ActiveSnapshot.Type), Playback, MovingMass01, Path);
		}
		PrimaryGain = bAcceptedProgress ? .58f * FMath::Sqrt(FMath::Clamp(MovingMass01, 0.f, 1.f)) : 0.f;
	}
	else if (bPhysicalWater)
	{
		if (bAcceptedProgress && SourceDuration > 0. && SourceElapsed < SourceDuration)
		{
			const double Scale = FMath::Max(1., Simulation->GetPhysicalEpisodeState().SimulationTimeScale);
			const double FadeIn = FMath::Min(.8 * Scale, SourceDuration * .1);
			const double FadeOut = FMath::Min(4. * Scale, SourceDuration * .1);
			const float SourceEnvelope = static_cast<float>(FMath::SmoothStep(0., FMath::Max(.001, FadeIn), SourceElapsed)
				* (1. - FMath::SmoothStep(FMath::Max(0., SourceDuration - FadeOut), SourceDuration, SourceElapsed)));
			PrimaryGain = SourceEnvelope * (.28f + .30f * FMath::Clamp(ActiveSnapshot.EffectiveIntensity / 100.f, 0.f, 1.f));
		}
	}
	else if (Age < AudioDuration)
	{
		const float Envelope = FMath::SmoothStep(0.f, 1.2f, Age) * (1.f - FMath::SmoothStep(FMath::Max(0.f, AudioDuration - 4.f), AudioDuration, Age));
		const float FireGain = ActiveSnapshot.Type == EDisasterType::Fire ? FMath::Sqrt(BurningFuelRatio) : 1.f;
		PrimaryGain = Envelope * (.28f + .30f * FMath::Clamp(ActiveSnapshot.EffectiveIntensity / 100.f, 0.f, 1.f)) * FireGain;
	}
	// Exactly one common distance gain covers ALL primary disaster beds,
	// including wind, quake, drought and volcano, not only fire/water/mass.
	PrimaryTargetGain = PrimaryGain * HazardPerspectiveGain;
	if (PrimaryTargetGain <= .001f) PrimaryTargetGain = 0.f;
	if (PrimaryLayer)
	{
		if ((bMass || bPhysicalWater) && bMatchingEpisode && !Simulation->IsPhysicalEventRunning())
		{
			PrimaryTargetGain = 0.f;
			PrimaryLayer->Stop();
			LastPrimaryAppliedTarget = -1.f;
		}
		else ApplyDisasterLayerPerspective(PrimaryLayer, PrimaryTargetGain, AppliedLowPassHz, LastPrimaryAppliedTarget);
	}
	float WaterGain = 0.f;
	if (WaterLayer)
	{
		if (bPersistentWater)
			WaterGain = FMath::Clamp(FMath::Sqrt(WaterCoverage) * .65f, 0.f, .55f);
		else
		{
			const float Envelope = Age < AudioDuration ? FMath::SmoothStep(0.f, 1.2f, Age) * (1.f - FMath::SmoothStep(FMath::Max(0.f, AudioDuration - 4.f), AudioDuration, Age)) : 0.f;
			WaterGain = ActiveSnapshot.Type == EDisasterType::Tsunami
				? Envelope * (.08f + .10f * FMath::Clamp(ActiveSnapshot.EffectiveIntensity / 100.f, 0.f, 1.f))
				: Envelope * FMath::Clamp(FMath::Sqrt(WaterCoverage) * .65f, 0.f, .55f);
		}
	}
	WaterTargetGain = WaterGain * LocalDetailGain;
	if (WaterTargetGain <= .001f) WaterTargetGain = 0.f;
	ApplyDisasterLayerPerspective(WaterLayer, WaterTargetGain, AppliedLowPassHz, LastWaterAppliedTarget);
	const float DuckBlend = FMath::SmoothStep(0.f, .25f, PrimaryTargetGain + WaterTargetGain);
	CityDuckGain = FMath::Lerp(.6f, .12f, DuckBlend);
	if (UGameInstance* Instance = World->GetGameInstance())
		if (UAftershockAudioSubsystem* Audio = Instance->GetSubsystem<UAftershockAudioSubsystem>())
			Audio->SetAmbienceVolume(CityDuckGain);
	if (!bLoggedPeak && (PrimaryTargetGain > .02f || WaterTargetGain > .02f))
	{
		bLoggedPeak = true;
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_DISASTER_AUDIO_PEAK %s"), *DescribeSoundscape());
	}
}

void UDisasterAudioBridgeSubsystem::UpdateLocalHazardSamples()
{
	const UWorld* World = GetWorld();
	const auto* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	const auto* Visuals = World ? World->GetSubsystem<UDisasterPresentationSubsystem>() : nullptr;
	const auto* Simulation = World ? World->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
	const bool bPhysicalWater = Simulation && Simulation->IsHydraulicModel2Active();
	SpatialSampleChecks = SpatialLandSamples = 0;
	if (!Terrain || Terrain->Cells.IsEmpty() || !AudioPerspective.bHasListener
		|| (LocalDetailGain <= 0.f && AudioPerspective.HazardGain <= 0.f))
	{ WaterCoverage = BurningCoverage = BurningFuelRatio = 0.f; return; }
	const APlayerController* PC = World->GetFirstPlayerController();
	const auto* Camera = PC ? Cast<AAftershockCameraPawn>(PC->GetPawn()) : nullptr;
	const FIntRect Bounds = Terrain->GetResidentCellBounds();
	FVector Focus = Camera ? Camera->GetLocalMapFocus() : Terrain->GridToWorld(Bounds.Min + (Bounds.Max - Bounds.Min) / 2);
	const double GlobalX = (Focus.X - Terrain->GridOrigin.X) / Terrain->CellSize, GlobalY = (Focus.Y - Terrain->GridOrigin.Y) / Terrain->CellSize;
	if (!FMath::IsFinite(GlobalX) || !FMath::IsFinite(GlobalY)
		|| FMath::Abs(GlobalX) > UTerrainGridSubsystem::MaximumAbsGlobalCellCoord || FMath::Abs(GlobalY) > UTerrainGridSubsystem::MaximumAbsGlobalCellCoord)
	{ WaterCoverage = BurningCoverage = BurningFuelRatio = 0.f; return; }
	AudioFocusCell = FIntPoint(FMath::FloorToInt(GlobalX), FMath::FloorToInt(GlobalY));
	const bool bHasView = PC && PC->PlayerCameraManager;
	FVector ViewLocation = Focus + FVector(0, 0, 5000), Forward(0, 0, -1), Right(1, 0, 0), Up(0, 1, 0);
	double TanHalfFOV = 1., AspectRatio = 1.6, RadiusCells = 12.;
	if (bHasView)
	{
		const FMinimalViewInfo& View = PC->PlayerCameraManager->GetCameraCacheView();
		ViewLocation = View.Location; const FRotationMatrix Rotation(View.Rotation);
		Forward = Rotation.GetUnitAxis(EAxis::X); Right = Rotation.GetUnitAxis(EAxis::Y); Up = Rotation.GetUnitAxis(EAxis::Z);
		TanHalfFOV = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(View.FOV, 10.f, 150.f) * .5f));
		AspectRatio = FMath::Max(.25, double(View.AspectRatio));
		RadiusCells = FMath::Clamp(FVector::Distance(ViewLocation, Focus) * TanHalfFOV / Terrain->CellSize, 8., 24.);
	}
	const int32 Radius = FMath::CeilToInt(RadiusCells);
	const int32 Step = FMath::Max(1, FMath::CeilToInt((2.f * Radius + 1.f) / 25.f));
	const double RadiusSquared = FMath::Square(double(Radius) * Terrain->CellSize);
	double LandWeight = 0., SurfaceWeight = 0., WetWeight = 0., BurningWeight = 0., FuelWeight = 0.;
	// At most 25x25 registered-cell queries per 100ms callback, including holes.
	// TryGetCell reads the same append-only Cells authority for negative/global
	// coordinates; initial GridWidth/Height are not a world or audio boundary.
	for (int32 Y = -Radius; Y <= Radius; Y += Step) for (int32 X = -Radius; X <= Radius; X += Step)
	{
		++SpatialSampleChecks;
		FGridCell Cell;
		if (!Terrain->TryGetCell(AudioFocusCell + FIntPoint(X, Y), Cell) || !Cell.bIsPlayable || Cell.bIsOcean) continue;
		const FVector Position = Terrain->GridToWorld(Cell.Coords);
		const double DistanceSquared = FVector::DistSquared2D(Position, Focus);
		if (DistanceSquared >= RadiusSquared) continue;
		if (bHasView)
		{
			const FVector Offset = Position - ViewLocation; const double Depth = FVector::DotProduct(Offset, Forward);
			if (Depth <= 0. || FMath::Abs(FVector::DotProduct(Offset, Right)) > Depth * TanHalfFOV * 1.1
				|| FMath::Abs(FVector::DotProduct(Offset, Up)) > Depth * TanHalfFOV / AspectRatio * 1.1) continue;
		}
		const double Weight = 1. - DistanceSquared / RadiusSquared;
		SurfaceWeight += Weight;
		if (!Cell.bIsRiverbed) { LandWeight += Weight; ++SpatialLandSamples; }
		if (bPhysicalWater)
		{
			double HeadCm = 0., ColumnCm = 0.; FVector2D Velocity = FVector2D::ZeroVector;
			if (Simulation->TryGetPhysicalWaterSample(Cell.Coords, HeadCm, ColumnCm, Velocity) && ColumnCm > 1.)
			{
				// Display-only sound calibration from actual SI state. Still ponded
				// water is quiet; moving river/overbank water remains audible after
				// rain stops. No velocity/depth feeds back into damage or hydrology.
				const double SpeedMps = Velocity.Size() * .01;
				const double FlowAudibility = FMath::Clamp(SpeedMps / 2., 0., 1.) * FMath::Clamp(ColumnCm / 25., 0., 1.);
				WetWeight += Weight * FlowAudibility;
			}
		}
		else if (!Cell.bIsRiverbed && Visuals && Visuals->GetDisplayedFloodDepthCm(Cell.Coords) > 12.f) WetWeight += Weight;
		const float LocalFire = FMath::Clamp(ActiveSnapshot.LocalSeverity.FindRef(Cell.Coords) / 100.f, 0.f, 1.f);
		if (ActiveSnapshot.Type == EDisasterType::Fire && Cell.bBurning && LocalFire > 0.f)
		{
			BurningWeight += Weight;
			FuelWeight += Weight * FMath::Clamp(Cell.ForestFuel01, 0.f, 1.f) * LocalFire;
		}
	}
	const double WetDenominator = bPhysicalWater ? SurfaceWeight : LandWeight;
	const float Wet = WetDenominator > 0. ? static_cast<float>(WetWeight / WetDenominator) : 0.f;
	const float Burning = LandWeight > 0. ? static_cast<float>(BurningWeight / LandWeight) : 0.f;
	const float Fuel = LandWeight > 0. ? static_cast<float>(FuelWeight / LandWeight) : 0.f;
	WaterCoverage = FMath::Clamp(FMath::FInterpTo(WaterCoverage, Wet, .1f, 6.f), 0.f, 1.f);
	BurningCoverage = FMath::Clamp(FMath::FInterpTo(BurningCoverage, Burning, .1f, 6.f), 0.f, 1.f);
	BurningFuelRatio = FMath::Clamp(FMath::FInterpTo(BurningFuelRatio, Fuel, .1f, 6.f), 0.f, 1.f);
}

FString UDisasterAudioBridgeSubsystem::DescribeSoundscape() const
{
	const auto* Simulation = GetWorld() ? GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
	double Playback = 0., SourceElapsed = 0., SourceDuration = 0.; float Moving = 0.f;
	const bool bAccepted = Simulation && Simulation->GetPhysicalEpisodeState().Type == ActiveSnapshot.Type
		&& Simulation->TryGetPhysicalAudioProgress(Playback, SourceElapsed, SourceDuration, Moving);
	return FString::Printf(TEXT("revision=%d type=%d primaryPlaying=%d waterPlaying=%d waterFlowAudibility01=%.4f snapshotAge=%.2f snapshotDuration=%.2f burningLandFraction=%.4f burningFuelRatio=%.4f localDetailGain=%.3f spatialSampleChecks=%d spatialLandSamples=%d focusCell=%d,%d acceptedPhysicalProgress=%d physicalPlaybackSeconds=%.6f sourceElapsedSeconds=%.6f sourceDurationSeconds=%.6f movingMassFraction=%.6f publishedHydraulicSeconds=%.6f loopVoiceLimit=2 audioEmittersPerCell=0 listenerResolved=%d listenerDistanceCm=%.2f overviewBlend=%.4f perspectiveCityGain=%.4f perspectiveDetailGain=%.4f perspectiveHazardGain=%.4f primaryTargetGain=%.4f waterTargetGain=%.4f lowPassHz=%.1f cityDuckGain=%.4f"),
		ActiveSnapshot.Revision, static_cast<int32>(ActiveSnapshot.Type), PrimaryLayer && PrimaryLayer->IsPlaying(), WaterLayer && WaterLayer->IsPlaying(), WaterCoverage,
		GetWorld() ? GetWorld()->GetTimeSeconds() - ActiveSnapshot.StartedAtWorldSeconds : 0.f, AudioDuration,
		BurningCoverage, BurningFuelRatio, LocalDetailGain, SpatialSampleChecks, SpatialLandSamples, AudioFocusCell.X, AudioFocusCell.Y,
		bAccepted, Playback, SourceElapsed, SourceDuration, Moving, Simulation ? Simulation->GetPublishedHydraulicSeconds() : 0.,
		AudioPerspective.bHasListener, AudioPerspective.DistanceCm, AudioPerspective.OverviewBlend,
		AudioPerspective.CityGain, AudioPerspective.DetailGain, AudioPerspective.HazardGain,
		PrimaryTargetGain, WaterTargetGain, AppliedLowPassHz, CityDuckGain);
}

void UDisasterAudioBridgeSubsystem::StopSoundscape()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(SoundscapeTimer);
		if (UGameInstance* Instance = World->GetGameInstance())
			if (UAftershockAudioSubsystem* Audio = Instance->GetSubsystem<UAftershockAudioSubsystem>()) Audio->SetAmbienceVolume(.6f);
	}
	for (UAudioComponent* Component : { PrimaryLayer.Get(), WaterLayer.Get() })
		if (IsValid(Component)) { Component->Stop(); Component->DestroyComponent(); }
	if (AudioDuration > 0.f) UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_DISASTER_AUDIO_STOP revision=%d"), ActiveSnapshot.Revision);
	PrimaryLayer = nullptr; WaterLayer = nullptr; AudioDuration = 0.f; WaterCoverage = 0.f;
	BurningCoverage = BurningFuelRatio = 0.f; SpatialSampleChecks = SpatialLandSamples = 0;
	LocalDetailGain = PrimaryTargetGain = WaterTargetGain = 0.f;
	LastPrimaryAppliedTarget = LastWaterAppliedTarget = -1.f;
	AudioPerspective = FAftershockAudioPerspective(); AppliedLowPassHz = 20000.f; CityDuckGain = .6f;
}

void UDisasterAudioBridgeSubsystem::Deinitialize()
{
	StopSoundscape();
	if (UDisasterSimulationSubsystem* Simulation = GetWorld() ? GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr)
		Simulation->OnDisasterVisualSnapshotChanged.Remove(SnapshotHandle);
	LoopConcurrency = nullptr;
	Super::Deinitialize();
}
