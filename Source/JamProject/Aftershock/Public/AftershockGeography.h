#pragma once

#include "CoreMinimal.h"
#include "Aftershock/Public/AftershockRegionRules.h"

/** Real WGS84 anchors. V1 uses authored compressed neighbourhoods; V2 uses bounded metre-scale DEM/OSM source windows. */
struct JAMPROJECT_API FAftershockGeographicLocation
{
	const TCHAR* Id;
	const TCHAR* Label;
	double Latitude;
	double Longitude;
	double HalfLatitudeExtent;
	double HalfLongitudeExtent;
	/** Stable local-focus key; Id remains the region key for legacy callers. */
	const TCHAR* FocusId = TEXT("");
	const TCHAR* FocusLabel = TEXT("");
};

JAMPROJECT_API int32 GetAftershockGeographyCount();
/** Strict stable-ID lookup. Invalid keys return INDEX_NONE, never a different region. */
JAMPROJECT_API int32 FindAftershockGeographyIndex(FName RegionId);
JAMPROJECT_API const FAftershockGeographicLocation& GetAftershockGeography(int32 ScenarioIndex);
JAMPROJECT_API int32 GetAftershockGeographyFocusCount(int32 RegionIndex);
/** NEW sessions only. Explicit existing focus IDs/save coordinates are never redirected. */
JAMPROJECT_API int32 GetAftershockDefaultNewGameFocusIndex(int32 RegionIndex);
/** Stable index into the25 immutable provider source windows. A new focus may
 * share a source bbox/payload while retaining its own geographic anchor. */
JAMPROJECT_API int32 GetAftershockFocusSourceIndex(int32 RegionIndex,int32 FocusIndex);
JAMPROJECT_API int32 FindAftershockGeographyFocusIndex(int32 RegionIndex, FName FocusId);
/** Strict getter for load/UI validation. Null on any invalid index. */
JAMPROJECT_API const FAftershockGeographicLocation* TryGetAftershockGeographyFocus(int32 RegionIndex, int32 FocusIndex);
/** Checked callers may use this convenience getter; invalid input falls back to region 0/focus 0. */
JAMPROJECT_API const FAftershockGeographicLocation& GetAftershockGeographyFocus(int32 RegionIndex, int32 FocusIndex = 0);
/** True only when a validated, finite 9x9 provider response is compiled for this focus. */
JAMPROJECT_API bool IsAftershockGeographyFocusReady(int32 RegionIndex, int32 FocusIndex = 0);
JAMPROJECT_API float SampleAftershockElevationMetres(int32 ScenarioIndex, float WestToEast01, float NorthToSouth01);
JAMPROJECT_API float SampleAftershockFocusElevationMetres(int32 RegionIndex, int32 FocusIndex, float WestToEast01, float NorthToSouth01);
/** Active-city tangent frame: +X east, +Y south, +Z through its actual geographic anchor. */
JAMPROJECT_API FVector AftershockLatLonToGlobeNormal(double Latitude, double Longitude, int32 AnchorScenarioIndex);

/** Derived from a bounded offline DEM/OSM source footprint, never a forecast or soil survey. */
struct JAMPROJECT_API FAftershockGeographicTerrainSample
{
	float ElevationMetres=0.f; // Source vertical datum/mean-sea-level metres, no min/max normalization.
	float RiverDistanceMetres=6553.5f;
	float SignedCoastDistanceMetres=3276.7f; // Positive land, negative ocean; bounded at +/-3276.7 m.
	uint8 SurfaceFlags=0; // 1 ocean, 2 water, 4 forest, 8 agriculture, 16 unspecified/perennial channel; detail-only32 intermittent channel.
	bool bOcean=false;
	bool bInlandWater=false;
	bool bMappedForest=false;
	bool bMappedAgriculture=false;
	bool bHasCoastlineSource=false;
	bool bIntermittentWaterway=false; // Explicit OSM seasonal/intermittent source tag; does not assert current flow.
};

/** Version-2 real-distance source, separate from the unchanged legacy sparse DEM. */
JAMPROJECT_API bool IsAftershockFocusGeodataReady(int32 RegionIndex,int32 FocusIndex=0);
JAMPROJECT_API bool GetAftershockFocusGeodataBounds(int32 RegionIndex,int32 FocusIndex,FAftershockGeographicBounds& OutBounds);
JAMPROJECT_API bool TrySampleAftershockFocusGeodata(int32 RegionIndex,int32 FocusIndex,double Latitude,double Longitude,FAftershockGeographicTerrainSample& OutSample);
/** Higher-sampling-density real source for new V3 terrain. Unchanged base65 API remains the V2 save authority. */
JAMPROJECT_API bool IsAftershockFocusDetailGeodataReady(int32 RegionIndex,int32 FocusIndex=0);
JAMPROJECT_API bool GetAftershockFocusDetailGeodataBounds(int32 RegionIndex,int32 FocusIndex,FAftershockGeographicBounds& OutBounds);
JAMPROJECT_API bool TrySampleAftershockFocusDetailGeodata(int32 RegionIndex,int32 FocusIndex,double Latitude,double Longitude,FAftershockGeographicTerrainSample& OutSample);
/** Wider factual regional overview, generalized coastline; never a construction/physics fallback. */
JAMPROJECT_API bool GetAftershockRegionalContextBounds(int32 RegionIndex,FAftershockGeographicBounds& OutBounds);
JAMPROJECT_API bool TrySampleAftershockRegionalContext(int32 RegionIndex,double Latitude,double Longitude,FAftershockGeographicTerrainSample& OutSample);
/** Local WGS84 tangent approximation in metres: +X east, +Y south; exact anchor and scale. */
JAMPROJECT_API bool AftershockFocusLatLonToLocalMetres(int32 RegionIndex,int32 FocusIndex,double Latitude,double Longitude,FVector2D& OutMetres);
JAMPROJECT_API bool AftershockFocusLocalMetresToLatLon(int32 RegionIndex,int32 FocusIndex,FVector2D Metres,double& OutLatitude,double& OutLongitude);
JAMPROJECT_API const TCHAR* GetAftershockGeodataAttribution();
/** Full redistribution notices for credits/help; the compact map footer is insufficient for modified Copernicus data. */
JAMPROJECT_API const TCHAR* GetAftershockGeodataLicenseNotice();

struct JAMPROJECT_API FAftershockGeographicCacheStats
{
	int32 ReadyTiles=0;
	int32 AttemptedTiles=0;
	int64 DecodedAllocatedBytes=0;
	int64 CompiledCompressedBytes=0;
	int32 DetailReadyTiles=0;
	int32 DetailAttemptedTiles=0;
	int64 DetailDecodedAllocatedBytes=0;
	int64 DetailCompiledCompressedBytes=0;
	int32 OverviewResidentTiles=0;
	int64 OverviewDecodedAllocatedBytes=0;
	int64 OverviewCompiledCompressedBytes=0;
};
/** Read-only counters; never decode a tile, generate a cell or start a timer. */
JAMPROJECT_API FAftershockGeographicCacheStats GetAftershockGeodataCacheStats();
JAMPROJECT_API void GetAftershockGeographicBasis(int32 AnchorScenarioIndex, FVector& East, FVector& South, FVector& Up);
JAMPROJECT_API FVector AftershockLatLonToFocusGlobeNormal(double Latitude, double Longitude, int32 RegionIndex, int32 FocusIndex);
JAMPROJECT_API void GetAftershockGeographicFocusBasis(int32 RegionIndex, int32 FocusIndex, FVector& East, FVector& South, FVector& Up);
