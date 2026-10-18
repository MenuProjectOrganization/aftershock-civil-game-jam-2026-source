// AFTERSHOCK — Civil Game Jam 2026
// Deterministik, ISM tabanlı vatandaş görselleştirme ve yaşam döngüsü.

#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "AftershockRoadCpuScope.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/DisasterPresentationSubsystem.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/TransportSubsystem.h"
#include "Aftershock/Public/CityPresentationSubsystem.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "AftershockTerrainSurface.h"
#include "AftershockIncrementalNormalRoutes.h"
#include "AftershockPresentationBudget.h"
#include "AftershockPresentationAudit.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

CSV_DEFINE_CATEGORY(AftershockCrowd, true);

namespace
{
	constexpr int32 MaxVisualCitizens = 1500;
    uint64 NormalRouteAgentKey(const FCitizenAgent& Agent)
    {return Agent.ResidentID>0?static_cast<uint64>(Agent.ResidentID)<<1:(static_cast<uint64>(FMath::Max(0,Agent.ISMIndex))<<1)|1ull;}
	constexpr float CrowdUpdateSeconds = 0.08f;
	constexpr float CitizenZOffset = 5.0f;
	constexpr float CitizenVisibleHeightCm = 175.0f;
	constexpr int32 CitizenCustomFloatCount = 7;
	constexpr float CitizenBodyRadiusCm=32.f;
	constexpr float CitizenSpatialBucketCm=128.f;
	constexpr int32 CitizenAdmissionCheckBudget=64;
	FIntPoint CitizenBucket(const FVector& At)
	{ return FIntPoint(FMath::FloorToInt(At.X/CitizenSpatialBucketCm),FMath::FloorToInt(At.Y/CitizenSpatialBucketCm)); }
	uint32 CitizenHash(uint32 Value)
	{
		Value ^= Value >> 16; Value *= 0x7feb352du; Value ^= Value >> 15; Value *= 0x846ca68bu; return Value ^ (Value >> 16);
	}
	float CitizenUnit(uint32 Seed) { return (CitizenHash(Seed) & 0x00ffffffu) / 16777216.f; }
	float CitizenDwell(FCitizenAgent& Agent,float BaseSeconds)
	{
		const uint32 Sequence=static_cast<uint32>(Agent.RandomSequence);
		Agent.RandomSequence=static_cast<int32>((Sequence+1u)&0x7fffffffu);
		return BaseSeconds*(.65f+CitizenUnit(static_cast<uint32>(Agent.RandomSeed)^Sequence*0x9e3779b9u)*.9f);
	}
	// This is a display-routing cutoff, not a physical pedestrian safety model.
	constexpr float VisibleWaterRouteCutoffCm = 35.f;
	const FIntPoint CitizenNeighborOffsets[] = { FIntPoint(1, 0), FIntPoint(0, 1), FIntPoint(-1, 0), FIntPoint(0, -1) };
	bool IsCitizenIndoors(const FCitizenAgent& Agent,const UWorld* World)
	{
		// A derived service vehicle exclusively represents this existing worker
		// until its road return releases the assignment, even if its station
		// becomes unsafe meanwhile. Never expose a duplicate walking body.
		if(const auto* Transport=World?World->GetSubsystem<UTransportSubsystem>():nullptr)
		{FAftershockMunicipalDutyView Duty;if(Transport->GetMunicipalDuty(Agent.ResidentID,Duty))return true;}
		const auto Usable=[&](int32 ID)
		{
			if(ID==INDEX_NONE)return false;
			const auto* Buildings=World?World->GetSubsystem<UBuildingManagerSubsystem>():nullptr;FBuildingInstance Building;
			if(Buildings&&Buildings->GetBuildingByID(ID,Building))return Building.DamageState<EDamageState::Collapsed;
			const auto* Life=World?World->GetSubsystem<UCityLifeSubsystem>():nullptr;
			return !Life||!Life->IsCityInitialized(); // Explicit legacy isolated fixtures only.
		};
		if(Agent.bWaitingForOutdoorSpace&&!Agent.bInsideVehicle)
		{
			if(Agent.OutdoorDepartureBuildingID!=INDEX_NONE)return Usable(Agent.OutdoorDepartureBuildingID)&&FVector::DistSquared2D(Agent.CurrentLocation,Agent.OutdoorDepartureLocation)<=FMath::Square(64.f);
			const auto At=[&](int32 ID,const FVector& Anchor){return Usable(ID)&&FVector::DistSquared2D(Agent.CurrentLocation,Anchor)<=FMath::Square(64.f);};
			return At(Agent.HomeBuildingID,Agent.HomeLocation)||At(Agent.WorkBuildingID,Agent.WorkLocation)||At(Agent.FoodBuildingID,Agent.FoodLocation)||At(Agent.HealthBuildingID,Agent.HealthLocation)
				||(Agent.bEmergencyPlaceReserved&&At(Agent.EmergencyBuildingID,Agent.ShelterLocation));
		}
		// A legacy display representative can still physically enter a real,
		// reserved aid centre. It must not keep blocking the entrance outdoors.
		if(!Agent.bInsideVehicle&&Agent.MoveSpeed<=0.f&&Agent.Activity==EAftershockCitizenActivity::AtShelter&&Agent.bEmergencyPlaceReserved)
		{
			const auto* Buildings=World?World->GetSubsystem<UBuildingManagerSubsystem>():nullptr;FBuildingInstance Centre;
			if(Buildings&&Buildings->GetBuildingByID(Agent.EmergencyBuildingID,Centre)&&Centre.DamageState<EDamageState::Collapsed)return true;
		}
		if (Agent.ResidentID==INDEX_NONE || Agent.bInsideVehicle || Agent.MoveSpeed>0.f) return false;
		const bool IndoorActivity=Agent.Activity==EAftershockCitizenActivity::AtHome||Agent.Activity==EAftershockCitizenActivity::AtWork||Agent.Activity==EAftershockCitizenActivity::AtFoodService||Agent.Activity==EAftershockCitizenActivity::AtHealthService||Agent.Activity==EAftershockCitizenActivity::AtShelter;
		if(IndoorActivity&&Agent.OutdoorDepartureBuildingID!=INDEX_NONE&&FVector::DistSquared2D(Agent.CurrentLocation,Agent.OutdoorDepartureLocation)<=FMath::Square(64.f))return Usable(Agent.OutdoorDepartureBuildingID);
		switch (Agent.Activity)
		{
		// A power/service outage does not materialize an entire household at one
		// frontage. Departure/evacuation admits bodies through the same doorway.
		case EAftershockCitizenActivity::AtHome: return Usable(Agent.HomeBuildingID);
		case EAftershockCitizenActivity::AtWork: return Usable(Agent.WorkBuildingID);
		case EAftershockCitizenActivity::AtFoodService: return Usable(Agent.FoodBuildingID);
		case EAftershockCitizenActivity::AtHealthService: return Usable(Agent.HealthBuildingID);
		case EAftershockCitizenActivity::AtShelter:
		{
			const auto* Life=World?World->GetSubsystem<UCityLifeSubsystem>():nullptr;
			return Agent.bEmergencyPlaceReserved&&(Usable(Agent.EmergencyBuildingID)||(!Life||!Life->IsCityInitialized()));
		}
		default: return false;
		}
	}
	bool IsMunicipalWorkAnchorSafe(const FCitizenAgent& Agent,const UWorld* World)
	{
		if(!World||Agent.Activity!=EAftershockCitizenActivity::AtWork||!Agent.bWorkOperating||!IsCitizenIndoors(Agent,World))return false;
		const auto* Life=World->GetSubsystem<UCityLifeSubsystem>();FCityResidentRecord Resident;
		if(!Life||!Life->GetResident(Agent.ResidentID,Resident)||!Resident.bAlive||Resident.WorkBuildingID!=Agent.WorkBuildingID)return false;
		// Job reassignment retains the previous real indoor departure anchor.
		// Its AtWork label must not authorize a vehicle at the NEW employer.
		if(Agent.OutdoorDepartureBuildingID!=INDEX_NONE&&FVector::DistSquared2D(Agent.CurrentLocation,Agent.OutdoorDepartureLocation)<=FMath::Square(64.f))
		{if(Agent.OutdoorDepartureBuildingID!=Agent.WorkBuildingID)return false;}
		else if(FVector::DistSquared2D(Agent.CurrentLocation,Agent.WorkLocation)>FMath::Square(64.f))return false;
		const auto* Buildings=World->GetSubsystem<UBuildingManagerSubsystem>();FBuildingInstance Work;
		const auto* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();
		if(!Buildings||!Terrain||!Buildings->GetBuildingByID(Agent.WorkBuildingID,Work)||Work.DamageState>=EDamageState::Damaged)return false;
		const auto Cell=Terrain->GetCell(Work.GridCoords);
		const auto* Weather=World->GetSubsystem<UDisasterPresentationSubsystem>();
		return !Cell.bBurning&&Cell.LavaCoverage01<=.01f&&(!Weather||Weather->GetDisplayedFloodDepthCm(Work.GridCoords)<=VisibleWaterRouteCutoffCm);
	}
	void CaptureIndoorDepartureAnchor(FCitizenAgent& Agent,const UWorld* World,bool bForce=false)
	{
		if(bForce){Agent.OutdoorDepartureBuildingID=INDEX_NONE;Agent.OutdoorDepartureLocation=FVector::ZeroVector;}
		if(!IsCitizenIndoors(Agent,World))return;
		if(!bForce&&Agent.OutdoorDepartureBuildingID!=INDEX_NONE&&FVector::DistSquared2D(Agent.CurrentLocation,Agent.OutdoorDepartureLocation)<=FMath::Square(64.f))return;
		int32 ID=INDEX_NONE;
		switch(Agent.Activity)
		{
		case EAftershockCitizenActivity::AtHome:ID=Agent.HomeBuildingID;break;
		case EAftershockCitizenActivity::AtWork:ID=Agent.WorkBuildingID;break;
		case EAftershockCitizenActivity::AtFoodService:ID=Agent.FoodBuildingID;break;
		case EAftershockCitizenActivity::AtHealthService:ID=Agent.HealthBuildingID;break;
		case EAftershockCitizenActivity::AtShelter:ID=Agent.EmergencyBuildingID;break;
		default:break;
		}
		if(ID!=INDEX_NONE){Agent.OutdoorDepartureBuildingID=ID;Agent.OutdoorDepartureLocation=Agent.CurrentLocation;}
	}

	// One weighted reverse tree per destination/centre, shared by every agent.
	// The explicit min heap and stable cell-index tie break avoid hash/random order.
	FAftershockCitizenRouteTree BuildCitizenRouteTree(const UTerrainGridSubsystem& Terrain,
		const TArray<int32>& CellCosts, const TArray<FIntPoint>& Sources)
	{
		FAftershockCitizenRouteTree Tree;
		const URoadNetworkSubsystem* Roads=Terrain.GetWorld()?Terrain.GetWorld()->GetSubsystem<URoadNetworkSubsystem>():nullptr;
		const int32 Count = Terrain.Cells.Num();
		Tree.Next.Init(INDEX_NONE, Count);
		Tree.Cost.Init(MAX_int32, Count);
		struct FEntry { int32 Cost; int32 Index; };
		TArray<FEntry> Heap;
		const auto Earlier = [](const FEntry& A, const FEntry& B)
		{ return A.Cost < B.Cost || (A.Cost == B.Cost && A.Index < B.Index); };
		const auto Push = [&Heap, &Earlier](FEntry Entry)
		{
			int32 At = Heap.Add(Entry);
			while (At > 0)
			{
				const int32 Parent = (At - 1) / 2;
				if (!Earlier(Heap[At], Heap[Parent])) break;
				Heap.Swap(At, Parent);
				At = Parent;
			}
		};
		for (const FIntPoint Source : Sources)
		{
			if (!Terrain.IsPlayableCoord(Source)) continue;
			const int32 Index = Terrain.CoordToIndex(Source);
			if (!CellCosts.IsValidIndex(Index) || CellCosts[Index] == MAX_int32 || Tree.Cost[Index] == 0) continue;
			Tree.Cost[Index] = 0;
			Tree.Next[Index] = Index;
			Push({0, Index});
		}
		while (!Heap.IsEmpty())
		{
			const FEntry Entry = Heap[0];
			Heap[0] = Heap.Last();
			Heap.Pop(EAllowShrinking::No);
			for (int32 At = 0; At < Heap.Num();)
			{
				const int32 Left = At * 2 + 1, Right = Left + 1;
				int32 Best = At;
				if (Left < Heap.Num() && Earlier(Heap[Left], Heap[Best])) Best = Left;
				if (Right < Heap.Num() && Earlier(Heap[Right], Heap[Best])) Best = Right;
				if (Best == At) break;
				Heap.Swap(At, Best);
				At = Best;
			}
			if (Entry.Cost != Tree.Cost[Entry.Index]) continue;
			const FIntPoint Current = Terrain.IndexToCoord(Entry.Index);
			for (const FIntPoint Offset : CitizenNeighborOffsets)
			{
				const FIntPoint Neighbor = Current + Offset;
				if (!Terrain.IsPlayableCoord(Neighbor)) continue;
				if (Roads && !Roads->CanTraverseWalkingEdge(Neighbor,Current)) continue;
				const int32 Index = Terrain.CoordToIndex(Neighbor);
				if (!CellCosts.IsValidIndex(Index) || CellCosts[Index] == MAX_int32) continue;
				const int32 Candidate = Entry.Cost + CellCosts[Entry.Index];
				if (Candidate >= Tree.Cost[Index]) continue;
				Tree.Cost[Index] = Candidate;
				Tree.Next[Index] = Entry.Index;
				Push({Candidate, Index});
			}
		}
		return Tree;
	}

	FBox CitizenSourceBounds(const UStaticMesh* Mesh)
	{
		if (!Mesh) return FBox(FVector(-25,-25,0),FVector(25,25,175));
		const FBox RenderBounds=Mesh->GetBoundingBox();
		// WPO culling margins are not anatomy. Do not shrink the body or lift its
		// feet when the shared animated mesh reserves space for a moving limb.
		return FBox(RenderBounds.Min+Mesh->GetNegativeBoundsExtension(),RenderBounds.Max-Mesh->GetPositiveBoundsExtension());
	}

	FVector CitizenRenderScale(const UStaticMesh* Mesh)
	{
		const float SourceHeight = CitizenSourceBounds(Mesh).GetSize().Z;
		const float Scale = CitizenVisibleHeightCm / FMath::Max(1.0f, SourceHeight);
		return FVector(Scale);
	}

	bool IsPublicWalkingCell(const UTerrainGridSubsystem& Terrain, FIntPoint Coord)
	{
		if (const URoadNetworkSubsystem* Roads=Terrain.GetWorld()?Terrain.GetWorld()->GetSubsystem<URoadNetworkSubsystem>():nullptr)
			return Roads->IsPublicWalkingCell(Coord);
		if (!Terrain.IsPlayableCoord(Coord)) return false;
		const FGridCell Cell = Terrain.GetCell(Coord);
		return !Cell.bIsRiverbed && !Cell.bIsOcean && !Cell.bBurning && Cell.LavaCoverage01 <= .1f && Cell.OccupyingBuildingID == INDEX_NONE;
	}

	// Roster synchronization repairs edited geometry only. A hazard changing
	// traversability must never teleport an existing person to safety.
	bool IsCitizenGeometryCell(const UTerrainGridSubsystem& Terrain, FIntPoint Coord)
	{
		if (!Terrain.IsPlayableCoord(Coord)) return false;
		const FGridCell Cell=Terrain.GetCell(Coord);
		if (Cell.bIsOcean || (Cell.bIsRiverbed && !(Cell.bHasRoad && Cell.BridgeID!=INDEX_NONE))) return false;
		if (Cell.OccupyingBuildingID==INDEX_NONE) return true;
		const UBuildingManagerSubsystem* Buildings=Terrain.GetWorld()?Terrain.GetWorld()->GetSubsystem<UBuildingManagerSubsystem>():nullptr;
		FBuildingInstance Building;
		return Buildings && Buildings->GetBuildingByID(Cell.OccupyingBuildingID,Building) && Building.BuildingData && Building.BuildingData->Type==EBuildingType::GreenSpace;
	}

	bool CanTakeCitizenEscapeStep(const UTerrainGridSubsystem& Terrain, FIntPoint From, FIntPoint To)
	{
		if (!Terrain.IsPlayableCoord(To)) return false;
		// A person caught in an ordinary ground cell may leave it once. Elevated
		// decks still obey the bridge graph: no sideways jump or water shortcut.
		if (Terrain.GetCell(From).BridgeID==INDEX_NONE && Terrain.GetCell(To).BridgeID==INDEX_NONE) return true;
		const URoadNetworkSubsystem* Roads=Terrain.GetWorld()?Terrain.GetWorld()->GetSubsystem<URoadNetworkSubsystem>():nullptr;
		return Roads && Roads->CanTraverseWalkingEdge(From,To);
	}

	// This is only a sample of the visible interpolation between canonical heights.
	float VisibleGroundHeight(const UTerrainGridSubsystem& Terrain, const FVector& Position)
	{
		return Terrain.CellSize>0.f?SampleAftershockResidentElevation(Terrain,(Position.X-Terrain.GridOrigin.X)/Terrain.CellSize-.5f,(Position.Y-Terrain.GridOrigin.Y)/Terrain.CellSize-.5f):Terrain.GridOrigin.Z;
	}

	FLinearColor GetCitizenWardrobeColor(const FCitizenAgent& Agent, int32 CitySeed)
	{
		// Clothing belongs to the person. A commute or evacuation must not turn
		// the entire city into an identically coloured activity uniform; needs and
		// emergency status already have their separate billboard/gait channels.
		static const FLinearColor Wardrobe[]={
			{.20f,.30f,.43f},{.48f,.52f,.55f},{.58f,.23f,.16f},{.34f,.41f,.23f},
			{.70f,.65f,.52f},{.78f,.77f,.70f},{.22f,.46f,.48f},{.42f,.28f,.40f},
			{.25f,.33f,.58f},{.71f,.55f,.20f},{.10f,.13f,.15f},{.60f,.42f,.30f}};
		const uint32 Identity=Agent.ResidentID!=INDEX_NONE?static_cast<uint32>(Agent.ResidentID):static_cast<uint32>(Agent.ISMIndex+1)^0x8a279u;
		return Wardrobe[CitizenHash(Identity^static_cast<uint32>(CitySeed)^0x62d54u)%UE_ARRAY_COUNT(Wardrobe)];
	}

	bool IsShelterVisualRecord(const UBuildingData* Data)
	{
		if (!Data)
		{
			return false;
		}

	// Exact canonical type; label matching keeps older authored shelter records compatible.
	const FString Identity = (Data->DisplayName.ToString() + TEXT(" ") + Data->GetName()).ToLower();
	return Data->Type == EBuildingType::Shelter || Identity.Contains(TEXT("sığınak")) || Identity.Contains(TEXT("siginak")) || Identity.Contains(TEXT("shelter"));
	}
}

AAftershockCrowdHostActor::AAftershockCrowdHostActor()
{
	PrimaryActorTick.bCanEverTick = false;

	RootComp = CreateDefaultSubobject<USceneComponent>(TEXT("CrowdRoot"));
	SetRootComponent(RootComp);

	CitizenISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("CitizenISM"));
	CitizenISM->SetupAttachment(RootComp);
	CitizenISM->SetMobility(EComponentMobility::Movable);
	CitizenISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CitizenISM->SetCastShadow(true);
	CitizenISM->SetCanEverAffectNavigation(false);
	CitizenISM->SetCullDistances(0, 0);
	CitizenISM->NumCustomDataFloats = CitizenCustomFloatCount;
	EmotionISM=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ResidentEmotionBillboards"));
	EmotionISM->SetupAttachment(RootComp);EmotionISM->SetMobility(EComponentMobility::Movable);
	EmotionISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);EmotionISM->SetCanEverAffectNavigation(false);
	EmotionISM->SetCastShadow(false);EmotionISM->NumCustomDataFloats=1;EmotionISM->SetCullDistances(0,35000);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> EmotionPlane(TEXT("/Engine/BasicShapes/Plane.Plane"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> EmotionMaterial(TEXT("/Game/Environment/Transport/Materials/M_ResidentEmotions.M_ResidentEmotions"));
	if(EmotionPlane.Succeeded())EmotionISM->SetStaticMesh(EmotionPlane.Object);
	if(EmotionMaterial.Succeeded())EmotionISM->SetMaterial(0,EmotionMaterial.Object);

	TerrainISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("TerrainISM"));
	TerrainISM->SetupAttachment(RootComp);
	TerrainISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	TerrainISM->SetCastShadow(false);
	TerrainISM->SetCanEverAffectNavigation(false);
	TerrainISM->NumCustomDataFloats = 3;

	RiverISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("RiverISM"));
	RiverISM->SetupAttachment(RootComp);
	RiverISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RiverISM->SetCastShadow(false);
	RiverISM->SetCanEverAffectNavigation(false);

	TerrainSurface = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ContinuousTerrainSurface"));
	TerrainSurface->SetupAttachment(RootComp);
	TerrainSurface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	TerrainSurface->SetCanEverAffectNavigation(false);
	TerrainSurface->SetCastShadow(true);
	RiverSurface = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ContinuousRiverSurface"));
	RiverSurface->SetupAttachment(RootComp);
	RiverSurface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RiverSurface->SetCanEverAffectNavigation(false);
	RiverSurface->SetCastShadow(false);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GroundPresentationFinder(TEXT("/Game/Environment/Materials/Presentation/M_PresentationGroundPBR.M_PresentationGroundPBR"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> WaterPresentationFinder(TEXT("/Game/Environment/Materials/Presentation/M_PresentationWater.M_PresentationWater"));
	if (GroundPresentationFinder.Succeeded()) TerrainSurface->SetMaterial(0, GroundPresentationFinder.Object);
	if (WaterPresentationFinder.Succeeded()) RiverSurface->SetMaterial(0, WaterPresentationFinder.Object);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> DefaultMeshFinder(TEXT("/Game/Aftershock/KenneyMeshes/Imported/SM_CitizenProxy.SM_CitizenProxy"));
	if (DefaultMeshFinder.Succeeded())
	{
		CitizenISM->SetStaticMesh(DefaultMeshFinder.Object);
	}
	else if (UStaticMesh* FallbackCitizenMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder")))
	{
		CitizenISM->SetStaticMesh(FallbackCitizenMesh);
	}

	static ConstructorHelpers::FObjectFinder<UStaticMesh> TerrainCubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (TerrainCubeFinder.Succeeded())
	{
		// Use shallow cubes instead of zero-thickness planes so the terrain remains visible
		// from the default 2.5D camera and keeps a clear cell/elevation silhouette.
		TerrainISM->SetStaticMesh(TerrainCubeFinder.Object);
		RiverISM->SetStaticMesh(TerrainCubeFinder.Object);
	}

	const FSoftObjectPath CitizenMaterialPath(TEXT("/Game/Environment/Materials/M_CitizenActivity.M_CitizenActivity"));
	if (UMaterialInterface* CitizenMaterial = Cast<UMaterialInterface>(CitizenMaterialPath.TryLoad()))
	{
		CitizenISM->SetMaterial(0, CitizenMaterial);
	}
	const FSoftObjectPath TerrainMaterialPath(TEXT("/Game/Environment/Materials/M_TerrainCell.M_TerrainCell"));
	if (UMaterialInterface* TerrainMaterial = Cast<UMaterialInterface>(TerrainMaterialPath.TryLoad()))
	{
		TerrainISM->SetMaterial(0, TerrainMaterial);
	}
	const FSoftObjectPath RiverMaterialPath(TEXT("/Game/Environment/Materials/M_AftershockWater.M_AftershockWater"));
	if (UMaterialInterface* RiverMaterial = Cast<UMaterialInterface>(RiverMaterialPath.TryLoad()))
	{
		RiverISM->SetMaterial(0, RiverMaterial);
	}

	static ConstructorHelpers::FObjectFinder<UNiagaraSystem> FloodSystemFinder(TEXT("/Game/FX/Aftershock/Systems/NS_FloodSpray.NS_FloodSpray"));
	if (FloodSystemFinder.Succeeded())
	{
		FloodSpraySystem = FloodSystemFinder.Object;
	}
	static ConstructorHelpers::FObjectFinder<UNiagaraSystem> EarthquakeSystemFinder(TEXT("/Game/FX/Aftershock/Systems/NS_EarthquakeDust.NS_EarthquakeDust"));
	if (EarthquakeSystemFinder.Succeeded())
	{
		EarthquakeDustSystem = EarthquakeSystemFinder.Object;
	}
	static ConstructorHelpers::FObjectFinder<UNiagaraSystem> WindSystemFinder(TEXT("/Game/FX/Aftershock/Systems/NS_WindDebris.NS_WindDebris"));
	if (WindSystemFinder.Succeeded())
	{
		WindDebrisSystem = WindSystemFinder.Object;
	}
}

bool AAftershockCrowdHostActor::SpawnCosmeticFx(EAftershockCosmeticFx Effect, const FVector& WorldLocation, float Scale)
{
	UNiagaraSystem* System = nullptr;
	switch (Effect)
	{
	case EAftershockCosmeticFx::FloodSpray:     System = FloodSpraySystem; break;
	case EAftershockCosmeticFx::EarthquakeDust: System = EarthquakeDustSystem; break;
	case EAftershockCosmeticFx::WindDebris:     System = WindDebrisSystem; break;
	default: return false;
	}

	if (!System || !GetWorld())
	{
		return false;
	}

	return IsValid(UNiagaraFunctionLibrary::SpawnSystemAtLocation(
		GetWorld(), System, WorldLocation, FRotator::ZeroRotator,
		FVector(FMath::Clamp(Scale, 0.1f, 5.0f)), true, true, ENCPoolMethod::AutoRelease, true));
}

void AAftershockCrowdHostActor::BuildTerrainVisual(const UTerrainGridSubsystem* Terrain)
{
	if (!Terrain || !TerrainISM || !RiverISM)
	{
		return;
	}

	RenderedLandCellCount = 0;
	RenderedRiverCellCount = 0;
	BuiltTerrainRevision = Terrain->GetTerrainRevision();
	const auto* HydraulicOwner=GetWorld()?GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>():nullptr;
	bBuiltHydraulicModel2=HydraulicOwner&&HydraulicOwner->IsHydraulicModel2Active();
	BuiltVisibleCellBounds=GetAftershockVisibleCellBounds(*Terrain);
	for (const FGridCell& Cell : Terrain->Cells)
	{
		if (!Cell.bIsPlayable||!BuiltVisibleCellBounds.Contains(Cell.Coords)) continue;
		Cell.bIsRiverbed ? ++RenderedRiverCellCount : ++RenderedLandCellCount;
	}
	if (BuildAftershockTerrainSurface(this, Terrain, TerrainSurface, RiverSurface))
	{
		// Replace the visible tile pipeline rather than drawing a second terrain.
		TerrainISM->ClearInstances();
		RiverISM->ClearInstances();
		TerrainSurface->SetVisibility(true);
		RiverSurface->SetVisibility(true);
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_RUNTIME_TERRAIN grid=%dx%d land=%d river=%d mode=continuous seed=%d groundMaterial=%s waterMaterial=%s"),
			Terrain->GridWidth, Terrain->GridHeight, RenderedLandCellCount, RenderedRiverCellCount, Terrain->TerrainSeed,
			TerrainSurface->GetMaterial(0) ? *TerrainSurface->GetMaterial(0)->GetPathName() : TEXT("None"),
			RiverSurface->GetMaterial(0) ? *RiverSurface->GetMaterial(0)->GetPathName() : TEXT("None"));
		return;
	}
	UE_LOG(LogTemp, Warning, TEXT("AFTERSHOCK_PRESENTATION_SURFACE_FAILED: using cell fallback"));
	TerrainSurface->SetVisibility(false);
	RiverSurface->SetVisibility(false);

	// Mesh atanmamışsa /Engine/BasicShapes/Cube.Cube fallback'i yükle
	if (!TerrainISM->GetStaticMesh())
	{
		if (UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
		{
			TerrainISM->SetStaticMesh(CubeMesh);
		}
	}
	if (!RiverISM->GetStaticMesh())
	{
		if (UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
		{
			RiverISM->SetStaticMesh(CubeMesh);
		}
	}

	TerrainISM->ClearInstances();
	RiverISM->ClearInstances();

	const float CellScale = FMath::Max(0.01f, (Terrain->CellSize / 100.0f));
	const FLinearColor LowlandColor(0.20f, 0.52f, 0.24f, 1.0f);   // Canlı çimen yeşili
	const FLinearColor HighlandColor(0.65f, 0.58f, 0.40f, 1.0f);  // Tepe / yayla tonu
	const FLinearColor RiverWaterColor(0.10f, 0.42f, 0.88f, 0.90f); // Kristal berrak nehir mavisi

	const bool bIsCube = TerrainISM->GetStaticMesh() && TerrainISM->GetStaticMesh()->GetName().Contains(TEXT("Cube"));

	for (const FGridCell& Cell : Terrain->Cells)
	{
			const FIntPoint Coord = Cell.Coords;
			if (!Terrain->IsPlayableCoord(Coord)||!BuiltVisibleCellBounds.Contains(Coord)) continue;
			FVector Location = Terrain->GridToWorld(Coord, true);

			if (Cell.bIsRiverbed)
			{
				// Nehir hücreleri alçak ve kesintisiz
				FTransform RiverTransform;
				if (bIsCube)
				{
					RiverTransform = FTransform(FRotator::ZeroRotator, Location + FVector(0.f, 0.f, -10.f), FVector(CellScale, CellScale, 0.08f));
				}
				else
				{
					RiverTransform = FTransform(FRotator::ZeroRotator, Location + FVector(0.f, 0.f, -10.f), FVector(CellScale, CellScale, 1.0f));
				}
				RiverISM->AddInstance(RiverTransform, true);
				continue;
			}

			// Arazi hücreleri neredeyse bitişik ve ince kalsın; 20 cm'lik yarıklar ile
			// 25 cm'lik yükseltilmiş bloklar tüm şehri oyuncak dama tahtası gibi gösteriyordu.
			FTransform LandTransform;
			if (bIsCube)
			{
				LandTransform = FTransform(FRotator::ZeroRotator, Location, FVector(CellScale * 0.99f, CellScale * 0.99f, 0.04f));
			}
			else
			{
				LandTransform = FTransform(FRotator::ZeroRotator, Location, FVector(CellScale * 0.96f, CellScale * 0.96f, 1.0f));
			}

			const int32 InstanceIndex = TerrainISM->AddInstance(LandTransform, true);
			if (InstanceIndex != INDEX_NONE && TerrainISM->NumCustomDataFloats >= 3)
			{
				const FLinearColor GroundColor = FLinearColor::LerpUsingHSV(LowlandColor, HighlandColor, FMath::Clamp(Cell.Elevation / 600.0f, 0.0f, 1.0f));
				TerrainISM->SetCustomDataValue(InstanceIndex, 0, GroundColor.R, false);
				TerrainISM->SetCustomDataValue(InstanceIndex, 1, GroundColor.G, false);
				TerrainISM->SetCustomDataValue(InstanceIndex, 2, GroundColor.B, false);
			}
	}

	// Nehir materyali için dinamik mavi su rengi garantisi
	if (UMaterialInstanceDynamic* ExistingDyn = Cast<UMaterialInstanceDynamic>(RiverISM->GetMaterial(0)))
	{
		ExistingDyn->SetVectorParameterValue(TEXT("BaseColor"), RiverWaterColor);
		ExistingDyn->SetVectorParameterValue(TEXT("Color"), RiverWaterColor);
	}
	else if (UMaterialInterface* BaseRiverMat = RiverISM->GetMaterial(0))
	{
		if (UMaterialInstanceDynamic* RiverDyn = UMaterialInstanceDynamic::Create(BaseRiverMat, RiverISM))
		{
			RiverDyn->SetVectorParameterValue(TEXT("BaseColor"), RiverWaterColor);
			RiverDyn->SetVectorParameterValue(TEXT("Color"), RiverWaterColor);
			RiverISM->SetMaterial(0, RiverDyn);
		}
	}

	TerrainISM->MarkRenderStateDirty();
	RiverISM->MarkRenderStateDirty();
	FTransform FirstLandInstance = FTransform::Identity;
	if (TerrainISM->GetInstanceCount() > 0)
	{
		TerrainISM->GetInstanceTransform(0, FirstLandInstance, true);
	}
	const FBoxSphereBounds TerrainBounds = TerrainISM->CalcBounds(TerrainISM->GetComponentTransform());
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_RUNTIME_TERRAIN grid=%dx%d land=%d river=%d landMesh=%s landMaterial=%s host=%s registered=%s visible=%s renderState=%s component=%s firstInstance=%s boundsOrigin=%s boundsExtent=%s"),
		Terrain->GridWidth,
		Terrain->GridHeight,
		TerrainISM->GetInstanceCount(),
		RiverISM->GetInstanceCount(),
		TerrainISM->GetStaticMesh() ? *TerrainISM->GetStaticMesh()->GetPathName() : TEXT("None"),
		TerrainISM->GetMaterial(0) ? *TerrainISM->GetMaterial(0)->GetPathName() : TEXT("None"),
		*GetPathName(),
		TerrainISM->IsRegistered() ? TEXT("true") : TEXT("false"),
		TerrainISM->IsVisible() ? TEXT("true") : TEXT("false"),
		TerrainISM->IsRenderStateCreated() ? TEXT("true") : TEXT("false"),
		*TerrainISM->GetComponentTransform().ToHumanReadableString(),
		*FirstLandInstance.ToHumanReadableString(),
		*TerrainBounds.Origin.ToCompactString(),
		*TerrainBounds.BoxExtent.ToCompactString());
}

bool UCrowdRenderSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World=Cast<UWorld>(Outer);
	// The live ISM/timer owner depends on CityLife/Transport, both Game/PIE-only.
	// Tooling/thumbnail/editor worlds have no resident authority or gameplay loop.
	return Super::ShouldCreateSubsystem(Outer)&&World
		&&(World->WorldType==EWorldType::Game||World->WorldType==EWorldType::PIE);
}

void UCrowdRenderSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Collection.InitializeDependency<UTerrainGridSubsystem>();
	Collection.InitializeDependency<UBuildingManagerSubsystem>();
	Collection.InitializeDependency<UDisasterSimulationSubsystem>();
	Collection.InitializeDependency<UEconomyAndStateSubsystem>();
	Collection.InitializeDependency<URoadNetworkSubsystem>();
	Collection.InitializeDependency<UCityLifeSubsystem>();
	Collection.InitializeDependency<UTransportSubsystem>();

	Agents.Reset();
	Agents.Reserve(MaxVisualCitizens);
	UpdatedTransforms.Reset();
	UpdatedTransforms.Reserve(MaxVisualCitizens);
	WalkableCellCoords.Reset();
	WalkableCellIndices.Reset();
	RoadCellCoords.Reset();
	RoadCellIndices.Reset();
	CityVergeCellCoords.Reset();
	HomeLocations.Reset();
	WorkLocations.Reset();
	LeisureLocations.Reset();
	ShelterLocations.Reset();
	bIsPanicModeActive = false;
	CrowdHostActor = nullptr;

	if (UWorld* World = GetWorld())
	{
		if (UTerrainGridSubsystem* Terrain=World->GetSubsystem<UTerrainGridSubsystem>())
			TerrainStorageDelegateHandle=Terrain->OnTerrainStorageChanged.AddUObject(this,&UCrowdRenderSubsystem::HandleTerrainStorageChanged);
		if (UDisasterSimulationSubsystem* DisasterSubsystem = World->GetSubsystem<UDisasterSimulationSubsystem>())
		{
			DisasterSubsystem->OnDisasterCompleted.AddDynamic(this, &UCrowdRenderSubsystem::HandleDisasterCompleted);
			DisasterSnapshotDelegateHandle = DisasterSubsystem->OnDisasterVisualSnapshotChanged.AddUObject(this, &UCrowdRenderSubsystem::HandleDisasterVisualSnapshot);
		}
		if (UEconomyAndStateSubsystem* EconomySubsystem = World->GetSubsystem<UEconomyAndStateSubsystem>())
		{
			EconomySubsystem->OnEconomyUpdated.AddDynamic(this, &UCrowdRenderSubsystem::HandleEconomyUpdated);
		}
		if (URoadNetworkSubsystem* Roads = World->GetSubsystem<URoadNetworkSubsystem>())
		{
			Roads->OnRoadNetworkChanged.AddDynamic(this, &UCrowdRenderSubsystem::HandleRoadNetworkChanged);
		}
		if(UCityLifeSubsystem* Life=World->GetSubsystem<UCityLifeSubsystem>())Life->OnCityLifeUpdated.AddUniqueDynamic(this,&UCrowdRenderSubsystem::HandleCityLifeUpdated);

		EnsureTerrainVisualBuilt();

		// Mevcut 0.08 sn timer korunuyor; hareket yalnızca sınırlı sayıdaki ISM agent'ını günceller.
		World->GetTimerManager().SetTimer(
			CrowdTimerHandle,
			FTimerDelegate::CreateUObject(this, &UCrowdRenderSubsystem::UpdateCrowdPositions),
			CrowdUpdateSeconds,
			true
		);
	}
}

void UCrowdRenderSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	EnsureTerrainVisualBuilt();
	BeginAftershockPresentationAudit(InWorld);
}

bool UCrowdRenderSubsystem::TryGetInstalledTerrainVisualBounds(FIntRect& OutBounds) const
{
	OutBounds=FIntRect(0,0,0,0);
	if(!IsValid(CrowdHostActor)||!CrowdHostActor->TerrainSurface||!CrowdHostActor->TerrainSurface->GetStaticMesh()
		||CrowdHostActor->BuiltVisibleCellBounds.IsEmpty())return false;
	OutBounds=CrowdHostActor->BuiltVisibleCellBounds;return true;
}

void UCrowdRenderSubsystem::RefreshTerrainPresentationBudget()
{
	if (IsValid(CrowdHostActor)) ApplyAftershockTerrainPresentationBudget(CrowdHostActor->TerrainSurface, GetWorld());
}

void UCrowdRenderSubsystem::EnsureTerrainVisualBuilt()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>();
	if (!Terrain)
	{
		return;
	}

	if (AAftershockCrowdHostActor* TerrainHost = GetOrCreateHostActor())
	{
		ApplyAftershockTerrainPresentationBudget(TerrainHost->TerrainSurface, World);
		bool InstalledChanged=false;
		const auto* HydraulicOwner=World->GetSubsystem<UDisasterSimulationSubsystem>();
		const bool HydraulicModel2=HydraulicOwner&&HydraulicOwner->IsHydraulicModel2Active();
		if (TerrainHost->BuiltTerrainRevision!=Terrain->GetTerrainRevision()
			||TerrainHost->bBuiltHydraulicModel2!=HydraulicModel2
			||TerrainHost->BuiltVisibleCellBounds!=GetAftershockVisibleCellBounds(*Terrain))
		{
			TerrainHost->BuildTerrainVisual(Terrain);
			InstalledChanged=true;
		}

		// This subsystem can build the grid before the gameplay world has finished
		// initialization. In that case the ISM has instances but no render state, and
		// the old "only build when empty" check leaves the entire city invisible.
		// Re-register once the world is ready so the instance buffer is created.
		const auto EnsureRenderState = [](UPrimitiveComponent* Component)
		{
			if (!Component || Component->IsRenderStateCreated())
			{
				return;
			}

			if (Component->IsRegistered())
			{
				Component->ReregisterComponent();
			}
			else if (Component->GetOwner() && Component->GetOwner()->HasActorBegunPlay())
			{
				Component->RegisterComponent();
			}
		};

		EnsureRenderState(TerrainHost->TerrainISM);
		EnsureRenderState(TerrainHost->RiverISM);
		EnsureRenderState(TerrainHost->CitizenISM);
		EnsureRenderState(TerrainHost->TerrainSurface);
		EnsureRenderState(TerrainHost->RiverSurface);
		// Only after ground installation, and only in an initialized gameplay
		// world: never recursively create subsystems during early world setup.
		FIntRect Installed;
		if(InstalledChanged&&World->HasBegunPlay()&&!bRefreshingTerrainDraws&&TryGetInstalledTerrainVisualBounds(Installed))
		{
			TGuardValue<bool> DrawGuard(bRefreshingTerrainDraws,true);
			if(auto* Buildings=World->GetSubsystem<UBuildingManagerSubsystem>())Buildings->RefreshVisibleBuildingPresentation();
			if(auto* City=World->GetSubsystem<UCityPresentationSubsystem>())City->RefreshVisiblePresentation();
			RebuildCitizenInstanceTransforms();UpdateEmotionBillboards();
			if(auto* Traffic=World->GetSubsystem<UTransportSubsystem>())Traffic->RefreshVisiblePresentation();
		}
	}
}

void UCrowdRenderSubsystem::Deinitialize()
{
    IncrementalNormalRoutes.Reset();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CrowdTimerHandle);
		World->GetTimerManager().ClearTimer(DisasterVisualCompletionHandle);
		if (UTerrainGridSubsystem* Terrain=World->GetSubsystem<UTerrainGridSubsystem>()) Terrain->OnTerrainStorageChanged.Remove(TerrainStorageDelegateHandle);

		if (UDisasterSimulationSubsystem* DisasterSubsystem = World->GetSubsystem<UDisasterSimulationSubsystem>())
		{
			DisasterSubsystem->OnDisasterCompleted.RemoveDynamic(this, &UCrowdRenderSubsystem::HandleDisasterCompleted);
			DisasterSubsystem->OnDisasterVisualSnapshotChanged.Remove(DisasterSnapshotDelegateHandle);
		}
		if (UEconomyAndStateSubsystem* EconomySubsystem = World->GetSubsystem<UEconomyAndStateSubsystem>())
		{
			EconomySubsystem->OnEconomyUpdated.RemoveDynamic(this, &UCrowdRenderSubsystem::HandleEconomyUpdated);
		}
		if (URoadNetworkSubsystem* Roads = World->GetSubsystem<URoadNetworkSubsystem>())
		{
			Roads->OnRoadNetworkChanged.RemoveDynamic(this, &UCrowdRenderSubsystem::HandleRoadNetworkChanged);
		}
		if(UCityLifeSubsystem* Life=World->GetSubsystem<UCityLifeSubsystem>())Life->OnCityLifeUpdated.RemoveDynamic(this,&UCrowdRenderSubsystem::HandleCityLifeUpdated);
	}

	if (IsValid(CrowdHostActor))
	{
		CrowdHostActor->Destroy();
		CrowdHostActor = nullptr;
	}

	Agents.Empty();
    LastSynchronizedLivingPopulation=LastSynchronizedTotalDeaths=INDEX_NONE;bCityLifeSyncPending=false;
	UpdatedTransforms.Empty();
	WalkableCellCoords.Empty();
	WalkableCellIndices.Empty();
	RoadCellCoords.Empty();
	RoadCellIndices.Empty();
	CityVergeCellCoords.Empty();
	HomeLocations.Empty();
	WorkLocations.Empty();
	LeisureLocations.Empty();
	ShelterLocations.Empty();
	HomeRecords.Empty();
	WorkRecords.Empty();
	LeisureRecords.Empty();
	EmergencyCentres.Empty();
	AllBuildingFrontages.Empty();
	AssemblyCoords.Empty();
	RoutingCellCosts.Empty();
	NormalRouteTrees.Empty();
	NormalRouteTreeUseSerial.Empty();
	EmotionPooledTransforms.Empty();
	SpacingRemainingCorrection.Empty();
	SpacingBucketCounts.Empty();
	SpacingWalkabilityCache.Empty();
	SpacingWalkingEdgeCache.Empty();
	Super::Deinitialize();
}

AAftershockCrowdHostActor* UCrowdRenderSubsystem::GetOrCreateHostActor()
{
	if (IsValid(CrowdHostActor))
	{
		return CrowdHostActor;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = TEXT("Aftershock_CrowdHostActor");
	SpawnParams.ObjectFlags |= RF_Transient;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	CrowdHostActor = World->SpawnActor<AAftershockCrowdHostActor>(
		AAftershockCrowdHostActor::StaticClass(),
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		SpawnParams
	);

	return CrowdHostActor;
}

bool UCrowdRenderSubsystem::RefreshRoutingSafety(bool bWalkingListJustRebuilt)
{
    FAftershockRoadCpuScope Cpu(TEXT("Crowd.RefreshRoutingSafety"));
	const UWorld* World = GetWorld();
	const UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	const UDisasterPresentationSubsystem* Weather = World ? World->GetSubsystem<UDisasterPresentationSubsystem>() : nullptr;
	if (!Terrain) return false;
	TArray<int32> NewCosts;
	NewCosts.Init(MAX_int32, Terrain->Cells.Num());
	for (const FIntPoint Coord : WalkableCellCoords)
	{
        // This same GT stack just evaluated the SAME public predicate for every
        // WalkableCellCoords entry. No world edits or callbacks occurred between
        // the two passes. Independent safety polls retain the full live check.
        if (!bWalkingListJustRebuilt && !IsPublicWalkingCell(*Terrain, Coord)) continue;
		const URoadNetworkSubsystem* Roads=World?World->GetSubsystem<URoadNetworkSubsystem>():nullptr;
		const float Depth = Roads && Terrain->GetCell(Coord).bHasRoad ? Roads->GetRoadFloodDepthCm(Coord) : (Weather ? Weather->GetDisplayedFloodDepthCm(Coord) : 0.f);
		if (Depth > VisibleWaterRouteCutoffCm) continue;
		const int32 Index = Terrain->CoordToIndex(Coord);
		if (!NewCosts.IsValidIndex(Index)) continue;
		NewCosts[Index] = (Terrain->GetCell(Coord).bHasRoad ? 10 : 30)
			+ FMath::FloorToInt(FMath::Max(0.f, Depth) / 10.f) * 8;
		if(bIsPanicModeActive && EmergencyDisasterType==EDisasterType::Tsunami)
		{
			// Warning-time routing prefers inland streets; do not mark every
			// potentially affected parcel impassable before residents can leave.
			if(Terrain->GetCell(Coord).bIsCoastal) NewCosts[Index] += 25;
			const UDisasterSimulationSubsystem* D=World->GetSubsystem<UDisasterSimulationSubsystem>();
			if(D && D->GetLatestVisualSnapshot().Type==EDisasterType::Tsunami
				&& D->GetLatestVisualSnapshot().LocalSeverity.FindRef(Coord)>0.f)
				NewCosts[Index] += 35;
		}
	}
	uint32 Topology = HashCombine(GetTypeHash(Terrain->GridOrigin), GetTypeHash(Terrain->CellSize));
	for (const FGridCell& Cell : Terrain->Cells)
		Topology = HashCombine(Topology, HashCombine(GetTypeHash(Cell.Coords), GetTypeHash(Cell.Elevation)));
	const bool bChanged = Topology != RoutingTopologySignature || NewCosts != RoutingCellCosts;
	if (bChanged)
	{
		RoutingTopologySignature = Topology;
		RoutingCellCosts = MoveTemp(NewCosts);
		ResetNormalRouteCache();
	}
	return bChanged;
}

bool UCrowdRenderSubsystem::UsesIncrementalNormalRouting() const
{return !IsRunningCommandlet()&&!bIsPanicModeActive&&GetWorld()&&GetWorld()->HasBegunPlay();}
bool UCrowdRenderSubsystem::HasPendingNormalRoute(const FCitizenAgent& Agent) const
{return IncrementalNormalRoutes.IsValid()&&IncrementalNormalRoutes->HasRequest(NormalRouteAgentKey(Agent));}
int32 UCrowdRenderSubsystem::GetNormalRouteCacheEntryCount() const
{return NormalRouteTrees.Num()+(IncrementalNormalRoutes.IsValid()?IncrementalNormalRoutes->GetJobCount():0);}
uint64 UCrowdRenderSubsystem::GetNormalRouteCacheAllocatedBytes() const
{
	uint64 Bytes = NormalRouteTrees.GetAllocatedSize() + NormalRouteTreeUseSerial.GetAllocatedSize();
	for (const auto& Pair : NormalRouteTrees)
		Bytes += Pair.Value.Cost.GetAllocatedSize() + Pair.Value.Next.GetAllocatedSize();
    if(IncrementalNormalRoutes.IsValid())Bytes+=IncrementalNormalRoutes->GetAllocatedBytes();
	return Bytes;
}

void UCrowdRenderSubsystem::ResetNormalRouteCache()
{
    // Keep the same pending stable-ID intent through a graph invalidation;
    // only partial distance fields are invalidated. Panic/restore reset all.
    if(IncrementalNormalRoutes.IsValid())IncrementalNormalRoutes->InvalidateFields();
	NormalRouteTrees.Reset();
	NormalRouteTreeUseSerial.Reset();
}

void UCrowdRenderSubsystem::TrimNormalRouteCache(uint64 IncomingBytes)
{
	// Reserve map/bucket overhead too. Every field is indexed by a canonical cell,
	// so expanded cities must not retain one full-grid field per resident target.
	constexpr uint64 MapReserve = 128ull * 1024ull;
	while (!NormalRouteTrees.IsEmpty() && (NormalRouteTrees.Num() >= MaximumNormalRouteTrees
		|| GetNormalRouteCacheAllocatedBytes() + IncomingBytes + MapReserve > MaximumNormalRouteCacheBytes))
	{
		int32 OldestKey = INDEX_NONE;
		uint64 OldestSerial = MAX_uint64;
		for (const auto& Pair : NormalRouteTreeUseSerial)
			if (Pair.Value < OldestSerial) { OldestKey = Pair.Key; OldestSerial = Pair.Value; }
		if (OldestKey == INDEX_NONE) { ResetNormalRouteCache(); break; }
		NormalRouteTrees.Remove(OldestKey);
		NormalRouteTreeUseSerial.Remove(OldestKey);
	}
}

void UCrowdRenderSubsystem::RebuildDestinationCaches()
{
    FAftershockRoadCpuScope Cpu(TEXT("Crowd.RebuildDestinationCaches"));
	WalkableCellCoords.Reset(); WalkableCellIndices.Reset();
	RoadCellCoords.Reset(); RoadCellIndices.Reset(); CityVergeCellCoords.Reset();
	HomeLocations.Reset(); WorkLocations.Reset(); LeisureLocations.Reset(); ShelterLocations.Reset();
	HomeRecords.Reset(); WorkRecords.Reset(); LeisureRecords.Reset(); EmergencyCentres.Reset(); AllBuildingFrontages.Reset(); AssemblyCoords.Reset();
	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain) return;
	for (const FGridCell& Cell : Terrain->Cells)
	{
			const FIntPoint Coord = Cell.Coords;
			if (!IsPublicWalkingCell(*Terrain, Coord)) continue;
			WalkableCellCoords.Add(Coord);
			WalkableCellIndices.Add(Terrain->CoordToIndex(Coord));
			if (Terrain->GetCell(Coord).bHasRoad)
			{
				RoadCellCoords.Add(Coord);
				RoadCellIndices.Add(Terrain->CoordToIndex(Coord));
			}
	}
	RefreshRoutingSafety(true);
	if (const UAftershockScenarioSubsystem* Scenario = World->GetSubsystem<UAftershockScenarioSubsystem>())
	{
		for (const FIntPoint Coord : Scenario->GetAssemblyCells())
			if (IsPublicWalkingCell(*Terrain, Coord)) AssemblyCoords.AddUnique(Coord);
	}
	UBuildingManagerSubsystem* BuildingMgr = World->GetSubsystem<UBuildingManagerSubsystem>();
	if (!BuildingMgr) return;
	TArray<FBuildingInstance> Buildings;
	BuildingMgr->GetAllBuildings(Buildings);
	Buildings.Sort([](const FBuildingInstance& A, const FBuildingInstance& B) { return A.InstanceID < B.InstanceID; });
	for (const FBuildingInstance& Building : Buildings)
	{
		// Damaged structures are closed and cannot be homes, workplaces or aid centres.
		if (!Building.BuildingData || !Terrain->IsPlayableCoord(Building.GridCoords)
			|| Terrain->GetCell(Building.GridCoords).bBurning) continue;
		FAftershockCitizenDestination Record;
		Record.BuildingID = Building.InstanceID;
		Record.BuildingCoord = Building.GridCoords;
		Record.Type = Building.BuildingData->Type;
		// A nearby road is a frontage only if reachable from this building's
		// immediate verge. Manhattan distance alone could jump across a river
		// or through another occupied parcel.
		TArray<FIntPoint> Search; Search.Add(Building.GridCoords);
		TArray<int32> Depths; Depths.Add(0);
		TSet<int32> Seen; Seen.Add(Terrain->CoordToIndex(Building.GridCoords));
		TArray<FIntPoint> ImmediateVerges;
		int32 RoadDepth = MAX_int32;
		for (int32 Read = 0; Read < Search.Num(); ++Read)
		{
			if (Depths[Read] >= 3 || Depths[Read] >= RoadDepth) continue;
			for (const FIntPoint Offset : CitizenNeighborOffsets)
			{
				const FIntPoint Coord = Search[Read] + Offset;
				if (!IsPublicWalkingCell(*Terrain, Coord)) continue;
				const int32 Index = Terrain->CoordToIndex(Coord);
				if (Seen.Contains(Index)) continue;
				Seen.Add(Index);
				const int32 Depth = Depths[Read] + 1;
				if (Depth == 1) ImmediateVerges.Add(Coord);
				if (Terrain->GetCell(Coord).bHasRoad)
				{
					if (Depth < RoadDepth) { Record.Frontages.Reset(); RoadDepth = Depth; }
					if (Depth == RoadDepth) Record.Frontages.Add(Coord);
				}
				Search.Add(Coord); Depths.Add(Depth);
			}
		}
		if (Record.Frontages.IsEmpty()) Record.Frontages = MoveTemp(ImmediateVerges);
		for (const FIntPoint Coord : Record.Frontages) CityVergeCellCoords.AddUnique(Coord);
		if (Record.Frontages.IsEmpty()) continue;
		AllBuildingFrontages.Add(Record);
		if(Building.DamageState>=EDamageState::Damaged)continue;
		TArray<FVector> Frontages;
		for (const FIntPoint Coord : Record.Frontages) Frontages.Add(Terrain->GridToWorld(Coord, true));
		const int32 Residents = FMath::Max(0, Building.BuildingData->Residents);
		const int32 Workers = FMath::Max(0, Building.BuildingData->Workers);
		if (Residents > 0 && (Record.Type == EBuildingType::House || Record.Type == EBuildingType::Apartment))
		{
			Record.Weight = Residents; HomeRecords.Add(Record); HomeLocations.Append(Frontages);
		}
		if (Workers > 0) { Record.Weight = Workers; WorkRecords.Add(Record); WorkLocations.Append(Frontages); }
		if (Record.Type == EBuildingType::GreenSpace)
		{
			Record.Weight = 1; LeisureRecords.Add(Record); LeisureLocations.Append(Frontages);
			for (const FIntPoint Coord : Record.Frontages) AssemblyCoords.AddUnique(Coord);
		}
		if (IsShelterVisualRecord(Building.BuildingData) || Record.Type == EBuildingType::School
			|| Record.Type == EBuildingType::Hospital || Record.Type == EBuildingType::FireStation)
		{
			// No bed count exists in shared data. This quota spaces representative
			// visuals only; it never grants real admission, rescue, population or aid.
			Record.Weight = static_cast<int32>(FMath::Min<int64>((static_cast<int64>(Residents) + Workers) / 2, MaxVisualCitizens));
			EmergencyCentres.Add(Record); ShelterLocations.Append(Frontages);
		}
	}
}

