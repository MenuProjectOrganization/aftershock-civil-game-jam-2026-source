#include "AftershockHydrologyAsyncStepperReferenceTests.h"
#include "AftershockHydrologyAsyncStepper.h"
#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace AftershockWater
{
namespace
{
InitializeInput Rect(int W = 2, int H = 2, double Depth = 0.0)
{
    InitializeInput In; auto& D = In.Geometry; D.CellWidthM = D.CellHeightM = 10.0;
    for (int Y = 0; Y < H; ++Y) for (int X = 0; X < W; ++X)
    { D.Cells.push_back({X - 2, Y - 3, 0.0, 0.0}); In.Cells.push_back({Depth, 0.0, 0.0}); }
    std::int64_t ID = 1;
    const auto Add = [&](int L, int R, double NX, double NY, FaceKind Kind)
    { FaceGeometry F; F.Left = L; F.Right = R; F.NormalX = NX; F.NormalY = NY;
      F.LengthM = 10.0; F.Kind = Kind; F.StableID = ID++; D.Faces.push_back(F); };
    for (int Y = 0; Y < H; ++Y) for (int X = 0; X < W; ++X)
    {
        const int I = X + Y * W;
        if (X + 1 < W) Add(I, I + 1, 1, 0, FaceKind::Interior); else Add(I, -1, 1, 0, FaceKind::Wall);
        if (Y + 1 < H) Add(I, I + W, 0, 1, FaceKind::Interior); else Add(I, -1, 0, 1, FaceKind::Wall);
        if (X == 0) Add(I, -1, -1, 0, FaceKind::Wall);
        if (Y == 0) Add(I, -1, 0, -1, FaceKind::Wall);
    }
    In.NumericalSettings.MaximumStepSeconds = 0.001;
    return In;
}
InitializeInput From(const PublishedFrontier& P)
{
    InitializeInput In; In.Geometry = *P.Geometry;
    for (const auto& C : P.Cells)
    { In.Cells.push_back({C.DepthM, C.DischargeXM2PerS, C.DischargeYM2PerS}); In.CumulativeInfiltrationM3.push_back(C.CumulativeInfiltrationM3); }
    In.Storages = P.Storages; In.NumericalSettings.MaximumStepSeconds = 0.001;
    In.Origin.HasLedger = true; In.Origin.SimulatedSeconds = P.Stamp.SimulatedSeconds; In.Origin.Ledger = P.Ledger;
    return In;
}
std::shared_ptr<const ForcingPacket> Rain(AsyncStepper& S, double Rate, std::uint64_t Rev = 1)
{
    std::string Error; Forcing F; F.Cells.resize(4);
    for (auto& C : F.Cells) C.RainMPerS = Rate;
    return AsyncStepper::MakeForcingPacket(std::move(F), S.DesiredGeneration(), Rev, Error);
}
template<class Predicate> bool Pump(AsyncStepper& S, Predicate Done, bool AllowFailure = false)
{
    const double Deadline = FPlatformTime::Seconds() + 5.0;
    while (FPlatformTime::Seconds() < Deadline)
    {
        const auto R = S.Poll();
        if (Done()) return true;
        if (R.Failed && !AllowFailure) return false;
        FPlatformProcess::SleepNoStats(0.0005f);
    }
    return false;
}
bool Init(AsyncStepper& S)
{
    std::string E;
    return S.BeginInitialize(Rect(), 0, E) && Pump(S, [&] { return S.LastPublished() != nullptr; });
}
bool Drain(AsyncStepper& S)
{ return Pump(S, [&] { return S.CaptureSaveFrontier().Unconsumed.empty() && !S.IsWorkerInFlight(); }); }
bool Near(double A, double B) { return std::abs(A - B) < 1e-10 * std::max(1.0, std::max(std::abs(A), std::abs(B))); }
bool SameWorkerQuantumLedger(const Budget& A,const Budget& B)
{
    return Near(A.InitialSurfaceM3,B.InitialSurfaceM3)&&Near(A.InitialStorageM3,B.InitialStorageM3)
        &&Near(A.RainM3,B.RainM3)&&Near(A.ExternalInflowM3,B.ExternalInflowM3)
        &&Near(A.BoundaryInflowM3,B.BoundaryInflowM3)&&Near(A.BoundaryOutflowM3,B.BoundaryOutflowM3)
        &&Near(A.InfiltrationM3,B.InfiltrationM3)&&Near(A.ExternalDrainageM3,B.ExternalDrainageM3)
        &&Near(A.EvaporationM3,B.EvaporationM3)&&Near(A.StorageExternalOutflowM3,B.StorageExternalOutflowM3)
        &&Near(A.FinalSurfaceM3,B.FinalSurfaceM3)&&Near(A.FinalStorageM3,B.FinalStorageM3)
        &&Near(A.NumericalVolumeCorrectionM3,B.NumericalVolumeCorrectionM3)&&Near(A.ResidualM3,B.ResidualM3)
        &&Near(A.SurfaceToStorageM3,B.SurfaceToStorageM3)&&Near(A.StorageToSurfaceM3,B.StorageToSurfaceM3)
        &&A.AcceptedSteps==B.AcceptedSteps&&A.FaceEvaluations==B.FaceEvaluations;
}
bool SameWorkerQuantumCells(const PublishedFrontier& A,const PublishedFrontier& B)
{
    if(A.Cells.size()!=B.Cells.size()||A.Storages.size()!=B.Storages.size())return false;
    for(std::size_t I=0;I<A.Cells.size();++I)
    {
        const auto& L=A.Cells[I];const auto& R=B.Cells[I];
        if(L.X!=R.X||L.Y!=R.Y||!Near(L.BedM,R.BedM)||!Near(L.DepthM,R.DepthM)
            ||!Near(L.DischargeXM2PerS,R.DischargeXM2PerS)||!Near(L.DischargeYM2PerS,R.DischargeYM2PerS)
            ||!Near(L.CumulativeInfiltrationM3,R.CumulativeInfiltrationM3)
            ||L.BoundaryMask!=R.BoundaryMask||L.ConnectedMask!=R.ConnectedMask||L.OpenBoundaryMask!=R.OpenBoundaryMask)return false;
    }
    for(std::size_t I=0;I<A.Storages.size();++I)if(!Near(A.Storages[I].VolumeM3,B.Storages[I].VolumeM3))return false;
    return true;
}
}

std::vector<ReferenceCheck> RunHydrologyAsyncStepperReferenceChecks()
{
    check(IsInGameThread()); std::vector<ReferenceCheck> Out;
    const auto Check = [&](const char* N, bool P) { Out.push_back({N, P}); };
    {
        AsyncStepper S; std::string E; const bool Accepted = S.BeginInitialize(Rect(), 0, E);
        const auto Before = S.CaptureSaveFrontier();
        Check("async.init_moves_plain_input_not_published_until_poll", Accepted && !Before.Published
            && Before.DesiredInitialization && S.IsWorkerInFlight());
        const bool Ready = Pump(S, [&] { return S.LastPublished() != nullptr; });
        auto P = S.LastPublished();
        Check("async.init_publishes_real_finite_frontier_and_separate_budgets", Ready && P && P->Cells.size() == 4
            && P->Stamp.SimulatedSeconds == 0.0 && P->Ledger.AcceptedSteps == 0
            && P->SolverOwnedBytes <= 64ull * 1024ull * 1024ull && P->SnapshotBytes <= 64ull * 1024ull * 1024ull);
        Check("async.global_negative_coords_and_wall_masks_preserved", P && P->Cells[0].X == -2 && P->Cells[0].Y == -3
            && P->Cells[0].BoundaryMask == (1u | 8u) && P->Cells[0].ConnectedMask == 0);
        auto A = Rain(S, 0.001); const bool Queued = S.BeginAdvance(0.13, A, E);
        const auto During = S.CaptureSaveFrontier();
        Check("async.save_counts_full_inflight_request_exactly_once", Queued && During.Published == P
            && Near(During.UnconsumedSeconds, 0.13) && Near(During.InFlightRequestedSeconds, 0.13)
            && During.Unconsumed.size() == 1 && During.Unconsumed[0].Sources == A);
        const bool Prefix = Pump(S, [&] { return S.LastPublished()->Stamp.CompletedStep > 0; });
        const auto Partial = S.CaptureSaveFrontier();
        Check("async.worker_quantum_at_most_eight_and_cfl_remainder_preserved", Prefix
            && Partial.Published->Stamp.CompletedStep <= 8
            && Near(Partial.Published->Stamp.SimulatedSeconds + Partial.UnconsumedSeconds, 0.13));
        const bool Finished = Drain(S); P = S.LastPublished();
        Check("async.physical_clock_and_rain_ledger_match_consumed_time", Finished && P
            && Near(P->Stamp.SimulatedSeconds, 0.13) && Near(P->Ledger.RainM3, 0.001 * 400.0 * 0.13)
            && std::abs(P->Ledger.ResidualM3) < 1e-10);
        const auto Stamp = P->Stamp; for (int I = 0; I < 8; ++I) S.Poll();
        Check("async.repeated_idle_polls_do_not_advance_or_republish", S.LastPublished()->Stamp.Publication == Stamp.Publication
            && S.LastPublished()->Stamp.CompletedStep == Stamp.CompletedStep && S.LastPublished()->Stamp.SimulatedSeconds == Stamp.SimulatedSeconds);
        const auto Generation = S.DesiredGeneration();
        Check("async.stale_topology_seed_rejected_without_mutation", !S.BeginInitialize(Rect(), 0, E)
            && S.DesiredGeneration() == Generation && S.LastPublished()->Stamp.Publication == Stamp.Publication);
    }
    {
        AsyncStepper S; std::string E; const bool Ready = Init(S);
        auto A = Rain(S, 0.001, 1), B = Rain(S, 0.002, 2), C = Rain(S, 0.003, 3);
        const bool AB = Ready && S.BeginAdvance(0.1, A, E) && S.BeginAdvance(0.2, B, E);
        const bool Rejected = !S.BeginAdvance(0.3, C, E);
        const bool Coalesced = S.BeginAdvance(0.05, B, E);
        const auto Save = S.CaptureSaveFrontier();
        Check("async.two_segment_backpressure_never_accepts_or_loses_third_time", AB && Rejected && Coalesced
            && Save.Unconsumed.size() == 2 && Near(Save.UnconsumedSeconds, 0.35));
        Check("async.current_worker_forcing_cannot_be_rebound", !S.RebindPendingForcing(0, C, E));
        const bool Done = Drain(S);
        Check("async.forcing_epochs_remain_in_order_and_keep_own_rainfall", Done && Near(S.LastPublished()->Ledger.RainM3,
            400.0 * (0.1 * 0.001 + 0.25 * 0.002)));
    }
    {
        StepperLimits LiveLimits;LiveLimits.MaximumWorkerSteps=32;
        AsyncStepper Default,Live(LiveLimits);std::string E;
        const bool DefaultAccepted=Default.BeginInitialize(Rect(),0,E);
        const bool LiveAccepted=Live.BeginInitialize(Rect(),0,E);
        const auto DefaultPending=Default.CaptureSaveFrontier(),LivePending=Live.CaptureSaveFrontier();
        Check("async.configured_worker_quantum_is_latched_in_plain_initialization_snapshot",
            DefaultAccepted&&LiveAccepted&&DefaultPending.DesiredInitialization&&LivePending.DesiredInitialization
            &&DefaultPending.DesiredInitialization->NumericalSettings.MaximumStepsPerAdvance==8
            &&LivePending.DesiredInitialization->NumericalSettings.MaximumStepsPerAdvance==32
            &&Near(DefaultPending.DesiredInitialization->NumericalSettings.MaximumStepSeconds,.001)
            &&Near(LivePending.DesiredInitialization->NumericalSettings.MaximumStepSeconds,.001)
            &&DefaultPending.DesiredInitialization->NumericalSettings.CFL==LivePending.DesiredInitialization->NumericalSettings.CFL);
        bool OK=DefaultAccepted&&LiveAccepted
            &&Pump(Default,[&]{return Default.LastPublished()!=nullptr;})
            &&Pump(Live,[&]{return Live.LastPublished()!=nullptr;});
        auto SlowRain=Rain(Default,.001),FastRain=Rain(Live,.001);
        OK=OK&&Default.BeginAdvance(.1,SlowRain,E)&&Live.BeginAdvance(.1,FastRain,E);
        const auto Submitted=Live.CaptureSaveFrontier();
        Check("async.configured_32_step_job_keeps_full_accepted_request_in_save_frontier",OK
            &&Submitted.Published&&Submitted.Published->Stamp.CompletedStep==0
            &&Near(Submitted.UnconsumedSeconds,.1)&&Near(Submitted.InFlightRequestedSeconds,.1)
            &&Submitted.Unconsumed.size()==1&&Submitted.Unconsumed[0].Sources==FastRain);
        const bool Prefix=OK&&Pump(Live,[&]{return Live.LastPublished()&&Live.LastPublished()->Stamp.CompletedStep>0;});
        const auto Partial=Live.CaptureSaveFrontier();
        Check("async.configured_32_step_job_really_advances_more_than_eight_but_at_most_32",Prefix
            &&Partial.Published&&Partial.Published->Stamp.CompletedStep>8&&Partial.Published->Stamp.CompletedStep<=32
            &&Partial.Published->Ledger.AcceptedSteps==Partial.Published->Stamp.CompletedStep
            &&Near(Partial.Published->Stamp.SimulatedSeconds,.001*Partial.Published->Stamp.CompletedStep)
            &&Near(Partial.Published->Stamp.SimulatedSeconds+Partial.UnconsumedSeconds,.1));
        // Restore at this real accepted prefix into the DEFAULT eight-step owner.
        // Rescheduling the remaining immutable forcing must not replay rainfall.
        AsyncStepper Restored;bool RestoredOK=Prefix&&Partial.Published;
        if(RestoredOK)
        {
            RestoredOK=Restored.BeginInitialize(From(*Partial.Published),0,E)
                &&Pump(Restored,[&]{return Restored.LastPublished()!=nullptr;});
            for(const auto& D:Partial.Unconsumed)
            {
                Forcing Copy=D.Sources->Values();
                auto Packet=AsyncStepper::MakeForcingPacket(std::move(Copy),Restored.DesiredGeneration(),D.Sources->Revision(),E);
                RestoredOK=RestoredOK&&Restored.BeginAdvance(D.RemainingSeconds,Packet,E);
            }
            RestoredOK=RestoredOK&&Drain(Restored);
        }
        const bool Finished=OK&&Drain(Default)&&Drain(Live);
        const auto L=Default.LastPublished(),R=Live.LastPublished(),Saved=Restored.LastPublished();
        Check("async.configured_quantum_preserves_real_physical_end_time_and_rain_volume",Finished&&L&&R
            &&Near(L->Stamp.SimulatedSeconds,.1)&&Near(R->Stamp.SimulatedSeconds,.1)
            &&Near(L->Ledger.RainM3,.04)&&Near(R->Ledger.RainM3,.04)
            &&std::abs(L->Ledger.ResidualM3)<1e-10&&std::abs(R->Ledger.ResidualM3)<1e-10
            &&Default.CaptureSaveFrontier().Unconsumed.empty()&&Live.CaptureSaveFrontier().Unconsumed.empty());
        Check("async.configured_quantum_preserves_every_budget_field_and_canonical_cell",Finished&&L&&R
            &&SameWorkerQuantumLedger(L->Ledger,R->Ledger)&&SameWorkerQuantumCells(*L,*R)
            &&L->Stamp.CompletedStep==R->Stamp.CompletedStep);
        Check("async.save_32_step_prefix_resumes_with_eight_step_owner_without_replay",RestoredOK&&Finished&&R&&Saved
            &&Near(R->Stamp.SimulatedSeconds,Saved->Stamp.SimulatedSeconds)
            &&R->Stamp.CompletedStep==Saved->Stamp.CompletedStep
            &&SameWorkerQuantumLedger(R->Ledger,Saved->Ledger)&&SameWorkerQuantumCells(*R,*Saved));
    }
    {
        // A retained positive tail below the public submission resolution is
        // already accepted time, not a new request. A second bounded worker
        // must drain it under the SAME immutable forcing packet.
        StepperLimits Limits;Limits.MaximumWorkerSteps=1;AsyncStepper S(Limits);std::string Error;
        const bool Ready=Init(S);const double Requested=.001+5.e-13;
        const auto Packet=Rain(S,.001,1);
        const bool Submitted=Ready&&S.BeginAdvance(Requested,Packet,Error);
        const bool Prefix=Submitted&&Pump(S,[&]{return S.LastPublished()&&S.LastPublished()->Stamp.CompletedStep>0;});
        const auto Frontier=S.CaptureSaveFrontier();
        Check("async.sub_resolution_accepted_tail_remains_scheduled_with_original_forcing",Prefix&&Frontier.Published
            &&Frontier.Published->Stamp.CompletedStep==1&&Frontier.Unconsumed.size()==1
            &&Frontier.Unconsumed[0].RemainingSeconds>0.&&Frontier.Unconsumed[0].RemainingSeconds<=1.e-12
            &&Frontier.Unconsumed[0].Sources==Packet&&Frontier.WorkerInFlight);
        const bool Done=Submitted&&Drain(S);const auto Final=S.LastPublished();
        Check("async.sub_resolution_tail_drains_without_hung_future_or_fake_completion",Done&&Final
            &&Final->Stamp.CompletedStep==2&&Final->Ledger.AcceptedSteps==2
            &&std::abs(Final->Stamp.SimulatedSeconds-Requested)<1.e-18
            &&S.CaptureSaveFrontier().Unconsumed.empty()&&!S.IsWorkerInFlight());
        Check("async.sub_resolution_tail_conserves_all_rain_and_closed_mass_ledger",Done&&Final
            &&std::abs(Final->Ledger.RainM3-.001*400.*Requested)<1.e-16
            &&Final->Ledger.RainM3>.001*400.*.001&&std::abs(Final->Ledger.ResidualM3)<1.e-16);
    }
    {
        // Construction-time limits are bounded even for malformed callers. The
        // input's smaller numerical budget is preserved; zero remains invalid.
        const std::size_t Limits[]={0,std::numeric_limits<std::size_t>::max(),32,32};
        const std::size_t Requested[]={64,std::numeric_limits<std::size_t>::max(),4,0};
        const std::size_t Expected[]={1,32,4,0};
        const char* Names[]={"async.zero_worker_limit_clamps_to_one_real_step",
            "async.oversized_worker_limit_and_input_clamp_to_32_real_steps",
            "async.smaller_requested_numerical_budget_is_not_enlarged",
            "async.zero_numerical_step_budget_rejected_without_generation_or_job"};
        for(int Variant=0;Variant<4;++Variant)
        {
            StepperLimits Config;Config.MaximumWorkerSteps=Limits[Variant];AsyncStepper S(Config);std::string E;
            auto Input=Rect();Input.NumericalSettings.MaximumStepsPerAdvance=Requested[Variant];
            const bool Accepted=S.BeginInitialize(std::move(Input),0,E);const auto Pending=S.CaptureSaveFrontier();
            if(Expected[Variant]==0)
            {
                Check(Names[Variant],!Accepted&&!Pending.DesiredInitialization&&!Pending.Published
                    &&Pending.DesiredGeneration==0&&!Pending.WorkerInFlight&&Pending.Unconsumed.empty());
                continue;
            }
            bool OK=Accepted&&Pending.DesiredInitialization
                &&Pending.DesiredInitialization->NumericalSettings.MaximumStepsPerAdvance==Expected[Variant]
                &&Near(Pending.DesiredInitialization->NumericalSettings.MaximumStepSeconds,.001)
                &&Pump(S,[&]{return S.LastPublished()!=nullptr;});
            auto Packet=Rain(S,.001);OK=OK&&S.BeginAdvance(.1,Packet,E)
                &&Pump(S,[&]{return S.LastPublished()&&S.LastPublished()->Stamp.CompletedStep>0;});
            const auto Prefix=S.CaptureSaveFrontier();
            Check(Names[Variant],OK&&Prefix.Published&&Prefix.Published->Stamp.CompletedStep==Expected[Variant]
                &&Near(Prefix.Published->Stamp.SimulatedSeconds,.001*Expected[Variant])
                &&Near(Prefix.Published->Stamp.SimulatedSeconds+Prefix.UnconsumedSeconds,.1));
        }
    }
    {
        StepperLimits Limits;Limits.MaximumWorkerSteps=32;AsyncStepper S(Limits);std::string E;
        const bool Ready=Init(S);auto Packet=Rain(S,.001);const auto Original=S.LastPublished();
        const bool Accepted=Ready&&S.BeginAdvance(.1,Packet,E);S.CancelGeneration();
        const bool Joined=Pump(S,[&]{return !S.IsWorkerInFlight();});const auto Canceled=S.CaptureSaveFrontier();
        Check("async.configured_32_step_cancel_discards_unpublished_work_but_not_time",Accepted&&Joined
            &&Canceled.Published==Original&&Near(Canceled.UnconsumedSeconds,.1)&&!Canceled.DesiredInitialization);
        bool Replayed=Original&&S.BeginInitialize(From(*Original),Original->Stamp.Publication,E);
        auto Fresh=Rain(S,.001);Replayed=Replayed&&S.RebindPendingForcing(0,Fresh,E)&&Drain(S);
        const auto Final=S.LastPublished();
        Check("async.configured_32_step_cancel_replay_preserves_actual_clock_and_rain_ledger",Replayed&&Final
            &&Near(Final->Stamp.SimulatedSeconds,.1)&&Near(Final->Ledger.RainM3,.04)
            &&std::abs(Final->Ledger.ResidualM3)<1e-10&&S.CaptureSaveFrontier().Unconsumed.empty());
    }
    {
        AsyncStepper S; std::string E; const bool Ready = Init(S); auto A = Rain(S, 0.001);
        const bool Accepted = Ready && S.BeginAdvance(0.2, A, E); auto P = S.LastPublished();
        bool First = false, Latest = false, Rebound = false;
        if (P)
        {
            auto One = From(*P); One.Geometry.Cells[0].ManningN = 0.04;
            First = S.BeginInitialize(std::move(One), P->Stamp.Publication, E);
            auto Two = From(*P); Two.Geometry.Cells[0].ManningN = 0.05;
            Latest = S.BeginInitialize(std::move(Two), P->Stamp.Publication, E);
            auto New = Rain(S, 0.001); Rebound = S.RebindPendingForcing(0, New, E);
        }
        const auto NewGeneration = S.DesiredGeneration(); const auto Saved = S.CaptureSaveFrontier();
        Check("async.coalesced_topology_keeps_old_frontier_and_full_unpublished_time", Accepted && First && Latest && Rebound
            && Saved.Published == P && Saved.DesiredInitialization
            && Near(Saved.DesiredInitialization->Geometry->Cells[0].ManningN, 0.05) && Near(Saved.UnconsumedSeconds, 0.2));
        const bool Finished = Drain(S); auto Final = S.LastPublished();
        Check("async.stale_worker_never_double_applies_time_or_rain", Finished && Final
            && Final->Stamp.Generation == NewGeneration && Near(Final->Stamp.SimulatedSeconds, 0.2)
            && Near(Final->Ledger.RainM3, 0.08) && Near(Final->Geometry->Cells[0].ManningN, 0.05));
    }
    {
        AsyncStepper S; std::string E; const bool Ready = Init(S); auto A = Rain(S, 0.001);
        const bool Accepted = Ready && S.BeginAdvance(0.02, A, E); const auto P = S.LastPublished();
        S.CancelGeneration();
        const bool Joined = Pump(S, [&] { return !S.IsWorkerInFlight(); });
        const auto Save = S.CaptureSaveFrontier();
        Check("async.generation_cancel_discards_worker_not_accepted_time", Accepted && Joined && Save.Published == P
            && Near(Save.UnconsumedSeconds, 0.02) && !Save.DesiredInitialization);
        bool Restored = false;
        if (P)
        { Restored = S.BeginInitialize(From(*P), P->Stamp.Publication, E);
          auto New = Rain(S, 0.001); Restored = Restored && S.RebindPendingForcing(0, New, E) && Drain(S); }
        Check("async.cancelled_generation_can_reinitialize_and_replay_once", Restored
            && Near(S.LastPublished()->Stamp.SimulatedSeconds, 0.02) && Near(S.LastPublished()->Ledger.RainM3, 0.008));
    }
    {
        AsyncStepper S; std::string E; const bool Ready = Init(S); auto Bad = Rain(S, 1e300);
        const auto P = S.LastPublished(); const bool Accepted = Ready && S.BeginAdvance(0.02, Bad, E);
        const bool Failed = Pump(S, [&] { return S.CaptureSaveFrontier().Faulted; }, true);
        const auto Save = S.CaptureSaveFrontier();
        Check("async.failed_source_step_keeps_last_frontier_and_all_unconsumed_time", Accepted && Failed
            && Save.Published == P && Near(Save.UnconsumedSeconds, 0.02));
        auto Corrected = Rain(S, 0.001, 2);
        const bool Recovered = S.RebindPendingForcing(0, Corrected, E) && S.RetryPending(E) && Drain(S);
        Check("async.explicit_retry_replaces_only_unconsumed_forcing", Recovered
            && Near(S.LastPublished()->Stamp.SimulatedSeconds, 0.02) && Near(S.LastPublished()->Ledger.RainM3, 0.008));
    }
    {
        AsyncStepper Original; std::string E; bool OK = Init(Original);
        auto A = Rain(Original, 0.001); OK = OK && Original.BeginAdvance(0.013, A, E) && Drain(Original);
        OK = OK && Original.BeginAdvance(0.05, A, E); const auto Save = Original.CaptureSaveFrontier();
        AsyncStepper Restored;
        if (Save.Published)
        {
            OK = OK && Restored.BeginInitialize(From(*Save.Published), 0, E)
                && Pump(Restored, [&] { return Restored.LastPublished() != nullptr; });
            for (const auto& D : Save.Unconsumed)
            {
                Forcing Copy = D.Sources->Values();
                auto Packet = AsyncStepper::MakeForcingPacket(std::move(Copy), Restored.DesiredGeneration(), D.Sources->Revision(), E);
                OK = OK && Restored.BeginAdvance(D.RemainingSeconds, Packet, E);
            }
            OK = OK && Drain(Original) && Drain(Restored);
        }
        else OK = false;
        auto L = Original.LastPublished(), R = Restored.LastPublished();
        bool Identical = OK && L && R && L->Cells.size() == R->Cells.size();
        if (Identical) for (std::size_t I = 0; I < L->Cells.size(); ++I)
            Identical = Identical && Near(L->Cells[I].DepthM, R->Cells[I].DepthM)
                && Near(L->Cells[I].DischargeXM2PerS, R->Cells[I].DischargeXM2PerS)
                && Near(L->Cells[I].DischargeYM2PerS, R->Cells[I].DischargeYM2PerS);
        Check("async.save_during_job_restores_published_clock_ledger_plus_full_pending_request", Identical
            && Near(L->Stamp.SimulatedSeconds, R->Stamp.SimulatedSeconds)
            && Near(L->Ledger.RainM3, R->Ledger.RainM3) && Near(L->Ledger.FinalSurfaceM3, R->Ledger.FinalSurfaceM3));
    }
    for (int Variant = 0; Variant < 4; ++Variant)
    {
        AsyncStepper S; std::string E; auto In = Rect(2, 1, Variant == 1 ? 1.0 : 0.25);
        for (auto& F : In.Geometry.Faces) if (F.Right >= 0)
        {
            if (Variant < 2) { F.HasSill = true; F.SillM = 0.5; }
            else
            { F.Kind = FaceKind::HydraulicStructure; F.WeirCrestM = 1.0;
              F.GateInvertM = 0.1; F.GateHeightM = 0.2; F.GateOpenFraction = Variant == 3 ? 1.0 : 0.0; }
        }
        const bool Ready = S.BeginInitialize(std::move(In), 0, E)
            && Pump(S, [&] { return S.LastPublished() != nullptr; });
        const bool ShouldOpen = Variant == 1 || Variant == 3;
        const auto P = S.LastPublished();
        Check(Variant == 0 ? "async.mask_raised_sill_blocks_shallow_water"
            : Variant == 1 ? "async.mask_overtopped_sill_connects_east_west"
            : Variant == 2 ? "async.mask_closed_dam_blocks_below_crest"
            : "async.mask_open_gate_connects_below_weir_crest",
            Ready && P && ((P->Cells[0].ConnectedMask & 2u) != 0) == ShouldOpen
            && ((P->Cells[1].ConnectedMask & 8u) != 0) == ShouldOpen);
    }
    {
        AsyncStepper S; std::string E; auto In = Rect(1, 1);
        for (auto& F : In.Geometry.Faces) if (F.NormalY < 0.0)
        { F.Kind = FaceKind::StageBoundary; F.StageHeadM = 2.0; F.HasSill = true; F.SillM = 1.0; }
        const bool Ready = S.BeginInitialize(std::move(In), 0, E)
            && Pump(S, [&] { return S.LastPublished() != nullptr; });
        auto P = S.LastPublished();
        Check("async.mask_stage_boundary_uses_actual_crest_and_north_bit", Ready && P
            && P->Cells[0].BoundaryMask == 15u && P->Cells[0].OpenBoundaryMask == 1u && P->Cells[0].ConnectedMask == 0);
    }
    {
        StepperLimits Tiny; Tiny.MaximumPendingBytes = 1; AsyncStepper S(Tiny); std::string E;
        Check("async.pending_budget_rejection_has_no_job_or_generation_change", !S.BeginInitialize(Rect(), 0, E)
            && !S.IsWorkerInFlight() && S.DesiredGeneration() == 0 && !S.LastPublished());
    }
    {
        AsyncStepper S; std::string E; const bool Ready = Init(S);
        Forcing Bad; Bad.Cells.resize(1); Bad.Cells[0].RainMPerS = -1;
        auto Negative = AsyncStepper::MakeForcingPacket(std::move(Bad), S.DesiredGeneration(), 1, E);
        Forcing Wrong; Wrong.Cells.resize(3); auto Shape = AsyncStepper::MakeForcingPacket(std::move(Wrong), S.DesiredGeneration(), 1, E);
        Check("async.invalid_sources_and_wrong_cardinality_rejected_without_accepting_time", Ready && !Negative && Shape
            && !S.BeginAdvance(0.01, Shape, E) && S.CaptureSaveFrontier().Unconsumed.empty());
        S.Deinitialize();
        Check("async.explicit_deinit_joins_and_releases_all_owned_frontiers", !S.IsWorkerInFlight()
            && !S.LastPublished() && S.CaptureSaveFrontier().Unconsumed.empty());
    }
    {
        AsyncStepper Original; std::string E;
        bool OK=Original.BeginInitialize(Rect(2,2,.1),0,E)
            &&Pump(Original,[&]{return Original.LastPublished()!=nullptr;});
        Forcing Values;Values.Cells.resize(4);
        for(auto& C:Values.Cells)C.InfiltrationCapacityMPerS=.001;
        auto A=AsyncStepper::MakeForcingPacket(std::move(Values),Original.DesiredGeneration(),100,E);
        OK=OK&&Original.BeginAdvance(.02,A,E)&&Drain(Original);
        auto First=Original.LastPublished();double Sum=0.;bool PerCell=First&&First->Cells.size()==4;
        if(First)for(const auto& C:First->Cells){Sum+=C.CumulativeInfiltrationM3;PerCell=PerCell&&Near(C.CumulativeInfiltrationM3,.002);}
        Check("async.published_infiltration_is_per_cell_and_matches_cumulative_ledger",OK&&PerCell&&Near(Sum,.008)&&Near(Sum,First->Ledger.InfiltrationM3));
        OK=OK&&Original.BeginAdvance(.03,A,E);const auto Save=Original.CaptureSaveFrontier();
        AsyncStepper Restored;
        if(Save.Published)
        {
            OK=OK&&Restored.BeginInitialize(From(*Save.Published),0,E)
                &&Pump(Restored,[&]{return Restored.LastPublished()!=nullptr;});
            for(const auto& D:Save.Unconsumed)
            {
                Forcing Copy=D.Sources->Values();auto Packet=AsyncStepper::MakeForcingPacket(std::move(Copy),Restored.DesiredGeneration(),D.Sources->Revision(),E);
                OK=OK&&Restored.BeginAdvance(D.RemainingSeconds,Packet,E);
            }
            OK=OK&&Drain(Original)&&Drain(Restored);
        }
        else OK=false;
        auto L=Original.LastPublished(),R=Restored.LastPublished();bool Same=OK&&L&&R&&L->Cells.size()==R->Cells.size();
        if(Same)for(std::size_t I=0;I<L->Cells.size();++I)Same=Same&&Near(L->Cells[I].CumulativeInfiltrationM3,R->Cells[I].CumulativeInfiltrationM3)
            &&Near(R->Cells[I].CumulativeInfiltrationM3,.005);
        Check("async.restore_carries_infiltration_history_and_only_adds_new_delta",Same&&Near(R->Ledger.InfiltrationM3,.02)
            &&Near(R->Stamp.SimulatedSeconds,.05)&&std::abs(R->Ledger.ResidualM3)<1e-10);
        AsyncStepper Rejected;bool Failed=false;
        if(L)
        {
            auto Bad=From(*L);Bad.CumulativeInfiltrationM3[0]+=.5;
            Failed=Rejected.BeginInitialize(std::move(Bad),0,E)
                &&Pump(Rejected,[&]{return Rejected.CaptureSaveFrontier().Faulted;},true);
        }
        Check("async.infiltration_origin_must_match_saved_ledger_before_publication",Failed&&!Rejected.LastPublished());
    }
    {
        AsyncStepper S;std::string E;
        bool OK=S.BeginInitialize(Rect(2,2,.1),0,E)&&Pump(S,[&]{return S.LastPublished()!=nullptr;});
        Forcing Inputs;Inputs.Cells.resize(4);for(auto& C:Inputs.Cells)C.InfiltrationCapacityMPerS=.001;
        auto Packet=AsyncStepper::MakeForcingPacket(std::move(Inputs),S.DesiredGeneration(),7,E);
        OK=OK&&S.BeginAdvance(.02,Packet,E)&&Drain(S);
        const auto Before=S.LastPublished();
        const auto AddSource=[](const PublishedFrontier& Old,double Delta)
        {
            auto In=From(Old);In.ExternalSurfaceSourceM3=Delta;
            In.Cells[0].DepthM+=Delta/(In.Geometry.CellWidthM*In.Geometry.CellHeightM);
            In.Origin.Ledger.ExternalInflowM3+=Delta;In.Origin.Ledger.FinalSurfaceM3+=Delta;
            return In;
        };
        if(Before)OK=OK&&S.BeginInitialize(AddSource(*Before,2.5),Before->Stamp.Publication,E)
            &&Pump(S,[&]{return S.LastPublished()->Stamp.Publication!=Before->Stamp.Publication;});
        else OK=false;
        const auto After=S.LastPublished();bool History=OK&&After&&Before&&After->Cells.size()==Before->Cells.size();
        if(History)for(std::size_t I=0;I<After->Cells.size();++I)
            History=History&&After->Cells[I].CumulativeInfiltrationM3==Before->Cells[I].CumulativeInfiltrationM3;
        Check("async.declared_external_source_adds_only_real_volume_and_keeps_clock_history",History
            &&Near(After->Ledger.FinalSurfaceM3,Before->Ledger.FinalSurfaceM3+2.5)
            &&Near(After->Ledger.ExternalInflowM3,Before->Ledger.ExternalInflowM3+2.5)
            &&After->Stamp.SimulatedSeconds==Before->Stamp.SimulatedSeconds&&After->Stamp.CompletedStep==Before->Stamp.CompletedStep
            &&std::abs(After->Ledger.ResidualM3)<1e-10);
        bool Negative=false,OtherLedger=false,Unlabelled=false,Busy=false;
        if(After)
        {
            auto Bad=AddSource(*After,-1.);Negative=!S.BeginInitialize(std::move(Bad),After->Stamp.Publication,E);
            Bad=AddSource(*After,1.);Bad.Origin.Ledger.RainM3+=1.;OtherLedger=!S.BeginInitialize(std::move(Bad),After->Stamp.Publication,E);
            Bad=AddSource(*After,1.);Bad.ExternalSurfaceSourceM3=0.;
            Unlabelled=S.BeginInitialize(std::move(Bad),After->Stamp.Publication,E)
                &&Pump(S,[&]{return S.CaptureSaveFrontier().Faulted;},true)&&S.LastPublished()==After;
        }
        AsyncStepper Guard;bool Ready=Init(Guard);auto Active=Rain(Guard,.001);
        Ready=Ready&&Guard.BeginAdvance(.1,Active,E);const auto GuardBefore=Guard.LastPublished();
        if(GuardBefore)Busy=Ready&&!Guard.BeginInitialize(AddSource(*GuardBefore,1.),GuardBefore->Stamp.Publication,E)
            &&Near(Guard.CaptureSaveFrontier().UnconsumedSeconds,.1);
        Check("async.external_source_rejects_negative_unlabelled_changed_ledger_and_busy_mutations",Negative&&OtherLedger&&Unlabelled&&Busy);
    }
    return Out;
}
}
