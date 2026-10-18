#include "AftershockHydrologyAsyncStepper.h"
#include "CoreMinimal.h"
#include "Async/Async.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <utility>

namespace AftershockWater
{
namespace
{
constexpr std::uint64_t SolverByteCeiling = 64ull * 1024ull * 1024ull;
constexpr double NumericalTimeResolution = 1e-12; // existing Solver::Advance
constexpr std::size_t MaximumDebtSegments = 2;
void RequireGameThread() { check(IsInGameThread()); }
static inline bool Finite(double V) { return std::isfinite(V); }
std::uint64_t AddBytes(std::uint64_t A, std::uint64_t B)
{ return B > UINT64_MAX - A ? UINT64_MAX : A + B; }
template<class T> std::uint64_t Bytes(const std::vector<T>& V)
{ return V.capacity() > UINT64_MAX / sizeof(T) ? UINT64_MAX : V.capacity() * sizeof(T); }
std::uint64_t DomainBytes(const Domain& D)
{
    return AddBytes(sizeof(Domain), AddBytes(AddBytes(Bytes(D.Cells), Bytes(D.Faces)),
        AddBytes(Bytes(D.Storages), Bytes(D.Exchanges))));
}
std::uint64_t SourceBytes(const Forcing& F)
{ return AddBytes(sizeof(ForcingPacket), AddBytes(AddBytes(Bytes(F.Cells), Bytes(F.Boundaries)), Bytes(F.StorageExternalOutflowM3PerS))); }
bool DomainCountsValid(const Domain& D)
{
    return !D.Cells.empty() && D.Cells.size() <= MaximumCells
        && D.Faces.size() <= MaximumCells * 4 && D.Storages.size() <= MaximumCells
        && D.Exchanges.size() <= MaximumCells * 4;
}
bool ForcingValuesValid(const Forcing& F)
{
    if (F.Cells.size() > MaximumCells || F.Boundaries.size() > MaximumCells * 4
        || F.StorageExternalOutflowM3PerS.size() > MaximumCells) return false;
    for (const auto& C : F.Cells)
        if (!Finite(C.RainMPerS) || C.RainMPerS < 0.0
            || !Finite(C.InfiltrationCapacityMPerS) || C.InfiltrationCapacityMPerS < 0.0
            || !Finite(C.ExternalDrainCapacityM3PerS) || C.ExternalDrainCapacityM3PerS < 0.0
            || !Finite(C.EvaporationMPerS) || C.EvaporationMPerS < 0.0
            || !Finite(C.ExternalInflowM3PerS) || C.ExternalInflowM3PerS < 0.0
            || !Finite(C.InflowVelocityXMps) || !Finite(C.InflowVelocityYMps)) return false;
    for (const auto& B : F.Boundaries)
        if (!Finite(B.HeadM) || !Finite(B.VelocityXMps) || !Finite(B.VelocityYMps)) return false;
    for (double V : F.StorageExternalOutflowM3PerS) if (!Finite(V) || V < 0.0) return false;
    return true;
}
bool ShapeMatches(const Forcing& F, const Domain& D)
{
    return (F.Cells.empty() || F.Cells.size() == D.Cells.size())
        && (F.Boundaries.empty() || F.Boundaries.size() == D.Faces.size())
        && (F.StorageExternalOutflowM3PerS.empty() || F.StorageExternalOutflowM3PerS.size() == D.Storages.size());
}
bool FiniteBudget(const Budget& B)
{
    for (double V : {B.InitialSurfaceM3, B.InitialStorageM3, B.RainM3, B.ExternalInflowM3,
        B.BoundaryInflowM3, B.BoundaryOutflowM3, B.InfiltrationM3, B.ExternalDrainageM3,
        B.EvaporationM3, B.StorageExternalOutflowM3, B.FinalSurfaceM3, B.FinalStorageM3,
        B.NumericalVolumeCorrectionM3, B.ResidualM3, B.SurfaceToStorageM3, B.StorageToSurfaceM3})
        if (!Finite(V)) return false;
    return true;
}
bool NearVolume(double A, double B)
{ return std::abs(A - B) <= 1e-8 * std::max(1.0, std::max(std::abs(A), std::abs(B))); }
bool OriginValid(const FrontierOrigin& O, const Budget& Initial, const std::vector<double>& InfiltrationM3, std::string& Error)
{
    if (!Finite(O.SimulatedSeconds) || O.SimulatedSeconds < 0.0
        || (!O.HasLedger && O.SimulatedSeconds != 0.0))
    { Error = "Invalid resume clock or nonzero fresh clock"; return false; }
    double PriorInfiltration = 0.0;
    for (double V : InfiltrationM3) PriorInfiltration += V;
    if (!Finite(PriorInfiltration) || (!O.HasLedger && PriorInfiltration != 0.0)
        || (O.HasLedger && !NearVolume(PriorInfiltration, O.Ledger.InfiltrationM3)))
    { Error = "Per-cell cumulative infiltration origin differs from published ledger"; return false; }
    if (O.HasLedger && (!FiniteBudget(O.Ledger)
        || !NearVolume(O.Ledger.FinalSurfaceM3, Initial.InitialSurfaceM3)
        || !NearVolume(O.Ledger.FinalStorageM3, Initial.InitialStorageM3)))
    { Error = "Resume/topology state volume differs from published ledger; canonical owner must map water conservatively"; return false; }
    return true;
}
Budget CombinedBudget(const Budget& K, const FrontierOrigin& O)
{
    if (!O.HasLedger) return K;
    Budget B = O.Ledger;
    B.RainM3 += K.RainM3; B.ExternalInflowM3 += K.ExternalInflowM3;
    B.BoundaryInflowM3 += K.BoundaryInflowM3; B.BoundaryOutflowM3 += K.BoundaryOutflowM3;
    B.InfiltrationM3 += K.InfiltrationM3; B.ExternalDrainageM3 += K.ExternalDrainageM3;
    B.EvaporationM3 += K.EvaporationM3; B.StorageExternalOutflowM3 += K.StorageExternalOutflowM3;
    B.NumericalVolumeCorrectionM3 += K.NumericalVolumeCorrectionM3;
    B.SurfaceToStorageM3 += K.SurfaceToStorageM3; B.StorageToSurfaceM3 += K.StorageToSurfaceM3;
    B.FinalSurfaceM3 = K.FinalSurfaceM3; B.FinalStorageM3 = K.FinalStorageM3;
    B.AcceptedSteps += K.AcceptedSteps; B.FaceEvaluations += K.FaceEvaluations;
    B.ResidualM3 = B.InitialSurfaceM3 + B.InitialStorageM3 + B.RainM3 + B.ExternalInflowM3
        + B.BoundaryInflowM3 + B.NumericalVolumeCorrectionM3 - B.BoundaryOutflowM3
        - B.InfiltrationM3 - B.ExternalDrainageM3 - B.EvaporationM3 - B.StorageExternalOutflowM3
        - B.FinalSurfaceM3 - B.FinalStorageM3;
    return B;
}
bool SameBudget(const Budget& A, const Budget& B)
{
    return A.InitialSurfaceM3==B.InitialSurfaceM3&&A.InitialStorageM3==B.InitialStorageM3
        &&A.RainM3==B.RainM3&&A.ExternalInflowM3==B.ExternalInflowM3
        &&A.BoundaryInflowM3==B.BoundaryInflowM3&&A.BoundaryOutflowM3==B.BoundaryOutflowM3
        &&A.InfiltrationM3==B.InfiltrationM3&&A.ExternalDrainageM3==B.ExternalDrainageM3
        &&A.EvaporationM3==B.EvaporationM3&&A.StorageExternalOutflowM3==B.StorageExternalOutflowM3
        &&A.FinalSurfaceM3==B.FinalSurfaceM3&&A.FinalStorageM3==B.FinalStorageM3
        &&A.NumericalVolumeCorrectionM3==B.NumericalVolumeCorrectionM3&&A.ResidualM3==B.ResidualM3
        &&A.SurfaceToStorageM3==B.SurfaceToStorageM3&&A.StorageToSurfaceM3==B.StorageToSurfaceM3
        &&A.AcceptedSteps==B.AcceptedSteps&&A.FaceEvaluations==B.FaceEvaluations;
}
bool ValidateDeclaredSource(const InitializeInput& In, const PublishedFrontier& Old, std::string& Error)
{
    const double Delta=In.ExternalSurfaceSourceM3;
    Budget Adjusted=Old.Ledger;Adjusted.ExternalInflowM3+=Delta;Adjusted.FinalSurfaceM3+=Delta;
    if(!In.Origin.HasLedger||In.Origin.SimulatedSeconds!=Old.Stamp.SimulatedSeconds||!FiniteBudget(Adjusted)
        ||Adjusted.ExternalInflowM3<=Old.Ledger.ExternalInflowM3||Adjusted.FinalSurfaceM3<=Old.Ledger.FinalSurfaceM3
        ||!NearVolume(In.Origin.Ledger.ExternalInflowM3,Adjusted.ExternalInflowM3)
        ||!NearVolume(In.Origin.Ledger.FinalSurfaceM3,Adjusted.FinalSurfaceM3))
    {Error="Declared surface source must add exactly to external inflow/final volume without advancing time";return false;}
    Budget Unchanged=In.Origin.Ledger;Unchanged.ExternalInflowM3=Old.Ledger.ExternalInflowM3;Unchanged.FinalSurfaceM3=Old.Ledger.FinalSurfaceM3;
    if(!SameBudget(Unchanged,Old.Ledger)||In.Geometry.Cells.size()!=Old.Cells.size())
    {Error="Declared surface source may not alter other ledger fields, counters or resident cell set";return false;}
    // The narrow cross-owner source transaction keeps the canonical resident
    // storage order. A simultaneous region reorder/expansion is a different
    // topology operation, performed separately with stable-coordinate mapping.
    for(std::size_t I=0;I<Old.Cells.size();++I)
    {
        const auto& C=In.Geometry.Cells[I];const auto& Prior=Old.Cells[I];
        const double History=In.CumulativeInfiltrationM3.empty()?0.:In.CumulativeInfiltrationM3[I];
        if(C.X!=Prior.X||C.Y!=Prior.Y||History!=Prior.CumulativeInfiltrationM3)
        {Error="Declared surface source must preserve resident cell order and per-cell infiltration history";return false;}
    }
    return true;
}
std::uint8_t SideBit(const FaceGeometry& F)
{ return F.NormalY < 0.0 ? 1u : F.NormalX > 0.0 ? 2u : F.NormalY > 0.0 ? 4u : 8u; }
std::uint8_t Opposite(std::uint8_t B)
{ return B == 1u ? 4u : B == 2u ? 8u : B == 4u ? 1u : 2u; }

std::unique_ptr<PublishedFrontier> MakeFrontier(const Solver& S,
    std::shared_ptr<const Domain> Geometry, const FrontierOrigin& Origin,
    const Forcing& Sources, std::uint64_t Revision, std::uint64_t MaximumBytes, std::string& Error)
{
    // Derived masks inspect the SAME accepted state and face crest/configuration
    // as EvaluateFace. No flux, damage, volume or second simulation is computed.
    if (Origin.HasLedger && (S.VolumeBudget().AcceptedSteps > UINT64_MAX - Origin.Ledger.AcceptedSteps
        || S.VolumeBudget().FaceEvaluations > UINT64_MAX - Origin.Ledger.FaceEvaluations))
    { Error = "Cumulative hydrology counters overflow"; return {}; }
    auto F = std::make_unique<PublishedFrontier>(); F->Geometry = std::move(Geometry);
    F->Ledger = CombinedBudget(S.VolumeBudget(), Origin);
    F->Stamp.SimulatedSeconds = Origin.SimulatedSeconds + S.SimulatedSeconds();
    F->Stamp.CompletedStep = F->Ledger.AcceptedSteps; F->ForcingRevision = Revision;
    if (!Finite(F->Stamp.SimulatedSeconds) || !FiniteBudget(F->Ledger))
    { Error = "Nonfinite published hydrology clock/ledger"; return {}; }
    F->Cells.resize(S.Cells().size()); F->Storages = S.Storages();
    F->SolverOwnedBytes = S.EstimatedOwnedBytes();
    F->SnapshotBytes = AddBytes(sizeof(PublishedFrontier), AddBytes(DomainBytes(*F->Geometry),
        AddBytes(Bytes(F->Cells), Bytes(F->Storages))));
    if (F->SnapshotBytes > MaximumBytes)
    { Error = "Separate immutable frontier payload budget exceeded"; return {}; }
    const auto& D = S.Geometry(); const auto& States = S.Cells();
    for (std::size_t I = 0; I < States.size(); ++I)
    {
        auto& C = F->Cells[I]; C.X = D.Cells[I].X; C.Y = D.Cells[I].Y; C.BedM = D.Cells[I].BedM;
        C.DepthM = States[I].DepthM; C.DischargeXM2PerS = States[I].DischargeXM2PerS;
        C.DischargeYM2PerS = States[I].DischargeYM2PerS;
        C.CumulativeInfiltrationM3 = S.CumulativeInfiltrationM3()[I];
    }
    for (std::size_t I = 0; I < D.Faces.size(); ++I)
    {
        const auto& Face = D.Faces[I]; const auto Bit = SideBit(Face);
        auto& L = F->Cells[Face.Left];
        if (Face.Right < 0) L.BoundaryMask |= Bit;
        if (Face.Kind == FaceKind::Wall || Face.OpenFraction <= 0.0) continue;
        const double EtaL = S.SurfaceHeadM(Face.Left);
        double EtaR = Face.Right >= 0 ? S.SurfaceHeadM(Face.Right) : D.Cells[Face.Left].BedM;
        if (Face.Kind == FaceKind::StageBoundary)
            EtaR = Sources.Boundaries.empty() ? Face.StageHeadM : Sources.Boundaries[I].HeadM;
        bool Open = false;
        if (Face.Kind == FaceKind::HydraulicStructure)
        {
            const bool Wet = States[Face.Left].DepthM > 0.0 || States[Face.Right].DepthM > 0.0;
            Open = Wet && ((Face.WeirCoefficientM05PerS > 0.0 && std::max(EtaL, EtaR) > Face.WeirCrestM)
                || (Face.GateDischargeCoefficient > 0.0 && Face.GateOpenFraction > 0.0
                    && Face.GateHeightM > 0.0 && std::max(EtaL, EtaR) > Face.GateInvertM));
        }
        else
        {
            double Crest = D.Cells[Face.Left].BedM;
            if (Face.Right >= 0) Crest = std::max(Crest, D.Cells[Face.Right].BedM);
            if (Face.HasSill) Crest = std::max(Crest, Face.SillM);
            Open = std::max(EtaL, EtaR) > Crest;
        }
        if (!Open) continue;
        if (Face.Right >= 0)
        { L.ConnectedMask |= Bit; F->Cells[Face.Right].ConnectedMask |= Opposite(Bit); }
        else L.OpenBoundaryMask |= Bit;
    }
    return F;
}
}

struct AsyncStepper::Impl
{
    enum class WorkKind { None, Initialize, Advance };
    struct WorkResult
    {
        WorkKind Kind = WorkKind::None;
        std::uint64_t Generation = 0;
        bool Initialized = false, Canceled = false;
        double RequestedSeconds = 0.0;
        AdvanceResult Advance;
        FrontierOrigin Origin;
        std::unique_ptr<Solver> Engine;
        std::unique_ptr<PublishedFrontier> Frame;
        std::string Error;
    };
    StepperLimits Limits;
    std::shared_ptr<std::atomic<std::uint64_t>> Epoch = std::make_shared<std::atomic<std::uint64_t>>(0);
    std::uint64_t Generation = 0, Publications = 0, RunningGeneration = 0;
    WorkKind RunningKind = WorkKind::None;
    TFuture<WorkResult> Future;
    std::unique_ptr<Solver> IdleEngine;
    FrontierOrigin Origin;
    std::shared_ptr<const PublishedFrontier> Published;
    std::shared_ptr<const InitializationSnapshot> DesiredInit, RunningInit;
    std::shared_ptr<const Domain> DesiredGeometry;
    std::shared_ptr<const ForcingPacket> RunningSources;
    std::vector<TimeDebt> Debt;
    double RunningRequestedSeconds = 0.0;
    bool PendingInitialize = false, Faulted = false;
    std::string LastError;