void UCrowdRenderSubsystem::AssignScheduleDestinations(FCitizenAgent& Agent, int32 AgentIndex)
{
	const UTerrainGridSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain) return;
	const auto Assign = [this, Terrain, &Agent, AgentIndex](const TArray<FAftershockCitizenDestination>& Records,
		int32& ID, FVector& Location, const FVector& Fallback)
	{
		const FAftershockCitizenDestination* Selected = Records.FindByPredicate([ID](const FAftershockCitizenDestination& R) { return R.BuildingID == ID; });
		if (!Selected && !Records.IsEmpty())
		{
			int64 TotalWeight = 0;
			for (const FAftershockCitizenDestination& R : Records) TotalWeight += FMath::Max(1, R.Weight);
			int64 Slot = (static_cast<int64>(AgentIndex) * 2) % TotalWeight;
			for (const FAftershockCitizenDestination& R : Records)
			{
				Slot -= FMath::Max(1, R.Weight);
				if (Slot < 0) { Selected = &R; break; }
			}
		}
		ID = Selected ? Selected->BuildingID : INDEX_NONE;
		if (Selected)
		{
			const int32 FrontageIndex = static_cast<uint32>(Agent.RandomSeed) % Selected->Frontages.Num();
			Location = MakeVisibleCellLocation(*Terrain, Selected->Frontages[FrontageIndex], Agent);
		}
		else Location = Fallback;
	};
	const FVector Fallback = PickValidGroundLocation(Agent);
	Assign(HomeRecords, Agent.HomeBuildingID, Agent.HomeLocation, Fallback);
	Assign(WorkRecords, Agent.WorkBuildingID, Agent.WorkLocation, Agent.HomeLocation);
	Assign(LeisureRecords, Agent.LeisureBuildingID, Agent.LeisureLocation, Agent.HomeLocation);
	if (Agent.HomeBuildingID == INDEX_NONE && Agent.WorkBuildingID != INDEX_NONE) Agent.HomeLocation = Agent.WorkLocation;
}

FVector UCrowdRenderSubsystem::PickValidGroundLocation(FCitizenAgent& Agent) const
{
	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain)
	{
		return FVector::ZeroVector;
	}

	const TArray<FIntPoint>& PreferredCells = !RoadCellCoords.IsEmpty() ? RoadCellCoords
		: (!CityVergeCellCoords.IsEmpty() ? CityVergeCellCoords : WalkableCellCoords);
	if (PreferredCells.Num() > 0)
	{
		const int32 StableIndex = static_cast<uint32>(Agent.RandomSeed) % PreferredCells.Num();
		const FIntPoint Coord = PreferredCells[StableIndex];
		return MakeVisibleCellLocation(*Terrain, Coord, Agent);
	}

	const FIntPoint CurrentCoord = Terrain->WorldToGrid(Agent.CurrentLocation);
	if (IsPublicWalkingCell(*Terrain, CurrentCoord))
	{
		return MakeVisibleCellLocation(*Terrain, CurrentCoord, Agent);
	}

	if (HomeLocations.Num() > 0)
	{
		return MakeVisibleCellLocation(*Terrain, Terrain->WorldToGrid(HomeLocations[static_cast<uint32>(Agent.RandomSeed) % HomeLocations.Num()]), Agent);
	}

	return FVector::ZeroVector;
}

FVector UCrowdRenderSubsystem::MakeVisibleCellLocation(const UTerrainGridSubsystem& Terrain, FIntPoint Coord, const FCitizenAgent& Agent) const
{
	FVector Location = Terrain.GridToWorld(Coord, true);
	FRandomStream Spread(static_cast<int32>(static_cast<uint32>(Agent.RandomSeed) ^ 0x6d2b79f5u));
	const float Side = Spread.FRand() < 0.5f ? -1.0f : 1.0f;
	if (Terrain.GetCell(Coord).bHasRoad)
	{
		const auto HasRoad = [&Terrain](FIntPoint Other) { return IsPublicWalkingCell(Terrain, Other) && Terrain.GetCell(Other).bHasRoad; };
		const bool bAlongX = HasRoad(Coord + FIntPoint(1, 0)) || HasRoad(Coord + FIntPoint(-1, 0));
		const bool bAlongY = HasRoad(Coord + FIntPoint(0, 1)) || HasRoad(Coord + FIntPoint(0, -1));
		const float Along = Spread.FRandRange(-0.28f, 0.28f) * Terrain.CellSize;
		// Two finite footway bands on a ten metre street; retain legacy five metre geometry.
		const float Verge = Side * (Terrain.CellSize * (Terrain.CellSize>=900.f?.40f:.42f)
			+ (Terrain.CellSize>=900.f?Spread.FRandRange(-40.f,40.f):0.f));
		Location += bAlongX && !bAlongY ? FVector(Along, Verge, 0.f) : FVector(Verge, Along, 0.f);
		if (const URoadNetworkSubsystem* Roads=Terrain.GetWorld()?Terrain.GetWorld()->GetSubsystem<URoadNetworkSubsystem>():nullptr)
			Location.Z=Roads->SampleRoadSurfaceElevation(Location);
		Location.Z += CitizenZOffset;
	}
	else
	{
		Location.X += Spread.FRandRange(-0.27f, 0.27f) * Terrain.CellSize;
		Location.Y += Spread.FRandRange(-0.27f, 0.27f) * Terrain.CellSize;
		Location.Z = VisibleGroundHeight(Terrain, Location) + CitizenZOffset;
	}
	return Location;
}

void UCrowdRenderSubsystem::SetCitizenInstanceColor(const FCitizenAgent& Agent, bool bMarkRenderStateDirty)
{
	AAftershockCrowdHostActor* Host = IsValid(CrowdHostActor) ? CrowdHostActor : nullptr;
	if (!Host || !Host->CitizenISM || Agent.ISMIndex == INDEX_NONE)
	{
		return;
	}

	const FLinearColor Color = GetCitizenWardrobeColor(Agent, SimulationSeed);
	Host->CitizenISM->SetCustomDataValue(Agent.ISMIndex, 0, Color.R, false);
	Host->CitizenISM->SetCustomDataValue(Agent.ISMIndex, 1, Color.G, false);
	Host->CitizenISM->SetCustomDataValue(Agent.ISMIndex, 2, Color.B, false);
	if (bMarkRenderStateDirty)
	{
		Host->CitizenISM->MarkRenderStateDirty();
	}
}

void UCrowdRenderSubsystem::SetAgentActivity(FCitizenAgent& Agent, EAftershockCitizenActivity NewActivity)
{
	Agent.Activity = NewActivity;
	Agent.bIsPanicking = NewActivity == EAftershockCitizenActivity::Evacuating;

	switch (NewActivity)
	{
	case EAftershockCitizenActivity::WalkingToTransport:
	case EAftershockCitizenActivity::Commuting:
		Agent.MoveSpeed = 145.f + CitizenUnit(static_cast<uint32>(Agent.RandomSeed)^0x197c31u)*55.f;
		break;
	case EAftershockCitizenActivity::Evacuating:
		Agent.MoveSpeed = 420.f + CitizenUnit(static_cast<uint32>(Agent.RandomSeed)^0x8fa37u)*80.f;
		break;
	case EAftershockCitizenActivity::Rescued:
		Agent.MoveSpeed = 145.f + CitizenUnit(static_cast<uint32>(Agent.RandomSeed)^0x197c31u)*55.f;
		break;
	default:
		Agent.MoveSpeed = 0.0f;
		break;
	}

	SetCitizenInstanceColor(Agent);
}

int32 UCrowdRenderSubsystem::DestinationBuildingFor(const FCitizenAgent& Agent,EAftershockCitizenActivity Arrival) const
{
	switch(Arrival)
	{
	case EAftershockCitizenActivity::AtHome:return Agent.HomeBuildingID;
	case EAftershockCitizenActivity::AtWork:return Agent.WorkBuildingID;
	case EAftershockCitizenActivity::AtLeisure:return Agent.LeisureBuildingID;
	case EAftershockCitizenActivity::AtFoodService:return Agent.FoodBuildingID;
	case EAftershockCitizenActivity::AtHealthService:return Agent.HealthBuildingID;
	default:return INDEX_NONE;
	}
}

void UCrowdRenderSubsystem::StartCommute(FCitizenAgent& Agent,const FVector& Destination,EAftershockCitizenActivity ArrivalActivity,bool bAllowTransport)
{
	const bool bDepartingIndoors=IsCitizenIndoors(Agent,GetWorld());
	if(bDepartingIndoors)CaptureIndoorDepartureAnchor(Agent,GetWorld());
	Agent.FinalDestinationLocation=Destination;
	Agent.ActivityAfterArrival=ArrivalActivity;
	Agent.ScheduleSecondsRemaining=0.f;
	if(bAllowTransport&&!bIsPanicModeActive&&Agent.ResidentID!=INDEX_NONE&&Agent.TransportTripID==INDEX_NONE)
	{
		if(UTransportSubsystem* Transport=GetWorld()?GetWorld()->GetSubsystem<UTransportSubsystem>():nullptr)
		{
			FAftershockPassengerTrip Trip;
			if(Transport->PlanTrip(Agent.ResidentID,Agent.CurrentLocation,Destination,DestinationBuildingFor(Agent,ArrivalActivity),Agent.bPrefersPrivateCar,Trip))
			{
				Agent.TransportTripID=Trip.TripID;Agent.TransportFinalDestination=Destination;Agent.TransportArrivalActivity=ArrivalActivity;
				BuildSafeRoute(Agent,Trip.PickupLocation);
				if(Agent.bRouteReachesDestination||HasPendingNormalRoute(Agent)){SetAgentActivity(Agent,EAftershockCitizenActivity::WalkingToTransport);Agent.bWaitingForOutdoorSpace=bDepartingIndoors;return;}
				Transport->CancelTrip(Trip.TripID);Agent.TransportTripID=INDEX_NONE;
			}
		}
	}
	BuildSafeRoute(Agent,Destination);
	SetAgentActivity(Agent,EAftershockCitizenActivity::Commuting);
	Agent.bWaitingForOutdoorSpace=bDepartingIndoors;
}

void UCrowdRenderSubsystem::CancelAgentTransport(FCitizenAgent& Agent)
{
    if(IncrementalNormalRoutes.IsValid())IncrementalNormalRoutes->Cancel(NormalRouteAgentKey(Agent));
	if(Agent.TransportTripID!=INDEX_NONE)
		if(UTransportSubsystem* Transport=GetWorld()?GetWorld()->GetSubsystem<UTransportSubsystem>():nullptr)
		{
			FAftershockPassengerTrip Trip;
			const bool WasRiding=Transport->GetTrip(Agent.TransportTripID,Trip)&&(Agent.bInsideVehicle||Trip.State==EAftershockPassengerState::Riding);
			Transport->CancelTrip(Agent.TransportTripID);
			if(WasRiding)
			{
				FVector Release;
				if(!Transport->FindPassengerReleaseLocation(Agent.TransportTripID,Release))
				{Agent.CurrentLocation=Trip.CurrentLocation;Agent.bInsideVehicle=true;Agent.bWaitingForOutdoorSpace=false;Agent.OutdoorDepartureBuildingID=INDEX_NONE;Agent.OutdoorDepartureLocation=FVector::ZeroVector;SetAgentActivity(Agent,EAftershockCitizenActivity::RidingTransport);return;}
				Agent.CurrentLocation=Release;
			}
		}
	Agent.TransportTripID=INDEX_NONE;Agent.bInsideVehicle=false;
}

bool UCrowdRenderSubsystem::UpdatePassengerState(FCitizenAgent& Agent)
{
	if(Agent.TransportTripID==INDEX_NONE)return false;
	UTransportSubsystem* Transport=GetWorld()?GetWorld()->GetSubsystem<UTransportSubsystem>():nullptr;
	FAftershockPassengerTrip Trip;
	if(!Transport||!Transport->GetTrip(Agent.TransportTripID,Trip))
    {
        if(IncrementalNormalRoutes.IsValid())IncrementalNormalRoutes->Cancel(NormalRouteAgentKey(Agent));
        Agent.TransportTripID=INDEX_NONE;Agent.bInsideVehicle=false;
        if(Agent.Activity==EAftershockCitizenActivity::WalkingToTransport)StartCommute(Agent,Agent.TransportFinalDestination,Agent.TransportArrivalActivity,false);
        return false;
    }
	if(Trip.State==EAftershockPassengerState::Riding)
	{
		Agent.CurrentLocation=Trip.CurrentLocation;Agent.TargetLocation=Agent.CurrentLocation;Agent.FinalDestinationLocation=Agent.CurrentLocation;
		Agent.bInsideVehicle=true;Agent.bWaitingForOutdoorSpace=false;Agent.OutdoorDepartureBuildingID=INDEX_NONE;Agent.OutdoorDepartureLocation=FVector::ZeroVector;Agent.RouteGridCoords.Reset();SetAgentActivity(Agent,EAftershockCitizenActivity::RidingTransport);return true;
	}
	if(Trip.State==EAftershockPassengerState::Waiting)
	{
		Agent.TargetLocation=Agent.CurrentLocation;Agent.FinalDestinationLocation=Agent.CurrentLocation;Agent.RouteGridCoords.Reset();
		SetAgentActivity(Agent,EAftershockCitizenActivity::WaitingForTransport);return true;
	}
	if(Trip.State==EAftershockPassengerState::Arrived||Trip.State==EAftershockPassengerState::Cancelled||Trip.State==EAftershockPassengerState::Stranded)
	{
		if(Agent.bInsideVehicle||Trip.State==EAftershockPassengerState::Arrived)
		{
			FVector Release=Trip.CurrentLocation;
			if(!CanAdmitCitizenAt(Release,Agent.ResidentID)&&!Transport->FindPassengerReleaseLocation(Trip.TripID,Release))
			{Transport->RetainPendingPassengerHandoff(Trip.TripID);Agent.bInsideVehicle=true;SetAgentActivity(Agent,EAftershockCitizenActivity::RidingTransport);return true;}
			Agent.CurrentLocation=Release;
		}
		Agent.bInsideVehicle=false;Agent.TransportTripID=INDEX_NONE;
		StartCommute(Agent,Agent.TransportFinalDestination,Agent.TransportArrivalActivity,false);
		if(bIsPanicModeActive)SetAgentActivity(Agent,EAftershockCitizenActivity::Evacuating);
		return false;
	}
	return false;
}

FAftershockScopedCrowdRefresh::FAftershockScopedCrowdRefresh(UCrowdRenderSubsystem* InOwner):Owner(InOwner)
{if(Owner)Owner->BeginScopedCrowdRefresh();}
FAftershockScopedCrowdRefresh::~FAftershockScopedCrowdRefresh()
{if(Owner)Owner->EndScopedCrowdRefresh();}

void UCrowdRenderSubsystem::BeginScopedCrowdRefresh()
{
	check(IsInGameThread());++ScopedCrowdRefreshDepth;
}
bool UCrowdRenderSubsystem::FlushScopedRefreshForSnapshot()
{
	check(IsInGameThread());
	if(!GetWorld()||GetWorld()->bIsTearingDown)return false;
	const auto* Life=GetWorld()->GetSubsystem<UCityLifeSubsystem>();
	if(Life&&Life->IsCityInitialized()&&(LastSynchronizedLivingPopulation!=Life->GetState().LivingPopulation
		||LastSynchronizedTotalDeaths!=Life->GetState().TotalDeaths))bScopedRosterRefreshPending=true;
	if(!bScopedRosterRefreshPending&&!bScopedEmergencyPlanPending&&!bCityLifeSyncPending)return true;
	// A save cannot read half a movement callback or recursively enter an active
	// roster flush. Reject explicitly; never emit a mixed canonical/crowd DTO.
	if(bUpdatingAgents||bDeferEmergencyPlanDuringRosterFlush)return false;
	{
		// End performs the actual flush at depth zero. Restore the full prior
		// nesting, so notices after this checkpoint continue to coalesce normally.
		TGuardValue<int32> DepthGuard(ScopedCrowdRefreshDepth,1);
		EndScopedCrowdRefresh();
	}
	return !bScopedRosterRefreshPending&&!bCityLifeSyncPending&&!bScopedEmergencyPlanPending
		&&(!Life||!Life->IsCityInitialized()||(LastSynchronizedLivingPopulation==Life->GetState().LivingPopulation
			&&LastSynchronizedTotalDeaths==Life->GetState().TotalDeaths));
}
void UCrowdRenderSubsystem::EndScopedCrowdRefresh()
{
	check(IsInGameThread());check(ScopedCrowdRefreshDepth>0);
	if(--ScopedCrowdRefreshDepth>0)return;
	if(!GetWorld()||GetWorld()->bIsTearingDown)
	{
		bScopedRosterRefreshPending=false;bScopedEmergencyPlanPending=false;bScopedPlanningAuditPending=false;
		return;
	}
	const bool Sync=bScopedRosterRefreshPending||bCityLifeSyncPending;
	const bool Plan=bScopedEmergencyPlanPending;
	bScopedRosterRefreshPending=false;bScopedEmergencyPlanPending=false;
	if(bUpdatingAgents)
	{
		// Preserve the existing safe end-of-movement barrier when a caller uses
		// this scope from an update callback. Never impersonate that update flag.
		bCityLifeSyncPending|=Sync||Plan;bScopedEmergencyPlanPending=Plan;return;
	}
	bCityLifeSyncPending=false;
	if(Sync)
	{
		TGuardValue<bool> Guard(bDeferEmergencyPlanDuringRosterFlush,true);
		SynchronizeCityLifeRoster();
	}
	if(Plan||bScopedEmergencyPlanPending)
	{
		bScopedEmergencyPlanPending=false;
		if(!Sync)RebuildDestinationCaches();
		ReplanEmergencyEvacuation();
	}
	if(bScopedPlanningAuditPending){bScopedPlanningAuditPending=false;AuditCitizenPlanning();}
}

void UCrowdRenderSubsystem::HandleCityLifeUpdated()
{
	if(ScopedCrowdRefreshDepth>0){bScopedRosterRefreshPending=true;return;}
    // Actual roster membership changes MUST be visible before pause/game-over:
    // compare full canonical counters, never the capped1500 draw-agent count.
    const auto* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr;
    const bool SameMembership=Life&&Life->IsCityInitialized()
        &&Life->GetState().LivingPopulation==LastSynchronizedLivingPopulation
        &&Life->GetState().TotalDeaths==LastSynchronizedTotalDeaths;
    // Only repeated settled-world service/job/economy notices are coalesced.
    // Initial membership, births/deaths and panic remain synchronous; a notice
    // raised DURING the update uses the existing safe end-of-callback barrier.
    if(bUpdatingAgents||(!bIsPanicModeActive&&SameMembership&&Agents.Num()>0&&GetWorld()&&GetWorld()->HasBegunPlay()))
    {bCityLifeSyncPending=true;return;}
    SynchronizeCityLifeRoster();
}

void UCrowdRenderSubsystem::SynchronizeCityLifeRoster()
{
    FAftershockRoadCpuScope Cpu(TEXT("Crowd.SynchronizeCityLifeRoster"));
    FAftershockRoadGoalCpuFrame GoalCpu(TEXT("Crowd.SynchronizeCityLifeRoster"));
	if(bSyntheticRenderProfileActive)return;
	UWorld* World=GetWorld();UCityLifeSubsystem* Life=World?World->GetSubsystem<UCityLifeSubsystem>():nullptr;
	if(!Life||!Life->IsCityInitialized())return;
	if(ScopedCrowdRefreshDepth>0){bScopedRosterRefreshPending=true;return;}
	if(bUpdatingAgents){bCityLifeSyncPending=true;return;}
	AAftershockCrowdHostActor* Host=GetOrCreateHostActor();const UTerrainGridSubsystem* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();
	if(!Host||!Host->CitizenISM||!Terrain)return;
	RebuildDestinationCaches();
	++RosterRefreshWork.Synchronizations;
	FAftershockCityServiceLookup ServiceLookup;Life->BuildServiceDestinationLookup(ServiceLookup);
	++RosterRefreshWork.ServiceIndexBuilds;RosterRefreshWork.ServiceBuildingsVisited+=ServiceLookup.BuildingsVisited;
	TMap<int32,const FAftershockCitizenDestination*> FrontageByBuilding;FrontageByBuilding.Reserve(AllBuildingFrontages.Num());
	for(const auto& Frontage:AllBuildingFrontages)if(!FrontageByBuilding.Contains(Frontage.BuildingID))FrontageByBuilding.Add(Frontage.BuildingID,&Frontage);
	RosterRefreshWork.FrontagesIndexed+=AllBuildingFrontages.Num();
	TMap<int32,FCitizenAgent> Previous;
	for(const FCitizenAgent& Agent:Agents)if(Agent.ResidentID!=INDEX_NONE)Previous.Add(Agent.ResidentID,Agent);
	TArray<FCityResidentRecord> Living;
	for(const FCityResidentRecord& Resident:Life->GetResidents())if(Resident.bAlive)Living.Add(Resident);
	Living.Sort([](const FCityResidentRecord&A,const FCityResidentRecord&B){return A.ResidentID<B.ResidentID;});
	if(Living.Num()>MaxVisualCitizens)
	{
		TArray<int32> MunicipalStaff;TSet<int32> PriorityMunicipalStaff;
		if(const auto* Transport=World->GetSubsystem<UTransportSubsystem>())Transport->GetMunicipalStaffResidentIDs(MunicipalStaff);
		// At most 1,024 existing facility drivers are reserved inside the same
		// 1,500-person budget; no staff, jobs or residents are created here.
		for(int32 ResidentID:MunicipalStaff)PriorityMunicipalStaff.Add(ResidentID);
		// Stratify by actual home before taking the display budget. An expanded
		// district must not disappear because its residents have newer IDs.
		TMap<int32,int32> HomeOrdinal;TMap<int32,int32> Rank;
		for(const auto& Resident:Living)Rank.Add(Resident.ResidentID,HomeOrdinal.FindOrAdd(Resident.HomeBuildingID)++);
		Living.Sort([&](const auto& A,const auto& B)
		{
			const bool AService=PriorityMunicipalStaff.Contains(A.ResidentID),BService=PriorityMunicipalStaff.Contains(B.ResidentID);
			if(AService!=BService)return AService;
			const int32 ARank=Rank[A.ResidentID],BRank=Rank[B.ResidentID];
			if(ARank!=BRank)return ARank<BRank;
			const uint32 AH=CitizenHash(static_cast<uint32>(A.HomeBuildingID)),BH=CitizenHash(static_cast<uint32>(B.HomeBuildingID));
			return AH!=BH?AH<BH:A.ResidentID<B.ResidentID;
		});
		Living.SetNum(MaxVisualCitizens,EAllowShrinking::No);
		Living.Sort([](const auto& A,const auto& B){return A.ResidentID<B.ResidentID;});
	}
	TSet<int32> VisibleIDs;for(int32 I=0;I<FMath::Min(MaxVisualCitizens,Living.Num());++I)VisibleIDs.Add(Living[I].ResidentID);
	for(TPair<int32,FCitizenAgent>& Pair:Previous)if(!VisibleIDs.Contains(Pair.Key))CancelAgentTransport(Pair.Value);
	TArray<FCitizenAgent> BeforeRoster=MoveTemp(Agents);Agents.Reset();bCitizenSpatialDirty=true;
	for(const FCityResidentRecord& Resident:Living)
	{
		if(Agents.Num()>=MaxVisualCitizens||WalkableCellCoords.IsEmpty())break;
		const FCitizenAgent* Old=Previous.Find(Resident.ResidentID);
		FCitizenAgent Agent=Old?*Old:FCitizenAgent();
		if(Old)CaptureIndoorDepartureAnchor(Agent,GetWorld());
		Agent.ResidentID=Resident.ResidentID;
		if(!Old)Agent.RandomSeed=static_cast<int32>(static_cast<uint32>(SimulationSeed)^static_cast<uint32>(Resident.ResidentID)*2654435761u);
		Agent.HomeBuildingID=Resident.HomeBuildingID;Agent.WorkBuildingID=Resident.WorkBuildingID;
		Agent.Hunger01=Resident.Hunger01;Agent.Health01=Resident.Health01;Agent.Happiness01=Resident.Happiness01;Agent.bPrefersPrivateCar=Resident.bPrefersPrivateCar;
		Agent.bHomePowered=Life->GetBuildingStatus(Resident.HomeBuildingID).bPowered;
		Agent.bHomeOperating=Life->GetBuildingStatus(Resident.HomeBuildingID).bOperating;
		Agent.bWorkOperating=Resident.WorkBuildingID!=INDEX_NONE&&Life->GetBuildingStatus(Resident.WorkBuildingID).bOperating;
		Agent.LeisureBuildingID=Life->FindServiceDestination(Resident.ResidentID,EBuildingType::GreenSpace,ServiceLookup);
		Agent.FoodBuildingID=Life->FindServiceDestination(Resident.ResidentID,EBuildingType::Restaurant,ServiceLookup);
		if(Agent.FoodBuildingID==INDEX_NONE)Agent.FoodBuildingID=Life->FindServiceDestination(Resident.ResidentID,EBuildingType::Bakery,ServiceLookup);
		Agent.HealthBuildingID=Life->FindServiceDestination(Resident.ResidentID,EBuildingType::Hospital,ServiceLookup);
		const auto Anchor=[&](int32 ID,FVector Fallback)
		{
			++RosterRefreshWork.FrontageLookups;
			const FAftershockCitizenDestination* Record=FrontageByBuilding.FindRef(ID);
			return Record&&!Record->Frontages.IsEmpty()?MakeVisibleCellLocation(*Terrain,Record->Frontages[static_cast<uint32>(Agent.RandomSeed)%Record->Frontages.Num()],Agent):Fallback;
		};
		const FVector Fallback=PickValidGroundLocation(Agent);
		Agent.HomeLocation=Anchor(Agent.HomeBuildingID,Fallback);Agent.WorkLocation=Anchor(Agent.WorkBuildingID,Agent.HomeLocation);
		Agent.LeisureLocation=Anchor(Agent.LeisureBuildingID,Agent.HomeLocation);Agent.FoodLocation=Anchor(Agent.FoodBuildingID,Agent.HomeLocation);Agent.HealthLocation=Anchor(Agent.HealthBuildingID,Agent.HomeLocation);
		FBuildingInstance Park;
		if (Agent.LeisureBuildingID!=INDEX_NONE && World->GetSubsystem<UBuildingManagerSubsystem>()->GetBuildingByID(Agent.LeisureBuildingID,Park)
			&& Park.BuildingData && Park.BuildingData->Type==EBuildingType::GreenSpace && IsPublicWalkingCell(*Terrain,Park.GridCoords))
		{
			// A real reachable park has its own public interior, not a single door
			// coordinate shared by every visitor on the pavement.
			FRandomStream Place(Agent.RandomSeed^0x4c8e719);
			Agent.LeisureLocation=Terrain->GridToWorld(Park.GridCoords,true)+FVector(Place.FRandRange(-.32f,.32f)*Terrain->CellSize,Place.FRandRange(-.32f,.32f)*Terrain->CellSize,0.f);
			Agent.LeisureLocation.Z=VisibleGroundHeight(*Terrain,Agent.LeisureLocation)+CitizenZOffset;
		}
		else if (Agent.LeisureBuildingID == INDEX_NONE && !WalkableCellCoords.IsEmpty())
		{
			// Uniform spatial distribution across all public sidewalks, plazas and promenade cells
			const uint32 Stride = 2654435761u;
			const int32 CellIdx = static_cast<int32>((static_cast<uint32>(Agent.ResidentID) * Stride) % static_cast<uint32>(WalkableCellCoords.Num()));
			Agent.LeisureLocation = MakeVisibleCellLocation(*Terrain, WalkableCellCoords[CellIdx], Agent);
		}
		if(!Old)
		{
			Agent.CurrentLocation=Agent.HomeLocation;Agent.TargetLocation=Agent.CurrentLocation;Agent.FinalDestinationLocation=Agent.CurrentLocation;
			Agent.Activity=EAftershockCitizenActivity::AtHome;Agent.MoveSpeed=0;
			Agent.ScheduleSecondsRemaining=2.0f+CitizenUnit(static_cast<uint32>(Agent.RandomSeed)^0x724c81u)*45.f;
			Agent.WalkingPhaseRadians=CitizenUnit(static_cast<uint32>(Agent.RandomSeed)^0x324a95u)*2.f*PI;
		}
		else
		{
			const FIntPoint Coord=Terrain->WorldToGrid(Agent.CurrentLocation);
			if(!Agent.bInsideVehicle&&!IsCitizenGeometryCell(*Terrain,Coord))
			{CancelAgentTransport(Agent);Agent.CurrentLocation=Agent.HomeLocation;Agent.TargetLocation=Agent.CurrentLocation;Agent.FinalDestinationLocation=Agent.CurrentLocation;Agent.RouteGridCoords.Reset();}
			if(Agent.TransportTripID!=INDEX_NONE)
			{
				FAftershockPassengerTrip Trip;
				if(UTransportSubsystem* Transport=World->GetSubsystem<UTransportSubsystem>())
					if(Transport->GetTrip(Agent.TransportTripID,Trip)&&Trip.DestinationBuildingID!=DestinationBuildingFor(Agent,Agent.TransportArrivalActivity))
					{CancelAgentTransport(Agent);SetAgentActivity(Agent,EAftershockCitizenActivity::Displaced);Agent.TargetLocation=Agent.CurrentLocation;Agent.FinalDestinationLocation=Agent.CurrentLocation;Agent.ScheduleSecondsRemaining=0;}
			}
			else if(!bIsPanicModeActive&&(Agent.MoveSpeed>0||HasPendingNormalRoute(Agent)))
			{
				FVector Goal=Agent.HomeLocation;
				switch(Agent.ActivityAfterArrival){case EAftershockCitizenActivity::AtWork:Goal=Agent.WorkLocation;break;case EAftershockCitizenActivity::AtLeisure:Goal=Agent.LeisureLocation;break;case EAftershockCitizenActivity::AtFoodService:Goal=Agent.FoodLocation;break;case EAftershockCitizenActivity::AtHealthService:Goal=Agent.HealthLocation;break;default:break;}
				// A newly cheaper road does not invalidate an already valid journey.
                // Preserve an unchanged goal/route instead of regenerating one
                // whole-grid reverse field for every moving resident in this sync.
                // Every movement still checks current canonical walking edges;
                // a removed/closed unsafe edge stops and replans at its real position.
                if(!Agent.bRouteReachesDestination||!Agent.FinalDestinationLocation.Equals(Goal,.1f))
                    BuildSafeRoute(Agent,Goal);
			}
			// A repaired home releases its actual living residents from temporary
			// shelter. Those states deliberately do not advance ordinary schedules.
			if(!bIsPanicModeActive&&Agent.bHomeOperating
				&&(Agent.Activity==EAftershockCitizenActivity::AtShelter
					||Agent.Activity==EAftershockCitizenActivity::AwaitingShelter
					||Agent.Activity==EAftershockCitizenActivity::Displaced))
				StartCommute(Agent,Agent.HomeLocation,EAftershockCitizenActivity::AtHome);
		}
		Agent.ISMIndex=Agents.Num();Agents.Add(Agent);bCitizenSpatialDirty=true;
	}
	RosterRefreshWork.ServiceQueries+=ServiceLookup.Queries;RosterRefreshWork.ServiceCacheHits+=ServiceLookup.CacheHits;RosterRefreshWork.ServiceCandidatesVisited+=ServiceLookup.CandidatesVisited;
	FText PlacementError;int32 Migrated=0;float MaximumMove=0.f;const TArray<FCitizenAgent> BeforePlacement=Agents;
	if(!ReconcileCitizenPlacements(Agents,PlacementError,Migrated,MaximumMove))
	{
		Agents=MoveTemp(BeforeRoster);bCitizenSpatialDirty=true;
		UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_CROWD_VISUAL_RECONCILIATION_FAILED %s noNewBodySubmitted=1"),*PlacementError.ToString());return;
	}
	for(int32 I=0;I<Agents.Num();++I)if(!Agents[I].CurrentLocation.Equals(BeforePlacement[I].CurrentLocation,.01f)&&Agents[I].MoveSpeed>0.f)BuildSafeRoute(Agents[I],Agents[I].FinalDestinationLocation);
	RebuildCitizenInstanceTransforms();
	if(Migrated>0)UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CROWD_VISUAL_RECONCILED reason=Roster moved=%d maxDistanceCm=%.2f residentIDsPreserved=1 newNPCs=0"),Migrated,MaximumMove);
	if(bIsPanicModeActive)ReplanEmergencyEvacuation();Host->CitizenISM->MarkRenderStateDirty();UpdateEmotionBillboards();
	RebuildCitizenSpatialIndex();
    if(IncrementalNormalRoutes.IsValid())
    {TSet<uint64> LiveKeys;for(const auto& A:Agents)LiveKeys.Add(NormalRouteAgentKey(A));IncrementalNormalRoutes->Prune(LiveKeys);}
    // Commit this derived watermark only after the actual roster reconciliation
    // succeeded. Failed initial/replacement syncs must remain immediately retryable.
    LastSynchronizedLivingPopulation=Life->GetState().LivingPopulation;
    LastSynchronizedTotalDeaths=Life->GetState().TotalDeaths;
    bCityLifeSyncPending=false;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_LIVE_RESIDENT_CROWD actualLiving=%d renderedResidentIDs=%d cap=%d workerSlotsAddedAsPeople=0 duplicatePopulationAuthority=0"),Life->GetState().LivingPopulation,Agents.Num(),MaxVisualCitizens);
}

