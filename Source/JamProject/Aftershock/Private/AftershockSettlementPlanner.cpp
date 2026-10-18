#include "AftershockSettlementPlanner.h"
#include "AftershockSettlementMorphology.h"

#include "Aftershock/Public/AftershockRegionEnvironmentProfile.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "HAL/PlatformTime.h"

namespace
{
	enum class EDistrictUse:uint8 { Centre,MixedResidential,GardenResidential,Agricultural,Employment,Civic };
	struct FPlanSlot { FIntPoint Coord; int32 Priority=0; uint32 Seed=0; };
	struct FPlanBlock
	{
		FIntRect Bounds;
		EDistrictUse Use=EDistrictUse::MixedResidential;
		uint32 Order=0;
		TArray<FPlanSlot> Slots;
		int32 NextSlot=0,Placed=0;
	};
	uint32 Mix(uint32 Value)
	{
		Value^=Value>>16;Value*=0x7feb352du;Value^=Value>>15;
		Value*=0x846ca68bu;return Value^(Value>>16);
	}
	uint32 CellSeed(int32 Seed,FIntPoint Coord,uint32 Salt)
	{
		return Mix(static_cast<uint32>(Seed)^Mix(static_cast<uint32>(Coord.X)+0x9e3779b9u)
			^Mix(static_cast<uint32>(Coord.Y)+0x85ebca6bu)^Salt);
	}
	bool IsLand(const UTerrainGridSubsystem& Terrain,const TSet<FIntPoint>& Land,FIntPoint Coord)
	{
		if(!Land.Contains(Coord)||Coord.X<0||Coord.Y<0||Coord.X>=Terrain.GridWidth||Coord.Y>=Terrain.GridHeight)return false;
		const FGridCell C=Terrain.GetCell(Coord);
		return C.bIsPlayable&&!C.bIsOcean&&!C.bIsRiverbed&&!C.bVolcanicVent&&!C.bHasDam
			&&C.OccupyingBuildingID==INDEX_NONE&&!C.bHasRoad&&!C.bIsEmergencyAssemblyArea
			&&!C.bBurning&&C.LavaCoverage01<=.1f&&FMath::IsFinite(C.Elevation)&&FMath::IsFinite(C.Slope);
	}
	TArray<int32> MakeBlockBreaks(int32 First,int32 Last,int32 Seed,uint32 Salt,bool bRural)
	{
		TArray<int32> Breaks;Breaks.Add(First);
		FRandomStream Random(static_cast<int32>(Mix(static_cast<uint32>(Seed)^Salt)));
		while(Breaks.Last()<Last)
		{
			const int32 Width=(bRural?9:7)+Random.RandRange(0,3)*2;
			int32 Next=FMath::Min(Last,Breaks.Last()+Width);
			if(Last-Next<6)Next=Last;
			Breaks.Add(Next);
		}
		return Breaks;
	}
	bool IsRuralBiome(EAftershockRegionalBiome Biome)
	{
		return Biome==EAftershockRegionalBiome::SemiAridSteppe||Biome==EAftershockRegionalBiome::ContinentalTaiga
			||Biome==EAftershockRegionalBiome::TemperateRainforest||Biome==EAftershockRegionalBiome::SubpolarVolcanic;
	}
	EDistrictUse ChooseUse(EAftershockRegionalBiome Biome,float RelativeDistance,float MeanSlope,
		float CropFraction,float ForestFraction,float CoastFraction,uint32 Seed)
	{
		const uint32 Roll=Seed%100u;
		if(MeanSlope>13.f||ForestFraction>.67f)return EDistrictUse::GardenResidential;
		if(CropFraction>.35f&&RelativeDistance>.3f)return EDistrictUse::Agricultural;
		if(RelativeDistance<.24f)return EDistrictUse::Centre;
		if(CoastFraction>.15f)return Roll<25?EDistrictUse::Civic:EDistrictUse::MixedResidential;
		if(IsRuralBiome(Biome)&&RelativeDistance>.47f&&Roll<47)return EDistrictUse::Agricultural;
		if(RelativeDistance>.4f&&MeanSlope<10.f&&Roll>=77)return EDistrictUse::Employment;
		if(Roll<13)return EDistrictUse::Civic;
		if(Roll<38)return EDistrictUse::GardenResidential;
		return EDistrictUse::MixedResidential;
	}
	EBuildingType PickService(uint32 Roll)
	{
		const EBuildingType Services[]={EBuildingType::School,EBuildingType::Hospital,EBuildingType::FireStation,
			EBuildingType::WaterPlant,EBuildingType::PowerPlant,EBuildingType::Substation,EBuildingType::Warehouse,
			EBuildingType::Bakery,EBuildingType::Restaurant,EBuildingType::PoliceStation,EBuildingType::Shelter,
			EBuildingType::TaxOffice,EBuildingType::BusStop,EBuildingType::WindTurbine,EBuildingType::Courthouse};
		return Services[Roll%UE_ARRAY_COUNT(Services)];
	}
	EBuildingType ChooseBuilding(EDistrictUse Use,uint32 Seed,float Slope)
	{
		const uint32 Roll=Seed%100u;EBuildingType Type=EBuildingType::House;
		switch(Use)
		{
		case EDistrictUse::Centre:
			Type=Roll<50?EBuildingType::Apartment:Roll<65?EBuildingType::Office:Roll<71?EBuildingType::House:Roll<80?EBuildingType::GreenSpace:PickService(Seed>>8);break;
		case EDistrictUse::MixedResidential:
			Type=Roll<32?EBuildingType::Apartment:Roll<59?EBuildingType::House:Roll<70?EBuildingType::Office:Roll<80?EBuildingType::GreenSpace:PickService(Seed>>8);break;
		case EDistrictUse::GardenResidential:
			Type=Roll<54?EBuildingType::House:Roll<65?EBuildingType::Apartment:Roll<82?EBuildingType::GreenSpace:PickService(Seed>>8);break;
		case EDistrictUse::Agricultural:
			Type=Roll<28?EBuildingType::Farm:Roll<39?EBuildingType::LivestockFarm:Roll<61?EBuildingType::House:Roll<67?EBuildingType::Apartment:Roll<76?EBuildingType::Warehouse:Roll<84?EBuildingType::WindTurbine:Roll<90?EBuildingType::GreenSpace:PickService(Seed>>8);break;
		case EDistrictUse::Employment:
			Type=Roll<22?EBuildingType::Factory:Roll<44?EBuildingType::Office:Roll<61?EBuildingType::Warehouse:Roll<68?EBuildingType::PowerPlant:Roll<74?EBuildingType::Substation:Roll<84?EBuildingType::GreenSpace:Roll<93?EBuildingType::House:PickService(Seed>>8);break;
		case EDistrictUse::Civic:
			Type=Roll<24?EBuildingType::GreenSpace:Roll<35?EBuildingType::School:Roll<44?EBuildingType::Hospital:Roll<59?EBuildingType::Office:Roll<75?EBuildingType::Apartment:Roll<82?EBuildingType::House:PickService(Seed>>8);break;
		}
		// A steep garden parcel never receives a tall tower, farm or heavy plant.
		if(Slope>18.f&&(Type==EBuildingType::Apartment||Type==EBuildingType::Office||Type==EBuildingType::Factory||Type==EBuildingType::PowerPlant))Type=EBuildingType::House;
		if(Slope>8.f&&(Type==EBuildingType::Farm||Type==EBuildingType::LivestockFarm))Type=EBuildingType::GreenSpace;
		return Type;
	}
	bool IsEmergency(EBuildingType Type)
	{
		return Type==EBuildingType::Hospital||Type==EBuildingType::School||Type==EBuildingType::FireStation||Type==EBuildingType::Shelter;
	}
	int32 RequestedFloors(EBuildingType Type,EDistrictUse Use,uint32 Seed)
	{
		if(Type==EBuildingType::House)return Use==EDistrictUse::Agricultural?1:1+static_cast<int32>((Seed>>12)%2);
		if(Type==EBuildingType::Apartment)return Use==EDistrictUse::Centre?6+static_cast<int32>((Seed>>12)%5):Use==EDistrictUse::GardenResidential?4:4+static_cast<int32>((Seed>>12)%4);
		if(Type==EBuildingType::Office)return Use==EDistrictUse::Centre?7+static_cast<int32>((Seed>>12)%6):5+static_cast<int32>((Seed>>12)%3);
		if(Type==EBuildingType::Factory)return 2+static_cast<int32>((Seed>>12)%3);
		return 1+static_cast<int32>((Seed>>12)%3);
	}
}

