#pragma once
#include "CoreMinimal.h"
class UWorld;
/** Editor-only source/render-buffer readback; returns false without CPU buffers. */
bool ReadAftershockEnvironmentRenderFrames(const FString& AbsoluteReportPath);
