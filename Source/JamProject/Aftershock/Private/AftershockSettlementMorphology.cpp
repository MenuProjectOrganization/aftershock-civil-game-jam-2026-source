#include "AftershockSettlementMorphology.h"

#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/AftershockRegionEnvironmentProfile.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"

FAftershockSettlementMorphology GetAftershockSettlementMorphology(int32 RegionIndex,int32 FocusIndex)
{
	FAftershockSettlementMorphology P;
	const auto* Focus=TryGetAftershockGeographyFocus(RegionIndex,FocusIndex);
	const FString Region=Focus?Focus->Id:TEXT("");
	const FString Local=Focus?Focus->FocusId:TEXT("");
	// These bounds describe deliberately authored playable settlements within a
	// 1.25km initial window. They are NOT measured municipal populations, legal
	// height limits, cadastral reconstruction or a demand to fill every slot.
	if(Region==TEXT("cyprus"))
	{
		P.bAllowHeavyIndustry=false;P.OfficeFloorCap=0;P.PublicServiceFloorCap=3;
		if(Local==TEXT("kalkanli"))
		{
			P.Id=TEXT("kalkanli_village");P.MaximumLots=140;P.LotSpacingCells=4;P.SlotKeepPercent=63;
			P.SettlementRadiusMetres=410.f;P.MaximumSlopeDegrees=19.f;P.ApartmentFloorCap=0;
			P.bRural=true;P.bCompactCore=true;
		}
		else if(Local==TEXT("alaykoy"))
		{
			P.Id=TEXT("alaykoy_periurban_industrial");P.MaximumLots=340;P.LotSpacingCells=3;P.SlotKeepPercent=77;
			P.SettlementRadiusMetres=660.f;P.MaximumSlopeDegrees=20.f;P.ApartmentFloorCap=4;
			P.bIndustrial=true;P.bAllowHeavyIndustry=true;P.FactoryFloorCap=2;
		}
		else
		{
			P.Id=TEXT("lefke_orchard_town");P.MaximumLots=270;P.LotSpacingCells=3;P.SlotKeepPercent=73;
			P.SettlementRadiusMetres=570.f;P.MaximumSlopeDegrees=23.f;P.ApartmentFloorCap=4;P.bRural=true;
		}
	}
	else if(Region==TEXT("miami_keys"))
	{
		if(Local==TEXT("miami"))
		{
			P.Id=TEXT("miami_central_business_district");P.MaximumLots=1000;P.SlotKeepPercent=94;
			P.SettlementRadiusMetres=1000.f;P.MaximumSlopeDegrees=20.f;P.ApartmentFloorCap=10;P.OfficeFloorCap=15;
			P.PublicServiceFloorCap=4;P.FactoryFloorCap=3;
		}
		else
		{
			P.bAllowHeavyIndustry=false;P.OfficeFloorCap=0;P.ApartmentFloorCap=0;P.MaximumSlopeDegrees=16.f;
			P.LotSpacingCells=3;P.SettlementRadiusMetres=680.f;
			if(Local==TEXT("key_largo")){P.Id=TEXT("key_largo_village_maritime");P.MaximumLots=210;P.SlotKeepPercent=67;P.bRural=true;}
			else if(Local==TEXT("marathon")){P.Id=TEXT("marathon_island_service_town");P.MaximumLots=190;P.SlotKeepPercent=71;}
			else {P.Id=TEXT("key_west_lowrise_town");P.MaximumLots=390;P.SlotKeepPercent=87;P.LotSpacingCells=2;}
		}
	}
	else if(Region==TEXT("etna"))
	{
		P.Id=TEXT("etna_rural_lower_flanks_no_summit_city");P.MaximumLots=110;P.LotSpacingCells=4;
		P.SlotKeepPercent=55;P.ApartmentFloorCap=0;P.OfficeFloorCap=0;P.bAllowHeavyIndustry=false;
		P.bRural=true;P.MaximumSlopeDegrees=20.f;P.MaximumSourceElevationMetres=1800.f;P.SettlementRadiusMetres=500.f;
		// Authored conservative exclusion, NOT an imported park zoning boundary.
		// The current summit-focused DEM window can correctly yield zero city lots.
	}
	else
	{
		const auto* Environment=TryGetAftershockRegionEnvironmentProfile(RegionIndex);
		const auto Biome=Environment?Environment->RegionalBiome:EAftershockRegionalBiome::TemperateUrban;
		if(Region==TEXT("atakent")||Region==TEXT("rotterdam")||Region==TEXT("jakarta"))
		{P.Id=TEXT("dense_urban_neighbourhood");P.MaximumLots=850;P.ApartmentFloorCap=10;P.OfficeFloorCap=10;P.SlotKeepPercent=90;P.SettlementRadiusMetres=950.f;}
		else if(Region==TEXT("konya"))
		{P.Id=TEXT("karapinar_agricultural_fringe");P.MaximumLots=250;P.LotSpacingCells=3;P.ApartmentFloorCap=4;P.OfficeFloorCap=0;P.bRural=true;}
		else if(Region==TEXT("grindavik"))
		{P.Id=TEXT("grindavik_lowrise_volcanic_town");P.MaximumLots=260;P.LotSpacingCells=3;P.ApartmentFloorCap=0;P.OfficeFloorCap=0;P.MaximumSlopeDegrees=20.f;P.MaximumSourceElevationMetres=400.f;}
		else if(Region==TEXT("vesuvius"))
		{P.Id=TEXT("torre_del_greco_lower_slopes");P.MaximumLots=500;P.ApartmentFloorCap=6;P.OfficeFloorCap=5;P.MaximumSourceElevationMetres=650.f;}
		else if(Biome==EAftershockRegionalBiome::ContinentalTaiga||Biome==EAftershockRegionalBiome::TemperateRainforest)
		{P.Id=TEXT("low_midrise_river_town");P.MaximumLots=380;P.LotSpacingCells=3;P.ApartmentFloorCap=5;P.OfficeFloorCap=5;}
		else if(Region==TEXT("dereyolu")||Region==TEXT("yelova")||Region==TEXT("mugla"))
		{P.Id=TEXT("mediterranean_hillside_town");P.MaximumLots=390;P.ApartmentFloorCap=5;P.OfficeFloorCap=5;P.MaximumSlopeDegrees=25.f;}
	}
	return P;
}