    std::uint64_t PendingBytes(const InitializationSnapshot* Replacement = nullptr,
        std::size_t ReplaceDebt = SIZE_MAX, const ForcingPacket* NewPacket = nullptr) const
    {
        std::uint64_t Total = RunningInit ? RunningInit->PayloadBytes : 0;
        const auto* Desired = Replacement ? Replacement : DesiredInit.get();
        if (Desired && Desired != RunningInit.get()) Total = AddBytes(Total, Desired->PayloadBytes);
        const ForcingPacket* Seen[MaximumDebtSegments + 2] = {}; std::size_t SeenCount = 0;
        const auto Add = [&](const ForcingPacket* P)
        {
            if (!P) return;
            for (std::size_t I = 0; I < SeenCount; ++I) if (Seen[I] == P) return;
            Seen[SeenCount++] = P; Total = AddBytes(Total, P->PayloadBytes());
        };
        // A stale-generation job can still hold its old immutable forcing
        // after canonical debt has been rebound to a new topology.
        Add(RunningSources.get());
        for (std::size_t I = 0; I < Debt.size(); ++I) Add(I == ReplaceDebt ? NewPacket : Debt[I].Sources.get());
        if (ReplaceDebt == SIZE_MAX) Add(NewPacket);
        return Total;
    }
    double Remaining() const
    { double V = 0.0; for (const auto& D : Debt) V += D.RemainingSeconds; return V; }
    bool PacketValid(const std::shared_ptr<const ForcingPacket>& P, std::string& Error) const
    {
        if (!P || !DesiredGeometry || P->Generation() != Generation
            || !ShapeMatches(P->Values(), *DesiredGeometry))
        { Error = "Forcing packet is null, stale-generation or differs from desired domain cardinality"; return false; }
        return true;
    }
    void StartIfPossible()
    {
        if (Future.IsValid() || Faulted) return;
        const auto Token = Epoch; const auto BytesLimit = Limits.MaximumSnapshotBytes;
        if (PendingInitialize && DesiredInit)
        {
            RunningInit = DesiredInit; PendingInitialize = false;
            RunningKind = WorkKind::Initialize; RunningGeneration = Generation;
            const auto Input = RunningInit;
            Future = Async(EAsyncExecution::ThreadPool, [Input, Token, BytesLimit]()
            {
                WorkResult R; R.Kind = WorkKind::Initialize; R.Generation = Input->Generation;
                R.Origin = Input->Origin;
                if (Token->load(std::memory_order_acquire) != R.Generation) { R.Canceled = true; return R; }
                R.Engine = std::make_unique<Solver>();
                R.Initialized = R.Engine->Initialize(*Input->Geometry, Input->Cells, Input->Storages,
                    Input->NumericalSettings, R.Error, Input->CumulativeInfiltrationM3);
                if (R.Initialized) R.Initialized = OriginValid(R.Origin, R.Engine->VolumeBudget(), R.Engine->CumulativeInfiltrationM3(), R.Error);
                if (Token->load(std::memory_order_acquire) != R.Generation)
                { R.Canceled = true; R.Engine.reset(); return R; }
                if (R.Initialized) R.Frame = MakeFrontier(*R.Engine, Input->Geometry, R.Origin,
                    {}, 0, BytesLimit, R.Error);
                return R;
            });
            return;
        }
        if (!IdleEngine || Debt.empty() || !Debt.front().Sources
            || Debt.front().Sources->Generation() != Generation
            // An accepted job can exhaust its step budget with a positive
            // sub-resolution tail. It still owns forcing/time: schedule that
            // final bounded integration instead of wedging a nonempty queue.
            || Debt.front().RemainingSeconds <= 0.0) return;
        RunningKind = WorkKind::Advance; RunningGeneration = Generation;
        RunningRequestedSeconds = Debt.front().RemainingSeconds;
        auto Engine = std::move(IdleEngine); const auto Sources = Debt.front().Sources;
        RunningSources = Sources;
        const auto Geometry = Published->Geometry; const auto PriorOrigin = Origin;
        const double Seconds = RunningRequestedSeconds; const auto JobGeneration = Generation;
        Future = Async(EAsyncExecution::ThreadPool,
            [Engine = std::move(Engine), Sources, Geometry, PriorOrigin, Seconds, JobGeneration, Token, BytesLimit]() mutable
        {
            WorkResult R; R.Kind = WorkKind::Advance; R.Generation = JobGeneration;
            R.RequestedSeconds = Seconds; R.Origin = PriorOrigin; R.Engine = std::move(Engine);
            if (Token->load(std::memory_order_acquire) != R.Generation)
            { R.Canceled = true; R.Engine.reset(); return R; }
            R.Advance = R.Engine->Advance(Seconds, Sources->Values()); R.Error = R.Advance.Error;
            if (Token->load(std::memory_order_acquire) != R.Generation)
            { R.Canceled = true; R.Engine.reset(); return R; }
            // A failed Advance can contain a valid accepted prefix. Publish it
            // before faulting; never replay that prefix after a retry/save.
            if (R.Advance.AdvancedSeconds > 0.0)
                R.Frame = MakeFrontier(*R.Engine, Geometry, PriorOrigin, Sources->Values(),
                    Sources->Revision(), BytesLimit, R.Error);
            return R;
        });
    }
};

AsyncStepper::AsyncStepper(const StepperLimits& Limits) : State(std::make_unique<Impl>())
{
    RequireGameThread(); State->Limits = Limits;
    State->Limits.MaximumWorkerSteps=std::max<std::size_t>(1,std::min<std::size_t>(32,Limits.MaximumWorkerSteps));
    State->Limits.MaximumSolverBytes = std::min(Limits.MaximumSolverBytes, SolverByteCeiling);
    State->Debt.reserve(MaximumDebtSegments);
}
AsyncStepper::~AsyncStepper() { Deinitialize(); }

bool AsyncStepper::BeginInitialize(InitializeInput&& Input, std::uint64_t ExpectedPublication, std::string& Error)
{
    RequireGameThread(); Error.clear(); auto& S = *State;
    const auto CurrentPublication = S.Published ? S.Published->Stamp.Publication : 0;
    if (ExpectedPublication != CurrentPublication || !DomainCountsValid(Input.Geometry)
        || Input.Cells.size() != Input.Geometry.Cells.size() || Input.Storages.size() != Input.Geometry.Storages.size()
        || (!Input.CumulativeInfiltrationM3.empty() && Input.CumulativeInfiltrationM3.size() != Input.Geometry.Cells.size()))
    { Error = "Initialization is based on a stale publication or invalid bounded input cardinality"; return false; }
    if(!Finite(Input.ExternalSurfaceSourceM3)||Input.ExternalSurfaceSourceM3<0.)
    {Error="Declared external surface volume must be finite/nonnegative";return false;}
    if(Input.ExternalSurfaceSourceM3>0.)
    {
        if(!S.Published||S.Future.IsValid()||!S.Debt.empty()||S.PendingInitialize||S.DesiredInit)
        {Error="Declared external surface source requires an idle published frontier and no accepted time debt";return false;}
        if(!ValidateDeclaredSource(Input,*S.Published,Error))return false;
    }
    if (S.Generation == UINT64_MAX || S.Limits.MaximumSolverBytes == 0)
    { Error = "Invalid generation/solver budget"; return false; }
    auto P = std::make_shared<InitializationSnapshot>();
    P->Geometry = std::make_shared<const Domain>(std::move(Input.Geometry));
    P->Cells = std::move(Input.Cells); P->Storages = std::move(Input.Storages);
    P->CumulativeInfiltrationM3 = std::move(Input.CumulativeInfiltrationM3);
    P->NumericalSettings = Input.NumericalSettings;
    // The owner fixes this bounded quantum at construction. A high game speed
    // or backlog never silently enlarges it. Physical seconds/CFL stay intact.
    if (P->NumericalSettings.MaximumStepsPerAdvance == 0)
    { Error = "Numerical step budget must be positive"; return false; }
    P->NumericalSettings.MaximumStepsPerAdvance = std::min(S.Limits.MaximumWorkerSteps,P->NumericalSettings.MaximumStepsPerAdvance);
    P->NumericalSettings.MaximumOwnedBytes = std::min(S.Limits.MaximumSolverBytes, P->NumericalSettings.MaximumOwnedBytes);
    P->Origin = Input.Origin;P->ExternalSurfaceSourceM3=Input.ExternalSurfaceSourceM3;
    if (S.Published)
    {
        P->Origin.HasLedger = true; P->Origin.Ledger = S.Published->Ledger; P->Origin.SimulatedSeconds = S.Published->Stamp.SimulatedSeconds;
        if(P->ExternalSurfaceSourceM3>0.)
        {P->Origin.Ledger.ExternalInflowM3+=P->ExternalSurfaceSourceM3;P->Origin.Ledger.FinalSurfaceM3+=P->ExternalSurfaceSourceM3;}
    }
    P->Generation = S.Generation + 1; P->ExpectedPublication = ExpectedPublication;
    P->PayloadBytes = AddBytes(sizeof(InitializationSnapshot), AddBytes(DomainBytes(*P->Geometry),
        AddBytes(AddBytes(Bytes(P->Cells), Bytes(P->Storages)), Bytes(P->CumulativeInfiltrationM3))));
    const auto SnapshotEstimate = AddBytes(sizeof(PublishedFrontier), AddBytes(DomainBytes(*P->Geometry),
        AddBytes(P->Cells.size() * sizeof(FrontierCell), P->Storages.size() * sizeof(StorageState))));
    if (S.PendingBytes(P.get()) > S.Limits.MaximumPendingBytes || SnapshotEstimate > S.Limits.MaximumSnapshotBytes)
    { Error = "Separate pending-input/frontier payload budget exceeded"; return false; }
    S.Generation = P->Generation; S.Epoch->store(S.Generation, std::memory_order_release);
    S.DesiredInit = std::move(P); S.DesiredGeometry = S.DesiredInit->Geometry;
    S.PendingInitialize = true; S.Faulted = false; S.LastError.clear(); S.IdleEngine.reset();
    S.StartIfPossible(); return true;
}

std::shared_ptr<const ForcingPacket> AsyncStepper::MakeForcingPacket(Forcing&& Sources,
    std::uint64_t Generation, std::uint64_t Revision, std::string& Error)
{
    RequireGameThread(); Error.clear();
    if (Generation == 0 || !ForcingValuesValid(Sources) || SourceBytes(Sources) > SolverByteCeiling)
    { Error = "Invalid immutable forcing packet or payload capacity"; return {}; }
    auto P = std::make_shared<ForcingPacket>(); P->Data = std::move(Sources);
    P->TopologyGeneration = Generation; P->RevisionNumber = Revision; P->Bytes = SourceBytes(P->Data);
    return P;
}

bool AsyncStepper::BeginAdvance(double Seconds, std::shared_ptr<const ForcingPacket> Sources, std::string& Error)
{
    RequireGameThread(); Error.clear(); auto& S = *State;
    if (!Finite(Seconds) || Seconds <= NumericalTimeResolution || S.Faulted || !S.PacketValid(Sources, Error))
    { if (Error.empty()) Error = "Physical time must be finite and >1e-12, and stepper must not be faulted"; return false; }
    const double Total = S.Remaining() + Seconds;
    if (!Finite(Total) || !Finite(S.Limits.MaximumUnconsumedSeconds)
        || Total > S.Limits.MaximumUnconsumedSeconds || Total <= S.Remaining())
    { Error = "Unconsumed physical time exceeds backlog/representability limit; caller retains ALL seconds"; return false; }
    const bool Coalesce = !S.Debt.empty() && S.Debt.back().Sources == Sources;
    if ((!Coalesce && S.Debt.size() == MaximumDebtSegments)
        || S.PendingBytes(nullptr, SIZE_MAX, Sources.get()) > S.Limits.MaximumPendingBytes)
    { Error = "Bounded forcing queue/input budget full; caller retains ALL unaccepted seconds"; return false; }
    if (Coalesce) S.Debt.back().RemainingSeconds += Seconds;
    else S.Debt.push_back({Seconds, std::move(Sources)});
    S.StartIfPossible(); return true;
}

bool AsyncStepper::RebindPendingForcing(std::size_t Index,
    std::shared_ptr<const ForcingPacket> Sources, std::string& Error)
{
    RequireGameThread(); Error.clear(); auto& S = *State;
    if (Index >= S.Debt.size() || !S.PacketValid(Sources, Error))
    { if (Error.empty()) Error = "Invalid accepted-debt index"; return false; }
    if (Index == 0 && S.Future.IsValid() && S.RunningKind == Impl::WorkKind::Advance && S.RunningGeneration == S.Generation)
    { Error = "Cannot change forcing used by the current-generation worker"; return false; }
    if (S.PendingBytes(nullptr, Index, Sources.get()) > S.Limits.MaximumPendingBytes)
    { Error = "Rebound forcing exceeds separate pending budget"; return false; }
    S.Debt[Index].Sources = std::move(Sources); S.StartIfPossible(); return true;
}

bool AsyncStepper::RetryPending(std::string& Error)
{
    RequireGameThread(); Error.clear(); auto& S = *State;
    if (!S.IdleEngine && !S.DesiredInit)
    { Error = "No recoverable solver; initialize from published frontier first"; return false; }
    if (!S.IdleEngine && S.DesiredInit) S.PendingInitialize = true;
    S.Faulted = false; S.LastError.clear(); S.StartIfPossible(); return true;
}

PollResult AsyncStepper::Poll()
{
    RequireGameThread(); auto& S = *State; PollResult Out;
    if (S.Future.IsValid() && S.Future.IsReady())
    {
        auto R = S.Future.Consume(); S.RunningInit.reset(); S.RunningSources.reset(); S.RunningKind = Impl::WorkKind::None;
        S.RunningRequestedSeconds = 0.0; S.RunningGeneration = 0;
        if (R.Canceled || R.Generation != S.Generation) Out.DiscardedStaleGeneration = true;
        else
        {
            bool Valid = true;
            if (R.Kind == Impl::WorkKind::Initialize)
                Valid = R.Initialized && R.Frame != nullptr;
            else
            {
                const double TimeTolerance = 1e-12 * std::max(1.0, R.RequestedSeconds);
                Valid = Finite(R.Advance.AdvancedSeconds) && Finite(R.Advance.RemainingSeconds)
                    && R.Advance.AdvancedSeconds >= 0.0 && R.Advance.RemainingSeconds >= 0.0
                    && R.Advance.AdvancedSeconds <= R.RequestedSeconds + TimeTolerance
                    && std::abs(R.Advance.AdvancedSeconds + R.Advance.RemainingSeconds - R.RequestedSeconds) <= TimeTolerance
                    && !S.Debt.empty()
                    && (R.Advance.AdvancedSeconds == 0.0 || R.Frame != nullptr);
                if (Valid)
                {
                    S.Debt.front().RemainingSeconds = std::max(0.0,
                        S.Debt.front().RemainingSeconds - R.Advance.AdvancedSeconds);
                    if (S.Debt.front().RemainingSeconds == 0.0) S.Debt.erase(S.Debt.begin());
                    Out.AdvancedSeconds = R.Advance.AdvancedSeconds;
                }
            }
            if (Valid)
            {
                S.IdleEngine = std::move(R.Engine); S.Origin = R.Origin;
                if (R.Frame)
                {
                    R.Frame->Stamp.Generation = S.Generation; R.Frame->Stamp.Publication = ++S.Publications;
                    S.Published = std::shared_ptr<const PublishedFrontier>(std::move(R.Frame));
                    Out.Published = true;
                    if (R.Kind == Impl::WorkKind::Initialize) S.DesiredInit.reset();
                }
            }
            if (!Valid || (R.Kind == Impl::WorkKind::Advance && !R.Advance.Success))
            {
                S.Faulted = true; Out.Failed = true;
                S.LastError = R.Error.empty() ? "Hydrology worker result could not publish a valid frontier" : std::move(R.Error);
                // No snapshot after accepted work means retain ALL old debt and
                // require reinitialization from Published; never replay on the
                // already-advanced but unpublished mutable solver.
                if (!Valid) S.IdleEngine.reset();
            }
        }
    }
    S.StartIfPossible();
    Out.WorkerInFlight = S.Future.IsValid(); Out.UnconsumedSeconds = S.Remaining();
    Out.Error = S.LastError;
    if (S.Published) Out.Stamp = S.Published->Stamp;
    return Out;
}

SaveFrontier AsyncStepper::CaptureSaveFrontier() const
{
    RequireGameThread(); const auto& S = *State; SaveFrontier Out;
    Out.Published = S.Published; Out.DesiredInitialization = S.DesiredInit;
    Out.Unconsumed = S.Debt; Out.UnconsumedSeconds = S.Remaining();
    Out.InFlightRequestedSeconds = S.RunningRequestedSeconds; Out.DesiredGeneration = S.Generation;
    Out.WorkerInFlight = S.Future.IsValid(); Out.Faulted = S.Faulted; Out.Error = S.LastError;
    return Out;
}
std::shared_ptr<const PublishedFrontier> AsyncStepper::LastPublished() const
{ RequireGameThread(); return State->Published; }
std::uint64_t AsyncStepper::DesiredGeneration() const
{ RequireGameThread(); return State->Generation; }
bool AsyncStepper::IsWorkerInFlight() const
{ RequireGameThread(); return State->Future.IsValid(); }
void AsyncStepper::CancelGeneration()
{
    RequireGameThread(); auto& S = *State; check(S.Generation != UINT64_MAX);
    ++S.Generation; S.Epoch->store(S.Generation, std::memory_order_release);
    S.DesiredInit.reset(); S.DesiredGeometry.reset(); S.PendingInitialize = false;
    S.IdleEngine.reset(); S.Faulted = false; S.LastError.clear();
}
void AsyncStepper::Deinitialize()
{
    if (!State) return;
    RequireGameThread(); CancelGeneration(); auto& S = *State;
    if (S.Future.IsValid()) { S.Future.Wait(); (void)S.Future.Consume(); }
    S.RunningInit.reset(); S.RunningSources.reset(); S.RunningKind = Impl::WorkKind::None;
    S.RunningRequestedSeconds = 0.0; S.RunningGeneration = 0;
    S.Published.reset(); S.Debt.clear(); S.Publications = 0;
}
}
