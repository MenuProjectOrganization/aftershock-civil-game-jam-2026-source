#include "AftershockChunkAuditCommandlet.h"
#if WITH_EDITOR
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include <limits>

namespace
{
	struct FChunkCheck { FString Name; bool bPassed; };
	struct FChunkWorld
	{
		UWorld* World=nullptr;
		~FChunkWorld() { if (World) { World->DestroyWorld(false); if (GEngine) GEngine->DestroyWorldContext(World); World->RemoveFromRoot(); } }
	};
	bool SameCell(const FGridCell& A,const FGridCell& B) { return FGridCell::StaticStruct()->CompareScriptStruct(&A,&B,0); }
	bool SameCells(const TArray<FGridCell>& A,const TArray<FGridCell>& B)
	{
		if (A.Num()!=B.Num()) return false;
		for (int32 I=0;I<A.Num();++I) if (!SameCell(A[I],B[I])) return false;
		return true;
	}
}

UAftershockChunkAuditCommandlet::UAftershockChunkAuditCommandlet()
{ IsClient=false; IsServer=false; IsEditor=true; LogToConsole=true; }

int32 UAftershockChunkAuditCommandlet::Main(const FString& Params)
{
	TArray<FChunkCheck> Checks; int32 Failed=0;
	const auto Check=[&](const TCHAR* Name,bool bPassed)
	{
		Checks.Add({Name,bPassed}); Failed+=bPassed ? 0 : 1;
		if (bPassed) { UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHUNK_AUDIT_PASS %s"),Name); }
		else { UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_CHUNK_AUDIT_FAIL %s"),Name); }
	};
	const auto Finish=[&]()
	{
		const FString Directory=FPaths::ProjectSavedDir()/TEXT("PresentationValidation"); IFileManager::Get().MakeDirectory(*Directory,true);
		FString Json=FString::Printf(TEXT("{\n\"schema_version\":1,\n\"scope\":\"Native canonical global coordinates, stable append storage, negative chunks, explicit mask unlock, deterministic authored continuation and terrain snapshots. Resident memory cap is not infinite-world streaming. No GPU/pixel/build or live car claim.\",\n\"passed\":%d,\n\"failed\":%d,\n\"checks\":[\n"),Checks.Num()-Failed,Failed);
		for (int32 I=0;I<Checks.Num();++I) Json+=FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}%s\n"),*Checks[I].Name,Checks[I].bPassed ? TEXT("true") : TEXT("false"),I+1<Checks.Num() ? TEXT(",") : TEXT(""));
		Json+=TEXT("]\n}\n");
		if (!FFileHelper::SaveStringToFile(Json,*(Directory/TEXT("chunk_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return 2;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHUNK_AUDIT_RESULT passed=%d failed=%d"),Checks.Num()-Failed,Failed); return Failed==0 ? 0 : 1;
	};
	Check(TEXT("engine_initialized"),GEngine!=nullptr); if (!GEngine) return Finish();
	FChunkWorld Fixture;
	Fixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("AftershockChunkFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("isolated_world_created"),Fixture.World!=nullptr); if (!Fixture.World) return Finish();
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(Fixture.World);
	const auto MakeTerrain=[&]() { return NewObject<UTerrainGridSubsystem>(Fixture.World); };
	TStrongObjectPtr<UTerrainGridSubsystem> Holder(MakeTerrain()),OtherHolder(MakeTerrain());
	UTerrainGridSubsystem* Terrain=Holder.Get(); UTerrainGridSubsystem* Other=OtherHolder.Get();
	Check(TEXT("terrain_owners_created"),Terrain && Other); if (!Terrain || !Other) return Finish();
	Terrain->GenerateProceduralTerrain(50,50,500.f,1337); Other->GenerateProceduralTerrain(50,50,500.f,1337);
	Check(TEXT("initial_50x50_seeded_cells_exactly_reproducible"),SameCells(Terrain->Cells,Other->Cells));
	Check(TEXT("initial_four_chunks_each625_cells"),Terrain->GetResidentChunks().Num()==4 && !Terrain->GetResidentChunks().ContainsByPredicate([](const FAftershockTerrainChunkState& C) { return C.CellCount!=625 || C.bContainsAuthoredContinuation; }));
	Check(TEXT("initial_frame_and_bounds_unchanged"),Terrain->GridWidth==50 && Terrain->GridHeight==50 && Terrain->GetResidentCellBounds()==FIntRect(0,0,50,50));
	Check(TEXT("initial_oval_remains_locked_until_expansion"),!Terrain->bInitialMaskUnlocked && !Terrain->GetCell(FIntPoint(0,0)).bIsPlayable);
	const FIntPoint Cases[]={FIntPoint(-1,-1),FIntPoint(-25,-25),FIntPoint(-26,24),FIntPoint(25,-26)};
	const FIntPoint ExpectedChunk[]={FIntPoint(-1,-1),FIntPoint(-1,-1),FIntPoint(-2,0),FIntPoint(1,-2)};
	const FIntPoint ExpectedLocal[]={FIntPoint(24,24),FIntPoint(0,0),FIntPoint(24,24),FIntPoint(0,24)};
	bool bNegativeFloor=true;
	for (int32 I=0;I<UE_ARRAY_COUNT(Cases);++I) bNegativeFloor&=UTerrainGridSubsystem::CellToChunk(Cases[I])==ExpectedChunk[I] && UTerrainGridSubsystem::CellToChunkLocal(Cases[I])==ExpectedLocal[I];
	Check(TEXT("negative_floor_division_and_positive_chunk_local"),bNegativeFloor);
	Check(TEXT("missing_coord_never_aliases_initial_storage"),Terrain->CoordToIndex(FIntPoint(50,0))==INDEX_NONE && Terrain->CoordToIndex(FIntPoint(-1,-1))==INDEX_NONE);
	Check(TEXT("missing_cell_is_not_playable"),!Terrain->GetCell(FIntPoint(-1,-1)).bIsPlayable && !Terrain->IsPlayableCoord(FIntPoint(-1,-1)));
	Check(TEXT("invalid_storage_coord_has_max_sentinel_not_minus_one"),Terrain->IndexToCoord(INDEX_NONE)==FIntPoint(MAX_int32,MAX_int32));
	bool bInitialRoundTrip=true;
	for (int32 I=0;I<Terrain->Cells.Num();++I) bInitialRoundTrip&=Terrain->CoordToIndex(Terrain->IndexToCoord(I))==I;
	Check(TEXT("all2500_initial_storage_ids_roundtrip"),bInitialRoundTrip);
	FGridCell Investment=Terrain->GetCell(FIntPoint(8,8)); Investment.DrainageQuality=.8f; Investment.RiverCapacity=2.3f; Investment.bGroundSurveyed=true;
	Terrain->SetCell(Investment.Coords,Investment);
	const TArray<FGridCell> Initial=Terrain->Cells; const uint64 BeforeRevision=Terrain->GetTerrainRevision();
	int32 StorageEvents=0; bool bEventSawCommittedState=true;
	Terrain->OnTerrainStorageChanged.AddLambda([&](uint64 Revision)
	{
		++StorageEvents; bEventSawCommittedState&=Revision==Terrain->GetTerrainRevision() && Terrain->GetResidentChunks().Num()>=5 && Terrain->bInitialMaskUnlocked;
	});
	FText Error;
	Check(TEXT("first_east_chunk_generated"),Terrain->EnsureChunkGenerated(FIntPoint(2,0),Error));
	Check(TEXT("first_append625_cells_with_single_revision_and_event"),Terrain->Cells.Num()==3125 && StorageEvents==1 && Terrain->GetTerrainRevision()==BeforeRevision+1 && bEventSawCommittedState);
	bool bPrefixPreserved=true; int32 ExplicitUnlocks=0;
	for (int32 I=0;I<Initial.Num();++I)
	{
		FGridCell Expected=Initial[I]; if (!Expected.bIsOcean && !Expected.bIsPlayable) { Expected.bIsPlayable=true; ++ExplicitUnlocks; }
		bPrefixPreserved&=SameCell(Expected,Terrain->Cells[I]) && Terrain->CoordToIndex(Initial[I].Coords)==I;
	}
	Check(TEXT("initial_physical_and_investment_data_preserved_only_explicit_mask_unlock"),bPrefixPreserved && ExplicitUnlocks>0 && Terrain->bInitialMaskUnlocked);
	Check(TEXT("second_adjacent_east_chunk_generated"),Terrain->EnsureChunkGenerated(FIntPoint(2,1),Error));
	Check(TEXT("negative_west_chunk_generated"),Terrain->EnsureChunkGenerated(FIntPoint(-1,0),Error));
	Check(TEXT("negative_both_axes_chunk_generated"),Terrain->EnsureChunkGenerated(FIntPoint(-1,-1),Error));
	Check(TEXT("original_generation_frame_not_stretched_by_append"),Terrain->GridWidth==50 && Terrain->GridHeight==50 && Terrain->Cells.Num()==5000 && Terrain->GetResidentCellBounds()==FIntRect(-25,-25,75,50));
	Check(TEXT("negative_minus_one_is_real_resident_coord"),Terrain->IsValidCoord(FIntPoint(-1,-1)) && Terrain->IndexToCoord(Terrain->CoordToIndex(FIntPoint(-1,-1)))==FIntPoint(-1,-1));
	Check(TEXT("negative_grid_world_roundtrip"),Terrain->WorldToGrid(Terrain->GridToWorld(FIntPoint(-1,-1)))==FIntPoint(-1,-1));
	bool bAllRoundTrip=true,bAuthoredFlags=true;
	for (int32 I=0;I<Terrain->Cells.Num();++I)
	{
		bAllRoundTrip&=Terrain->CoordToIndex(Terrain->IndexToCoord(I))==I;
		if (I>=Initial.Num()) bAuthoredFlags&=Terrain->Cells[I].bAuthoredContinuation && !Terrain->Cells[I].bHasRealElevationSample && Terrain->Cells[I].RealElevationMetres==0.f;
	}
	Check(TEXT("all5000_storage_ids_roundtrip_across_gaps"),bAllRoundTrip);
	Check(TEXT("new_terrain_never_claims_real_dem"),bAuthoredFlags);
	Check(TEXT("sparse_bounds_hole_is_not_resident"),!Terrain->IsValidCoord(FIntPoint(20,-1)) && !Terrain->GetCell(FIntPoint(20,-1)).bIsPlayable);
	const uint64 BeforeIdempotent=Terrain->GetTerrainRevision(); const int32 BeforeEvents=StorageEvents;
	Check(TEXT("existing_full_chunk_is_idempotent"),Terrain->EnsureChunkGenerated(FIntPoint(2,0),Error) && Terrain->GetTerrainRevision()==BeforeIdempotent && StorageEvents==BeforeEvents && Terrain->Cells.Num()==5000);
	Check(TEXT("invalid_huge_chunk_rejected_atomically"),!Terrain->EnsureChunkGenerated(FIntPoint(MAX_int32,0),Error) && Terrain->GetTerrainRevision()==BeforeIdempotent && StorageEvents==BeforeEvents && Terrain->Cells.Num()==5000);
	const FIntPoint Chunks[]={FIntPoint(-1,-1),FIntPoint(-1,0),FIntPoint(2,1),FIntPoint(2,0)};
	for (FIntPoint Chunk:Chunks) Other->EnsureChunkGenerated(Chunk,Error);
	bool bOrderIndependent=true;
	for (int32 I=2500;I<Terrain->Cells.Num();++I) bOrderIndependent&=SameCell(Terrain->Cells[I],Other->GetCell(Terrain->Cells[I].Coords));
	Check(TEXT("authored_continuation_deterministic_across_generation_order"),bOrderIndependent);
	FAftershockTerrainSaveState Snapshot; Terrain->CaptureSaveState(Snapshot);
	Check(TEXT("snapshot_version2_manifest_and_mask_metadata"),Snapshot.TerrainStorageVersion==2 && Snapshot.ChunkSize==25 && Snapshot.ContinuationGeneratorVersion==1 && Snapshot.Chunks.Num()==8 && Snapshot.bInitialMaskUnlocked);
	Check(TEXT("cold_cdo_snapshot_preflight_is_pure_and_valid"),GetDefault<UTerrainGridSubsystem>()->ValidateSaveState(Snapshot,Error));
	int32 RestoreEvents=0; Other->OnTerrainStorageChanged.AddLambda([&](uint64) { ++RestoreEvents; });
	Check(TEXT("terrain_bulk_restore_preserves_order_and_every_cell_property"),Other->RestoreSaveState(Snapshot,Error) && SameCells(Terrain->Cells,Other->Cells));
	Check(TEXT("bulk_restore_defers_callbacks"),RestoreEvents==0);
	Other->NotifyTerrainStorageRestored(); Check(TEXT("restore_notification_after_bulk_commit"),RestoreEvents==1);
	const TArray<FGridCell> BeforeInvalid=Other->Cells; const uint64 BeforeInvalidRevision=Other->GetTerrainRevision();
	const auto InvalidRejected=[&](FAftershockTerrainSaveState Bad)
	{
		return !GetDefault<UTerrainGridSubsystem>()->ValidateSaveState(Bad,Error) && !Other->RestoreSaveState(Bad,Error)
			&& SameCells(Other->Cells,BeforeInvalid) && Other->GetTerrainRevision()==BeforeInvalidRevision && RestoreEvents==1;
	};
	FAftershockTerrainSaveState Bad=Snapshot; Bad.Cells.Last().Coords=Bad.Cells[0].Coords;
	Check(TEXT("duplicate_coord_rejected_before_mutation"),InvalidRejected(Bad));
	Bad=Snapshot; Bad.Cells.Last().bHasRealElevationSample=true;
	Check(TEXT("fabricated_continuation_dem_rejected_before_mutation"),InvalidRejected(Bad));
	Bad=Snapshot; Bad.Chunks[0].CellCount++;
	Check(TEXT("manifest_count_mismatch_rejected_before_mutation"),InvalidRejected(Bad));
	Bad=Snapshot; Bad.Cells.Last().Elevation=std::numeric_limits<float>::quiet_NaN();
	Check(TEXT("nan_height_rejected_before_mutation"),InvalidRejected(Bad));
	Bad=Snapshot; Bad.bInitialMaskUnlocked=false;
	Check(TEXT("missing_expansion_mask_policy_rejected_before_mutation"),InvalidRejected(Bad));
	Bad=Snapshot; Bad.TerrainStorageVersion=99;
	Check(TEXT("unsupported_terrain_storage_version_rejected_before_mutation"),InvalidRejected(Bad));
	Terrain->GenerateProceduralTerrain(12,12,500,1337);
	Check(TEXT("small_legacy_initial_frame_supported"),Terrain->Cells.Num()==144 && Terrain->GetResidentChunks().Num()==1);
	Check(TEXT("partial_initial_chunk_fills_missing_cells_without_reorder"),Terrain->EnsureChunkGenerated(FIntPoint(0,0),Error) && Terrain->Cells.Num()==625 && Terrain->CoordToIndex(FIntPoint(11,11))==143 && Terrain->IsValidCoord(FIntPoint(24,24)) && Terrain->GridWidth==12);
	Terrain->GenerateProceduralTerrain(50,50,500,1337);
	// V1 compatibility fixture: these assertions preserve the original normalized
	// heights and authored continuation. V2 source-bounded expansion is covered
	// independently by AftershockRealGeographyAudit.
	const bool bApplied=Terrain->ApplyLegacyOfflineGeography(3,0,true); const TArray<FGridCell> GeographicInitial=Terrain->Cells;
	Check(TEXT("geography_fixture_ready"),bApplied && Terrain->bUsesRegionalLayout && GeographicInitial[0].bHasRealElevationSample);
	Check(TEXT("geographic_negative_chunk_generated"),Terrain->EnsureChunkGenerated(FIntPoint(-1,0),Error));
	bool bRealMetadataPreserved=true,bNoInventedDem=true;
	for (int32 I=0;I<GeographicInitial.Num();++I) bRealMetadataPreserved&=Terrain->Cells[I].RealElevationMetres==GeographicInitial[I].RealElevationMetres && Terrain->Cells[I].bHasRealElevationSample && Terrain->Cells[I].Elevation==GeographicInitial[I].Elevation;
	for (int32 I=GeographicInitial.Num();I<Terrain->Cells.Num();++I) bNoInventedDem&=!Terrain->Cells[I].bHasRealElevationSample && Terrain->Cells[I].bAuthoredContinuation && Terrain->Cells[I].RealElevationMetres==0.f;
	Check(TEXT("source_dem_and_original_normalized_heights_unchanged"),bRealMetadataPreserved);
	Check(TEXT("outside_focus_authored_not_claimed_as_copernicus"),bNoInventedDem);
	Check(TEXT("west_coast_continues_as_nonplayable_ocean"),Terrain->GetCell(FIntPoint(-1,20)).bIsOcean && !Terrain->GetCell(FIntPoint(-1,20)).bIsPlayable);
	Terrain->CaptureSaveState(Snapshot); Check(TEXT("geographic_expanded_snapshot_valid"),Terrain->ValidateSaveState(Snapshot,Error));
	Terrain->GenerateProceduralTerrain(256,256,500,1337); const uint64 AtCap=Terrain->GetTerrainRevision();
	Check(TEXT("resident_memory_budget_is_explicit_and_atomic"),Terrain->Cells.Num()==UTerrainGridSubsystem::MaximumResidentCells && !Terrain->EnsureChunkGenerated(FIntPoint(-1,0),Error) && Terrain->Cells.Num()==65536 && Terrain->GetTerrainRevision()==AtCap && !Terrain->bInitialMaskUnlocked);
	return Finish();
}
#else
UAftershockChunkAuditCommandlet::UAftershockChunkAuditCommandlet() {}
int32 UAftershockChunkAuditCommandlet::Main(const FString&) { return 1; }
#endif
