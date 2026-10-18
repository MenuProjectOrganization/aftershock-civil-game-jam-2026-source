#include "Aftershock/Public/AftershockStartupPreparationSubsystem.h"
#include "Aftershock/Public/AftershockStartupLoadingWidget.h"
#include "Aftershock/Game/AftershockPlayerController.h"
#include "Aftershock/Public/AftershockMainHUDWidget.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/AftershockPhysicalHazardState.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/StreamableManager.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "MaterialShared.h"
#include "RHIGlobals.h"
#include "UnrealEngine.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "PSOPrecache.h"
#include "PipelineStateCache.h"
#include "RenderCommandFence.h"
#include "ShaderPipelineCache.h"
#include "VertexFactory.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectGlobals.h"
#include "UnrealClient.h"
#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "IAssetCompilingManager.h"
#include "ShaderCompiler.h"
#endif
#include "AftershockStartupMaterialPreparation.h"

namespace AftershockStartup
{
    constexpr int32 MaximumBundleAssets=128;
    constexpr int32 MaximumWarmComponents=2048;
    constexpr int32 MaximumWarmMaterials=256;
    constexpr int32 MaximumPSOEvents=16384;
    constexpr uint64 MaximumEstimatedResourceBytes=256ull*1024ull*1024ull;
    constexpr double MinimumBrandingSeconds=2.5;
    constexpr double TimeoutSeconds=180.;
    const FSoftObjectPath Logo(TEXT("/Game/UI/Branding/T_CyberSecurityStudios.T_CyberSecurityStudios"));

    TArray<FSoftObjectPath> Bundle()
    {
        TArray<FSoftObjectPath> Paths;
        const auto Add=[&](const FString& Package)
        {const FString Name=FPaths::GetCleanFilename(Package);Paths.AddUnique(FSoftObjectPath(Package+TEXT(".")+Name));};
        for(const TCHAR* Name:{TEXT("Body"),TEXT("Door"),TEXT("RoofFlat"),TEXT("RoofGable"),TEXT("RoofHip"),TEXT("RoofShed"),TEXT("Canopy")})
            Add(FString(TEXT("/Game/Buildings/Architecture/Meshes/SM_Arch_"))+Name);
        for(int32 Bays=1;Bays<=4;++Bays)for(int32 Floors=1;Floors<=15;++Floors)
            Add(FString::Printf(TEXT("/Game/Buildings/Architecture/Meshes/SM_Arch_Facade_B%d_F%02d"),Bays,Floors));
        for(const TCHAR* Name:{TEXT("Plaster"),TEXT("Wood"),TEXT("Brick"),TEXT("Metal"),TEXT("Glass"),TEXT("Roof"),TEXT("Concrete")})
            Add(FString(TEXT("/Game/Buildings/Architecture/Materials/M_Arch_"))+Name);
        for(const TCHAR* Name:{TEXT("GrassClump"),TEXT("Shrub"),TEXT("Conifer"),TEXT("Palm"),TEXT("Broadleaf")})for(int32 I=1;I<=3;++I)
            Add(FString::Printf(TEXT("/Game/Environment/Meshes/Realistic/SM_%s_%02d"),Name,I));
        Add(TEXT("/Game/Environment/Materials/Presentation/M_PresentationGroundPBR"));
        Add(TEXT("/Game/Environment/Materials/Presentation/M_PresentationWater"));
        for(const TCHAR* Name:{TEXT("FlameVolume"),TEXT("CoreVolume"),TEXT("SmokeVolume"),TEXT("Ember")})
            Add(FString(TEXT("/Game/FX/Aftershock/Fire/M_Fire"))+Name);
        Add(TEXT("/Game/Aftershock/KenneyMeshes/Imported/SM_CitizenProxy"));
        Add(TEXT("/Game/Environment/Materials/M_CitizenActivity"));
        Add(TEXT("/Game/Environment/Transport/Materials/M_ResidentEmotions"));
        Add(TEXT("/Game/Aftershock/Audio/S_Ambience_City"));
        Add(TEXT("/Game/Aftershock/Audio/S_Toggle"));
        Add(TEXT("/Game/Aftershock/Audio/S_Rotate"));
        Add(TEXT("/Game/UI/Opening/T_AftershockOpening"));
        Paths.Add(Logo);
        return Paths; //103 explicitly owned shared assets, zero region/map packages.
    }
}

