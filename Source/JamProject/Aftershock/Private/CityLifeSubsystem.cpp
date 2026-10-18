#include "Aftershock/Public/CityLifeSubsystem.h"
#include "AftershockRoadCpuScope.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Aftershock/Narrative/CasualtyLedgerGenerator.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

namespace
{
	bool IsHousing(EBuildingType T) { return T == EBuildingType::House || T == EBuildingType::Apartment; }
	int32 JobPriority(EBuildingType T)
	{
		if (T == EBuildingType::PowerPlant || T == EBuildingType::Generator || T == EBuildingType::WaterPlant || T == EBuildingType::Substation || T == EBuildingType::WindTurbine) return 0;
		if (T == EBuildingType::Farm || T == EBuildingType::LivestockFarm || T == EBuildingType::Bakery || T == EBuildingType::Warehouse) return 1;
		if (T == EBuildingType::Hospital || T == EBuildingType::FireStation || T == EBuildingType::PoliceStation) return 2;
		if (T == EBuildingType::BusTerminal || T == EBuildingType::TaxOffice || T == EBuildingType::Courthouse) return 3;
		return 4;
	}
	float StructuralOperation(const FBuildingInstance& B)
	{
		return B.BuildingData && B.DamageState < EDamageState::Damaged ? FMath::Clamp(1.f - B.CurrentDamage * .015f, 0.f, 1.f) : 0.f;
	}
	struct FComponentNeedsAccess
	{
		int32 Population=0;
		bool bFoodAccess=false;
		bool bWaterAccess=false;
		float HealthCapacity=0.f;
		float SafetyCapacity=0.f;
		float JusticeCapacity=0.f;
	};
	// Derived access only. The existing canonical road-component assignments and
	// finite city inventories remain the only network and stock authorities.
	TMap<int32,FComponentNeedsAccess> BuildComponentNeedsAccess(const TArray<FBuildingInstance>& Buildings,
		const TMap<int32,int32>& Components,const TMap<int32,FCityBuildingStatus>& Statuses,const TArray<FCityResidentRecord>& Residents)
	{
		TMap<int32,FComponentNeedsAccess> Result;
		for(const FCityResidentRecord& R:Residents) if(R.bAlive)
			if(const int32* Component=Components.Find(R.HomeBuildingID)) ++Result.FindOrAdd(*Component).Population;
		for(const FBuildingInstance& B:Buildings) if(B.BuildingData)
		{
			const int32* Component=Components.Find(B.InstanceID);if(!Component)continue;
			const FCityBuildingStatus* Status=Statuses.Find(B.InstanceID);if(!Status)continue;
			FComponentNeedsAccess& Access=Result.FindOrAdd(*Component);
			const float Operation=FMath::Clamp(Status->Operation01,0.f,1.f);const EBuildingType Type=B.BuildingData->Type;
			if(Operation>.1f)
			{
				Access.bFoodAccess|=Type==EBuildingType::Farm||Type==EBuildingType::LivestockFarm||Type==EBuildingType::Bakery||Type==EBuildingType::Restaurant||Type==EBuildingType::Warehouse;
				Access.bWaterAccess|=Type==EBuildingType::WaterPlant;
			}
			if(Type==EBuildingType::Hospital)Access.HealthCapacity+=Operation*250.f;
			if(Type==EBuildingType::PoliceStation)Access.SafetyCapacity+=Operation*400.f;
			if(Type==EBuildingType::Courthouse)Access.JusticeCapacity+=Operation*400.f;
		}
		return Result;
	}
}

bool UCityLifeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* W = Cast<UWorld>(Outer);
	return Super::ShouldCreateSubsystem(Outer) && W && (W->WorldType == EWorldType::Game || W->WorldType == EWorldType::PIE);
}

void UCityLifeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UTerrainGridSubsystem>();
	Collection.InitializeDependency<UBuildingManagerSubsystem>();
}

void UCityLifeSubsystem::InitializeForCity()
{
	if (bInitializedForCity) { RefreshCity(); return; }
	bInitializedForCity = true;
	SyncResidentsAndJobs();
	State.FoodDemandPerTurn = State.LivingPopulation * .20f;
	State.FoodStock = State.FoodDemandPerTurn * 4.f;
	State.RawFoodStock = State.FoodDemandPerTurn * 2.f;
	State.WaterStock = State.LivingPopulation * 2.f;
	State.FuelStock = 90.f;
	RecomputeServices();
	float InitialFuelDemand=0.f;
	if(auto* Manager=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>())
	{TArray<FBuildingInstance> Buildings;Manager->GetAllBuildings(Buildings);for(const auto& B:Buildings)if(B.BuildingData)
		InitialFuelDemand+=(B.BuildingData->Type==EBuildingType::PowerPlant?4.f:B.BuildingData->Type==EBuildingType::Generator?2.f:0.f)*GetBuildingOperation01(B.InstanceID);}
	State.FuelStock=FMath::Max(90.f,4.f*InitialFuelDemand);
	OnCityLifeUpdated.Broadcast();
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CITY_LIFE_READY residents=%d jobs=%d employed=%d populationIncludesJobs=0 persistentResidentIDs=1 foodReserveTurns=4"), State.LivingPopulation, State.JobSlots, State.EmployedResidents);
}

TMap<int32, int32> UCityLifeSubsystem::BuildRoadComponents() const
{
	TMap<int32,int32> Components;
	const UTerrainGridSubsystem* T = GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
	const auto* Roads=GetWorld()->GetSubsystem<URoadNetworkSubsystem>();
	if (!T||!Roads) return Components;
	// Storage may contain disjoint resident chunks; never scan the bounding
	// rectangle or reinterpret the append-only array index as an (X,Y) pair.
	const int32 ResidentCellCount=T->Cells.Num();
	int32 Component = 0;
	const FIntPoint Steps[] = { FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1) };
	for (const FGridCell& Cell : T->Cells)
	{
		const int32 Index = T->CoordToIndex(Cell.Coords);
		if (Index==INDEX_NONE||!Roads->IsRoadTraversable(Cell.Coords)||Components.Contains(Index)) continue;
		TArray<FIntPoint> Queue; Queue.Add(Cell.Coords); Components.Add(Index, Component);
		for (int32 Head=0;Head<Queue.Num()&&Head<ResidentCellCount;++Head) for (FIntPoint Step : Steps)
		{
			const FIntPoint C = Queue[Head]+Step;
			if (!T->IsPlayableCoord(C)) continue;
			const FGridCell N=T->GetCell(C); const int32 I=T->CoordToIndex(C);
			if (I==INDEX_NONE||!Roads->CanTraverseRoadEdge(Queue[Head],C)||Components.Contains(I)) continue;
			Components.Add(I,Component); Queue.Add(C);
		}
		++Component;
	}
	return Components;
}

