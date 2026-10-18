#include "AftershockFireRuntimeAudit.h"

#if !UE_BUILD_SHIPPING && WITH_EDITOR
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/AftershockDisasterDirectorSubsystem.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/AftershockStartupPreparationSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/DisasterPresentationSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Game/AftershockPlayerController.h"
#include "AftershockPresentationBudget.h"
#include "AftershockTerrainSurface.h"
#include "Camera/CameraTypes.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialShared.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "RHIGlobals.h"
#include "Scalability.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StaticMeshResources.h"
#include "UnrealEngine.h"
#include "AftershockStartupMaterialPreparation.h"

namespace AftershockFireRuntimeAuditPrivate
{
constexpr double WallDeadlineSeconds=240.;
const TCHAR* MaterialPaths[]={
    TEXT("/Game/FX/Aftershock/Fire/M_FireFlameVolume.M_FireFlameVolume"),
    TEXT("/Game/FX/Aftershock/Fire/M_FireCoreVolume.M_FireCoreVolume"),
    TEXT("/Game/FX/Aftershock/Fire/M_FireSmokeVolume.M_FireSmokeVolume"),
    TEXT("/Game/FX/Aftershock/Fire/M_FireEmber.M_FireEmber")};
struct FFireLiveState
{
    TWeakObjectPtr<UWorld> World;
    FAftershockFireSceneCapture Capture;
    FAftershockFireMetadataRead ReadMetadata;
    TSharedRef<FJsonObject> Report=MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Frames;
    FMinimalViewInfo PinnedView;
    FIntPoint FramingCell=FIntPoint(INDEX_NONE,INDEX_NONE);
    FIntPoint SourceCell=FIntPoint(INDEX_NONE,INDEX_NONE);
    FVector SourceWorld=FVector::ZeroVector;
    FString Failure,WaitReason=TEXT("Startup"),TriggerError;
    double BeganAt=0.,FramedAt=0.,TriggerCpuMilliseconds=0.,LastCheckpoint=-1.e30;
    int32 Revision=0,Stage=0,TriggerCalls=0;
    bool bFramed=false,bPinned=false,bTriggered=false,bFinished=false,bWarningRequested=false;
};
FString Folder(){return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/FireRuntime"));}
bool Write(FFireLiveState& State)
{
    State.Report->SetArrayField(TEXT("frames"),State.Frames);State.Report->SetStringField(TEXT("failure"),State.Failure);
    State.Report->SetStringField(TEXT("last_wait_reason"),State.WaitReason);State.Report->SetStringField(TEXT("last_trigger_error"),State.TriggerError);
    FString JSON;if(!FJsonSerializer::Serialize(State.Report,TJsonWriterFactory<>::Create(&JSON)))return false;
    IFileManager::Get().MakeDirectory(*Folder(),true);
    return FFileHelper::SaveStringToFile(JSON,*(Folder()/TEXT("fire_runtime_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
bool Active(UWorld* World)
{
    if(!IsValid(World)||World->bIsTearingDown||!World->IsGameWorld()||!GEngine||IsRunningCommandlet())return false;
    const auto* Context=GEngine->GetWorldContextFromWorld(World);return Context&&Context->World()==World;
}
void Finish(FFireLiveState& State,bool Passed)
{
    if(State.bFinished)return;State.bFinished=true;
    State.Report->SetBoolField(TEXT("valid"),Passed);State.Report->SetBoolField(TEXT("in_progress"),false);
    State.Report->SetNumberField(TEXT("wall_seconds"),FPlatformTime::Seconds()-State.BeganAt);
    State.Report->SetNumberField(TEXT("production_trigger_calls"),State.TriggerCalls);
    State.Report->SetNumberField(TEXT("production_trigger_inclusive_game_thread_ms"),State.TriggerCpuMilliseconds);
    State.Report->SetStringField(TEXT("trigger_cpu_scope"),TEXT("Actual Director TriggerPendingNow inclusive canonical TriggerDisaster and synchronous presentation/gameplay subscribers. Excludes this audit's captures. Not isolated solver time or GPU time."));
    const bool Saved=Write(State);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_FIRE_RUNTIME_RESULT valid=%d saved=%d frames=%d triggerCalls=%d triggerInclusiveGameThreadMs=%.6f failure=%s"),Passed,Saved,State.Frames.Num(),State.TriggerCalls,State.TriggerCpuMilliseconds,*State.Failure);
    FPlatformMisc::RequestExit(false);
}
void CpuStats(TSharedRef<FJsonObject> Row,const TCHAR* Field,const FAftershockPresentationCpuStats& Stats)
{
    auto Value=MakeShared<FJsonObject>();Value->SetNumberField(TEXT("updates"),Stats.Updates);
    Value->SetNumberField(TEXT("total_game_thread_ms"),Stats.TotalMilliseconds);Value->SetNumberField(TEXT("peak_game_thread_ms"),Stats.PeakMilliseconds);
    Value->SetNumberField(TEXT("average_game_thread_ms"),Stats.Updates?Stats.TotalMilliseconds/Stats.Updates:0.);
    Value->SetStringField(TEXT("scope"),TEXT("Actual production animation batch transforms/material submission only; excludes GPU execution, frame rendering and audit scene readback."));
    Row->SetObjectField(Field,Value);
}
bool ExactMaterialReady(UMaterialInterface* Material,const TCHAR* Expected,UWorld& World,TSharedRef<FJsonObject> Row)
{
    const auto* MID=Cast<UMaterialInstanceDynamic>(Material);
    const auto* Parent=MID?MID->Parent.Get():Material;
    const bool Exact=Parent&&Parent->GetPathName()==Expected;
    TSharedPtr<FJsonObject> Details;FString Reason;
    const bool Ready=Exact&&IsAftershockActiveOwnedMaterialReady(Material,World,&Reason,&Details);
    Row->SetBoolField(TEXT("exact_parent"),Exact);Row->SetBoolField(TEXT("own_current_resource_ready"),Ready);
    Row->SetStringField(TEXT("expected_parent"),Expected);Row->SetStringField(TEXT("actual_parent"),GetPathNameSafe(Parent));
    Row->SetStringField(TEXT("wait_reason"),Reason);
    if(Details)Row->SetObjectField(TEXT("actual_owned_resource"),Details);
    return Ready;
}
template<typename ActorType>
ActorType* FindDisplay(UWorld& World)
{
    for(TActorIterator<ActorType> It(&World);It;++It)if(!It->IsHidden())return *It;
    return nullptr;
}
bool FireSourcesInFrame(APlayerController& PC,const FAftershockFireVisualState& State,int32& InFrame)
{
    InFrame=0;int32 Width=0,Height=0;PC.GetViewportSize(Width,Height);
    for(const auto& Source:State.Sources)
    {
        FVector2D Pixel;
        if(UGameplayStatics::ProjectWorldToScreen(&PC,Source.Anchor+FVector(0,0,Source.HeightCm*.5f),Pixel,false)
            &&Pixel.X>=0.&&Pixel.Y>=0.&&Pixel.X<Width&&Pixel.Y<Height)++InFrame;
    }
    return Width==1600&&Height==900&&InFrame>0;
}
template<typename ActorType>
bool ReadDisplay(ActorType* Actor,UWorld& World,const UTerrainGridSubsystem& Terrain,TSharedRef<FJsonObject> Row,
    UInstancedStaticMeshComponent*& OutOuter,UInstancedStaticMeshComponent*& OutSmoke)
{
    if(!Actor){Row->SetBoolField(TEXT("actual_fire_display_ready"),false);return false;}
    const auto& State=Actor->GetFireVisualStateForAudit();const auto Budget=GetAftershockPresentationBudget(&World);
    const int32 Quality=FMath::Clamp(Scalability::GetQualityLevels().EffectsQuality,0,3);
    const int32 TierLimit=Budget.Tier==EAftershockPresentationTier::Globe?0:Budget.Tier==EAftershockPresentationTier::Regional?32:Budget.Tier==EAftershockPresentationTier::City?128:192;
    const int32 Limit=FMath::Min(TierLimit,48+Quality*48);
    const bool Bounded=State.Sources.Num()>0&&State.Sources.Num()<=Limit;
    bool Canonical=Bounded;for(const auto& Source:State.Sources){FGridCell Cell;Canonical&=Terrain.TryGetCell(Source.Cell,Cell)&&Cell.bBurning&&Cell.bIsPlayable&&!Cell.bIsOcean;}
    bool Ready=Bounded&&Canonical&&State.AuthoredMaterials==4;TArray<TSharedPtr<FJsonValue>> Layers;
    const int32 PerSource[]={2,1,3,2};
    for(int32 Layer=0;Layer<4;++Layer)
    {
        auto Value=MakeShared<FJsonObject>();auto* Batch=Actor->GetFireBatchForAudit(Layer);const auto* Mesh=Batch?Batch->GetStaticMesh().Get():nullptr;
        const auto* Data=Mesh?Mesh->GetRenderData():nullptr;const FVector Size=Mesh?Mesh->GetBoundingBox().GetSize():FVector::ZeroVector;
        const int32 Triangles=Data&&!Data->LODResources.IsEmpty()?Data->LODResources[0].GetNumTriangles():0;
        const bool NonPlanar=Size.GetMin()>1.f&&Triangles>24;
        const bool BuffersReady=Data&&!Data->LODResources.IsEmpty()
            &&Data->LODResources[0].VertexBuffers.PositionVertexBuffer.GetRHI().IsValid()
            &&Data->LODResources[0].IndexBuffer.GetRHI().IsValid();
        const bool Material=ExactMaterialReady(Batch?Batch->GetMaterial(0):nullptr,MaterialPaths[Layer],World,Value);
        int32 VisibleTransforms=0;bool Finite=true;
        if(Batch)for(int32 Index=0;Index<Batch->GetInstanceCount();++Index)
        {
            FTransform Transform;if(!Batch->GetInstanceTransform(Index,Transform,true)||Transform.ContainsNaN()){Finite=false;continue;}
            VisibleTransforms+=Transform.GetScale3D().GetMin()>.001;
        }
        const bool RequiredVisible=Layer!=3||Budget.Tier==EAftershockPresentationTier::Street;
        const bool Valid=Batch&&Batch->IsRegistered()&&Batch->GetSceneProxy()!=nullptr&&Mesh&&NonPlanar&&BuffersReady&&Material&&Finite
            &&Batch->GetInstanceCount()==State.Sources.Num()*PerSource[Layer]
            &&(!RequiredVisible||(Batch->IsVisible()&&VisibleTransforms>0));
        Ready&=Valid;Value->SetNumberField(TEXT("layer"),Layer);Value->SetBoolField(TEXT("valid"),Valid);
        Value->SetBoolField(TEXT("mesh_is_non_planar"),NonPlanar);Value->SetNumberField(TEXT("lod0_triangles"),Triangles);
        Value->SetBoolField(TEXT("actual_vertex_and_index_rhi_buffers_ready"),BuffersReady);Value->SetBoolField(TEXT("actual_scene_proxy_present"),Batch&&Batch->GetSceneProxy()!=nullptr);
        Value->SetNumberField(TEXT("instances"),Batch?Batch->GetInstanceCount():0);Value->SetNumberField(TEXT("nonzero_instance_transforms"),VisibleTransforms);
        Value->SetBoolField(TEXT("component_visible"),Batch&&Batch->IsVisible());Value->SetBoolField(TEXT("transforms_finite"),Finite);
        Layers.Add(MakeShared<FJsonValueObject>(Value));if(Layer==0)OutOuter=Batch;if(Layer==2)OutSmoke=Batch;
    }
    int32 InFrame=0;const bool Framed=World.GetFirstPlayerController()&&FireSourcesInFrame(*World.GetFirstPlayerController(),State,InFrame);
    Row->SetBoolField(TEXT("actual_fire_display_ready"),Ready);Row->SetBoolField(TEXT("all_display_sources_are_canonical_burning_cells"),Canonical);
    Row->SetBoolField(TEXT("actual_nonzero_fire_source_in_frame"),Framed);Row->SetNumberField(TEXT("sources_in_frame"),InFrame);
    Row->SetNumberField(TEXT("source_count"),State.Sources.Num());Row->SetNumberField(TEXT("source_limit"),Limit);
    Row->SetNumberField(TEXT("building_sources"),State.BuildingSources);Row->SetNumberField(TEXT("effects_quality"),Quality);
    Row->SetNumberField(TEXT("presentation_tier"),static_cast<int32>(Budget.Tier));Row->SetArrayField(TEXT("layers"),Layers);
    return Ready&&Framed;
}
bool CaptureFrame(FFireLiveState& State,UWorld& World,const UTerrainGridSubsystem& Terrain,const UDisasterSimulationSubsystem& Sim,const FString& Label)
{
    auto* PC=World.GetFirstPlayerController();if(!PC||!PC->PlayerCameraManager)return false;
    auto Row=State.ReadMetadata(World,Label);const auto View=PC->PlayerCameraManager->GetCameraCacheView();
    const auto& Snapshot=Sim.GetLatestVisualSnapshot();const double Age=State.bTriggered?World.GetTimeSeconds()-Snapshot.StartedAtWorldSeconds:0.;
    const bool SameCamera=View.Location.Equals(State.PinnedView.Location,1.f)&&View.Rotation.Equals(State.PinnedView.Rotation,.05f)&&FMath::IsNearlyEqual(View.FOV,State.PinnedView.FOV,.01f);
    Row->SetStringField(TEXT("label"),Label);Row->SetNumberField(TEXT("event_age_seconds"),Age);Row->SetNumberField(TEXT("snapshot_revision"),Snapshot.Revision);
    Row->SetBoolField(TEXT("same_pinned_camera"),SameCamera);Row->SetNumberField(TEXT("world_seconds"),World.GetTimeSeconds());Row->SetNumberField(TEXT("frame_counter"),GFrameCounter);
    Row->SetNumberField(TEXT("region_index"),Terrain.GeographyIndex);Row->SetNumberField(TEXT("focus_index"),Terrain.GeographyFocusIndex);Row->SetNumberField(TEXT("source_version"),Terrain.ContinuationGeneratorVersion);
    Row->SetBoolField(TEXT("actual_model2_active"),Sim.IsHydraulicModel2Active());Row->SetNumberField(TEXT("actual_published_hydraulic_seconds"),Sim.GetPublishedHydraulicSeconds());
    Row->SetBoolField(TEXT("fixture_set_time_dilation"),false);Row->SetBoolField(TEXT("fixture_mutated_fire_or_terrain_cells"),false);
    Row->SetBoolField(TEXT("fixture_advanced_or_shortened_event"),false);
    bool Valid=SameCamera&&Terrain.ContinuationGeneratorVersion==3&&Sim.IsHydraulicModel2Active();
    UInstancedStaticMeshComponent* Outer=nullptr;UInstancedStaticMeshComponent* Smoke=nullptr;
    auto* Event=FindDisplay<AAftershockDisasterPresentationActor>(World);auto* Persistent=FindDisplay<AAftershockEcologyStateActor>(World);
    if(State.bTriggered)
    {
        const bool SameEvent=Snapshot.Revision==State.Revision&&Snapshot.Type==EDisasterType::Fire&&FMath::IsNearlyEqual(Snapshot.DurationSeconds,24.f);
        Row->SetBoolField(TEXT("same_unchanged_24_second_fire_event"),SameEvent);Valid&=SameEvent;
        if(State.Stage==3)
        {
            Valid&=Age>=26.f&&ReadDisplay(Persistent,World,Terrain,Row,Outer,Smoke);
            bool EventReleased=true;
            for(TActorIterator<AAftershockDisasterPresentationActor> It(&World);It;++It)
            {
                EventReleased&=It->IsHidden()&&It->GetFireVisualStateForAudit().Sources.IsEmpty();
                for(int32 Layer=0;Layer<4;++Layer)if(const auto* Batch=It->GetFireBatchForAudit(Layer))EventReleased&=Batch->GetInstanceCount()==0;
            }
            Row->SetBoolField(TEXT("event_flame_smoke_instances_released_after_handoff"),EventReleased);Valid&=EventReleased;
            CpuStats(Row,TEXT("persistent_animation_cpu"),Persistent?Persistent->GetFireAnimationCpuStatsForAudit():FAftershockPresentationCpuStats());
            Row->SetBoolField(TEXT("persistent_handoff"),true);
        }
        else {Valid&=ReadDisplay(Event,World,Terrain,Row,Outer,Smoke);Row->SetBoolField(TEXT("persistent_handoff"),false);}
    }
    else
    {
        bool NoFire=true;for(const auto& Cell:Terrain.Cells)NoFire&=!Cell.bBurning;
        Row->SetBoolField(TEXT("canonical_before_fire"),NoFire);Valid&=NoFire;
    }
    if(const auto* Presentation=World.GetSubsystem<UDisasterPresentationSubsystem>())
    {
        CpuStats(Row,TEXT("active_animation_cpu"),Presentation->GetAnimationCpuStatsForAudit());
        Row->SetNumberField(TEXT("actual_persistent_handoff_game_thread_ms"),Presentation->GetLastHandoffCpuMillisecondsForAudit());
        Row->SetStringField(TEXT("handoff_cpu_scope"),TEXT("Actual event-completion clear and UpdatePersistentEcology callback. If the periodic ecology observer already built the persistent meshes, that earlier rebuild cost is outside this callback measurement."));
        Row->SetStringField(TEXT("presentation_state"),Presentation->DescribeCurrentVisuals());
    }
    if(!Valid){State.Report->SetObjectField(TEXT("last_frame_admission"),Row);State.WaitReason=TEXT("FireDisplayOwnMaterialGeometrySourceOrCameraNotReady");return false;}
    const FString Path=Folder()/(Label+TEXT("-scene.png"));const double CaptureStart=FPlatformTime::Seconds();
    const bool Saved=State.Capture(World,State.PinnedView,Path,nullptr);
    Row->SetBoolField(TEXT("scene_saved"),Saved);Row->SetNumberField(TEXT("scene_capture_readback_cpu_ms"),(FPlatformTime::Seconds()-CaptureStart)*1000.);
    if(State.Stage==2&&State.bTriggered)
    {
        const uint64 PairFrame=GFrameCounter;const double PairWorld=World.GetTimeSeconds();
        const bool FlameHidden=Outer&&State.Capture(World,State.PinnedView,Folder()/TEXT("fire-live-active-6-hidden-outer-scene.png"),Outer);
        const bool SmokeHidden=Smoke&&State.Capture(World,State.PinnedView,Folder()/TEXT("fire-live-active-6-hidden-smoke-scene.png"),Smoke);
        const bool SameMoment=PairFrame==GFrameCounter&&PairWorld==World.GetTimeSeconds();
        Row->SetBoolField(TEXT("matched_outer_hidden_capture_saved"),FlameHidden);Row->SetBoolField(TEXT("matched_smoke_hidden_capture_saved"),SmokeHidden);
        Row->SetBoolField(TEXT("diagnostic_pairs_same_frame_and_world_time"),SameMoment);
        Row->SetStringField(TEXT("diagnostic_hiding_scope"),TEXT("Single-component SceneCapture hidden filter only. Production component visibility and gameplay are unchanged."));
        Valid&=FlameHidden&&SmokeHidden&&SameMoment;
    }
    Row->SetBoolField(TEXT("valid"),Valid&&Saved);State.Frames.Add(MakeShared<FJsonValueObject>(Row));Write(State);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_FIRE_RUNTIME_FRAME label=%s valid=%d age=%.3f revision=%d sameCamera=%d model2=%d"),*Label,Valid&&Saved,Age,Snapshot.Revision,SameCamera,Sim.IsHydraulicModel2Active());
    if(!Valid||!Saved){State.Failure=TEXT("Real fire frame or matched component-hidden diagnostic failed");Finish(State,false);return false;}
    ++State.Stage;return true;
}
}
#endif

void BeginAftershockFireRuntimeAudit(UWorld& World,FAftershockFireSceneCapture Capture,FAftershockFireMetadataRead ReadMetadata)
{
#if !UE_BUILD_SHIPPING && WITH_EDITOR
    using namespace AftershockFireRuntimeAuditPrivate;
    if(!FParse::Param(FCommandLine::Get(),TEXT("AftershockFireRuntimeAudit")))return;
    auto State=MakeShared<FFireLiveState>();State->World=&World;State->Capture=MoveTemp(Capture);State->ReadMetadata=MoveTemp(ReadMetadata);State->BeganAt=FPlatformTime::Seconds();
    State->Report->SetNumberField(TEXT("schema_version"),1);State->Report->SetBoolField(TEXT("valid"),false);State->Report->SetBoolField(TEXT("in_progress"),true);
    State->Report->SetStringField(TEXT("started_utc"),FDateTime::UtcNow().ToIso8601());State->Report->SetStringField(TEXT("run_id"),FGuid::NewGuid().ToString());
    State->Report->SetNumberField(TEXT("wall_deadline_seconds"),WallDeadlineSeconds);
    State->Report->SetStringField(TEXT("scope"),TEXT("Real configured V3 game with the actual shared startup and published Model2 owner; one normal director Fire request, four fixed-camera scene frames, current owned fire shaders, 3D geometry, canonical burning sources, and actual >24-second persistence. Extra matched captures prove rendered component contribution separately. No forced fire field, altered casualty result, source shortening, solver tick, time dilation, GPU timing or combustion/fluid accuracy claim."));
    State->Report->SetBoolField(TEXT("fixture_mutated_fire_or_terrain_cells"),false);State->Report->SetBoolField(TEXT("gpu_timing_measured"),false);
    Write(*State);
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([State](float)
    {
        auto& S=*State;if(S.bFinished)return false;UWorld* W=S.World.Get();const double Wall=FPlatformTime::Seconds()-S.BeganAt;
        if(!Active(W)||GUsingNullRHI||!GDynamicRHI||FString(GDynamicRHI->GetName())!=TEXT("D3D12"))
        {S.Failure=TEXT("Actual non-commandlet D3D12 game world is required");Finish(S,false);return false;}
        if(Wall>=WallDeadlineSeconds){S.Failure=TEXT("240 wall-second deadline; startup, production fire, materials or real frames incomplete");Finish(S,false);return false;}
        if(Wall-S.LastCheckpoint>=3.){S.LastCheckpoint=Wall;Write(S);UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_FIRE_RUNTIME_PROGRESS wall=%.2f world=%.3f stage=%d wait=%s"),Wall,W->GetTimeSeconds(),S.Stage,*S.WaitReason);}
        auto* GI=W->GetGameInstance();auto* Startup=GI?GI->GetSubsystem<UAftershockStartupPreparationSubsystem>():nullptr;
        auto* Session=GI?GI->GetSubsystem<UAftershockOpeningSessionSubsystem>():nullptr;
        auto* Scenario=W->GetSubsystem<UAftershockScenarioSubsystem>();auto* Terrain=W->GetSubsystem<UTerrainGridSubsystem>();
        auto* Sim=W->GetSubsystem<UDisasterSimulationSubsystem>();auto* Director=W->GetSubsystem<UAftershockDisasterDirectorSubsystem>();
        auto* Life=W->GetSubsystem<UCityLifeSubsystem>();auto* PC=Cast<AAftershockPlayerController>(W->GetFirstPlayerController());
        auto* Camera=PC?Cast<AAftershockCameraPawn>(PC->GetPawn()):nullptr;
        if(!Startup||!Session||!Scenario||!Terrain||!Sim||!Director||!Life||!PC||!Camera||!PC->PlayerCameraManager){S.WaitReason=TEXT("ProductionOwners");return true;}
        if(!UAftershockStartupPreparationSubsystem::IsPreparationEnabled()||!Startup->GetView().bReady||Startup->IsGameplayBlocked(W)||!Startup->HasCompletedBranding())
        {
            if(Startup->GetView().bFailed){S.Failure=TEXT("Real startup preparation failed: ")+Startup->GetView().Error.ToString();Finish(S,false);return false;}
            S.WaitReason=TEXT("RealStartupPreparation");return true;
        }
        if(!Session->HasLiveSession()||!Session->HasConfiguredSession()||!Scenario->IsPrepared()||!Life->IsCityInitialized()||W->IsPaused()||PC->IsOpeningMenuVisible()||PC->IsNarrativeFlowActive())
        {S.WaitReason=TEXT("ConfiguredPlayingSession");return true;}
        if(Terrain->ContinuationGeneratorVersion!=3){S.Failure=TEXT("Real configured V3 geography is required");Finish(S,false);return false;}
        if(!Sim->IsHydraulicModel2Active()||Sim->GetPublishedHydraulicSeconds()<=0.){S.WaitReason=TEXT("ActualPublishedModel2");return true;}
        S.Report->SetBoolField(TEXT("real_startup_preparation_completed"),true);S.Report->SetBoolField(TEXT("actual_model2_active_before_fire"),true);
        S.Report->SetNumberField(TEXT("startup_resolved_assets"),Startup->GetView().ResolvedAssets);S.Report->SetNumberField(TEXT("startup_requested_assets"),Startup->GetView().RequestedAssets);
        TArray<TSharedPtr<FJsonValue>> MaterialReadiness;bool MaterialsReady=true;
        for(const TCHAR* Path:MaterialPaths){auto Row=MakeShared<FJsonObject>();auto* Material=LoadObject<UMaterialInterface>(nullptr,Path);MaterialsReady&=ExactMaterialReady(Material,Path,*W,Row);MaterialReadiness.Add(MakeShared<FJsonValueObject>(Row));}
        S.Report->SetArrayField(TEXT("preloaded_fire_material_resources"),MaterialReadiness);
        if(!MaterialsReady){S.WaitReason=TEXT("FourExactFireOwnedMaterialResources");return true;}
        if(!S.bFramed)
        {
            FText Error;if(!Sim->CanTriggerDisaster(EDisasterType::Fire,Error)){S.WaitReason=TEXT("ProductionFireEligibility");S.TriggerError=Error.ToString();return true;}
            const auto Preview=Sim->SimulateForestFireMap(Scenario->GetScenarioSeverityForHazard(EDisasterType::Fire));
            float Highest=-1.f;for(const auto& Pair:Preview)if(FMath::IsFinite(Pair.Value)&&Pair.Value>Highest&&Terrain->IsPlayableCoord(Pair.Key))
            {Highest=Pair.Value;S.FramingCell=Pair.Key;}
            if(S.FramingCell.X==INDEX_NONE){S.Failure=TEXT("Production canonical wildfire preview contains no eligible source; no fire cells will be fabricated");Finish(S,false);return false;}
            S.Report->SetNumberField(TEXT("read_only_preview_cells"),Preview.Num());
            FAftershockCameraSaveState View;Camera->CaptureSaveState(View);View.LocalFocus=Terrain->GridToWorld(S.FramingCell);View.Location=View.LocalFocus;View.Zoom=8000.f;View.OverviewBlend=0.f;View.SavedLocalZoom=8000.f;
            if(!Camera->RestoreSaveState(View,Error)){S.Failure=TEXT("Normal camera framing failed: ")+Error.ToString();Finish(S,false);return false;}
            S.bFramed=true;S.FramedAt=W->GetTimeSeconds();S.Report->SetBoolField(TEXT("read_only_canonical_fire_preview_for_camera_only"),true);S.WaitReason=TEXT("FramingCameraSettle");return true;
        }
        if(!S.bPinned)
        {
            if(W->GetTimeSeconds()-S.FramedAt<1.f)return true;
            S.PinnedView=PC->PlayerCameraManager->GetCameraCacheView();S.bPinned=true;
        }
        if(S.Stage==0){CaptureFrame(S,*W,*Terrain,*Sim,TEXT("fire-live-before"));return !S.bFinished;}
        if(!S.bTriggered)
        {
            FText Error;if(!S.bWarningRequested){S.bWarningRequested=Director->RequestWarning(EDisasterType::Fire,30.f,Error);if(!S.bWarningRequested){S.WaitReason=TEXT("ProductionWarningEligibility");S.TriggerError=Error.ToString();return true;}}
            if(!Sim->CanTriggerDisaster(EDisasterType::Fire,Error)){S.WaitReason=TEXT("ProductionTriggerEligibility");S.TriggerError=Error.ToString();return true;}
            ++S.TriggerCalls;const double Start=FPlatformTime::Seconds();S.bTriggered=Director->TriggerPendingNow(Error);S.TriggerCpuMilliseconds+=(FPlatformTime::Seconds()-Start)*1000.;
            S.TriggerError=Error.ToString();if(!S.bTriggered){S.Failure=TEXT("One admitted production fire request was rejected: ")+Error.ToString();Finish(S,false);return false;}
            const auto& Snapshot=Sim->GetLatestVisualSnapshot();S.Revision=Snapshot.Revision;S.SourceCell=Snapshot.SourceCoord;S.SourceWorld=Terrain->GridToWorld(S.SourceCell);
            const bool Actual=Snapshot.Revision>0&&Snapshot.Type==EDisasterType::Fire&&!Snapshot.LocalSeverity.IsEmpty()&&Snapshot.DurationSeconds==24.f;
            S.Report->SetBoolField(TEXT("one_production_fire_request_accepted"),Actual);S.Report->SetNumberField(TEXT("snapshot_revision"),S.Revision);
            S.Report->SetNumberField(TEXT("canonical_fire_cells"),Snapshot.LocalSeverity.Num());S.Report->SetNumberField(TEXT("event_duration_seconds"),Snapshot.DurationSeconds);
            S.Report->SetNumberField(TEXT("production_trigger_inclusive_game_thread_ms"),S.TriggerCpuMilliseconds);
            if(!Actual){S.Failure=TEXT("Production Fire did not publish the unchanged canonical 24-second event");Finish(S,false);return false;}
            Write(S);S.WaitReason=TEXT("RealFireAge");return true;
        }
        const auto& Snapshot=Sim->GetLatestVisualSnapshot();if(Snapshot.Revision!=S.Revision||Snapshot.Type!=EDisasterType::Fire){S.Failure=TEXT("Another canonical event replaced the observed production fire");Finish(S,false);return false;}
        const double Age=W->GetTimeSeconds()-Snapshot.StartedAtWorldSeconds;
        const double Targets[]={0.,2.,6.,26.};const TCHAR* Labels[]={TEXT("fire-live-before"),TEXT("fire-live-active-2"),TEXT("fire-live-active-6"),TEXT("fire-live-persistent-26")};
        if(S.Stage<4&&Age>=Targets[S.Stage]){CaptureFrame(S,*W,*Terrain,*Sim,Labels[S.Stage]);return !S.bFinished;}
        if(S.Stage==4)
        {
            const bool Passed=S.Frames.Num()==4&&S.TriggerCalls==1&&Director->GetState().LastReport.DisasterType==EDisasterType::Fire&&!Director->GetState().LastReport.bWasRejected;
            if(!Passed)S.Failure=TEXT("Four accepted frames and one final production fire outcome are required");Finish(S,Passed);return false;
        }
        return true;
    }),.10f);
#endif
}