struct FAftershockStartupPreparationRuntime
{
    FAftershockStartupPreparationView View;
    TWeakObjectPtr<AAftershockPlayerController> Controller;
    TWeakObjectPtr<UWorld> PreparingWorld;
    TWeakObjectPtr<UWorld> ReadyWorld;
    FSimpleDelegate Completion;
    TSharedPtr<FStreamableHandle> BundleHandle;
    TArray<FSoftObjectPath> BundlePaths;
    FGraphEventArray PSOEvents;
    TArray<FString> IncompleteMaterials;
    TArray<TSharedPtr<FJsonValue>> ActiveMaterialDetails;
    TSet<TWeakObjectPtr<UMaterialInterface>> ShaderWarmRequested;
    TSet<TWeakObjectPtr<UMaterialInterface>> ShaderCacheRequested;
    TMap<FString,int32> PendingAssetTypes;
    TArray<TWeakObjectPtr<UPrimitiveComponent>> WarmComponents;
    FRenderCommandFence ResourceFence;
    FTSTicker::FDelegateHandle Ticker;
    FDelegateHandle PreLoadHandle;
    double StartedAt=0.;
    double NextSceneCheck=0.;
    double NextAuditCheckpoint=0.;
    double LogoShownAt=-1.;
    int32 ConsecutiveReady=0;
    bool bTravelPending=true;
    bool bBundleVerified=false;
    bool bGraphicsStarted=false;
    bool bFenceStarted=false;
    bool bActive=false;
    bool bBrandingCaptureQueued=false;
};

UAftershockStartupPreparationSubsystem::UAftershockStartupPreparationSubsystem(const FObjectInitializer& Initializer):Super(Initializer){}
UAftershockStartupPreparationSubsystem::UAftershockStartupPreparationSubsystem(FVTableHelper& Helper):Super(Helper){}
UAftershockStartupPreparationSubsystem::~UAftershockStartupPreparationSubsystem()=default;

bool UAftershockStartupPreparationSubsystem::IsPreparationEnabled()
{
    if(IsRunningCommandlet()||!FApp::CanEverRender())return false;
    if(FParse::Param(FCommandLine::Get(),TEXT("AftershockStartupAudit")))return true;
    return !FParse::Param(FCommandLine::Get(),TEXT("AftershockVisualAudit"))
        &&!FParse::Param(FCommandLine::Get(),TEXT("AftershockSkipIntro"))
        &&!FParse::Param(FCommandLine::Get(),TEXT("AftershockSettingsMenuAudit"));
}

void UAftershockStartupPreparationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);Runtime=MakeUnique<FAftershockStartupPreparationRuntime>();
    Runtime->PreLoadHandle=FCoreUObjectDelegates::PreLoadMapWithContext.AddUObject(this,&ThisClass::PreLoadMap);
}

void UAftershockStartupPreparationSubsystem::Deinitialize()
{
    if(Runtime)
    {
        FCoreUObjectDelegates::PreLoadMapWithContext.Remove(Runtime->PreLoadHandle);
        if(Runtime->Ticker.IsValid())FTSTicker::GetCoreTicker().RemoveTicker(Runtime->Ticker);
        if(Runtime->BundleHandle)Runtime->BundleHandle->ReleaseHandle();
        Runtime->Completion.Unbind();Runtime->Controller.Reset();Runtime->bActive=false;
    }
    if(LoadingWidget)LoadingWidget->RemoveFromParent();LoadingWidget=nullptr;HeldBundleAssets.Reset();
    // No flush/release of individual render resources. Engine resource owners and graph events own those lifetimes.
    Runtime.Reset();Super::Deinitialize();
}

void UAftershockStartupPreparationSubsystem::PreLoadMap(const FWorldContext& Context,const FString&)
{
    if(!Runtime||Context.OwningGameInstance!=GetGameInstance())return;
    if(LoadingWidget)LoadingWidget->RemoveFromParent();LoadingWidget=nullptr;
    Runtime->Completion.Unbind();Runtime->Controller.Reset();Runtime->PreparingWorld.Reset();Runtime->ReadyWorld.Reset();
    Runtime->bTravelPending=true;Runtime->bActive=false;Runtime->bGraphicsStarted=false;
    Runtime->PSOEvents.Reset();Runtime->WarmComponents.Reset();
    // Keep the managed common bundle across map travel; branding is once per GameInstance.
}

bool UAftershockStartupPreparationSubsystem::IsGameplayBlocked(const UWorld* World) const
{
    return Runtime&&World&&World->IsGameWorld()&&World->GetGameInstance()==GetGameInstance()
        &&IsPreparationEnabled()&&(Runtime->bTravelPending||Runtime->bActive);
}