bool UCrowdRenderSubsystem::ApplyRouteTree(FCitizenAgent& Agent, const FAftershockCitizenRouteTree& Tree)
{
	const UTerrainGridSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	Agent.RouteGridCoords.Reset();
	Agent.NextRouteCoordIndex = 0;
	Agent.TargetLocation = Agent.CurrentLocation;
	Agent.FinalDestinationLocation = Agent.CurrentLocation;
	Agent.bRouteReachesDestination = false;
	if (!Terrain) return false;
	const FIntPoint Current = Terrain->WorldToGrid(Agent.CurrentLocation);
	if (!Terrain->IsPlayableCoord(Current)) return false;
	int32 At = Terrain->CoordToIndex(Current);
	if (!Tree.Cost.IsValidIndex(At)) return false;
	// An agent caught by rising visible water may exit once to an adjacent safe
	// cell; blocked cells are never used as through routes.
	if (Tree.Cost[At] == MAX_int32)
	{
		int32 Escape = INDEX_NONE, BestCost = MAX_int32;
		for (const FIntPoint Offset : CitizenNeighborOffsets)
		{
			const FIntPoint Coord = Current + Offset;
			if (!CanTakeCitizenEscapeStep(*Terrain,Current,Coord)) continue;
			const int32 Index = Terrain->CoordToIndex(Coord);
			if (Tree.Cost.IsValidIndex(Index) && Tree.Cost[Index] < BestCost) { BestCost = Tree.Cost[Index]; Escape = Index; }
		}
		if (Escape == INDEX_NONE) return false;
		At = Escape;
		Agent.RouteGridCoords.Add(Terrain->IndexToCoord(At));
	}
	int32 Guard = 0;
	while (Tree.Next.IsValidIndex(At) && Tree.Next[At] != INDEX_NONE && Tree.Next[At] != At && Guard++ < Tree.Next.Num())
	{
		int32 Chosen=Tree.Next[At];
		uint32 BestTie=MAX_uint32;
		const FIntPoint From=Terrain->IndexToCoord(At);
		const URoadNetworkSubsystem* Roads=GetWorld()->GetSubsystem<URoadNetworkSubsystem>();
		// Share the optimal distance field, not a single-file route. Only strictly
		// decreasing equal-cost successors qualify, so variation cannot introduce loops.
		for(const FIntPoint Offset:CitizenNeighborOffsets)
		{
			const FIntPoint To=From+Offset;const int32 N=Terrain->CoordToIndex(To);
			if(!Tree.Cost.IsValidIndex(N)||!RoutingCellCosts.IsValidIndex(N)||Tree.Cost[N]==MAX_int32
				||Tree.Cost[N]>=Tree.Cost[At]||RoutingCellCosts[N]<=0
				||static_cast<int64>(Tree.Cost[N])+RoutingCellCosts[N]!=Tree.Cost[At]
				||!Roads||!Roads->CanTraverseWalkingEdge(From,To))continue;
			const uint32 Tie=CitizenHash(static_cast<uint32>(Agent.RandomSeed)^static_cast<uint32>(Agent.RandomSequence)*0x9e3779b9u
				^static_cast<uint32>(To.X)*0x85ebca6bu^static_cast<uint32>(To.Y)*0xc2b2ae35u);
			if(Tie<BestTie){BestTie=Tie;Chosen=N;}
		}
		At = Chosen;
		if (!Tree.Next.IsValidIndex(At)) break;
		Agent.RouteGridCoords.Add(Terrain->IndexToCoord(At));
	}
	if (!Tree.Next.IsValidIndex(At) || Tree.Next[At] != At)
	{
		Agent.RouteGridCoords.Reset();
		return false;
	}
	const FIntPoint Goal = Terrain->IndexToCoord(At);
	Agent.FinalDestinationLocation = MakeVisibleCellLocation(*Terrain, Goal, Agent);
	Agent.TargetLocation = Agent.RouteGridCoords.IsEmpty() ? Agent.FinalDestinationLocation
		: MakeVisibleCellLocation(*Terrain, Agent.RouteGridCoords[0], Agent);
	Agent.bRouteReachesDestination = true;
	return true;
}

void UCrowdRenderSubsystem::BuildSafeRoute(FCitizenAgent& Agent, const FVector& Destination)
{
    const bool CpuDiagnostics=AftershockRoadCpuDiagnosticsEnabled();
    if(CpuDiagnostics)++AftershockRoadGoalCpuTotals().Calls;
	// Several legitimate replans pass Agent.FinalDestinationLocation itself.
	// ApplyRouteTree resets that member while building the new route, so retain
	// the caller's requested destination before mutating any agent fields.
	const FVector RequestedDestination=Destination;
	const UTerrainGridSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain) return;
	const FIntPoint Goal = Terrain->WorldToGrid(RequestedDestination);
	if (!Terrain->IsPlayableCoord(Goal))
	{
        if(IncrementalNormalRoutes.IsValid())IncrementalNormalRoutes->Cancel(NormalRouteAgentKey(Agent));
		FAftershockCitizenRouteTree Empty;
		ApplyRouteTree(Agent, Empty);
		return;
	}
	const int32 GoalIndex = Terrain->CoordToIndex(Goal);
    if(UsesIncrementalNormalRouting())
    {
        if(!IncrementalNormalRoutes.IsValid())
        {
            // Completed eager fields are legacy/panic scratch. One total cache
            // ceiling remains; reserve bookkeeping outside the helper byte cap.
            NormalRouteTrees.Empty();NormalRouteTreeUseSerial.Empty();
            IncrementalNormalRoutes=MakeShared<FAftershockIncrementalNormalRoutes>(MaximumNormalRouteCacheBytes-256ull*1024ull);
        }
        TArray<int32,TInlineAllocator<4>> Sources;
        const FIntPoint Source=Terrain->WorldToGrid(Agent.CurrentLocation);const int32 SourceIndex=Terrain->CoordToIndex(Source);
        if(RoutingCellCosts.IsValidIndex(SourceIndex)&&RoutingCellCosts[SourceIndex]!=MAX_int32)Sources.Add(SourceIndex);
        else for(FIntPoint Offset:CitizenNeighborOffsets)
        {
            const FIntPoint Escape=Source+Offset;const int32 I=Terrain->CoordToIndex(Escape);
            if(CanTakeCitizenEscapeStep(*Terrain,Source,Escape)&&RoutingCellCosts.IsValidIndex(I)&&RoutingCellCosts[I]!=MAX_int32)Sources.Add(I);
        }
        const uint64 Key=NormalRouteAgentKey(Agent);
        if(Sources.IsEmpty())
        {IncrementalNormalRoutes->Cancel(Key);FAftershockCitizenRouteTree Empty;ApplyRouteTree(Agent,Empty);return;}
        const bool KeepCurrent=Agent.bRouteReachesDestination&&Agent.MoveSpeed>0.f&&!Agent.RouteGridCoords.IsEmpty()
            &&Agent.FinalDestinationLocation.Equals(RequestedDestination,.1f)&&CanMoveCitizenSegment(*Terrain,Agent.CurrentLocation,Agent.TargetLocation);
        const FAftershockCitizenRouteTree* Tree=nullptr;
        const auto Result=IncrementalNormalRoutes->Request(Key,Goal,Sources,RequestedDestination,KeepCurrent,*Terrain,RoutingCellCosts,Tree);
        if(Result==FAftershockIncrementalNormalRoutes::EResult::Ready&&Tree)
        {
            if(CpuDiagnostics)++AftershockRoadGoalCpuTotals().CacheHits;
            const double DeliveryStarted=FPlatformTime::Seconds();
            if(ApplyRouteTree(Agent,*Tree)){Agent.FinalDestinationLocation=RequestedDestination;if(Agent.RouteGridCoords.IsEmpty())Agent.TargetLocation=RequestedDestination;}
            // Charge actual reconstruction CPU, not elapsed time spent moving
            // lower-ID residents; otherwise high IDs could starve every frame.
            IncrementalNormalRoutes->ConsumeDeliveryCpu(FPlatformTime::Seconds()-DeliveryStarted);
        }
        else if(Result==FAftershockIncrementalNormalRoutes::EResult::Unreachable)
        {FAftershockCitizenRouteTree Empty;ApplyRouteTree(Agent,Empty);}
        else if(!KeepCurrent)
        {
            Agent.RouteGridCoords.Reset();Agent.NextRouteCoordIndex=0;Agent.TargetLocation=Agent.CurrentLocation;
            Agent.FinalDestinationLocation=RequestedDestination;Agent.bRouteReachesDestination=false;
        }
        return;
    }
	if (!NormalRouteTrees.Contains(GoalIndex))
	{
		TArray<FIntPoint> Sources;
		Sources.Add(Goal);
		const double FieldStarted=CpuDiagnostics?FPlatformTime::Seconds():0.;
		FAftershockCitizenRouteTree Tree = BuildCitizenRouteTree(*Terrain, RoutingCellCosts, Sources);
		if(CpuDiagnostics){++AftershockRoadGoalCpuTotals().Fields;AftershockRoadGoalCpuTotals().FieldBuildMs+=(FPlatformTime::Seconds()-FieldStarted)*1000.;}
		TrimNormalRouteCache(Tree.Cost.GetAllocatedSize() + Tree.Next.GetAllocatedSize());
		NormalRouteTrees.Add(GoalIndex, MoveTemp(Tree));
		++NormalRouteTreeBuildCount;
	}
	else if(CpuDiagnostics)++AftershockRoadGoalCpuTotals().CacheHits;
	NormalRouteTreeUseSerial.Add(GoalIndex, ++NormalRouteCacheSerial);
	if(ApplyRouteTree(Agent,NormalRouteTrees.FindChecked(GoalIndex)))
	{
		Agent.FinalDestinationLocation=RequestedDestination;
		if(Agent.RouteGridCoords.IsEmpty())Agent.TargetLocation=RequestedDestination;
	}
}

void UCrowdRenderSubsystem::HandleAgentArrival(FCitizenAgent& Agent)
{
	if(Agent.Activity==EAftershockCitizenActivity::WalkingToTransport)
	{
		if(Agent.bRouteReachesDestination)
			if(UTransportSubsystem* Transport=GetWorld()?GetWorld()->GetSubsystem<UTransportSubsystem>():nullptr)
				if(Transport->MarkPassengerAtPickup(Agent.TransportTripID,Agent.ResidentID,Agent.CurrentLocation))
				{SetAgentActivity(Agent,EAftershockCitizenActivity::WaitingForTransport);return;}
		CancelAgentTransport(Agent);
		StartCommute(Agent,Agent.TransportFinalDestination,Agent.TransportArrivalActivity,false);
		return;
	}

	if (!Agent.bRouteReachesDestination && (Agent.Activity == EAftershockCitizenActivity::Commuting
		|| Agent.Activity == EAftershockCitizenActivity::Evacuating || Agent.Activity == EAftershockCitizenActivity::Rescued))
	{
		SetAgentActivity(Agent, bIsPanicModeActive ? EAftershockCitizenActivity::AwaitingShelter : EAftershockCitizenActivity::WaitingForRoute);
		Agent.ScheduleSecondsRemaining = 10.f;
		return;
	}
	if (Agent.Activity == EAftershockCitizenActivity::Rescued)
	{
		SetAgentActivity(Agent, EAftershockCitizenActivity::AtHome);
		Agent.ScheduleSecondsRemaining = CitizenDwell(Agent,8.f);
		return;
	}

	if (Agent.Activity == EAftershockCitizenActivity::Commuting || Agent.Activity == EAftershockCitizenActivity::Evacuating)
	{
		SetAgentActivity(Agent, Agent.ActivityAfterArrival);
		CaptureIndoorDepartureAnchor(Agent,GetWorld(),true);
		if(Agent.ResidentID!=INDEX_NONE&&(Agent.Activity==EAftershockCitizenActivity::AtFoodService||Agent.Activity==EAftershockCitizenActivity::AtHealthService))
			if(UCityLifeSubsystem* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr)
				if(Life->ReportServiceVisit(Agent.ResidentID,DestinationBuildingFor(Agent,Agent.Activity),Agent.CurrentLocation))
				{
					FCityResidentRecord Resident;
					if(Life->GetResident(Agent.ResidentID,Resident)){Agent.Hunger01=Resident.Hunger01;Agent.Health01=Resident.Health01;Agent.Happiness01=Resident.Happiness01;}
				}
		if(Agent.Activity==EAftershockCitizenActivity::AtFoodService)Agent.FoodVisitCooldown=40.f;
		if(Agent.Activity==EAftershockCitizenActivity::AtHealthService)Agent.HealthVisitCooldown=50.f;
		switch (Agent.Activity)
		{
		case EAftershockCitizenActivity::AtHome:
			Agent.ScheduleSecondsRemaining = CitizenDwell(Agent,14.f);
			break;
		case EAftershockCitizenActivity::AtWork:
			Agent.ScheduleSecondsRemaining = CitizenDwell(Agent,24.f);
			break;
		case EAftershockCitizenActivity::AtLeisure:
			Agent.ScheduleSecondsRemaining = CitizenDwell(Agent,10.f);
			break;
		case EAftershockCitizenActivity::AtShelter:
			Agent.ScheduleSecondsRemaining = 0.0f;
			break;
		default:
			Agent.ScheduleSecondsRemaining = CitizenDwell(Agent,10.f);
			break;
		}
	}
}

void UCrowdRenderSubsystem::AdvanceAgentSchedule(FCitizenAgent& Agent)
{
	if (Agent.TransportTripID!=INDEX_NONE || bIsPanicModeActive || Agent.Activity == EAftershockCitizenActivity::AwaitingShelter || Agent.Activity == EAftershockCitizenActivity::AtShelter ||
		Agent.Activity == EAftershockCitizenActivity::Evacuating ||
		Agent.Activity == EAftershockCitizenActivity::Rescued)
	{
		return;
	}

	if (Agent.ScheduleSecondsRemaining > 0.0f)
	{
		Agent.ScheduleSecondsRemaining = FMath::Max(0.0f, Agent.ScheduleSecondsRemaining - CrowdUpdateSeconds);
		return;
	}

	if(Agent.ResidentID!=INDEX_NONE)
	{
		// A shared building doorway cannot emit an entire household on one timer.
		// Keep deferred people indoors and retain their canonical resident identity.
		int32 Portal=INDEX_NONE;
		if(Agent.Activity==EAftershockCitizenActivity::AtHome)Portal=Agent.HomeBuildingID;
		else if(Agent.Activity==EAftershockCitizenActivity::AtWork)Portal=Agent.WorkBuildingID;
		else if(Agent.Activity==EAftershockCitizenActivity::AtFoodService)Portal=Agent.FoodBuildingID;
		else if(Agent.Activity==EAftershockCitizenActivity::AtHealthService)Portal=Agent.HealthBuildingID;
		if(Portal!=INDEX_NONE)
		{
			int32& Released=DeparturesThisUpdate.FindOrAdd(Portal);
			if(Released>=1){Agent.ScheduleSecondsRemaining=.16f+CitizenUnit(static_cast<uint32>(Agent.RandomSeed))* .32f;return;}
			++Released;
		}
		if(Agent.Health01<.65f&&Agent.HealthBuildingID!=INDEX_NONE&&Agent.Activity!=EAftershockCitizenActivity::AtHealthService&&Agent.HealthVisitCooldown<=0)
		{StartCommute(Agent,Agent.HealthLocation,EAftershockCitizenActivity::AtHealthService);return;}
		if(Agent.Hunger01>.45f&&Agent.FoodBuildingID!=INDEX_NONE&&Agent.Activity!=EAftershockCitizenActivity::AtFoodService&&Agent.FoodVisitCooldown<=0)
		{StartCommute(Agent,Agent.FoodLocation,EAftershockCitizenActivity::AtFoodService);return;}
		if(Agent.Activity==EAftershockCitizenActivity::AtFoodService||Agent.Activity==EAftershockCitizenActivity::AtHealthService)
		{StartCommute(Agent,Agent.bWorkOperating?Agent.WorkLocation:Agent.HomeLocation,Agent.bWorkOperating?EAftershockCitizenActivity::AtWork:EAftershockCitizenActivity::AtHome);return;}
	}
	if (Agent.Activity == EAftershockCitizenActivity::WaitingForRoute)
	{
		FVector Destination=Agent.HomeLocation;
		switch(Agent.ActivityAfterArrival)
		{
		case EAftershockCitizenActivity::AtWork:Destination=Agent.WorkLocation;break;
		case EAftershockCitizenActivity::AtLeisure:Destination=Agent.LeisureLocation;break;
		case EAftershockCitizenActivity::AtFoodService:Destination=Agent.FoodLocation;break;
		case EAftershockCitizenActivity::AtHealthService:Destination=Agent.HealthLocation;break;
		default:break;
		}
		StartCommute(Agent, Destination, Agent.ActivityAfterArrival);
		return;
	}
	if (Agent.Activity == EAftershockCitizenActivity::AtHome)
	{
		if (Agent.WorkBuildingID != INDEX_NONE && Agent.bWorkOperating)
		{
			StartCommute(Agent, Agent.WorkLocation, EAftershockCitizenActivity::AtWork);
		}
		else
		{
			const FVector Destination = Agent.LeisureBuildingID != INDEX_NONE ? Agent.LeisureLocation : Agent.HomeLocation;
			StartCommute(Agent, Destination, EAftershockCitizenActivity::AtLeisure);
		}
		return;
	}

	if (Agent.Activity == EAftershockCitizenActivity::AtWork)
	{
		if (Agent.LeisureBuildingID != INDEX_NONE)
		{
			StartCommute(Agent, Agent.LeisureLocation, EAftershockCitizenActivity::AtLeisure);
		}
		else
		{
			StartCommute(Agent, Agent.HomeLocation, EAftershockCitizenActivity::AtHome);
		}
		return;
	}

	if (Agent.Activity == EAftershockCitizenActivity::AtLeisure)
	{
		StartCommute(Agent, Agent.HomeLocation, EAftershockCitizenActivity::AtHome);
		return;
	}

	// A missing destination returns to the existing home/base anchor, never a random grid point.
	StartCommute(Agent, Agent.HomeLocation, EAftershockCitizenActivity::AtHome);
}

void UCrowdRenderSubsystem::UpdatePopulationCount(int32 TotalResidents, int32 TotalWorkers)
{
	if(const UCityLifeSubsystem* Life=!bSyntheticRenderProfileActive&&GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr)
		if(Life->IsCityInitialized()){SynchronizeCityLifeRoster();return;}
	if(!bSyntheticRenderProfileActive&&GetWorld())
	{
		const auto* Scenario=GetWorld()->GetSubsystem<UAftershockScenarioSubsystem>();
		const auto* Session=GetWorld()->GetGameInstance()?GetWorld()->GetGameInstance()->GetSubsystem<UAftershockOpeningSessionSubsystem>():nullptr;
		// Settlement construction broadcasts capacities before the canonical life
		// roster exists. Those capacities are never a second source of people.
		if((Scenario&&Scenario->IsPrepared())||(Session&&Session->HasConfiguredSession()))return;
	}

	AAftershockCrowdHostActor* Host = GetOrCreateHostActor();
	if (!Host || !Host->CitizenISM)
	{
		return;
	}

	RebuildDestinationCaches();
	const UTerrainGridSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	for (int32 Index = 0; Index < Agents.Num(); ++Index)
	{
		FCitizenAgent& Agent = Agents[Index];
		AssignScheduleDestinations(Agent, Index);
		if (Terrain)
		{
			const FIntPoint Coord = Terrain->WorldToGrid(Agent.CurrentLocation);
			// Only repair invalid geometry after a building edit; never teleport
			// citizens out of rising water or fire to manufacture an evacuation.
			if (!IsCitizenGeometryCell(*Terrain,Coord))
			{
				Agent.CurrentLocation = Agent.HomeLocation;
				Agent.TargetLocation = Agent.CurrentLocation;
				Agent.FinalDestinationLocation = Agent.CurrentLocation;
				Agent.RouteGridCoords.Reset();
				Agent.NextRouteCoordIndex = 0;
			}
		}
		if (!bIsPanicModeActive && Agent.MoveSpeed > 0.f)
		{
			const FVector Destination = Agent.ActivityAfterArrival == EAftershockCitizenActivity::AtWork ? Agent.WorkLocation
				: (Agent.ActivityAfterArrival == EAftershockCitizenActivity::AtLeisure ? Agent.LeisureLocation : Agent.HomeLocation);
			BuildSafeRoute(Agent, Destination);
		}
	}

	const int64 TotalPopulation = static_cast<int64>(FMath::Max(0, TotalResidents)) + FMath::Max(0, TotalWorkers);
	const int32 TargetCount = WalkableCellCoords.IsEmpty() ? 0
		: static_cast<int32>(FMath::Min<int64>(TotalPopulation / 2, MaxVisualCitizens));
	const FVector RenderScale = CitizenRenderScale(Host->CitizenISM->GetStaticMesh());
	const float PivotOffset = -CitizenSourceBounds(Host->CitizenISM->GetStaticMesh()).Min.Z * RenderScale.Z;

	while (Agents.Num() < TargetCount)
	{
		FCitizenAgent NewAgent;
		NewAgent.RandomSeed = static_cast<int32>(static_cast<uint32>(SimulationSeed) ^ (static_cast<uint32>(Agents.Num() + 1) * 2654435761u));

		AssignScheduleDestinations(NewAgent, Agents.Num());
		NewAgent.ShelterLocation = NewAgent.HomeLocation;
		NewAgent.CurrentLocation = NewAgent.HomeLocation;
		NewAgent.TargetLocation = NewAgent.HomeLocation;
		NewAgent.FinalDestinationLocation = NewAgent.HomeLocation;
		NewAgent.ScheduleSecondsRemaining = .35f+CitizenUnit(static_cast<uint32>(NewAgent.RandomSeed)^0x724c81u)*24.f;
		NewAgent.WalkingPhaseRadians=CitizenUnit(static_cast<uint32>(NewAgent.RandomSeed)^0x324a95u)*2.f*PI;
		NewAgent.Activity = EAftershockCitizenActivity::AtHome;
		NewAgent.MoveSpeed = 0.0f;
		NewAgent.FacingYaw = static_cast<float>(NewAgent.RandomSeed % 360);
		NewAgent.bIsPanicking = bIsPanicModeActive;
		if(bSyntheticRenderProfileActive&&Terrain)
		{
			// The explicit render fixture must exercise actual separated bodies,
			// rather than drawing1500 copies at a few residential portals.
			bool Found=false;const int32 Base=CitizenHash(static_cast<uint32>(NewAgent.RandomSeed))%WalkableCellCoords.Num();
			for(int32 Attempt=0;Attempt<FMath::Min(256,WalkableCellCoords.Num());++Attempt)
			{
				const FIntPoint Coord=WalkableCellCoords[(Base+Attempt)%WalkableCellCoords.Num()];
				const FVector Candidate=MakeVisibleCellLocation(*Terrain,Coord,NewAgent);
				if(CanAdmitCitizenAt(Candidate)){NewAgent.CurrentLocation=Candidate;NewAgent.TargetLocation=Candidate;NewAgent.FinalDestinationLocation=Candidate;Found=true;break;}
			}
			if(!Found){UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_SYNTHETIC_PROFILE_CAPACITY requested=%d admitted=%d noUnsafeBodySpawn=1"),TargetCount,Agents.Num());break;}
		}

		const FTransform SpawnTransform(FRotator(0.f, NewAgent.FacingYaw, 0.f), NewAgent.CurrentLocation + FVector(0.f, 0.f, PivotOffset), RenderScale);
		NewAgent.ISMIndex = Host->CitizenISM->AddInstance(SpawnTransform, true);
		if (NewAgent.ISMIndex == INDEX_NONE)
		{
			break;
		}

		Agents.Add(NewAgent);bCitizenSpatialDirty=true;
		SetCitizenInstanceColor(Agents.Last());
	}

	// Remove the last instance with the matching agent; hidden instances would otherwise accumulate on regrowth.
	while (Agents.Num() > TargetCount)
	{
		const int32 LastAgentIndex = Agents.Num() - 1;
		const int32 InstanceIndex = Agents[LastAgentIndex].ISMIndex;
		if (!Host->CitizenISM->RemoveInstance(InstanceIndex))
		{
			break;
		}
		Agents.RemoveAt(LastAgentIndex, 1, EAllowShrinking::No);bCitizenSpatialDirty=true;
	}

	if (bIsPanicModeActive) ReplanEmergencyEvacuation();
	Host->CitizenISM->MarkRenderStateDirty();
	if (!Host->CitizenISM->IsRenderStateCreated() && Host->GetWorld() && Host->GetWorld()->HasBegunPlay())
	{
		Host->CitizenISM->ReregisterComponent();
	}
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_RUNTIME_CROWD residents=%d workers=%d agents=%d instances=%d mesh=%s material=%s"),
		TotalResidents,
		TotalWorkers,
		Agents.Num(),
		Host->CitizenISM->GetInstanceCount(),
		Host->CitizenISM->GetStaticMesh() ? *Host->CitizenISM->GetStaticMesh()->GetPathName() : TEXT("None"),
		Host->CitizenISM->GetMaterial(0) ? *Host->CitizenISM->GetMaterial(0)->GetPathName() : TEXT("None"));
	AuditCrowdVisibility();
}