void UCityLifeSubsystem::GetBuildingStaffResidentIDs(int32 BuildingID,TArray<int32>& OutResidentIDs,int32 Maximum) const
{
	OutResidentIDs.Reset();Maximum=FMath::Clamp(Maximum,0,64);
	if(BuildingID==INDEX_NONE||Maximum==0)return;
	for(const FCityResidentRecord& Resident:Residents)
		if(Resident.bAlive&&Resident.bWorkingAge&&Resident.WorkBuildingID==BuildingID)OutResidentIDs.Add(Resident.ResidentID);
	OutResidentIDs.Sort();if(OutResidentIDs.Num()>Maximum)OutResidentIDs.SetNum(Maximum,EAllowShrinking::No);
}

void UCityLifeSubsystem::SyncResidentsAndJobs()
{
    FAftershockRoadCpuScope Cpu(TEXT("Life.SyncResidentsAndJobs"));
	InvalidateScopedOccupancyLookup();
	UBuildingManagerSubsystem* Manager=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();
	UTerrainGridSubsystem* Terrain=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
	if (!Manager || !Terrain) return;
	TArray<FBuildingInstance> Buildings; Manager->GetAllBuildings(Buildings);
	Buildings.Sort([](const FBuildingInstance& A,const FBuildingInstance& B){return A.InstanceID<B.InstanceID;});
	const TMap<int32,int32> RoadComponents=BuildRoadComponents();
	BuildingRoadComponents.Reset();
	const FIntPoint Steps[]={FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1)};
	TMap<int32,FBuildingInstance> ByID;
	for(const FBuildingInstance& B:Buildings)
	{
		if(!B.BuildingData)continue;
		ByID.Add(B.InstanceID,B);
		if(B.DamageState==EDamageState::Memorial)continue;
		for(FIntPoint Step:Steps)
		{
			const FIntPoint C=B.GridCoords+Step;
			if(!Terrain->IsValidCoord(C)||!GetWorld()->GetSubsystem<URoadNetworkSubsystem>()->CanAccessRoadFromCell(B.GridCoords,C))continue;
			if(const int32* Comp=RoadComponents.Find(Terrain->CoordToIndex(C))) {BuildingRoadComponents.Add(B.InstanceID,*Comp);break;}
		}
		if(IsHousing(B.BuildingData->Type) && B.DamageState<EDamageState::Damaged && !SeededHomes.Contains(B.InstanceID))
		{
			SeededHomes.Add(B.InstanceID);
			for(int32 I=0;I<FMath::Clamp(B.BuildingData->Residents,0,1000);++I)
			{
				FCityResidentRecord& R=Residents.AddDefaulted_GetRef();
				R.ResidentID=NextResidentID++;R.HomeBuildingID=B.InstanceID;
				R.bWorkingAge=R.ResidentID%5!=0;R.bPrefersPrivateCar=R.bWorkingAge && R.ResidentID%4==0;
				UCasualtyLedgerGenerator::GetStableDemographics(R.ResidentID,R.bWorkingAge,R.FullName,R.Age);
			}
		}
	}
	// Keep valid jobs, then allocate missing workers in essential-service order.
	// A job is capacity, never a new resident; deaths never regenerate on repair.
	TMap<int32,int32> Assigned;
	State.LivingPopulation=0;State.EmployedResidents=0;State.JobSlots=0;
	for(FCityResidentRecord& R:Residents)
	{
		if(R.FullName.IsEmpty()||R.Age<1)UCasualtyLedgerGenerator::GetStableDemographics(R.ResidentID,R.bWorkingAge,R.FullName,R.Age);
		if(!R.bAlive) {R.WorkBuildingID=INDEX_NONE;continue;}
		++State.LivingPopulation;
		const FBuildingInstance* Work=ByID.Find(R.WorkBuildingID);
		const int32* HC=BuildingRoadComponents.Find(R.HomeBuildingID);
		const int32* WC=BuildingRoadComponents.Find(R.WorkBuildingID);
		if(!R.bWorkingAge || !Work || StructuralOperation(*Work)<=0.f || !HC || !WC || *HC!=*WC
			|| Assigned.FindRef(R.WorkBuildingID)>=FMath::Max(0,Work->BuildingData->Workers)) R.WorkBuildingID=INDEX_NONE;
		else ++Assigned.FindOrAdd(R.WorkBuildingID);
	}
	Buildings.Sort([](const FBuildingInstance&A,const FBuildingInstance&B)
	{
		const int32 PA=A.BuildingData?JobPriority(A.BuildingData->Type):99,PB=B.BuildingData?JobPriority(B.BuildingData->Type):99;
		return PA==PB?A.InstanceID<B.InstanceID:PA<PB;
	});
	const auto MinimumStaff = [](const FBuildingInstance& B)
	{
		const int32 Capacity=B.BuildingData?FMath::Max(0,B.BuildingData->Workers):0;
		return Capacity>0?FMath::Max(1,FMath::DivideAndRoundUp(Capacity,5)):0;
	};
	// Retain valid jobs, first guarantee a small operating team at every connected
	// employer, then fill essential services. Donor minimums remain protected.
	// New schools/restaurants can recruit a few workers from an established office
	// without continuously reallocating the whole workforce on every refresh.
	struct FHouseholdWorkQueue {int32 Home=INDEX_NONE,Work=INDEX_NONE,Head=0;TArray<int32> Members;};
	TArray<FHouseholdWorkQueue> Queues;TMap<FIntPoint,int32> QueueByHomeWork;TMap<int32,TArray<int32>> QueuesByComponent;
	const auto AddQueueMember=[&](int32 ResidentIndex,int32 Work)
	{
		const int32 Home=Residents[ResidentIndex].HomeBuildingID;const FIntPoint Key(Home,Work);
		int32* Existing=QueueByHomeWork.Find(Key);int32 QueueIndex=Existing?*Existing:INDEX_NONE;
		if(QueueIndex==INDEX_NONE){QueueIndex=Queues.AddDefaulted();Queues[QueueIndex].Home=Home;Queues[QueueIndex].Work=Work;QueueByHomeWork.Add(Key,QueueIndex);QueuesByComponent.FindOrAdd(BuildingRoadComponents.FindRef(Home)).Add(QueueIndex);}
		Queues[QueueIndex].Members.Add(ResidentIndex);
	};
	for(int32 I=0;I<Residents.Num();++I)if(Residents[I].bAlive&&Residents[I].bWorkingAge&&BuildingRoadComponents.Contains(Residents[I].HomeBuildingID))AddQueueMember(I,Residents[I].WorkBuildingID);
	for(auto& Queue:Queues)Queue.Members.Sort([&](int32 A,int32 B){return Residents[A].ResidentID<Residents[B].ResidentID;});
	// Scan/sort household+employer queues once per employer, not the complete
	// resident array for every vacant job. A queue can contain an entire block's
	// workers. IDs remain canonical and donor minimums are checked at each pop.
	for(int32 Pass=0;Pass<2;++Pass) for(const FBuildingInstance& B:Buildings)
	{
		if(!B.BuildingData || StructuralOperation(B)<=0.f)continue;
		if(Pass==0)State.JobSlots+=FMath::Max(0,B.BuildingData->Workers);
		const int32* WC=BuildingRoadComponents.Find(B.InstanceID);if(!WC)continue;
		const int32 Target=Pass==0?MinimumStaff(B):FMath::Max(0,B.BuildingData->Workers);
		if(Assigned.FindRef(B.InstanceID)>=Target)continue;
		struct FCandidateQueue {int32 Index;int64 Cost;int32 FirstResident;};TArray<FCandidateQueue> Candidates;
		if(const auto* ComponentQueues=QueuesByComponent.Find(*WC))for(int32 Q:*ComponentQueues)
		{
			const auto& Queue=Queues[Q];const auto* Home=ByID.Find(Queue.Home);const auto* Previous=ByID.Find(Queue.Work);
			if(!Home||Queue.Work==B.InstanceID||Queue.Head>=Queue.Members.Num())continue;
			if(Previous&&(Assigned.FindRef(Queue.Work)<=MinimumStaff(*Previous)||(Pass==1&&JobPriority(Previous->BuildingData->Type)<=JobPriority(B.BuildingData->Type))))continue;
			const int64 Penalty=Previous?100000+(4-JobPriority(Previous->BuildingData->Type))*10000:0;
			const int64 Distance=FMath::Abs(static_cast<int64>(Home->GridCoords.X)-B.GridCoords.X)+FMath::Abs(static_cast<int64>(Home->GridCoords.Y)-B.GridCoords.Y);
			Candidates.Add({Q,Distance+Penalty,Residents[Queue.Members[Queue.Head]].ResidentID});
		}
		Candidates.Sort([](const auto& A,const auto& C){return A.Cost==C.Cost?A.FirstResident<C.FirstResident:A.Cost<C.Cost;});
		for(const auto& Candidate:Candidates)
		{
			if(Assigned.FindRef(B.InstanceID)>=Target)break;
			while(Queues[Candidate.Index].Head<Queues[Candidate.Index].Members.Num()&&Assigned.FindRef(B.InstanceID)<Target)
			{
				const int32 PreviousWork=Queues[Candidate.Index].Work;const auto* Previous=ByID.Find(PreviousWork);
				if(Previous&&Assigned.FindRef(PreviousWork)<=MinimumStaff(*Previous))break;
				const int32 I=Queues[Candidate.Index].Members[Queues[Candidate.Index].Head++];
				if(Residents[I].WorkBuildingID!=PreviousWork)continue;
				if(PreviousWork!=INDEX_NONE)--Assigned.FindOrAdd(PreviousWork);
				Residents[I].WorkBuildingID=B.InstanceID;++Assigned.FindOrAdd(B.InstanceID);AddQueueMember(I,B.InstanceID);
			}
		}
	}
	int32 Adults=0;for(const FCityResidentRecord& R:Residents)if(R.bAlive){Adults+=R.bWorkingAge?1:0;State.EmployedResidents+=R.WorkBuildingID!=INDEX_NONE?1:0;}
	State.Employment01=Adults>0?static_cast<float>(State.EmployedResidents)/Adults:1.f;
	const TMap<int32,FCityBuildingStatus> PreviousStates=MoveTemp(BuildingStates);
	BuildingStates.Reset();
	for(const FBuildingInstance& B:Buildings)if(B.BuildingData)
	{
		FCityBuildingStatus& S=BuildingStates.Add(B.InstanceID);
		S.BuildingID=B.InstanceID;S.bRoadConnected=BuildingRoadComponents.Contains(B.InstanceID);
		S.AssignedWorkers=Assigned.FindRef(B.InstanceID);S.JobCapacity=B.DamageState==EDamageState::Memorial?0:FMath::Max(0,B.BuildingData->Workers);
		S.Staffing01=S.JobCapacity>0?static_cast<float>(S.AssignedWorkers)/S.JobCapacity:1.f;
		S.FoodOutputPerTurn=PreviousStates.FindRef(B.InstanceID).FoodOutputPerTurn;
	}
}

