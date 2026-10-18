#include "Aftershock/Public/AftershockAudioPerspective.h"

#include "Aftershock/Game/AftershockPlayerController.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

FAftershockAudioPerspective MakeAftershockAudioPerspective(double DistanceCm, float OverviewBlend)
{
	FAftershockAudioPerspective Mix;
	if (!FMath::IsFinite(DistanceCm) || DistanceCm < 0. || !FMath::IsFinite(OverviewBlend)) return Mix;
	Mix.bHasListener = true;
	Mix.DistanceCm = static_cast<float>(FMath::Min(DistanceCm, double(MAX_flt)));
	Mix.OverviewBlend = FMath::Clamp(OverviewBlend, 0.f, 1.f);
	// Metres: 15, 80, 240, 800, 2500. Broad events carry further than
	// individual citizens, engines and splashes; no discontinuous LOD switch.
	struct FAudioKnot { float Cm, City, Detail, Hazard, Hz; };
	static constexpr FAudioKnot Knots[] = {
		{1500.f, 1.f, 1.f, 1.f, 20000.f},
		{8000.f, .92f, .80f, .95f, 13000.f},
		{24000.f, .65f, .35f, .75f, 6500.f},
		{80000.f, .22f, .04f, .32f, 2400.f},
		{250000.f, 0.f, 0.f, 0.f, 800.f}
	};
	int32 Upper = 1;
	while (Upper < UE_ARRAY_COUNT(Knots)-1 && DistanceCm > Knots[Upper].Cm) ++Upper;
	const auto& A = Knots[Upper-1]; const auto& B = Knots[Upper];
	const double T = FMath::Clamp((FMath::Loge(FMath::Max(1., DistanceCm))-FMath::Loge(double(A.Cm)))
		/ (FMath::Loge(double(B.Cm))-FMath::Loge(double(A.Cm))), 0., 1.);
	const float Ease = static_cast<float>(T*T*(3.-2.*T));
	const float GlobeGain = 1.f-FMath::SmoothStep(.02f, .20f, Mix.OverviewBlend);
	Mix.CityGain = FMath::Lerp(A.City, B.City, Ease)*GlobeGain;
	Mix.DetailGain = FMath::Lerp(A.Detail, B.Detail, Ease)*GlobeGain;
	Mix.HazardGain = FMath::Lerp(A.Hazard, B.Hazard, Ease)*GlobeGain;
	Mix.LowPassHz = FMath::Exp(FMath::Lerp(FMath::Loge(A.Hz), FMath::Loge(B.Hz), Ease));
	return Mix;
}

FAftershockAudioPerspective GetAftershockAudioPerspective(const UWorld* World)
{
	if (!World || (World->WorldType != EWorldType::Game && World->WorldType != EWorldType::PIE)) return {};
	const auto* PC = World->GetFirstPlayerController();
	const auto* Camera = PC ? Cast<AAftershockCameraPawn>(PC->GetPawn()) : nullptr;
	if (!Camera || !PC->PlayerCameraManager) return {};
	if (const auto* Controller = Cast<AAftershockPlayerController>(PC);
		Controller && (Controller->IsOpeningMenuVisible() || Controller->IsWaterGlassEpilogueVisible())) return {};
	// BeginPlay starts audio before ShowOpeningMenu; don't leak city audio in
	// that first frame, or while returning to a session's opening screen.
	if (const auto* Instance = World->GetGameInstance())
		if (const auto* Session = Instance->GetSubsystem<UAftershockOpeningSessionSubsystem>(); Session && !Session->HasLiveSession()) return {};
	const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
	const FVector Focus = Camera->GetLocalMapFocus();
	if (Eye.ContainsNaN() || Focus.ContainsNaN()) return {};
	return MakeAftershockAudioPerspective(FVector::Distance(Eye, Focus), Camera->GetPlanetOverviewBlend());
}