bool UAftershockStartupPreparationSubsystem::Prepare(AAftershockPlayerController* Controller,FSimpleDelegate Completion)
{
    if(!Runtime||!Controller||!Controller->IsLocalPlayerController()||!IsPreparationEnabled())return false;
    auto& R=*Runtime;
    if(R.ReadyWorld==Controller->GetWorld()&&!R.bTravelPending)return false;
    R.Controller=Controller;R.PreparingWorld=Controller->GetWorld();R.Completion=MoveTemp(Completion);
    R.StartedAt=FPlatformTime::Seconds();R.NextSceneCheck=0.;R.LogoShownAt=-1.;R.ConsecutiveReady=0;R.bActive=true;R.bTravelPending=true;
    R.NextAuditCheckpoint=0.;
    R.ShaderWarmRequested.Reset();R.ShaderCacheRequested.Reset();R.ActiveMaterialDetails.Reset();
    R.bBrandingCaptureQueued=false;
    R.bGraphicsStarted=false;R.bFenceStarted=false;R.PSOEvents.Reset();R.WarmComponents.Reset();
    R.View={};R.View.bBranding=!bBrandingCompleted;R.View.MinimumBrandingSeconds=R.View.bBranding?AftershockStartup::MinimumBrandingSeconds:0.;
    R.View.bPSOPrecachingSupported=PipelineStateCache::IsPSOPrecachingEnabled();
    LoadingWidget=CreateWidget<UAftershockStartupLoadingWidget>(Controller);
    if(!LoadingWidget){Fail(FText::FromString(TEXT("Başlangıç ekranı oluşturulamadı.")));return true;}
    LoadingWidget->SetPreparation(this);LoadingWidget->AddToViewport(10000);
    if(auto* HUD=Controller->GetMainHUD())HUD->SetOpeningMenuActive(true);
    // World initialization timers must run. Input and game progression are gated separately.
    Controller->SetPause(false);
    if(APawn* Pawn=Controller->GetPawn())Pawn->DisableInput(Controller);
    FInputModeUIOnly Input;Input.SetWidgetToFocus(LoadingWidget->TakeWidget());Controller->SetInputMode(Input);Controller->bShowMouseCursor=true;
    BeginBundle();
    if(!R.Ticker.IsValid())R.Ticker=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this,&ThisClass::TickPreparation),.05f);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_STARTUP_BEGIN branding=%d world=%s psoSupported=%d sharedBundleCap=%d resourceEstimateCapMiB=256 gate=realAssetSceneGraphics noAllRegions=1 noForcedMip=1"),
        R.View.bBranding,*GetNameSafe(Controller->GetWorld()),R.View.bPSOPrecachingSupported,AftershockStartup::MaximumBundleAssets);
    RefreshUI();return true;
}

void UAftershockStartupPreparationSubsystem::Detach(AAftershockPlayerController* Controller)
{
    if(!Runtime||Runtime->Controller!=Controller)return;
    if(LoadingWidget)LoadingWidget->RemoveFromParent();LoadingWidget=nullptr;
    Runtime->Controller.Reset();Runtime->PreparingWorld.Reset();Runtime->Completion.Unbind();Runtime->bActive=false;
}

void UAftershockStartupPreparationSubsystem::BeginBundle()
{
    auto& R=*Runtime;R.View.Phase=TEXT("Assets");R.View.Status=FText::FromString(TEXT("Oyun kaynakları hazırlanıyor"));
    if(!R.BundleHandle)
    {
        R.BundlePaths=AftershockStartup::Bundle();
        if(R.BundlePaths.Num()>AftershockStartup::MaximumBundleAssets){Fail(FText::FromString(TEXT("Ortak varlık listesi güvenli sınırı aşıyor.")));return;}
        R.BundleHandle=UAssetManager::GetStreamableManager().RequestAsyncLoad(R.BundlePaths,FStreamableDelegate(),FStreamableManager::DefaultAsyncLoadPriority,true,false,TEXT("AftershockStartupShared"));
        R.bBundleVerified=false;
        if(!R.BundleHandle){Fail(FText::FromString(TEXT("Asenkron varlık yükleme isteği başlatılamadı.")));return;}
    }
    R.View.RequestedAssets=R.BundlePaths.Num();
}