uint32 HashAftershockSettlementPlan(const TArray<FAftershockScenarioLot>& Lots,const TArray<FIntPoint>& Corridors)
{
	uint32 Hash=0x43545932u;
	for(const auto& Lot:Lots)
	{
		Hash=Mix(Hash^static_cast<uint32>(Lot.Coord.X));Hash=Mix(Hash^static_cast<uint32>(Lot.Coord.Y));
		Hash=Mix(Hash^static_cast<uint32>(Lot.Type));Hash=Mix(Hash^static_cast<uint32>(Lot.Floors));Hash=Mix(Hash^static_cast<uint32>(Lot.Foundation));
	}
	for(FIntPoint Coord:Corridors){Hash=Mix(Hash^static_cast<uint32>(Coord.X));Hash=Mix(Hash^static_cast<uint32>(Coord.Y));}
	return Hash;
}

void ExpandAftershockSettlementPlan(const UTerrainGridSubsystem& Terrain,UBuildingManagerSubsystem& Buildings,
	const TSet<FIntPoint>& ConnectedLand,FIntPoint CivicCentre,int32 Seed,int32 RegionIndex,int32 FocusIndex,
	TArray<FAftershockScenarioLot>& Lots,TArray<FIntPoint>& OutPublicCorridors,FAftershockCityPlanStatistics& Stats)
{
	const double Started=FPlatformTime::Seconds();Stats={};Stats.GeneratorVersion=3;Stats.Seed=Seed;OutPublicCorridors.Reset();
	const FAftershockSettlementMorphology Morphology=GetAftershockSettlementMorphology(RegionIndex,FocusIndex);
	Stats.MorphologyId=Morphology.Id;Stats.ProfileMaximumLots=Morphology.MaximumLots;
	const int32 MaximumCityLots=Morphology.MaximumLots;
	TSet<FIntPoint> Used;for(const auto& Lot:Lots)Used.Add(Lot.Coord);
	const auto* Profile=TryGetAftershockRegionEnvironmentProfile(RegionIndex);
	const EAftershockRegionalBiome Biome=Profile?Profile->RegionalBiome:EAftershockRegionalBiome::TemperateUrban;
	FIntRect Bounds(Terrain.GridWidth,Terrain.GridHeight,0,0);
	for(int32 Y=0;Y<Terrain.GridHeight;++Y)for(int32 X=0;X<Terrain.GridWidth;++X)
	{
		++Stats.TerrainCellsVisited;const FIntPoint Coord(X,Y);
		if(!IsLand(Terrain,ConnectedLand,Coord))continue;
		Bounds.Min.X=FMath::Min(Bounds.Min.X,X);Bounds.Min.Y=FMath::Min(Bounds.Min.Y,Y);
		Bounds.Max.X=FMath::Max(Bounds.Max.X,X+1);Bounds.Max.Y=FMath::Max(Bounds.Max.Y,Y+1);
	}
	if(Bounds.Width()<5||Bounds.Height()<5){Stats.ActualLots=Lots.Num();Stats.LayoutHash=HashAftershockSettlementPlan(Lots,OutPublicCorridors);return;}
	const bool bRural=Morphology.bRural||IsRuralBiome(Biome);
	const TArray<int32> XBreaks=MakeBlockBreaks(Bounds.Min.X,Bounds.Max.X,Seed,0x58424c4bu,bRural);
	const TArray<int32> YBreaks=MakeBlockBreaks(Bounds.Min.Y,Bounds.Max.Y,Seed,0x59424c4bu,bRural);
	TArray<FPlanBlock> Blocks;TSet<uint8> UsedDistrictTypes;
	const float Radius=FMath::Max(1.f,FMath::Max(Bounds.Width(),Bounds.Height())*.5f);
	for(int32 BY=0;BY+1<YBreaks.Num();++BY)for(int32 BX=0;BX+1<XBreaks.Num();++BX)
	{
		const FIntRect BlockBounds(XBreaks[BX],YBreaks[BY],XBreaks[BX+1],YBreaks[BY+1]);
		const FIntPoint Centre((BlockBounds.Min.X+BlockBounds.Max.X)/2,(BlockBounds.Min.Y+BlockBounds.Max.Y)/2);
		const uint32 BlockSeed=CellSeed(Seed,BlockBounds.Min,static_cast<uint32>(RegionIndex+1)*0x632be59bu^static_cast<uint32>(FocusIndex+1)*0x45d9f3bu);
		int32 LandCount=0;float MeanSlope=0.f,Crops=0.f,Forest=0.f,Coast=0.f;
		for(int32 Y=BlockBounds.Min.Y;Y<BlockBounds.Max.Y;++Y)for(int32 X=BlockBounds.Min.X;X<BlockBounds.Max.X;++X)
		{
			++Stats.TerrainCellsVisited;const FIntPoint C(X,Y);if(!IsLand(Terrain,ConnectedLand,C))continue;
			const FGridCell Cell=Terrain.GetCell(C);++LandCount;MeanSlope+=Cell.Slope;Crops+=Cell.bAgriculturalLand?1.f:0.f;Forest+=Cell.ForestFuel01;Coast+=Cell.bIsCoastal?1.f:0.f;
		}
		if(LandCount<FMath::Max(9,BlockBounds.Width()*BlockBounds.Height()/3))continue;
		MeanSlope/=LandCount;Crops/=LandCount;Forest/=LandCount;Coast/=LandCount;
		const float RelativeDistance=(FVector2D(Centre)-FVector2D(CivicCentre)).Size()/Radius;
		FPlanBlock& Block=Blocks.AddDefaulted_GetRef();Block.Bounds=BlockBounds;Block.Order=Mix(BlockSeed^0x42524c4bu);
		Block.Use=ChooseUse(Biome,RelativeDistance,MeanSlope,Crops,Forest,Coast,BlockSeed);
		if(Morphology.bRural&&Block.Use==EDistrictUse::Centre)Block.Use=EDistrictUse::GardenResidential;
		if(Morphology.bRural&&Block.Use==EDistrictUse::Employment)Block.Use=Crops>.05f?EDistrictUse::Agricultural:EDistrictUse::GardenResidential;
		if(Morphology.bIndustrial&&MeanSlope<10.f&&RelativeDistance>.3f&&BlockSeed%100u<45u)Block.Use=EDistrictUse::Employment;
		const bool bLoose=Block.Use==EDistrictUse::Agricultural||Block.Use==EDistrictUse::GardenResidential;
		const int32 StepX=FMath::Max(Morphology.LotSpacingCells,bLoose&&((BlockSeed>>3)%3==0)?3:2);
		const int32 StepY=FMath::Max(Morphology.LotSpacingCells,bLoose&&((BlockSeed>>5)%3==0)?3:2);
		const int32 FirstX=BlockBounds.Min.X+1+static_cast<int32>((BlockSeed>>7)%StepX),FirstY=BlockBounds.Min.Y+1+static_cast<int32>((BlockSeed>>11)%StepY);
		const int32 KeepPercent=Morphology.SlotKeepPercent;
		for(int32 Y=FirstY;Y<BlockBounds.Max.Y-1;Y+=StepY)for(int32 X=FirstX;X<BlockBounds.Max.X-1;X+=StepX)
		{
			const FIntPoint Coord(X,Y);const uint32 SlotSeed=CellSeed(Seed,Coord,BlockSeed^0x4c4f5453u);
			if(static_cast<int32>(SlotSeed%100)>=KeepPercent||!IsLand(Terrain,ConnectedLand,Coord))continue;
			const FGridCell Cell=Terrain.GetCell(Coord);
			if(!IsAftershockMorphologyCellSuitable(Morphology,Cell)){++Stats.SteepSlotsRejected;continue;}
			const float DistanceMetres=(FVector2D(Coord)-FVector2D(CivicCentre)).Size()*Terrain.CellSize*.01f;
			if(DistanceMetres>Morphology.SettlementRadiusMetres)continue;
			// Compact village edges thin out rather than filling the entire DEM
			// window. The curve is game authoring, not a measured population map.
			if(Morphology.bCompactCore&&DistanceMetres>Morphology.SettlementRadiusMetres*.55f
				&&((SlotSeed>>20)%100u)>45u)continue;
			bool bSafe=true;
			for(int32 DY=-1;DY<=1&&bSafe;++DY)for(int32 DX=-1;DX<=1;++DX)
			{
				++Stats.FootprintChecks;const FIntPoint N=Coord+FIntPoint(DX,DY);
				if(!IsLand(Terrain,ConnectedLand,N)||!IsAftershockMorphologyCellSuitable(Morphology,Terrain.GetCell(N))||Used.Contains(N)){bSafe=false;break;}
			}
			if(!bSafe)continue;
			FPlanSlot& Slot=Block.Slots.AddDefaulted_GetRef();Slot.Coord=Coord;Slot.Seed=SlotSeed;
			Slot.Priority=static_cast<int32>((SlotSeed>>8)%1000)+FMath::RoundToInt(FMath::Clamp(Cell.ParcelSuitability,0.f,1.f)*650.f)-FMath::RoundToInt(Cell.Slope*8.f);
		}
		Block.Slots.Sort([](const FPlanSlot& A,const FPlanSlot& B){return A.Priority==B.Priority?(A.Coord.Y==B.Coord.Y?A.Coord.X<B.Coord.X:A.Coord.Y<B.Coord.Y):A.Priority>B.Priority;});
		Stats.CandidateSlots+=Block.Slots.Num();
	}
	Stats.Blocks=Blocks.Num();
	Blocks.Sort([](const FPlanBlock& A,const FPlanBlock& B){return A.Order==B.Order?(A.Bounds.Min.Y==B.Bounds.Min.Y?A.Bounds.Min.X<B.Bounds.Min.X:A.Bounds.Min.Y<B.Bounds.Min.Y):A.Order<B.Order;});
	bool bAny=true;
	// Interleave blocks so a bounded city budget does not fill only the first
	// north-west blocks. All stochastic draws are per block/cell, never coupled
	// to skipped cells, hash-set iteration, object IDs or a global RNG stream.
	while(bAny&&Lots.Num()<MaximumCityLots)
	{
		bAny=false;
		for(FPlanBlock& Block:Blocks)
		{
			if(Lots.Num()>=MaximumCityLots)break;
			while(Block.NextSlot<Block.Slots.Num())
			{
				bAny=true;const FPlanSlot& Slot=Block.Slots[Block.NextSlot++];bool bFree=true;
				for(int32 DY=-1;DY<=1&&bFree;++DY)for(int32 DX=-1;DX<=1;++DX)
					if(Used.Contains(Slot.Coord+FIntPoint(DX,DY))){bFree=false;break;}
				if(!bFree)continue;
				const FGridCell Cell=Terrain.GetCell(Slot.Coord);
				const EBuildingType Type=ResolveAftershockMorphologyBuildingType(Morphology,ChooseBuilding(Block.Use,Slot.Seed,Cell.Slope),Slot.Seed);
				const UBuildingData* Data=Buildings.GetCatalogData(Type);if(!Data)continue;
				const int32 FloorCap=FMath::Min(Data->FloorsMax,GetAftershockMorphologyFloorCap(Morphology,Type));
				if(FloorCap<Data->FloorsMin)continue; // Never bypass catalogue minima.
				FAftershockScenarioLot& Lot=Lots.AddDefaulted_GetRef();Lot.Coord=Slot.Coord;Lot.Type=Type;
				Lot.Floors=FMath::Clamp(RequestedFloors(Type,Block.Use,Slot.Seed),Data->FloorsMin,FloorCap);
				Lot.Foundation=IsEmergency(Type)||Lot.Floors>=8||(Cell.SoilType==EAftershockSoilType::Alluvium&&Lot.Floors>=4)?EFoundationQuality::Strong:EFoundationQuality::Medium;
				Used.Add(Slot.Coord);++Block.Placed;UsedDistrictTypes.Add(static_cast<uint8>(Block.Use));break;
			}
		}
	}
	TSet<FIntPoint> Streets;
	for(const FPlanBlock& Block:Blocks)
	{
		if(Block.Placed==0)continue;++Stats.ActiveBlocks;
		const auto AddStreet=[&](FIntPoint Coord)
		{
			if(IsLand(Terrain,ConnectedLand,Coord)&&!Used.Contains(Coord))Streets.Add(Coord);
		};
		// Adjacent blocks share ONE canonical boundary street. Drawing both
		// sides at Max-1/Min doubled every road's width and its instance budget.
		const int32 Right=FMath::Min(Block.Bounds.Max.X,Bounds.Max.X-1),Bottom=FMath::Min(Block.Bounds.Max.Y,Bounds.Max.Y-1);
		for(int32 X=Block.Bounds.Min.X;X<=Right;++X){AddStreet(FIntPoint(X,Block.Bounds.Min.Y));AddStreet(FIntPoint(X,Bottom));}
		for(int32 Y=Block.Bounds.Min.Y;Y<=Bottom;++Y){AddStreet(FIntPoint(Block.Bounds.Min.X,Y));AddStreet(FIntPoint(Right,Y));}
	}
	OutPublicCorridors=Streets.Array();OutPublicCorridors.Sort([](FIntPoint A,FIntPoint B){return A.Y==B.Y?A.X<B.X:A.Y<B.Y;});
	Stats.DistrictUseTypes=UsedDistrictTypes.Num();Stats.PlannedStreetCells=OutPublicCorridors.Num();Stats.ActualLots=Lots.Num();
	for(const auto& Lot:Lots)
	{
		if(Lot.Type==EBuildingType::House||Lot.Type==EBuildingType::Apartment)++Stats.ResidentialLots;
		else if(Lot.Type==EBuildingType::Office||Lot.Type==EBuildingType::Factory||Lot.Type==EBuildingType::Warehouse)++Stats.WorkLots;
		else if(Lot.Type==EBuildingType::Farm||Lot.Type==EBuildingType::LivestockFarm||Lot.Type==EBuildingType::Bakery||Lot.Type==EBuildingType::Restaurant)++Stats.FoodLots;
		else if(Lot.Type==EBuildingType::GreenSpace)++Stats.OpenSpaceLots;
	}
	Stats.LayoutHash=HashAftershockSettlementPlan(Lots,OutPublicCorridors);Stats.PlannerCpuMilliseconds=(FPlatformTime::Seconds()-Started)*1000.;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SETTLEMENT_PLAN version=3 seed=%d region=%d focus=%d morphology=%s biome=%d blocks=%d activeBlocks=%d useTypes=%d candidateSlots=%d steepRejected=%d actualLots=%d authoredUpperBound=%d streetCorridors=%d residential=%d work=%d food=%d openSpace=%d terrainVisits=%d footprintChecks=%d plannerCpuMs=%.3f layoutHash=%u singleCanonicalPopulation=1 perPropActors=0 initialFrameOnly=1 sourceTerrainChanged=0 censusOrZoningReconstruction=0"),
		Seed,RegionIndex,FocusIndex,Morphology.Id,static_cast<int32>(Biome),Stats.Blocks,Stats.ActiveBlocks,Stats.DistrictUseTypes,Stats.CandidateSlots,Stats.SteepSlotsRejected,Stats.ActualLots,MaximumCityLots,Stats.PlannedStreetCells,Stats.ResidentialLots,Stats.WorkLots,Stats.FoodLots,Stats.OpenSpaceLots,Stats.TerrainCellsVisited,Stats.FootprintChecks,Stats.PlannerCpuMilliseconds,Stats.LayoutHash);
}
