#pragma once

// Plain-data ownership adapter for the existing numerical Solver. This header
// has no UObject/Actor/world dependency; scheduling is implemented with UE Core.
#include "AftershockHydrologySolver.h"
#include <memory>

namespace AftershockWater
{
struct FrontierStamp
{
    std::uint64_t Generation = 0, Publication = 0, CompletedStep = 0;
    double SimulatedSeconds = 0.0;
};

struct FrontierCell
{
    std::int32_t X = 0, Y = 0;
    double BedM = 0.0, DepthM = 0.0;
    double DischargeXM2PerS = 0.0, DischargeYM2PerS = 0.0;
    double CumulativeInfiltrationM3 = 0.0;
    // -Y north=1, +X east=2, +Y south=4, -X west=8. These are
    // hydraulic face-admissibility masks, NOT walking/evacuation permissions.
    // BoundaryMask: declared external faces, including walls.
    // ConnectedMask: internal face wet above reconstructed crest, or an open
    // structure. Equal heads can be connected while instantaneous flux is zero.
    // OpenBoundaryMask: declared stage/outlet currently hydraulically open.
    std::uint8_t BoundaryMask = 0, ConnectedMask = 0, OpenBoundaryMask = 0;
};

struct PublishedFrontier
{
    FrontierStamp Stamp;
    // Exact immutable geometry corresponding to this frontier, independent of
    // later canonical topology edits. Its allocation is a SNAPSHOT cost; the
    // solver's own 64 MiB limit does not pretend to include these copies.
    std::shared_ptr<const Domain> Geometry;
    std::vector<FrontierCell> Cells;
    std::vector<StorageState> Storages;
    Budget Ledger;
    std::uint64_t ForcingRevision = 0, SolverOwnedBytes = 0, SnapshotBytes = 0;
};

struct FrontierOrigin
{
    // Restore supplies the prior published physical clock and complete ledger.
    // Fresh initialization leaves HasLedger=false and starts at time zero.
    double SimulatedSeconds = 0.0;
    bool HasLedger = false;
    Budget Ledger;
};

struct InitializeInput
{
    Domain Geometry;
    std::vector<CellState> Cells;
    std::vector<StorageState> Storages;
    std::vector<double> CumulativeInfiltrationM3; // optional zero origin, otherwise exact cell count
    Settings NumericalSettings;
    FrontierOrigin Origin;
    double ExternalSurfaceSourceM3 = 0.0; // declared cross-owner source, idle frontier only
};

// Immutable plain initialization state, also visible to an explicit save owner
// during a pending topology transition. No pointer ever refers into a Solver.
struct InitializationSnapshot
{
    std::shared_ptr<const Domain> Geometry;
    std::vector<CellState> Cells;
    std::vector<StorageState> Storages;
    std::vector<double> CumulativeInfiltrationM3; // optional zero origin, otherwise exact cell count
    Settings NumericalSettings;
    FrontierOrigin Origin;
    double ExternalSurfaceSourceM3 = 0.0; // declared cross-owner source, idle frontier only
    std::uint64_t Generation = 0, ExpectedPublication = 0, PayloadBytes = 0;
};

class ForcingPacket
{
public:
    const Forcing& Values() const { return Data; }
    std::uint64_t Generation() const { return TopologyGeneration; }
    std::uint64_t Revision() const { return RevisionNumber; }
    std::uint64_t PayloadBytes() const { return Bytes; }
private:
    friend class AsyncStepper;
    Forcing Data;
    std::uint64_t TopologyGeneration = 0, RevisionNumber = 0, Bytes = 0;
};

struct TimeDebt
{
    // Includes the ENTIRE current job request until Poll publishes its accepted
    // prefix. A save neither reads worker progress nor double-adds that request.
    double RemainingSeconds = 0.0;
    std::shared_ptr<const ForcingPacket> Sources;
};

struct SaveFrontier
{
    std::shared_ptr<const PublishedFrontier> Published;
    std::shared_ptr<const InitializationSnapshot> DesiredInitialization;
    std::vector<TimeDebt> Unconsumed; // maximum two, including the in-flight one
    double UnconsumedSeconds = 0.0, InFlightRequestedSeconds = 0.0;
    std::uint64_t DesiredGeneration = 0;
    bool WorkerInFlight = false, Faulted = false;
    std::string Error;
    // Published may be null during first initialization: not a valid zero-water
    // save! During topology transition save BOTH states, or postpone physics
    // capture until it settles. Never pair Published with a different Domain.
};

struct StepperLimits
{
    std::uint64_t MaximumSolverBytes = 64ull * 1024ull * 1024ull;
    std::uint64_t MaximumSnapshotBytes = 64ull * 1024ull * 1024ull;
    // At most two accepted forcing segments + one desired initialization + the
    // input of the single running initialization. This is separately bounded.
    std::uint64_t MaximumPendingBytes = 64ull * 1024ull * 1024ull;
    // Backpressure, not time scaling: rejected seconds remain with the caller.
    // A bounded debt also avoids accepting seconds that double arithmetic
    // could no longer subtract at the solver's minimum numerical time scale.
    double MaximumUnconsumedSeconds = 3600.0;
    // Immutable scheduling quantum, not a time multiplier or CFL change.
    // Small/default owners keep eight; live accelerated water opts into32.
    std::size_t MaximumWorkerSteps = 8;
};

struct PollResult
{
    bool Published = false, DiscardedStaleGeneration = false, Failed = false;
    bool WorkerInFlight = false;
    double AdvancedSeconds = 0.0, UnconsumedSeconds = 0.0;
    FrontierStamp Stamp;
    std::string Error;
};

// All public calls, construction, destruction and published-state access are
// game-thread-only. One in-flight ThreadPool task owns the mutable Solver.
// Worker closures capture ONLY plain immutable data, an atomic generation and
// the unique solver. No task captures this, UObject, delegates or a world.
class AsyncStepper
{
public:
    explicit AsyncStepper(const StepperLimits& Limits = {});
    ~AsyncStepper();
    AsyncStepper(const AsyncStepper&) = delete;
    AsyncStepper& operator=(const AsyncStepper&) = delete;
    AsyncStepper(AsyncStepper&&) = delete;
    AsyncStepper& operator=(AsyncStepper&&) = delete;