bool UAftershockStartupPreparationSubsystem::CheckSceneReady()
{
    auto& R=*Runtime;auto* World=R.PreparingWorld.Get();if(!World)return false;
    R.View.Phase=TEXT("Scene");R.View.Status=FText::FromString(TEXT("Şehrin arazisi ve su yüzeyi hazırlanıyor"));
    int32 Ready=0,Total=5;
    const auto* Saves=GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>();
    if(Saves&&Saves->IsWorldLoadFailed(World)){Fail(Saves->GetLoadFailure(World));return false;}
    if(!Saves||(!Saves->IsRestoring()&&!Saves->HasPendingLoad()))++Ready;
    const auto* Scenario=World->GetSubsystem<UAftershockScenarioSubsystem>();if(Scenario&&Scenario->IsPrepared())++Ready;
    const auto* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();if(Terrain&&!Terrain->Cells.IsEmpty())++Ready;
    const auto* Life=World->GetSubsystem<UCityLifeSubsystem>();if(Life&&Life->IsCityInitialized())++Ready;
    const auto* Simulation=World->GetSubsystem<UDisasterSimulationSubsystem>();
    bool SurfaceReady=false;
    for(TActorIterator<AAftershockCrowdHostActor> It(World);It;++It)
    {
        SurfaceReady=Terrain&&It->TerrainSurface&&It->TerrainSurface->GetStaticMesh()
            &&It->BuiltTerrainRevision==Terrain->GetTerrainRevision()
            &&It->bBuiltHydraulicModel2==(Simulation&&Simulation->IsHydraulicModel2Active());
        if(SurfaceReady)break;
    }
    if(SurfaceReady)++Ready;
    if(Terrain&&Terrain->ContinuationGeneratorVersion==3)
    {
        ++Total;
        // Only during warmup, at 4 Hz. No extra persistent physics or alternate initialization.
        FAftershockPhysicalHazardSaveState Physical;if(Simulation)Simulation->CapturePhysicalSaveState(Physical);
        if(Physical.ModelVersion==2&&Physical.bWaterInitialized&&Physical.DomainRevision==Terrain->GetTerrainRevision()
            &&Physical.WaterCells.Num()==Terrain->Cells.Num())++Ready;
    }
    R.View.ReadySceneChecks=Ready;R.View.SceneChecks=Total;
    return Ready==Total;
}

