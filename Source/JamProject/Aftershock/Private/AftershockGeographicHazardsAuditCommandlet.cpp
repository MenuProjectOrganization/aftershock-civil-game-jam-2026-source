#include "AftershockGeographicHazardsAuditCommandlet.h"

#if WITH_EDITOR
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/DisasterPresentationSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "StaticMeshResources.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "AftershockTerrainSurface.h"
#include "GameFramework/PlayerController.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include <limits>

namespace
{
	struct FGeographicCheck { FString Name; bool bPass; };
	struct FGeographicWorld
	{
		UWorld* World=nullptr;
		~FGeographicWorld() { if (World) { World->DestroyWorld(false); if (GEngine) GEngine->DestroyWorldContext(World); World->RemoveFromRoot(); } }
	};
	bool SameField(const TMap<FIntPoint,float>& A,const TMap<FIntPoint,float>& B)
	{
		if (A.Num()!=B.Num()) return false;
		for (const auto& P:A) { const float* V=B.Find(P.Key); if (!V || !FMath::IsNearlyEqual(P.Value,*V)) return false; }
		return true;
	}
	UStaticMeshComponent* FindVolcanoSurface(AActor* Actor,const FName Name)
	{TArray<UStaticMeshComponent*> Components;if(Actor)Actor->GetComponents(Components);for(auto* Component:Components)if(Component->GetFName()==Name)return Component;return nullptr;}
	bool HighestVolcanoSurface(const UStaticMeshComponent* Component,FVector Point,float& OutHeight)
	{
		const UStaticMesh* Mesh=Component?Component->GetStaticMesh().Get():nullptr;const FStaticMeshRenderData* Data=Mesh?Mesh->GetRenderData():nullptr;if(!Data||Data->LODResources.IsEmpty())return false;
		const auto& LOD=Data->LODResources[0];const auto Indices=LOD.IndexBuffer.GetArrayView();bool Found=false;OutHeight=-MAX_flt;
		for(int32 I=0;I+2<Indices.Num();I+=3)
		{
			const FVector A=Component->GetComponentTransform().TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Indices[I]))),B=Component->GetComponentTransform().TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Indices[I+1]))),C=Component->GetComponentTransform().TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Indices[I+2])));
			const double Den=(B.Y-C.Y)*(A.X-C.X)+(C.X-B.X)*(A.Y-C.Y);if(FMath::Abs(Den)<.001)continue;
			const double U=((B.Y-C.Y)*(Point.X-C.X)+(C.X-B.X)*(Point.Y-C.Y))/Den,V=((C.Y-A.Y)*(Point.X-C.X)+(A.X-C.X)*(Point.Y-C.Y))/Den,W=1.-U-V;
			if(U>=-.000001&&V>=-.000001&&W>=-.000001){OutHeight=FMath::Max(OutHeight,static_cast<float>(A.Z*U+B.Z*V+C.Z*W));Found=true;}
		}
		return Found;
	}
}

UAftershockGeographicHazardsAuditCommandlet::UAftershockGeographicHazardsAuditCommandlet()
{ IsClient=false; IsServer=false; IsEditor=true; LogToConsole=true; }

