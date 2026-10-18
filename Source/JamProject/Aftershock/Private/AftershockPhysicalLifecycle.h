#pragma once
#include "CoreMinimal.h"
#include "Aftershock/Public/BuildingData.h"
#include <limits>

/** GT owner policy/math only. No UObject, source sampling, worker submission,
 * save-version change or second physical clock authority. */
namespace AftershockPhysicalLifecycle
{
enum class ESourceControlPhase : uint8 { BeforeStart,Active,Ended };

/** Remaining presentation on its original world-time timeline. Completion of
 * asynchronous physics must not restart an already elapsed visual duration.
 * Invalid display inputs cannot create a nonfinite or permanent UI countdown;
 * this never changes the canonical physical clock or settles an event. */
inline float RemainingVisualSeconds(float Now,float StartedAt,float Duration)
{
    if(!FMath::IsFinite(Now)||!FMath::IsFinite(StartedAt)||!FMath::IsFinite(Duration))return 0.f;
    return FMath::Max(0.f,Duration-FMath::Max(0.f,Now-StartedAt));
}

/** Numerical source-control comparison only. The absolute floor is the
 * EXISTING async/solver minimum schedulable quantum, not a new physics epsilon.
 * Neither physical time, pending time nor immutable accepted controls change. */
inline double SourcePhaseTolerance(double Clock,double Boundary)
{
    return FMath::Max(1.e-12,16.*std::numeric_limits<double>::epsilon()*
        FMath::Max3(1.,FMath::Abs(Clock),FMath::Abs(Boundary)));
}
inline bool MeaningfulSourceGap(double Clock,double Boundary)
{
    return Boundary>Clock&&Boundary-Clock>SourcePhaseTolerance(Clock,Boundary);
}
inline bool ClassifySourceControls(double Clock,double Start,double Duration,
    ESourceControlPhase& OutPhase,double& OutBoundaryGap,double& OutElapsed,double& OutTolerance,FString& Failure)
{
    Failure.Reset();const double End=Start+Duration;
    if(!FMath::IsFinite(Clock)||Clock<0.||!FMath::IsFinite(Start)||Start<0.
        ||!FMath::IsFinite(Duration)||Duration<0.||!FMath::IsFinite(End)
        ||(Duration>0.&&End<=Start))
    {Failure=TEXT("Physical source-control boundary is invalid or unrepresentable; no clock/debt changed");return false;}
    ESourceControlPhase Phase=ESourceControlPhase::Ended;double Gap=0.,Tolerance=SourcePhaseTolerance(Clock,End);
    if(MeaningfulSourceGap(Clock,Start))
    {Phase=ESourceControlPhase::BeforeStart;Gap=Start-Clock;Tolerance=SourcePhaseTolerance(Clock,Start);}
    else if(MeaningfulSourceGap(Clock,End))
    {Phase=ESourceControlPhase::Active;Gap=End-Clock;}
    OutPhase=Phase;OutBoundaryGap=Gap;OutElapsed=FMath::Max(0.,Clock-Start);OutTolerance=Tolerance;return true;
}

inline bool LocksGranularBuildingInputs(bool bEpisodeActive,EDisasterType Type)
{
    return bEpisodeActive&&(Type==EDisasterType::Avalanche||Type==EDisasterType::Landslide);
}

/** Atomic accrual of UNACCEPTED caller time. Accepted/frontier time is only
 * observed for finite-total validation and never added again. Entry-phase
 * bWaterPaused remains fixed for the entire callback: a mass completion must
 * not retroactively charge its same playback interval to the water clock.
 * On failure all owner inputs remain unchanged; OutWorldDelta is zero. */
inline bool AccrueCallerTime(double Now,double PublishedSeconds,double AcceptedSeconds,
    bool bWaterPaused,double EntryTimeScale,double& LastWorldSeconds,double& CallerDebt,
    double& OutWorldDelta,FString& Failure)
{
    Failure.Reset();OutWorldDelta=0.;
    if(!FMath::IsFinite(Now)||Now<0.||!FMath::IsFinite(LastWorldSeconds)||LastWorldSeconds<0.
        ||Now<LastWorldSeconds||!FMath::IsFinite(CallerDebt)||CallerDebt<0.
        ||!FMath::IsFinite(PublishedSeconds)||PublishedSeconds<0.
        ||!FMath::IsFinite(AcceptedSeconds)||AcceptedSeconds<0.
        ||!FMath::IsFinite(EntryTimeScale)||EntryTimeScale<=0.)
    {Failure=TEXT("Physical caller clock has nonfinite, negative or reversed inputs; no owner time changed");return false;}
    const double Delta=Now-LastWorldSeconds;
    const double Added=bWaterPaused?0.:Delta*EntryTimeScale;
    const double NewDebt=CallerDebt+Added;
    const double FrontierTotal=PublishedSeconds+AcceptedSeconds;
    if(!FMath::IsFinite(Delta)||!FMath::IsFinite(Added)||!FMath::IsFinite(NewDebt)
        ||!FMath::IsFinite(FrontierTotal)||!FMath::IsFinite(FrontierTotal+NewDebt))
    {Failure=TEXT("Physical caller clock would overflow; no elapsed seconds silently dropped");return false;}
    if(!bWaterPaused&&Delta>0.&&(Added==0.||NewDebt==CallerDebt))
    {Failure=TEXT("Physical caller clock cannot represent positive elapsed time at this magnitude; no owner time changed");return false;}
    LastWorldSeconds=Now;CallerDebt=NewDebt;OutWorldDelta=Delta;return true;
}

/** The accepted granular solve uses fixed building geometry and resistance
 * inputs. Its progressive damage is allowed; placement, repair and structural
 * rewrites cannot change the frozen solve while it is waiting or replaying. */
inline bool PreservesFrozenBuildingInputs(const FBuildingInstance& Before,const FBuildingInstance& After)
{
    return Before.InstanceID==After.InstanceID&&Before.GridCoords==After.GridCoords
        &&Before.BuildingData==After.BuildingData&&Before.ISMInstanceIndex==After.ISMInstanceIndex
        &&Before.CurrentFloors==After.CurrentFloors&&Before.Foundation==After.Foundation
        &&Before.FoundationQuality==After.FoundationQuality&&Before.StructuralQuality==After.StructuralQuality
        &&Before.RoofQuality==After.RoofQuality&&Before.Preparedness==After.Preparedness
        &&Before.AgeInYears==After.AgeInYears&&Before.bIsRetrofitted==After.bIsRetrofitted
        &&FMath::IsFinite(After.CurrentDamage)&&After.CurrentDamage>=Before.CurrentDamage&&After.CurrentDamage<=100.f
        &&static_cast<uint8>(After.DamageState)>=static_cast<uint8>(Before.DamageState)
        &&static_cast<uint8>(After.DamageState)<=static_cast<uint8>(EDamageState::Memorial);
}
}
