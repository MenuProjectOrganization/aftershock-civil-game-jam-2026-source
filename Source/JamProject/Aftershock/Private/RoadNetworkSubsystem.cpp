#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "AftershockRoadCpuScope.h"
#include "AftershockHydraulicTerrain.h"

#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/DisasterPresentationSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Engine/World.h"
#include "TimerManager.h"

namespace
{
	constexpr float RoadSurfaceOffsetCm=8.f;
	constexpr float BridgeClearanceAboveBedCm=160.f;
	constexpr float MaximumBridgeGrade=.12f;
	constexpr int32 MaximumRiverSpanCells=8;
	constexpr int32 MaximumApproachCellsPerBank=12;
	constexpr int32 BridgeFoundationCostTL=6000;
	constexpr int32 BridgeDeckCellCostTL=1400;
	constexpr int32 BridgeApproachUpgradeCostTL=400;
	const FIntPoint AdjacentRoadSteps[]={FIntPoint(1,0),FIntPoint(0,1),FIntPoint(-1,0),FIntPoint(0,-1)};
	bool SameAxisStep(uint8 Axis,FIntPoint Step) { return Axis==1 ? Step.Y==0 : Axis==2 && Step.X==0; }
	// Model2 roads sit on the SAME half-grid triangle surface as the installed
	// terrain. The road is ordinary land, so every incident knot has a land
	// contributor and can never become the channel's excavated hydraulic bed.
	bool TrySamplePhysicalRoadGround(const UTerrainGridSubsystem& Terrain,FVector Position,double& Out)
	{
		const FIntPoint Coord=Terrain.WorldToGrid(Position);
		FGridCell Cell;FText Error;
		if(!Terrain.TryGetCell(Coord,Cell))return false;
		if(Cell.bIsRiverbed||Cell.bIsOcean)
			return TryGetAftershockMaterialGroundCm(Cell,true,Out,Error);
		const double GX=(Position.X-Terrain.GridOrigin.X)/Terrain.CellSize-.5;
		const double GY=(Position.Y-Terrain.GridOrigin.Y)/Terrain.CellSize-.5;
		const int32 HX=FMath::FloorToInt(GX*2.),HY=FMath::FloorToInt(GY*2.);
		const double U=GX*2.-HX,V=GY*2.-HY;
		double H[4];const FIntPoint Knot[4]={FIntPoint(HX,HY),FIntPoint(HX+1,HY),FIntPoint(HX,HY+1),FIntPoint(HX+1,HY+1)};
		for(int32 I=0;I<4;++I)
			if(!TryGetAftershockHydraulicGroundKnotCm(Terrain,Knot[I],true,H[I],Error))return false;
		// Identical diagonal selection to AftershockTerrainSurface LOD0. A rising
		// river pair uses A-D; every other quad uses B-C. No bilinear saddle and
		// no raycast, mesh scan, cache, source DEM edit or new terrain authority.
		const int32 QX=FMath::FloorToInt(HX*.5+.25),QY=FMath::FloorToInt(HY*.5+.25);
		FGridCell C[4];const FIntPoint Q[4]={FIntPoint(QX,QY),FIntPoint(QX+1,QY),FIntPoint(QX,QY+1),FIntPoint(QX+1,QY+1)};
		bool Complete=true;for(int32 I=0;I<4;++I)Complete&=Terrain.TryGetCell(Q[I],C[I]);
		const bool Rising=Complete&&C[0].bIsPlayable&&C[3].bIsPlayable&&C[0].bIsRiverbed&&C[3].bIsRiverbed&&!C[1].bIsRiverbed&&!C[2].bIsRiverbed;
		const double Candidate=Rising
			?(U>=V?H[0]*(1.-U)+H[1]*(U-V)+H[3]*V:H[0]*(1.-V)+H[2]*(V-U)+H[3]*U)
			:(U+V<=1.?H[0]*(1.-U-V)+H[1]*U+H[2]*V:H[3]*(U+V-1.)+H[1]*(1.-V)+H[2]*(1.-U));
		if(!FMath::IsFinite(Candidate))return false;
		Out=Candidate;return true;
	}
}

void URoadNetworkSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UTerrainGridSubsystem>();
	Collection.InitializeDependency<UEconomyAndStateSubsystem>();
	if (UTerrainGridSubsystem* Terrain=GetWorld()->GetSubsystem<UTerrainGridSubsystem>())
		TerrainStorageDelegateHandle=Terrain->OnTerrainStorageChanged.AddUObject(this,&URoadNetworkSubsystem::HandleTerrainStorageChanged);
	// Scenario startup may seed authored streets after building lots are occupied.
}

void URoadNetworkSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	RebuildBridgeMembership();
	InWorld.GetTimerManager().SetTimer(BridgeClosureTimer,this,&URoadNetworkSubsystem::RefreshBridgeClosures,.25f,true);
}

