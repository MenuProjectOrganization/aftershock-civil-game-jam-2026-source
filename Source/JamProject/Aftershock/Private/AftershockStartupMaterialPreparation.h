#pragma once
#if WITH_EDITOR
#include "ShaderCompiler.h"
#endif

static bool IsAftershockMaterialMapFinalizationRequired(const FMaterialResource& Resource)
{
#if WITH_EDITOR
    // UE 5.8 ShaderCompiler.cpp:2336 deliberately leaves ordinary per-shader
    // DDC maps unfinalized. Required/special maps and cooked maps stay strict.
    return IsMaterialMapDDCEnabled()||Resource.IsRequiredComplete();
#else
    return true;
#endif
}

// Active renderer resource checks and bounded pre-menu preparation.
// Included only by AftershockStartupPreparationSubsystem.cpp.
static bool IsAftershockActiveOwnedMaterialReady(UMaterialInterface* Material,
    UWorld& World,FString* OutReason=nullptr,TSharedPtr<FJsonObject>* OutDetails=nullptr)
{
    TSharedPtr<FJsonObject> Row;
    if(OutDetails)
    {
        Row=MakeShared<FJsonObject>();*OutDetails=Row;
        Row->SetBoolField(TEXT("ready"),false);
        Row->SetStringField(TEXT("interface"),GetPathNameSafe(Material));
        Row->SetNumberField(TEXT("world_feature_level"),static_cast<int32>(World.GetFeatureLevel()));
        Row->SetBoolField(TEXT("resource_present"),false);
        Row->SetBoolField(TEXT("resource_owner_in_requested_ancestry"),false);
        Row->SetBoolField(TEXT("resource_platform_matches"),false);
        Row->SetBoolField(TEXT("resource_quality_matches"),false);
        Row->SetBoolField(TEXT("map_present"),false);
        Row->SetBoolField(TEXT("map_platform_matches"),false);
        Row->SetBoolField(TEXT("valid_finalized_map"),false);
        Row->SetBoolField(TEXT("active_map_complete"),false);
    }
    const auto Reject=[&](const TCHAR* Reason)
    {
        if(OutReason)*OutReason=Reason;
        if(Row)Row->SetStringField(TEXT("reason"),Reason);
        return false;
    };
    if(!IsValid(Material))return Reject(TEXT("invalid_interface"));
    const EShaderPlatform Platform=GShaderPlatformForFeatureLevel[World.GetFeatureLevel()];
    const EMaterialQualityLevel::Type Quality=GetCachedScalabilityCVars().MaterialQualityLevel;
    if(Row)
    {
        Row->SetNumberField(TEXT("requested_platform"),static_cast<int32>(Platform));
        Row->SetNumberField(TEXT("requested_quality"),static_cast<int32>(Quality));
        Row->SetBoolField(TEXT("broad_is_complete_diagnostic_only"),Material->IsComplete());
    }
    FMaterialResource* Resource=Material->GetMaterialResource(Platform,Quality);
    if(Row&&Resource)
    {
        Row->SetBoolField(TEXT("resource_present"),true);
        Row->SetStringField(TEXT("resource_owner"),GetPathNameSafe(Resource->GetMaterialInterface()));
        Row->SetStringField(TEXT("resource_base_material"),GetPathNameSafe(Resource->GetMaterial()));
        Row->SetNumberField(TEXT("resource_platform"),static_cast<int32>(Resource->GetShaderPlatform()));
        Row->SetNumberField(TEXT("resource_quality"),static_cast<int32>(Resource->GetQualityLevel()));
        Row->SetBoolField(TEXT("resource_platform_matches"),Resource->GetShaderPlatform()==Platform);
        Row->SetBoolField(TEXT("resource_quality_matches"),Resource->GetQualityLevel()==Quality||Resource->GetQualityLevel()==EMaterialQualityLevel::Num);
        Row->SetBoolField(TEXT("resource_quality_is_shared_generic"),Resource->GetQualityLevel()==EMaterialQualityLevel::Num);
    }
    if(!Resource)return Reject(TEXT("missing_active_owned_resource"));
    if(Resource->GetShaderPlatform()!=Platform)return Reject(TEXT("wrong_resource_platform"));
    const auto ResourceQuality=Resource->GetQualityLevel();
    if(ResourceQuality!=Quality&&ResourceQuality!=EMaterialQualityLevel::Num)
        return Reject(TEXT("wrong_resource_quality"));
    // Num is Engine's shared non-quality-specific resource, NOT a default
    // material. FindMaterialResource(...,bAllowDefaultQuality=true) chooses it
    // only if no exact-quality resource exists (Material.cpp1012).
    UMaterialInterface* ResourceOwner=Resource->GetMaterialInterface();
    TSet<const UMaterialInterface*> Seen;
    bool Owns=false;
    const UMaterialInterface* Ancestor=Material;
    for(int32 Depth=0;Ancestor&&Depth<32;++Depth)
    {
        if(Seen.Contains(Ancestor))return Reject(TEXT("cyclic_material_parent"));
        Seen.Add(Ancestor);
        if(Ancestor==ResourceOwner){Owns=true;break;}
        const auto* Instance=Cast<UMaterialInstance>(Ancestor);
        Ancestor=Instance?Instance->Parent.Get():nullptr;
    }
    if(Row)Row->SetBoolField(TEXT("resource_owner_in_requested_ancestry"),Owns);
    if(!Owns)return Reject(TEXT("resource_is_not_owned_by_requested_parent_chain"));
    const FMaterialShaderMap* Map=Resource->GetGameThreadShaderMap();
    if(Row)Row->SetBoolField(TEXT("map_present"),Map!=nullptr);
    if(!Map)return Reject(TEXT("missing_active_shader_map"));
    if(Row)
    {
        Row->SetNumberField(TEXT("map_platform"),static_cast<int32>(Map->GetShaderPlatform()));
        Row->SetBoolField(TEXT("map_platform_matches"),Map->GetShaderPlatform()==Platform);
        Row->SetBoolField(TEXT("valid_finalized_map"),Resource->HasValidGameThreadShaderMap());
        Row->SetBoolField(TEXT("active_map_complete"),Resource->IsGameThreadShaderMapComplete());
    }
    if(Map->GetShaderPlatform()!=Platform)return Reject(TEXT("wrong_shader_map_platform"));
    if(!Resource->IsGameThreadShaderMapComplete())return Reject(TEXT("active_shader_map_incomplete"));
    const bool RequireFinalized=IsAftershockMaterialMapFinalizationRequired(*Resource);
    if(Row)
    {
        Row->SetBoolField(TEXT("map_finalization_required_in_engine_mode"),RequireFinalized);
        Row->SetBoolField(TEXT("map_is_finalized"),Map->IsCompilationFinalized());
        Row->SetBoolField(TEXT("map_compiled_successfully_flag"),Map->CompiledSuccessfully());
    }
    if(RequireFinalized&&(!Resource->HasValidGameThreadShaderMap()||!Map->CompiledSuccessfully()))
        return Reject(TEXT("required_shader_map_not_finalized_successfully"));
#if WITH_EDITOR
    if(Row)
    {
        Row->SetBoolField(TEXT("shader_job_cache_ddc_enabled"),IsShaderJobCacheDDCEnabled());
        Row->SetBoolField(TEXT("material_map_ddc_enabled"),IsMaterialMapDDCEnabled());
        Row->SetBoolField(TEXT("resource_required_complete"),Resource->IsRequiredComplete());
    }
    // Engine derives this published bit from its map-owned IsComplete check
    // (MaterialShared.cpp). The underlying method is not DLL-exported in the
    // installed Windows Engine; do not claim a separate uncached invocation.
    const bool CompleteSet=Resource->IsGameThreadShaderMapComplete();
    if(Row)Row->SetBoolField(TEXT("actual_owned_shader_set_complete"),CompleteSet);
    if(Row)Row->SetStringField(TEXT("shader_set_verification"),TEXT("Engine-published map-owned completeness bit; no independent uncached map invocation"));
    if(!CompleteSet)return Reject(TEXT("actual_owned_shader_set_incomplete"));
    if(Row)Row->SetNumberField(TEXT("compile_error_count"),Resource->GetCompileErrors().Num());
    if(!Resource->GetCompileErrors().IsEmpty())return Reject(TEXT("owned_resource_compile_errors"));
#endif
    if(Row){Row->SetBoolField(TEXT("ready"),true);Row->SetStringField(TEXT("reason"),TEXT("ready"));}
    if(OutReason)*OutReason=TEXT("ready");
    return true;
}

