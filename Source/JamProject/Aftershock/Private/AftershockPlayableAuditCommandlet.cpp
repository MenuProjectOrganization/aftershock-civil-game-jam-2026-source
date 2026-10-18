#include "AftershockPlayableAuditCommandlet.h"

#if WITH_EDITOR

#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/AftershockScaledChaos.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	struct FPlayableAuditCheck
	{
		FString Name;
		bool bPassed;
		FString Detail;
	};

	FString EscapeAuditJson(FString Value)
	{
		Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Value.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Value.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		Value.ReplaceInline(TEXT("\t"), TEXT("\\t"));
		return Value;
	}

	/** Exact visible rigid-leaf vertex bounds in the collection's authored local
	 * frame. Do not add MassToLocal: that belongs to the paired component pose
	 * API, while these vertices already use the rest hierarchy's render frame. */
	bool GetVisibleRigidRestBounds(const FGeometryCollection& Collection, FBox& OutBounds,
		int32& OutRigidLeaves, int32& OutVertices)
	{
		OutBounds = FBox(ForceInit);
		OutRigidLeaves = 0;
		OutVertices = 0;
		TArray<FTransform> Global;
		GeometryCollectionAlgo::GlobalMatrices(Collection.Transform, Collection.Parent, Global);
		bool bValid = true;
		for (int32 Transform = 0; Transform < Collection.SimulationType.Num(); ++Transform)
		{
			if (Collection.SimulationType[Transform] != FGeometryCollection::FST_Rigid
				|| !Collection.Children[Transform].IsEmpty()) continue;
			const int32 Geometry = Collection.TransformToGeometryIndex[Transform];
			if (!Global.IsValidIndex(Transform) || !Collection.VertexStart.IsValidIndex(Geometry)
				|| !Collection.VertexCount.IsValidIndex(Geometry) || !Collection.FaceStart.IsValidIndex(Geometry)
				|| !Collection.FaceCount.IsValidIndex(Geometry)) { bValid = false; continue; }
			const int32 FirstFace = Collection.FaceStart[Geometry], Faces = Collection.FaceCount[Geometry];
			if (FirstFace < 0 || Faces <= 0 || FirstFace > Collection.Visible.Num()
				|| Faces > Collection.Visible.Num() - FirstFace) { bValid = false; continue; }
			bool bVisible = false;
			for (int32 Face = FirstFace; Face < FirstFace + Faces; ++Face) bVisible |= Collection.Visible[Face];
			if (!bVisible) continue;
			++OutRigidLeaves;
			const int32 FirstVertex = Collection.VertexStart[Geometry], Vertices = Collection.VertexCount[Geometry];
			if (FirstVertex < 0 || Vertices <= 0 || FirstVertex > Collection.Vertex.Num()
				|| Vertices > Collection.Vertex.Num() - FirstVertex || Global[Transform].ContainsNaN())
			{ bValid = false; continue; }
			for (int32 Vertex = FirstVertex; Vertex < FirstVertex + Vertices; ++Vertex)
			{
				const FVector Position = Global[Transform].TransformPosition(FVector(Collection.Vertex[Vertex]));
				if (Position.ContainsNaN()) { bValid = false; continue; }
				OutBounds += Position;
				++OutVertices;
			}
		}
		return bValid && OutBounds.IsValid && OutVertices > 0 && OutRigidLeaves >= 20 && OutRigidLeaves <= 40;
	}

	bool EqualPresentationBounds(const FBox& A, const FBox& B)
	{
		return A.IsValid && B.IsValid && A.Min.Equals(B.Min, .5) && A.Max.Equals(B.Max, .5);
	}

	/** Mirror the existing crowd commandlet's world lifetime without touching the user's map. */
	struct FScopedPlayableAuditWorld
	{
		UWorld* World = nullptr;
		~FScopedPlayableAuditWorld()
		{
			if (!World) return;
			World->DestroyWorld(false);
			if (GEngine) GEngine->DestroyWorldContext(World);
			World->RemoveFromRoot();
		}
	};
}

