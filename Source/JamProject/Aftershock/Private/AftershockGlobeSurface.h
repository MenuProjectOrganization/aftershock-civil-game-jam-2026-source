#pragma once

#include "CoreMinimal.h"

class AActor;
class UTerrainGridSubsystem;
class UStaticMeshComponent;
class UWorld;

/** Coordinate transform only; validates WGS84 but neither samples nor extends construction source coverage. Z is the tangent origin. */
bool AftershockGeographicPresentationWorld(const UTerrainGridSubsystem& Terrain,double Latitude,double Longitude,FVector& OutWorld);

struct FAftershockRegionalMeshBuildStats
{
	int32 PlanetBuilds=0,ContextBuilds=0,CachedOwners=0,ActiveMeshReferences=0;
};
/** Observes bounded weak caches without retaining or collecting any mesh. */
FAftershockRegionalMeshBuildStats GetAftershockRegionalMeshBuildStats(const UWorld* World);

/** The installed rectangle can contain missing/ocean cells: exclude only actual ground. */
bool IsAftershockInstalledGroundCoverage(const UTerrainGridSubsystem& Terrain,
	const FIntRect& InstalledDrawBounds, FIntPoint Coord);

/** Analytic sea-level tessellation only; never extends the bounded DEM provider. */
void GetAftershockGlobeLatitudeAngles(bool bRealGeography, TArray<double>& OutAngles);

/** Shared presentation scale consumed by the globe builder and its camera. */
struct FAftershockGlobeLayout
{
	FVector Center = FVector::ZeroVector;
	float RadiusCm = 200000.0f;
	float GridSpanCm = 25000.0f;
};

FAftershockGlobeLayout GetAftershockGlobeLayout(const UTerrainGridSubsystem& Terrain);

/** Natural Earth cartographic globe, rotated to the selected real geographic anchor.
 * City cap remains a compressed authored neighbourhood, never a 1:1 global map. */
bool BuildAftershockRegionalGlobe(AActor* Owner, const UTerrainGridSubsystem& Terrain,
	UStaticMeshComponent* GlobeComponent, UStaticMeshComponent* TransitionComponent,
	UStaticMeshComponent* AtmosphereComponent, UStaticMeshComponent* CloudComponent);