#if !UE_BUILD_SHIPPING
bool UCrowdRenderSubsystem::BeginSyntheticRenderProfile(int32 RequestedAgents)
{
	int32 ExplicitCount=0;
	if(!FParse::Value(FCommandLine::Get(),TEXT("AftershockProfileAgents="),ExplicitCount))return false;
	RequestedAgents=FMath::Clamp(RequestedAgents,1,MaxVisualCitizens);
	if(RequestedAgents!=FMath::Clamp(ExplicitCount,1,MaxVisualCitizens))return false;
	for(FCitizenAgent& Agent:Agents)CancelAgentTransport(Agent);
	Agents.Reset();
	if(IsValid(CrowdHostActor)&&CrowdHostActor->CitizenISM)CrowdHostActor->CitizenISM->ClearInstances();
    IncrementalNormalRoutes.Reset();
	bSyntheticRenderProfileActive=true;
	SyntheticProfileSamples=0;SyntheticProfileTotalMilliseconds=0.;SyntheticProfileMaxMilliseconds=0.;
	UpdatePopulationCount(RequestedAgents*2,0);
	for(FCitizenAgent& Agent:Agents)
	{
		Agent.ResidentID=INDEX_NONE;
		Agent.ScheduleSecondsRemaining=0.f;
		StartCommute(Agent,Agent.WorkLocation,EAftershockCitizenActivity::AtWork,false);
	}
	const UCityLifeSubsystem* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SYNTHETIC_RENDER_PROFILE requested=%d actual=%d canonicalLiving=%d syntheticResidentID=-1 economyWrites=0 transportPlanning=0 timerSeconds=0.08"),RequestedAgents,Agents.Num(),Life?Life->GetState().LivingPopulation:0);
	return Agents.Num()==RequestedAgents;
}
#endif

void UCrowdRenderSubsystem::BeginEmergencyEvacuation(EDisasterType Type)
{
    IncrementalNormalRoutes.Reset();
	for(FCitizenAgent& Agent:Agents)CancelAgentTransport(Agent);
	if(UTransportSubsystem* Transport=GetWorld()?GetWorld()->GetSubsystem<UTransportSubsystem>():nullptr)Transport->SetEmergencyMode(true);
	EmergencyDisasterType = Type;
	bIsPanicModeActive = true;
	EmergencyStartedAtWorldSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	RoutingRefreshSeconds = 0.f;
	if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(DisasterVisualCompletionHandle);
	if(ScopedCrowdRefreshDepth==0)RebuildDestinationCaches();
	ReplanEmergencyEvacuation();
	if(ScopedCrowdRefreshDepth>0)bScopedPlanningAuditPending=true;
	else AuditCitizenPlanning();
}

void UCrowdRenderSubsystem::SetPanicMode(bool bPanic)
{
	if (bPanic) { BeginEmergencyEvacuation(EmergencyDisasterType); return; }
	bIsPanicModeActive = false;
	if(UTransportSubsystem* Transport=GetWorld()?GetWorld()->GetSubsystem<UTransportSubsystem>():nullptr)Transport->SetEmergencyMode(false);
	for (FCitizenAgent& Agent : Agents)
	{
		Agent.bIsPanicking = false;
		Agent.bEmergencyPlaceReserved = false;
		Agent.bEmergencyOverflow = false;
		Agent.bEmergencyUnreachable = false;
		Agent.EmergencyBuildingID = INDEX_NONE;
		if (Agent.Activity == EAftershockCitizenActivity::Evacuating
			|| Agent.Activity == EAftershockCitizenActivity::AtShelter
			|| Agent.Activity == EAftershockCitizenActivity::AwaitingShelter)
			StartCommute(Agent, Agent.HomeLocation, EAftershockCitizenActivity::AtHome);
	}
	if (IsValid(CrowdHostActor) && CrowdHostActor->CitizenISM) CrowdHostActor->CitizenISM->MarkRenderStateDirty();
}

void UCrowdRenderSubsystem::HandleDisasterVisualSnapshot(const FAftershockDisasterVisualSnapshot& Snapshot)
{
	// Warning and post-damage snapshots use one planner. Damage/casualty results
	// remain exclusively owned by the disaster/economy subsystems.
	if (Snapshot.Revision > 0) BeginEmergencyEvacuation(Snapshot.Type);
}

void UCrowdRenderSubsystem::ReplanEmergencyEvacuation()
{
	CSV_SCOPED_TIMING_STAT(AftershockCrowd, ReplanEvacuation);
	if (!bIsPanicModeActive || !GetWorld()) return;
	if(ScopedCrowdRefreshDepth>0||bDeferEmergencyPlanDuringRosterFlush)
	{bScopedEmergencyPlanPending=true;return;}
	const UTerrainGridSubsystem* Terrain = GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
	const UDisasterPresentationSubsystem* Weather = GetWorld()->GetSubsystem<UDisasterPresentationSubsystem>();
	if (!Terrain) return;
	bScopedEmergencyPlanPending=false;++RosterRefreshWork.EmergencyPlans;
	UTransportSubsystem* MunicipalTransport=GetWorld()->GetSubsystem<UTransportSubsystem>();
	TSet<int32> MunicipalOnDuty;
	if(MunicipalTransport)for(int32 A=0;A<Agents.Num();++A)
	{
		const auto& Agent=Agents[A];FAftershockMunicipalDutyView Duty;
		if(MunicipalTransport->GetMunicipalDuty(Agent.ResidentID,Duty))
		{
			if(!IsMunicipalWorkAnchorSafe(Agent,GetWorld()))MunicipalTransport->CancelMunicipalDutyForResident(Agent.ResidentID);
			if(MunicipalTransport->GetMunicipalDuty(Agent.ResidentID,Duty))MunicipalOnDuty.Add(A);
		}
		else if(IsMunicipalWorkAnchorSafe(Agent,GetWorld())&&MunicipalTransport->IsMunicipalStaffResident(Agent.ResidentID,Agent.WorkBuildingID))MunicipalOnDuty.Add(A);
	}
	const int32 CellCount = Terrain->Cells.Num();
	TArray<FAftershockCitizenRouteTree> Trees;
	const URoadNetworkSubsystem* Roads=GetWorld()->GetSubsystem<URoadNetworkSubsystem>();
	const FVector Inland=Terrain->GetCoastInlandDirection();
	float ShoreProjection=-MAX_flt;
	for(const FGridCell& Cell:Terrain->Cells)if(Cell.bIsOcean)
		ShoreProjection=FMath::Max(ShoreProjection,static_cast<float>(Cell.Coords.X*Inland.X+Cell.Coords.Y*Inland.Y));
	TArray<int32> Remaining;
	Trees.Reserve(EmergencyCentres.Num());
	Remaining.Reserve(EmergencyCentres.Num());
	ActiveEligibleCentres = 0;
	ActiveVisualPlaceQuota = 0;
	for (const FAftershockCitizenDestination& Centre : EmergencyCentres)
	{
		TArray<FIntPoint> Sources;
		bool bBuildingDry = !Weather || Weather->GetDisplayedFloodDepthCm(Centre.BuildingCoord) <= VisibleWaterRouteCutoffCm;
		if(EmergencyDisasterType==EDisasterType::Tsunami)
		{
			bBuildingDry &= !Terrain->GetCell(Centre.BuildingCoord).bIsCoastal;
			const UDisasterSimulationSubsystem* D=GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>();
			if(D && D->GetLatestVisualSnapshot().Type==EDisasterType::Tsunami)
				bBuildingDry &= D->GetLatestVisualSnapshot().LocalSeverity.FindRef(Centre.BuildingCoord)<=0.f;
		}
		if (bBuildingDry)
		{
			for (const FIntPoint Coord : Centre.Frontages)
			{
				const int32 Index = Terrain->CoordToIndex(Coord);
				if (RoutingCellCosts.IsValidIndex(Index) && RoutingCellCosts[Index] != MAX_int32) Sources.Add(Coord);
			}
		}
		Trees.Add(BuildCitizenRouteTree(*Terrain, RoutingCellCosts, Sources));
		const int32 Quota = Sources.IsEmpty() ? 0 : Centre.Weight;
		Remaining.Add(Quota);
		ActiveEligibleCentres += Sources.IsEmpty() ? 0 : 1;
		ActiveVisualPlaceQuota += Quota;
	}
	const auto CostFromAgent = [Terrain](const FAftershockCitizenRouteTree& Tree, const FCitizenAgent& Agent)
	{
		const FIntPoint Coord = Terrain->WorldToGrid(Agent.CurrentLocation);
		if (!Terrain->IsPlayableCoord(Coord)) return MAX_int32;
		const int32 Index = Terrain->CoordToIndex(Coord);
		if (!Tree.Cost.IsValidIndex(Index)) return MAX_int32;
		if (Tree.Cost[Index] != MAX_int32) return Tree.Cost[Index];
		int32 EscapeCost = MAX_int32;
		for (const FIntPoint Offset : CitizenNeighborOffsets)
		{
			const FIntPoint Escape = Coord + Offset;
			if (!CanTakeCitizenEscapeStep(*Terrain,Coord,Escape)) continue;
			const int32 EscapeIndex = Terrain->CoordToIndex(Escape);
			if (!Tree.Cost.IsValidIndex(EscapeIndex)) continue;
			const int32 Cost = Tree.Cost[EscapeIndex];
			if (Cost != MAX_int32) EscapeCost = FMath::Min(EscapeCost, Cost + 30);
		}
		return EscapeCost;
	};
	// Keep valid reservations across rising-water/road replans before admitting
	// anyone else. Arrival never changes the authoritative population.
	TArray<int32> Selected;
	Selected.Init(INDEX_NONE, Agents.Num());
	for (int32 A = 0; A < Agents.Num(); ++A)
	{
		if(MunicipalOnDuty.Contains(A))continue;
		const FCitizenAgent& Agent = Agents[A];
		if (!Agent.bEmergencyPlaceReserved) continue;
		for (int32 C = 0; C < EmergencyCentres.Num(); ++C)
		{
			if (Agent.EmergencyBuildingID == EmergencyCentres[C].BuildingID && Remaining[C] > 0
				&& CostFromAgent(Trees[C], Agent) != MAX_int32)
			{
				Selected[A] = C; --Remaining[C]; break;
			}
		}
	}
	// Prefer dedicated shelter, then the nearest intact school/hospital/fire
	// station forecourt. For wind/fire, enclosed aid centres win equal routes.
	const auto PolicyTieCost = [this](EBuildingType Type)
	{
		if (Type == EBuildingType::Shelter) return 0;
		if (EmergencyDisasterType == EDisasterType::Earthquake)
			return Type == EBuildingType::School ? 1 : (Type == EBuildingType::Hospital ? 2 : 3);
		return Type == EBuildingType::Hospital ? 1 : (Type == EBuildingType::FireStation ? 2 : 3);
	};
	for (int32 A = 0; A < Agents.Num(); ++A)
	{
		if (Selected[A] != INDEX_NONE||MunicipalOnDuty.Contains(A)) continue;
		int32 Best = INDEX_NONE, BestScore = MAX_int32;
		for (int32 C = 0; C < EmergencyCentres.Num(); ++C)
		{
			if (Remaining[C] <= 0) continue;
			const int32 Cost = CostFromAgent(Trees[C], Agents[A]);
			if (Cost == MAX_int32) continue;
			int32 Score = Cost + PolicyTieCost(EmergencyCentres[C].Type);
			if(EmergencyDisasterType==EDisasterType::Tsunami)
			{
				// Explicit evacuation preference, not additional hazard physics.
				const FGridCell CentreCell=Terrain->GetCell(EmergencyCentres[C].BuildingCoord);
				Score -= FMath::Clamp(FMath::RoundToInt(CentreCell.Elevation/10.f),0,80);
				if(ShoreProjection>-MAX_flt)
					Score -= FMath::Clamp(FMath::RoundToInt(CentreCell.Coords.X*Inland.X+CentreCell.Coords.Y*Inland.Y-ShoreProjection),0,20)*3;
			}
			if (Score < BestScore) { BestScore = Score; Best = C; }
		}
		if (Best != INDEX_NONE) { Selected[A] = Best; --Remaining[Best]; }
	}

	TArray<FIntPoint> AssemblySources;
	for (const FIntPoint Coord : AssemblyCoords)
	{
		const int32 Index = Terrain->CoordToIndex(Coord);
		if (RoutingCellCosts.IsValidIndex(Index) && RoutingCellCosts[Index] != MAX_int32) AssemblySources.AddUnique(Coord);
	}
	// A district isolated by a river can still assemble on its own public high
	// ground. One deterministic fallback per safe connected component avoids an
	// unreachable global point or off-map random escape vector.
	TSet<int32> Visited;
	for (int32 Start = 0; Start < CellCount; ++Start)
	{
		if (RoutingCellCosts[Start] == MAX_int32 || Visited.Contains(Start)) continue;
		TArray<int32> Queue; Queue.Add(Start); Visited.Add(Start);
		int32 Best = Start;
		bool bHasDesignedAssembly = false;
		for (int32 Read = 0; Read < Queue.Num(); ++Read)
		{
			const int32 At = Queue[Read];
			const FIntPoint Coord = Terrain->IndexToCoord(At);
			bHasDesignedAssembly |= AssemblySources.Contains(Coord);
			const FIntPoint BestCoord = Terrain->IndexToCoord(Best);
			if (Terrain->GetCell(Coord).Elevation > Terrain->GetCell(BestCoord).Elevation) Best = At;
			for (const FIntPoint Offset : CitizenNeighborOffsets)
			{
				const FIntPoint Neighbor = Coord + Offset;
				if (!Terrain->IsPlayableCoord(Neighbor) || (Roads&&!Roads->CanTraverseWalkingEdge(Coord,Neighbor))) continue;
				const int32 Next = Terrain->CoordToIndex(Neighbor);
				if (!RoutingCellCosts.IsValidIndex(Next) || RoutingCellCosts[Next] == MAX_int32 || Visited.Contains(Next)) continue;
				Visited.Add(Next); Queue.Add(Next);
			}
		}
		if (!bHasDesignedAssembly) AssemblySources.Add(Terrain->IndexToCoord(Best));
	}
	const FAftershockCitizenRouteTree AssemblyTree = BuildCitizenRouteTree(*Terrain, RoutingCellCosts, AssemblySources);
	for (int32 A = 0; A < Agents.Num(); ++A)
	{
		FCitizenAgent& Agent = Agents[A];
		const bool bDepartingIndoors=IsCitizenIndoors(Agent,GetWorld());
		if(MunicipalOnDuty.Contains(A))
		{
			Agent.bIsPanicking=false;Agent.bEmergencyPlaceReserved=false;Agent.bEmergencyOverflow=false;
			Agent.bEmergencyUnreachable=false;Agent.EmergencyBuildingID=INDEX_NONE;
			continue; // safe on-shift responders do not consume a civilian shelter place.
		}
		if(bDepartingIndoors)CaptureIndoorDepartureAnchor(Agent,GetWorld());
		int32 CentreIndex = Selected[A];
		const bool bReserved = CentreIndex != INDEX_NONE;
		// Full centres remain recognizable public forecourts; no false admission.
		if (!bReserved)
		{
			int32 BestScore = MAX_int32;
			for (int32 C = 0; C < EmergencyCentres.Num(); ++C)
			{
				const int32 Cost = CostFromAgent(Trees[C], Agent);
				if (Cost != MAX_int32 && Cost < BestScore) { BestScore = Cost; CentreIndex = C; }
			}
		}
		Agent.EmergencyBuildingID = CentreIndex == INDEX_NONE ? INDEX_NONE : EmergencyCentres[CentreIndex].BuildingID;
		Agent.bEmergencyPlaceReserved = bReserved;
		Agent.bEmergencyOverflow = !bReserved && CentreIndex != INDEX_NONE;
		Agent.bEmergencyUnreachable = CentreIndex == INDEX_NONE;
		const FAftershockCitizenRouteTree& Tree = CentreIndex == INDEX_NONE ? AssemblyTree : Trees[CentreIndex];
		const bool bReached = ApplyRouteTree(Agent, Tree);
		Agent.bEmergencyUnreachable |= !bReached;
		Agent.ShelterLocation = Agent.FinalDestinationLocation;
		Agent.ActivityAfterArrival = bReserved ? EAftershockCitizenActivity::AtShelter : EAftershockCitizenActivity::AwaitingShelter;
		if(Agent.bInsideVehicle){Agent.TransportFinalDestination=Agent.ShelterLocation;Agent.TransportArrivalActivity=Agent.ActivityAfterArrival;}
		Agent.ScheduleSecondsRemaining = 0.f;
		SetAgentActivity(Agent, EAftershockCitizenActivity::Evacuating);
		Agent.bWaitingForOutdoorSpace=bDepartingIndoors;
	}
	bCitizenSpatialDirty=true;
	if (IsValid(CrowdHostActor) && CrowdHostActor->CitizenISM) CrowdHostActor->CitizenISM->MarkRenderStateDirty();
	if(bScopedPlanningAuditPending&&ScopedCrowdRefreshDepth==0)
	{bScopedPlanningAuditPending=false;AuditCitizenPlanning();}
}

bool UCrowdRenderSubsystem::SpawnCosmeticFx(EAftershockCosmeticFx Effect, FVector WorldLocation, float Scale)
{
	AAftershockCrowdHostActor* Host = GetOrCreateHostActor();
	return Host && Host->SpawnCosmeticFx(Effect, WorldLocation, Scale);
}

void UCrowdRenderSubsystem::UpdateEmotionBillboards()
{
	AAftershockCrowdHostActor* Host=IsValid(CrowdHostActor)?CrowdHostActor.Get():nullptr;
	if(!Host||!Host->EmotionISM)return;
	const bool bDrawEmotions = GetAftershockPresentationBudget(GetWorld()).Tier == EAftershockPresentationTier::Street;
	if (Host->EmotionISM->IsVisible() != bDrawEmotions) Host->EmotionISM->SetVisibility(bDrawEmotions);
	if (!bDrawEmotions) return;
	APlayerController* PC=GetWorld()?GetWorld()->GetFirstPlayerController():nullptr;
	APlayerCameraManager* Camera=PC?PC->PlayerCameraManager:nullptr;
	// A commandlet has no player camera. Do not invent view-facing status geometry.
	if(!Camera)
	{
		if(Host->EmotionISM->GetInstanceCount() > 0) Host->EmotionISM->ClearInstances();
		return;
	}
	const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const FIntRect Drawable=Terrain?GetAftershockDrawableCellBounds(*Terrain):FIntRect(0,0,0,0);
	const FVector CameraLocation=Camera->GetCameraLocation();
	const FVector Right=Camera->GetCameraRotation().RotateVector(FVector::RightVector);
	constexpr int32 MaxEmotionBillboards = 96;
	if (Host->EmotionISM->GetInstanceCount() != MaxEmotionBillboards)
	{
		Host->EmotionISM->ClearInstances();
		TArray<FTransform> InitialTransforms;
		InitialTransforms.Init(FTransform(FRotator::ZeroRotator, FVector(0,0,-100000.f), FVector::ZeroVector), MaxEmotionBillboards);
		Host->EmotionISM->AddInstances(InitialTransforms, false, true, false);
		for (int32 I = 0; I < MaxEmotionBillboards; ++I)
		{
			Host->EmotionISM->SetCustomDataValue(I, 0, 0.0f, false);
		}
		Host->EmotionISM->MarkRenderStateDirty();
	}
	EmotionPooledTransforms.SetNumUninitialized(MaxEmotionBillboards);
	int32 ActiveCount = 0;
	for(const FCitizenAgent& Agent:Agents)
	{
		if(Agent.ResidentID==INDEX_NONE||Agent.bInsideVehicle||IsCitizenIndoors(Agent,GetWorld()))continue;
		if(Terrain&&!Drawable.Contains(Terrain->WorldToGrid(Agent.CurrentLocation)))continue;
		int32 Icon=INDEX_NONE;
		if(Agent.bIsPanicking||Agent.Activity==EAftershockCitizenActivity::AwaitingShelter)Icon=4;
		else if(Agent.Health01<.65f)Icon=5;
		else if(Agent.Hunger01>.45f)Icon=2;
		else if(!Agent.bHomePowered)Icon=3;
		else if(Agent.Happiness01<.45f)Icon=1;
		else if(Agent.Happiness01>.72f)Icon=0;
		if(Icon==INDEX_NONE)continue;
		// Status is real, sampling is deterministic. Urgent statuses use 1/4,
		// ordinary happy/sad 1/16; bounded to 96 shared-atlas billboards.
		const uint32 Stable=static_cast<uint32>(Agent.ResidentID)*2654435761u;
		if((Stable%(Icon>=2?4u:16u))!=0)continue;
		const FVector At=Agent.CurrentLocation+FVector(0,0,250);
		const FVector ToCamera=(CameraLocation-At).GetSafeNormal();
		const FQuat Rotation=FRotationMatrix::MakeFromXZ(Right,ToCamera).ToQuat();
		const float Size=FMath::Clamp(FVector::Dist(CameraLocation,At)*.010f,95.f,175.f)/100.f;
		EmotionPooledTransforms[ActiveCount] = FTransform(Rotation, At, FVector(Size, Size, 1.f));
		Host->EmotionISM->SetCustomDataValue(ActiveCount, 0, static_cast<float>(Icon), false);
		++ActiveCount;
		if(ActiveCount>=MaxEmotionBillboards)break;
	}
	for(int32 I=ActiveCount; I<MaxEmotionBillboards; ++I)
	{
		EmotionPooledTransforms[I] = FTransform(FRotator::ZeroRotator, FVector(0,0,-100000.f), FVector::ZeroVector);
		Host->EmotionISM->SetCustomDataValue(I, 0, 0.f, false);
	}
	Host->EmotionISM->BatchUpdateInstancesTransforms(0, EmotionPooledTransforms, true, false, false);
	Host->EmotionISM->MarkRenderInstancesDirty();
}

void UCrowdRenderSubsystem::RebuildCitizenSpatialIndex() const
{
	CitizenSpatialBuckets.Reset();CitizenSpatialKeys.SetNum(Agents.Num());CitizenSpatialIncluded.Init(0,Agents.Num());
	for(int32 I=0;I<Agents.Num();++I)
	{
		const auto& Agent=Agents[I];CitizenSpatialKeys[I]=CitizenBucket(Agent.CurrentLocation);
		if(Agent.bInsideVehicle||IsCitizenIndoors(Agent,GetWorld()))continue;
		CitizenSpatialBuckets.FindOrAdd(CitizenSpatialKeys[I]).Add(I);CitizenSpatialIncluded[I]=1;
	}
	bCitizenSpatialDirty=false;
}

void UCrowdRenderSubsystem::UpdateCitizenSpatialEntry(int32 I) const
{
	if(bCitizenSpatialDirty||CitizenSpatialKeys.Num()!=Agents.Num()){RebuildCitizenSpatialIndex();return;}
	if(!Agents.IsValidIndex(I))return;
	const auto& Agent=Agents[I];const FIntPoint Key=CitizenBucket(Agent.CurrentLocation);
	const bool Included=!Agent.bInsideVehicle&&!IsCitizenIndoors(Agent,GetWorld());
	if(CitizenSpatialIncluded[I]&&(!Included||CitizenSpatialKeys[I]!=Key))
	{
		if(auto* Bucket=CitizenSpatialBuckets.Find(CitizenSpatialKeys[I]))
		{Bucket->RemoveSingleSwap(I,EAllowShrinking::No);if(Bucket->IsEmpty())CitizenSpatialBuckets.Remove(CitizenSpatialKeys[I]);}
		CitizenSpatialIncluded[I]=0;
	}
	if(Included&&!CitizenSpatialIncluded[I]){CitizenSpatialBuckets.FindOrAdd(Key).Add(I);CitizenSpatialIncluded[I]=1;}
	CitizenSpatialKeys[I]=Key;
}

bool UCrowdRenderSubsystem::IsCitizenSpaceClear(const FVector& From,const FVector& To,int32 IgnoredAgentIndex,bool bAllowExistingOverlap) const
{
	if(From.ContainsNaN()||To.ContainsNaN())return false;
	if(bCitizenSpatialDirty||CitizenSpatialKeys.Num()!=Agents.Num())RebuildCitizenSpatialIndex();
	constexpr float Diameter=CitizenBodyRadiusCm*2.f;
	const FIntPoint Min=CitizenBucket(From.ComponentMin(To)-FVector(Diameter,Diameter,0)),Max=CitizenBucket(From.ComponentMax(To)+FVector(Diameter,Diameter,0));
	if(Max.X-Min.X>8||Max.Y-Min.Y>8)return false;
	const FVector2D Delta(To-From);const double LengthSq=Delta.SizeSquared();int32 Checks=0;
	for(int32 Y=Min.Y;Y<=Max.Y;++Y)for(int32 X=Min.X;X<=Max.X;++X)
	{
		const auto* Bucket=CitizenSpatialBuckets.Find(FIntPoint(X,Y));if(!Bucket)continue;
		for(int32 J:*Bucket)
		{
			if(++Checks>CitizenAdmissionCheckBudget)return false; // A full neighbourhood never grants unverified admission.
			++SpacingStats.AdmissionChecks;
			if(J==IgnoredAgentIndex||!Agents.IsValidIndex(J))continue;
			const auto& Other=Agents[J];
			if(FMath::Abs(Other.CurrentLocation.Z-From.Z)>=175.f&&FMath::Abs(Other.CurrentLocation.Z-To.Z)>=175.f)continue;
			const FVector2D Relative(Other.CurrentLocation-From);
			const double Alpha=LengthSq>SMALL_NUMBER?FMath::Clamp(FVector2D::DotProduct(Relative,Delta)/LengthSq,0.,1.):0.;
			const double ClosestSq=(Relative-Delta*Alpha).SizeSquared();
			if(ClosestSq>=FMath::Square(Diameter))continue;
			// Old snapshots/test fixtures may already interpenetrate. Permit only
			// continuous outward repair, never a new pair or a deeper penetration.
			if(bAllowExistingOverlap&&Relative.SizeSquared()<FMath::Square(Diameter)
				&&(FVector2D(Other.CurrentLocation-To).SizeSquared()>Relative.SizeSquared()+.01)
				&&FVector2D::DotProduct(-Relative,Delta)>=-.001)continue;
			return false;
		}
	}
	return true;
}

bool UCrowdRenderSubsystem::CanAdmitCitizenAt(const FVector& Location,int32 IgnoredResidentID) const
{
	const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	if(!Terrain||!IsPublicWalkingCell(*Terrain,Terrain->WorldToGrid(Location)))return false;
	const int32 Ignored=IgnoredResidentID==INDEX_NONE?INDEX_NONE:Agents.IndexOfByPredicate([&](const auto& A){return A.ResidentID==IgnoredResidentID;});
	if(!IsCitizenSpaceClear(Location,Location,Ignored))return false;
	const auto* Transport=GetWorld()->GetSubsystem<UTransportSubsystem>();
	return !Transport||Transport->IsPedestrianSpaceClear(Location,Location,CitizenBodyRadiusCm,false);
}