void UCityLifeSubsystem::RecomputeServices()
{
    FAftershockRoadCpuScope Cpu(TEXT("Life.RecomputeServices"));
	UBuildingManagerSubsystem* M=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();
	if(!M)return;
	TArray<FBuildingInstance> Buildings;M->GetAllBuildings(Buildings);
	TMap<int32,float> Supply,Demand,Distribution;
	for(const FBuildingInstance& B:Buildings)if(B.BuildingData&&B.BuildingData->Type==EBuildingType::Substation)
	{
		const int32* Comp=BuildingRoadComponents.Find(B.InstanceID);
		if(Comp)Distribution.FindOrAdd(*Comp)=FMath::Max(Distribution.FindRef(*Comp),StructuralOperation(B)*BuildingStates.FindRef(B.InstanceID).Staffing01);
	}
	State.PowerSupply=0;State.PowerDemand=0;State.OperatingBuildings=0;
	for(const FBuildingInstance& B:Buildings)if(B.BuildingData)
	{
		FCityBuildingStatus& S=BuildingStates.FindOrAdd(B.InstanceID);
		const float Base=StructuralOperation(B)*(S.bRoadConnected?1.f:0.f);
		const int32 Comp=BuildingRoadComponents.Contains(B.InstanceID)?BuildingRoadComponents[B.InstanceID]:INDEX_NONE;
		float Produced=B.BuildingData->PowerProductionUnitsPerTurn*Base*S.Staffing01;
		if(B.BuildingData->Type!=EBuildingType::Generator)Produced*=Distribution.FindRef(Comp);
		if((B.BuildingData->Type==EBuildingType::PowerPlant||B.BuildingData->Type==EBuildingType::Generator)&&State.FuelStock<=0.f)Produced=0.f;
		if(Comp!=INDEX_NONE){Supply.FindOrAdd(Comp)+=Produced;Demand.FindOrAdd(Comp)+=B.BuildingData->PowerDemandUnitsPerTurn*Base;}
		State.PowerSupply+=Produced;State.PowerDemand+=B.BuildingData->PowerDemandUnitsPerTurn*Base;
	}
	State.PowerCoverage01=State.PowerDemand>0?FMath::Clamp(State.PowerSupply/State.PowerDemand,0.f,1.f):1.f;
	float Transit=0,Tax=0;State.ProjectedMaintenanceTL=0;State.ProjectedTaxIncomeTL=0;
	for(const FBuildingInstance& B:Buildings)if(B.BuildingData)
	{
		FCityBuildingStatus& S=BuildingStates.FindOrAdd(B.InstanceID);
		const int32* Comp=BuildingRoadComponents.Find(B.InstanceID);
		S.PowerCoverage01=Comp?(Demand.FindRef(*Comp)>0?FMath::Clamp(Supply.FindRef(*Comp)/Demand.FindRef(*Comp),0.f,1.f):1.f):0.f;
		S.bPowered=S.PowerCoverage01>.25f;
		const bool Producer=B.BuildingData->PowerProductionUnitsPerTurn>0.f;
		const bool NoPowerNeed=B.BuildingData->PowerDemandUnitsPerTurn<=0.f;
		const bool NeedsFuel=B.BuildingData->Type==EBuildingType::PowerPlant||B.BuildingData->Type==EBuildingType::Generator;
		const float PowerFactor=Producer||NoPowerNeed?1.f:S.PowerCoverage01;
		S.Operation01=StructuralOperation(B)*(S.bRoadConnected?1.f:0.f)*S.Staffing01*PowerFactor*(NeedsFuel&&State.FuelStock<=0.f?0.f:1.f);
		S.bOperating=S.Operation01>.1f;
		if(!S.bRoadConnected)S.FailureReason=FText::FromString(TEXT("Yol bağlantısı yok"));
		else if(StructuralOperation(B)<=0)S.FailureReason=FText::FromString(TEXT("Hasar nedeniyle kapalı"));
		else if(S.Staffing01<=.1f)S.FailureReason=FText::FromString(TEXT("Personel yok"));
		else if(NeedsFuel&&State.FuelStock<=0.f)S.FailureReason=FText::FromString(TEXT("Yakıt tükendi"));
		else if(!S.bPowered&&!Producer&&!NoPowerNeed)S.FailureReason=FText::FromString(TEXT("Elektrik kesintisi"));
		else S.FailureReason=FText::FromString(TEXT("Faaliyette"));
		State.OperatingBuildings+=S.bOperating?1:0;
		State.ProjectedMaintenanceTL+=B.DamageState>=EDamageState::Collapsed?0:B.BuildingData->MaintenancePerTurn;
		const EBuildingType T=B.BuildingData->Type;
		if(T==EBuildingType::BusTerminal)Transit+=S.Operation01*B.BuildingData->TransitCapacity;
		if(T==EBuildingType::TaxOffice)Tax+=S.Operation01;
		M->SetBuildingOperationalVisual(B.InstanceID,S.PowerCoverage01,S.Operation01);
	}
	const float Pop=FMath::Max(1,State.LivingPopulation);
	const TMap<int32,FComponentNeedsAccess> ComponentAccess=BuildComponentNeedsAccess(Buildings,BuildingRoadComponents,BuildingStates,Residents);
	float ServedHealth=0.f,ServedSafety=0.f,ServedJustice=0.f;
	for(const auto& Pair:ComponentAccess)
	{
		const FComponentNeedsAccess& A=Pair.Value;
		ServedHealth+=FMath::Min(A.HealthCapacity,static_cast<float>(A.Population));
		ServedSafety+=FMath::Min(A.SafetyCapacity,static_cast<float>(A.Population));
		ServedJustice+=FMath::Min(A.JusticeCapacity,static_cast<float>(A.Population));
	}
	State.HealthCoverage01=FMath::Clamp(ServedHealth/Pop,0.f,1.f);
	State.SafetyCoverage01=FMath::Clamp(ServedSafety/Pop,0.f,1.f);
	State.JusticeCoverage01=FMath::Clamp(ServedJustice/Pop,0.f,1.f);
	State.TransitCoverage01=FMath::Clamp(Transit/Pop,0.f,1.f);
	State.TaxCollectionEfficiency01=FMath::Clamp(.70f+.20f*Tax+.10f*State.JusticeCoverage01,.5f,1.f);
	TMap<int32,int32> LivingByHome;for(const auto& R:Residents)if(R.bAlive)++LivingByHome.FindOrAdd(R.HomeBuildingID);
	for(const FBuildingInstance& B:Buildings)if(B.BuildingData)
	{
		const FCityBuildingStatus S=BuildingStates.FindRef(B.InstanceID);
		if(IsHousing(B.BuildingData->Type))
		{
			const int32 LivingHere=LivingByHome.FindRef(B.InstanceID);
			const float Occupancy=B.BuildingData->Residents>0?static_cast<float>(LivingHere)/B.BuildingData->Residents:0.f;
			State.ProjectedTaxIncomeTL+=FMath::RoundToInt64(B.BuildingData->TaxPerTurn*Occupancy*StructuralOperation(B)*(State.ResidentialTaxRate/.10f)*State.TaxCollectionEfficiency01);
		}
		else State.ProjectedTaxIncomeTL+=FMath::RoundToInt64(B.BuildingData->TaxPerTurn*S.Operation01*(State.BusinessTaxRate/.12f)*State.TaxCollectionEfficiency01);
	}
}

