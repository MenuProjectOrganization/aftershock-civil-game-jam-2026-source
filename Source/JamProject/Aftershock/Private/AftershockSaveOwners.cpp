#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Quiz/QuizSubsystem.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Engine/World.h"
#include "TimerManager.h"

void UBuildingManagerSubsystem::CaptureSaveState(FAftershockBuildingsSaveState& Out) const
{
 Out={};Out.NextBuildingID=NextBuildingID;
 for(const auto& Pair:ActiveBuildings)
 {
  FAftershockBuildingSaveRecord R;R.State=Pair.Value;
  if(!R.State.BuildingData)continue;
  R.DefinitionType=R.State.BuildingData->Type;
  if(const auto* Batch=BuildingISMs.Find(R.State.BuildingData))if(*Batch)(*Batch)->GetInstanceTransform(R.State.ISMInstanceIndex,R.WorldTransform,true);
  R.Yaw=R.WorldTransform.Rotator().Yaw;R.State.BuildingData=nullptr;R.State.ISMInstanceIndex=INDEX_NONE;Out.Buildings.Add(R);
 }
 Out.Buildings.Sort([](const auto&A,const auto&B){return A.State.InstanceID<B.State.InstanceID;});
}
bool UBuildingManagerSubsystem::RestoreSaveState(const FAftershockBuildingsSaveState& Saved,FText& Error)
{
 auto* Terrain=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();TSet<int32> IDs;TSet<FIntPoint> Coords;
 for(const auto& R:Saved.Buildings)
 {
  if(!Terrain||R.State.InstanceID<1||R.State.InstanceID>=Saved.NextBuildingID||IDs.Contains(R.State.InstanceID)||Coords.Contains(R.State.GridCoords)
   ||!Terrain->IsPlayableCoord(R.State.GridCoords)||static_cast<uint8>(R.DefinitionType)>23||R.WorldTransform.ContainsNaN())
  {Error=FText::FromString(TEXT("Bina kaydı veya kimlikleri geçersiz."));return false;}
  IDs.Add(R.State.InstanceID);Coords.Add(R.State.GridCoords);
 }
 InvalidateBuildingPresentation();
 for(auto& Pair:BuildingISMs)if(Pair.Value)Pair.Value->ClearInstances();ActiveBuildings.Reset();NextBuildingID=Saved.NextBuildingID;
 for(const auto& R:Saved.Buildings)
 {
  FBuildingInstance B=R.State;B.BuildingData=GetCatalogData(R.DefinitionType);auto* Batch=GetOrCreateISM(B.BuildingData);
  if(!Batch){Error=FText::FromString(TEXT("Bina görseli yüklenemedi."));return false;}
  B.ISMInstanceIndex=Batch->AddInstance(R.WorldTransform,true);ActiveBuildings.Add(B.InstanceID,B);UpdateBuildingVisualState(B.InstanceID);
 }
 return true;
}
void UEconomyAndStateSubsystem::CaptureSaveState(FAftershockEconomySaveState& Out) const
{
 Out.Turn=CurrentTurn;Out.Year=CurrentYear;Out.Treasury=TreasuryTL;Out.Satisfaction=PublicSatisfaction;Out.CO2=CityCO2Level;
 Out.ClimateRisk=ClimateRiskMultiplier;Out.DisasterLevel=CurrentDisasterLevel;Out.PendingAid=PendingStateAidTL;Out.bGameOver=bIsGameOver;Out.SurvivedDisasters=SuccessfulDisastersSurvived;
 CaptureAppliedDisasterEvents(Out.AppliedDisasterEventIDs);
}
bool UEconomyAndStateSubsystem::RestoreSaveState(const FAftershockEconomySaveState& S,FText& Error)
{
 if(S.Turn<1||S.Year<1900||!FMath::IsFinite(S.Satisfaction)||S.Satisfaction<0||S.Satisfaction>100||!FMath::IsFinite(S.CO2)||!FMath::IsFinite(S.ClimateRisk))
 {Error=FText::FromString(TEXT("Ekonomi kaydı geçersiz."));return false;}
 CurrentTurn=S.Turn;CurrentYear=S.Year;TreasuryTL=S.Treasury;PublicSatisfaction=S.Satisfaction;CityCO2Level=S.CO2;ClimateRiskMultiplier=S.ClimateRisk;
 if(!RestoreAppliedDisasterEvents(S.AppliedDisasterEventIDs,Error))return false;
 CurrentDisasterLevel=S.DisasterLevel;PendingStateAidTL=S.PendingAid;bIsGameOver=S.bGameOver;SuccessfulDisastersSurvived=S.SurvivedDisasters;bAdvancingTurn=false;return true;
}
void UCityLifeSubsystem::CaptureSaveState(FAftershockCityLifeSaveState& Out) const
{
 Out.bInitialized=bInitializedForCity;Out.Residents=Residents;Out.State=State;Out.NextResidentID=NextResidentID;Out.SeededHomes=SeededHomes;
 Out.SupplyDisruptionTurns=SupplyDisruptionTurns;Out.LastServiceVisitTurn=LastServiceVisitTurn;Out.PaidTransitTrips=PaidTransitTrips;
 Out.PendingTransitRevenueTL=PendingTransitRevenueTL;Out.BuildingStates=BuildingStates;Out.BuildingRoadComponents=BuildingRoadComponents;
}
bool UCityLifeSubsystem::RestoreSaveState(const FAftershockCityLifeSaveState& S,FText& Error)
{
 TSet<int32> IDs;int32 Living=0;
 for(const auto& R:S.Residents)
 {
  if(R.ResidentID<1||R.ResidentID>=S.NextResidentID||IDs.Contains(R.ResidentID)||!FMath::IsFinite(R.Health01)||!FMath::IsFinite(R.Hunger01)||!FMath::IsFinite(R.Happiness01))
  {Error=FText::FromString(TEXT("Vatandaş kimliği/ihtiyaç kaydı geçersiz."));return false;}
  IDs.Add(R.ResidentID);Living+=R.bAlive?1:0;
 }
 if(Living!=S.State.LivingPopulation){Error=FText::FromString(TEXT("Kayıttaki nüfus sayımı uyuşmuyor."));return false;}
 InvalidateScopedOccupancyLookup();
 bInitializedForCity=S.bInitialized;Residents=S.Residents;State=S.State;NextResidentID=S.NextResidentID;SeededHomes=S.SeededHomes;
 SupplyDisruptionTurns=S.SupplyDisruptionTurns;LastServiceVisitTurn=S.LastServiceVisitTurn;PaidTransitTrips=S.PaidTransitTrips;PendingTransitRevenueTL=S.PendingTransitRevenueTL;
 BuildingStates=S.BuildingStates;BuildingRoadComponents=S.BuildingRoadComponents;bRefreshing=false;bAdvancing=false;
 IndexedResidentCount=INDEX_NONE;ResidentIndexLookup.Reset();
 // Restore presentation from the imported canonical service state. Recomputing
 // services here would reassign jobs/seed homes midway through the transaction.
 if(auto* Buildings=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>())
  for(const auto& Pair:BuildingStates)Buildings->SetBuildingOperationalVisual(Pair.Key,Pair.Value.PowerCoverage01,Pair.Value.Operation01);
 return true;
}
void UTransportSubsystem::CaptureSaveState(FAftershockTransportSaveState& Out) const
{
 Out.Trips=GetTripRoster();Out.Vehicles=Vehicles;Out.Stops=Stops;Out.bEmergencyActive=bEmergencyActive;Out.NextTripID=NextTripID;Out.NextVehicleID=NextVehicleID;
 Out.HistoryClockSeconds=HistoryClockSeconds;Out.RetiredTripStats=RetiredTripStats;
 Out.bInfrastructureDirty=bInfrastructureDirty;Out.InfrastructureRefreshSeconds=InfrastructureRefreshSeconds;Out.HistoryRefreshSeconds=HistoryRefreshSeconds;
}
bool UTransportSubsystem::RestoreSaveState(const FAftershockTransportSaveState& S,FText& Error)
{
 TSet<int32> IDs;
 for(const auto& P:S.Trips)if(P.TripID<1||P.TripID>=S.NextTripID||IDs.Contains(P.TripID)||P.CurrentLocation.ContainsNaN())
 {Error=FText::FromString(TEXT("Yolculuk kaydı geçersiz."));return false;}else IDs.Add(P.TripID);
 IDs.Reset();for(const auto& V:S.Vehicles)if(V.VehicleID<1||V.VehicleID>=S.NextVehicleID||IDs.Contains(V.VehicleID)||V.Location.ContainsNaN()||V.NextPathStep<0||V.NextPathStep>V.RoadPath.Num())
 {Error=FText::FromString(TEXT("Araç kaydı geçersiz."));return false;}else IDs.Add(V.VehicleID);
 // Public owner imports must be safe independently of the aggregate save UI.
 // Validate every pending handoff before replacing any trip, vehicle or timer.
 for(const auto& V:S.Vehicles)
 {
  if(!FMath::IsFinite(V.AlightReleaseSeconds)||V.AlightReleaseSeconds<0.f||V.AlightReleaseSeconds>.5f
   ||(V.PendingAlightTripIDs.IsEmpty()&&V.AlightReleaseSeconds!=0.f)
   ||(!V.PendingAlightTripIDs.IsEmpty()&&(!V.bBus||!V.bActive||V.bStalled||!V.ServiceStopIDs.IsValidIndex(V.NextServiceStop))))
  {Error=FText::FromString(TEXT("Araç iniş kuyruğu veya saati geçersiz."));return false;}
  if(V.PendingAlightTripIDs.IsEmpty())continue; // Legacy snapshots have empty default queues.
  const int32 StopID=V.ServiceStopIDs[V.NextServiceStop];
  const auto* Stop=S.Stops.FindByPredicate([StopID](const auto& Candidate){return Candidate.BuildingID==StopID;});
  if(!Stop||Stop->bTerminal){Error=FText::FromString(TEXT("İniş kuyruğunun gerçek durağı eksik."));return false;}
  TSet<int32> Pending;
  for(int32 ID:V.PendingAlightTripIDs)
  {
   const auto* P=S.Trips.FindByPredicate([ID](const auto& Candidate){return Candidate.TripID==ID;});
   if(Pending.Contains(ID)||!V.PassengerTripIDs.Contains(ID)||!P||P->State!=EAftershockPassengerState::Riding
    ||P->Mode!=EAftershockTravelMode::Bus||P->VehicleID!=V.VehicleID||P->DropoffStopID!=StopID||P->DropoffRoadCell!=Stop->RoadCell)
   {Error=FText::FromString(TEXT("İniş kuyruğu otobüs, durak veya gerçek yolcusuyla eşleşmiyor."));return false;}
   Pending.Add(ID);
  }
 }
 FAftershockTransportSaveState PlacementState=S;
 if(!ReconcileVehiclePlacements(PlacementState,Error))return false;
 Trips.Reset();for(const auto& P:PlacementState.Trips)Trips.Add(P.TripID,P);Vehicles=PlacementState.Vehicles;Stops=S.Stops;bEmergencyActive=S.bEmergencyActive;
 NextTripID=S.NextTripID;NextVehicleID=S.NextVehicleID;HistoryClockSeconds=S.HistoryClockSeconds;RetiredTripStats=S.RetiredTripStats;bInfrastructureDirty=S.bInfrastructureDirty;InfrastructureRefreshSeconds=S.InfrastructureRefreshSeconds;HistoryRefreshSeconds=S.HistoryRefreshSeconds;
 ResetMunicipalPresentationAfterRestore();
 if(Host)Host->DrawFleet(Vehicles);return true;
}
void UAftershockScenarioSubsystem::CaptureSaveState(FAftershockScenarioSaveState& Out) const
{
 Out.ScenarioIndex=ScenarioIndex;Out.bPrimaryDisasterSurvived=bPrimaryDisasterSurvived;Out.AssemblyCells=AssemblyCells;Out.StarterRoadCellCount=StarterRoadCellCount;
 Out.Name=Definition.Name;Out.Briefing=Definition.Briefing;Out.Hazard=Definition.Hazard;Out.Seed=Definition.Seed;Out.StartYear=Definition.StartYear;
 Out.StartingBudgetTL=Definition.StartingBudgetTL;Out.BaseSeverity=Definition.BaseSeverity;Out.InitialAnomalyC=Definition.InitialAnomalyC;
 Out.GeographyIndex=Definition.GeographyIndex;Out.Latitude=Definition.Latitude;Out.Longitude=Definition.Longitude;
}
bool UAftershockScenarioSubsystem::RestoreSaveState(const FAftershockScenarioSaveState& S,FText& Error)
{
 ScenarioIndex=S.ScenarioIndex;bPrepared=true;bPrimaryDisasterSurvived=S.bPrimaryDisasterSurvived;AssemblyCells=S.AssemblyCells;
 StarterRoadCellCount=S.StarterRoadCellCount;bStarterRoadsInitialized=true;
 Definition.Name=S.Name;Definition.Briefing=S.Briefing;Definition.Hazard=S.Hazard;Definition.Seed=S.Seed;Definition.StartYear=S.StartYear;
 Definition.StartingBudgetTL=S.StartingBudgetTL;Definition.BaseSeverity=S.BaseSeverity;Definition.InitialAnomalyC=S.InitialAnomalyC;
 Definition.GeographyIndex=S.GeographyIndex;Definition.Latitude=S.Latitude;Definition.Longitude=S.Longitude;
 if(AssemblyMarkerHost){AssemblyMarkerHost->Destroy();AssemblyMarkerHost=nullptr;}BuildAssemblyMarkers();return true;
}
bool UWorldClimateSubsystem::RestoreSaveState(const FAftershockWorldClimateState& S,FText& Error)
{
 if(!FMath::IsFinite(S.InitialAnomalyC)||!FMath::IsFinite(S.GlobalTemperatureAnomalyC)||!FMath::IsFinite(S.AnnualTrendC))
 {Error=FText::FromString(TEXT("İklim kaydı geçersiz."));return false;}State=S;return true;
}
void UQuizSubsystem::CaptureSaveState(FAftershockQuizSaveState& Out) const
{Out.CurrentQuestion=CurrentQuestion;Out.AskedRowNames=AskedRowNames;Out.AnsweredCount=AnsweredCount;Out.CorrectCount=CorrectCount;Out.bQuestionPending=bQuestionPending;Out.LastAskedTurn=LastAskedTurn;Out.RandomSeed=QuestionRandom.GetCurrentSeed();}
bool UQuizSubsystem::RestoreSaveState(const FAftershockQuizSaveState& S,FText& Error)
{
 if(S.CorrectCount<0||S.AnsweredCount<S.CorrectCount){Error=FText::FromString(TEXT("Eğitim kaydı geçersiz."));return false;}
 CurrentQuestion=S.CurrentQuestion;AskedRowNames=S.AskedRowNames;AnsweredCount=S.AnsweredCount;CorrectCount=S.CorrectCount;bQuestionPending=S.bQuestionPending;LastAskedTurn=S.LastAskedTurn;QuestionRandom.Initialize(S.RandomSeed);return true;
}
void AAftershockCameraPawn::CaptureSaveState(FAftershockCameraSaveState& Out) const
{
 Out.Location=GetActorLocation();Out.Rotation=GetActorRotation();Out.Orbit=SpringArm?SpringArm->GetRelativeRotation():FRotator(-48,-45,0);
 Out.Zoom=CurrentZoomDistance;Out.OverviewBlend=CurrentOverviewBlend;Out.LocalFocus=LocalMapFocus;Out.SavedLocalZoom=SavedLocalZoomDistance;Out.PlanetOrbitPitch=PlanetOrbitPitch;
}
bool AAftershockCameraPawn::RestoreSaveState(const FAftershockCameraSaveState& S,FText& Error)
{
 if(S.Location.ContainsNaN()||S.Rotation.ContainsNaN()||!FMath::IsFinite(S.Zoom)||S.Zoom<500.f||!FMath::IsFinite(S.SavedLocalZoom)||S.SavedLocalZoom<500.f||!FMath::IsFinite(S.PlanetOrbitPitch)||S.PlanetOrbitPitch< -85||S.PlanetOrbitPitch>70)
 {Error=FText::FromString(TEXT("Kamera kaydı geçersiz."));return false;}
 SetActorLocationAndRotation(S.Location,S.Rotation);LocalMapFocus=S.LocalFocus;CurrentZoomDistance=S.Zoom;DesiredArmLength=S.Zoom;CurrentOverviewBlend=S.OverviewBlend;SavedLocalZoomDistance=S.SavedLocalZoom;PlanetOrbitPitch=S.PlanetOrbitPitch;
 if(SpringArm){SpringArm->TargetArmLength=S.Zoom+PlanetRadius*S.OverviewBlend;SpringArm->SetRelativeRotation(S.Orbit);}return true;
}
