#include "Aftershock/World/GridManager.h"
#include "Aftershock/Buildings/BuildingActor.h"
#include "Aftershock/Data/BuildingData.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Components/InstancedStaticMeshComponent.h"

AGridManager::AGridManager()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AGridManager::BeginPlay()
{
	Super::BeginPlay();

	UWorld* World = GetWorld();
	UTerrainGridSubsystem* TerrainSubsystem = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;

	if (TerrainSubsystem)
	{
		// Subsystem'den grid parametrelerini senkronize et
		GridSize = FIntPoint(TerrainSubsystem->GridWidth, TerrainSubsystem->GridHeight);
		CellSizeCm = TerrainSubsystem->CellSize;
		GridOrigin = TerrainSubsystem->GridOrigin;
	}

	if (Cells.Num() == 0)
	{
		InitializeFlatTerrain();
	}
}

int32 AGridManager::CoordToIndex(FIntPoint Coord) const
{
	return Coord.X + Coord.Y * GridSize.X;
}

FVector AGridManager::CoordToWorld(FIntPoint Coord) const
{
	if (UWorld* World = GetWorld())
	{
		if (UTerrainGridSubsystem* TerrainSubsystem = World->GetSubsystem<UTerrainGridSubsystem>())
		{
			return TerrainSubsystem->GridToWorld(Coord, true);
		}
	}

	return GridOrigin + FVector(Coord.X * CellSizeCm + CellSizeCm * 0.5f,
	                            Coord.Y * CellSizeCm + CellSizeCm * 0.5f,
	                            0.f);
}

FIntPoint AGridManager::WorldToCoord(FVector World) const
{
	if (UWorld* W = GetWorld())
	{
		if (UTerrainGridSubsystem* TerrainSubsystem = W->GetSubsystem<UTerrainGridSubsystem>())
		{
			return TerrainSubsystem->WorldToGrid(World);
		}
	}

	const FVector Local = World - GridOrigin;
	return FIntPoint(FMath::FloorToInt(Local.X / CellSizeCm),
	                 FMath::FloorToInt(Local.Y / CellSizeCm));
}

bool AGridManager::IsCoordValid(FIntPoint Coord) const
{
	if (UWorld* World = GetWorld())
	{
		if (UTerrainGridSubsystem* TerrainSubsystem = World->GetSubsystem<UTerrainGridSubsystem>())
		{
			return TerrainSubsystem->IsValidCoord(Coord);
		}
	}

	return Coord.X >= 0 && Coord.X < GridSize.X && Coord.Y >= 0 && Coord.Y < GridSize.Y;
}

bool AGridManager::IsCoordFree(FIntPoint Coord) const
{
	if (UWorld* World = GetWorld())
	{
		if (UTerrainGridSubsystem* TerrainSubsystem = World->GetSubsystem<UTerrainGridSubsystem>())
		{
			if (!TerrainSubsystem->IsPlayableCoord(Coord))
			{
				return false;
			}
			const FGridCell Cell = TerrainSubsystem->GetCell(Coord);
			if (Cell.OccupyingBuildingID != -1 || Cell.bIsRiverbed || Cell.bHasRoad || Cell.bIsEmergencyAssemblyArea || Cell.bVolcanicVent)
			{
				return false;
			}
		}
	}

	return IsCoordValid(Coord) && !BuildingByCoord.Contains(Coord);
}

FTerrainCell AGridManager::GetCell(FIntPoint Coord) const
{
	if (UWorld* World = GetWorld())
	{
		if (UTerrainGridSubsystem* TerrainSubsystem = World->GetSubsystem<UTerrainGridSubsystem>())
		{
			if (TerrainSubsystem->IsValidCoord(Coord))
			{
				const FGridCell GridCell = TerrainSubsystem->GetCell(Coord);
				FTerrainCell OutCell;
				OutCell.GridCoord = Coord;
				OutCell.Elevation = GridCell.Elevation;
				OutCell.RiverProximity = GridCell.RiverProximity;
				OutCell.ValleyChannelFactor = GridCell.ValleyWindMultiplier;
				OutCell.FaultLineProximity = GridCell.FaultLineProximity;
				OutCell.FloodDepth = GridCell.FloodDepth;
				OutCell.bBurning = GridCell.bBurning;
				OutCell.BurningTurns = GridCell.BurningTurns;
				return OutCell;
			}
		}
	}

	if (!IsCoordValid(Coord) || Cells.Num() == 0) return FTerrainCell();
	return Cells[CoordToIndex(Coord)];
}

