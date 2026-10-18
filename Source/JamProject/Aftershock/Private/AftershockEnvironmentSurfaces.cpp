#include "AftershockEnvironmentSurfaces.h"
#include "AftershockEnvironmentQuality.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameters.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
namespace
{
    struct FLightOriginal {TWeakObjectPtr<UDirectionalLightComponent> Light;bool bCastShadows=true;};
    TArray<FLightOriginal> LightOriginals;
    struct FSurfacePass {TWeakObjectPtr<UWorld> World;uint32 Signature=0;double Time=-1.;};
    TArray<FSurfacePass> SurfacePasses;
    void SetScalar(UMaterialInstanceDynamic& M,const TCHAR* Name,float Value)
    {if(!FMath::IsNearlyEqual(M.K2_GetScalarParameterValue(Name),Value))M.SetScalarParameterValue(Name,Value);}
}
void ApplyAftershockEnvironmentSurfaceQuality(UWorld* World)
{
    if(!World)return;
    const auto Q=GetAftershockEnvironmentQuality(World);
    const auto Camera=GetAftershockPresentationBudget(World);
    const auto B=MakeAftershockEnvironmentBudget(Q,Camera);
    const uint32 Signature=HashCombine(Q.GetSignature(),GetTypeHash(static_cast<uint8>(Camera.Tier)));
    const double Now=FPlatformTime::Seconds();
    SurfacePasses.RemoveAll([](const auto& P){return !P.World.IsValid();});
    auto* Pass=SurfacePasses.FindByPredicate([World](const auto& P){return P.World.Get()==World;});
    if(Pass&&Pass->Signature==Signature&&Now-Pass->Time<.9)return;
    if(!Pass&&SurfacePasses.Num()<8){SurfacePasses.Add({World,Signature,Now});Pass=&SurfacePasses.Last();}
    if(Pass){Pass->Signature=Signature;Pass->Time=Now;}
    LightOriginals.RemoveAll([](const auto& E){return !E.Light.IsValid();});
    for(TActorIterator<AActor> It(World);It;++It)
    {
        TInlineComponentArray<UStaticMeshComponent*> Meshes(*It);
        for(auto* Mesh:Meshes)for(int32 Slot=0;Slot<Mesh->GetNumMaterials();++Slot)
        {
            auto* Material=Mesh->GetMaterial(Slot);if(!Material)continue;
            float Ignored=0.f;
            const bool Ground=Material->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("GlobalGroundNormalQuality")),Ignored);
            const bool Water=Material->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("GlobalWaterNormalQuality")),Ignored);
            if(!Ground&&!Water)continue;
            auto* MID=Cast<UMaterialInstanceDynamic>(Material);
            if(!MID)MID=Mesh->CreateAndSetMaterialInstanceDynamic(Slot);
            if(!MID)continue;
            if(Ground)SetScalar(*MID,TEXT("GlobalGroundNormalQuality"),B.GroundNormalStrength);
            if(Water)
            {
                SetScalar(*MID,TEXT("GlobalWaterNormalQuality"),B.WaterNormalStrength/.08f);
                SetScalar(*MID,TEXT("GlobalWaterFoamQuality"),B.WaterFoamStrength);
            }
        }
        TInlineComponentArray<UDirectionalLightComponent*> Lights(*It);
        for(auto* Light:Lights)
        {
            auto* Original=LightOriginals.FindByPredicate([Light](const auto& E){return E.Light.Get()==Light;});
            if(!Original&&LightOriginals.Num()<16){LightOriginals.Add({Light,Light->CastShadows!=0});Original=&LightOriginals.Last();}
            if(!Original)continue;
            const bool bShadow=Original->bCastShadows&&B.DynamicShadowDistanceCm>0.f;
            if((Light->CastShadows!=0)!=bShadow)Light->SetCastShadows(bShadow);
            if(!FMath::IsNearlyEqual(Light->DynamicShadowDistanceMovableLight,B.DynamicShadowDistanceCm))Light->SetDynamicShadowDistanceMovableLight(B.DynamicShadowDistanceCm);
            const int32 Cascades=FMath::Clamp(Q.Shadows,1,4);
            if(Light->DynamicShadowCascades!=Cascades){Light->SetDynamicShadowCascades(Cascades);}
        }
    }
}
