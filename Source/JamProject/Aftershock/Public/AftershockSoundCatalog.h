// AFTERSHOCK — Civil Game Jam 2026
// Comprehensive ElevenLabs Sound Effects Catalog & Lookup Utilities
// Maps all 93 studio-engineered audio assets & variations to core gameplay enums.
// Implements Industry Audio Design Best Practices (SFX Engine, GameAnalytics, SAE Institute).

#pragma once

#include "CoreMinimal.h"
#include "Aftershock/Public/BuildingData.h"

namespace AftershockElevenLabsPaths
{
	// -------- 24 BUILDINGS OPERATIONAL (48kHz Mono Loops — Calibrated to -22.5 dBFS RMS) --------
	static const TCHAR* BLD_House           = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_01_House_Domestic.SFX_BLD_01_House_Domestic");
	static const TCHAR* BLD_Apartment       = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_02_Apartment_Lobby.SFX_BLD_02_Apartment_Lobby");
	static const TCHAR* BLD_Office          = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_03_Office_HVAC.SFX_BLD_03_Office_HVAC");
	static const TCHAR* BLD_Factory         = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_04_Factory_Industrial.SFX_BLD_04_Factory_Industrial");
	static const TCHAR* BLD_Hospital        = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_05_Hospital_Medical.SFX_BLD_05_Hospital_Medical");
	static const TCHAR* BLD_School          = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_06_School_Courtyard.SFX_BLD_06_School_Courtyard");
	static const TCHAR* BLD_FireStation     = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_07_FireStation_Ready.SFX_BLD_07_FireStation_Ready");
	static const TCHAR* BLD_PowerPlant      = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_08_PowerPlant_Turbine.SFX_BLD_08_PowerPlant_Turbine");
	static const TCHAR* BLD_GreenSpace      = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_09_GreenSpace_Park.SFX_BLD_09_GreenSpace_Park");
	static const TCHAR* BLD_WindTurbine     = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_10_WindTurbine_Rotor.SFX_BLD_10_WindTurbine_Rotor");
	static const TCHAR* BLD_Shelter         = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_11_Shelter_Bunker.SFX_BLD_11_Shelter_Bunker");
	static const TCHAR* BLD_Farm            = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_12_Farm_Fields.SFX_BLD_12_Farm_Fields");
	static const TCHAR* BLD_LivestockFarm   = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_13_Livestock_Barn.SFX_BLD_13_Livestock_Barn");
	static const TCHAR* BLD_Generator       = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_14_Generator_Diesel.SFX_BLD_14_Generator_Diesel");
	static const TCHAR* BLD_Bakery          = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_15_Bakery_Oven.SFX_BLD_15_Bakery_Oven");
	static const TCHAR* BLD_Restaurant      = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_16_Restaurant_Kitchen.SFX_BLD_16_Restaurant_Kitchen");
	static const TCHAR* BLD_TaxOffice       = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_17_TaxOffice_Bureau.SFX_BLD_17_TaxOffice_Bureau");
	static const TCHAR* BLD_Courthouse      = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_18_Courthouse_Hall.SFX_BLD_18_Courthouse_Hall");
	static const TCHAR* BLD_PoliceStation   = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_19_PoliceStation_Dispatch.SFX_BLD_19_PoliceStation_Dispatch");
	static const TCHAR* BLD_BusTerminal     = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_20_BusTerminal_Depot.SFX_BLD_20_BusTerminal_Depot");
	static const TCHAR* BLD_BusStop         = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_21_BusStop_Shelter.SFX_BLD_21_BusStop_Shelter");
	static const TCHAR* BLD_WaterPlant      = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_22_WaterPlant_Pumps.SFX_BLD_22_WaterPlant_Pumps");
	static const TCHAR* BLD_Warehouse       = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_23_Warehouse_Logistics.SFX_BLD_23_Warehouse_Logistics");
	static const TCHAR* BLD_Substation      = TEXT("/Game/Aftershock/Audio/ElevenLabs/Buildings/SFX_BLD_24_Substation_Electrical.SFX_BLD_24_Substation_Electrical");

