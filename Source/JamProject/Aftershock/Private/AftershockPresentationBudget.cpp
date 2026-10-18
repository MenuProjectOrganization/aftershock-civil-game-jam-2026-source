#include "AftershockPresentationBudget.h"

#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

FAftershockPresentationBudget MakeAftershockPresentationBudget(float ViewDistanceCm, float OverviewBlend)
{
	FAftershockPresentationBudget Budget;
	if (!FMath::IsFinite(ViewDistanceCm) || ViewDistanceCm < 0.f || !FMath::IsFinite(OverviewBlend)) return Budget;
	Budget.ViewDistanceCm = ViewDistanceCm;
	Budget.OverviewBlend = FMath::Clamp(OverviewBlend, 0.f, 1.f);
	if (Budget.OverviewBlend >= .2f) Budget.Tier = EAftershockPresentationTier::Globe;
	else if (ViewDistanceCm >= 100000.f) Budget.Tier = EAftershockPresentationTier::Regional;
	else if (ViewDistanceCm >= 16000.f) Budget.Tier = EAftershockPresentationTier::City;
	switch (Budget.Tier)
	{
	case EAftershockPresentationTier::City:
		Budget.PropLODFloor = 2; Budget.GeometryStride = 2; break;
	case EAftershockPresentationTier::Regional:
		Budget.bDrawCitizens = Budget.bDrawVehicleDetails = Budget.bDrawDecorations = false;
		Budget.PropLODFloor = 3; Budget.GeometryStride = 4; break;
	case EAftershockPresentationTier::Globe:
		Budget.bDrawCitizens = Budget.bDrawVehicleDetails = Budget.bDrawDecorations = false;
		Budget.PropLODFloor = 4; Budget.GeometryStride = 8; break;
	default: break;
	}
	return Budget;
}

FAftershockPresentationBudget GetAftershockPresentationBudget(const UWorld* World)
{
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const AAftershockCameraPawn* Camera = PC ? Cast<AAftershockCameraPawn>(PC->GetPawn()) : nullptr;
	if (!Camera || !PC->PlayerCameraManager) return {};
	const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
	const FVector Focus = Camera->GetLocalMapFocus();
	if (Eye.ContainsNaN() || Focus.ContainsNaN()) return {};
	return MakeAftershockPresentationBudget(static_cast<float>(FVector::Distance(Eye, Focus)), Camera->GetPlanetOverviewBlend());
}
