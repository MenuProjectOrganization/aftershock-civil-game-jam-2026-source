#include "AftershockBridgeAuditCommandlet.h"
#include "AftershockHydraulicTerrain.h"
#if WITH_EDITOR
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/DisasterPresentationSubsystem.h"
#include "Aftershock/Public/CityPresentationSubsystem.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	struct FBridgeCheck { FString Name; bool bPassed; };
	struct FBridgeWorld
	{
		UWorld* World=nullptr;
		~FBridgeWorld() { if (World) { World->DestroyWorld(false); if (GEngine) GEngine->DestroyWorldContext(World); World->RemoveFromRoot(); } }
	};
}

UAftershockBridgeAuditCommandlet::UAftershockBridgeAuditCommandlet()
{ IsClient=false; IsServer=false; IsEditor=true; LogToConsole=true; }

int32 UAftershockBridgeAuditCommandlet::Main(const FString& Params)
{
	TArray<FBridgeCheck> Checks; int32 Failed=0;
	const auto Check=[&](const TCHAR* Name,bool bPassed)
	{
		Checks.Add({Name,bPassed}); Failed+=bPassed ? 0 : 1;
		if (bPassed) { UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_BRIDGE_AUDIT_PASS %s"),Name); }
		else { UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_BRIDGE_AUDIT_FAIL %s"),Name); }
	};
	const auto Finish=[&]()
	{
		const FString Directory=FPaths::ProjectSavedDir()/TEXT("PresentationValidation"); IFileManager::Get().MakeDirectory(*Directory,true);
		FString Json=FString::Printf(TEXT("{\n\"schema_version\":1,\n\"scope\":\"Native isolated canonical bridge APIs, HISM transforms and displayed-water routing. No pixel, GPU timing, scientific bridge-engineering, car/bus passenger or packaged-build claim.\",\n\"passed\":%d,\n\"failed\":%d,\n\"checks\":[\n"),Checks.Num()-Failed,Failed);
		for (int32 I=0;I<Checks.Num();++I) Json+=FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}%s\n"),*Checks[I].Name,Checks[I].bPassed ? TEXT("true") : TEXT("false"),I+1<Checks.Num() ? TEXT(",") : TEXT(""));
		Json+=TEXT("]\n}\n");
		if (!FFileHelper::SaveStringToFile(Json,*(Directory/TEXT("bridge_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return 2;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_BRIDGE_AUDIT_RESULT passed=%d failed=%d"),Checks.Num()-Failed,Failed); return Failed==0 ? 0 : 1;
	};
	Check(TEXT("engine_initialized"),GEngine!=nullptr); if (!GEngine) return Finish();
	FBridgeWorld Fixture;
	Fixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("AftershockBridgeFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("isolated_world_created"),Fixture.World!=nullptr); if (!Fixture.World) return Finish();
	UWorld* World=Fixture.World; GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UTerrainGridSubsystem* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();
	URoadNetworkSubsystem* Roads=World->GetSubsystem<URoadNetworkSubsystem>();
	UEconomyAndStateSubsystem* Economy=World->GetSubsystem<UEconomyAndStateSubsystem>();
	UBuildingManagerSubsystem* Buildings=World->GetSubsystem<UBuildingManagerSubsystem>();
	UDisasterPresentationSubsystem* Water=World->GetSubsystem<UDisasterPresentationSubsystem>();
	UDisasterSimulationSubsystem* Simulation=World->GetSubsystem<UDisasterSimulationSubsystem>();
	Check(TEXT("canonical_subsystems_ready"),Terrain && Roads && Economy && Buildings && Water && Simulation);
	if (!Terrain || !Roads || !Economy || !Buildings || !Water || !Simulation) return Finish();
	Terrain->GenerateProceduralTerrain(24,12,500.f,1337); Terrain->GeographyIndex=INDEX_NONE;
	for (FGridCell& Cell:Terrain->Cells)
	{
		const FIntPoint Coord=Cell.Coords; Cell=FGridCell(); Cell.Coords=Coord; Cell.bIsPlayable=true;
		Cell.bIsRiverbed=Coord.X==11 || Coord.X==12; Cell.Elevation=Cell.bIsRiverbed ? 0.f : 100.f;
		Cell.InitialFloodDepth=Cell.bIsRiverbed ? 100.f : 0.f; Cell.FloodDepth=Cell.InitialFloodDepth;
		Cell.RiverCapacity=2.25f; Cell.WaterFlowDamping=.15f;
	}
	const FIntPoint Start(9,5),End(14,5); Terrain->Cells[Terrain->CoordToIndex(Start)].bHasRoad=true;
	const auto HydraulicHash=[&]()
	{
		uint32 Hash=0; for (const FGridCell& Cell:Terrain->Cells)
		{
			Hash=HashCombine(Hash,GetTypeHash(Cell.Elevation)); Hash=HashCombine(Hash,GetTypeHash(Cell.bIsRiverbed));
			Hash=HashCombine(Hash,GetTypeHash(Cell.RiverCapacity)); Hash=HashCombine(Hash,GetTypeHash(Cell.WaterFlowDamping));
			Hash=HashCombine(Hash,GetTypeHash(Cell.InitialFloodDepth)); Hash=HashCombine(Hash,GetTypeHash(Cell.FloodDepth));
		} return Hash;
	};
	const uint32 OriginalHydraulics=HydraulicHash(); const int64 OriginalTreasury=Economy->TreasuryTL;
	TStrongObjectPtr<UAftershockBridgeAuditListener> Listener(NewObject<UAftershockBridgeAuditListener>(World));
	Roads->OnRoadNetworkChanged.AddDynamic(Listener.Get(),&UAftershockBridgeAuditListener::OnChanged);
	FAftershockRoadQuote Quote=Roads->QuoteRoad(Start,End);
	Check(TEXT("automatic_bridge_quote_has_parallel_profile_and_no_mutation"),Quote.bRouteValid && Quote.bIncludesBridge && Quote.BridgeRiverCells==2
		&& Quote.Path.Num()==Quote.SurfaceElevationsCm.Num() && Quote.CostTL>0 && HydraulicHash()==OriginalHydraulics && Roads->GetBridgeCount()==0
		&& Economy->TreasuryTL==OriginalTreasury && Listener->Changes==0 && Roads->CountRoadCells()==1);
	Economy->TreasuryTL=Quote.CostTL-1; FAftershockRoadQuote Purchase;
	Check(TEXT("insufficient_budget_has_no_partial_bridge_or_event"),!Roads->BuildRoad(Start,End,Purchase) && Roads->CountRoadCells()==1 && Listener->Changes==0 && HydraulicHash()==OriginalHydraulics);
	Economy->TreasuryTL=OriginalTreasury;
	const bool bBuilt=Roads->BuildRoad(Start,End,Purchase);
	Check(TEXT("one_bridge_one_exact_payment_one_network_event"),bBuilt && Economy->TreasuryTL==OriginalTreasury-Quote.CostTL && Listener->Changes==1 && Roads->GetBridgeCount()==1);
	if (!bBuilt) return Finish();
	bool bGrade=true,bEdges=true,bRiver=true;
	for (int32 I=0;I<Purchase.Path.Num();++I)
	{
		const FGridCell Cell=Terrain->GetCell(Purchase.Path[I]); bRiver&=Cell.bHasRoad && Cell.BridgeID==0 && Cell.BridgeAxis==1 && Cell.bBridgeEndpoint==(I==0 || I==Purchase.Path.Num()-1);
		bGrade&=FMath::IsNearlyEqual(Roads->RoadGridToWorld(Cell.Coords).Z,Purchase.SurfaceElevationsCm[I],.01f);
		if (I>0) { bGrade&=FMath::Abs(Purchase.SurfaceElevationsCm[I]-Purchase.SurfaceElevationsCm[I-1])<=60.01f; bEdges&=Roads->CanTraverseRoadEdge(Purchase.Path[I-1],Cell.Coords) && Roads->CanTraverseWalkingEdge(Purchase.Path[I-1],Cell.Coords); }
	}
	Check(TEXT("river_flags_elevation_capacity_and_flow_are_preserved"),bRiver && HydraulicHash()==OriginalHydraulics && Terrain->GetCell(FIntPoint(11,5)).bIsRiverbed);
	Check(TEXT("car_and_walking_graph_connect_banks_on_safe_deck"),bGrade && bEdges && Roads->IsPublicWalkingCell(FIntPoint(11,5)) && !Roads->IsPublicWalkingCell(FIntPoint(11,4)));
	Terrain->Cells[Terrain->CoordToIndex(FIntPoint(10,4))].bHasRoad=true;
	Check(TEXT("mid_ramp_side_street_cannot_jump_to_elevated_deck"),!Roads->CanTraverseRoadEdge(FIntPoint(10,4),FIntPoint(10,5)) && !Roads->CanTraverseWalkingEdge(FIntPoint(10,4),FIntPoint(10,5)));
	const FVector Mid=(Roads->RoadGridToWorld(Start)+Roads->RoadGridToWorld(FIntPoint(10,5)))*.5;
	Check(TEXT("ramp_surface_is_continuous_at_cell_boundary"),FMath::IsNearlyEqual(Roads->SampleRoadSurfaceElevation(Mid),Mid.Z,.05f));
	const FIntPoint Deck(11,5);
	Check(TEXT("normal_river_water_does_not_close_elevated_deck"),Roads->GetRoadFloodDepthCm(Deck)==0.f && Roads->IsRoadTraversable(Deck));
	AAftershockCityPresentationActor* Visual=World->SpawnActor<AAftershockCityPresentationActor>();
	int32 BridgePieces=0,RoadTiles=0;
	if (Visual)
	{
		Visual->RebuildCosmeticDistrict(*Terrain,*Buildings);
		TInlineComponentArray<UHierarchicalInstancedStaticMeshComponent*> Batches(Visual);
		for (const auto* Batch:Batches) { if (Batch->GetName()==TEXT("BridgeStructureHISM")) BridgePieces=Batch->GetInstanceCount(); else if (Batch->GetName().StartsWith(TEXT("Road"))) RoadTiles+=Batch->GetInstanceCount(); }
	}
	Check(TEXT("real_hism_deck_barriers_piers_and_asphalt_are_created"),Visual && BridgePieces>=Purchase.Path.Num()*3 && RoadTiles>=Purchase.Path.Num());
	FAftershockTerrainSaveState Saved; Terrain->CaptureSaveState(Saved); FText Error;
	Terrain->Cells[Terrain->CoordToIndex(Deck)].RoadSurfaceElevationCm+=100.f;
	Check(TEXT("save_roundtrip_restores_canonical_bridge_profile"),Terrain->RestoreSaveState(Saved,Error) && Terrain->GetCell(Deck).RoadSurfaceElevationCm==Saved.Cells[Terrain->CoordToIndex(Deck)].RoadSurfaceElevationCm);
	FAftershockTerrainSaveState Corrupt=Saved; Corrupt.Cells[Terrain->CoordToIndex(Deck)].BridgeAxis=0;
	Check(TEXT("invalid_bridge_restore_is_atomic"),!Terrain->RestoreSaveState(Corrupt,Error) && Terrain->GetCell(Deck).BridgeAxis==1 && HydraulicHash()==OriginalHydraulics);
	Terrain->Cells[Terrain->CoordToIndex(Deck)].LavaCoverage01=.5f; Roads->NotifySaveRestored();
	Check(TEXT("canonical_lava_closes_the_entire_span_for_services_and_walkers"),Roads->IsBridgeClosed(0) && !Roads->IsRoadTraversable(Start) && !Roads->IsPublicWalkingCell(End));
	Terrain->Cells[Terrain->CoordToIndex(Deck)].LavaCoverage01=0.f; Roads->NotifySaveRestored();
	Check(TEXT("cleared_hazard_reopens_existing_span_without_charging"),!Roads->IsBridgeClosed(0) && Roads->IsRoadTraversable(Deck) && Economy->TreasuryTL==OriginalTreasury-Quote.CostTL);
	// Exercise actual snapshot -> displayed surface -> network refresh. The opt-in
	// visual fixture begins at the flood peak; real-time closure latency is live QA.
	Roads->OnWorldBeginPlay(*World); Water->OnWorldBeginPlay(*World);
	FAftershockDisasterVisualSnapshot Snapshot; Snapshot.Revision=1; Snapshot.Type=EDisasterType::Flood; Snapshot.EffectiveIntensity=100.f; Snapshot.DurationSeconds=24.f; Snapshot.StartedAtWorldSeconds=World->GetTimeSeconds()-13.f;
	for (const FGridCell& Cell:Terrain->Cells) Snapshot.LocalSeverity.Add(Cell.Coords,100.f);
	Simulation->OnDisasterVisualSnapshotChanged.Broadcast(Snapshot);
	Roads->NotifySaveRestored();
	float MaximumDepth=0.f; for (FIntPoint Coord:Purchase.Path) MaximumDepth=FMath::Max(MaximumDepth,Roads->GetRoadFloodDepthCm(Coord));
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_BRIDGE_FLOOD_FIXTURE kind=belowDeck riverCapacity=2.25 deckZ=%.2f maximumRoadWaterCm=%.2f closed=%d realDisplayedSurface=1"),Roads->GetRoadSurfaceElevation(Deck),MaximumDepth,Roads->IsBridgeClosed(0));
	Check(TEXT("actual_displayed_rain_below_deck_keeps_bridge_open"),MaximumDepth>0.f && MaximumDepth<=35.f && !Roads->IsBridgeClosed(0) && Roads->IsRoadTraversable(Start));
	Snapshot.Revision=2; Snapshot.Type=EDisasterType::Earthquake; Snapshot.DurationSeconds=0.f;
	Simulation->OnDisasterVisualSnapshotChanged.Broadcast(Snapshot); Roads->NotifySaveRestored();
	Check(TEXT("receded_display_water_reopens_bridge_without_new_hazard_solver"),!Roads->IsBridgeClosed(0) && Roads->IsRoadTraversable(Deck));
	const int64 BeforeRemoval=Economy->TreasuryTL; const bool bRemoved=Roads->RemoveRoadAt(Deck);
	Check(TEXT("demolition_removes_whole_bridge_preserving_preexisting_bank_road"),bRemoved && Roads->GetBridgeCount()==0 && Terrain->GetCell(Start).bHasRoad && !Terrain->GetCell(End).bHasRoad && !Terrain->GetCell(Deck).bHasRoad && Economy->TreasuryTL==BeforeRemoval && HydraulicHash()==OriginalHydraulics);
	FGridCell Blocked=Terrain->GetCell(FIntPoint(13,5)); Blocked.OccupyingBuildingID=999; Terrain->SetCell(Blocked.Coords,Blocked);
	Check(TEXT("occupied_bank_rejects_bridge_without_payment"),!Roads->QuoteRoad(Start,End).bRouteValid && Economy->TreasuryTL==BeforeRemoval);
	Blocked.OccupyingBuildingID=INDEX_NONE; Terrain->SetCell(Blocked.Coords,Blocked);
	const FAftershockRoadQuote Reversed=Roads->QuoteRoad(End,Start);
	bool bReversed=Reversed.bRouteValid && Reversed.bIncludesBridge && Reversed.Path.Num()==Purchase.Path.Num() && Reversed.CostTL==Purchase.CostTL;
	for (int32 I=0;bReversed && I<Reversed.Path.Num();++I) bReversed&=Reversed.Path[I]==Purchase.Path[Purchase.Path.Num()-1-I]
		&& FMath::IsNearlyEqual(Reversed.SurfaceElevationsCm[I],Purchase.SurfaceElevationsCm[Purchase.SurfaceElevationsCm.Num()-1-I],.01f);
	Check(TEXT("reversed_bank_selection_has_identical_cost_and_geometry"),bReversed);
	FGridCell Dam=Terrain->GetCell(Deck); Dam.bHasDam=true; Terrain->SetCell(Deck,Dam);
	Check(TEXT("bridge_cannot_overwrite_existing_canonical_dam"),!Roads->QuoteRoad(Start,End).bRouteValid);
	Dam.bHasDam=false; Terrain->SetCell(Deck,Dam);
	FGridCell ExtraChannel=Terrain->GetCell(FIntPoint(8,5)); ExtraChannel.bIsRiverbed=true; Terrain->SetCell(ExtraChannel.Coords,ExtraChannel);
	Check(TEXT("separate_channels_require_separate_bridges"),!Roads->QuoteRoad(FIntPoint(7,5),End).bRouteValid);
	ExtraChannel.bIsRiverbed=false; Terrain->SetCell(ExtraChannel.Coords,ExtraChannel);
	FGridCell Steep=Terrain->GetCell(FIntPoint(10,5)); const float OldElevation=Steep.Elevation; Steep.Elevation=3000.f; Terrain->SetCell(Steep.Coords,Steep);
	Check(TEXT("impossible_ramp_rejects_instead_of_teleporting_or_terraforming"),!Roads->QuoteRoad(Start,End).bRouteValid && Terrain->GetCell(Steep.Coords).Elevation==3000.f && Economy->TreasuryTL==BeforeRemoval);
	Steep.Elevation=OldElevation; Terrain->SetCell(Steep.Coords,Steep);
	Check(TEXT("all_rejected_quotes_preserve_hydraulic_source"),HydraulicHash()==OriginalHydraulics);
	// Separate positive fixture: eight river cells and high flat banks retain
	// enough actual reconstructed water to overtop the real 160cm deck. The first
	// narrow/high-capacity fixture legitimately remained below deck (~119cm water
	// level), so closing it would have been a false positive. No closure threshold,
	// hazard coefficient or display depth is overridden to make this test pass.
	Terrain->GenerateProceduralTerrain(14,12,500.f,1337);
	for (FGridCell& Cell:Terrain->Cells)
	{
		const FIntPoint Coord=Cell.Coords; Cell=FGridCell(); Cell.Coords=Coord; Cell.bIsPlayable=true;
		Cell.bIsRiverbed=Coord.X>=3 && Coord.X<=10; Cell.Elevation=Cell.bIsRiverbed ? 0.f : 150.f;
		Cell.RiverCapacity=0.f; Cell.InitialFloodDepth=Cell.bIsRiverbed ? 100.f : 0.f; Cell.FloodDepth=Cell.InitialFloodDepth;
	}
	Roads->NotifySaveRestored(); const uint32 PositiveHydraulics=HydraulicHash();
	FAftershockRoadQuote WideBridge; const FIntPoint WideStart(1,5),WideEnd(12,5);
	const bool bWideBuilt=Roads->BuildRoad(WideStart,WideEnd,WideBridge);
	Snapshot.Revision=3; Snapshot.Type=EDisasterType::Flood; Snapshot.DurationSeconds=24.f; Snapshot.EffectiveIntensity=100.f;
	Snapshot.StartedAtWorldSeconds=World->GetTimeSeconds()-13.f; Snapshot.LocalSeverity.Reset();
	for (const FGridCell& Cell:Terrain->Cells) Snapshot.LocalSeverity.Add(Cell.Coords,100.f);
	Simulation->OnDisasterVisualSnapshotChanged.Broadcast(Snapshot); Roads->NotifySaveRestored();
	float MaximumOvertopping=0.f; for (FIntPoint Coord:WideBridge.Path) MaximumOvertopping=FMath::Max(MaximumOvertopping,Roads->GetRoadFloodDepthCm(Coord));
	const int32 WideID=Terrain->GetCell(WideStart).BridgeID;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_BRIDGE_FLOOD_FIXTURE kind=overtopping riverCells=8 riverCapacity=0 bankZ=150 deckZ=%.2f maximumRoadWaterCm=%.2f closed=%d realDisplayedSurface=1"),Roads->GetRoadSurfaceElevation(FIntPoint(5,5)),MaximumOvertopping,Roads->IsBridgeClosed(WideID));
	Check(TEXT("actual_displayed_overtopping_closes_whole_bridge"),bWideBuilt && WideBridge.BridgeRiverCells==8 && MaximumOvertopping>35.f
		&& Roads->IsBridgeClosed(WideID) && !Roads->IsRoadTraversable(WideStart) && !Roads->IsPublicWalkingCell(WideEnd) && HydraulicHash()==PositiveHydraulics);
	// Separate synthetic append-only fixture: actual negative global storage
	// coordinates, unequal neighbouring mineral deposits, no geographic truth
	// claim. The water owner is imported through its public restore API.
	FAftershockPhysicalHazardSaveState LegacyPhysical;FText RoadHeightError;
	const bool LegacyRestored=Simulation->RestorePhysicalSaveState(LegacyPhysical,RoadHeightError);
	Terrain->GenerateProceduralTerrain(8,8,1000.f,1337);
	Terrain->GeographyIndex=INDEX_NONE;Terrain->ContinuationGeneratorVersion=3;
	Terrain->GridOrigin=FVector(-2500000.,3000000.,123456.);
	for(FGridCell& C:Terrain->Cells)
	{
		const FIntPoint Coord=C.Coords;C=FGridCell();C.Coords=Coord;C.bIsPlayable=true;C.Elevation=1000.f;
		C.HydraulicProfileVersion=1;
	}
	const FIntPoint FarMin(-20000,-30000),RoadCoord=FarMin+FIntPoint(4,4);
	for(int32 Y=0;Y<9;++Y)for(int32 X=0;X<9;++X)
	{
		FGridCell C;C.Coords=FarMin+FIntPoint(X,Y);C.bIsPlayable=true;C.bHasRoad=true;
		C.Elevation=200000.f+X*8.f+Y*12.f;C.MaterialScenarioVersion=1;C.HydraulicProfileVersion=1;
		C.GroundErodedDepthM=X*.01f;C.GroundDepositedDepthM=(X*Y)*.0125f;
		Terrain->Cells.Add(C);
	}
	const FGridCell SourceRoad=Terrain->GetCell(RoadCoord);
	const FVector CentreRoad=Terrain->GridToWorld(RoadCoord);
	const FVector OffCentreRoad=CentreRoad+FVector(200.,100.,0.);
	Check(TEXT("road_height_fixture_resolves_far_negative_resident_storage"),LegacyRestored&&Terrain->IsValidCoord(RoadCoord)
		&&Terrain->GetResidentCellBounds().Min==FarMin&&SourceRoad.Coords==RoadCoord);
	Check(TEXT("legacy_road_centre_ignores_physical_overlay_exactly_as_before"),FMath::IsNearlyEqual(Roads->GetRoadSurfaceElevation(RoadCoord),SourceRoad.Elevation+8.f,.001f));
	Check(TEXT("legacy_road_offcentre_remains_original_cell_height"),FMath::IsNearlyEqual(Roads->SampleRoadSurfaceElevation(OffCentreRoad),SourceRoad.Elevation+8.f,.001f));
	FAftershockPhysicalHazardSaveState PhysicalRoad;
	PhysicalRoad.ModelVersion=2;PhysicalRoad.bWaterInitialized=true;PhysicalRoad.CapturedCellAreaM2=100.;
	for(const FGridCell& C:Terrain->Cells)
	{
		FAftershockHydraulicCellSaveRecord W;W.Coord=C.Coords;FText E;
		TryGetAftershockHydraulicBedCm(C,true,W.BedElevationCm,E);
		W.VolumeM3=C.Coords==RoadCoord?200.:0.;PhysicalRoad.WaterCells.Add(W);
	}
	PhysicalRoad.WaterBudget.InitialSurfaceM3=200.;PhysicalRoad.WaterBudget.FinalSurfaceM3=200.;
	const bool PhysicalRoadRestored=Simulation->RestorePhysicalSaveState(PhysicalRoad,RoadHeightError);
	Check(TEXT("road_height_fixture_imports_real_model2_owner_frontier"),PhysicalRoadRestored&&Simulation->IsHydraulicModel2Active());
	double Ground00=0.,Ground10=0.,Ground01=0.;
	const bool SourceGround=TryGetAftershockMaterialGroundCm(SourceRoad,true,Ground00,RoadHeightError)
		&&TryGetAftershockMaterialGroundCm(Terrain->GetCell(RoadCoord+FIntPoint(1,0)),true,Ground10,RoadHeightError)
		&&TryGetAftershockMaterialGroundCm(Terrain->GetCell(RoadCoord+FIntPoint(0,1)),true,Ground01,RoadHeightError);
	Check(TEXT("model2_road_centre_follows_mineral_deposit_minus_erosion"),PhysicalRoadRestored&&SourceGround
		&&FMath::IsNearlyEqual(static_cast<double>(Roads->GetRoadSurfaceElevation(RoadCoord)),Ground00+8.,.02));
	Check(TEXT("model2_sample_and_grid_centre_are_identical"),PhysicalRoadRestored&&FMath::IsNearlyEqual(Roads->SampleRoadSurfaceElevation(CentreRoad),Roads->GetRoadSurfaceElevation(RoadCoord),.02f));
	// At U=.4,V=.2 in the positive half-cell triangle, its three knot
	// weights reduce independently to .7*C00+.2*C10+.1*C01 (no C11).
	Check(TEXT("model2_offcentre_uses_installed_halfgrid_triangle_not_bilinear"),PhysicalRoadRestored&&SourceGround
		&&FMath::IsNearlyEqual(static_cast<double>(Roads->SampleRoadSurfaceElevation(OffCentreRoad)),.7*Ground00+.2*Ground10+.1*Ground01+8.,.02));
	Check(TEXT("model2_road_flood_depth_is_head_minus_same_mineral_road"),PhysicalRoadRestored&&FMath::IsNearlyEqual(Roads->GetRoadFloodDepthCm(RoadCoord),192.f,.02f));
	const FIntPoint ChannelCoord=FarMin+FIntPoint(1,1);FGridCell Channel=Terrain->GetCell(ChannelCoord);
	Channel.bHasRoad=false;Channel.bIsRiverbed=true;Channel.HydraulicExcavationCm=300.f;Terrain->SetCell(ChannelCoord,Channel);
	double ChannelGround=0.;const bool HasChannelGround=TryGetAftershockMaterialGroundCm(Channel,true,ChannelGround,RoadHeightError);
	Check(TEXT("nonbridge_public_support_never_uses_excavated_channel_waterbed"),HasChannelGround
		&&FMath::IsNearlyEqual(static_cast<double>(Roads->GetRoadSurfaceElevation(ChannelCoord)),ChannelGround+8.,.02)
		&&FMath::IsNearlyEqual(static_cast<double>(Roads->SampleRoadSurfaceElevation(Terrain->GridToWorld(ChannelCoord))),ChannelGround+8.,.02));
	FGridCell DeckProbe=Terrain->GetCell(RoadCoord);DeckProbe.BridgeID=777;DeckProbe.BridgeAxis=1;
	DeckProbe.RoadSurfaceElevationCm=DeckProbe.Elevation+800.f;Terrain->SetCell(RoadCoord,DeckProbe);
	Check(TEXT("model2_existing_bridge_deck_profile_is_unchanged_by_mineral_offsets"),Roads->GetRoadSurfaceElevation(RoadCoord)==DeckProbe.RoadSurfaceElevationCm
		&&Roads->SampleRoadSurfaceElevation(CentreRoad)==DeckProbe.RoadSurfaceElevationCm);
	Terrain->SetCell(RoadCoord,SourceRoad);
	Check(TEXT("road_support_queries_preserve_immutable_dem_and_finite_mineral_stock"),Terrain->GetCell(RoadCoord).Elevation==SourceRoad.Elevation
		&&Terrain->GetCell(RoadCoord).GroundDepositedDepthM==SourceRoad.GroundDepositedDepthM&&Terrain->GetCell(RoadCoord).GroundErodedDepthM==SourceRoad.GroundErodedDepthM);

	return Finish();
}
#else
UAftershockBridgeAuditCommandlet::UAftershockBridgeAuditCommandlet() {}
int32 UAftershockBridgeAuditCommandlet::Main(const FString&) { return 1; }
#endif
