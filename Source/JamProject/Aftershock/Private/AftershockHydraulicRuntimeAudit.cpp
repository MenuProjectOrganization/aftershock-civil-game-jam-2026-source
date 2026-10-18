#include "AftershockHydraulicRuntimeAudit.h"
#if !UE_BUILD_SHIPPING && WITH_EDITOR
#include "AftershockHydraulicSurfaceActor.h"
#include "Aftershock/Public/AftershockDisasterDirectorSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Camera/CameraTypes.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialShared.h"
#include "Misc/CommandLine.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "RHIGlobals.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TextureResource.h"
#include "UnrealEngine.h"
#include "AftershockStartupMaterialPreparation.h"

namespace AftershockHydraulicRuntimeAuditPrivate
{
constexpr int32 FieldSize=256;
struct FReadbackJob
{
    FReadbackJob():Field(FName(TEXT("AftershockHydraulicFieldAudit"))),Mask(FName(TEXT("AftershockHydraulicMaskAudit"))){}
    FRHIGPUTextureReadback Field,Mask;
    TAtomic<bool> bEnqueued{false};
    TAtomic<bool> bMapped{false};
    bool bMapQueued=false,bMapValid=false;
    TArray<FVector4f> Expected;
    TArray<FVector4f> Actual;
    TArray<uint8> ActualMasks;
    TBitArray<> Valid;
    FAftershockHydraulicRenderStats Stats;
    TSharedPtr<FJsonObject> Receipt;
    double BeganAt=0.;
    uint64 CanonicalStep=0;
    bool bSceneSaved=false;
};
struct FAuditState
{
    TWeakObjectPtr<UWorld> World;
    FAftershockHydraulicSceneCapture Capture;
    FAftershockHydraulicMetadataRead ReadMetadata;
    TSharedPtr<FReadbackJob,ESPMode::ThreadSafe> Job;
    TSharedRef<FJsonObject> Report=MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Frames;
    FMinimalViewInfo PinnedView;
    FGuid EventID;
    FString Failure,TriggerFailure;
    double BeganAt=0.,SourcePhysicalSeconds=0.,SourceDuration=0.,SimulationScale=0.;
    double WallDeadlineSeconds=300.;
    int32 Stage=0,TriggerAttempts=0;
    bool bPinned=false,bBaseline=false,bWarningRequested=false,bTriggered=false,bSawFlow=false;
    bool bFinished=false,bSawFieldChange=false,bAnyGPUReadback=false;
    uint32 FirstCRC=0;
    double LastCheckpointAt=-1.e30;
    uint64 AdmissionAttempts=0;
    FString LastAdmissionWait=TEXT("NotAttempted");
    TMap<FString,uint64> AdmissionWaitCounts;
};
FString Folder(){return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/HydraulicRuntime"));}
bool Write(FAuditState& S)
{
    S.Report->SetArrayField(TEXT("frames"),S.Frames);S.Report->SetStringField(TEXT("failure"),S.Failure);
    FString JSON;if(!FJsonSerializer::Serialize(S.Report,TJsonWriterFactory<>::Create(&JSON)))return false;
    IFileManager::Get().MakeDirectory(*Folder(),true);
    return FFileHelper::SaveStringToFile(JSON,*(Folder()/TEXT("hydraulic_runtime_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
bool Active(UWorld* W)
{
    if(!IsValid(W)||W->bIsTearingDown||!W->IsGameWorld()||!GEngine||IsRunningCommandlet())return false;
    const auto* Context=GEngine->GetWorldContextFromWorld(W);return Context&&Context->World()==W;
}
bool MaterialReady(AAftershockHydraulicSurfaceActor& Actor,TSharedRef<FJsonObject> Row)
{
    auto* Surface=Actor.GetSurfaceForAudit();auto* MID=Surface?Cast<UMaterialInstanceDynamic>(Surface->GetMaterial(0)):nullptr;
    auto* Base=MID?MID->GetMaterial():nullptr;
    const auto Quality=GetCachedScalabilityCVars().MaterialQualityLevel;
    const auto Feature=Surface&&Surface->GetWorld()?Surface->GetWorld()->GetFeatureLevel():GMaxRHIFeatureLevel;
    const auto Platform=GShaderPlatformForFeatureLevel[Feature];
    auto* Resource=Base?Base->GetMaterialResource(Platform,Quality):nullptr;
    auto* Map=Resource?Resource->GetGameThreadShaderMap():nullptr;
    const bool RequireFinalized=Resource&&IsAftershockMaterialMapFinalizationRequired(*Resource);
    TSharedPtr<FJsonObject> ActiveResourceDetails;
    const bool ActiveOwnedReady=MID&&Surface&&Surface->GetWorld()
        &&IsAftershockActiveOwnedMaterialReady(MID,*Surface->GetWorld(),nullptr,&ActiveResourceDetails);
    if(ActiveResourceDetails)Row->SetObjectField(TEXT("active_owned_resource"),ActiveResourceDetails);
    Row->SetBoolField(TEXT("map_finalization_required_in_engine_mode"),RequireFinalized);
    Row->SetBoolField(TEXT("raw_map_finalized"),Map&&Map->IsCompilationFinalized());
    Row->SetBoolField(TEXT("raw_map_compiled_successfully_flag"),Map&&Map->CompiledSuccessfully());
    UTexture *FieldBinding=nullptr,*MaskBinding=nullptr;
    // Query BOTH names, even when the first is absent, for an independent
    // readiness diagnosis. No material/compile/texture state is changed.
    const bool FieldFound=MID&&MID->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("HydraulicField")),FieldBinding);
    const bool MaskFound=MID&&MID->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("HydraulicConnectivity")),MaskBinding);
    const bool Bound=MID&&FieldFound&&MaskFound
        &&FieldBinding==Actor.GetDataTextureForAudit()&&MaskBinding==Actor.GetConnectivityTextureForAudit();
    const bool Ready=Base&&Base->GetPathName()==TEXT("/Game/Environment/Water/M_HydraulicSurface.M_HydraulicSurface")
        &&Base->GetBlendMode()==BLEND_Translucent&&Resource&&Map&&Resource->GetMaterial()==Base
        &&Resource->GetMaterialInterface()==Base&&!Resource->IsDefaultMaterial()&&Resource->GetCompileErrors().IsEmpty()
        &&ActiveOwnedReady&&Resource->IsCompilationFinished()&&Resource->IsGameThreadShaderMapComplete()
        &&(!RequireFinalized||(Resource->HasValidGameThreadShaderMap()&&Map->IsCompilationFinalized()&&Map->CompiledSuccessfully()))
        &&Map->GetShaderPlatform()==Platform
        &&Resource->GetShaderPlatform()==Platform&&Map->GetShaderMapId().FeatureLevel==Feature
        &&Resource->GetFeatureLevel()==Feature
        &&(Resource->GetQualityLevel()==Quality||Resource->GetQualityLevel()==EMaterialQualityLevel::Num)
        &&Map->GetShaderMapId().QualityLevel==Resource->GetQualityLevel()&&Map->GetShaderMapId().BaseMaterialId==Base->StateId
        &&Resource->GetSamplerUsage()>=0&&Resource->GetSamplerUsage()<=16&&Bound;
    Row->SetBoolField(TEXT("actual_translucent_own_material_current_resource_ready"),Ready);
    Row->SetBoolField(TEXT("actual_mid_binds_actor_field_and_connectivity"),Bound);
    Row->SetStringField(TEXT("base_material"),GetPathNameSafe(Base));
    auto Predicates=MakeShared<FJsonObject>();TArray<TSharedPtr<FJsonValue>> FailedPredicates;
    const auto Predicate=[&](const TCHAR* Name,bool Value)
    {Predicates->SetBoolField(Name,Value);if(!Value)FailedPredicates.Add(MakeShared<FJsonValueString>(Name));};
    Predicate(TEXT("surface_present"),Surface!=nullptr);Predicate(TEXT("mid_present"),MID!=nullptr);
    Predicate(TEXT("base_present"),Base!=nullptr);
    Predicate(TEXT("expected_base_path"),Base&&Base->GetPathName()==TEXT("/Game/Environment/Water/M_HydraulicSurface.M_HydraulicSurface"));
    Predicate(TEXT("base_is_translucent"),Base&&Base->GetBlendMode()==BLEND_Translucent);
    Predicate(TEXT("resource_present"),Resource!=nullptr);Predicate(TEXT("map_present"),Map!=nullptr);
    Predicate(TEXT("resource_material_is_base"),Resource&&Base&&Resource->GetMaterial()==Base);
    Predicate(TEXT("resource_interface_is_base"),Resource&&Base&&Resource->GetMaterialInterface()==Base);
    Predicate(TEXT("resource_is_not_default"),Resource&&!Resource->IsDefaultMaterial());
    Predicate(TEXT("resource_compile_errors_empty"),Resource&&Resource->GetCompileErrors().IsEmpty());
    Predicate(TEXT("resource_compilation_finished"),Resource&&Resource->IsCompilationFinished());
    Predicate(TEXT("active_owned_resource_ready_for_engine_mode"),ActiveOwnedReady);
    Predicate(TEXT("resource_finalization_mode_satisfied"),Resource&&(!RequireFinalized||Resource->HasValidGameThreadShaderMap()));
    Predicate(TEXT("resource_shader_map_complete"),Resource&&Resource->IsGameThreadShaderMapComplete());
    Predicate(TEXT("map_finalization_mode_satisfied"),Map&&(!RequireFinalized||Map->IsCompilationFinalized()));
    Predicate(TEXT("map_success_mode_satisfied"),Map&&(!RequireFinalized||Map->CompiledSuccessfully()));
    Predicate(TEXT("map_platform_matches"),Map&&Map->GetShaderPlatform()==Platform);
    Predicate(TEXT("resource_platform_matches"),Resource&&Resource->GetShaderPlatform()==Platform);
    Predicate(TEXT("map_feature_matches"),Map&&Map->GetShaderMapId().FeatureLevel==Feature);
    Predicate(TEXT("resource_feature_matches"),Resource&&Resource->GetFeatureLevel()==Feature);
    Predicate(TEXT("resource_quality_matches_or_independent"),Resource&&(Resource->GetQualityLevel()==Quality||Resource->GetQualityLevel()==EMaterialQualityLevel::Num));
    Predicate(TEXT("map_quality_matches_resource"),Map&&Resource&&Map->GetShaderMapId().QualityLevel==Resource->GetQualityLevel());
    Predicate(TEXT("map_base_state_id_matches"),Map&&Base&&Map->GetShaderMapId().BaseMaterialId==Base->StateId);
    const int32 Samplers=Resource&&Map?Resource->GetSamplerUsage():INDEX_NONE;
    Predicate(TEXT("samplers_nonnegative"),Samplers>=0);Predicate(TEXT("samplers_at_most_16"),Samplers>=0&&Samplers<=16);
    Predicate(TEXT("field_parameter_found"),FieldFound);Predicate(TEXT("mask_parameter_found"),MaskFound);
    Predicate(TEXT("field_texture_is_actor_texture"),FieldFound&&FieldBinding==Actor.GetDataTextureForAudit());
    Predicate(TEXT("mask_texture_is_actor_texture"),MaskFound&&MaskBinding==Actor.GetConnectivityTextureForAudit());
    Row->SetObjectField(TEXT("predicates"),Predicates);Row->SetArrayField(TEXT("failed_predicates"),FailedPredicates);
    Row->SetStringField(TEXT("surface_material_interface"),GetPathNameSafe(Surface?Surface->GetMaterial(0):nullptr));
    Row->SetStringField(TEXT("mid_parent"),GetPathNameSafe(MID?MID->Parent.Get():nullptr));
    Row->SetStringField(TEXT("resource_material"),GetPathNameSafe(Resource?Resource->GetMaterial():nullptr));
    Row->SetStringField(TEXT("resource_interface"),GetPathNameSafe(Resource?Resource->GetMaterialInterface():nullptr));
    Row->SetStringField(TEXT("field_parameter_texture"),GetPathNameSafe(FieldBinding));
    Row->SetStringField(TEXT("actor_field_texture"),GetPathNameSafe(Actor.GetDataTextureForAudit()));
    Row->SetStringField(TEXT("mask_parameter_texture"),GetPathNameSafe(MaskBinding));
    Row->SetStringField(TEXT("actor_mask_texture"),GetPathNameSafe(Actor.GetConnectivityTextureForAudit()));
    Row->SetNumberField(TEXT("expected_shader_platform"),static_cast<int32>(Platform));
    Row->SetNumberField(TEXT("expected_feature_level"),static_cast<int32>(Feature));
    Row->SetNumberField(TEXT("expected_material_quality"),static_cast<int32>(Quality));
    Row->SetNumberField(TEXT("world_feature_level"),Surface&&Surface->GetWorld()?static_cast<int32>(Surface->GetWorld()->GetFeatureLevel()):INDEX_NONE);
    Row->SetNumberField(TEXT("base_blend_mode"),Base?static_cast<int32>(Base->GetBlendMode()):INDEX_NONE);
    Row->SetNumberField(TEXT("resource_shader_platform"),Resource?static_cast<int32>(Resource->GetShaderPlatform()):INDEX_NONE);
    Row->SetNumberField(TEXT("resource_feature_level"),Resource?static_cast<int32>(static_cast<ERHIFeatureLevel::Type>(Resource->GetFeatureLevel())):INDEX_NONE);
    Row->SetNumberField(TEXT("resource_material_quality"),Resource?static_cast<int32>(Resource->GetQualityLevel()):INDEX_NONE);
    Row->SetNumberField(TEXT("map_shader_platform"),Map?static_cast<int32>(Map->GetShaderPlatform()):INDEX_NONE);
    Row->SetNumberField(TEXT("map_feature_level"),Map?static_cast<int32>(Map->GetShaderMapId().FeatureLevel):INDEX_NONE);
    Row->SetNumberField(TEXT("map_material_quality"),Map?static_cast<int32>(Map->GetShaderMapId().QualityLevel):INDEX_NONE);
    Row->SetNumberField(TEXT("samplers"),Samplers);
    Row->SetStringField(TEXT("base_state_id"),Base?Base->StateId.ToString():FString());
    Row->SetStringField(TEXT("map_base_material_id"),Map?Map->GetShaderMapId().BaseMaterialId.ToString():FString());
    TArray<TSharedPtr<FJsonValue>> Errors;if(Resource)for(const auto& Error:Resource->GetCompileErrors())Errors.Add(MakeShared<FJsonValueString>(Error));
    Row->SetArrayField(TEXT("compile_errors"),Errors);
    Row->SetStringField(TEXT("readiness_diagnostic_scope"),TEXT("Passive current resource readback; no SubmitCompileJobs/FinishCompilation, fallback substitution or shader-state modification. Standalone MaterialResources compiles/finishes the inspected resource, so its earlier PASS cannot replace these real-game predicates."));
    return Ready;
}
bool WaitAdmission(FAuditState& S,const TCHAR* Reason)
{
    S.LastAdmissionWait=Reason;++S.AdmissionWaitCounts.FindOrAdd(S.LastAdmissionWait);return false;
}
void AdmissionCheckpoint(FAuditState& S,UWorld& W)
{
    const double Now=FPlatformTime::Seconds();if(Now-S.LastCheckpointAt<1.)return;S.LastCheckpointAt=Now;
    auto Row=MakeShared<FJsonObject>();auto* Sim=W.GetSubsystem<UDisasterSimulationSubsystem>();
    FAftershockPhysicalHazardSaveState Physical;
    // Audit-only, <=1Hz bounded snapshot; no validation/import/step/clock write.
    // Do not serialize any cells/people/source arrays into this diagnostic.
    if(Sim)Sim->CapturePhysicalSaveState(Physical);
    AAftershockHydraulicSurfaceActor* Actor=nullptr;int32 ActorCount=0;
    for(TActorIterator<AAftershockHydraulicSurfaceActor> It(&W);It;++It){Actor=*It;++ActorCount;}
    const FAftershockHydraulicRenderStats Stats=Actor?Actor->GetRenderStats():FAftershockHydraulicRenderStats();
    Row->SetNumberField(TEXT("wall_seconds"),Now-S.BeganAt);Row->SetNumberField(TEXT("world_seconds"),W.GetTimeSeconds());
    Row->SetNumberField(TEXT("canonical_physical_seconds"),Sim?Sim->GetPublishedHydraulicSeconds():0.);
    Row->SetNumberField(TEXT("canonical_published_step"),Physical.PublishedStep);
    Row->SetNumberField(TEXT("observed_total_pending_seconds"),Physical.PendingPhysicalSeconds);
    Row->SetNumberField(TEXT("observed_accepted_pending_seconds"),Physical.PendingAcceptedSeconds);
    Row->SetBoolField(TEXT("snapshot_frontier_resavable"),Physical.bWaterInitialized);
    Row->SetBoolField(TEXT("actual_model2_active"),Sim&&Sim->IsHydraulicModel2Active());
    Row->SetBoolField(TEXT("baseline_admitted"),S.bBaseline);Row->SetBoolField(TEXT("event_triggered"),S.bTriggered);
    Row->SetNumberField(TEXT("stage"),S.Stage);Row->SetNumberField(TEXT("admission_attempts"),S.AdmissionAttempts);
    Row->SetStringField(TEXT("last_admission_wait"),S.LastAdmissionWait);Row->SetBoolField(TEXT("readback_job_active"),S.Job.IsValid());
    Row->SetNumberField(TEXT("surface_actors"),ActorCount);Row->SetNumberField(TEXT("pending_upload_packets"),Stats.PendingPackets);
    Row->SetNumberField(TEXT("queued_revision"),Stats.LastQueuedRevision);Row->SetNumberField(TEXT("consumed_revision"),Stats.LastConsumedRevision);
    Row->SetNumberField(TEXT("consumed_physical_seconds"),Stats.ConsumedPhysicalSeconds);
    Row->SetNumberField(TEXT("render_source_lag_seconds"),(Sim?Sim->GetPublishedHydraulicSeconds():0.)-Stats.ConsumedPhysicalSeconds);
    Row->SetNumberField(TEXT("wet_cells"),Stats.LastWetCells);Row->SetNumberField(TEXT("valid_local_cells"),Stats.LastValidLocalCells);
    Row->SetNumberField(TEXT("expected_local_cells"),Stats.ExpectedLocalCells);Row->SetNumberField(TEXT("failed_rhi_uploads"),Stats.FailedRhiUploads);
    Row->SetNumberField(TEXT("connectivity_revision"),Stats.LastConnectivityRevision);
    auto Counts=MakeShared<FJsonObject>();for(const auto& Pair:S.AdmissionWaitCounts)Counts->SetNumberField(Pair.Key,Pair.Value);
    Row->SetObjectField(TEXT("aggregate_wait_counts"),Counts);
    const TSharedPtr<FJsonObject>* LastMaterial=nullptr;
    if(S.Report->TryGetObjectField(TEXT("last_material_readiness"),LastMaterial)&&LastMaterial&&LastMaterial->IsValid())
        Row->SetObjectField(TEXT("last_material_readiness"),LastMaterial->ToSharedRef());
    S.Report->SetObjectField(TEXT("last_checkpoint"),Row);Write(S);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_HYDRAULIC_RUNTIME_CHECKPOINT wall=%.3f world=%.6f physical=%.17g step=%llu pendingSeconds=%.17g acceptedSeconds=%.17g stage=%d frames=%d attempts=%llu wait=%s renderPending=%d queued=%llu consumed=%llu consumedSeconds=%.17g lag=%.9g wet=%d coverage=%d/%d uploadFailures=%llu readback=%d"),
        Now-S.BeganAt,W.GetTimeSeconds(),Sim?Sim->GetPublishedHydraulicSeconds():0.,Physical.PublishedStep,Physical.PendingPhysicalSeconds,Physical.PendingAcceptedSeconds,
        S.Stage,S.Frames.Num(),S.AdmissionAttempts,*S.LastAdmissionWait,Stats.PendingPackets,Stats.LastQueuedRevision,Stats.LastConsumedRevision,
        Stats.ConsumedPhysicalSeconds,(Sim?Sim->GetPublishedHydraulicSeconds():0.)-Stats.ConsumedPhysicalSeconds,Stats.LastWetCells,
        Stats.LastValidLocalCells,Stats.ExpectedLocalCells,Stats.FailedRhiUploads,S.Job.IsValid());
}
void Finish(FAuditState& S,bool Passed)
{
    if(S.bFinished)return;S.bFinished=true;
    S.Report->SetBoolField(TEXT("in_progress"),false);S.Report->SetBoolField(TEXT("valid"),Passed);
    S.Report->SetNumberField(TEXT("wall_seconds"),FPlatformTime::Seconds()-S.BeganAt);
    S.Report->SetBoolField(TEXT("actual_gpu_flow_observed"),S.bSawFlow);
    S.Report->SetBoolField(TEXT("actual_gpu_field_changed"),S.bSawFieldChange);
    S.Report->SetBoolField(TEXT("actual_gpu_readback_performed"),S.bAnyGPUReadback);
    S.Report->SetNumberField(TEXT("trigger_attempts"),S.TriggerAttempts);
    S.Report->SetStringField(TEXT("last_trigger_error"),S.TriggerFailure);
    const bool Saved=Write(S);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_HYDRAULIC_RUNTIME_RESULT passed=%d saved=%d frames=%d event=%s actualGPUReadback=%d sourceShortened=0 physicsTimeForged=0 failure=%s"),Passed,Saved,S.Frames.Num(),*S.EventID.ToString(),S.bAnyGPUReadback,*S.Failure);
    FPlatformMisc::RequestExit(false);
}
bool QueueFrame(FAuditState& S,UWorld& W,UDisasterSimulationSubsystem& Sim,UTerrainGridSubsystem& Terrain,const FString& Label)
{
    AAftershockHydraulicSurfaceActor* Actor=nullptr;int32 Actors=0;
    for(TActorIterator<AAftershockHydraulicSurfaceActor> It(&W);It;++It){Actor=*It;++Actors;}
    ++S.AdmissionAttempts;
    if(Actors!=1||!Actor)return WaitAdmission(S,TEXT("SurfaceActorCount"));
    // Retire an ALREADY consumed upload/fence before reading cached stats.
    // No wait/flush/new upload/canonical publication/time change is requested.
    Actor->PumpUploads();
    const auto Stats=Actor->GetRenderStats();auto* Surface=Actor->GetSurfaceForAudit();
    auto* Field=Actor->GetDataTextureForAudit();auto* Mask=Actor->GetConnectivityTextureForAudit();
    const double Physical=Sim.GetPublishedHydraulicSeconds();
    if(Stats.PendingPackets!=0)return WaitAdmission(S,TEXT("UploadPacketPending"));
    if(Stats.LastConsumedRevision==0)return WaitAdmission(S,TEXT("NoConsumedRevision"));
    if(Stats.LastQueuedRevision!=Stats.LastConsumedRevision)return WaitAdmission(S,TEXT("QueuedConsumedRevisionMismatch"));
    if(FMath::Abs(Stats.ConsumedPhysicalSeconds-Physical)>1.e-6)return WaitAdmission(S,TEXT("ExactCanonicalFrontierMismatch"));
    if(Stats.LastWetCells<=0)return WaitAdmission(S,TEXT("NoWetCells"));
    if(Stats.FailedRhiUploads!=0)return WaitAdmission(S,TEXT("RhiUploadFailure"));
    if(Stats.DrawableBounds.IsEmpty()||Stats.DrawableBounds.Width()>255||Stats.DrawableBounds.Height()>255)
        return WaitAdmission(S,TEXT("DrawableBoundsInvalid"));
    const int32 DrawableCellCount=Stats.DrawableBounds.Width()*Stats.DrawableBounds.Height();
    if(Stats.SurfaceVertices!=DrawableCellCount*9||Stats.SurfaceTriangles!=DrawableCellCount*8)
        return WaitAdmission(S,TEXT("PersistentCentreAndSharedFacePatchTopologyMismatch"));
    if(Stats.LastValidLocalCells!=Stats.ExpectedLocalCells)return WaitAdmission(S,TEXT("LocalCoverageMismatch"));
    if(Stats.LastConnectivityRevision==0||Stats.SubmittedMaskUploads==0)return WaitAdmission(S,TEXT("ConnectivityNotUploaded"));
    if(!Surface||!Surface->IsRegistered()||!Surface->IsVisible()||Surface->GetCollisionEnabled()!=ECollisionEnabled::NoCollision)
        return WaitAdmission(S,TEXT("SurfaceVisibilityOrCollision"));
    if(!Field||!Mask||Field->GetPixelFormat()!=PF_A32B32G32R32F||Mask->GetPixelFormat()!=PF_G8
        ||Field->GetSizeX()!=FieldSize||Field->GetSizeY()!=FieldSize||Mask->GetSizeX()!=FieldSize||Mask->GetSizeY()!=FieldSize
        ||!Field->GetResource()||!Mask->GetResource())return WaitAdmission(S,TEXT("TextureFormatDimensionsOrResource"));
    const FTextureRHIRef FieldRHI=Field->GetResource()->TextureRHI,MaskRHI=Mask->GetResource()->TextureRHI;
    if(!FieldRHI.IsValid()||!MaskRHI.IsValid()||FieldRHI->GetDesc().Format!=PF_A32B32G32R32F||MaskRHI->GetDesc().Format!=PF_G8)return WaitAdmission(S,TEXT("RhiTextureFormatOrResource"));
    auto Row=MakeShared<FJsonObject>();const bool CurrentMaterialReady=MaterialReady(*Actor,Row);
    S.Report->SetObjectField(TEXT("last_material_readiness"),Row);
    if(!CurrentMaterialReady)return WaitAdmission(S,TEXT("OwnCurrentMaterialNotReady"));
    auto Job=MakeShared<FReadbackJob,ESPMode::ThreadSafe>();Job->Expected.SetNumZeroed(FieldSize*FieldSize);Job->Valid.Init(false,FieldSize*FieldSize);
    Job->Stats=Stats;Job->BeganAt=FPlatformTime::Seconds();Job->Receipt=Row;
    int32 Valid=0;
    for(int32 Y=0;Y<Stats.DrawableBounds.Height();++Y)for(int32 X=0;X<Stats.DrawableBounds.Width();++X)
    {
        const FIntPoint Coord=Stats.DrawableBounds.Min+FIntPoint(X,Y);if(!Terrain.IsValidCoord(Coord))continue;
        double Head=0.,Depth=0.;FVector2D Velocity;
        if(!Sim.TryGetPhysicalWaterSample(Coord,Head,Depth,Velocity))return WaitAdmission(S,TEXT("CanonicalSampleMissing"));
        const float BedCm=static_cast<float>(Head-Depth),DepthCm=static_cast<float>(Depth),HeadCm=BedCm+DepthCm;
        const FVector4f Expected(static_cast<float>((static_cast<double>(HeadCm)-Terrain.GridOrigin.Z)*.01),DepthCm*.01f,
            static_cast<float>(Velocity.X)*.01f,static_cast<float>(Velocity.Y)*.01f);
        const int32 At=X+Y*FieldSize;Job->Expected[At]=Expected;Job->Valid[At]=true;++Valid;
    }
    if(Valid!=Stats.ExpectedLocalCells)return WaitAdmission(S,TEXT("ExpectedSnapshotCoverageMismatch"));
    Row->SetStringField(TEXT("label"),Label);Row->SetNumberField(TEXT("canonical_physical_seconds"),Physical);
    Row->SetNumberField(TEXT("consumed_physical_seconds"),Stats.ConsumedPhysicalSeconds);
    Row->SetNumberField(TEXT("physical_age_seconds_at_copy_admission"),Physical-Stats.ConsumedPhysicalSeconds);
    Row->SetNumberField(TEXT("event_physical_age_at_copy"),S.bTriggered?Physical-S.SourcePhysicalSeconds:0.);
    Row->SetNumberField(TEXT("configuration_epoch"),Stats.ConfigurationEpoch);Row->SetNumberField(TEXT("consumed_revision"),Stats.LastConsumedRevision);
    Row->SetNumberField(TEXT("connectivity_revision"),Stats.LastConnectivityRevision);Row->SetNumberField(TEXT("mask_uploads"),Stats.SubmittedMaskUploads);
    Row->SetNumberField(TEXT("pending_packets_at_copy_admission"),Stats.PendingPackets);Row->SetNumberField(TEXT("expected_local_cells"),Stats.ExpectedLocalCells);
    Row->SetNumberField(TEXT("geometry_builds"),Stats.GeometryBuilds);Row->SetNumberField(TEXT("surface_triangles"),Stats.SurfaceTriangles);
    Row->SetNumberField(TEXT("surface_vertices"),Stats.SurfaceVertices);
    Row->SetBoolField(TEXT("persistent_cell_centres_and_face_midpoints"),Stats.SurfaceVertices==DrawableCellCount*9&&Stats.SurfaceTriangles==DrawableCellCount*8);
    Row->SetNumberField(TEXT("drawable_min_x"),Stats.DrawableBounds.Min.X);Row->SetNumberField(TEXT("drawable_min_y"),Stats.DrawableBounds.Min.Y);
    Row->SetNumberField(TEXT("drawable_width"),Stats.DrawableBounds.Width());Row->SetNumberField(TEXT("drawable_height"),Stats.DrawableBounds.Height());
    Row->SetStringField(TEXT("field_format"),TEXT("PF_A32B32G32R32F"));Row->SetStringField(TEXT("connectivity_format"),TEXT("PF_G8"));
    Row->SetBoolField(TEXT("actor_tick_disabled"),!Actor->PrimaryActorTick.bCanEverTick);
    Row->SetObjectField(TEXT("render_metadata"),S.ReadMetadata(W,Label));
    ENQUEUE_RENDER_COMMAND(AftershockHydraulicAuditCopy)([Job,FieldRHI,MaskRHI](FRHICommandListImmediate& Cmd)
    {
        Job->Field.EnqueueCopy(Cmd,FieldRHI.GetReference(),FIntVector::ZeroValue,0,FIntVector(FieldSize,FieldSize,1));
        Job->Mask.EnqueueCopy(Cmd,MaskRHI.GetReference(),FIntVector::ZeroValue,0,FIntVector(FieldSize,FieldSize,1));
        Job->bEnqueued.Store(true);
    });
    // Same GT call: no new world timer/physics publication can enqueue another
    // owner upload between the copy and this same-view scene capture.
    const double GameSecondsBefore=W.GetTimeSeconds();
    S.LastAdmissionWait=TEXT("CaptureSceneInProgress");
    S.Report->SetStringField(TEXT("last_admission_wait"),S.LastAdmissionWait);S.Report->SetStringField(TEXT("last_capture_label"),Label);Write(S);
    const double CaptureBeganAt=FPlatformTime::Seconds();
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_HYDRAULIC_RUNTIME_CAPTURE_BEGIN label=%s physical=%.17g consumedRevision=%llu"),*Label,Physical,Stats.LastConsumedRevision);
    Job->bSceneSaved=S.Capture(W,S.PinnedView,Folder()/(Label+TEXT(".png")),nullptr);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_HYDRAULIC_RUNTIME_CAPTURE_END label=%s saved=%d wallMs=%.3f notGPUTime=1"),*Label,Job->bSceneSaved,(FPlatformTime::Seconds()-CaptureBeganAt)*1000.);
    if(S.bTriggered&&(S.Stage==2||S.Stage==4))
    {
        // Hide only in the temporary capture, never change the real actor.
        const bool Hidden=S.Capture(W,S.PinnedView,Folder()/(Label+TEXT("-without-water.png")),Surface);
        const bool Control=S.Capture(W,S.PinnedView,Folder()/(Label+TEXT("-shown-control.png")),nullptr);
        Row->SetBoolField(TEXT("capture_only_water_occlusion_pair_saved"),Hidden&&Control);
        Job->bSceneSaved&=Hidden&&Control;
    }
    Row->SetBoolField(TEXT("world_clock_unchanged_during_capture_batch"),W.GetTimeSeconds()==GameSecondsBefore);
    Job->bSceneSaved&=W.GetTimeSeconds()==GameSecondsBefore;
    S.LastAdmissionWait=TEXT("ReadbackInFlight");S.Job=Job;return true;
}
bool PollReadback(FAuditState& S)
{
    if(!S.Job)return true;auto Job=S.Job;
    if(!Job->bEnqueued.Load()||(!Job->bMapped.Load()&&(!Job->Field.IsReady()||!Job->Mask.IsReady())))
    {if(FPlatformTime::Seconds()-Job->BeganAt>15.){S.Failure=TEXT("GPU texture readback exceeded 15 seconds");Finish(S,false);}return false;}
    if(!Job->bMapQueued)
    {
        Job->bMapQueued=true;
        ENQUEUE_RENDER_COMMAND(AftershockHydraulicAuditMap)([Job](FRHICommandListImmediate&)
        {
            int32 FP=0,FH=0,MP=0,MH=0;
            const auto* Pixels=static_cast<const FVector4f*>(Job->Field.Lock(FP,&FH));
            const auto* Masks=static_cast<const uint8*>(Job->Mask.Lock(MP,&MH));
            Job->bMapValid=Pixels&&Masks&&FP>=FieldSize&&MP>=FieldSize&&FH>=FieldSize&&MH>=FieldSize;
            if(Job->bMapValid)
            {
                Job->Actual.SetNumUninitialized(FieldSize*FieldSize);Job->ActualMasks.SetNumUninitialized(FieldSize*FieldSize);
                for(int32 Y=0;Y<FieldSize;++Y)
                {
                    FMemory::Memcpy(Job->Actual.GetData()+Y*FieldSize,Pixels+Y*FP,FieldSize*sizeof(FVector4f));
                    FMemory::Memcpy(Job->ActualMasks.GetData()+Y*FieldSize,Masks+Y*MP,FieldSize);
                }
            }
            if(Pixels)Job->Field.Unlock();if(Masks)Job->Mask.Unlock();
            Job->bMapped.Store(true); // Publish immutable copied POD to GT.
        });
        return false;
    }
    if(!Job->bMapped.Load())
    {if(FPlatformTime::Seconds()-Job->BeganAt>15.){S.Failure=TEXT("GPU staging map exceeded 15 seconds");Finish(S,false);}return false;}
    const auto* Pixels=Job->Actual.GetData();const auto* Masks=Job->ActualMasks.GetData();
    S.bAnyGPUReadback|=Job->bMapValid;
    bool Valid=Job->bMapValid;
    double MaxError=0.;int32 Compared=0,Wet=0,Flow=0,OpenMasks=0,BlockedMasks=0,IllegalMasks=0,Asymmetric=0;
    uint32 CRC=0;
    if(Valid)for(int32 Y=0;Y<Job->Stats.DrawableBounds.Height();++Y)for(int32 X=0;X<Job->Stats.DrawableBounds.Width();++X)
    {
        const int32 At=X+Y*FieldSize;if(!Job->Valid[At])continue;
        const auto Actual=Pixels[At];const auto Expected=Job->Expected[At];const uint8 Mask=Masks[At];
        CRC=FCrc::MemCrc32(&Actual,sizeof(Actual),CRC);CRC=FCrc::MemCrc32(&Mask,sizeof(Mask),CRC);
        for(int32 C=0;C<4;++C){const double Error=FMath::Abs(static_cast<double>(Actual[C])-Expected[C]);MaxError=FMath::Max(MaxError,Error);
            Valid&=FMath::IsFinite(Actual[C])&&Error<=FMath::Max(2.e-4,FMath::Abs(static_cast<double>(Expected[C]))*2.e-6);}
        Valid&=Actual.Y>=0.f;Wet+=Actual.Y>.002f;Flow+=FVector2f(Actual.Z,Actual.W).Size()>1.e-5f;
        OpenMasks+=Mask!=15;BlockedMasks+=Mask!=0;IllegalMasks+=(Mask&0xf0)!=0;++Compared;
        if(X+1<Job->Stats.DrawableBounds.Width()&&Job->Valid[At+1])Asymmetric+=((Mask&2)!=0)!=((Masks[At+1]&8)!=0);
        if(Y+1<Job->Stats.DrawableBounds.Height()&&Job->Valid[At+FieldSize])Asymmetric+=((Mask&4)!=0)!=((Masks[At+FieldSize]&1)!=0);
    }
    Valid&=Compared==Job->Stats.ExpectedLocalCells&&Wet>0&&OpenMasks>0&&BlockedMasks>0&&IllegalMasks==0&&Asymmetric==0&&Job->bSceneSaved;
    auto Row=Job->Receipt;Row->SetBoolField(TEXT("gpu_readback_valid"),Valid);Row->SetNumberField(TEXT("gpu_compared_cells"),Compared);
    Row->SetNumberField(TEXT("gpu_max_channel_abs_error"),MaxError);Row->SetNumberField(TEXT("gpu_wet_cells"),Wet);
    Row->SetNumberField(TEXT("gpu_flow_cells"),Flow);Row->SetNumberField(TEXT("gpu_open_mask_cells"),OpenMasks);
    Row->SetNumberField(TEXT("gpu_blocked_mask_cells"),BlockedMasks);Row->SetNumberField(TEXT("gpu_illegal_masks"),IllegalMasks);
    Row->SetNumberField(TEXT("gpu_asymmetric_shared_faces"),Asymmetric);Row->SetNumberField(TEXT("gpu_field_mask_crc32"),CRC);
    Row->SetBoolField(TEXT("same_camera_scene_saved"),Job->bSceneSaved);
    S.bSawFlow|=Flow>0;if(S.Frames.IsEmpty())S.FirstCRC=CRC;else S.bSawFieldChange|=CRC!=S.FirstCRC;
    S.Frames.Add(MakeShared<FJsonValueObject>(Row));S.Job.Reset();Write(S);
    if(!Valid){S.Failure=TEXT("Actual field/mask pixels, source correlation, material or scene acceptance failed");Finish(S,false);return false;}
    if(!S.bBaseline)S.bBaseline=true;else ++S.Stage;return true;
}
}
#endif

