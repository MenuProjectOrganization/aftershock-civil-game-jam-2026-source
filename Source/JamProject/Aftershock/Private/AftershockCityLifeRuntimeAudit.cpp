#include "AftershockCityLifeRuntimeAudit.h"

#if !UE_BUILD_SHIPPING
#include "Aftershock/Public/TransportSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Citizens/CitizenInstanceManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "TimerManager.h"
#include "UnrealClient.h"

namespace
{
	struct FLiveCityAuditState { TMap<int32,FVector> LastVehiclePositions; };
	void CaptureLiveCity(UWorld& W,int32 Stage,FLiveCityAuditState& Previous)
	{
		if(W.bIsTearingDown||!GEngine||!GEngine->GetWorldContextFromWorld(&W))return;
		const auto* Life=W.GetSubsystem<UCityLifeSubsystem>();const auto* Crowd=W.GetSubsystem<UCrowdRenderSubsystem>();
		const auto* Transport=W.GetSubsystem<UTransportSubsystem>();const auto* Terrain=W.GetSubsystem<UTerrainGridSubsystem>();
		const auto* Buildings=W.GetSubsystem<UBuildingManagerSubsystem>();
		if(!Life||!Crowd||!Transport||!Terrain||!Buildings||!Life->IsCityInitialized())
		{UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_CITY_LIVE_AUDIT stage=%d initialized=0"),Stage);return;}
		Transport->AuditTransport();
		const auto Stats=Transport->GetTransportStats();const auto Agents=Crowd->GetCitizenDebugRoster();const auto Trips=Transport->GetTripRoster();
		TSet<int32> ResidentIDs;int32 InvalidResidents=0,InvalidCitizenCells=0,HiddenPassengers=0;
		for(const auto& Agent:Agents)
		{
			FCityResidentRecord R;
			InvalidResidents+=!Life->GetResident(Agent.ResidentID,R)||!R.bAlive||ResidentIDs.Contains(Agent.ResidentID)?1:0;
			ResidentIDs.Add(Agent.ResidentID);HiddenPassengers+=Agent.bInsideVehicle?1:0;
			const FIntPoint C=Terrain->WorldToGrid(Agent.CurrentLocation);
			const FGridCell Cell=Terrain->GetCell(C);FBuildingInstance Building;
			const bool Park=Cell.OccupyingBuildingID!=INDEX_NONE&&Buildings->GetBuildingByID(Cell.OccupyingBuildingID,Building)&&Building.BuildingData&&Building.BuildingData->Type==EBuildingType::GreenSpace;
			const bool InvalidGeometry=!Terrain->IsPlayableCoord(C)||Cell.bIsOcean||(Cell.bIsRiverbed&&!(Cell.bHasRoad&&Cell.BridgeID!=INDEX_NONE))||(Cell.OccupyingBuildingID!=INDEX_NONE&&!Park);
			InvalidCitizenCells+=!Agent.bInsideVehicle&&InvalidGeometry?1:0;
		}
		int32 CrowdHosts=0,TransportHosts=0,TickingHosts=0,LegacyInstances=0,BodyInstances=0,VisibleBodyInstances=0,EmotionInstances=0,RenderReady=0,VisibleCars=0,VisibleBuses=0;
		for(TActorIterator<AAftershockCrowdHostActor> It(&W);It;++It)
		{
			++CrowdHosts;TickingHosts+=It->PrimaryActorTick.bCanEverTick&&It->PrimaryActorTick.IsTickFunctionRegistered()&&It->IsActorTickEnabled()?1:0;
			BodyInstances+=It->CitizenISM?It->CitizenISM->GetInstanceCount():0;EmotionInstances+=It->EmotionISM?It->EmotionISM->GetInstanceCount():0;
			if(It->CitizenISM)for(int32 I=0;I<It->CitizenISM->GetInstanceCount();++I){FTransform Transform;if(It->CitizenISM->GetInstanceTransform(I,Transform,true)&&!Transform.GetScale3D().IsNearlyZero())++VisibleBodyInstances;}
			RenderReady+=It->CitizenISM&&It->CitizenISM->IsVisible()&&It->CitizenISM->IsRenderStateCreated()&&It->CitizenISM->GetStaticMesh()?1:0;
		}
		for(TActorIterator<ACitizenInstanceManager> It(&W);It;++It)LegacyInstances+=It->CitizenMesh?It->CitizenMesh->GetInstanceCount():0;
		for(TActorIterator<AAftershockTransportHost> It(&W);It;++It)
		{
			++TransportHosts;TickingHosts+=It->PrimaryActorTick.bCanEverTick&&It->PrimaryActorTick.IsTickFunctionRegistered()&&It->IsActorTickEnabled()?1:0;
			TArray<UInstancedStaticMeshComponent*> Components;It->GetComponents(Components);
			for(auto* ISM:Components)
			{
				if(!ISM->GetStaticMesh()||!ISM->IsRenderStateCreated())continue;
				const bool Car=ISM->GetName().Contains(TEXT("ResidentCars")),Bus=ISM->GetName().Contains(TEXT("ServiceBuses"));
				if(!Car&&!Bus)continue;
				for(int32 I=0;I<ISM->GetInstanceCount();++I)
				{
					FTransform Transform;if(!ISM->GetInstanceTransform(I,Transform,true)||Transform.GetScale3D().IsNearlyZero())continue;
					if(Car)++VisibleCars;else ++VisibleBuses;
				}
			}
		}
		int32 BoardedBus=Stats.BusBoardings,ArrivedBus=Stats.BusArrivals,CarTrips=Stats.PrivateCarTrips,MovedVehicles=0,CapacityViolations=0;
		FString TripJSON=TEXT("[");bool First=true;
		for(const auto& P:Trips)
		{
			if(!First)TripJSON+=TEXT(",");First=false;
			TripJSON+=FString::Printf(TEXT("{\"trip\":%d,\"resident\":%d,\"mode\":%d,\"state\":%d,\"vehicle\":%d,\"pickup\":%d,\"dropoff\":%d}"),P.TripID,P.ResidentID,static_cast<int32>(P.Mode),static_cast<int32>(P.State),P.VehicleID,P.PickupStopID,P.DropoffStopID);
		}
		TripJSON+=TEXT("]");
		FString VehicleJSON=TEXT("[");First=true;
		for(const auto& V:Transport->GetVehicleRoster())if(V.bActive)
		{
			if(const FVector* Old=Previous.LastVehiclePositions.Find(V.VehicleID))MovedVehicles+=FVector::DistSquared2D(*Old,V.Location)>100.f?1:0;
			Previous.LastVehiclePositions.Add(V.VehicleID,V.Location);
			CapacityViolations+=V.PassengerTripIDs.Num()>V.Capacity?1:0;
			if(!First)VehicleJSON+=TEXT(",");First=false;
			const float Operation=V.bBus?Life->GetBuildingStatus(V.TerminalBuildingID).Operation01:1.f;
			const FIntPoint Next=V.RoadPath.IsValidIndex(V.NextPathStep)?V.RoadPath[V.NextPathStep]:Terrain->WorldToGrid(V.Location);
			VehicleJSON+=FString::Printf(TEXT("{\"id\":%d,\"owner\":%d,\"bus\":%s,\"capacity\":%d,\"occupants\":%d,\"terminalOperation01\":%.4f,\"x\":%.2f,\"y\":%.2f,\"yaw\":%.2f,\"segmentOriginX\":%d,\"segmentOriginY\":%d,\"nextRoadX\":%d,\"nextRoadY\":%d,\"remainingRoadSteps\":%d,\"dwellSeconds\":%.3f,\"stalled\":%s}"),V.VehicleID,V.ResidentOwnerID,V.bBus?TEXT("true"):TEXT("false"),V.Capacity,V.PassengerTripIDs.Num(),Operation,V.Location.X,V.Location.Y,V.Yaw,V.SegmentOriginCell.X,V.SegmentOriginCell.Y,Next.X,Next.Y,FMath::Max(0,V.RoadPath.Num()-V.NextPathStep),V.DwellSeconds==MAX_flt?-1.f:V.DwellSeconds,V.bStalled?TEXT("true"):TEXT("false"));
		}
		VehicleJSON+=TEXT("]");
		const bool Valid=InvalidResidents==0&&InvalidCitizenCells==0&&Stats.InvalidRoadSteps==0&&CapacityViolations==0&&CrowdHosts==1&&TransportHosts==1&&TickingHosts==0&&LegacyInstances==0&&BodyInstances==Agents.Num()&&RenderReady==1&&Agents.Num()==FMath::Min(1500,Life->GetState().LivingPopulation);
		FString Report=FString::Printf(TEXT("{\n\"stageSeconds\":%d,\"worldSeconds\":%.3f,\"valid\":%s,\"livingPopulation\":%d,\"renderedResidents\":%d,\"invalidResidents\":%d,\"invalidCitizenCells\":%d,\"crowdHosts\":%d,\"transportHosts\":%d,\"tickingHosts\":%d,\"legacyCitizenInstances\":%d,\"renderReadyCrowdHosts\":%d,\"emotionInstances\":%d,\"hiddenActualPassengers\":%d,\"visibleCarBodies\":%d,\"visibleBusBodies\":%d,\"busBoardings\":%d,\"busArrivals\":%d,\"privateCarTrips\":%d,\"lastTurnTransitIncomeTL\":%lld,\"pendingTransitRevenueTL\":%lld,\"unsafeRemainingRoadSteps\":%d,\"capacityViolations\":%d,\"vehiclesMovedSincePriorCapture\":%d,\n\"vehicles\":%s,\n\"trips\":%s\n}\n"),
			Stage,W.GetTimeSeconds(),Valid?TEXT("true"):TEXT("false"),Life->GetState().LivingPopulation,Agents.Num(),InvalidResidents,InvalidCitizenCells,CrowdHosts,TransportHosts,TickingHosts,LegacyInstances,RenderReady,EmotionInstances,HiddenPassengers,VisibleCars,VisibleBuses,BoardedBus,ArrivedBus,CarTrips,Life->GetState().LastTransitIncomeTL,Life->GetPendingTransitRevenueTL(),Stats.InvalidRoadSteps,CapacityViolations,MovedVehicles,*VehicleJSON,*TripJSON);
		const auto Spacing=Crowd->GetCrowdSpacingStats();
		Report.RemoveFromEnd(TEXT("}\n"));
		Report+=FString::Printf(TEXT(",\"citizenInstanceSlots\":%d,\"visibleOutdoorBodyInstances\":%d,\"indoorResidents\":%d,\"outdoorResidents\":%d,\"spacingNeighbourChecks\":%d,\"spacingCorrections\":%d,\"sampledClosePairs\":%d,\"maxSpacingBucketOccupancy\":%d}\n"),BodyInstances,VisibleBodyInstances,Spacing.IndoorAgents,Spacing.OutdoorAgents,Spacing.NeighbourChecks,Spacing.AcceptedCorrections,Spacing.ObservedClosePairs,Spacing.MaxBucketOccupancy);
		const FString Folder=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation"));
		IFileManager::Get().MakeDirectory(*Folder,true);
		FFileHelper::SaveStringToFile(Report,*(Folder/FString::Printf(TEXT("city-life-runtime-%d.json"),Stage)));
		FScreenshotRequest::RequestScreenshot(Folder/FString::Printf(TEXT("city-life-runtime-%d.png"),Stage),true,false);
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITY_LIVE_AUDIT stage=%d valid=%d population=%d citizenRosterSlots=%d emoji=%d renderedCars=%d renderedBuses=%d actualBusBoardings=%d actualBusArrivals=%d privateCarTrips=%d lastTurnFaresTL=%lld pendingFaresTL=%lld roadInvalid=%d capacityInvalid=%d movedSincePrior=%d"),Stage,Valid,Life->GetState().LivingPopulation,Agents.Num(),EmotionInstances,VisibleCars,VisibleBuses,BoardedBus,ArrivedBus,CarTrips,Life->GetState().LastTransitIncomeTL,Life->GetPendingTransitRevenueTL(),Stats.InvalidRoadSteps,CapacityViolations,MovedVehicles);
	}
}
#endif

void BeginAftershockCityLifeRuntimeAudit(UWorld& World)
{
#if !UE_BUILD_SHIPPING
	if(!FParse::Param(FCommandLine::Get(),TEXT("AftershockCityLifeRuntimeAudit")))return;
	TWeakObjectPtr<UWorld> Weak(&World);const TSharedRef<FLiveCityAuditState> State=MakeShared<FLiveCityAuditState>();
	for(int32 Stage:{45,90})
	{
		FTimerHandle Handle;World.GetTimerManager().SetTimer(Handle,FTimerDelegate::CreateLambda([Weak,State,Stage]()
		{if(UWorld* Current=Weak.Get())CaptureLiveCity(*Current,Stage,*State);}),static_cast<float>(Stage),false);
	}
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITY_LIVE_AUDIT_ARMED stages=45,90 realResidents=1 noSimulationMutation=1"));
#endif
}
