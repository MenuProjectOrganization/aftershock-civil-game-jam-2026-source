#pragma once

// Original hydraulic terrain adapter. Native/runtime acceptance is tracked separately.
// A read-only adapter, not a second hydrology owner. Call on an immutable GT
// snapshot, then move the result to the one canonical solver/worker.
#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "AftershockHydrologySolver.h"

enum class EAftershockHydraulicBoundary : uint8 { Wall, SeaStage, RiverStage, FreeOutflow, DeclaredCellInflow };

struct FAftershockHydraulicBoundarySpec
{
	FIntPoint Coord = FIntPoint::ZeroValue;
	FIntPoint OutwardStep = FIntPoint::ZeroValue; // N=(0,-1), E=(1,0), S=(0,1), W=(-1,0).
	EAftershockHydraulicBoundary Kind = EAftershockHydraulicBoundary::Wall;
	double StageElevationCm = 0.;
	FVector2d VelocityCmPerSecond = FVector2d::ZeroVector;
	double InflowM3PerSecond = 0.; // DeclaredCellInflow only: a labelled source, not a hidden face flux.
};

struct FAftershockHydraulicPersistentCell
{
	FIntPoint Coord = FIntPoint::ZeroValue;
	double VolumeM3 = 0.;
	double HuM2PerSecond = 0., HvM2PerSecond = 0.;
	double CapturedCellAreaM2 = 0.; // Must match. Grid-resize/remeshing needs an explicit owner migration.
};

struct FAftershockHydraulicTerrainInput
{
	TConstArrayView<FGridCell> Cells;
	TConstArrayView<FAftershockHydraulicPersistentCell> PersistentCells;
	TConstArrayView<FAftershockHydraulicBoundarySpec> ExteriorBoundaries;
	TConstArrayView<float> ManningNByStorageIndex; // Empty uses the explicit authored default below.
	double CellSizeCm = 1000., CoastalSeaLevelCm = 0.;
	int32 SourceTerrainVersion = 3; // This adapter requires world-centimetre V3 geometry.
	bool bModel2Active = false;
	double DefaultManningN = .03;
	double AuthoredGateHeightM = 1.5;
	double AuthoredWeirCoefficientM05PerS = 1.7;
	double AuthoredGateDischargeCoefficient = .62;
};

struct FAftershockHydraulicTerrainDiagnostics
{
	int32 ResidentCells = 0, NonPlayableCells = 0, ExcavatedCells = 0;
	int32 InteriorFaces = 0, WallFaces = 0, SeaStageFaces = 0, RiverStageFaces = 0;
	int32 FreeOutflowFaces = 0, DeclaredInflowFaces = 0, StructureFaces = 0;
	double InitialVolumeM3 = 0., DeclaredInflowM3PerSecond = 0.;
	bool bAuthoredHydraulicGeometryNotSurveyed = true;
};

struct FAftershockHydraulicTerrainBuild
{
	AftershockWater::Domain Domain;
	std::vector<AftershockWater::CellState> InitialCells;
	AftershockWater::Forcing DefaultForcing;
	TArray<FIntPoint> StorageIndexToCoord; // Same order as input canonical Cells; no row-major reconstruction.
	FAftershockHydraulicTerrainDiagnostics Diagnostics;
};

// Pure common source. NEVER derives a new excavation from RiverCapacity or DEM.
// Inactive returns the unchanged canonical bank elevation (legacy contract).
bool TryGetAftershockMaterialGroundCm(const FGridCell& Cell, bool bUsePhysicalOverlays,
	double& OutGroundCm, FText& OutError);
bool TryGetAftershockHydraulicBedCm(const FGridCell& Cell, bool bModel2Active,
	double& OutBedCm, FText& OutError);
bool IsAftershockHydraulicExcavatedCell(const FGridCell& Cell, bool bModel2Active);

// Validates everything into a local candidate; failure leaves OutBuild untouched.
// No cells, source DEM, callbacks, terrain caches, render settings or live solver are mutated.
bool BuildAftershockHydraulicTerrain(const FAftershockHydraulicTerrainInput& Input,
	FAftershockHydraulicTerrainBuild& OutBuild, FText& OutError);

// Rebind capture from an already committed solver: V=h*A, q copied exactly.
// Coordinates and cardinality must match, not merely index positions. Does NOT
// reset the canonical ledger/clock; owner carries those through its transaction.
bool CaptureAftershockHydraulicPersistentCells(const AftershockWater::Domain& Geometry,
	const std::vector<AftershockWater::CellState>& Cells,
	TArray<FAftershockHydraulicPersistentCell>& OutCells, FText& OutError);

// The FV bed is the effective cell-centre elevation, not a demand for flat
// visual cell plates. Shared half-grid knots reconstruct a C0 continuous cut:
// exact authored centre; shared channel edges/corners interpolate the same
// beds; any land-facing edge/corner keeps the ORIGINAL bank surface knot.
// Old land/building/road/bridge support samplers stay intact. Renderer and
// collision consume these SAME knots in the SAME ground mesh/collision cook.
// Caller omits original fine patches belonging to profile cells at EVERY LOD.
struct FAftershockHydraulicGroundVertex
{
	FVector Position = FVector::ZeroVector;
	FVector Normal = FVector::UpVector;
	FVector2f UV = FVector2f::ZeroVector;
};
struct FAftershockHydraulicGroundPatch
{
	TArray<FAftershockHydraulicGroundVertex> Vertices;
	TArray<int32> Indices;
	int32 ProfileCells = 0, SharedKnots = 0;
};
// HalfGrid is twice the canonical cell-centre grid coordinate. Negative odd
// values use floor division correctly. This is a mesh knot source, not a
// separate bilinear physical sampler; ray/collision use the generated triangles.
bool TryGetAftershockHydraulicGroundKnotCm(const UTerrainGridSubsystem& Terrain,
	FIntPoint HalfGrid, bool bModel2Active, double& OutGroundCm, FText& OutError);
bool BuildAftershockHydraulicGroundPatch(const UTerrainGridSubsystem& Terrain,
	const FIntRect& Visible, bool bModel2Active,
	FAftershockHydraulicGroundPatch& OutPatch, FText& OutError);

// Cell-centre physical ground query. FV bed and the reconstructed mesh centre
// agree exactly; this intentionally does not replace generic bank/road sampling.
bool TryGetAftershockHydraulicCellGroundCm(const UTerrainGridSubsystem& Terrain,
	FIntPoint Coord, bool bModel2Active, double& OutGroundCm, FText& OutError);
