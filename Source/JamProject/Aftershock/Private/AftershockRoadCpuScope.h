#pragma once
#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

inline bool AftershockRoadCpuDiagnosticsEnabled()
{
    static const bool Enabled=FParse::Param(FCommandLine::Get(),TEXT("AftershockRoadCpuDiagnostics"));
    return Enabled;
}

// Opt-in source-side CPU attribution. No gameplay, scheduling or budget changes.
// Nested durations overlap; do not sum parent and child phases.
struct FAftershockRoadCpuScope
{
    explicit FAftershockRoadCpuScope(const TCHAR* InPhase):Phase(InPhase)
    {
        bEnabled=AftershockRoadCpuDiagnosticsEnabled(); if(bEnabled)Started=FPlatformTime::Seconds();
    }
    ~FAftershockRoadCpuScope()
    {
        if(bEnabled)UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_ROAD_CPU phase=%s cpuMs=%.3f frame=%llu nestedScopesOverlap=1 cpuOnly=1"),
            Phase,(FPlatformTime::Seconds()-Started)*1000.,static_cast<unsigned long long>(GFrameCounter));
    }
    const TCHAR* Phase;double Started=0.;bool bEnabled=false;
};

// Scratch counters only; never serialized, consulted for scheduling or used as
// a route budget. Totals count NORMAL goal fields, not emergency-centre fields.
struct FAftershockRoadGoalCpuTotals
{
    uint64 Calls=0,CacheHits=0,Fields=0;double FieldBuildMs=0.;
};
inline FAftershockRoadGoalCpuTotals& AftershockRoadGoalCpuTotals()
{static FAftershockRoadGoalCpuTotals Totals;return Totals;}
struct FAftershockRoadGoalCpuFrame
{
    explicit FAftershockRoadGoalCpuFrame(const TCHAR* InPhase):Phase(InPhase)
    {bEnabled=AftershockRoadCpuDiagnosticsEnabled();if(bEnabled)Before=AftershockRoadGoalCpuTotals();}
    ~FAftershockRoadGoalCpuFrame()
    {
        if(!bEnabled)return;
        const auto& Now=AftershockRoadGoalCpuTotals();
        UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_ROAD_CPU_GOALS phase=%s calls=%llu cacheHits=%llu normalGoalFields=%llu normalFieldBuildMs=%.3f frame=%llu aggregateOnly=1 cpuOnly=1"),
            Phase,static_cast<unsigned long long>(Now.Calls-Before.Calls),static_cast<unsigned long long>(Now.CacheHits-Before.CacheHits),
            static_cast<unsigned long long>(Now.Fields-Before.Fields),Now.FieldBuildMs-Before.FieldBuildMs,
            static_cast<unsigned long long>(GFrameCounter));
    }
    const TCHAR* Phase;FAftershockRoadGoalCpuTotals Before;bool bEnabled=false;
};
