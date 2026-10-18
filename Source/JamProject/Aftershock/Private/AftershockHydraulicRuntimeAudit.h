#pragma once
#include "CoreMinimal.h"
class UWorld;
class FJsonObject;
class UPrimitiveComponent;
struct FMinimalViewInfo;
using FAftershockHydraulicSceneCapture=TFunction<bool(UWorld&,const FMinimalViewInfo&,const FString&,UPrimitiveComponent*)>;
using FAftershockHydraulicMetadataRead=TFunction<TSharedRef<FJsonObject>(UWorld&,const FString&)>;
/** Opt-in real -game RHI observer. Never initializes a synthetic world. */
void BeginAftershockHydraulicRuntimeAudit(UWorld& World,FAftershockHydraulicSceneCapture Capture,
    FAftershockHydraulicMetadataRead ReadMetadata);