void UCityLifeSubsystem::RefreshCity()
{
    FAftershockRoadCpuScope Cpu(TEXT("Life.RefreshCity"));
	// A complete snapshot already contains the canonical resident roster, homes,
	// jobs and derived service/component maps. Restore notifications must never
	// reseed homes or reassign that imported population midway through the load.
	if(const auto* GI=GetWorld()?GetWorld()->GetGameInstance():nullptr)if(GI->GetSubsystem<UAftershockSaveSubsystem>()->IsRestoring())return;
	if(!bInitializedForCity||bRefreshing||bAdvancing||!GetWorld())return;
	TGuardValue<bool> Guard(bRefreshing,true);
	SyncResidentsAndJobs();RecomputeServices();OnCityLifeUpdated.Broadcast();
}

void UCityLifeSubsystem::AdvanceCityTurn(int32 Turn)
{
	if(!bInitializedForCity||bAdvancing||Turn<=State.LastProcessedTurn)return;
	TGuardValue<bool> Guard(bAdvancing,true);
	State.LastProcessedTurn=Turn;State.LastSupplyExpenseTL=0;
	State.LastTransitIncomeTL=PendingTransitRevenueTL;PendingTransitRevenueTL=0;
	PaidTransitTrips.Reset();
	LastServiceVisitTurn.Reset();
	UBuildingManagerSubsystem* M=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();
	UTerrainGridSubsystem* T=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
	UEconomyAndStateSubsystem* E=GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>();
	if(!M||!T)return;
	T->AdvanceEnvironmentalTurn();
	SyncResidentsAndJobs();RecomputeServices();
	for(auto& Pair:BuildingStates)Pair.Value.FoodOutputPerTurn=0.f;
	TArray<FBuildingInstance> Buildings;M->GetAllBuildings(Buildings);Buildings.Sort([](const FBuildingInstance&A,const FBuildingInstance&B){return A.InstanceID<B.InstanceID;});
	float Warehouse=0.f;int32 FuelUnloadingWarehouses=0;float PotentialFuelDemand=0.f;
	for(const auto& B:Buildings)if(B.BuildingData)
	{
		const auto S=BuildingStates.FindRef(B.InstanceID);
		const float FuelPerTurn=B.BuildingData->Type==EBuildingType::PowerPlant?4.f:B.BuildingData->Type==EBuildingType::Generator?2.f:0.f;
		PotentialFuelDemand+=FuelPerTurn*StructuralOperation(B)*(S.bRoadConnected?1.f:0.f)*S.Staffing01;
	}
	for(const FBuildingInstance& B:Buildings)if(B.BuildingData&&B.BuildingData->Type==EBuildingType::Warehouse)
	{
		Warehouse+=GetBuildingOperation01(B.InstanceID);
		const FCityBuildingStatus S=BuildingStates.FindRef(B.InstanceID);
		// A staffed, intact road-connected warehouse can unload paid emergency
		// fuel by hand even when the generator has run dry and power is off.
		FuelUnloadingWarehouses+=StructuralOperation(B)>0.f&&S.bRoadConnected&&S.Staffing01>.1f?1:0;
	}
	if(E&&FuelUnloadingWarehouses>0&&SupplyDisruptionTurns<=0&&State.FuelStock<FMath::Max(45.f,2.f*PotentialFuelDemand))
	{
		const float Target=FMath::Max(45.f,4.f*PotentialFuelDemand);
		int32 Batches=FMath::Min(FuelUnloadingWarehouses,FMath::Max(1,FMath::CeilToInt((Target-State.FuelStock)/40.f)));
		if(!E->CanAfford(static_cast<int64>(Batches)*600))Batches=static_cast<int32>(FMath::Min<int64>(Batches,FMath::Max<int64>(0,E->TreasuryTL/600)));
		const int64 Cost=static_cast<int64>(Batches)*600;
		if(Batches>0&&E->TrySpendMoney(Cost)){State.FuelStock+=Batches*40.f;State.LastSupplyExpenseTL+=Cost;RecomputeServices();}
	}
	float WaterProduced=0.f,WaterDemand=State.LivingPopulation*.30f,FuelDemand=0.f,MealsProduced=0.f;
	for(const FBuildingInstance& B:Buildings)if(B.BuildingData)
	{
		const float Op=GetBuildingOperation01(B.InstanceID);const EBuildingType Type=B.BuildingData->Type;
		if(Type==EBuildingType::WaterPlant)WaterProduced+=250.f*Op*(1.f-T->GetCell(B.GridCoords).DroughtStress01*.75f);
		if(Type==EBuildingType::Farm)WaterDemand+=14.f*Op;
		if(Type==EBuildingType::LivestockFarm)WaterDemand+=10.f*Op;
		if(Type==EBuildingType::PowerPlant)FuelDemand+=4.f*Op;
		if(Type==EBuildingType::Generator)FuelDemand+=2.f*Op;
	}
	State.FuelStock=FMath::Max(0.f,State.FuelStock-FuelDemand);
	State.WaterStock=FMath::Min(State.WaterStock+WaterProduced,State.LivingPopulation*8.f+200.f);
	State.WaterCoverage01=WaterDemand>0?FMath::Clamp(State.WaterStock/WaterDemand,0.f,1.f):1.f;
	State.WaterStock=FMath::Max(0.f,State.WaterStock-WaterDemand);
	for(const FBuildingInstance& B:Buildings)if(B.BuildingData)
	{
		FCityBuildingStatus& S=BuildingStates.FindChecked(B.InstanceID);
		const EBuildingType Type=B.BuildingData->Type;const FGridCell Cell=T->GetCell(B.GridCoords);
		if(Type==EBuildingType::Farm)
		{
			const float Yield=B.BuildingData->FoodProductionUnitsPerTurn*S.Operation01*State.WaterCoverage01*Cell.CropHealth01*(1.f-.8f*Cell.DroughtStress01);
			State.RawFoodStock+=Yield;S.FoodOutputPerTurn=Yield;
		}
		else if(Type==EBuildingType::LivestockFarm)
		{
			const float Feed=FMath::Min(State.RawFoodStock,12.f*S.Operation01);State.RawFoodStock-=Feed;
			const float FeedCoverage=S.Operation01>0?FMath::Clamp(Feed/(12.f*S.Operation01),0.f,1.f):0.f;
			State.LivestockCondition01=FMath::Clamp(State.LivestockCondition01+.08f*FeedCoverage*State.WaterCoverage01-.12f*(1.f-State.WaterCoverage01)-.10f*(1.f-FeedCoverage),0.f,1.f);
			const float Yield=B.BuildingData->FoodProductionUnitsPerTurn*S.Operation01*State.LivestockCondition01*State.WaterCoverage01;
			State.RawFoodStock+=Yield;S.FoodOutputPerTurn=Yield;
		}
	}
	for(const FBuildingInstance& B:Buildings)if(B.BuildingData)
	{
		FCityBuildingStatus& S=BuildingStates.FindChecked(B.InstanceID);
		if(B.BuildingData->Type!=EBuildingType::Bakery&&B.BuildingData->Type!=EBuildingType::Restaurant)continue;
		const float Input=FMath::Min(State.RawFoodStock,B.BuildingData->FoodProcessingInputUnitsPerTurn*S.Operation01*State.WaterCoverage01);
		State.RawFoodStock-=Input;const float Meals=Input*(B.BuildingData->Type==EBuildingType::Bakery?1.25f:1.10f);
		State.FoodStock+=Meals;MealsProduced+=Meals;S.FoodOutputPerTurn=Meals;
	}
	// Households can prepare part of the remaining raw harvest; bakeries/restaurants
	// produce more efficiently and need actual workers, water and electricity.
	State.FoodDemandPerTurn=State.LivingPopulation*.20f;
	const float HomeCooking=FMath::Min(State.RawFoodStock,FMath::Max(0.f,State.FoodDemandPerTurn-State.FoodStock));
	State.RawFoodStock-=HomeCooking;State.FoodStock+=HomeCooking*.7f;
	State.FoodProductionPerTurn=MealsProduced+HomeCooking*.7f;
	State.FoodCoverage01=State.FoodDemandPerTurn>0?FMath::Clamp(State.FoodStock/State.FoodDemandPerTurn,0.f,1.f):1.f;
	State.FoodStock=FMath::Max(0.f,State.FoodStock-State.FoodDemandPerTurn);
	const float Spoilage=State.PowerCoverage01<.3f?.08f:.01f;
	State.FoodStock=FMath::Min(State.FoodStock*(1.f-Spoilage),State.FoodDemandPerTurn*(6.f+Warehouse*6.f)+100.f);
	State.RawFoodStock=FMath::Min(State.RawFoodStock*.99f,State.FoodDemandPerTurn*(8.f+Warehouse*8.f)+100.f);
	UpdateResidentNeeds(State.FoodCoverage01,State.WaterCoverage01);
	if(SupplyDisruptionTurns>0)--SupplyDisruptionTurns;
	SyncResidentsAndJobs();RecomputeServices();OnCityLifeUpdated.Broadcast();
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITY_LIFE_TURN turn=%d living=%d employed=%d food=%.2f raw=%.2f coverage=%.3f water=%.3f power=%.3f supplyExpense=%lld deathsLedger=%d"),Turn,State.LivingPopulation,State.EmployedResidents,State.FoodStock,State.RawFoodStock,State.FoodCoverage01,State.WaterCoverage01,State.PowerCoverage01,State.LastSupplyExpenseTL,State.TotalDeaths);
}

