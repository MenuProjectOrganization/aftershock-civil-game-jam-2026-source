#pragma once

#include "CoreMinimal.h"

class UWorld;

/** Read-only presentation choice. Never determines population, routes, exposure or save data. */
enum class EAftershockPresentationTier : uint8 { Street, City, Regional, Globe };

struct FAftershockPresentationBudget
{
	EAftershockPresentationTier Tier = EAftershockPresentationTier::Street;
	bool bDrawCitizens = true;
	bool bDrawVehicleDetails = true;
	bool bDrawDecorations = true;
	/** StaticMeshComponent convention: 0 = automatic, 1 = LOD0, 2 = LOD1. Clamp to actual asset LODs. */
	int32 PropLODFloor = 0;
	/** Relative to the exact half-cell terrain LOD0, not to canonical grid resolution. */
	int32 GeometryStride = 1;
	float ViewDistanceCm = 0.f;
	float OverviewBlend = 0.f;
};

/** Pure numeric policy, also usable by native fixtures without constructing a renderer. */
FAftershockPresentationBudget MakeAftershockPresentationBudget(float ViewDistanceCm, float OverviewBlend);
/** No usable camera/controller returns full Street detail, preserving headless fixture semantics. */
FAftershockPresentationBudget GetAftershockPresentationBudget(const UWorld* World);