int32 UAftershockGeographicHazardsAuditCommandlet::Main(const FString& Params)
{
	TArray<FGeographicCheck> Checks; int32 Failed=0;
	const auto Check=[&](const TCHAR* Name,bool bPass)
	{
		Checks.Add({Name,bPass}); Failed+=bPass ? 0 : 1;
		if (bPass) { UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_GEOGRAPHIC_AUDIT_PASS %s"),Name); }
		else { UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_GEOGRAPHIC_AUDIT_FAIL %s"),Name); }
	};
	const auto Finish=[&]()
	{
		const FString Dir=FPaths::ProjectSavedDir()/TEXT("PresentationValidation"); IFileManager::Get().MakeDirectory(*Dir,true);
		FString JSON=FString::Printf(TEXT("{\"schema_version\":1,\"scope\":\"Actual engine canonical geography, preview, localized hazard, persistent drought and recovery APIs. No rendered image, UI, physical forecast, throughput or packaged-build claim.\",\"passed\":%d,\"failed\":%d,\"checks\":["),Checks.Num()-Failed,Failed);
		for (int32 I=0;I<Checks.Num();++I) JSON+=FString::Printf(TEXT("%s{\"name\":\"%s\",\"passed\":%s}"),I ? TEXT(",") : TEXT(""),*Checks[I].Name,Checks[I].bPass ? TEXT("true") : TEXT("false"));
		JSON+=TEXT("]}"); const FString Path=Dir/TEXT("geographic_hazards_audit.json");
		if (!FFileHelper::SaveStringToFile(JSON,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return 2;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_GEOGRAPHIC_AUDIT_RESULT passed=%d failed=%d report=%s"),Checks.Num()-Failed,Failed,*Path);
		return Failed==0 ? 0 : 1;
	};
	Check(TEXT("engine_initialized"),GEngine!=nullptr); if (!GEngine) return Finish();
	FGeographicWorld Fixture;
	Fixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("GeographicHazardFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("isolated_game_world_created"),Fixture.World!=nullptr); if (!Fixture.World) return Finish();
	UWorld* World=Fixture.World;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UTerrainGridSubsystem* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();
	UDisasterSimulationSubsystem* Simulation=World->GetSubsystem<UDisasterSimulationSubsystem>();
	UEconomyAndStateSubsystem* Economy=World->GetSubsystem<UEconomyAndStateSubsystem>();
	Check(TEXT("canonical_subsystems_ready"),Terrain && Simulation && Economy); if (!Terrain || !Simulation || !Economy) return Finish();
	{
		// An isolated high-proxy/alluvium fixture distinguishes one canonical
		// terrain evaluation from feeding an already amplified visual field back
		// into the evaluator. No real geotechnical/source-data claim is made.
		FGeographicWorld QuakeFixture;
		QuakeFixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("EarthquakeAmplificationRegression")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
		bool OnceMatches=false,FieldMatches=false;
		if(QuakeFixture.World)
		{
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(QuakeFixture.World);
			auto* QuakeTerrain=QuakeFixture.World->GetSubsystem<UTerrainGridSubsystem>();
			auto* QuakeSimulation=QuakeFixture.World->GetSubsystem<UDisasterSimulationSubsystem>();
			auto* QuakeBuildings=QuakeFixture.World->GetSubsystem<UBuildingManagerSubsystem>();
			if(QuakeTerrain&&QuakeSimulation&&QuakeBuildings)
			{
				QuakeTerrain->GenerateProceduralTerrain(12,12,500.f,11473);
				const FIntPoint TestCoord(3,3);
				for(FGridCell& Cell:QuakeTerrain->Cells)
				{
					const FIntPoint Coord=Cell.Coords;Cell=FGridCell();Cell.Coords=Coord;
					Cell.FaultLineProximity=.95f;Cell.SoilType=Coord==TestCoord?EAftershockSoilType::Alluvium:EAftershockSoilType::Bedrock;
				}
				UBuildingData* QuakeHouse=NewObject<UBuildingData>(QuakeSimulation);
				QuakeHouse->MeshIntact=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
				QuakeHouse->Residents=0;QuakeHouse->Workers=0;QuakeHouse->BaseResistance.Add(EDisasterType::Earthquake,.6f);
				int32 QuakeHouseID=INDEX_NONE;FBuildingInstance BeforeQuake;
				if(QuakeBuildings->PlaceBuilding(TestCoord,QuakeHouse,EFoundationQuality::Medium,1,QuakeHouseID)
					&&QuakeBuildings->GetBuildingByID(QuakeHouseID,BeforeQuake))
				{
					constexpr float Base=30.f;const FGridCell TestCell=QuakeTerrain->GetCell(TestCoord);
					const float ExpectedLocal=Base*(1.f+TestCell.FaultLineProximity*.5f)*1.18f;
					FBuildingInstance ExpectedBuilding=BeforeQuake,DoubleBuilding=BeforeQuake;
					const FDisasterOutcome Expected=QuakeTerrain->EvaluateBuildingInDisaster(ExpectedBuilding,EDisasterType::Earthquake,Base,1.f,true);
					const FDisasterOutcome Doubled=QuakeTerrain->EvaluateBuildingInDisaster(DoubleBuilding,EDisasterType::Earthquake,ExpectedLocal,1.f,true);
					const FDisasterReport ActualReport=QuakeSimulation->TriggerDisaster(EDisasterType::Earthquake,Base,1.f,true);
					FBuildingInstance ActualBuilding;
					OnceMatches=!ActualReport.bWasRejected&&QuakeBuildings->GetBuildingByID(QuakeHouseID,ActualBuilding)
						&&Expected.FinalDamage>0.f&&Doubled.FinalDamage>Expected.FinalDamage+.1f
						&&FMath::IsNearlyEqual(ActualBuilding.CurrentDamage,Expected.FinalDamage,.0001f)&&ActualBuilding.DamageState==Expected.NewDamageState;
					const auto& Snapshot=QuakeSimulation->GetLatestVisualSnapshot();
					FieldMatches=Snapshot.Type==EDisasterType::Earthquake&&FMath::IsNearlyEqual(Snapshot.DurationSeconds,14.f)
						&&Snapshot.LocalSeverity.Num()==QuakeTerrain->Cells.Num();
					for(const FGridCell& Cell:QuakeTerrain->Cells)
					{
						const float* Local=Snapshot.LocalSeverity.Find(Cell.Coords);
						const float ExpectedCell=Base*(1.f+Cell.FaultLineProximity*.5f)*(Cell.SoilType==EAftershockSoilType::Alluvium?1.18f:1.f);
						FieldMatches&=Local&&FMath::IsNearlyEqual(*Local,ExpectedCell,.0001f);
					}
				}
			}
		}
		Check(TEXT("earthquake_actual_building_damage_equals_one_canonical_terrain_evaluation"),OnceMatches);
		Check(TEXT("earthquake_snapshot_matches_canonical_local_field_and_retains_14_second_duration"),FieldMatches);
	}
	{
		const float NaN=std::numeric_limits<float>::quiet_NaN();
		const EDisasterType Types[]={EDisasterType::Earthquake,EDisasterType::Flood,EDisasterType::Wind,EDisasterType::Tsunami,EDisasterType::Fire,EDisasterType::Volcano,EDisasterType::Drought};
		const auto TerrainSignature=[Terrain]()
		{
			uint32 Signature=GetTypeHash(Terrain->ActiveDroughtSeverity);
			for (const FGridCell& Cell:Terrain->Cells)
			{
				Signature=HashCombine(Signature,GetTypeHash(Cell.FloodDepth));
				Signature=HashCombine(Signature,GetTypeHash(Cell.ForestFuel01));
				Signature=HashCombine(Signature,GetTypeHash(Cell.bBurning));
				Signature=HashCombine(Signature,GetTypeHash(Cell.BurningTurns));
				Signature=HashCombine(Signature,GetTypeHash(Cell.SoilMoisture01));
				Signature=HashCombine(Signature,GetTypeHash(Cell.CropHealth01));
				Signature=HashCombine(Signature,GetTypeHash(Cell.DroughtStress01));
				Signature=HashCombine(Signature,GetTypeHash(Cell.LavaCoverage01));
				Signature=HashCombine(Signature,GetTypeHash(Cell.AshLoad01));
				Signature=HashCombine(Signature,GetTypeHash(Cell.RiverFill01));
				Signature=HashCombine(Signature,GetTypeHash(Cell.DamStoredWaterUnits));
			}
			return Signature;
		};
		const uint32 TerrainBefore=TerrainSignature(); const int32 RevisionBefore=Simulation->GetLatestVisualSnapshot().Revision;
		const int64 MoneyBefore=Economy->TreasuryTL, AidBefore=Economy->PendingStateAidTL;
		const float SatisfactionBefore=Economy->PublicSatisfaction;
		int32 SnapshotEvents=0;
		const FDelegateHandle InvalidInputHandle=Simulation->OnDisasterVisualSnapshotChanged.AddLambda([&SnapshotEvents](const FAftershockDisasterVisualSnapshot&){++SnapshotEvents;});
		bool RejectedAll=true;
		for (EDisasterType Type:Types)
		{
			const FDisasterReport Reports[]={Simulation->TriggerDisaster(Type,NaN,1.f,true),Simulation->TriggerDisaster(Type,-1.f,1.f,true),Simulation->TriggerDisaster(Type,1.f,NaN,true)};
			for (const FDisasterReport& Invalid:Reports) RejectedAll &= Invalid.bWasRejected && !Invalid.RejectionReason.IsEmpty();
			RejectedAll &= Simulation->GetLatestVisualSnapshot().Revision==RevisionBefore && Economy->TreasuryTL==MoneyBefore
				&& Economy->PendingStateAidTL==AidBefore && Economy->PublicSatisfaction==SatisfactionBefore && TerrainSignature()==TerrainBefore;
		}
		Simulation->OnDisasterVisualSnapshotChanged.Remove(InvalidInputHandle);
		Check(TEXT("all_seven_hazards_reject_nan_negative_and_invalid_awareness_without_side_effects"),RejectedAll && SnapshotEvents==0);
	}
	for (int32 Index=0;Index<7;++Index)
	{
		Terrain->GenerateProceduralTerrain(50,50,500,1337+Index);
		const bool Applied=Terrain->ApplyOfflineGeography(Index); const auto& Geo=GetAftershockGeography(Index);
		bool Finite=Applied; float Min=MAX_flt,Max=-MAX_flt;
		for (const FGridCell& Cell:Terrain->Cells) { Finite&=FMath::IsFinite(Cell.RealElevationMetres) && FMath::IsFinite(Cell.Elevation); Min=FMath::Min(Min,Cell.RealElevationMetres); Max=FMath::Max(Max,Cell.RealElevationMetres); }
		const FVector Anchor=AftershockLatLonToGlobeNormal(Geo.Latitude,Geo.Longitude,Index);
		const FString Name=FString::Printf(TEXT("offline_source_and_shared_real_globe_anchor_%d"),Index);
		Check(*Name,Finite && Max>Min && Anchor.Equals(FVector::UpVector,.00001) && Terrain->GeographyIndex==Index);
	}
	// These fixed-cell physics regressions target the original authored V1
	// campaign layouts. The source coverage loop above deliberately exercises V2.
	Terrain->GenerateProceduralTerrain(50,50,500,43881); Terrain->ApplyLegacyOfflineGeography(4);
	const FGridCell FireBefore=Terrain->GetCell(FIntPoint(12,16));
	const auto Fire=Simulation->SimulateForestFireMap(76.f), FireAgain=Simulation->SimulateForestFireMap(76.f);
	bool ValidFire=!Fire.IsEmpty() && Fire.Num()<Terrain->Cells.Num();
	for (const auto& P:Fire) ValidFire&=Terrain->IsPlayableCoord(P.Key) && !Terrain->GetCell(P.Key).bIsRiverbed && P.Value>0.f && FMath::IsFinite(P.Value);
	Check(TEXT("forest_fire_local_finite_and_preview_pure"),ValidFire && SameField(Fire,FireAgain) && Terrain->GetCell(FIntPoint(12,16)).ForestFuel01==FireBefore.ForestFuel01 && !Terrain->GetCell(FIntPoint(12,16)).bBurning);
	// A complete nonburnable river strip cannot be jumped by four-neighbour fire.
	for (FGridCell& Cell:Terrain->Cells)
	{
		// Explicit fixture keeps the reproducible source left of the barrier;
		// fuel remains available on BOTH banks, so this is a connectivity probe.
		if (Cell.bIsPlayable) { Cell.bIsRiverbed=false; Cell.ForestFuel01=.65f; }
		if (Cell.Coords==FIntPoint(12,16)) Cell.ForestFuel01=1.f;
		if (Cell.Coords.X==15) { Cell.bIsRiverbed=true; Cell.ForestFuel01=0.f; }
	}
	const auto BlockedFire=Simulation->SimulateForestFireMap(100.f); bool Crossed=false;
	for (const auto& P:BlockedFire) Crossed|=P.Key.X>15;
	Check(TEXT("forest_fire_cannot_jump_disconnected_nonfuel_river_strip"),!BlockedFire.IsEmpty() && !Crossed);
	FText Reason; const int32 Revision=Simulation->GetLatestVisualSnapshot().Revision;
	const FDisasterReport Rejected=Simulation->TriggerDisaster(EDisasterType::Volcano,80.f,1.f,true);
	Check(TEXT("volcano_unmapped_rejected_without_snapshot"),!Simulation->CanTriggerDisaster(EDisasterType::Volcano,Reason) && Rejected.bWasRejected && Simulation->GetLatestVisualSnapshot().Revision==Revision);
	Terrain->GenerateProceduralTerrain(50,50,500,55153); Terrain->ApplyLegacyOfflineGeography(5);
	TMap<FIntPoint,float> Lava,Ash; const auto Volcanic=Simulation->SimulateVolcanoMap(80.f,Lava,Ash);
	bool Downhill=!Lava.IsEmpty() && Lava.Contains(Terrain->VolcanicVentCoord) && !Ash.IsEmpty();
	const FIntPoint Steps[]={FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1)};
	for (const auto& P:Lava)
	{
		Downhill&=Terrain->IsPlayableCoord(P.Key) && FMath::IsFinite(P.Value) && P.Value>0.f;
		if (P.Key==Terrain->VolcanicVentCoord) continue;
		bool HasUpstream=false;
		for (FIntPoint Step:Steps) if (Lava.FindRef(P.Key+Step)>P.Value && Terrain->GetCell(P.Key).Elevation<=Terrain->GetCell(P.Key+Step).Elevation+3.01f) HasUpstream=true;
		Downhill&=HasUpstream;
	}
	Check(TEXT("mapped_etna_has_separate_downhill_lava_and_directional_ash"),Downhill && !Volcanic.IsEmpty() && Simulation->CanTriggerDisaster(EDisasterType::Volcano,Reason));
	Terrain->GenerateProceduralTerrain(50,50,500,67319); Terrain->ApplyLegacyOfflineGeography(6);
	const FIntPoint Field(25,25); const FGridCell Before=Terrain->GetCell(Field);
	const auto DryPreview=Simulation->SimulateDroughtMap(82.f);
	Check(TEXT("drought_preview_is_pure"),!DryPreview.IsEmpty() && Terrain->GetCell(Field).SoilMoisture01==Before.SoilMoisture01);
	const FDisasterReport Dry=Simulation->TriggerDisaster(EDisasterType::Drought,82.f,1.f,true);
	const FGridCell After=Terrain->GetCell(Field);
	Check(TEXT("drought_is_persistent_food_water_state_never_fake_collapse"),Dry.DisasterType==EDisasterType::Drought && !Dry.bWasRejected && Dry.TotalBuildingsCollapsed==0 && Dry.TotalBuildingsAffected==0 && Dry.TotalDeaths==0
		&& After.SoilMoisture01<Before.SoilMoisture01 && After.CropHealth01<Before.CropHealth01 && Terrain->ActiveDroughtSeverity==82.f && Simulation->GetLatestVisualSnapshot().LocalSeverity.Num()==DryPreview.Num());
	Terrain->AdvanceEnvironmentalTurn(); const FGridCell NextTurn=Terrain->GetCell(Field);
	Check(TEXT("environment_turn_degrades_actual_soil_crop_and_source"),NextTurn.SoilMoisture01<After.SoilMoisture01 && NextTurn.CropHealth01<After.CropHealth01 && NextTurn.RiverFill01<After.RiverFill01);
	Simulation->TriggerDisaster(EDisasterType::Flood,100.f,1.f,true);
	Simulation->TriggerDisaster(EDisasterType::Flood,100.f,1.f,true);
	const FGridCell Rain=Terrain->GetCell(Field);
	Terrain->AdvanceEnvironmentalTurn();
	Check(TEXT("actual_rainfall_ends_drought_and_allows_gradual_crop_recovery"),Terrain->ActiveDroughtSeverity<=0.f && Rain.SoilMoisture01>NextTurn.SoilMoisture01 && Terrain->GetCell(Field).CropHealth01>Rain.CropHealth01);
	const int64 Money=Economy->TreasuryTL; int32 Cost=0; const bool Surveyed=Simulation->SurveyGround(Field,Cost); int32 DuplicateCost=-1;
	Check(TEXT("ground_survey_atomic_single_charge_and_known_class"),Surveyed && Cost==500 && Money-Economy->TreasuryTL==500 && Terrain->GetCell(Field).bGroundSurveyed && !Simulation->SurveyGround(Field,DuplicateCost) && DuplicateCost==0 && Money-Economy->TreasuryTL==500);
	{
		// Separate real-source fixture: Etna's mapped vent is outside the city's
		// clipped ground window. Regional volcanic geometry may use the SAME
		// versioned source, but may not invent cells, elevations or hazard results.
		FGeographicWorld VolcanoFixture;VolcanoFixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("RealEtnaVolcano3DFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
		Check(TEXT("real_etna_three_dimensional_visual_fixture_created"),VolcanoFixture.World!=nullptr);
		if(VolcanoFixture.World)
		{
			auto* VolcanoWorld=VolcanoFixture.World;GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(VolcanoWorld);
			auto* Grid=VolcanoWorld->GetSubsystem<UTerrainGridSubsystem>();auto* Hazards=VolcanoWorld->GetSubsystem<UDisasterSimulationSubsystem>();FText Error;
			Grid->GenerateProceduralTerrain(125,125,1000.f,55153);const bool RealSource=Grid->ApplyOfflineGeography(5,0,false,3);
			const bool SourceChunk=RealSource&&Grid->EnsureChunkGenerated(UTerrainGridSubsystem::CellToChunk(Grid->VolcanicVentCoord),Error);
			Check(TEXT("real_etna_uses_detailed_source_and_actual_mapped_vent_chunk"),RealSource&&SourceChunk&&Grid->ContinuationGeneratorVersion==3&&Hazards->CanTriggerDisaster(EDisasterType::Volcano,Error));
			if(RealSource&&SourceChunk)
			{
				auto* PC=VolcanoWorld->SpawnActor<APlayerController>();auto* Camera=VolcanoWorld->SpawnActor<AAftershockCameraPawn>();
				if(PC&&Camera){VolcanoWorld->AddController(PC);PC->Possess(Camera);Camera->InitializeCameraForGrid(125,125,1000.f);FAftershockCameraSaveState View;Camera->CaptureSaveState(View);View.LocalFocus=Grid->GridToWorld(FIntPoint(62,62));View.Location=View.LocalFocus;View.Zoom=28000.f;View.OverviewBlend=0;Camera->RestoreSaveState(View,Error);}
				auto* Passive=VolcanoWorld->SpawnActor<AAftershockEcologyStateActor>();auto* Event=VolcanoWorld->SpawnActor<AAftershockDisasterPresentationActor>();
				if(Passive)Passive->Tags.AddUnique(TEXT("Aftershock_MeshCpuAudit"));if(Event)Event->Tags.AddUnique(TEXT("Aftershock_MeshCpuAudit"));
				Check(TEXT("real_etna_uses_real_camera_and_bounded_existing_display_actors"),PC&&Camera&&VolcanoWorld->GetFirstPlayerController()==PC&&Passive&&Event);
				if(Passive&&Event)
				{
					FAftershockTerrainSaveState BeforePassive,AfterPassive;Grid->CaptureSaveState(BeforePassive);Passive->RefreshFromTerrain(*Grid,EDisasterType::Flood,false);Grid->CaptureSaveState(AfterPassive);
					auto* Vent=FindVolcanoSurface(Passive,TEXT("RegionalCanonicalVolcanoVent"));const auto VentStats=Passive->GetRegionalVolcanoVisualStats();
					Check(TEXT("real_etna_passive_local_vent_exists_before_eruption_outside_city_window"),VentStats.bSourceResident&&VentStats.bSourceContextBacked&&!VentStats.bSourceInDrawableWindow&&VentStats.VentTriangles>0&&Vent&&Vent->GetStaticMesh());
					Check(TEXT("passive_vent_preserves_all_actual_dem_cells_and_volcanic_authority"),FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&BeforePassive,&AfterPassive,0));
					Hazards->TriggerDisaster(EDisasterType::Volcano,80.f,1.f,true);const auto Snapshot=Hazards->GetLatestVisualSnapshot();
					FAftershockTerrainSaveState BeforeDisplay,AfterDisplay;Grid->CaptureSaveState(BeforeDisplay);const int32 ActorsBefore=VolcanoWorld->GetActorCount();Event->BeginSnapshot(Snapshot,*Grid);Event->AnimateSnapshot(Snapshot,*Grid,7.f);
					const auto Stats=Event->GetVolcanoVisualStats();auto* LavaSurface=FindVolcanoSurface(Event,TEXT("CanonicalVolcanoLavaVolume"));auto* Plume=FindVolcanoSurface(Event,TEXT("CanonicalVolcanoWindBentPlume"));
					Check(TEXT("real_etna_eruption_builds_canonical_bounded_closed_lava_volume"),Stats.CanonicalLavaCells>0&&Stats.DrawnLavaCells>0&&Stats.DrawnLavaCells<=1500&&Stats.LavaTopTriangles==Stats.DrawnLavaCells*8&&Stats.LavaSkirtTriangles>0&&LavaSurface&&LavaSurface->GetStaticMesh());
					float ActualLava=0,ActualGround=0;const bool SourceClear=HighestVolcanoSurface(LavaSurface,Stats.SourceWorld,ActualLava)&&Grid->TrySampleWorldElevation(Stats.SourceWorld,ActualGround)&&ActualLava-ActualGround>=13.9f;
					Check(TEXT("real_etna_actual_lava_render_mesh_clears_same_versioned_source_ground"),SourceClear&&Stats.MinimumGroundClearanceCm>=14.f);
					Check(TEXT("real_etna_has_actual_wind_bent_plume_and_single_spatter_batch"),Stats.bMaterialsReady&&Stats.PlumeTriangles>0&&Plume&&Plume->GetStaticMesh()&&Stats.SpatterInstances==96&&VolcanoWorld->GetActorCount()==ActorsBefore);
					Passive->RefreshFromTerrain(*Grid,EDisasterType::Volcano,true);auto* Persistent=FindVolcanoSurface(Passive,TEXT("PersistentCanonicalLavaVolume"));
					Check(TEXT("active_eruption_does_not_duplicate_persistent_coplanar_lava"),Persistent&&!Persistent->GetStaticMesh());
					Grid->CaptureSaveState(AfterDisplay);Check(TEXT("actual_eruption_visuals_preserve_canonical_hazard_and_real_terrain_snapshot"),FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&BeforeDisplay,&AfterDisplay,0));
					Event->ClearPresentation();TArray<UInstancedStaticMeshComponent*> Batches;Event->GetComponents(Batches);int32 Spatter=-1;for(auto* Batch:Batches)if(Batch->GetName()==TEXT("CanonicalVolcanoSpatter"))Spatter=Batch->GetInstanceCount();
					Check(TEXT("volcano_event_clear_releases_actual_mesh_references_and_spatter_instances"),LavaSurface&&!LavaSurface->GetStaticMesh()&&Plume&&!Plume->GetStaticMesh()&&Spatter==0&&Event->GetRetainedTransientMeshBytes()==0);
					Passive->RefreshFromTerrain(*Grid,EDisasterType::Volcano,false);
					Check(TEXT("cooled_crust_handoff_keeps_actual_lava_mesh_without_active_duplicate"),Persistent&&Persistent->GetStaticMesh()&&Passive->GetRegionalVolcanoVisualStats().DrawnLavaCells>0&&LavaSurface&&!LavaSurface->GetStaticMesh());
				}
			}
		}
	}
	Terrain->GenerateProceduralTerrain(24,24,500,1337);
	Simulation->TriggerDisaster(EDisasterType::Flood,80.f,1.f,true);
	const FAftershockDisasterVisualSnapshot WaterSnapshot=Simulation->GetLatestVisualSnapshot();
	FActorSpawnParameters Spawn; Spawn.ObjectFlags|=RF_Transient; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AAftershockDisasterPresentationActor* Visual=World->SpawnActor<AAftershockDisasterPresentationActor>(FVector::ZeroVector,FRotator::ZeroRotator,Spawn);
	bool RiverFirst=false,LandLater=false;
	if (Visual)
	{
		Visual->BeginSnapshot(WaterSnapshot,*Terrain); Visual->AnimateSnapshot(WaterSnapshot,*Terrain,3.f);
		int32 StageInstances=0; FParse::Value(*Visual->DescribeCurrentVisuals(),TEXT("riverStageInstances="),StageInstances);
		bool EarlyDry=true; for (const FGridCell& Cell:Terrain->Cells) if (Cell.bIsPlayable && !Cell.bIsRiverbed) EarlyDry&=Visual->GetDisplayedFloodDepthCm(Cell.Coords)<=0.f;
		RiverFirst=StageInstances>0 && EarlyDry && !WaterSnapshot.RiverFillBefore.IsEmpty() && WaterSnapshot.RiverFillAfter.Num()==WaterSnapshot.RiverFillBefore.Num();
		Visual->AnimateSnapshot(WaterSnapshot,*Terrain,10.f);
		for (const FGridCell& Cell:Terrain->Cells) if (Cell.bIsPlayable && !Cell.bIsRiverbed && Visual->GetDisplayedFloodDepthCm(Cell.Coords)>12.f) LandLater=true;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_RIVER_SEQUENCE_AUDIT %s"),*Visual->DescribeCurrentVisuals());
		Visual->ClearPresentation(); Visual->Destroy();
	}
	Check(TEXT("canonical_river_fill_visual_precedes_land_overflow"),RiverFirst && LandLater);
	UBuildingManagerSubsystem* Buildings=World->GetSubsystem<UBuildingManagerSubsystem>();
	UBuildingData* TestHouse=NewObject<UBuildingData>(Simulation);
	TestHouse->MeshIntact=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")); TestHouse->Residents=0; TestHouse->Workers=0;
	int32 HouseID=INDEX_NONE; bool ExistingRubbleNoFreshAid=false;
	for (const FGridCell& Cell:Terrain->Cells)
	{
		if (!Buildings || !Buildings->PlaceBuilding(Cell.Coords,TestHouse,EFoundationQuality::Strong,1,HouseID)) continue;
		FBuildingInstance House; Buildings->GetBuildingByID(HouseID,House);
		House.CurrentDamage=100.f; House.DamageState=EDamageState::Collapsed; Buildings->UpdateBuildingInstance(House);
		const FDisasterReport Repeat=Simulation->TriggerDisaster(EDisasterType::Earthquake,1.f,1.f,true);
		ExistingRubbleNoFreshAid=Repeat.TotalBuildingsCollapsed==0 && Repeat.TotalStateAidGranted==0 && Repeat.TotalDeaths==0;
		break;
	}
	Check(TEXT("preexisting_rubble_never_repeats_new_collapse_count_or_1000tl_grant"),ExistingRubbleNoFreshAid);
	// Persistent ecology is a separate, bounded read-only display. Event timers
	// must not erase ongoing terrain consequences or keep healed ones visible.
	AAftershockEcologyStateActor* Ecology=World->SpawnActor<AAftershockEcologyStateActor>(FVector::ZeroVector,FRotator::ZeroRotator,Spawn);
	Check(TEXT("persistent_ecology_display_actor_created"),Ecology!=nullptr);
	if (Ecology)
	{
		Ecology->Tags.AddUnique(TEXT("Aftershock_MeshCpuAudit"));
		const auto Count=[Ecology](const TCHAR* Name)
		{
			int32 Value=-1; FParse::Value(*Ecology->DescribeCurrentVisuals(),Name,Value); return Value;
		};
		Terrain->GenerateProceduralTerrain(50,50,500,43881); Terrain->ApplyLegacyOfflineGeography(4);
		Simulation->TriggerDisaster(EDisasterType::Fire,76.f,1.f,true);
		Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Fire,true);
		const bool bNoDuplicateFire=Count(TEXT("fireInstances="))==0;
		Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Fire,false);
		const int32 Flames=Count(TEXT("fireInstances="));
		Check(TEXT("burning_terrain_remains_visible_after_event_without_duplicate_flames"),bNoDuplicateFire && Flames>0 && Flames<=720);
		for (int32 Turn=0;Turn<3;++Turn) Terrain->AdvanceEnvironmentalTurn();
		Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Flood,true);
		Check(TEXT("canonical_extinguished_fire_clears_persistent_flames"),Count(TEXT("fireInstances="))==0);
		Terrain->GenerateProceduralTerrain(50,50,500,55153); Terrain->ApplyLegacyOfflineGeography(5);
		Simulation->TriggerDisaster(EDisasterType::Volcano,80.f,1.f,true);
		FAftershockTerrainSaveState BeforeLava;Terrain->CaptureSaveState(BeforeLava);
		Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Volcano,false);
		auto* LavaSurface=FindVolcanoSurface(Ecology,TEXT("PersistentCanonicalLavaVolume"));
		const auto& LavaStats=Ecology->GetRegionalVolcanoVisualStats();
		const auto* LavaRender=LavaSurface&&LavaSurface->GetStaticMesh()?LavaSurface->GetStaticMesh()->GetRenderData():nullptr;
		bool PositiveClearance=false;
		for(const auto& Cell:Terrain->Cells)if(Cell.LavaCoverage01>.01f&&Cell.bIsPlayable)
		{const FVector Point=Terrain->GridToWorld(Cell.Coords);float Height=0,Ground=0;if(HighestVolcanoSurface(LavaSurface,Point,Height)&&Terrain->TrySampleWorldElevation(Point,Ground)){PositiveClearance=Height-Ground>=13.9f;break;}}
		Check(TEXT("canonical_lava_and_ash_remain_bounded_after_event"),LavaStats.DrawnLavaCells>0&&LavaStats.DrawnLavaCells<=1500&&LavaStats.LavaTopTriangles>0&&LavaStats.LavaSkirtTriangles>0&&LavaRender&&!LavaRender->LODResources.IsEmpty()&&LavaRender->LODResources[0].IndexBuffer.GetNumIndices()>0&&PositiveClearance&&Count(TEXT("ashInstances="))>0&&Count(TEXT("ashInstances="))<=240);
		FAftershockTerrainSaveState AfterLava;Terrain->CaptureSaveState(AfterLava);
		Check(TEXT("persistent_three_dimensional_lava_does_not_mutate_canonical_terrain"),FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&BeforeLava,&AfterLava,0));
		for (int32 Turn=0;Turn<100;++Turn) Terrain->AdvanceEnvironmentalTurn();
		Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Wind,true);
		Check(TEXT("canonical_cooled_lava_and_settled_ash_clear_display"),Ecology->GetRegionalVolcanoVisualStats().DrawnLavaCells==0&&LavaSurface&&!LavaSurface->GetStaticMesh()&&Count(TEXT("ashInstances="))==0);
		Terrain->GenerateProceduralTerrain(50,50,500,67319); Terrain->ApplyLegacyOfflineGeography(6);
		Simulation->TriggerDisaster(EDisasterType::Drought,82.f,1.f,true);
		const FGridCell DryCell=Terrain->GetCell(Field); const float DrySeverity=Terrain->ActiveDroughtSeverity;
		Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Drought,false);
		const int32 DryInstances=Count(TEXT("droughtInstances="));
		Check(TEXT("persistent_drought_display_is_bounded_and_never_writes_gameplay"),DryInstances>0 && DryInstances<=800 && Terrain->ActiveDroughtSeverity==DrySeverity
			&& Terrain->GetCell(Field).SoilMoisture01==DryCell.SoilMoisture01 && Terrain->GetCell(Field).CropHealth01==DryCell.CropHealth01);
		Simulation->TriggerDisaster(EDisasterType::Flood,100.f,1.f,true);
		Simulation->TriggerDisaster(EDisasterType::Flood,100.f,1.f,true);
		Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Flood,true);
		Check(TEXT("actual_rain_recovery_clears_persistent_drought_during_new_weather"),Terrain->ActiveDroughtSeverity<=0.f && Count(TEXT("droughtInstances="))==0);
		Ecology->Destroy();
	}
	Terrain->GenerateProceduralTerrain(24,24,500,1337);
	Visual=World->SpawnActor<AAftershockDisasterPresentationActor>(FVector::ZeroVector,FRotator::ZeroRotator,Spawn);
	if (Visual)
	{
		const auto CountVisual=[Visual](const TCHAR* Name)
		{
			int32 Value=-1; FParse::Value(*Visual->DescribeCurrentVisuals(),Name,Value); return Value;
		};
		Simulation->TriggerDisaster(EDisasterType::Wind,78.f,1.f,true);
		const FAftershockDisasterVisualSnapshot WindSnapshot=Simulation->GetLatestVisualSnapshot();
		// The canonical field now uses pressure (q=.5*rho*v^2), upwind shelter
		// and channel alignment. Do not silently restore the former linear-only
		// ValleyWindMultiplier formula in this visual-contract test.
		FAftershockTerrainSaveState BeforeWindPreview,AfterWindPreview;Terrain->CaptureSaveState(BeforeWindPreview);
		const auto ExpectedWind=Simulation->SimulateWindMap(WindSnapshot.EffectiveIntensity);Terrain->CaptureSaveState(AfterWindPreview);
		bool SameTerrainWind=SameField(WindSnapshot.LocalSeverity,ExpectedWind)&&!ExpectedWind.IsEmpty()
			&&FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&BeforeWindPreview,&AfterWindPreview,0);
		for(const auto& Pair:WindSnapshot.LocalSeverity)SameTerrainWind&=Terrain->IsPlayableCoord(Pair.Key)&&!Terrain->GetCell(Pair.Key).bIsOcean&&FMath::IsFinite(Pair.Value)&&Pair.Value>=0.f&&Pair.Value<=100.f;
		Visual->BeginSnapshot(WindSnapshot,*Terrain); Visual->AnimateSnapshot(WindSnapshot,*Terrain,4.f);
		bool ExactWindInstances=Visual->WindStreakISM&&Visual->WindStreakISM->GetInstanceCount()==Visual->WindStreaks.Num()&&WindSnapshot.GetEffectiveDurationSeconds()>=12.f;int32 DrawnWindInstances=0;
		const FVector WindDirection=WindSnapshot.DominantWindDirection;const float WindYaw=FMath::RadiansToDegrees(FMath::Atan2(WindDirection.Y,WindDirection.X));
		for(int32 Index=0;Index<Visual->WindStreaks.Num();++Index)
		{
			const auto& Streak=Visual->WindStreaks[Index];const float SourceStrength=FMath::Clamp(WindSnapshot.LocalSeverity.FindRef(Streak.Cell)/100.f,0.f,1.5f);
			const float BaseLength=FMath::Lerp(120.f,480.f,FMath::Min(1.f,SourceStrength));
			ExactWindInstances&=FMath::IsNearlyEqual(Streak.LocalStrength,SourceStrength,.0001f)
				&&FMath::IsNearlyEqual(Streak.VisualSpeed,FMath::Lerp(300.f,1600.f,FMath::Min(1.f,SourceStrength)),.001f)
				&&Streak.Length>=BaseLength*.7f-.001f&&Streak.Length<=BaseLength*1.2f+.001f;
			const float Travel=Terrain->CellSize*4.f;const float Along=FMath::Fmod(Streak.Phase*Travel+4.f*Streak.VisualSpeed,Travel)-Travel*.5f;
			FVector ExpectedPosition=Terrain->GridToWorld(Streak.Cell)+Streak.Offset+WindDirection*Along;const FIntPoint At=Terrain->WorldToGrid(ExpectedPosition);FTransform Actual;
			const bool HasTransform=Visual->WindStreakISM&&Visual->WindStreakISM->GetInstanceTransform(Index,Actual,true);ExactWindInstances&=HasTransform;
			if(!HasTransform)continue;
			if(!Visual->GetBuiltDrawableCellBounds().Contains(At)||!Terrain->IsPlayableCoord(At))
			{ExactWindInstances&=Actual.GetScale3D().IsNearlyZero();continue;}
			++DrawnWindInstances;ExpectedPosition.Z=Terrain->GetCell(At).Elevation+Streak.Offset.Z;
			const float CurrentStrength=FMath::Clamp(WindSnapshot.LocalSeverity.FindRef(At)/100.f,0.f,1.5f);
			const float Pulse=.55f+.45f*FMath::Sin((Streak.Phase+4.f*.7f)*2.f*PI);
			const FVector ExpectedScale(Streak.Length/100.f,(18.f+12.f*CurrentStrength)*Pulse/100.f,1.f);
			ExactWindInstances&=Actual.GetLocation().Equals(ExpectedPosition,.01)&&Actual.GetScale3D().Equals(ExpectedScale,.0001)
				&&Actual.Rotator().Equals(FRotator(0.f,WindYaw,0.f),.001f);
		}
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_WIND_VISUAL_CONTRACT canonicalFieldMatches=%d exactActualISM=%d drawnInstances=%d totalInstances=%d canonicalPressureField=1 linearValleyFormulaAssumed=0"),SameTerrainWind,ExactWindInstances,DrawnWindInstances,Visual->WindStreaks.Num());
		Check(TEXT("wind_uses_same_terrain_amplification_and_bounded_streaks_only"),SameTerrainWind&&ExactWindInstances&&DrawnWindInstances>0&&CountVisual(TEXT("windInstances="))>0 && CountVisual(TEXT("windInstances="))<=320
			&& CountVisual(TEXT("rainInstances="))==0 && CountVisual(TEXT("floodMeshInstances="))==0);
		Simulation->TriggerDisaster(EDisasterType::Flood,80.f,1.f,true);
		const FAftershockDisasterVisualSnapshot FloodSnapshot=Simulation->GetLatestVisualSnapshot();
		Visual->BeginSnapshot(FloodSnapshot,*Terrain); Visual->AnimateSnapshot(FloodSnapshot,*Terrain,10.f);
		Check(TEXT("weather_switch_clears_wind_and_uses_bounded_rain_single_flood_mesh"),CountVisual(TEXT("windInstances="))==0 && CountVisual(TEXT("rainInstances="))>0
			&& CountVisual(TEXT("rainInstances="))<=640 && CountVisual(TEXT("floodMeshInstances="))<=1 && CountVisual(TEXT("riverStageInstances="))>0);
		Visual->ClearPresentation();
		Check(TEXT("event_clear_removes_weather_water_and_river_instances"),Visual->IsHidden() && CountVisual(TEXT("windInstances="))==0 && CountVisual(TEXT("rainInstances="))==0
			&& CountVisual(TEXT("floodMeshInstances="))==0 && CountVisual(TEXT("riverStageInstances="))==0);
		Visual->Destroy();
	}
	else
	{
		Check(TEXT("wind_uses_same_terrain_amplification_and_bounded_streaks_only"),false);
		Check(TEXT("weather_switch_clears_wind_and_uses_bounded_rain_single_flood_mesh"),false);
		Check(TEXT("event_clear_removes_weather_water_and_river_instances"),false);
	}
	return Finish();
}
#else
UAftershockGeographicHazardsAuditCommandlet::UAftershockGeographicHazardsAuditCommandlet() {}
int32 UAftershockGeographicHazardsAuditCommandlet::Main(const FString& Params) { return 1; }
#endif
