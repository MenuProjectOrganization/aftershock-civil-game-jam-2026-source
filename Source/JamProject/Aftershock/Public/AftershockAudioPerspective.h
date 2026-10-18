#pragma once

#include "CoreMinimal.h"

class UWorld;

/** Derived presentation only. No audio value is saved or written to simulation. */
struct FAftershockAudioPerspective
{
	bool bHasListener = false;
	float DistanceCm = 0.f;
	float OverviewBlend = 0.f;
	float CityGain = 0.f;
	float DetailGain = 0.f;
	float HazardGain = 0.f;
	float LowPassHz = 20000.f;
};

/** Continuous log-distance curves; exact silence at the existing globe tier. */
JAMPROJECT_API FAftershockAudioPerspective MakeAftershockAudioPerspective(double DistanceCm, float OverviewBlend);
/** Actual player camera to canonical local focus; missing/menu listeners are silent. */
JAMPROJECT_API FAftershockAudioPerspective GetAftershockAudioPerspective(const UWorld* World);
