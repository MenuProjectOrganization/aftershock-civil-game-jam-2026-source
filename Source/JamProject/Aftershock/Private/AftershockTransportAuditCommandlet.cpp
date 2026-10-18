#include "AftershockTransportAuditCommandlet.h"
#include "AftershockTerrainSurface.h"
#include "Aftershock/Public/TransportSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include <limits>

UAftershockTransportAuditCommandlet::UAftershockTransportAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}

int32 UAftershockTransportAuditCommandlet::Main(const FString& Params)
{
	if(!GEngine)return 1;
	UWorld* W=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("CityTransitFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	if(!W)return 1;
	FWorldContext& Context=GEngine->CreateNewWorldContext(EWorldType::Game);Context.SetCurrentWorld(W);
	auto* T=W->GetSubsystem<UTerrainGridSubsystem>();auto* B=W->GetSubsystem<UBuildingManagerSubsystem>();
	auto* L=W->GetSubsystem<UCityLifeSubsystem>();auto* C=W->GetSubsystem<UCrowdRenderSubsystem>();auto* R=W->GetSubsystem<UTransportSubsystem>();
	bool Passed=T&&B&&L&&C&&R;int32 Checks=0;
	const auto Check=[&](const TCHAR* Name,bool Value){++Checks;Passed&=Value;UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_TRANSPORT_CHECK case=%s passed=%d"),Name,Value);};
	if(Passed)
	{
		T->GenerateProceduralTerrain(20,10,500,1337);
		for(int32 Y=0;Y<10;++Y)for(int32 X=0;X<20;++X)
		{
			FGridCell Cell;Cell.Coords=FIntPoint(X,Y);Cell.bIsPlayable=true;Cell.Elevation=Y*2;
			Cell.bHasRoad=Y==4;T->Cells[T->CoordToIndex(Cell.Coords)]=Cell;
		}
		UStaticMesh* Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
		const auto Place=[&](EBuildingType Type,FIntPoint At,int32 Residents,int32 Workers,int32 Transit)
		{
			UBuildingData* D=NewObject<UBuildingData>(B);D->Type=Type;D->Residents=Residents;D->Workers=Workers;
			D->MeshIntact=Cube;D->TransitCapacity=Transit;D->PowerDemandUnitsPerTurn=0;
			D->PowerProductionUnitsPerTurn=Type==EBuildingType::Generator?500.f:0.f;
			int32 ID=INDEX_NONE;Check(TEXT("fixture_building_placed"),B->PlaceBuilding(At,D,EFoundationQuality::Medium,1,ID));return ID;
		};
		const int32 Home=Place(EBuildingType::House,FIntPoint(2,3),18,0,0);
		const int32 Work=Place(EBuildingType::Office,FIntPoint(14,3),0,8,0);
		Place(EBuildingType::Generator,FIntPoint(5,3),0,0,0);
		const int32 Terminal=Place(EBuildingType::BusTerminal,FIntPoint(8,3),0,0,2);
		Place(EBuildingType::BusStop,FIntPoint(3,3),0,0,2);
		Place(EBuildingType::BusStop,FIntPoint(13,3),0,0,2);
		L->InitializeForCity();C->SynchronizeCanonicalPopulation();R->RefreshInfrastructure();
		Check(TEXT("actual_living_population_excludes_eight_job_slots"),L->GetState().LivingPopulation==18&&C->Agents.Num()==18);
		bool Identities=true;TSet<int32> IDs;
		for(const FCitizenAgent& A:C->Agents){FCityResidentRecord Real;Identities&=L->GetResident(A.ResidentID,Real)&&Real.bAlive&&A.HomeBuildingID==Home&&A.WorkBuildingID==Real.WorkBuildingID;IDs.Add(A.ResidentID);}
		Check(TEXT("visual_people_have_unique_canonical_resident_ids"),Identities&&IDs.Num()==18);
		Check(TEXT("one_actual_terminal_bus_two_actual_stops"),R->GetTransportStats().Buses==1&&R->GetTransportStats().Stops==2);
		FActorSpawnParameters PaintParams;PaintParams.ObjectFlags|=RF_Transient;
		AAftershockTransportHost* PaintHost=W->SpawnActor<AAftershockTransportHost>(FVector::ZeroVector,FRotator::ZeroRotator,PaintParams);
		bool PaintPayloadValid=PaintHost!=nullptr;TArray<FAftershockRoadVehicle> PaintFleet;
		for(int32 I=1;I<=16;++I){FAftershockRoadVehicle V;V.VehicleID=I;V.ResidentOwnerID=I*4;V.Location=R->RoadLocation(FIntPoint(I+1,4));PaintFleet.Add(V);}
		TArray<FLinearColor> FirstPaint,SecondPaint;TSet<FString> DistinctPaint;
		if(PaintHost)
		{
			const auto ReadPaint=[&](TArray<FLinearColor>& Out)
			{
				TArray<UInstancedStaticMeshComponent*> Batches;PaintHost->GetComponents(Batches);
				for(const auto* Batch:Batches)
				{
					if(!Batch->GetName().StartsWith(TEXT("Resident")))continue;
					for(int32 I=0;I<Batch->GetInstanceCount();++I)
					{
						FTransform Transform;if(!Batch->GetInstanceTransform(I,Transform,true)||Transform.GetScale3D().IsNearlyZero())continue;
						const int32 Base=I*Batch->NumCustomDataFloats;
						if(Batch->NumCustomDataFloats<3||!Batch->PerInstanceSMCustomData.IsValidIndex(Base+2)){PaintPayloadValid=false;continue;}
						Out.Emplace(Batch->PerInstanceSMCustomData[Base],Batch->PerInstanceSMCustomData[Base+1],Batch->PerInstanceSMCustomData[Base+2],1.f);
					}
				}
			};
			PaintHost->DrawFleet(PaintFleet);ReadPaint(FirstPaint);
			for(const FLinearColor& Color:FirstPaint)DistinctPaint.Add(FString::Printf(TEXT("%.4f/%.4f/%.4f"),Color.R,Color.G,Color.B));
			PaintHost->DrawFleet(PaintFleet);ReadPaint(SecondPaint);
			bool LegacyBodyFit=true,LegacyActualScale=true;TArray<UInstancedStaticMeshComponent*> SizingBatches;PaintHost->GetComponents(SizingBatches);
			for(const auto* Batch:SizingBatches)if(Batch->GetName().StartsWith(TEXT("Resident")))
				for(int32 I=0;I<Batch->GetInstanceCount();++I)
				{
					FTransform Pose;if(!Batch->GetInstanceTransform(I,Pose,true)||Pose.GetScale3D().IsNearlyZero())continue;
					const FVector Extent=Batch->GetStaticMesh()->GetBoundingBox().GetExtent()*Pose.GetScale3D();
					LegacyBodyFit&=Extent.Y+32.f+8.f<=T->CellSize*(.42f-.22f)+.1f&&Extent.X*2.f<=T->CellSize*.84f+.1f;
					LegacyActualScale&=Pose.GetScale3D().X<1.f&&Pose.GetScale3D().Equals(FVector(Pose.GetScale3D().X),.00001);
				}
			Check(TEXT("legacy_five_metre_rendered_body_and_turn_tail_fit_real_road_and_175cm_pedestrian_clearance"),LegacyBodyFit&&LegacyActualScale);
			T->CellSize=1000.f;PaintHost->DrawFleet(PaintFleet);bool RealTenMetreScale=true;
			for(const auto* Batch:SizingBatches)if(Batch->GetName().StartsWith(TEXT("Resident")))
				for(int32 I=0;I<Batch->GetInstanceCount();++I){FTransform Pose;if(Batch->GetInstanceTransform(I,Pose,true)&&!Pose.GetScale3D().IsNearlyZero())RealTenMetreScale&=Pose.GetScale3D().Equals(FVector::OneVector,.00001);}
			Check(TEXT("ten_metre_car_render_preserves_authored_real_world_centimetres"),RealTenMetreScale);
			T->CellSize=500.f;PaintHost->Destroy();
		}
		Check(TEXT("regular_private_owner_ids_have_three_or_more_actual_instance_rgb_colors"),PaintPayloadValid&&FirstPaint.Num()==16&&DistinctPaint.Num()>=3);
		Check(TEXT("private_car_paint_payload_is_deterministic_across_redraws"),PaintPayloadValid&&FirstPaint.Num()==16&&FirstPaint==SecondPaint);
		// Pure geometry queries at the new configured ten-metre street scale;
		// restore legacy scale before moving this legacy fixture's actual vehicles.
		T->CellSize=1000.f;
		const auto* GeometryStop=R->Stops.FindByPredicate([](const auto& Stop){return !Stop.bTerminal;});
		TArray<FVector> QueuePositions;bool GeometrySafe=GeometryStop!=nullptr,QueueSeparated=true;
		if(GeometryStop)
		{
			for(int32 Slot=0;Slot<12;++Slot)
			{
				FVector Position;GeometrySafe&=R->GetPassengerKerbLocation(GeometryStop->BuildingID,Slot,false,Position)
					&&T->WorldToGrid(Position)==GeometryStop->RoadCell;
				for(const FVector& Other:QueuePositions)QueueSeparated&=FVector::Dist2D(Position,Other)>=79.9f;
				QueuePositions.Add(Position);
			}
			FVector ExitA,ExitB,OverflowPosition;
			GeometrySafe&=R->GetPassengerKerbLocation(GeometryStop->BuildingID,0,true,ExitA)&&R->GetPassengerKerbLocation(GeometryStop->BuildingID,1,true,ExitB)
				&&FVector::Dist2D(ExitA,ExitB)>=79.9f&&!R->GetPassengerKerbLocation(GeometryStop->BuildingID,12,false,OverflowPosition);
			for(const FVector& Waiting:QueuePositions)QueueSeparated&=FVector::Dist2D(Waiting,ExitA)>=79.9f&&FVector::Dist2D(Waiting,ExitB)>=79.9f;
		}
		Check(TEXT("ten_metre_stop_has_twelve_finite_nonoverlapping_waiting_spots_and_two_exits"),GeometrySafe&&QueueSeparated);

        // Independent circle-vs-centre/arm union check, not the production
        // helper. Cell membership alone never proves a passenger is on pavement.
        const auto FullBodyOnSidewalk=[&](FIntPoint Coord,const FVector& Position,int32 Mask)
        {
            constexpr double Radius=32.;
            const double Cell=T->CellSize,X=Position.X-(T->GridOrigin.X+Coord.X*Cell),Y=Position.Y-(T->GridOrigin.Y+Coord.Y*Cell);
            if(X<Radius||Y<Radius||X>Cell-Radius||Y>Cell-Radius||Position.ContainsNaN())return false;
            const auto ClearRect=[&](double X0,double Y0,double X1,double Y1)
            {const double DX=X-FMath::Clamp(X,X0*Cell,X1*Cell),DY=Y-FMath::Clamp(Y,Y0*Cell,Y1*Cell);return DX*DX+DY*DY>=Radius*Radius;};
            bool Clear=ClearRect(.12,.12,.88,.88);
            if(Mask&1)Clear&=ClearRect(.88,.12,1.,.88);
            if(Mask&2)Clear&=ClearRect(.12,.88,.88,1.);
            if(Mask&4)Clear&=ClearRect(0.,.12,.12,.88);
            if(Mask&8)Clear&=ClearRect(.12,0.,.88,.12);
            return Clear&&T->WorldToGrid(Position)==Coord;
        };
        if(GeometryStop)
        {
            bool BodiesSafe=true;
            for(const FVector& Position:QueuePositions)BodiesSafe&=FullBodyOnSidewalk(GeometryStop->RoadCell,Position,5);
            for(int32 Slot=0;Slot<2;++Slot){FVector Position;BodiesSafe&=R->GetPassengerKerbLocation(GeometryStop->BuildingID,Slot,true,Position)&&FullBodyOnSidewalk(GeometryStop->RoadCell,Position,5);}
            Check(TEXT("ten_metre_waiting_and_exits_entire_32cm_bodies_clear_actual_asphalt_union"),BodiesSafe);
            FAftershockPassengerTrip Fake;Fake.TripID=987651;Fake.ResidentID=1;Fake.Mode=EAftershockTravelMode::Bus;Fake.State=EAftershockPassengerState::WalkingToPickup;
            Fake.PickupStopID=GeometryStop->BuildingID;Fake.PickupRoadCell=GeometryStop->RoadCell;Fake.PickupQueueSlot=0;
            R->GetPassengerKerbLocation(Fake.PickupStopID,0,false,Fake.PickupLocation);R->Trips.Add(Fake.TripID,Fake);
            Check(TEXT("road_cell_centre_cannot_board_as_if_it_were_reserved_sidewalk_position"),
                !R->MarkPassengerAtPickup(Fake.TripID,1,T->GridToWorld(Fake.PickupRoadCell))&&R->Trips.FindChecked(Fake.TripID).State==EAftershockPassengerState::WalkingToPickup);
            R->Trips.Remove(Fake.TripID);
            const auto SavedCells=T->Cells;const FVector SavedOrigin=T->GridOrigin;const float SavedSize=T->CellSize;
            auto* Mutable=R->Stops.FindByPredicate([&](const auto& Stop){return Stop.BuildingID==GeometryStop->BuildingID;});
            const FIntPoint SavedRoad=Mutable->RoadCell,TestRoad(10,7);Mutable->RoadCell=TestRoad;
            const FIntPoint Dirs[]={FIntPoint(1,0),FIntPoint(0,1),FIntPoint(-1,0),FIntPoint(0,-1)};
            const auto SetMask=[&](int32 Mask)
            {
                auto Centre=T->GetCell(TestRoad);Centre.bHasRoad=true;Centre.bIsPlayable=true;Centre.bIsRiverbed=false;Centre.bIsOcean=false;Centre.BridgeID=INDEX_NONE;Centre.OccupyingBuildingID=INDEX_NONE;T->SetCell(TestRoad,Centre);
                for(int32 I=0;I<4;++I){const auto At=TestRoad+Dirs[I];auto Cell=T->GetCell(At);Cell.bHasRoad=(Mask&(1<<I))!=0;Cell.bIsPlayable=true;Cell.bIsRiverbed=false;Cell.bIsOcean=false;Cell.BridgeID=INDEX_NONE;Cell.OccupyingBuildingID=INDEX_NONE;T->SetCell(At,Cell);}
            };
            const auto ValidRoster=[&](int32 Count,int32 Mask)
            {
                bool Valid=true;TArray<FVector> Positions;
                for(int32 Slot=0;Slot<Count;++Slot){FVector At=FVector::ZeroVector;Valid&=R->GetPassengerKerbLocation(Mutable->BuildingID,Slot,false,At)&&FullBodyOnSidewalk(TestRoad,At,Mask);Positions.Add(At);}
                for(int32 Slot=0;Slot<2;++Slot){FVector At=FVector::ZeroVector;Valid&=R->GetPassengerKerbLocation(Mutable->BuildingID,Slot,true,At)&&FullBodyOnSidewalk(TestRoad,At,Mask);Positions.Add(At);}
                for(int32 A=0;A<Positions.Num();++A)for(int32 B=A+1;B<Positions.Num();++B)Valid&=FVector::Dist2D(Positions[A],Positions[B])>=79.9;
                FVector Extra;Valid&=!R->GetPassengerKerbLocation(Mutable->BuildingID,Count,false,Extra)&&!R->GetPassengerKerbLocation(Mutable->BuildingID,2,true,Extra);
                return Valid;
            };
            SetMask(10);Check(TEXT("vertical_street_queue_and_two_exits_fit_full_sidewalk_bodies_and_80cm_spacing"),ValidRoster(12,10));
            SetMask(7);Check(TEXT("t_junction_accepts_only_actual_uninterrupted_sidewalk_side"),ValidRoster(12,7));
            SetMask(15);FVector Untouched(123,456,789),Before=Untouched;
            Check(TEXT("cross_junction_rejects_carriageway_queue_without_output_mutation"),!R->GetPassengerKerbLocation(Mutable->BuildingID,0,false,Untouched)&&Untouched==Before);
            T->CellSize=900.f;SetMask(5);Check(TEXT("nine_metre_frontage_exposes_ten_real_slots_instead_of_twelve_overlaps"),ValidRoster(10,5));
            T->CellSize=1000.f;T->GridOrigin=FVector(-1234567,-7654321,SavedOrigin.Z);SetMask(5);
            Check(TEXT("negative_world_origin_preserves_exact_sidewalk_roster"),ValidRoster(12,5));
            T->Cells=SavedCells;T->GridOrigin=SavedOrigin;T->CellSize=SavedSize;Mutable->RoadCell=SavedRoad;
        }

		FAftershockRoadVehicle RenderedBus;RenderedBus.bBus=true;
		const float OpposingLaneSeparation=FVector::Dist2D(R->RoadLocation(FIntPoint(8,4),FVector::ForwardVector),R->RoadLocation(FIntPoint(8,4),-FVector::ForwardVector));
		Check(TEXT("ten_metre_oncoming_bus_lanes_fit_actual_rendered_model_width"),OpposingLaneSeparation>R->VehicleDimensions(RenderedBus).Y+20.f);
		T->CellSize=500.f;
		Check(TEXT("legacy_oncoming_bus_lanes_fit_same_effective_dimensions_as_render"),FVector::Dist2D(R->RoadLocation(FIntPoint(8,4),FVector::ForwardVector),R->RoadLocation(FIntPoint(8,4),-FVector::ForwardVector))>R->VehicleDimensions(RenderedBus).Y+20.f);
		Check(TEXT("swept_vehicle_disc_rejects_crosswalk_tunnelling"),UTransportSubsystem::VehicleSweepIntersectsDisc(FVector(0,0,0),0,FVector(500,0,0),0,FVector(450,180,150),FVector(300,-200,5),FVector(300,200,5),32.f));
		Check(TEXT("swept_vehicle_disc_accepts_physically_clear_sidewalk"),!UTransportSubsystem::VehicleSweepIntersectsDisc(FVector(0,0,0),0,FVector(40,0,0),0,FVector(450,180,150),FVector(0,160,5),FVector(40,160,5),32.f));
		const FVector From=R->RoadLocation(FIntPoint(2,4)),To=R->RoadLocation(FIntPoint(14,4));
		FAftershockPassengerTrip A,D,Overflow,Invalid;
		Check(TEXT("unknown_resident_cannot_create_traffic"),!R->PlanTrip(987654,From,To,Work,true,Invalid));
		Check(TEXT("removed_destination_cannot_create_traffic"),!R->PlanTrip(3,From,To,987654,true,Invalid));
		Check(TEXT("real_resident_prefers_connected_bus"),R->PlanTrip(1,From,To,Work,false,A)&&A.Mode==EAftershockTravelMode::Bus);
		Check(TEXT("second_resident_reserves_second_waiting_place"),R->PlanTrip(2,From,To,Work,false,D));
		Check(TEXT("finite_stop_queue_has_no_third_reservation"),!R->PlanTrip(3,From,To,Work,false,Overflow));
		Check(TEXT("boarding_requires_matching_actual_resident"),!R->MarkPassengerAtPickup(A.TripID,2,A.PickupLocation));
		Check(TEXT("boarding_rejects_remote_location"),!R->MarkPassengerAtPickup(A.TripID,1,To));
		Check(TEXT("walking_passenger_waits_at_actual_pickup"),R->MarkPassengerAtPickup(A.TripID,1,A.PickupLocation));
		R->MarkPassengerAtPickup(D.TripID,2,D.PickupLocation);
		bool EverBoarded=false,CapacityValid=true,EverQueuedAlight=false,QueueMembership=true;
		for(int32 Step=0;Step<1000;++Step)
		{
			R->UpdateTransport();
			for(const FAftershockRoadVehicle& V:R->Vehicles)
			{
				CapacityValid&=V.PassengerTripIDs.Num()<=V.Capacity;EverBoarded|=V.bBus&&!V.PassengerTripIDs.IsEmpty();
				EverQueuedAlight|=!V.PendingAlightTripIDs.IsEmpty();
				for(int32 ID:V.PendingAlightTripIDs)
				{
					const auto* Passenger=R->Trips.Find(ID);
					QueueMembership&=V.bBus&&V.PassengerTripIDs.Contains(ID)&&Passenger&&Passenger->State==EAftershockPassengerState::Riding
						&&V.ServiceStopIDs.IsValidIndex(V.NextServiceStop)&&Passenger->DropoffStopID==V.ServiceStopIDs[V.NextServiceStop];
				}
			}
			R->GetTrip(A.TripID,A);if(A.State==EAftershockPassengerState::Arrived)break;
		}
		Check(TEXT("finite_capacity_bus_actually_boards_then_alights"),EverBoarded&&CapacityValid&&A.State==EAftershockPassengerState::Arrived);
		Check(TEXT("alighting_is_at_actual_destination_stop"),T->WorldToGrid(A.CurrentLocation)==A.DropoffRoadCell);
		R->GetTrip(D.TripID,D);
		Check(TEXT("bus_alighting_is_queued_without_changing_riding_identity"),EverQueuedAlight&&QueueMembership&&D.State==EAftershockPassengerState::Riding);
		FAftershockTransportSaveState QueuedSnapshot;R->CaptureSaveState(QueuedSnapshot);FText QueueError;
		const auto* QueuedBus=QueuedSnapshot.Vehicles.FindByPredicate([](const auto& V){return !V.PendingAlightTripIDs.IsEmpty();});
		Check(TEXT("queued_alighting_timer_and_passengers_survive_owner_restore"),QueuedBus&&QueuedBus->PendingAlightTripIDs.Contains(D.TripID)
			&&QueuedBus->AlightReleaseSeconds>0.f&&R->RestoreSaveState(QueuedSnapshot,QueueError));
		if(QueuedBus)
		{
			const auto SaveBytes=[](FAftershockTransportSaveState State)
			{
				TArray<uint8> Bytes;FMemoryWriter Writer(Bytes,true);FObjectAndNameAsStringProxyArchive Archive(Writer,false);
				FAftershockTransportSaveState::StaticStruct()->SerializeItem(Archive,&State,nullptr);return Bytes;
			};
			const auto RejectOwnerQueue=[&](const TCHAR* Name,TFunctionRef<void(FAftershockTransportSaveState&,FAftershockRoadVehicle&)> Mutate)
			{
				FAftershockTransportSaveState BeforeState;R->CaptureSaveState(BeforeState);auto InvalidState=BeforeState;
				auto* Queue=InvalidState.Vehicles.FindByPredicate([](const auto& V){return !V.PendingAlightTripIDs.IsEmpty();});
				if(!Queue){Check(Name,false);return;}
				const auto BeforeStats=R->GetTransportStats();Mutate(InvalidState,*Queue);FText Error;
				const bool Rejected=!R->RestoreSaveState(InvalidState,Error);FAftershockTransportSaveState AfterState;R->CaptureSaveState(AfterState);
				const auto AfterStats=R->GetTransportStats();
				Check(Name,Rejected&&!Error.IsEmpty()&&SaveBytes(BeforeState)==SaveBytes(AfterState)
					&&BeforeStats.RidingPassengers==AfterStats.RidingPassengers&&BeforeStats.CompletedTrips==AfterStats.CompletedTrips
					&&BeforeStats.BusArrivals==AfterStats.BusArrivals&&BeforeStats.Cars==AfterStats.Cars&&BeforeStats.Buses==AfterStats.Buses);
			};
			RejectOwnerQueue(TEXT("direct_owner_rejects_duplicate_alight_without_mutation"),[](auto&,auto& V){const int32 Duplicate=V.PendingAlightTripIDs[0];V.PendingAlightTripIDs.Add(Duplicate);});
			RejectOwnerQueue(TEXT("direct_owner_rejects_nan_alight_timer_without_mutation"),[](auto&,auto& V){V.AlightReleaseSeconds=std::numeric_limits<float>::quiet_NaN();});
			RejectOwnerQueue(TEXT("direct_owner_rejects_negative_alight_timer_without_mutation"),[](auto&,auto& V){V.AlightReleaseSeconds=-.01f;});
			RejectOwnerQueue(TEXT("direct_owner_rejects_excessive_alight_timer_without_mutation"),[](auto&,auto& V){V.AlightReleaseSeconds=.51f;});
			RejectOwnerQueue(TEXT("direct_owner_rejects_wrong_current_alight_stop_without_mutation"),[](auto&,auto& V){V.NextServiceStop=(V.NextServiceStop+1)%V.ServiceStopIDs.Num();});
			RejectOwnerQueue(TEXT("direct_owner_rejects_unknown_alighting_trip_without_mutation"),[](auto&,auto& V){V.PendingAlightTripIDs[0]=MAX_int32;});
			RejectOwnerQueue(TEXT("direct_owner_rejects_unassigned_alighting_passenger_without_mutation"),[](auto&,auto& V){V.PassengerTripIDs.Remove(V.PendingAlightTripIDs[0]);});
			RejectOwnerQueue(TEXT("direct_owner_rejects_inactive_alighting_bus_without_mutation"),[](auto&,auto& V){V.bActive=false;});
			RejectOwnerQueue(TEXT("direct_owner_rejects_nonriding_alighting_trip_without_mutation"),[](auto& State,auto& V)
			{for(auto& Trip:State.Trips)if(Trip.TripID==V.PendingAlightTripIDs[0])Trip.State=EAftershockPassengerState::Arrived;});
			RejectOwnerQueue(TEXT("direct_owner_rejects_missing_alighting_stop_without_mutation"),[](auto& State,auto& V)
			{const int32 Stop=V.ServiceStopIDs[V.NextServiceStop];State.Stops.RemoveAll([Stop](const auto& S){return S.BuildingID==Stop;});});
			RejectOwnerQueue(TEXT("direct_owner_rejects_empty_queue_with_residual_timer_without_mutation"),[](auto&,auto& V){V.PendingAlightTripIDs.Reset();V.AlightReleaseSeconds=.2f;});
		}
		R->UpdateTransport();R->GetTrip(D.TripID,D);
		Check(TEXT("bus_does_not_release_all_passengers_on_one_callback"),D.State==EAftershockPassengerState::Riding);
		for(int32 Step=0;Step<12;++Step)R->UpdateTransport();R->GetTrip(D.TripID,D);
		Check(TEXT("queued_passenger_eventually_alights_at_public_curb"),D.State==EAftershockPassengerState::Arrived
			&&T->WorldToGrid(D.CurrentLocation)==D.DropoffRoadCell&&FVector::Dist2D(D.CurrentLocation,A.CurrentLocation)<1.f);
		FAftershockTransportSaveState EmptyQueueSnapshot;R->CaptureSaveState(EmptyQueueSnapshot);
		Check(TEXT("direct_owner_accepts_legacy_empty_default_alight_queue"),EmptyQueueSnapshot.Vehicles.ContainsByPredicate([](const auto& V){return V.bBus&&V.PendingAlightTripIDs.IsEmpty()&&V.AlightReleaseSeconds==0.f;})
			&&R->RestoreSaveState(EmptyQueueSnapshot,QueueError));
		Check(TEXT("same_boarding_fare_cannot_be_charged_twice"),!L->RecordTransitFare(1,A.TripID));
		B->RemoveBuilding(Terminal);L->RefreshCity();R->RefreshInfrastructure();
		Check(TEXT("removed_terminal_stops_service"),R->GetTransportStats().Buses==0);
		FAftershockPassengerTrip Car;
		Check(TEXT("private_car_trip_is_owned_by_real_person"),R->PlanTrip(4,From,To,Work,true,Car)&&Car.Mode==EAftershockTravelMode::PrivateCar);
		R->MarkPassengerAtPickup(Car.TripID,4,Car.PickupLocation);
		for(int32 Step=0;Step<10;++Step)R->UpdateTransport();
		const FVector Before=R->Vehicles.Last().Location;
		T->Cells[T->CoordToIndex(FIntPoint(9,4))].bHasRoad=false;R->HandleRoadsChanged();
		bool CrossedGap=false;
		for(int32 Step=0;Step<500;++Step){R->UpdateTransport();CrossedGap|=T->WorldToGrid(R->Vehicles.Last().Location).X>=9;}
		R->GetTrip(Car.TripID,Car);
		Check(TEXT("removed_road_strands_actual_car_without_offroad_teleport"),!CrossedGap&&Car.State==EAftershockPassengerState::Stranded&&Car.CurrentLocation.X>=Before.X);
		Check(TEXT("disconnected_road_cannot_plan_new_vehicle_route"),!R->PlanTrip(6,From,To,Work,true,Invalid));
		R->SetEmergencyMode(true);const FVector EmergencyAt=R->Vehicles.Last().Location;
		for(int32 Step=0;Step<20;++Step)R->UpdateTransport();
		Check(TEXT("emergency_stops_vehicles_at_actual_positions"),R->Vehicles.Last().Location.Equals(EmergencyAt,.1));
		R->SetEmergencyMode(false);
		T->Cells[T->CoordToIndex(FIntPoint(9,4))].bHasRoad=true;R->HandleRoadsChanged();
		FAftershockPassengerTrip RecoveredCar;
		const FVector ParkedAt=R->Vehicles.Last().Location;const int32 ParkedVehicleID=R->Vehicles.Last().VehicleID;
		Check(TEXT("repaired_road_reuses_stranded_car_without_teleport"),R->PlanTrip(4,ParkedAt,To,Work,true,RecoveredCar)
			&&RecoveredCar.VehicleID==ParkedVehicleID&&R->Vehicles.Last().Location.Equals(ParkedAt,.1)
			&&RecoveredCar.PickupLocation.Equals(R->VehicleDoorLocation(R->Vehicles.Last()),.1)
			&&R->IsPedestrianSpaceClear(RecoveredCar.PickupLocation,RecoveredCar.PickupLocation,32.f,false));
		R->MarkPassengerAtPickup(RecoveredCar.TripID,4,RecoveredCar.PickupLocation);
		for(int32 Step=0;Step<500;++Step)R->UpdateTransport();
		R->GetTrip(RecoveredCar.TripID,RecoveredCar);
		Check(TEXT("recovered_car_completes_new_connected_trip"),RecoveredCar.State==EAftershockPassengerState::Arrived);
		Check(TEXT("arrived_owned_car_uses_real_finite_destination_garage"),R->Vehicles.Last().ParkedAtBuildingID==Work&&R->Vehicles.Last().VehicleID==ParkedVehicleID&&R->Vehicles.Last().PassengerTripIDs.IsEmpty());
		FAftershockPassengerTrip FirstFrontageCar,OverlappingCar;
		Check(TEXT("new_private_car_starts_at_departure_kerb"),R->PlanTrip(6,From,To,Work,true,FirstFrontageCar)
			&&R->Vehicles.Last().Location.Equals(R->RoadLocation(FIntPoint(2,4)),.1)
			&&FirstFrontageCar.PickupLocation.Equals(R->VehicleDoorLocation(R->Vehicles.Last()),.1)
			&&R->IsPedestrianSpaceClear(FirstFrontageCar.PickupLocation,FirstFrontageCar.PickupLocation,32.f,false));
		Check(TEXT("shared_housing_frontage_does_not_stack_private_cars"),!R->PlanTrip(7,From,To,Work,true,OverlappingCar));
		R->CancelTrip(FirstFrontageCar.TripID);
		for(int32 Y=2;Y<=6;++Y)T->Cells[T->CoordToIndex(FIntPoint(10,Y))].bHasRoad=true;
		R->HandleRoadsChanged();
		const int32 FirstIsolatedDrivingVehicleID=R->NextVehicleID;
		const auto DrivingFixture=[&](int32 ResidentID,FIntPoint Start,TArray<FIntPoint> Path)
		{
			// Each subsequent scenario starts after the previous destinationless
			// test cars completed. They have no real building/garage and are fixture
			// teardown, not parked gameplay cars that may be ignored by collision.
			for(auto& Old:R->Vehicles)if(Old.VehicleID>=FirstIsolatedDrivingVehicleID&&Old.PassengerTripIDs.IsEmpty()&&!Old.RoadPath.IsValidIndex(Old.NextPathStep))Old.bActive=false;
			FAftershockRoadVehicle V;V.VehicleID=R->NextVehicleID++;V.ResidentOwnerID=ResidentID;V.SegmentOriginCell=Start;V.RoadPath=MoveTemp(Path);
			const FVector Direction=(T->GridToWorld(V.RoadPath[0])-T->GridToWorld(Start)).GetSafeNormal2D();
			V.Location=R->RoadLocation(Start,Direction);V.Yaw=Direction.Rotation().Yaw;
			FAftershockPassengerTrip P;P.TripID=R->NextTripID++;P.ResidentID=ResidentID;P.Mode=EAftershockTravelMode::PrivateCar;
			P.State=EAftershockPassengerState::Riding;P.VehicleID=V.VehicleID;P.CurrentLocation=V.Location;P.DropoffRoadCell=V.RoadPath.Last();
			V.PassengerTripIDs.Add(P.TripID);R->Trips.Add(P.TripID,P);R->Vehicles.Add(V);return P.TripID;
		};
		const int32 ExactWaypointTrip=DrivingFixture(7,FIntPoint(6,4),{FIntPoint(7,4),FIntPoint(8,4)});
		R->Vehicles.Last().Location=R->RoadLocation(FIntPoint(7,4));R->Vehicles.Last().bSegmentLaneAligned=true;
		R->Trips.FindChecked(ExactWaypointTrip).CurrentLocation=R->Vehicles.Last().Location;
		R->UpdateTransport();
		Check(TEXT("already_reached_waypoint_advances_with_zero_remaining_distance"),R->Vehicles.Last().NextPathStep==1&&R->Vehicles.Last().SegmentOriginCell==FIntPoint(7,4));
		for(int32 Step=0;Step<500;++Step)R->UpdateTransport();
		Check(TEXT("exact_waypoint_trip_finishes_connected_route"),R->Trips.FindChecked(ExactWaypointTrip).State==EAftershockPassengerState::Arrived);
		const int32 ClampedWaypointTrip=DrivingFixture(8,FIntPoint(6,4),{FIntPoint(7,4),FIntPoint(8,4)});
		const FVector ClampedGoal=R->RoadLocation(FIntPoint(7,4));
		R->Vehicles.Last().Location=ClampedGoal-FVector(20.f,0,0);R->Vehicles.Last().bSegmentLaneAligned=true;
		R->Trips.FindChecked(ClampedWaypointTrip).CurrentLocation=R->Vehicles.Last().Location;
		R->UpdateTransport();const bool LandedExactly=FVector::Dist2D(R->Vehicles.Last().Location,ClampedGoal)<=.01f;
		R->UpdateTransport();
		Check(TEXT("distance_clamped_last_step_acknowledges_waypoint_on_next_callback"),LandedExactly&&R->Vehicles.Last().NextPathStep==1&&R->Vehicles.Last().SegmentOriginCell==FIntPoint(7,4));
		for(int32 Step=0;Step<500;++Step)R->UpdateTransport();
		Check(TEXT("distance_clamped_waypoint_trip_eventually_arrives"),R->Trips.FindChecked(ClampedWaypointTrip).State==EAftershockPassengerState::Arrived);
		const int32 CrossingA=DrivingFixture(8,FIntPoint(9,4),{FIntPoint(10,4),FIntPoint(11,4),FIntPoint(12,4)});
		const int32 CrossingB=DrivingFixture(9,FIntPoint(10,5),{FIntPoint(10,4),FIntPoint(10,3),FIntPoint(10,2)});
		for(int32 Step=0;Step<500;++Step)R->UpdateTransport();
		FAftershockPassengerTrip CrossA,CrossB;R->GetTrip(CrossingA,CrossA);R->GetTrip(CrossingB,CrossB);
		Check(TEXT("crossing_traffic_clears_junction_without_mutual_wait"),CrossA.State==EAftershockPassengerState::Arrived&&CrossB.State==EAftershockPassengerState::Arrived);
		const int32 EnteringTrip=DrivingFixture(12,FIntPoint(9,4),{FIntPoint(10,4),FIntPoint(11,4)});
		const int32 InsideTrip=DrivingFixture(13,FIntPoint(10,4),{FIntPoint(10,3),FIntPoint(10,2)});
		const FVector EnteringBefore=R->Trips.FindChecked(EnteringTrip).CurrentLocation;
		R->UpdateTransport();
		Check(TEXT("occupied_crossing_keeps_priority_over_lower_id_approach"),R->Trips.FindChecked(EnteringTrip).CurrentLocation.Equals(EnteringBefore,.1)
			&&!R->Trips.FindChecked(InsideTrip).CurrentLocation.Equals(R->RoadLocation(FIntPoint(10,4),FVector(0,-1,0)),.1));
		for(int32 Step=0;Step<300;++Step)R->UpdateTransport();
		T->Cells[T->CoordToIndex(FIntPoint(11,5))].bHasRoad=true;R->HandleRoadsChanged();
		const int32 OncomingA=DrivingFixture(10,FIntPoint(10,4),{FIntPoint(11,4),FIntPoint(12,4)});
		const int32 OncomingB=DrivingFixture(11,FIntPoint(11,4),{FIntPoint(10,4),FIntPoint(9,4)});
		for(int32 Step=0;Step<300;++Step)R->UpdateTransport();
		FAftershockPassengerTrip LaneA,LaneB;R->GetTrip(OncomingA,LaneA);R->GetTrip(OncomingB,LaneB);
		Check(TEXT("oncoming_lanes_exchange_adjacent_junctions_without_deadlock"),LaneA.State==EAftershockPassengerState::Arrived&&LaneB.State==EAftershockPassengerState::Arrived);
		// Replay the actual 90-second city deadlock coordinates: vehicle 9 was
		// turning east from the north/south lane while vehicle 21 was westbound.
		FText ExtensionError;
		Check(TEXT("traffic_fixture_extends_via_canonical_chunks"),T->EnsureChunkGenerated(FIntPoint(0,0),ExtensionError)&&T->EnsureChunkGenerated(FIntPoint(0,1),ExtensionError));
		for(FGridCell& Cell:T->Cells)if(Cell.Coords.Y>=10||Cell.Coords.X>=20)
		{const FIntPoint Coord=Cell.Coords;Cell=FGridCell();Cell.Coords=Coord;Cell.bIsPlayable=true;}
		for(int32 X=13;X<=18;++X)T->Cells[T->CoordToIndex(FIntPoint(X,20))].bHasRoad=true;
		for(int32 X:{15,16})for(int32 Y=19;Y<=21;++Y)T->Cells[T->CoordToIndex(FIntPoint(X,Y))].bHasRoad=true;
		R->HandleRoadsChanged();
		const int32 LiveTurn=DrivingFixture(14,FIntPoint(15,20),{FIntPoint(16,20),FIntPoint(17,20),FIntPoint(18,20)});
		const int32 LiveWest=DrivingFixture(15,FIntPoint(16,20),{FIntPoint(15,20),FIntPoint(14,20),FIntPoint(13,20)});
		R->Vehicles[R->Vehicles.Num()-2].Location=FVector(7860,10250,3);R->Vehicles[R->Vehicles.Num()-2].Yaw=-90.f;
		R->Vehicles.Last().Location=FVector(8250,10140,3);R->Vehicles.Last().Yaw=180.f;
		FVector PreviousTurn=R->Vehicles[R->Vehicles.Num()-2].Location,PreviousWest=R->Vehicles.Last().Location;
		bool ContinuousOnRoad=true;
		for(int32 Step=0;Step<500;++Step)
		{
			R->UpdateTransport();
			const FVector TurnAt=R->Vehicles[R->Vehicles.Num()-2].Location,WestAt=R->Vehicles.Last().Location;
			ContinuousOnRoad&=FVector::Dist2D(PreviousTurn,TurnAt)<=53.f&&FVector::Dist2D(PreviousWest,WestAt)<=53.f
				&&R->IsRoadSafe(T->WorldToGrid(TurnAt))&&R->IsRoadSafe(T->WorldToGrid(WestAt));
			PreviousTurn=TurnAt;PreviousWest=WestAt;
		}
		FAftershockPassengerTrip LiveTurnResult,LiveWestResult;R->GetTrip(LiveTurn,LiveTurnResult);R->GetTrip(LiveWest,LiveWestResult);
		Check(TEXT("live_90_second_turning_lane_deadlock_coordinates_clear"),LiveTurnResult.State==EAftershockPassengerState::Arrived&&LiveWestResult.State==EAftershockPassengerState::Arrived);
		Check(TEXT("lane_alignment_is_continuous_inside_existing_road_cells"),ContinuousOnRoad);
		for(int32 X=14;X<=18;++X)for(int32 Y=18;Y<=23;++Y)T->Cells[T->CoordToIndex(FIntPoint(X,Y))].bHasRoad=true;
		R->HandleRoadsChanged();
		const int32 MultiA=DrivingFixture(16,FIntPoint(14,22),{FIntPoint(15,22),FIntPoint(16,22),FIntPoint(16,21),FIntPoint(16,20),FIntPoint(15,20),FIntPoint(14,20)});
		const int32 MultiB=DrivingFixture(17,FIntPoint(15,20),{FIntPoint(16,20),FIntPoint(17,20),FIntPoint(17,21),FIntPoint(17,22),FIntPoint(16,22),FIntPoint(15,22),FIntPoint(14,22)});
		for(int32 Step=0;Step<1000;++Step)R->UpdateTransport();
		FAftershockPassengerTrip MultiAResult,MultiBResult;R->GetTrip(MultiA,MultiAResult);R->GetTrip(MultiB,MultiBResult);
		Check(TEXT("multiple_junction_turning_and_oncoming_routes_both_complete"),MultiAResult.State==EAftershockPassengerState::Arrived&&MultiBResult.State==EAftershockPassengerState::Arrived);
		for(FIntPoint Road:{FIntPoint(8,26),FIntPoint(9,26),FIntPoint(10,26),FIntPoint(10,25),FIntPoint(10,24)})T->Cells[T->CoordToIndex(Road)].bHasRoad=true;
		R->HandleRoadsChanged();
		const int32 CornerA=DrivingFixture(18,FIntPoint(9,26),{FIntPoint(10,26),FIntPoint(10,25),FIntPoint(10,24)});
		const int32 CornerB=DrivingFixture(1,FIntPoint(8,26),{FIntPoint(9,26),FIntPoint(10,26),FIntPoint(10,25),FIntPoint(10,24)});
		// Both drivers genuinely arrive at one destination with three garage
		// places. A destinationless car would remain parked on this dead-end lane
		// and physically prevent its follower from reaching the identical bay.
		const int32 CornerDestination=Place(EBuildingType::Office,FIntPoint(11,24),0,0,0);
		R->Trips.FindChecked(CornerA).DestinationBuildingID=CornerDestination;
		R->Trips.FindChecked(CornerB).DestinationBuildingID=CornerDestination;
		bool CornerNoStack=true;
		for(int32 Step=0;Step<500;++Step)
		{
			R->UpdateTransport();
			const auto& Lead=R->Trips.FindChecked(CornerA);const auto& Follower=R->Trips.FindChecked(CornerB);
			if(Lead.State==EAftershockPassengerState::Riding&&Follower.State==EAftershockPassengerState::Riding)
				CornerNoStack&=FVector::DistSquared2D(Lead.CurrentLocation,Follower.CurrentLocation)>100.f;
		}
		Check(TEXT("two_arm_corner_followers_do_not_stack_and_both_arrive"),CornerNoStack&&R->Trips.FindChecked(CornerA).State==EAftershockPassengerState::Arrived&&R->Trips.FindChecked(CornerB).State==EAftershockPassengerState::Arrived);
		C->BeginEmergencyEvacuation(EDisasterType::Earthquake);
		FDisasterReport VisualReport;VisualReport.DisasterType=EDisasterType::Earthquake;VisualReport.TotalSurvivors=9;VisualReport.TotalDeaths=9;
		C->ApplyDisasterCompletedVisuals(VisualReport);
		bool EveryLivingResidentReturns=true;
		for(const FCitizenAgent& Agent:C->Agents)EveryLivingResidentReturns&=Agent.Activity==EAftershockCitizenActivity::Rescued;
		Check(TEXT("canonical_survivors_do_not_receive_casualty_fraction_twice"),EveryLivingResidentReturns&&C->Agents.Num()==18&&L->GetState().LivingPopulation==18);
		// A temporarily displaced resident resumes the schedule when the same
		// canonical home is repaired; no population is invented by the renderer.
		if(!C->Agents.IsEmpty())
		{
			C->Agents[0].Activity=EAftershockCitizenActivity::AtShelter;C->Agents[0].MoveSpeed=0;
			C->SynchronizeCityLifeRoster();
			Check(TEXT("operating_home_releases_temporary_shelter_state"),C->Agents[0].Activity==EAftershockCitizenActivity::Commuting&&C->Agents[0].ActivityAfterArrival==EAftershockCitizenActivity::AtHome);
		}
		Check(TEXT("transport_does_not_mutate_living_population"),L->GetState().LivingPopulation==18&&L->GetResidents().Num()==18);
		R->HistoryClockSeconds=100.f;
		for(TPair<int32,FAftershockPassengerTrip>& Pair:R->Trips)Pair.Value.TerminalObservedAtSeconds=0.f;
		for(int32 I=0;I<2100;++I)
		{
			FAftershockPassengerTrip P;P.TripID=R->NextTripID++;P.ResidentID=1;P.Mode=EAftershockTravelMode::PrivateCar;
			P.State=EAftershockPassengerState::Arrived;P.TerminalObservedAtSeconds=0.f;R->Trips.Add(P.TripID,P);
		}
		TArray<int32> ActiveIDs;
		for(EAftershockPassengerState State:{EAftershockPassengerState::WalkingToPickup,EAftershockPassengerState::Waiting,EAftershockPassengerState::Riding})
		{
			FAftershockPassengerTrip P;P.TripID=R->NextTripID++;P.ResidentID=1;P.State=State;R->Trips.Add(P.TripID,P);ActiveIDs.Add(P.TripID);
		}
		const auto BeforePruning=R->GetTransportStats();R->PruneTripHistory();
		int32 TerminalCount=0;for(const TPair<int32,FAftershockPassengerTrip>& Pair:R->Trips)
			TerminalCount+=Pair.Value.State==EAftershockPassengerState::Arrived||Pair.Value.State==EAftershockPassengerState::Cancelled||Pair.Value.State==EAftershockPassengerState::Stranded?1:0;
		bool ActivePreserved=true;for(int32 ID:ActiveIDs)ActivePreserved&=R->Trips.Contains(ID);
		Check(TEXT("history_retains_2048_terminal_trips_and_all_active_states"),TerminalCount==2048&&ActivePreserved);
		const auto AfterPruning=R->GetTransportStats();
		Check(TEXT("history_pruning_preserves_lifetime_trip_and_boarding_totals"),BeforePruning.CompletedTrips==AfterPruning.CompletedTrips&&BeforePruning.StrandedTrips==AfterPruning.StrandedTrips&&BeforePruning.BusBoardings==AfterPruning.BusBoardings&&BeforePruning.BusArrivals==AfterPruning.BusArrivals&&BeforePruning.PrivateCarTrips==AfterPruning.PrivateCarTrips);
		Check(TEXT("retired_boarding_identity_cannot_charge_fare_again"),!R->Trips.Contains(A.TripID)&&!L->RecordTransitFare(1,A.TripID));
		FAftershockPassengerTrip JustFinished;JustFinished.TripID=A.TripID;JustFinished.State=EAftershockPassengerState::Arrived;JustFinished.TerminalObservedAtSeconds=R->HistoryClockSeconds;
		R->Trips.Add(JustFinished.TripID,JustFinished);R->PruneTripHistory();
		Check(TEXT("old_trip_id_finishing_now_keeps_crowd_read_grace_period"),R->Trips.Contains(JustFinished.TripID));
		R->AuditTransport();
	}
	// Reindexing service stops during the real saved dwell timer must not board
	// people at a remote stop selected by the changed previous-index expression.
	UWorld* DwellWorld=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("TransitDwellEditFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("dwell_edit_world_created"),DwellWorld!=nullptr);
	if(DwellWorld)
	{
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(DwellWorld);
		auto* DT=DwellWorld->GetSubsystem<UTerrainGridSubsystem>();auto* DB=DwellWorld->GetSubsystem<UBuildingManagerSubsystem>();
		auto* DL=DwellWorld->GetSubsystem<UCityLifeSubsystem>();auto* DR=DwellWorld->GetSubsystem<UTransportSubsystem>();
		const bool Available=DT&&DB&&DL&&DR;Check(TEXT("dwell_edit_canonical_subsystems_available"),Available);
		if(Available)
		{
			DT->GenerateProceduralTerrain(20,10,500.f,1337);
			for(auto& Cell:DT->Cells){const FIntPoint Coord=Cell.Coords;Cell=FGridCell();Cell.Coords=Coord;Cell.bIsPlayable=true;Cell.bHasRoad=Coord.Y==4;}
			const auto PlaceDwell=[&](EBuildingType Type,FIntPoint At,int32 People,int32 Capacity)
			{
				auto* Data=NewObject<UBuildingData>(DB);Data->Type=Type;Data->Residents=People;Data->Workers=0;Data->TransitCapacity=Capacity;
				Data->MeshIntact=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));Data->PowerDemandUnitsPerTurn=0;
				Data->PowerProductionUnitsPerTurn=Type==EBuildingType::Generator?500.f:0.f;int32 ID=INDEX_NONE;
				Check(TEXT("dwell_edit_fixture_building_placed"),DB->PlaceBuilding(At,Data,EFoundationQuality::Medium,1,ID));return ID;
			};
			PlaceDwell(EBuildingType::House,FIntPoint(2,3),18,0);PlaceDwell(EBuildingType::Generator,FIntPoint(5,3),0,0);
			PlaceDwell(EBuildingType::BusTerminal,FIntPoint(8,3),0,8);
			const int32 StopA=PlaceDwell(EBuildingType::BusStop,FIntPoint(3,3),0,4),StopB=PlaceDwell(EBuildingType::BusStop,FIntPoint(10,3),0,4),StopC=PlaceDwell(EBuildingType::BusStop,FIntPoint(15,3),0,4);
			DL->InitializeForCity();DR->RefreshInfrastructure();
			FAftershockPassengerTrip WaitingB;
			const bool PlannedB=DR->PlanTrip(1,DR->RoadLocation(FIntPoint(10,4)),DR->RoadLocation(FIntPoint(15,4)),StopC,false,WaitingB)
				&&WaitingB.PickupStopID==StopB&&DR->MarkPassengerAtPickup(WaitingB.TripID,1,WaitingB.PickupLocation);
			Check(TEXT("dwell_edit_real_resident_waits_at_second_of_three_stops"),PlannedB&&DR->Vehicles.Num()==1&&DR->Vehicles[0].ServiceStopIDs.Num()==3);
			if(PlannedB&&DR->Vehicles.Num()==1)
			{
				auto& Bus=DR->Vehicles[0];Bus.Location=DR->RoadLocation(FIntPoint(3,4));Bus.SegmentOriginCell=FIntPoint(3,4);
				Bus.NextServiceStop=Bus.ServiceStopIDs.Find(StopA);Bus.RoadPath.Reset();Bus.NextPathStep=0;Bus.DwellSeconds=0.f;DR->HandleBusStop(Bus);
				const int64 BeforeRemovedStopFare=DL->GetPendingTransitRevenueTL();const FVector AtRemovedStop=Bus.Location;
				const bool Removed=DB->RemoveBuilding(StopA);DL->RefreshCity();DR->RefreshInfrastructure();
				for(int32 Step=0;Step<4;++Step)DR->UpdateTransport();DR->GetTrip(WaitingB.TripID,WaitingB);
				Check(TEXT("removing_current_stop_during_dwell_does_not_board_remote_waiting_resident"),Removed&&WaitingB.State==EAftershockPassengerState::Waiting
					&&WaitingB.VehicleID==INDEX_NONE&&Bus.Location.Equals(AtRemovedStop,.01f)&&DL->GetPendingTransitRevenueTL()==BeforeRemovedStopFare);
				Bus.Location=DR->RoadLocation(FIntPoint(10,4));Bus.SegmentOriginCell=FIntPoint(10,4);Bus.NextServiceStop=Bus.ServiceStopIDs.Find(StopB);
				Bus.RoadPath.Reset();Bus.NextPathStep=0;Bus.DwellSeconds=0.f;DR->HandleBusStop(Bus);DR->GetTrip(WaitingB.TripID,WaitingB);
				Check(TEXT("same_resident_boards_and_pays_once_when_bus_really_reaches_stop"),WaitingB.State==EAftershockPassengerState::Riding&&WaitingB.VehicleID==Bus.VehicleID
					&&DL->GetPendingTransitRevenueTL()==BeforeRemovedStopFare+5);
				for(int32 Step=0;Step<1000;++Step){DR->UpdateTransport();DR->GetTrip(WaitingB.TripID,WaitingB);if(WaitingB.State==EAftershockPassengerState::Arrived)break;}
				Check(TEXT("legitimate_dwell_edit_passenger_still_alights_at_destination"),WaitingB.State==EAftershockPassengerState::Arrived&&DT->WorldToGrid(WaitingB.CurrentLocation)==FIntPoint(15,4));
				// At the previous last stop, NextServiceStop wraps to zero. Appending
				// a new stop changes the inferred previous slot while the bus stays put.
				Bus.Location=DR->RoadLocation(FIntPoint(15,4));Bus.SegmentOriginCell=FIntPoint(15,4);Bus.NextServiceStop=Bus.ServiceStopIDs.Find(StopC);
				Bus.RoadPath.Reset();Bus.NextPathStep=0;Bus.DwellSeconds=0.f;DR->HandleBusStop(Bus);
				const int32 StopD=PlaceDwell(EBuildingType::BusStop,FIntPoint(17,3),0,4);DL->RefreshCity();DR->RefreshInfrastructure();
				FAftershockPassengerTrip WaitingD;const bool PlannedD=DR->PlanTrip(2,DR->RoadLocation(FIntPoint(17,4)),DR->RoadLocation(FIntPoint(10,4)),StopB,false,WaitingD)
					&&WaitingD.PickupStopID==StopD&&DR->MarkPassengerAtPickup(WaitingD.TripID,2,WaitingD.PickupLocation);
				const int64 BeforeAddedStopFare=DL->GetPendingTransitRevenueTL();const FVector AtPreviousLastStop=Bus.Location;
				for(int32 Step=0;Step<4;++Step)DR->UpdateTransport();DR->GetTrip(WaitingD.TripID,WaitingD);
				Check(TEXT("adding_stop_during_wrapped_dwell_does_not_board_new_remote_queue"),PlannedD&&WaitingD.State==EAftershockPassengerState::Waiting&&WaitingD.VehicleID==INDEX_NONE
					&&Bus.Location.Equals(AtPreviousLastStop,.01f)&&DL->GetPendingTransitRevenueTL()==BeforeAddedStopFare);
				Bus.Location=DR->RoadLocation(FIntPoint(17,4));DR->BoardBusPassenger(Bus,StopD);DR->GetTrip(WaitingD.TripID,WaitingD);
				Check(TEXT("new_stop_allows_boarding_only_after_actual_bus_arrival"),PlannedD&&WaitingD.State==EAftershockPassengerState::Riding
					&&DL->GetPendingTransitRevenueTL()==BeforeAddedStopFare+5&&DL->GetState().LivingPopulation==18);
			}
		}
		DwellWorld->DestroyWorld(false);GEngine->DestroyWorldContext(DwellWorld);
	}
	// A real two-bank passenger/car journey exercises the production bridge graph,
	// ramps and surface sampler; no mirrored or teleporting crossing implementation.
	UWorld* BridgeWorld=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("BridgeTransitFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("bridge_transit_world_created"),BridgeWorld!=nullptr);
	if(BridgeWorld)
	{
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(BridgeWorld);
		auto* BT=BridgeWorld->GetSubsystem<UTerrainGridSubsystem>();auto* BB=BridgeWorld->GetSubsystem<UBuildingManagerSubsystem>();auto* BL=BridgeWorld->GetSubsystem<UCityLifeSubsystem>();auto* BR=BridgeWorld->GetSubsystem<UTransportSubsystem>();auto* Roads=BridgeWorld->GetSubsystem<URoadNetworkSubsystem>();
		BT->GenerateProceduralTerrain(24,12,500,1337);
		for(FGridCell& Cell:BT->Cells)
		{const FIntPoint At=Cell.Coords;Cell=FGridCell();Cell.Coords=At;Cell.bIsPlayable=true;Cell.bIsRiverbed=At.X==11||At.X==12;Cell.Elevation=Cell.bIsRiverbed?0:100;Cell.InitialFloodDepth=Cell.bIsRiverbed?100:0;Cell.FloodDepth=Cell.InitialFloodDepth;Cell.bHasRoad=At.Y==5&&!Cell.bIsRiverbed;}
		const auto PlaceBridgeBuilding=[&](EBuildingType Type,FIntPoint At,int32 People,int32 Jobs,int32 Capacity)
		{auto* Data=NewObject<UBuildingData>(BB);Data->Type=Type;Data->Residents=People;Data->Workers=Jobs;Data->TransitCapacity=Capacity;Data->MeshIntact=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));Data->PowerDemandUnitsPerTurn=0;Data->PowerProductionUnitsPerTurn=Type==EBuildingType::Generator?500:0;int32 ID=INDEX_NONE;Check(TEXT("bridge_fixture_building_placed"),BB->PlaceBuilding(At,Data,EFoundationQuality::Medium,1,ID));return ID;};
		PlaceBridgeBuilding(EBuildingType::House,FIntPoint(2,4),18,0,0);const int32 WorkID=PlaceBridgeBuilding(EBuildingType::Office,FIntPoint(20,4),0,8,0);
		// One queue slot is a deliberate capacity fixture. The first passenger
		// reserves it, so allowPrivateCar on the second plan exercises its real
		// fallback contract rather than assuming that the flag forces a car.
		PlaceBridgeBuilding(EBuildingType::Generator,FIntPoint(7,4),0,0,0);PlaceBridgeBuilding(EBuildingType::BusTerminal,FIntPoint(5,4),0,0,6);PlaceBridgeBuilding(EBuildingType::BusStop,FIntPoint(3,4),0,0,1);PlaceBridgeBuilding(EBuildingType::BusStop,FIntPoint(19,4),0,0,1);
		FAftershockRoadQuote BridgeQuote;const bool Built=Roads->BuildRoad(FIntPoint(9,5),FIntPoint(14,5),BridgeQuote);Check(TEXT("actual_two_bank_bridge_built_for_transit"),Built&&BridgeQuote.bIncludesBridge);
		BL->InitializeForCity();BR->RefreshInfrastructure();Check(TEXT("bridge_connects_real_jobs_across_river"),BL->GetState().EmployedResidents==8&&BL->GetBuildingStatus(WorkID).bRoadConnected);
		FAftershockPassengerTrip Bus,Car;const FVector Origin=BR->RoadLocation(FIntPoint(2,5)),Destination=BR->RoadLocation(FIntPoint(20,5));
		const bool BusPlanned=BR->PlanTrip(1,Origin,Destination,WorkID,false,Bus),CarPlanned=BR->PlanTrip(4,Origin,Destination,WorkID,true,Car);
		Check(TEXT("real_bus_and_private_car_plan_over_bridge"),BusPlanned&&CarPlanned&&Bus.Mode==EAftershockTravelMode::Bus&&Car.Mode==EAftershockTravelMode::PrivateCar);
		if(BusPlanned)BR->MarkPassengerAtPickup(Bus.TripID,1,Bus.PickupLocation);if(CarPlanned)BR->MarkPassengerAtPickup(Car.TripID,4,Car.PickupLocation);
		bool CrossedDeck=false,StayedOnSurface=true;
		for(int32 Step=0;Step<2400;++Step)
		{
			BR->UpdateTransport();for(const auto& Vehicle:BR->Vehicles)
			if(BT->GetCell(BT->WorldToGrid(Vehicle.Location)).bIsRiverbed)
			{CrossedDeck=true;StayedOnSurface&=FMath::Abs(Vehicle.Location.Z-Roads->SampleRoadSurfaceElevation(Vehicle.Location)-3.f)<3.f;}
		}
		BR->GetTrip(Bus.TripID,Bus);BR->GetTrip(Car.TripID,Car);
		Check(TEXT("bus_boards_crosses_bridge_and_alights"),BusPlanned&&Bus.Mode==EAftershockTravelMode::Bus&&Bus.State==EAftershockPassengerState::Arrived&&BR->GetTransportStats().BusArrivals>0);
		Check(TEXT("private_car_crosses_bridge_and_arrives"),CarPlanned&&Car.Mode==EAftershockTravelMode::PrivateCar&&Car.State==EAftershockPassengerState::Arrived);
		Check(TEXT("actual_vehicles_follow_bridge_deck_height"),CrossedDeck&&StayedOnSurface&&BR->GetTransportStats().InvalidRoadSteps==0);
		BridgeWorld->DestroyWorld(false);GEngine->DestroyWorldContext(BridgeWorld);
	}
	// Exercise the actual consumers across an appended negative-coordinate chunk.
	// The initial 25x25 frame stays immutable; no renderer-owned grid is created.
	UWorld* ChunkWorld=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("ChunkConsumerFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	const int32 ChecksBeforeChunk=Checks;
	bool ChunkPassed=true;
	const auto CheckChunk=[&](const TCHAR* Name,bool Value){ChunkPassed&=Value;Check(Name,Value);};
	CheckChunk(TEXT("chunk_consumer_world_created"),ChunkWorld!=nullptr);
	if(ChunkWorld)
	{
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(ChunkWorld);
		auto* CT=ChunkWorld->GetSubsystem<UTerrainGridSubsystem>();auto* CB=ChunkWorld->GetSubsystem<UBuildingManagerSubsystem>();
		auto* CL=ChunkWorld->GetSubsystem<UCityLifeSubsystem>();auto* CC=ChunkWorld->GetSubsystem<UCrowdRenderSubsystem>();
		auto* CR=ChunkWorld->GetSubsystem<UTransportSubsystem>();auto* Roads=ChunkWorld->GetSubsystem<URoadNetworkSubsystem>();
		if(CT&&CB&&CL&&CC&&CR&&Roads)
		{
			CT->GenerateProceduralTerrain(25,25,500,1337);const int32 OriginalIndex=CT->CoordToIndex(FIntPoint(0,0));
			FText Error;const bool Extended=CT->EnsureChunkGenerated(FIntPoint(-1,0),Error);
			CheckChunk(TEXT("two_chunk_storage_appends_without_changing_generation_frame"),Extended&&CT->Cells.Num()==1250&&CT->GridWidth==25&&CT->GridHeight==25&&CT->CoordToIndex(FIntPoint(0,0))==OriginalIndex);
			for(FGridCell& Cell:CT->Cells){const FIntPoint Coord=Cell.Coords;Cell=FGridCell();Cell.Coords=Coord;Cell.bIsPlayable=true;Cell.Elevation=100;}
			const auto Quote=Roads->QuoteRoad(FIntPoint(-12,12),FIntPoint(12,12));
			CheckChunk(TEXT("road_quote_reconstructs_real_global_coords_across_chunk_seam"),Quote.bRouteValid&&Quote.Path.Num()==25&&Quote.Path[0]==FIntPoint(-12,12)&&Quote.Path.Last()==FIntPoint(12,12)&&Quote.Path.Contains(FIntPoint(-1,12))&&Quote.Path.Contains(FIntPoint(0,12)));
			FAftershockRoadQuote Built;CheckChunk(TEXT("two_chunk_road_build_commits_canonical_cells"),Roads->BuildRoad(FIntPoint(-12,12),FIntPoint(12,12),Built)&&Roads->CountRoadCells()==25);
			const auto PlaceChunkBuilding=[&](EBuildingType Type,FIntPoint At,int32 People,int32 Jobs,int32 Capacity)
			{
				auto* Data=NewObject<UBuildingData>(CB);Data->Type=Type;Data->Residents=People;Data->Workers=Jobs;Data->TransitCapacity=Capacity;Data->MeshIntact=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
				Data->PowerDemandUnitsPerTurn=0;Data->PowerProductionUnitsPerTurn=Type==EBuildingType::Generator?500:0;int32 ID=INDEX_NONE;
				CheckChunk(TEXT("chunk_fixture_building_placed"),CB->PlaceBuilding(At,Data,EFoundationQuality::Medium,1,ID));return ID;
			};
			PlaceChunkBuilding(EBuildingType::House,FIntPoint(-10,11),18,0,0);const int32 WorkID=PlaceChunkBuilding(EBuildingType::Office,FIntPoint(10,11),0,8,0);
			PlaceChunkBuilding(EBuildingType::Generator,FIntPoint(-5,11),0,0,0);PlaceChunkBuilding(EBuildingType::BusTerminal,FIntPoint(-3,11),0,0,6);
			PlaceChunkBuilding(EBuildingType::BusStop,FIntPoint(-8,11),0,0,1);PlaceChunkBuilding(EBuildingType::BusStop,FIntPoint(9,11),0,0,1);
			CL->InitializeForCity();CC->SynchronizeCanonicalPopulation();CR->RefreshInfrastructure();
			CheckChunk(TEXT("city_services_and_real_jobs_connect_across_global_zero"),CL->GetState().LivingPopulation==18&&CL->GetState().EmployedResidents==8&&CL->GetBuildingStatus(WorkID).bRoadConnected);
			TSet<int32> ResidentIDs;for(const auto& Agent:CC->Agents)ResidentIDs.Add(Agent.ResidentID);
			CheckChunk(TEXT("chunk_crowd_preserves_unique_canonical_population"),CC->Agents.Num()==18&&ResidentIDs.Num()==18);
			bool WalkedAcross=false,WalkStayedValid=true;
			if(!CC->Agents.IsEmpty())
			{
				for(auto& Agent:CC->Agents){CC->CancelAgentTransport(Agent);Agent.MoveSpeed=0;Agent.Activity=EAftershockCitizenActivity::AtHome;Agent.CurrentLocation=Agent.HomeLocation;Agent.TargetLocation=Agent.CurrentLocation;Agent.FinalDestinationLocation=Agent.CurrentLocation;Agent.RouteGridCoords.Reset();Agent.ScheduleSecondsRemaining=MAX_flt;}
				auto& Walker=CC->Agents[0];Walker.CurrentLocation=CC->MakeVisibleCellLocation(*CT,FIntPoint(-10,12),Walker);
				const FVector Goal=CC->MakeVisibleCellLocation(*CT,FIntPoint(10,12),Walker);CC->StartCommute(Walker,Goal,EAftershockCitizenActivity::AtWork,false);
				CheckChunk(TEXT("crowd_route_backtracking_uses_negative_and_positive_global_cells"),Walker.bRouteReachesDestination&&Walker.RouteGridCoords.Contains(FIntPoint(-1,12))&&Walker.RouteGridCoords.Contains(FIntPoint(0,12)));
				for(int32 Step=0;Step<1800;++Step){CC->UpdateCrowdPositions();WalkStayedValid&=CT->IsPlayableCoord(CT->WorldToGrid(Walker.CurrentLocation));if(Walker.Activity==EAftershockCitizenActivity::AtWork){WalkedAcross=CT->WorldToGrid(Walker.CurrentLocation)==FIntPoint(10,12);break;}}
			}
			CheckChunk(TEXT("actual_crowd_timer_walk_crosses_chunk_seam_without_invalid_cells"),WalkedAcross&&WalkStayedValid&&CL->GetState().LivingPopulation==18);
			FAftershockPassengerTrip Bus,Car;const FVector From=CR->RoadLocation(FIntPoint(-10,12)),To=CR->RoadLocation(FIntPoint(10,12));
			const bool BusPlanned=CR->PlanTrip(1,From,To,WorkID,false,Bus),CarPlanned=CR->PlanTrip(4,From,To,WorkID,true,Car);
			CheckChunk(TEXT("two_chunk_bus_and_real_private_car_both_plan"),BusPlanned&&CarPlanned&&Bus.Mode==EAftershockTravelMode::Bus&&Car.Mode==EAftershockTravelMode::PrivateCar);
			if(BusPlanned)CR->MarkPassengerAtPickup(Bus.TripID,1,Bus.PickupLocation);if(CarPlanned)CR->MarkPassengerAtPickup(Car.TripID,4,Car.PickupLocation);
			bool BusCrossed=false,CarCrossed=false,VehiclePositionsValid=true;
			for(int32 Step=0;Step<2800;++Step)
			{
				CR->UpdateTransport();for(const auto& Vehicle:CR->Vehicles)if(Vehicle.bActive){const FIntPoint At=CT->WorldToGrid(Vehicle.Location);VehiclePositionsValid&=CT->IsValidCoord(At)&&Roads->IsRoadTraversable(At);if(At.X>=0){BusCrossed|=Vehicle.bBus;CarCrossed|=!Vehicle.bBus;}}
			}
			if(BusPlanned)CR->GetTrip(Bus.TripID,Bus);if(CarPlanned)CR->GetTrip(Car.TripID,Car);
			CheckChunk(TEXT("actual_bus_boards_and_arrives_across_chunk_seam"),BusPlanned&&BusCrossed&&Bus.State==EAftershockPassengerState::Arrived&&CR->GetTransportStats().BusArrivals>0);
			CheckChunk(TEXT("actual_private_car_arrives_across_chunk_seam"),CarPlanned&&Car.Mode==EAftershockTravelMode::PrivateCar&&CarCrossed&&Car.State==EAftershockPassengerState::Arrived);
			CheckChunk(TEXT("chunk_vehicles_never_leave_canonical_road_membership"),VehiclePositionsValid&&CR->GetTransportStats().InvalidRoadSteps==0);
			const int32 StableNegativeIndex=CT->CoordToIndex(FIntPoint(-1,12));const int32 BusID=Bus.VehicleID,CarID=Car.VehicleID;
			CheckChunk(TEXT("negative_one_one_is_a_real_cell_not_index_none_sentinel"),CT->EnsureChunkGenerated(FIntPoint(-1,-1),Error)&&CT->IsValidCoord(FIntPoint(-1,-1))&&CT->IndexToCoord(CT->CoordToIndex(FIntPoint(-1,-1)))==FIntPoint(-1,-1));
			FGridCell Negative=CT->GetCell(FIntPoint(-1,-1));Negative.bIsPlayable=true;Negative.bIsOcean=false;Negative.bIsRiverbed=false;Negative.bVolcanicVent=false;Negative.bBurning=false;Negative.LavaCoverage01=0;Negative.OccupyingBuildingID=INDEX_NONE;Negative.Elevation=100;CT->SetCell(Negative.Coords,Negative);
			const auto NegativeQuote=Roads->QuoteRoad(FIntPoint(-1,-1),FIntPoint(-1,0));
			CheckChunk(TEXT("road_quote_accepts_negative_one_one_global_coordinate"),NegativeQuote.bRouteValid&&NegativeQuote.Path[0]==FIntPoint(-1,-1));
			CheckChunk(TEXT("appending_another_chunk_preserves_existing_routes_and_vehicle_ids"),CT->CoordToIndex(FIntPoint(-1,12))==StableNegativeIndex&&CR->Vehicles.ContainsByPredicate([BusID](const auto& V){return V.VehicleID==BusID;})&&CR->Vehicles.ContainsByPredicate([CarID](const auto& V){return V.VehicleID==CarID;}));
			CheckChunk(TEXT("unresident_cells_are_not_implicitly_generated_by_path_search"),!CT->IsValidCoord(FIntPoint(-26,-1))&&!Roads->QuoteRoad(FIntPoint(-26,-1),FIntPoint(-1,-1)).bRouteValid&&CL->GetState().LivingPopulation==18);
		}
		else CheckChunk(TEXT("chunk_consumer_subsystems_exist"),false);
		ChunkWorld->DestroyWorld(false);GEngine->DestroyWorldContext(ChunkWorld);
	}
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHUNK_CONSUMER_AUDIT result=%s checks=%d canonicalConsumerTest=1 renderedLiveGameProof=0"),ChunkPassed?TEXT("PASS"):TEXT("FAIL"),Checks-ChecksBeforeChunk);
	UWorld* ClearanceWorld=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("PhysicalTrafficClearanceFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("physical_clearance_world_created"),ClearanceWorld!=nullptr);
	if(ClearanceWorld)
	{
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(ClearanceWorld);
		auto* Grid=ClearanceWorld->GetSubsystem<UTerrainGridSubsystem>();auto* Traffic=ClearanceWorld->GetSubsystem<UTransportSubsystem>();auto* People=ClearanceWorld->GetSubsystem<UCrowdRenderSubsystem>();
		Grid->GenerateProceduralTerrain(20,20,1000.f,1337);
		for(auto& Cell:Grid->Cells){const auto Coord=Cell.Coords;Cell=FGridCell();Cell.Coords=Coord;Cell.bIsPlayable=true;Cell.bHasRoad=Coord.X==10||Coord.Y==10;}
		// This fixture bulk-imports cells directly, bypassing terrain/road commit
		// delegates. Refresh the same derived routing cache those delegates build.
		People->RebuildDestinationCaches();
		Check(TEXT("crossing_fixture_has_canonical_walkable_route_cost"),People->RoutingCellCosts.IsValidIndex(Grid->CoordToIndex(FIntPoint(9,10)))&&People->RoutingCellCosts[Grid->CoordToIndex(FIntPoint(9,10))]!=MAX_int32);
		Traffic->bInfrastructureDirty=false;
		const auto Driving=[&](int32 Owner,FIntPoint Start,TArray<FIntPoint> Path)
		{
			FAftershockRoadVehicle V;V.VehicleID=Traffic->NextVehicleID++;V.ResidentOwnerID=Owner;V.RoadPath=MoveTemp(Path);V.SegmentOriginCell=Start;
			const FVector Direction=(Grid->GridToWorld(V.RoadPath[0])-Grid->GridToWorld(Start)).GetSafeNormal2D();V.Location=Traffic->RoadLocation(Start,Direction);V.Yaw=Direction.Rotation().Yaw;
			FAftershockPassengerTrip P;P.TripID=Traffic->NextTripID++;P.ResidentID=Owner;P.VehicleID=V.VehicleID;P.Mode=EAftershockTravelMode::PrivateCar;P.State=EAftershockPassengerState::Riding;P.CurrentLocation=V.Location;P.DropoffRoadCell=V.RoadPath.Last();
			V.PassengerTripIDs.Add(P.TripID);Traffic->Vehicles.Add(V);Traffic->Trips.Add(P.TripID,P);return P.TripID;
		};
		TArray<int32> RealMotionTrips;
		RealMotionTrips.Add(Driving(1,FIntPoint(7,10),{FIntPoint(8,10),FIntPoint(9,10),FIntPoint(10,10),FIntPoint(11,10),FIntPoint(12,10),FIntPoint(13,10)}));
		RealMotionTrips.Add(Driving(2,FIntPoint(13,10),{FIntPoint(12,10),FIntPoint(11,10),FIntPoint(10,10),FIntPoint(9,10),FIntPoint(8,10),FIntPoint(7,10)}));
		RealMotionTrips.Add(Driving(3,FIntPoint(10,7),{FIntPoint(10,8),FIntPoint(10,9),FIntPoint(10,10),FIntPoint(10,11),FIntPoint(10,12),FIntPoint(10,13)}));
		RealMotionTrips.Add(Driving(4,FIntPoint(10,13),{FIntPoint(10,12),FIntPoint(10,11),FIntPoint(10,10),FIntPoint(10,9),FIntPoint(10,8),FIntPoint(10,7)}));
		FCitizenAgent Walker;Walker.RandomSeed=8123;Walker.CurrentLocation=Grid->GridToWorld(FIntPoint(9,10))+FVector(0,-400,5);Walker.TargetLocation=Walker.CurrentLocation+FVector(0,800,0);Walker.FinalDestinationLocation=Walker.TargetLocation;Walker.HomeLocation=Walker.CurrentLocation;Walker.Activity=EAftershockCitizenActivity::Commuting;Walker.ActivityAfterArrival=EAftershockCitizenActivity::AtLeisure;Walker.MoveSpeed=160.f;People->Agents.Add(Walker);
		const FVector RequestedCrossingGoal=Walker.FinalDestinationLocation;
		People->BuildSafeRoute(People->Agents[0],People->Agents[0].FinalDestinationLocation);
		Check(TEXT("same_agent_destination_reference_replan_preserves_exact_crossing_goal"),People->Agents[0].bRouteReachesDestination&&People->Agents[0].FinalDestinationLocation.Equals(RequestedCrossingGoal,.01f)&&People->Agents[0].TargetLocation.Equals(RequestedCrossingGoal,.01f));
		bool CarsNeverOverlap=true,PedestriansNeverOverlapCars=true,FiniteContinuous=true,Crossed=false;
		for(int32 Step=0;Step<1200;++Step)
		{
			Traffic->UpdateTransport();People->UpdateCrowdPositions();
			const auto& Person=People->Agents[0];Crossed|=Person.CurrentLocation.Y>Grid->GridToWorld(FIntPoint(9,10)).Y+320.f;
			PedestriansNeverOverlapCars&=Traffic->IsPedestrianSpaceClear(Person.CurrentLocation,Person.CurrentLocation,32.f,false);
			for(const auto& V:Traffic->Vehicles)if(V.bActive&&V.ParkedAtBuildingID==INDEX_NONE)
			{CarsNeverOverlap&=Traffic->CanMoveVehicle(V,V.Location,V.Yaw,false);FiniteContinuous&=!V.Location.ContainsNaN()&&Grid->GetCell(Grid->WorldToGrid(V.Location)).bHasRoad;}
		}
		bool Completed=true;for(int32 ID:RealMotionTrips)Completed&=Traffic->Trips.FindChecked(ID).State==EAftershockPassengerState::Arrived;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_TRAFFIC_DIAGNOSTICS carsClear=%d finiteRoad=%d pedestrianClear=%d crossed=%d completed=%d walker=%s target=%s final=%s moveSpeed=%.2f routeReaches=%d"),CarsNeverOverlap,FiniteContinuous,PedestriansNeverOverlapCars,Crossed,Completed,*People->Agents[0].CurrentLocation.ToCompactString(),*People->Agents[0].TargetLocation.ToCompactString(),*People->Agents[0].FinalDestinationLocation.ToCompactString(),People->Agents[0].MoveSpeed,People->Agents[0].bRouteReachesDestination);
		if(!Completed)for(const auto& V:Traffic->Vehicles)
		{
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_TRAFFIC_PENDING vehicle=%d at=%s yaw=%.2f next=%d/%d origin=%s aligned=%d parking=%d passengers=%d"),V.VehicleID,*V.Location.ToCompactString(),V.Yaw,V.NextPathStep,V.RoadPath.Num(),*V.SegmentOriginCell.ToString(),V.bSegmentLaneAligned,V.bParking,V.PassengerTripIDs.Num());
		}
		Check(TEXT("four_actual_crossing_and_oncoming_vehicle_routes_have_zero_render_obb_overlaps"),CarsNeverOverlap&&FiniteContinuous&&Completed);
		Check(TEXT("actual_pedestrian_crossing_and_vehicle_timers_yield_without_body_intersection"),PedestriansNeverOverlapCars&&Crossed&&Completed);
		Traffic->Vehicles.Reset();Traffic->Trips.Reset();People->Agents.Reset();People->RebuildCitizenSpatialIndex();
		FAftershockTransportSaveState LegacyTraffic;LegacyTraffic.bInfrastructureDirty=false;LegacyTraffic.NextVehicleID=4;LegacyTraffic.NextTripID=4;
		for(int32 I=1;I<=3;++I)
		{
			FAftershockRoadVehicle V;V.VehicleID=I;V.ResidentOwnerID=I;V.Location=Traffic->RoadLocation(FIntPoint(6,10));V.SegmentOriginCell=FIntPoint(6,10);for(int32 X=7;X<=18;++X)V.RoadPath.Add(FIntPoint(X,10));V.PassengerTripIDs.Add(I);LegacyTraffic.Vehicles.Add(V);
			FAftershockPassengerTrip P;P.TripID=I;P.ResidentID=I;P.VehicleID=I;P.Mode=EAftershockTravelMode::PrivateCar;P.State=EAftershockPassengerState::Riding;P.CurrentLocation=V.Location;P.DropoffRoadCell=FIntPoint(18,10);LegacyTraffic.Trips.Add(P);
		}
		FText LegacyTrafficError;const bool LegacyTrafficLoaded=Traffic->RestoreSaveState(LegacyTraffic,LegacyTrafficError);bool RestoredCarBodiesClear=LegacyTrafficLoaded,TripOwnershipPreserved=LegacyTrafficLoaded;
		for(const auto& V:Traffic->Vehicles)
		{
			RestoredCarBodiesClear&=Traffic->CanMoveVehicle(V,V.Location,V.Yaw,false)&&Grid->GetCell(Grid->WorldToGrid(V.Location)).bHasRoad;
			const auto* P=Traffic->Trips.Find(V.VehicleID);TripOwnershipPreserved&=P&&P->ResidentID==V.ResidentOwnerID&&P->VehicleID==V.VehicleID&&P->CurrentLocation.Equals(V.Location,.01f)&&V.PassengerTripIDs==TArray<int32>{V.VehicleID};
		}
		Check(TEXT("legacy_overlapping_loaded_cars_reconcile_before_render_on_same_road_component"),LegacyTrafficLoaded&&RestoredCarBodiesClear&&Traffic->Vehicles.Num()==3);
		Check(TEXT("legacy_car_pose_repair_preserves_all_actual_passenger_and_vehicle_ids"),TripOwnershipPreserved&&Traffic->NextTripID==4&&Traffic->NextVehicleID==4);
		const auto CapacityTerrain=Grid->Cells;for(auto& Cell:Grid->Cells)Cell.bHasRoad=Cell.Coords==FIntPoint(6,10);
		for(auto& V:LegacyTraffic.Vehicles){V.RoadPath.Reset();V.NextPathStep=0;}for(auto& P:LegacyTraffic.Trips)P.DropoffRoadCell=FIntPoint(6,10);
		const int32 BeforeVehicleID=Traffic->NextVehicleID;TArray<FVector> BeforeFailedImport;for(const auto& V:Traffic->Vehicles)BeforeFailedImport.Add(V.Location);
		const bool TrafficCapacityRejected=!Traffic->RestoreSaveState(LegacyTraffic,LegacyTrafficError);bool TrafficImportUnchanged=Traffic->NextVehicleID==BeforeVehicleID&&Traffic->Vehicles.Num()==BeforeFailedImport.Num();
		for(int32 I=0;I<Traffic->Vehicles.Num()&&BeforeFailedImport.IsValidIndex(I);++I)TrafficImportUnchanged&=Traffic->Vehicles[I].Location==BeforeFailedImport[I];
		Grid->Cells=CapacityTerrain;
		Check(TEXT("legacy_road_capacity_exhaustion_rejects_import_without_partial_vehicle_mutation"),TrafficCapacityRejected&&!LegacyTrafficError.IsEmpty()&&TrafficImportUnchanged);
		Traffic->Vehicles.Reset();Traffic->Trips.Reset();
		FAftershockRoadVehicle Turning;Turning.VehicleID=1;Turning.ResidentOwnerID=4;Turning.Location=Traffic->RoadLocation(FIntPoint(6,10));
		FAftershockRoadVehicle Obstacle=Turning;Obstacle.VehicleID=2;Obstacle.ResidentOwnerID=8;Obstacle.Location+=FVector(0,270,0);Obstacle.bStalled=true;
		Traffic->Vehicles={Turning,Obstacle};const FVector BeforeTurn=Traffic->Vehicles[0].Location;
		Check(TEXT("in_cell_rotation_rejects_rendered_tail_sweep_through_stalled_vehicle"),Traffic->CanMoveVehicle(Traffic->Vehicles[0],BeforeTurn,0.f,false)&&!Traffic->TryMoveVehicle(Traffic->Vehicles[0],BeforeTurn,90.f)&&Traffic->Vehicles[0].Location==BeforeTurn&&Traffic->Vehicles[0].Yaw==0.f);
		FText DrawError;Check(TEXT("transport_draw_fixture_expands_canonical_negative_chunk"),Grid->EnsureChunkGenerated(FIntPoint(-3,0),DrawError));
		const FIntPoint NearCar(10,10),NearBus(10,12),FarCar(-65,10),FarBus(-65,12);
		for(FIntPoint Coord:{NearCar,NearBus,FarCar,FarBus}){auto Cell=Grid->GetCell(Coord);Cell.bHasRoad=true;Cell.bIsPlayable=true;Cell.bIsRiverbed=false;Cell.bIsOcean=false;Grid->SetCell(Coord,Cell);}
		auto* DrawPC=ClearanceWorld->SpawnActor<APlayerController>();auto* DrawCamera=ClearanceWorld->SpawnActor<AAftershockCameraPawn>();
		if(DrawPC&&DrawCamera){ClearanceWorld->AddController(DrawPC);DrawPC->Possess(DrawCamera);}
		const auto SetDrawFocus=[&](FIntPoint Coord)
		{
			if(!DrawPC||!DrawCamera||ClearanceWorld->GetFirstPlayerController()!=DrawPC||DrawPC->GetPawn()!=DrawCamera)return false;
			FAftershockCameraSaveState State;DrawCamera->CaptureSaveState(State);State.LocalFocus=Grid->GridToWorld(Coord);State.Location=State.LocalFocus;
			return DrawCamera->RestoreSaveState(State,DrawError);
		};
		Check(TEXT("transport_draw_fixture_camera_starts_in_initial_city"),SetDrawFocus(NearCar));People->EnsureTerrainVisualBuilt();
		Traffic->Vehicles.Reset();Traffic->Trips.Reset();Traffic->bInfrastructureDirty=false;
		const TArray<FIntPoint> DrawCoords={NearCar,NearBus,FarCar,FarBus};
		for(int32 I=0;I<DrawCoords.Num();++I)
		{
			FAftershockRoadVehicle V;V.VehicleID=501+I;V.ResidentOwnerID=601+I;V.bBus=I%2==1;V.Location=Traffic->RoadLocation(DrawCoords[I]);V.SegmentOriginCell=DrawCoords[I];V.PassengerTripIDs.Add(701+I);
			FAftershockPassengerTrip P;P.TripID=701+I;P.ResidentID=801+I;P.VehicleID=V.VehicleID;P.Mode=V.bBus?EAftershockTravelMode::Bus:EAftershockTravelMode::PrivateCar;P.State=EAftershockPassengerState::Riding;P.CurrentLocation=V.Location;P.PickupRoadCell=DrawCoords[I];P.DropoffRoadCell=DrawCoords[I];
			Traffic->Vehicles.Add(V);Traffic->Trips.Add(P.TripID,P);
		}
		if(!Traffic->Host)Traffic->Host=ClearanceWorld->SpawnActor<AAftershockTransportHost>();
		const auto RenderCounts=[&]()
		{
			FIntVector Counts(0,0,0);TArray<UInstancedStaticMeshComponent*> Batches;Traffic->Host->GetComponents(Batches);
			for(const auto* Batch:Batches)for(int32 I=0;I<Batch->GetInstanceCount();++I)
			{
				FTransform Pose;if(!Batch->GetInstanceTransform(I,Pose,true)||Pose.GetScale3D().IsNearlyZero())continue;
				if(Batch->GetName().StartsWith(TEXT("Resident")))++Counts.X;
				else if(Batch->GetName()==TEXT("ServiceBuses"))++Counts.Y;
				else if(Batch->GetName()==TEXT("VehicleWheels"))++Counts.Z;
			}
			return Counts;
		};
		const auto RenderMatches=[&](int32 CarVehicleIndex,int32 BusVehicleIndex)
		{
			const auto& Car=Traffic->Vehicles[CarVehicleIndex];const auto& Bus=Traffic->Vehicles[BusVehicleIndex];
			TArray<FTransform> ExpectedWheels;
			for(const auto* Vehicle:{&Car,&Bus})
			{
				const FQuat Rotation=FRotator(0,Vehicle->Yaw,0).Quaternion();
				const float Fit=Traffic->Host->GetVehicleRenderScale(*Vehicle,Grid->CellSize),Radius=Vehicle->bBus?43.f:31.f,HalfWidth=Vehicle->bBus?107.f:78.f;
				const float Axles[]={Vehicle->bBus?210.f:120.f,Vehicle->bBus?-180.f:-120.f};
				for(float Axle:Axles)for(float Side:{-1.f,1.f})
					ExpectedWheels.Emplace(Rotation*FQuat(FVector::RightVector,Vehicle->WheelAngle)*FQuat(FVector::ForwardVector,HALF_PI),
						Vehicle->Location+Rotation.RotateVector(FVector(Axle,Side*HalfWidth,Radius)*Fit),FVector(Radius/50.f,Radius/50.f,.20f)*Fit);
			}
			const TCHAR* CarBatches[]={TEXT("ResidentCars"),TEXT("ResidentHatchbacks"),TEXT("ResidentUtilities")};
			const int32 Variant=static_cast<uint32>(FMath::Max(0,Car.ResidentOwnerID))%3u;
			int32 CarBodies=0,BusBodies=0;TArray<UInstancedStaticMeshComponent*> Batches;Traffic->Host->GetComponents(Batches);
			for(const auto* Batch:Batches)for(int32 I=0;I<Batch->GetInstanceCount();++I)
			{
				FTransform Pose;if(!Batch->GetInstanceTransform(I,Pose,true)||Pose.GetScale3D().IsNearlyZero())continue;
				if(Batch->GetName()==TEXT("VehicleWheels"))
				{
					const int32 Match=ExpectedWheels.IndexOfByPredicate([&](const FTransform& Expected){return Pose.Equals(Expected,.001f);});
					if(Match==INDEX_NONE)return false;ExpectedWheels.RemoveAtSwap(Match);
				}
				else
				{
					const bool IsBus=Batch->GetName()==TEXT("ServiceBuses");const auto& Vehicle=IsBus?Bus:Car;
					if(!IsBus&&Batch->GetName()!=CarBatches[Variant])return false;
					const FTransform Expected(FRotator(0,Vehicle.Yaw,0),Vehicle.Location,FVector(Traffic->Host->GetVehicleRenderScale(Vehicle,Grid->CellSize)));
					if(!Pose.Equals(Expected,.001f))return false;if(IsBus)++BusBodies;else ++CarBodies;
				}
			}
			return CarBodies==1&&BusBodies==1&&ExpectedWheels.IsEmpty();
		};
		Traffic->RefreshVisiblePresentation();FAftershockTransportSaveState BeforeDraw;Traffic->CaptureSaveState(BeforeDraw);
		Check(TEXT("expanded_vehicle_draw_masks_far_car_bus_and_exactly_their_eight_wheels"),RenderCounts()==FIntVector(1,1,8)&&RenderMatches(0,1)&&Traffic->Vehicles.Num()==4&&Traffic->Trips.Num()==4);
		Check(TEXT("transport_draw_camera_moves_to_negative_chunk"),SetDrawFocus(FarCar));Traffic->RefreshVisiblePresentation();
		Check(TEXT("vehicle_and_wheels_wait_for_ground_install_in_new_camera_window"),RenderCounts()==FIntVector::ZeroValue);
		People->EnsureTerrainVisualBuilt();Traffic->RefreshVisiblePresentation();
		Check(TEXT("negative_installed_window_renders_same_far_car_bus_and_four_wheels_each"),RenderCounts()==FIntVector(1,1,8)&&RenderMatches(2,3));
		Check(TEXT("transport_draw_camera_returns_to_initial_city"),SetDrawFocus(NearCar));People->EnsureTerrainVisualBuilt();Traffic->RefreshVisiblePresentation();
		FAftershockTransportSaveState AfterDraw;Traffic->CaptureSaveState(AfterDraw);
		Check(TEXT("draw_window_roundtrip_preserves_all_vehicle_trip_ids_and_saved_poses"),RenderCounts()==FIntVector(1,1,8)&&RenderMatches(0,1)&&FAftershockTransportSaveState::StaticStruct()->CompareScriptStruct(&BeforeDraw,&AfterDraw,0));
		Check(TEXT("offwindow_vehicle_remains_a_physical_obstacle"),!Traffic->IsPedestrianSpaceClear(Traffic->Vehicles[2].Location,Traffic->Vehicles[2].Location,32.f,false));
		ClearanceWorld->DestroyWorld(false);GEngine->DestroyWorldContext(ClearanceWorld);
	}
	// Isolated runtime retirement fixture: no legacy snapshot repair. Eight
	// retained bus slots include disabled buses awaiting real curb handoffs.
	UWorld* LifecycleWorld=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("TransitRetirementFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("transit_retirement_fixture_world_created"),LifecycleWorld!=nullptr);
	if(LifecycleWorld)
	{
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(LifecycleWorld);
		auto* LT=LifecycleWorld->GetSubsystem<UTerrainGridSubsystem>();auto* LB=LifecycleWorld->GetSubsystem<UBuildingManagerSubsystem>();
		auto* LL=LifecycleWorld->GetSubsystem<UCityLifeSubsystem>();auto* LC=LifecycleWorld->GetSubsystem<UCrowdRenderSubsystem>();auto* LR=LifecycleWorld->GetSubsystem<UTransportSubsystem>();
		Check(TEXT("transit_retirement_canonical_dependencies"),LT&&LB&&LL&&LC&&LR);
		if(LT&&LB&&LL&&LC&&LR)
		{
			LT->GenerateProceduralTerrain(106,12,1000.f,21682);
			for(auto& Cell:LT->Cells){const FIntPoint Coord=Cell.Coords;Cell=FGridCell();Cell.Coords=Coord;Cell.bIsPlayable=true;Cell.bHasRoad=Coord.Y==5&&Coord.X>0&&Coord.X<105;}
			auto* LifecycleCube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
			const auto DataFor=[&](EBuildingType Type,int32 Residents,int32 Workers,int32 Capacity)
			{auto* Data=NewObject<UBuildingData>(LB);Data->Type=Type;Data->Residents=Residents;Data->Workers=Workers;Data->TransitCapacity=Capacity;Data->MeshIntact=LifecycleCube;Data->PowerDemandUnitsPerTurn=0.f;return Data;};
			auto* HouseData=DataFor(EBuildingType::House,2,0,0);auto* StopData=DataFor(EBuildingType::BusStop,0,0,12);
			auto* TerminalData=DataFor(EBuildingType::BusTerminal,0,2,8);auto* ParkData=DataFor(EBuildingType::GreenSpace,0,0,0);
			const auto PlaceLifecycle=[&](FIntPoint Coord,UBuildingData* Data){int32 ID=INDEX_NONE;return LB->PlaceBuilding(Coord,Data,EFoundationQuality::Medium,1,ID)?ID:INDEX_NONE;};
			TArray<int32> Homes;for(int32 X=2;X<82;++X)Homes.Add(PlaceLifecycle(FIntPoint(X,4),HouseData));
			const int32 Park=PlaceLifecycle(FIntPoint(80,6),ParkData),StopA=PlaceLifecycle(FIntPoint(10,6),StopData),StopB=PlaceLifecycle(FIntPoint(78,6),StopData);
			int32 Terminal=PlaceLifecycle(FIntPoint(50,6),TerminalData);LL->InitializeForCity();LR->RefreshInfrastructure();LC->SynchronizeCanonicalPopulation();
			bool Setup=Homes.Num()==80&&Park!=INDEX_NONE&&StopA!=INDEX_NONE&&StopB!=INDEX_NONE&&Terminal!=INDEX_NONE;
			TArray<FAftershockRoadVehicle> ParkedCars;
			for(int32 Home:Homes)
			{
				const auto* Resident=LL->GetResidents().FindByPredicate([&](const auto& Person){return Person.bAlive&&Person.bWorkingAge&&Person.HomeBuildingID==Home;});FBuildingInstance House;
				if(!Resident||!LB->GetBuildingByID(Home,House)){Setup=false;continue;}
				FAftershockRoadVehicle Car;Car.VehicleID=LR->NextVehicleID++;Car.ResidentOwnerID=Resident->ResidentID;Car.ParkedAtBuildingID=Home;Car.SegmentOriginCell=House.GridCoords+FIntPoint(0,1);Car.Location=LR->RoadLocation(Car.SegmentOriginCell);LR->Vehicles.Add(Car);ParkedCars.Add(Car);
			}
			Check(TEXT("retirement_fixture_has_eighty_canonical_private_cars_with_distinct_real_garages"),Setup&&ParkedCars.Num()==80&&LR->GetTransportStats().Cars==80&&LR->GetTransportStats().Buses==1&&LL->GetState().LivingPopulation==160);
			const auto CarsUnchanged=[&]()
			{for(const auto& Before:ParkedCars){const auto* After=LR->Vehicles.FindByPredicate([&](const auto& Car){return Car.VehicleID==Before.VehicleID;});if(!After||!FAftershockRoadVehicle::StaticStruct()->CompareScriptStruct(&Before,After,0))return false;}return true;};
			const auto RetainedBusCount=[&](){int32 Count=0;for(const auto& Vehicle:LR->Vehicles)Count+=Vehicle.bBus?1:0;return Count;};
			const auto ActiveBus=[&](){return LR->Vehicles.FindByPredicate([](const auto& Vehicle){return Vehicle.bBus&&Vehicle.bActive;});};
			const auto OwnerSnapshotRoundtrip=[&]()
			{FAftershockTransportSaveState Before,After;LR->CaptureSaveState(Before);FText Error;const bool Valid=LR->RestoreSaveState(Before,Error);LR->CaptureSaveState(After);return Valid&&FAftershockTransportSaveState::StaticStruct()->CompareScriptStruct(&Before,&After,0);};
			const auto InitialStats=LR->GetTransportStats();const int64 InitialFare=LL->GetPendingTransitRevenueTL();
			bool FortyBounds=Setup,FortyIDs=Setup,FortyOwners=Setup,FortySnapshots=Setup,FortyStats=Setup;
			int32 PreviousBusID=ActiveBus()?ActiveBus()->VehicleID:INDEX_NONE;
			for(int32 Cycle=0;Cycle<40&&Setup;++Cycle)
			{
				Setup=LB->RemoveBuilding(Terminal);LL->RefreshCity();LR->RefreshInfrastructure();FortyBounds&=LR->Vehicles.Num()==80&&RetainedBusCount()==0;
				Terminal=PlaceLifecycle(FIntPoint(50,6),TerminalData);Setup&=Terminal!=INDEX_NONE;LL->RefreshCity();LR->RefreshInfrastructure();
				const auto* Bus=ActiveBus();FortyBounds&=LR->Vehicles.Num()==81&&RetainedBusCount()==1&&LR->Vehicles.Num()<=88;
				FortyIDs&=Bus&&Bus->VehicleID>PreviousBusID&&LR->NextVehicleID>Bus->VehicleID;if(Bus)PreviousBusID=Bus->VehicleID;
				FortyOwners&=CarsUnchanged()&&LL->GetState().LivingPopulation==160;FortySnapshots&=OwnerSnapshotRoundtrip();
				const auto Stats=LR->GetTransportStats();FortyStats&=FAftershockTransportStats::StaticStruct()->CompareScriptStruct(&InitialStats,&Stats,0)&&LL->GetPendingTransitRevenueTL()==InitialFare;
			}
			Check(TEXT("forty_actual_terminal_demolish_rebuild_cycles_stay_within_eighty_eight_vehicle_records"),Setup&&FortyBounds);
			Check(TEXT("forty_terminal_cycles_preserve_private_ownership_and_monotonic_unique_bus_ids"),FortyIDs&&FortyOwners);
			Check(TEXT("every_terminal_cycle_actual_transport_owner_snapshot_roundtrip_remains_valid"),FortySnapshots);
			Check(TEXT("terminal_retirement_does_not_change_trip_statistics_or_transit_revenue"),FortyStats);
			TSet<int32> UsedResidents;TArray<int32> PinnedBusIDs,PinnedTripIDs;
			const auto BoardRealPassenger=[&](int32& OutResident,int32& OutTrip,int32& OutBus)
			{
				const FCityResidentRecord* Person=nullptr;for(const auto& Resident:LL->GetResidents())
				{FBuildingInstance Home;if(Resident.bAlive&&Resident.bWorkingAge&&!UsedResidents.Contains(Resident.ResidentID)&&LB->GetBuildingByID(Resident.HomeBuildingID,Home)&&Home.GridCoords.X>=7&&Home.GridCoords.X<=12){Person=&Resident;break;}}
				auto* Bus=ActiveBus();FBuildingInstance Home,Destination;if(!Person||!Bus||!LB->GetBuildingByID(Person->HomeBuildingID,Home)||!LB->GetBuildingByID(Park,Destination))return false;
				FAftershockPassengerTrip Trip;if(!LR->PlanTrip(Person->ResidentID,LT->GridToWorld(Home.GridCoords),LT->GridToWorld(Destination.GridCoords),Park,false,Trip)||Trip.Mode!=EAftershockTravelMode::Bus||Trip.PickupStopID!=StopA||Trip.DropoffStopID!=StopB)return false;
				Bus=ActiveBus();if(!Bus||!LR->MarkPassengerAtPickup(Trip.TripID,Person->ResidentID,Trip.PickupLocation))return false;
				// Fixture placement at two real stops exercises the actual boarding,
				// fare and queued-alighting APIs. This is not a driving-speed test.
				Bus->Location=LR->RoadLocation(Trip.PickupRoadCell);Bus->SegmentOriginCell=Trip.PickupRoadCell;LR->BoardBusPassenger(*Bus,StopA);
				if(LR->Trips.FindChecked(Trip.TripID).State!=EAftershockPassengerState::Riding)return false;
				Bus->Location=LR->RoadLocation(Trip.DropoffRoadCell);Bus->SegmentOriginCell=Trip.DropoffRoadCell;Bus->NextServiceStop=Bus->ServiceStopIDs.IndexOfByKey(StopB);LR->Trips.FindChecked(Trip.TripID).CurrentLocation=Bus->Location;LR->HandleBusStop(*Bus);
				auto* Agent=LC->Agents.FindByPredicate([&](const auto& Candidate){return Candidate.ResidentID==Person->ResidentID;});if(!Agent||!Bus->PendingAlightTripIDs.Contains(Trip.TripID))return false;
				Agent->TransportTripID=Trip.TripID;Agent->bInsideVehicle=true;Agent->Activity=EAftershockCitizenActivity::RidingTransport;Agent->MoveSpeed=0.f;Agent->CurrentLocation=Bus->Location;Agent->TargetLocation=Agent->CurrentLocation;Agent->RouteGridCoords.Reset();Agent->NextRouteCoordIndex=0;Agent->bWaitingForOutdoorSpace=false;Agent->TransportFinalDestination=Agent->LeisureLocation;Agent->TransportArrivalActivity=EAftershockCitizenActivity::AtLeisure;
				OutResident=Person->ResidentID;OutTrip=Trip.TripID;OutBus=Bus->VehicleID;UsedResidents.Add(OutResident);return true;
			};
			int32 Passenger=INDEX_NONE,PassengerTrip=INDEX_NONE,RetiredBus=INDEX_NONE;const bool Boarded=BoardRealPassenger(Passenger,PassengerTrip,RetiredBus);
			Check(TEXT("retirement_fixture_boards_real_resident_charges_once_and_enters_real_alighting_queue"),Boarded&&LL->GetPendingTransitRevenueTL()>InitialFare);
			if(Boarded)
			{
				LB->RemoveBuilding(Terminal);Terminal=INDEX_NONE;LL->RefreshCity();LR->RefreshInfrastructure();
				const auto* Retired=LR->Vehicles.FindByPredicate([&](const auto& Bus){return Bus.VehicleID==RetiredBus;});
				Check(TEXT("terminal_loss_retains_disabled_bus_for_terminal_trip_still_hidden_in_real_crowd"),Retired&&!Retired->bActive&&LR->Trips.FindChecked(PassengerTrip).State==EAftershockPassengerState::Stranded&&LC->Agents.ContainsByPredicate([&](const auto& Agent){return Agent.ResidentID==Passenger&&Agent.bInsideVehicle&&Agent.TransportTripID==PassengerTrip;}));
				TArray<FVector> Kerbs;for(int32 Slot=0;Slot<2;++Slot){FVector At;if(LR->GetPassengerKerbLocation(StopB,Slot,true,At))Kerbs.Add(At);}if(Retired)for(float Side:{-1.f,1.f})Kerbs.Add(LR->VehicleDoorLocation(*Retired,Side));
				// Crowd also accepts the already recorded terminal-trip position if
				// it has become physically clear. Occupy that exact real candidate
				// as well, rather than assuming every stranded exit must use a curb.
				Kerbs.Add(LR->Trips.FindChecked(PassengerTrip).CurrentLocation);
				TArray<FCitizenAgent> BlockerBefore;for(const FVector& At:Kerbs)
				{
					if(BlockerBefore.ContainsByPredicate([&](const auto& Before){const auto* Live=LC->Agents.FindByPredicate([&](const auto& Agent){return Agent.ResidentID==Before.ResidentID;});return Live&&FVector::DistSquared2D(Live->CurrentLocation,At)<FMath::Square(60.f);}))continue;
					auto* Blocker=LC->Agents.FindByPredicate([&](const auto& Agent){return Agent.ResidentID!=Passenger&&!Agent.bInsideVehicle&&!BlockerBefore.ContainsByPredicate([&](const auto& Prior){return Prior.ResidentID==Agent.ResidentID;});});if(!Blocker)continue;
					BlockerBefore.Add(*Blocker);Blocker->CurrentLocation=At;Blocker->TargetLocation=At;Blocker->FinalDestinationLocation=At;Blocker->Activity=EAftershockCitizenActivity::AtLeisure;Blocker->MoveSpeed=0.f;Blocker->bWaitingForOutdoorSpace=false;Blocker->OutdoorDepartureBuildingID=INDEX_NONE;Blocker->ScheduleSecondsRemaining=3600.f;Blocker->RouteGridCoords.Reset();
				}
				LC->RebuildCitizenSpatialIndex();FVector Release;const int64 FareBeforeHandoff=LL->GetPendingTransitRevenueTL();const auto StatsBeforeHandoff=LR->GetTransportStats();
				Check(TEXT("retired_bus_passenger_has_no_physical_release_while_real_curb_spaces_are_occupied"),!Kerbs.IsEmpty()&&!LR->FindPassengerReleaseLocation(PassengerTrip,Release));
				for(auto& Agent:LC->Agents)Agent.ScheduleSecondsRemaining=3600.f;LC->UpdateCrowdPositions();
				Check(TEXT("inactive_bus_cleanup_preserves_blocked_curb_handoff_and_transport_snapshot"),LC->Agents.ContainsByPredicate([&](const auto& Agent){return Agent.ResidentID==Passenger&&Agent.bInsideVehicle&&Agent.TransportTripID==PassengerTrip;})&&!LR->PruneInactiveBuses()&&LR->Vehicles.ContainsByPredicate([&](const auto& Bus){return Bus.VehicleID==RetiredBus;})&&OwnerSnapshotRoundtrip());
				for(const auto& Before:BlockerBefore)if(auto* Agent=LC->Agents.FindByPredicate([&](const auto& Candidate){return Candidate.ResidentID==Before.ResidentID;}))*Agent=Before;
				for(auto& Agent:LC->Agents)Agent.ScheduleSecondsRemaining=3600.f;LC->RebuildCitizenSpatialIndex();LC->UpdateCrowdPositions();LR->UpdateTransport();
				const auto StatsAfterHandoff=LR->GetTransportStats();
				Check(TEXT("real_crowd_curb_release_allows_bus_reclamation_without_removing_history_or_recharging_fare"),!LR->Vehicles.ContainsByPredicate([&](const auto& Bus){return Bus.VehicleID==RetiredBus;})&&LR->Trips.Contains(PassengerTrip)&&LR->Trips.FindChecked(PassengerTrip).VehicleID==RetiredBus&&LL->GetPendingTransitRevenueTL()==FareBeforeHandoff&&StatsAfterHandoff.BusBoardings==StatsBeforeHandoff.BusBoardings&&StatsAfterHandoff.StrandedTrips==StatsBeforeHandoff.StrandedTrips&&CarsUnchanged());
			}
			bool EightPinned=true;for(int32 Slot=0;Slot<8;++Slot)
			{
				Terminal=PlaceLifecycle(FIntPoint(50,6),TerminalData);LL->RefreshCity();LR->RefreshInfrastructure();int32 Resident=INDEX_NONE,Trip=INDEX_NONE,Bus=INDEX_NONE;
				if(Terminal==INDEX_NONE||!BoardRealPassenger(Resident,Trip,Bus)){EightPinned=false;break;}PinnedBusIDs.Add(Bus);PinnedTripIDs.Add(Trip);
				LB->RemoveBuilding(Terminal);Terminal=INDEX_NONE;LL->RefreshCity();LR->RefreshInfrastructure();EightPinned&=RetainedBusCount()==Slot+1&&LR->Vehicles.Num()==81+Slot&&LR->Vehicles.Num()<=88;
			}
			const int32 NextBeforeNinth=LR->NextVehicleID;Terminal=PlaceLifecycle(FIntPoint(50,6),TerminalData);LL->RefreshCity();LR->RefreshInfrastructure();
			Check(TEXT("eight_retained_hidden_handoffs_apply_bus_slot_backpressure_before_a_ninth_bus_is_created"),EightPinned&&PinnedBusIDs.Num()==8&&LR->Vehicles.Num()==88&&RetainedBusCount()==8&&ActiveBus()==nullptr&&LR->NextVehicleID==NextBeforeNinth&&OwnerSnapshotRoundtrip());
			const int64 FareBeforeAllRelease=LL->GetPendingTransitRevenueTL();for(auto& Agent:LC->Agents)Agent.ScheduleSecondsRemaining=3600.f;
			for(int32 Step=0;Step<600;++Step)
			{
				bool Pending=false;for(const auto& Agent:LC->Agents)Pending|=Agent.bInsideVehicle&&PinnedTripIDs.Contains(Agent.TransportTripID);if(!Pending)break;
				LC->UpdateCrowdPositions();LR->UpdateTransport();
			}
			bool AllReleased=EightPinned;for(int32 ID:PinnedBusIDs)AllReleased&=!LR->Vehicles.ContainsByPredicate([&](const auto& Bus){return Bus.VehicleID==ID;});
			for(int32 I=0;I<PinnedTripIDs.Num();++I)AllReleased&=LR->Trips.Contains(PinnedTripIDs[I])&&LR->Trips.FindChecked(PinnedTripIDs[I]).VehicleID==PinnedBusIDs[I];
			Check(TEXT("eight_actual_handoffs_eventually_release_slots_preserving_history_fares_private_cars_and_snapshot_validity"),AllReleased&&LR->Vehicles.Num()<=88&&LL->GetPendingTransitRevenueTL()==FareBeforeAllRelease&&CarsUnchanged()&&OwnerSnapshotRoundtrip());
		}
		LifecycleWorld->DestroyWorld(false);GEngine->DestroyWorldContext(LifecycleWorld);
	}
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_TRANSPORT_AUDIT result=%s checks=%d"),Passed?TEXT("PASS"):TEXT("FAIL"),Checks);
	W->DestroyWorld(false);GEngine->DestroyWorldContext(W);return Passed?0:1;
}