bool UAftershockStartupPreparationSubsystem::BeginGraphicsPreparation()
{
    auto& R=*Runtime;auto* World=R.PreparingWorld.Get();if(!World)return false;
    R.View.Phase=TEXT("Graphics");R.View.Status=FText::FromString(TEXT("Şehrin görünümü hazırlanıyor"));
    TSet<UMaterialInterface*> Materials;TSet<UObject*> Resources;
    for(UObject* Object:HeldBundleAssets)
    {
        if(!Object)continue;Resources.Add(Object);
        if(auto* Material=Cast<UMaterialInterface>(Object))Materials.Add(Material);
        if(auto* Mesh=Cast<UStaticMesh>(Object))for(const auto& Slot:Mesh->GetStaticMaterials())if(Slot.MaterialInterface)Materials.Add(Slot.MaterialInterface);
    }
    R.WarmComponents.Reset();
    for(TActorIterator<AActor> It(World);It;++It)
    {
        TInlineComponentArray<UPrimitiveComponent*> Components;It->GetComponents(Components);
        for(auto* Component:Components)
        {
            if(!IsValid(Component)||!Component->IsRegistered())continue;
            if(R.WarmComponents.Num()>=AftershockStartup::MaximumWarmComponents){Fail(FText::FromString(TEXT("Sahne bileşeni hazırlama sınırını aşıyor.")));return false;}
            R.WarmComponents.Add(Component);
            for(int32 Slot=0;Slot<Component->GetNumMaterials();++Slot)if(auto* Material=Component->GetMaterial(Slot))Materials.Add(Material);
        }
    }
    if(Materials.Num()>AftershockStartup::MaximumWarmMaterials){Fail(FText::FromString(TEXT("Paylaşılan materyal hazırlama sınırı aşıldı.")));return false;}
    for(auto* Material:Materials)
    {
        Resources.Add(Material);TArray<UTexture*> Textures;Material->GetUsedTextures(Textures);
        for(auto* Texture:Textures)if(Texture)Resources.Add(Texture);
    }
    uint64 Estimated=0;for(auto* Resource:Resources)Estimated+=Resource->GetResourceSizeBytes(EResourceSizeMode::Exclusive);
    R.View.EstimatedExclusiveResourceBytes=Estimated;
    if(Estimated>AftershockStartup::MaximumEstimatedResourceBytes)
    {Fail(FText::FromString(TEXT("Hazırlanan ortak kaynakların tahmini özel kaynak belleği 256 MiB sınırını aşıyor.")));return false;}
#if WITH_EDITOR
    // Request only missing resources for this world/platform/quality, at most
    // four interfaces per 250 ms heartbeat. Readiness is checked separately.
    if(RequestAftershockBoundedMaterialWarmup(Materials,*World,R.ShaderWarmRequested,R.ShaderCacheRequested)>0)return false;
#endif
#if WITH_EDITOR
    R.View.RemainingShaders=GShaderCompilingManager?GShaderCompilingManager->GetNumRemainingJobs():0;
    R.View.RemainingAssetCompiles=FAssetCompilingManager::Get().GetNumRemainingAssets();
    R.PendingAssetTypes.Reset();
    for(const auto* Manager:FAssetCompilingManager::Get().GetRegisteredManagers())
        if(Manager&&Manager->GetNumRemainingAssets()>0)
            R.PendingAssetTypes.Add(Manager->GetAssetTypeName().ToString(),Manager->GetNumRemainingAssets());
    if(R.View.RemainingShaders>0||R.View.RemainingAssetCompiles>0)return false;
#endif
    R.IncompleteMaterials.Reset();R.ActiveMaterialDetails.Reset();
    TArray<UMaterialInterface*> OrderedMaterials=Materials.Array();
    OrderedMaterials.Sort([](const UMaterialInterface& A,const UMaterialInterface& B)
        {return A.GetPathName()<B.GetPathName();});
    bool MaterialsReady=true;
    for(auto* Material:OrderedMaterials)
    {
        FString Why;TSharedPtr<FJsonObject> Detail;
        const bool Ready=IsAftershockActiveOwnedMaterialReady(Material,*World,&Why,
            R.ActiveMaterialDetails.Num()<8?&Detail:nullptr);
        if(Detail)R.ActiveMaterialDetails.Add(MakeShared<FJsonValueObject>(Detail.ToSharedRef()));
        if(!Ready)
        {
            MaterialsReady=false;
            if(R.IncompleteMaterials.Num()<8)R.IncompleteMaterials.Add(Material->GetPathName()+TEXT(" :: ")+Why);
        }
    }
    if(!MaterialsReady)return false;
    R.PSOEvents.Reset();
    if(R.View.bPSOPrecachingSupported)
    {
        TArray<const FVertexFactoryType*> Factories;
        if(auto* Factory=FindVertexFactoryType(FHashedName(TEXT("FLocalVertexFactory"))))Factories.Add(Factory);
        if(auto* Factory=FindVertexFactoryType(FHashedName(TEXT("FInstancedStaticMeshVertexFactory"))))Factories.Add(Factory);
        if(Factories.IsEmpty()){Fail(FText::FromString(TEXT("Gerekli vertex factory türleri kayıtlı değil.")));return false;}
        for(auto* Material:Materials)for(int32 Mobility=0;Mobility<2;++Mobility)
        {
            FPSOPrecacheParams Params;Params.SetMobility(Mobility==0?EComponentMobility::Static:EComponentMobility::Movable);
            R.PSOEvents.Append(Material->PrecachePSOs(Factories,Params));
            if(R.PSOEvents.Num()>AftershockStartup::MaximumPSOEvents){Fail(FText::FromString(TEXT("PSO hazırlama işi güvenli sınırını aşıyor.")));return false;}
        }
        for(const auto& Weak:R.WarmComponents)if(auto* Component=Weak.Get())Component->PrecachePSOs();
    }
    // This fence proves queued render-thread setup completed, not that all future gameplay has rendered.
    R.ResourceFence.BeginFence();R.bFenceStarted=true;R.bGraphicsStarted=true;
    R.View.RequestedPSOEvents=R.PSOEvents.Num();return true;
}

bool UAftershockStartupPreparationSubsystem::CheckGraphicsReady()
{
    auto& R=*Runtime;auto& V=R.View;V.CompletedPSOEvents=0;
    for(const auto& Event:R.PSOEvents)if(!Event.IsValid()||Event->IsComplete())++V.CompletedPSOEvents;
    V.PendingComponents=0;for(const auto& Weak:R.WarmComponents)if(auto* Component=Weak.Get())if(Component->IsPSOPrecaching())++V.PendingComponents;
    V.ActivePrecacheRequests=static_cast<int32>(PipelineStateCache::NumActivePrecacheRequests());
    V.ActivePipelineTasks=PipelineStateCache::GetNumActivePipelinePrecompileTasks();
    V.RemainingPipelineCacheJobs=FShaderPipelineCache::NumPrecompilesRemaining();
    V.RemainingShaders=0;V.RemainingAssetCompiles=0;
#if WITH_EDITOR
    if(GShaderCompilingManager)V.RemainingShaders=GShaderCompilingManager->GetNumRemainingJobs();
    V.RemainingAssetCompiles=FAssetCompilingManager::Get().GetNumRemainingAssets();
#endif
    return V.CompletedPSOEvents==V.RequestedPSOEvents&&V.PendingComponents==0&&V.ActivePrecacheRequests==0
        &&V.ActivePipelineTasks==0&&V.RemainingPipelineCacheJobs==0&&V.RemainingShaders==0
        &&V.RemainingAssetCompiles==0&&R.bFenceStarted&&R.ResourceFence.IsFenceComplete();
}

