// Immutable authored-generation compatibility implementation copied from0152f4d.
// V3 real-place new cities use AftershockSettlementPlanner instead.
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Engine/World.h"

void UAftershockScenarioSubsystem::BuildLegacyDesignedLots()
{
	UTerrainGridSubsystem* Terrain = GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
	UBuildingManagerSubsystem* Buildings=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();
	DesignedLots.Reset(); AssemblyCells.Reset();
	int32 RiverX = Terrain->GridWidth / 2;
	const bool bCityDistricts = Terrain->GridWidth > 50 && Terrain->CellSize >= 999.f;
	const int32 CentreRow=Terrain->GridHeight/2;
	for (int32 X = 0; X < Terrain->GridWidth; ++X) if (Terrain->GetCell(FIntPoint(X, CentreRow)).bIsRiverbed) { RiverX = X; break; }
	const FIntPoint Centre(FMath::Clamp(RiverX - (ScenarioIndex == 1 ? 4 : 7), 10, bCityDistricts ? Terrain->GridWidth-10 : 31), CentreRow);
	const FIntPoint Directions[] = { FIntPoint(0,1), FIntPoint(1,0), FIntPoint(-1,0), FIntPoint(0,-1) };
	FIntPoint BankAnchor(INDEX_NONE, INDEX_NONE);
	int32 NearestDistance = MAX_int32;
	for (const FGridCell& Cell : Terrain->Cells)
	{
		if (!Cell.bIsPlayable || Cell.bIsRiverbed || Cell.bVolcanicVent) continue;
		const int32 Distance = FMath::Square(Cell.Coords.X - Centre.X) + FMath::Square(Cell.Coords.Y - Centre.Y);
		if (Distance < NearestDistance) { BankAnchor = Cell.Coords; NearestDistance = Distance; }
	}
	TSet<FIntPoint> BankCells;
	TArray<FIntPoint> BankQueue;
	if (BankAnchor.X != INDEX_NONE) { BankQueue.Add(BankAnchor); BankCells.Add(BankAnchor); }
	for (int32 Head = 0; Head < BankQueue.Num(); ++Head)
	{
		for (const FIntPoint Direction : Directions)
		{
			const FIntPoint Coord = BankQueue[Head] + Direction;
			if (!Terrain->IsPlayableCoord(Coord) || Terrain->GetCell(Coord).bIsRiverbed || BankCells.Contains(Coord)) continue;
			BankCells.Add(Coord); BankQueue.Add(Coord);
		}
	}
	TArray<FIntPoint> Candidates;
	for (const FGridCell& Cell : Terrain->Cells)
	{
		if (!BankCells.Contains(Cell.Coords) || Cell.bVolcanicVent || Cell.Coords.X % 2 != Centre.X % 2 || Cell.Coords.Y % 2 != 1) continue;
		// An eight-neighbour land ring remains free under the two-cell lot parity.
		// Removing any authored lot therefore cannot cut a narrow river-bank route.
		bool bHasPublicAccessRing = true;
		for (int32 DY = -1; DY <= 1 && bHasPublicAccessRing; ++DY)
			for (int32 DX = -1; DX <= 1; ++DX)
				if (!BankCells.Contains(Cell.Coords + FIntPoint(DX, DY))) { bHasPublicAccessRing = false; break; }
		if (bHasPublicAccessRing) Candidates.Add(Cell.Coords);
	}
	TSet<FIntPoint> Used;
	const auto AddLot = [&](EBuildingType Type, FIntPoint Desired, int32 Floors, bool bEmergency)
	{
		const UBuildingData* Catalog=Buildings ? Buildings->GetCatalogData(Type) : nullptr;
		if (!Catalog)
		{
			UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_SCENARIO_LOT_MISSING_CATALOG type=%d authoredLotNotCreated=1"),static_cast<int32>(Type));
			return;
		}
		// Exactly the same catalogue floor limits used by PlaceBuilding. The
		// authored plan must describe the resulting structure before seeding it.
		const int32 CanonicalFloors=FMath::Clamp(Floors,Catalog->FloorsMin,Catalog->FloorsMax);
		float MinimumShelterElevation=-MAX_flt;
		if (!bCityDistricts && ScenarioIndex==1 && Type==EBuildingType::Shelter)
		{
			for (const FAftershockScenarioLot& Existing:DesignedLots)
				if (Existing.Foundation==EFoundationQuality::Strong)
					MinimumShelterElevation=FMath::Max(MinimumShelterElevation,Terrain->GetCell(Existing.Coord).Elevation);
			// Fixed flood neighbourhood: a shelter belongs above the existing
			// dry public centres, not in the nearby low hollow. This authored
			// siting margin is not a measured flood-level/engineering guarantee.
			if (MinimumShelterElevation>-MAX_flt) MinimumShelterElevation+=20.f;
		}
		FIntPoint Best(INDEX_NONE, INDEX_NONE); double BestScore = TNumericLimits<double>::Max();
		for (FIntPoint Coord : Candidates)
		{
			if (Used.Contains(Coord)) continue;
			const FGridCell Cell = Terrain->GetCell(Coord);
			if (Cell.Elevation<MinimumShelterElevation) continue;
			const double D2 = static_cast<double>(FMath::Square(Coord.X - Desired.X) + FMath::Square(Coord.Y - Desired.Y));
			const double Score = D2 * 20.0 + (bEmergency ? -Cell.Elevation * .35 + Cell.RiverProximity * 180.0 : 0.0);
			if (Score < BestScore) { Best = Coord; BestScore = Score; }
		}
		if (Best.X == INDEX_NONE) return;
		Used.Add(Best);
		FAftershockScenarioLot& Lot = DesignedLots.AddDefaulted_GetRef();
		Lot.Coord = Best; Lot.Type = Type; Lot.Floors = CanonicalFloors;
		Lot.Foundation = bEmergency ? EFoundationQuality::Strong : EFoundationQuality::Medium;
		if (Floors!=CanonicalFloors)
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SCENARIO_LOT_CANONICAL_FLOORS type=%d requested=%d authored=%d catalogueMin=%d catalogueMax=%d"),static_cast<int32>(Type),Floors,CanonicalFloors,Catalog->FloorsMin,Catalog->FloorsMax);
		if (Type==EBuildingType::Shelter)
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SHELTER_SITING coord=%s elevationCm=%.3f minimumAuthoredElevationCm=%.3f strongFoundation=%d sameConnectedLandBank=1 publicAccessRing=1"),*Best.ToString(),Terrain->GetCell(Best).Elevation,MinimumShelterElevation,bEmergency);
	};
	// Schools/hospital/fire station stand on higher ground; residential blocks
	// leave a free grid lane between every pair of lots for player-built roads.
	AddLot(EBuildingType::Hospital, Centre + FIntPoint(-2, -6), 3, true);
	AddLot(EBuildingType::School, Centre + FIntPoint(2, -6), 2, true);
	AddLot(EBuildingType::FireStation, Centre + FIntPoint(-4, 2), 2, true);
	for (int32 I = 0; I < 12; ++I)
	{
		FIntPoint Desired = Centre + FIntPoint(-4 + (I % 4) * 2, -2 + (I / 4) * 2);
		// The coastal expansion retains real empty-land access rings. Four existing
		// cottages form a shore district while emergency centres stay inland.
		if (ScenarioIndex == 3 && I >= 8) Desired.Y = FMath::RoundToInt(Terrain->GridHeight*.66f) + ((I - 8) / 2) * 2;
		AddLot(EBuildingType::House, Desired, 1 + I % 2, false);
	}
	for (int32 I = 0; I < 8; ++I) AddLot(EBuildingType::Apartment, Centre + FIntPoint(-4 + (I % 4) * 2, 4 + (I / 4) * 2), 4 + I % 3, false);
	for (int32 I = 0; I < 3; ++I) AddLot(EBuildingType::Office, Centre + FIntPoint(4, -4 + I * 2), 5 + I, false);
	AddLot(EBuildingType::Factory, Centre + FIntPoint(4, 6), 2, false);
	AddLot(EBuildingType::GreenSpace, Centre + FIntPoint(0, -4), 1, false);
	// A functioning starter economy, with ONE population authority in CityLife.
	// Job capacities are services, never extra residents or disaster victims.
	const EBuildingType StarterServices[] = { EBuildingType::Farm, EBuildingType::LivestockFarm,
		EBuildingType::Bakery, EBuildingType::Restaurant, EBuildingType::PowerPlant,
		EBuildingType::Generator, EBuildingType::TaxOffice, EBuildingType::Courthouse,
		EBuildingType::PoliceStation, EBuildingType::BusTerminal, EBuildingType::BusStop,
		EBuildingType::BusStop, EBuildingType::Shelter, EBuildingType::WaterPlant,
		EBuildingType::Warehouse, EBuildingType::Substation };
	for (int32 I=0;I<UE_ARRAY_COUNT(StarterServices);++I)
	{
		FIntPoint Desired=Centre+FIntPoint(-8+(I%5)*2,-10+(I/5)*2);
		if (I==10) Desired=Centre+FIntPoint(-4,2); // Residential frontage stop.
		if (I==11) Desired=Centre+FIntPoint(-4,-10); // Distinct employment/services stop.
		AddLot(StarterServices[I],Desired,1,StarterServices[I]==EBuildingType::Shelter);
	}
	if (bCityDistricts)
	{
		// Real canonical lots, jobs and residents. The 1,500 crowd draw budget is
		// deliberately unrelated to city size. Each neighbourhood repeats a
		// service mix, rather than placing every person at one starter portal.
		const EBuildingType Mix[]={
			EBuildingType::Apartment,EBuildingType::Apartment,EBuildingType::Apartment,EBuildingType::Apartment,
			EBuildingType::Apartment,EBuildingType::Apartment,EBuildingType::Apartment,EBuildingType::Apartment,
			EBuildingType::Apartment,EBuildingType::Apartment,EBuildingType::Apartment,EBuildingType::Apartment,
			EBuildingType::Apartment,EBuildingType::Apartment,EBuildingType::Apartment,EBuildingType::Apartment,
			EBuildingType::Apartment,EBuildingType::Apartment,EBuildingType::Apartment,EBuildingType::Apartment,
			EBuildingType::House,EBuildingType::House,EBuildingType::House,EBuildingType::House,EBuildingType::House,EBuildingType::House,
			EBuildingType::Office,EBuildingType::Office,EBuildingType::Office,EBuildingType::Office,EBuildingType::Office,EBuildingType::Office,
			EBuildingType::Factory,EBuildingType::Hospital,EBuildingType::Hospital,EBuildingType::School,EBuildingType::FireStation,
			EBuildingType::Farm,EBuildingType::Farm,EBuildingType::Bakery,EBuildingType::Restaurant,EBuildingType::WaterPlant,
			EBuildingType::PowerPlant,EBuildingType::PowerPlant,EBuildingType::WindTurbine,EBuildingType::Substation,EBuildingType::Warehouse,
			EBuildingType::PoliceStation,EBuildingType::TaxOffice,EBuildingType::Courthouse,EBuildingType::Shelter,EBuildingType::BusStop,EBuildingType::GreenSpace};
		FIntRect DistrictBounds(Candidates.IsEmpty()?Centre:Candidates[0],Candidates.IsEmpty()?Centre+FIntPoint(1,1):Candidates[0]+FIntPoint(1,1));
		for(FIntPoint Coord:Candidates)
		{
			DistrictBounds.Min.X=FMath::Min(DistrictBounds.Min.X,Coord.X);DistrictBounds.Min.Y=FMath::Min(DistrictBounds.Min.Y,Coord.Y);
			DistrictBounds.Max.X=FMath::Max(DistrictBounds.Max.X,Coord.X+1);DistrictBounds.Max.Y=FMath::Max(DistrictBounds.Max.Y,Coord.Y+1);
		}
		constexpr int32 DistrictColumns=4,DistrictRows=5,DistrictCount=DistrictColumns*DistrictRows;
		const int32 Target=FMath::Min(1000,Candidates.Num()*9/10);
		for(int32 Index=0;DesignedLots.Num()<Target && Index<1000;++Index)
		{
			const int32 District=Index%DistrictCount,Local=Index/DistrictCount;
			const FIntPoint DistrictCentre(DistrictBounds.Min.X+(2*(District%DistrictColumns)+1)*DistrictBounds.Width()/(DistrictColumns*2),
				DistrictBounds.Min.Y+(2*(District/DistrictColumns)+1)*DistrictBounds.Height()/(DistrictRows*2));
			const EBuildingType Type=Mix[(Local+District*7)%UE_ARRAY_COUNT(Mix)];
			const FIntPoint Desired=DistrictCentre+FIntPoint(((Local*3+District)%7-3)*2,((Local/7)%7-3)*2);
			const UBuildingData* Catalog=Buildings->GetCatalogData(Type);
			const int32 RequestedFloors=Type==EBuildingType::Apartment?4+(District+Local)%5:Type==EBuildingType::Office?5+(District+Local)%7:Type==EBuildingType::House?1+(Local%2):1;
			const int32 Floors=Catalog?FMath::Clamp(RequestedFloors,Catalog->FloorsMin,Catalog->FloorsMax):RequestedFloors;
			AddLot(Type,Desired,Floors,Type==EBuildingType::Hospital||Type==EBuildingType::School||Type==EBuildingType::Shelter);
		}
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITY_DISTRICTS width=%d height=%d cellCm=%.0f targetLots=%d actualLots=%d plannedDistricts=%d cityPopulationIsNotDrawBudget=1"),Terrain->GridWidth,Terrain->GridHeight,Terrain->CellSize,Target,DesignedLots.Num(),DistrictCount);
	}
	for (int32 I = 0; I < FMath::Min(3, DesignedLots.Num()); ++I)
	{
		for (FIntPoint Direction : Directions)
		{
			const FIntPoint Coord = DesignedLots[I].Coord + Direction;
			if (Terrain->IsPlayableCoord(Coord) && !Terrain->GetCell(Coord).bIsRiverbed && !Used.Contains(Coord) && !AssemblyCells.Contains(Coord))
			{
				AssemblyCells.Add(Coord);
				FGridCell Cell = Terrain->GetCell(Coord); Cell.bIsEmergencyAssemblyArea = true; Terrain->SetCell(Coord, Cell);
				break;
			}
		}
	}
	if (!bCityDistricts && ScenarioIndex == 1 && Definition.Seed == 7447 && !AssemblyCells.IsEmpty())
	{
		// Four existing cottages deliberately occupy the fixed scenario's low bank.
		// These authored coordinates are not generated by a hazard/display solver.
		// They are narrow-frontage exceptions to the main district's 3x3 land ring.
		const FIntPoint RiversideCottages[] = { FIntPoint(26,21), FIntPoint(25,19), FIntPoint(24,17), FIntPoint(22,13) };
		TArray<int32> HouseIndices;
		for (int32 I = 0; I < DesignedLots.Num(); ++I)
			if (DesignedLots[I].Type == EBuildingType::House) HouseIndices.Add(I);
		int32 RelocatedCottages = 0;
		for (int32 I = 0; I < UE_ARRAY_COUNT(RiversideCottages) && I < HouseIndices.Num(); ++I)
		{
			const FIntPoint Proposed = RiversideCottages[I];
			if (!Terrain->IsPlayableCoord(Proposed) || !BankCells.Contains(Proposed) || Used.Contains(Proposed)
				|| Terrain->GetCell(Proposed).bIsRiverbed || Terrain->GetCell(Proposed).OccupyingBuildingID != INDEX_NONE
				|| Terrain->GetCell(Proposed).bIsEmergencyAssemblyArea || AssemblyCells.Contains(Proposed)) continue;
			const int32 HouseIndex = HouseIndices[I];
			TSet<FIntPoint> TrialOccupied = Used;
			TrialOccupied.Remove(DesignedLots[HouseIndex].Coord);
			TrialOccupied.Add(Proposed);
			TSet<FIntPoint> Accessible;
			TArray<FIntPoint> WalkQueue;
			Accessible.Add(AssemblyCells[0]); WalkQueue.Add(AssemblyCells[0]);
			for (int32 Head = 0; Head < WalkQueue.Num(); ++Head)
			{
				for (FIntPoint Direction : Directions)
				{
					const FIntPoint Coord = WalkQueue[Head] + Direction;
					if (!Terrain->IsPlayableCoord(Coord) || Terrain->GetCell(Coord).bIsRiverbed
						|| TrialOccupied.Contains(Coord) || Accessible.Contains(Coord)) continue;
					Accessible.Add(Coord); WalkQueue.Add(Coord);
				}
			}
			bool bPreservesPublicAccess = true;
			for (FIntPoint Assembly : AssemblyCells) bPreservesPublicAccess &= Accessible.Contains(Assembly);
			for (int32 LotIndex = 0; LotIndex < DesignedLots.Num() && bPreservesPublicAccess; ++LotIndex)
			{
				const FIntPoint LotCoord = LotIndex == HouseIndex ? Proposed : DesignedLots[LotIndex].Coord;
				bool bHasConnectedFrontage = false;
				for (FIntPoint Direction : Directions) bHasConnectedFrontage |= Accessible.Contains(LotCoord + Direction);
				bPreservesPublicAccess &= bHasConnectedFrontage;
			}
			if (!bPreservesPublicAccess)
			{
				UE_LOG(LogTemp, Warning, TEXT("AFTERSHOCK_RIVERSIDE_COTTAGE_REJECTED coord=%s reason=DisconnectedPublicFrontage"), *Proposed.ToString());
				continue;
			}
			const FIntPoint Previous = DesignedLots[HouseIndex].Coord;
			Used = MoveTemp(TrialOccupied);
			DesignedLots[HouseIndex].Coord = Proposed;
			++RelocatedCottages;
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_RIVERSIDE_COTTAGE old=%s coord=%s elevationCm=%.2f riverDistanceCm=%.2f allLotFrontagesConnected=1 populationAdded=0"),
				*Previous.ToString(), *Proposed.ToString(), Terrain->GetCell(Proposed).Elevation, Terrain->GetCell(Proposed).RiverDistance);
		}
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_RIVERSIDE_LAYOUT cottages=%d totalLots=%d emergencyCentresMoved=0 automaticRoads=0"), RelocatedCottages, DesignedLots.Num());
	}
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_SCENARIO_ACCESS seed=%d connectedBankCells=%d servicedLotCandidates=%d authoredLots=%d assemblyCells=%d automaticRoads=0"),
		Definition.Seed, BankCells.Num(), Candidates.Num(), DesignedLots.Num(), AssemblyCells.Num());
}