	// -------- 7 DISASTERS (Continuous Beds + Transient Strikes — Protected Sub-Bass Lane) --------
	static const TCHAR* DIS_Quake_Bed       = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_01_Earthquake_TectonicBed.SFX_DIS_01_Earthquake_TectonicBed");
	static const TCHAR* DIS_Quake_Rupture   = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_01_Earthquake_FaultRupture.SFX_DIS_01_Earthquake_FaultRupture");
	static const TCHAR* DIS_Flood_Bed       = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_02_Flood_TorrentBed.SFX_DIS_02_Flood_TorrentBed");
	static const TCHAR* DIS_Flood_Breach    = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_02_Flood_LeveeBreach.SFX_DIS_02_Flood_LeveeBreach");
	static const TCHAR* DIS_Storm_Bed       = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_03_Storm_TempestBed.SFX_DIS_03_Storm_TempestBed");
	static const TCHAR* DIS_Storm_Impact    = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_03_Storm_DebrisImpact.SFX_DIS_03_Storm_DebrisImpact");
	static const TCHAR* DIS_Fire_Bed        = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_04_Fire_WildfireBed.SFX_DIS_04_Fire_WildfireBed");
	static const TCHAR* DIS_Fire_Ignition   = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_04_Fire_ExplosiveIgnition.SFX_DIS_04_Fire_ExplosiveIgnition");
	static const TCHAR* DIS_Tsunami_Bed     = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_05_Tsunami_OceanSurgeBed.SFX_DIS_05_Tsunami_OceanSurgeBed");
	static const TCHAR* DIS_Tsunami_Impact  = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_05_Tsunami_CoastImpact.SFX_DIS_05_Tsunami_CoastImpact");
	static const TCHAR* DIS_Volcano_Bed     = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_06_Volcano_PyroclasticBed.SFX_DIS_06_Volcano_PyroclasticBed");
	static const TCHAR* DIS_Volcano_Blast   = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_06_Volcano_DetonationBlast.SFX_DIS_06_Volcano_DetonationBlast");
	static const TCHAR* DIS_Drought_Bed     = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_07_Drought_AridWindBed.SFX_DIS_07_Drought_AridWindBed");
	static const TCHAR* DIS_Drought_Stress  = TEXT("/Game/Aftershock/Audio/ElevenLabs/Disasters/SFX_DIS_07_Drought_SoilStress.SFX_DIS_07_Drought_SoilStress");

	// -------- MATERIALS & STRUCTURAL DESTRUCTION (48kHz Mono) --------
	static const TCHAR* MAT_Concrete_Crack    = TEXT("/Game/Aftershock/Audio/ElevenLabs/Materials/SFX_MAT_Concrete_Crack.SFX_MAT_Concrete_Crack");
	static const TCHAR* MAT_Concrete_Collapse = TEXT("/Game/Aftershock/Audio/ElevenLabs/Materials/SFX_MAT_Concrete_Collapse.SFX_MAT_Concrete_Collapse");
	static const TCHAR* MAT_Wood_Strain       = TEXT("/Game/Aftershock/Audio/ElevenLabs/Materials/SFX_MAT_Wood_Strain.SFX_MAT_Wood_Strain");
	static const TCHAR* MAT_Wood_Fracture     = TEXT("/Game/Aftershock/Audio/ElevenLabs/Materials/SFX_MAT_Wood_Fracture.SFX_MAT_Wood_Fracture");
	static const TCHAR* MAT_Steel_Groan       = TEXT("/Game/Aftershock/Audio/ElevenLabs/Materials/SFX_MAT_Steel_Groan.SFX_MAT_Steel_Groan");
	static const TCHAR* MAT_Steel_Buckle      = TEXT("/Game/Aftershock/Audio/ElevenLabs/Materials/SFX_MAT_Steel_Buckle.SFX_MAT_Steel_Buckle");
	static const TCHAR* MAT_Glass_Shatter     = TEXT("/Game/Aftershock/Audio/ElevenLabs/Materials/SFX_MAT_Glass_Shatter.SFX_MAT_Glass_Shatter");
	static const TCHAR* MAT_Foundation_Sink   = TEXT("/Game/Aftershock/Audio/ElevenLabs/Materials/SFX_MAT_Foundation_Liquefy.SFX_MAT_Foundation_Liquefy");

