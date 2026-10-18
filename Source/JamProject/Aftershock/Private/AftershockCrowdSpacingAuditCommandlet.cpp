#include "AftershockCrowdSpacingAuditCommandlet.h"
#include "AftershockTerrainSurface.h"

#if WITH_EDITOR
#include "Aftershock/Public/AftershockSoundCatalog.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/TransportSubsystem.h"
#include "Aftershock/Public/DisasterPresentationSubsystem.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "GameFramework/PlayerController.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "StaticMeshResources.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "TimerManager.h"
#include <limits>

namespace
{
	struct FSpacingAuditWorld
	{
		UWorld* World=nullptr;
		~FSpacingAuditWorld()
		{
			if (!World) return;
			World->DestroyWorld(false);
			if (GEngine) GEngine->DestroyWorldContext(World);
			World->RemoveFromRoot();
		}
	};
	FString EscapeSpacingJson(FString Value)
	{
		Value.ReplaceInline(TEXT("\\"),TEXT("\\\\"));Value.ReplaceInline(TEXT("\""),TEXT("\\\""));
		Value.ReplaceInline(TEXT("\n"),TEXT("\\n"));Value.ReplaceInline(TEXT("\r"),TEXT("\\r"));
		return Value;
	}
	TArray<uint8> SerializeCrowdState(FAftershockCrowdSaveState State)
	{
		TArray<uint8> Bytes;
		FMemoryWriter Writer(Bytes,true);
		FObjectAndNameAsStringProxyArchive Archive(Writer,false);
		// Match reflected struct serialization; these fields are UPROPERTY rather
		// than CPF_SaveGame-only fields. No UObject addresses are written.
		FAftershockCrowdSaveState::StaticStruct()->SerializeItem(Archive,&State,nullptr);
		return Bytes;
	}
	bool SampleRiverMeshHeight(const UStaticMeshComponent* Component,const FVector2D& Point,float& OutHeight)
	{
		const UStaticMesh* Mesh=Component?Component->GetStaticMesh():nullptr;
		const auto* Data=Mesh?Mesh->GetRenderData():nullptr;if(!Data||Data->LODResources.IsEmpty())return false;
		const auto& LOD=Data->LODResources[0];const auto Indices=LOD.IndexBuffer.GetArrayView();
		for(int32 I=0;I+2<Indices.Num();I+=3)
		{
			const FVector A=Component->GetComponentTransform().TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Indices[I])));
			const FVector B=Component->GetComponentTransform().TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Indices[I+1])));
			const FVector C=Component->GetComponentTransform().TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Indices[I+2])));
			const double Den=(B.Y-C.Y)*(A.X-C.X)+(C.X-B.X)*(A.Y-C.Y);if(FMath::Abs(Den)<.001)continue;
			const double U=((B.Y-C.Y)*(Point.X-C.X)+(C.X-B.X)*(Point.Y-C.Y))/Den;
			const double V=((C.Y-A.Y)*(Point.X-C.X)+(A.X-C.X)*(Point.Y-C.Y))/Den;
			const double W=1.-U-V;
			if(U>=-.000001&&V>=-.000001&&W>=-.000001){OutHeight=static_cast<float>(A.Z*U+B.Z*V+C.Z*W);return true;}
		}
		return false;
	}
	bool RiverMeshSpansRowCentres(const UStaticMeshComponent* Component,int32 FirstRow,int32 LastRow,float CellSize,float OriginY)
	{
		const UStaticMesh* Mesh=Component?Component->GetStaticMesh():nullptr;
		const auto* Data=Mesh?Mesh->GetRenderData():nullptr;if(!Data||Data->LODResources.IsEmpty())return false;
		const auto& LOD=Data->LODResources[0];const auto Indices=LOD.IndexBuffer.GetArrayView();
		const float First=OriginY+(FirstRow+.5f)*CellSize,Last=OriginY+(LastRow+.5f)*CellSize;
		for(int32 I=0;I+2<Indices.Num();I+=3)
		{
			const float A=LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Indices[I]).Y;
			const float B=LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Indices[I+1]).Y;
			const float C=LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Indices[I+2]).Y;
			if(FMath::Min3(A,B,C)<=First+.01f&&FMath::Max3(A,B,C)>=Last-.01f)return true;
		}
		return false;
	}
}

UAftershockCrowdSpacingAuditCommandlet::UAftershockCrowdSpacingAuditCommandlet()
{
	IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;
}