bool UCrowdRenderSubsystem::IsVehicleSpaceClear(const FVector& From,float FromYaw,const FVector& To,float ToYaw,
	const FVector& Dimensions,int32 IgnoredResidentID,float CrossingLookaheadCm) const
{
	if(bCitizenSpatialDirty||CitizenSpatialKeys.Num()!=Agents.Num())RebuildCitizenSpatialIndex();
	const float Reach=Dimensions.Size2D()*.5f+CitizenBodyRadiusCm+16.f;
	const FVector Forward=FRotator(0,ToYaw,0).Vector(),Across(-Forward.Y,Forward.X,0);
	const FVector LookaheadEnd=To+Forward*FMath::Max(0.f,CrossingLookaheadCm);
	const FIntPoint Min=CitizenBucket(From.ComponentMin(To).ComponentMin(LookaheadEnd)-FVector(Reach,Reach,0)),Max=CitizenBucket(From.ComponentMax(To).ComponentMax(LookaheadEnd)+FVector(Reach,Reach,0));
	if(Max.X-Min.X>16||Max.Y-Min.Y>16)return false;
	int32 Checks=0;
	for(int32 Y=Min.Y;Y<=Max.Y;++Y)for(int32 X=Min.X;X<=Max.X;++X)
	{
		const auto* Bucket=CitizenSpatialBuckets.Find(FIntPoint(X,Y));if(!Bucket)continue;
		for(int32 J:*Bucket)
		{
			if(++Checks>256)return false;
			if(!Agents.IsValidIndex(J)||(IgnoredResidentID!=INDEX_NONE&&Agents[J].ResidentID==IgnoredResidentID))continue;
			const FCitizenAgent& Person=Agents[J];
			if(UTransportSubsystem::VehicleSweepIntersectsDisc(From,FromYaw,To,ToYaw,Dimensions,Person.CurrentLocation,Person.CurrentLocation,CitizenBodyRadiusCm))return false;
			if(CrossingLookaheadCm<=0.f||Person.MoveSpeed<=0.f||FMath::Abs(Person.CurrentLocation.Z-To.Z)>=175.f)continue;
			const FVector Walk=Person.TargetLocation-Person.CurrentLocation;
			if(Walk.SizeSquared2D()<1.f||FMath::Abs(FVector::DotProduct(Walk.GetSafeNormal2D(),Forward))>.7f)continue;
			const float Side=FVector::DotProduct(Person.CurrentLocation-To,Across),GoalSide=FVector::DotProduct(Person.TargetLocation-To,Across);
			const float LaneHalf=Dimensions.Y*.5f+CitizenBodyRadiusCm+8.f;
			const bool CrossesLane=(Side>LaneHalf&&GoalSide<LaneHalf)||(Side<-LaneHalf&&GoalSide>-LaneHalf)
				||FMath::Abs(Side)<=LaneHalf;
			const float Ahead=FVector::DotProduct(Person.CurrentLocation-To,Forward);
			// Stop before the pedestrian's line *and* the pedestrian-side .4s
			// vehicle forecast. Waiting nose-to-kerb made both safe queries reject
			// forever. Only an actual transverse walking intention reserves space.
			if(CrossesLane&&Ahead>0.f&&Ahead<=Dimensions.X*.5f+CrossingLookaheadCm+CitizenBodyRadiusCm+64.f)return false;
		}
	}
	return true;
}

bool UCrowdRenderSubsystem::NormalizeDeferredDepartureAnchor(FCitizenAgent& Agent,TFunctionRef<bool(int32,const FVector&)> IsKnownIndoorAnchor,FText& Error)
{
	if(!Agent.bWaitingForOutdoorSpace)return true;
	const auto Fail=[&](){Error=NSLOCTEXT("Aftershock","InvalidDeferredDeparture","Bekleyen vatandaşın gerçek bina çıkışı geçersiz.");return false;};
	if(Agent.bInsideVehicle||Agent.ResidentID==INDEX_NONE||Agent.OutdoorDepartureLocation.ContainsNaN())return Fail();
	if(Agent.OutdoorDepartureBuildingID==INDEX_NONE)
	{
		// Tagged legacy saves have no explicit source. Infer it only from a
		// matching old indoor anchor, never from the newly selected destination.
		const auto Infer=[&](int32 ID,const FVector& Anchor)
		{if(ID==INDEX_NONE||FVector::DistSquared2D(Agent.CurrentLocation,Anchor)>FMath::Square(64.f)||!IsKnownIndoorAnchor(ID,Agent.CurrentLocation))return false;Agent.OutdoorDepartureBuildingID=ID;Agent.OutdoorDepartureLocation=Agent.CurrentLocation;return true;};
		if(!Infer(Agent.HomeBuildingID,Agent.HomeLocation)&&!Infer(Agent.WorkBuildingID,Agent.WorkLocation)&&!Infer(Agent.FoodBuildingID,Agent.FoodLocation)&&!Infer(Agent.HealthBuildingID,Agent.HealthLocation)
			&&!(Agent.bEmergencyPlaceReserved&&Infer(Agent.EmergencyBuildingID,Agent.ShelterLocation)))return Fail();
	}
	return IsKnownIndoorAnchor(Agent.OutdoorDepartureBuildingID,Agent.OutdoorDepartureLocation)
		&&FVector::DistSquared2D(Agent.CurrentLocation,Agent.OutdoorDepartureLocation)<=FMath::Square(64.f)?true:Fail();
}

bool UCrowdRenderSubsystem::ReconcileCitizenPlacements(TArray<FCitizenAgent>& InOutAgents,FText& Error,int32& OutMoved,float& OutMaxDistanceCm) const
{
	OutMoved=0;OutMaxDistanceCm=0.f;Error=FText::GetEmpty();
	const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const auto* Roads=GetWorld()?GetWorld()->GetSubsystem<URoadNetworkSubsystem>():nullptr;
	const auto* Transport=GetWorld()?GetWorld()->GetSubsystem<UTransportSubsystem>():nullptr;
	if(!Terrain||!Roads||InOutAgents.Num()>MaxVisualCitizens)return false;
	TArray<FCitizenAgent> Candidate=InOutAgents;
	TMap<FIntPoint,TArray<int32>> Occupied;TSet<FIntPoint> FullCells;
	TArray<int32> Order;for(int32 I=0;I<Candidate.Num();++I)Order.Add(I);
	Order.Sort([&](int32 A,int32 B){const int32 AI=Candidate[A].ResidentID,BI=Candidate[B].ResidentID;return AI!=BI?AI<BI:A<B;});
	const auto Clear=[&](const FVector& At)
	{
		if(At.ContainsNaN()||!IsCitizenGeometryCell(*Terrain,Terrain->WorldToGrid(At)))return false;
		const FIntPoint Min=CitizenBucket(At-FVector(64,64,0)),Max=CitizenBucket(At+FVector(64,64,0));int32 Checks=0;
		for(int32 Y=Min.Y;Y<=Max.Y;++Y)for(int32 X=Min.X;X<=Max.X;++X)if(const auto* Bucket=Occupied.Find(FIntPoint(X,Y)))for(int32 Other:*Bucket)
		{
			if(++Checks>CitizenAdmissionCheckBudget)return false;
			if(FMath::Abs(Candidate[Other].CurrentLocation.Z-At.Z)<175.f&&FVector::DistSquared2D(Candidate[Other].CurrentLocation,At)<FMath::Square(64.f))return false;
		}
		return !Transport||Transport->IsPedestrianSpaceClear(At,At,32.f,false);
	};
	for(int32 I:Order)
	{
		auto& Agent=Candidate[I];if(Agent.bInsideVehicle||IsCitizenIndoors(Agent,GetWorld()))continue;
		if(Agent.bWaitingForOutdoorSpace){Agent.bWaitingForOutdoorSpace=false;Agent.OutdoorDepartureBuildingID=INDEX_NONE;Agent.OutdoorDepartureLocation=FVector::ZeroVector;}
		const FVector Original=Agent.CurrentLocation;
		if(!Clear(Original))
		{
			const FIntPoint Start=Terrain->WorldToGrid(Original);TArray<FIntPoint,TInlineAllocator<128>> Queue;TSet<FIntPoint> Seen;
			Queue.Add(Start);Seen.Add(Start);bool Found=false;
			FAftershockPassengerTrip Trip;const bool MustStayAtPickup=Transport&&Agent.TransportTripID!=INDEX_NONE&&Transport->GetTrip(Agent.TransportTripID,Trip)&&Trip.State==EAftershockPassengerState::Waiting;
			for(int32 Read=0;Read<Queue.Num()&&Read<128&&!Found;++Read)
			{
				const FIntPoint Coord=Queue[Read];
				if(!FullCells.Contains(Coord)&&Roads->IsPublicWalkingCell(Coord)&&(!MustStayAtPickup||Coord==Trip.PickupRoadCell))
				{
					TArray<FVector,TInlineAllocator<128>> Slots;const FVector Centre=Terrain->GridToWorld(Coord);
					const int32 Span=FMath::Clamp(FMath::FloorToInt((Terrain->CellSize*.5f-32.f)/80.f),0,12);
					const FGridCell Cell=Terrain->GetCell(Coord);
					if(Cell.bHasRoad)
					{
						const bool AlongX=Cell.BridgeID!=INDEX_NONE?Cell.BridgeAxis==1:(Roads->IsRoadTraversable(Coord+FIntPoint(1,0))||Roads->IsRoadTraversable(Coord-FIntPoint(1,0)));
						const bool AlongY=Cell.BridgeID!=INDEX_NONE?Cell.BridgeAxis!=1:(Roads->IsRoadTraversable(Coord+FIntPoint(0,1))||Roads->IsRoadTraversable(Coord-FIntPoint(0,1)));
						for(int32 Axis=0;Axis<2;++Axis)
						{
							if(Axis==0&&!AlongX)continue;if(Axis==1&&!AlongY)continue;
							for(float Side:{-1.f,1.f})for(int32 Row=0;Row<(Terrain->CellSize>=900.f?2:1);++Row)for(int32 Along=-Span;Along<=Span;++Along)
							{
								const float Across=Side*(Terrain->CellSize*(Terrain->CellSize>=900.f?.40f:.42f)+(Terrain->CellSize>=900.f?(Row==0?-40.f:40.f):0.f));
								Slots.Add(Centre+(Axis==0?FVector(Along*80.f,Across,0):FVector(Across,Along*80.f,0)));
							}
						}
					}
					else for(int32 Y=-Span;Y<=Span;++Y)for(int32 X=-Span;X<=Span;++X)Slots.Add(Centre+FVector(X*80.f,Y*80.f,0));
					Slots.Sort([&](const FVector& A,const FVector& B){const double AD=FVector::DistSquared2D(A,Original),BD=FVector::DistSquared2D(B,Original);return AD!=BD?AD<BD:(A.X!=B.X?A.X<B.X:A.Y<B.Y);});
					for(FVector Slot:Slots)
					{
						Slot.Z=(Cell.bHasRoad?Roads->SampleRoadSurfaceElevation(Slot):VisibleGroundHeight(*Terrain,Slot))+CitizenZOffset;
						if(Terrain->WorldToGrid(Slot)==Coord&&Clear(Slot)){Agent.CurrentLocation=Slot;Found=true;break;}
					}
					if(!Found)FullCells.Add(Coord);
				}
				for(FIntPoint Offset:CitizenNeighborOffsets)
				{
					const FIntPoint Next=Coord+Offset;
					if(Queue.Num()>=128||Seen.Contains(Next)||!Terrain->IsValidCoord(Next)||!Roads->IsPublicWalkingCell(Next))continue;
					if(!Roads->CanTraverseWalkingEdge(Coord,Next)&&!(Coord==Start&&CanTakeCitizenEscapeStep(*Terrain,Coord,Next)))continue;
					Seen.Add(Next);Queue.Add(Next);
				}
			}
			if(!Found){Error=NSLOCTEXT("Aftershock","CrowdPhysicalCapacity","Kayıtlı yayalar için erişilebilir, çakışmasız alan yok; mevcut şehir değiştirilmedi.");return false;}
			++OutMoved;OutMaxDistanceCm=FMath::Max(OutMaxDistanceCm,static_cast<float>(FVector::Dist2D(Original,Agent.CurrentLocation)));Agent.WalkVelocityCmPerSec=FVector2D::ZeroVector;
		}
		Occupied.FindOrAdd(CitizenBucket(Agent.CurrentLocation)).Add(I);
	}
	InOutAgents=MoveTemp(Candidate);return true;
}

bool UCrowdRenderSubsystem::TryAdmitOutdoorDeparture(FCitizenAgent& Agent)
{
	if(!Agent.bWaitingForOutdoorSpace)return true;
	if(!CanAdmitCitizenAt(Agent.CurrentLocation,Agent.ResidentID)){++SpacingStats.DeferredEmergences;return false;}
	Agent.bWaitingForOutdoorSpace=false;
	Agent.OutdoorDepartureBuildingID=INDEX_NONE;Agent.OutdoorDepartureLocation=FVector::ZeroVector;
	const int32 Index=Agents.IndexOfByPredicate([&](const auto& A){return &A==&Agent;});UpdateCitizenSpatialEntry(Index);
	return true;
}

void UCrowdRenderSubsystem::ResolveCrowdSpacing(const UTerrainGridSubsystem& Terrain)
{
	CSV_SCOPED_TIMING_STAT(AftershockCrowd, LocalSeparation);
	const int32 AdmissionChecks=SpacingStats.AdmissionChecks,BlockedMoves=SpacingStats.BlockedMoves,DeferredEmergences=SpacingStats.DeferredEmergences;
	SpacingStats=FAftershockCrowdSpacingStats();
	SpacingStats.AdmissionChecks=AdmissionChecks;SpacingStats.BlockedMoves=BlockedMoves;SpacingStats.DeferredEmergences=DeferredEmergences;
	if(bCitizenSpatialDirty||CitizenSpatialKeys.Num()!=Agents.Num())RebuildCitizenSpatialIndex();
	const URoadNetworkSubsystem* Roads=GetWorld()?GetWorld()->GetSubsystem<URoadNetworkSubsystem>():nullptr;
	constexpr float BucketSize=128.f, SeparationCm=80.f;
	constexpr int32 MaxChecksPerAgentPerPass=32;
	const auto IsOutdoor=[this](const FCitizenAgent& Agent) { return !Agent.bInsideVehicle&&!IsCitizenIndoors(Agent,GetWorld()); };
	const auto Bucket=[](const FVector& At) { return FIntPoint(FMath::FloorToInt(At.X/BucketSize),FMath::FloorToInt(At.Y/BucketSize)); };
	// No canonical edits or timer dispatch occurs inside these two synchronous
	// passes. Reuse the exact native answer for repeated cells/edges only until
	// this callback ends; the next update starts with a completely fresh cache.
	if (SpacingWalkabilityCache.Num() != Terrain.Cells.Num())
	{
		SpacingWalkabilityCache.Init(-1, Terrain.Cells.Num());
	}
	else
	{
		FMemory::Memset(SpacingWalkabilityCache.GetData(), -1, SpacingWalkabilityCache.Num() * sizeof(int8));
	}
	SpacingWalkingEdgeCache.Reset();
	const auto CanStand=[&](FIntPoint Coord)
	{
		const int32 Index=Terrain.CoordToIndex(Coord);
		if(!SpacingWalkabilityCache.IsValidIndex(Index))return false;
		int8& Cached=SpacingWalkabilityCache[Index];
		if(Cached<0)Cached=(Roads?Roads->IsPublicWalkingCell(Coord):IsPublicWalkingCell(Terrain,Coord))?1:0;
		return Cached!=0;
	};
	const auto CanCross=[&](FIntPoint From,FIntPoint To)
	{
		if(!Roads)return FMath::Abs(From.X-To.X)+FMath::Abs(From.Y-To.Y)==1;
		const int32 A=Terrain.CoordToIndex(From),B=Terrain.CoordToIndex(To);
		if(A==INDEX_NONE||B==INDEX_NONE)return false;
		const uint64 Key=(static_cast<uint64>(static_cast<uint32>(A))<<32)|static_cast<uint32>(B);
		if(const bool* Cached=SpacingWalkingEdgeCache.Find(Key))return *Cached;
		const bool Allowed=Roads->CanTraverseWalkingEdge(From,To);SpacingWalkingEdgeCache.Add(Key,Allowed);return Allowed;
	};
	for (const FCitizenAgent& Agent:Agents)
	{
		if (Agent.bInsideVehicle) ++SpacingStats.VehiclePassengers;
		else if (IsCitizenIndoors(Agent,GetWorld())) ++SpacingStats.IndoorAgents;
		else ++SpacingStats.OutdoorAgents;
	}
	// Two bounded relaxation passes, using linked indices rather than allocating
	// a dynamic array for every spatial bucket on every80ms callback.
	SpacingRemainingCorrection.SetNumUninitialized(Agents.Num(), EAllowShrinking::No);
	for (int32 Pass=0;Pass<2;++Pass)
	{
		SpacingBucketHeads.Reset();SpacingNextIndices.Init(INDEX_NONE,Agents.Num());
		SpacingBucketCounts.Reset();
		for (int32 I=0;I<Agents.Num();++I)
		{
			SpacingRemainingCorrection[I]=8.f;
			if (!IsOutdoor(Agents[I])) continue;
			const FIntPoint Key=Bucket(Agents[I].CurrentLocation);
			int32& Head=SpacingBucketHeads.FindOrAdd(Key,INDEX_NONE);
			SpacingNextIndices[I]=Head;Head=I;
			SpacingStats.MaxBucketOccupancy=FMath::Max(SpacingStats.MaxBucketOccupancy,++SpacingBucketCounts.FindOrAdd(Key));
		}
		const auto TryCorrection=[&](int32 I,FVector Delta)
		{
			if (Delta.SizeSquared2D()<.0001f || SpacingRemainingCorrection[I]<=0.f) return;
			FCitizenAgent& Agent=Agents[I];
			Delta=Delta.GetClampedToMaxSize2D(SpacingRemainingCorrection[I]);
			const FVector Candidate=Agent.CurrentLocation+Delta;
			const FIntPoint From=Terrain.WorldToGrid(Agent.CurrentLocation),To=Terrain.WorldToGrid(Candidate);
			bool bSafe=CanStand(To)&&IsCitizenSpaceClear(Agent.CurrentLocation,Candidate,I,true);
			if(const auto* Transport=GetWorld()?GetWorld()->GetSubsystem<UTransportSubsystem>():nullptr)bSafe&=Transport->IsPedestrianSpaceClear(Agent.CurrentLocation,Candidate,CitizenBodyRadiusCm,false);
			if (From!=To) bSafe &= CanCross(From,To);
			if (!bSafe) { ++SpacingStats.RejectedUnsafeCorrections;return; }
			Agent.CurrentLocation=Candidate;UpdateCitizenSpatialEntry(I);SpacingRemainingCorrection[I]-=Delta.Size2D();++SpacingStats.AcceptedCorrections;
		};
		for (int32 I=0;I<Agents.Num();++I)
		{
			if (!IsOutdoor(Agents[I])) continue;
			const FIntPoint Centre=Bucket(Agents[I].CurrentLocation);
			int32 Checked=0;
			for (int32 Y=-1;Y<=1 && Checked<MaxChecksPerAgentPerPass;++Y)
			for (int32 X=-1;X<=1 && Checked<MaxChecksPerAgentPerPass;++X)
			{
				const int32* Head=SpacingBucketHeads.Find(Centre+FIntPoint(X,Y));
				for (int32 J=Head?*Head:INDEX_NONE;J!=INDEX_NONE && Checked<MaxChecksPerAgentPerPass;J=SpacingNextIndices[J])
				{
					// Count every visited link, including self/already-processed pairs.
					// Otherwise a dense bucket could hide an unbounded scan behind a
					// superficially bounded pair counter.
					++Checked;++SpacingStats.NeighbourChecks;
					if (J<=I) continue;
					if (FMath::Abs(Agents[I].CurrentLocation.Z-Agents[J].CurrentLocation.Z)>100.f) continue;
					FVector Difference=Agents[I].CurrentLocation-Agents[J].CurrentLocation;Difference.Z=0.f;
					const float Distance=Difference.Size2D();
					if (Distance>=SeparationCm) continue;
					FVector Away;
					if (Distance>.01f) Away=Difference/Distance;
					else
					{
						const uint32 Hash=HashCombineFast(GetTypeHash(Agents[I].RandomSeed),GetTypeHash(Agents[J].RandomSeed));
						const float Angle=(Hash%4096)*2.f*PI/4096.f;
						Away=FVector(FMath::Cos(Angle),FMath::Sin(Angle),0.f);
					}
					const float HalfOverlap=(SeparationCm-Distance)*.5f;
					TryCorrection(I,Away*HalfOverlap);TryCorrection(J,-Away*HalfOverlap);
				}
			}
		}
	}
	// Diagnostic samples use the same bounded neighbourhood search. This counts
	// observed close pairs, never claims an exhaustive all-pairs safety proof.
	for (int32 I=0;I<Agents.Num();++I)
	{
		if (!IsOutdoor(Agents[I])) continue;
		const FIntPoint Centre=Bucket(Agents[I].CurrentLocation);int32 Checked=0;
		for (int32 Y=-1;Y<=1 && Checked<32;++Y) for(int32 X=-1;X<=1 && Checked<32;++X)
		{
			const int32* Head=SpacingBucketHeads.Find(Centre+FIntPoint(X,Y));
			for(int32 J=Head?*Head:INDEX_NONE;J!=INDEX_NONE && Checked<32;J=SpacingNextIndices[J])
			{
				++Checked;if(J<=I)continue;
				SpacingStats.ObservedClosePairs+=FMath::Abs(Agents[I].CurrentLocation.Z-Agents[J].CurrentLocation.Z)<100.f
					&& FVector::DistSquared2D(Agents[I].CurrentLocation,Agents[J].CurrentLocation)<FMath::Square(45.f)?1:0;
			}
		}
	}
}

bool UCrowdRenderSubsystem::CanMoveCitizenSegment(const UTerrainGridSubsystem& Terrain,const FVector& From,const FVector& To) const
{
	if(From.ContainsNaN()||To.ContainsNaN()||Terrain.CellSize<=0.f)return false;
	FIntPoint Cell=Terrain.WorldToGrid(From);const FIntPoint End=Terrain.WorldToGrid(To);
	if(!IsCitizenGeometryCell(Terrain,Cell)||!IsCitizenGeometryCell(Terrain,End))return false;
	if(Cell==End)return true;
	const URoadNetworkSubsystem* Roads=GetWorld()?GetWorld()->GetSubsystem<URoadNetworkSubsystem>():nullptr;
	const FIntPoint Initial=Cell;
	const auto Cross=[&](FIntPoint A,FIntPoint B)
	{
		return (Roads&&Roads->CanTraverseWalkingEdge(A,B))
			||(A==Initial&&!IsPublicWalkingCell(Terrain,A)&&IsPublicWalkingCell(Terrain,B)&&CanTakeCitizenEscapeStep(Terrain,A,B));
	};
	const FVector Delta=To-From;
	const int32 StepX=Delta.X>0.?1:(Delta.X<0.?-1:0),StepY=Delta.Y>0.?1:(Delta.Y<0.?-1:0);
	const double Huge=1.e30;
	double TX=StepX?(Terrain.GridOrigin.X+(Cell.X+(StepX>0?1:0))*Terrain.CellSize-From.X)/Delta.X:Huge;
	double TY=StepY?(Terrain.GridOrigin.Y+(Cell.Y+(StepY>0?1:0))*Terrain.CellSize-From.Y)/Delta.Y:Huge;
	const double DX=StepX?Terrain.CellSize/FMath::Abs(Delta.X):Huge,DY=StepY?Terrain.CellSize/FMath::Abs(Delta.Y):Huge;
	const int32 Budget=FMath::Abs(End.X-Cell.X)+FMath::Abs(End.Y-Cell.Y)+2;
	// Supercover traversal checks the actual swept segment after separation,
	// including both sides of an exact corner. Endpoint-only tests miss river wedges.
	for(int32 I=0;I<Budget&&Cell!=End;++I)
	{
		if(FMath::Abs(TX-TY)<1.e-9)
		{
			const FIntPoint X=Cell+FIntPoint(StepX,0),Y=Cell+FIntPoint(0,StepY),N=Cell+FIntPoint(StepX,StepY);
			if(!Cross(Cell,X)||!Cross(Cell,Y)||!Cross(X,N)||!Cross(Y,N))return false;
			Cell=N;TX+=DX;TY+=DY;
		}
		else if(TX<TY){const FIntPoint N=Cell+FIntPoint(StepX,0);if(!Cross(Cell,N))return false;Cell=N;TX+=DX;}
		else{const FIntPoint N=Cell+FIntPoint(0,StepY);if(!Cross(Cell,N))return false;Cell=N;TY+=DY;}
	}
	return Cell==End;
}

void UCrowdRenderSubsystem::RebuildCitizenInstanceTransforms()
{
	AAftershockCrowdHostActor* Host=GetOrCreateHostActor();if(!Host||!Host->CitizenISM)return;
	const FAftershockPresentationBudget Budget = GetAftershockPresentationBudget(GetWorld());
	if (Host->CitizenISM->IsVisible() != Budget.bDrawCitizens) Host->CitizenISM->SetVisibility(Budget.bDrawCitizens);
	const int32 LODCount = Host->CitizenISM->GetStaticMesh() ? Host->CitizenISM->GetStaticMesh()->GetNumLODs() : 1;
	const int32 ForcedLOD = FMath::Min(Budget.PropLODFloor, LODCount);
	if (Host->CitizenISM->ForcedLodModel != ForcedLOD) Host->CitizenISM->SetForcedLodModel(ForcedLOD);
	if (!Budget.bDrawCitizens && Host->CitizenISM->GetInstanceCount() == Agents.Num())
	{
		for (int32 I = 0; I < Agents.Num(); ++I) Agents[I].ISMIndex = I;
		bCitizenSpatialDirty = true;
		return; // Movement/admission/schedules still ran; only GPU submission is skipped.
	}
	const FVector Scale=CitizenRenderScale(Host->CitizenISM->GetStaticMesh());
	const float Pivot=-CitizenSourceBounds(Host->CitizenISM->GetStaticMesh()).Min.Z*Scale.Z;
	const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const FIntRect Drawable=Terrain?GetAftershockDrawableCellBounds(*Terrain):FIntRect(0,0,0,0);
	UpdatedTransforms.SetNumUninitialized(Agents.Num());
	for(int32 I=0;I<Agents.Num();++I)
	{
		FCitizenAgent& Agent=Agents[I];Agent.ISMIndex=I;
		const bool Outside=Terrain&&!Drawable.Contains(Terrain->WorldToGrid(Agent.CurrentLocation));
		UpdatedTransforms[I]=FTransform(FRotator(0,Agent.FacingYaw,0),Agent.CurrentLocation+FVector(0,0,Pivot),(Agent.bInsideVehicle||IsCitizenIndoors(Agent,GetWorld())||Outside)?FVector::ZeroVector:Scale);
	}
	if(Host->CitizenISM->GetInstanceCount()!=Agents.Num())
	{
		Host->CitizenISM->ClearInstances();Host->CitizenISM->AddInstances(UpdatedTransforms,false,true,false);
		for(const FCitizenAgent& Agent:Agents)SetCitizenInstanceColor(Agent);
		Host->CitizenISM->MarkRenderStateDirty();
	}
	else if(!UpdatedTransforms.IsEmpty())Host->CitizenISM->BatchUpdateInstancesTransforms(0,UpdatedTransforms,true,false,false);
	const float Now=GetWorld()?GetWorld()->GetTimeSeconds():0.f;
	for(const FCitizenAgent& Agent:Agents)
	{
		const bool bWalking=Agent.MoveSpeed>0.f&&!Agent.bInsideVehicle&&!IsCitizenIndoors(Agent,GetWorld());
		Host->CitizenISM->SetCustomDataValue(Agent.ISMIndex,3,Agent.WalkingPhaseRadians,false);
		Host->CitizenISM->SetCustomDataValue(Agent.ISMIndex,4,bWalking?Agent.WalkVelocityCmPerSec.Size():0.f,false);
		Host->CitizenISM->SetCustomDataValue(Agent.ISMIndex,5,Now,false);
		Host->CitizenISM->SetCustomDataValue(Agent.ISMIndex,6,Agent.bIsPanicking?1.f:0.f,false);
	}
	Host->CitizenISM->MarkRenderInstancesDirty();
	bCitizenSpatialDirty=true;
}