	// -------- VEHICLES & INFRASTRUCTURE --------
	static const TCHAR* VEH_Ambulance_Siren   = TEXT("/Game/Aftershock/Audio/ElevenLabs/Vehicles/SFX_VEH_Ambulance_Siren.SFX_VEH_Ambulance_Siren");
	static const TCHAR* VEH_FireEngine_Siren  = TEXT("/Game/Aftershock/Audio/ElevenLabs/Vehicles/SFX_VEH_FireEngine_Siren.SFX_VEH_FireEngine_Siren");
	static const TCHAR* VEH_Police_Siren      = TEXT("/Game/Aftershock/Audio/ElevenLabs/Vehicles/SFX_VEH_Police_Siren.SFX_VEH_Police_Siren");
	static const TCHAR* VEH_Bus_Idle          = TEXT("/Game/Aftershock/Audio/ElevenLabs/Vehicles/SFX_VEH_Bus_EngineIdle.SFX_VEH_Bus_EngineIdle");
	static const TCHAR* VEH_Bus_AirBrake      = TEXT("/Game/Aftershock/Audio/ElevenLabs/Vehicles/SFX_VEH_Bus_AirBrakeDoor.SFX_VEH_Bus_AirBrakeDoor");
	static const TCHAR* INF_Road_Pave         = TEXT("/Game/Aftershock/Audio/ElevenLabs/Vehicles/SFX_INF_Road_PaveAsphalt.SFX_INF_Road_PaveAsphalt");
	static const TCHAR* INF_Road_Demolish     = TEXT("/Game/Aftershock/Audio/ElevenLabs/Vehicles/SFX_INF_Road_Demolish.SFX_INF_Road_Demolish");
	static const TCHAR* INF_Bridge_Construct  = TEXT("/Game/Aftershock/Audio/ElevenLabs/Vehicles/SFX_INF_Bridge_Construct.SFX_INF_Bridge_Construct");
	static const TCHAR* INF_Bridge_Collapse   = TEXT("/Game/Aftershock/Audio/ElevenLabs/Vehicles/SFX_INF_Bridge_Collapse.SFX_INF_Bridge_Collapse");

	// -------- CITIZENS & MASS AI FOLEY (With 4x Round-Robin Variations) --------
	static const TCHAR* CIT_Footstep_Sidewalk = TEXT("/Game/Aftershock/Audio/ElevenLabs/Citizens/SFX_CIT_Footstep_Sidewalk.SFX_CIT_Footstep_Sidewalk");
	static const TCHAR* CIT_Footstep_Sidewalk_Vars[4] = {
		TEXT("/Game/Aftershock/Audio/ElevenLabs/Citizens/SFX_CIT_Footstep_Sidewalk_01.SFX_CIT_Footstep_Sidewalk_01"),
		TEXT("/Game/Aftershock/Audio/ElevenLabs/Citizens/SFX_CIT_Footstep_Sidewalk_02.SFX_CIT_Footstep_Sidewalk_02"),
		TEXT("/Game/Aftershock/Audio/ElevenLabs/Citizens/SFX_CIT_Footstep_Sidewalk_03.SFX_CIT_Footstep_Sidewalk_03"),
		TEXT("/Game/Aftershock/Audio/ElevenLabs/Citizens/SFX_CIT_Footstep_Sidewalk_04.SFX_CIT_Footstep_Sidewalk_04")
	};

	static const TCHAR* CIT_Footstep_Rubble   = TEXT("/Game/Aftershock/Audio/ElevenLabs/Citizens/SFX_CIT_Footstep_Rubble.SFX_CIT_Footstep_Rubble");
	static const TCHAR* CIT_Footstep_Rubble_Vars[4] = {
		TEXT("/Game/Aftershock/Audio/ElevenLabs/Citizens/SFX_CIT_Footstep_Rubble_01.SFX_CIT_Footstep_Rubble_01"),
		TEXT("/Game/Aftershock/Audio/ElevenLabs/Citizens/SFX_CIT_Footstep_Rubble_02.SFX_CIT_Footstep_Rubble_02"),
		TEXT("/Game/Aftershock/Audio/ElevenLabs/Citizens/SFX_CIT_Footstep_Rubble_03.SFX_CIT_Footstep_Rubble_03"),
		TEXT("/Game/Aftershock/Audio/ElevenLabs/Citizens/SFX_CIT_Footstep_Rubble_04.SFX_CIT_Footstep_Rubble_04")
	};

