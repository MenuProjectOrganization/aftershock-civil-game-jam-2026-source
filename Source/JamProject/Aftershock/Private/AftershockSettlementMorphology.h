#pragma once

#include "CoreMinimal.h"
#include "Aftershock/Public/BuildingData.h"

struct FGridCell;

/** Conservative NEW-CITY authoring limits, not census, parcel zoning or OSM
 * building measurements. Source packets contain DEM/water/forest/agriculture,
 * not building footprints/levels. Recorded save layouts never pass through it.
 * Evidence/qualifications: docs/SETTLEMENT_MORPHOLOGY_SOURCES.json. */
struct FAftershockSettlementMorphology
{
	const TCHAR* Id=TEXT("bounded_town");
	int32 MaximumLots=450;
	int32 LotSpacingCells=2;
	int32 SlotKeepPercent=75;
	float SettlementRadiusMetres=750.f;
	float MaximumSlopeDegrees=24.f;
	float MaximumSourceElevationMetres=MAX_flt;
	int32 ApartmentFloorCap=6; // 0 excludes this catalogue type; its minimum is4.
	int32 OfficeFloorCap=6; // 0 excludes this catalogue type; its minimum is5.
	int32 PublicServiceFloorCap=3; // Explicit starter-service abstraction.
	int32 FactoryFloorCap=2;
	bool bRural=false;
	bool bIndustrial=false;
	bool bAllowHeavyIndustry=true;
	bool bCompactCore=false;
};

FAftershockSettlementMorphology GetAftershockSettlementMorphology(int32 RegionIndex,int32 FocusIndex);
bool IsAftershockMorphologyCellSuitable(const FAftershockSettlementMorphology& Profile,const FGridCell& Cell);
EBuildingType ResolveAftershockMorphologyBuildingType(const FAftershockSettlementMorphology& Profile,
	EBuildingType Requested,uint32 StableSeed,bool bEssentialCore=false);
int32 GetAftershockMorphologyFloorCap(const FAftershockSettlementMorphology& Profile,EBuildingType Type);
