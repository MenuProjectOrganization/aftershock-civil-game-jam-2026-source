#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"
#include "TransportSubsystem.generated.h"

class UInstancedStaticMeshComponent;
class UTerrainGridSubsystem;
class UAftershockTransportAuditCommandlet;
class UAftershockMunicipalAuditCommandlet;
struct FBuildingInstance;

UENUM(BlueprintType)
enum class EAftershockTravelMode : uint8 { Walking, PrivateCar, Bus };
UENUM(BlueprintType)
enum class EAftershockPassengerState : uint8 { WalkingToPickup, Waiting, Riding, Arrived, Cancelled, Stranded };

struct FAftershockTransportSaveState;

/** Derived presentation, deliberately outside frozen V4/V5 save descriptors. */
enum class EAftershockMunicipalService : uint8 { None, FireResponse, Ambulance, PolicePatrol, CourtTransport, MunicipalUtility };
struct FAftershockMunicipalDutyView
{
	int32 OwnerBuildingID=INDEX_NONE;
	int32 ResidentID=INDEX_NONE;
	EAftershockMunicipalService Service=EAftershockMunicipalService::None;
	FVector VehicleLocation=FVector::ZeroVector;
	bool bRiding=false;
	bool bOnScene=false;
};

/** One actual CityLife resident's trip, never a synthetic traffic population. */
USTRUCT(BlueprintType)
struct FAftershockPassengerTrip
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) int32 TripID=INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) int32 ResidentID=INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) int32 DestinationBuildingID=INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) int32 VehicleID=INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) int32 PickupStopID=INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) int32 DropoffStopID=INDEX_NONE;
	/** Reserved deterministic waiting spot, not an extra resident. */
	UPROPERTY(BlueprintReadOnly) int32 PickupQueueSlot=INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) EAftershockTravelMode Mode=EAftershockTravelMode::Walking;
	UPROPERTY(BlueprintReadOnly) EAftershockPassengerState State=EAftershockPassengerState::WalkingToPickup;
	UPROPERTY(BlueprintReadOnly) FIntPoint PickupRoadCell=FIntPoint(MAX_int32,MAX_int32);
	UPROPERTY(BlueprintReadOnly) FIntPoint DropoffRoadCell=FIntPoint(MAX_int32,MAX_int32);
	UPROPERTY(BlueprintReadOnly) FVector CurrentLocation=FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly) FVector PickupLocation=FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly) FVector DropoffLocation=FVector::ZeroVector;
	/** History retention clock only; active trips are never removed by history pruning. */
	UPROPERTY() float TerminalObservedAtSeconds=-1.f;
};

USTRUCT()
struct FAftershockTransitStop
{
	GENERATED_BODY()
	UPROPERTY() int32 BuildingID=INDEX_NONE;
	UPROPERTY() FIntPoint RoadCell=FIntPoint(MAX_int32,MAX_int32);
	UPROPERTY() int32 Capacity=0;
	UPROPERTY() bool bTerminal=false;
};

USTRUCT()
struct FAftershockRoadVehicle
{
	GENERATED_BODY()
	UPROPERTY() int32 VehicleID=INDEX_NONE;
	UPROPERTY() int32 ResidentOwnerID=INDEX_NONE;
	UPROPERTY() int32 TerminalBuildingID=INDEX_NONE;
	UPROPERTY() int32 Capacity=1;
	UPROPERTY() bool bBus=false;
	UPROPERTY() bool bActive=true;
	UPROPERTY() bool bStalled=false;
	/** The same owned car is inside a real destination's finite offstreet parking. */
	UPROPERTY() int32 ParkedAtBuildingID=INDEX_NONE;
	UPROPERTY() FVector Location=FVector::ZeroVector;
	UPROPERTY() float Yaw=0.f;
	UPROPERTY() float WheelAngle=0.f;
	UPROPERTY() float DwellSeconds=0.f;
	UPROPERTY() FIntPoint SegmentOriginCell=FIntPoint(MAX_int32,MAX_int32);
	/** Finish a continuous in-cell turn/merge before reserving the next road cell. */
	UPROPERTY() bool bSegmentLaneAligned=false;
	UPROPERTY() bool bParking=false;
	UPROPERTY() FVector ParkingGoal=FVector::ZeroVector;
	UPROPERTY() TArray<FIntPoint> RoadPath;
	UPROPERTY() int32 NextPathStep=0;
	UPROPERTY() TArray<int32> ServiceStopIDs;
	UPROPERTY() int32 NextServiceStop=0;
	UPROPERTY() TArray<int32> PassengerTripIDs;
	/** Still-riding passengers waiting for a finite, physical curb release. */
	UPROPERTY() TArray<int32> PendingAlightTripIDs;
	UPROPERTY() float AlightReleaseSeconds=0.f;
};

