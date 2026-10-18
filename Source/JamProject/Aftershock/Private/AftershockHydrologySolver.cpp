#include "AftershockHydrologySolver.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <tuple>
#include <utility>

namespace AftershockWater
{
namespace
{
static inline bool HydrologySolverFinite(double V) { return std::isfinite(V); }
double Clamp(double V, double A, double B) { return std::max(A, std::min(V, B)); }
double Positive(double V) { return std::max(0.0, V); }
bool ValidState(const CellState& S)
{
    return HydrologySolverFinite(S.DepthM) && S.DepthM >= 0.0 && HydrologySolverFinite(S.DischargeXM2PerS)
        && HydrologySolverFinite(S.DischargeYM2PerS)
        && (S.DepthM > 0.0 || (S.DischargeXM2PerS == 0.0 && S.DischargeYM2PerS == 0.0));
}
double Velocity(double Q, double H) { return H > 1e-12 ? Q / H : 0.0; }

// Homogeneous reflected-state HLL wall reaction. Unlike a static gh^2/2
// shortcut this also reacts to incident normal momentum. It carries no mass.
std::pair<double,double> PorosityWallReaction(const CellState& S,double NX,double NY)
{
    const double QN=S.DischargeXM2PerS*NX+S.DischargeYM2PerS*NY;
    const double QXR=S.DischargeXM2PerS-2.0*QN*NX;
    const double QYR=S.DischargeYM2PerS-2.0*QN*NY;
    const double UL=Velocity(S.DischargeXM2PerS,S.DepthM),VL=Velocity(S.DischargeYM2PerS,S.DepthM);
    const double UR=Velocity(QXR,S.DepthM),VR=Velocity(QYR,S.DepthM);
    const double UNL=UL*NX+VL*NY,UNR=UR*NX+VR*NY;
    const double C=std::sqrt(Gravity*S.DepthM);
    const double SL=std::min(0.0,std::min(UNL-C,UNR-C));
    const double SR=std::max(0.0,std::max(UNL+C,UNR+C));
    if(SR-SL<=1e-14)return {0.0,0.0};
    const double Pressure=0.5*Gravity*S.DepthM*S.DepthM;
    const double FLX=S.DischargeXM2PerS*UNL+Pressure*NX;
    const double FLY=S.DischargeYM2PerS*UNL+Pressure*NY;
    const double FRX=QXR*UNR+Pressure*NX,FRY=QYR*UNR+Pressure*NY;
    return {(SR*FLX-SL*FRX+SL*SR*(QXR-S.DischargeXM2PerS))/(SR-SL),
        (SR*FLY-SL*FRY+SL*SR*(QYR-S.DischargeYM2PerS))/(SR-SL)};
}

template<class T> void IndexOrder(std::vector<std::size_t>& O, const std::vector<T>& V)
{ O.resize(V.size()); std::iota(O.begin(), O.end(), std::size_t{0}); }
}

bool Solver::Initialize(const Domain& D, const std::vector<CellState>& Initial,
    const std::vector<StorageState>& InitialStorage, const Settings& S, std::string& Error,
    const std::vector<double>& InitialInfiltrationM3)
{
    Error.clear();
    const auto Fail = [&](const char* Why) { Error = Why; return false; };
    if (D.Cells.empty() || D.Cells.size() > MaximumCells || Initial.size() != D.Cells.size()
        || (!InitialInfiltrationM3.empty() && InitialInfiltrationM3.size() != D.Cells.size())
        || InitialStorage.size() != D.Storages.size() || D.Storages.size() > MaximumCells
        || D.Exchanges.size() > MaximumCells * 4 || D.Faces.size() > MaximumCells * 4
        || !HydrologySolverFinite(D.CellWidthM) || !HydrologySolverFinite(D.CellHeightM) || D.CellWidthM <= 0.0
        || D.CellHeightM <= 0.0 || !HydrologySolverFinite(S.CFL) || S.CFL <= 0.0 || S.CFL > 0.45
        || !HydrologySolverFinite(S.MaximumStepSeconds) || S.MaximumStepSeconds <= 0.0
        || !HydrologySolverFinite(S.DryMomentumDepthM) || S.DryMomentumDepthM < 0.0
        || !HydrologySolverFinite(S.NegativeDepthToleranceM) || S.NegativeDepthToleranceM < 0.0
        || S.MaximumStepsPerAdvance == 0 || S.MaximumStepsPerAdvance > 4096)
        return Fail("Invalid domain cardinality, SI dimensions or numerical settings");
    const double Area = D.CellWidthM * D.CellHeightM;
    if (!HydrologySolverFinite(Area) || Area <= 0.0) return Fail("Invalid cell area");
    // Payload estimate, not process working set: excludes allocator capacity,
    // validation sets, the caller's immutable snapshot and published frames.
    const std::uint64_t Bytes = D.Cells.size() * (sizeof(CellGeometry) + 3 * sizeof(CellState) + 8 * sizeof(double) + sizeof(std::size_t))
        + D.Faces.size() * (sizeof(FaceGeometry) + sizeof(FaceFlux) + sizeof(std::size_t))
        + D.Storages.size() * (sizeof(StorageGeometry) + 2 * sizeof(StorageState) + 3 * sizeof(double) + sizeof(std::size_t))
        + D.Exchanges.size() * (sizeof(StorageExchange) + sizeof(double) + sizeof(std::size_t));
    if (Bytes > std::min(S.MaximumOwnedBytes, MaximumSolverOwnedBytes)) return Fail("Configured solver-owned payload memory budget exceeded");
    std::set<std::pair<std::int32_t, std::int32_t>> Keys;
    double InitialSurfaceTotal = 0.0, InitialStorageTotal = 0.0;
    double InitialInfiltrationTotal = 0.0;
    for (std::size_t I = 0; I < D.Cells.size(); ++I)
    {
        const auto& C = D.Cells[I];
        if (!Keys.emplace(C.X, C.Y).second || !HydrologySolverFinite(C.BedM) || !HydrologySolverFinite(C.ManningN)
            || C.ManningN < 0.0 || C.ManningN > 1.0 || !HydrologySolverFinite(C.StoragePorosity)
            || C.StoragePorosity <= 0.0 || C.StoragePorosity > 1.0
            || !HydrologySolverFinite(Area*C.StoragePorosity) || Area*C.StoragePorosity <= 0.0 || !ValidState(Initial[I])
            || !HydrologySolverFinite(Initial[I].DepthM * (Area*C.StoragePorosity)) || !HydrologySolverFinite(C.BedM + Initial[I].DepthM)
            || !HydrologySolverFinite(Gravity * Initial[I].DepthM * Initial[I].DepthM))
            return Fail("Duplicate cell key, invalid bed/roughness or initial state");
        if (!InitialInfiltrationM3.empty())
        {
            const double Prior = InitialInfiltrationM3[I];
            if (!HydrologySolverFinite(Prior) || Prior < 0.0) return Fail("Invalid cumulative infiltration origin");
            InitialInfiltrationTotal += Prior;
            if (!HydrologySolverFinite(InitialInfiltrationTotal)) return Fail("Cumulative infiltration origin sum overflows");
        }
        InitialSurfaceTotal += Initial[I].DepthM * (Area*C.StoragePorosity);
        if (!HydrologySolverFinite(InitialSurfaceTotal)) return Fail("Initial surface volume sum overflows");
    }
    std::vector<unsigned> Sides(D.Cells.size(), 0u);
    std::set<std::int64_t> FaceIDs;
    for (const auto& F : D.Faces)
    {
        if (F.Left < 0 || static_cast<std::size_t>(F.Left) >= D.Cells.size()
            || F.Right < -1 || (F.Right >= 0 && static_cast<std::size_t>(F.Right) >= D.Cells.size())
            || F.Left == F.Right || !FaceIDs.insert(F.StableID).second)
            return Fail("Invalid face index or duplicate face identity");
        const bool XAxis = (F.NormalX == 1.0 || F.NormalX == -1.0) && F.NormalY == 0.0;
        const bool YAxis = (F.NormalY == 1.0 || F.NormalY == -1.0) && F.NormalX == 0.0;
        if ((!XAxis && !YAxis) || !HydrologySolverFinite(F.LengthM)
            || std::abs(F.LengthM - (XAxis ? D.CellHeightM : D.CellWidthM)) > 1e-9
            || !HydrologySolverFinite(F.OpenFraction) || F.OpenFraction < 0.0 || F.OpenFraction > 1.0
            || (F.HasSill && !HydrologySolverFinite(F.SillM)) || !HydrologySolverFinite(F.StageHeadM)
            || !HydrologySolverFinite(F.StageVelocityXMps) || !HydrologySolverFinite(F.StageVelocityYMps))
            return Fail("Only complete axis-aligned equal-area cell faces are supported");
        const unsigned Bit = F.NormalX > 0 ? 1u : F.NormalX < 0 ? 2u : F.NormalY > 0 ? 4u : 8u;
        const unsigned Opposite = Bit == 1u ? 2u : Bit == 2u ? 1u : Bit == 4u ? 8u : 4u;
        if (Sides[F.Left] & Bit) return Fail("Overlapping face on a cell side");
        Sides[F.Left] |= Bit;
        if (F.Right >= 0)
        {
            const auto& L = D.Cells[F.Left]; const auto& R = D.Cells[F.Right];
            if (static_cast<std::int64_t>(R.X) - L.X != static_cast<std::int64_t>(F.NormalX)
                || static_cast<std::int64_t>(R.Y) - L.Y != static_cast<std::int64_t>(F.NormalY)
                || (Sides[F.Right] & Opposite)) return Fail("Face does not join actual neighbouring cells");
            Sides[F.Right] |= Opposite;
            if (F.Kind != FaceKind::Interior && F.Kind != FaceKind::HydraulicStructure)
                return Fail("Interior face has a boundary kind");
        }
        else if (F.Kind != FaceKind::Wall && F.Kind != FaceKind::StageBoundary && F.Kind != FaceKind::FreeOutflow)
            return Fail("Boundary face has an interior kind");
        if (F.Kind == FaceKind::HydraulicStructure
            && (!HydrologySolverFinite(F.WeirCrestM) || !HydrologySolverFinite(F.WeirCoefficientM05PerS) || F.WeirCoefficientM05PerS < 0.0
                || !HydrologySolverFinite(F.GateInvertM) || !HydrologySolverFinite(F.GateHeightM) || F.GateHeightM < 0.0
                || !HydrologySolverFinite(F.GateOpenFraction) || F.GateOpenFraction < 0.0 || F.GateOpenFraction > 1.0
                || !HydrologySolverFinite(F.GateDischargeCoefficient) || F.GateDischargeCoefficient < 0.0))
            return Fail("Invalid hydraulic structure coefficients");
    }
    for (unsigned Side : Sides) if (Side != 15u) return Fail("Every cell needs exactly four declared faces");
    std::set<std::int64_t> StorageIDs, ExchangeIDs;
    for (std::size_t I = 0; I < D.Storages.size(); ++I)
    {
        const auto& R = D.Storages[I];
        if (!StorageIDs.insert(R.StableID).second || !HydrologySolverFinite(R.BaseElevationM)
            || !HydrologySolverFinite(R.SurfaceAreaM2) || R.SurfaceAreaM2 <= 0.0 || !HydrologySolverFinite(R.CapacityM3)
            || R.CapacityM3 < 0.0 || !HydrologySolverFinite(InitialStorage[I].VolumeM3)
            || InitialStorage[I].VolumeM3 < 0.0 || InitialStorage[I].VolumeM3 > R.CapacityM3
            || !HydrologySolverFinite(R.BaseElevationM + R.CapacityM3 / R.SurfaceAreaM2)
            || !HydrologySolverFinite(R.BaseElevationM + InitialStorage[I].VolumeM3 / R.SurfaceAreaM2))
            return Fail("Invalid finite storage geometry or initial volume");
        InitialStorageTotal += InitialStorage[I].VolumeM3;
        if (!HydrologySolverFinite(InitialStorageTotal)) return Fail("Initial finite-storage volume sum overflows");
    }
    for (const auto& E : D.Exchanges)
        if (!ExchangeIDs.insert(E.StableID).second || E.Cell < 0 || E.Storage < 0
            || static_cast<std::size_t>(E.Cell) >= D.Cells.size() || static_cast<std::size_t>(E.Storage) >= D.Storages.size()
            || !HydrologySolverFinite(E.InvertM) || !HydrologySolverFinite(E.ApertureAreaM2) || E.ApertureAreaM2 < 0.0
            || !HydrologySolverFinite(E.DischargeCoefficient) || E.DischargeCoefficient < 0.0
            || !HydrologySolverFinite(E.MaximumAbsFlowM3PerS) || E.MaximumAbsFlowM3PerS < 0.0
            || !HydrologySolverFinite(E.DirectionX) || !HydrologySolverFinite(E.DirectionY)
            || std::abs(E.DirectionX * E.DirectionX + E.DirectionY * E.DirectionY - 1.0) > 1e-9)
            return Fail("Invalid storage exchange");
    // Commit only after the full immutable geometry and state passed validation.
    GeometryData = D; Options = S; State = Initial; Stored = InitialStorage;
    IndexOrder(CellOrder, D.Cells); IndexOrder(FaceOrder, D.Faces);
    IndexOrder(StorageOrder, D.Storages); IndexOrder(ExchangeOrder, D.Exchanges);
    std::sort(CellOrder.begin(), CellOrder.end(), [&](std::size_t A, std::size_t B)
        { return std::tie(D.Cells[A].Y, D.Cells[A].X) < std::tie(D.Cells[B].Y, D.Cells[B].X); });
    std::sort(FaceOrder.begin(), FaceOrder.end(), [&](std::size_t A, std::size_t B)
        { return D.Faces[A].StableID < D.Faces[B].StableID; });
    std::sort(StorageOrder.begin(), StorageOrder.end(), [&](std::size_t A, std::size_t B)
        { return D.Storages[A].StableID < D.Storages[B].StableID; });
    std::sort(ExchangeOrder.begin(), ExchangeOrder.end(), [&](std::size_t A, std::size_t B)
        { return D.Exchanges[A].StableID < D.Exchanges[B].StableID; });
    if (InitialInfiltrationM3.empty()) InfiltratedM3.assign(State.size(), 0.0);
    else InfiltratedM3 = InitialInfiltrationM3;
    CandidateInfiltratedM3.resize(State.size());
    Candidate.resize(State.size()); Next.resize(State.size()); CandidateStored.resize(Stored.size());
    Fluxes.resize(D.Faces.size()); Outgoing.resize(State.size()); DonorScale.resize(State.size());
    ExchangeRates.resize(D.Exchanges.size()); ExchangeCellOut.resize(State.size());
    ExchangeStorageOut.resize(Stored.size()); ExchangeStorageIn.resize(Stored.size());
    DeltaH.resize(State.size()); DeltaQX.resize(State.size()); DeltaQY.resize(State.size()); DeltaStorage.resize(Stored.size());
    Accounting = {}; ClockSeconds = 0.0; OwnedBytes = Bytes; Ready = true;
    for (auto I : CellOrder) Accounting.InitialSurfaceM3 += State[I].DepthM * (Area*GeometryData.Cells[I].StoragePorosity);
    for (auto I : StorageOrder) Accounting.InitialStorageM3 += Stored[I].VolumeM3;
    RefreshBudget(); return true;
}

bool Solver::ValidateForcing(const Forcing& F, std::string& Error) const
{
    if ((!F.Cells.empty() && F.Cells.size() != State.size())
        || (!F.Boundaries.empty() && F.Boundaries.size() != GeometryData.Faces.size())
        || (!F.StorageExternalOutflowM3PerS.empty() && F.StorageExternalOutflowM3PerS.size() != Stored.size()))
    { Error = "Forcing cardinality differs from immutable domain"; return false; }
    for (const auto& C : F.Cells)
        if (!HydrologySolverFinite(C.RainMPerS) || C.RainMPerS < 0.0 || !HydrologySolverFinite(C.InfiltrationCapacityMPerS) || C.InfiltrationCapacityMPerS < 0.0
            || !HydrologySolverFinite(C.ExternalDrainCapacityM3PerS) || C.ExternalDrainCapacityM3PerS < 0.0
            || !HydrologySolverFinite(C.EvaporationMPerS) || C.EvaporationMPerS < 0.0
            || !HydrologySolverFinite(C.ExternalInflowM3PerS) || C.ExternalInflowM3PerS < 0.0
            || !HydrologySolverFinite(C.InflowVelocityXMps) || !HydrologySolverFinite(C.InflowVelocityYMps))
        { Error = "Invalid source, sink or inflow velocity"; return false; }
    for (const auto& B : F.Boundaries)
        if (!HydrologySolverFinite(B.HeadM) || !HydrologySolverFinite(B.VelocityXMps) || !HydrologySolverFinite(B.VelocityYMps))
        { Error = "Invalid prescribed boundary state"; return false; }
    for (double Q : F.StorageExternalOutflowM3PerS) if (!HydrologySolverFinite(Q) || Q < 0.0)
    { Error = "Invalid declared storage outflow"; return false; }
    return true;
}

double Solver::StableStep(const std::vector<CellState>& S, const Forcing& F) const
{
    double DT = Options.MaximumStepSeconds;
    for (auto I : CellOrder)
    {
        const auto& C = S[I]; const double Wave = std::sqrt(Gravity * C.DepthM);
        const double Speed = (std::abs(Velocity(C.DischargeXM2PerS, C.DepthM)) + Wave) / GeometryData.CellWidthM
            + (std::abs(Velocity(C.DischargeYM2PerS, C.DepthM)) + Wave) / GeometryData.CellHeightM;
        if (Speed > 0.0) DT = std::min(DT, Options.CFL * GeometryData.Cells[I].StoragePorosity / Speed);
    }
    // A dry domain must see an incoming stage wave BEFORE the first wet step.
    for (auto I : FaceOrder)
    {
        const auto& Face = GeometryData.Faces[I];
        if (Face.Kind == FaceKind::HydraulicStructure)
        {
            const double HL = S[Face.Left].DepthM, HR = S[Face.Right].DepthM;
            const double HeadL = GeometryData.Cells[Face.Left].BedM + HL;
            const double HeadR = GeometryData.Cells[Face.Right].BedM + HR;
            const double Jet = std::sqrt(2.0 * Gravity * std::abs(HeadL - HeadR));
            const double Wave = std::sqrt(Gravity * std::max(HL, HR));
            const double Speed = (Jet * std::abs(Face.NormalX) + Wave) / GeometryData.CellWidthM
                + (Jet * std::abs(Face.NormalY) + Wave) / GeometryData.CellHeightM;
            if (Speed > 0.0) DT = std::min(DT, Options.CFL * std::min(GeometryData.Cells[Face.Left].StoragePorosity,GeometryData.Cells[Face.Right].StoragePorosity) / Speed);
        }
        if (Face.Kind != FaceKind::StageBoundary) continue;
        const BoundaryForcing B = F.Boundaries.empty() ? BoundaryForcing{Face.StageHeadM, Face.StageVelocityXMps, Face.StageVelocityYMps} : F.Boundaries[I];
        const double H = Positive(B.HeadM - GeometryData.Cells[Face.Left].BedM);
        const double Speed = (std::abs(B.VelocityXMps) + std::sqrt(Gravity * H)) / GeometryData.CellWidthM
            + (std::abs(B.VelocityYMps) + std::sqrt(Gravity * H)) / GeometryData.CellHeightM;
        if (Speed > 0.0) DT = std::min(DT, Options.CFL * GeometryData.Cells[Face.Left].StoragePorosity / Speed);
    }
    return DT;
}

Solver::FaceFlux Solver::EvaluateFace(std::size_t I, const std::vector<CellState>& S, const Forcing& F) const
{
    const auto& Face = GeometryData.Faces[I];
    const auto& L = S[Face.Left]; CellState R = Face.Right >= 0 ? S[Face.Right] : L;
    const double ZL = GeometryData.Cells[Face.Left].BedM;
    double ZR = Face.Right >= 0 ? GeometryData.Cells[Face.Right].BedM : ZL;
    const double NX = Face.NormalX, NY = Face.NormalY;
    if (Face.Kind == FaceKind::Wall)
    {
        const double QN = L.DischargeXM2PerS * NX + L.DischargeYM2PerS * NY;
        R.DischargeXM2PerS -= 2.0 * QN * NX; R.DischargeYM2PerS -= 2.0 * QN * NY;
    }
    else if (Face.Kind == FaceKind::StageBoundary)
    {
        const BoundaryForcing B = F.Boundaries.empty() ? BoundaryForcing{Face.StageHeadM, Face.StageVelocityXMps, Face.StageVelocityYMps} : F.Boundaries[I];
        R.DepthM = Positive(B.HeadM - ZR); R.DischargeXM2PerS = R.DepthM * B.VelocityXMps; R.DischargeYM2PerS = R.DepthM * B.VelocityYMps;
    }
    else if (Face.Kind == FaceKind::FreeOutflow) R = {}; // Declared drop/outlet, not every map edge.
    const double UL = Velocity(L.DischargeXM2PerS, L.DepthM), VL = Velocity(L.DischargeYM2PerS, L.DepthM);
    const double UR = Velocity(R.DischargeXM2PerS, R.DepthM), VR = Velocity(R.DischargeYM2PerS, R.DepthM);
    FaceFlux Out;
    if (Face.Kind == FaceKind::HydraulicStructure)
    {
        const double EtaL = ZL + L.DepthM, EtaR = ZR + R.DepthM;
        const double HL = Positive(EtaL - Face.WeirCrestM), HR = Positive(EtaR - Face.WeirCrestM);
        // Explicit broad-crested approximation; submergence is approximated by
        // the difference of positive head powers. This is NOT an exact weir law.
        double Q = Face.WeirCoefficientM05PerS * (std::pow(HL, 1.5) - std::pow(HR, 1.5));
        const double HeadDifference = EtaL - EtaR;
        const double AvailableOpening = std::min(Face.GateHeightM, Positive(std::max(EtaL, EtaR) - Face.GateInvertM));
        Q += (HeadDifference >= 0.0 ? 1.0 : -1.0) * Face.GateDischargeCoefficient * Face.GateOpenFraction
            * AvailableOpening * std::sqrt(2.0 * Gravity * std::abs(HeadDifference));
        if ((Q > 0.0 && L.DepthM == 0.0) || (Q < 0.0 && R.DepthM == 0.0)) Q = 0.0;
        const double UpstreamX = Q >= 0.0 ? UL : UR, UpstreamY = Q >= 0.0 ? VL : VR;
        const double UpstreamN = UpstreamX * NX + UpstreamY * NY;
        // Lumped contraction/jet approximation: release into a dry downstream
        // cell must carry momentum, even when the upstream reservoir is still.
        // Do not subtract that newly generated jet impulse from a stationary
        // upstream reservoir as if it were pre-existing advected momentum.
        const double JetN = (Q >= 0.0 ? 1.0 : -1.0) * std::sqrt(2.0 * Gravity * std::abs(HeadDifference));
        const double JetX = UpstreamX + (JetN - UpstreamN) * NX;
        const double JetY = UpstreamY + (JetN - UpstreamN) * NY;
        Out.Mass = Q; Out.QX = Q * UpstreamX; Out.QY = Q * UpstreamY;
        if (Q >= 0.0) { Out.RightTransferX = Q * (JetX - UpstreamX); Out.RightTransferY = Q * (JetY - UpstreamY); }
        else { Out.LeftTransferX = Q * (JetX - UpstreamX); Out.LeftTransferY = Q * (JetY - UpstreamY); }
        // Separate wall/structure pressure reaction; not falsely advertised as
        // global momentum conservation across an anchored dam.
        Out.LeftCorrectionX = 0.5 * Gravity * L.DepthM * L.DepthM * NX;
        Out.LeftCorrectionY = 0.5 * Gravity * L.DepthM * L.DepthM * NY;
        Out.RightCorrectionX = 0.5 * Gravity * R.DepthM * R.DepthM * NX;
        Out.RightCorrectionY = 0.5 * Gravity * R.DepthM * R.DepthM * NY;
        return ApplyOpenFraction(Out,L,R,NX,NY,Face.OpenFraction);
    }
    const double ZStar = Face.HasSill ? std::max(std::max(ZL, ZR), Face.SillM) : std::max(ZL, ZR);
    const double HL = Positive(ZL + L.DepthM - ZStar), HR = Positive(ZR + R.DepthM - ZStar);
    const double QXL = HL * UL, QYL = HL * VL, QXR = HR * UR, QYR = HR * VR;
    const double UNL = UL * NX + VL * NY, UNR = UR * NX + VR * NY;
    const double CL = std::sqrt(Gravity * HL), CR = std::sqrt(Gravity * HR);
    const double SL = std::min(0.0, std::min(UNL - CL, UNR - CR));
    const double SR = std::max(0.0, std::max(UNL + CL, UNR + CR));
    const double FLH = HL * UNL, FRH = HR * UNR;
    const double FLX = QXL * UNL + 0.5 * Gravity * HL * HL * NX;
    const double FRX = QXR * UNR + 0.5 * Gravity * HR * HR * NX;
    const double FLY = QYL * UNL + 0.5 * Gravity * HL * HL * NY;
    const double FRY = QYR * UNR + 0.5 * Gravity * HR * HR * NY;
    if (SR - SL > 1e-14)
    {
        const double Inverse = 1.0 / (SR - SL);
        Out.Mass = (SR * FLH - SL * FRH + SL * SR * (HR - HL)) * Inverse;
        Out.QX = (SR * FLX - SL * FRX + SL * SR * (QXR - QXL)) * Inverse;
        Out.QY = (SR * FLY - SL * FRY + SL * SR * (QYR - QYL)) * Inverse;
    }
    Out.LeftCorrectionX = 0.5 * Gravity * (L.DepthM * L.DepthM - HL * HL) * NX;
    Out.LeftCorrectionY = 0.5 * Gravity * (L.DepthM * L.DepthM - HL * HL) * NY;
    Out.RightCorrectionX = 0.5 * Gravity * (R.DepthM * R.DepthM - HR * HR) * NX;
    Out.RightCorrectionY = 0.5 * Gravity * (R.DepthM * R.DepthM - HR * HR) * NY;
    return ApplyOpenFraction(Out,L,R,NX,NY,Face.OpenFraction);
}

Solver::FaceFlux Solver::ApplyOpenFraction(const FaceFlux& Base,const CellState& L,
    const CellState& R,double NX,double NY,double Theta) const
{
    if(Theta==1.0)return Base; // Exact legacy branch, including wall/structure law.
    FaceFlux Out=Base;
    const auto WallL=PorosityWallReaction(L,NX,NY);
    const auto WallR=PorosityWallReaction(R,-NX,-NY);
    Out.Mass*=Theta;Out.QX*=Theta;Out.QY*=Theta;
    Out.LeftTransferX*=Theta;Out.LeftTransferY*=Theta;
    Out.RightTransferX*=Theta;Out.RightTransferY*=Theta;
    // Right's outward wall normal is -N, so express its reaction in +N.
    Out.LeftCorrectionX=Theta*Base.LeftCorrectionX+(1.0-Theta)*WallL.first;
    Out.LeftCorrectionY=Theta*Base.LeftCorrectionY+(1.0-Theta)*WallL.second;
    Out.RightCorrectionX=Theta*Base.RightCorrectionX-(1.0-Theta)*WallR.first;
    Out.RightCorrectionY=Theta*Base.RightCorrectionY-(1.0-Theta)*WallR.second;
    return Out;
}

void Solver::ApplySources(double DT, const Forcing& F, std::vector<CellState>& S,
    std::vector<StorageState>& Reservoirs, Budget& B, std::vector<double>& InfiltrationM3) const
{
    const double Area = GeometryData.CellWidthM * GeometryData.CellHeightM;
    if (!F.Cells.empty()) for (auto I : CellOrder)
    {
        auto& C = S[I]; const auto& In = F.Cells[I];
        const double Phi=GeometryData.Cells[I].StoragePorosity,FluidArea=Area*Phi;
        // Rain falls on the whole gross catchment. With no separate roof
        // storage it runs off locally into the open water area; never delete it.
        const double GrossRain = In.RainMPerS * DT;
        const double Rain=GrossRain/Phi, Incoming=In.ExternalInflowM3PerS*DT/FluidArea;
        C.DepthM += Rain + Incoming;
        C.DischargeXM2PerS += Incoming * In.InflowVelocityXMps;
        C.DischargeYM2PerS += Incoming * In.InflowVelocityYMps;
        B.RainM3 += GrossRain * Area; B.ExternalInflowM3 += Incoming * FluidArea;
        const double BeforeRemoval = C.DepthM;
        // Infiltration capacity is explicitly per OPEN soil area. The owner
        // must already exclude paved fractions; this is not gross-area capacity.
        const double Infiltration = std::min(C.DepthM, In.InfiltrationCapacityMPerS * DT); C.DepthM -= Infiltration;
        const double Drain = std::min(C.DepthM, In.ExternalDrainCapacityM3PerS * DT / FluidArea); C.DepthM -= Drain;
        const double Evap = std::min(C.DepthM, In.EvaporationMPerS * DT); C.DepthM -= Evap;
        const double Ratio = BeforeRemoval > 0.0 ? C.DepthM / BeforeRemoval : 0.0;
        C.DischargeXM2PerS *= Ratio; C.DischargeYM2PerS *= Ratio;
        const double RemovedM3 = Infiltration * FluidArea;
        InfiltrationM3[I] += RemovedM3;
        B.InfiltrationM3 += RemovedM3; B.ExternalDrainageM3 += Drain * FluidArea; B.EvaporationM3 += Evap * FluidArea;
    }
    if (!F.StorageExternalOutflowM3PerS.empty()) for (auto I : StorageOrder)
    {
        const double Out = std::min(Reservoirs[I].VolumeM3, F.StorageExternalOutflowM3PerS[I] * DT);
        Reservoirs[I].VolumeM3 -= Out; B.StorageExternalOutflowM3 += Out;
    }
}

void Solver::ApplyExchanges(double DT, std::vector<CellState>& S, std::vector<StorageState>& R, Budget& B)
{
    const double Area = GeometryData.CellWidthM * GeometryData.CellHeightM;
    std::fill(ExchangeCellOut.begin(), ExchangeCellOut.end(), 0.0);
    std::fill(ExchangeStorageOut.begin(), ExchangeStorageOut.end(), 0.0);
    std::fill(ExchangeStorageIn.begin(), ExchangeStorageIn.end(), 0.0);
    std::fill(DeltaH.begin(), DeltaH.end(), 0.0); std::fill(DeltaQX.begin(), DeltaQX.end(), 0.0);
    std::fill(DeltaQY.begin(), DeltaQY.end(), 0.0); std::fill(DeltaStorage.begin(), DeltaStorage.end(), 0.0);
    for (auto I : ExchangeOrder)
    {
        const auto& E = GeometryData.Exchanges[I]; const auto& G = GeometryData.Storages[E.Storage];
        const double HC = std::max(E.InvertM, GeometryData.Cells[E.Cell].BedM + S[E.Cell].DepthM);
        const double HR = std::max(E.InvertM, G.BaseElevationM + R[E.Storage].VolumeM3 / G.SurfaceAreaM2);
        const double Difference = HC - HR;
        double Q = (Difference >= 0.0 ? 1.0 : -1.0) * std::min(E.MaximumAbsFlowM3PerS,
            E.DischargeCoefficient * E.ApertureAreaM2 * std::sqrt(2.0 * Gravity * std::abs(Difference)));
        if ((Q < 0.0 && !E.AllowBackflow) || (Q > 0.0 && S[E.Cell].DepthM == 0.0)) Q = 0.0;
        ExchangeRates[I] = Q;
        if (Q >= 0.0) { ExchangeCellOut[E.Cell] += Q; ExchangeStorageIn[E.Storage] += Q; }
        else ExchangeStorageOut[E.Storage] -= Q;
    }
    for (auto I : ExchangeOrder)
    {
        const auto& E = GeometryData.Exchanges[I]; const auto& G = GeometryData.Storages[E.Storage];
        const double CellArea=Area*GeometryData.Cells[E.Cell].StoragePorosity;
        double Q = ExchangeRates[I];
        if (Q >= 0.0)
        {
            const double Donor = ExchangeCellOut[E.Cell] > 0.0 ? std::min(1.0, S[E.Cell].DepthM * CellArea / (DT * ExchangeCellOut[E.Cell])) : 1.0;
            const double Capacity = ExchangeStorageIn[E.Storage] > 0.0 ? std::min(1.0, Positive(G.CapacityM3 - R[E.Storage].VolumeM3) / (DT * ExchangeStorageIn[E.Storage])) : 1.0;
            Q *= std::min(Donor, Capacity);
            const double H = Q * DT / CellArea;
            DeltaQX[E.Cell] -= H * Velocity(S[E.Cell].DischargeXM2PerS, S[E.Cell].DepthM);
            DeltaQY[E.Cell] -= H * Velocity(S[E.Cell].DischargeYM2PerS, S[E.Cell].DepthM);
            B.SurfaceToStorageM3 += Q * DT;
        }
        else
        {
            Q *= ExchangeStorageOut[E.Storage] > 0.0 ? std::min(1.0, R[E.Storage].VolumeM3 / (DT * ExchangeStorageOut[E.Storage])) : 1.0;
            const double Jet = E.ApertureAreaM2 > 0.0 ? -Q / E.ApertureAreaM2 : 0.0;
            const double H = -Q * DT / CellArea;
            DeltaQX[E.Cell] -= H * Jet * E.DirectionX; DeltaQY[E.Cell] -= H * Jet * E.DirectionY;
            B.StorageToSurfaceM3 -= Q * DT;
        }
        DeltaH[E.Cell] -= Q * DT / CellArea; DeltaStorage[E.Storage] += Q * DT;
    }
    for (auto I : CellOrder) { S[I].DepthM += DeltaH[I]; S[I].DischargeXM2PerS += DeltaQX[I]; S[I].DischargeYM2PerS += DeltaQY[I]; }
    for (auto I : StorageOrder) R[I].VolumeM3 += DeltaStorage[I];
}

bool Solver::TryStep(double DT, const Forcing& F, std::string& Error)
{
    Candidate = State; CandidateStored = Stored; Budget Pending = Accounting;
    // Retry scratch is reset from the accepted history on EVERY attempted DT.
    // Rejected source-CFL attempts never become pore-water inventory.
    CandidateInfiltratedM3 = InfiltratedM3;
    ApplySources(DT, F, Candidate, CandidateStored, Pending, CandidateInfiltratedM3);
    ApplyExchanges(DT, Candidate, CandidateStored, Pending);
    // Recheck after sources/exchanges: a dry-domain inflow must not bypass CFL.
    const double AllowedDT = StableStep(Candidate, F);
    if (!HydrologySolverFinite(AllowedDT) || DT > AllowedDT * (1.0 + 1e-12))
    { Error = "RetrySmallerStep"; return false; }
    const double Area = GeometryData.CellWidthM * GeometryData.CellHeightM;
    std::fill(Outgoing.begin(), Outgoing.end(), 0.0);
    for (auto I : FaceOrder)
    {
        const auto& Face = GeometryData.Faces[I]; const auto Flux = EvaluateFace(I, Candidate, F); Fluxes[I] = Flux;
        if (Flux.Mass > 0.0) Outgoing[Face.Left] += Flux.Mass * Face.LengthM;
        else if (Face.Right >= 0) Outgoing[Face.Right] -= Flux.Mass * Face.LengthM;
    }
    for (auto I : CellOrder) DonorScale[I] = Outgoing[I] > 0.0 ? std::min(1.0, Candidate[I].DepthM * (Area*GeometryData.Cells[I].StoragePorosity) / (DT * Outgoing[I])) : 1.0;
    Next = Candidate;
    for (auto I : FaceOrder)
    {
        const auto& Face = GeometryData.Faces[I]; const auto& Flux = Fluxes[I];
        const double Scale = Flux.Mass > 0.0 ? DonorScale[Face.Left] : Face.Right >= 0 ? DonorScale[Face.Right] : 1.0;
        const double Factor = DT * Face.LengthM / (Area*GeometryData.Cells[Face.Left].StoragePorosity);
        const double Mass = Flux.Mass * Scale, QX = Flux.QX * Scale, QY = Flux.QY * Scale;
        auto& L = Next[Face.Left]; L.DepthM -= Factor * Mass;
        L.DischargeXM2PerS -= Factor * (QX + Scale * Flux.LeftTransferX + Flux.LeftCorrectionX);
        L.DischargeYM2PerS -= Factor * (QY + Scale * Flux.LeftTransferY + Flux.LeftCorrectionY);
        if (Face.Right >= 0)
        {
            const double RightFactor=DT*Face.LengthM/(Area*GeometryData.Cells[Face.Right].StoragePorosity);
            auto& R = Next[Face.Right]; R.DepthM += RightFactor * Mass;
            R.DischargeXM2PerS += RightFactor * (QX + Scale * Flux.RightTransferX + Flux.RightCorrectionX);
            R.DischargeYM2PerS += RightFactor * (QY + Scale * Flux.RightTransferY + Flux.RightCorrectionY);
        }
        else if (Mass >= 0.0) Pending.BoundaryOutflowM3 += DT * Face.LengthM * Mass;
        else Pending.BoundaryInflowM3 -= DT * Face.LengthM * Mass;
    }
    for (auto I : CellOrder)
    {
        auto& C = Next[I];
        if (!HydrologySolverFinite(C.DepthM) || C.DepthM < -Options.NegativeDepthToleranceM
            || !HydrologySolverFinite(C.DischargeXM2PerS) || !HydrologySolverFinite(C.DischargeYM2PerS))
        { Error = "Non-finite or materially negative candidate; no step committed"; return false; }
        if (C.DepthM < 0.0) { Pending.NumericalVolumeCorrectionM3 -= C.DepthM * (Area*GeometryData.Cells[I].StoragePorosity); C.DepthM = 0.0; }
        if (C.DepthM <= Options.DryMomentumDepthM) { C.DischargeXM2PerS = 0.0; C.DischargeYM2PerS = 0.0; }
        else
        {
            const double Q = std::hypot(C.DischargeXM2PerS, C.DischargeYM2PerS);
            const double N = GeometryData.Cells[I].ManningN;
            const double Denominator = 1.0 + DT * Gravity * N * N * Q / std::pow(C.DepthM, 7.0 / 3.0);
            C.DischargeXM2PerS /= Denominator; C.DischargeYM2PerS /= Denominator;
        }
    }
    for (auto I : StorageOrder)
    {
        double& Volume = CandidateStored[I].VolumeM3;
        if (!HydrologySolverFinite(Volume) || Volume < -Options.NegativeDepthToleranceM * Area
            || Volume > GeometryData.Storages[I].CapacityM3 + Options.NegativeDepthToleranceM * Area)
        { Error = "HydrologySolverFinite storage volume/capacity violated; no step committed"; return false; }
        const double Corrected = Clamp(Volume, 0.0, GeometryData.Storages[I].CapacityM3);
        Pending.NumericalVolumeCorrectionM3 += Corrected - Volume; Volume = Corrected;
    }
    for (auto I : CellOrder)
        if (!HydrologySolverFinite(CandidateInfiltratedM3[I]) || CandidateInfiltratedM3[I] < InfiltratedM3[I])
        { Error = "Invalid cumulative infiltration candidate; no step committed"; return false; }
    State.swap(Next); Stored.swap(CandidateStored);
    InfiltratedM3.swap(CandidateInfiltratedM3); Pending.AcceptedSteps++;
    Pending.FaceEvaluations += GeometryData.Faces.size(); Accounting = Pending; ClockSeconds += DT;
    RefreshBudget(); return true;
}

AdvanceResult Solver::Advance(double Requested, const Forcing& F)
{
    AdvanceResult R; R.RemainingSeconds = Requested;
    if (!Ready || !HydrologySolverFinite(Requested) || Requested < 0.0)
    { R.Error = "Solver not initialized or invalid requested physical time"; return R; }
    if (!ValidateForcing(F, R.Error)) return R;
    while (R.RemainingSeconds > 0.0 && R.Steps < Options.MaximumStepsPerAdvance)
    {
        double DT = std::min(R.RemainingSeconds, StableStep(State, F));
        // A final substep can differ from the CFL quantum by a few ulps
        // (e.g. 0.013 after thirteen 0.001 steps). Integrate that complete
        // remainder under the SAME forcing instead of leaving an unschedulable
        // positive time debt. TryStep already permits this relative CFL
        // round-off. No seconds or source volume are discarded.
        if (DT > 0.0 && R.RemainingSeconds > DT
            && R.RemainingSeconds <= DT * (1.0 + 1e-12))
            DT = R.RemainingSeconds;
        bool Accepted = false;
        for (int Retry = 0; Retry < 24; ++Retry)
        {
            // A positive FINAL remainder may be smaller than the scheduling
            // resolution but remains a real source/CFL-safe integration. A
            // tiny nonfinal CFL quantum still fails without committing work.
            if (!HydrologySolverFinite(DT) || DT <= 0.0 || (DT <= 1e-12 && DT != R.RemainingSeconds))
            { R.Error = "CFL step underflow; unconsumed time retained"; return R; }
            std::string Error;
            if (TryStep(DT, F, Error)) { Accepted = true; break; }
            if (Error != "RetrySmallerStep") { R.Error = Error; return R; }
            DT *= 0.5;
        }
        if (!Accepted) { R.Error = "Bounded source-CFL retries exhausted; no attempted step committed"; return R; }
        R.Steps++;
        R.RemainingSeconds = DT == R.RemainingSeconds ? 0.0 : R.RemainingSeconds - DT;
        R.AdvancedSeconds = Requested - R.RemainingSeconds;
    }
    R.Success = true; return R; // Remaining>0 means budget exhaustion, NOT completion.
}

void Solver::RefreshBudget()
{
    Accounting.FinalSurfaceM3 = 0.0; Accounting.FinalStorageM3 = 0.0;
    const double Area = GeometryData.CellWidthM * GeometryData.CellHeightM;
    for (auto I : CellOrder) Accounting.FinalSurfaceM3 += State[I].DepthM * (Area*GeometryData.Cells[I].StoragePorosity);
    for (auto I : StorageOrder) Accounting.FinalStorageM3 += Stored[I].VolumeM3;
    Accounting.ResidualM3 = Accounting.InitialSurfaceM3 + Accounting.InitialStorageM3 + Accounting.RainM3
        + Accounting.ExternalInflowM3 + Accounting.BoundaryInflowM3 + Accounting.NumericalVolumeCorrectionM3
        - Accounting.BoundaryOutflowM3 - Accounting.InfiltrationM3 - Accounting.ExternalDrainageM3
        - Accounting.EvaporationM3 - Accounting.StorageExternalOutflowM3
        - Accounting.FinalSurfaceM3 - Accounting.FinalStorageM3;
}

double Solver::SurfaceHeadM(std::size_t I) const
{ return I < State.size() ? GeometryData.Cells[I].BedM + State[I].DepthM : std::numeric_limits<double>::quiet_NaN(); }

std::int32_t Solver::FindCell(std::int32_t X, std::int32_t Y) const
{
    const auto It = std::lower_bound(CellOrder.begin(), CellOrder.end(), std::make_pair(Y, X), [&](std::size_t I, const auto& Key)
        { return std::make_pair(GeometryData.Cells[I].Y, GeometryData.Cells[I].X) < Key; });
    return It != CellOrder.end() && GeometryData.Cells[*It].X == X && GeometryData.Cells[*It].Y == Y
        ? static_cast<std::int32_t>(*It) : -1;
}
}
