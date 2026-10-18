#pragma once
// Original Saved-only proposal; NOT compiled or integrated. No Unreal dependency.
#include "AftershockHydrologySolver.h"
#include <array>

namespace AftershockWater
{
constexpr std::uint32_t ObstructionRasterResolution = 64;
constexpr std::uint32_t ObstructionModelVersion = 1;
constexpr std::uint64_t ObstructionFormatFingerprint = 0x4153484f42533031ull; // ASHOBS01
constexpr std::size_t MaximumObstructionFootprints = MaximumCells;
constexpr std::size_t MaximumFootprintsPerCell = 16;
constexpr std::size_t MaximumFootprintSpanCells = 8;
constexpr std::uint64_t MaximumObstructionOwnedBytes = 64ull*1024ull*1024ull;
constexpr std::size_t MaximumObstructionRasterTileTests = 32ull*1024ull*1024ull;
// A supported-geometry limit, NEVER a clamp. Rasters below this fail admission.
constexpr double MinimumSupportedStoragePorosity = 0.05;

struct ObstructionPointMM { std::int64_t X=0, Y=0; };
struct ObstructionFootprint
{
    std::int32_t BuildingID=0, OwnerX=0, OwnerY=0;
    std::uint8_t DefinitionType=0;
    // Schema/versioned hash of the canonical SOURCE transform, not render proxy.
    std::uint64_t SourceTransformBinding=0;
    // Strictly convex counter-clockwise world-mm quad. Authored ground-contact
    // envelope; roof overhangs/wind-blade swept bounds must not be substituted.
    std::array<ObstructionPointMM,4> Corners{};
};
struct ObstructionInput
{
    std::uint32_t ModelVersion=0;
    std::uint32_t RasterResolution=ObstructionRasterResolution;
    std::uint64_t FormatFingerprint=ObstructionFormatFingerprint;
    // GridOrigin is the MINIMUM cell corner, not cell centre. Integer SI binding.
    std::int64_t GridOriginXMM=0, GridOriginYMM=0;
    std::int64_t CellWidthMM=10000, CellHeightMM=10000;
    std::uint64_t SourceTerrainRevision=0, SourceBuildingRevision=0;
    std::vector<ObstructionFootprint> Footprints;
};
struct ObstructionCellOpening
{
    std::int32_t X=0,Y=0;
    double StoragePorosity=1.;
    // N=-Y,E=+X,S=+Y,W=-X; N/S bits increase X, W/E bits increase Y.
    std::array<std::uint64_t,4> OpenPorts{{UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX}};
};
struct ObstructionFaceOpening
{
    std::int64_t StableID=0;
    double OpenFraction=1.;
};
struct ObstructionDiagnostics
{
    std::size_t AffectedCells=0, CandidateVisits=0, RasterTileTests=0;
    std::size_t MaximumCandidatesInCell=0, Footprints=0;
    std::uint64_t EstimatedOwnedBytes=0;
};
struct ObstructionGeometry
{
    ObstructionInput Binding;
    std::uint64_t TopologyFingerprint=0, GeometryFingerprint=0;
    std::vector<ObstructionCellOpening> Cells; // Exact source domain order.
    std::vector<ObstructionFaceOpening> Faces; // Exact source domain order.
    ObstructionDiagnostics Diagnostics;
};
struct ObstructionBuildLimits
{
    // Caller may LOWER these admission limits, never raise the hard ceilings.
    std::uint64_t MaximumOwnedBytes=MaximumObstructionOwnedBytes;
    std::size_t MaximumRasterTileTests=MaximumObstructionRasterTileTests;
};

// All outputs are staged; errors leave Out unchanged. Bounding failures never
// omit a building, clamp porosity or clear a wall. Model 0 requires no descriptors
// and explicitly produces phi=theta=1, retaining the old physical interpretation.
bool BuildHydraulicObstructions(const Domain&, const ObstructionInput&,
    ObstructionGeometry& Out, std::string& Error,const ObstructionBuildLimits& Limits={});
// Restore/preflight: re-rasterize the STORED integer descriptors and compare all
// exact dyadic phi/theta/ports/digests. Do not recompute from mutable mesh assets.
bool ValidateHydraulicObstructions(const Domain&, const ObstructionGeometry&,
    std::string& Error);
// Preserves each cell's V=phi*A*h and integrated X/Y momentum=phi*A*q.
// Moving solid geometry can do work: energy is NOT claimed conserved. No ledger,
// time, accepted forcing, terrain, buildings or input geometry is mutated here.
bool ReprojectHydraulicObstructions(const Domain& OldGeometry,
    const std::vector<CellState>& OldCells, const ObstructionGeometry& NewGeometry,
    std::vector<CellState>& Out, std::string& Error);
// Pure format gate for the proposed new typed section. V4 is decoded elsewhere
// by the frozen V4 decoder, then explicitly migrates to ModelVersion 0.
bool IsSupportedObstructionFormat(std::uint32_t ModelVersion,
    std::uint32_t RasterResolution,std::uint64_t Fingerprint);
}
