#include "AftershockFloodVisualAuditCommandlet.h"

#if WITH_EDITOR
#include "AftershockTerrainSurface.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/DisasterPresentationSubsystem.h"
#include "AftershockGlobeSurface.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	struct FFloodCheck { FString Name; bool Passed; FString Detail; };
	struct FFloodFixtureResult
	{
		FString Name;
		FAftershockFloodSurfaceStats Stats;
		int32 VisibleDepthCells = 0;
		int32 ActualMeshTriangles = 0;
		bool Built = false;
		double Milliseconds = 0.0;
	};
	struct FScopedFloodWorld
	{
		UWorld* World = nullptr;
		~FScopedFloodWorld()
		{
			if (!World) return;
			World->DestroyWorld(false);
			if (GEngine) GEngine->DestroyWorldContext(World);
			World->RemoveFromRoot();
		}
	};
	FString EscapeFloodJson(FString Value)
	{
		Value.ReplaceInline(TEXT("\\"), TEXT("\\\\")); Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Value.ReplaceInline(TEXT("\n"), TEXT("\\n")); Value.ReplaceInline(TEXT("\r"), TEXT("\\r")); Value.ReplaceInline(TEXT("\t"), TEXT("\\t"));
		return Value;
	}
}

UAftershockFloodVisualAuditCommandlet::UAftershockFloodVisualAuditCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UAftershockFloodVisualAuditCommandlet::Main(const FString& Params)
{
	TArray<FFloodCheck> Checks;
	TArray<FFloodFixtureResult> Results;
	int32 Failed = 0;
	const auto Check = [&](const FString& Name, bool Passed, const FString& Detail)
	{
		Checks.Add({ Name, Passed, Detail }); Failed += Passed ? 0 : 1;
		if (Passed) { UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_FLOOD_VISUAL_AUDIT_PASS %s: %s"), *Name, *Detail); }
		else { UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_FLOOD_VISUAL_AUDIT_FAIL %s: %s"), *Name, *Detail); }
	};
	const auto Finish = [&]()
	{
		const FString Directory = FPaths::ProjectSavedDir() / TEXT("PresentationValidation");
		IFileManager::Get().MakeDirectory(*Directory, true);
		const FString Path = Directory / TEXT("flood_visual_audit.json");
		FString Json = FString::Printf(TEXT("{\n\"schema_version\":1,\n\"utc\":\"%s\",\n\"scope\":\"Actual C++ BuildAftershockFloodSurface and Engine fast StaticMesh build on an isolated24x24 canonical-grid fixture. Checks display reconstruction, volume bookkeeping, output depths, mesh counts/bounds and input immutability. No pixel, shader visual appearance, rain renderer, GPU timing, physical fluid accuracy, casualties or packaged-build claim.\",\n\"passed\":%d,\n\"failed\":%d,\n\"fixtures\":[\n"),
			*FDateTime::UtcNow().ToIso8601(), Checks.Num() - Failed, Failed);
		for (int32 I = 0; I < Results.Num(); ++I)
		{
			const FFloodFixtureResult& R = Results[I];
			Json += FString::Printf(TEXT("{\"name\":\"%s\",\"built\":%s,\"positive_playable_cells\":%d,\"wet_cells\":%d,\"land_cells\":%d,\"returned_depth_cells\":%d,\"top_triangles\":%d,\"front_triangles\":%d,\"actual_mesh_triangles\":%d,\"max_display_depth_cm\":%.6f,\"initial_column_sum_cm\":%.9f,\"final_column_sum_cm\":%.9f,\"build_cpu_ms\":%.4f}%s\n"),
				*EscapeFloodJson(R.Name), R.Built ? TEXT("true") : TEXT("false"), R.Stats.CanonicalPositiveCells, R.Stats.ReconstructedWetCells, R.Stats.ReconstructedLandCells,
				R.VisibleDepthCells, R.Stats.SurfaceTriangles, R.Stats.FrontTriangles, R.ActualMeshTriangles, R.Stats.MaximumDisplayDepthCm,
				R.Stats.InitialDisplayColumnSumCm, R.Stats.FinalDisplayColumnSumCm, R.Milliseconds, I + 1 < Results.Num() ? TEXT(",") : TEXT(""));
		}
		Json += TEXT("],\n\"checks\":[\n");
		for (int32 I = 0; I < Checks.Num(); ++I)
		{
			const FFloodCheck& C = Checks[I];
			Json += FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s,\"detail\":\"%s\"}%s\n"),
				*EscapeFloodJson(C.Name), C.Passed ? TEXT("true") : TEXT("false"), *EscapeFloodJson(C.Detail), I + 1 < Checks.Num() ? TEXT(",") : TEXT(""));
		}
		Json += TEXT("]\n}\n");
		if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return 2;
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_FLOOD_VISUAL_AUDIT_RESULT fixtures=%d passed=%d failed=%d report=%s"), Results.Num(), Checks.Num() - Failed, Failed, *Path);
		return Failed == 0 ? 0 : 1;
	};
	Check(TEXT("engine_initialized"), GEngine != nullptr, TEXT("The initialized Engine, not a source-equivalent Python model, executes every mesh fixture."));
	if (!GEngine) return Finish();
	FScopedFloodWorld Fixture;
	Fixture.World = UWorld::CreateWorld(EWorldType::Game, false, FName(TEXT("AftershockFloodVisualFixture")), GetTransientPackage(), true, ERHIFeatureLevel::SM5, nullptr, false);
	Check(TEXT("isolated_world_created"), Fixture.World != nullptr, TEXT("No gameplay map, canonical live save or project asset is modified."));
	if (!Fixture.World) return Finish();
	UWorld* World = Fixture.World;
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game); Context.SetCurrentWorld(World);
	UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>();
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/Aftershock/Materials/M_DisasterFloodVolume.M_DisasterFloodVolume"));
	Check(TEXT("canonical_grid_and_authored_material_loaded"), Terrain && Material && Material->GetBlendMode() == BLEND_Translucent,
		TEXT("Requires the real project-owned translucent M_DisasterFloodVolume material; missing assets fail instead of using a default material."));
	if (!Terrain || !Material) return Finish();
	FActorSpawnParameters Spawn; Spawn.ObjectFlags |= RF_Transient;
	AActor* Owner = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	Check(TEXT("transient_mesh_owner_created"), Owner != nullptr, TEXT("A single no-tick fixture actor owns the transient output mesh/component."));
	if (!Owner) return Finish();
	Owner->PrimaryActorTick.bCanEverTick = false;
	USceneComponent* Root = NewObject<USceneComponent>(Owner); Owner->SetRootComponent(Root); Root->RegisterComponent();
	UStaticMeshComponent* Surface = NewObject<UStaticMeshComponent>(Owner, TEXT("FloodAuditSurface"));
	Surface->SetupAttachment(Root); Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Surface->SetCanEverAffectNavigation(false); Surface->SetMaterial(0, Material); Surface->RegisterComponent();
	constexpr int32 W = 24, H = 24, Total = W * H;
	Terrain->GenerateProceduralTerrain(W,H,500.f,1337); Terrain->GridOrigin = FVector::ZeroVector;
	const auto InputHash = [&]()
	{
		uint32 Hash = 0;
		for (const FGridCell& C : Terrain->Cells)
		{
			Hash = HashCombine(Hash, GetTypeHash(C.Coords)); Hash = HashCombine(Hash, GetTypeHash(C.Elevation));
			Hash = HashCombine(Hash, GetTypeHash(C.bIsRiverbed)); Hash = HashCombine(Hash, GetTypeHash(C.bIsPlayable));
			Hash = HashCombine(Hash, GetTypeHash(C.RiverCapacity)); Hash = HashCombine(Hash, GetTypeHash(C.DrainageQuality));
			Hash = HashCombine(Hash, GetTypeHash(C.FloodDepth)); Hash = HashCombine(Hash, GetTypeHash(C.bHasRoad));
			Hash = HashCombine(Hash, GetTypeHash(C.OccupyingBuildingID));
		}
		return Hash;
	};
	// Cases0..5: flat weak rain, river/valley overflow, below-capacity rain,
	// pluvial bowl, unplayable half-plane, then dry clear after a visible mesh.
	const TCHAR* Names[] = { TEXT("weak_flat_rain_no_blanket"), TEXT("valley_overflow_high_ground_dry"), TEXT("below_capacity_no_land_flood"),
		TEXT("pluvial_bowl_collects_runoff"), TEXT("unplayable_mask_stays_dry"), TEXT("empty_map_clears_previous_volume") };
	for (int32 Case = 0; Case < 6; ++Case)
	{
		Terrain->GenerateProceduralTerrain(W,H,500.f,1337);
		TMap<FIntPoint, float> Severity;
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				FGridCell C; C.Coords = FIntPoint(X, Y); C.Elevation = 200.f;
				C.bIsPlayable = true; C.RiverCapacity = 1.f;
				float S = Case == 0 ? 20.f : 36.f;
				if (Case == 1 || Case == 2)
				{
					C.Elevation = 30.f + FMath::Min(300.f, static_cast<float>(FMath::Abs(X - W/2) * 60));
					C.bIsRiverbed = X == W/2;
					S = Case == 1 ? (C.bIsRiverbed ? 60.f : 36.f) : (C.bIsRiverbed ? 9.f : 5.4f);
				}
				else if (Case == 3) C.Elevation = 100.f + FMath::Min(180.f, static_cast<float>((FMath::Square(X - W/2) + FMath::Square(Y - H/2)) * 6));
				else if (Case == 4) { C.Elevation = 0.f; C.bIsPlayable = X < W/2; S = 100.f; }
				Terrain->Cells[X + Y * W] = C;
				if (Case != 5) Severity.Add(C.Coords, S);
			}
		}
		const uint32 BeforeHash = InputHash();
		const TMap<FIntPoint, float> SeverityBefore = Severity;
		FAftershockFloodSurfaceStats Stats;
		TMap<FIntPoint, float> Depths;
		const double Start = FPlatformTime::Seconds();
		const bool Built = BuildAftershockFloodSurface(Owner, Terrain, Severity, Surface, &Stats, &Depths);
		const double Milliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
		UStaticMesh* Mesh = Surface->GetStaticMesh();
		const int32 ActualTriangles = Mesh && Mesh->HasValidRenderData() ? Mesh->GetNumTriangles(0) : 0;
		Results.Add({ Names[Case], Stats, Depths.Num(), ActualTriangles, Built, Milliseconds });
		const FString Prefix = FString(Names[Case]) + TEXT(".");
		const double VolumeError = FMath::Abs(Stats.InitialDisplayColumnSumCm - Stats.FinalDisplayColumnSumCm);
		const double VolumeTolerance = FMath::Max(0.05, Stats.InitialDisplayColumnSumCm * 0.0001);
		Check(Prefix + TEXT("display_volume_conserved"), FMath::IsFinite(Stats.InitialDisplayColumnSumCm) && FMath::IsFinite(Stats.FinalDisplayColumnSumCm)
			&& Stats.InitialDisplayColumnSumCm >= 0.0 && Stats.FinalDisplayColumnSumCm >= 0.0 && VolumeError <= VolumeTolerance,
			FString::Printf(TEXT("initial=%.9f final=%.9f absoluteError=%.9f tolerance=%.9f columnSumCm; equal-size-cell display volume only"),
				Stats.InitialDisplayColumnSumCm, Stats.FinalDisplayColumnSumCm, VolumeError, VolumeTolerance));
		bool OutputsFinite = FMath::IsFinite(Stats.MaximumDisplayDepthCm) && Stats.MaximumDisplayDepthCm >= 0.f;
		bool MaskRespected = true, HighGroundDry = true;
		for (const TPair<FIntPoint, float>& Pair : Depths)
		{
			OutputsFinite &= FMath::IsFinite(Pair.Value) && Pair.Value >= 12.f && Pair.Value <= 504.f;
			MaskRespected &= Terrain->IsPlayableCoord(Pair.Key) && Severity.FindRef(Pair.Key) > 0.f;
			if (Case == 1 && Terrain->GetCell(Pair.Key).Elevation >= 270.f) HighGroundDry = false;
		}
		if (Mesh) OutputsFinite &= !Mesh->GetBoundingBox().GetCenter().ContainsNaN() && !Mesh->GetBoundingBox().GetExtent().ContainsNaN();
		Check(Prefix + TEXT("finite_nonnegative_outputs_and_canonical_mask"), OutputsFinite && MaskRespected,
			FString::Printf(TEXT("returnedDepthCells=%d maxDisplayDepthCm=%.6f; checks returned depths and Engine mesh bounds, not inaccessible discarded vertex CPU buffers"), Depths.Num(), Stats.MaximumDisplayDepthCm));
		bool InputUnchanged = BeforeHash == InputHash() && Severity.Num() == SeverityBefore.Num();
		for (const TPair<FIntPoint, float>& Pair : SeverityBefore) InputUnchanged &= Severity.FindRef(Pair.Key) == Pair.Value;
		Check(Prefix + TEXT("canonical_inputs_unchanged"), InputUnchanged, TEXT("The actual helper did not change the fixture's canonical elevation, river, drainage, flood, road, occupancy or input severity data."));
		const bool ExpectVisible = Case == 1 || Case == 3 || Case == 4;
		if (ExpectVisible)
		{
			Check(Prefix + TEXT("actual_top_and_front_mesh_built"), Built && Mesh && ActualTriangles > 0 && Stats.SurfaceTriangles > 0 && Stats.FrontTriangles > 0
				&& ActualTriangles == Stats.SurfaceTriangles + Stats.FrontTriangles && Depths.Num() == Stats.ReconstructedWetCells && Depths.Num() > 0 && Depths.Num() < Total,
				FString::Printf(TEXT("built=%d visibleCells=%d/%d actualTriangles=%d top=%d front=%d cpuMs=%.4f"), Built, Depths.Num(), Total, ActualTriangles, Stats.SurfaceTriangles, Stats.FrontTriangles, Milliseconds));
			if (Case == 1) Check(Prefix + TEXT("river_overtops_into_valley_not_high_ground"), HighGroundDry && Stats.ReconstructedLandCells > 0 && Depths.Num() < Total/3,
				TEXT("Real reconstructed river overflow reaches some valley land while every returned cell at elevation>=270cm remains dry."));
			if (Case == 3) Check(Prefix + TEXT("depression_traps_some_pluvial_water"), Stats.ReconstructedLandCells > 0 && Depths.Num() < Total/3 && Depths.Contains(FIntPoint(W/2, H/2)),
				TEXT("Uniform severity36 leaves a localized depression pond including the bowl centre, rather than a map-wide sheet."));
			if (Case == 4) Check(Prefix + TEXT("mesh_stops_at_unplayable_half_plane"), Depths.Num() == Total/2 && Mesh && Mesh->GetBoundingBox().Max.X <= Terrain->CellSize * W/2 + 0.25f,
				TEXT("Right-half cells are unplayable; returned depths and actual Engine mesh XY extent remain in the left half."));
		}
		else
		{
			Check(Prefix + TEXT("no_inundation_mesh_or_visible_depth"), !Built && !Mesh && Depths.IsEmpty() && Stats.ReconstructedWetCells == 0,
				FString::Printf(TEXT("built=%d mesh=%d depthCells=%d maxResidualColumnCm=%.6f; no visible blue sheet"), Built, Mesh != nullptr, Depths.Num(), Stats.MaximumDisplayDepthCm));
			if (Case == 2) Check(Prefix + TEXT("below_capacity_has_no_land_inundation"), Stats.ReconstructedLandCells == 0 && Stats.MaximumDisplayDepthCm <= 12.01f,
				TEXT("Canonical-equivalent river severity9 is below capacity gate18.75; only the hidden12cm normal-river baseline remains."));
		}
	}
	// Production helper over negative chunks, sparse holes and clipped far water.
	Terrain->GenerateProceduralTerrain(25,25,500.f,1337);
	const auto InitialGlobe=GetAftershockGlobeLayout(*Terrain);FText ChunkError;
	Check(TEXT("chunk_fixture_generated"),Terrain->EnsureChunkGenerated(FIntPoint(-1,0),ChunkError)&&Terrain->EnsureChunkGenerated(FIntPoint(-1,-1),ChunkError),ChunkError.ToString());
	const auto FillChunkFixture=[&](bool bGradient)
	{
		TMap<FIntPoint,float> Severity;
		for(FGridCell& Cell:Terrain->Cells){const FIntPoint Coord=Cell.Coords;Cell=FGridCell();Cell.Coords=Coord;Cell.bIsPlayable=true;Cell.Elevation=bGradient?100.f+FMath::Abs(Coord.X+2)*2.f+FMath::Abs(Coord.Y-5)*.5f:200.f;Severity.Add(Coord,bGradient?85.f:100.f);}
		return Severity;
	};
	TMap<FIntPoint,float> ChunkSeverity=FillChunkFixture(false),ChunkDepths;FAftershockFloodSurfaceStats ChunkStats;
	const bool ChunkBuilt=BuildAftershockFloodSurface(Owner,Terrain,ChunkSeverity,Surface,&ChunkStats,&ChunkDepths);
	Check(TEXT("negative_chunks_and_sparse_hole_use_canonical_membership"),ChunkBuilt&&ChunkDepths.Contains(FIntPoint(-1,-1))&&ChunkDepths.Contains(FIntPoint(-1,0))&&ChunkDepths.Contains(FIntPoint(0,0))&&!ChunkDepths.Contains(FIntPoint(0,-1))&&!Terrain->IsValidCoord(FIntPoint(0,-1)),TEXT("Three actual resident chunks form an L; (-1,-1) is land and the missing fourth chunk stays absent."));
	Check(TEXT("chunk_flood_display_volume_conserved"),FMath::Abs(ChunkStats.InitialDisplayColumnSumCm-ChunkStats.FinalDisplayColumnSumCm)<FMath::Max(.05,ChunkStats.InitialDisplayColumnSumCm*.0001),TEXT("The unchanged160-iteration conservative display reconstruction spans appended cells."));
	const auto ExpandedGlobe=GetAftershockGlobeLayout(*Terrain);
	Check(TEXT("chunk_growth_does_not_move_or_resize_globe"),InitialGlobe.Center.Equals(ExpandedGlobe.Center,0)&&InitialGlobe.RadiusCm==ExpandedGlobe.RadiusCm,TEXT("The original generation frame remains the shared globe/camera anchor."));
	ChunkSeverity=FillChunkFixture(true);TMap<FIntPoint,float> OrderA,OrderB;FAftershockFloodSurfaceStats StatsA,StatsB;
	BuildAftershockFloodSurface(Owner,Terrain,ChunkSeverity,Surface,&StatsA,&OrderA);
	Terrain->GenerateProceduralTerrain(25,25,500.f,1337);const bool ReverseBuilt=Terrain->EnsureChunkGenerated(FIntPoint(-1,-1),ChunkError)&&Terrain->EnsureChunkGenerated(FIntPoint(-1,0),ChunkError);
	ChunkSeverity=FillChunkFixture(true);BuildAftershockFloodSurface(Owner,Terrain,ChunkSeverity,Surface,&StatsB,&OrderB);
	bool SameOrder=ReverseBuilt&&OrderA.Num()==OrderB.Num()&&OrderA.Num()>0;
	for(const auto& Pair:OrderA){const float* Other=OrderB.Find(Pair.Key);SameOrder&=Other&&FMath::IsNearlyEqual(Pair.Value,*Other,.00001f);}
	Check(TEXT("flood_reconstruction_is_independent_of_chunk_append_order"),SameOrder&&StatsA.InitialDisplayColumnSumCm==StatsB.InitialDisplayColumnSumCm&&StatsA.FinalDisplayColumnSumCm==StatsB.FinalDisplayColumnSumCm,TEXT("Non-flat elevations force cross-cell flux. GlobalY/X edge traversal yields matching depths and bookkeeping for reversed chunk append order."));
	const bool FarGenerated=Terrain->EnsureChunkGenerated(FIntPoint(-20,0),ChunkError);FillChunkFixture(false);
	const FIntPoint FarCell(-490,12);const FIntRect Visible=GetAftershockVisibleCellBounds(*Terrain);
	TMap<FIntPoint,float> FarSeverity,FarDepths;FarSeverity.Add(FarCell,100.f);FAftershockFloodSurfaceStats FarStats;
	const bool FarMesh=BuildAftershockFloodSurface(Owner,Terrain,FarSeverity,Surface,&FarStats,&FarDepths);
	Check(TEXT("visible_mesh_window_is_bounded_to_three_chunks_per_axis"),FarGenerated&&Visible.Width()<=75&&Visible.Height()<=75&&!Visible.Contains(FarCell),TEXT("A distant resident chunk does not allocate a bounding-rectangle megamesh; this headless fixture uses a deterministic nearest-resident window."));
	Check(TEXT("clipping_mesh_does_not_erase_far_resident_flood_depth"),!FarMesh&&!Surface->GetStaticMesh()&&FarDepths.Contains(FarCell)&&FarDepths.FindRef(FarCell)>=12.f&&FarStats.ReconstructedWetCells==1,TEXT("The local draw mesh is absent while the complete canonical display-depth field retains the far inundated cell."));
	AAftershockDisasterPresentationActor* Display=World->SpawnActor<AAftershockDisasterPresentationActor>(FVector::ZeroVector,FRotator::ZeroRotator,Spawn);
	if(Display){FAftershockDisasterVisualSnapshot Snapshot;Snapshot.Revision=1;Snapshot.Type=EDisasterType::Flood;Snapshot.DurationSeconds=24;Snapshot.EffectiveIntensity=100;Snapshot.LocalSeverity=FarSeverity;Display->BeginSnapshot(Snapshot,*Terrain);Display->AnimateSnapshot(Snapshot,*Terrain,12.f);}
	Check(TEXT("far_bridge_routing_water_query_is_independent_of_local_mesh_visibility"),Display&&Display->GetDisplayedFloodDepthCm(FarCell)>=12.f,TEXT("Actual presentation actor at the same event age returns far water even when no local flood mesh instance is rendered."));
	return Finish();
}
#else
UAftershockFloodVisualAuditCommandlet::UAftershockFloodVisualAuditCommandlet() {}
int32 UAftershockFloodVisualAuditCommandlet::Main(const FString& Params) { return 1; }
#endif