bool UAftershockStartupPreparationSubsystem::TickPreparation(float)
{
    if(!Runtime)return false;auto& R=*Runtime;if(!R.bActive||!R.Controller.IsValid())return true;
    auto& V=R.View;V.ElapsedSeconds=FPlatformTime::Seconds()-R.StartedAt;
    if(auto* World=R.PreparingWorld.Get())if(const auto* Saves=GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>();Saves&&Saves->IsWorldLoadFailed(World))
    {
        // The existing failed-load recovery menu owns this path; it must still offer a clean New Game.
        V.Error=Saves->GetLoadFailure(World);WriteAudit(false);
        if(LoadingWidget)LoadingWidget->RemoveFromParent();LoadingWidget=nullptr;
        R.bActive=false;R.bTravelPending=false;R.ReadyWorld=World;R.Completion.Unbind();
        if(auto* Controller=R.Controller.Get())Controller->PresentLoadFailureRecovery();
        R.Ticker.Reset();return false;
    }
    if(V.bFailed){RefreshUI();return true;}
    if(V.ElapsedSeconds>AftershockStartup::TimeoutSeconds)
    {Fail(FText::FromString(TEXT("Başlangıç hazırlığı beklenenden uzun sürdü. Yeniden deneyebilir veya çıkabilirsiniz.")));return true;}
    if(!R.BundleHandle){Fail(FText::FromString(TEXT("Ortak yükleme tutamacı kayboldu.")));return true;}
    if(V.bBranding&&GetStudioTexture())
    {
        if(R.LogoShownAt<0.)R.LogoShownAt=FPlatformTime::Seconds();
        V.BrandingDisplayedSeconds=FPlatformTime::Seconds()-R.LogoShownAt;
        if(!R.bBrandingCaptureQueued&&V.BrandingDisplayedSeconds>=.25
            &&FParse::Param(FCommandLine::Get(),TEXT("AftershockStartupAudit"))
            &&!FScreenshotRequest::IsScreenshotRequested())
        {
            const FString Directory=FPaths::ProjectSavedDir()/TEXT("PresentationValidation");
            IFileManager::Get().MakeDirectory(*Directory,true);
            FScreenshotRequest::RequestScreenshot(Directory/TEXT("startup-branding.png"),true,false);
            R.bBrandingCaptureQueued=true;
        }
    }
    V.AssetProgress=R.BundleHandle->GetLoadProgress();V.ResolvedAssets=0;
    for(const auto& Path:R.BundlePaths)if(Path.ResolveObject())++V.ResolvedAssets;
    if(R.BundleHandle->WasCanceled()){Fail(FText::FromString(TEXT("Ortak varlık yüklemesi iptal edildi.")));return true;}
    if(!R.BundleHandle->HasLoadCompleted()){RefreshUI();return true;}
    if(!R.bBundleVerified)
    {
        FString Missing;for(const auto& Path:R.BundlePaths)if(!Path.ResolveObject()){if(!Missing.IsEmpty())Missing+=TEXT("\n");Missing+=Path.ToString();}
        if(!Missing.IsEmpty())
        {
            UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_STARTUP_MISSING_ASSETS %s"),*Missing);
            Fail(FText::FromString(TEXT("Bazı oyun dosyaları yüklenemedi. Kurulumu doğrulayın ve yeniden deneyin.")));return true;
        }
        HeldBundleAssets.Reset();for(const auto& Path:R.BundlePaths)HeldBundleAssets.Add(Path.ResolveObject());R.bBundleVerified=true;
    }
    if(!R.bGraphicsStarted)
    {
        if(FPlatformTime::Seconds()<R.NextSceneCheck){RefreshUI();return true;}R.NextSceneCheck=FPlatformTime::Seconds()+.25;
        if(!CheckSceneReady()||V.bFailed){RefreshUI();return true;}
        if(!BeginGraphicsPreparation()){RefreshUI();return true;}
    }
    const bool GraphicsReady=CheckGraphicsReady();R.ConsecutiveReady=GraphicsReady?R.ConsecutiveReady+1:0;
    if(R.ConsecutiveReady>=2)
    {
        if(V.bBranding&&(R.LogoShownAt<0.||V.BrandingDisplayedSeconds<V.MinimumBrandingSeconds))
        {V.Phase=TEXT("Branding");V.Status=FText::FromString(TEXT("Hazır"));}
        else {Complete();R.Ticker.Reset();return false;}
    }
    RefreshUI();return true;
}