void UCityLifeSubsystem::UpdateResidentNeeds(float FoodCoverage,float WaterCoverage)
{
	InvalidateScopedOccupancyLookup();
	const TArray<FCityResidentRecord> BeforeNeeds=Residents;TArray<int32> NeedsFatalities;
	float Happiness=0,DeliveredFood=0,DeliveredWater=0;int32 Living=0;
	UBuildingManagerSubsystem* M=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();
	TArray<FBuildingInstance> Buildings;M->GetAllBuildings(Buildings);
	const TMap<int32,FComponentNeedsAccess> ComponentAccess=BuildComponentNeedsAccess(Buildings,BuildingRoadComponents,BuildingStates,Residents);
	for(FCityResidentRecord& R:Residents)if(R.bAlive)
	{
		FBuildingInstance Home;const bool Housed=M->GetBuildingByID(R.HomeBuildingID,Home)&&StructuralOperation(Home)>0;
		const int32* Component=BuildingRoadComponents.Find(R.HomeBuildingID);
		const FComponentNeedsAccess* Access=Component?ComponentAccess.Find(*Component):nullptr;
		const float LocalFood=Access&&Access->bFoodAccess?FMath::Clamp(FoodCoverage,0.f,1.f):0.f;
		const float LocalWater=Access&&Access->bWaterAccess?FMath::Clamp(WaterCoverage,0.f,1.f):0.f;
		const float LocalPopulation=Access?static_cast<float>(FMath::Max(1,Access->Population)):1.f;
		const float LocalHealth=Access?FMath::Clamp(Access->HealthCapacity/LocalPopulation,0.f,1.f):0.f;
		const float LocalSafety=Access?FMath::Clamp(Access->SafetyCapacity/LocalPopulation,0.f,1.f):0.f;
		const float LocalJustice=Access?FMath::Clamp(Access->JusticeCapacity/LocalPopulation,0.f,1.f):0.f;
		R.Hunger01=FMath::Clamp(R.Hunger01+(1.f-LocalFood)*.28f-LocalFood*.18f,0.f,1.f);
		const float HealthDelta=LocalHealth*.025f-(R.Hunger01>.6f?.06f:0.f)-(1.f-LocalWater)*.07f;
		R.Health01=FMath::Clamp(R.Health01+HealthDelta,0.f,1.f);
		const float TaxPressure=FMath::Max(0.f,State.ResidentialTaxRate-.10f)*2.f+FMath::Max(0.f,State.BusinessTaxRate-.12f);
		const float Employed=!R.bWorkingAge||R.WorkBuildingID!=INDEX_NONE?1.f:0.f;
		const float Power=BuildingStates.FindRef(R.HomeBuildingID).PowerCoverage01;
		const float Target=FMath::Clamp(.20f+.20f*LocalFood+.12f*LocalWater+.13f*Power+.12f*Employed+.08f*LocalHealth+.06f*LocalSafety+.04f*LocalJustice+.05f*(Housed?1.f:0.f)-TaxPressure-.20f*R.Hunger01,0.f,1.f);
		R.Happiness01=FMath::Lerp(R.Happiness01,Target,.35f);
		if(!Housed)R.Emotion=ECityResidentEmotion::Emergency;
		else if(R.Hunger01>.35f)R.Emotion=ECityResidentEmotion::Hungry;
		else if(R.Health01<.45f)R.Emotion=ECityResidentEmotion::Sick;
		else R.Emotion=R.Happiness01>=.65f?ECityResidentEmotion::Happy:R.Happiness01<.45f?ECityResidentEmotion::Sad:ECityResidentEmotion::Neutral;
		// Starvation affects the SAME ledger; one resident can die only once.
		if(R.Hunger01>=.98f&&R.Health01<=.05f){R.bAlive=false;R.WorkBuildingID=INDEX_NONE;NeedsFatalities.Add(R.ResidentID);++State.TotalDeaths;continue;}
		Happiness+=R.Happiness01;DeliveredFood+=LocalFood;DeliveredWater+=LocalWater;++Living;
	}
	State.LivingPopulation=Living;State.AverageHappiness01=Living>0?Happiness/Living:0.f;
	State.FoodCoverage01=Living>0?DeliveredFood/Living:0.f;
	State.WaterCoverage01=Living>0?DeliveredWater/Living:0.f;
	if(!NeedsFatalities.IsEmpty())if(auto* Ledger=GetWorld()->GetSubsystem<UCasualtyLedgerGenerator>())Ledger->RecordNeedsFatalities(State.LastProcessedTurn,BeforeNeeds,Buildings,NeedsFatalities);
}

