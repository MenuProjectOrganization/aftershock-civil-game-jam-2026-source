#pragma once
#include "CoreMinimal.h"
class UWorld;
/** Opt-in -AftershockNarrativeAudit. Actual new-game tutorial autoplay/skip and ending UI screenshots; normally 42s, bounded real-time failure exit. */
void BeginAftershockNarrativeRuntimeAudit(UWorld& World);