void URoadNetworkSubsystem::Deinitialize()
{
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(BridgeClosureTimer);
		if (UTerrainGridSubsystem* Terrain=GetWorld()->GetSubsystem<UTerrainGridSubsystem>()) Terrain->OnTerrainStorageChanged.Remove(TerrainStorageDelegateHandle);
	}
	BridgeCellsByID.Reset(); ClosedBridgeIDs.Reset(); OnRoadNetworkChanged.Clear();
	Super::Deinitialize();
}

float URoadNetworkSubsystem::GetRoadSurfaceElevation(FIntPoint Coord) const
{
	const UTerrainGridSubsystem* Terrain=GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || !Terrain->IsValidCoord(Coord)) return 0.f;
	const FGridCell Cell=Terrain->GetCell(Coord);
	if(Cell.bHasRoad&&Cell.BridgeID!=INDEX_NONE)return Cell.RoadSurfaceElevationCm;
	double Ground=Cell.Elevation;FText Error;
	const auto* Simulation=GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>();
	if(Simulation&&Simulation->IsHydraulicModel2Active())
	{
		const bool Valid=TryGetAftershockMaterialGroundCm(Cell,true,Ground,Error)&&FMath::Abs(Ground)<MAX_flt-RoadSurfaceOffsetCm;
		if(!ensureMsgf(Valid,TEXT("Invalid Model2 road mineral support: %s"),*Error.ToString()))Ground=Cell.Elevation;
	}
	return static_cast<float>(Ground+RoadSurfaceOffsetCm);
}

FVector URoadNetworkSubsystem::RoadGridToWorld(FIntPoint Coord,bool bCenter) const
{
	const UTerrainGridSubsystem* Terrain=GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain) return FVector::ZeroVector;
	FVector Position=Terrain->GridToWorld(Coord,bCenter); Position.Z=GetRoadSurfaceElevation(Coord); return Position;
}

float URoadNetworkSubsystem::SampleRoadSurfaceElevation(FVector Position) const
{
	const UTerrainGridSubsystem* Terrain=GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || Terrain->CellSize<=0.f) return 0.f;
	const FIntPoint Coord=Terrain->WorldToGrid(Position);
	if (!Terrain->IsValidCoord(Coord)) return Position.Z;
	const FGridCell Cell=Terrain->GetCell(Coord);
	if(Cell.BridgeID==INDEX_NONE||!Cell.bHasRoad)
	{
		const auto* Simulation=GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>();
		if(!Simulation||!Simulation->IsHydraulicModel2Active())return Cell.Elevation+RoadSurfaceOffsetCm;
		double Ground=0.;
		if(TrySamplePhysicalRoadGround(*Terrain,Position,Ground)&&FMath::Abs(Ground)<MAX_flt-RoadSurfaceOffsetCm)
			return static_cast<float>(Ground+RoadSurfaceOffsetCm);
		// Bad/sparse input cannot invent a surface. The centre fallback still
		// consumes the one validated mineral source and emits its diagnostic.
		return GetRoadSurfaceElevation(Coord);
	}
	const FVector Centre=Terrain->GridToWorld(Coord);
	const float Offset=Cell.BridgeAxis==1 ? static_cast<float>((Position.X-Centre.X)/Terrain->CellSize) : static_cast<float>((Position.Y-Centre.Y)/Terrain->CellSize);
	const FIntPoint Direction=Cell.BridgeAxis==1 ? FIntPoint(Offset>=0.f ? 1 : -1,0) : FIntPoint(0,Offset>=0.f ? 1 : -1);
	const FIntPoint Neighbor=Coord+Direction;
	if (!Terrain->IsValidCoord(Neighbor) || !Terrain->GetCell(Neighbor).bHasRoad) return Cell.RoadSurfaceElevationCm;
	const FGridCell Other=Terrain->GetCell(Neighbor);
	if (Other.BridgeID!=Cell.BridgeID && !Cell.bBridgeEndpoint) return Cell.RoadSurfaceElevationCm;
	return FMath::Lerp(Cell.RoadSurfaceElevationCm,GetRoadSurfaceElevation(Neighbor),FMath::Clamp(FMath::Abs(Offset),0.f,1.f));
}

bool URoadNetworkSubsystem::IsCellHazardSafe(FIntPoint Coord,bool bVehicle) const
{
	const UTerrainGridSubsystem* Terrain=GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || !Terrain->IsPlayableCoord(Coord)) return false;
	const FGridCell Cell=Terrain->GetCell(Coord);
	if (Cell.bIsOcean || Cell.bBurning || Cell.LavaCoverage01>.1f || Cell.bVolcanicVent) return false;
	return GetRoadFloodDepthCm(Coord)<=(bVehicle ? 35.f : 28.f);
}

