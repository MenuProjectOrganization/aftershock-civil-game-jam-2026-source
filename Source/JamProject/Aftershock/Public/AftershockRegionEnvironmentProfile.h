#pragma once
#include "CoreMinimal.h"

/** Regional presentation context, never a parcel soil classification or hazard coefficient. */
enum class EAftershockRegionalBiome:uint8
{
	TemperateUrban,HumidBlackSea,Mediterranean,VolcanicMediterranean,SemiAridSteppe,
	TropicalMaritime,ContinentalTaiga,TemperateRainforest,TemperateCoastal,DeltaWetland,
	SubpolarVolcanic,MonsoonHighland,TropicalMonsoon
};

struct JAMPROJECT_API FAftershockRegionEnvironmentProfile
{
	const TCHAR* RegionId=TEXT("");
	EAftershockRegionalBiome RegionalBiome=EAftershockRegionalBiome::TemperateUrban;
	FLinearColor DrySoilTint=FLinearColor(.35f,.29f,.19f);
	FLinearColor RockTint=FLinearColor(.30f,.29f,.27f);
	const TCHAR* GeologySummary=TEXT("");
	const TCHAR* GeologySourceUrl=TEXT("");
};

/** NASA POWER/MERRA2 spatial-grid monthly climatology, not weather/station observations. */
struct JAMPROJECT_API FAftershockMonthlyClimate
{
	float T2MC=0.f;
	float PrecipMmPerDay=0.f;
	float RHPercent=0.f;
	float WindMps=0.f;
	float SourceElevationM=0.f;
	int32 StartYear=0,EndYear=0;
	float SourceGridLatitudeDegrees=.5f,SourceGridLongitudeDegrees=.625f;
};

JAMPROJECT_API const FAftershockRegionEnvironmentProfile* TryGetAftershockRegionEnvironmentProfile(int32 RegionIndex);
JAMPROJECT_API bool TryGetAftershockFocusMonthlyClimate(int32 RegionIndex,int32 FocusIndex,int32 Month1To12,FAftershockMonthlyClimate& Out);
/** NASA Glenn standard-atmosphere lapse-rate approximation for VISUAL elevation adaptation only.
 * Does not infer precipitation, snow storage, ground temperature, fire probability or real weather. */
JAMPROJECT_API bool TryEstimateAftershockVisualTemperatureAtElevation(const FAftershockMonthlyClimate& Climate,float ElevationMetres,float& OutTemperatureC);
