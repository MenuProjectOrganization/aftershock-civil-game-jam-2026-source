#pragma once

// Original, standalone C++17 numerical core. No Unreal/UObject/renderer/asset
// dependency. SI units throughout. This is an unvalidated depth-averaged game
// model, not full 3D CFD, a flood forecast or a replacement for surveyed data.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AftershockWater
{
constexpr double Gravity = 9.80665;
constexpr std::size_t MaximumCells = 65536;
constexpr std::uint64_t MaximumSolverOwnedBytes = 64ull * 1024ull * 1024ull;

struct CellGeometry
{
    std::int32_t X = 0, Y = 0; // Global key. Negative coordinates are valid.
    double BedM = 0.0;
    double ManningN = 0.03;
    // Fraction of gross plan area available for water storage; (0,1].
    // Appended to preserve existing four-field aggregate initializers.
    double StoragePorosity = 1.0;
};

struct CellState
{
    double DepthM = 0.0;
    double DischargeXM2PerS = 0.0; // h*u, not cell-integrated momentum.
    double DischargeYM2PerS = 0.0; // h*v.
};

enum class FaceKind : std::uint8_t
{
    Interior, Wall, StageBoundary, FreeOutflow, HydraulicStructure
};

struct FaceGeometry
{
    std::int32_t Left = -1, Right = -1; // Right==-1 only for a boundary.
    double NormalX = 1.0, NormalY = 0.0; // Axis unit normal Left -> Right/outside.
    double LengthM = 1.0;
    FaceKind Kind = FaceKind::Interior;
    std::int64_t StableID = 0;
    // Interior hydraulic reconstruction may include a bank/levee sill. The
    // default has no additional sill. Never overwrite source DEM with this.
    bool HasSill = false;
    double SillM = 0.0;
    double StageHeadM = 0.0, StageVelocityXMps = 0.0, StageVelocityYMps = 0.0;
    // Explicit structure approximation, not calibrated dam-break engineering.
    // Closed gate: over-crest broad-crested-weir flux only. Open gate adds an
    // orifice below the crest. One shared flux carries equal/opposite volume.
    double WeirCrestM = 0.0;
    double WeirCoefficientM05PerS = 1.7;
    double GateInvertM = 0.0, GateHeightM = 0.0, GateOpenFraction = 0.0;
    double GateDischargeCoefficient = 0.62;
    // One shared open fraction for this face, never two one-sided fluxes.
    double OpenFraction = 1.0;
};

struct StorageGeometry
{
    std::int64_t StableID = 0;
    double BaseElevationM = 0.0, SurfaceAreaM2 = 1.0, CapacityM3 = 1.0;
    // A deliberately lumped, stationary 0D node. Its footprint must NOT also
    // be represented as the same volume in surface cells. No hidden reservoir.
};
struct StorageState { double VolumeM3 = 0.0; };

struct StorageExchange
{
    std::int64_t StableID = 0;
    std::int32_t Cell = -1, Storage = -1;
    double InvertM = 0.0, ApertureAreaM2 = 0.01;
    double DischargeCoefficient = 0.62, MaximumAbsFlowM3PerS = 1.0;
    bool AllowBackflow = true;
    // Direction from surface cell into drain/reservoir node. Backflow jets use
    // the opposite direction. Node walls dissipate momentum, explicitly.
    double DirectionX = 1.0, DirectionY = 0.0;
};

struct Domain
{
    double CellWidthM = 10.0, CellHeightM = 10.0;
    std::vector<CellGeometry> Cells;
    std::vector<FaceGeometry> Faces;
    std::vector<StorageGeometry> Storages;
    std::vector<StorageExchange> Exchanges;
};

struct CellForcing
{
    // Rain is per gross catchment; infiltration/evaporation per open area.
    // The owner excludes paved fractions from the open-area capacity input.
    double RainMPerS = 0.0, InfiltrationCapacityMPerS = 0.0;
    // Only a declared external outlet. An actual pipe inlet belongs in an
    // Exchange; counting both would discharge the same water twice.
    double ExternalDrainCapacityM3PerS = 0.0;
    double EvaporationMPerS = 0.0;
    double ExternalInflowM3PerS = 0.0, InflowVelocityXMps = 0.0, InflowVelocityYMps = 0.0;
};
struct BoundaryForcing
{
    double HeadM = 0.0, VelocityXMps = 0.0, VelocityYMps = 0.0;
};
struct Forcing
{
    // Empty means all-zero/default. Otherwise exact domain cardinality.
    std::vector<CellForcing> Cells;
    std::vector<BoundaryForcing> Boundaries;
    std::vector<double> StorageExternalOutflowM3PerS;
    // Owner slices Advance at the next hydrograph/control discontinuity.
};

struct Settings
{
    double CFL = 0.35, MaximumStepSeconds = 0.25;
    double DryMomentumDepthM = 1e-7;
    double NegativeDepthToleranceM = 1e-12;
    std::size_t MaximumStepsPerAdvance = 64;
    std::uint64_t MaximumOwnedBytes = MaximumSolverOwnedBytes;
};

struct Budget
{
    double InitialSurfaceM3 = 0.0, InitialStorageM3 = 0.0;
    double RainM3 = 0.0, ExternalInflowM3 = 0.0;
    double BoundaryInflowM3 = 0.0, BoundaryOutflowM3 = 0.0;
    double InfiltrationM3 = 0.0, ExternalDrainageM3 = 0.0, EvaporationM3 = 0.0;
    double StorageExternalOutflowM3 = 0.0;
    double FinalSurfaceM3 = 0.0, FinalStorageM3 = 0.0;
    double NumericalVolumeCorrectionM3 = 0.0, ResidualM3 = 0.0;
    double SurfaceToStorageM3 = 0.0, StorageToSurfaceM3 = 0.0; // INTERNAL, not a sink.
    std::uint64_t AcceptedSteps = 0, FaceEvaluations = 0;
};

struct AdvanceResult
{
    bool Success = false;
    double AdvancedSeconds = 0.0, RemainingSeconds = 0.0;
    std::size_t Steps = 0;
    std::string Error;
};

// One owner holds one Solver. Thread confinement is the owner's contract.
// No callbacks, wall-clock timers, random numbers, UObject access or authority.
class Solver
{
public:
    bool Initialize(const Domain&, const std::vector<CellState>&,
        const std::vector<StorageState>&, const Settings&, std::string& Error,
        const std::vector<double>& InitialCumulativeInfiltrationM3 = {});
    AdvanceResult Advance(double RequestedPhysicalSeconds, const Forcing&);

    const Domain& Geometry() const { return GeometryData; }
    const std::vector<CellState>& Cells() const { return State; }
    const std::vector<StorageState>& Storages() const { return Stored; }
    // Absolute per-cell history, in unchanged Geometry().Cells index order.
    // Optional initialization origin is carried forward; VolumeBudget() still
    // accounts only fluxes since THIS initialization, so it is not double-added.
    const std::vector<double>& CumulativeInfiltrationM3() const { return InfiltratedM3; }
    const Budget& VolumeBudget() const { return Accounting; }
    double SimulatedSeconds() const { return ClockSeconds; }
    std::uint64_t CompletedStep() const { return Accounting.AcceptedSteps; }
    std::uint64_t EstimatedOwnedBytes() const { return OwnedBytes; }
    double SurfaceHeadM(std::size_t Cell) const;
    std::int32_t FindCell(std::int32_t X, std::int32_t Y) const;

private:
    struct FaceFlux
    {
        double Mass = 0.0, QX = 0.0, QY = 0.0;
        double LeftCorrectionX = 0.0, LeftCorrectionY = 0.0;
        double RightCorrectionX = 0.0, RightCorrectionY = 0.0;
        // Head-driven structure jet impulse, scaled with the actual transferred
        // volume. Structure reaction is an external force, not conserved fluid
        // momentum; these are distinct from the unscaled hydrostatic pressure.
        double LeftTransferX = 0.0, LeftTransferY = 0.0;
        double RightTransferX = 0.0, RightTransferY = 0.0;
    };
    bool ValidateForcing(const Forcing&, std::string&) const;
    double StableStep(const std::vector<CellState>&, const Forcing&) const;
    FaceFlux EvaluateFace(std::size_t, const std::vector<CellState>&, const Forcing&) const;
    FaceFlux ApplyOpenFraction(const FaceFlux&, const CellState&, const CellState&, double, double, double) const;
    void ApplySources(double, const Forcing&, std::vector<CellState>&,
        std::vector<StorageState>&, Budget&, std::vector<double>& InfiltrationM3) const;
    void ApplyExchanges(double, std::vector<CellState>&, std::vector<StorageState>&, Budget&);
    bool TryStep(double, const Forcing&, std::string&);
    void RefreshBudget();

    Domain GeometryData;
    Settings Options;
    std::vector<CellState> State, Candidate, Next;
    std::vector<double> InfiltratedM3, CandidateInfiltratedM3;
    std::vector<StorageState> Stored, CandidateStored;
    std::vector<std::size_t> CellOrder, FaceOrder, StorageOrder, ExchangeOrder;
    std::vector<FaceFlux> Fluxes;
    std::vector<double> Outgoing, DonorScale, ExchangeRates, ExchangeCellOut,
        ExchangeStorageOut, ExchangeStorageIn, DeltaH, DeltaQX, DeltaQY, DeltaStorage;
    Budget Accounting;
    double ClockSeconds = 0.0;
    std::uint64_t OwnedBytes = 0;
    bool Ready = false;
};
}