float URoadNetworkSubsystem::GetRoadFloodDepthCm(FIntPoint Coord) const
{
	const UTerrainGridSubsystem* Terrain=GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || !Terrain->IsValidCoord(Coord)) return MAX_flt;
	if(const auto* D=GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>();D&&D->IsHydraulicModel2Active())
	{
		double Head=0.,Column=0.;FVector2D Velocity;
		return D->TryGetPhysicalWaterSample(Coord,Head,Column,Velocity)?FMath::Max(0.,Head-GetRoadSurfaceElevation(Coord)):0.f;
	}
	const UDisasterPresentationSubsystem* Water=GetWorld()->GetSubsystem<UDisasterPresentationSubsystem>();
	const float Column=Water ? Water->GetDisplayedFloodDepthCm(Coord) : Terrain->GetCell(Coord).FloodDepth;
	return FMath::Max(0.f,Terrain->GetCell(Coord).Elevation+Column-GetRoadSurfaceElevation(Coord));
}

bool URoadNetworkSubsystem::IsRoadTraversable(FIntPoint Coord) const
{
	const UTerrainGridSubsystem* Terrain=GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || !Terrain->IsPlayableCoord(Coord)) return false;
	const FGridCell Cell=Terrain->GetCell(Coord);
	if (!Cell.bHasRoad || Cell.OccupyingBuildingID!=INDEX_NONE || !IsCellHazardSafe(Coord,true)) return false;
	if (Cell.BridgeID!=INDEX_NONE)
		return (Cell.BridgeAxis==1 || Cell.BridgeAxis==2) && FMath::IsFinite(Cell.RoadSurfaceElevationCm) && !ClosedBridgeIDs.Contains(Cell.BridgeID);
	return !Cell.bIsRiverbed;
}

bool URoadNetworkSubsystem::IsProfileEdgeAllowed(FIntPoint From,FIntPoint To) const
{
	const UTerrainGridSubsystem* Terrain=GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	const FIntPoint Step=To-From;
	if (!Terrain || FMath::Abs(Step.X)+FMath::Abs(Step.Y)!=1) return false;
	const FGridCell A=Terrain->GetCell(From),B=Terrain->GetCell(To);
	if (A.BridgeID==INDEX_NONE && B.BridgeID==INDEX_NONE) return true;
	if (A.BridgeID!=INDEX_NONE && (!SameAxisStep(A.BridgeAxis,Step) && !A.bBridgeEndpoint)) return false;
	if (B.BridgeID!=INDEX_NONE && (!SameAxisStep(B.BridgeAxis,Step) && !B.bBridgeEndpoint)) return false;
	if (A.BridgeID!=B.BridgeID && ((A.BridgeID!=INDEX_NONE && !A.bBridgeEndpoint) || (B.BridgeID!=INDEX_NONE && !B.bBridgeEndpoint))) return false;
	return FMath::Abs(GetRoadSurfaceElevation(From)-GetRoadSurfaceElevation(To))<=Terrain->CellSize*MaximumBridgeGrade+1.f;
}

bool URoadNetworkSubsystem::CanTraverseRoadEdge(FIntPoint From,FIntPoint To) const
{
	return IsRoadTraversable(From) && IsRoadTraversable(To) && IsProfileEdgeAllowed(From,To);
}

bool URoadNetworkSubsystem::IsPublicWalkingCell(FIntPoint Coord) const
{
	const UTerrainGridSubsystem* Terrain=GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || !Terrain->IsPlayableCoord(Coord) || !IsCellHazardSafe(Coord,false)) return false;
	const FGridCell Cell=Terrain->GetCell(Coord);
	if (Cell.BridgeID!=INDEX_NONE) return IsRoadTraversable(Coord);
	if (Cell.bIsRiverbed || Cell.bIsOcean) return false;
	if (Cell.OccupyingBuildingID==INDEX_NONE) return true;
	const UBuildingManagerSubsystem* Buildings=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>(); FBuildingInstance Park;
	if (!Buildings || !Buildings->GetBuildingByID(Cell.OccupyingBuildingID,Park) || !Park.BuildingData || Park.BuildingData->Type!=EBuildingType::GreenSpace || Park.CurrentDamage>=50.f) return false;
	const UCityLifeSubsystem* Life=GetWorld()->GetSubsystem<UCityLifeSubsystem>();
	return !Life || !Life->IsCityInitialized() || Life->GetBuildingOperation01(Park.InstanceID)>.01f;
}

bool URoadNetworkSubsystem::CanTraverseWalkingEdge(FIntPoint From,FIntPoint To) const
{
	return IsPublicWalkingCell(From) && IsPublicWalkingCell(To) && IsProfileEdgeAllowed(From,To);
}

bool URoadNetworkSubsystem::CanAccessRoadFromCell(FIntPoint From,FIntPoint Road) const
{
	const UTerrainGridSubsystem* Terrain=GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || !Terrain->IsPlayableCoord(From) || !IsRoadTraversable(Road)) return false;
	const FGridCell Source=Terrain->GetCell(From),Target=Terrain->GetCell(Road);
	if (IsRoadTraversable(From)) return From==Road || CanTraverseRoadEdge(From,Road);
	if (Source.bIsRiverbed || Source.bIsOcean || Source.bBurning || Source.LavaCoverage01>.1f || Source.bVolcanicVent) return false;
	if (Target.BridgeID!=INDEX_NONE && Source.BridgeID!=Target.BridgeID && !Target.bBridgeEndpoint) return false;
	return IsProfileEdgeAllowed(From,Road);
}