    // Moves input buffers. ExpectedPublication==0 on first init/restore; after
    // that it must equal LastPublished()->Stamp.Publication. Replaces at most
    // ONE pending desired topology; the old task is joined naturally by Poll.
    // A replacement inherits the last published clock/ledger. Mapping water
    // conservatively to a changed domain is the canonical owner's obligation.
    bool BeginInitialize(InitializeInput&& Input, std::uint64_t ExpectedPublication,
        std::string& Error);

    // Builder takes ownership: callers cannot mutate packet data afterward.
    // Revision is provenance only; identical revisions do not merge packets.
    static std::shared_ptr<const ForcingPacket> MakeForcingPacket(Forcing&& Sources,
        std::uint64_t Generation, std::uint64_t Revision, std::string& Error);

    // true means ALL Seconds were accepted. false means NONE were accepted;
    // caller retains them. Adjacent submissions coalesce only for the same
    // immutable packet identity. At most two distinct segments are retained.
    // Slice at hydrograph/control discontinuities; never replace old rainfall
    // with a later forcing merely to catch up to wall time.
    bool BeginAdvance(double Seconds, std::shared_ptr<const ForcingPacket> Sources,
        std::string& Error);

    // Changed topology invalidates old cell-indexed forcing, not accepted time.
    // Rebind each old-generation debt explicitly after canonical re-projection.
    // Also allows correcting invalid, wholly unconsumed forcing after failure.
    // Cannot change a segment currently used by a SAME-generation worker.
    bool RebindPendingForcing(std::size_t DebtIndex,
        std::shared_ptr<const ForcingPacket> Sources, std::string& Error);
    bool RetryPending(std::string& Error);

    PollResult Poll(); // nonblocking, never advances without accepted time
    SaveFrontier CaptureSaveFrontier() const; // nonblocking, immutable DTOs only
    std::shared_ptr<const PublishedFrontier> LastPublished() const;
    std::uint64_t DesiredGeneration() const;
    bool IsWorkerInFlight() const;

    // Generation cancellation preserves the published state AND accepted time.
    // BeginInitialize is required before more work; debt then needs rebind.
    void CancelGeneration();
    // Owner teardown only. Joins its one task; no callbacks survive destruction.
    // Capture a save FIRST if the owner needs to persist unconsumed time.
    void Deinitialize();
private:
    struct Impl;
    std::unique_ptr<Impl> State;
};
}
