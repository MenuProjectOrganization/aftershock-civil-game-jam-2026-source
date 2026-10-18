#pragma once
#include "CoreMinimal.h"

// Editor-only opt-in authoring operation. Requires -NullRHI; never called by play.
bool RepairAftershockBroadleafExplicitTangents(const FString& AbsoluteReportPath);
