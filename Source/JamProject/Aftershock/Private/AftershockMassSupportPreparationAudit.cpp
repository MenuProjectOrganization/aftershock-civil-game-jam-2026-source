#include "AftershockMassSupportPreparation.h"
#include "Algo/Reverse.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/Class.h"

#if WITH_EDITOR
bool RunAftershockMassSupportPreparationFixtures(const FString& AbsoluteReportPath)
{
    using namespace AftershockMassSupportPreparation;
    TArray<TSharedPtr<FJsonValue>> Rows;int32 Failed=0;
    const auto Check=[&](const TCHAR* Name,bool Passed)
    {
        auto R=MakeShared<FJsonObject>();R->SetStringField(TEXT("name"),Name);R->SetBoolField(TEXT("passed"),Passed);
        Rows.Add(MakeShared<FJsonValueObject>(R));Failed+=Passed?0:1;
        if(Passed){UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MASS_SUPPORT_AUDIT_PASS %s"),Name);}
        else{UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_MASS_SUPPORT_AUDIT_FAIL %s"),Name);}
    };
    TArray<FIntPoint> Initial;
    for(int32 Y=0;Y<125;++Y)for(int32 X=0;X<125;++X)Initial.Add(FIntPoint(X,Y));
    const FIntRect Source(-1000,-1000,1000,1000);FString Why;FPlan Plan;
    Check(TEXT("original125_frame_has_source_backed225_window"),BuildPlan(FIntPoint(125),Source,Initial,Plan,Why)
        &&Plan.RequestedCells==FIntRect(-50,-50,175,175)&&Plan.FinalCells==50625&&Plan.AdditionalCells==35000);
    for(int32 Y=-150;Y<-125;++Y)for(int32 X=-100;X<-75;++X)Initial.Add(FIntPoint(X,Y));
    Check(TEXT("separate_mapped_vent_chunk_retained_in51250_budget"),BuildPlan(FIntPoint(125),Source,Initial,Plan,Why)
        &&Plan.ExistingCells==16250&&Plan.FinalCells==51250&&Plan.AdditionalCells==35000);
    Check(TEXT("window_keeps_negative_chunk_floor_semantics"),Plan.MissingChunks.Contains(FIntPoint(-2,-2))&&!Plan.MissingChunks.Contains(FIntPoint(-3,-2)));
    const FPlan OldPlan=Plan;TArray<FIntPoint> Reverse=Initial;Algo::Reverse(Reverse);FPlan Other;
    Check(TEXT("resident_storage_order_does_not_reroll_requested_chunks"),BuildPlan(FIntPoint(125),Source,Reverse,Other,Why)
        &&Other.RequestedCells==OldPlan.RequestedCells&&Other.MissingChunks==OldPlan.MissingChunks&&Other.FinalCells==OldPlan.FinalCells);
    FPlan Clipped;
    Check(TEXT("source_clip_uses_only_complete_covered_chunks"),BuildPlan(FIntPoint(125),FIntRect(-44,-44,165,165),Initial,Clipped,Why)
        &&Clipped.bClippedToSource&&Clipped.RequestedCells==FIntRect(-25,-25,150,150));
    TArray<FIntPoint> TooMany=Initial;
    for(int32 Y=500;Y<700;++Y)for(int32 X=500;X<600;++X)TooMany.Add(FIntPoint(X,Y));
    Other=OldPlan;
    Check(TEXT("total_residency_cap_rejects_before_plan_mutation"),!BuildPlan(FIntPoint(125),Source,TooMany,Other,Why)
        &&Other.FinalCells==OldPlan.FinalCells&&Other.MissingChunks==OldPlan.MissingChunks);
    TArray<FIntPoint> Duplicates=Initial;Duplicates.Add(Initial[0]);
    Check(TEXT("duplicate_canonical_coordinates_rejected"),!BuildPlan(FIntPoint(125),Source,Duplicates,Other,Why));
    Check(TEXT("absent_source_rejects_without_authored_extension"),!BuildPlan(FIntPoint(125),FIntRect(0,0,0,0),Initial,Other,Why));
    Check(TEXT("negative_ceil_floor_round_trip"),FloorDiv(-1,25)==-1&&FloorDiv(-25,25)==-1&&FloorDiv(-26,25)==-2);
    UWorld* World=GEngine?UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("MassSupportTransactionFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false):nullptr;
    Check(TEXT("isolated_engine_world_created"),World!=nullptr);
    if(World)
    {
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        UTerrainGridSubsystem* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();
        Check(TEXT("canonical_terrain_owner_available"),Terrain!=nullptr);
        if(Terrain)
        {
            // This synthetic fixture validates the canonical transaction API,
            // not real DEM coverage or successful avalanche/landslide motion.
            Terrain->GenerateProceduralTerrain(5,5,1000.f,918273);
            const auto OriginalCells=Terrain->Cells;const uint64 Revision=Terrain->GetTerrainRevision();
            FText Error;FAftershockTerrainChunkExpansion Candidate;
            TArray<FIntPoint> Chunks={FIntPoint(1,0)};
            const bool Began=Terrain->BeginChunkExpansion(Chunks,Candidate,Error);
            Check(TEXT("begin_only_stages_private_candidate"),Began&&Terrain->Cells.Num()==25&&Terrain->GetTerrainRevision()==Revision&&Candidate.GetRequiredCells()==625);
            bool Staged=Began,Bounded=Began;
            while(Staged&&!Candidate.IsReady())
            {
                const int32 Before=Candidate.GetPreparedCells();Staged=Terrain->ContinueChunkExpansion(Candidate,100,Error);
                Bounded&=Candidate.GetPreparedCells()-Before<=100&&Terrain->Cells.Num()==25&&Terrain->GetTerrainRevision()==Revision;
            }
            Check(TEXT("cooperative_generation_never_exceeds_requested_slice"),Staged&&Bounded&&Candidate.GetPreparedCells()==625);
            Check(TEXT("commit_adds_entire_candidate_once"),Staged&&Terrain->CommitChunkExpansion(Candidate,Error)&&Terrain->Cells.Num()==650&&Terrain->GetTerrainRevision()==Revision+1);
            bool Preserved=true;for(int32 I=0;I<OriginalCells.Num();++I)
            {
                FGridCell Old=OriginalCells[I],Now=Terrain->Cells[I];
                // Existing explicit expansion unlock may change ONLY this mask.
                Old.bIsPlayable=Now.bIsPlayable;
                Preserved&=FGridCell::StaticStruct()->CompareScriptStruct(&Old,&Now,0);
            }
            Check(TEXT("commit_preserves_all_original_physical_stocks_and_dem"),Preserved);
            Check(TEXT("same_candidate_cannot_commit_twice"),!Terrain->CommitChunkExpansion(Candidate,Error)&&Terrain->Cells.Num()==650);
            FAftershockTerrainChunkExpansion Stale;Chunks={FIntPoint(2,0)};
            const bool PreparedStale=Terrain->BeginChunkExpansion(Chunks,Stale,Error)&&Terrain->ContinueChunkExpansion(Stale,100,Error);
            FGridCell Changed=Terrain->Cells[0];Changed.DrainageQuality=.25f;Terrain->SetCell(Changed.Coords,Changed);
            Check(TEXT("canonical_edit_invalidates_private_staging_without_append"),PreparedStale&&!Terrain->ContinueChunkExpansion(Stale,100,Error)&&Terrain->Cells.Num()==650);
            FAftershockTerrainChunkExpansion Invalid;Chunks={FIntPoint(3,0),FIntPoint(3,0)};
            Check(TEXT("duplicate_batch_rejected_before_any_live_edit"),!Terrain->BeginChunkExpansion(Chunks,Invalid,Error)&&Terrain->Cells.Num()==650);
            Chunks={FIntPoint(50000,0)};
            Check(TEXT("out_of_world_batch_rejected_before_any_live_edit"),!Terrain->BeginChunkExpansion(Chunks,Invalid,Error)&&Terrain->Cells.Num()==650);
            Terrain->ContinuationGeneratorVersion=3;Terrain->GeographyIndex=INDEX_NONE;
            Chunks={FIntPoint(3,0)};
            Check(TEXT("missing_actual_geodata_rejects_without_partial_commit"),!Terrain->BeginChunkExpansion(Chunks,Invalid,Error)&&Terrain->Cells.Num()==650);
        }
        World->DestroyWorld(false);GEngine->DestroyWorldContext(World);World->RemoveFromRoot();
    }
    auto Root=MakeShared<FJsonObject>();Root->SetNumberField(TEXT("schema_version"),1);
    Root->SetStringField(TEXT("scope"),TEXT("Production support planner and isolated canonical cooperative transaction. Synthetic API fixture is explicitly not a real-DEM, hydraulic rebind, solver completion, save/load, RHI or CPU throughput proof."));
    Root->SetNumberField(TEXT("passed"),Rows.Num()-Failed);Root->SetNumberField(TEXT("failed"),Failed);Root->SetArrayField(TEXT("checks"),Rows);
    FString JSON;FJsonSerializer::Serialize(Root,TJsonWriterFactory<>::Create(&JSON));IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsoluteReportPath),true);
    const bool Saved=FFileHelper::SaveStringToFile(JSON,*AbsoluteReportPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MASS_SUPPORT_AUDIT_RESULT passed=%d failed=%d report=%s"),Rows.Num()-Failed,Failed,*AbsoluteReportPath);
    return Saved&&Failed==0;
}
#endif
