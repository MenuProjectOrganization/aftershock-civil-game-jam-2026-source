#include "AftershockChunkRuntimeAudit.h"
#if !UE_BUILD_SHIPPING
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/AftershockDisasterDirectorSubsystem.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/CityPresentationSubsystem.h"
#include "AftershockGlobeSurface.h"
#include "AftershockTerrainSurface.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "GeometryCollection/GeometryCollectionActor.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"

namespace
{
struct FChunkRuntimeState
{
	TArray<FGridCell> InitialCells;
	TArray<FBuildingInstance> InitialBuildings;
	TMap<int32,FBuildingInstance> ExplicitCollapseFixtures;
	FAftershockEconomySaveState Economy;
	FAftershockDirectorState Director;
	FAftershockGlobeLayout Globe;
	FAftershockChunkSceneCapture Capture;
	FVector InitialFocus=FVector::ZeroVector;
	FString Failure;
	FTimerHandle MovementTimer,CaptureTimer;
	TWeakObjectPtr<AGeometryCollectionActor> NearActor,FarActor;
	TArray<FTransform> FarPose;
	FIntPoint NearCoord=FIntPoint(MAX_int32,MAX_int32);
	FIntRect NearOriginalWindow=FIntRect(0,0,0,0);
	int32 InitialWidth=0,InitialHeight=0,Steps=0,StalledSteps=0,Deaths=0;
	float StartedAt=0,NearSpawnAt=0,FarSpawnAt=0;
	bool bStarted=false,bWarning=false,bPanFinished=false,bReported=false,bChaosRequested=false;
	bool bNearAttempted=false,bNearProxyStarted=false,bNearReleasedBeforeFour=false,bFarStaticStarted=false;
};

bool ValidChunkWorld(UWorld* World)
{ return IsValid(World)&&!World->bIsTearingDown&&GEngine&&GEngine->GetWorldContextFromWorld(World); }

FString EscapeChunkJson(FString Value)
{
	Value.ReplaceInline(TEXT("\\"),TEXT("\\\\"));Value.ReplaceInline(TEXT("\""),TEXT("\\\""));
	Value.ReplaceInline(TEXT("\n"),TEXT("\\n"));Value.ReplaceInline(TEXT("\r"),TEXT("\\r"));Value.ReplaceInline(TEXT("\t"),TEXT("\\t"));return Value;
}

AAftershockCrowdHostActor* ChunkTerrainHost(UWorld& World)
{
	for(TActorIterator<AAftershockCrowdHostActor> It(&World);It;++It)if(!It->IsActorBeingDestroyed())return *It;
	return nullptr;
}

AGeometryCollectionActor* RubbleAt(UWorld& World,const UTerrainGridSubsystem& Terrain,FIntPoint Coord)
{
	const FVector Origin=Terrain.GridToWorld(Coord);
	for(TActorIterator<AGeometryCollectionActor> It(&World);It;++It)
		if(!It->IsActorBeingDestroyed()&&It->GetActorLocation().Equals(Origin,.1))return *It;
	return nullptr;
}

bool SameChunkPoses(const TArray<FTransform>& A,const TArray<FTransform>& B)
{
	if(A.IsEmpty()||A.Num()!=B.Num())return false;
	for(int32 I=0;I<A.Num();++I)
		if(!B[I].IsValid()||!A[I].GetTranslation().Equals(B[I].GetTranslation(),.05)||!A[I].GetRotation().Equals(B[I].GetRotation(),.00001)||!A[I].GetScale3D().Equals(B[I].GetScale3D(),.00001))return false;
	return true;
}

void CheckNearWindowExit(UWorld& World,FChunkRuntimeState& State)
{
	const auto* Host=ChunkTerrainHost(World);
	const auto* Actor=State.NearActor.Get();
	const auto* Component=Actor?Actor->GetGeometryCollectionComponent():nullptr;
	if(Component&&Host&&State.bNearProxyStarted&&World.GetTimeSeconds()-State.NearSpawnAt<4.f
		&&Host->BuiltVisibleCellBounds!=State.NearOriginalWindow&&!Host->BuiltVisibleCellBounds.Contains(State.NearCoord)
		&&Component->GetPhysicsProxy()==nullptr&&!Component->IsSimulatingPhysics())State.bNearReleasedBeforeFour=true;
}

bool FreshChunkPNG(const FString& Path)
{
	TArray<uint8> Bytes;if(!FFileHelper::LoadFileToArray(Bytes,*Path)||Bytes.Num()<24)return false;
	const uint8 Signature[]={137,80,78,71,13,10,26,10};for(int32 I=0;I<8;++I)if(Bytes[I]!=Signature[I])return false;
	const auto Read32=[&Bytes](int32 I){return(uint32(Bytes[I])<<24)|(uint32(Bytes[I+1])<<16)|(uint32(Bytes[I+2])<<8)|uint32(Bytes[I+3]);};
	return Bytes[12]=='I'&&Bytes[13]=='H'&&Bytes[14]=='D'&&Bytes[15]=='R'&&Read32(16)==1600&&Read32(20)==1000;
}

void WriteChunkRuntimeReport(UWorld& World,FChunkRuntimeState& State)
{
	if(State.bReported)return;State.bReported=true;
	World.GetTimerManager().ClearTimer(State.MovementTimer);World.GetTimerManager().ClearTimer(State.CaptureTimer);
	const auto* Terrain=World.GetSubsystem<UTerrainGridSubsystem>();const auto* Buildings=World.GetSubsystem<UBuildingManagerSubsystem>();
	const auto* Economy=World.GetSubsystem<UEconomyAndStateSubsystem>();const auto* Director=World.GetSubsystem<UAftershockDisasterDirectorSubsystem>();
	const auto* Life=World.GetSubsystem<UCityLifeSubsystem>();const auto* PC=World.GetFirstPlayerController();
	const auto* Camera=PC?Cast<AAftershockCameraPawn>(PC->GetPawn()):nullptr;const auto* Host=ChunkTerrainHost(World);
	TArray<TPair<FString,bool>> Checks;
	const auto Check=[&](const TCHAR* Name,bool bPassed){Checks.Emplace(Name,bPassed);if(!bPassed)UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_CHUNK_RUNTIME_FAIL %s"),Name);};
	Check(TEXT("startup_owners_and_canonical_snapshots_present"),State.bStarted&&Terrain&&Buildings&&Economy&&Director&&Life&&Camera&&!State.InitialCells.IsEmpty()&&!State.InitialBuildings.IsEmpty());
	const FIntPoint Focus=Terrain&&Camera?Terrain->WorldToGrid(Camera->GetLocalMapFocus()):FIntPoint(MAX_int32,MAX_int32);
	Check(TEXT("public_camera_orders_reached_negative_resident_terrain"),State.bPanFinished&&State.Steps>0&&State.Steps<=1200&&Terrain&&Terrain->IsValidCoord(Focus)&&Focus.Y<0);
	Check(TEXT("initial_generation_frame_and_globe_anchor_are_stable"),Terrain&&Terrain->GridWidth==State.InitialWidth&&Terrain->GridHeight==State.InitialHeight
		&&GetAftershockGlobeLayout(*Terrain).Center.Equals(State.Globe.Center,.001)&&FMath::IsNearlyEqual(GetAftershockGlobeLayout(*Terrain).RadiusCm,State.Globe.RadiusCm,.001f));
	bool Prefix=Terrain&&Terrain->Cells.Num()>=State.InitialCells.Num();int32 Unlocked=0;
	if(Prefix)for(int32 I=0;I<State.InitialCells.Num();++I)
	{
		FGridCell Expected=State.InitialCells[I];const FGridCell& Actual=Terrain->Cells[I];
		if(Expected.bIsPlayable!=Actual.bIsPlayable){++Unlocked;Prefix&=!Expected.bIsPlayable&&Actual.bIsPlayable&&!Expected.bIsOcean;Expected.bIsPlayable=Actual.bIsPlayable;}
		Prefix&=Terrain->CoordToIndex(Expected.Coords)==I&&FGridCell::StaticStruct()->CompareScriptStruct(&Expected,&Actual,0);
	}
	Check(TEXT("all_initial_cells_and_indices_preserved_except_explicit_mask_unlock"),Prefix&&Terrain&&Terrain->bInitialMaskUnlocked&&Unlocked>0);
	TArray<FBuildingInstance> NowBuildings;if(Buildings)Buildings->GetAllBuildings(NowBuildings);
	bool SameBuildings=Buildings&&NowBuildings.Num()==State.InitialBuildings.Num();
	for(const auto& Before:State.InitialBuildings)
	{
		FBuildingInstance Current;const FBuildingInstance* Fixture=State.ExplicitCollapseFixtures.Find(Before.InstanceID);
		SameBuildings&=Buildings&&Buildings->GetBuildingByID(Before.InstanceID,Current)&&FBuildingInstance::StaticStruct()->CompareScriptStruct(Fixture?Fixture:&Before,&Current,0);
	}
	Check(TEXT("all_original_building_ids_coords_and_state_preserved_except_logged_chaos_fixture"),SameBuildings);
	FAftershockEconomySaveState CurrentEconomy;if(Economy)Economy->CaptureSaveState(CurrentEconomy);
	Check(TEXT("camera_expansion_does_not_spend_money_advance_turn_or_grant_aid"),Economy&&FAftershockEconomySaveState::StaticStruct()->CompareScriptStruct(&State.Economy,&CurrentEconomy,0));
	Check(TEXT("camera_and_optional_visual_fixture_do_not_apply_casualties"),Life&&Life->GetState().TotalDeaths==State.Deaths);
	const FIntRect Window=Host?Host->BuiltVisibleCellBounds:FIntRect(0,0,0,0);
	Check(TEXT("installed_visible_window_is_bounded75x75_and_contains_camera_focus"),Host&&!Window.IsEmpty()&&Window.Width()<=75&&Window.Height()<=75&&Window.Contains(Focus)
		&&Terrain&&Window==GetAftershockVisibleCellBounds(*Terrain)&&Host->RenderedLandCellCount>0&&Host->RenderedLandCellCount<=75*75);
	if(Terrain&&Buildings)
	{
		const FIntRect Drawable=GetAftershockDrawableCellBounds(*Terrain);int32 ExpectedBuildings=0;
		for(const auto& B:NowBuildings)ExpectedBuildings+=Drawable.Contains(B.GridCoords)&&B.DamageState<EDamageState::Collapsed?1:0;
		Check(TEXT("static_building_draw_view_matches_actual_installed_ground"),Buildings->UsesDerivedBuildingPresentation()&&Buildings->GetBuildingPresentationBounds()==Drawable&&Buildings->GetPresentedBuildingCount()==ExpectedBuildings);
		bool FoundRoadActor=false;for(TActorIterator<AAftershockCityPresentationActor> It(&World);It;++It){FoundRoadActor=true;Check(TEXT("road_surfaces_match_installed_window_without_visible_missing_tiles"),It->GetRenderedCellBounds()==Drawable&&It->GetRenderedInitialRoadCount()==It->GetExpectedVisibleInitialRoadCount());break;}
		Check(TEXT("real_road_presentation_actor_present"),FoundRoadActor);
	}
	UStaticMeshComponent* Ground=Host?Host->TerrainSurface:nullptr;
	const bool RenderReady=Ground&&Ground->GetStaticMesh()&&Ground->GetMaterial(0)&&Ground->IsRegistered()&&Ground->IsRenderStateCreated()&&Ground->IsVisible()&&!Host->IsHidden();
	Check(TEXT("real_terrain_mesh_material_and_render_state_ready"),RenderReady);
	Check(TEXT("actual_terrain_physics_collider_exists"),Ground&&Ground->IsPhysicsStateCreated()&&CollisionEnabledHasPhysics(Ground->GetCollisionEnabled()));
	int32 CollisionChecked=0,CollisionPassed=0;
	if(Terrain&&Ground)
	{
		const FIntPoint Offsets[]={FIntPoint(0,0),FIntPoint(2,0),FIntPoint(-2,0),FIntPoint(0,2),FIntPoint(0,-2)};
		for(FIntPoint Offset:Offsets)
		{
			const FIntPoint Coord=Focus+Offset;if(!Window.Contains(Coord)||!Terrain->IsPlayableCoord(Coord))continue;++CollisionChecked;
			const FVector Center=Terrain->GridToWorld(Coord);FHitResult Hit;FCollisionQueryParams Query(SCENE_QUERY_STAT(AftershockChunkRuntimeGround),true);
			if(Ground->LineTraceComponent(Hit,Center+FVector(0,0,3000),Center-FVector(0,0,3000),Query)&&FMath::Abs(Hit.ImpactPoint.Z-Center.Z)<.5)++CollisionPassed;
		}
	}
	Check(TEXT("negative_area_canonical_cell_centres_have_real_collision"),CollisionChecked>=3&&CollisionPassed==CollisionChecked);
	const float Elapsed=World.GetTimeSeconds()-State.StartedAt;
	const FAftershockDirectorState CurrentDirector=Director?Director->GetState():FAftershockDirectorState();
	const float ExpectedRemaining=State.Director.RemainingSeconds-Elapsed;
	Check(TEXT("real_director_warning_clock_advances_without_reset_or_duplicate_event"),State.bWarning&&Director&&CurrentDirector.Phase==EAftershockDisasterPhase::Warning
		&&CurrentDirector.EventSequence==State.Director.EventSequence&&CurrentDirector.LastAppliedEventSequence==State.Director.LastAppliedEventSequence
		&&CurrentDirector.RemainingSeconds<State.Director.RemainingSeconds-.5f&&FMath::Abs(CurrentDirector.RemainingSeconds-ExpectedRemaining)<1.f);
	if(State.bChaosRequested)
	{
		const auto* Far=State.FarActor.Get();auto* Component=Far?Far->GetGeometryCollectionComponent():nullptr;
		Check(TEXT("visible_proxy_released_on_window_exit_before_four_second_budget"),State.bNearProxyStarted&&State.bNearReleasedBeforeFour);
		Check(TEXT("offscreen_authored_gc_has_no_physics_proxy_actor_or_component_tick"),State.bFarStaticStarted&&Far&&Component&&Far->Tags.Contains(TEXT("Aftershock.AuthoredOffscreenRubble"))
			&&Component->GetPhysicsProxy()==nullptr&&!Component->IsPhysicsStateCreated()&&!Component->IsSimulatingPhysics()&&!Far->IsActorTickEnabled()&&!Component->IsComponentTickEnabled());
		Check(TEXT("offscreen_native_fragment_poses_stable_after_four_seconds"),Component&&World.GetTimeSeconds()-State.FarSpawnAt>=4.f&&SameChunkPoses(State.FarPose,Component->GetCurrentTransforms()));
		const FIntRect Drawable=Terrain?GetAftershockDrawableCellBounds(*Terrain):FIntRect(0,0,0,0);
		const FIntPoint FarCoord=Terrain&&Far?Terrain->WorldToGrid(Far->GetActorLocation()):FIntPoint(MAX_int32,MAX_int32);
		Check(TEXT("offscreen_authored_rubble_hidden_outside_installed_drawable_ground"),Component&&Far&&Terrain&&!Drawable.Contains(FarCoord)&&!Component->IsVisible()&&Component->bHiddenInGame&&!Far->IsHidden());
		const auto* Near=State.NearActor.Get();const auto* NearComponent=Near?Near->GetGeometryCollectionComponent():nullptr;
		Check(TEXT("frozen_window_exit_rubble_visibility_matches_installed_drawable_ground"),Near&&NearComponent&&Terrain&&NearComponent->IsVisible()==Drawable.Contains(State.NearCoord)&&NearComponent->bHiddenInGame!=Drawable.Contains(State.NearCoord));
	}
	const FString Folder=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation"));IFileManager::Get().MakeDirectory(*Folder,true);
	const FString ScenePath=Folder/TEXT("studio-chunk-runtime.png");IFileManager::Get().Delete(*ScenePath,false,true);
	const bool SceneSaved=PC&&PC->PlayerCameraManager&&State.Capture&&State.Capture(World,PC->PlayerCameraManager->GetCameraCacheView(),ScenePath)&&FreshChunkPNG(ScenePath);
	Check(TEXT("fresh_actual_camera_scene_png1600x1000_saved"),SceneSaved);
	bool Passed=State.Failure.IsEmpty();for(const auto& Entry:Checks)Passed&=Entry.Value;
	FString Json=TEXT("{\n");
	const auto AddString=[&](const TCHAR* Name,const FString& Value){Json+=FString::Printf(TEXT("\"%s\":\"%s\",\n"),Name,*EscapeChunkJson(Value));};
	const auto AddInt=[&](const TCHAR* Name,int64 Value){Json+=FString::Printf(TEXT("\"%s\":%lld,\n"),Name,Value);};
	const auto AddFloat=[&](const TCHAR* Name,double Value){Json+=FString::Printf(TEXT("\"%s\":%.4f,\n"),Name,Value);};
	const auto AddBool=[&](const TCHAR* Name,bool Value){Json+=FString::Printf(TEXT("\"%s\":%s,\n"),Name,Value?TEXT("true"):TEXT("false"));};
	AddInt(TEXT("schema_version"),1);AddString(TEXT("created_utc"),FDateTime::UtcNow().ToIso8601());AddBool(TEXT("passed"),Passed);
	AddString(TEXT("scope"),TEXT("Opt-in real startup city, scripted public MoveCamera calls every15ms, negative canonical chunks, actual bounded terrain collision/render and PNG. No OS/human input, FPS profile, infinite streaming or scientific hazard claim. Optional95-damage visual fixtures are explicitly listed; no hazard/death formula executed."));
	AddString(TEXT("failure"),State.Failure);AddInt(TEXT("camera_order_calls"),State.Steps);AddInt(TEXT("stalled_camera_calls"),State.StalledSteps);
	AddInt(TEXT("initial_cells"),State.InitialCells.Num());AddInt(TEXT("resident_cells"),Terrain?Terrain->Cells.Num():0);AddInt(TEXT("resident_chunks"),Terrain?Terrain->GetResidentChunks().Num():0);
	AddInt(TEXT("initial_mask_unlocked_cells"),Unlocked);AddInt(TEXT("focus_x"),Focus.X);AddInt(TEXT("focus_y"),Focus.Y);
	AddInt(TEXT("window_min_x"),Window.Min.X);AddInt(TEXT("window_min_y"),Window.Min.Y);AddInt(TEXT("window_max_x"),Window.Max.X);AddInt(TEXT("window_max_y"),Window.Max.Y);
	AddInt(TEXT("rendered_land_cells"),Host?Host->RenderedLandCellCount:0);AddInt(TEXT("collision_samples"),CollisionChecked);AddInt(TEXT("collision_passed"),CollisionPassed);
	AddInt(TEXT("treasury_before"),State.Economy.Treasury);AddInt(TEXT("treasury_after"),CurrentEconomy.Treasury);
	AddFloat(TEXT("elapsed_world_seconds"),Elapsed);AddFloat(TEXT("warning_seconds_before"),State.Director.RemainingSeconds);AddFloat(TEXT("warning_seconds_after"),CurrentDirector.RemainingSeconds);
	AddBool(TEXT("optional_chaos_fixture_requested"),State.bChaosRequested);AddInt(TEXT("explicit95_damage_buildings"),State.ExplicitCollapseFixtures.Num());
	AddBool(TEXT("near_proxy_started"),State.bNearProxyStarted);AddBool(TEXT("near_released_before4s"),State.bNearReleasedBeforeFour);AddBool(TEXT("far_static_started"),State.bFarStaticStarted);
	AddString(TEXT("scene"),ScenePath);Json+=TEXT("\"checks\":[\n");
	for(int32 I=0;I<Checks.Num();++I)Json+=FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}%s\n"),*Checks[I].Key,Checks[I].Value?TEXT("true"):TEXT("false"),I+1<Checks.Num()?TEXT(","):TEXT(""));
	Json+=TEXT("]\n}\n");const FString Report=Folder/TEXT("chunk_runtime_audit.json");
	const bool Saved=FFileHelper::SaveStringToFile(Json,*Report,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	if(Passed&&Saved){UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHUNK_RUNTIME_RESULT passed=1 steps=%d focus=(%d,%d) residentCells=%d window=%dx%d collision=%d/%d sceneSaved=1 scriptedPublicAPI=1 humanInputClaim=0 report=%s"),State.Steps,Focus.X,Focus.Y,Terrain?Terrain->Cells.Num():0,Window.Width(),Window.Height(),CollisionPassed,CollisionChecked,*Report);}
	else{UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_CHUNK_RUNTIME_RESULT passed=0 steps=%d focus=(%d,%d) sceneSaved=%d reportSaved=%d failure=%s report=%s"),State.Steps,Focus.X,Focus.Y,SceneSaved,Saved,*State.Failure,*Report);}
	FTimerHandle Exit;World.GetTimerManager().SetTimer(Exit,FTimerDelegate::CreateLambda([](){FPlatformMisc::RequestExit(false);}),1.f,false);
}

void TryNearCollapse(UWorld& World,FChunkRuntimeState& State)
{
	if(!State.bChaosRequested||State.bNearAttempted)return;State.bNearAttempted=true;
	auto* Terrain=World.GetSubsystem<UTerrainGridSubsystem>();auto* Buildings=World.GetSubsystem<UBuildingManagerSubsystem>();auto* Simulation=World.GetSubsystem<UDisasterSimulationSubsystem>();
	const auto* Host=ChunkTerrainHost(World);if(!Terrain||!Buildings||!Simulation||!Host)return;
	for(const auto& Original:State.InitialBuildings)
	{
		if(!Original.BuildingData||Original.BuildingData->MeshFractured.IsNull()||Original.GridCoords.Y<28||Original.GridCoords.Y>37)continue;
		Buildings->ApplyDamageToBuilding(Original.InstanceID,95.f);FBuildingInstance Collapsed;Buildings->GetBuildingByID(Original.InstanceID,Collapsed);
		State.ExplicitCollapseFixtures.Add(Collapsed.InstanceID,Collapsed);Simulation->SpawnChaosDestructionForBuilding(Collapsed);
		AGeometryCollectionActor* Actor=RubbleAt(World,*Terrain,Collapsed.GridCoords);State.NearActor=Actor;State.NearCoord=Collapsed.GridCoords;
		State.NearSpawnAt=World.GetTimeSeconds();State.NearOriginalWindow=Host->BuiltVisibleCellBounds;
		State.bNearProxyStarted=Actor&&Actor->GetGeometryCollectionComponent()->GetPhysicsProxy()!=nullptr;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHUNK_RUNTIME_INDUCED_COLLAPSE stage=BeforeWindowExit building=%d coord=%s explicitDamage=95 activeProxy=%d hazardFormulaCalled=0 casualtyFormulaCalled=0"),Collapsed.InstanceID,*Collapsed.GridCoords.ToString(),State.bNearProxyStarted);return;
	}
}

void PrepareFinalChunkCapture(UWorld& World,const TSharedRef<FChunkRuntimeState>& State)
{
	CheckNearWindowExit(World,*State);
	if(!State->bChaosRequested){WriteChunkRuntimeReport(World,*State);return;}
	auto* Terrain=World.GetSubsystem<UTerrainGridSubsystem>();auto* Buildings=World.GetSubsystem<UBuildingManagerSubsystem>();auto* Simulation=World.GetSubsystem<UDisasterSimulationSubsystem>();const auto* Host=ChunkTerrainHost(World);
	if(Terrain&&Buildings&&Simulation&&Host)for(const auto& Original:State->InitialBuildings)
	{
		if(!Original.BuildingData||Original.BuildingData->MeshFractured.IsNull()||State->ExplicitCollapseFixtures.Contains(Original.InstanceID)||Host->BuiltVisibleCellBounds.Contains(Original.GridCoords))continue;
		Buildings->ApplyDamageToBuilding(Original.InstanceID,95.f);FBuildingInstance Collapsed;Buildings->GetBuildingByID(Original.InstanceID,Collapsed);
		State->ExplicitCollapseFixtures.Add(Collapsed.InstanceID,Collapsed);Simulation->SpawnChaosDestructionForBuilding(Collapsed);
		AGeometryCollectionActor* Actor=RubbleAt(World,*Terrain,Collapsed.GridCoords);State->FarActor=Actor;State->FarSpawnAt=World.GetTimeSeconds();
		auto* Component=Actor?Actor->GetGeometryCollectionComponent():nullptr;State->bFarStaticStarted=Component&&Component->GetPhysicsProxy()==nullptr;
		if(Component)State->FarPose=Component->GetCurrentTransforms();
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHUNK_RUNTIME_INDUCED_COLLAPSE stage=AlreadyOutsideWindow building=%d coord=%s explicitDamage=95 staticNoProxy=%d hazardFormulaCalled=0 casualtyFormulaCalled=0"),Collapsed.InstanceID,*Collapsed.GridCoords.ToString(),State->bFarStaticStarted);break;
	}
	TWeakObjectPtr<UWorld> Weak(&World);World.GetTimerManager().SetTimer(State->CaptureTimer,FTimerDelegate::CreateLambda([Weak,State](){if(UWorld* W=Weak.Get();ValidChunkWorld(W))WriteChunkRuntimeReport(*W,*State);}),4.5f,false);
}
}
#endif

void BeginAftershockChunkRuntimeAudit(UWorld& World,FAftershockChunkSceneCapture CaptureScene)
{
#if !UE_BUILD_SHIPPING
	if(!FParse::Param(FCommandLine::Get(),TEXT("AftershockChunkVisualAudit")))return;
	TWeakObjectPtr<UWorld> Weak(&World);const TSharedRef<FChunkRuntimeState> State=MakeShared<FChunkRuntimeState>();State->Capture=MoveTemp(CaptureScene);
	State->bChaosRequested=FParse::Param(FCommandLine::Get(),TEXT("AftershockChunkChaosAudit"));
	FTimerHandle Start;World.GetTimerManager().SetTimer(Start,FTimerDelegate::CreateLambda([Weak,State]()
	{
		UWorld* W=Weak.Get();if(!ValidChunkWorld(W))return;
		auto* Terrain=W->GetSubsystem<UTerrainGridSubsystem>();auto* Buildings=W->GetSubsystem<UBuildingManagerSubsystem>();auto* Economy=W->GetSubsystem<UEconomyAndStateSubsystem>();
		auto* Director=W->GetSubsystem<UAftershockDisasterDirectorSubsystem>();const auto* Scenario=W->GetSubsystem<UAftershockScenarioSubsystem>();const auto* Life=W->GetSubsystem<UCityLifeSubsystem>();
		auto* PC=W->GetFirstPlayerController();auto* Camera=PC?Cast<AAftershockCameraPawn>(PC->GetPawn()):nullptr;
		if(!Terrain||!Buildings||!Economy||!Director||!Scenario||!Life||!Camera||Terrain->Cells.IsEmpty())
		{State->Failure=TEXT("Actual startup city owners or camera missing");WriteChunkRuntimeReport(*W,*State);return;}
		int32 Width=0,Height=0;PC->GetViewportSize(Width,Height);if(Width>0&&Height>0)PC->SetMouseLocation(Width/2,Height/2);
		FText Error;State->bWarning=Director->RequestWarning(Scenario->GetDefinition().Hazard,120.f,Error);
		if(!State->bWarning)State->Failure=TEXT("Real120s warning request failed: ")+Error.ToString();
		State->InitialCells=Terrain->Cells;Buildings->GetAllBuildings(State->InitialBuildings);Economy->CaptureSaveState(State->Economy);
		State->InitialWidth=Terrain->GridWidth;State->InitialHeight=Terrain->GridHeight;State->InitialFocus=Camera->GetLocalMapFocus();
		State->Globe=GetAftershockGlobeLayout(*Terrain);State->Director=Director->GetState();State->Deaths=Life->GetState().TotalDeaths;
		State->StartedAt=W->GetTimeSeconds();State->bStarted=true;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHUNK_RUNTIME_START initialCells=%d buildings=%d treasury=%lld warningAccepted=%d warningSeconds=%.2f cameraInterval=.015 maxSteps=1200 scriptedPublicAPI=1 humanInputClaim=0 optionalChaos=%d"),State->InitialCells.Num(),State->InitialBuildings.Num(),State->Economy.Treasury,State->bWarning,State->Director.RemainingSeconds,State->bChaosRequested);
		W->GetTimerManager().SetTimer(State->MovementTimer,FTimerDelegate::CreateLambda([Weak,State]()
		{
			UWorld* Current=Weak.Get();if(!ValidChunkWorld(Current)||State->bReported)return;
			auto* T=Current->GetSubsystem<UTerrainGridSubsystem>();auto* Controller=Current->GetFirstPlayerController();auto* Pawn=Controller?Cast<AAftershockCameraPawn>(Controller->GetPawn()):nullptr;
			if(!T||!Pawn){State->Failure=TEXT("Camera/world lost during scripted pan");WriteChunkRuntimeReport(*Current,*State);return;}
			CheckNearWindowExit(*Current,*State);
			const FIntPoint Focus=T->WorldToGrid(Pawn->GetLocalMapFocus());
			if(Focus.Y>=0&&Focus.Y<=2)TryNearCollapse(*Current,*State);
			if(Focus.Y<=-3||State->Steps>=1200)
			{
				State->bPanFinished=Focus.Y<=-3;if(!State->bPanFinished)State->Failure=TEXT("Scripted MoveCamera did not reach negative Y within1200 calls; input may be blocked");
				Current->GetTimerManager().ClearTimer(State->MovementTimer);
				Current->GetTimerManager().SetTimer(State->CaptureTimer,FTimerDelegate::CreateLambda([Weak,State](){if(UWorld* C=Weak.Get();ValidChunkWorld(C))PrepareFinalChunkCapture(*C,State);}),2.5f,false);return;
			}
			const FVector Before=Pawn->GetLocalMapFocus();const float Yaw=Pawn->SpringArm?Pawn->SpringArm->GetRelativeRotation().Yaw:Pawn->GetActorRotation().Yaw;
			const FRotationMatrix Rotation(FRotator(0,Yaw,0));const FVector Direction(0,-1,0);
			const FVector2D Input(FVector::DotProduct(Direction,Rotation.GetUnitAxis(EAxis::Y)),FVector::DotProduct(Direction,Rotation.GetUnitAxis(EAxis::X)));
			Pawn->MoveCamera(Input);++State->Steps;State->StalledSteps+=Pawn->GetLocalMapFocus().Equals(Before,.001)?1:0;
		}),.015f,true);
	}),6.f,false);
	FTimerHandle Deadline;World.GetTimerManager().SetTimer(Deadline,FTimerDelegate::CreateLambda([Weak,State]()
	{if(UWorld* W=Weak.Get();ValidChunkWorld(W)&&!State->bReported){State->Failure=TEXT("Runtime fixture exceeded40s deadline");WriteChunkRuntimeReport(*W,*State);}}),40.f,false);
#endif
}
