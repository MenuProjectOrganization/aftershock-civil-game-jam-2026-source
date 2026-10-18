#include "AftershockPhysicalLifecycle.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include <limits>

#if WITH_EDITOR
bool RunAftershockPhysicalLifecycleFixtures(const FString& AbsoluteReportPath)
{
    using namespace AftershockPhysicalLifecycle;
    TArray<TSharedPtr<FJsonValue>> Rows;int32 Failed=0;
    const auto Check=[&](const TCHAR* Name,bool Passed)
    {
        auto R=MakeShared<FJsonObject>();R->SetStringField(TEXT("name"),Name);R->SetBoolField(TEXT("passed"),Passed);
        Rows.Add(MakeShared<FJsonValueObject>(R));Failed+=Passed?0:1;
        if(Passed){UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_LIFECYCLE_PASS %s"),Name);}
        else{UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_PHYSICAL_LIFECYCLE_FAIL %s"),Name);}
    };
    const auto Near=[](double A,double B){return FMath::IsFinite(A)&&FMath::IsFinite(B)&&FMath::Abs(A-B)<=1.e-11*FMath::Max3(1.,FMath::Abs(A),FMath::Abs(B));};
    Check(TEXT("visual_remaining_at_start_keeps_original_duration"),RemainingVisualSeconds(100.f,100.f,180.f)==180.f);
    Check(TEXT("visual_remaining_partial_uses_only_unelapsed_timeline"),RemainingVisualSeconds(145.f,100.f,180.f)==135.f);
    Check(TEXT("settled_flood_visual_expiry_does_not_restart_180_seconds"),RemainingVisualSeconds(286.f,100.f,180.f)==0.f&&RemainingVisualSeconds(280.f,100.f,180.f)==0.f);
    Check(TEXT("visual_clock_before_start_cannot_extend_past_original_duration"),RemainingVisualSeconds(90.f,100.f,180.f)==180.f);
    const float InvalidVisual=std::numeric_limits<float>::quiet_NaN(),InfiniteVisual=std::numeric_limits<float>::infinity();
    Check(TEXT("nonfinite_visual_timeline_does_not_create_a_permanent_countdown"),RemainingVisualSeconds(InvalidVisual,100.f,180.f)==0.f
        &&RemainingVisualSeconds(100.f,InvalidVisual,180.f)==0.f&&RemainingVisualSeconds(100.f,100.f,InfiniteVisual)==0.f);
    Check(TEXT("zero_or_negative_visual_duration_never_delays_completed_event"),RemainingVisualSeconds(100.f,100.f,0.f)==0.f&&RemainingVisualSeconds(100.f,100.f,-1.f)==0.f);
    double Last=0.,Debt=0.,Delta=0.;FString Error;
    Check(TEXT("startup_before_first_water_publication_retains_elapsed_time"),
        AccrueCallerTime(.08,0.,0.,false,1.,Last,Debt,Delta,Error)&&Near(Debt,.08)&&Near(Delta,.08));
    bool Initializing=true;for(int32 I=2;I<=4;++I)Initializing=AccrueCallerTime(.08*I,0.,0.,false,1.,Last,Debt,Delta,Error)&&Initializing;
    Check(TEXT("repeated_initialization_early_return_time_not_dropped"),Initializing&&Near(Debt,.32));
    const double AfterInit=Debt;
    Check(TEXT("same_callback_time_not_accrued_twice"),AccrueCallerTime(.32,0.,0.,false,1.,Last,Debt,Delta,Error)&&Debt==AfterInit&&Delta==0.);
    Last=0.;Debt=0.;
    Check(TEXT("topology_return_preserves_scaled_flood_delta_before_work"),
        AccrueCallerTime(.08,100.,4.,false,20.,Last,Debt,Delta,Error)&&Near(Debt,1.6));
    Last=10.;Debt=1.25;const double AcceptedLatch=4.;
    Check(TEXT("restored_latch_not_double_added_to_caller_debt"),
        AccrueCallerTime(10.08,100.,AcceptedLatch,false,2.,Last,Debt,Delta,Error)&&Near(Debt,1.41)&&AcceptedLatch==4.);
    Last=0.;Debt=3.;bool Paused=true;
    for(int32 I=1;I<=3;++I)Paused=AccrueCallerTime(I*.08,100.,2.,true,1.,Last,Debt,Delta,Error)&&Paused;
    Check(TEXT("granular_boundary_worker_and_replay_pause_water_only"),Paused&&Debt==3.&&Near(Last,.24)&&Near(Delta,.08));
    const double BeforeComplete=Debt;
    Check(TEXT("mass_completion_interval_not_retrocharged_to_water"),
        AccrueCallerTime(.32,100.,0.,true,1.,Last,Debt,Delta,Error)&&Debt==BeforeComplete&&Near(Delta,.08));
    Check(TEXT("ordinary_water_resumes_once_on_following_callback"),
        AccrueCallerTime(.40,100.,0.,false,1.,Last,Debt,Delta,Error)&&Near(Debt,3.08));
    Last=0.;Debt=0.;
    const bool QuietFlushed=AccrueCallerTime(.03,100.,0.,false,1.,Last,Debt,Delta,Error);
    const double NewEventStart=100.+Debt;
    Check(TEXT("new_disaster_preserves_prior_quiet_prefix_before_rate_change"),QuietFlushed&&Near(NewEventStart,100.03)
        &&AccrueCallerTime(.08,100.,0.,false,20.,Last,Debt,Delta,Error)&&Near(Debt,1.03));
    const auto Reject=[&](const TCHAR* Name,double Now,double Published,double Accepted,bool Pause,double Rate,double LastInput,double DebtInput)
    {
        double L=LastInput,D=DebtInput,Out=123.;FString Why;
        const bool Applied=AccrueCallerTime(Now,Published,Accepted,Pause,Rate,L,D,Out,Why);
        Check(Name,!Applied&&FMemory::Memcmp(&L,&LastInput,sizeof(double))==0
            &&FMemory::Memcmp(&D,&DebtInput,sizeof(double))==0&&Out==0.&&!Why.IsEmpty());
    };
    const double NaN=std::numeric_limits<double>::quiet_NaN(),Max=std::numeric_limits<double>::max();
    Reject(TEXT("nonfinite_world_time_rejects_without_mutation"),NaN,0.,0.,false,1.,10.,1.);
    Reject(TEXT("reversed_world_clock_rejects_without_silent_clamp"),9.,0.,0.,false,1.,10.,1.);
    Reject(TEXT("nonfinite_existing_debt_rejects_without_normalizing"),11.,0.,0.,false,1.,10.,NaN);
    Reject(TEXT("negative_accepted_debt_rejected"),11.,0.,-1.,false,1.,10.,1.);
    Reject(TEXT("zero_scale_rejected_before_initialization"),.08,0.,0.,false,0.,0.,0.);
    Reject(TEXT("nonfinite_scale_rejected_before_initialization"),.08,0.,0.,false,NaN,0.,0.);
    Reject(TEXT("elapsed_times_scale_overflow_never_poison_debt"),2.,0.,0.,false,Max,0.,0.);
    Reject(TEXT("published_plus_accepted_overflow_rejected"),.08,Max,Max,false,1.,0.,0.);
    Reject(TEXT("total_pending_plus_published_overflow_rejected"),.08,Max,0.,false,1.,0.,Max);
    Reject(TEXT("unrepresentable_positive_debt_increment_rejected"),.08,0.,0.,false,1.,0.,1.e100);
    Reject(TEXT("underflowed_positive_interval_never_deleted"),1.e-100,0.,0.,false,1.e-300,0.,0.);
    Last=4.;Debt=1.e100;
    Check(TEXT("paused_world_zero_delta_valid_without_fake_advance"),
        AccrueCallerTime(4.,0.,0.,false,1.,Last,Debt,Delta,Error)&&Debt==1.e100&&Delta==0.);
    Last=0.;Debt=1.;double Published=100.,Accepted=4.;
    const bool Accrued=AccrueCallerTime(.1,Published,Accepted,false,20.,Last,Debt,Delta,Error);
    const double BeforePoll=Published+Accepted+Debt;
    Published+=3.;Accepted-=3.;const double AfterPoll=Published+Accepted+Debt;
    Debt-=1.;Accepted+=1.;
    Check(TEXT("caller_accepted_publication_repartition_preserves_total_frontier"),Accrued&&Near(BeforePoll,107.)&&Near(AfterPoll,BeforePoll)&&Near(Published+Accepted+Debt,BeforePoll));

    Check(TEXT("avalanche_acceptance_to_commit_locks_geometry"),LocksGranularBuildingInputs(true,EDisasterType::Avalanche));
    Check(TEXT("landslide_acceptance_to_commit_locks_geometry"),LocksGranularBuildingInputs(true,EDisasterType::Landslide));
    Check(TEXT("inactive_granular_episode_does_not_keep_lock"),!LocksGranularBuildingInputs(false,EDisasterType::Avalanche)&&!LocksGranularBuildingInputs(false,EDisasterType::Landslide));
    Check(TEXT("flood_tsunami_policy_not_broadened_by_granular_fix"),!LocksGranularBuildingInputs(true,EDisasterType::Flood)&&!LocksGranularBuildingInputs(true,EDisasterType::Tsunami));
    FBuildingInstance Before;Before.InstanceID=12;Before.GridCoords=FIntPoint(3,4);Before.ISMInstanceIndex=2;Before.CurrentDamage=10.f;Before.DamageState=EDamageState::Cracked;
    Check(TEXT("unchanged_building_inputs_allowed"),PreservesFrozenBuildingInputs(Before,Before));
    auto Changed=Before;Changed.CurrentDamage=30.f;Changed.DamageState=EDamageState::Damaged;
    Check(TEXT("progressive_physical_damage_remains_allowed"),PreservesFrozenBuildingInputs(Before,Changed));
    Changed=Before;Changed.CurrentDamage=0.f;Changed.DamageState=EDamageState::Intact;
    Check(TEXT("direct_record_repair_cannot_bypass_frozen_inputs"),!PreservesFrozenBuildingInputs(Before,Changed));
    Changed=Before;++Changed.CurrentFloors;
    Check(TEXT("floor_count_change_rejected_while_frozen"),!PreservesFrozenBuildingInputs(Before,Changed));
    Changed=Before;++Changed.GridCoords.X;
    Check(TEXT("parcel_move_rejected_while_frozen"),!PreservesFrozenBuildingInputs(Before,Changed));
    Changed=Before;Changed.Foundation=EFoundationQuality::Strong;
    Check(TEXT("foundation_change_rejected_while_frozen"),!PreservesFrozenBuildingInputs(Before,Changed));
    Changed=Before;Changed.bIsRetrofitted=true;
    Check(TEXT("retrofit_change_rejected_while_frozen"),!PreservesFrozenBuildingInputs(Before,Changed));
    Changed=Before;++Changed.ISMInstanceIndex;
    Check(TEXT("canonical_instance_slot_change_rejected_while_frozen"),!PreservesFrozenBuildingInputs(Before,Changed));
    Changed=Before;Changed.RoofQuality=.8f;
    Check(TEXT("engineering_quality_change_rejected_while_frozen"),!PreservesFrozenBuildingInputs(Before,Changed));
    Changed=Before;Changed.DamageState=static_cast<EDamageState>(255);
    Check(TEXT("unknown_progressive_damage_state_not_accepted"),!PreservesFrozenBuildingInputs(Before,Changed));

    ESourceControlPhase Phase=ESourceControlPhase::Ended;double Gap=0.,Elapsed=0.,Tolerance=0.;
    Check(TEXT("meaningful_quiet_prefix_splits_exactly_before_start"),
        ClassifySourceControls(5.,5.25,10.,Phase,Gap,Elapsed,Tolerance,Error)
        &&Phase==ESourceControlPhase::BeforeStart&&Near(Gap,.25)&&FMath::Min(4.,Gap)==.25);
    Check(TEXT("exact_start_activates_new_source_controls"),
        ClassifySourceControls(100.,100.,10.,Phase,Gap,Elapsed,Tolerance,Error)
        &&Phase==ESourceControlPhase::Active&&Gap==10.&&Elapsed==0.);
    Check(TEXT("sub_ps_quiet_boundary_is_reached_without_tiny_request"),
        ClassifySourceControls(100.,100.+8.e-13,10.,Phase,Gap,Elapsed,Tolerance,Error)
        &&Phase==ESourceControlPhase::Active&&Gap>9.&&Tolerance>=1.e-12);
    Check(TEXT("meaningful_start_gap_is_not_bypassed_by_tolerance"),
        ClassifySourceControls(100.,100.+1.e-7,10.,Phase,Gap,Elapsed,Tolerance,Error)
        &&Phase==ESourceControlPhase::BeforeStart&&Gap>Tolerance&&Gap<2.e-7);
    Check(TEXT("mid_source_uses_meaningful_remaining_program_time"),
        ClassifySourceControls(105.,100.,10.,Phase,Gap,Elapsed,Tolerance,Error)
        &&Phase==ESourceControlPhase::Active&&Gap==5.&&Elapsed==5.);
    const double NearEndClock=110.-8.e-13;const double PendingBefore=4.;
    const bool EndReached=ClassifySourceControls(NearEndClock,100.,10.,Phase,Gap,Elapsed,Tolerance,Error);
    const bool NewSourceActive=Phase==ESourceControlPhase::Active;
    const double NextRequest=NewSourceActive?FMath::Min(PendingBefore,Gap):PendingBefore;
    Check(TEXT("sub_ps_end_residue_selects_full_caller_quantum_without_deleting_time"),
        EndReached&&Phase==ESourceControlPhase::Ended&&Gap==0.&&NextRequest==4.
        &&NearEndClock<110.&&PendingBefore==4.);
    Check(TEXT("near_end_rain_and_wave_share_inactive_new_control_phase"),EndReached&&!NewSourceActive);
    Check(TEXT("meaningful_end_gap_still_splits_before_quiet_controls"),
        ClassifySourceControls(110.-1.e-7,100.,10.,Phase,Gap,Elapsed,Tolerance,Error)
        &&Phase==ESourceControlPhase::Active&&Gap>Tolerance&&Gap<2.e-7&&FMath::Min(4.,Gap)>1.e-12);
    const double LargeStart=1.e8,LargeEnd=LargeStart+10.;
    const double LargeTolerance=SourcePhaseTolerance(LargeEnd,LargeEnd);
    Check(TEXT("relative_ulp_scale_handles_large_clock_source_end"),
        LargeTolerance>1.e-12&&ClassifySourceControls(LargeEnd-.5*LargeTolerance,LargeStart,10.,Phase,Gap,Elapsed,Tolerance,Error)
        &&Phase==ESourceControlPhase::Ended);
    Check(TEXT("gap_beyond_relative_ulp_budget_is_still_meaningful"),
        ClassifySourceControls(LargeEnd-4.*LargeTolerance,LargeStart,10.,Phase,Gap,Elapsed,Tolerance,Error)
        &&Phase==ESourceControlPhase::Active&&Gap>Tolerance);
    Check(TEXT("zero_duration_source_has_no_rain_or_wave_division"),
        ClassifySourceControls(10.,10.,0.,Phase,Gap,Elapsed,Tolerance,Error)&&Phase==ESourceControlPhase::Ended);
    Check(TEXT("source_boundary_overflow_rejected_before_packet"),
        !ClassifySourceControls(Max,Max,Max,Phase,Gap,Elapsed,Tolerance,Error));
    Check(TEXT("positive_source_duration_unrepresentable_at_epoch_rejected"),
        !ClassifySourceControls(1.e20,1.e20,.1,Phase,Gap,Elapsed,Tolerance,Error));
    auto Report=MakeShared<FJsonObject>();Report->SetBoolField(TEXT("valid"),Failed==0);
    Report->SetNumberField(TEXT("passed"),Rows.Num()-Failed);Report->SetNumberField(TEXT("failed"),Failed);
    Report->SetStringField(TEXT("scope"),TEXT("Actual shared owner clock accrual, remaining visual timeline, source-control ULP/minimum-quantum boundary comparison and frozen-building policy functions on deterministic inputs. No world timing, actual Build/Remove integration, SaveWorld, solver/replay, GPU, RHI or packaged proof."));
    Report->SetArrayField(TEXT("checks"),Rows);FString Text;
    if(!FJsonSerializer::Serialize(Report,TJsonWriterFactory<>::Create(&Text)))return false;
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsoluteReportPath),true);
    return FFileHelper::SaveStringToFile(Text,*AbsoluteReportPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)&&Failed==0;
}
#endif