bool URoadNetworkSubsystem::CanBuildAutomaticAccessCell(FIntPoint Coord) const
{
	const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	if(!Terrain||!Terrain->IsPlayableCoord(Coord))return false;
	const FGridCell Cell=Terrain->GetCell(Coord);
	return Cell.OccupyingBuildingID==INDEX_NONE&&!Cell.bHasRoad&&!Cell.bIsOcean&&!Cell.bIsRiverbed
		&&!Cell.bIsEmergencyAssemblyArea&&!Cell.bVolcanicVent&&!Cell.bBurning&&Cell.LavaCoverage01<=.1f
		&&Cell.BridgeID==INDEX_NONE&&FMath::IsFinite(Cell.Elevation)&&IsCellHazardSafe(Coord,true);
}

bool URoadNetworkSubsystem::CanBuildAutomaticAccessEdge(FIntPoint From,FIntPoint To) const
{
	const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	if(!Terrain||!Terrain->IsPlayableCoord(From)||!Terrain->IsPlayableCoord(To)
		||!FMath::IsFinite(Terrain->CellSize)||Terrain->CellSize<=0.f||!IsProfileEdgeAllowed(From,To))return false;
	const float A=GetRoadSurfaceElevation(From),B=GetRoadSurfaceElevation(To);
	// Use the same road/deck profile and grade bound as bridge approaches. An
	// automatic extension never tunnels, fills a river or attaches to a high deck.
	return FMath::IsFinite(A)&&FMath::IsFinite(B)&&FMath::Abs(A-B)<=Terrain->CellSize*MaximumBridgeGrade+1.f;
}

void URoadNetworkSubsystem::NotifyBuildingAccessCommitted()
{
	NotifyNetworkCommitted();
}

void URoadNetworkSubsystem::RebuildBridgeMembership()
{
	BridgeCellsByID.Reset();
	const UTerrainGridSubsystem* Terrain=GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (Terrain) for (const FGridCell& Cell:Terrain->Cells) if (Cell.bHasRoad && Cell.BridgeID!=INDEX_NONE) BridgeCellsByID.FindOrAdd(Cell.BridgeID).Add(Cell.Coords);
}

void URoadNetworkSubsystem::RefreshBridgeClosures()
{
	TSet<int32> NewClosed;
	for (const auto& Bridge:BridgeCellsByID)
		for (FIntPoint Coord:Bridge.Value) if (!IsCellHazardSafe(Coord,true)) { NewClosed.Add(Bridge.Key); break; }
	bool Changed=NewClosed.Num()!=ClosedBridgeIDs.Num();
	if (!Changed) for (int32 ID:NewClosed) if (!ClosedBridgeIDs.Contains(ID)) { Changed=true; break; }
	if (!Changed) return;
	ClosedBridgeIDs=MoveTemp(NewClosed);
	if (UCityLifeSubsystem* Life=GetWorld()->GetSubsystem<UCityLifeSubsystem>(); Life && Life->IsCityInitialized()) Life->RefreshCity();
	OnRoadNetworkChanged.Broadcast();
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_BRIDGE_CLOSURE bridges=%d closed=%d source=DisplayedWaterRelativeToCanonicalDeck extraHazardSolver=0"),BridgeCellsByID.Num(),ClosedBridgeIDs.Num());
}

void URoadNetworkSubsystem::NotifyNetworkCommitted()
{
    FAftershockRoadCpuScope Cpu(TEXT("Road.NotifyNetworkCommitted"));
	RebuildBridgeMembership();
	TSet<int32> NewClosed;
	for (const auto& Bridge:BridgeCellsByID) for (FIntPoint Coord:Bridge.Value)
		if (!IsCellHazardSafe(Coord,true)) { NewClosed.Add(Bridge.Key); break; }
	ClosedBridgeIDs=MoveTemp(NewClosed);
	if (UCityLifeSubsystem* Life=GetWorld()->GetSubsystem<UCityLifeSubsystem>(); Life && Life->IsCityInitialized()) Life->RefreshCity();
	OnRoadNetworkChanged.Broadcast();
}

void URoadNetworkSubsystem::NotifySaveRestored() { NotifyNetworkCommitted(); }

void URoadNetworkSubsystem::HandleTerrainStorageChanged(uint64 Revision)
{
	// A committed resident chunk can join existing streets. Refresh only derived
	// connectivity; citizen IDs, bridge IDs and passenger ledgers are unchanged.
	NotifyNetworkCommitted();
}

