#include "AftershockPhysicalHazardsAuditCommandlet.h"

#if WITH_EDITOR
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Simulation/DisasterMath.h"
#include "AftershockTerrainSurface.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "StaticMeshResources.h"
#include <limits>

namespace
{
	struct FPhysicalCheck { FString Name; bool bPass=false; };
	struct FPhysicalWorld
	{
		UWorld* World=nullptr;
		~FPhysicalWorld()
		{
			if(World){World->DestroyWorld(false);if(GEngine)GEngine->DestroyWorldContext(World);World->RemoveFromRoot();}
		}
	};
	bool SamePhysicalField(const TMap<FIntPoint,float>& A,const TMap<FIntPoint,float>& B)
	{
		if(A.Num()!=B.Num())return false;
		for(const auto& Pair:A){const float* Value=B.Find(Pair.Key);if(!Value||!FMath::IsNearlyEqual(Pair.Value,*Value,.0001f))return false;}
		return true;
	}
	bool FinitePhysicalField(const TMap<FIntPoint,float>& Field)
	{
		for(const auto& Pair:Field)if(!FMath::IsFinite(Pair.Value)||Pair.Value<0.f||Pair.Value>100.f)return false;
		return true;
	}
	bool SamePhysicalFloodBudget(const FAftershockFloodBudgetStats& A,const FAftershockFloodBudgetStats& B)
	{
		// Reduction order can differ; permit only double-precision roundoff, not
		// a changed rainfall, transport, storage or loss accounting equation.
		const auto Same=[](double X,double Y)
		{return FMath::IsFinite(X)&&FMath::IsFinite(Y)&&FMath::Abs(X-Y)<=1.e-9*FMath::Max(1.,FMath::Max(FMath::Abs(X),FMath::Abs(Y)));};
		return Same(A.RainfallM3,B.RainfallM3)&&Same(A.InitialRiverStorageM3,B.InitialRiverStorageM3)
			&&Same(A.InfiltrationM3,B.InfiltrationM3)&&Same(A.DrainageM3,B.DrainageM3)
			&&Same(A.OutletM3,B.OutletM3)&&Same(A.DamRetainedM3,B.DamRetainedM3)
			&&Same(A.FinalStorageM3,B.FinalStorageM3)&&Same(A.MassResidualM3,B.MassResidualM3)
			&&A.ActiveCells==B.ActiveCells&&A.Iterations==B.Iterations&&A.EdgeVisits==B.EdgeVisits
			&&A.bCommittedStorage==B.bCommittedStorage;
	}
}

UAftershockPhysicalHazardsAuditCommandlet::UAftershockPhysicalHazardsAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}