UAftershockPlayableAuditCommandlet::UAftershockPlayableAuditCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UAftershockPlayableAuditCommandlet::Main(const FString& Params)
{
	if (FParse::Param(*Params,TEXT("AftershockAuthorScaledChaos")))
	{
		int32 Validated=0;FString Error;
		const bool bAuthored=AftershockScaledChaos::AuthorScaledChaosAssets(true,Validated,Error);
		if (!bAuthored) { UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_SCALED_CHAOS_AUTHORING_FAILED validated=%d reason=%s"),Validated,*Error); }
		else { UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SCALED_CHAOS_AUTHORING_COMPLETE validated=%d canonicalAssetsChanged=0"),Validated); }
		return bAuthored?0:1;
	}
	TArray<FPlayableAuditCheck> Checks;
	int32 PassedCount = 0;
	int32 FailedCount = 0;
	auto Check = [&](const TCHAR* Name, bool bPassed, const FString& Detail)
	{
		Checks.Add({Name, bPassed, Detail});
		bPassed ? ++PassedCount : ++FailedCount;
		if (bPassed)
		{
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_PLAYABLE_AUDIT_PASS %s: %s"), Name, *Detail);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_PLAYABLE_AUDIT_FAIL %s: %s"), Name, *Detail);
		}
	};
	auto Finish = [&]() -> int32
	{
		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PresentationValidation"));
		const FString ReportPath = FPaths::Combine(Directory, TEXT("playable_audit.json"));
		IFileManager::Get().MakeDirectory(*Directory, true);
		FString Json = FString::Printf(TEXT("{\n  \"schema_version\": 1,\n  \"utc\": \"%s\",\n  \"engine\": \"%s\",\n  \"scope\": \"Canonical subsystem transactions and CPU-side ISM transforms in an isolated world; no UI, screenshot, GPU, package or scientific-physics validation.\",\n  \"passed\": %d,\n  \"failed\": %d,\n  \"checks\": [\n"),
			*FDateTime::UtcNow().ToIso8601(), *EscapeAuditJson(FEngineVersion::Current().ToString()), PassedCount, FailedCount);
		for (int32 Index = 0; Index < Checks.Num(); ++Index)
		{
			const FPlayableAuditCheck& Result = Checks[Index];
			Json += FString::Printf(TEXT("    {\"name\":\"%s\",\"passed\":%s,\"detail\":\"%s\"}%s\n"),
				*EscapeAuditJson(Result.Name), Result.bPassed ? TEXT("true") : TEXT("false"), *EscapeAuditJson(Result.Detail), Index + 1 < Checks.Num() ? TEXT(",") : TEXT(""));
		}
		Json += TEXT("  ]\n}\n");
		if (!FFileHelper::SaveStringToFile(Json, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_PLAYABLE_AUDIT_REPORT_WRITE_FAILED %s"), *ReportPath);
			return 2;
		}
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_PLAYABLE_AUDIT_RESULT passed=%d failed=%d report=%s"), PassedCount, FailedCount, *ReportPath);
		return FailedCount == 0 ? 0 : 1;
	};

	Check(TEXT("engine_available"), GEngine != nullptr, TEXT("Commandlet requires an initialized editor engine."));
	if (!GEngine) return Finish();
	FScopedPlayableAuditWorld ScopedWorld;
	ScopedWorld.World = UWorld::CreateWorld(EWorldType::Game, false, FName(TEXT("AftershockPlayableAuditWorld")), GetTransientPackage(), true, ERHIFeatureLevel::SM5, nullptr, false);
	Check(TEXT("isolated_world_created"), ScopedWorld.World != nullptr, TEXT("No gameplay map or project asset is saved by this commandlet."));
	if (!ScopedWorld.World) return Finish();
	UWorld* World = ScopedWorld.World;
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>();
	UBuildingManagerSubsystem* Buildings = World->GetSubsystem<UBuildingManagerSubsystem>();
	UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>();
	Check(TEXT("canonical_subsystems_initialized"), Terrain && Buildings && Economy, TEXT("Terrain, BuildingManager and Economy must all belong to this same world."));
	if (!Terrain || !Buildings || !Economy) return Finish();

	const int64 BudgetBeforeSeed = Economy->TreasuryTL;
	Buildings->SeedInitialSettlement();
	const int32 SeedCount = Buildings->GetActiveBuildingsCount();
	Check(TEXT("initial_city_has_buildings"), SeedCount > 0, FString::Printf(TEXT("seeded_buildings=%d"), SeedCount));
	Check(TEXT("initial_city_does_not_charge_construction"), Economy->TreasuryTL == BudgetBeforeSeed, FString::Printf(TEXT("treasury_before=%lld after=%lld"), BudgetBeforeSeed, Economy->TreasuryTL));
	Buildings->SeedInitialSettlement();
	Check(TEXT("initial_city_seed_is_idempotent"), Buildings->GetActiveBuildingsCount() == SeedCount && Economy->TreasuryTL == BudgetBeforeSeed, TEXT("Repeated initialization must not duplicate buildings or charge money."));
	int64 ProjectedIncome = 0;
	int64 ProjectedMaintenance = 0;
	Economy->CalculateProjectedFinances(ProjectedIncome, ProjectedMaintenance);
	Check(TEXT("initial_city_projects_finances"), ProjectedIncome > 0 && ProjectedMaintenance > 0,
		FString::Printf(TEXT("income=%lld maintenance=%lld"), ProjectedIncome, ProjectedMaintenance));
	const int64 BudgetBeforeTurn = Economy->TreasuryTL;
	const int32 TurnBefore = Economy->CurrentTurn;
	Economy->AdvanceTurn();
	Check(TEXT("projected_finances_match_actual_turn"), Economy->CurrentTurn == TurnBefore + 1 && Economy->TreasuryTL - BudgetBeforeTurn == ProjectedIncome - ProjectedMaintenance,
		FString::Printf(TEXT("projected_net=%lld actual_net=%lld"), ProjectedIncome - ProjectedMaintenance, Economy->TreasuryTL - BudgetBeforeTurn));

	// Read actual shipped assets rather than trusting their shared source names.
	// No placement/collapse, asset authoring, solver step or geometry mutation.
	const float CellSizeBeforeBounds = Terrain->CellSize;
	const int64 TreasuryBeforeBounds = Economy->TreasuryTL;
	const int32 BuildingsBeforeBounds = Buildings->GetActiveBuildingsCount();
	FIntPoint BoundsProbeCoord = FIntPoint(MAX_int32, MAX_int32);
	for (const FGridCell& Cell : Terrain->Cells)
	{
		if (Cell.bIsPlayable) { BoundsProbeCoord = Cell.Coords; break; }
	}
	const EBuildingType CoreTypes[] = { EBuildingType::House, EBuildingType::Apartment,
		EBuildingType::Office, EBuildingType::Factory, EBuildingType::Hospital };
	const TCHAR* CoreNames[] = { TEXT("House"), TEXT("Apartment"), TEXT("Office"), TEXT("Factory"), TEXT("Hospital") };
	// Published presentation heights above authored grade, in centimetres. The
	// negative foundation skirt is not part of the visible floor/roof height.
	const float CoreFloorHeights[] = { 300.f, 310.f, 310.f, 450.f, 330.f };
	const float CoreRoofHeights[] = { 100.f, 60.f, 60.f, 0.f, 60.f };
	for (int32 CoreIndex = 0; CoreIndex < UE_ARRAY_COUNT(CoreTypes); ++CoreIndex)
	{
		UBuildingData* CoreData = Buildings->GetCatalogData(CoreTypes[CoreIndex]);
		UStaticMesh* Intact = CoreData ? CoreData->MeshIntact.LoadSynchronous() : nullptr;
		UGeometryCollection* Fractured = CoreData ? CoreData->MeshFractured.LoadSynchronous() : nullptr;
		const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Rest = Fractured ? Fractured->GetGeometryCollection() : nullptr;
		const bool bReady = CoreData && CoreData->Type == CoreTypes[CoreIndex] && Intact && Rest && Terrain->IsValidCoord(BoundsProbeCoord);
		Check(*FString::Printf(TEXT("%s_native_intact_and_gc_available"), CoreNames[CoreIndex]), bReady,
			FString::Printf(TEXT("intact=%s gc=%s; each type uses its own canonical catalogue definition"), *GetPathNameSafe(Intact), *GetPathNameSafe(Fractured)));
		if (!bReady) continue;
		const FSoftObjectPath ExpectedSource(Intact);
		const bool bSourceIdentity = Fractured->GeometrySource.Num() == 1
			&& Fractured->GeometrySource[0].SourceGeometryObject == ExpectedSource
			&& Fractured->GeometrySource[0].LocalTransform.Equals(FTransform::Identity, .0001);
		Check(*FString::Printf(TEXT("%s_gc_source_path_and_local_pivot_identity"), CoreNames[CoreIndex]), bSourceIdentity,
			FString::Printf(TEXT("source=%s sourceRecords=%d; metadata identity alone is not a vertex-bounds proof"), *ExpectedSource.ToString(), Fractured->GeometrySource.Num()));
		FBox RestBounds(ForceInit);
		int32 RigidLeaves = 0, Vertices = 0;
		const bool bRestValid = GetVisibleRigidRestBounds(*Rest, RestBounds, RigidLeaves, Vertices);
		const FBox IntactBounds = Intact->GetBoundingBox();
		Check(*FString::Printf(TEXT("%s_native_gc_vertex_bounds_match_intact_local_pivot"), CoreNames[CoreIndex]),
			bRestValid && EqualPresentationBounds(RestBounds, IntactBounds),
			FString::Printf(TEXT("rigidLeaves=%d restVertices=%d intactMin=%s intactMax=%s gcMin=%s gcMax=%s toleranceCm=0.5; actual rigid rest vertices/hierarchy, not a source manifest"),
				RigidLeaves, Vertices, *IntactBounds.Min.ToString(), *IntactBounds.Max.ToString(), *RestBounds.Min.ToString(), *RestBounds.Max.ToString()));
		Terrain->CellSize = 1000.f;
		const int32 FloorCounts[] = { CoreData->FloorsMin, CoreData->FloorsMax };
		const float Yaws[] = { 0.f, 37.f };
		for (int32 FloorIndex = 0; FloorIndex < UE_ARRAY_COUNT(FloorCounts); ++FloorIndex)
		{
			for (const float Yaw : Yaws)
			{
				const FTransform Visual = Buildings->GetBuildingVisualTransform(BoundsProbeCoord, CoreData, FloorCounts[FloorIndex], Yaw);
				const FBox IntactWorld = IntactBounds.TransformBy(Visual);
				const FBox FracturedWorld = RestBounds.TransformBy(Visual);
				const float Ground = Terrain->GetCell(BoundsProbeCoord).Elevation;
				const bool bSourceHasBuriedFooting = IntactBounds.Min.Z < 0.f && IntactBounds.Max.Z > 0.f;
				const FVector WorldGrade = Visual.TransformPosition(FVector::ZeroVector);
				const float ExpectedVisibleHeight = FloorCounts[FloorIndex] * CoreFloorHeights[CoreIndex] + CoreRoofHeights[CoreIndex];
				// The source's local Z=0 marks grade. Seating its MinZ=-150cm at
				// terrain height would lift the buried footing into view and suppress
				// genuine foundation contact. Other meshes still sit by their bottom.
				const bool bGroundConvention = bSourceHasBuriedFooting
					? WorldGrade.Equals(Terrain->GridToWorld(BoundsProbeCoord, true), .5)
						&& IntactWorld.Min.Z < Ground - .5f
					: FMath::IsNearlyEqual(IntactWorld.Min.Z, Ground, .5f);
				Check(*FString::Printf(TEXT("%s_ten_metre_gc_intact_bounds_%s_floors_yaw_%d"), CoreNames[CoreIndex], FloorIndex == 0 ? TEXT("min") : TEXT("max"), FMath::RoundToInt(Yaw)),
					bRestValid && !Visual.ContainsNaN() && EqualPresentationBounds(IntactWorld, FracturedWorld)
					&& bGroundConvention && FMath::IsNearlyEqual(IntactWorld.Max.Z - Ground, ExpectedVisibleHeight, .5f),
					FString::Printf(TEXT("floors=%d yaw=%.0f scale=%s intactWorldMin=%s intactWorldMax=%s gcWorldMin=%s gcWorldMax=%s groundZ=%.3f gradePivotWorldZ=%.3f sourceHasBuriedFooting=%d visibleHeightCm=%.3f expectedVisibleHeightCm=%.3f; shared pure visual helper, actual grade/footing and bbox equality only, no solver/surface equivalence claim"),
						FloorCounts[FloorIndex], Yaw, *Visual.GetScale3D().ToString(), *IntactWorld.Min.ToString(), *IntactWorld.Max.ToString(),
						*FracturedWorld.Min.ToString(), *FracturedWorld.Max.ToString(), Ground, WorldGrade.Z, bSourceHasBuriedFooting,
						IntactWorld.Max.Z - Ground, ExpectedVisibleHeight));
			}
		}
		Terrain->CellSize = CellSizeBeforeBounds;
	}
	Terrain->CellSize = CellSizeBeforeBounds;
	Check(TEXT("native_gc_bbox_audit_preserves_cell_size_city_and_budget"), Terrain->CellSize == CellSizeBeforeBounds
		&& Economy->TreasuryTL == TreasuryBeforeBounds && Buildings->GetActiveBuildingsCount() == BuildingsBeforeBounds,
		TEXT("Five asset pairs inspected without placing a building, running collapse, charging money, or leaving a changed generation cell size."));
	int32 ScaledAssetsValidated=0;FString ScaledAssetError;
	Check(TEXT("ten_metre_gc_baked_geometry_preserves_all_catalog_floors"),AftershockScaledChaos::AuthorScaledChaosAssets(false,ScaledAssetsValidated,ScaledAssetError),
		FString::Printf(TEXT("validated=%d expected=27 error=%s; exact scaled vertices, stable leaf IDs/order, normalized normals, complete cooked collision and native render data; read-only packages"),ScaledAssetsValidated,*ScaledAssetError));

	TStrongObjectPtr<UBuildingData> Data(NewObject<UBuildingData>(World));
	Data->DisplayName = FText::FromString(TEXT("Playable Audit House"));
	Data->Type = EBuildingType::House;
	Data->CostTL = 1000;
	Data->FloorsMin = 1;
	Data->FloorsMax = 3;
	Data->Residents = 4;
	Data->TaxPerTurn = 50;
	Data->MaintenancePerTurn = 10;
	Data->MeshIntact = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/Buildings/Meshes/SM_House_01.SM_House_01")));
	// Fixed published design examples, not a reimplementation of the cost formula.
	Check(TEXT("cost_weak_one_floor"), Buildings->CalculateConstructionCost(Data.Get(), EFoundationQuality::Weak, 1) == 1000, TEXT("Design contract: 1000 TL base house with weak foundation."));
	Check(TEXT("cost_medium_one_floor"), Buildings->CalculateConstructionCost(Data.Get(), EFoundationQuality::Medium, 1) == 1300, TEXT("Design contract: medium foundation quote is 1300 TL."));
	Check(TEXT("cost_strong_one_floor"), Buildings->CalculateConstructionCost(Data.Get(), EFoundationQuality::Strong, 1) == 1800, TEXT("Design contract: strong foundation quote is 1800 TL."));
	Check(TEXT("cost_strong_two_floors"), Buildings->CalculateConstructionCost(Data.Get(), EFoundationQuality::Strong, 2) == 2070, TEXT("Design contract: two-floor strong-foundation quote is 2070 TL."));
	auto FindFreeCell = [&]() -> FIntPoint
	{
		for (const FGridCell& Cell : Terrain->Cells) if (Buildings->CanPlaceBuildingAt(Cell.Coords, Data.Get())) return Cell.Coords;
		return FIntPoint(INDEX_NONE, INDEX_NONE);
	};
	const FIntPoint BuildCoord = FindFreeCell();
	UInstancedStaticMeshComponent* FixtureISM = Buildings->GetOrCreateISM(Data.Get());
	Check(TEXT("placement_fixture_available"), Terrain->IsValidCoord(BuildCoord) && FixtureISM && FixtureISM->GetStaticMesh(), TEXT("Fixture uses the project's imported house mesh and an unoccupied canonical land cell."));
	if (!Terrain->IsValidCoord(BuildCoord) || !FixtureISM || !FixtureISM->GetStaticMesh()) return Finish();

	Economy->TreasuryTL = 100000;
	const int32 BeforeCount = Buildings->GetActiveBuildingsCount();
	const int32 BeforeInstances = FixtureISM->GetInstanceCount();
	const int64 BeforeBudget = Economy->TreasuryTL;
	const int32 Quote = Buildings->CalculateConstructionCost(Data.Get(), EFoundationQuality::Strong, 2);
	const bool bPlaced = Economy->BuildBuildingWithBudget(BuildCoord, Data.Get(), EFoundationQuality::Strong, 2);
	FBuildingInstance Placed;
	const bool bHasRecord = Buildings->GetBuildingAtCoords(BuildCoord, Placed);
	Check(TEXT("placement_one_record_one_instance_one_payment"), bPlaced && bHasRecord && Buildings->GetActiveBuildingsCount() == BeforeCount + 1 && FixtureISM->GetInstanceCount() == BeforeInstances + 1 && BeforeBudget - Economy->TreasuryTL == Quote,
		FString::Printf(TEXT("result=%d record=%d count_delta=%d instance_delta=%d quote=%d actual_charge=%lld"), bPlaced, bHasRecord, Buildings->GetActiveBuildingsCount() - BeforeCount, FixtureISM->GetInstanceCount() - BeforeInstances, Quote, BeforeBudget - Economy->TreasuryTL));
	Check(TEXT("placement_record_matches_selected_contract"), bHasRecord && Placed.CurrentFloors == 2 && Placed.Foundation == EFoundationQuality::Strong && Terrain->GetCell(BuildCoord).OccupyingBuildingID == Placed.InstanceID,
		TEXT("Placed floors, foundation and terrain occupancy reference must agree with the purchase."));
	const int64 BeforeDuplicate = Economy->TreasuryTL;
	const int32 CountBeforeDuplicate = Buildings->GetActiveBuildingsCount();
	const int32 InstancesBeforeDuplicate = FixtureISM->GetInstanceCount();
	const bool bDuplicate = Economy->BuildBuildingWithBudget(BuildCoord, Data.Get(), EFoundationQuality::Strong, 2);
	Check(TEXT("duplicate_placement_rejected_without_spend"), !bDuplicate && Economy->TreasuryTL == BeforeDuplicate && Buildings->GetActiveBuildingsCount() == CountBeforeDuplicate && FixtureISM->GetInstanceCount() == InstancesBeforeDuplicate, TEXT("Same occupied cell requested a second time."));

	const FIntPoint UnaffordableCoord = FindFreeCell();
	Economy->TreasuryTL = Quote - 1;
	const int64 BeforeUnaffordable = Economy->TreasuryTL;
	const bool bUnaffordable = Economy->BuildBuildingWithBudget(UnaffordableCoord, Data.Get(), EFoundationQuality::Strong, 2);
	FBuildingInstance Unexpected;
	Check(TEXT("unaffordable_placement_rejected_without_record"), Terrain->IsValidCoord(UnaffordableCoord) && !bUnaffordable && Economy->TreasuryTL == BeforeUnaffordable && !Buildings->GetBuildingAtCoords(UnaffordableCoord, Unexpected) && Buildings->GetActiveBuildingsCount() == CountBeforeDuplicate && FixtureISM->GetInstanceCount() == InstancesBeforeDuplicate,
		TEXT("Treasury is exactly one TL below the canonical quote."));
	Economy->TreasuryTL = 100000;
	FIntPoint RiverCoord(INDEX_NONE, INDEX_NONE);
	for (const FGridCell& Cell : Terrain->Cells) if (Cell.bIsRiverbed && Terrain->IsPlayableCoord(Cell.Coords)) { RiverCoord = Cell.Coords; break; }
	const int64 BeforeRiver = Economy->TreasuryTL;
	const bool bRiverPlacement = Economy->BuildBuildingWithBudget(RiverCoord, Data.Get(), EFoundationQuality::Strong, 2);
	Check(TEXT("river_placement_rejected_without_spend"), Terrain->IsValidCoord(RiverCoord) && !bRiverPlacement && Economy->TreasuryTL == BeforeRiver && Buildings->GetActiveBuildingsCount() == CountBeforeDuplicate && FixtureISM->GetInstanceCount() == InstancesBeforeDuplicate && Terrain->GetCell(RiverCoord).OccupyingBuildingID == INDEX_NONE,
		TEXT("A canonical river cell cannot acquire a building record, instance or charge."));

	if (bPlaced && bHasRecord)
	{
		FTransform IntactTransform;
		const bool bIntactTransform = FixtureISM->GetInstanceTransform(Placed.ISMInstanceIndex, IntactTransform, true);
		Check(TEXT("placed_instance_visible"), bIntactTransform && !IntactTransform.GetScale3D().IsNearlyZero(), TEXT("CPU-side instance transform exists and has nonzero scale; does not prove GPU visibility."));
		if (bIntactTransform)
		{
			const FBox Bounds=FixtureISM->GetStaticMesh()->GetBoundingBox();
			FBuildingInstance Picked;float HitDistance=0.f;
			const FVector RoofOrigin=IntactTransform.TransformPosition(FVector(Bounds.GetCenter().X,Bounds.GetCenter().Y,Bounds.Max.Z+100.f));
			Check(TEXT("building_roof_is_selectable_without_imported_collision"),Buildings->RaycastBuilding(RoofOrigin,FVector::DownVector,10000.f,Picked,HitDistance)&&Picked.InstanceID==Placed.InstanceID,TEXT("Click-only ray intersects rendered instance bounds rather than the ground parcel."));
			FTransform Rotated=IntactTransform;Rotated.SetRotation(FRotator(0,37,0).Quaternion());
			FixtureISM->UpdateInstanceTransform(Placed.ISMInstanceIndex,Rotated,true,true,true);
			const FVector SideOrigin=Rotated.TransformPosition(FVector(Bounds.Max.X+20.f,Bounds.GetCenter().Y,Bounds.GetCenter().Z));
			const FVector SideDirection=Rotated.TransformVectorNoScale(FVector(-1,0,0));
			Check(TEXT("rotated_building_facade_is_selectable"),Buildings->RaycastBuilding(SideOrigin,SideDirection,10000.f,Picked,HitDistance)&&Picked.InstanceID==Placed.InstanceID,TEXT("OBB test follows the live transform, including yaw and nonuniform floor scale."));
			Check(TEXT("building_pick_respects_foreground_occlusion_limit"),!Buildings->RaycastBuilding(SideOrigin,SideDirection,1.f,Picked,HitDistance),TEXT("Terrain or a foreground scene hit shortens the selectable ray."));
			Check(TEXT("invalid_building_pick_direction_is_rejected"),!Buildings->RaycastBuilding(SideOrigin,FVector::ZeroVector,10000.f,Picked,HitDistance),TEXT("No NaN, zero-direction or unbounded selection query."));
			FixtureISM->UpdateInstanceTransform(Placed.ISMInstanceIndex,IntactTransform,true,true,true);
			const float OldCellSize=Terrain->CellSize;Terrain->CellSize=1000.f;
			const FTransform Wide=Buildings->GetBuildingVisualTransform(BuildCoord,Data.Get(),2);
			const FBox WideBounds=Bounds.TransformBy(Wide);
			const float WideGround=Terrain->GetCell(BuildCoord).Elevation;
			Check(TEXT("new_ten_metre_lot_has_human_scale_house"),FMath::IsNearlyEqual(FMath::Max(WideBounds.GetSize().X,WideBounds.GetSize().Y),760.f,1.f)&&FMath::IsNearlyEqual(WideBounds.Max.Z-WideGround,700.f,1.f),TEXT("Two three-metre floors plus one-metre roof ABOVE grade; the buried foundation is excluded from the700cm visible height."));
			Check(TEXT("new_building_pivot_sits_on_canonical_ground"),FMath::IsNearlyEqual(Wide.TransformPosition(FVector::ZeroVector).Z,WideGround,1.f)&&WideBounds.Min.Z<WideGround-.5f,TEXT("Authored localZ0 grade sits on canonical terrain, while the negative-Z foundation remains buried; seating the skirt bottom at ground is a regression."));
			Terrain->CellSize=OldCellSize;
		}
		Buildings->ApplyDamageToBuilding(Placed.InstanceID, 79.99f);
		FBuildingInstance Damaged;
		FTransform DamagedTransform;
		const bool bDamageRecord = Buildings->GetBuildingByID(Placed.InstanceID, Damaged);
		const bool bDamageTransform = FixtureISM->GetInstanceTransform(Placed.ISMInstanceIndex, DamagedTransform, true);
		Check(TEXT("damage_below_80_remains_severe_and_visible"), bDamageRecord && Damaged.DamageState == EDamageState::Severe && bDamageTransform && !DamagedTransform.GetScale3D().IsNearlyZero(), TEXT("Latest memorial rule: collapse starts at 80; 79.99 remains repairable and visible."));
		// Reproduce the stale transform left by older builds; the real paid repair must heal it.
		FTransform LegacyHiddenTransform = IntactTransform;
		LegacyHiddenTransform.SetScale3D(FVector::ZeroVector);
		FixtureISM->UpdateInstanceTransform(Placed.ISMInstanceIndex, LegacyHiddenTransform, true, true, true);
		const int64 BeforeRepair = Economy->TreasuryTL;
		const bool bRepaired = Economy->RepairWithBudget(Placed.InstanceID);
		FBuildingInstance Repaired;
		FTransform RepairedTransform;
		const bool bRepairRecord = Buildings->GetBuildingByID(Placed.InstanceID, Repaired);
		const bool bRepairTransform = FixtureISM->GetInstanceTransform(Placed.ISMInstanceIndex, RepairedTransform, true);
		Check(TEXT("repair_one_payment_and_intact_record"), bRepaired && bRepairRecord && Repaired.CurrentDamage == 0.0f && Repaired.DamageState == EDamageState::Intact && BeforeRepair - Economy->TreasuryTL == 5000 && Buildings->GetActiveBuildingsCount() == BeforeCount + 1,
			FString::Printf(TEXT("result=%d actual_charge=%lld; severe repair contract is 5000 TL"), bRepaired, BeforeRepair - Economy->TreasuryTL));
		Check(TEXT("repair_restores_legacy_zero_scale"), bIntactTransform && bRepairTransform && RepairedTransform.GetScale3D().Equals(IntactTransform.GetScale3D(), 0.001f), TEXT("Injected historical zero-scale visual state; repair must restore original scale without creating another building."));
		const int64 BeforeRetrofit = Economy->TreasuryTL;
		const bool bRetrofitted = Economy->RetrofitWithBudget(Placed.InstanceID);
		FBuildingInstance Retrofitted;
		Check(TEXT("retrofit_one_payment_updates_existing_record"), bRetrofitted && Buildings->GetBuildingByID(Placed.InstanceID, Retrofitted) && Retrofitted.bIsRetrofitted && BeforeRetrofit - Economy->TreasuryTL == 1000 && Buildings->GetActiveBuildingsCount() == BeforeCount + 1,
			TEXT("Existing building strengthened once for the published 1000 TL cost."));
		Buildings->ApplyDamageToBuilding(Placed.InstanceID, 80.0f);
		const int64 BeforeCollapsedRepair = Economy->TreasuryTL;
		Check(TEXT("collapsed_building_cannot_be_repaired"), !Economy->RepairWithBudget(Placed.InstanceID) && Economy->TreasuryTL == BeforeCollapsedRepair, TEXT("Damage above 80 requires rubble clearance rather than repair."));
		const int64 BeforeClearance = Economy->TreasuryTL;
		const bool bCleared = Economy->ClearRubbleWithBudget(BuildCoord);
		FBuildingInstance Cleared;
		Check(TEXT("rubble_clearance_one_payment_keeps_memorial_occupied"), bCleared && BeforeClearance - Economy->TreasuryTL == 3000 && Buildings->GetBuildingAtCoords(BuildCoord, Cleared) && Cleared.DamageState==EDamageState::Memorial && Buildings->GetActiveBuildingsCount() == BeforeCount+1 && Terrain->GetCell(BuildCoord).OccupyingBuildingID == Placed.InstanceID && !Buildings->CanPlaceBuildingAt(BuildCoord,Placed.BuildingData),
			TEXT("Exactly 3000 TL clears debris; the original ID and occupied plot remain as a three-turn memorial."));
		const int64 MemorialBalance=Economy->TreasuryTL;
		Check(TEXT("memorial_clear_cannot_be_charged_twice"),!Economy->ClearRubbleWithBudget(BuildCoord)&&Economy->TreasuryTL==MemorialBalance,TEXT("A memorial is not another debris purchase."));
		const int32 OriginalTurn=Economy->CurrentTurn;Economy->CurrentTurn=Cleared.MemorialStartedTurn+2;
		Buildings->AdvanceMemorialTurn(Economy->CurrentTurn);
		Check(TEXT("memorial_still_blocks_before_third_turn"),Buildings->GetBuildingAtCoords(BuildCoord,Cleared)&&Cleared.DamageState==EDamageState::Memorial&&!Buildings->CanPlaceBuildingAt(BuildCoord,Placed.BuildingData),TEXT("No early parcel reuse."));
		Economy->CurrentTurn=Cleared.MemorialStartedTurn+3;Buildings->AdvanceMemorialTurn(Economy->CurrentTurn);
		Check(TEXT("memorial_expires_after_three_turns_without_second_charge"),!Buildings->GetBuildingAtCoords(BuildCoord,Cleared)&&Buildings->GetActiveBuildingsCount()==BeforeCount&&Terrain->GetCell(BuildCoord).OccupyingBuildingID==INDEX_NONE&&Economy->TreasuryTL==MemorialBalance,TEXT("Only the expired memorial record is removed; no second clearance fee."));
		Economy->CurrentTurn=OriginalTurn;
	}
	else Check(TEXT("maintenance_fixture_available"), false, TEXT("Maintenance regression checks require the preceding placement to succeed."));
	URoadNetworkSubsystem* Roads = World->GetSubsystem<URoadNetworkSubsystem>();
	Check(TEXT("road_subsystem_starts_empty"), Roads && Roads->CountRoadCells() == 0, TEXT("The player's city must not gain automatically generated canonical roads."));
	if (Roads)
	{
		FIntPoint RoadStart(INDEX_NONE, INDEX_NONE);
		for (const FGridCell& Candidate : Terrain->Cells)
		{
			bool bFreeRun = true;
			for (int32 Offset = 0; Offset < 4; ++Offset)
			{
				const FIntPoint Target = Candidate.Coords + FIntPoint(Offset, 0);
				// This route later becomes building frontage after its first tile
				// is removed. Empty land alone does not guarantee a legal grade.
				if (!Buildings->CanPlaceBuildingAt(Target, Data.Get()) || !Roads->CanBuildAutomaticAccessCell(Target)
					|| (Offset > 0 && !Roads->CanBuildAutomaticAccessEdge(Target-FIntPoint(1,0),Target)))
				{ bFreeRun = false; break; }
			}
			if (bFreeRun) { RoadStart = Candidate.Coords; break; }
		}
		Check(TEXT("road_fixture_free_playable_land"), Terrain->IsPlayableCoord(RoadStart), TEXT("Four adjacent empty playable land cells with legal automatic-access grades for transaction and follow-up building checks."));
		if (Terrain->IsPlayableCoord(RoadStart))
		{
			const FIntPoint RoadEnd = RoadStart + FIntPoint(2, 0);
			Economy->TreasuryTL = 100000;
			const int32 InitialRoadCount = Roads->CountRoadCells();
			const int64 BeforeQuote = Economy->TreasuryTL;
			const FAftershockRoadQuote RoadQuote = Roads->QuoteRoad(RoadStart, RoadEnd);
			Check(TEXT("road_preview_quotes_without_mutation"), RoadQuote.bRouteValid && RoadQuote.Path.Num() == 3 && RoadQuote.NewRoadCells == 3 && RoadQuote.CostTL == 600 && Roads->CountRoadCells() == InitialRoadCount && Economy->TreasuryTL == BeforeQuote,
				TEXT("Published road price: three new cells cost 600 TL; quoting changes no cells or treasury."));
			Economy->TreasuryTL = RoadQuote.CostTL - 1;
			const int64 BeforeRoadNoFunds = Economy->TreasuryTL;
			FAftershockRoadQuote Requoted;
			const bool bRoadNoFunds = Roads->BuildRoad(RoadStart, RoadEnd, Requoted);
			Check(TEXT("unaffordable_road_has_no_partial_cells_or_spend"), !bRoadNoFunds && Roads->CountRoadCells() == InitialRoadCount && Economy->TreasuryTL == BeforeRoadNoFunds,
				TEXT("One TL below the full quote must reject the entire route."));
			Economy->TreasuryTL = 100000;
			const int64 BeforeRoadBuild = Economy->TreasuryTL;
			const bool bRoadBuilt = Roads->BuildRoad(RoadStart, RoadEnd, Requoted);
			bool bEveryPathCellBuilt = true;
			for (const FIntPoint Coord : RoadQuote.Path) bEveryPathCellBuilt &= Roads->HasRoad(Coord);
			Check(TEXT("road_build_commits_all_cells_one_payment"), bRoadBuilt && bEveryPathCellBuilt && Roads->CountRoadCells() == InitialRoadCount + 3 && BeforeRoadBuild - Economy->TreasuryTL == 600,
				FString::Printf(TEXT("built=%d road_delta=%d charge=%lld"), bRoadBuilt, Roads->CountRoadCells() - InitialRoadCount, BeforeRoadBuild - Economy->TreasuryTL));
			const int64 BeforeRoadBuilding = Economy->TreasuryTL;
			const int32 BeforeRoadBuildingCount = Buildings->GetActiveBuildingsCount();
			const bool bBuildingOnRoad = Economy->BuildBuildingWithBudget(RoadStart, Data.Get(), EFoundationQuality::Medium, 1);
			Check(TEXT("building_on_road_rejected_without_spend"), !bBuildingOnRoad && !Buildings->CanPlaceBuildingAt(RoadStart, Data.Get()) && Economy->TreasuryTL == BeforeRoadBuilding && Buildings->GetActiveBuildingsCount() == BeforeRoadBuildingCount,
				TEXT("Road and building occupancy share one canonical grid contract."));
			const int64 BeforeRepeatedRoad = Economy->TreasuryTL;
			const bool bRepeatedRoad = Roads->BuildRoad(RoadStart, RoadEnd, Requoted);
			Check(TEXT("repeated_road_has_no_second_payment"), !bRepeatedRoad && Requoted.NewRoadCells == 0 && Requoted.CostTL == 0 && Economy->TreasuryTL == BeforeRepeatedRoad && Roads->CountRoadCells() == InitialRoadCount + 3,
				TEXT("Already-built route contains no newly billable cells."));
			const int64 BeforeExtension = Economy->TreasuryTL;
			const bool bExtended = Roads->BuildRoad(RoadStart, RoadStart + FIntPoint(3, 0), Requoted);
			Check(TEXT("road_extension_only_charges_new_cell"), bExtended && Requoted.NewRoadCells == 1 && Requoted.CostTL == 200 && BeforeExtension - Economy->TreasuryTL == 200 && Roads->CountRoadCells() == InitialRoadCount + 4,
				TEXT("Three existing road cells reused; only the fourth costs 200 TL."));
			const int64 BeforeDemolition = Economy->TreasuryTL;
			const bool bRoadRemoved = Roads->RemoveRoadAt(RoadStart);
			const bool bRemovedAgain = Roads->RemoveRoadAt(RoadStart);
			Check(TEXT("road_demolition_frees_land_without_refund"), bRoadRemoved && !bRemovedAgain && !Roads->HasRoad(RoadStart) && Buildings->CanPlaceBuildingAt(RoadStart, Data.Get()) && Roads->CountRoadCells() == InitialRoadCount + 3 && Economy->TreasuryTL == BeforeDemolition,
				TEXT("Removing a road is idempotent, free and never grants construction refunds."));
			const int64 BeforeInvalidRoad = Economy->TreasuryTL;
			const int32 BeforeInvalidRoadCount = Roads->CountRoadCells();
			const bool bRiverRoad = Roads->BuildRoad(RiverCoord, RoadEnd, Requoted);
			Check(TEXT("river_road_rejected_without_bridge_or_mutation"), !bRiverRoad && !Requoted.bRouteValid && Roads->CountRoadCells() == BeforeInvalidRoadCount && Economy->TreasuryTL == BeforeInvalidRoad,
				TEXT("River endpoints cannot silently become bridges or roads."));
			const bool bOutsideRoad = Roads->BuildRoad(FIntPoint(-1, -1), RoadEnd, Requoted);
			Check(TEXT("outside_road_rejected_without_partial_cells"), !bOutsideRoad && Roads->CountRoadCells() == BeforeInvalidRoadCount && Economy->TreasuryTL == BeforeInvalidRoad,
				TEXT("Out-of-bounds route request changes no terrain or treasury."));
			const bool bEllipseRoad = Roads->BuildRoad(FIntPoint(0, 0), RoadEnd, Requoted);
			Check(TEXT("nonplayable_world_corner_rejects_road"), !Terrain->IsPlayableCoord(FIntPoint(0, 0)) && !bEllipseRoad && Roads->CountRoadCells() == BeforeInvalidRoadCount && Economy->TreasuryTL == BeforeInvalidRoad,
				TEXT("The round world's nonplayable map corner cannot acquire road cells."));
			TArray<FBuildingInstance> Existing;
			Buildings->GetAllBuildings(Existing);
			const bool bRoadOverBuilding = Existing.Num() > 0 && Roads->BuildRoad(Existing[0].GridCoords, RoadEnd, Requoted);
			Check(TEXT("occupied_endpoint_road_rejected_without_spend"), Existing.Num() > 0 && !bRoadOverBuilding && Roads->CountRoadCells() == BeforeInvalidRoadCount && Economy->TreasuryTL == BeforeInvalidRoad,
				TEXT("Occupied building parcels cannot be used as road endpoints."));
			const FAftershockBuildingRoadAccessQuote InterveningAccess=Buildings->QuoteAutomaticRoadAccess(RoadStart,Data.Get());
			const int32 InterveningBuildingCost=Buildings->CalculateConstructionCost(Data.Get(),EFoundationQuality::Medium,1);
			Check(TEXT("stale_road_preview_fixture_has_affordable_grade_safe_building_frontage"),Buildings->CanPlaceBuildingAt(RoadStart,Data.Get())
				&&Roads->CanBuildAutomaticAccessEdge(RoadStart,RoadStart+FIntPoint(1,0))
				&&Roads->CanAccessRoadFromCell(RoadStart,RoadStart+FIntPoint(1,0))
				&&InterveningAccess.bRouteValid&&InterveningAccess.NewRoadCells==0&&InterveningAccess.RoadCostTL==0
				&&InterveningAccess.RoadPath.Num()==1&&Economy->CanAfford(InterveningBuildingCost),
				FString::Printf(TEXT("plot=%s accessValid=%d newRoadCells=%d accessCost=%d buildingCost=%d treasury=%lld; intervening paid building must be independently legal before testing stale road authorization."),
					*RoadStart.ToString(),InterveningAccess.bRouteValid,InterveningAccess.NewRoadCells,InterveningAccess.RoadCostTL,InterveningBuildingCost,Economy->TreasuryTL));
			const FAftershockRoadQuote StalePreview = Roads->QuoteRoad(RoadStart, RoadStart);
			const bool bInterveningBuilding = Economy->BuildBuildingWithBudget(RoadStart, Data.Get(), EFoundationQuality::Medium, 1);
			const int64 AfterInterveningBuilding = Economy->TreasuryTL;
			const bool bStaleRoadBuilt = Roads->BuildRoad(RoadStart, RoadStart, Requoted);
			Check(TEXT("road_commit_revalidates_stale_preview"), StalePreview.bRouteValid && bInterveningBuilding && !bStaleRoadBuilt && !Roads->HasRoad(RoadStart) && Roads->CountRoadCells() == BeforeInvalidRoadCount && Economy->TreasuryTL == AfterInterveningBuilding,
				FString::Printf(TEXT("previewValid=%d interveningBuilding=%d staleRoadBuilt=%d roadCount=%d expectedRoadCount=%d balanceAfterBuilding=%lld balanceAfterRejectedRoad=%lld; a building inserted after preview blocks road commit without extra spend."),
					StalePreview.bRouteValid,bInterveningBuilding,bStaleRoadBuilt,Roads->CountRoadCells(),BeforeInvalidRoadCount,AfterInterveningBuilding,Economy->TreasuryTL));
		}
	}

	return Finish();
}

#else

UAftershockPlayableAuditCommandlet::UAftershockPlayableAuditCommandlet() {}
int32 UAftershockPlayableAuditCommandlet::Main(const FString& Params) { return 1; }

#endif
