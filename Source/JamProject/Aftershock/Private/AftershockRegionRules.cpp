#include "Aftershock/Public/AftershockRegionRules.h"
#include "Aftershock/Public/AftershockGeography.h"

bool IsAftershockFocusHazardAllowed(int32 RegionIndex, int32 FocusIndex, EDisasterType Hazard)
{
	const int32 TypeIndex = static_cast<int32>(Hazard);
	if (TypeIndex < 0 || TypeIndex > static_cast<int32>(EDisasterType::Landslide) ||
		!IsAftershockGeographyFocusReady(RegionIndex, FocusIndex)) return false;
	// These two authored material hazards have no historical seven-weight array
	// slot. This is only a static region gate: actual snow/soil/slope feasibility
	// MUST be checked by the canonical Simulation API before warning/selection.
	if(Hazard==EDisasterType::Avalanche||Hazard==EDisasterType::Landslide)return true;
	if(TypeIndex>=FAftershockRegionRules::HazardCount)return false;
	const FAftershockRegionRules& Rules = GetAftershockRegionRules(RegionIndex);
	const float Weight = Rules.Weights[TypeIndex];
	if (!FMath::IsFinite(Weight) || Weight <= 0.f) return false;
	const FAftershockLocalFocusRules& Local = GetAftershockLocalFocusRules(RegionIndex, FocusIndex);
	if (Hazard == EDisasterType::Tsunami && !Local.bHasCoast) return false;
	if (Hazard == EDisasterType::Volcano && !Local.bHasVolcanicSource) return false;
	if (Hazard == EDisasterType::Fire && Local.LandscapeProfile.ForestFuel01 <= 0.f) return false;
	// Rain-driven surface flooding does not require a permanent river.
	return true;
}

bool IsAftershockHazardAllowed(int32 RegionIndex, EDisasterType Hazard)
{
	return IsAftershockFocusHazardAllowed(RegionIndex, 0, Hazard);
}

float GetAftershockHazardWeight(int32 RegionIndex, EDisasterType Hazard, int32 FocusIndex)
{
	if(Hazard==EDisasterType::Avalanche||Hazard==EDisasterType::Landslide)
		return IsAftershockFocusHazardAllowed(RegionIndex,FocusIndex,Hazard)?1.f:0.f;
	return IsAftershockFocusHazardAllowed(RegionIndex, FocusIndex, Hazard)
		? GetAftershockRegionRules(RegionIndex).Weights[static_cast<int32>(Hazard)] : 0.f;
}
