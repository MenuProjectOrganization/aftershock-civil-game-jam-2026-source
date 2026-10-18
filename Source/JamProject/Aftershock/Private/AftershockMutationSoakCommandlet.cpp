#include "AftershockMutationSoakCommandlet.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/DisasterPresentationSubsystem.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/PlatformMemory.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "TimerManager.h"
#include "CoreGlobals.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectIterator.h"

UAftershockMutationSoakCommandlet::UAftershockMutationSoakCommandlet()
{ IsClient=false; IsServer=false; IsEditor=true; LogToConsole=true; }

int32 UAftershockMutationSoakCommandlet::Main(const FString& Params)
{
    if (!GEngine) return 1;
    UWorld* W=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("MutationSoakFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
    if (!W) return 1;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
    auto* Terrain=W->GetSubsystem<UTerrainGridSubsystem>();
    auto* Buildings=W->GetSubsystem<UBuildingManagerSubsystem>();
    bool Valid=Terrain&&Buildings;
    if (!Valid) return 1;
    int32 DeferredFrameProbes=0,LogicalFrameProbes=0;bool FrameCounterAdvanced=true;
    const auto AdvanceLogicalFrame=[&](bool bRecordedDamageCycle)
    {
        // FTimerManager::Tick is guarded by GFrameCounter. Repeated native
        // calls in one engine frame otherwise skip every deferred callback
        // after the first Tick, unfairly omitting the changed code's work.
        bool ProbeRan=false;
        FTimerHandle ProbeHandle=W->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([&ProbeRan](){ProbeRan=true;}));
        const uint64 PreviousFrame=GFrameCounter;++GFrameCounter;
        FrameCounterAdvanced&=GFrameCounter==PreviousFrame+1;
        W->GetTimerManager().Tick(.08f);
        // Never retain a stack-capturing probe if a broken timer fails to run.
        W->GetTimerManager().ClearTimer(ProbeHandle);
        Valid&=ProbeRan;if(ProbeRan){++LogicalFrameProbes;if(bRecordedDamageCycle)++DeferredFrameProbes;}
        return ProbeRan;
    };
    Terrain->GenerateProceduralTerrain(64,64,500.f,1337);
    for (auto Cell:Terrain->Cells)
    {
        Cell.bIsPlayable=true; Cell.bIsRiverbed=false; Cell.bIsOcean=false; Cell.bHasRoad=false;
        Cell.bIsEmergencyAssemblyArea=false; Cell.bVolcanicVent=false; Cell.Elevation=0.f;
        Terrain->SetCell(Cell.Coords,Cell);
    }
    auto* Data=Buildings->GetCatalogData(EBuildingType::House);
    TArray<int32> IDs;
    const double ConstructionStart=FPlatformTime::Seconds();
    for (int32 Y=2;Y<34&&IDs.Num()<1000;++Y) for (int32 X=2;X<34&&IDs.Num()<1000;++X)
    { int32 ID=INDEX_NONE; if (Buildings->PlaceBuilding(FIntPoint(X,Y),Data,EFoundationQuality::Medium,1,ID)) IDs.Add(ID); }
    AdvanceLogicalFrame(false);
    const double ConstructionMs=(FPlatformTime::Seconds()-ConstructionStart)*1000.;
    Valid&=IDs.Num()==1000;
    auto* Weather=W->SpawnActor<AAftershockDisasterPresentationActor>();
    FAftershockDisasterVisualSnapshot Snapshot;
    Snapshot.Revision=900; Snapshot.Type=EDisasterType::Flood; Snapshot.DurationSeconds=24; Snapshot.EffectiveIntensity=100;
    for (int32 Y=44;Y<49;++Y) for (int32 X=44;X<49;++X) Snapshot.LocalSeverity.Add(FIntPoint(X,Y),100.f);
    TArray<FString> Rows;
    const auto Record=[&](int32 Cycle,double DamageMs,double FlushMs,double FloodMs,bool bAfterGC,bool bDeferredFrameProbe)
    {
        int32 Meshes=0,Materials=0;uint64 MeshBytes=0;
        for(TObjectIterator<UStaticMesh> It;It;++It)if(It->IsIn(W)&&!It->HasAnyFlags(RF_ClassDefaultObject))
        {++Meshes;FResourceSizeEx Size(EResourceSizeMode::EstimatedTotal);It->GetResourceSizeEx(Size);MeshBytes+=Size.GetTotalMemoryBytes();}
        for(TObjectIterator<UMaterialInstanceDynamic> It;It;++It)if(It->IsIn(W))++Materials;
        const auto Memory=FPlatformMemory::GetStats();
        Rows.Add(FString::Printf(TEXT("{\"cycle\":%d,\"damage1000_cpu_ms\":%.5f,\"timer_flush_cpu_ms\":%.5f,\"flood_build_clear_cpu_ms\":%.5f,\"deferred_frame_probe_ran\":%s,\"after_explicit_gc\":%s,\"world_owned_static_meshes\":%d,\"world_owned_material_instances\":%d,\"estimated_mesh_resource_bytes\":%llu,\"process_used_physical_bytes\":%llu,\"process_used_virtual_bytes\":%llu}"),Cycle,DamageMs,FlushMs,FloodMs,bDeferredFrameProbe?TEXT("true"):TEXT("false"),bAfterGC?TEXT("true"):TEXT("false"),Meshes,Materials,MeshBytes,Memory.UsedPhysical,Memory.UsedVirtual));
    };
    for (int32 Cycle=-2;Cycle<16;++Cycle)
    {
        const double Start=FPlatformTime::Seconds();
        for(int32 I=0;I<IDs.Num();++I) Buildings->ApplyDamageToBuilding(IDs[I], ((I+Cycle)&1)?30.f:60.f);
        const double Applied=FPlatformTime::Seconds();
        const bool ProbeRan=AdvanceLogicalFrame(Cycle>=0);
        const double Flushed=FPlatformTime::Seconds();
        Weather->BeginSnapshot(Snapshot,*Terrain); Weather->ClearPresentation();
        const double Flooded=FPlatformTime::Seconds();
        const bool GC=(Cycle<0)||(Cycle%4==3);
        if(GC)CollectGarbage(RF_NoFlags,true);
        if(Cycle>=0)Record(Cycle,(Applied-Start)*1000.,(Flushed-Applied)*1000.,(Flooded-Flushed)*1000.,GC,ProbeRan);
        Valid&=Buildings->GetActiveBuildingsCount()==1000;
    }
    const int32 SlotsBefore=Buildings->GetOrCreateISM(Data)->GetInstanceCount();
    int32 Latest=IDs.Last();bool Unique=true;
    const double RebuildStart=FPlatformTime::Seconds();
    for(int32 I=0;I<100;++I)
    {
        FBuildingInstance Previous;Valid&=Buildings->GetBuildingByID(IDs.Last(),Previous);
        Valid&=Buildings->RemoveBuilding(IDs.Last());int32 Replacement=INDEX_NONE;
        Valid&=Buildings->PlaceBuilding(Previous.GridCoords,Data,EFoundationQuality::Medium,1,Replacement);
        Unique&=Replacement>Latest;Latest=Replacement;IDs.Last()=Replacement;
        AdvanceLogicalFrame(false);
    }
   
    const double RebuildMs=(FPlatformTime::Seconds()-RebuildStart)*1000.;
    const int32 SlotsAfter=Buildings->GetOrCreateISM(Data)->GetInstanceCount();
    CollectGarbage(RF_NoFlags,true);Record(16,0.,0.,0.,true,false);
    Valid&=Unique&&Buildings->GetActiveBuildingsCount()==1000&&FrameCounterAdvanced&&DeferredFrameProbes==16&&LogicalFrameProbes==119;
    const FString JSON=FString::Printf(TEXT("{\"schema_version\":1,\"valid\":%s,\"scope\":\"Identical native diagnostic harness on baseline and changed game code. Actual 1000-building API mutations, 16 damage/flood cycles, 100 demolition/rebuild pairs. Every logical timer frame advances GFrameCounter and proves a next-tick callback ran, including all16 recorded damage cycles, so deferred timer work is included. Explicit GC is marked; no GPU/real-frame/normal-game leak claim, no CityLife population or real hazard formulas invoked.\",\"frame_counter_advanced\":%s,\"deferred_frame_probes\":%d,\"logical_frame_probes\":%d,\"buildings\":%d,\"construction1000_cpu_ms\":%.5f,\"demolition_rebuild100_cpu_ms\":%.5f,\"canonical_ism_slots_before\":%d,\"canonical_ism_slots_after\":%d,\"monotonic_ids\":%s,\"samples\":[%s]}"),Valid?TEXT("true"):TEXT("false"),FrameCounterAdvanced?TEXT("true"):TEXT("false"),DeferredFrameProbes,LogicalFrameProbes,Buildings->GetActiveBuildingsCount(),ConstructionMs,RebuildMs,SlotsBefore,SlotsAfter,Unique?TEXT("true"):TEXT("false"),*FString::Join(Rows,TEXT(",")));
    const FString Path=FPaths::ProjectSavedDir()/TEXT("PresentationValidation/mutation_soak.json");IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
    Valid&=FFileHelper::SaveStringToFile(JSON,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MUTATION_SOAK_RESULT valid=%d buildings=%d slotsBefore=%d slotsAfter=%d constructionMs=%.4f rebuild100Ms=%.4f frameCounterAdvanced=%d deferredFrameProbes=%d logicalFrameProbes=%d"),Valid,IDs.Num(),SlotsBefore,SlotsAfter,ConstructionMs,RebuildMs,FrameCounterAdvanced,DeferredFrameProbes,LogicalFrameProbes);
    W->DestroyWorld(false);GEngine->DestroyWorldContext(W);W->RemoveFromRoot();
    return Valid?0:1;
}