bool URoadNetworkSubsystem::TryQuoteBridge(FIntPoint Start,FIntPoint End,FAftershockRoadQuote& Quote) const
{
	const UTerrainGridSubsystem* Terrain=GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || Start==End || (Start.X!=End.X && Start.Y!=End.Y)) return false;
	const FIntPoint Direction(Start.X==End.X ? 0 : End.X>Start.X ? 1 : -1,Start.Y==End.Y ? 0 : End.Y>Start.Y ? 1 : -1);
	const int32 Distance=FMath::Abs(End.X-Start.X)+FMath::Abs(End.Y-Start.Y);
	if (Distance>64) return false;
	bool HasRiver=false,HasExistingBridge=false;
	for (int32 I=0;I<=Distance;++I)
	{
		const FGridCell Cell=Terrain->GetCell(Start+Direction*I);
		HasRiver|=Cell.bIsRiverbed; HasExistingBridge|=Cell.BridgeID!=INDEX_NONE;
	}
	if (!HasRiver || HasExistingBridge) return false;
	Quote.bIncludesBridge=true; Quote.BridgeAxis=Direction.X ? 1 : 2;
	const auto Fail=[&Quote](const TCHAR* Message) { Quote.Feedback=FText::FromString(Message); return true; };
	if (!Terrain->IsPlayableCoord(Start) || !Terrain->IsPlayableCoord(End) || Terrain->GetCell(Start).bIsRiverbed || Terrain->GetCell(End).bIsRiverbed)
		return Fail(TEXT("Köprü için nehrin iki tarafında kuru kara uçları seçin."));
	FIntPoint ExtendedStart=Start,ExtendedEnd=End;
	for (int32 Attempt=0;Attempt<=MaximumApproachCellsPerBank*2;++Attempt)
	{
		const int32 Length=FMath::Abs(ExtendedEnd.X-ExtendedStart.X)+FMath::Abs(ExtendedEnd.Y-ExtendedStart.Y);
		TArray<FIntPoint> Path; Path.Reserve(Length+1);
		int32 FirstRiver=INDEX_NONE,LastRiver=INDEX_NONE,RiverCount=0;
		float HighestRiverBed=-MAX_flt;
		for (int32 I=0;I<=Length;++I)
		{
			const FIntPoint Coord=ExtendedStart+Direction*I;
			if (!Terrain->IsPlayableCoord(Coord)) return Fail(TEXT("Köprü veya eğimli yaklaşımı şehir sınırlarının dışına çıkıyor."));
			const FGridCell Cell=Terrain->GetCell(Coord);
			if (Cell.bIsOcean || Cell.bHasDam || Cell.bVolcanicVent || Cell.bBurning || Cell.LavaCoverage01>.1f || Cell.OccupyingBuildingID!=INDEX_NONE || Cell.BridgeID!=INDEX_NONE || Cell.bIsEmergencyAssemblyArea)
				return Fail(TEXT("Köprü güzergâhında yapı, baraj, toplanma alanı, başka köprü veya etkin afet engeli var."));
			Path.Add(Coord);
			if (Cell.bIsRiverbed)
			{
				if (FirstRiver==INDEX_NONE) FirstRiver=I;
				LastRiver=I; ++RiverCount; HighestRiverBed=FMath::Max(HighestRiverBed,Cell.Elevation);
			}
		}
		Quote.Path=Path;
		if (FirstRiver<=0 || LastRiver>=Length || LastRiver-FirstRiver+1!=RiverCount)
			return Fail(TEXT("Bir köprü yalnız tek ve kesintisiz nehir yatağını iki kuru kıyı arasında aşabilir."));
		if (RiverCount>MaximumRiverSpanCells) return Fail(TEXT("Bu açıklık ilk köprü tasarımının 8 nehir hücresi sınırını aşıyor. Daha dar bir geçit seçin."));
		const int32 LeftBank=FirstRiver-1,RightBank=LastRiver+1;
		if (LeftBank>MaximumApproachCellsPerBank || Length-RightBank>MaximumApproachCellsPerBank)
			return Fail(TEXT("Köprü için her kıyıda en fazla 12 hücrelik yaklaşım seçin; uzun kara yollarını ayrıca inşa edin."));
		const float DeckZ=FMath::Max3(Terrain->GetCell(Path[LeftBank]).Elevation+RoadSurfaceOffsetCm,
			Terrain->GetCell(Path[RightBank]).Elevation+RoadSurfaceOffsetCm,HighestRiverBed+BridgeClearanceAboveBedCm);
		const float LeftGround=Terrain->GetCell(Path[0]).Elevation+RoadSurfaceOffsetCm;
		const float RightGround=Terrain->GetCell(Path.Last()).Elevation+RoadSurfaceOffsetCm;
		const bool LeftTooSteep=LeftBank==0 ? FMath::Abs(DeckZ-LeftGround)>1.f : FMath::Abs(DeckZ-LeftGround)>LeftBank*Terrain->CellSize*MaximumBridgeGrade;
		const bool RightTooSteep=RightBank==Length ? FMath::Abs(DeckZ-RightGround)>1.f : FMath::Abs(DeckZ-RightGround)>(Length-RightBank)*Terrain->CellSize*MaximumBridgeGrade;
		if (LeftTooSteep || RightTooSteep)
		{
			if ((LeftTooSteep && LeftBank>=MaximumApproachCellsPerBank) || (RightTooSteep && Length-RightBank>=MaximumApproachCellsPerBank))
				return Fail(TEXT("Köprü yaklaşımı %12 azami eğim için fazla dik veya uzun. Daha uygun bir kıyı seçin."));
			if (LeftTooSteep) ExtendedStart-=Direction;
			if (RightTooSteep) ExtendedEnd+=Direction;
			continue;
		}
		TArray<float> Heights; Heights.SetNum(Path.Num());
		bool LeftCutsGround=false,RightCutsGround=false;
		for (int32 I=0;I<Path.Num();++I)
		{
			float Z=DeckZ;
			if (I<LeftBank) Z=FMath::Lerp(LeftGround,DeckZ,static_cast<float>(I)/LeftBank);
			else if (I>RightBank) Z=FMath::Lerp(DeckZ,RightGround,static_cast<float>(I-RightBank)/(Length-RightBank));
			Heights[I]=Z;
			if (!Terrain->GetCell(Path[I]).bIsRiverbed && Z<Terrain->GetCell(Path[I]).Elevation+RoadSurfaceOffsetCm-1.f)
			{
				LeftCutsGround|=I<=LeftBank; RightCutsGround|=I>=RightBank;
			}
		}
		if (LeftCutsGround || RightCutsGround)
		{
			if ((LeftCutsGround && LeftBank>=MaximumApproachCellsPerBank) || (RightCutsGround && Length-RightBank>=MaximumApproachCellsPerBank))
				return Fail(TEXT("Rampa mevcut araziyle kesişiyor; araziyi değiştirmeden bu köprü kurulamaz."));
			if (LeftCutsGround) ExtendedStart-=Direction;
			if (RightCutsGround) ExtendedEnd+=Direction;
			continue;
		}
		Quote.Path=MoveTemp(Path); Quote.SurfaceElevationsCm=MoveTemp(Heights);
		Quote.NewRoadCells=0;
		int32 NewLandRoadCells=0;
		for (FIntPoint Coord:Quote.Path)
			if (!Terrain->GetCell(Coord).bHasRoad) { ++Quote.NewRoadCells; if (!Terrain->GetCell(Coord).bIsRiverbed) ++NewLandRoadCells; }
		Quote.BridgeRiverCells=RiverCount; Quote.BridgeApproachCells=Quote.Path.Num()-RiverCount;
		Quote.CostTL=BridgeFoundationCostTL+RiverCount*BridgeDeckCellCostTL+Quote.BridgeApproachCells*BridgeApproachUpgradeCostTL+NewLandRoadCells*GetRoadCellCostTL();
		Quote.bRouteValid=true;
		Quote.Feedback=FText::FromString(FString::Printf(TEXT("KÖPRÜ: %d nehir hücresi + %d kıyı/rampa | %d TL\nYaklaşım dahil; nehir yatağı korunur. Sol tık: inşa et. Sağ tık/Esc: iptal."),RiverCount,Quote.BridgeApproachCells,Quote.CostTL));
		return true;
	}
	return Fail(TEXT("Köprü için uygun eğimli yaklaşım oluşturulamadı."));
}