void UCityLifeSubsystem::ApplyDisasterShock(EDisasterType Type,float Severity)
{
	if(!bInitializedForCity||!FMath::IsFinite(Severity)||Severity<=0)return;
	const float Strength=FMath::Clamp(Severity/100.f,0.f,1.f);
	if(Type==EDisasterType::Flood||Type==EDisasterType::Tsunami||Type==EDisasterType::Fire||Type==EDisasterType::Volcano)
	{
		State.FoodStock*=1.f-.15f*Strength;SupplyDisruptionTurns=FMath::Max(SupplyDisruptionTurns,2);
	}
	RefreshCity();
}

FAftershockScopedOccupancyBatch::FAftershockScopedOccupancyBatch(UCityLifeSubsystem* InOwner):Owner(InOwner)
{if(Owner)Owner->BeginOccupancyBatch();}
FAftershockScopedOccupancyBatch::~FAftershockScopedOccupancyBatch()
{if(Owner)Owner->EndOccupancyBatch();}

void UCityLifeSubsystem::BeginOccupancyBatch()
{
	check(IsInGameThread());
	if(OccupancyBatchDepth==0)InvalidateScopedOccupancyLookup();
	++OccupancyBatchDepth;
}
void UCityLifeSubsystem::EndOccupancyBatch()
{
	check(IsInGameThread());check(OccupancyBatchDepth>0);
	if(--OccupancyBatchDepth==0)InvalidateScopedOccupancyLookup();
}
void UCityLifeSubsystem::InvalidateScopedOccupancyLookup()
{
	bScopedOccupancyLookupValid=false;ScopedOccupancyResidentCount=INDEX_NONE;
	// Empty releases nested arrays and the map slabs, including after a large
	// event. No per-event history or idle population duplicate is retained.
	ScopedDayOccupants.Empty();ScopedNightOccupants.Empty();
}
void UCityLifeSubsystem::EnsureScopedOccupancyLookup() const
{
	check(IsInGameThread());check(OccupancyBatchDepth>0);
	if(bScopedOccupancyLookupValid&&ScopedOccupancyResidentCount==Residents.Num())return;
	ScopedDayOccupants.Empty();ScopedNightOccupants.Empty();
	for(int32 I=0;I<Residents.Num();++I)
	{
		const auto& Resident=Residents[I];if(!Resident.bAlive)continue;
		auto& Night=ScopedNightOccupants.FindOrAdd(Resident.HomeBuildingID);Night.Indices.Add(I);++Night.Living;
		const int32 DayLocation=Resident.WorkBuildingID!=INDEX_NONE?Resident.WorkBuildingID:Resident.HomeBuildingID;
		auto& Day=ScopedDayOccupants.FindOrAdd(DayLocation);Day.Indices.Add(I);++Day.Living;
	}
	ScopedOccupancyResidentCount=Residents.Num();bScopedOccupancyLookupValid=true;
}