/** One building-owned visual fleet unit staffed by an existing CityLife employee. */
struct FAftershockMunicipalFleetUnit
{
	int32 OwnerBuildingID=INDEX_NONE,ResidentID=INDEX_NONE;
	EAftershockMunicipalService Service=EAftershockMunicipalService::None;
	FIntPoint HomeRoadCell=FIntPoint(MAX_int32,MAX_int32);
	FIntPoint IncidentCell=FIntPoint(MAX_int32,MAX_int32);
	int32 OnScenePathStep=INDEX_NONE;
	float OnSceneSeconds=0.f,CooldownSeconds=0.f;
	bool bDeployed=false,bReturning=false,bCancellationRequested=false;
	FAftershockRoadVehicle Vehicle;
};

USTRUCT(BlueprintType)
struct FAftershockTransportStats
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) int32 Cars=0;
	UPROPERTY(BlueprintReadOnly) int32 Buses=0;
	UPROPERTY(BlueprintReadOnly) int32 Stops=0;
	UPROPERTY(BlueprintReadOnly) int32 WaitingPassengers=0;
	UPROPERTY(BlueprintReadOnly) int32 RidingPassengers=0;
	UPROPERTY(BlueprintReadOnly) int32 CompletedTrips=0;
	UPROPERTY(BlueprintReadOnly) int32 StrandedTrips=0;
	UPROPERTY(BlueprintReadOnly) int32 InvalidRoadSteps=0;
	UPROPERTY(BlueprintReadOnly) int32 BusBoardings=0;
	UPROPERTY(BlueprintReadOnly) int32 BusArrivals=0;
	UPROPERTY(BlueprintReadOnly) int32 PrivateCarTrips=0;
};

/** Fixed-capacity ISM batches: no vehicle actor, pawn, skeletal mesh or tick. */
UCLASS(NotBlueprintable, Transient)
class JAMPROJECT_API AAftershockTransportHost final : public AActor
{
	GENERATED_BODY()
public:
	AAftershockTransportHost();
	void DrawFleet(const TArray<FAftershockRoadVehicle>& Fleet,const TMap<int32,EAftershockMunicipalService>* MunicipalRoles=nullptr);
	FVector GetVehicleDimensionsCm(const FAftershockRoadVehicle& Vehicle,float CellSize=1000.f) const;
	float GetVehicleRenderScale(const FAftershockRoadVehicle& Vehicle,float CellSize) const;
private:
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> Cars;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> Hatchbacks;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> Utilities;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> Buses;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> Wheels;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> ServiceEquipment;
	// Fixed-capacity render scratch and last submissions, never a second fleet authority.
	TArray<FTransform> CarTransformScratch[3], BusTransformScratch, WheelTransformScratch;
	TArray<FLinearColor> CarColorScratch[3], BusColorScratch;
	TArray<FTransform> LastSubmittedTransforms[5];
	TArray<FLinearColor> LastSubmittedColors[4];
	TArray<FTransform> ServiceEquipmentScratch,LastServiceEquipment;
	TArray<FLinearColor> ServiceEquipmentColours,LastServiceEquipmentColours;
};

/** Road-derived routes and boarding ledger; CityLife owns people, jobs and needs. */
UCLASS()
class JAMPROJECT_API UTransportSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	void CaptureSaveState(FAftershockTransportSaveState& Out) const;
	bool RestoreSaveState(const FAftershockTransportSaveState& State,FText& OutError);
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	bool PlanTrip(int32 ResidentID,FVector Origin,FVector Destination,int32 DestinationBuildingID,bool bAllowPrivateCar,FAftershockPassengerTrip& OutTrip);
	bool MarkPassengerAtPickup(int32 TripID,int32 ResidentID,FVector PassengerLocation);
	bool GetTrip(int32 TripID,FAftershockPassengerTrip& OutTrip) const;
	void CancelTrip(int32 TripID);
	void SetEmergencyMode(bool bEmergency);
	UFUNCTION(BlueprintPure,Category="Aftershock|Transport") FAftershockTransportStats GetTransportStats() const;
	UFUNCTION(BlueprintCallable,Category="Aftershock|Transport|Debug") void AuditTransport() const;
	TArray<FAftershockPassengerTrip> GetTripRoster() const;
	/** Read-only diagnostic roster; trip/service ownership stays here. */
	const TArray<FAftershockRoadVehicle>& GetVehicleRoster() const { return Vehicles; }
	/** Called only by a resident already inside its real workplace. No new resident/trip. */
	bool TryBeginServiceDuty(int32 ResidentID,int32 WorkBuildingID);
	bool GetMunicipalDuty(int32 ResidentID,FAftershockMunicipalDutyView& OutView) const;
	/** O(1) emergency roster membership; ordinary court couriers are excluded. */
	bool IsMunicipalStaffResident(int32 ResidentID,int32 WorkBuildingID) const;
	/** Existing canonical staff, including new facilities before the next transport timer. */
	void GetMunicipalStaffResidentIDs(TArray<int32>& OutResidentIDs) const;
	/** A valid worker returns by road; deceased/reassigned staff retire display ownership. */
	void CancelMunicipalDutyForResident(int32 ResidentID);
	int32 GetMunicipalFleetCount() const { return MunicipalFleet.Num(); }
	int32 GetMunicipalDeployedCount() const;
	/** Redraw an existing host only; no timers, infrastructure, trips or movement. */
	void RefreshVisiblePresentation();
	/** All movement/parking/admission paths use the same rendered body footprint. */
	bool IsPedestrianSpaceClear(const FVector& From,const FVector& To,float RadiusCm=32.f,bool bYieldToApproaching=true) const;
	bool FindPassengerReleaseLocation(int32 TripID,FVector& OutLocation) const;
	void RetainPendingPassengerHandoff(int32 TripID);
	static int32 GetOffstreetParkingCapacity(const FBuildingInstance& Building);
	/** Covered-load repair on a caller-owned snapshot copy; identity and fares survive. */
	bool ReconcileVehiclePlacements(FAftershockTransportSaveState& InOutState,FText& Error) const;
	/** Demolition may proceed only after every real parked car has a clear road entry. */
	bool EvictBuildingParking(int32 BuildingID);
	static bool VehicleSweepIntersectsDisc(const FVector& From,float FromYaw,const FVector& To,float ToYaw,
		const FVector& Dimensions,const FVector& DiscFrom,const FVector& DiscTo,float RadiusCm);