void BeginAftershockHydraulicRuntimeAudit(UWorld& World,FAftershockHydraulicSceneCapture Capture,FAftershockHydraulicMetadataRead ReadMetadata)
{
#if !UE_BUILD_SHIPPING && WITH_EDITOR
    using namespace AftershockHydraulicRuntimeAuditPrivate;
    if(!FParse::Param(FCommandLine::Get(),TEXT("AftershockHydraulicRuntimeAudit")))return;
    auto State=MakeShared<FAuditState>();State->World=&World;State->Capture=MoveTemp(Capture);State->ReadMetadata=MoveTemp(ReadMetadata);State->BeganAt=FPlatformTime::Seconds();
    FParse::Value(FCommandLine::Get(),TEXT("AftershockHydraulicRuntimeDeadline="),State->WallDeadlineSeconds);
    if(!FMath::IsFinite(State->WallDeadlineSeconds)||State->WallDeadlineSeconds<300.||State->WallDeadlineSeconds>600.)
        State->WallDeadlineSeconds=300.;
    State->Report->SetNumberField(TEXT("schema_version"),1);State->Report->SetBoolField(TEXT("valid"),false);State->Report->SetBoolField(TEXT("in_progress"),true);
    State->Report->SetStringField(TEXT("run_id"),FGuid::NewGuid().ToString());State->Report->SetStringField(TEXT("started_utc"),FDateTime::UtcNow().ToIso8601());
    State->Report->SetStringField(TEXT("scope"),TEXT("Real configured V3 game, actual persistent Model2 owner, GPU float-field/mask correlation and same-camera scene captures. No source shortening, forced water, fake world, time dilation, solver steps or gameplay writes except one production flood request. Separate native conservation fixtures remain required."));
    State->Report->SetNumberField(TEXT("wall_deadline_seconds"),State->WallDeadlineSeconds);State->Report->SetBoolField(TEXT("source_shortened"),false);Write(*State);
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([State](float)
    {
        auto& S=*State;if(S.bFinished)return false;
        UWorld* W=S.World.Get();const double Wall=FPlatformTime::Seconds()-S.BeganAt;
        if(!Active(W)||GUsingNullRHI||!GDynamicRHI||FString(GDynamicRHI->GetName())!=TEXT("D3D12"))
        {S.Failure=TEXT("A live non-commandlet D3D12 game world is required");Finish(S,false);return false;}
        AdmissionCheckpoint(S,*W);
        if(Wall>=S.WallDeadlineSeconds){S.Failure=FString::Printf(TEXT("%.0f wall-second deadline: production source/tail/captures incomplete, no time was forged"),S.WallDeadlineSeconds);Finish(S,false);return false;}
        if(S.Job){PollReadback(S);return !S.bFinished;}
        auto* Terrain=W->GetSubsystem<UTerrainGridSubsystem>();auto* Sim=W->GetSubsystem<UDisasterSimulationSubsystem>();auto* Director=W->GetSubsystem<UAftershockDisasterDirectorSubsystem>();
        auto* PC=W->GetFirstPlayerController();if(!Terrain||!Sim||!Director||!PC||!PC->PlayerCameraManager)return true;
        if(Terrain->ContinuationGeneratorVersion!=3){S.Failure=TEXT("Actual canonical terrain is not V3");Finish(S,false);return false;}
        if(!Sim->IsHydraulicModel2Active()||Sim->GetPublishedHydraulicSeconds()<=0.){S.LastAdmissionWait=TEXT("AwaitPublishedModel2");return true;}
        if(!S.bPinned)
        {
            S.PinnedView=PC->PlayerCameraManager->GetCameraCacheView();FVector Focus=Terrain->GridToWorld(FIntPoint(Terrain->GridWidth/2,Terrain->GridHeight/2),true);
            double Best=MAX_dbl;for(const auto& Cell:Terrain->Cells)if(Cell.bIsRiverbed&&!Cell.bIsOcean)
            {const FVector Point=Terrain->GridToWorld(Cell.Coords,true);const double Distance=FVector::DistSquared2D(Point,Focus);if(Distance<Best){Best=Distance;S.PinnedView.Location=Point+FVector(-18000,-18000,24000);S.PinnedView.Rotation=(Point-S.PinnedView.Location).Rotation();}}
            if(Best==MAX_dbl){S.Failure=TEXT("Configured flood region has no real canonical river");Finish(S,false);return false;}
            S.PinnedView.FOV=55.f;S.bPinned=true;
        }
        if(!S.bBaseline){QueueFrame(S,*W,*Sim,*Terrain,TEXT("hydraulic-before"));return true;}
        if(!S.bTriggered)
        {
            FText Error;if(!S.bWarningRequested){if(!Sim->CanTriggerDisaster(EDisasterType::Flood,Error))return true;S.bWarningRequested=Director->RequestWarning(EDisasterType::Flood,30.f,Error);}
            if(S.bWarningRequested){++S.TriggerAttempts;S.bTriggered=Director->TriggerPendingNow(Error);}
            S.TriggerFailure=Error.ToString();if(!S.bTriggered)return true;
            const auto& Episode=Sim->GetPhysicalEpisodeState();S.EventID=Episode.EventID;S.SourcePhysicalSeconds=Episode.StartPhysicalSeconds;S.SourceDuration=Episode.SourceDurationSeconds;S.SimulationScale=Episode.SimulationTimeScale;
            if(!Episode.bActive||Episode.Type!=EDisasterType::Flood||!S.EventID.IsValid()||S.SourceDuration!=3600.||S.SimulationScale!=20.)
            {S.Failure=TEXT("Production flood was not the unchanged 3600physical/20scale=180game-second source");Finish(S,false);return false;}
            S.Report->SetStringField(TEXT("event_id"),S.EventID.ToString());S.Report->SetNumberField(TEXT("source_physical_seconds"),S.SourceDuration);S.Report->SetNumberField(TEXT("production_simulation_time_scale"),S.SimulationScale);Write(S);return true;
        }
        const auto& Episode=Sim->GetPhysicalEpisodeState();const double Age=Sim->GetPublishedHydraulicSeconds()-S.SourcePhysicalSeconds;
        if(Episode.EventID!=S.EventID){S.Failure=TEXT("Canonical event identity changed during observation");Finish(S,false);return false;}
        const double Targets[]={10.,60.,180.,S.SourceDuration,S.SourceDuration+120.};
        if(S.Stage<5&&Age>=Targets[S.Stage])
        {
            if(S.Stage==4&&(!Episode.bOutcomeCommitted||Episode.bActive))return true;
            QueueFrame(S,*W,*Sim,*Terrain,FString::Printf(TEXT("hydraulic-stage-%d"),S.Stage));return true;
        }
        if(S.Stage==5)
        {
            const auto& Final=Director->GetState();const bool Completed=Episode.bOutcomeCommitted&&!Episode.bActive
                &&Final.LastReport.DisasterEventID==S.EventID&&!Final.LastReport.bIsPending&&!Final.LastReport.bWasRejected
                &&Final.EventSequence==Final.LastAppliedEventSequence;
            const bool Passed=Completed&&S.Frames.Num()==6&&S.bSawFlow&&S.bSawFieldChange;
            S.Report->SetBoolField(TEXT("production_source_and_tail_completed"),Completed);
            if(!Passed)S.Failure=TEXT("Full production event or actual GPU evolution acceptance incomplete");Finish(S,Passed);return false;
        }
        return true;
    }),.05f);
#endif
}