int32 UCityLifeSubsystem::GetLivingOccupants(int32 BuildingID,bool bDaytime) const
{
	if(OccupancyBatchDepth>0)
	{
		EnsureScopedOccupancyLookup();
		const auto* At=(bDaytime?ScopedDayOccupants:ScopedNightOccupants).Find(BuildingID);
		return At?At->Living:0;
	}
	int32 Count=0;
	for(const FCityResidentRecord& R:Residents)if(R.bAlive)
	{
		const int32 Location=bDaytime&&R.WorkBuildingID!=INDEX_NONE?R.WorkBuildingID:R.HomeBuildingID;
		Count+=Location==BuildingID?1:0;
	}
	return Count;
}

int32 UCityLifeSubsystem::CommitFatalities(int32 BuildingID,int32 RequestedDeaths,bool bDaytime)
{
	return ApplyCasualties(BuildingID,RequestedDeaths,bDaytime).Num();
}

TArray<int32> UCityLifeSubsystem::ApplyCasualties(int32 BuildingID,int32 RequestedDeaths,bool bDaytime)
{
	TArray<int32> Assigned;
	if(RequestedDeaths<=0)return Assigned;
	if(OccupancyBatchDepth>0)
	{
		EnsureScopedOccupancyLookup();
		auto* At=(bDaytime?ScopedDayOccupants:ScopedNightOccupants).Find(BuildingID);
		if(At)while(At->NextCandidate<At->Indices.Num()&&Assigned.Num()<RequestedDeaths)
		{
			// Indices preserve the original canonical ARRAY order, not ID order.
			// A death through the other day/night view remains in this list and
			// is skipped here; both living counts were updated at its real death.
			FCityResidentRecord& Resident=Residents[At->Indices[At->NextCandidate++]];
			if(!Resident.bAlive)continue;
			const int32 DayLocation=Resident.WorkBuildingID!=INDEX_NONE?Resident.WorkBuildingID:Resident.HomeBuildingID;
			if(auto* Night=ScopedNightOccupants.Find(Resident.HomeBuildingID))--Night->Living;
			if(auto* Day=ScopedDayOccupants.Find(DayLocation))--Day->Living;
			Resident.bAlive=false;Resident.WorkBuildingID=INDEX_NONE;
			Assigned.Add(Resident.ResidentID);++State.TotalDeaths;
		}
		State.LivingPopulation=FMath::Max(0,State.LivingPopulation-Assigned.Num());
		return Assigned;
	}
	for(FCityResidentRecord& R:Residents)
	{
		if(Assigned.Num()>=FMath::Max(0,RequestedDeaths))break;
		if(!R.bAlive)continue;
		const int32 Location=bDaytime&&R.WorkBuildingID!=INDEX_NONE?R.WorkBuildingID:R.HomeBuildingID;
		if(Location!=BuildingID)continue;
		R.bAlive=false;R.WorkBuildingID=INDEX_NONE;Assigned.Add(R.ResidentID);++State.TotalDeaths;
	}
	State.LivingPopulation=FMath::Max(0,State.LivingPopulation-Assigned.Num());
	return Assigned;
}

bool UCityLifeSubsystem::GetResident(int32 ID,FCityResidentRecord& Out) const
{
	const auto Rebuild=[&](){ResidentIndexLookup.Reset();for(int32 I=0;I<Residents.Num();++I)ResidentIndexLookup.Add(Residents[I].ResidentID,I);IndexedResidentCount=Residents.Num();};
	if(IndexedResidentCount!=Residents.Num())Rebuild();
	const int32* I=ResidentIndexLookup.Find(ID);
	if(I&&Residents.IsValidIndex(*I)&&Residents[*I].ResidentID==ID){Out=Residents[*I];return true;}
	// If the lookup was stale for this specific key (e.g. elements swapped in place), rebuild once.
	if(I!=nullptr)
	{
		Rebuild();I=ResidentIndexLookup.Find(ID);
		if(I&&Residents.IsValidIndex(*I)&&Residents[*I].ResidentID==ID){Out=Residents[*I];return true;}
	}
	return false;
}

FCityBuildingStatus UCityLifeSubsystem::GetBuildingStatus(int32 ID) const {return BuildingStates.FindRef(ID);}
float UCityLifeSubsystem::GetBuildingOperation01(int32 ID) const {return BuildingStates.FindRef(ID).Operation01;}

int32 UCityLifeSubsystem::FindServiceDestination(int32 ID,EBuildingType Type) const
{
	FCityResidentRecord R;if(!GetResident(ID,R)||!R.bAlive)return INDEX_NONE;
	UBuildingManagerSubsystem* M=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();
	FBuildingInstance Home;if(!M->GetBuildingByID(R.HomeBuildingID,Home))return INDEX_NONE;
	const int32* HomeComp=BuildingRoadComponents.Find(R.HomeBuildingID);if(!HomeComp)return INDEX_NONE;
	TArray<FBuildingInstance> Buildings;M->GetAllBuildings(Buildings);int32 Result=INDEX_NONE,Best=MAX_int32;
	for(const FBuildingInstance& B:Buildings)
	{
		if(!B.BuildingData||B.BuildingData->Type!=Type||!GetBuildingStatus(B.InstanceID).bOperating)continue;
		const int32* Comp=BuildingRoadComponents.Find(B.InstanceID);if(!Comp||*Comp!=*HomeComp)continue;
		const int32 D=FMath::Abs(Home.GridCoords.X-B.GridCoords.X)+FMath::Abs(Home.GridCoords.Y-B.GridCoords.Y);
		if(D<Best||(D==Best&&B.InstanceID<Result)){Best=D;Result=B.InstanceID;}
	}
	return Result;
}

void UCityLifeSubsystem::BuildServiceDestinationLookup(FAftershockCityServiceLookup& Out) const
{
	Out={};
	const auto* Manager=GetWorld()?GetWorld()->GetSubsystem<UBuildingManagerSubsystem>():nullptr;
	if(!Manager)return;
	TArray<FBuildingInstance> Buildings;Manager->GetAllBuildings(Buildings);
	Out.BuildingCoords.Reserve(Buildings.Num());Out.RoadComponents=BuildingRoadComponents;
	for(const auto& B:Buildings)
	{
		++Out.BuildingsVisited;Out.BuildingCoords.Add(B.InstanceID,B.GridCoords);
		const int32* Component=BuildingRoadComponents.Find(B.InstanceID);
		if(!B.BuildingData||!Component||!GetBuildingStatus(B.InstanceID).bOperating)continue;
		Out.OperatingCandidates.FindOrAdd(FIntPoint(*Component,static_cast<int32>(B.BuildingData->Type))).Add({B.InstanceID,B.GridCoords});
	}
}

