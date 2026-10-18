#include "AftershockEnvironmentQuality.h"

#include "Engine/World.h"
#include "GameFramework/GameUserSettings.h"

namespace
{
	int32 Q(int32 Value){return FMath::Clamp(Value,0,4);}
	void Groups(const FAftershockEnvironmentQuality& V,int32 (&Out)[10])
	{
		const int32 Values[]={V.ViewDistance,V.Shadows,V.GlobalIllumination,V.Reflections,V.AntiAliasing,V.Textures,V.Effects,V.PostProcess,V.Foliage,V.Shading};
		for(int32 I=0;I<10;++I)Out[I]=Q(Values[I]);
	}
}

uint32 FAftershockEnvironmentQuality::GetSignature() const
{
	int32 Values[10];Groups(*this,Values);uint32 Signature=0;
	for(int32 Value:Values)Signature=HashCombine(Signature,GetTypeHash(Value));
	return Signature;
}
int32 FAftershockEnvironmentQuality::GetPresetIndex() const
{
	int32 Values[10];Groups(*this,Values);
	for(int32 I=1;I<10;++I)if(Values[I]!=Values[0])return 5;
	return Values[0];
}
FAftershockEnvironmentQuality GetAftershockEnvironmentQuality(const UWorld* World)
{
	FAftershockEnvironmentQuality V;
	// Native policy fixtures call Make... directly with authored input records.
	// A missing context never requires/creates a settings UObject.
	const UGameUserSettings* S=World?UGameUserSettings::GetGameUserSettings():nullptr;
	if(!S)return V;
	V.ViewDistance=Q(S->GetViewDistanceQuality());V.Shadows=Q(S->GetShadowQuality());
	V.GlobalIllumination=Q(S->GetGlobalIlluminationQuality());V.Reflections=Q(S->GetReflectionQuality());
	V.AntiAliasing=Q(S->GetAntiAliasingQuality());V.Textures=Q(S->GetTextureQuality());
	V.Effects=Q(S->GetVisualEffectQuality());V.PostProcess=Q(S->GetPostProcessingQuality());
	V.Foliage=Q(S->GetFoliageQuality());V.Shading=Q(S->GetShadingQuality());return V;
}
FAftershockEnvironmentBudget MakeAftershockEnvironmentBudget(
	const FAftershockEnvironmentQuality& Quality,const FAftershockPresentationBudget& Camera)
{
	FAftershockEnvironmentBudget B;
	const int32 F=Q(Quality.Foliage),E=Q(Quality.Effects),S=Q(Quality.Shading),H=Q(Quality.Shadows),R=Q(Quality.Reflections);
	const float Trees[]={.25f,.45f,.65f,.85f,1.f},Grass[]={0.f,.18f,.38f,.65f,1.f},Shrubs[]={0.f,.25f,.5f,.75f,1.f};
	const int32 TreeCaps[]={512,1024,2048,3072,4096},GrassCaps[]={0,2000,6000,12000,20000},ShrubCaps[]={0,256,768,1536,2048};
	const float TreeEnds[]={20000.f,30000.f,45000.f,60000.f,75000.f},GrassEnds[]={0.f,5500.f,8500.f,12000.f,16000.f};
	const float Normals[]={.15f,.35f,.60f,.85f,1.f},WaterNormals[]={.015f,.035f,.05f,.065f,.08f},Foam[]={.25f,.4f,.65f,.85f,1.f};
	const float ShadowRanges[]={0.f,15000.f,30000.f,50000.f,70000.f};
	B.GroundNormalStrength=Normals[S];B.WaterNormalStrength=WaterNormals[E];B.WaterFoamStrength=Foam[E];B.WaterDetailLevel=E;
	B.DynamicShadowDistanceCm=ShadowRanges[H];
	// Regional/globe retain all essential water/hazard geometry. Tiny ground
	// decorations are omitted by camera scale independently of any user preset.
	if(!Camera.bDrawDecorations)
	{
		B.GroundNormalStrength=FMath::Min(B.GroundNormalStrength,.15f);B.WaterNormalStrength=FMath::Min(B.WaterNormalStrength,.025f);B.WaterDetailLevel=FMath::Min(E,1);
		B.DynamicShadowDistanceCm=0.f;return B;
	}
	B.TreeDensity=Trees[F];B.GrassDensity=Grass[F];B.ShrubDensity=Shrubs[F];
	B.TreeInstanceCap=TreeCaps[F];B.GrassInstanceCap=GrassCaps[F];B.ShrubInstanceCap=ShrubCaps[F];
	// Fixed cm caps prevent sg.ViewDistanceQuality@Cine's x10 multiplier from
	// multiplying authored instance admission. Use these to admit desired cells,
	// not solely as a render-cull setting that the engine could scale again.
	B.TreeCullEndCm=TreeEnds[F];B.TreeCullStartCm=B.TreeCullEndCm*.75f;
	B.GrassCullEndCm=GrassEnds[F];B.GrassCullStartCm=B.GrassCullEndCm*.7f;
	B.bTreeShadows=H>0&&(Camera.Tier==EAftershockPresentationTier::Street||(H>=2&&Camera.ViewDistanceCm<ShadowRanges[H]));
	B.bShrubShadows=H>=3&&Camera.Tier==EAftershockPresentationTier::Street;
	B.bRequestWaterSSR=R>=2&&E>=2;
	return B;
}