FAftershockRoadQuote URoadNetworkSubsystem::QuoteRoad(FIntPoint Start, FIntPoint End) const
{
	FAftershockRoadQuote Quote;
	const UWorld* World = GetWorld();
	const UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || !Terrain->IsPlayableCoord(Start) || !Terrain->IsPlayableCoord(End))
	{
		Quote.Feedback = NSLOCTEXT("Aftershock", "RoadOutsideCity", "Yol uçları oynanabilir kara sınırları içinde olmalı.");
		return Quote;
	}
	if (TryQuoteBridge(Start,End,Quote)) return Quote;
	auto IsTraversable = [Terrain,this](FIntPoint Coords)
	{
		if (!Terrain->IsPlayableCoord(Coords)) return false;
		const FGridCell Cell = Terrain->GetCell(Coords);
		return (!Cell.bIsRiverbed || (Cell.BridgeID!=INDEX_NONE && IsRoadTraversable(Coords))) && !Cell.bVolcanicVent && !Cell.bBurning && Cell.LavaCoverage01<=.1f && Cell.OccupyingBuildingID == INDEX_NONE;
	};
	if (!IsTraversable(Start) || !IsTraversable(End))
	{
		Quote.Feedback = NSLOCTEXT("Aftershock", "RoadEndpointBlocked", "Yol için boş kara seçin. Köprü için nehrin karşı kıyısında aynı satır veya sütunda ikinci uç seçin.");
		return Quote;
	}

	// Search resident canonical cells only; storage indices are not global coordinates.
	// It routes around obstacles and never creates roads while calculating a preview.
	const int32 CellCount = Terrain->Cells.Num();
	if (CellCount <= 0) return Quote;
	TArray<int32> Parent;
	Parent.Init(INDEX_NONE, CellCount);
	TArray<FIntPoint> Queue;
	Queue.Reserve(CellCount);
	const int32 StartIndex = Terrain->CoordToIndex(Start);
	const int32 EndIndex = Terrain->CoordToIndex(End);
	if (!Parent.IsValidIndex(StartIndex) || !Parent.IsValidIndex(EndIndex)) return Quote;
	Parent[StartIndex] = StartIndex;
	Queue.Add(Start);
	const FIntPoint Directions[] = {FIntPoint(1, 0), FIntPoint(0, 1), FIntPoint(-1, 0), FIntPoint(0, -1)};
	for (int32 QueueIndex = 0; QueueIndex < Queue.Num() && QueueIndex < CellCount && Parent[EndIndex] == INDEX_NONE; ++QueueIndex)
	{
		const FIntPoint Current = Queue[QueueIndex];
		for (const FIntPoint Direction : Directions)
		{
			const FIntPoint Next = Current + Direction;
			if (!IsTraversable(Next)) continue;
			if (!IsProfileEdgeAllowed(Current,Next)) continue;
			const int32 NextIndex = Terrain->CoordToIndex(Next);
			if (!Parent.IsValidIndex(NextIndex) || Parent[NextIndex] != INDEX_NONE) continue;
			Parent[NextIndex] = Terrain->CoordToIndex(Current);
			Queue.Add(Next);
		}
	}
	if (Parent[EndIndex] == INDEX_NONE)
	{
		Quote.Feedback = NSLOCTEXT("Aftershock", "RoadNoRoute", "Bu uçlar arasında binaları ve nehri aşmayan bir kara güzergâhı bulunamadı.");
		return Quote;
	}
	TArray<FIntPoint> ReversePath;
	for (int32 Index = EndIndex;; Index = Parent[Index])
	{
		ReversePath.Add(Terrain->IndexToCoord(Index));
		if (Index == StartIndex) break;
	}
	Quote.Path.Reserve(ReversePath.Num());
	for (int32 Index = ReversePath.Num() - 1; Index >= 0; --Index)
	{
		const FIntPoint Coords = ReversePath[Index];
		Quote.Path.Add(Coords);
		Quote.SurfaceElevationsCm.Add(GetRoadSurfaceElevation(Coords));
		if (!Terrain->GetCell(Coords).bHasRoad) ++Quote.NewRoadCells;
	}
	Quote.bRouteValid = true;
	Quote.CostTL = Quote.NewRoadCells * GetRoadCellCostTL();
	Quote.Feedback = Quote.NewRoadCells > 0
		? FText::Format(NSLOCTEXT("Aftershock", "RoadQuote", "{0} yeni yol hücresi | {1} TL — Sol tık: inşa et. Sağ tık / Esc: iptal."), FText::AsNumber(Quote.NewRoadCells), FText::AsNumber(Quote.CostTL))
		: NSLOCTEXT("Aftershock", "RoadAlreadyBuilt", "Bu güzergâhta yeni yol hücresi yok. Yeni bir uç nokta seçin.");
	return Quote;
}

