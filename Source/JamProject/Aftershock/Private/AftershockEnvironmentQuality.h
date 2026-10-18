#pragma once

#include "CoreMinimal.h"
#include "AftershockPresentationBudget.h"

class UWorld;

/** Proposed local presentation inputs. UGameUserSettings remains the only saved graphics owner. */
struct FAftershockEnvironmentQuality
{
	int32 ViewDistance=3,Shadows=3,GlobalIllumination=3,Reflections=3,AntiAliasing=3;
	int32 Textures=3,Effects=3,PostProcess=3,Foliage=3,Shading=3;
	uint32 GetSignature() const;
	/** 0..4 for matching groups; 5 means Custom. No new persisted preset field. */
	int32 GetPresetIndex() const;
};

struct FAftershockEnvironmentBudget
{
	float TreeDensity=0.f,GrassDensity=0.f,ShrubDensity=0.f;
	int32 TreeInstanceCap=0,GrassInstanceCap=0,ShrubInstanceCap=0;
	float TreeCullStartCm=0.f,TreeCullEndCm=0.f,GrassCullStartCm=0.f,GrassCullEndCm=0.f;
	float GroundNormalStrength=0.f,WaterNormalStrength=0.f,WaterFoamStrength=0.f;
	float DynamicShadowDistanceCm=0.f;
	int32 WaterDetailLevel=0;
	bool bTreeShadows=false,bShrubShadows=false;
	/** Requests only; renderer/material capability must be validated by the integration. */
	bool bRequestWaterSSR=false;
};

/** Read at startup/Apply/rollback or a presentation rebuild, never once per instance. */
FAftershockEnvironmentQuality GetAftershockEnvironmentQuality(const UWorld* World);
/** Pure bounded policy. Never changes geometry, collision, population, weather or player settings. */
FAftershockEnvironmentBudget MakeAftershockEnvironmentBudget(
	const FAftershockEnvironmentQuality& Quality,const FAftershockPresentationBudget& Camera);