bool IsAftershockMorphologyCellSuitable(const FAftershockSettlementMorphology& P,const FGridCell& C)
{
	return C.bIsPlayable&&!C.bIsOcean&&!C.bIsRiverbed&&!C.bVolcanicVent&&!C.bHasDam
		&&!C.bBurning&&C.LavaCoverage01<=.1f&&FMath::IsFinite(C.Elevation)&&FMath::IsFinite(C.Slope)
		&&C.Slope<=P.MaximumSlopeDegrees&&C.ParcelSuitability>.02f
		&&(!C.bHasRealElevationSample||(FMath::IsFinite(C.RealElevationMetres)&&C.RealElevationMetres<=P.MaximumSourceElevationMetres));
}

EBuildingType ResolveAftershockMorphologyBuildingType(const FAftershockSettlementMorphology& P,EBuildingType Type,uint32 Seed,bool bEssentialCore)
{
	if(Type==EBuildingType::Apartment&&P.ApartmentFloorCap<4)return EBuildingType::House;
	if(Type==EBuildingType::Office&&P.OfficeFloorCap<5)
	{
		const EBuildingType SmallEmployment[]={EBuildingType::Bakery,EBuildingType::Restaurant,EBuildingType::Warehouse};
		return SmallEmployment[Seed%UE_ARRAY_COUNT(SmallEmployment)];
	}
	if(Type==EBuildingType::Factory&&!P.bAllowHeavyIndustry)return EBuildingType::Warehouse;
	// Core life-support types remain available as deliberately authored public
	// services, not assertions these exact institutions exist in this village.
	if(!bEssentialCore&&P.bRural&&Type==EBuildingType::PowerPlant)return EBuildingType::GreenSpace;
	return Type;
}

int32 GetAftershockMorphologyFloorCap(const FAftershockSettlementMorphology& P,EBuildingType Type)
{
	if(Type==EBuildingType::House)return 2;
	if(Type==EBuildingType::Apartment)return P.ApartmentFloorCap;
	if(Type==EBuildingType::Office)return P.OfficeFloorCap;
	if(Type==EBuildingType::Factory)return P.FactoryFloorCap;
	if(Type==EBuildingType::Hospital||Type==EBuildingType::School||Type==EBuildingType::FireStation)return P.PublicServiceFloorCap;
	if(Type==EBuildingType::TaxOffice||Type==EBuildingType::Courthouse||Type==EBuildingType::PoliceStation)return 2;
	return 2;
}