private:
	friend class UAftershockTransportAuditCommandlet;
	friend class UAftershockMunicipalAuditCommandlet;
	UFUNCTION() void HandleRoadsChanged();
	UFUNCTION() void HandleCityLifeUpdated();
	void RefreshInfrastructure();
	void UpdateTransport();
	void PruneTripHistory();
	/** Reclaim retired bus slots only after every real passenger handoff releases. */
	bool PruneInactiveBuses();
	bool BuildRoadPath(FIntPoint Start,FIntPoint Goal,TArray<FIntPoint>& OutPath) const;
	bool FindRoadNear(FIntPoint Start,FIntPoint& OutCoord,int32 Radius=3) const;
	bool IsRoadSafe(FIntPoint Coord) const;
	FVector RoadLocation(FIntPoint Coord,FVector Direction=FVector::ForwardVector) const;
	void HandleBusStop(FAftershockRoadVehicle& Bus);
	void AdvanceBusService(FAftershockRoadVehicle& Bus);
	void BoardBusPassenger(FAftershockRoadVehicle& Bus,int32 StopID);
	bool GetPassengerKerbLocation(int32 StopID,int32 Slot,bool bAlighting,FVector& OutLocation) const;
	FVector VehicleDimensions(const FAftershockRoadVehicle& Vehicle) const;
	bool CanMoveVehicle(const FAftershockRoadVehicle& Vehicle,const FVector& To,float ToYaw,bool bCheckPedestrians=true) const;
	bool TryMoveVehicle(FAftershockRoadVehicle& Vehicle,const FVector& To,float ToYaw);
	FVector VehicleDoorLocation(const FAftershockRoadVehicle& Vehicle,float Side=1.f) const;
	bool HasOffstreetParkingSpace(int32 BuildingID,int32 IgnoredOwnerID=INDEX_NONE) const;
	bool CanParkOffstreet(const FAftershockRoadVehicle& Vehicle,int32 BuildingID) const;
	void StrandPassengers(FAftershockRoadVehicle& Vehicle);
	void RefreshMunicipalFleet(const TArray<FBuildingInstance>& Buildings);
	bool SelectMunicipalRoute(FAftershockMunicipalFleetUnit& Unit,TArray<FIntPoint>& OutRoute,FIntPoint& OutIncident) const;
	void UpdateMunicipalFleet();
	void DrawAllFleets();
	bool IsMunicipalBodyClear(const FAftershockRoadVehicle& Vehicle,const FVector& To,float ToYaw) const;
	void ResetMunicipalPresentationAfterRestore();
	bool TryAcquireMunicipalQueryBudget();
	TArray<FAftershockMunicipalFleetUnit> MunicipalFleet;
	TMap<int32,int32> MunicipalFleetByResident;
	bool bMunicipalRebuildPending=true;
	uint64 MunicipalQueryFrame=MAX_uint64;
	uint8 MunicipalQueriesThisFrame=0;
	TArray<FAftershockRoadVehicle> FleetRenderScratch;
	TMap<int32,EAftershockMunicipalService> MunicipalRenderRoles;
	UPROPERTY() TObjectPtr<AAftershockTransportHost> Host;
	UPROPERTY() TMap<int32,FAftershockPassengerTrip> Trips;
	TArray<FAftershockRoadVehicle> Vehicles;
	TArray<FAftershockTransitStop> Stops;
	FTimerHandle UpdateTimer;
	bool bEmergencyActive=false;
	bool bInfrastructureDirty=true;
	int32 NextTripID=1;
	int32 NextVehicleID=1;
	float InfrastructureRefreshSeconds=0.f;
	float HistoryClockSeconds=0.f;
	float HistoryRefreshSeconds=0.f;
	FAftershockTransportStats RetiredTripStats;
};