#if WITH_EDITOR
static int32 RequestAftershockBoundedMaterialWarmup(
    const TSet<UMaterialInterface*>& Collected, UWorld& World,
    TSet<TWeakObjectPtr<UMaterialInterface>>& Once,
    TSet<TWeakObjectPtr<UMaterialInterface>>& CachedOnce)
{
    constexpr int32 RequestsPerHeartbeat=4;
    const EShaderPlatform Platform=GShaderPlatformForFeatureLevel[World.GetFeatureLevel()];
    const EMaterialQualityLevel::Type Quality=GetCachedScalabilityCVars().MaterialQualityLevel;
    TSet<UMaterialInterface*> Candidates=Collected;
    // A dynamic MID's IsComplete deliberately does not inspect its parent in
    // UE5.8; request/check the real parent too (MaterialInterface.cpp2594 TODO).
    for(auto* Material:Collected)if(IsValid(Material))if(auto* Base=Material->GetMaterial())Candidates.Add(Base);
    TArray<UMaterialInterface*> Ordered=Candidates.Array();
    Ordered.Sort([](const UMaterialInterface& A,const UMaterialInterface& B)
        {return A.GetPathName()<B.GetPathName();});
    int32 Requested=0;
    for(auto* Material:Ordered)
    {
        if(!IsValid(Material))continue;
        const TWeakObjectPtr<UMaterialInterface> Key(Material);
        FMaterialResource* Resource=Material->GetMaterialResource(Platform,Quality);
        if(IsAftershockActiveOwnedMaterialReady(Material,World))continue;
        const TCHAR* Method=TEXT("CacheShadersBackground");
        if(Resource&&Resource->GetGameThreadShaderMap())
        {
            if(Once.Contains(Key))continue;
            Once.Add(Key);++Requested;
            // Same job-submit primitive as UMaterialInterface::EnsureIsComplete,
            // WITHOUT its blocking FinishCompilation(). Preserves current UB/resource.
            // Zero queued jobs can mean DEFERRED, so IsCompilationFinished must
            // not gate submission. Engine checks its private ID and priority.
            Resource->SubmitCompileJobs_GameThread(EShaderCompileJobPriority::High);
            Method=TEXT("SubmitCurrentDeferredResource");
        }
        else
        {
            if(CachedOnce.Contains(Key))continue;
            CachedOnce.Add(Key);++Requested;
            // Public/runtime CacheShaders API selects current rendering resources;
            // does not regenerate material ID (UMaterial::CacheShaders -> false).
            // Match Engine's SubmitRemainingJobsForWorld safety scope: bare
            // CacheShaders recreates uniform buffers and must not be used alone.
            FMaterialUpdateContext Update(FMaterialUpdateContext::EOptions::SyncWithRenderingThread,Platform);
            Update.AddMaterialInterface(Material);
            Material->CacheShaders(EMaterialShaderPrecompileMode::Background);
        }
        UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_STARTUP_SHADER_REQUEST material=%s platform=%d quality=%d method=%s requestNotCompletion=1 blockingFinishAll=0"),
            *Material->GetPathName(),static_cast<int32>(Platform),static_cast<int32>(Quality),Method);
        if(Requested>=RequestsPerHeartbeat)break;
    }
    return Requested;
}
#endif