void UCrowdRenderSubsystem::CaptureSaveState(FAftershockCrowdSaveState& OutState) const
{
	OutState=FAftershockCrowdSaveState();
	if(bSyntheticRenderProfileActive)return;
	OutState.Agents=Agents;OutState.SimulationSeed=SimulationSeed;OutState.bPanic=bIsPanicModeActive;
    if(IncrementalNormalRoutes.IsValid())for(auto& A:OutState.Agents)
    {
        FVector Destination;bool Keep=false;
        if(IncrementalNormalRoutes->FindRequest(NormalRouteAgentKey(A),Destination,Keep)&&!Keep)
        {
            A.RouteGridCoords.Reset();A.NextRouteCoordIndex=0;A.TargetLocation=A.CurrentLocation;A.FinalDestinationLocation=Destination;A.MoveSpeed=0.f;A.bRouteReachesDestination=false;
            if(A.TransportTripID==INDEX_NONE){A.Activity=EAftershockCitizenActivity::WaitingForRoute;A.ScheduleSecondsRemaining=0.f;}
            // WalkingToTransport keeps its real saved TripID; restore resumes
            // that pickup request rather than creating/charging another trip.
        }
    }
	OutState.EmergencyDisasterType=EmergencyDisasterType;
	OutState.RemainingEmergencySeconds=GetWorld()?FMath::Max(0.f,GetWorld()->GetTimerManager().GetTimerRemaining(DisasterVisualCompletionHandle)):0.f;
}

bool UCrowdRenderSubsystem::RestoreSaveState(const FAftershockCrowdSaveState& State,FText& OutError)
{
	const UTerrainGridSubsystem* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const UCityLifeSubsystem* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr;
	TSet<int32> Seen;
	TArray<FCitizenAgent> Reconciled=State.Agents;
	const auto* Buildings=GetWorld()?GetWorld()->GetSubsystem<UBuildingManagerSubsystem>():nullptr;
	for(auto& Agent:Reconciled)if(!NormalizeDeferredDepartureAnchor(Agent,[&](int32 ID,const FVector& Location)
	{FBuildingInstance B;if(!Terrain||!Buildings||!Buildings->GetBuildingByID(ID,B)||!B.BuildingData||B.BuildingData->Type==EBuildingType::GreenSpace||B.BuildingData->Type==EBuildingType::BusStop)return false;const FIntPoint Source=Terrain->WorldToGrid(Location);return Terrain->IsValidCoord(Source)&&FMath::Abs(Source.X-B.GridCoords.X)+FMath::Abs(Source.Y-B.GridCoords.Y)<=3;},OutError))return false;
	bool bValid=Terrain&&State.Agents.Num()<=MaxVisualCitizens&&FMath::IsFinite(State.RemainingEmergencySeconds)
		&& State.RemainingEmergencySeconds>=0.f&&State.RemainingEmergencySeconds<=3600.f
		&& static_cast<uint8>(State.EmergencyDisasterType)<=static_cast<uint8>(EDisasterType::Landslide);
	if(Life&&Life->IsCityInitialized())bValid &= State.Agents.Num()==FMath::Min(MaxVisualCitizens,Life->GetState().LivingPopulation);
	for(const FCitizenAgent& Agent:Reconciled)
	{
		bValid &= !Agent.CurrentLocation.ContainsNaN()&&!Agent.TargetLocation.ContainsNaN()&&!Agent.FinalDestinationLocation.ContainsNaN()
			&& !Agent.HomeLocation.ContainsNaN()&&!Agent.WorkLocation.ContainsNaN()&&!Agent.LeisureLocation.ContainsNaN()&&!Agent.ShelterLocation.ContainsNaN()
			&& !Agent.FoodLocation.ContainsNaN()&&!Agent.HealthLocation.ContainsNaN()&&!Agent.TransportFinalDestination.ContainsNaN()
			&& FMath::IsFinite(Agent.MoveSpeed)&&Agent.MoveSpeed>=0.f&&Agent.MoveSpeed<=1000.f
			&& FMath::IsFinite(Agent.ScheduleSecondsRemaining)&&Agent.ScheduleSecondsRemaining>=0.f&&Agent.ScheduleSecondsRemaining<=3600.f
			&& FMath::IsFinite(Agent.FoodVisitCooldown)&&Agent.FoodVisitCooldown>=0.f&&Agent.FoodVisitCooldown<=3600.f
			&& FMath::IsFinite(Agent.HealthVisitCooldown)&&Agent.HealthVisitCooldown>=0.f&&Agent.HealthVisitCooldown<=3600.f
			&& FMath::IsFinite(Agent.Hunger01)&&Agent.Hunger01>=0.f&&Agent.Hunger01<=1.f
			&& FMath::IsFinite(Agent.Health01)&&Agent.Health01>=0.f&&Agent.Health01<=1.f
			&& FMath::IsFinite(Agent.Happiness01)&&Agent.Happiness01>=0.f&&Agent.Happiness01<=1.f
			&& FMath::IsFinite(Agent.FacingYaw)&&FMath::IsFinite(Agent.WalkVelocityCmPerSec.X)&&FMath::IsFinite(Agent.WalkVelocityCmPerSec.Y)
			&& FMath::IsFinite(Agent.WalkingPhaseRadians)&&Agent.WalkingPhaseRadians>=0.f&&Agent.WalkingPhaseRadians<=2.f*PI
			&& static_cast<uint8>(Agent.Activity)<=static_cast<uint8>(EAftershockCitizenActivity::Displaced)
			&& static_cast<uint8>(Agent.ActivityAfterArrival)<=static_cast<uint8>(EAftershockCitizenActivity::Displaced)
			&& static_cast<uint8>(Agent.TransportArrivalActivity)<=static_cast<uint8>(EAftershockCitizenActivity::Displaced)
			&& Agent.NextRouteCoordIndex>=0&&Agent.NextRouteCoordIndex<=Agent.RouteGridCoords.Num()
			&& (!Agent.bInsideVehicle||Agent.TransportTripID!=INDEX_NONE)
			&& !Agent.OutdoorDepartureLocation.ContainsNaN()
			&& (!Terrain||Agent.RouteGridCoords.Num()<=Terrain->Cells.Num()+1)
			&& (!Terrain||Terrain->IsValidCoord(Terrain->WorldToGrid(Agent.CurrentLocation)));
		for(FIntPoint Coord:Agent.RouteGridCoords)bValid &= Terrain&&Terrain->IsValidCoord(Coord);
		if(Agent.ResidentID!=INDEX_NONE)
		{
			FCityResidentRecord Resident;
			bValid &= !Seen.Contains(Agent.ResidentID)&&Life&&Life->GetResident(Agent.ResidentID,Resident)&&Resident.bAlive;
			Seen.Add(Agent.ResidentID);
		}
		else if(Life&&Life->IsCityInitialized())bValid=false;
	}
	if(!bValid){OutError=NSLOCTEXT("Aftershock","InvalidSavedCrowd","Vatandaş kaydı geçersiz veya kanonik nüfusla uyuşmuyor.");return false;}
	int32 Migrated=0;float MaximumMove=0.f;
	if(!ReconcileCitizenPlacements(Reconciled,OutError,Migrated,MaximumMove))return false;
	GetWorld()->GetTimerManager().ClearTimer(DisasterVisualCompletionHandle);
    IncrementalNormalRoutes.Reset();
	Agents=MoveTemp(Reconciled);bCitizenSpatialDirty=true;SimulationSeed=State.SimulationSeed;bIsPanicModeActive=State.bPanic;EmergencyDisasterType=State.EmergencyDisasterType;
	bSyntheticRenderProfileActive=false;bUpdatingAgents=false;bCityLifeSyncPending=false;RoutingRefreshSeconds=0.f;
	RebuildDestinationCaches();
	for(int32 I=0;I<Agents.Num();++I)if(!Agents[I].CurrentLocation.Equals(State.Agents[I].CurrentLocation,.01f))
	{
		if(Agents[I].MoveSpeed>0.f){FAftershockPassengerTrip Trip;const auto* Transport=GetWorld()->GetSubsystem<UTransportSubsystem>();const FVector Goal=Agents[I].TransportTripID!=INDEX_NONE&&Transport&&Transport->GetTrip(Agents[I].TransportTripID,Trip)&&Trip.State==EAftershockPassengerState::WalkingToPickup?Trip.PickupLocation:Agents[I].FinalDestinationLocation;BuildSafeRoute(Agents[I],Goal);}
		else{Agents[I].TargetLocation=Agents[I].CurrentLocation;Agents[I].FinalDestinationLocation=Agents[I].CurrentLocation;}
	}
    for(auto& A:Agents)if(!bIsPanicModeActive&&A.TransportTripID!=INDEX_NONE&&A.Activity==EAftershockCitizenActivity::WalkingToTransport&&A.MoveSpeed<=0.f&&A.RouteGridCoords.IsEmpty())
    {FAftershockPassengerTrip Trip;if(const auto* Transport=GetWorld()->GetSubsystem<UTransportSubsystem>();Transport&&Transport->GetTrip(A.TransportTripID,Trip)&&Trip.State==EAftershockPassengerState::WalkingToPickup)BuildSafeRoute(A,Trip.PickupLocation);}
	RebuildCitizenInstanceTransforms();UpdateEmotionBillboards();
	if(Migrated>0)UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CROWD_VISUAL_RECONCILED reason=CoveredLoad moved=%d maxDistanceCm=%.2f residentIDsPreserved=1 newNPCs=0"),Migrated,MaximumMove);
	if(State.bPanic&&State.RemainingEmergencySeconds>0.f)
	{
		// The authoritative result has already been restored. Finish only its
		// visual phase; never trigger damage, deaths, aid or rewards again.
		GetWorld()->GetTimerManager().SetTimer(DisasterVisualCompletionHandle,FTimerDelegate::CreateWeakLambda(this,[this]()
		{
			FDisasterReport Report;Report.DisasterType=EmergencyDisasterType;ApplyDisasterCompletedVisuals(Report);
		}),State.RemainingEmergencySeconds,false);
	}
	return true;
}

void UCrowdRenderSubsystem::UpdateCrowdPositions()
{
    FAftershockRoadCpuScope Cpu(TEXT("Crowd.UpdateCrowdPositions"));
    FAftershockRoadGoalCpuFrame GoalCpu(TEXT("Crowd.UpdateCrowdPositions"));
	CSV_SCOPED_TIMING_STAT(AftershockCrowd, UpdatePositions);
	const double ProfileStarted=bSyntheticRenderProfileActive?FPlatformTime::Seconds():0.;
	if (Agents.Num() == 0)
	{
		if(IsValid(CrowdHostActor)&&CrowdHostActor->EmotionISM)CrowdHostActor->EmotionISM->ClearInstances();
		return;
	}

	AAftershockCrowdHostActor* Host = GetOrCreateHostActor();
	if (!Host || !Host->CitizenISM)
	{
		return;
	}

	bUpdatingAgents=true;
	SpacingStats=FAftershockCrowdSpacingStats();RebuildCitizenSpatialIndex();
	DeparturesThisUpdate.Reset();
	// A returned responder must join civilian evacuation when its station is
	// unsafe (or its courier role is nonessential). Replan once per callback,
	// before the ordinary panic guard can leave an AtWork resident frozen.
	if(bIsPanicModeActive)if(const auto* Transport=GetWorld()?GetWorld()->GetSubsystem<UTransportSubsystem>():nullptr)
	{
		bool ReplanReleasedWorker=false;
		for(const auto& Agent:Agents)if(Agent.Activity==EAftershockCitizenActivity::AtWork)
		{
			FAftershockMunicipalDutyView Duty;if(Transport->GetMunicipalDuty(Agent.ResidentID,Duty))continue;
			if(!Transport->IsMunicipalStaffResident(Agent.ResidentID,Agent.WorkBuildingID)||!IsMunicipalWorkAnchorSafe(Agent,GetWorld())){ReplanReleasedWorker=true;break;}
		}
		if(ReplanReleasedWorker)ReplanEmergencyEvacuation();
	}
	RoutingRefreshSeconds += CrowdUpdateSeconds;
    if(bCityLifeSyncPending)RoutingRefreshSeconds=0.f;
	if (RoutingRefreshSeconds >= 0.8f)
	{
		RoutingRefreshSeconds = 0.f;
		if (RefreshRoutingSafety())
		{
			if (bIsPanicModeActive) ReplanEmergencyEvacuation();
            // Calm, already-started journeys keep their physical route. Any
            // removed/flooded/closed edge is still rejected by bSafeMotion below
            // before displacement and replanned from that actual position.
            // New departures read the freshly invalidated goal cache on demand;
            // do not rebuild every whole-grid goal field in one80ms callback.
		}
	}

    if(UsesIncrementalNormalRouting()&&IncrementalNormalRoutes.IsValid()&&!bCityLifeSyncPending)
    {
        const auto* Grid=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
        if(Grid)
        {
            const uint64 Before=IncrementalNormalRoutes->GetStartedJobs();
            IncrementalNormalRoutes->Advance(*Grid,RoutingCellCosts,.002,4096);
            const uint64 New=IncrementalNormalRoutes->GetStartedJobs()-Before;NormalRouteTreeBuildCount+=New;
            if(AftershockRoadCpuDiagnosticsEnabled())
            {AftershockRoadGoalCpuTotals().Fields+=New;AftershockRoadGoalCpuTotals().FieldBuildMs+=IncrementalNormalRoutes->GetLastCpuMs();
             UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_INCREMENTAL_NORMAL_ROUTES jobs=%d pendingAgents=%d syntheticProfile=%d pops=%d searchCpuMs=%.3f retainedBytes=%llu existingByteCap=%llu newFields=%llu completedRequests=%llu oldestPendingWallMs=%.3f maximumCompletedWallMs=%.3f frame=%llu noNewPeople=1 noTeleport=1 sameWeightedDijkstra=1"),
                IncrementalNormalRoutes->GetJobCount(),IncrementalNormalRoutes->GetRequestCount(),bSyntheticRenderProfileActive?1:0,IncrementalNormalRoutes->GetLastPops(),IncrementalNormalRoutes->GetLastCpuMs(),
                IncrementalNormalRoutes->GetAllocatedBytes(),MaximumNormalRouteCacheBytes,New,IncrementalNormalRoutes->GetCompletedRequests(),
                IncrementalNormalRoutes->GetOldestPendingWallSeconds()*1000.,IncrementalNormalRoutes->GetMaximumCompletedWaitSeconds()*1000.,static_cast<unsigned long long>(GFrameCounter));}
        }
    }
    if(IncrementalNormalRoutes.IsValid()&&UsesIncrementalNormalRouting())IncrementalNormalRoutes->BeginDeliverySlice(.002,16);
	PreMovementPositions.SetNumUninitialized(Agents.Num());
	for(int32 I=0;I<Agents.Num();++I)PreMovementPositions[I]=Agents[I].CurrentLocation;
	const UTerrainGridSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	// Overflow remains visibly outside, but a stationary forecourt queue must
	// yield the real doorway to a resident with a reserved place. Move aside
	// through ordinary swept walking, retaining every ID and final destination.
	// Otherwise a64cm body at the door makes the45cm admission radius unreachable.
	int32 CourtesyStarts=0,CourtesyCandidates=0;
	if(Terrain)for(int32 R=0;R<Agents.Num()&&CourtesyStarts<8&&CourtesyCandidates<128;++R)
	{
		const auto& Reserved=Agents[R];
		if(!Reserved.bEmergencyPlaceReserved||Reserved.Activity!=EAftershockCitizenActivity::Evacuating||!Reserved.bRouteReachesDestination
			||FVector::DistSquared2D(Reserved.CurrentLocation,Reserved.FinalDestinationLocation)>FMath::Square(160.f))continue;
		const FVector Door=Reserved.FinalDestinationLocation;const FIntPoint Bucket=CitizenBucket(Door);int32 Visited=0;
		for(int32 Y=-1;Y<=1&&Visited<32&&CourtesyStarts<8&&CourtesyCandidates<128;++Y)for(int32 X=-1;X<=1&&Visited<32&&CourtesyStarts<8&&CourtesyCandidates<128;++X)
		{
			const auto* Nearby=CitizenSpatialBuckets.Find(Bucket+FIntPoint(X,Y));if(!Nearby)continue;
			for(int32 J:*Nearby)
			{
				if(++Visited>32||CourtesyStarts>=8||CourtesyCandidates>=128)break;
				if(J==R||!Agents.IsValidIndex(J))continue;
				auto& Waiting=Agents[J];
				const bool AtRest=Waiting.Activity==EAftershockCitizenActivity::AwaitingShelter&&Waiting.MoveSpeed<=0.f;
				const bool QueuedAtDoor=Waiting.bEmergencyOverflow&&Waiting.Activity==EAftershockCitizenActivity::Evacuating
					&&Waiting.ActivityAfterArrival==EAftershockCitizenActivity::AwaitingShelter&&Waiting.WalkVelocityCmPerSec.SizeSquared()<625.f
					&&Waiting.TargetLocation.Equals(Waiting.FinalDestinationLocation,1.f);
				if(Waiting.bEmergencyPlaceReserved||(!AtRest&&!QueuedAtDoor)
					||Waiting.bInsideVehicle||Waiting.EmergencyBuildingID!=Reserved.EmergencyBuildingID
					||FVector::DistSquared2D(Waiting.CurrentLocation,Door)>FMath::Square(112.f))continue;
				FVector Away=(Waiting.CurrentLocation-Door).GetSafeNormal2D();
				if(Away.IsNearlyZero())Away=FVector(1,0,0);
				bool Yielded=false;
				for(float Distance:{128.f,192.f})
				{
					for(float Angle:{0.f,45.f,-45.f,90.f,-90.f,135.f,-135.f,180.f})
					{
						if(CourtesyCandidates>=128)break;++CourtesyCandidates;
						FVector Candidate=Waiting.CurrentLocation+FRotator(0,Angle,0).RotateVector(Away)*Distance;
						const FIntPoint CandidateCell=Terrain->WorldToGrid(Candidate);
						if(!Terrain->IsPlayableCoord(CandidateCell))continue;
						const auto* Roads=GetWorld()->GetSubsystem<URoadNetworkSubsystem>();
						Candidate.Z=(Terrain->GetCell(CandidateCell).bHasRoad&&Roads?Roads->SampleRoadSurfaceElevation(Candidate):VisibleGroundHeight(*Terrain,Candidate))+CitizenZOffset;
						if(FVector::DistSquared2D(Candidate,Door)<FMath::Square(128.f)
							||!CanMoveCitizenSegment(*Terrain,Waiting.CurrentLocation,Candidate)
							||!IsCitizenSpaceClear(Waiting.CurrentLocation,Candidate,J))continue;
						const auto* Traffic=GetWorld()->GetSubsystem<UTransportSubsystem>();
						if(Traffic&&!Traffic->IsPedestrianSpaceClear(Waiting.CurrentLocation,Candidate,CitizenBodyRadiusCm))continue;
						Waiting.RouteGridCoords.Reset();Waiting.NextRouteCoordIndex=0;
						Waiting.TargetLocation=Candidate;Waiting.ActivityAfterArrival=EAftershockCitizenActivity::AwaitingShelter;
						SetAgentActivity(Waiting,EAftershockCitizenActivity::Evacuating);
						++CourtesyStarts;Yielded=true;break;
					}
					if(Yielded||CourtesyCandidates>=128)break;
				}
			}
		}
	}
	for (int32 Index = 0; Index < Agents.Num(); ++Index)
	{
		FCitizenAgent& Agent = Agents[Index];
		Agent.FoodVisitCooldown=FMath::Max(0.f,Agent.FoodVisitCooldown-CrowdUpdateSeconds);
		Agent.HealthVisitCooldown=FMath::Max(0.f,Agent.HealthVisitCooldown-CrowdUpdateSeconds);
		if(auto* Transport=GetWorld()?GetWorld()->GetSubsystem<UTransportSubsystem>():nullptr)
		{
			FAftershockMunicipalDutyView Duty;bool HasDuty=Transport->GetMunicipalDuty(Agent.ResidentID,Duty);
			const bool SafeWorkAnchor=IsMunicipalWorkAnchorSafe(Agent,GetWorld());
			if(HasDuty&&!SafeWorkAnchor)
			{Transport->CancelMunicipalDutyForResident(Agent.ResidentID);HasDuty=Transport->GetMunicipalDuty(Agent.ResidentID,Duty);}
			if(!HasDuty&&SafeWorkAnchor&&(!bIsPanicModeActive||Transport->IsMunicipalStaffResident(Agent.ResidentID,Agent.WorkBuildingID)))
			{
				Transport->TryBeginServiceDuty(Agent.ResidentID,Agent.WorkBuildingID);
				HasDuty=Transport->GetMunicipalDuty(Agent.ResidentID,Duty);
			}
			if(HasDuty||(SafeWorkAnchor&&bIsPanicModeActive&&Transport->IsMunicipalStaffResident(Agent.ResidentID,Agent.WorkBuildingID)))
			{
				// Keep the one saved workplace anchor until transport releases its
				// actual road return; never fabricate passenger/bus state flags.
				PreMovementPositions[Index]=Agent.CurrentLocation;Agent.WalkVelocityCmPerSec=FVector2D::ZeroVector;
				Agent.MoveSpeed=0.f;Agent.bIsPanicking=false;UpdateCitizenSpatialEntry(Index);continue;
			}
		}
		const bool bTransportOwnsPosition=UpdatePassengerState(Agent);
		// Riding/alighting copies the authoritative vehicle/curb position. That
		// handoff is not a walking stride and must not create a one-frame5000cm/s gait.
		PreMovementPositions[Index]=Agent.CurrentLocation;
        if(bTransportOwnsPosition&&IncrementalNormalRoutes.IsValid())IncrementalNormalRoutes->Cancel(NormalRouteAgentKey(Agent));
        FVector PendingDestination;bool KeepPendingMotion=false;
        if(!bTransportOwnsPosition&&IncrementalNormalRoutes.IsValid()&&IncrementalNormalRoutes->FindRequest(NormalRouteAgentKey(Agent),PendingDestination,KeepPendingMotion))
        {
            // An infrastructure refresh may have reassigned the actual pickup.
            // Read the live reservation; do not deliver a stale pending stop.
            if(Agent.TransportTripID!=INDEX_NONE)
            {FAftershockPassengerTrip Trip;const auto* Transport=GetWorld()->GetSubsystem<UTransportSubsystem>();
             if(Transport&&Transport->GetTrip(Agent.TransportTripID,Trip)&&Trip.State==EAftershockPassengerState::WalkingToPickup)PendingDestination=Trip.PickupLocation;}
            BuildSafeRoute(Agent,PendingDestination);
            if(HasPendingNormalRoute(Agent))
            {
                IncrementalNormalRoutes->FindRequest(NormalRouteAgentKey(Agent),PendingDestination,KeepPendingMotion);
                if(!KeepPendingMotion){Agent.MoveSpeed=0.f;Agent.WalkVelocityCmPerSec=FVector2D::ZeroVector;UpdateCitizenSpatialEntry(Index);continue;}
            }
            else if(Agent.bRouteReachesDestination)
                SetAgentActivity(Agent,Agent.Activity==EAftershockCitizenActivity::WaitingForRoute?EAftershockCitizenActivity::Commuting:Agent.Activity);
            else if(Agent.TransportTripID!=INDEX_NONE)
            {
                const FVector Final=Agent.TransportFinalDestination;const auto Arrival=Agent.TransportArrivalActivity;
                CancelAgentTransport(Agent);StartCommute(Agent,Final,Arrival,false);
                if(HasPendingNormalRoute(Agent)){Agent.MoveSpeed=0.f;UpdateCitizenSpatialEntry(Index);continue;}
            }
            else{SetAgentActivity(Agent,EAftershockCitizenActivity::WaitingForRoute);Agent.ScheduleSecondsRemaining=10.f;}
        }
		if(!bTransportOwnsPosition&&Agent.MoveSpeed<=0.f)AdvanceAgentSchedule(Agent);
        // A schedule can have created a NEW pending departure in this callback.
        // Retain the real indoor doorway/curb until a finalized path is ready.
        if(!bTransportOwnsPosition&&HasPendingNormalRoute(Agent))
        {
            bool Keep=false;FVector Goal;
            IncrementalNormalRoutes->FindRequest(NormalRouteAgentKey(Agent),Goal,Keep);
            if(!Keep){Agent.MoveSpeed=0.f;Agent.WalkVelocityCmPerSec=FVector2D::ZeroVector;UpdateCitizenSpatialEntry(Index);continue;}
        }
		UpdateCitizenSpatialEntry(Index);
		if(!bTransportOwnsPosition&&!TryAdmitOutdoorDeparture(Agent))continue;
		// Centre admission uses the existing45cm service reach from a safe actual
		// position; an occupied exact door point must not prevent entry forever.
		if(!bTransportOwnsPosition&&Agent.Activity==EAftershockCitizenActivity::Evacuating
			&&Agent.bEmergencyPlaceReserved&&Agent.bRouteReachesDestination&&Terrain
			&&Terrain->WorldToGrid(Agent.CurrentLocation)==Terrain->WorldToGrid(Agent.FinalDestinationLocation)
			&&FVector::Dist(Agent.CurrentLocation,Agent.FinalDestinationLocation)<=45.f)
		{
			FBuildingInstance Centre;const auto* Buildings=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();
			if(Buildings&&Buildings->GetBuildingByID(Agent.EmergencyBuildingID,Centre)&&Centre.DamageState<EDamageState::Collapsed)
			{HandleAgentArrival(Agent);UpdateCitizenSpatialEntry(Index);continue;}
		}
		const FVector ToTarget = Agent.TargetLocation - Agent.CurrentLocation;
		const float Distance = ToTarget.Size();
		const float Step=FMath::Min(Distance,Agent.MoveSpeed*CrowdUpdateSeconds);
		bool bAtWaypoint=Distance<=FMath::Max(1.f,Agent.MoveSpeed*CrowdUpdateSeconds);
		FVector Candidate=bAtWaypoint?Agent.TargetLocation:Agent.CurrentLocation+(Distance>SMALL_NUMBER?ToTarget/Distance*Step:FVector::ZeroVector);
		const bool bSafeMotion=bTransportOwnsPosition||Agent.MoveSpeed<=0.f||(Terrain&&CanMoveCitizenSegment(*Terrain,Agent.CurrentLocation,Candidate));
		if(bSafeMotion&&!bTransportOwnsPosition&&Agent.MoveSpeed>0.f)
		{
			const auto* Transport=GetWorld()->GetSubsystem<UTransportSubsystem>();
			const auto Clear=[&](const FVector& At)
			{return IsCitizenSpaceClear(Agent.CurrentLocation,At,Index,true)&&(!Transport||Transport->IsPedestrianSpaceClear(Agent.CurrentLocation,At,CitizenBodyRadiusCm));};
			if(!Clear(Candidate))
			{
				bool Found=false;
				const FVector Forward=ToTarget.GetSafeNormal2D(),Side(-Forward.Y,Forward.X,0);
				const float Preferred=(CitizenHash(static_cast<uint32>(Agent.RandomSeed))&1u)?1.f:-1.f;
				// A forward-biased sidestep alone cannot get around a full forecourt
				// queue: both diagonals still approach the blocking body's radius.
				// Six deterministic, bounded candidates include a genuine tangent;
				// each retains the same per-callback distance and swept safety tests.
				for(float Angle:{30.f,60.f,90.f})
				{
					const float Radians=FMath::DegreesToRadians(Angle);
					for(float Sign:{Preferred,-Preferred})
					{
						const FVector Direction=Forward*FMath::Max(0.f,FMath::Cos(Radians))+Side*(Sign*FMath::Sin(Radians));
						const FVector Detour=Agent.CurrentLocation+Direction*Step;
						if(Terrain&&CanMoveCitizenSegment(*Terrain,Agent.CurrentLocation,Detour)&&Clear(Detour))
						{Candidate=Detour;bAtWaypoint=false;Found=true;break;}
					}
					if(Found)break;
				}
				if(!Found)
				{
					++SpacingStats.BlockedMoves;
					if ((Index + SpacingStats.BlockedMoves) % 6 == 0)
					{
						BuildSafeRoute(Agent, Agent.FinalDestinationLocation);
					}
					continue;
				}
			}
		}
		if(!bSafeMotion)
		{
			// Separation may have crossed into a different legal parcel. Rebuild
			// from that real coordinate rather than cutting a diagonal to a stale waypoint.
			BuildSafeRoute(Agent,Agent.FinalDestinationLocation);
		}
		else if (!bTransportOwnsPosition && Agent.MoveSpeed>0.f && bAtWaypoint)
		{
			Agent.CurrentLocation = Agent.TargetLocation;
			const bool FinishedCourtesy=Agent.Activity==EAftershockCitizenActivity::Evacuating&&Agent.bEmergencyOverflow&&!Agent.bEmergencyPlaceReserved
				&&Agent.ActivityAfterArrival==EAftershockCitizenActivity::AwaitingShelter&&Agent.RouteGridCoords.IsEmpty()
				&&!Agent.TargetLocation.Equals(Agent.FinalDestinationLocation,1.f);
			if(FinishedCourtesy)
			{
				SetAgentActivity(Agent,EAftershockCitizenActivity::AwaitingShelter);
				Agent.TargetLocation=Agent.CurrentLocation;Agent.ScheduleSecondsRemaining=0.f;
			}
			else if (Agent.RouteGridCoords.IsValidIndex(Agent.NextRouteCoordIndex + 1))
			{
				++Agent.NextRouteCoordIndex;
				if (UWorld* World = GetWorld())
				{
					if (UTerrainGridSubsystem* RouteTerrain = World->GetSubsystem<UTerrainGridSubsystem>())
					{
						Agent.TargetLocation = MakeVisibleCellLocation(*RouteTerrain, Agent.RouteGridCoords[Agent.NextRouteCoordIndex], Agent);
					}
				}
			}
			else
			{
				Agent.RouteGridCoords.Reset();
				Agent.NextRouteCoordIndex = 0;
				Agent.TargetLocation = Agent.FinalDestinationLocation;
				if (FVector::Dist(Agent.CurrentLocation, Agent.FinalDestinationLocation) <= 45.0f)
				{
					HandleAgentArrival(Agent);
					AdvanceAgentSchedule(Agent);
				}
				else
				{
					BuildSafeRoute(Agent, Agent.FinalDestinationLocation);
				}
			}
		}
		else if (!bTransportOwnsPosition && Agent.MoveSpeed > 0.0f)
		{
			Agent.CurrentLocation = Candidate;
		}
		if (Terrain && Terrain->IsPlayableCoord(Terrain->WorldToGrid(Agent.CurrentLocation)))
		{
			const FGridCell Cell = Terrain->GetCell(Terrain->WorldToGrid(Agent.CurrentLocation));
			const URoadNetworkSubsystem* Roads=GetWorld()->GetSubsystem<URoadNetworkSubsystem>();
			Agent.CurrentLocation.Z = (Cell.bHasRoad && Roads ? Roads->SampleRoadSurfaceElevation(Agent.CurrentLocation) : VisibleGroundHeight(*Terrain, Agent.CurrentLocation)) + CitizenZOffset;
		}
		if (Agent.MoveSpeed > 0.f && ToTarget.SizeSquared2D() > 1.f) Agent.FacingYaw=FMath::FixedTurn(Agent.FacingYaw,ToTarget.Rotation().Yaw,360.f*CrowdUpdateSeconds);
		UpdateCitizenSpatialEntry(Index);
	}
	if(Terrain)ResolveCrowdSpacing(*Terrain);
	for(int32 I=0;I<Agents.Num();++I)
	{
		FCitizenAgent& Agent=Agents[I];
		Agent.WalkVelocityCmPerSec=(Agent.bInsideVehicle||IsCitizenIndoors(Agent,GetWorld()))?FVector2D::ZeroVector:FVector2D(Agent.CurrentLocation-PreMovementPositions[I])/CrowdUpdateSeconds;
		if(Agent.MoveSpeed>0.f&&!Agent.bInsideVehicle)
		{
			const float Stride=Agent.bIsPanicking?170.f:140.f;
			Agent.WalkingPhaseRadians=FMath::Fmod(Agent.WalkingPhaseRadians+Agent.WalkVelocityCmPerSec.Size()*CrowdUpdateSeconds*(2.f*PI/Stride),2.f*PI);
		}
	}
	RebuildCitizenInstanceTransforms();
	UpdateEmotionBillboards();
    if(IncrementalNormalRoutes.IsValid())IncrementalNormalRoutes->EndDeliverySlice();
	bUpdatingAgents=false;
	if(bCityLifeSyncPending){bCityLifeSyncPending=false;SynchronizeCityLifeRoster();}
	if(bSyntheticRenderProfileActive)
	{
		int32 Moving=0;for(const FCitizenAgent& Agent:Agents)Moving+=Agent.MoveSpeed>0.f?1:0;
		const double Elapsed=(FPlatformTime::Seconds()-ProfileStarted)*1000.;
		SyntheticProfileTotalMilliseconds+=Elapsed;SyntheticProfileMaxMilliseconds=FMath::Max(SyntheticProfileMaxMilliseconds,Elapsed);++SyntheticProfileSamples;
		CSV_CUSTOM_STAT(AftershockCrowd,SyntheticAgents,Agents.Num(),ECsvCustomStatOp::Set);
		CSV_CUSTOM_STAT(AftershockCrowd,SyntheticMovingAgents,Moving,ECsvCustomStatOp::Set);
		if(SyntheticProfileSamples==60)
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SYNTHETIC_RENDER_PROFILE_CPU agents=%d moving=%d samples=%d meanCallbackMs=%.6f maxCallbackMs=%.6f scope=CpuMovementAndIsmSubmit_NotGpu syntheticResidentID=-1"),Agents.Num(),Moving,SyntheticProfileSamples,SyntheticProfileTotalMilliseconds/SyntheticProfileSamples,SyntheticProfileMaxMilliseconds);
	}

}