bool URoadNetworkSubsystem::BuildRoad(FIntPoint Start, FIntPoint End, FAftershockRoadQuote& OutQuote)
{
    FAftershockRoadCpuScope Cpu(TEXT("Road.BuildRoad"));
	OutQuote = QuoteRoad(Start, End);
	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	UEconomyAndStateSubsystem* Economy = World ? World->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	if (bTransactionInProgress || !Terrain || !Economy || Economy->bIsGameOver || !OutQuote.bRouteValid || OutQuote.NewRoadCells <= 0) return false;
	if (!Economy->CanAfford(OutQuote.CostTL))
	{
		OutQuote.Feedback = FText::Format(NSLOCTEXT("Aftershock", "RoadNoBudget", "Yetersiz bütçe. Yol güzergâhı {0} TL gerektiriyor; hiçbir hücre değiştirilmedi."), FText::AsNumber(OutQuote.CostTL));
		return false;
	}
	TGuardValue<bool> TransactionGuard(bTransactionInProgress, true);
	TArray<FGridCell> OriginalCells;
	OriginalCells.Reserve(OutQuote.Path.Num());
	int32 NewBridgeID=INDEX_NONE;
	if (OutQuote.bIncludesBridge)
	{
		if (OutQuote.SurfaceElevationsCm.Num()!=OutQuote.Path.Num()) return false;
		NewBridgeID=0;
		for (const FGridCell& Cell:Terrain->Cells) if (Cell.BridgeID>=NewBridgeID)
		{
			if (Cell.BridgeID==MAX_int32) { OutQuote.Feedback=FText::FromString(TEXT("Köprü kimlik kapasitesi dolu.")); return false; }
			NewBridgeID=Cell.BridgeID+1;
		}
	}
	for (int32 Index=0;Index<OutQuote.Path.Num();++Index)
	{
		const FIntPoint Coords=OutQuote.Path[Index];
		FGridCell Cell = Terrain->GetCell(Coords);
		if (Cell.bHasRoad && !OutQuote.bIncludesBridge) continue;
		OriginalCells.Add(Cell);
		if (OutQuote.bIncludesBridge)
		{
			Cell.bRoadPredatedBridge=Cell.bHasRoad;
			Cell.BridgeID=NewBridgeID; Cell.BridgeAxis=OutQuote.BridgeAxis;
			Cell.RoadSurfaceElevationCm=OutQuote.SurfaceElevationsCm[Index];
			Cell.bBridgeEndpoint=Index==0 || Index==OutQuote.Path.Num()-1;
		}
		Cell.bHasRoad = true;
		if (!Terrain->SetCell(Coords, Cell))
		{
			for (const FGridCell& Original:OriginalCells) Terrain->SetCell(Original.Coords,Original);
			return false;
		}
	}
	// Commit cells before the economy delegate fires, so observers see a complete purchase.
	// TrySpendMoney cannot fail after the synchronous precheck; retain rollback defensively.
	if (!Economy->TrySpendMoney(OutQuote.CostTL))
	{
		for (const FGridCell& Cell : OriginalCells) Terrain->SetCell(Cell.Coords, Cell);
		OutQuote.Feedback = NSLOCTEXT("Aftershock", "RoadPurchaseRolledBack", "Yol ödemesi reddedildi; güzergâhın tamamı geri alındı.");
		return false;
	}
	NotifyNetworkCommitted();
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_ROAD_BUILT start=%d,%d end=%d,%d newCells=%d cost=%d totalCells=%d bridge=%d bridgeID=%d deckCells=%d approachCells=%d riverDataModified=0"), Start.X, Start.Y, End.X, End.Y, OutQuote.NewRoadCells, OutQuote.CostTL, CountRoadCells(),OutQuote.bIncludesBridge,NewBridgeID,OutQuote.BridgeRiverCells,OutQuote.BridgeApproachCells);
	return true;
}

