#pragma once
#include "CoreMinimal.h"

class UWorld;
struct FMinimalViewInfo;
using FAftershockChunkSceneCapture=TFunction<bool(UWorld&,const FMinimalViewInfo&,const FString&)>;

/** Opt-in scripted public-API camera fixture; never represents OS/human input. */
void BeginAftershockChunkRuntimeAudit(UWorld& World,FAftershockChunkSceneCapture CaptureScene);