int32 UAftershockCrowdSpacingAuditCommandlet::Main(const FString& Params)
{
	int32 Passed=0,Failed=0,MaxDenseChecks=0;
	TArray<FString> Results;
	const auto Check=[&](const TCHAR* Name,bool bPass,const FString& Detail=FString())
	{
		bPass?++Passed:++Failed;
		Results.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s,\"detail\":\"%s\"}"),Name,bPass?TEXT("true"):TEXT("false"),*EscapeSpacingJson(Detail)));
		if (bPass) { UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CROWD_SPACING_PASS %s %s"),Name,*Detail); }
		else { UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_CROWD_SPACING_FAIL %s %s"),Name,*Detail); }
	};
	const auto Finish=[&]()
	{
		const FString Directory=FPaths::ProjectSavedDir()/TEXT("PresentationValidation");
		IFileManager::Get().MakeDirectory(*Directory,true);
		const FString JSON=FString::Printf(TEXT("{\"schema_version\":1,\"scope\":\"Isolated native visual spacing, bounded reported neighbour checks, ISM visibility and reflected crowd snapshot validation. No live gameplay, GPU, real-world crowd-safety or throughput claim.\",\"dense_agent_count\":64,\"dense_max_neighbour_checks\":%d,\"dense_check_budget\":4096,\"passed\":%d,\"failed\":%d,\"checks\":[%s]}"),MaxDenseChecks,Passed,Failed,*FString::Join(Results,TEXT(",")));
		const FString Path=Directory/TEXT("crowd_spacing_audit.json");
		if (!FFileHelper::SaveStringToFile(JSON,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return 2;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CROWD_SPACING_RESULT passed=%d failed=%d denseMaxChecks=%d report=%s"),Passed,Failed,MaxDenseChecks,*Path);
		return Failed==0?0:1;
	};
	Check(TEXT("engine_initialized"),GEngine!=nullptr);if(!GEngine)return Finish();
	FSpacingAuditWorld Fixture;
	Fixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("CrowdSpacingFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("isolated_game_world_created"),Fixture.World!=nullptr);if(!Fixture.World)return Finish();
	UWorld* World=Fixture.World;GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();auto* Crowd=World->GetSubsystem<UCrowdRenderSubsystem>();
	auto* Life=World->GetSubsystem<UCityLifeSubsystem>();auto* Buildings=World->GetSubsystem<UBuildingManagerSubsystem>();
	auto* Economy=World->GetSubsystem<UEconomyAndStateSubsystem>();auto* Roads=World->GetSubsystem<URoadNetworkSubsystem>();
	Check(TEXT("canonical_subsystems_available"),Terrain&&Crowd&&Life&&Buildings&&Economy&&Roads);if(!Terrain||!Crowd||!Life||!Buildings||!Economy||!Roads)return Finish();
	World->GetTimerManager().ClearTimer(Crowd->CrowdTimerHandle);
	World->GetTimerManager().ClearTimer(Crowd->DisasterVisualCompletionHandle);
	Terrain->GenerateProceduralTerrain(12,12,500.f,1337);Terrain->GridOrigin=FVector::ZeroVector;
	for(int32 Y=0;Y<12;++Y)for(int32 X=0;X<12;++X)
	{
		FGridCell Cell;Cell.Coords=FIntPoint(X,Y);Cell.bIsPlayable=true;Cell.Elevation=0.f;Cell.bHasRoad=Y==3;
		Terrain->Cells[Terrain->CoordToIndex(Cell.Coords)]=Cell;
	}
	const int64 InitialTreasury=Economy->TreasuryTL;
	const float InitialSatisfaction=Economy->PublicSatisfaction;
	const auto AgentAt=[](const FVector& Location,int32 Seed)
	{
		FCitizenAgent Agent;Agent.CurrentLocation=Location;Agent.TargetLocation=Location;Agent.FinalDestinationLocation=Location;
		Agent.HomeLocation=Location;Agent.WorkLocation=Location;Agent.LeisureLocation=Location;
		Agent.RandomSeed=Seed;Agent.RandomSequence=0;Agent.MoveSpeed=80.f;Agent.Activity=EAftershockCitizenActivity::Commuting;
		return Agent;
	};
	const FVector Centre=Terrain->GridToWorld(FIntPoint(5,5))+FVector(0,0,5.f);
	const TArray<FCitizenAgent> Pair={AgentAt(Centre,123),AgentAt(Centre,456)};
	Crowd->Agents=Pair;
	for(int32 Iteration=0;Iteration<5;++Iteration)Crowd->ResolveCrowdSpacing(*Terrain);
	const TArray<FCitizenAgent> FirstPair=Crowd->Agents;
	Check(TEXT("two_overlapping_agents_separate_in_bounded_updates"),Crowd->Agents.Num()==2&&FVector::Dist2D(Crowd->Agents[0].CurrentLocation,Crowd->Agents[1].CurrentLocation)>=79.5f,
		FString::Printf(TEXT("distanceCm=%.3f afterFiveResolves"),FVector::Dist2D(Crowd->Agents[0].CurrentLocation,Crowd->Agents[1].CurrentLocation)));
	Crowd->Agents=Pair;
	for(int32 Iteration=0;Iteration<5;++Iteration)Crowd->ResolveCrowdSpacing(*Terrain);
	Check(TEXT("same_roster_seed_and_updates_are_deterministic"),Crowd->Agents[0].CurrentLocation.Equals(FirstPair[0].CurrentLocation,.0001f)&&Crowd->Agents[1].CurrentLocation.Equals(FirstPair[1].CurrentLocation,.0001f));
	Crowd->Agents.Reset();
	for(int32 I=0;I<64;++I)Crowd->Agents.Add(AgentAt(Centre,1000+I));
	bool bDenseBounded=true,bDenseSafe=true;
	for(int32 Iteration=0;Iteration<8;++Iteration)
	{
		Crowd->ResolveCrowdSpacing(*Terrain);const auto Stats=Crowd->GetCrowdSpacingStats();
		MaxDenseChecks=FMath::Max(MaxDenseChecks,Stats.NeighbourChecks);
		bDenseBounded&=Stats.NeighbourChecks<=2*64*32&&Stats.OutdoorAgents==64;
		for(const auto& Agent:Crowd->Agents)bDenseSafe&=Roads->IsPublicWalkingCell(Terrain->WorldToGrid(Agent.CurrentLocation))&&!Agent.CurrentLocation.ContainsNaN();
	}
	Check(TEXT("dense_64_cluster_has_bounded_reported_neighbour_checks"),bDenseBounded,FString::Printf(TEXT("max=%d budget=%d; bounded counter is not a measured all-pairs timing proof"),MaxDenseChecks,2*64*32));
	Check(TEXT("dense_cluster_preserves_roster_and_safe_positions"),Crowd->Agents.Num()==64&&bDenseSafe&&Crowd->GetCrowdSpacingStats().AcceptedCorrections>0);
	FAftershockCrowdSaveState LegacyOverlap;for(int32 I=0;I<64;++I)LegacyOverlap.Agents.Add(AgentAt(Centre,40000+I));
	FText MigrationError;
	const bool LegacyRestored=Crowd->RestoreSaveState(LegacyOverlap,MigrationError);bool LegacySeparated=LegacyRestored;
	for(int32 I=0;I<Crowd->Agents.Num();++I)for(int32 J=I+1;J<Crowd->Agents.Num();++J)LegacySeparated&=FVector::DistSquared2D(Crowd->Agents[I].CurrentLocation,Crowd->Agents[J].CurrentLocation)>=FMath::Square(63.99f);
	Check(TEXT("legacy_64_same_point_snapshot_reconciles_all_pairs_before_first_ism_submission"),LegacyRestored&&LegacySeparated&&Crowd->Agents.Num()==64);
	FAftershockCrowdSaveState BeforeCapacityFailure;Crowd->CaptureSaveState(BeforeCapacityFailure);const auto TerrainBeforeCapacity=Terrain->Cells;
	for(auto& Cell:Terrain->Cells)Cell.bIsPlayable=Cell.Coords==FIntPoint(5,5);
	const bool CapacityRejected=!Crowd->RestoreSaveState(LegacyOverlap,MigrationError);FAftershockCrowdSaveState AfterCapacityFailure;Crowd->CaptureSaveState(AfterCapacityFailure);
	Terrain->Cells=TerrainBeforeCapacity;
	Check(TEXT("legacy_capacity_exhaustion_rejects_load_without_hiding_or_replacing_live_agents"),CapacityRejected&&!MigrationError.IsEmpty()&&SerializeCrowdState(BeforeCapacityFailure)==SerializeCrowdState(AfterCapacityFailure));
	Crowd->Agents.Reset();Crowd->RebuildDestinationCaches();
	for(int32 I=0;I<1500;++I)
	{
		const FIntPoint Cell(1+I%10,1+(I/10)%10);
		const FVector At=Terrain->GridToWorld(Cell)+FVector((I%3-1)*240.f,((I/3)%3-1)*240.f,5.f);
		auto Agent=AgentAt(At,71000+I);Agent.ActivityAfterArrival=EAftershockCitizenActivity::AtWork;
		Crowd->BuildSafeRoute(Agent,Terrain->GridToWorld(FIntPoint(11-Cell.X,11-Cell.Y))+FVector(0,0,5.f));Crowd->Agents.Add(Agent);
	}
	bool bStressSafe=true,bStressMoved=false;
	for(int32 Step=0;Step<16;++Step)
	{
		Crowd->UpdateCrowdPositions();bStressMoved|=Crowd->Agents[0].WalkVelocityCmPerSec.SizeSquared()>1.f;
		for(const auto& Agent:Crowd->Agents)bStressSafe&=Terrain->IsPlayableCoord(Terrain->WorldToGrid(Agent.CurrentLocation))&&!Agent.CurrentLocation.ContainsNaN();
	}
	Check(TEXT("fifteen_hundred_real_motion_updates_keep_all_visual_agents_on_playable_cells"),Crowd->Agents.Num()==1500&&bStressSafe&&bStressMoved);
	Crowd->Agents={AgentAt(Centre,123),AgentAt(Centre+FVector(0,0,200.f),456)};
	Crowd->ResolveCrowdSpacing(*Terrain);
	Check(TEXT("same_xy_different_height_is_not_pushed"),Crowd->Agents[0].CurrentLocation==Centre&&Crowd->Agents[1].CurrentLocation==Centre+FVector(0,0,200.f)&&Crowd->GetCrowdSpacingStats().AcceptedCorrections==0);
	const FIntPoint HazardCoord(4,3);const FGridCell OriginalCell=Terrain->GetCell(HazardCoord);
	const FVector Boundary(1998.f,1750.f,5.f);
	Terrain->Cells[Terrain->CoordToIndex(HazardCoord)].bIsRiverbed=true;
	Crowd->Agents={AgentAt(Boundary,123),AgentAt(Boundary-FVector(1,0,0),456)};
	Crowd->ResolveCrowdSpacing(*Terrain);
	Check(TEXT("separation_never_projects_into_river"),Crowd->GetCrowdSpacingStats().RejectedUnsafeCorrections>0&&Crowd->Agents[0].CurrentLocation==Boundary&&Roads->IsPublicWalkingCell(Terrain->WorldToGrid(Crowd->Agents[1].CurrentLocation)));
	Terrain->Cells[Terrain->CoordToIndex(HazardCoord)]=OriginalCell;

	// A separation correction can put a walker in an adjacent legal cell while
	// its previous route waypoint remains across a blocked diagonal corner.
	const FIntPoint CornerHazard(6,6);const FGridCell BeforeCorner=Terrain->GetCell(CornerHazard);
	Terrain->Cells[Terrain->CoordToIndex(CornerHazard)].bIsRiverbed=true;
	const FVector CornerStart(2999.f,3008.f,5.f),CornerGoal(3250.f,2750.f,5.f);
	Check(TEXT("swept_walking_segment_rejects_invalid_corner_even_with_legal_endpoints"),Roads->IsPublicWalkingCell(Terrain->WorldToGrid(CornerStart))
		&&Roads->IsPublicWalkingCell(Terrain->WorldToGrid(CornerGoal))&&!Crowd->CanMoveCitizenSegment(*Terrain,CornerStart,CornerGoal));
	Crowd->Agents={AgentAt(CornerStart,7187)};Crowd->Agents[0].TargetLocation=CornerGoal;Crowd->Agents[0].FinalDestinationLocation=CornerGoal;
	Crowd->Agents[0].ActivityAfterArrival=EAftershockCitizenActivity::AtWork;Crowd->Agents[0].bRouteReachesDestination=true;
	bool bMotionSafe=true,bMoved=false;FVector LastPosition=CornerStart;
	for(int32 Step=0;Step<160;++Step)
	{
		Crowd->UpdateCrowdPositions();const auto& Agent=Crowd->Agents[0];
		bMotionSafe&=Roads->IsPublicWalkingCell(Terrain->WorldToGrid(Agent.CurrentLocation))&&Crowd->CanMoveCitizenSegment(*Terrain,LastPosition,Agent.CurrentLocation);
		bMoved|=FVector::DistSquared2D(Agent.CurrentLocation,CornerStart)>100.f;LastPosition=Agent.CurrentLocation;
	}
	Check(TEXT("real_timer_motion_replans_stale_waypoint_without_river_cutting_or_freeze"),bMotionSafe&&bMoved);
	Terrain->Cells[Terrain->CoordToIndex(CornerHazard)]=BeforeCorner;
	Terrain->Cells[Terrain->CoordToIndex(HazardCoord)].OccupyingBuildingID=777;
	Crowd->Agents={AgentAt(Boundary,123),AgentAt(Boundary-FVector(1,0,0),456)};
	Crowd->ResolveCrowdSpacing(*Terrain);
	Check(TEXT("separation_never_projects_into_occupied_building"),Crowd->GetCrowdSpacingStats().RejectedUnsafeCorrections>0&&Crowd->Agents[0].CurrentLocation==Boundary&&Roads->IsPublicWalkingCell(Terrain->WorldToGrid(Crowd->Agents[1].CurrentLocation)));
	Terrain->Cells[Terrain->CoordToIndex(HazardCoord)]=OriginalCell;
	Crowd->RebuildDestinationCaches();Crowd->RoutingRefreshSeconds=0.f;
	Crowd->Agents={AgentAt(Centre,9123),AgentAt(Centre+FVector(64.f,0,0),9456)};
	Crowd->Agents[0].MoveSpeed=160.f;Crowd->Agents[0].TargetLocation=Centre+FVector(600,0,0);Crowd->Agents[0].FinalDestinationLocation=Crowd->Agents[0].TargetLocation;Crowd->Agents[0].bRouteReachesDestination=true;
	Crowd->Agents[1].MoveSpeed=0.f;Crowd->Agents[1].Activity=EAftershockCitizenActivity::AtLeisure;Crowd->Agents[1].ScheduleSecondsRemaining=1000.f;
	Crowd->RebuildCitizenSpatialIndex();const FVector TangentGoal=Crowd->Agents[0].FinalDestinationLocation;
	const bool ForwardBlocked=!Crowd->IsCitizenSpaceClear(Centre,Centre+FVector(12.8f,0,0),0);
	Crowd->UpdateCrowdPositions();
	Check(TEXT("blocked_forecourt_walker_uses_safe_tangent_and_preserves_exact_goal"),ForwardBlocked&&FMath::Abs(Crowd->Agents[0].CurrentLocation.Y-Centre.Y)>1.f
		&&Crowd->Agents[0].FinalDestinationLocation.Equals(TangentGoal,.01f)&&Crowd->CanMoveCitizenSegment(*Terrain,Centre,Crowd->Agents[0].CurrentLocation)
		&&FVector::DistSquared2D(Crowd->Agents[0].CurrentLocation,Crowd->Agents[1].CurrentLocation)>=FMath::Square(63.99f));
	TArray<float> SavedSlope;for(auto& Cell:Terrain->Cells){SavedSlope.Add(Cell.Elevation);Cell.Elevation=Cell.Coords.X*50.f;}
	FVector AsideStart=Centre;AsideStart.Z=SampleAftershockResidentElevation(*Terrain,5.f,5.f)+5.f;
	FVector AsidePoint=AsideStart+FVector(128,0,0);AsidePoint.Z=SampleAftershockResidentElevation(*Terrain,5.f+128.f/Terrain->CellSize,5.f)+5.f;
	const FVector OriginalDoor=AsideStart-FVector(128,0,0);
	Crowd->Agents={AgentAt(AsideStart,9612)};auto& AsideAgent=Crowd->Agents[0];
	AsideAgent.Activity=EAftershockCitizenActivity::Evacuating;AsideAgent.ActivityAfterArrival=EAftershockCitizenActivity::AwaitingShelter;
	AsideAgent.bEmergencyOverflow=true;AsideAgent.MoveSpeed=160.f;AsideAgent.TargetLocation=AsidePoint;AsideAgent.FinalDestinationLocation=OriginalDoor;AsideAgent.bRouteReachesDestination=true;
	Crowd->RebuildDestinationCaches();Crowd->RoutingRefreshSeconds=0.f;Crowd->RebuildCitizenSpatialIndex();
	bool AsideReached=false,AsideGoalPreserved=true;
	for(int32 I=0;I<80;++I)
	{
		Crowd->UpdateCrowdPositions();const auto& A=Crowd->Agents[0];
		AsideReached|=A.Activity==EAftershockCitizenActivity::AwaitingShelter&&A.CurrentLocation.Equals(AsidePoint,.1f);
		AsideGoalPreserved&=A.FinalDestinationLocation.Equals(OriginalDoor,.01f);
	}
	Check(TEXT("sloped_courtesy_waypoint_is_reached_and_waits_outside_without_returning_to_door"),AsideReached&&AsideGoalPreserved
		&&Crowd->Agents[0].Activity==EAftershockCitizenActivity::AwaitingShelter&&Crowd->Agents[0].MoveSpeed==0.f&&Crowd->Agents[0].CurrentLocation.Equals(AsidePoint,.1f));
	for(int32 I=0;I<Terrain->Cells.Num();++I)Terrain->Cells[I].Elevation=SavedSlope[I];
	// Begin from valid separated bodies, then drive the real timer callback
	// through opposing routes. Count every pair here, independent of the bounded
	// runtime diagnostic: zero samples is not accepted as zero intersections.
	for(int32 Count:{500,1500})
	{
		Crowd->Agents.Reset();Crowd->RebuildDestinationCaches();
		for(int32 I=0;I<Count;++I)
		{
			const FVector At(400.f+(I%50)*96.f,400.f+(I/50)*96.f,5.f);
			auto Agent=AgentAt(At,90000+I);Agent.MoveSpeed=160.f;
			Agent.TargetLocation=FVector(400.f+(49-I%50)*96.f,At.Y,5.f);Agent.FinalDestinationLocation=Agent.TargetLocation;
			Agent.ActivityAfterArrival=EAftershockCitizenActivity::AtLeisure;Crowd->Agents.Add(Agent);
		}
		bool NoBodyIntersection=true,SomeActualMotion=false,AllPlayable=true;double MinimumDistance=std::numeric_limits<double>::max();int32 MaxAdmissionChecks=0;
		for(int32 Step=0;Step<40;++Step)
		{
			Crowd->UpdateCrowdPositions();MaxAdmissionChecks=FMath::Max(MaxAdmissionChecks,Crowd->GetCrowdSpacingStats().AdmissionChecks);
			for(int32 I=0;I<Count;++I)
			{
				const auto& Agent=Crowd->Agents[I];SomeActualMotion|=Agent.WalkVelocityCmPerSec.SizeSquared()>1.;
				AllPlayable&=Terrain->IsPlayableCoord(Terrain->WorldToGrid(Agent.CurrentLocation));
				for(int32 J=I+1;J<Count;++J)
				{
					const double Distance=FVector::DistSquared2D(Agent.CurrentLocation,Crowd->Agents[J].CurrentLocation);
					MinimumDistance=FMath::Min(MinimumDistance,FMath::Sqrt(Distance));NoBodyIntersection&=Distance>=FMath::Square(63.99f);
				}
			}
		}
		Check(*FString::Printf(TEXT("reservation_%d_timer_agents_have_zero_exhaustive_64cm_body_overlaps"),Count),NoBodyIntersection&&SomeActualMotion&&AllPlayable&&Crowd->Agents.Num()==Count,
			FString::Printf(TEXT("minimumCentreDistanceCm=%.4f actualAgents=%d steps=40 allPairsPerStep=1 maxAdmissionChecks=%d"),MinimumDistance,Count,MaxAdmissionChecks));
	}
	// An unpowered home stays occupied indoors. A full doorway must never emit
	// the same person on top of an existing outside body, including after reload.
	Crowd->Agents={AgentAt(Centre,711),AgentAt(Centre,722)};
	Crowd->Agents[0].ResidentID=71;Crowd->Agents[0].HomeBuildingID=1;Crowd->Agents[0].Activity=EAftershockCitizenActivity::AtHome;Crowd->Agents[0].MoveSpeed=0.f;Crowd->Agents[0].bHomeOperating=false;
	Crowd->RebuildCitizenInstanceTransforms();auto* AdmissionHost=Crowd->GetOrCreateHostActor();FTransform IndoorPose;
	Check(TEXT("power_outage_does_not_materialize_household_on_frontage"),AdmissionHost&&AdmissionHost->CitizenISM->GetInstanceTransform(0,IndoorPose,true)&&IndoorPose.GetScale3D().IsNearlyZero());
	Crowd->StartCommute(Crowd->Agents[0],Centre+FVector(300,0,0),EAftershockCitizenActivity::AtWork,false);
	Crowd->RebuildCitizenSpatialIndex();
	Check(TEXT("occupied_doorway_keeps_real_departure_inside"),Crowd->Agents[0].bWaitingForOutdoorSpace&&!Crowd->TryAdmitOutdoorDeparture(Crowd->Agents[0]));
	Crowd->Agents[1].CurrentLocation+=FVector(200,0,0);Crowd->RebuildCitizenSpatialIndex();
	Check(TEXT("same_resident_emerges_when_physical_door_slot_is_free"),Crowd->TryAdmitOutdoorDeparture(Crowd->Agents[0])&&!Crowd->Agents[0].bWaitingForOutdoorSpace&&Crowd->Agents[0].ResidentID==71&&Crowd->Agents.Num()==2);
	// The source is the building currently occupied, not whichever shelter or
	// service the next warning/needs refresh chooses as a future destination.
	auto& Retargeted=Crowd->Agents[0];Retargeted.Activity=EAftershockCitizenActivity::AtShelter;Retargeted.MoveSpeed=0;Retargeted.bEmergencyPlaceReserved=true;Retargeted.EmergencyBuildingID=7;Retargeted.ShelterLocation=Centre;
	Crowd->StartCommute(Retargeted,Centre+FVector(1000,0,0),EAftershockCitizenActivity::AtShelter,false);
	Retargeted.EmergencyBuildingID=8;Retargeted.ShelterLocation=Centre+FVector(1000,0,0);Retargeted.HomeLocation=Retargeted.ShelterLocation;Retargeted.WorkLocation=Retargeted.ShelterLocation;
	FText DepartureError;
	Check(TEXT("shelter_retarget_preserves_actual_deferred_indoor_source"),Retargeted.bWaitingForOutdoorSpace&&Retargeted.OutdoorDepartureBuildingID==7&&Retargeted.OutdoorDepartureLocation==Centre
		&&UCrowdRenderSubsystem::NormalizeDeferredDepartureAnchor(Retargeted,[&](int32 ID,const FVector& At){return ID==7&&At==Centre;},DepartureError));
	Crowd->RebuildCitizenInstanceTransforms();FTransform RetargetedPose;
	Check(TEXT("shelter_retarget_does_not_materialize_waiting_person_before_admission"),AdmissionHost->CitizenISM->GetInstanceTransform(0,RetargetedPose,true)&&RetargetedPose.GetScale3D().IsNearlyZero());
	Retargeted.bWaitingForOutdoorSpace=false;Retargeted.OutdoorDepartureBuildingID=INDEX_NONE;Retargeted.OutdoorDepartureLocation=FVector::ZeroVector;Retargeted.Activity=EAftershockCitizenActivity::AtWork;Retargeted.MoveSpeed=0;Retargeted.WorkBuildingID=9;Retargeted.WorkLocation=Centre;
	Crowd->StartCommute(Retargeted,Centre+FVector(1000,0,0),EAftershockCitizenActivity::AtFoodService,false);Retargeted.WorkBuildingID=10;Retargeted.WorkLocation=Centre+FVector(1000,0,0);
	Check(TEXT("work_reassignment_preserves_departure_building_instead_of_new_job"),Retargeted.bWaitingForOutdoorSpace&&Retargeted.OutdoorDepartureBuildingID==9
		&&UCrowdRenderSubsystem::NormalizeDeferredDepartureAnchor(Retargeted,[&](int32 ID,const FVector& At){return ID==9&&At==Centre;},DepartureError));

	// This eight-state roster is an isolated visual fixture, not eight newly
	// authored canonical residents. Canonical resident setup follows below.
	Crowd->Agents.Reset();
	for(int32 I=0;I<8;++I){auto Agent=AgentAt(Centre+FVector(I*100.f,0,0),1000+I);Agent.ResidentID=100+I;Agent.MoveSpeed=0.f;Crowd->Agents.Add(Agent);}
	Crowd->Agents[0].Activity=EAftershockCitizenActivity::AtHome;Crowd->Agents[0].HomeBuildingID=1;Crowd->Agents[0].bHomeOperating=true;
	Crowd->Agents[1].Activity=EAftershockCitizenActivity::AtWork;Crowd->Agents[1].WorkBuildingID=2;Crowd->Agents[1].bWorkOperating=true;
	Crowd->Agents[2].Activity=EAftershockCitizenActivity::AtFoodService;Crowd->Agents[2].FoodBuildingID=3;
	Crowd->Agents[3].Activity=EAftershockCitizenActivity::AtHealthService;Crowd->Agents[3].HealthBuildingID=4;
	Crowd->Agents[4].Activity=EAftershockCitizenActivity::AtShelter;Crowd->Agents[4].bEmergencyPlaceReserved=true;
	Crowd->Agents[5].Activity=EAftershockCitizenActivity::RidingTransport;Crowd->Agents[5].bInsideVehicle=true;
	Crowd->Agents[6].Activity=EAftershockCitizenActivity::AtLeisure;
	Crowd->Agents[7].Activity=EAftershockCitizenActivity::Commuting;Crowd->Agents[7].MoveSpeed=100.f;
	Crowd->RebuildCitizenInstanceTransforms();auto* Host=Crowd->GetOrCreateHostActor();
	Check(TEXT("single_zero_tick_host_ism_keeps_eight_state_roster"),Host&&Host->CitizenISM&&!Host->CanEverTick()&&Crowd->Agents.Num()==8&&Host->CitizenISM->GetInstanceCount()==8);
	bool bIndoorHidden=Host&&Host->CitizenISM,bOutdoorVisible=bIndoorHidden;
	if(Host&&Host->CitizenISM)
	{
		for(int32 I=0;I<8;++I)
		{
			FTransform Transform;const bool bRead=Host->CitizenISM->GetInstanceTransform(I,Transform,true);
			if(I<6)bIndoorHidden&=bRead&&Transform.GetScale3D().IsNearlyZero();
			else bOutdoorVisible&=bRead&&Transform.GetScale3D().Z>0.f;
		}
	}
	Check(TEXT("indoor_and_vehicle_states_have_zero_body_scale"),bIndoorHidden);
	Check(TEXT("outdoor_leisure_and_walking_keep_body_scale"),bOutdoorVisible);
	if(Host&&Host->CitizenISM&&Host->CitizenISM->GetStaticMesh())
	{
		const UStaticMesh* Mesh=Host->CitizenISM->GetStaticMesh();const FBox Extended=Mesh->GetBoundingBox();
		const FBox Body(Extended.Min+Mesh->GetNegativeBoundsExtension(),Extended.Max-Mesh->GetPositiveBoundsExtension());
		FTransform Outdoor;const bool ReadBody=Host->CitizenISM->GetInstanceTransform(6,Outdoor,true);
		Check(TEXT("wpo_culling_extension_does_not_shrink_175cm_body"),ReadBody&&FMath::IsNearlyEqual(Body.GetSize().Z*Outdoor.GetScale3D().Z,175.,.1));
		Check(TEXT("wpo_culling_extension_does_not_lift_body_feet"),ReadBody&&FMath::IsNearlyEqual(Outdoor.TransformPosition(FVector(0,0,Body.Min.Z)).Z,Crowd->Agents[6].CurrentLocation.Z,.1));
	}
	Check(TEXT("citizen_gait_has_seven_shared_instance_floats"),Host&&Host->CitizenISM&&Host->CitizenISM->NumCustomDataFloats==7);
	if(Host&&Host->CitizenISM&&Host->CitizenISM->NumCustomDataFloats==7)
	{
		const auto& Data=Host->CitizenISM->PerInstanceSMCustomData;TSet<uint32> WardrobeColors;
		for(int32 I=0;I<8&&Data.IsValidIndex(I*7+2);++I)
			WardrobeColors.Add((FMath::RoundToInt(Data[I*7]*255)<<16)|(FMath::RoundToInt(Data[I*7+1]*255)<<8)|FMath::RoundToInt(Data[I*7+2]*255));
		Check(TEXT("citizen_clothing_has_individual_instance_color_variety"),WardrobeColors.Num()>=3);
		const float Red=Data[6*7],Green=Data[6*7+1],Blue=Data[6*7+2];
		const auto Previous= Crowd->Agents[6].Activity;Crowd->Agents[6].Activity=EAftershockCitizenActivity::Evacuating;
		Crowd->SetCitizenInstanceColor(Crowd->Agents[6],false);
		Check(TEXT("changing_activity_preserves_person_clothing_color"),Data[6*7]==Red&&Data[6*7+1]==Green&&Data[6*7+2]==Blue);
		Crowd->Agents[6].Activity=Previous;
	}
	Crowd->ResolveCrowdSpacing(*Terrain);const auto VisibilityStats=Crowd->GetCrowdSpacingStats();
	Check(TEXT("indoor_vehicle_outdoor_stats_preserve_roster"),VisibilityStats.IndoorAgents==5&&VisibilityStats.VehiclePassengers==1&&VisibilityStats.OutdoorAgents==2&&Crowd->Agents.Num()==8);
	Check(TEXT("visual_spacing_does_not_create_canonical_residents_or_change_economy"),!Life->IsCityInitialized()&&Life->GetState().LivingPopulation==0&&Economy->TreasuryTL==InitialTreasury&&Economy->PublicSatisfaction==InitialSatisfaction);

	Crowd->Agents.Reset();
	UStaticMesh* Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
	const auto Place=[&](EBuildingType Type,FIntPoint Coord,int32 Residents)
	{
		auto* Data=NewObject<UBuildingData>(Buildings);Data->Type=Type;Data->Residents=Residents;Data->Workers=0;Data->MeshIntact=Cube;
		int32 ID=INDEX_NONE;const bool bPlaced=Buildings->PlaceBuilding(Coord,Data,EFoundationQuality::Medium,1,ID);
		Check(*FString::Printf(TEXT("snapshot_fixture_building_%d_placed"),static_cast<int32>(Type)),bPlaced&&ID!=INDEX_NONE);
		return ID;
	};
	const int32 HomeID=Place(EBuildingType::House,FIntPoint(2,2),4);
	const int32 FoodID=Place(EBuildingType::Restaurant,FIntPoint(6,2),0);
	const int32 HealthID=Place(EBuildingType::Hospital,FIntPoint(7,2),0);
	Life->InitializeForCity();Crowd->SynchronizeCanonicalPopulation();
	const bool bRealRoster=HomeID!=INDEX_NONE&&FoodID!=INDEX_NONE&&HealthID!=INDEX_NONE&&Life->GetState().LivingPopulation==4&&Crowd->Agents.Num()==4;
	Check(TEXT("snapshot_uses_four_real_canonical_resident_ids"),bRealRoster);
	if(!bRealRoster)return Finish();
	{
		FAftershockCityLifeSaveState BeforeLife,ExpectedLife,AfterLife;
		FAftershockEconomySaveState BeforeEconomy,AfterEconomy;
		FAftershockTerrainSaveState BeforeTerrain,AfterTerrain;
		FAftershockCrowdSaveState BeforeCrowd;Crowd->CaptureSaveState(BeforeCrowd);
		Life->CaptureSaveState(BeforeLife);Economy->CaptureSaveState(BeforeEconomy);Terrain->CaptureSaveState(BeforeTerrain);
		const auto BeforeWork=Crowd->GetRosterRefreshWork();TArray<int32> ExplicitDeaths;
		FAftershockDisasterVisualSnapshot Notice;Notice.Type=EDisasterType::Fire;Notice.Revision=1;Notice.DurationSeconds=24.f;
		bool Deferred=false,Nested=false;
		{
			FAftershockScopedCrowdRefresh Outer(Crowd);
			Life->RefreshCity();Crowd->SynchronizeCanonicalPopulation();
			{
				FAftershockScopedCrowdRefresh Inner(Crowd);
				// Exercise the actual production snapshot handler, without applying
				// a second hazard or fabricating a numerical disaster result.
				Crowd->HandleDisasterVisualSnapshot(Notice);
				ExplicitDeaths=Life->ApplyCasualties(HomeID,1,false);Life->RefreshCity();
				Crowd->SynchronizeCanonicalPopulation();Crowd->ReplanEmergencyEvacuation();
				const auto During=Crowd->GetRosterRefreshWork();
				Deferred=During.Synchronizations==BeforeWork.Synchronizations&&During.EmergencyPlans==BeforeWork.EmergencyPlans;
			}
			const auto AfterInner=Crowd->GetRosterRefreshWork();
			Nested=AfterInner.Synchronizations==BeforeWork.Synchronizations&&AfterInner.EmergencyPlans==BeforeWork.EmergencyPlans;
			Crowd->HandleDisasterVisualSnapshot(Notice);Life->RefreshCity();
			Life->CaptureSaveState(ExpectedLife);
		}
		const auto Flushed=Crowd->GetRosterRefreshWork();
		Check(TEXT("scoped_refresh_defers_real_life_roster_and_snapshot_plan_requests"),Deferred,TEXT("Production Life broadcasts, canonical-population entry point and snapshot handler execute; no work counters are forged."));
		Check(TEXT("nested_crowd_refresh_scope_does_not_flush_outer_requests"),Nested);
		Check(TEXT("outer_crowd_refresh_exit_runs_one_final_canonical_roster"),Flushed.Synchronizations==BeforeWork.Synchronizations+1);
		Check(TEXT("outer_crowd_refresh_exit_runs_one_final_emergency_plan"),Flushed.EmergencyPlans==BeforeWork.EmergencyPlans+1);
		TSet<int32> AliveIDs;for(const auto& Resident:Life->GetResidents())if(Resident.bAlive)AliveIDs.Add(Resident.ResidentID);
		TSet<int32> DrawnIDs;bool RosterValid=ExplicitDeaths.Num()==1&&Life->GetState().LivingPopulation==3&&Crowd->Agents.Num()==3;
		for(const auto& Agent:Crowd->Agents){RosterValid&=AliveIDs.Contains(Agent.ResidentID)&&!DrawnIDs.Contains(Agent.ResidentID)&&!ExplicitDeaths.Contains(Agent.ResidentID);DrawnIDs.Add(Agent.ResidentID);}
		Check(TEXT("coalesced_roster_contains_exact_final_living_resident_ids"),RosterValid&&DrawnIDs.Num()==AliveIDs.Num());
		const auto Plan=Crowd->GetCitizenPlanningStats();
		Check(TEXT("coalesced_final_evacuation_has_only_safe_remaining_route_cells"),Crowd->IsPanicModeActive()&&Plan.Representatives==3&&Plan.UnsafeRemainingRouteSteps==0);
		Life->CaptureSaveState(AfterLife);Economy->CaptureSaveState(AfterEconomy);Terrain->CaptureSaveState(AfterTerrain);
		Check(TEXT("presentation_refresh_batch_preserves_exact_post_mutation_life_state"),FAftershockCityLifeSaveState::StaticStruct()->CompareScriptStruct(&ExpectedLife,&AfterLife,0),TEXT("The single death is an explicit fixture input; the batch itself never changes canonical population, needs, jobs or outcomes."));
		Check(TEXT("presentation_refresh_batch_preserves_all_money_and_terrain_fields"),FAftershockEconomySaveState::StaticStruct()->CompareScriptStruct(&BeforeEconomy,&AfterEconomy,0)&&FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&BeforeTerrain,&AfterTerrain,0));
		FAftershockCrowdSaveState FinalCrowd;Crowd->CaptureSaveState(FinalCrowd);FText BatchError;
		Check(TEXT("coalesced_final_roster_and_routes_pass_real_snapshot_restore_validation"),Crowd->RestoreSaveState(FinalCrowd,BatchError),BatchError.ToString());
		const auto BeforeEmpty=Crowd->GetRosterRefreshWork();
		{FAftershockScopedCrowdRefresh Empty(Crowd);}
		const auto AfterEmpty=Crowd->GetRosterRefreshWork();
		Check(TEXT("empty_refresh_batch_does_not_invent_roster_or_plan_work"),BeforeEmpty.Synchronizations==AfterEmpty.Synchronizations&&BeforeEmpty.EmergencyPlans==AfterEmpty.EmergencyPlans);
		const auto BeforeOutside=Crowd->GetRosterRefreshWork();Life->RefreshCity();const auto AfterOutside=Crowd->GetRosterRefreshWork();
		Check(TEXT("outside_scope_panic_life_notice_keeps_original_synchronous_semantics"),AfterOutside.Synchronizations==BeforeOutside.Synchronizations+1&&AfterOutside.EmergencyPlans==BeforeOutside.EmergencyPlans+1);
		const bool RestoredLife=Life->RestoreSaveState(BeforeLife,BatchError);
		Check(TEXT("coalescing_fixture_restores_original_life_and_crowd_for_following_checks"),RestoredLife&&Crowd->RestoreSaveState(BeforeCrowd,BatchError),BatchError.ToString());
	}
	FAftershockCrowdSaveState Original;Crowd->CaptureSaveState(Original);
	Original.SimulationSeed=9181;auto& Sample=Original.Agents[0];
	Sample.FoodBuildingID=FoodID;Sample.HealthBuildingID=HealthID;
	Sample.FoodLocation=Terrain->GridToWorld(FIntPoint(6,3))+FVector(0,0,5.f);
	Sample.HealthLocation=Terrain->GridToWorld(FIntPoint(7,3))+FVector(0,0,5.f);
	Sample.TransportFinalDestination=Terrain->GridToWorld(FIntPoint(8,3))+FVector(0,0,5.f);
	Sample.FoodVisitCooldown=17.25f;Sample.HealthVisitCooldown=29.5f;
	Sample.Hunger01=.35f;Sample.Health01=.72f;Sample.Happiness01=.61f;
	Sample.TransportArrivalActivity=EAftershockCitizenActivity::AtFoodService;
	Sample.WalkVelocityCmPerSec=FVector2D(12.5f,-3.75f);
	Sample.WalkingPhaseRadians=2.35f;
	Sample.bWaitingForOutdoorSpace=true;
	Sample.OutdoorDepartureBuildingID=HomeID;Sample.OutdoorDepartureLocation=Sample.CurrentLocation;
	const TArray<uint8> Bytes=SerializeCrowdState(Original);
	FAftershockCrowdSaveState Decoded;
	FMemoryReader Reader(Bytes,true);FObjectAndNameAsStringProxyArchive ReadArchive(Reader,false);
	FAftershockCrowdSaveState::StaticStruct()->SerializeItem(ReadArchive,&Decoded,nullptr);
	const bool bDecoded=!Reader.IsError()&&Decoded.Agents.Num()==Original.Agents.Num();
	Check(TEXT("reflected_save_struct_serializes_agent_roster"),bDecoded&&Decoded.SimulationSeed==Original.SimulationSeed&&!Bytes.IsEmpty());
	if(!bDecoded)return Finish();
	const auto& Read=Decoded.Agents[0];
	Check(TEXT("food_health_destinations_and_service_ids_are_reflected"),Read.FoodLocation==Sample.FoodLocation&&Read.HealthLocation==Sample.HealthLocation&&Read.FoodBuildingID==FoodID&&Read.HealthBuildingID==HealthID);
	Check(TEXT("visit_cooldowns_and_needs_survive_serializeitem"),Read.FoodVisitCooldown==Sample.FoodVisitCooldown&&Read.HealthVisitCooldown==Sample.HealthVisitCooldown&&Read.Hunger01==Sample.Hunger01&&Read.Health01==Sample.Health01&&Read.Happiness01==Sample.Happiness01);
	Check(TEXT("transport_final_destination_arrival_and_velocity_are_reflected"),Read.TransportFinalDestination==Sample.TransportFinalDestination&&Read.TransportArrivalActivity==Sample.TransportArrivalActivity&&Read.WalkVelocityCmPerSec==Sample.WalkVelocityCmPerSec);
	Check(TEXT("distance_integrated_gait_phase_survives_reflected_snapshot"),Read.WalkingPhaseRadians==Sample.WalkingPhaseRadians);
	Check(TEXT("deferred_actual_indoor_departure_survives_reflected_snapshot"),Read.bWaitingForOutdoorSpace==Sample.bWaitingForOutdoorSpace);
	Check(TEXT("actual_departure_building_and_location_survive_reflected_snapshot"),Read.OutdoorDepartureBuildingID==HomeID&&Read.OutdoorDepartureLocation==Sample.CurrentLocation);
	FText RestoreError;
	Check(TEXT("valid_reflected_snapshot_restores_without_population_or_money_changes"),Crowd->RestoreSaveState(Decoded,RestoreError)&&Crowd->Agents.Num()==4&&Life->GetState().LivingPopulation==4&&Economy->TreasuryTL==InitialTreasury);
	FAftershockCrowdSaveState Restored;Crowd->CaptureSaveState(Restored);
	Check(TEXT("capture_after_restore_preserves_reflected_agent_payload"),SerializeCrowdState(Restored)==SerializeCrowdState(Decoded));
	auto LegacyDeferred=Decoded;LegacyDeferred.Agents[0].OutdoorDepartureBuildingID=INDEX_NONE;LegacyDeferred.Agents[0].OutdoorDepartureLocation=FVector::ZeroVector;
	const bool LegacyDepartureLoaded=Crowd->RestoreSaveState(LegacyDeferred,RestoreError);FAftershockCrowdSaveState MigratedDeparture;Crowd->CaptureSaveState(MigratedDeparture);
	Check(TEXT("legacy_empty_departure_fields_migrate_from_known_matching_indoor_anchor"),LegacyDepartureLoaded&&MigratedDeparture.Agents[0].OutdoorDepartureBuildingID==HomeID&&MigratedDeparture.Agents[0].OutdoorDepartureLocation==Sample.CurrentLocation&&MigratedDeparture.Agents[0].ResidentID==Sample.ResidentID&&Life->GetState().LivingPopulation==4);
	const float NaN=std::numeric_limits<float>::quiet_NaN(),Infinity=std::numeric_limits<float>::infinity();
	const auto Reject=[&](const TCHAR* Name,TFunctionRef<void(FAftershockCrowdSaveState&)> Mutate)
	{
		FAftershockCrowdSaveState Before;Crowd->CaptureSaveState(Before);auto Bad=Before;Mutate(Bad);
		FText Error;const bool bRejected=!Crowd->RestoreSaveState(Bad,Error);
		FAftershockCrowdSaveState After;Crowd->CaptureSaveState(After);
		Check(Name,bRejected&&!Error.IsEmpty()&&SerializeCrowdState(Before)==SerializeCrowdState(After),TEXT("Invalid candidate must not partially replace any live crowd field."));
	};
	Reject(TEXT("reject_nan_current_without_mutation"),[&](auto& S){S.Agents[0].CurrentLocation.X=NaN;});
	Reject(TEXT("reject_nan_target_without_mutation"),[&](auto& S){S.Agents[0].TargetLocation.Y=NaN;});
	Reject(TEXT("reject_nan_final_without_mutation"),[&](auto& S){S.Agents[0].FinalDestinationLocation.Z=NaN;});
	Reject(TEXT("reject_nan_food_location_without_mutation"),[&](auto& S){S.Agents[0].FoodLocation.X=NaN;});
	Reject(TEXT("reject_nan_health_location_without_mutation"),[&](auto& S){S.Agents[0].HealthLocation.Y=NaN;});
	Reject(TEXT("reject_nan_transport_final_without_mutation"),[&](auto& S){S.Agents[0].TransportFinalDestination.X=NaN;});
	Reject(TEXT("reject_infinite_move_speed_without_mutation"),[&](auto& S){S.Agents[0].MoveSpeed=Infinity;});
	Reject(TEXT("reject_nan_walking_phase_without_mutation"),[&](auto& S){S.Agents[0].WalkingPhaseRadians=NaN;});
	Reject(TEXT("reject_out_of_range_walking_phase_without_mutation"),[](auto& S){S.Agents[0].WalkingPhaseRadians=2.f*PI+1.f;});
	Reject(TEXT("reject_negative_move_speed_without_mutation"),[](auto& S){S.Agents[0].MoveSpeed=-1.f;});
	Reject(TEXT("reject_nan_food_cooldown_without_mutation"),[&](auto& S){S.Agents[0].FoodVisitCooldown=NaN;});
	Reject(TEXT("reject_negative_health_cooldown_without_mutation"),[](auto& S){S.Agents[0].HealthVisitCooldown=-1.f;});
	Reject(TEXT("reject_infinite_health_cooldown_without_mutation"),[&](auto& S){S.Agents[0].HealthVisitCooldown=Infinity;});
	Reject(TEXT("reject_hunger_outside_unit_range_without_mutation"),[](auto& S){S.Agents[0].Hunger01=1.25f;});
	Reject(TEXT("reject_negative_health_without_mutation"),[](auto& S){S.Agents[0].Health01=-.1f;});
	Reject(TEXT("reject_nan_happiness_without_mutation"),[&](auto& S){S.Agents[0].Happiness01=NaN;});
	Reject(TEXT("reject_invalid_activity_without_mutation"),[](auto& S){S.Agents[0].Activity=static_cast<EAftershockCitizenActivity>(255);});
	Reject(TEXT("reject_invalid_arrival_activity_without_mutation"),[](auto& S){S.Agents[0].ActivityAfterArrival=static_cast<EAftershockCitizenActivity>(255);});
	Reject(TEXT("reject_invalid_transport_arrival_without_mutation"),[](auto& S){S.Agents[0].TransportArrivalActivity=static_cast<EAftershockCitizenActivity>(255);});
	Reject(TEXT("reject_deferred_departure_away_from_actual_indoor_anchor"),[&](auto& S){S.Agents[0].bWaitingForOutdoorSpace=true;S.Agents[0].CurrentLocation=Terrain->GridToWorld(FIntPoint(10,10))+FVector(0,0,5.f);});
	Reject(TEXT("reject_simultaneous_indoor_and_vehicle_body_hiding"),[](auto& S){S.Agents[0].bWaitingForOutdoorSpace=true;S.Agents[0].bInsideVehicle=true;S.Agents[0].TransportTripID=123;});
	Reject(TEXT("reject_unknown_explicit_departure_building_without_mutation"),[](auto& S){S.Agents[0].bWaitingForOutdoorSpace=true;S.Agents[0].OutdoorDepartureBuildingID=MAX_int32;});
	Reject(TEXT("reject_nonfinite_explicit_departure_location_without_mutation"),[&](auto& S){S.Agents[0].OutdoorDepartureLocation.X=NaN;});
	Reject(TEXT("reject_duplicate_resident_without_mutation"),[](auto& S){S.Agents[1].ResidentID=S.Agents[0].ResidentID;});
	Reject(TEXT("reject_unknown_resident_without_mutation"),[](auto& S){S.Agents[0].ResidentID=2147483000;});
	Reject(TEXT("reject_roster_population_mismatch_without_mutation"),[](auto& S){S.Agents.RemoveAt(S.Agents.Num()-1);});
	Reject(TEXT("reject_negative_emergency_remaining_without_mutation"),[](auto& S){S.RemainingEmergencySeconds=-1.f;});
	Reject(TEXT("reject_nan_emergency_remaining_without_mutation"),[&](auto& S){S.RemainingEmergencySeconds=NaN;});
	Reject(TEXT("reject_invalid_emergency_type_without_mutation"),[](auto& S){S.EmergencyDisasterType=static_cast<EDisasterType>(255);});
	Reject(TEXT("reject_invalid_route_coord_without_mutation"),[](auto& S){S.Agents[0].RouteGridCoords={FIntPoint(-99,0)};S.Agents[0].NextRouteCoordIndex=0;});
	Reject(TEXT("reject_invalid_route_index_without_mutation"),[](auto& S){S.Agents[0].NextRouteCoordIndex=S.Agents[0].RouteGridCoords.Num()+2;});
	Check(TEXT("snapshot_checks_leave_canonical_population_and_economy_unchanged"),Life->GetState().LivingPopulation==4&&Life->GetResidents().Num()==4&&Economy->TreasuryTL==InitialTreasury&&Economy->PublicSatisfaction==InitialSatisfaction);
	FSpacingAuditWorld WindowFixture;
	WindowFixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("CrowdDrawableWindowFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("draw_window_isolated_world_created"),WindowFixture.World!=nullptr);
	if(WindowFixture.World)
	{
		UWorld* ViewWorld=WindowFixture.World;GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(ViewWorld);
		auto* Grid=ViewWorld->GetSubsystem<UTerrainGridSubsystem>();auto* People=ViewWorld->GetSubsystem<UCrowdRenderSubsystem>();
		Grid->GenerateProceduralTerrain(50,50,500.f,1337);FText WindowError;
		Check(TEXT("draw_window_canonical_negative_chunk_generated"),Grid->EnsureChunkGenerated(FIntPoint(-2,0),WindowError));
		const FIntPoint NearCell(5,5),FarCell(-40,5);
		for(FIntPoint Coord:{NearCell,FarCell}){auto Cell=Grid->GetCell(Coord);Cell.bIsPlayable=true;Cell.bIsRiverbed=false;Cell.bIsOcean=false;Grid->SetCell(Coord,Cell);}
		auto* PC=ViewWorld->SpawnActor<APlayerController>();auto* ViewCamera=ViewWorld->SpawnActor<AAftershockCameraPawn>();
		if(PC&&ViewCamera){ViewWorld->AddController(PC);PC->Possess(ViewCamera);}
		const auto SetFocus=[&](FIntPoint Coord)
		{
			if(!PC||!ViewCamera||ViewWorld->GetFirstPlayerController()!=PC||PC->GetPawn()!=ViewCamera)return false;
			FAftershockCameraSaveState CameraState;ViewCamera->CaptureSaveState(CameraState);
			CameraState.LocalFocus=Grid->GridToWorld(Coord);CameraState.Location=CameraState.LocalFocus;
			return ViewCamera->RestoreSaveState(CameraState,WindowError);
		};
		Check(TEXT("draw_window_native_camera_focus_restored"),SetFocus(NearCell));People->EnsureTerrainVisualBuilt();
		auto* ViewHost=People->GetOrCreateHostActor();FIntRect Installed;
		const FIntRect InitialInstalled=ViewHost->BuiltVisibleCellBounds;
		People->CrowdHostActor=nullptr;
		Check(TEXT("early_no_installed_host_draw_query_is_read_only_requested_fallback"),GetAftershockDrawableCellBounds(*Grid)==GetAftershockVisibleCellBounds(*Grid)&&People->CrowdHostActor==nullptr);
		People->CrowdHostActor=ViewHost;
		ViewHost->BuiltVisibleCellBounds=FIntRect(0,0,0,0);
		Check(TEXT("empty_installed_window_uses_requested_fallback_without_creation"),!People->TryGetInstalledTerrainVisualBounds(Installed)
			&&Installed.IsEmpty()&&GetAftershockDrawableCellBounds(*Grid)==GetAftershockVisibleCellBounds(*Grid));
		ViewHost->BuiltVisibleCellBounds=InitialInstalled;
		People->Agents={AgentAt(Grid->GridToWorld(NearCell)+FVector(0,0,5),1401),AgentAt(Grid->GridToWorld(FarCell)+FVector(0,0,5),1402)};
		for(int32 I=0;I<2;++I){People->Agents[I].ResidentID=1401+I;People->Agents[I].Activity=EAftershockCitizenActivity::AtLeisure;People->Agents[I].MoveSpeed=0;}
		People->RebuildCitizenInstanceTransforms();FAftershockCrowdSaveState BeforeWindow;People->CaptureSaveState(BeforeWindow);
		const auto VisibleSlot=[&](int32 I){FTransform Body;return ViewHost->CitizenISM->GetInstanceTransform(I,Body,true)&&!Body.GetScale3D().IsNearlyZero();};
		Check(TEXT("expanded_draw_window_masks_far_body_only_and_keeps_both_ism_slots"),VisibleSlot(0)&&!VisibleSlot(1)&&ViewHost->CitizenISM->GetInstanceCount()==2&&People->Agents.Num()==2);
		Check(TEXT("draw_window_camera_can_move_to_negative_chunk"),SetFocus(FarCell));
		People->RebuildCitizenInstanceTransforms();
		Check(TEXT("new_requested_window_cannot_show_body_before_ground_install"),!GetAftershockDrawableCellBounds(*Grid).Contains(FarCell)&&!VisibleSlot(0)&&!VisibleSlot(1));
		FAftershockDisasterVisualSnapshot DrawHazard;DrawHazard.Revision=1;DrawHazard.Type=EDisasterType::Volcano;DrawHazard.DurationSeconds=24.f;DrawHazard.EffectiveIntensity=100.f;
		DrawHazard.LocalSeverity.Add(NearCell,100.f);DrawHazard.LocalSeverity.Add(FarCell,100.f);DrawHazard.LavaSeverity=DrawHazard.LocalSeverity;
		auto* HazardView=ViewWorld->SpawnActor<AAftershockDisasterPresentationActor>();
		if(HazardView)HazardView->Tags.AddUnique(TEXT("Aftershock_MeshCpuAudit"));
		const auto LavaSurface=[&]()->UStaticMeshComponent*
		{
			if(!HazardView)return nullptr;TArray<UStaticMeshComponent*> Surfaces;HazardView->GetComponents(Surfaces);
			for(auto* Surface:Surfaces)if(Surface->GetName()==TEXT("CanonicalVolcanoLavaVolume"))return Surface;return nullptr;
		};
		FAftershockTerrainSaveState BeforeLava;Grid->CaptureSaveState(BeforeLava);
		if(HazardView)HazardView->BeginSnapshot(DrawHazard,*Grid);
		Check(TEXT("induced_lava_snapshot_uses_installed_ground_intersection_before_new_ground"),HazardView&&HazardView->GetBuiltDrawableCellBounds()==GetAftershockDrawableCellBounds(*Grid)&&HazardView->GetVolcanoVisualStats().DrawnLavaCells==0&&LavaSurface()&&!LavaSurface()->GetStaticMesh());
		auto* DrawFlood=NewObject<UStaticMeshComponent>(ViewHost);DrawFlood->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/FX/Aftershock/Materials/M_DisasterFloodVolume.M_DisasterFloodVolume")));
		FAftershockFloodSurfaceStats DrawFloodStats;TMap<FIntPoint,float> AllResidentDepths;
		const bool FloodMesh=BuildAftershockFloodSurface(ViewHost,Grid,DrawHazard.LocalSeverity,DrawFlood,&DrawFloodStats,&AllResidentDepths);
		Check(TEXT("pending_ground_hides_flood_mesh_but_keeps_full_near_and_far_depth_maps"),!FloodMesh&&!DrawFlood->GetStaticMesh()
			&&AllResidentDepths.Contains(NearCell)&&AllResidentDepths.Contains(FarCell)&&AllResidentDepths.FindRef(NearCell)>0.f&&AllResidentDepths.FindRef(FarCell)>0.f);
		People->EnsureTerrainVisualBuilt();People->RebuildCitizenInstanceTransforms();
		Check(TEXT("installed_negative_window_shows_same_far_resident_and_masks_old_window"),!VisibleSlot(0)&&VisibleSlot(1));
		if(HazardView)HazardView->RefreshVisibleWindow(DrawHazard,*Grid,12.f);
		float LavaHeight=0,GroundHeight=0;const FVector At=Grid->GridToWorld(FarCell);const auto* ActualLava=LavaSurface();
		const bool LavaAboveGround=SampleRiverMeshHeight(ActualLava,FVector2D(At),LavaHeight)&&Grid->TrySampleWorldElevation(At,GroundHeight)&&LavaHeight-GroundHeight>=13.9f;
		const auto* MeshData=ActualLava&&ActualLava->GetStaticMesh()?ActualLava->GetStaticMesh()->GetRenderData():nullptr;
		Check(TEXT("existing_lava_event_rebuilds_at_same_age_when_ground_is_installed"),HazardView&&HazardView->GetBuiltDrawableCellBounds()==GetAftershockDrawableCellBounds(*Grid)&&HazardView->GetVolcanoVisualStats().DrawnLavaCells==1
			&&HazardView->GetVolcanoVisualStats().LavaTopTriangles==8&&HazardView->GetVolcanoVisualStats().LavaSkirtTriangles>0&&MeshData&&!MeshData->LODResources.IsEmpty()&&MeshData->LODResources[0].IndexBuffer.GetNumIndices()>=24&&LavaAboveGround&&DrawHazard.Revision==1&&DrawHazard.DurationSeconds==24.f);
		FAftershockTerrainSaveState AfterLava;Grid->CaptureSaveState(AfterLava);
		Check(TEXT("three_dimensional_lava_window_rebuild_never_changes_canonical_terrain"),FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&BeforeLava,&AfterLava,0));
		Check(TEXT("draw_window_camera_can_return_without_moving_residents"),SetFocus(NearCell));People->EnsureTerrainVisualBuilt();People->RebuildCitizenInstanceTransforms();
		FAftershockCrowdSaveState AfterWindow;People->CaptureSaveState(AfterWindow);
		int32 Outdoor=0,Overlaps=0,VehicleHits=0;float Minimum=0;People->GetPhysicalClearance(Outdoor,Overlaps,VehicleHits,Minimum);
		Check(TEXT("returning_draw_window_restores_visibility_with_identical_saved_agent_payload"),VisibleSlot(0)&&!VisibleSlot(1)&&SerializeCrowdState(BeforeWindow)==SerializeCrowdState(AfterWindow));
		Check(TEXT("render_window_never_removes_offscreen_simulation_bodies_from_physical_audit"),Outdoor==2&&Overlaps==0&&VehicleHits==0&&People->Agents[0].ResidentID==1401&&People->Agents[1].ResidentID==1402);
	}
	FSpacingAuditWorld RiverFixture;
	RiverFixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("RiverDiagonalMeshFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("river_diagonal_mesh_isolated_world_created"),RiverFixture.World!=nullptr);
	if(RiverFixture.World)
	{
		UWorld* RiverWorld=RiverFixture.World;GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(RiverWorld);
		auto* Grid=RiverWorld->GetSubsystem<UTerrainGridSubsystem>();auto* People=RiverWorld->GetSubsystem<UCrowdRenderSubsystem>();FText RiverError;
		Grid->GenerateProceduralTerrain(125,125,1000.f,1337);
		Check(TEXT("river_diagonal_fixture_uses_expanded_original125_seed1337"),Grid->EnsureChunkGenerated(FIntPoint(5,0),RiverError),RiverError.ToString());
		auto* PC=RiverWorld->SpawnActor<APlayerController>();auto* Camera=RiverWorld->SpawnActor<AAftershockCameraPawn>();
		if(PC&&Camera){RiverWorld->AddController(PC);PC->Possess(Camera);}
		FAftershockCameraSaveState View;if(Camera)Camera->CaptureSaveState(View);View.LocalFocus=Grid->GridToWorld(FIntPoint(51,13));View.Location=View.LocalFocus;
		Check(TEXT("river_diagonal_fixture_actual_camera_selects_seeded_links"),PC&&Camera&&RiverWorld->GetFirstPlayerController()==PC&&Camera->RestoreSaveState(View,RiverError));
		auto* RiverHost=People->GetOrCreateHostActor();RiverHost->Tags.AddUnique(TEXT("Aftershock_MeshCpuAudit"));RiverHost->BuiltTerrainRevision=MAX_uint64;
		FAftershockTerrainSaveState BeforeRiver;Grid->CaptureSaveState(BeforeRiver);People->EnsureTerrainVisualBuilt();
		const FIntRect RiverWindow=GetAftershockDrawableCellBounds(*Grid);
		const auto RowCentre=[&](int32 Y)
		{
			int32 First=MAX_int32,Last=MIN_int32;
			for(int32 X=RiverWindow.Min.X;X<RiverWindow.Max.X;++X){FGridCell Cell;if(Grid->TryGetCell(FIntPoint(X,Y),Cell)&&Cell.bIsRiverbed&&Cell.bIsPlayable){First=FMath::Min(First,X);Last=FMath::Max(Last,X);}}
			return First!=MAX_int32?(First+Last+1)*.5f:MAX_flt;
		};
		int32 ConnectedLinks=0,ClearanceSamples=0,OccludedSamples=0;float MinimumClearance=MAX_flt;
		for(int32 Y=14;Y<=24;++Y)
		{
			if(RiverMeshSpansRowCentres(RiverHost->RiverSurface,Y-1,Y,Grid->CellSize,Grid->GridOrigin.Y))++ConnectedLinks;
			const float A=RowCentre(Y-1),B=RowCentre(Y);if(A==MAX_flt||B==MAX_flt)continue;
			const FVector2D Point(Grid->GridOrigin.X+(A+B)*.5f*Grid->CellSize,Grid->GridOrigin.Y+Y*Grid->CellSize);float WaterHeight=0,GroundHeight=0;
			if(SampleRiverMeshHeight(RiverHost->RiverSurface,Point,WaterHeight)&&SampleRiverMeshHeight(RiverHost->TerrainSurface,Point,GroundHeight))
			{const float Clearance=WaterHeight-GroundHeight;++ClearanceSamples;MinimumClearance=FMath::Min(MinimumClearance,Clearance);if(Clearance<0.f)++OccludedSamples;}
		}
		Check(TEXT("actual_seeded_river_mesh_connects_all_eleven_diagonal_row_links"),ConnectedLinks==11,FString::Printf(TEXT("actualMeshLinks=%d expected=11 rows=13..24"),ConnectedLinks));
		Check(TEXT("actual_seeded_river_connectors_clear_rendered_ground_by_unchanged_twelve_cm_water_offset"),ClearanceSamples==11&&OccludedSamples==0&&MinimumClearance>=11.9f,
			FString::Printf(TEXT("sampled=%d minimumClearanceCm=%.3f occludedSamples=%d visuallyClear=%d"),ClearanceSamples,MinimumClearance,OccludedSamples,ClearanceSamples==11&&OccludedSamples==0));
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_RIVER_CONNECTOR_CLEARANCE links=%d samples=%d occludedSamples=%d minimumClearanceCm=%.3f visualContinuityComplete=%d canonicalElevationsChanged=0"),ConnectedLinks,ClearanceSamples,OccludedSamples,MinimumClearance,ClearanceSamples==11&&OccludedSamples==0);
		FAftershockTerrainSaveState AfterRiver;Grid->CaptureSaveState(AfterRiver);
		Check(TEXT("river_render_rebuild_preserves_all_canonical_cells_elevations_and_hydrology"),FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&BeforeRiver,&AfterRiver,0));
		bool SeededCentresExact=true;for(const auto& Cell:Grid->Cells)SeededCentresExact&=SampleAftershockResidentElevation(*Grid,Cell.Coords.X,Cell.Coords.Y)==Cell.Elevation;
		Check(TEXT("river_aware_interpolation_preserves_every_seeded_canonical_cell_centre"),SeededCentresExact);
		// An explicit flat fixture distinguishes a genuine diagonal from a missing
		// river row or a whole intervening non-river cell. Only fixture data changes.
		Check(TEXT("river_diagonal_fixture_contains_real_negative_coordinates"),Grid->EnsureChunkGenerated(FIntPoint(-1,0),RiverError),RiverError.ToString());
		View.LocalFocus=Grid->GridToWorld(FIntPoint(-8,12));View.Location=View.LocalFocus;
		Check(TEXT("river_negative_diagonal_mesh_uses_actual_requested_camera_window"),Camera&&Camera->RestoreSaveState(View,RiverError));
		for(auto Cell:Grid->Cells){Cell.bIsRiverbed=false;Cell.Elevation=0.f;Cell.bIsPlayable=true;Grid->SetCell(Cell.Coords,Cell);}
		for(const FIntPoint Coord:{FIntPoint(10,5),FIntPoint(11,5),FIntPoint(12,6),FIntPoint(13,6),FIntPoint(12,8),FIntPoint(13,8),FIntPoint(15,9),FIntPoint(16,9)})
		{auto Cell=Grid->GetCell(Coord);Cell.bIsRiverbed=true;Grid->SetCell(Coord,Cell);}
		const FIntPoint QuadOrigins[]={FIntPoint(-12,12),FIntPoint(-8,12),FIntPoint(-4,12)};
		const float QuadHeights[][4]={{8.f,140.f,180.f,28.f},{160.f,18.f,38.f,200.f},{8.f,92.f,54.f,110.f}};
		const FIntPoint QuadOffsets[]={FIntPoint(0,0),FIntPoint(1,0),FIntPoint(0,1),FIntPoint(1,1)};
		for(int32 Q=0;Q<3;++Q)for(int32 I=0;I<4;++I)
		{const FIntPoint Coord=QuadOrigins[Q]+QuadOffsets[I];auto Cell=Grid->GetCell(Coord);Cell.Elevation=QuadHeights[Q][I];Cell.bIsRiverbed=Q==0?(I==0||I==3):Q==1?(I==1||I==2):false;Grid->SetCell(Coord,Cell);}
		FAftershockTerrainSaveState BeforeFlat;Grid->CaptureSaveState(BeforeFlat);People->EnsureTerrainVisualBuilt();float FlatWater=0,FlatGround=0;
		const FVector2D FlatPoint(Grid->GridOrigin.X+12.f*Grid->CellSize,Grid->GridOrigin.Y+6.f*Grid->CellSize);
		Check(TEXT("diagonal_river_connector_is_real_mesh_above_flat_canonical_ground"),RiverMeshSpansRowCentres(RiverHost->RiverSurface,5,6,Grid->CellSize,Grid->GridOrigin.Y)
			&&SampleRiverMeshHeight(RiverHost->RiverSurface,FlatPoint,FlatWater)&&SampleRiverMeshHeight(RiverHost->TerrainSurface,FlatPoint,FlatGround)&&FlatWater>=FlatGround+11.9f);
		Check(TEXT("river_mesh_never_bridges_an_absent_row"),!RiverMeshSpansRowCentres(RiverHost->RiverSurface,6,8,Grid->CellSize,Grid->GridOrigin.Y));
		Check(TEXT("river_mesh_never_jumps_over_an_intervening_nonriver_cell"),!RiverMeshSpansRowCentres(RiverHost->RiverSurface,8,9,Grid->CellSize,Grid->GridOrigin.Y));
		bool BothDiagonalsMatch=true,EdgesContinuous=true;
		for(int32 Q=0;Q<2;++Q)
		{
			const FIntPoint O=QuadOrigins[Q];const float Expected=(QuadHeights[Q][Q==0?0:1]+QuadHeights[Q][Q==0?3:2])*.5f;
			float ActualMeshHeight=0;const FVector2D Midpoint(Grid->GridOrigin.X+(O.X+1.f)*Grid->CellSize,Grid->GridOrigin.Y+(O.Y+1.f)*Grid->CellSize);
			BothDiagonalsMatch&=FMath::IsNearlyEqual(SampleAftershockResidentElevation(*Grid,O.X+.5f,O.Y+.5f),Expected,.001f)
				&&SampleRiverMeshHeight(RiverHost->TerrainSurface,Midpoint,ActualMeshHeight)&&FMath::IsNearlyEqual(ActualMeshHeight,Expected,.001f);
			for(float T:{0.f,.25f,.5f,.75f,1.f})for(int32 Edge=0;Edge<4;++Edge)
			{
				const bool Vertical=Edge>=2;const float X=O.X+(Vertical?(Edge==3?1.f:0.f):T),Y=O.Y+(Vertical?T:(Edge==1?1.f:0.f));
				const int32 A=Edge==0?0:Edge==1?2:Edge==2?0:1,B=Edge==0?1:Edge==1?3:Edge==2?2:3;
				const float ExpectedEdge=FMath::Lerp(QuadHeights[Q][A],QuadHeights[Q][B],T);
				EdgesContinuous&=FMath::IsNearlyEqual(SampleAftershockResidentElevation(*Grid,X,Y),ExpectedEdge,.001f)
					&&FMath::Abs(SampleAftershockResidentElevation(*Grid,X+(Vertical?.00001f:0.f),Y+(Vertical?0.f:.00001f))-ExpectedEdge)<.01f
					&&FMath::Abs(SampleAftershockResidentElevation(*Grid,X-(Vertical?.00001f:0.f),Y-(Vertical?0.f:.00001f))-ExpectedEdge)<.01f;
			}
		}
		Check(TEXT("both_opposite_river_diagonals_match_actual_ground_mesh_at_negative_coordinates"),BothDiagonalsMatch);
		Check(TEXT("river_aware_triangulation_keeps_all_quad_edges_and_neighbour_limits_continuous"),EdgesContinuous);
		const float Ordinary=SampleAftershockResidentElevation(*Grid,QuadOrigins[2].X+.31f,QuadOrigins[2].Y+.69f);
		const float ActualX=(QuadOrigins[2].X+.31f)-QuadOrigins[2].X,ActualY=(QuadOrigins[2].Y+.69f)-QuadOrigins[2].Y;
		Check(TEXT("ordinary_nonriver_quad_keeps_previous_bilinear_arithmetic_exactly"),Ordinary==FMath::Lerp(FMath::Lerp(8.f,92.f,ActualX),FMath::Lerp(54.f,110.f,ActualX),ActualY));
		bool AllCentresExact=true;for(const auto& Cell:Grid->Cells)AllCentresExact&=SampleAftershockResidentElevation(*Grid,Cell.Coords.X,Cell.Coords.Y)==Cell.Elevation;
		Check(TEXT("both_diagonal_patterns_keep_all_positive_and_negative_canonical_centre_heights"),AllCentresExact);
		FAftershockTerrainSaveState AfterFlat;Grid->CaptureSaveState(AfterFlat);
		Check(TEXT("flat_diagonal_render_fixture_does_not_write_canonical_terrain"),FAftershockTerrainSaveState::StaticStruct()->CompareScriptStruct(&BeforeFlat,&AfterFlat,0));
	}
	return Finish();
}
#else
UAftershockCrowdSpacingAuditCommandlet::UAftershockCrowdSpacingAuditCommandlet() {}
int32 UAftershockCrowdSpacingAuditCommandlet::Main(const FString& Params) { return 1; }
#endif