int32 UAftershockPhysicalHazardsAuditCommandlet::Main(const FString& Params)
{
	TArray<FPhysicalCheck> Checks;int32 Failed=0;
	const auto Check=[&](const TCHAR* Name,bool bPass)
	{
		Checks.Add({Name,bPass});Failed+=bPass?0:1;
		if(bPass){UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_AUDIT_PASS %s"),Name);}
		else{UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_PHYSICAL_AUDIT_FAIL %s"),Name);}
	};
	const auto Finish=[&]()
	{
		const FString Dir=FPaths::ProjectSavedDir()/TEXT("PresentationValidation");IFileManager::Get().MakeDirectory(*Dir,true);
		FString JSON=FString::Printf(TEXT("{\"schema_version\":1,\"scope\":\"Isolated V3 Model1 compatibility pulse mass accounting and serial/parallel parity, seven-hazard causal APIs, a synthetic below-bank-head shoreline mesh regression and actual static-mesh render-buffer UV checks on Atakent/Miami source worlds. Legacy flood calls explicitly scope and restore Editor WorldType over Game-created canonical subsystem dependencies; they do not start or accept the live Model2 lifecycle. Numerical coefficients are gameplay calibration; no CFD, GMPE, catchment forecast, displayed image, GPU throughput or packaged acceptance.\",\"passed\":%d,\"failed\":%d,\"checks\":["),Checks.Num()-Failed,Failed);
		for(int32 I=0;I<Checks.Num();++I)JSON+=FString::Printf(TEXT("%s{\"name\":\"%s\",\"passed\":%s}"),I?TEXT(","):TEXT(""),*Checks[I].Name,Checks[I].bPass?TEXT("true"):TEXT("false"));
		JSON+=TEXT("]}");if(!FFileHelper::SaveStringToFile(JSON,*(Dir/TEXT("physical_hazards_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return 2;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_AUDIT_RESULT passed=%d failed=%d"),Checks.Num()-Failed,Failed);return Failed?1:0;
	};
	Check(TEXT("engine_initialized"),GEngine!=nullptr);if(!GEngine)return Finish();
	FPhysicalWorld Fixture;Fixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("PhysicalHazardsFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("isolated_world_created"),Fixture.World!=nullptr);if(!Fixture.World)return Finish();
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(Fixture.World);
	auto* Terrain=Fixture.World->GetSubsystem<UTerrainGridSubsystem>();auto* Simulation=Fixture.World->GetSubsystem<UDisasterSimulationSubsystem>();
	Check(TEXT("canonical_subsystems_ready"),Terrain&&Simulation);if(!Terrain||!Simulation)return Finish();
	const auto Reset=[&]()
	{
		Terrain->GenerateProceduralTerrain(12,12,1000.f,73119);
		for(auto& Cell:Terrain->Cells){const FIntPoint Coord=Cell.Coords;Cell=FGridCell();Cell.Coords=Coord;}
		Terrain->ContinuationGeneratorVersion=3;Terrain->CoastalSeaLevelCm=0.f;Terrain->CoastStartRow=INDEX_NONE;Terrain->ActiveDroughtSeverity=0.f;
		Terrain->VolcanicVentCoord=FIntPoint(INDEX_NONE,INDEX_NONE);Simulation->ScenarioWindDirection=FVector(1,0,0);
	};
	// Exercise the actual V3 numerical path, not a legacy fixture whose empty
	// LastFloodBudget would make a zero mass residual pass without any work.
	IConsoleVariable* FloodParallelCVar=IConsoleManager::Get().FindConsoleVariable(TEXT("r.Aftershock.ParallelFlood"));
	Check(TEXT("flood_parallel_cvar_registered"),FloodParallelCVar!=nullptr);
	if(FloodParallelCVar)
	{
		const int32 PreviousValue=FloodParallelCVar->GetInt();
		const EConsoleVariableFlags PreviousFlags=FloodParallelCVar->GetFlags();
		{
			ON_SCOPE_EXIT
			{
				FloodParallelCVar->Set(PreviousValue,ECVF_SetByConsole);
				FloodParallelCVar->SetFlags(PreviousFlags);
			};
			for(int32 Case=0;Case<2;++Case)
			{
				Reset();
				int32 ExpectedActive=0;
				for(auto& Cell:Terrain->Cells)
				{
					Cell.Elevation=static_cast<float>(Cell.Coords.X*3+FMath::Abs(Cell.Coords.Y-6)*2);
					Cell.SoilMoisture01=.2f+.05f*(Cell.Coords.X%5);
					Cell.ForestFuel01=.2f+.1f*(Cell.Coords.Y%4);
					Cell.bHasRoad=Cell.Coords.Y==5;
					Cell.DrainageQuality=Cell.bHasRoad?.2f:.05f;
					Cell.WaterFlowDamping=.1f*(Cell.Coords.X%3);
					Cell.bIsRiverbed=Cell.Coords.X==4;
					Cell.RiverCapacity=1.3f;Cell.RiverFill01=.45f;
					Cell.bHasDam=Cell.bIsRiverbed&&Cell.Coords.Y==7;
					if(Cell.bHasDam){Cell.DamStorageCapacityUnits=400.f;Cell.DamStoredWaterUnits=40.f;}
					// Resident non-playable holes exercise blocked (-3) edges;
					// ocean (-1) and outside-resident (-2) outlets also coexist.
					if(Case==1)
					{
						Cell.bIsOcean=Cell.Coords.X==0;
						Cell.bIsPlayable=!Cell.bIsOcean&&!(Cell.Coords.X==6&&Cell.Coords.Y>=3&&Cell.Coords.Y<=8);
					}
					if(Cell.bIsPlayable&&!Cell.bIsOcean)++ExpectedActive;
				}
				const FString Prefix=Case==0?TEXT("flood_parallel_v3_graded"):TEXT("flood_parallel_v3_blocked_ocean_open_edges");
				FAftershockTerrainSaveState Before,AfterSerial,AfterParallel;
				Terrain->CaptureSaveState(Before);
				FloodParallelCVar->Set(0,ECVF_SetByConsole);
				Check(*(Prefix+TEXT("_serial_selected")),FloodParallelCVar->GetInt()==0);
				const auto SerialField=Simulation->SimulateFloodMap(60.f);
				const auto SerialBudget=Simulation->GetLastPhysicalFloodBudget();
				Terrain->CaptureSaveState(AfterSerial);
				FloodParallelCVar->Set(1,ECVF_SetByConsole);
				Check(*(Prefix+TEXT("_parallel_selected")),FloodParallelCVar->GetInt()==1);
				const auto ParallelField=Simulation->SimulateFloodMap(60.f);
				const auto ParallelBudget=Simulation->GetLastPhysicalFloodBudget();
				Terrain->CaptureSaveState(AfterParallel);
				Check(*(Prefix+TEXT("_both_execute_real_positive_v3_work")),ExpectedActive>0
					&&SerialBudget.ActiveCells==ExpectedActive&&ParallelBudget.ActiveCells==ExpectedActive
					&&SerialBudget.Iterations==48&&ParallelBudget.Iterations==48
					&&SerialBudget.RainfallM3>0.&&ParallelBudget.RainfallM3>0.
					&&!SerialField.IsEmpty()&&!ParallelField.IsEmpty());
				Check(*(Prefix+TEXT("_all_budget_fields_match")),SamePhysicalFloodBudget(SerialBudget,ParallelBudget));
				Check(*(Prefix+TEXT("_actual_public_water_exposure_fields_match")),SamePhysicalField(SerialField,ParallelField)
					&&FinitePhysicalField(SerialField)&&FinitePhysicalField(ParallelField));
				Check(*(Prefix+TEXT("_both_preserve_entire_canonical_terrain")),
					FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&Before,&AfterSerial,0)
					&&FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&Before,&AfterParallel,0));
				Check(*(Prefix+TEXT("_all_four_edge_probes_counted_even_when_blocked")),
					SerialBudget.EdgeVisits==static_cast<int64>(ExpectedActive)*4*48
					&&ParallelBudget.EdgeVisits==static_cast<int64>(ExpectedActive)*4*48);
				Check(*(Prefix+TEXT("_both_mass_conserved_without_storage_commit")),!SerialBudget.bCommittedStorage&&!ParallelBudget.bCommittedStorage
					&&FMath::Abs(SerialBudget.MassResidualM3)<1.e-6&&FMath::Abs(ParallelBudget.MassResidualM3)<1.e-6);
				Check(*(Prefix+TEXT("_parallel_result_repeatable")),SamePhysicalField(ParallelField,Simulation->SimulateFloodMap(60.f))
					&&SamePhysicalFloodBudget(ParallelBudget,Simulation->GetLastPhysicalFloodBudget()));
			}
		}
		Check(TEXT("flood_parallel_cvar_value_and_priority_restored"),FloodParallelCVar->GetInt()==PreviousValue
			&&FloodParallelCVar->GetFlags()==PreviousFlags);
	}
	Reset();
	const uint64 RevisionBeforePreview=Terrain->GetTerrainRevision();
	const auto RainPreview=Simulation->SimulateFloodMap(60.f);const auto RainBudget=Simulation->GetLastPhysicalFloodBudget();
	Check(TEXT("flood_preview_repeatable"),SamePhysicalField(RainPreview,Simulation->SimulateFloodMap(60.f)));
	Check(TEXT("flood_preview_does_not_mutate_terrain"),Terrain->GetTerrainRevision()==RevisionBeforePreview&&FMath::IsNearlyEqual(Terrain->GetCell(FIntPoint(5,5)).SoilMoisture01,.65f));
	Check(TEXT("rain_mm_area_conversion"),FMath::Abs(RainBudget.RainfallM3-1728.)<.00001); //144 cells *100m2 *120mm.
	Check(TEXT("flood_positive_storage_and_outlet"),RainBudget.FinalStorageM3>0.&&RainBudget.OutletM3>0.);
	Check(TEXT("flood_mass_conserved"),FMath::Abs(RainBudget.MassResidualM3)<FMath::Max(1.e-7,RainBudget.RainfallM3*1.e-9));
	// Teammate eeb1008 deliberately raised the legacy event relaxation to 48 sweeps.
	// Keep an exact work assertion; conservation and real-source checks below remain independent.
	Check(TEXT("flood_work_bounded"),RainBudget.ActiveCells==144&&RainBudget.Iterations==48&&RainBudget.EdgeVisits==144*4*48);
	Check(TEXT("flood_preview_finite"),!RainPreview.IsEmpty()&&FinitePhysicalField(RainPreview));
	Check(TEXT("flood_zero_and_nan_empty"),Simulation->SimulateFloodMap(0.f).IsEmpty()&&Simulation->SimulateFloodMap(std::numeric_limits<float>::quiet_NaN()).IsEmpty());
	for(auto& Cell:Terrain->Cells)Cell.DrainageQuality=1.f;
	Check(TEXT("drainage_can_keep_low_rain_dry"),Simulation->SimulateFloodMap(20.f).IsEmpty());
	const auto DrainedBudget=Simulation->GetLastPhysicalFloodBudget();
	Check(TEXT("drainage_is_accounted_outflow"),DrainedBudget.DrainageM3>0.&&DrainedBudget.FinalStorageM3<.0001&&FMath::Abs(DrainedBudget.MassResidualM3)<.000001);
	Reset();
	// Model1 remains the non-game compatibility contract. Keep the Game-created
	// Life/Ledger dependency graph, but explicitly select that legacy call scope.
	// A real Game/PIE world still rejects an uninitialized Model2 runtime.
	FDisasterReport FloodReport;
	{
		TGuardValue<TEnumAsByte<EWorldType::Type>> LegacyCallType(Fixture.World->WorldType,TEnumAsByte<EWorldType::Type>(EWorldType::Editor));
		FloodReport=Simulation->TriggerDisaster(EDisasterType::Flood,60.f,0.f,true);
	}
	Check(TEXT("legacy_model1_flood_call_restores_game_world"),Fixture.World->WorldType==EWorldType::Game&&!Simulation->IsHydraulicModel2Active());
	const auto PhysicalSnapshot=Simulation->GetLatestVisualSnapshot();
	Check(TEXT("flood_snapshot_physical_version_and_units"),!FloodReport.bWasRejected&&PhysicalSnapshot.FloodModelVersion==1&&PhysicalSnapshot.RainfallPulseMm==120.f&&PhysicalSnapshot.FloodPulseDurationSeconds==3600.f);
	bool Paired=!PhysicalSnapshot.WaterDepthCm.IsEmpty()&&PhysicalSnapshot.WaterDepthCm.Num()==PhysicalSnapshot.WaterSurfaceElevationCm.Num()&&PhysicalSnapshot.WaterDepthCm.Num()==PhysicalSnapshot.WaterArrivalFraction.Num();
	for(const auto& Pair:PhysicalSnapshot.WaterDepthCm)
	{
		const float* Head=PhysicalSnapshot.WaterSurfaceElevationCm.Find(Pair.Key);const float* Arrival=PhysicalSnapshot.WaterArrivalFraction.Find(Pair.Key);const auto Cell=Terrain->GetCell(Pair.Key);
		const float Bed=Cell.Elevation-(Cell.bIsRiverbed?Cell.RiverCapacity*100.f:0.f);
		Paired=Paired&&FMath::IsFinite(Pair.Value)&&Pair.Value>=0.f&&Head&&Arrival;
		if(Head&&Arrival)Paired=Paired&&FMath::IsNearlyEqual(*Head,Bed+Pair.Value,.01f)&&FMath::IsFinite(*Arrival)&&*Arrival>=0.f&&*Arrival<=1.f;
	}
	Check(TEXT("flood_depth_head_arrival_paired"),Paired);
	Check(TEXT("legacy_snapshot_defaults_unchanged"),FAftershockDisasterVisualSnapshot().FloodModelVersion==0&&FAftershockDisasterVisualSnapshot().WaterDepthCm.IsEmpty());
	Check(TEXT("physical_snapshot_direct_restore_roundtrip"),Simulation->RestoreVisualSaveState(PhysicalSnapshot,0.f));
	const int32 BeforeInvalidRevision=Simulation->GetLatestVisualSnapshot().Revision;
	FAftershockDisasterVisualSnapshot InvalidSnapshot=PhysicalSnapshot;InvalidSnapshot.FloodModelVersion=2;
	Check(TEXT("unknown_flood_model_rejected_atomically"),!Simulation->RestoreVisualSaveState(InvalidSnapshot,0.f)&&Simulation->GetLatestVisualSnapshot().Revision==BeforeInvalidRevision);
	InvalidSnapshot=PhysicalSnapshot;InvalidSnapshot.WaterSurfaceElevationCm.Reset();
	Check(TEXT("unpaired_water_head_rejected"),!Simulation->RestoreVisualSaveState(InvalidSnapshot,0.f));
	InvalidSnapshot=PhysicalSnapshot;for(auto& Pair:InvalidSnapshot.WaterDepthCm){Pair.Value=-1.f;break;}
	Check(TEXT("negative_water_depth_rejected"),!InvalidSnapshot.WaterDepthCm.IsEmpty()&&!Simulation->RestoreVisualSaveState(InvalidSnapshot,0.f));
	InvalidSnapshot=PhysicalSnapshot;for(auto& Pair:InvalidSnapshot.WaterArrivalFraction){Pair.Value=1.1f;break;}
	Check(TEXT("out_of_range_water_arrival_rejected"),!InvalidSnapshot.WaterArrivalFraction.IsEmpty()&&!Simulation->RestoreVisualSaveState(InvalidSnapshot,0.f));
	Reset();
	for(auto& Cell:Terrain->Cells){Cell.Elevation=1000.f;Cell.DrainageQuality=1.f;if(Cell.Coords.Y==5){Cell.bIsRiverbed=true;Cell.Elevation=100.f;Cell.RiverCapacity=2.f;Cell.RiverFill01=.2f;}}
	{
		TGuardValue<TEnumAsByte<EWorldType::Type>> LegacyCallType(Fixture.World->WorldType,TEnumAsByte<EWorldType::Type>(EWorldType::Editor));
		Simulation->TriggerDisaster(EDisasterType::Flood,1.f,0.f,true);
	}
	Check(TEXT("legacy_model1_river_call_restores_game_world"),Fixture.World->WorldType==EWorldType::Game&&!Simulation->IsHydraulicModel2Active());
	const auto RiverSnapshot=Simulation->GetLatestVisualSnapshot();
	const FIntPoint RiverPoint(5,5);const float* RiverHead=RiverSnapshot.WaterSurfaceElevationCm.Find(RiverPoint);const float* RiverDepth=RiverSnapshot.WaterDepthCm.Find(RiverPoint);
	Check(TEXT("river_fills_below_bank_before_overtop"),RiverHead&&RiverDepth&&*RiverDepth>0.f&&*RiverHead<Terrain->GetCell(RiverPoint).Elevation);
	Reset();
	const auto Wind40=Simulation->SimulateWindMap(40.f),Wind80=Simulation->SimulateWindMap(80.f);
	Check(TEXT("wind_pressure_quadratic"),FMath::IsNearlyEqual(Wind80.FindRef(FIntPoint(5,5)),4.f*Wind40.FindRef(FIntPoint(5,5)),.001f));
	FGridCell Hill=Terrain->GetCell(FIntPoint(1,5));Hill.Elevation=4000.f;Terrain->SetCell(Hill.Coords,Hill);
	const auto Sheltered=Simulation->SimulateWindMap(40.f);
	Check(TEXT("wind_upwind_hill_reduces_exposure"),Sheltered.FindRef(FIntPoint(5,5))<Wind40.FindRef(FIntPoint(5,5)));
	Check(TEXT("wind_fields_finite"),FinitePhysicalField(Sheltered)&&Simulation->SimulateWindMap(std::numeric_limits<float>::infinity()).IsEmpty());
	Reset();
	Check(TEXT("wildfire_requires_fuel"),Simulation->SimulateForestFireMap(80.f).IsEmpty());
	for(auto& Cell:Terrain->Cells){Cell.ForestFuel01=.8f;Cell.SoilMoisture01=.99f;}
	Check(TEXT("wildfire_wet_fuel_cannot_ignite"),Simulation->SimulateForestFireMap(80.f).IsEmpty());
	for(auto& Cell:Terrain->Cells)Cell.SoilMoisture01=.1f;
	const auto DryFire=Simulation->SimulateForestFireMap(80.f);
	Check(TEXT("wildfire_dry_fuel_connected_field"),!DryFire.IsEmpty()&&FinitePhysicalField(DryFire)&&SamePhysicalField(DryFire,Simulation->SimulateForestFireMap(80.f)));
	Reset();FText Reason;
	Check(TEXT("tsunami_rejected_inland"),!Simulation->CanTriggerDisaster(EDisasterType::Tsunami,Reason)&&Simulation->SimulateTsunamiMap(80.f).IsEmpty());
	for(auto& Cell:Terrain->Cells){if(Cell.Coords.Y==0){Cell.bIsOcean=true;Cell.bIsPlayable=false;Cell.Elevation=-100.f;}if(Cell.Coords.Y==3)Cell.Elevation=1000.f;}
	const auto Tsunami=Simulation->SimulateTsunamiMap(80.f);
	Check(TEXT("tsunami_reaches_connected_coastal_land"),Tsunami.FindRef(FIntPoint(5,1))>0.f);
	Check(TEXT("tsunami_does_not_cross_above_crest_ridge"),!Tsunami.Contains(FIntPoint(5,3))&&!Tsunami.Contains(FIntPoint(5,4)));
	Reset();Terrain->VolcanicVentCoord=FIntPoint(5,5);
	for(auto& Cell:Terrain->Cells){Cell.Elevation=0.f;if(Cell.Coords==Terrain->VolcanicVentCoord){Cell.bVolcanicVent=true;Cell.Elevation=100.f;}if(Cell.Coords==FIntPoint(6,5))Cell.Elevation=200.f;}
	TMap<FIntPoint,float> Lava,Ash;const auto Volcano=Simulation->SimulateVolcanoMap(80.f,Lava,Ash);
	TMap<FIntPoint,float> RepeatedLava,RepeatedAsh;const auto RepeatedVolcano=Simulation->SimulateVolcanoMap(80.f,RepeatedLava,RepeatedAsh);
	Check(TEXT("lava_stays_downhill"),Lava.Contains(Terrain->VolcanicVentCoord)&&!Lava.Contains(FIntPoint(6,5)));
	Check(TEXT("volcano_preview_repeatable_finite"),FinitePhysicalField(Volcano)&&FinitePhysicalField(Lava)&&FinitePhysicalField(Ash)
		&&SamePhysicalField(Volcano,RepeatedVolcano)&&SamePhysicalField(Lava,RepeatedLava)&&SamePhysicalField(Ash,RepeatedAsh));
	Simulation->TriggerDisaster(EDisasterType::Volcano,80.f,0.f,true);const auto LavaSnapshot=Simulation->GetLatestVisualSnapshot();
	bool LavaArrivalValid=LavaSnapshot.LavaSeverity.Num()==LavaSnapshot.LavaArrivalFraction.Num()&&!LavaSnapshot.LavaArrivalFraction.IsEmpty();
	for(const auto& Pair:LavaSnapshot.LavaArrivalFraction)LavaArrivalValid=LavaArrivalValid&&FMath::IsFinite(Pair.Value)&&Pair.Value>=0.f&&Pair.Value<=1.f;
	Check(TEXT("lava_canonical_arrival_bounded"),LavaArrivalValid&&LavaSnapshot.LavaArrivalFraction.FindRef(Terrain->VolcanicVentCoord)==0.f);
	Reset();for(auto& Cell:Terrain->Cells)Cell.bAgriculturalLand=true;
	const auto DroughtReport=Simulation->TriggerDisaster(EDisasterType::Drought,80.f,0.f,true);const float AfterPulse=Terrain->GetCell(FIntPoint(5,5)).SoilMoisture01;
	Check(TEXT("drought_begins_gradually_not_instant_season"),AfterPulse<.65f&&AfterPulse>.625f&&Terrain->ActiveDroughtSeverity==80.f);
	Terrain->AdvanceEnvironmentalTurn();
	Check(TEXT("drought_persists_over_environmental_turns"),Terrain->GetCell(FIntPoint(5,5)).SoilMoisture01<AfterPulse);
	Check(TEXT("drought_no_structural_casualty_event"),DroughtReport.TotalDeaths==0&&DroughtReport.TotalBuildingsCollapsed==0);
	Reset();UBuildingData* House=NewObject<UBuildingData>(Simulation);House->BaseResistance.Add(EDisasterType::Earthquake,.3f);
	FBuildingInstance Building;Building.BuildingData=House;Building.GridCoords=FIntPoint(5,5);Building.Foundation=EFoundationQuality::Weak;
	const auto RockOutcome=Terrain->EvaluateBuildingInDisaster(Building,EDisasterType::Earthquake,40.f,0.f,true);
	Building.CurrentDamage=0.f;Building.DamageState=EDamageState::Intact;FGridCell Soil=Terrain->GetCell(Building.GridCoords);Soil.SoilType=EAftershockSoilType::Alluvium;Terrain->SetCell(Soil.Coords,Soil);
	const auto AlluvialOutcome=Terrain->EvaluateBuildingInDisaster(Building,EDisasterType::Earthquake,40.f,0.f,true);
	Check(TEXT("quake_alluvium_amplifies_same_input"),AlluvialOutcome.FinalDamage>RockOutcome.FinalDamage);
	Building.CurrentDamage=0.f;Building.DamageState=EDamageState::Intact;Soil.Elevation=100000.f;Terrain->SetCell(Soil.Coords,Soil);
	const auto MountainOutcome=Terrain->EvaluateBuildingInDisaster(Building,EDisasterType::Earthquake,40.f,0.f,true);
	Check(TEXT("quake_elevation_alone_not_immunity"),FMath::IsNearlyEqual(MountainOutcome.FinalDamage,AlluvialOutcome.FinalDamage,.0001f));
	Building.CurrentDamage=0.f;Building.DamageState=EDamageState::Intact;Building.Foundation=EFoundationQuality::Strong;
	Check(TEXT("quake_strong_foundation_reduces_damage"),Terrain->EvaluateBuildingInDisaster(Building,EDisasterType::Earthquake,40.f,0.f,true).FinalDamage<AlluvialOutcome.FinalDamage);
	FBuildingSnapshot LegacyHouse;FDisasterEvent LegacyQuake;LegacyQuake.Type=EDisasterType::Earthquake;FCityPreventionState CleanCity,DirtyCity;CleanCity.EnvironmentScore=100.f;DirtyCity.EnvironmentScore=0.f;FTerrainCell LegacyCell;
	Check(TEXT("instant_quake_not_scaled_by_city_environment"),UDisasterMath::ComputeDamage(LegacyHouse,LegacyQuake,CleanCity,LegacyCell,40.f).Damage==UDisasterMath::ComputeDamage(LegacyHouse,LegacyQuake,DirtyCity,LegacyCell,40.f).Damage);
	// This isolated, manually authored pair tests presentation interpolation only.
	// Land DEM/head100/110cm and river DEM/head0/-50cm give ground50/head30
	// at their midpoint. Dropping the below-bank river head would invent a wet wedge.
	{
		FPhysicalWorld ShoreFixture;ShoreFixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("PhysicalBelowBankShoreFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
		if(ShoreFixture.World)GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(ShoreFixture.World);
		auto* ShoreTerrain=ShoreFixture.World?ShoreFixture.World->GetSubsystem<UTerrainGridSubsystem>():nullptr;
		FAftershockDisasterVisualSnapshot ShoreSnapshot;ShoreSnapshot.Type=EDisasterType::Flood;ShoreSnapshot.FloodModelVersion=1;
		ShoreSnapshot.RainfallPulseMm=20.f;ShoreSnapshot.FloodPulseDurationSeconds=3600.f;
		if(ShoreTerrain)
		{
			ShoreTerrain->GenerateProceduralTerrain(2,1,1000.f,73121);ShoreTerrain->ContinuationGeneratorVersion=3;ShoreTerrain->CoastalSeaLevelCm=0.f;
			for(auto& Cell:ShoreTerrain->Cells)
			{
				const FIntPoint Coord=Cell.Coords;Cell=FGridCell();Cell.Coords=Coord;Cell.bIsPlayable=true;Cell.Elevation=Coord.X==0?100.f:0.f;
				Cell.bIsRiverbed=Coord.X==1;Cell.RiverCapacity=Cell.bIsRiverbed?1.f:0.f;
				ShoreSnapshot.WaterDepthCm.Add(Coord,Coord.X==0?10.f:50.f);
				ShoreSnapshot.WaterSurfaceElevationCm.Add(Coord,Coord.X==0?110.f:-50.f);
				ShoreSnapshot.WaterArrivalFraction.Add(Coord,.25f);ShoreSnapshot.LocalSeverity.Add(Coord,10.f);
			}
		}
		const bool PairReady=ShoreTerrain&&ShoreTerrain->Cells.Num()==2;
		Check(TEXT("below_bank_pair_has_dry_midpoint_and_paired_bed_columns"),PairReady
			&&FMath::IsNearlyEqual(ShoreSnapshot.WaterDepthCm.FindRef(FIntPoint(0,0)),ShoreSnapshot.WaterSurfaceElevationCm.FindRef(FIntPoint(0,0))-100.f)
			&&FMath::IsNearlyEqual(ShoreSnapshot.WaterDepthCm.FindRef(FIntPoint(1,0)),ShoreSnapshot.WaterSurfaceElevationCm.FindRef(FIntPoint(1,0))+100.f)
			&&FMath::Lerp(110.f,-50.f,.5f)<FMath::Lerp(100.f,0.f,.5f));
		FAftershockTerrainSaveState BeforeShore,AfterShore;if(ShoreTerrain)ShoreTerrain->CaptureSaveState(BeforeShore);
		const FAftershockDisasterVisualSnapshot BeforeShoreSnapshot=ShoreSnapshot;
		auto* ShoreOwner=ShoreFixture.World?ShoreFixture.World->SpawnActor<AActor>():nullptr;
		if(ShoreOwner)ShoreOwner->Tags.AddUnique(TEXT("Aftershock_MeshCpuAudit"));
		auto* ShoreSurface=ShoreOwner?NewObject<UStaticMeshComponent>(ShoreOwner):nullptr;
		if(ShoreSurface){ShoreOwner->AddInstanceComponent(ShoreSurface);ShoreOwner->SetRootComponent(ShoreSurface);ShoreSurface->SetCollisionEnabled(ECollisionEnabled::NoCollision);}
		auto* ShoreBase=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/FX/Aftershock/Materials/M_DisasterFloodVolume.M_DisasterFloodVolume"));
		auto* ShoreMID=ShoreSurface&&ShoreBase?UMaterialInstanceDynamic::Create(ShoreBase,ShoreSurface):nullptr;
		if(ShoreMID)ShoreSurface->SetMaterial(0,ShoreMID);
		Check(TEXT("below_bank_mesh_fixture_ready"),PairReady&&ShoreOwner&&ShoreSurface&&ShoreMID);
		const bool ShoreBuilt=PairReady&&ShoreOwner&&ShoreSurface&&ShoreMID
			&&BuildAftershockFloodSurface(ShoreOwner,ShoreTerrain,ShoreSnapshot.LocalSeverity,ShoreSurface,nullptr,nullptr,&ShoreSnapshot);
		const UStaticMesh* ShoreMesh=ShoreSurface?ShoreSurface->GetStaticMesh().Get():nullptr;const auto* ShoreData=ShoreMesh?ShoreMesh->GetRenderData():nullptr;
		const bool HasShoreBuffer=ShoreBuilt&&ShoreData&&!ShoreData->LODResources.IsEmpty()
			&&ShoreData->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()==3
			&&ShoreData->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetUseFullPrecisionUVs()&&ShoreMesh->bAllowCPUAccess;
		Check(TEXT("below_bank_mesh_has_actual_full_precision_render_buffer"),HasShoreBuffer);
		bool CoversWetLand=false,CoversDryMidpoint=false,NoForbiddenWedge=HasShoreBuffer;int32 ShoreTopTriangles=0;
		if(HasShoreBuffer)
		{
			const auto& LOD=ShoreData->LODResources[0];const auto& PositionBuffer=LOD.VertexBuffers.PositionVertexBuffer;
			const auto& UVBuffer=LOD.VertexBuffers.StaticMeshVertexBuffer;const auto& Colors=LOD.VertexBuffers.ColorVertexBuffer;
			const auto Triangles=LOD.IndexBuffer.GetArrayView();const double OriginX=ShoreTerrain->GridOrigin.X,OriginY=ShoreTerrain->GridOrigin.Y;
			const auto CoversPoint=[](const FVector3f& A,const FVector3f& B,const FVector3f& C,double X,double Y)
			{
				const double Den=(B.Y-C.Y)*(A.X-C.X)+(C.X-B.X)*(A.Y-C.Y);if(FMath::Abs(Den)<1.e-8)return false;
				const double U=((B.Y-C.Y)*(X-C.X)+(C.X-B.X)*(Y-C.Y))/Den;
				const double V=((C.Y-A.Y)*(X-C.X)+(A.X-C.X)*(Y-C.Y))/Den;
				return U>=-1.e-6&&V>=-1.e-6&&U+V<=1.+1.e-6;
			};
			for(int32 I=0;I+2<Triangles.Num();I+=3)
			{
				const uint32 A=Triangles[I],B=Triangles[I+1],C=Triangles[I+2];
				if(A>=PositionBuffer.GetNumVertices()||B>=PositionBuffer.GetNumVertices()||C>=PositionBuffer.GetNumVertices()
					||A>=Colors.GetNumVertices()||B>=Colors.GetNumVertices()||C>=Colors.GetNumVertices()){NoForbiddenWedge=false;continue;}
				if(Colors.VertexColor(A).B!=255||Colors.VertexColor(B).B!=255||Colors.VertexColor(C).B!=255)continue;
				++ShoreTopTriangles;const auto PA=PositionBuffer.VertexPosition(A),PB=PositionBuffer.VertexPosition(B),PC=PositionBuffer.VertexPosition(C);
				CoversWetLand|=CoversPoint(PA,PB,PC,OriginX+400.,OriginY+400.);
				CoversDryMidpoint|=CoversPoint(PA,PB,PC,OriginX+1000.,OriginY+500.);
				for(uint32 Vertex:{A,B,C})
				{
					const auto Position=PositionBuffer.VertexPosition(Vertex);const auto Hydraulic=UVBuffer.GetVertexUV(Vertex,1);
					const double T=FMath::Clamp((Position.X-OriginX-500.)/1000.,0.,1.);
					const double ExpectedDepth=10.-60.*T;
					NoForbiddenWedge=NoForbiddenWedge&&FMath::IsFinite(Hydraulic.Y)&&ExpectedDepth>=.24
						&&FMath::Abs(Hydraulic.Y-ExpectedDepth)<.02&&Position.X-OriginX<=663.;
				}
			}
		}
		Check(TEXT("below_bank_mesh_retains_real_wet_land_surface"),ShoreTopTriangles>0&&CoversWetLand);
		Check(TEXT("below_bank_dry_midpoint_has_no_top_water_triangle"),HasShoreBuffer&&ShoreTopTriangles>0&&!CoversDryMidpoint);
		Check(TEXT("below_bank_top_vertices_obey_absolute_head_shoreline_without_wet_wedge"),NoForbiddenWedge&&ShoreTopTriangles>0);
		if(ShoreTerrain)ShoreTerrain->CaptureSaveState(AfterShore);
		Check(TEXT("below_bank_mesh_preserves_manual_canonical_pair"),PairReady&&FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&BeforeShore,&AfterShore,0)
			&&FAftershockDisasterVisualSnapshot::StaticStruct()->CompareScriptStruct(&BeforeShoreSnapshot,&ShoreSnapshot,0));
	}
	// Separate factual-source fixtures. Synthetic fields above isolate causality;
	// these use the actual selected V3 DEM/OSM cells without replacing their data.
	for(int32 Region:{0,7}) // Atakent and Miami, stable existing region indices.
	{
		const FString Prefix=Region==0?TEXT("real_v3_atakent"):TEXT("real_v3_miami");
		FPhysicalWorld RealFixture;RealFixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(*Prefix),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
		Check(*(Prefix+TEXT("_world_created")),RealFixture.World!=nullptr);if(!RealFixture.World)continue;
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(RealFixture.World);
		auto* RealTerrain=RealFixture.World->GetSubsystem<UTerrainGridSubsystem>();auto* RealSimulation=RealFixture.World->GetSubsystem<UDisasterSimulationSubsystem>();
		Check(*(Prefix+TEXT("_subsystems_ready")),RealTerrain&&RealSimulation);if(!RealTerrain||!RealSimulation)continue;
		RealTerrain->GenerateProceduralTerrain(125,125,1000.f,73119+Region);
		const bool SourceReady=RealTerrain->ApplyOfflineGeography(Region,0,false,3);
		int32 RealSourceCells=0;for(const auto& Cell:RealTerrain->Cells)if(Cell.bIsPlayable&&!Cell.bIsOcean&&Cell.bHasRealElevationSample&&!Cell.bAuthoredContinuation)++RealSourceCells;
		Check(*(Prefix+TEXT("_uses_selected_actual_detail_source")),SourceReady&&RealTerrain->ContinuationGeneratorVersion==3&&RealTerrain->GeographyIndex==Region&&RealSourceCells>0);
		if(!SourceReady||RealSourceCells==0)continue;
		FAftershockTerrainSaveState BeforeSourcePreview,AfterSourcePreview;RealTerrain->CaptureSaveState(BeforeSourcePreview);
		const auto RealPreview=RealSimulation->SimulateFloodMap(60.f);const auto RealBudget=RealSimulation->GetLastPhysicalFloodBudget();
		RealTerrain->CaptureSaveState(AfterSourcePreview);
		Check(*(Prefix+TEXT("_preview_preserves_entire_canonical_terrain")),FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&BeforeSourcePreview,&AfterSourcePreview,0));
		Check(*(Prefix+TEXT("_rainfall_matches_actual_active_area")),RealBudget.ActiveCells==RealSourceCells
			&&FMath::Abs(RealBudget.RainfallM3-RealSourceCells*100.*.12)<FMath::Max(1.e-5,RealBudget.RainfallM3*1.e-9));
		Check(*(Prefix+TEXT("_mass_and_work_bounded")),RealBudget.Iterations==48&&RealBudget.EdgeVisits==static_cast<int64>(RealBudget.ActiveCells)*4*48
			&&FMath::Abs(RealBudget.MassResidualM3)<FMath::Max(1.e-5,(RealBudget.RainfallM3+RealBudget.InitialRiverStorageM3)*1.e-9));
		FDisasterReport RealReport;
		{
			TGuardValue<TEnumAsByte<EWorldType::Type>> LegacyCallType(RealFixture.World->WorldType,TEnumAsByte<EWorldType::Type>(EWorldType::Editor));
			RealReport=RealSimulation->TriggerDisaster(EDisasterType::Flood,60.f,0.f,true);
		}
		Check(*(Prefix+TEXT("_legacy_model1_call_restores_game_world")),RealFixture.World->WorldType==EWorldType::Game&&!RealSimulation->IsHydraulicModel2Active());
		const auto RealSnapshot=RealSimulation->GetLatestVisualSnapshot();
		bool RealPairs=!RealReport.bWasRejected&&RealSnapshot.FloodModelVersion==1&&SamePhysicalField(RealPreview,RealSnapshot.LocalSeverity)
			&&RealSnapshot.WaterDepthCm.Num()==RealSnapshot.WaterSurfaceElevationCm.Num()&&RealSnapshot.WaterDepthCm.Num()==RealSnapshot.WaterArrivalFraction.Num();
		for(const auto& Pair:RealSnapshot.WaterDepthCm)
		{
			FGridCell Cell;const float* Head=RealSnapshot.WaterSurfaceElevationCm.Find(Pair.Key);const float* Arrival=RealSnapshot.WaterArrivalFraction.Find(Pair.Key);
			RealPairs=RealPairs&&RealTerrain->TryGetCell(Pair.Key,Cell)&&Head&&Arrival&&FMath::IsFinite(Pair.Value)&&Pair.Value>=0.f;
			if(Head&&Arrival)RealPairs=RealPairs&&FMath::IsFinite(*Head)&&FMath::IsFinite(*Arrival)&&*Arrival>=0.f&&*Arrival<=1.f
				&&FMath::IsNearlyEqual(*Head,Cell.Elevation-(Cell.bIsRiverbed?Cell.RiverCapacity*100.f:0.f)+Pair.Value,.125f);
		}
		Check(*(Prefix+TEXT("_canonical_depth_head_arrival_pairs")),RealPairs);
		bool SameElevation=BeforeSourcePreview.Cells.Num()==RealTerrain->Cells.Num();
		for(const auto& Cell:BeforeSourcePreview.Cells){FGridCell Current;SameElevation=SameElevation&&RealTerrain->TryGetCell(Cell.Coords,Current)&&Cell.Elevation==Current.Elevation&&Cell.bHasRealElevationSample==Current.bHasRealElevationSample&&Cell.bIsOcean==Current.bIsOcean&&Cell.bIsRiverbed==Current.bIsRiverbed;}
		Check(*(Prefix+TEXT("_event_preserves_dem_and_mapped_water_authority")),SameElevation);
		FAftershockTerrainSaveState BeforeMesh,AfterMesh;RealTerrain->CaptureSaveState(BeforeMesh);
		const FAftershockDisasterVisualSnapshot BeforeMeshSnapshot=RealSimulation->GetLatestVisualSnapshot();
		auto* SurfaceOwner=RealFixture.World->SpawnActor<AActor>();
		if(SurfaceOwner)SurfaceOwner->Tags.AddUnique(TEXT("Aftershock_MeshCpuAudit"));
		auto* Surface=SurfaceOwner?NewObject<UStaticMeshComponent>(SurfaceOwner):nullptr;
		if(Surface){SurfaceOwner->AddInstanceComponent(Surface);SurfaceOwner->SetRootComponent(Surface);Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);}
		auto* WaterBase=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/FX/Aftershock/Materials/M_DisasterFloodVolume.M_DisasterFloodVolume"));
		auto* WaterMID=Surface&&WaterBase?UMaterialInstanceDynamic::Create(WaterBase,Surface):nullptr;
		if(Surface&&WaterMID)Surface->SetMaterial(0,WaterMID);
		Check(*(Prefix+TEXT("_physical_mesh_fixture_material_ready")),SurfaceOwner&&Surface&&WaterMID);
		FAftershockFloodSurfaceStats MeshStats;
		const bool MeshBuilt=SurfaceOwner&&Surface&&WaterMID&&BuildAftershockFloodSurface(SurfaceOwner,RealTerrain,RealSnapshot.LocalSeverity,Surface,&MeshStats,nullptr,&RealSnapshot);
		const UStaticMesh* Mesh=Surface?Surface->GetStaticMesh().Get():nullptr;const auto* RenderData=Mesh?Mesh->GetRenderData():nullptr;
		const bool HasLOD=MeshBuilt&&RenderData&&!RenderData->LODResources.IsEmpty();
		Check(*(Prefix+TEXT("_physical_flood_has_actual_render_buffer")),HasLOD&&RenderData->LODResources[0].GetNumTriangles()>0);
		bool FullPrecision=false,FiniteData=false,RiseContract=false,Unquantized=false;int32 DataVertices=0,BottomVertices=0;
		if(HasLOD)
		{
			const auto& LOD=RenderData->LODResources[0];const auto& Buffer=LOD.VertexBuffers.StaticMeshVertexBuffer;
			FullPrecision=Buffer.GetNumTexCoords()==3&&Buffer.GetUseFullPrecisionUVs()&&Mesh->bAllowCPUAccess;
			FiniteData=FullPrecision&&Buffer.GetNumVertices()>0;RiseContract=FiniteData;
			if(FullPrecision)for(uint32 Vertex=0;Vertex<Buffer.GetNumVertices();++Vertex)
			{
				const auto Flow=Buffer.GetVertexUV(Vertex,0),Depth=Buffer.GetVertexUV(Vertex,1),Arrival=Buffer.GetVertexUV(Vertex,2);
				const auto Position=LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Vertex);
				FiniteData=FiniteData&&FMath::IsFinite(Flow.X)&&FMath::IsFinite(Flow.Y)&&FMath::IsFinite(Depth.X)&&FMath::IsFinite(Depth.Y)
					&&FMath::IsFinite(Arrival.X)&&FMath::IsFinite(Arrival.Y)&&FMath::IsFinite(Position.X)&&FMath::IsFinite(Position.Y)&&FMath::IsFinite(Position.Z)
					&&Depth.X>=0.f&&Depth.Y>=0.f&&Arrival.Y>=0.f&&Arrival.Y<=1.f&&(Arrival.X==0.f||Arrival.X==1.f);
				if(Depth.X>.0001f){++DataVertices;RiseContract=RiseContract&&FMath::Abs(Depth.X-Depth.Y-4.f)<.01f;}
				else ++BottomVertices; // Skirt bottom intentionally has exact rise0.
				if(LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices()>Vertex)
				{
					const float ByteDepth=LOD.VertexBuffers.ColorVertexBuffer.VertexColor(Vertex).G/255.f*MeshStats.EncodingCm;
					Unquantized|=FMath::Abs(Depth.Y-ByteDepth)>.02f;
				}
			}
		}
		Check(*(Prefix+TEXT("_physical_flood_uses_three_full_precision_uv_channels")),FullPrecision);
		Check(*(Prefix+TEXT("_physical_flood_uv_data_finite_arrival_and_river_masks_bounded")),FiniteData);
		Check(*(Prefix+TEXT("_physical_flood_exact_rise_depth_and_skirt_contract")),RiseContract&&DataVertices>0&&BottomVertices>0);
		Check(*(Prefix+TEXT("_physical_flood_depth_not_reconstructed_from_quantized_vertex_color")),Unquantized);
		Check(*(Prefix+TEXT("_physical_flood_mid_selects_exact_uv_data")),WaterMID&&WaterMID->K2_GetScalarParameterValue(TEXT("FloodPhysicalDataEnabled"))==1.f);
		RealTerrain->CaptureSaveState(AfterMesh);
		const auto AfterMeshSnapshot=RealSimulation->GetLatestVisualSnapshot();
		Check(*(Prefix+TEXT("_physical_mesh_preserves_canonical_terrain_and_snapshot")),FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&BeforeMesh,&AfterMesh,0)
			&&FAftershockDisasterVisualSnapshot::StaticStruct()->CompareScriptStruct(&BeforeMeshSnapshot,&AfterMeshSnapshot,0));
	}
	return Finish();
}
#else
UAftershockPhysicalHazardsAuditCommandlet::UAftershockPhysicalHazardsAuditCommandlet(){}
int32 UAftershockPhysicalHazardsAuditCommandlet::Main(const FString& Params){return 1;}
#endif
