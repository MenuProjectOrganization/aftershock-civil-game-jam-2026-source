#pragma once

#include "CoreMinimal.h"

class UWorld;
struct FMinimalViewInfo;

using FAftershockBridgeSceneCapture = TFunction<bool(UWorld&, const FMinimalViewInfo&, const FString&)>;

/** Explicit development fixture. Runs only with -AftershockBridgeVisualAudit. */
void BeginAftershockBridgeRuntimeAudit(UWorld& World, FAftershockBridgeSceneCapture CaptureScene);
