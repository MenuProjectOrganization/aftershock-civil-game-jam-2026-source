#include "AftershockPresentationPerformanceAuditCommandlet.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/DisasterPresentationSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "AftershockTerrainSurface.h"
#include "AftershockIncrementalNormalRoutes.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

UAftershockPresentationPerformanceAuditCommandlet::UAftershockPresentationPerformanceAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}

int32 UAftershockPresentationPerformanceAuditCommandlet::Main(const FString& Params)
{
	if(!GEngine)return 1;
	UWorld* W=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("PresentationPerformanceFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	if(!W)return 1;GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	int32 Passed=0,Failed=0;TArray<FString> Rows;
	const auto Check=[&](const TCHAR* Name,bool Value)
	{Value?++Passed:++Failed;Rows.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"),Name,Value?TEXT("true"):TEXT("false")));UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PRESENTATION_PERFORMANCE_CHECK case=%s passed=%d"),Name,Value);};
    const auto CheckFreshWalkingSnapshot=[&](const TCHAR* Name,UCrowdRenderSubsystem* Crowd)
    {
        // Same canonical state: fresh-stack snapshot versus the original full
        // public-predicate poll. Equivalence only, not callback timing proof.
        Crowd->RebuildDestinationCaches();const auto FreshCosts=Crowd->RoutingCellCosts;
        const uint32 FreshTopology=Crowd->RoutingTopologySignature;
        Crowd->RefreshRoutingSafety();
        Check(Name,FreshCosts==Crowd->RoutingCellCosts&&FreshTopology==Crowd->RoutingTopologySignature);
    };
	auto* Terrain=W->GetSubsystem<UTerrainGridSubsystem>();auto* Buildings=W->GetSubsystem<UBuildingManagerSubsystem>();
	Terrain->GenerateProceduralTerrain(32,32,500.f,1337);
	for(auto Cell:Terrain->Cells){Cell.bIsPlayable=true;Cell.bIsRiverbed=false;Cell.bIsOcean=false;Cell.bHasRoad=false;Cell.bIsEmergencyAssemblyArea=false;Cell.bVolcanicVent=false;Cell.Elevation=0.f;Terrain->SetCell(Cell.Coords,Cell);}
	TArray<int32> IDs;auto* Data=Buildings->GetCatalogData(EBuildingType::House);
	for(int32 Y=2;Y<12;++Y)for(int32 X=2;X<12;++X){int32 ID=INDEX_NONE;if(Buildings->PlaceBuilding(FIntPoint(X,Y),Data,EFoundationQuality::Medium,1,ID))IDs.Add(ID);}
	Check(TEXT("one_hundred_actual_canonical_buildings_placed"),IDs.Num()==100);
	FText Error;Check(TEXT("derived_view_fixture_expands_canonical_storage"),Terrain->EnsureChunkGenerated(FIntPoint(-1,0),Error));
	Buildings->RefreshVisibleBuildingPresentation(true);Buildings->FlushPendingPresentationUpdates();
	Check(TEXT("fixture_uses_actual_derived_building_view"),Buildings->UsesDerivedBuildingPresentation());
	const auto Before=Buildings->GetPresentationWork();const double Start=FPlatformTime::Seconds();
	for(int32 I=0;I<IDs.Num();++I)Buildings->ApplyDamageToBuilding(IDs[I],I%2==0?30.f:60.f);
	const double DamageMs=(FPlatformTime::Seconds()-Start)*1000.;const auto Queued=Buildings->GetPresentationWork();
	Check(TEXT("hundred_damage_updates_do_not_rebuild_global_decoration_layers_inline"),Queued.DamagedRebuilds==Before.DamagedRebuilds&&Queued.DecorationFlushes==Before.DecorationFlushes);
	const double FlushStart=FPlatformTime::Seconds();Buildings->FlushPendingPresentationUpdates();const double FlushMs=(FPlatformTime::Seconds()-FlushStart)*1000.;
	const auto Flushed=Buildings->GetPresentationWork();
	Check(TEXT("one_explicit_barrier_coalesces_all_damage_into_one_decoration_rebuild"),Flushed.DamagedRebuilds==Before.DamagedRebuilds+1&&Flushed.DecorationFlushes==Before.DecorationFlushes+1);
	Check(TEXT("decoration_rebuild_visits_at_most_two_passes_over_actual_buildings"),Flushed.DamagedBuildingsVisited-Before.DamagedBuildingsVisited<=2*IDs.Num());
	bool CanonicalCPD=true;for(int32 I=0;I<IDs.Num();++I){FBuildingInstance B;CanonicalCPD&=Buildings->GetBuildingByID(IDs[I],B);auto* Batch=Buildings->GetOrCreateISM(Data);const int32 At=B.ISMInstanceIndex*Batch->NumCustomDataFloats;CanonicalCPD&=Batch->PerInstanceSMCustomData.IsValidIndex(At)&&FMath::IsNearlyEqual(Batch->PerInstanceSMCustomData[At],I%2==0?.3f:.6f);}
	Check(TEXT("canonical_damage_custom_data_changes_immediately_without_identity_replacement"),CanonicalCPD&&Buildings->GetActiveBuildingsCount()==100);
	Buildings->RefreshVisibleBuildingPresentation();
	Check(TEXT("damage_only_refresh_does_not_rebuild_derived_view_membership"),Buildings->GetPresentationWork().ViewRebuilds==Before.ViewRebuilds);
	FAftershockBuildingsSaveState SaveBefore,SaveAfter;Buildings->CaptureSaveState(SaveBefore);Buildings->FlushPendingPresentationUpdates();Buildings->RefreshVisibleBuildingPresentation();Buildings->CaptureSaveState(SaveAfter);
	Check(TEXT("unchanged_flush_and_view_refresh_preserve_complete_canonical_building_snapshot"),FAftershockBuildingsSaveState::StaticStruct()->CompareScriptStruct(&SaveBefore,&SaveAfter,0));
	const auto NoWorkBefore=Buildings->GetPresentationWork();Buildings->FlushPendingPresentationUpdates();
	Check(TEXT("already_flushed_barrier_does_no_work"),Buildings->GetPresentationWork().DecorationFlushes==NoWorkBefore.DecorationFlushes);
	FBuildingInstance StableBuilding;Buildings->GetBuildingByID(IDs[0],StableBuilding);const int32 StableIndex=StableBuilding.ISMInstanceIndex;
	const int32 SlotCount=Buildings->GetOrCreateISM(Data)->GetInstanceCount();int32 LatestID=IDs.Last();bool ReuseStable=true,UniqueIncreasing=true;
	for(int32 I=0;I<32;++I)
	{
		const int32 OldID=IDs.Last();FBuildingInstance Old;Buildings->GetBuildingByID(OldID,Old);
		const bool Removed=Buildings->RemoveBuilding(OldID);int32 NewID=INDEX_NONE;
		const bool Placed=Buildings->PlaceBuilding(Old.GridCoords,Data,EFoundationQuality::Medium,1,NewID);
		UniqueIncreasing&=Removed&&Placed&&NewID>LatestID;LatestID=NewID;IDs.Last()=NewID;
		Buildings->GetBuildingByID(IDs[0],StableBuilding);
		ReuseStable&=Buildings->GetOrCreateISM(Data)->GetInstanceCount()==SlotCount&&StableBuilding.ISMInstanceIndex==StableIndex;
	}
	Check(TEXT("repeated_demolition_rebuild_reuses_inactive_slots_without_growing_canonical_ism"),ReuseStable);
	Check(TEXT("slot_reuse_preserves_active_indices_and_monotonically_unique_building_ids"),UniqueIncreasing&&Buildings->GetActiveBuildingsCount()==100);
	Buildings->FlushPendingPresentationUpdates();
	Check(TEXT("far_chunk_for_ecology_work_bound_generated"),Terrain->EnsureChunkGenerated(FIntPoint(8,8),Error));
	// Headless fixtures select a deterministic resident window. Record that same
	// real window rather than pretending an unregistered native camera moved it.
	W->GetSubsystem<UCrowdRenderSubsystem>()->EnsureTerrainVisualBuilt();
	const FIntRect Visible=GetAftershockDrawableCellBounds(*Terrain);
	auto* Ecology=W->SpawnActor<AAftershockEcologyStateActor>();Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Earthquake,false);
	const uint64 Visits=Ecology->GetCanonicalCellsVisited(),Rebuilds=Ecology->GetRebuildCount();
	for(int32 I=0;I<20;++I)Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Earthquake,false);
	Check(TEXT("unchanged_ecology_revision_and_window_visit_zero_additional_cells"),Ecology->GetCanonicalCellsVisited()==Visits&&Ecology->GetRebuildCount()==Rebuilds);
	FIntPoint Far(MAX_int32,MAX_int32);for(const auto& Cell:Terrain->Cells)if(!Visible.Contains(Cell.Coords)){Far=Cell.Coords;break;}
	if(Far.X!=MAX_int32){auto Cell=Terrain->GetCell(Far);Cell.bBurning=!Cell.bBurning;Cell.ForestFuel01=.8f;Terrain->SetCell(Far,Cell);}
	Ecology->RefreshFromTerrain(*Terrain,EDisasterType::Earthquake,false);
	const uint64 OffscreenVisits=Ecology->GetCanonicalCellsVisited()-Visits;
	Check(TEXT("offwindow_ecology_change_does_not_rebuild_visible_layers"),Far.X!=MAX_int32&&Ecology->GetRebuildCount()==Rebuilds);
	Check(TEXT("offwindow_revision_check_is_bounded_by_visible_cells_not_all_residents"),OffscreenVisits<=static_cast<uint64>(Visible.Width())*Visible.Height()&&OffscreenVisits<static_cast<uint64>(Terrain->Cells.Num()));
	// Return to the real initial city for a tiny actual transient flood mesh.
	Terrain->GenerateProceduralTerrain(24,24,500.f,1337);
	for(auto Cell:Terrain->Cells){Cell.bIsPlayable=true;Cell.bIsRiverbed=false;Cell.bIsOcean=false;Cell.Elevation=0.f;Terrain->SetCell(Cell.Coords,Cell);}
	W->GetSubsystem<UCrowdRenderSubsystem>()->EnsureTerrainVisualBuilt();
	FAftershockDisasterVisualSnapshot Snapshot;Snapshot.Revision=901;Snapshot.Type=EDisasterType::Flood;Snapshot.DurationSeconds=24;Snapshot.EffectiveIntensity=100;
	for(int32 Y=8;Y<13;++Y)for(int32 X=8;X<13;++X)Snapshot.LocalSeverity.Add(FIntPoint(X,Y),100.f);
	FAftershockTerrainSaveState TerrainBefore,TerrainAfter;Terrain->CaptureSaveState(TerrainBefore);
	auto* Weather=W->SpawnActor<AAftershockDisasterPresentationActor>();bool CreatedGeometry=true,ReleasedEveryTime=true;
	for(int32 I=0;I<3;++I){Weather->BeginSnapshot(Snapshot,*Terrain);CreatedGeometry&=Weather->GetRetainedTransientMeshBytes()>0;Weather->ClearPresentation();ReleasedEveryTime&=Weather->GetRetainedTransientMeshBytes()==0;}
	Terrain->CaptureSaveState(TerrainAfter);
	Check(TEXT("actual_flood_renderer_builds_transient_geometry"),CreatedGeometry);
	Check(TEXT("completed_event_releases_component_held_transient_meshes_each_cycle"),ReleasedEveryTime);
	Check(TEXT("weather_build_clear_cycles_do_not_mutate_canonical_terrain"),FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&TerrainBefore,&TerrainAfter,0));
	// Isolated actual road/bridge input: enough eligible road cells to exercise
	// the fissure cap, with the bridge early in canonical Y/X traversal. The
	// renderer must read deck/ramp planes without changing canonical elevations.
	UWorld* QuakeWorld=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("QuakeFissurePresentationFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("quake_fissure_fixture_world_created"),QuakeWorld!=nullptr);
	if(QuakeWorld)
	{
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(QuakeWorld);
		auto* QT=QuakeWorld->GetSubsystem<UTerrainGridSubsystem>();auto* QR=QuakeWorld->GetSubsystem<URoadNetworkSubsystem>();
		QT->GenerateProceduralTerrain(24,24,500.f,1337);
		for(const auto& Existing:QT->Cells)
		{
			FGridCell Cell;Cell.Coords=Existing.Coords;Cell.bIsRiverbed=Cell.Coords.X==11||Cell.Coords.X==12;
			Cell.Elevation=Cell.bIsRiverbed?0.f:100.f+Cell.Coords.X*4.f+Cell.Coords.Y*3.f;
			Cell.FloodDepth=Cell.InitialFloodDepth=Cell.bIsRiverbed?100.f:0.f;Cell.bHasRoad=!Cell.bIsRiverbed;Cell.FaultLineProximity=.95f;
			QT->SetCell(Cell.Coords,Cell);
		}
		FAftershockRoadQuote BridgeQuote;const bool BridgeBuilt=QR->BuildRoad(FIntPoint(9,3),FIntPoint(14,3),BridgeQuote);
		Check(TEXT("quake_fixture_uses_real_bridge_transaction_with_canonical_road_surface_profile"),BridgeBuilt&&BridgeQuote.bIncludesBridge&&BridgeQuote.BridgeRiverCells==2&&QR->GetBridgeCount()==1);
        CheckFreshWalkingSnapshot(TEXT("fresh_walking_costs_equal_full_live_predicate_with_actual_bridge_deck"),QuakeWorld->GetSubsystem<UCrowdRenderSubsystem>());
		QuakeWorld->GetSubsystem<UCrowdRenderSubsystem>()->EnsureTerrainVisualBuilt();
		FAftershockDisasterVisualSnapshot Quake;Quake.Revision=902;Quake.Type=EDisasterType::Earthquake;Quake.EffectiveIntensity=80.f;Quake.DurationSeconds=10.f;Quake.SourceCoord=FIntPoint(11,3);
		for(const auto& Cell:QT->Cells)Quake.LocalSeverity.Add(Cell.Coords,80.f);
		FAftershockTerrainSaveState QuakeBefore,QuakeAfter;QT->CaptureSaveState(QuakeBefore);
		auto* QuakeWeather=QuakeWorld->SpawnActor<AAftershockDisasterPresentationActor>();QuakeWeather->BeginSnapshot(Quake,*QT);
		TArray<UInstancedStaticMeshComponent*> Components;QuakeWeather->GetComponents(Components);UInstancedStaticMeshComponent* Fissures=nullptr;
		for(auto* Component:Components)if(Component&&Component->GetFName()==FName(TEXT("QuakeFissureISM"))){Fissures=Component;break;}
		Check(TEXT("quake_fissures_use_actual_shared_ism_and_enforce_512_instance_cap"),Fissures&&Fissures->GetInstanceCount()>0&&Fissures->GetInstanceCount()==512);
		const FIntRect BuiltBounds=QuakeWeather->GetBuiltDrawableCellBounds();bool Inside=true,PlaneHeight=true,PlaneNormal=true;int32 BridgeCracks=0;
		if(Fissures)for(int32 I=0;I<Fissures->GetInstanceCount();++I)
		{
			FTransform Transform;if(!Fissures->GetInstanceTransform(I,Transform,true)){Inside=false;PlaneHeight=false;PlaneNormal=false;continue;}
			const FVector Position=Transform.GetLocation();const FIntPoint Coord=QT->WorldToGrid(Position);
			if(!BuiltBounds.Contains(Coord)||!QT->IsValidCoord(Coord)){Inside=false;PlaneHeight=false;PlaneNormal=false;continue;}
			const auto Cell=QT->GetCell(Coord);Inside&=Cell.bHasRoad;
			const FVector Center=QR->RoadGridToWorld(Coord);
			const auto NeighborHeight=[&](FIntPoint Neighbor)
			{
				FGridCell Other;if(!QT->TryGetCell(Neighbor,Other))return static_cast<float>(Center.Z);
				if(Cell.BridgeID!=INDEX_NONE&&Other.BridgeID!=Cell.BridgeID)return static_cast<float>(Center.Z);
				if(Cell.BridgeID==INDEX_NONE&&Other.bIsRiverbed)return static_cast<float>(Center.Z);
				return QR->GetRoadSurfaceElevation(Neighbor);
			};
			const float GX=(NeighborHeight(Coord+FIntPoint(1,0))-NeighborHeight(Coord-FIntPoint(1,0)))/(2.f*QT->CellSize);
			const float GY=(NeighborHeight(Coord+FIntPoint(0,1))-NeighborHeight(Coord-FIntPoint(0,1)))/(2.f*QT->CellSize);
			const double PlaneZ=Center.Z+(Position.X-Center.X)*GX+(Position.Y-Center.Y)*GY;
			const double Clearance=Position.Z-PlaneZ;PlaneHeight&=FMath::IsNearlyEqual(Clearance,2.,.01)||FMath::IsNearlyEqual(Clearance,2.5,.01);
			PlaneNormal&=FVector::DotProduct(Transform.GetRotation().GetUpVector(),FVector(-GX,-GY,1.f).GetSafeNormal())>.99999;
			if(Cell.BridgeID!=INDEX_NONE){++BridgeCracks;PlaneHeight&=FMath::IsNearlyEqual(Center.Z,static_cast<double>(Cell.RoadSurfaceElevationCm),.01);}
		}
		Check(TEXT("all_actual_quake_fissure_transforms_stay_inside_installed_drawable_ground"),Fissures&&Inside&&BuiltBounds.Area()>0);
		Check(TEXT("actual_bridge_and_road_fissures_use_canonical_surface_plane_plus_clearance"),BridgeBuilt&&BridgeCracks>0&&PlaneHeight);
		Check(TEXT("actual_quake_fissure_rotations_follow_canonical_road_and_bridge_normals"),Fissures&&PlaneNormal);
		QuakeWeather->ClearPresentation();QT->CaptureSaveState(QuakeAfter);
		Check(TEXT("quake_clear_releases_every_fissure_instance"),Fissures&&Fissures->GetInstanceCount()==0);
		Check(TEXT("quake_visual_build_and_clear_preserve_complete_canonical_terrain_and_bridge_state"),FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&QuakeBefore,&QuakeAfter,0));
		QuakeWorld->DestroyWorld(false);GEngine->DestroyWorldContext(QuakeWorld);QuakeWorld->RemoveFromRoot();
	}
	uint64 SyntheticCachePeakBytes=0;int32 SyntheticCacheSurvivors=0;
	double LiveConstructionMs=0.;uint64 LiveServiceVisits=0,LiveServiceQueries=0,LiveServiceHits=0;
	// Real routing and deliberately oversized memory fixtures are separate. The
	// synthetic fields never become terrain cells or a resident's live route.
	UWorld* RouteWorld=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("NormalRouteCacheFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("real_route_cache_world_created"),RouteWorld!=nullptr);
	if(RouteWorld)
	{
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(RouteWorld);
		auto* RouteTerrain=RouteWorld->GetSubsystem<UTerrainGridSubsystem>();auto* RouteCrowd=RouteWorld->GetSubsystem<UCrowdRenderSubsystem>();
		RouteTerrain->GenerateProceduralTerrain(32,32,500.f,1337);
		for(const auto Existing:RouteTerrain->Cells){FGridCell Empty;Empty.Coords=Existing.Coords;RouteTerrain->SetCell(Empty.Coords,Empty);}
		RouteCrowd->RebuildDestinationCaches();RouteCrowd->ResetNormalRouteCache();
		FAftershockTerrainSaveState RouteTerrainBefore,RouteTerrainAfter;RouteTerrain->CaptureSaveState(RouteTerrainBefore);
		FCitizenAgent Walker;Walker.RandomSeed=7153;Walker.CurrentLocation=RouteTerrain->GridToWorld(FIntPoint(2,2));Walker.MoveSpeed=150.f;
		const FVector Goal=RouteTerrain->GridToWorld(FIntPoint(29,29));const int32 GoalIndex=RouteTerrain->CoordToIndex(FIntPoint(29,29));
		const uint64 BuildsBefore=RouteCrowd->GetNormalRouteTreeBuildCount();RouteCrowd->BuildSafeRoute(Walker,Goal);
		const TArray<FIntPoint> RealRoute=Walker.RouteGridCoords;
		bool ActualRoute=Walker.bRouteReachesDestination&&!RealRoute.IsEmpty()&&RealRoute.Last()==FIntPoint(29,29);
		FIntPoint Previous(2,2);for(const FIntPoint Step:RealRoute){ActualRoute&=RouteWorld->GetSubsystem<URoadNetworkSubsystem>()->CanTraverseWalkingEdge(Previous,Step);Previous=Step;}
		Check(TEXT("real_empty_32_grid_builds_one_reachable_canonical_route_field"),ActualRoute&&RouteTerrain->Cells.Num()==1024&&RouteCrowd->GetNormalRouteCacheEntryCount()==1&&RouteCrowd->GetNormalRouteTreeBuildCount()==BuildsBefore+1);
		const auto* Cached=RouteCrowd->NormalRouteTrees.Find(GoalIndex);
		const int32* CostStorage=Cached?Cached->Cost.GetData():nullptr;const int32* NextStorage=Cached?Cached->Next.GetData():nullptr;
		const TArray<int32> OriginalCosts=Cached?Cached->Cost:TArray<int32>();const TArray<int32> OriginalNext=Cached?Cached->Next:TArray<int32>();
		const uint64 HitSerial=RouteCrowd->NormalRouteCacheSerial;
		RouteCrowd->RebuildDestinationCaches();Cached=RouteCrowd->NormalRouteTrees.Find(GoalIndex);
		Check(TEXT("unchanged_destination_refresh_preserves_actual_field_storage_and_values"),Cached&&CostStorage&&NextStorage&&Cached->Cost.GetData()==CostStorage&&Cached->Next.GetData()==NextStorage&&Cached->Cost==OriginalCosts&&Cached->Next==OriginalNext&&RouteCrowd->GetNormalRouteTreeBuildCount()==BuildsBefore+1);
		RouteCrowd->BuildSafeRoute(Walker,Goal);
		Check(TEXT("unchanged_real_route_is_a_cache_hit_without_field_rebuild"),Walker.bRouteReachesDestination&&Walker.RouteGridCoords==RealRoute&&RouteCrowd->GetNormalRouteTreeBuildCount()==BuildsBefore+1&&RouteCrowd->NormalRouteTreeUseSerial.FindRef(GoalIndex)>HitSerial);

        // Pure derived-search fixture: same real32x32 terrain and same captured
        // public-cell weights as the eager golden route above. It deliberately
        // bypasses runtime-mode gating to test partial-field correctness under
        // deterministic17-pop slices, with no actor/citizen/economy mutation.
        using FIncremental=FAftershockIncrementalNormalRoutes;
        using EIncrementalResult=FIncremental::EResult;
        constexpr uint64 FixtureByteLimit=4ull*1024ull*1024ull;
        FIncremental Incremental(FixtureByteLimit);
        const TArray<int32> CapturedWeights=RouteCrowd->RoutingCellCosts;
        const FIntPoint NativeGoal(29,29);
        const int32 NearIndex=RouteTerrain->CoordToIndex(FIntPoint(28,29));
        const int32 FarIndex=RouteTerrain->CoordToIndex(FIntPoint(2,2));
        TArray<int32> NearSource{NearIndex},FarSource{FarIndex};
        const FAftershockCitizenRouteTree* PartialTree=nullptr;
        auto NearResult=Incremental.Request(101,NativeGoal,NearSource,Goal,false,*RouteTerrain,CapturedWeights,PartialTree);
        Check(TEXT("incremental_request_queues_without_eager_field_or_false_ready"),NearResult==EIncrementalResult::Pending&&PartialTree==nullptr&&Incremental.GetJobCount()==0&&Incremental.HasRequest(101));
        bool SlicePopBound=true,SliceByteBound=true;
        int32 NearSlices=0;
        while(NearResult==EIncrementalResult::Pending&&NearSlices++<128)
        {
            Incremental.Advance(*RouteTerrain,CapturedWeights,1.,17);
            SlicePopBound&=Incremental.GetLastPops()<=17;
            SliceByteBound&=Incremental.GetAllocatedBytes()<=FixtureByteLimit;
            Incremental.BeginDeliverySlice(1.,16);
            NearResult=Incremental.Request(101,NativeGoal,NearSource,Goal,false,*RouteTerrain,CapturedWeights,PartialTree);
            Incremental.EndDeliverySlice();
        }
        Check(TEXT("incremental_near_source_ready_only_after_finalization"),NearResult==EIncrementalResult::Ready&&PartialTree&&PartialTree->Cost[NearIndex]==OriginalCosts[NearIndex]&&!Incremental.HasRequest(101));
        auto FarResult=Incremental.Request(102,NativeGoal,FarSource,Goal,false,*RouteTerrain,CapturedWeights,PartialTree);
        Check(TEXT("incremental_existing_partial_goal_is_not_ready_for_unsettled_far_source"),FarResult==EIncrementalResult::Pending&&PartialTree==nullptr&&Incremental.HasRequest(102)&&Incremental.GetJobCount()==1);
        int32 FarSlices=0;
        while(FarResult==EIncrementalResult::Pending&&FarSlices++<128)
        {
            Incremental.Advance(*RouteTerrain,CapturedWeights,1.,17);
            SlicePopBound&=Incremental.GetLastPops()<=17;SliceByteBound&=Incremental.GetAllocatedBytes()<=FixtureByteLimit;
            Incremental.BeginDeliverySlice(1.,16);
            FarResult=Incremental.Request(102,NativeGoal,FarSource,Goal,false,*RouteTerrain,CapturedWeights,PartialTree);
            Incremental.EndDeliverySlice();
        }
        FCitizenAgent IncrementalWalker;IncrementalWalker.RandomSeed=7153;IncrementalWalker.CurrentLocation=RouteTerrain->GridToWorld(FIntPoint(2,2));IncrementalWalker.MoveSpeed=150.f;
        const bool Applied=FarResult==EIncrementalResult::Ready&&PartialTree&&RouteCrowd->ApplyRouteTree(IncrementalWalker,*PartialTree);
        Check(TEXT("incremental_finalized_actual_route_matches_eager_golden_path_and_cost"),Applied&&IncrementalWalker.bRouteReachesDestination&&IncrementalWalker.RouteGridCoords==RealRoute&&PartialTree->Cost[FarIndex]==OriginalCosts[FarIndex]);
        Check(TEXT("incremental_every_seventeen_pop_slice_obeys_node_and_retained_byte_bounds"),SlicePopBound&&SliceByteBound&&FarSlices>1&&Incremental.GetStartedJobs()==1);
        // A hot shared field must not fan out all completed routes in one timer.
        Incremental.BeginDeliverySlice(1.,1);
        const auto FirstDelivery=Incremental.Request(103,NativeGoal,FarSource,Goal,false,*RouteTerrain,CapturedWeights,PartialTree);
        const auto CappedDelivery=Incremental.Request(104,NativeGoal,FarSource,Goal,false,*RouteTerrain,CapturedWeights,PartialTree);
        Incremental.EndDeliverySlice();
        Incremental.BeginDeliverySlice(1.,16);Incremental.ConsumeDeliveryCpu(1.01);
        const auto CpuCappedDelivery=Incremental.Request(104,NativeGoal,FarSource,Goal,false,*RouteTerrain,CapturedWeights,PartialTree);Incremental.EndDeliverySlice();
        Check(TEXT("incremental_hot_path_delivery_cap_retains_second_real_intent"),FirstDelivery==EIncrementalResult::Ready&&CappedDelivery==EIncrementalResult::Pending&&CpuCappedDelivery==EIncrementalResult::Pending&&PartialTree==nullptr&&Incremental.HasRequest(104));
        Incremental.BeginDeliverySlice(1.,1);
        const auto ResumedDelivery=Incremental.Request(104,NativeGoal,FarSource,Goal,false,*RouteTerrain,CapturedWeights,PartialTree);Incremental.EndDeliverySlice();
        Check(TEXT("incremental_capped_hot_delivery_resumes_without_new_field_or_teleport"),ResumedDelivery==EIncrementalResult::Ready&&PartialTree&&Incremental.GetStartedJobs()==1&&Incremental.GetCompletedRequests()==3&&!Incremental.HasRequest(104)&&IncrementalWalker.CurrentLocation.Equals(RouteTerrain->GridToWorld(FIntPoint(2,2)),.01f));
        // Roster reconciliation retargets one stable intent instead of retaining
        // an obsolete workplace/pickup, and removes intents for departed IDs.
        Incremental.Request(105,FIntPoint(0,0),FarSource,RouteTerrain->GridToWorld(FIntPoint(0,0)),false,*RouteTerrain,CapturedWeights,PartialTree);
        Incremental.InvalidateFields();
        Check(TEXT("incremental_graph_invalidation_drops_fields_but_retains_pending_stable_id"),Incremental.GetJobCount()==0&&Incremental.HasRequest(105));
        const FVector Retarget=RouteTerrain->GridToWorld(FIntPoint(1,1));
        Incremental.Request(105,FIntPoint(1,1),FarSource,Retarget,false,*RouteTerrain,CapturedWeights,PartialTree);
        FVector RetainedDestination;bool Keep=false;const bool Retained=Incremental.FindRequest(105,RetainedDestination,Keep);
        Check(TEXT("incremental_same_resident_retarget_replaces_goal_without_duplicate_intent"),Retained&&RetainedDestination.Equals(Retarget,.01f)&&Incremental.GetRequestCount()==1);
        TSet<uint64> NoLivingRequests;Incremental.Prune(NoLivingRequests);
        Check(TEXT("incremental_membership_prune_cancels_dead_or_unrendered_intents"),Incremental.GetRequestCount()==0&&!Incremental.HasRequest(105));
        // Test heterogeneous captured weights against the existing eager
        // algorithm. These are fixture-derived costs, not terrain/road writes.
        TArray<int32> Weighted=CapturedWeights;
        for(int32 X=0;X<32;++X)Weighted[RouteTerrain->CoordToIndex(FIntPoint(X,15))]=10;
        RouteCrowd->RoutingCellCosts=Weighted;RouteCrowd->ResetNormalRouteCache();
        FCitizenAgent WeightedGolden;WeightedGolden.RandomSeed=7153;WeightedGolden.CurrentLocation=RouteTerrain->GridToWorld(FIntPoint(2,2));WeightedGolden.MoveSpeed=150.f;RouteCrowd->BuildSafeRoute(WeightedGolden,Goal);
        const auto* WeightedGoldenTree=RouteCrowd->NormalRouteTrees.Find(GoalIndex);
        const int32 WeightedGoldenCost=WeightedGoldenTree?WeightedGoldenTree->Cost[FarIndex]:MAX_int32;
        FIncremental WeightedSearch(FixtureByteLimit);auto WeightedResult=WeightedSearch.Request(201,NativeGoal,FarSource,Goal,false,*RouteTerrain,Weighted,PartialTree);
        int32 WeightedSlices=0;
        while(WeightedResult==EIncrementalResult::Pending&&WeightedSlices++<128)
        {WeightedSearch.Advance(*RouteTerrain,Weighted,1.,17);SlicePopBound&=WeightedSearch.GetLastPops()<=17;WeightedSearch.BeginDeliverySlice(1.,1);WeightedResult=WeightedSearch.Request(201,NativeGoal,FarSource,Goal,false,*RouteTerrain,Weighted,PartialTree);WeightedSearch.EndDeliverySlice();}
        FCitizenAgent WeightedWalker;WeightedWalker.RandomSeed=7153;WeightedWalker.CurrentLocation=RouteTerrain->GridToWorld(FIntPoint(2,2));WeightedWalker.MoveSpeed=150.f;
        const bool WeightedApplied=WeightedResult==EIncrementalResult::Ready&&PartialTree&&RouteCrowd->ApplyRouteTree(WeightedWalker,*PartialTree);
        Check(TEXT("incremental_weighted_route_and_optimum_match_eager_golden_algorithm"),WeightedApplied&&WeightedWalker.RouteGridCoords==WeightedGolden.RouteGridCoords&&PartialTree->Cost[FarIndex]==WeightedGoldenCost&&WeightedGoldenCost<OriginalCosts[FarIndex]);
        // Closed column is only a captured-mask fixture. No source data or
        // hazard formula is changed to manufacture this unreachable case.
        TArray<int32> Closed=CapturedWeights;for(int32 Y=0;Y<32;++Y)Closed[RouteTerrain->CoordToIndex(FIntPoint(16,Y))]=MAX_int32;
        RouteCrowd->RoutingCellCosts=Closed;RouteCrowd->ResetNormalRouteCache();FCitizenAgent ClosedGolden;ClosedGolden.RandomSeed=7153;ClosedGolden.CurrentLocation=RouteTerrain->GridToWorld(FIntPoint(2,2));ClosedGolden.MoveSpeed=150.f;RouteCrowd->BuildSafeRoute(ClosedGolden,Goal);
        FIncremental ClosedSearch(FixtureByteLimit);auto ClosedResult=ClosedSearch.Request(301,NativeGoal,FarSource,Goal,false,*RouteTerrain,Closed,PartialTree);int32 ClosedSlices=0;
        while(ClosedResult==EIncrementalResult::Pending&&ClosedSlices++<128)
        {ClosedSearch.Advance(*RouteTerrain,Closed,1.,17);SlicePopBound&=ClosedSearch.GetLastPops()<=17;ClosedSearch.BeginDeliverySlice(1.,1);ClosedResult=ClosedSearch.Request(301,NativeGoal,FarSource,Goal,false,*RouteTerrain,Closed,PartialTree);ClosedSearch.EndDeliverySlice();}
        Check(TEXT("incremental_unreachable_requires_frontier_exhaustion_and_matches_eager_closed_mask"),ClosedResult==EIncrementalResult::Unreachable&&PartialTree==nullptr&&ClosedSlices>1&&!ClosedGolden.bRouteReachesDestination&&!ClosedSearch.HasRequest(301));
        Check(TEXT("incremental_weighted_and_closed_cases_keep_same_per_slice_pop_limit"),SlicePopBound&&WeightedSearch.GetAllocatedBytes()<=FixtureByteLimit&&ClosedSearch.GetAllocatedBytes()<=FixtureByteLimit);
        FIncremental PressureSearch(FixtureByteLimit);
        bool PressurePending=true,PressureBound=true;
        for(int32 I=0;I<160;++I)
        {const FIntPoint G(I%32,I/32);PressurePending&=PressureSearch.Request(1000+I,G,FarSource,RouteTerrain->GridToWorld(G),false,*RouteTerrain,CapturedWeights,PartialTree)==EIncrementalResult::Pending;}
        for(int32 I=0;I<20;++I)
        {PressureSearch.Advance(*RouteTerrain,CapturedWeights,1.,512);PressureBound&=PressureSearch.GetLastPops()<=512&&PressureSearch.GetJobCount()<=128&&PressureSearch.GetAllocatedBytes()<=FixtureByteLimit;}
        Check(TEXT("incremental_160_live_intents_respect_128_job_and_byte_caps_without_false_unreachable"),PressurePending&&PressureBound&&PressureSearch.GetJobCount()==128&&PressureSearch.GetRequestCount()==160);
        PressureSearch.Prune(NoLivingRequests);const uint64 PressureStarted=PressureSearch.GetStartedJobs();
        const FIntPoint LastGoal(31,31);PressureSearch.Request(2000,LastGoal,FarSource,RouteTerrain->GridToWorld(LastGoal),false,*RouteTerrain,CapturedWeights,PartialTree);PressureSearch.Advance(*RouteTerrain,CapturedWeights,1.,17);
        Check(TEXT("incremental_released_demand_allows_lru_eviction_and_new_goal_progress"),PressureSearch.GetStartedJobs()==PressureStarted+1&&PressureSearch.GetJobCount()==128&&PressureSearch.GetRequestCount()==1&&PressureSearch.GetAllocatedBytes()<=FixtureByteLimit);
        RouteCrowd->RoutingCellCosts=CapturedWeights;RouteCrowd->ResetNormalRouteCache();RouteCrowd->BuildSafeRoute(Walker,Goal);
        RouteTerrain->CaptureSaveState(RouteTerrainAfter);
        Check(TEXT("incremental_search_fixtures_preserve_complete_canonical_grid_and_actual_agent_membership"),FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&RouteTerrainBefore,&RouteTerrainAfter,0)&&RouteCrowd->Agents.IsEmpty()&&Walker.RouteGridCoords==RealRoute);
        UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_INCREMENTAL_ROUTE_NATIVE_FIXTURE cells=%d nearSlices=%d farSlices=%d weightedSlices=%d unreachableSlices=%d maxPopsPerSlice=17 weightedOptimalCost=%d eagerOptimalCost=%d noRuntimeLatencyClaim=1"),RouteTerrain->Cells.Num(),NearSlices,FarSlices,WeightedSlices,ClosedSlices,WeightedGoldenCost,OriginalCosts.IsValidIndex(FarIndex)?OriginalCosts[FarIndex]:MAX_int32);

        const uint64 BuildsBeforeRoadEvent=RouteCrowd->GetNormalRouteTreeBuildCount();
		RouteWorld->GetSubsystem<URoadNetworkSubsystem>()->NotifySaveRestored();
		Check(TEXT("actual_road_network_event_invalidates_unchanged_cost_route_fields"),RouteCrowd->NormalRouteTrees.IsEmpty()&&RouteCrowd->NormalRouteTreeUseSerial.IsEmpty());
		RouteCrowd->BuildSafeRoute(Walker,Goal);
		Check(TEXT("road_event_rebuilds_identical_real_route_without_changing_canonical_grid"),Walker.bRouteReachesDestination&&Walker.RouteGridCoords==RealRoute&&RouteCrowd->GetNormalRouteTreeBuildCount()==BuildsBeforeRoadEvent+1);
		RouteCrowd->ResetNormalRouteCache();const uint64 RealBuildCount=RouteCrowd->GetNormalRouteTreeBuildCount();
		constexpr int32 SyntheticBase=10000,SyntheticFields=80,SyntheticCellsPerField=65536;
		bool ByteBound=true,CountBound=true,SerialMembership=true;
		const auto AddSynthetic=[&](int32 Key,int32 CellCount)
		{
			FAftershockCitizenRouteTree Field;Field.Cost.Init(30,CellCount);Field.Next.Init(INDEX_NONE,CellCount);
			RouteCrowd->TrimNormalRouteCache(Field.Cost.GetAllocatedSize()+Field.Next.GetAllocatedSize());
			RouteCrowd->NormalRouteTrees.Add(Key,MoveTemp(Field));RouteCrowd->NormalRouteTreeUseSerial.Add(Key,++RouteCrowd->NormalRouteCacheSerial);
			const uint64 Bytes=RouteCrowd->GetNormalRouteCacheAllocatedBytes();SyntheticCachePeakBytes=FMath::Max(SyntheticCachePeakBytes,Bytes);
			ByteBound&=Bytes<=UCrowdRenderSubsystem::MaximumNormalRouteCacheBytes;CountBound&=RouteCrowd->NormalRouteTrees.Num()<=UCrowdRenderSubsystem::MaximumNormalRouteTrees;
			SerialMembership&=RouteCrowd->NormalRouteTrees.Num()==RouteCrowd->NormalRouteTreeUseSerial.Num();
			for(const auto& Pair:RouteCrowd->NormalRouteTrees)SerialMembership&=RouteCrowd->NormalRouteTreeUseSerial.Contains(Pair.Key);
		};
		for(int32 I=0;I<SyntheticFields;++I)AddSynthetic(SyntheticBase+I,SyntheticCellsPerField);
		SyntheticCacheSurvivors=RouteCrowd->GetNormalRouteCacheEntryCount();const int32 Evicted=SyntheticFields-SyntheticCacheSurvivors;
		bool OldestFirst=Evicted>0&&SyntheticCacheSurvivors>1;
		for(int32 I=0;I<SyntheticFields;++I)OldestFirst&=RouteCrowd->NormalRouteTrees.Contains(SyntheticBase+I)==(I>=Evicted);
		Check(TEXT("synthetic_80_full_65536_cell_fields_remain_within_32_mib_after_every_insert"),ByteBound&&Evicted>0);
		Check(TEXT("synthetic_route_fields_preserve_256_entry_bound_and_serial_membership"),CountBound&&SerialMembership);
		Check(TEXT("synthetic_equal_sized_fields_evict_deterministic_oldest_prefix"),OldestFirst);
		if(OldestFirst)
		{
			const int32 Touched=SyntheticBase+Evicted;RouteCrowd->NormalRouteTreeUseSerial.FindChecked(Touched)=++RouteCrowd->NormalRouteCacheSerial;
			AddSynthetic(SyntheticBase+SyntheticFields,SyntheticCellsPerField);
			Check(TEXT("synthetic_cache_hit_retains_touched_field_and_evicts_next_oldest"),RouteCrowd->NormalRouteTrees.Contains(Touched)&&!RouteCrowd->NormalRouteTrees.Contains(Touched+1)&&RouteCrowd->NormalRouteTrees.Contains(SyntheticBase+SyntheticFields)&&ByteBound&&CountBound);
		}
		else Check(TEXT("synthetic_cache_hit_retains_touched_field_and_evicts_next_oldest"),false);
		RouteCrowd->ResetNormalRouteCache();
		for(int32 I=0;I<=UCrowdRenderSubsystem::MaximumNormalRouteTrees;++I)AddSynthetic(SyntheticBase+I,1);
		Check(TEXT("synthetic_small_fields_exercise_exact_256_entry_lru_limit"),RouteCrowd->NormalRouteTrees.Num()==UCrowdRenderSubsystem::MaximumNormalRouteTrees&&!RouteCrowd->NormalRouteTrees.Contains(SyntheticBase)&&RouteCrowd->NormalRouteTrees.Contains(SyntheticBase+UCrowdRenderSubsystem::MaximumNormalRouteTrees)&&ByteBound&&CountBound&&SerialMembership);
		RouteTerrain->CaptureSaveState(RouteTerrainAfter);
		Check(TEXT("synthetic_budget_fixture_never_changes_real_route_grid_population_or_build_counter"),FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&RouteTerrainBefore,&RouteTerrainAfter,0)&&Walker.RouteGridCoords==RealRoute&&RouteCrowd->Agents.IsEmpty()&&RouteCrowd->GetNormalRouteTreeBuildCount()==RealBuildCount);
		RouteCrowd->ResetNormalRouteCache();
		Check(TEXT("route_fixture_releases_all_field_arrays_and_use_serial_entries"),RouteCrowd->NormalRouteTrees.IsEmpty()&&RouteCrowd->NormalRouteTreeUseSerial.IsEmpty());
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_ROUTE_CACHE_BUDGET realGridCells=%d realRouteSteps=%d syntheticFields=%d syntheticCellsPerField=%d syntheticPeakAllocatedBytes=%llu syntheticLargeSurvivors=%d byteLimit=%llu entryLimit=%d syntheticNotGameplay=1"),RouteTerrain->Cells.Num(),RealRoute.Num(),SyntheticFields,SyntheticCellsPerField,SyntheticCachePeakBytes,SyntheticCacheSurvivors,UCrowdRenderSubsystem::MaximumNormalRouteCacheBytes,UCrowdRenderSubsystem::MaximumNormalRouteTrees);
		RouteWorld->DestroyWorld(false);GEngine->DestroyWorldContext(RouteWorld);RouteWorld->RemoveFromRoot();
	}
	// Separate from MutationSoak: initialized canonical CityLife and the complete
	// synchronous PlaceBuilding -> CityLife -> Crowd chain. Private definitions
	// remove power/staffing prerequisites; no synthetic render agents are used.
	UWorld* LifeWorld=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("LiveConstructionFanoutFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("live_construction_fanout_world_created"),LifeWorld!=nullptr);
	if(LifeWorld)
	{
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(LifeWorld);
		auto* LT=LifeWorld->GetSubsystem<UTerrainGridSubsystem>();auto* LB=LifeWorld->GetSubsystem<UBuildingManagerSubsystem>();
		auto* LL=LifeWorld->GetSubsystem<UCityLifeSubsystem>();auto* LC=LifeWorld->GetSubsystem<UCrowdRenderSubsystem>();
		LT->GenerateProceduralTerrain(64,64,1000.f,1337);
		for(const auto Existing:LT->Cells){FGridCell Empty;Empty.Coords=Existing.Coords;Empty.bHasRoad=Empty.Coords.Y%2==0||Empty.Coords.X==0;LT->SetCell(Empty.Coords,Empty);}
		TMap<EBuildingType,UBuildingData*> Definitions;
		for(const EBuildingType Type:{EBuildingType::House,EBuildingType::GreenSpace,EBuildingType::Restaurant,EBuildingType::Bakery,EBuildingType::Hospital})
		{
			auto* Definition=DuplicateObject<UBuildingData>(LB->GetCatalogData(Type),LifeWorld);
			if(Definition){Definition->Residents=Type==EBuildingType::House?6:0;Definition->Workers=0;Definition->PowerDemandUnitsPerTurn=0;Definitions.Add(Type,Definition);}
		}
		int32 Placed=0,TieHome=INDEX_NONE,TieRestaurant=INDEX_NONE;
		for(int32 Y=1;Y<64&&Placed<1000;Y+=2)for(int32 X=2;X<64&&Placed<1000;++X)
		{
			EBuildingType Type=EBuildingType::House;
			if(Y==1){if(X==4||X==6)Type=EBuildingType::GreenSpace;else if(X==8||X==12)Type=EBuildingType::Restaurant;else if(X==14)Type=EBuildingType::Hospital;else if(X==16)Type=EBuildingType::Bakery;}
			int32 ID=INDEX_NONE;if(LB->PlaceBuilding(FIntPoint(X,Y),Definitions.FindRef(Type),EFoundationQuality::Medium,1,ID)){++Placed;if(Y==1&&X==10)TieHome=ID;if(Y==1&&X==8)TieRestaurant=ID;}
		}
		LifeWorld->GetSubsystem<URoadNetworkSubsystem>()->NotifySaveRestored();LL->InitializeForCity();
		Check(TEXT("live_construction_fixture_has_1000_canonical_buildings_and_1500_real_resident_agents"),Placed==1000&&LL->IsCityInitialized()&&LL->GetState().LivingPopulation==5964&&LC->GetCurrentCrowdCount()==1500&&!LC->IsSyntheticRenderProfileActive());
		FAftershockCityServiceLookup Lookup;LL->BuildServiceDestinationLookup(Lookup);bool ServicesIdentical=true;int32 ComparedResidents=0,TieResident=INDEX_NONE;
		for(const auto& Resident:LL->GetResidents())
		{
			if(Resident.HomeBuildingID==TieHome&&TieResident==INDEX_NONE)TieResident=Resident.ResidentID;
			if(ComparedResidents++>=32)continue;
			for(const EBuildingType Type:{EBuildingType::GreenSpace,EBuildingType::Restaurant,EBuildingType::Bakery,EBuildingType::Hospital,EBuildingType::FireStation})
				ServicesIdentical&=LL->FindServiceDestination(Resident.ResidentID,Type,Lookup)==LL->FindServiceDestination(Resident.ResidentID,Type);
		}
		Check(TEXT("ephemeral_service_index_matches_canonical_api_including_missing_services"),ServicesIdentical&&Lookup.BuildingsVisited==1000&&Lookup.CacheHits>0);
		Check(TEXT("equal_distance_service_tie_still_selects_smallest_canonical_building_id"),TieResident!=INDEX_NONE&&LL->FindServiceDestination(TieResident,EBuildingType::Restaurant,Lookup)==TieRestaurant&&LL->FindServiceDestination(TieResident,EBuildingType::Restaurant)==TieRestaurant);
		TSet<int32> ResidentIDs;for(const auto& Resident:LL->GetResidents())ResidentIDs.Add(Resident.ResidentID);
		const auto BeforeConstruction=LC->GetRosterRefreshWork();const int32 PopulationBefore=LL->GetState().LivingPopulation,DeathsBefore=LL->GetState().TotalDeaths;
		const int64 BalanceBefore=LifeWorld->GetSubsystem<UEconomyAndStateSubsystem>()->TreasuryTL;int32 NewID=INDEX_NONE;
		const double ConstructionStarted=FPlatformTime::Seconds();const bool Added=LB->PlaceBuilding(FIntPoint(2,35),Definitions.FindRef(EBuildingType::House),EFoundationQuality::Medium,1,NewID);LiveConstructionMs=(FPlatformTime::Seconds()-ConstructionStarted)*1000.;
		const auto AfterConstruction=LC->GetRosterRefreshWork();const uint64 Syncs=AfterConstruction.Synchronizations-BeforeConstruction.Synchronizations;
		LiveServiceVisits=AfterConstruction.ServiceCandidatesVisited-BeforeConstruction.ServiceCandidatesVisited;LiveServiceQueries=AfterConstruction.ServiceQueries-BeforeConstruction.ServiceQueries;LiveServiceHits=AfterConstruction.ServiceCacheHits-BeforeConstruction.ServiceCacheHits;
		Check(TEXT("one_live_building_placement_runs_actual_population_and_crowd_event_chain"),Added&&NewID>0&&LB->GetActiveBuildingsCount()==1001&&Syncs==1&&LC->GetCurrentCrowdCount()==1500);
		Check(TEXT("live_roster_builds_one_service_index_instead_of_copying_all_buildings_per_agent_query"),AfterConstruction.ServiceIndexBuilds-BeforeConstruction.ServiceIndexBuilds==1&&AfterConstruction.ServiceBuildingsVisited-BeforeConstruction.ServiceBuildingsVisited==1001&&LiveServiceQueries==4500&&LiveServiceHits>0&&LiveServiceVisits<LiveServiceQueries*1001);
		Check(TEXT("live_frontage_anchors_use_one_id_index_and_five_constant_time_lookups_per_agent"),AfterConstruction.FrontagesIndexed-BeforeConstruction.FrontagesIndexed==1001&&AfterConstruction.FrontageLookups-BeforeConstruction.FrontageLookups==7500);
		for(const auto& Resident:LL->GetResidents())ResidentIDs.Remove(Resident.ResidentID);
		Check(TEXT("construction_lookup_optimization_preserves_resident_ids_deaths_treasury_and_single_population_owner"),ResidentIDs.IsEmpty()&&LL->GetState().LivingPopulation==PopulationBefore+6&&LL->GetState().TotalDeaths==DeathsBefore&&LifeWorld->GetSubsystem<UEconomyAndStateSubsystem>()->TreasuryTL==BalanceBefore);
		FAftershockCityServiceLookup Fresh;LL->BuildServiceDestinationLookup(Fresh);
		Check(TEXT("new_refresh_lookup_has_no_results_or_history_retained_from_previous_refresh"),Fresh.DestinationsByHomeTypeComponent.IsEmpty()&&Fresh.Queries==0&&Fresh.CacheHits==0&&Fresh.CandidatesVisited==0&&Fresh.BuildingsVisited==1001);
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_LIVE_CONSTRUCTION_FANOUT buildings=1001 canonicalResidents=%d agents=%d placementCpuMs=%.4f rosterSyncs=%llu serviceQueries=%llu serviceCacheHits=%llu actualServiceCandidateVisits=%llu oldAllBuildingVisitUpperWork=%llu source=CanonicalLifePrivateFixtureDefinitions noSyntheticAgents=1 noGpuClaim=1"),LL->GetState().LivingPopulation,LC->GetCurrentCrowdCount(),LiveConstructionMs,Syncs,LiveServiceQueries,LiveServiceHits,LiveServiceVisits,LiveServiceQueries*1001);
        // Explicit native lifecycle-branch fixture, not a boot/RHI/timing claim.
        // Public engine state is restored before destruction. Existing assertions
        // above still exercise the untouched not-begun synchronous contract.
        const bool FixtureWasBegun=LifeWorld->HasBegunPlay();
        Check(TEXT("coalescing_fixture_starts_with_legacy_synchronous_lifecycle"),!FixtureWasBegun);
        LifeWorld->SetBegunPlay(true);
        const auto MembershipValid=[&]()
        {
            TSet<int32> Drawn;bool Valid=LC->GetCurrentCrowdCount()==FMath::Min(1500,LL->GetState().LivingPopulation);
            for(const auto& A:LC->GetCitizenDebugRoster()){FCityResidentRecord R;Valid&=A.ResidentID>0&&!Drawn.Contains(A.ResidentID)&&LL->GetResident(A.ResidentID,R)&&R.bAlive;Drawn.Add(A.ResidentID);}
            return Valid;
        };
        auto* LE=LifeWorld->GetSubsystem<UEconomyAndStateSubsystem>();
        auto* LR=LifeWorld->GetSubsystem<URoadNetworkSubsystem>();
        LC->SynchronizeCanonicalPopulation();
        FAftershockCityLifeSaveState BeforeCurrencyLife,AfterCurrencyLife;LL->CaptureSaveState(BeforeCurrencyLife);
        const int64 BeforeCurrencyTreasury=LE->TreasuryTL;const uint64 BeforeCurrencySyncs=LC->GetRosterRefreshWork().Synchronizations;
        const bool CurrencyPaid=LE->TrySpendMoney(1);LL->CaptureSaveState(AfterCurrencyLife);
        Check(TEXT("pure_currency_transaction_preserves_full_life_snapshot_and_does_not_schedule_roster_work"),CurrencyPaid
            &&LE->TreasuryTL==BeforeCurrencyTreasury-1&&!LC->bCityLifeSyncPending&&LC->GetRosterRefreshWork().Synchronizations==BeforeCurrencySyncs
            &&FAftershockCityLifeSaveState::StaticStruct()->CompareScriptStruct(&BeforeCurrencyLife,&AfterCurrencyLife,0)&&MembershipValid());
        CheckFreshWalkingSnapshot(TEXT("fresh_walking_costs_equal_full_live_predicate_in_initialized_calm_city"),LC);
        const int32 StableLiving=LL->GetState().LivingPopulation,StableDeaths=LL->GetState().TotalDeaths;
        const int64 StableTreasury=LE->TreasuryTL;
        const uint64 SyncBeforeNotices=LC->GetRosterRefreshWork().Synchronizations;
        LL->RefreshCity();LL->RefreshCity();const bool ZeroPayment=LE->TrySpendMoney(0);
        Check(TEXT("settled_live_duplicate_notices_only_queue_one_existing_callback"),ZeroPayment&&LC->bCityLifeSyncPending&&LC->GetRosterRefreshWork().Synchronizations==SyncBeforeNotices);
        Check(TEXT("coalescing_never_defers_or_changes_canonical_people_deaths_treasury"),LL->GetState().LivingPopulation==StableLiving&&LL->GetState().TotalDeaths==StableDeaths&&LE->TreasuryTL==StableTreasury&&MembershipValid());
        LC->UpdateCrowdPositions();
        Check(TEXT("one_actual_crowd_callback_drains_duplicate_notices_once"),!LC->bCityLifeSyncPending&&LC->GetRosterRefreshWork().Synchronizations==SyncBeforeNotices+1&&MembershipValid());
        LL->RefreshCity();const uint64 BeforeBarrier=LC->GetRosterRefreshWork().Synchronizations;
        const bool QueuedBeforeBarrier=LC->bCityLifeSyncPending;LC->SynchronizeCanonicalPopulation();
        Check(TEXT("explicit_canonical_population_barrier_remains_immediate_in_live_world"),QueuedBeforeBarrier&&!LC->bCityLifeSyncPending&&LC->GetRosterRefreshWork().Synchronizations==BeforeBarrier+1&&MembershipValid());
        int32 BirthHome=INDEX_NONE;const uint64 BeforeBirth=LC->GetRosterRefreshWork().Synchronizations;
        const bool BirthPlaced=LB->PlaceBuilding(FIntPoint(3,35),Definitions.FindRef(EBuildingType::House),EFoundationQuality::Medium,1,BirthHome);
        Check(TEXT("live_birth_membership_above1500_cap_is_immediate_without_timer"),BirthPlaced&&BirthHome>0&&StableLiving>1500&&LL->GetState().LivingPopulation==StableLiving+6&&LC->GetCurrentCrowdCount()==1500
            &&LC->GetRosterRefreshWork().Synchronizations==BeforeBirth+1&&!LC->bCityLifeSyncPending&&LC->LastSynchronizedLivingPopulation==StableLiving+6&&MembershipValid());
        const int32 DeathHome=LC->Agents.IsEmpty()?INDEX_NONE:LC->Agents[0].HomeBuildingID;
        const uint64 BeforeDeath=LC->GetRosterRefreshWork().Synchronizations;
        const int32 RealKilled=LL->CommitFatalities(DeathHome,1,false);LL->RefreshCity();
        Check(TEXT("calm_death_above_draw_cap_syncs_before_pause_or_gameover"),RealKilled==1&&LL->GetState().LivingPopulation==StableLiving+5&&LL->GetState().TotalDeaths==StableDeaths+1
            &&LC->GetRosterRefreshWork().Synchronizations==BeforeDeath+1&&!LC->bCityLifeSyncPending&&LC->LastSynchronizedTotalDeaths==StableDeaths+1&&MembershipValid());
        const int32 DeathsAfter=LL->GetState().TotalDeaths;const int32 LivingAfter=LL->GetState().LivingPopulation;
        LC->BeginEmergencyEvacuation(EDisasterType::Flood);const uint64 BeforeEmergencyNotice=LC->GetRosterRefreshWork().Synchronizations;
        LL->RefreshCity();
        Check(TEXT("panic_service_notification_is_immediate_not_coalesced"),LC->IsPanicModeActive()&&!LC->bCityLifeSyncPending&&LC->GetRosterRefreshWork().Synchronizations==BeforeEmergencyNotice+1&&MembershipValid());
        // Warm one REAL normal field on a copied agent so the next assertion
        // proves invalidation, rather than accepting an already-empty cache.
        if(!LC->Agents.IsEmpty()){FCitizenAgent Probe=LC->Agents[0];LC->BuildSafeRoute(Probe,Probe.FinalDestinationLocation);}
        const bool WarmNormalCache=!LC->NormalRouteTrees.IsEmpty();
        const FIntPoint RemovedRoad(15,4);const uint64 BeforeRoadClosure=LC->GetRosterRefreshWork().Synchronizations;
        const bool RoadRemoved=LR->RemoveRoadAt(RemovedRoad);
        Check(TEXT("panic_real_road_removal_invalidates_cache_and_replans_before_return"),WarmNormalCache&&RoadRemoved&&!LR->HasRoad(RemovedRoad)&&LC->GetRosterRefreshWork().Synchronizations>BeforeRoadClosure&&!LC->bCityLifeSyncPending
            &&LC->NormalRouteTrees.IsEmpty()&&LC->GetCitizenPlanningStats().UnsafeRemainingRouteSteps==0&&MembershipValid());
        CheckFreshWalkingSnapshot(TEXT("fresh_walking_costs_equal_live_predicate_after_actual_panic_road_removal"),LC);
        // Removing asphalt leaves public soil walkable. A separate explicit
        // canonical burning-cell fixture verifies true unsafe-edge rejection.
        auto Closed=LT->GetCell(FIntPoint(16,4));Closed.bBurning=true;const bool HazardCellSet=LT->SetCell(Closed.Coords,Closed);
        LR->NotifySaveRestored();
        bool AvoidsUnsafe=true;for(const auto& A:LC->Agents)for(int32 I=A.NextRouteCoordIndex;I<A.RouteGridCoords.Num();++I)AvoidsUnsafe&=A.RouteGridCoords[I]!=Closed.Coords;
        Check(TEXT("panic_network_event_excludes_actual_closed_hazard_cell_immediately"),HazardCellSet&&AvoidsUnsafe&&LC->GetCitizenPlanningStats().UnsafeRemainingRouteSteps==0&&!LC->bCityLifeSyncPending&&MembershipValid());
        CheckFreshWalkingSnapshot(TEXT("fresh_walking_costs_equal_live_predicate_after_actual_burning_cell_closure"),LC);
        LB->ApplyDamageToBuilding(DeathHome,100.f);const bool Memorial=LB->BeginMemorial(DeathHome,LE->CurrentTurn);
        LL->RefreshCity();LC->SynchronizeCanonicalPopulation();FBuildingInstance Marker;
        Check(TEXT("memorial_refresh_preserves_dead_ledger_and_does_not_respawn_visuals"),Memorial&&LB->GetBuildingByID(DeathHome,Marker)&&Marker.DamageState==EDamageState::Memorial
            &&LT->GetCell(Marker.GridCoords).OccupyingBuildingID==DeathHome&&LL->GetState().TotalDeaths==DeathsAfter&&LL->GetState().LivingPopulation==LivingAfter&&MembershipValid());
        CheckFreshWalkingSnapshot(TEXT("fresh_walking_costs_equal_live_predicate_after_actual_memorial_conversion"),LC);
        LifeWorld->SetBegunPlay(FixtureWasBegun);
        Check(TEXT("native_lifecycle_branch_fixture_restores_engine_world_state"),LifeWorld->HasBegunPlay()==FixtureWasBegun);
        UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CROWD_COALESCENCE_REGRESSION source=ActualPrivateFixtureLifeAndCrowd enginePublicLifecycleFlagOnly=1 fullGameBootClaim=0 callbackPerformanceClaim=0 canonicalBirths=6 canonicalDeaths=1 drawCap=1500"));
		LifeWorld->DestroyWorld(false);GEngine->DestroyWorldContext(LifeWorld);LifeWorld->RemoveFromRoot();
	}
	const FString Path=FPaths::ProjectSavedDir()/TEXT("PresentationValidation/presentation_performance_audit.json");IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
	const FString JSON=FString::Printf(TEXT("{\"schema_version\":1,\"scope\":\"Isolated native100-building mutation counters, real32x32 routes, separate synthetic80x65536 route budget, and actual initialized1000-building/1500-agent PlaceBuilding fanout plus engine-public lifecycle-branch coalescence, real6-resident birth,1-death, panic road/hazard closure and memorial checks with private fixture definitions. Lifecycle-branch fixtures are not a full game boot or callback timing claim. Game-thread CPU, ecology reads and component-held mesh references only. No GPU FPS, GC completion, process working-set or measured leak claim.\",\"passed\":%d,\"failed\":%d,\"damage100_cpu_ms\":%.4f,\"decoration_flush_cpu_ms\":%.4f,\"damage_rebuild_delta\":%llu,\"decoration_buildings_visited_delta\":%llu,\"offwindow_ecology_cells_visited\":%llu,\"synthetic_route_cache_peak_bytes\":%llu,\"synthetic_large_route_cache_survivors\":%d,\"initialized_city_place_one_cpu_ms\":%.4f,\"live_service_candidate_visits\":%llu,\"live_service_queries\":%llu,\"live_service_cache_hits\":%llu,\"checks\":[%s]}"),Passed,Failed,DamageMs,FlushMs,Flushed.DamagedRebuilds-Before.DamagedRebuilds,Flushed.DamagedBuildingsVisited-Before.DamagedBuildingsVisited,OffscreenVisits,SyntheticCachePeakBytes,SyntheticCacheSurvivors,LiveConstructionMs,LiveServiceVisits,LiveServiceQueries,LiveServiceHits,*FString::Join(Rows,TEXT(",")));
	if(!FFileHelper::SaveStringToFile(JSON,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))++Failed;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PRESENTATION_PERFORMANCE_RESULT passed=%d failed=%d damage100CpuMs=%.4f decorationFlushCpuMs=%.4f damageRebuildDelta=%llu report=%s"),Passed,Failed,DamageMs,FlushMs,Flushed.DamagedRebuilds-Before.DamagedRebuilds,*Path);
	W->DestroyWorld(false);GEngine->DestroyWorldContext(W);W->RemoveFromRoot();return Failed==0?0:1;
}
