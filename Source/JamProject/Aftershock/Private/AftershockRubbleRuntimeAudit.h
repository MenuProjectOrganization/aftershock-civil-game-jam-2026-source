#pragma once
#include "CoreMinimal.h"
class UWorld;
struct FMinimalViewInfo;
using FAftershockRubbleSceneCapture=TFunction<bool(UWorld&,const FMinimalViewInfo&,const FString&)>;
/** Explicit development fixture; never runs in ordinary gameplay. */
void BeginAftershockRubbleRuntimeAudit(UWorld& World,FAftershockRubbleSceneCapture CaptureScene);