	static const TCHAR* CIT_Evac_Run          = TEXT("/Game/Aftershock/Audio/ElevenLabs/Citizens/SFX_CIT_Evacuation_Run.SFX_CIT_Evacuation_Run");
	static const TCHAR* CIT_Crowd_Peaceful    = TEXT("/Game/Aftershock/Audio/ElevenLabs/Citizens/SFX_CIT_Crowd_Peaceful.SFX_CIT_Crowd_Peaceful");
	static const TCHAR* CIT_Crowd_Panic       = TEXT("/Game/Aftershock/Audio/ElevenLabs/Citizens/SFX_CIT_Crowd_PanicScramble.SFX_CIT_Crowd_PanicScramble");

	// -------- UI & CITY MANAGEMENT (48kHz Stereo With Variations) --------
	static const TCHAR* UI_Hover              = TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Hover_Tock.SFX_UI_Hover_Tock");
	static const TCHAR* UI_Hover_Vars[4] = {
		TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Hover_Tock_01.SFX_UI_Hover_Tock_01"),
		TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Hover_Tock_02.SFX_UI_Hover_Tock_02"),
		TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Hover_Tock_03.SFX_UI_Hover_Tock_03"),
		TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Hover_Tock_04.SFX_UI_Hover_Tock_04")
	};

	static const TCHAR* UI_Click              = TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Click_Select.SFX_UI_Click_Select");
	static const TCHAR* UI_Click_Vars[4] = {
		TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Click_Select_01.SFX_UI_Click_Select_01"),
		TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Click_Select_02.SFX_UI_Click_Select_02"),
		TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Click_Select_03.SFX_UI_Click_Select_03"),
		TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Click_Select_04.SFX_UI_Click_Select_04")
	};

	static const TCHAR* UI_Rotate             = TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Ghost_Rotate.SFX_UI_Ghost_Rotate");
	static const TCHAR* UI_Rotate_Vars[4] = {
		TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Ghost_Rotate_01.SFX_UI_Ghost_Rotate_01"),
		TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Ghost_Rotate_02.SFX_UI_Ghost_Rotate_02"),
		TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Ghost_Rotate_03.SFX_UI_Ghost_Rotate_03"),
		TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Ghost_Rotate_04.SFX_UI_Ghost_Rotate_04")
	};

	static const TCHAR* UI_Place_Valid        = TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Place_Valid.SFX_UI_Place_Valid");
	static const TCHAR* UI_Place_Invalid      = TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Place_Invalid.SFX_UI_Place_Invalid");
	static const TCHAR* UI_Demolish           = TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Demolish_Bulldoze.SFX_UI_Demolish_Bulldoze");
	static const TCHAR* UI_Budget_Income      = TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Budget_Income.SFX_UI_Budget_Income");
	static const TCHAR* UI_Budget_Deficit     = TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Budget_Deficit.SFX_UI_Budget_Deficit");
	static const TCHAR* UI_Quiz_Correct       = TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Quiz_Correct.SFX_UI_Quiz_Correct");
	static const TCHAR* UI_Quiz_Incorrect     = TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Quiz_Incorrect.SFX_UI_Quiz_Incorrect");
	static const TCHAR* UI_EarlyWarning       = TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_EarlyWarning_Klaxon.SFX_UI_EarlyWarning_Klaxon");
	static const TCHAR* UI_Outcome_Victory    = TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Outcome_Victory.SFX_UI_Outcome_Victory");
	static const TCHAR* UI_Outcome_Defeat     = TEXT("/Game/Aftershock/Audio/ElevenLabs/UI/SFX_UI_Outcome_Defeat.SFX_UI_Outcome_Defeat");
}

class JAMPROJECT_API FAftershockSoundCatalog
{
public:
	// Dynamic Spatial & Sound Cue Configuration Parameters (Industry Guidelines)
	static constexpr float AttenuationInnerRadius = 300.0f;  // Full volume within 3 meters
	static constexpr float AttenuationOuterRadius = 3500.0f; // Soft logarithmic rolloff to 35 meters
	static constexpr float RandomPitchMin = 0.94f;           // -1.05 semitones
	static constexpr float RandomPitchMax = 1.06f;           // +1.01 semitones
	static constexpr float RandomVolumeMin = 0.92f;          // -0.7 dB
	static constexpr float RandomVolumeMax = 1.04f;          // +0.3 dB

