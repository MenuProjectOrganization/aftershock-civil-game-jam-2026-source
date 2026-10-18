#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "AftershockTerrainSurface.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Async/TaskGraphInterfaces.h"
#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_EDITOR
bool RunAftershockHydraulicGroundInstallFixtures(const FString& AbsoluteReportPath)
{
    TArray<TSharedPtr<FJsonValue>> Rows;int32 Failed=0;
    const auto Check=[&](const TCHAR* Name,bool Passed)
    {
        auto R=MakeShared<FJsonObject>();R->SetStringField(TEXT("name"),Name);R->SetBoolField(TEXT("passed"),Passed);
        Rows.Add(MakeShared<FJsonValueObject>(R));Failed+=Passed?0:1;
        if(Passed){UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_HYDRAULIC_GROUND_AUDIT_PASS %s"),Name);}
        else{UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_HYDRAULIC_GROUND_AUDIT_FAIL %s"),Name);}
    };
    const auto Tick=[](UWorld& World)
    {++GFrameCounter;FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);World.Tick(LEVELTICK_All,.08f);FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);FPlatformProcess::SleepNoStats(.001f);};
    UWorld* World=GEngine?UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("HydraulicGroundInstallFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false):nullptr;
    Check(TEXT("isolated_game_world_created"),World!=nullptr);
    if(World)
    {
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        auto* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();auto* Crowd=World->GetSubsystem<UCrowdRenderSubsystem>();
        auto* Simulation=World->GetSubsystem<UDisasterSimulationSubsystem>();auto* Roads=World->GetSubsystem<URoadNetworkSubsystem>();
        auto* Economy=World->GetSubsystem<UEconomyAndStateSubsystem>();
        Check(TEXT("real_canonical_owners_available"),Terrain&&Crowd&&Simulation&&Roads&&Economy);
        if(Terrain&&Crowd&&Simulation&&Roads&&Economy)
        {
            // Explicit synthetic terrain validates lifecycle and real engine
            // cache/collision behavior. It is not a measured river/DEM fixture.
            Terrain->GenerateProceduralTerrain(6,6,1000.f,918273);Terrain->ContinuationGeneratorVersion=3;
            Terrain->CoastalSeaLevelCm=0.f;Terrain->CoastStartRow=INDEX_NONE;
            for(auto& C:Terrain->Cells){const FIntPoint Coord=C.Coords;C=FGridCell();C.Coords=Coord;C.Elevation=1000.f;C.bIsPlayable=true;C.HydraulicProfileVersion=1;}
            const uint64 Revision=Terrain->GetTerrainRevision();Crowd->EnsureTerrainVisualBuilt();
            AAftershockCrowdHostActor* Host=nullptr;for(TActorIterator<AAftershockCrowdHostActor> It(World);It;++It){Host=*It;break;}
            Check(TEXT("legacy_mode_ground_installed"),Host&&Host->TerrainSurface&&Host->TerrainSurface->GetStaticMesh()&&!Host->bBuiltHydraulicModel2&&!Simulation->IsHydraulicModel2Active());
            const auto Before=GetAftershockTerrainSurfaceBuildStats(World);
            TWeakObjectPtr<UStaticMesh> OldMesh=Host&&Host->TerrainSurface?Host->TerrainSurface->GetStaticMesh():nullptr;
            FAftershockPhysicalHazardSaveState Saved;Saved.ModelVersion=2;Saved.bWaterInitialized=true;Saved.bNeedsLegacyWaterSeed=false;
            Saved.CapturedCellAreaM2=100.;Saved.DomainRevision=Revision;
            for(const auto& C:Terrain->Cells){auto& W=Saved.WaterCells.AddDefaulted_GetRef();W.Coord=C.Coords;W.BedElevationCm=C.Elevation;}
            FText Error;const bool Restored=Simulation->RestorePhysicalSaveState(Saved,Error);
            Check(TEXT("public_model2_restore_requires_no_canonical_revision_change"),Restored&&Terrain->GetTerrainRevision()==Revision&&Simulation->IsHydraulicModel2Active());
            FAftershockPhysicalHazardSaveState Published;bool Ready=false;const double Until=FPlatformTime::Seconds()+15.;
            while(Restored&&FPlatformTime::Seconds()<Until)
            {
                Tick(*World);Simulation->CapturePhysicalSaveState(Published);
                if(Published.bWaterInitialized&&Host&&Host->bBuiltHydraulicModel2){Ready=true;break;}
            }
            Check(TEXT("first_real_initialization_publication_installs_mode2_ground_without_external_refresh"),Ready&&Host&&Host->TerrainSurface&&Host->TerrainSurface->GetStaticMesh()!=OldMesh.Get());
            Check(TEXT("mode_install_does_not_mutate_terrain_revision"),Terrain->GetTerrainRevision()==Revision);
            const auto Installed=GetAftershockTerrainSurfaceBuildStats(World);
            Check(TEXT("physical_mode_change_builds_and_cooks_exactly_once"),Ready&&Installed.SuccessfulGroundBuilds==Before.SuccessfulGroundBuilds+1&&Installed.CollisionCookAttempts==Before.CollisionCookAttempts+1);
            const auto InstalledMesh=Host&&Host->TerrainSurface?Host->TerrainSurface->GetStaticMesh():nullptr;
            for(int32 I=0;I<12;++I)Tick(*World);
            const auto Flow=GetAftershockTerrainSurfaceBuildStats(World);
            Check(TEXT("ordinary_flow_publications_preserve_ground_and_collision"),Flow.SuccessfulGroundBuilds==Installed.SuccessfulGroundBuilds&&Flow.CollisionCookAttempts==Installed.CollisionCookAttempts&&Host&&Host->TerrainSurface->GetStaticMesh()==InstalledMesh);
            Economy->TreasuryTL=10000;const int64 Treasury=Economy->TreasuryTL;FAftershockRoadQuote Quote;
            const bool Built=Roads->BuildRoad(FIntPoint(2,2),FIntPoint(2,2),Quote);
            Check(TEXT("next_one_cell_road_uses_real_paid_transaction"),Built&&Terrain->GetCell(FIntPoint(2,2)).bHasRoad&&Quote.NewRoadCells==1&&Treasury-Economy->TreasuryTL==200);
            // The production road UI performs this same installation barrier.
            // This isolated world has no started map/camera/city delegate tree;
            // real affected-chunk/RHI throughput stays in the existing road probe.
            Crowd->EnsureTerrainVisualBuilt();const auto Road=GetAftershockTerrainSurfaceBuildStats(World);
            Check(TEXT("road_metadata_refresh_hits_installed_ground_cache"),Built&&Road.SuccessfulGroundBuilds==Installed.SuccessfulGroundBuilds&&Road.CollisionCookAttempts==Installed.CollisionCookAttempts&&Host&&Host->TerrainSurface->GetStaticMesh()==InstalledMesh);
            for(int32 I=0;I<12;++I)Tick(*World);
            const auto Rebound=GetAftershockTerrainSurfaceBuildStats(World);
            Check(TEXT("road_hydraulic_reinitialization_does_not_recook_unchanged_ground"),Rebound.SuccessfulGroundBuilds==Installed.SuccessfulGroundBuilds&&Rebound.CollisionCookAttempts==Installed.CollisionCookAttempts);
        }
        World->DestroyWorld(false);GEngine->DestroyWorldContext(World);World->RemoveFromRoot();
    }
    auto Root=MakeShared<FJsonObject>();Root->SetNumberField(TEXT("schema_version"),1);Root->SetNumberField(TEXT("passed"),Rows.Num()-Failed);Root->SetNumberField(TEXT("failed"),Failed);Root->SetArrayField(TEXT("checks"),Rows);
    Root->SetStringField(TEXT("scope"),TEXT("Actual public restore/init, engine static mesh/collision cache and paid road owner in isolated synthetic world. No user save slots, physical tuning, DEM edits or forged worker publications. No native RHI image, full road chunk coverage or 16 ms throughput proof."));
    FString JSON;FJsonSerializer::Serialize(Root,TJsonWriterFactory<>::Create(&JSON));IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsoluteReportPath),true);
    const bool Written=FFileHelper::SaveStringToFile(JSON,*AbsoluteReportPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_HYDRAULIC_GROUND_AUDIT_RESULT passed=%d failed=%d report=%s"),Rows.Num()-Failed,Failed,*AbsoluteReportPath);
    return Written&&Failed==0;
}
#endif