int32 UCityLifeSubsystem::FindServiceDestination(int32 ID,EBuildingType Type,FAftershockCityServiceLookup& Lookup) const
{
	++Lookup.Queries;
	FCityResidentRecord Resident;if(!GetResident(ID,Resident)||!Resident.bAlive)return INDEX_NONE;
	const FIntPoint* Home=Lookup.BuildingCoords.Find(Resident.HomeBuildingID);
	const int32* Component=Lookup.RoadComponents.Find(Resident.HomeBuildingID);
	if(!Home||!Component)return INDEX_NONE;
	const FIntVector Key(Resident.HomeBuildingID,static_cast<int32>(Type),*Component);
	if(const int32* Cached=Lookup.DestinationsByHomeTypeComponent.Find(Key)){++Lookup.CacheHits;return *Cached;}
	int32 Result=INDEX_NONE,Best=MAX_int32;
	if(const auto* Candidates=Lookup.OperatingCandidates.Find(FIntPoint(*Component,static_cast<int32>(Type))))for(const auto& Candidate:*Candidates)
	{
		++Lookup.CandidatesVisited;
		const int32 Distance=FMath::Abs(Home->X-Candidate.Coord.X)+FMath::Abs(Home->Y-Candidate.Coord.Y);
		if(Distance<Best||(Distance==Best&&Candidate.BuildingID<Result)){Best=Distance;Result=Candidate.BuildingID;}
	}
	Lookup.DestinationsByHomeTypeComponent.Add(Key,Result);return Result;
}

bool UCityLifeSubsystem::SetTaxRates(float Residential,float Business)
{
	if(!bInitializedForCity||!FMath::IsFinite(Residential)||!FMath::IsFinite(Business))return false;
	State.ResidentialTaxRate=FMath::Clamp(Residential,0.f,.30f);State.BusinessTaxRate=FMath::Clamp(Business,0.f,.30f);RefreshCity();return true;
}

bool UCityLifeSubsystem::ReportServiceVisit(int32 ID,int32 BuildingID,FVector ArrivalLocation)
{
	if(!bInitializedForCity||bAdvancing||ArrivalLocation.ContainsNaN())return false;
	UBuildingManagerSubsystem* M=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();
	const UTerrainGridSubsystem* T=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
	FCityResidentRecord* R=Residents.FindByPredicate([ID](const FCityResidentRecord& Item){return Item.ResidentID==ID;});
	FBuildingInstance B;
	if(!R||!R->bAlive||!M||!T||!M->GetBuildingByID(BuildingID,B)||!B.BuildingData||GetBuildingOperation01(BuildingID)<=.1f)return false;
	if(FVector::DistSquared2D(ArrivalLocation,T->GridToWorld(B.GridCoords))>FMath::Square(T->CellSize*1.8f))return false;
	const int32* HomeComp=BuildingRoadComponents.Find(R->HomeBuildingID),*ServiceComp=BuildingRoadComponents.Find(BuildingID);
	if(!HomeComp||!ServiceComp||*HomeComp!=*ServiceComp)return false;
	const EBuildingType Type=B.BuildingData->Type;
	const bool Food=Type==EBuildingType::Bakery||Type==EBuildingType::Restaurant;
	if(!Food&&Type!=EBuildingType::Hospital&&Type!=EBuildingType::GreenSpace&&Type!=EBuildingType::PoliceStation&&Type!=EBuildingType::Courthouse)return false;
	const int64 VisitKey=(static_cast<int64>(ID)<<8)|static_cast<uint8>(Type);
	if(const int32* Previous=LastServiceVisitTurn.Find(VisitKey);Previous&&*Previous>=State.LastProcessedTurn)return false;
	if(Food)
	{
		if(State.FoodStock<.05f)return false;
		State.FoodStock-=.05f;R->Hunger01=FMath::Max(0.f,R->Hunger01-.10f);
	}
	else if(Type==EBuildingType::Hospital)R->Health01=FMath::Min(1.f,R->Health01+.10f*GetBuildingOperation01(BuildingID));
	R->Happiness01=FMath::Min(1.f,R->Happiness01+.015f);
	R->Emotion=R->Hunger01>.35f?ECityResidentEmotion::Hungry:R->Health01<.45f?ECityResidentEmotion::Sick:R->Happiness01>=.65f?ECityResidentEmotion::Happy:R->Happiness01<.45f?ECityResidentEmotion::Sad:ECityResidentEmotion::Neutral;
	LastServiceVisitTurn.Add(VisitKey,State.LastProcessedTurn);
	return true;
}

bool UCityLifeSubsystem::RecordTransitFare(int32 ID,int32 TripID)
{
	FCityResidentRecord R;
	if(!bInitializedForCity||TripID<=0||PaidTransitTrips.Contains(TripID)||!GetResident(ID,R)||!R.bAlive)return false;
	PaidTransitTrips.Add(TripID);PendingTransitRevenueTL+=5;return true;
}

FText UCityLifeSubsystem::DescribeCity() const
{
	return FText::FromString(FString::Printf(TEXT("Nüfus %d | Çalışan %d/%d\nGıda %.0f | Ham ürün %.0f | Su %.0f | Yakıt %.0f\nGıda %%%.0f | Elektrik %%%.0f | Su %%%.0f\nSağlık %%%.0f | Güvenlik %%%.0f | Adalet %%%.0f\nHane vergisi %%%.0f | İşletme %%%.0f\nVergi tahsilatı %%%.0f | Net hizmet memnuniyeti %%%.0f"),State.LivingPopulation,State.EmployedResidents,State.JobSlots,State.FoodStock,State.RawFoodStock,State.WaterStock,State.FuelStock,State.FoodCoverage01*100,State.PowerCoverage01*100,State.WaterCoverage01*100,State.HealthCoverage01*100,State.SafetyCoverage01*100,State.JusticeCoverage01*100,State.ResidentialTaxRate*100,State.BusinessTaxRate*100,State.TaxCollectionEfficiency01*100,State.AverageHappiness01*100));
}

FText UCityLifeSubsystem::DescribeBuilding(int32 ID) const
{
	const FCityBuildingStatus S=GetBuildingStatus(ID);
	return FText::FromString(FString::Printf(TEXT("Personel %d/%d | İşletme %%%.0f\nElektrik %%%.0f | %s\nÜrün/öğün: %.1f / tur"),S.AssignedWorkers,S.JobCapacity,S.Operation01*100,S.PowerCoverage01*100,*S.FailureReason.ToString(),S.FoodOutputPerTurn));
}

void UCityLifeSubsystem::Deinitialize()
{
	InvalidateScopedOccupancyLookup();
	bInitializedForCity = false;
	bRefreshing = false;
	bAdvancing = false;
	State = FCityLifeState();
	Residents.Reset();
	BuildingStates.Reset();
	SeededHomes.Reset();
	PaidTransitTrips.Reset();
	LastServiceVisitTurn.Reset();
	BuildingRoadComponents.Reset();
	SupplyDisruptionTurns = 0;
	NextResidentID = 1;
	PendingTransitRevenueTL = 0;
	OnCityLifeUpdated.Clear();
	Super::Deinitialize();
}