	static const TCHAR* GetBuildingOperationalSound(EBuildingType Type)
	{
		using namespace AftershockElevenLabsPaths;
		switch (Type)
		{
		case EBuildingType::House:          return BLD_House;
		case EBuildingType::Apartment:      return BLD_Apartment;
		case EBuildingType::Office:         return BLD_Office;
		case EBuildingType::Factory:        return BLD_Factory;
		case EBuildingType::Hospital:       return BLD_Hospital;
		case EBuildingType::School:         return BLD_School;
		case EBuildingType::FireStation:    return BLD_FireStation;
		case EBuildingType::PowerPlant:     return BLD_PowerPlant;
		case EBuildingType::GreenSpace:     return BLD_GreenSpace;
		case EBuildingType::WindTurbine:    return BLD_WindTurbine;
		case EBuildingType::Shelter:        return BLD_Shelter;
		case EBuildingType::Farm:           return BLD_Farm;
		case EBuildingType::LivestockFarm:  return BLD_LivestockFarm;
		case EBuildingType::Generator:      return BLD_Generator;
		case EBuildingType::Bakery:         return BLD_Bakery;
		case EBuildingType::Restaurant:     return BLD_Restaurant;
		case EBuildingType::TaxOffice:      return BLD_TaxOffice;
		case EBuildingType::Courthouse:     return BLD_Courthouse;
		case EBuildingType::PoliceStation:  return BLD_PoliceStation;
		case EBuildingType::BusTerminal:    return BLD_BusTerminal;
		case EBuildingType::BusStop:        return BLD_BusStop;
		case EBuildingType::WaterPlant:     return BLD_WaterPlant;
		case EBuildingType::Warehouse:      return BLD_Warehouse;
		case EBuildingType::Substation:     return BLD_Substation;
		default:                            return BLD_House;
		}
	}

	static const TCHAR* GetDisasterBedSound(EDisasterType Type)
	{
		using namespace AftershockElevenLabsPaths;
		switch (Type)
		{
		case EDisasterType::Earthquake: return DIS_Quake_Bed;
		case EDisasterType::Flood:      return DIS_Flood_Bed;
		case EDisasterType::Wind:       return DIS_Storm_Bed;
		case EDisasterType::Fire:       return DIS_Fire_Bed;
		case EDisasterType::Tsunami:    return DIS_Tsunami_Bed;
		case EDisasterType::Volcano:    return DIS_Volcano_Bed;
		case EDisasterType::Drought:    return DIS_Drought_Bed;
		default:                        return DIS_Quake_Bed;
		}
	}

	static const TCHAR* GetDisasterStrikeSound(EDisasterType Type)
	{
		using namespace AftershockElevenLabsPaths;
		switch (Type)
		{
		case EDisasterType::Earthquake: return DIS_Quake_Rupture;
		case EDisasterType::Flood:      return DIS_Flood_Breach;
		case EDisasterType::Wind:       return DIS_Storm_Impact;
		case EDisasterType::Fire:       return DIS_Fire_Ignition;
		case EDisasterType::Tsunami:    return DIS_Tsunami_Impact;
		case EDisasterType::Volcano:    return DIS_Volcano_Blast;
		case EDisasterType::Drought:    return DIS_Drought_Stress;
		default:                        return DIS_Quake_Rupture;
		}
	}

	// Multi-variation round-robin pickers (Prevents machine-gun effect)
	static const TCHAR* GetRandomFootstepSidewalk()
	{
		using namespace AftershockElevenLabsPaths;
		const int32 Index = FMath::RandRange(0, 3);
		return CIT_Footstep_Sidewalk_Vars[Index];
	}

	static const TCHAR* GetRandomFootstepRubble()
	{
		using namespace AftershockElevenLabsPaths;
		const int32 Index = FMath::RandRange(0, 3);
		return CIT_Footstep_Rubble_Vars[Index];
	}

	static const TCHAR* GetRandomUIClick()
	{
		using namespace AftershockElevenLabsPaths;
		const int32 Index = FMath::RandRange(0, 3);
		return UI_Click_Vars[Index];
	}

	static const TCHAR* GetRandomUIHover()
	{
		using namespace AftershockElevenLabsPaths;
		const int32 Index = FMath::RandRange(0, 3);
		return UI_Hover_Vars[Index];
	}

	static const TCHAR* GetRandomUIRotate()
	{
		using namespace AftershockElevenLabsPaths;
		const int32 Index = FMath::RandRange(0, 3);
		return UI_Rotate_Vars[Index];
	}
};