void UCrowdRenderSubsystem::HandleDisasterCompleted(const FDisasterReport& Report)
{
	UWorld* World = GetWorld();
	const UDisasterSimulationSubsystem* Disaster = World ? World->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
	if (World && Disaster)
	{
		World->GetTimerManager().ClearTimer(DisasterVisualCompletionHandle);
		const FAftershockDisasterVisualSnapshot& Snapshot = Disaster->GetLatestVisualSnapshot();
		float Remaining = Snapshot.StartedAtWorldSeconds + Snapshot.DurationSeconds - World->GetTimeSeconds();
		if (Report.DisasterType == EDisasterType::Earthquake && bIsPanicModeActive)
		{
			// Instantaneous canonical quake results must not erase the visible
			// evacuation in the same frame. This delays only representative state.
			Remaining = FMath::Max(Remaining, EmergencyStartedAtWorldSeconds + 10.f - World->GetTimeSeconds());
		}
		if (Snapshot.Revision > 0 && Remaining > 0.01f)
		{
			const int32 Revision = Snapshot.Revision;
			World->GetTimerManager().SetTimer(DisasterVisualCompletionHandle,
				FTimerDelegate::CreateWeakLambda(this, [this, Report, Revision]()
				{
					const UDisasterSimulationSubsystem* Current = GetWorld() ? GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
					if (Current && Current->GetLatestVisualSnapshot().Revision == Revision) ApplyDisasterCompletedVisuals(Report);
				}), Remaining, false);
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CROWD_DISASTER_VISUAL deferredCompletionSeconds=%.2f revision=%d"), Remaining, Revision);
			return;
		}
	}
	ApplyDisasterCompletedVisuals(Report);
}

void UCrowdRenderSubsystem::ApplyDisasterCompletedVisuals(const FDisasterReport& Report)
{
	// CityLife already removed actual deaths from the roster. Applying the report
	// survival fraction to those living people a second time stranded survivors
	// in shelter forever. The aggregate fraction is legacy-fixture presentation only.
	bIsPanicModeActive = false;
	if(UTransportSubsystem* Transport=GetWorld()?GetWorld()->GetSubsystem<UTransportSubsystem>():nullptr)Transport->SetEmergencyMode(false);
	const UCityLifeSubsystem* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr;
	const bool bCanonicalResidents=Life&&Life->IsCityInitialized();
	const int64 ReportedPeople = static_cast<int64>(FMath::Max(0, Report.TotalSurvivors)) + FMath::Max(0, Report.TotalDeaths);
	const float SurvivorRatio = ReportedPeople > 0
		? FMath::Clamp(static_cast<float>(Report.TotalSurvivors) / static_cast<float>(ReportedPeople), 0.0f, 1.0f)
		: 1.0f;
	const int32 VisualRescuedCount = bCanonicalResidents?Agents.Num()
		:FMath::Clamp(FMath::RoundToInt(static_cast<float>(Agents.Num()) * SurvivorRatio), 0, Agents.Num());
	int32 AffectedAgentIndex = 0;

	for (FCitizenAgent& Agent : Agents)
	{
		Agent.bIsPanicking = false;
		if (Agent.Activity != EAftershockCitizenActivity::Evacuating && Agent.Activity != EAftershockCitizenActivity::AtShelter
			&& Agent.Activity != EAftershockCitizenActivity::AwaitingShelter)
		{
			continue;
		}

		if (AffectedAgentIndex++ < VisualRescuedCount && (!bCanonicalResidents||Agent.bHomeOperating))
		{
			Agent.bEmergencyPlaceReserved=false;Agent.bEmergencyOverflow=false;Agent.bEmergencyUnreachable=false;Agent.EmergencyBuildingID=INDEX_NONE;
			Agent.ActivityAfterArrival = EAftershockCitizenActivity::AtHome;
			SetAgentActivity(Agent, EAftershockCitizenActivity::Rescued);
			BuildSafeRoute(Agent, Agent.HomeLocation);
		}
		else
		{
			StartCommute(Agent, Agent.ShelterLocation, Agent.bEmergencyPlaceReserved
				? EAftershockCitizenActivity::AtShelter : EAftershockCitizenActivity::AwaitingShelter);
		}
	}

	if (IsValid(CrowdHostActor) && CrowdHostActor->CitizenISM)
	{
		CrowdHostActor->CitizenISM->MarkRenderStateDirty();
	}
}

void UCrowdRenderSubsystem::HandleEconomyUpdated(int64 InTreasuryTL, float InSatisfaction, int32 InCurrentTurn)
{
    if(const UCityLifeSubsystem* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr)
        if(Life->IsCityInitialized())
        {
            // Real jobs/services/membership already publish through CityLife.
            // Treasury alone is not a second roster/destination authority.
            if(bIsPanicModeActive||Life->GetState().LivingPopulation!=LastSynchronizedLivingPopulation
                ||Life->GetState().TotalDeaths!=LastSynchronizedTotalDeaths)HandleCityLifeUpdated();
            return;
        }

	UWorld* World = GetWorld();
	UBuildingManagerSubsystem* BuildingMgr = World ? World->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
	if (!BuildingMgr)
	{
		return;
	}

	TArray<FBuildingInstance> Buildings;
	BuildingMgr->GetAllBuildings(Buildings);

	int64 TotalResidents = 0;
	int64 TotalWorkers = 0;
	for (const FBuildingInstance& Building : Buildings)
	{
		if (Building.BuildingData)
		{
			// Whole-building capacities: match the occupancy contract used by hazards.
			TotalResidents += FMath::Max(0, Building.BuildingData->Residents);
			TotalWorkers += FMath::Max(0, Building.BuildingData->Workers);
		}
	}

	UpdatePopulationCount(static_cast<int32>(FMath::Min<int64>(TotalResidents, MAX_int32)), static_cast<int32>(FMath::Min<int64>(TotalWorkers, MAX_int32)));
}

void UCrowdRenderSubsystem::HandleRoadNetworkChanged()
{
    FAftershockRoadCpuScope Cpu(TEXT("Crowd.HandleRoadNetworkChanged"));
	ResetNormalRouteCache(); // Bridge edge legality can change without a different cell cost.
	if (const UGameInstance* Instance=GetWorld()?GetWorld()->GetGameInstance():nullptr)
		if (const UAftershockSaveSubsystem* Saves=Instance->GetSubsystem<UAftershockSaveSubsystem>(); Saves && Saves->IsRestoring())
		{
			// Bulk load imports the real roster after this event. Rebuild derived
			// geometry only; do not schedule, reseed or cancel old passengers here.
			RebuildDestinationCaches();
			return;
		}
	// Live CityLife already committed the canonical connectivity before this
    // event. Request one visual refresh, not a second unconditional full-roster
    // replan. Active movement still tests real edge safety on each accepted step.
    if(const auto* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr)
        if(Life->IsCityInitialized())
        {HandleCityLifeUpdated();return;}
	// Legacy isolated capacity fixtures retain their synchronous schedule path.
	HandleEconomyUpdated(0, 0.f, 0);
	if (bIsPanicModeActive) return;
	for (FCitizenAgent& Agent : Agents)
	{
		if(Agent.TransportTripID!=INDEX_NONE)continue;
		const EAftershockCitizenActivity Arrival = Agent.MoveSpeed > 0.f ? Agent.ActivityAfterArrival : Agent.Activity;
		FVector Destination = Agent.FinalDestinationLocation;
		switch (Arrival)
		{
		case EAftershockCitizenActivity::AtHome: Destination = Agent.HomeLocation; break;
		case EAftershockCitizenActivity::AtWork: Destination = Agent.WorkLocation; break;
		case EAftershockCitizenActivity::AtLeisure: Destination = Agent.LeisureLocation; break;
		case EAftershockCitizenActivity::AtFoodService: Destination=Agent.FoodLocation;break;
		case EAftershockCitizenActivity::AtHealthService: Destination=Agent.HealthLocation;break;
		case EAftershockCitizenActivity::AtShelter:
		case EAftershockCitizenActivity::AwaitingShelter: Destination = Agent.ShelterLocation; break;
		default: break;
		}
		if (Agent.MoveSpeed > 0.f)
		{
			BuildSafeRoute(Agent, Destination);
		}
		else if (FVector::DistSquared2D(Agent.CurrentLocation, Destination) > 100.f)
		{
			StartCommute(Agent, Destination, Arrival);
		}
	}
}

void UCrowdRenderSubsystem::HandleTerrainStorageChanged(uint64 Revision)
{
	EnsureTerrainVisualBuilt();
}

void UCrowdRenderSubsystem::SynchronizeCanonicalPopulation()
{
    // Explicit lifecycle/save/audit barrier retains its original immediate
    // contract; only ordinary notifications may request a coalesced refresh.
    if(const auto* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr)
        if(Life->IsCityInitialized()){SynchronizeCityLifeRoster();return;}
	HandleEconomyUpdated(0, 0.f, 0);
}

FAftershockCitizenPlanningStats UCrowdRenderSubsystem::GetCitizenPlanningStats() const
{
	FAftershockCitizenPlanningStats Stats;
	Stats.Representatives = Agents.Num();
	Stats.EligibleCentres = ActiveEligibleCentres;
	Stats.VisualPlaceQuota = ActiveVisualPlaceQuota;
	const UTerrainGridSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	for (const FCitizenAgent& Agent : Agents)
	{
		Stats.Reserved += Agent.bEmergencyPlaceReserved ? 1 : 0;
		Stats.ArrivedAtCentre += Agent.Activity == EAftershockCitizenActivity::AtShelter && Agent.bEmergencyPlaceReserved ? 1 : 0;
		Stats.MovingToSafety += Agent.Activity == EAftershockCitizenActivity::Evacuating && Agent.MoveSpeed > 0.f ? 1 : 0;
		Stats.Overflow += Agent.bEmergencyOverflow ? 1 : 0;
		Stats.CentreUnreachable += Agent.bEmergencyUnreachable ? 1 : 0;
		Stats.WaitingAtAssembly += Agent.Activity == EAftershockCitizenActivity::AwaitingShelter ? 1 : 0;
		if (!Terrain) continue;
		for (int32 Step = Agent.NextRouteCoordIndex; Step < Agent.RouteGridCoords.Num(); ++Step)
		{
			const FIntPoint Coord = Agent.RouteGridCoords[Step];
			const int32 Index = Terrain->IsPlayableCoord(Coord) ? Terrain->CoordToIndex(Coord) : INDEX_NONE;
			Stats.UnsafeRemainingRouteSteps += !RoutingCellCosts.IsValidIndex(Index) || RoutingCellCosts[Index] == MAX_int32 ? 1 : 0;
		}
	}
	return Stats;
}

void UCrowdRenderSubsystem::AuditCitizenPlanning() const
{
	const FAftershockCitizenPlanningStats S = GetCitizenPlanningStats();
	uint32 RosterHash = 2166136261u;
	const auto Mix = [&RosterHash](int32 Value) { RosterHash = (RosterHash ^ static_cast<uint32>(Value)) * 16777619u; };
	for (const FCitizenAgent& Agent : Agents)
	{
		Mix(Agent.HomeBuildingID); Mix(Agent.WorkBuildingID); Mix(Agent.LeisureBuildingID);
		Mix(Agent.EmergencyBuildingID); Mix(Agent.bEmergencyPlaceReserved); Mix(Agent.bEmergencyOverflow); Mix(Agent.bEmergencyUnreachable);
		for (const FIntPoint Coord : Agent.RouteGridCoords) { Mix(Coord.X); Mix(Coord.Y); }
	}
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CITIZEN_PLAN agents=%d centres=%d visualQuota=%d reserved=%d arrived=%d moving=%d overflow=%d centreUnreachable=%d waiting=%d unsafeRemainingSteps=%d rosterHash=%u quotaIsPresentationOnly=1 populationOwner=BuildingManager agentOwner=CrowdRenderSubsystem"),
		S.Representatives, S.EligibleCentres, S.VisualPlaceQuota, S.Reserved, S.ArrivedAtCentre, S.MovingToSafety,
		S.Overflow, S.CentreUnreachable, S.WaitingAtAssembly, S.UnsafeRemainingRouteSteps, RosterHash);
	for (const FAftershockCitizenDestination& Centre : EmergencyCentres)
	{
		int32 Reserved = 0, Arrived = 0, Overflow = 0;
		for (const FCitizenAgent& Agent : Agents)
		{
			if (Agent.EmergencyBuildingID != Centre.BuildingID) continue;
			Reserved += Agent.bEmergencyPlaceReserved ? 1 : 0;
			Arrived += Agent.bEmergencyPlaceReserved && Agent.Activity == EAftershockCitizenActivity::AtShelter ? 1 : 0;
			Overflow += Agent.bEmergencyOverflow ? 1 : 0;
		}
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CITIZEN_CENTRE buildingID=%d type=%d visualQuota=%d reserved=%d arrived=%d overflow=%d"),
			Centre.BuildingID, static_cast<int32>(Centre.Type), Centre.Weight, Reserved, Arrived, Overflow);
	}
}

void UCrowdRenderSubsystem::GetPhysicalClearance(int32& Outdoor,int32& CitizenOverlaps,int32& VehicleIntersections,float& MinimumDistanceCm) const
{
	TArray<const FCitizenAgent*> Bodies;
	for(const auto& Agent:Agents)if(!Agent.bInsideVehicle&&!IsCitizenIndoors(Agent,GetWorld()))Bodies.Add(&Agent);
	Outdoor=Bodies.Num();CitizenOverlaps=0;VehicleIntersections=0;MinimumDistanceCm=MAX_flt;
	const auto* Transport=GetWorld()?GetWorld()->GetSubsystem<UTransportSubsystem>():nullptr;
	for(int32 I=0;I<Bodies.Num();++I)
	{
		const FVector At=Bodies[I]->CurrentLocation;
		VehicleIntersections+=Transport&&!Transport->IsPedestrianSpaceClear(At,At,32.f,false)?1:0;
		for(int32 J=I+1;J<Bodies.Num();++J)
		{
			if(FMath::Abs(At.Z-Bodies[J]->CurrentLocation.Z)>=175.f)continue;
			const float Distance=FVector::Dist2D(At,Bodies[J]->CurrentLocation);
			MinimumDistanceCm=FMath::Min(MinimumDistanceCm,Distance);CitizenOverlaps+=Distance<63.99f?1:0;
		}
	}
	if(MinimumDistanceCm==MAX_flt)MinimumDistanceCm=0.f;
}

void UCrowdRenderSubsystem::AuditCrowdVisibility() const
{
	const AAftershockCrowdHostActor* Host = IsValid(CrowdHostActor) ? CrowdHostActor : nullptr;
	const UInstancedStaticMeshComponent* ISM = Host ? Host->CitizenISM.Get() : nullptr;
	const UStaticMesh* Mesh = ISM ? ISM->GetStaticMesh() : nullptr;
	const UWorld* World = GetWorld();
	const UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const FIntRect Drawable=Terrain?GetAftershockDrawableCellBounds(*Terrain):FIntRect(0,0,0,0);
	int32 ViewWidth = 0, ViewHeight = 0;
	if (PC) PC->GetViewportSize(ViewWidth, ViewHeight);
	int32 Invalid = 0, Occupied = 0, River = 0, OnRoad = 0, Moving = 0, HiddenIndoors=0, HiddenPassengers=0, ProjectedCandidates = 0;
	int32 RenderedOutdoor=0,WindowMasked=0,OutsideWindowDrawn=0;
	float ProjectedHeightSum = 0.f;
	TSet<FIntVector> DistinctPositions;
	TArray<const FCitizenAgent*> OutdoorBodies;
	for (const FCitizenAgent& Agent : Agents)
	{
		if(Agent.bInsideVehicle){++HiddenPassengers;continue;}
		if(IsCitizenIndoors(Agent,GetWorld())){++HiddenIndoors;continue;}
		OutdoorBodies.Add(&Agent);
		const bool InWindow=Terrain&&Drawable.Contains(Terrain->WorldToGrid(Agent.CurrentLocation));
		FTransform BodyTransform;const bool Drawn=ISM&&ISM->GetInstanceTransform(Agent.ISMIndex,BodyTransform,true)&&!BodyTransform.GetScale3D().IsNearlyZero();
		RenderedOutdoor+=Drawn?1:0;WindowMasked+=!InWindow?1:0;OutsideWindowDrawn+=Drawn&&!InWindow?1:0;
		DistinctPositions.Add(FIntVector(FMath::RoundToInt(Agent.CurrentLocation.X / 10.f), FMath::RoundToInt(Agent.CurrentLocation.Y / 10.f), FMath::RoundToInt(Agent.CurrentLocation.Z / 10.f)));
		Moving += Agent.MoveSpeed > 0.f ? 1 : 0;
		const FIntPoint Coord = Terrain ? Terrain->WorldToGrid(Agent.CurrentLocation) : FIntPoint(MAX_int32, MAX_int32);
		if (!Terrain || !IsCitizenGeometryCell(*Terrain,Coord)) { ++Invalid; continue; }
		const FGridCell Cell = Terrain->GetCell(Coord);
		Occupied += Cell.OccupyingBuildingID != INDEX_NONE ? 1 : 0;
		River += Cell.bIsRiverbed ? 1 : 0;
		OnRoad += Cell.bHasRoad ? 1 : 0;
		FVector2D Feet, Head;
		if (Drawn&&InWindow&&PC && ViewWidth > 0 && ViewHeight > 0
			&& PC->ProjectWorldLocationToScreen(Agent.CurrentLocation, Feet)
			&& PC->ProjectWorldLocationToScreen(Agent.CurrentLocation + FVector(0.f, 0.f, CitizenVisibleHeightCm), Head)
			&& Feet.X >= 0 && Feet.X < ViewWidth && Feet.Y >= 0 && Feet.Y < ViewHeight)
		{
			++ProjectedCandidates;
			ProjectedHeightSum += FVector2D::Distance(Feet, Head);
		}
	}
	const FVector SourceSize = Mesh ? CitizenSourceBounds(Mesh).GetSize() : FVector::ZeroVector;
	const FVector Scale = CitizenRenderScale(Mesh);
	const FBoxSphereBounds RenderBounds = ISM ? ISM->CalcBounds(ISM->GetComponentTransform()) : FBoxSphereBounds(EForceInit::ForceInit);
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CROWD_VISIBILITY agents=%d instances=%d distinct10cmPositions=%d moving=%d onPlayerRoad=%d invalidPlayable=%d occupied=%d river=%d meshRenderData=%d meshVertices=%d sourceSizeCm=%s renderScale=%s humanHeightCm=%.1f boundsOrigin=%s boundsExtent=%s registered=%d renderState=%d visible=%d projectedCandidates=%d meanProjectedHeightPx=%.2f projectionIsNotOcclusionProof=1"),
		Agents.Num(), ISM ? ISM->GetInstanceCount() : 0, DistinctPositions.Num(), Moving, OnRoad, Invalid, Occupied, River,
		Mesh && Mesh->HasValidRenderData() ? 1 : 0, Mesh && Mesh->HasValidRenderData() ? Mesh->GetNumVertices(0) : 0,
		*SourceSize.ToCompactString(), *Scale.ToCompactString(), SourceSize.Z * Scale.Z,
		*RenderBounds.Origin.ToCompactString(), *RenderBounds.BoxExtent.ToCompactString(),
		ISM && ISM->IsRegistered() ? 1 : 0, ISM && ISM->IsRenderStateCreated() ? 1 : 0, ISM && ISM->IsVisible() ? 1 : 0,
		ProjectedCandidates, ProjectedCandidates > 0 ? ProjectedHeightSum / ProjectedCandidates : 0.f);
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CROWD_DRAW_COVERAGE simulationOutdoor=%d renderedOutdoor=%d maskedByTerrainWindow=%d outsideWindowDrawn=%d drawableMin=%s drawableMax=%s physicalDiagnosticsRemainGlobal=1"),OutdoorBodies.Num(),RenderedOutdoor,WindowMasked,OutsideWindowDrawn,*Drawable.Min.ToString(),*Drawable.Max.ToString());
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CROWD_SPACING rosterSlots=%d hiddenIndoors=%d hiddenPassengers=%d outdoor=%d neighbourChecks=%d acceptedCorrections=%d rejectedUnsafeCorrections=%d sampledClosePairs=%d maxBucketOccupancy=%d closePairCountIsBoundedSample=1"),Agents.Num(),HiddenIndoors,HiddenPassengers,Agents.Num()-HiddenIndoors-HiddenPassengers,SpacingStats.NeighbourChecks,SpacingStats.AcceptedCorrections,SpacingStats.RejectedUnsafeCorrections,SpacingStats.ObservedClosePairs,SpacingStats.MaxBucketOccupancy);
	int32 ExactBodyOverlaps=0,PeopleIntersectingVehicles=0;int64 ExactPairs=0;double MinimumDistance=TNumericLimits<double>::Max();
	const auto* Transport=World?World->GetSubsystem<UTransportSubsystem>():nullptr;
	for(int32 I=0;I<OutdoorBodies.Num();++I)
	{
		const FVector At=OutdoorBodies[I]->CurrentLocation;
		PeopleIntersectingVehicles+=Transport&&!Transport->IsPedestrianSpaceClear(At,At,32.f,false)?1:0;
		for(int32 J=I+1;J<OutdoorBodies.Num();++J)
		{
			++ExactPairs;if(FMath::Abs(At.Z-OutdoorBodies[J]->CurrentLocation.Z)>=175.f)continue;
			const double Distance=FVector::Dist2D(At,OutdoorBodies[J]->CurrentLocation);MinimumDistance=FMath::Min(MinimumDistance,Distance);ExactBodyOverlaps+=Distance<63.99?1:0;
		}
	}
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CROWD_PHYSICAL_CLEARANCE outdoor=%d exactPairsTested=%lld bodyDiameterCm=64 crowdBodyOverlaps=%d peopleIntersectingVehicleBodies=%d minimumCentreDistanceCm=%.3f admissionChecks=%d blockedMoves=%d deferredEmergences=%d exhaustiveOnDemandOnly=1"),OutdoorBodies.Num(),ExactPairs,ExactBodyOverlaps,PeopleIntersectingVehicles,MinimumDistance==TNumericLimits<double>::Max()?0.:MinimumDistance,SpacingStats.AdmissionChecks,SpacingStats.BlockedMoves,SpacingStats.DeferredEmergences);
}
