#pragma once
#include "CoreMinimal.h"
class UWorld;
class UPrimitiveComponent;
class FJsonObject;
struct FMinimalViewInfo;
using FAftershockFireSceneCapture=TFunction<bool(UWorld&,const FMinimalViewInfo&,const FString&,UPrimitiveComponent*)>;
using FAftershockFireMetadataRead=TFunction<TSharedRef<FJsonObject>(UWorld&,const FString&)>;
/** Opt-in real configured startup/Model2 world; four actual fire-life-cycle frames. */
void BeginAftershockFireRuntimeAudit(UWorld& World,FAftershockFireSceneCapture Capture,FAftershockFireMetadataRead ReadMetadata);
