#pragma once

#include "CoreMinimal.h"
#include "Aftershock/Public/BuildingData.h"

/** Authored map edge, not a sampled coastline or a storm-surge solver. */
enum class EAftershockCoastEdge : uint8 { None, North, East, South, West };

struct JAMPROJECT_API FAftershockGeographicBounds
{
	double West = 0.;
	double South = 0.;
	double East = 0.;
	double North = 0.;
};

/** Dimensionless authored scenario controls except ReliefCm and wind direction. */
struct JAMPROJECT_API FAftershockLandscapeProfile
{
	float ReliefCm = 500.f;
	float Moisture01 = .5f;
	float ForestFuel01 = .2f;
	float Agriculture01 = .2f;
	float FaultInfluence01 = .5f;
	float ValleyWindExposure01 = .3f;
	float WindDirectionDegrees = 0.f;
	float RiverCenter01 = .5f;
};

/** Local hydrological gates. A broad regional corridor is not a playable 250m map. */
struct JAMPROJECT_API FAftershockLocalFocusRules
{
	bool bHasCoast = false;
	bool bHasRiver = false;
	bool bHasVolcanicSource = false;
	EAftershockCoastEdge CoastEdge = EAftershockCoastEdge::None;
	FAftershockLandscapeProfile LandscapeProfile;
};

/** Immutable authored game policy; weights are NOT measured annual probabilities. */
struct JAMPROJECT_API FAftershockRegionRules
{
	static constexpr int32 HazardCount = 7;
	const TCHAR* RegionId = TEXT("");
	EDisasterType PrimaryHazard = EDisasterType::Earthquake;
	/** Existing EDisasterType order: Earthquake, Flood, Wind, Fire, Tsunami, Volcano, Drought. */
	float Weights[HazardCount] = {};
	/** These defaults describe focus 0. Use GetAftershockLocalFocusRules for another focus. */
	bool bHasCoast = false;
	bool bHasRiver = false;
	bool bHasVolcanicSource = false;
	bool bStormSurgeContext = false;
	FAftershockLandscapeProfile LandscapeProfile;
	FAftershockGeographicBounds RegionalBounds;
	double VolcanoLatitude = 0.;
	double VolcanoLongitude = 0.;
	const TCHAR* Qualification = TEXT("");
};

/** Invalid-index fallback is for legacy display only; use stable-ID lookup before loading. */
JAMPROJECT_API const FAftershockRegionRules& GetAftershockRegionRules(int32 RegionIndex);
JAMPROJECT_API const FAftershockLocalFocusRules& GetAftershockLocalFocusRules(int32 RegionIndex, int32 FocusIndex = 0);
/** A zero authored weight disables that hazard. Tsunami additionally needs local coast, volcano a mapped source. */
JAMPROJECT_API bool IsAftershockHazardAllowed(int32 RegionIndex, EDisasterType Hazard);
JAMPROJECT_API bool IsAftershockFocusHazardAllowed(int32 RegionIndex, int32 FocusIndex, EDisasterType Hazard);
JAMPROJECT_API float GetAftershockHazardWeight(int32 RegionIndex, EDisasterType Hazard, int32 FocusIndex = 0);
