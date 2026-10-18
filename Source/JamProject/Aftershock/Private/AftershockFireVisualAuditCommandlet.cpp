#include "AftershockFireVisualAuditCommandlet.h"

#if WITH_EDITOR
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/DisasterPresentationSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "AftershockPresentationBudget.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Scalability.h"
#include "StaticMeshResources.h"
#include "TimerManager.h"

namespace
{
	struct FFireVisualAuditWorld
	{
		UWorld* World=nullptr;
		~FFireVisualAuditWorld()
		{
			if(!World)return;World->DestroyWorld(false);
			if(GEngine)GEngine->DestroyWorldContext(World);World->RemoveFromRoot();
		}
	};
	struct FFireVisualAuditQuality
	{
		Scalability::FQualityLevels Original=Scalability::GetQualityLevels();
		~FFireVisualAuditQuality(){Scalability::SetQualityLevels(Original);}
		void SetEffects(int32 Quality)const{auto Value=Original;Value.EffectsQuality=Quality;Scalability::SetQualityLevels(Value);}
	};
	bool FireVisualAuditNonPlanar(const UInstancedStaticMeshComponent* Batch)
	{
		const UStaticMesh* Mesh=Batch?Batch->GetStaticMesh().Get():nullptr;
		if(!Mesh||!Mesh->GetRenderData()||Mesh->GetRenderData()->LODResources.IsEmpty())return false;
		const FVector Size=Mesh->GetBoundingBox().GetSize();
		return Size.GetMin()>1.f&&Mesh->GetRenderData()->LODResources[0].GetNumTriangles()>24;
	}
	bool FireVisualAuditExactMaterial(const UInstancedStaticMeshComponent* Batch,const TCHAR* ExpectedPath)
	{
		const auto* Material=Batch?Cast<UMaterialInstanceDynamic>(Batch->GetMaterial(0)):nullptr;
		const UMaterialInterface* Parent=Material?Material->Parent.Get():nullptr;
		return Parent&&Parent->GetPathName()==ExpectedPath;
	}
	bool FireVisualAuditActive(const UInstancedStaticMeshComponent* Batch)
	{
		if(!Batch||!Batch->IsVisible())return false;
		for(int32 Index=0;Index<Batch->GetInstanceCount();++Index)
		{
			FTransform Transform;if(Batch->GetInstanceTransform(Index,Transform,true)&&!Transform.ContainsNaN()&&Transform.GetScale3D().GetMin()>0.001)return true;
		}
		return false;
	}
}
#endif

UAftershockFireVisualAuditCommandlet::UAftershockFireVisualAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}

int32 UAftershockFireVisualAuditCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	int32 Passed=0,Failed=0;TArray<FString> Checks;
	const auto Check=[&](const TCHAR* Name,bool Value)
	{
		Value?++Passed:++Failed;
		Checks.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"),Name,Value?TEXT("true"):TEXT("false")));
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_FIRE_VISUAL_CHECK case=%s passed=%d"),Name,Value);
	};
	const auto Finish=[&]()
	{
		const FString Folder=FPaths::ProjectSavedDir()/TEXT("PresentationValidation");IFileManager::Get().MakeDirectory(*Folder,true);
		const FString Path=Folder/TEXT("fire_visual_audit.json");
		const FString Report=FString::Printf(TEXT("{\"schema_version\":1,\"utc\":\"%s\",\"scope\":\"Isolated native fixture using actual fire display meshes, exact authored material parents, canonical burning-state persistence, real subsystem handoff, quality and camera tiers, and unchanged terrain/economy/population save states. No rendered image, shader RHI, GPU timing or physical combustion claim.\",\"live_rhi_verified\":false,\"passed\":%d,\"failed\":%d,\"checks\":[%s]}\n"),*FDateTime::UtcNow().ToIso8601(),Passed,Failed,*FString::Join(Checks,TEXT(",")));
		if(!FFileHelper::SaveStringToFile(Report,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return 2;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_FIRE_VISUAL_AUDIT_RESULT passed=%d failed=%d report=%s"),Passed,Failed,*Path);return Failed==0?0:1;
	};
	Check(TEXT("engine_ready"),GEngine!=nullptr);if(!GEngine)return Finish();
	FFireVisualAuditQuality Quality;Quality.SetEffects(3);
	FFireVisualAuditWorld Fixture;
	Fixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("AftershockFireVisualFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("isolated_game_world_created"),Fixture.World!=nullptr);if(!Fixture.World)return Finish();
	UWorld* World=Fixture.World;GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();auto* Economy=World->GetSubsystem<UEconomyAndStateSubsystem>();
	auto* Life=World->GetSubsystem<UCityLifeSubsystem>();auto* Presentation=World->GetSubsystem<UDisasterPresentationSubsystem>();
	Check(TEXT("canonical_subsystems_ready"),Terrain&&Economy&&Life&&Presentation);if(!Terrain||!Economy||!Life||!Presentation)return Finish();
	Terrain->GenerateProceduralTerrain(32,32,500.f,17653);
	FAftershockDisasterVisualSnapshot Snapshot;Snapshot.Revision=42;Snapshot.Type=EDisasterType::Fire;
	Snapshot.BaseSeverity=76.f;Snapshot.EffectiveIntensity=76.f;Snapshot.DurationSeconds=24.f;
	Snapshot.StartedAtWorldSeconds=World->GetTimeSeconds()-7.f;Snapshot.DominantWindDirection=FVector(1.f,.35f,0.f).GetSafeNormal();
	// Explicit deterministic fixture input; the renderer below may never alter it.
	// Real spread/casualties are tested by the separate geographic/simulation suite.
	for(FGridCell& Cell:Terrain->Cells)
	{
		Cell.bIsPlayable=true;Cell.bIsOcean=false;Cell.bIsRiverbed=false;Cell.Elevation=0.f;
		Cell.ForestFuel01=.80f;Cell.bBurning=Cell.Coords.X>=3&&Cell.Coords.X<29&&Cell.Coords.Y>=3&&Cell.Coords.Y<29;
		Cell.BurningTurns=Cell.bBurning?3:0;if(Cell.bBurning)Snapshot.LocalSeverity.Add(Cell.Coords,76.f);
	}
	FActorSpawnParameters Spawn;Spawn.ObjectFlags|=RF_Transient;Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	auto* Event=World->SpawnActor<AAftershockDisasterPresentationActor>(FVector::ZeroVector,FRotator::ZeroRotator,Spawn);
	auto* Ecology=World->SpawnActor<AAftershockEcologyStateActor>(FVector::ZeroVector,FRotator::ZeroRotator,Spawn);
	Check(TEXT("two_existing_display_actors_spawned"),Event&&Ecology);if(!Event||!Ecology)return Finish();
	Event->Tags.AddUnique(TEXT("Aftershock_MeshCpuAudit"));Ecology->Tags.AddUnique(TEXT("Aftershock_MeshCpuAudit"));
	Presentation->PresentationActor=Event;Presentation->EcologyStateActor=Ecology;
	FAftershockTerrainSaveState BeforeTerrain;Terrain->CaptureSaveState(BeforeTerrain);
	FAftershockEconomySaveState BeforeEconomy;Economy->CaptureSaveState(BeforeEconomy);
	FAftershockCityLifeSaveState BeforeLife;Life->CaptureSaveState(BeforeLife);
	const int32 ActorsBefore=World->GetActorCount();const uint64 RevisionBefore=Terrain->GetTerrainRevision();
	Presentation->HandleSnapshot(Snapshot);
	Check(TEXT("actual_subsystem_starts_bounded_fire_without_particle_actors"),Event->FireVisuals.Sources.Num()>0&&Event->FireVisuals.Sources.Num()<=192&&Event->FireBatches.Num()==4&&World->GetActorCount()==ActorsBefore);
	const TCHAR* MaterialPaths[]={TEXT("/Game/FX/Aftershock/Fire/M_FireFlameVolume.M_FireFlameVolume"),TEXT("/Game/FX/Aftershock/Fire/M_FireCoreVolume.M_FireCoreVolume"),TEXT("/Game/FX/Aftershock/Fire/M_FireSmokeVolume.M_FireSmokeVolume"),TEXT("/Game/FX/Aftershock/Fire/M_FireEmber.M_FireEmber")};
	bool Exact=Event->FireBatches.Num()==4,NonPlanar=Exact;
	if(Exact)for(int32 Layer=0;Layer<4;++Layer){Exact&=FireVisualAuditExactMaterial(Event->FireBatches[Layer],MaterialPaths[Layer]);NonPlanar&=FireVisualAuditNonPlanar(Event->FireBatches[Layer]);}
	Check(TEXT("all_four_authored_material_parents_match_exact_paths_no_fallback"),Exact&&Event->FireVisuals.AuthoredMaterials==4);
	Check(TEXT("all_four_fire_batches_use_actual_non_planar_3d_meshes"),NonPlanar);
	const int32 Count=Event->FireVisuals.Sources.Num();
	Check(TEXT("actual_flame_core_smoke_ember_instance_counts_are_bounded"),Event->FireBatches.Num()==4&&Event->FireBatches[0]->GetInstanceCount()==Count*2&&Event->FireBatches[1]->GetInstanceCount()==Count&&Event->FireBatches[2]->GetInstanceCount()==Count*3&&Event->FireBatches[3]->GetInstanceCount()==Count*2);
	Check(TEXT("active_canonical_fire_has_nonzero_visible_flame_and_smoke_transforms"),Event->FireBatches.Num()==4&&FireVisualAuditActive(Event->FireBatches[0])&&FireVisualAuditActive(Event->FireBatches[2]));
	Check(TEXT("active_event_suppresses_duplicate_persistent_flames"),Ecology->FireVisuals.Sources.IsEmpty()&&Ecology->FireBatches.IsEmpty());
	bool NoTicks=!Event->PrimaryActorTick.bCanEverTick&&!Ecology->PrimaryActorTick.bCanEverTick;
	for(UInstancedStaticMeshComponent* Batch:Event->FireBatches)NoTicks&=!Batch->IsComponentTickEnabled();
	Check(TEXT("fire_actor_and_instance_components_have_no_tick"),NoTicks);
	Check(TEXT("event_retention_reports_reusable_fire_mesh_resources"),Event->GetRetainedTransientMeshBytes()>0);
	// Invoke the real completion callback at its real >24s age. No replacement
	// handoff code, artificial terrain healing, new hazard or elapsed-time wait.
	Presentation->ActiveSnapshot.StartedAtWorldSeconds=World->GetTimeSeconds()-25.f;
	Presentation->UpdatePresentation();
	Check(TEXT("actual_subsystem_after_25_seconds_releases_event_instances"),Event->FireVisuals.Sources.IsEmpty()&&Event->IsHidden()&&Event->FireBatches.Num()==4&&Event->FireBatches[0]->GetInstanceCount()==0&&Event->FireBatches[2]->GetInstanceCount()==0);
	Check(TEXT("canonical_burning_keeps_real_3d_flame_and_smoke_after_24_second_event"),Ecology->FireVisuals.Sources.Num()>0&&Ecology->FireBatches.Num()==4&&FireVisualAuditActive(Ecology->FireBatches[0])&&FireVisualAuditActive(Ecology->FireBatches[2])&&FireVisualAuditNonPlanar(Ecology->FireBatches[0])&&FireVisualAuditNonPlanar(Ecology->FireBatches[2]));
	bool PersistentExact=Ecology->FireBatches.Num()==4;
	if(PersistentExact)for(int32 Layer=0;Layer<4;++Layer)PersistentExact&=FireVisualAuditExactMaterial(Ecology->FireBatches[Layer],MaterialPaths[Layer]);
	Check(TEXT("persistent_handoff_also_requires_all_four_exact_materials"),PersistentExact&&Ecology->FireVisuals.AuthoredMaterials==4);
	const int32 PersistentComponents=Ecology->GetInstanceComponents().Num();
	Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Fire,false);Ecology->AnimatePersistentFire();
	Check(TEXT("unchanged_refresh_reuses_components_and_never_spawns_an_actor"),Ecology->GetInstanceComponents().Num()==PersistentComponents&&World->GetActorCount()==ActorsBefore);
	Quality.SetEffects(0);Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Fire,false);
	Check(TEXT("effects_quality_low_uses_at_most_48_sources"),Ecology->FireVisuals.BuiltEffectsQuality==0&&Ecology->FireVisuals.Sources.Num()>0&&Ecology->FireVisuals.Sources.Num()<=48);
	Quality.SetEffects(3);Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Fire,false);
	Check(TEXT("effects_quality_high_rebuilds_with_larger_bounded_source_budget"),Ecology->FireVisuals.BuiltEffectsQuality==3&&Ecology->FireVisuals.Sources.Num()>48&&Ecology->FireVisuals.Sources.Num()<=192);
	// Use a real camera's existing presentation budget, not a new LOD authority.
	auto* PC=World->SpawnActor<APlayerController>();auto* Camera=World->SpawnActor<AAftershockCameraPawn>();
	if(PC&&Camera)
	{
		World->AddController(PC);PC->Possess(Camera);Camera->InitializeCameraForGrid(32,32,500.f);
		// This isolated world intentionally never starts a GameMode/login. The
		// normal PostInitialize/owning-player path therefore has not spawned a
		// camera manager. Use the same exported controller API as that path;
		// it sets the transient owner and calls InitializeFor(this) in Engine.
		if(!PC->PlayerCameraManager)PC->SpawnPlayerCameraManager();
	}
	Check(TEXT("native_tier_fixture_has_real_player_camera_manager"),PC&&Camera&&PC->PlayerCameraManager);
	if(PC&&Camera&&PC->PlayerCameraManager)
	{
		const auto SetView=[&](float Distance,float Overview)
		{
			FAftershockCameraSaveState View;Camera->CaptureSaveState(View);View.LocalFocus=Terrain->GridToWorld(FIntPoint(16,16));View.Location=View.LocalFocus;View.Zoom=Distance;View.OverviewBlend=Overview;FText Error;
			const bool Restored=Camera->RestoreSaveState(View,Error);FMinimalViewInfo POV;POV.Location=View.LocalFocus+FVector(0,0,Distance);PC->PlayerCameraManager->SetCameraCachePOV(POV);return Restored;
		};
		Check(TEXT("city_tier_uses_at_most_128_sources"),SetView(40000.f,0.f)&&(Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Fire,false),GetAftershockPresentationBudget(World).Tier==EAftershockPresentationTier::City&&Ecology->FireVisuals.Sources.Num()>0&&Ecology->FireVisuals.Sources.Num()<=128));
		Check(TEXT("regional_tier_uses_at_most_32_sources"),SetView(180000.f,0.f)&&(Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Fire,false),GetAftershockPresentationBudget(World).Tier==EAftershockPresentationTier::Regional&&Ecology->FireVisuals.Sources.Num()>0&&Ecology->FireVisuals.Sources.Num()<=32));
		const bool Globe=SetView(180000.f,1.f);Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Fire,false);
		bool GlobeHidden=Globe&&GetAftershockPresentationBudget(World).Tier==EAftershockPresentationTier::Globe&&Ecology->FireVisuals.Sources.IsEmpty();
		for(UInstancedStaticMeshComponent* Batch:Ecology->FireBatches)GlobeHidden&=Batch->GetInstanceCount()==0&&!Batch->IsVisible();
		Check(TEXT("globe_has_no_fire_instances_or_visible_fire_batches"),GlobeHidden);
		Check(TEXT("return_from_globe_rebuilds_current_canonical_fire"),SetView(8000.f,0.f)&&(Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Fire,false),!Ecology->FireVisuals.Sources.IsEmpty()));
	}
	else
	{
		Check(TEXT("city_tier_uses_at_most_128_sources"),false);Check(TEXT("regional_tier_uses_at_most_32_sources"),false);
		Check(TEXT("globe_has_no_fire_instances_or_visible_fire_batches"),false);Check(TEXT("return_from_globe_rebuilds_current_canonical_fire"),false);
	}
	FAftershockTerrainSaveState AfterTerrain;Terrain->CaptureSaveState(AfterTerrain);
	FAftershockEconomySaveState AfterEconomy;Economy->CaptureSaveState(AfterEconomy);
	FAftershockCityLifeSaveState AfterLife;Life->CaptureSaveState(AfterLife);
	Check(TEXT("display_start_animation_handoff_and_lod_preserve_every_terrain_field"),FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&BeforeTerrain,&AfterTerrain,0)&&Terrain->GetTerrainRevision()==RevisionBefore);
	Check(TEXT("display_and_lod_preserve_entire_economy_save_state"),FAftershockEconomySaveState::StaticStruct()->CompareScriptStruct(&BeforeEconomy,&AfterEconomy,0));
	Check(TEXT("display_and_lod_preserve_entire_population_and_city_life_state"),FAftershockCityLifeSaveState::StaticStruct()->CompareScriptStruct(&BeforeLife,&AfterLife,0));
	// Canonical extinguishing is a separately labelled fixture mutation. Only
	// this explicit input change, not the renderer, may change burning flags.
	for(const FGridCell& Existing:Terrain->Cells)if(Existing.bBurning){FGridCell Changed=Existing;Changed.bBurning=false;Changed.BurningTurns=0;Terrain->SetCell(Changed.Coords,Changed);}
	FAftershockTerrainSaveState BeforeExtinguished;Terrain->CaptureSaveState(BeforeExtinguished);
	Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Fire,false);Ecology->AnimatePersistentFire();
	bool Cleared=Ecology->FireVisuals.Sources.IsEmpty()&&!World->GetTimerManager().IsTimerActive(Ecology->FireAnimationTimer);
	for(UInstancedStaticMeshComponent* Batch:Ecology->FireBatches)Cleared&=Batch->GetInstanceCount()==0&&!Batch->IsVisible();
	Check(TEXT("canonical_extinguished_flags_clear_all_flame_smoke_ember_instances_and_timer"),Cleared);
	Terrain->CaptureSaveState(AfterTerrain);
	Check(TEXT("extinguished_refresh_preserves_exact_canonical_post_extinguish_state"),FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&BeforeExtinguished,&AfterTerrain,0));
	return Finish();
#else
	return 1;
#endif
}
