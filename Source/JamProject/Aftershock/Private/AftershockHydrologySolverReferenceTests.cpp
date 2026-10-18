#include "AftershockHydrologySolverReferenceTests.h"
#include "AftershockHydrologySolver.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace AftershockWater
{
namespace
{
Domain RectangularDomain(int W, int H, int OriginX = -2, int OriginY = -3)
{
    Domain D; D.CellWidthM = 10.0; D.CellHeightM = 10.0;
    for (int Y = 0; Y < H; ++Y) for (int X = 0; X < W; ++X)
        D.Cells.push_back({X + OriginX, Y + OriginY, 0.0, 0.0});
    std::int64_t ID = 1;
    const auto Add = [&](int L, int R, double NX, double NY, FaceKind K)
    { FaceGeometry F; F.Left = L; F.Right = R; F.NormalX = NX; F.NormalY = NY;
      F.LengthM = 10.0; F.Kind = K; F.StableID = ID++; D.Faces.push_back(F); };
    for (int Y = 0; Y < H; ++Y) for (int X = 0; X < W; ++X)
    {
        const int I = X + Y * W;
        if (X + 1 < W) Add(I, I + 1, 1, 0, FaceKind::Interior); else Add(I, -1, 1, 0, FaceKind::Wall);
        if (Y + 1 < H) Add(I, I + W, 0, 1, FaceKind::Interior); else Add(I, -1, 0, 1, FaceKind::Wall);
        if (X == 0) Add(I, -1, -1, 0, FaceKind::Wall);
        if (Y == 0) Add(I, -1, 0, -1, FaceKind::Wall);
    }
    return D;
}
bool RunSeconds(Solver& S, double Duration, const Forcing& F = {})
{
    double Remaining = Duration;
    for (int Pass = 0; Pass < 100000 && Remaining > 1e-10; ++Pass)
    {
        const auto R = S.Advance(Remaining, F);
        if (!R.Success || R.AdvancedSeconds <= 0.0) return false;
        Remaining = R.RemainingSeconds;
    }
    return Remaining <= 1e-10;
}
bool FiniteNonnegative(const Solver& S)
{
    for (const auto& C : S.Cells())
        if (!std::isfinite(C.DepthM) || C.DepthM < 0.0 || !std::isfinite(C.DischargeXM2PerS)
            || !std::isfinite(C.DischargeYM2PerS)) return false;
    for (const auto& R : S.Storages()) if (!std::isfinite(R.VolumeM3) || R.VolumeM3 < 0.0) return false;
    return std::isfinite(S.VolumeBudget().ResidualM3);
}
double MaxSpeed(const Solver& S)
{
    double V = 0.0;
    for (const auto& C : S.Cells()) if (C.DepthM > 1e-12)
        V = std::max(V, std::hypot(C.DischargeXM2PerS, C.DischargeYM2PerS) / C.DepthM);
    return V;
}
bool EqualState(const std::vector<CellState>& A, const std::vector<CellState>& B, double Tolerance = 0.0)
{
    if (A.size() != B.size()) return false;
    for (std::size_t I = 0; I < A.size(); ++I)
        if (std::abs(A[I].DepthM - B[I].DepthM) > Tolerance
            || std::abs(A[I].DischargeXM2PerS - B[I].DischargeXM2PerS) > Tolerance
            || std::abs(A[I].DischargeYM2PerS - B[I].DischargeYM2PerS) > Tolerance) return false;
    return true;
}
}

std::vector<ReferenceCheck> RunHydrologyReferenceChecks()
{
    std::vector<ReferenceCheck> C;
    const auto Check = [&](const char* N, bool P) { C.push_back({N, P}); };
    const auto Make = [&](Solver& S, const Domain& D, const std::vector<CellState>& V,
        const std::vector<StorageState>& R = std::vector<StorageState>{}, Settings Opt = Settings{})
    { std::string Error; return S.Initialize(D, V, R, Opt, Error); };

    // 1. Uniform rain, isolated external sinks and closed mass ledger.
    {
        auto D = RectangularDomain(4, 3); Solver S;
        std::vector<CellState> Initial(D.Cells.size()); Forcing F; F.Cells.resize(Initial.size());
        for (auto& Source : F.Cells) { Source.RainMPerS = .001; Source.InfiltrationCapacityMPerS = .0001;
            Source.ExternalDrainCapacityM3PerS = .002; Source.EvaporationMPerS = .00001; }
        const bool OK = Make(S, D, Initial) && RunSeconds(S, 20.0, F);
        const auto& B = S.VolumeBudget();
        Check("rainfall_si_area_time_24m3", OK && std::abs(B.RainM3 - 24.0) < 1e-8);
        Check("infiltration_capacity_2p4m3", OK && std::abs(B.InfiltrationM3 - 2.4) < 1e-8);
        double CellInfiltrationTotal=0.;bool PerCell=S.CumulativeInfiltrationM3().size()==Initial.size();
        for(double Value:S.CumulativeInfiltrationM3()){CellInfiltrationTotal+=Value;PerCell=PerCell&&std::abs(Value-.2)<1e-10;}
        Check("uniform_cell_infiltration_matches_actual_si_area_and_time",OK&&PerCell);
        Check("cell_infiltration_sum_matches_global_source_ledger",OK&&std::abs(CellInfiltrationTotal-B.InfiltrationM3)<1e-10);
        Check("declared_external_drain_0p48m3", OK && std::abs(B.ExternalDrainageM3 - .48) < 1e-8);
        Check("evaporation_0p24m3", OK && std::abs(B.EvaporationM3 - .24) < 1e-8);
        Check("closed_rain_sinks_mass_residual", OK && std::abs(B.ResidualM3) < 1e-8 && FiniteNonnegative(S));
        Check("negative_global_coordinate_lookup", OK && S.FindCell(-1, -1) >= 0 && S.FindCell(-999, -999) == -1);
    }
    // 2. Well-balanced lake over irregular bed; exact free-surface reference.
    {
        auto D = RectangularDomain(5, 4); std::vector<CellState> V(D.Cells.size());
        for (std::size_t I = 0; I < V.size(); ++I)
        { D.Cells[I].BedM = .07 * static_cast<double>((I * 7) % 11); V[I].DepthM = 2.0 - D.Cells[I].BedM; }
        Solver S; const bool OK = Make(S, D, V) && RunSeconds(S, 12.0);
        double EtaError = 0.0; for (std::size_t I = 0; I < V.size(); ++I) EtaError = std::max(EtaError, std::abs(S.SurfaceHeadM(I) - 2.0));
        Check("hydrostatic_irregular_bed_lake_at_rest", OK && EtaError < 1e-10 && MaxSpeed(S) < 1e-9);
        Check("closed_lake_mass_conserved", OK && std::abs(S.VolumeBudget().ResidualM3) < 1e-8);
        Solver Repeat; const bool Again = Make(Repeat, D, V) && RunSeconds(Repeat, 12.0);
        Check("repeatable_same_input_step_frontier", Again && EqualState(S.Cells(), Repeat.Cells()) && S.CompletedStep() == Repeat.CompletedStep());
    }
    // 3. Actual bed gradient drives flow, while dry/wet cells remain positive.
    {
        auto D = RectangularDomain(9, 1); std::vector<CellState> V(D.Cells.size());
        for (std::size_t I = 0; I < V.size(); ++I) { D.Cells[I].BedM = .04 * (8.0 - I); V[I].DepthM = I < 3 ? .6 : 0.0; }
        Solver S; const bool OK = Make(S, D, V) && RunSeconds(S, 10.0);
        double QX = 0.0; for (const auto& At : S.Cells()) QX += At.DischargeXM2PerS;
        Check("downhill_wet_front_moves_in_positive_x", OK && QX > 0.0 && S.Cells()[3].DepthM > 0.0);
        Check("dry_wet_front_positive_finite_and_mass_closed", OK && FiniteNonnegative(S) && std::abs(S.VolumeBudget().ResidualM3) < 1e-8);
    }
    // 4. Bank sill: upstream water cannot flood the neighbouring cell below crest.
    {
        auto D = RectangularDomain(2, 1); for (auto& Face : D.Faces) if (Face.Right >= 0) { Face.HasSill = true; Face.SillM = 1.0; }
        std::vector<CellState> Below(2); Below[0].DepthM = .5; Solver Closed;
        const bool A = Make(Closed, D, Below) && RunSeconds(Closed, 10.0);
        Check("river_bank_below_crest_no_overland_transfer", A && Closed.Cells()[1].DepthM == 0.0);
        std::vector<CellState> Above(2); Above[0].DepthM = 1.4; Solver Open;
        const bool B = Make(Open, D, Above) && RunSeconds(Open, 4.0);
        Check("river_bank_above_crest_flux_connected_overflow", B && Open.Cells()[1].DepthM > 0.0 && std::abs(Open.VolumeBudget().ResidualM3) < 1e-8);
    }
    // 5. Weir + gate: no percentage sink; actual existing volume crosses face.
    {
        auto D = RectangularDomain(2, 1);
        for (auto& Face : D.Faces) if (Face.Right >= 0)
        { Face.Kind = FaceKind::HydraulicStructure; Face.WeirCrestM = 1.0; Face.GateInvertM = 0.0; Face.GateHeightM = .5; }
        std::vector<CellState> V(2); V[0].DepthM = .7; Solver Closed;
        const bool A = Make(Closed, D, V) && RunSeconds(Closed, 4.0);
        Check("dam_closed_below_crest_stores_upstream_no_sink", A && Closed.Cells()[1].DepthM == 0.0 && std::abs(Closed.VolumeBudget().FinalSurfaceM3 - 70.0) < 1e-8);
        for (auto& Face : D.Faces) if (Face.Right >= 0) Face.GateOpenFraction = 1.0;
        Solver Gate; const bool B = Make(Gate, D, V) && RunSeconds(Gate, 4.0);
        Check("dam_gate_releases_same_existing_volume", B && Gate.Cells()[1].DepthM > 0.0 && std::abs(Gate.VolumeBudget().ResidualM3) < 1e-8);
        Solver Jet; const bool J = Make(Jet, D, V) && RunSeconds(Jet, .05);
        Check("dam_release_carries_downstream_jet_momentum", J && Jet.Cells()[1].DischargeXM2PerS > 0.0);
        for (auto& Face : D.Faces) if (Face.Right >= 0) Face.GateOpenFraction = 0.0;
        V[0].DepthM = 1.5; Solver Crest; const bool E = Make(Crest, D, V) && RunSeconds(Crest, 4.0);
        Check("dam_overtopping_above_crest_mass_conserved", E && Crest.Cells()[1].DepthM > 0.0 && std::abs(Crest.VolumeBudget().ResidualM3) < 1e-8);
    }
    // 6. Finite drain storage with actual backflow, capacity sharing and valve.
    {
        auto D = RectangularDomain(1, 1); D.Storages.push_back({1, 0.0, 1.0, 2.0});
        StorageExchange E; E.StableID = 1; E.Cell = 0; E.Storage = 0; E.ApertureAreaM2 = .05; E.MaximumAbsFlowM3PerS = .2;
        D.Exchanges.push_back(E); std::vector<CellState> V(1); V[0].DepthM = .1;
        Solver Back; const bool B = Make(Back, D, V, {{1.0}}) && RunSeconds(Back, 1.0);
        Check("drain_high_tailwater_causes_surface_backflow", B && Back.Cells()[0].DepthM > .1 && Back.Storages()[0].VolumeM3 < 1.0 && Back.VolumeBudget().StorageToSurfaceM3 > 0.0);
        Check("drain_backflow_internal_mass_not_external_sink", B && std::abs(Back.VolumeBudget().ResidualM3) < 1e-8 && Back.VolumeBudget().ExternalDrainageM3 == 0.0);
        D.Exchanges[0].AllowBackflow = false; Solver Valve;
        const bool A = Make(Valve, D, V, {{1.0}}) && RunSeconds(Valve, 1.0);
        Check("drain_check_valve_blocks_reverse_transfer", A && Valve.Cells()[0].DepthM == .1 && Valve.Storages()[0].VolumeM3 == 1.0);
        D.Storages[0].CapacityM3 = .01; D.Exchanges[0].AllowBackflow = true;
        D.Exchanges.push_back(D.Exchanges[0]); D.Exchanges[1].StableID = 2;
        V[0].DepthM = 2.0; Solver Capacity; const bool K = Make(Capacity, D, V, {{0.0}}) && RunSeconds(Capacity, 1.0);
        Check("multiple_inlets_share_finite_node_capacity", K && Capacity.Storages()[0].VolumeM3 <= .01 && Capacity.Storages()[0].VolumeM3 >= 0.0 && FiniteNonnegative(Capacity) && std::abs(Capacity.VolumeBudget().ResidualM3) < 1e-8);
    }
    // 7. Declared sea boundary contributes and later removes accounted volume.
    {
        auto D = RectangularDomain(3, 1); std::size_t Boundary = D.Faces.size();
        for (std::size_t I = 0; I < D.Faces.size(); ++I) if (D.Faces[I].Right == -1 && D.Faces[I].NormalX == -1.0)
        { Boundary = I; D.Faces[I].Kind = FaceKind::StageBoundary; D.Faces[I].StageHeadM = 1.0; }
        Solver S; const bool A = Make(S, D, std::vector<CellState>(3)) && RunSeconds(S, 3.0);
        Check("marine_boundary_inflow_accounted_on_dry_grid", A && S.VolumeBudget().BoundaryInflowM3 > 0.0 && S.Cells()[0].DepthM > 0.0 && std::abs(S.VolumeBudget().ResidualM3) < 1e-8);
        Forcing F; F.Boundaries.resize(D.Faces.size()); if (Boundary < F.Boundaries.size()) F.Boundaries[Boundary].HeadM = 0.0;
        const double Before = S.VolumeBudget().BoundaryOutflowM3; const bool B = A && RunSeconds(S, 4.0, F);
        Check("marine_withdrawal_returns_real_volume_to_boundary", B && S.VolumeBudget().BoundaryOutflowM3 > Before && std::abs(S.VolumeBudget().ResidualM3) < 1e-8);
    }
    // 8. Invalid inputs fail before mutation; work budget leaves pending time.
    {
        auto D = RectangularDomain(2, 2); std::vector<CellState> V(4); for (auto& At : V) At.DepthM = .4;
        Settings Opt; Opt.MaximumStepsPerAdvance = 1; Solver S; const bool A = Make(S, D, V, {}, Opt);
        const auto One = S.Advance(10.0, {});
        Check("bounded_advance_returns_unconsumed_time_not_fake_completion", A && One.Success && One.Steps == 1 && One.AdvancedSeconds > 0.0 && One.RemainingSeconds > 0.0 && std::abs(One.AdvancedSeconds + One.RemainingSeconds - 10.0) < 1e-12);
        const auto Before = S.Cells(); const double Clock = S.SimulatedSeconds();
        Forcing Bad; Bad.Cells.resize(4); Bad.Cells[0].RainMPerS = std::numeric_limits<double>::quiet_NaN();
        const auto Reject = S.Advance(1.0, Bad);
        Check("invalid_forcing_rejected_without_state_or_clock_mutation", !Reject.Success && EqualState(Before, S.Cells()) && S.SimulatedSeconds() == Clock);
        D.Cells[1].X = D.Cells[0].X; D.Cells[1].Y = D.Cells[0].Y; std::string Error;
        Check("invalid_reinitialization_preserves_prior_solver", !S.Initialize(D, V, {}, Opt, Error) && EqualState(Before, S.Cells()) && S.SimulatedSeconds() == Clock);
    }
    // 9. Face/container insertion does not select a different numerical order.
    {
        auto D = RectangularDomain(4, 3); std::vector<CellState> V(D.Cells.size());
        for (std::size_t I = 0; I < V.size(); ++I) { V[I].DepthM = .1 + .02 * (I % 5); D.Cells[I].BedM = .01 * (I % 3); }
        Solver A, B; const bool First = Make(A, D, V) && RunSeconds(A, 2.0);
        std::reverse(D.Faces.begin(), D.Faces.end()); // IDs + geometry unchanged.
        const bool Second = Make(B, D, V) && RunSeconds(B, 2.0);
        Check("stable_face_identity_removes_container_insertion_bias", First && Second && EqualState(A.Cells(), B.Cells()));
    }
    {
        auto D = RectangularDomain(2, 2); Solver S;
        const bool OK = Make(S, D, std::vector<CellState>(4)) && RunSeconds(S, 5.0);
        Check("no_undeclared_source_in_closed_dry_world", OK && S.VolumeBudget().FinalSurfaceM3 == 0.0
            && S.VolumeBudget().BoundaryInflowM3 == 0.0 && S.VolumeBudget().ExternalInflowM3 == 0.0 && S.VolumeBudget().RainM3 == 0.0);
        Settings TooSmall; TooSmall.MaximumOwnedBytes = 1; std::string Error; const auto Before = S.Cells();
        Check("owned_payload_memory_limit_fails_before_mutation", !S.Initialize(D, std::vector<CellState>(4), {}, TooSmall, Error) && EqualState(Before, S.Cells()));
    }
    {
        auto D=RectangularDomain(1,1);D.Storages.push_back({1,0.0,1e-308,1e308});
        Solver S;std::string Error;
        Check("finite_storage_fields_with_overflowing_head_fail_closed",
            !S.Initialize(D,std::vector<CellState>(1),std::vector<StorageState>{{1e308}},Settings{},Error));
    }
    // Per-cell absolute history follows the accepted source ledger. A rejected
    // source-CFL attempt must never silently become canonical pore water.
    {
        auto D = RectangularDomain(1, 1); std::vector<CellState> V(1); V[0].DepthM = .1;
        Settings Options; Options.MaximumStepsPerAdvance = 1; Solver S; std::string Error;
        const bool Ready = S.Initialize(D, V, {}, Options, Error, {1.25});
        Forcing F; F.Cells.resize(1); F.Cells[0].RainMPerS = 1e300; F.Cells[0].InfiltrationCapacityMPerS = .01;
        const auto R = S.Advance(.25, F);
        Check("source_cfl_rejected_attempts_do_not_commit_cell_infiltration", Ready && !R.Success && R.AdvancedSeconds == 0.
            && S.CumulativeInfiltrationM3()[0] == 1.25 && S.VolumeBudget().InfiltrationM3 == 0.
            && S.SimulatedSeconds() == 0. && EqualState(V, S.Cells()));
    }
    {
        auto D = RectangularDomain(1, 1); std::vector<CellState> V(1);
        Settings Options; Options.MaximumStepsPerAdvance = 1; Solver S; std::string Error;
        const bool Ready = S.Initialize(D, V, {}, Options, Error);
        Forcing F; F.Cells.resize(1); F.Cells[0].RainMPerS = 40.; F.Cells[0].InfiltrationCapacityMPerS = .01;
        const auto R = S.Advance(.25, F);
        Check("accepted_smaller_cfl_retry_commits_only_accepted_infiltration", Ready && R.Success && R.Steps == 1
            && R.AdvancedSeconds > 0. && R.AdvancedSeconds < .25
            && std::abs(S.CumulativeInfiltrationM3()[0] - R.AdvancedSeconds * .01 * 100.) < 1e-12
            && std::abs(S.CumulativeInfiltrationM3()[0] - S.VolumeBudget().InfiltrationM3) < 1e-12);
    }
    {
        auto D = RectangularDomain(1, 1); std::vector<CellState> V(1); V[0].DepthM = .1;
        Solver S; std::string Error; const bool Ready = S.Initialize(D, V, {}, Settings{}, Error, {1.25});
        Forcing F; F.Cells.resize(1); F.Cells[0].InfiltrationCapacityMPerS = .01;
        const bool Advanced = Ready && RunSeconds(S, 1., F);
        Check("infiltration_origin_is_preserved_without_double_adding_local_budget", Advanced
            && std::abs(S.CumulativeInfiltrationM3()[0] - 2.25) < 1e-10
            && std::abs(S.VolumeBudget().InfiltrationM3 - 1.) < 1e-10 && std::abs(S.VolumeBudget().ResidualM3) < 1e-10);
        const auto Before = S.CumulativeInfiltrationM3(); const auto BeforeState = S.Cells(); const double Clock = S.SimulatedSeconds();
        const bool SizeRejected = !S.Initialize(D, V, {}, Settings{}, Error, {1., 2.});
        const bool NegativeRejected = !S.Initialize(D, V, {}, Settings{}, Error, {-1.});
        Check("invalid_infiltration_origin_cannot_replace_existing_frontier", Advanced && SizeRejected && NegativeRejected
            && S.CumulativeInfiltrationM3() == Before && EqualState(S.Cells(), BeforeState) && S.SimulatedSeconds() == Clock);
    }
    {
        auto D=RectangularDomain(1,1);Settings Options;
        Options.MaximumStepSeconds=.001;Options.MaximumStepsPerAdvance=8;
        Solver S;const bool Ready=Make(S,D,std::vector<CellState>(1),{},Options);
        Forcing F;F.Cells.resize(1);F.Cells[0].RainMPerS=.001;
        double Remaining=.013;bool Good=Ready;int Jobs=0;
        while(Good&&Remaining>0.&&Jobs++<8)
        {
            const auto R=S.Advance(Remaining,F);Good=R.Success&&R.AdvancedSeconds>0.;
            Remaining=R.RemainingSeconds;
        }
        Check("final_decimal_cfl_quantum_integrates_all_time_and_rain_without_tiny_debt",Good&&Remaining==0.
            &&std::abs(S.SimulatedSeconds()-.013)<1e-16
            &&std::abs(S.VolumeBudget().RainM3-.001*100.*.013)<1e-16
            &&std::abs(S.VolumeBudget().ResidualM3)<1e-12);
    }
    {
        // A worker budget can end one sub-resolution interval before its
        // accepted request. That tail must be integrated, never rounded away.
        auto D=RectangularDomain(1,1);Settings Options;
        Options.MaximumStepSeconds=.001;Options.MaximumStepsPerAdvance=1;
        Solver S;const bool Ready=Make(S,D,std::vector<CellState>(1),{},Options);
        Forcing F;F.Cells.resize(1);F.Cells[0].RainMPerS=.001;
        const double Requested=.001+5.e-13;
        const auto Prefix=S.Advance(Requested,F);
        const double RainBeforeTail=S.VolumeBudget().RainM3;
        Check("positive_sub_resolution_tail_is_retained_at_worker_step_budget",Ready&&Prefix.Success&&Prefix.Steps==1
            &&Prefix.RemainingSeconds>0.&&Prefix.RemainingSeconds<=1.e-12);
        const auto Tail=S.Advance(Prefix.RemainingSeconds,F);
        Check("positive_final_sub_resolution_tail_integrates_real_time_and_rain",Ready&&Tail.Success&&Tail.Steps==1
            &&Tail.AdvancedSeconds==Prefix.RemainingSeconds&&Tail.RemainingSeconds==0.
            &&S.VolumeBudget().RainM3>RainBeforeTail&&std::abs(S.SimulatedSeconds()-Requested)<1.e-18
            &&std::abs(S.VolumeBudget().RainM3-.001*100.*Requested)<1.e-16&&std::abs(S.VolumeBudget().ResidualM3)<1.e-16);
        Options.MaximumStepSeconds=1.e-14;Solver Unsafe;
        const bool UnsafeReady=Make(Unsafe,D,std::vector<CellState>(1),{},Options);
        const auto Rejected=Unsafe.Advance(.001,F);
        Check("nonfinal_cfl_underflow_still_rejects_without_committing_time_or_volume",UnsafeReady&&!Rejected.Success
            &&Rejected.AdvancedSeconds==0.&&Rejected.RemainingSeconds==.001&&Unsafe.SimulatedSeconds()==0.
            &&Unsafe.VolumeBudget().RainM3==0.&&Unsafe.VolumeBudget().AcceptedSteps==0);
    }
    const auto Urban=RunUrbanPorosityReferenceChecks();
    C.insert(C.end(),Urban.begin(),Urban.end());
    return C;
}


// Isolated pure C++ candidate checks. They exercise the production Solver
// entry points, not Python-derived answers or a separate mock solver.
std::vector<ReferenceCheck> RunUrbanPorosityReferenceChecks()
{
    std::vector<ReferenceCheck> C;
    const auto Check=[&](const char* Name,bool Passed){C.push_back({Name,Passed});};
    const auto Make=[&](Solver& S,const Domain& D,const std::vector<CellState>& V,
        const std::vector<StorageState>& R=std::vector<StorageState>{},Settings Opt=Settings{})
    {std::string Error;return S.Initialize(D,V,R,Opt,Error);};
    {
        auto Default=RectangularDomain(3,2),Explicit=Default;
        for(auto& Cell:Explicit.Cells)Cell.StoragePorosity=1.0;
        for(auto& Face:Explicit.Faces)Face.OpenFraction=1.0;
        std::vector<CellState> V(6);for(std::size_t I=0;I<V.size();++I)V[I].DepthM=.2+.03*(I%3);
        Solver A,B;const bool OK=Make(A,Default,V)&&Make(B,Explicit,V)&&RunSeconds(A,.4)&&RunSeconds(B,.4);
        Check("porosity_default_one_matches_explicit_legacy_geometry_exactly",OK&&CellGeometry{}.StoragePorosity==1.0
            &&FaceGeometry{}.OpenFraction==1.0&&EqualState(A.Cells(),B.Cells())&&A.CompletedStep()==B.CompletedStep()
            &&A.VolumeBudget().FinalSurfaceM3==B.VolumeBudget().FinalSurfaceM3);
    }
    {
        auto D=RectangularDomain(4,4);std::vector<CellState> V(16);
        for(std::size_t I=0;I<V.size();++I){D.Cells[I].StoragePorosity=.3+.13*(I%5);D.Cells[I].BedM=.1*(I%4);V[I].DepthM=2.-D.Cells[I].BedM;}
        for(auto& F:D.Faces)F.OpenFraction=static_cast<double>(F.StableID%11)/10.;
        Solver S;const bool OK=Make(S,D,V)&&RunSeconds(S,2.);
        double EtaError=0.;if(OK)for(std::size_t I=0;I<V.size();++I)EtaError=std::max(EtaError,std::abs(S.SurfaceHeadM(I)-2.));
        Check("porosity_irregular_bed_and_aperture_lake_at_rest",OK&&EtaError<1e-10&&MaxSpeed(S)<1e-9&&std::abs(S.VolumeBudget().ResidualM3)<1e-9);
    }
    {
        auto D=RectangularDomain(6,4);std::vector<CellState> V(24);
        for(std::size_t I=0;I<V.size();++I){D.Cells[I].StoragePorosity=I%2?.7:1.;V[I].DepthM=I%6<3?1.:0.;}
        for(auto& F:D.Faces)if(F.Right>=0&&F.Right==F.Left+1&&F.Left%6==2)F.OpenFraction=0.;
        Solver S;const bool OK=Make(S,D,V)&&RunSeconds(S,2.);
        double Dry=0.;if(OK)for(std::size_t I=0;I<V.size();++I)if(I%6>=3)Dry=std::max(Dry,S.Cells()[I].DepthM);
        Check("porosity_closed_internal_faces_have_zero_mass_leak",OK&&Dry==0.&&std::abs(S.VolumeBudget().ResidualM3)<1e-9);
    }
    {
        auto D=RectangularDomain(2,1),Narrow=D;D.Cells[0].StoragePorosity=.21;D.Cells[1].StoragePorosity=.91;Narrow=D;
        for(auto& F:Narrow.Faces)if(F.Right>=0)F.OpenFraction=.22;
        std::vector<CellState> V(2);V[0].DepthM=1.;Solver A,B;
        const bool OK=Make(A,D,V)&&Make(B,Narrow,V)&&RunSeconds(A,.01)&&RunSeconds(B,.01);
        Check("porosity_partial_shared_face_uses_one_conservative_flux",OK&&B.Cells()[1].DepthM>0.&&B.Cells()[1].DepthM<A.Cells()[1].DepthM
            &&std::abs(A.VolumeBudget().InitialSurfaceM3-21.)<1e-12&&std::abs(B.VolumeBudget().ResidualM3)<1e-10);
    }
    {
        auto D=RectangularDomain(1,1);D.Cells[0].StoragePorosity=.4224;
        Forcing F;F.Cells.resize(1);F.Cells[0].RainMPerS=.01;Solver S;
        const bool OK=Make(S,D,std::vector<CellState>(1))&&RunSeconds(S,1.,F);
        Check("porosity_roof_rain_uses_whole_gross_catchment_without_deletion",OK&&std::abs(S.VolumeBudget().RainM3-1.)<1e-10
            &&std::abs(S.Cells()[0].DepthM-.01/.4224)<1e-10&&std::abs(S.VolumeBudget().ResidualM3)<1e-10);
    }
    {
        auto D=RectangularDomain(1,1);D.Cells[0].StoragePorosity=.25;std::vector<CellState> V(1);V[0].DepthM=.1;
        Forcing F;F.Cells.resize(1);F.Cells[0].InfiltrationCapacityMPerS=.01;Solver S;
        const bool OK=Make(S,D,V)&&RunSeconds(S,1.,F);
        Check("porosity_infiltration_is_per_open_area_and_absolute_history",OK&&std::abs(S.VolumeBudget().InfiltrationM3-.25)<1e-10
            &&std::abs(S.CumulativeInfiltrationM3()[0]-.25)<1e-10&&std::abs(S.VolumeBudget().ResidualM3)<1e-10);
    }
    {
        auto D=RectangularDomain(1,1);D.Cells[0].StoragePorosity=.25;std::vector<CellState> V(1);V[0].DepthM=.1;
        Forcing F;F.Cells.resize(1);F.Cells[0].ExternalDrainCapacityM3PerS=.01;F.Cells[0].EvaporationMPerS=.001;Solver S;
        const bool OK=Make(S,D,V)&&RunSeconds(S,1.,F);
        Check("porosity_external_drain_keeps_m3s_evaporation_uses_open_area",OK&&std::abs(S.VolumeBudget().ExternalDrainageM3-.01)<1e-10
            &&std::abs(S.VolumeBudget().EvaporationM3-.025)<1e-10&&std::abs(S.VolumeBudget().FinalSurfaceM3-2.465)<1e-10
            &&std::abs(S.VolumeBudget().ResidualM3)<1e-10);
    }
    {
        auto D=RectangularDomain(1,1);D.Cells[0].StoragePorosity=.25;D.Storages.push_back({1,0.,1.,2.});
        StorageExchange E;E.StableID=1;E.Cell=0;E.Storage=0;E.ApertureAreaM2=.05;E.MaximumAbsFlowM3PerS=.2;D.Exchanges.push_back(E);
        std::vector<CellState> V(1);V[0].DepthM=.1;Solver S;const bool OK=Make(S,D,V,{{1.}})&&RunSeconds(S,.2);
        Check("porosity_storage_backflow_transfers_real_volume_into_smaller_area",OK&&S.Cells()[0].DepthM>.1&&S.Storages()[0].VolumeM3<1.
            &&S.VolumeBudget().StorageToSurfaceM3>0.&&std::abs(S.VolumeBudget().ResidualM3)<1e-10);
    }
    {
        auto D=RectangularDomain(2,1);D.Cells[0].StoragePorosity=.05;D.Cells[1].StoragePorosity=.8;
        // Deliberately adversarial under-bed opening: its requested law can
        // withdraw more than the tiny donor inventory. Actual world authoring
        // need not produce this case; the numerical limiter must remain safe.
        for(auto& F:D.Faces)if(F.Right>=0)
        {F.Kind=FaceKind::HydraulicStructure;F.WeirCrestM=10.;F.GateInvertM=-1.;F.GateHeightM=1.;F.GateOpenFraction=1.;}
        std::vector<CellState> V(2);V[0].DepthM=.00001;
        Solver S;const bool Ready=Make(S,D,V);const auto R=S.Advance(.01,{});
        Check("porosity_tiny_open_area_orifice_donor_is_positive_conservative",Ready&&R.Success&&R.Steps==1&&R.RemainingSeconds==0.
            &&FiniteNonnegative(S)&&S.Cells()[0].DepthM<1e-11&&S.Cells()[1].DepthM>0.
            &&std::abs(S.VolumeBudget().InitialSurfaceM3-.00005)<1e-12&&std::abs(S.VolumeBudget().ResidualM3)<1e-12);
    }
    {
        auto Road=RectangularDomain(1,1),Forest=Road;Road.Cells[0].StoragePorosity=Forest.Cells[0].StoragePorosity=.6;
        Road.Cells[0].ManningN=.015;Forest.Cells[0].ManningN=.08;
        std::vector<CellState> V(1);V[0].DepthM=.2;V[0].DischargeXM2PerS=.1;Solver A,B;
        const bool OK=Make(A,Road,V)&&Make(B,Forest,V)&&RunSeconds(A,.001)&&RunSeconds(B,.001);
        Check("porosity_manning_damps_momentum_without_becoming_mass_sink",OK&&std::abs(B.Cells()[0].DischargeXM2PerS)<std::abs(A.Cells()[0].DischargeXM2PerS)
            &&A.Cells()[0].DepthM==V[0].DepthM&&B.Cells()[0].DepthM==V[0].DepthM&&std::abs(B.VolumeBudget().ResidualM3)<1e-12);
    }
    {
        auto Old=RectangularDomain(1,1),New=Old;Old.Cells[0].StoragePorosity=.8;New.Cells[0].StoragePorosity=.4;
        std::vector<CellState> V(1);V[0].DepthM=.15;V[0].DischargeXM2PerS=.02;
        Solver A,B;const bool Ready=Make(A,Old,V);auto Rebound=V;
        const double Ratio=Old.Cells[0].StoragePorosity/New.Cells[0].StoragePorosity;
        Rebound[0].DepthM*=Ratio;Rebound[0].DischargeXM2PerS*=Ratio;
        const bool OK=Ready&&Make(B,New,Rebound);
        Check("porosity_explicit_rebind_preserves_volume_and_integrated_momentum",OK&&std::abs(A.VolumeBudget().FinalSurfaceM3-B.VolumeBudget().FinalSurfaceM3)<1e-12
            &&std::abs(Old.Cells[0].StoragePorosity*V[0].DischargeXM2PerS-New.Cells[0].StoragePorosity*Rebound[0].DischargeXM2PerS)<1e-14);
    }
    {
        auto D=RectangularDomain(1,1);std::vector<CellState> V(1);V[0].DepthM=.2;Solver S;bool OK=Make(S,D,V);std::string Error;
        const auto Before=S.Cells();
        for(double Invalid:{0.,-.1,1.1,std::numeric_limits<double>::quiet_NaN()})
        {auto Bad=D;Bad.Cells[0].StoragePorosity=Invalid;OK=OK&&!S.Initialize(Bad,V,{},Settings{},Error);}
        for(double Invalid:{-.1,1.1,std::numeric_limits<double>::quiet_NaN()})
        {auto Bad=D;Bad.Faces[0].OpenFraction=Invalid;OK=OK&&!S.Initialize(Bad,V,{},Settings{},Error);}
        Settings Tiny;Tiny.MaximumOwnedBytes=1;OK=OK&&!S.Initialize(D,V,{},Tiny,Error);
        Check("porosity_invalid_geometry_and_payload_budget_fail_before_mutation",OK&&EqualState(Before,S.Cells())&&S.SimulatedSeconds()==0.);
    }
    return C;
}

}