void UAftershockStartupPreparationSubsystem::Complete()
{
    auto& R=*Runtime;R.View.bReady=true;R.View.Phase=TEXT("Ready");R.View.Status=FText::FromString(TEXT("Hazır"));
    R.bActive=false;R.bTravelPending=false;R.ReadyWorld=R.PreparingWorld;bBrandingCompleted=true;
    RefreshUI();WriteAudit(true);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_STARTUP_READY elapsedSeconds=%.3f assets=%d/%d psoEvents=%d/%d psoSupported=%d scene=%d/%d estimatedResourceBytes=%llu actualAllFuturePSOsClaim=0"),
        R.View.ElapsedSeconds,R.View.ResolvedAssets,R.View.RequestedAssets,R.View.CompletedPSOEvents,R.View.RequestedPSOEvents,R.View.bPSOPrecachingSupported,R.View.ReadySceneChecks,R.View.SceneChecks,R.View.EstimatedExclusiveResourceBytes);
    if(LoadingWidget)LoadingWidget->RemoveFromParent();LoadingWidget=nullptr;
    if(auto* Controller=R.Controller.Get())
    {
        if(auto* HUD=Controller->GetMainHUD())HUD->SetOpeningMenuActive(false);
        if(APawn* Pawn=Controller->GetPawn())Pawn->EnableInput(Controller);
        FInputModeGameAndUI Input;Input.SetHideCursorDuringCapture(false);Controller->SetInputMode(Input);
    }
    auto Completion=MoveTemp(R.Completion);R.Completion.Unbind();Completion.ExecuteIfBound();
}

void UAftershockStartupPreparationSubsystem::Fail(const FText& Reason)
{
    if(!Runtime)return;Runtime->View.bFailed=true;Runtime->View.Error=Reason;Runtime->View.Phase=TEXT("Failed");
    Runtime->View.Status=FText::FromString(TEXT("Hazırlık tamamlanamadı"));RefreshUI();WriteAudit(false);
    UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_STARTUP_FAILED %s"),*Reason.ToString());
}

void UAftershockStartupPreparationSubsystem::Retry()
{
    if(!Runtime||!Runtime->Controller.IsValid())return;
    // Reuse a successfully loaded bundle for shader/resource retries; retry actual failed loads.
    if(!Runtime->bBundleVerified)
    {if(Runtime->BundleHandle)Runtime->BundleHandle->ReleaseHandle();Runtime->BundleHandle.Reset();HeldBundleAssets.Reset();}
    auto Completion=Runtime->Completion;auto* Controller=Runtime->Controller.Get();
    if(LoadingWidget)LoadingWidget->RemoveFromParent();LoadingWidget=nullptr;
    Prepare(Controller,MoveTemp(Completion));
}
void UAftershockStartupPreparationSubsystem::Quit()
{if(Runtime)if(auto* Controller=Runtime->Controller.Get())UKismetSystemLibrary::QuitGame(Controller,Controller,EQuitPreference::Quit,false);}
void UAftershockStartupPreparationSubsystem::RefreshUI()
{
    if(LoadingWidget)LoadingWidget->Refresh();
    if(Runtime&&Runtime->bActive&&FParse::Param(FCommandLine::Get(),TEXT("AftershockStartupAudit"))
        &&FPlatformTime::Seconds()>=Runtime->NextAuditCheckpoint)
    {
        Runtime->NextAuditCheckpoint=FPlatformTime::Seconds()+2.;
        WriteAudit(false);
    }
}
const FAftershockStartupPreparationView& UAftershockStartupPreparationSubsystem::GetView() const
{static const FAftershockStartupPreparationView Empty;return Runtime?Runtime->View:Empty;}
UTexture2D* UAftershockStartupPreparationSubsystem::GetStudioTexture() const
{return Cast<UTexture2D>(AftershockStartup::Logo.ResolveObject());}

void UAftershockStartupPreparationSubsystem::AcknowledgeLiveWorldOwnership(UWorld* World)
{
    if(!Runtime||Runtime->bActive||Runtime->ReadyWorld!=World)return;
    const auto* Session=GetGameInstance()->GetSubsystem<UAftershockOpeningSessionSubsystem>();
    if(!Session||!Session->HasLiveSession())return;
    if(Runtime->BundleHandle)Runtime->BundleHandle->ReleaseHandle();Runtime->BundleHandle.Reset();HeldBundleAssets.Reset();
    Runtime->bBundleVerified=false;
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_STARTUP_BUNDLE_RELEASE liveWorld=%s gameplayOwnersRetained=1 forcedGC=0"),*GetNameSafe(World));
}