void AGridManager::SetCell(FIntPoint Coord, const FTerrainCell& Cell)
{
	if (UWorld* World = GetWorld())
	{
		if (UTerrainGridSubsystem* TerrainSubsystem = World->GetSubsystem<UTerrainGridSubsystem>())
		{
			if (TerrainSubsystem->IsValidCoord(Coord))
			{
				FGridCell GridCell = TerrainSubsystem->GetCell(Coord);
				GridCell.Elevation = Cell.Elevation;
				GridCell.RiverProximity = Cell.RiverProximity;
				GridCell.ValleyWindMultiplier = Cell.ValleyChannelFactor;
				GridCell.FaultLineProximity = Cell.FaultLineProximity;
				GridCell.FloodDepth = Cell.FloodDepth;
				GridCell.bBurning = Cell.bBurning;
				GridCell.BurningTurns = Cell.BurningTurns;
				TerrainSubsystem->SetCell(Coord, GridCell);
			}
		}
	}

	if (IsCoordValid(Coord) && Cells.IsValidIndex(CoordToIndex(Coord)))
	{
		Cells[CoordToIndex(Coord)] = Cell;
		Cells[CoordToIndex(Coord)].GridCoord = Coord;
	}
}

ABuildingActor* AGridManager::PlaceBuilding(UBuildingData* BuildingData, FIntPoint Coord, TSubclassOf<ABuildingActor> ActorClass)
{
	return PlaceBuildingWithOptions(BuildingData, Coord, ActorClass, EFoundationQuality::Medium, BuildingData ? BuildingData->AverageFloors() : 1);
}