bool URoadNetworkSubsystem::RemoveRoadAt(FIntPoint Coords)
{
	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	const UEconomyAndStateSubsystem* Economy = World ? World->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	if (bTransactionInProgress || !Terrain || !Terrain->IsPlayableCoord(Coords) || !Economy || Economy->bIsGameOver) return false;
	FGridCell Cell = Terrain->GetCell(Coords);
	if (!Cell.bHasRoad) return false;
	TGuardValue<bool> TransactionGuard(bTransactionInProgress, true);
	if (Cell.BridgeID!=INDEX_NONE)
	{
		const int32 RemovedID=Cell.BridgeID; int32 RemovedCells=0;
		for (FGridCell& SpanCell:Terrain->Cells)
		{
			if (SpanCell.BridgeID!=RemovedID) continue;
			SpanCell.bHasRoad=SpanCell.bRoadPredatedBridge;
			SpanCell.BridgeID=INDEX_NONE; SpanCell.BridgeAxis=0; SpanCell.RoadSurfaceElevationCm=0.f;
			SpanCell.bBridgeEndpoint=false; SpanCell.bRoadPredatedBridge=false; ++RemovedCells;
		}
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_BRIDGE_REMOVED bridge=%d spanCells=%d existingBankRoadsPreserved=1 riverDataModified=0 refundTL=0"),RemovedID,RemovedCells);
	}
	else
	{
		Cell.bHasRoad = false;
		if (!Terrain->SetCell(Coords, Cell)) return false;
	}
	NotifyNetworkCommitted();
	return true;
}

bool URoadNetworkSubsystem::HasRoad(FIntPoint Coords) const
{
	const UWorld* World = GetWorld();
	const UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	return Terrain && Terrain->IsPlayableCoord(Coords) && Terrain->GetCell(Coords).bHasRoad;
}

int32 URoadNetworkSubsystem::CountRoadCells() const
{
	const UWorld* World = GetWorld();
	const UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	int32 Count = 0;
	if (Terrain) for (const FGridCell& Cell : Terrain->Cells) if (Cell.bHasRoad) ++Count;
	return Count;
}

bool URoadNetworkSubsystem::SeedInitialRoads(const TArray<FIntPoint>& AuthoredCells)
{
	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	const UEconomyAndStateSubsystem* Economy = World ? World->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	if (bTransactionInProgress || !Terrain || !Economy || Economy->bIsGameOver || Economy->CurrentTurn != 1
		|| CountRoadCells() != 0 || AuthoredCells.IsEmpty()) return false;
	TSet<FIntPoint> Unique;
	for (FIntPoint Coord : AuthoredCells)
	{
		if (!Terrain->IsPlayableCoord(Coord)) return false;
		const FGridCell Cell = Terrain->GetCell(Coord);
		if (Cell.bIsRiverbed || Cell.bVolcanicVent || Cell.OccupyingBuildingID != INDEX_NONE || Unique.Contains(Coord)) return false;
		Unique.Add(Coord);
	}
	TGuardValue<bool> Guard(bTransactionInProgress, true);
	for (FIntPoint Coord : AuthoredCells)
	{
		FGridCell Cell = Terrain->GetCell(Coord); Cell.bHasRoad = true; Terrain->SetCell(Coord, Cell);
	}
	NotifyNetworkCommitted();
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_STARTER_ROADS cells=%d startingBudget=%lld charged=0 singleCanonicalNetwork=1"), Unique.Num(), Economy->TreasuryTL);
	return true;
}