void UAftershockStartupPreparationSubsystem::WriteAudit(bool Success) const
{
    if(!Runtime||!FParse::Param(FCommandLine::Get(),TEXT("AftershockStartupAudit")))return;
    const auto& V=Runtime->View;auto Root=MakeShared<FJsonObject>();
    Root->SetBoolField(TEXT("valid"),Success);Root->SetStringField(TEXT("scope"),TEXT("actual_requested_shared_assets_current_world_resources_and_supported_PSO_jobs_only"));
    Root->SetBoolField(TEXT("in_progress"),Runtime->bActive&&!V.bReady&&!V.bFailed);
    Root->SetStringField(TEXT("observed_utc"),FDateTime::UtcNow().ToIso8601());
    Root->SetStringField(TEXT("world"),GetNameSafe(Runtime->PreparingWorld.Get()));Root->SetBoolField(TEXT("branding"),V.bBranding);
    Root->SetStringField(TEXT("phase"),V.Phase);Root->SetStringField(TEXT("error"),V.Error.ToString());Root->SetNumberField(TEXT("elapsed_seconds"),V.ElapsedSeconds);
    Root->SetNumberField(TEXT("branding_displayed_seconds"),V.BrandingDisplayedSeconds);
    Root->SetBoolField(TEXT("branding_capture_requested"),Runtime->bBrandingCaptureQueued);
    Root->SetNumberField(TEXT("requested_assets"),V.RequestedAssets);Root->SetNumberField(TEXT("resolved_assets"),V.ResolvedAssets);
    Root->SetNumberField(TEXT("pso_events_requested"),V.RequestedPSOEvents);Root->SetNumberField(TEXT("pso_events_completed"),V.CompletedPSOEvents);
    Root->SetBoolField(TEXT("pso_precaching_supported"),V.bPSOPrecachingSupported);Root->SetNumberField(TEXT("pending_components"),V.PendingComponents);
    Root->SetNumberField(TEXT("pending_shaders"),V.RemainingShaders);Root->SetNumberField(TEXT("pending_asset_compiles"),V.RemainingAssetCompiles);
    TArray<TSharedPtr<FJsonValue>> Incomplete;
    for(const auto& Material:Runtime->IncompleteMaterials)Incomplete.Add(MakeShared<FJsonValueString>(Material));
    Root->SetArrayField(TEXT("incomplete_materials_first_eight"),Incomplete);
    Root->SetArrayField(TEXT("active_material_details"),Runtime->ActiveMaterialDetails);
    Root->SetNumberField(TEXT("material_warm_request_interfaces"),Runtime->ShaderWarmRequested.Num());
    Root->SetNumberField(TEXT("material_background_cache_requests"),Runtime->ShaderCacheRequested.Num());
    auto PendingTypes=MakeShared<FJsonObject>();
    for(const auto& Type:Runtime->PendingAssetTypes)PendingTypes->SetNumberField(Type.Key,Type.Value);
    Root->SetObjectField(TEXT("pending_asset_types"),PendingTypes);
    Root->SetNumberField(TEXT("pending_precache_requests"),V.ActivePrecacheRequests);Root->SetNumberField(TEXT("pending_pipeline_tasks"),V.ActivePipelineTasks);
    Root->SetNumberField(TEXT("pending_pipeline_cache_jobs"),V.RemainingPipelineCacheJobs);Root->SetNumberField(TEXT("resource_bytes_estimate"),static_cast<double>(V.EstimatedExclusiveResourceBytes));
    Root->SetBoolField(TEXT("all_maps_preloaded"),false);Root->SetBoolField(TEXT("forced_mips_or_all_lods"),false);Root->SetBoolField(TEXT("all_future_gameplay_rendered"),false);
    FString JSON;auto Writer=TJsonWriterFactory<>::Create(&JSON);FJsonSerializer::Serialize(Root,Writer);
    const FString Directory=FPaths::ProjectSavedDir()/TEXT("PresentationValidation");IFileManager::Get().MakeDirectory(*Directory,true);
    FFileHelper::SaveStringToFile(JSON,*(Directory/TEXT("startup_preparation_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FFileHelper::SaveStringToFile(JSON,*(Directory/(V.bBranding?TEXT("startup_preparation_boot.json"):TEXT("startup_preparation_selected.json"))),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