ABuildingActor* AGridManager::PlaceBuildingWithOptions(UBuildingData* BuildingData, FIntPoint Coord, TSubclassOf<ABuildingActor> ActorClass, EFoundationQuality Foundation, int32 Floors, float Yaw)
{
	if (!BuildingData || !ActorClass || !FMath::IsFinite(Yaw) || !IsCoordFree(Coord))
	{
		return nullptr;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	UBuildingManagerSubsystem* BuildingMgr = World->GetSubsystem<UBuildingManagerSubsystem>();
	UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>();

	if (!Economy || !BuildingMgr || Economy->bIsGameOver || !BuildingMgr->CanPlaceBuildingAt(Coord, BuildingData)) return nullptr;
	Floors = FMath::Clamp(Floors, BuildingData->FloorsMin, BuildingData->FloorsMax);
	const int32 Cost = BuildingMgr->CalculateConstructionCost(BuildingData, Foundation, Floors);
	if (!Economy->CanAfford(Cost)) return nullptr;

	// Blueprint compatibility returns an actor handle, but the canonical ISM and
	// economy transaction remain the only placement and rendering authorities.
	// Finish construction before spending: a failed actor construction cannot
	// leave a paid canonical lot or an unregistered, visible duplicate building.
	const FTransform SpawnTransform(FRotator::ZeroRotator, CoordToWorld(Coord));
	ABuildingActor* Building = World->SpawnActorDeferred<ABuildingActor>(ActorClass, SpawnTransform, this, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Building) return nullptr;

	Building->Data = BuildingData;
	Building->BuildingData = BuildingData;
	Building->Floors = Floors;
	Building->CurrentFloors = Floors;
	Building->Foundation = Foundation;
	Building->FoundationQuality = Foundation;
	Building->GridCoord = Coord;
	Building->FinishSpawning(SpawnTransform);
	if (!IsValid(Building)) return nullptr;
	Building->SetActorHiddenInGame(true);
	Building->SetActorEnableCollision(false);
	Building->SetActorTickEnabled(false);
	if (!Economy->BuildBuildingWithBudget(Coord, BuildingData, Foundation, Floors))
	{
		Building->Destroy();
		return nullptr;
	}
	FBuildingInstance Placed;
	if (!ensureMsgf(BuildingMgr->GetBuildingAtCoords(Coord, Placed), TEXT("Successful canonical placement must expose its registered building")))
	{
		Building->Destroy();
		return nullptr;
	}
	Building->BuildingID = Placed.InstanceID;
	const FRotator SelectedRotation(0.f, FRotator::NormalizeAxis(Yaw), 0.f);
	Building->SetActorRotation(SelectedRotation);
	if (UInstancedStaticMeshComponent* ISM = BuildingMgr->GetOrCreateISM(Placed.BuildingData))
	{
		FTransform InstanceTransform;
		if (ISM->GetInstanceTransform(Placed.ISMInstanceIndex, InstanceTransform, true))
		{
			InstanceTransform.SetRotation(SelectedRotation.Quaternion());
			ISM->UpdateInstanceTransform(Placed.ISMInstanceIndex, InstanceTransform, true, true, true);
		}
	}

	BuildingByCoord.Add(Coord, Building);
	AllBuildings.Add(Building);

	return Building;
}

bool AGridManager::DemolishBuilding(FIntPoint Coord)
{
	TObjectPtr<ABuildingActor>* Found = BuildingByCoord.Find(Coord);
	if (!Found || !*Found) return false;

	ABuildingActor* Building = Found->Get();
	const int32 BId = Building->BuildingID;
	bool bRemovedCanonicalBuilding = false;
	UWorld* World = GetWorld();
	if (World)
	{
		if (UBuildingManagerSubsystem* BuildingMgr = World->GetSubsystem<UBuildingManagerSubsystem>())
		{
			FBuildingInstance CanonicalBuilding;
			if (BId != -1 && BuildingMgr->GetBuildingByID(BId, CanonicalBuilding) && CanonicalBuilding.GridCoords == Coord)
			{
				// Canonical removal clears the cell only when that same ID still
				// occupies it. A stale Blueprint handle must never clear a newer lot.
				bRemovedCanonicalBuilding = BuildingMgr->RemoveBuilding(BId);
			}
		}
	}

	AllBuildings.Remove(Building);
	BuildingByCoord.Remove(Coord);
	Building->Destroy();
	return bRemovedCanonicalBuilding;
}

ABuildingActor* AGridManager::GetBuildingAt(FIntPoint Coord) const
{
	const TObjectPtr<ABuildingActor>* Found = BuildingByCoord.Find(Coord);
	return Found ? Found->Get() : nullptr;
}

void AGridManager::InitializeFlatTerrain()
{
	const int32 Total = GridSize.X * GridSize.Y;
	Cells.Reset();
	Cells.AddDefaulted(Total);
	for (int32 Y = 0; Y < GridSize.Y; ++Y)
	{
		for (int32 X = 0; X < GridSize.X; ++X)
		{
			FTerrainCell& Cell = Cells[X + Y * GridSize.X];
			Cell.GridCoord = FIntPoint(X, Y);
			Cell.Elevation = 0.f;
			Cell.RiverProximity = 0.f;
			Cell.ValleyChannelFactor = 1.f;
			Cell.FaultLineProximity = 0.f;
		}
	}
}

void AGridManager::MarkRiver(const TArray<FIntPoint>& RiverCoords, float ProximityRadius)
{
	for (const FIntPoint& RiverCell : RiverCoords)
	{
		if (!IsCoordValid(RiverCell)) continue;
		Cells[CoordToIndex(RiverCell)].RiverProximity = 1.f;

		const int32 R = FMath::CeilToInt(ProximityRadius);
		for (int32 DX = -R; DX <= R; ++DX)
		{
			for (int32 DY = -R; DY <= R; ++DY)
			{
				const FIntPoint Neighbor(RiverCell.X + DX, RiverCell.Y + DY);
				if (!IsCoordValid(Neighbor)) continue;
				const float Dist = FMath::Sqrt(static_cast<float>(DX * DX + DY * DY));
				if (Dist > ProximityRadius) continue;
				const float Prox = FMath::Max(0.f, 1.f - Dist / ProximityRadius);
				FTerrainCell& C = Cells[CoordToIndex(Neighbor)];
				C.RiverProximity = FMath::Max(C.RiverProximity, Prox);
			}
		}
	}
}

void AGridManager::MarkValley(const TArray<FIntPoint>& ValleyCoords, float ChannelFactor)
{
	for (const FIntPoint& Coord : ValleyCoords)
	{
		if (!IsCoordValid(Coord)) continue;
		Cells[CoordToIndex(Coord)].ValleyChannelFactor = FMath::Max(1.f, ChannelFactor);
	}
}

void AGridManager::MarkFaultLine(const TArray<FIntPoint>& FaultCoords, float ProximityRadius)
{
	for (const FIntPoint& FaultCell : FaultCoords)
	{
		if (!IsCoordValid(FaultCell)) continue;
		Cells[CoordToIndex(FaultCell)].FaultLineProximity = 1.f;

		const int32 R = FMath::CeilToInt(ProximityRadius);
		for (int32 DX = -R; DX <= R; ++DX)
		{
			for (int32 DY = -R; DY <= R; ++DY)
			{
				const FIntPoint Neighbor(FaultCell.X + DX, FaultCell.Y + DY);
				if (!IsCoordValid(Neighbor)) continue;
				const float Dist = FMath::Sqrt(static_cast<float>(DX * DX + DY * DY));
				if (Dist > ProximityRadius) continue;
				const float Prox = FMath::Max(0.f, 1.f - Dist / ProximityRadius);
				FTerrainCell& C = Cells[CoordToIndex(Neighbor)];
				C.FaultLineProximity = FMath::Max(C.FaultLineProximity, Prox);
			}
		}
	}
}
