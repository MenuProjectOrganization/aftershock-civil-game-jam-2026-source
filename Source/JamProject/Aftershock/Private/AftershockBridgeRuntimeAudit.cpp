#include "AftershockBridgeRuntimeAudit.h"

#if !UE_BUILD_SHIPPING
#include "Aftershock/Public/CityPresentationSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Camera/CameraTypes.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "TimerManager.h"
#include "UObject/Class.h"

namespace
{
struct FBridgeRuntimeState
{
	FAftershockRoadQuote Quote;
	FIntPoint Start = FIntPoint::ZeroValue, End = FIntPoint::ZeroValue;
	TArray<uint8> TerrainBefore, RiverBefore;
	FString Failure;
	int64 TreasuryBefore = 0, TreasuryAfter = 0;
	int32 RoadsBefore = 0, BridgesBefore = 0, BridgeID = INDEX_NONE;
	int32 Candidates = 0, ValidQuotes = 0, PurchaseCalls = 0;
	int32 StructureInstancesBefore = 0;
	bool bBuilt = false, bQuoteReadOnly = false, bExactCharge = false;
	bool bOriginalPathSafe = false, bReturnedQuoteEqual = false;
};

bool IsBridgeAuditWorld(UWorld* World)
{
	if (!IsValid(World) || World->bIsTearingDown || !GEngine) return false;
	const FWorldContext* Context = GEngine->GetWorldContextFromWorld(World);
	return Context && Context->World() == World;
}

FString EscapeBridgeJson(FString Value)
{
	Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Value.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	Value.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Value.ReplaceInline(TEXT("\t"), TEXT("\\t"));
	return Value;
}

// Compare reflected fields exactly, without structure padding. Only the six
// explicitly road-owned fields are normalized; terrain/hydrology stay native.
TArray<uint8> BridgeTerrainBytes(const UTerrainGridSubsystem& Terrain, bool bRiverOnly, bool bNormalizeRoadFields = true)
{
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes, true);
	FObjectAndNameAsStringProxyArchive Archive(Writer, false);
	int32 Width = Terrain.GridWidth, Height = Terrain.GridHeight, Seed = Terrain.TerrainSeed;
	float CellSize = Terrain.CellSize;
	FVector Origin = Terrain.GridOrigin;
	Archive << Width << Height << Seed << CellSize << Origin;
	for (const FGridCell& Cell : Terrain.Cells)
	{
		if (bRiverOnly && !Cell.bIsRiverbed) continue;
		FGridCell Copy = Cell;
		if (bNormalizeRoadFields)
		{
			Copy.bHasRoad = false; Copy.BridgeID = INDEX_NONE; Copy.BridgeAxis = 0;
			Copy.RoadSurfaceElevationCm = 0.f; Copy.bBridgeEndpoint = false;
			Copy.bRoadPredatedBridge = false;
		}
		FGridCell::StaticStruct()->SerializeItem(Archive, &Copy, nullptr);
	}
	return Bytes;
}

int32 CountBridgeStructureInstances(UWorld& World)
{
	int32 Count = 0;
	for (TActorIterator<AAftershockCityPresentationActor> It(&World); It; ++It)
	{
		TArray<UHierarchicalInstancedStaticMeshComponent*> Components;
		It->GetComponents(Components);
		for (const auto* Component : Components)
			if (Component && Component->GetFName() == TEXT("BridgeStructureHISM")) Count += Component->GetInstanceCount();
	}
	return Count;
}

bool IsSafeBridgePath(const UTerrainGridSubsystem& Terrain, const FAftershockRoadQuote& Quote)
{
	if (!Quote.bRouteValid || !Quote.bIncludesBridge || Quote.Path.Num() < 3
		|| Quote.Path.Num() != Quote.SurfaceElevationsCm.Num() || Quote.CostTL <= 0 || Quote.NewRoadCells <= 0) return false;
	int32 RiverCells = 0;
	for (FIntPoint Coord : Quote.Path)
	{
		if (!Terrain.IsPlayableCoord(Coord)) return false;
		const FGridCell Cell = Terrain.GetCell(Coord);
		if (Cell.bIsOcean || Cell.bHasDam || Cell.bVolcanicVent || Cell.bBurning || Cell.LavaCoverage01 > .1f
			|| Cell.OccupyingBuildingID != INDEX_NONE || Cell.BridgeID != INDEX_NONE || Cell.bIsEmergencyAssemblyArea) return false;
		RiverCells += Cell.bIsRiverbed ? 1 : 0;
	}
	return RiverCells > 0 && RiverCells == Quote.BridgeRiverCells
		&& !Terrain.GetCell(Quote.Path[0]).bIsRiverbed && !Terrain.GetCell(Quote.Path.Last()).bIsRiverbed;
}

bool SameBridgeQuote(const FAftershockRoadQuote& A, const FAftershockRoadQuote& B)
{
	return A.bRouteValid == B.bRouteValid && A.bIncludesBridge == B.bIncludesBridge && A.Path == B.Path
		&& A.SurfaceElevationsCm == B.SurfaceElevationsCm && A.NewRoadCells == B.NewRoadCells && A.CostTL == B.CostTL
		&& A.BridgeAxis == B.BridgeAxis && A.BridgeRiverCells == B.BridgeRiverCells && A.BridgeApproachCells == B.BridgeApproachCells;
}

bool IsFreshBridgePNG(const FString& Path)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 24) return false;
	const uint8 Signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
	for (int32 I = 0; I < 8; ++I) if (Bytes[I] != Signature[I]) return false;
	if (Bytes[12] != 'I' || Bytes[13] != 'H' || Bytes[14] != 'D' || Bytes[15] != 'R') return false;
	const auto Read32 = [&Bytes](int32 I) { return (uint32(Bytes[I]) << 24) | (uint32(Bytes[I + 1]) << 16) | (uint32(Bytes[I + 2]) << 8) | uint32(Bytes[I + 3]); };
	return Read32(16) == 1600 && Read32(20) == 1000;
}
}
#endif

void BeginAftershockBridgeRuntimeAudit(UWorld& World, FAftershockBridgeSceneCapture CaptureScene)
{
#if !UE_BUILD_SHIPPING
	if (!FParse::Param(FCommandLine::Get(), TEXT("AftershockBridgeVisualAudit"))) return;
	const TWeakObjectPtr<UWorld> WeakWorld(&World);
	const auto State = MakeShared<FBridgeRuntimeState>();
	FTimerHandle PurchaseTimer;
	World.GetTimerManager().SetTimer(PurchaseTimer, FTimerDelegate::CreateLambda([WeakWorld, State]()
	{
		UWorld* W = WeakWorld.Get();
		if (!IsBridgeAuditWorld(W)) return;
		auto* Terrain = W->GetSubsystem<UTerrainGridSubsystem>();
		auto* Roads = W->GetSubsystem<URoadNetworkSubsystem>();
		auto* Economy = W->GetSubsystem<UEconomyAndStateSubsystem>();
		if (!Terrain || !Roads || !Economy) { State->Failure = TEXT("Required current-world subsystem missing"); return; }
		State->TerrainBefore = BridgeTerrainBytes(*Terrain, false);
		State->RiverBefore = BridgeTerrainBytes(*Terrain, true);
		const TArray<uint8> PreviewBefore = BridgeTerrainBytes(*Terrain, false, false);
		State->TreasuryBefore = Economy->TreasuryTL;
		State->TreasuryAfter = State->TreasuryBefore;
		State->RoadsBefore = Roads->CountRoadCells(); State->BridgesBefore = Roads->GetBridgeCount();
		State->StructureInstancesBefore = CountBridgeStructureInstances(*W);
		// Each straight river run is considered only once per axis. QuoteRoad
		// extends ramps using actual elevations; this fixture never edits cells.
		const FIntPoint Directions[] = { FIntPoint(1, 0), FIntPoint(0, 1) };
		for (const FGridCell& Cell : Terrain->Cells)
		{
			if (!Terrain->IsPlayableCoord(Cell.Coords) || Cell.bIsRiverbed) continue;
			for (FIntPoint Direction : Directions)
			{
				FIntPoint End = Cell.Coords + Direction;
				if (!Terrain->IsValidCoord(End) || !Terrain->GetCell(End).bIsRiverbed) continue;
				int32 RiverRun = 0;
				while (Terrain->IsValidCoord(End) && Terrain->GetCell(End).bIsRiverbed && RiverRun <= 8) { End += Direction; ++RiverRun; }
				if (RiverRun == 0 || RiverRun > 8 || !Terrain->IsPlayableCoord(End)) continue;
				++State->Candidates;
				const FAftershockRoadQuote Quote = Roads->QuoteRoad(Cell.Coords, End);
				if (!IsSafeBridgePath(*Terrain, Quote)) continue;
				++State->ValidQuotes;
				if (!Economy->CanAfford(Quote.CostTL)) continue;
				if (State->Quote.bRouteValid && (Quote.CostTL > State->Quote.CostTL
					|| (Quote.CostTL == State->Quote.CostTL && Quote.Path.Num() >= State->Quote.Path.Num()))) continue;
				State->Quote = Quote; State->Start = Cell.Coords; State->End = End;
			}
		}
		State->bQuoteReadOnly = PreviewBefore == BridgeTerrainBytes(*Terrain, false, false)
			&& Economy->TreasuryTL == State->TreasuryBefore && Roads->CountRoadCells() == State->RoadsBefore && Roads->GetBridgeCount() == State->BridgesBefore;
		if (!State->bQuoteReadOnly) { State->Failure = TEXT("Preview unexpectedly mutated canonical state"); return; }
		if (!State->Quote.bRouteValid) { State->Failure = TEXT("No safe affordable bridge quote on the actual startup terrain; no terrain or budget workaround applied"); return; }
		State->bOriginalPathSafe = IsSafeBridgePath(*Terrain, State->Quote);
		FAftershockRoadQuote Actual;
		++State->PurchaseCalls;
		State->bBuilt = Roads->BuildRoad(State->Start, State->End, Actual);
		State->TreasuryAfter = Economy->TreasuryTL;
		State->bReturnedQuoteEqual = SameBridgeQuote(State->Quote, Actual);
		State->bExactCharge = State->bBuilt && State->TreasuryBefore - State->TreasuryAfter == State->Quote.CostTL;
		if (State->bBuilt) State->BridgeID = Terrain->GetCell(State->Quote.Path[0]).BridgeID;
		else State->Failure = TEXT("The single canonical BuildRoad purchase failed: ") + Actual.Feedback.ToString();
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_BRIDGE_RUNTIME_PURCHASE built=%d candidates=%d validQuotes=%d purchaseCalls=%d costTL=%d actualChargeTL=%lld quoteReadOnly=%d bridgeID=%d terrainHack=0"),
			State->bBuilt, State->Candidates, State->ValidQuotes, State->PurchaseCalls, State->Quote.CostTL, State->TreasuryBefore - State->TreasuryAfter, State->bQuoteReadOnly, State->BridgeID);
	}), 9.f, false);

	FTimerHandle CaptureTimer;
	World.GetTimerManager().SetTimer(CaptureTimer, FTimerDelegate::CreateLambda([WeakWorld, State, CaptureScene = MoveTemp(CaptureScene)]()
	{
		UWorld* W = WeakWorld.Get();
		if (!IsBridgeAuditWorld(W)) return;
		const auto* Terrain = W->GetSubsystem<UTerrainGridSubsystem>();
		const auto* Roads = W->GetSubsystem<URoadNetworkSubsystem>();
		const auto* Economy = W->GetSubsystem<UEconomyAndStateSubsystem>();
		if (!Terrain || !Roads || !Economy) State->Failure = TEXT("Required current-world subsystem absent at capture");
		const bool bTerrainPreserved = Terrain && State->TerrainBefore.Num() > 0 && State->TerrainBefore == BridgeTerrainBytes(*Terrain, false);
		const TArray<uint8> CurrentRiver = Terrain ? BridgeTerrainBytes(*Terrain, true) : TArray<uint8>();
		const bool bRiverPreserved = State->RiverBefore.Num() > 0 && State->RiverBefore == CurrentRiver;
		bool bSurfaceEqual = State->bBuilt && State->BridgeID != INDEX_NONE && Terrain && Roads;
		FBox Bounds(ForceInit);
		if (bSurfaceEqual) for (int32 I = 0; I < State->Quote.Path.Num(); ++I)
		{
			const FIntPoint Coord = State->Quote.Path[I];
			const FGridCell Cell = Terrain->GetCell(Coord);
			const FVector Surface = Roads->RoadGridToWorld(Coord);
			Bounds += Surface;
			bSurfaceEqual &= Cell.bHasRoad && Cell.BridgeID == State->BridgeID && Cell.BridgeAxis == State->Quote.BridgeAxis
				&& Cell.bBridgeEndpoint == (I == 0 || I == State->Quote.Path.Num() - 1)
				&& FMath::IsNearlyEqual(Cell.RoadSurfaceElevationCm, State->Quote.SurfaceElevationsCm[I], .01f)
				&& FMath::IsNearlyEqual(Roads->GetRoadSurfaceElevation(Coord), State->Quote.SurfaceElevationsCm[I], .01f)
				&& FMath::IsNearlyEqual(Surface.Z, double(State->Quote.SurfaceElevationsCm[I]), .01);
		}
		int32 Hosts = 0, StructureInstances = 0, MatchedDeckCells = 0;
		FString MeshPath, MaterialPath;
		bool bRenderReady = false;
		for (TActorIterator<AAftershockCityPresentationActor> It(W); It; ++It)
		{
			if (It->IsActorBeingDestroyed()) continue;
			++Hosts;
			TArray<UHierarchicalInstancedStaticMeshComponent*> Components; It->GetComponents(Components);
			for (const auto* Component : Components)
			{
				if (!Component || Component->GetFName() != TEXT("BridgeStructureHISM")) continue;
				StructureInstances += Component->GetInstanceCount();
				if (Component->GetStaticMesh()) MeshPath = Component->GetStaticMesh()->GetPathName();
				if (Component->GetMaterial(0)) MaterialPath = Component->GetMaterial(0)->GetPathName();
				bRenderReady = Component->GetStaticMesh() && Component->GetMaterial(0) && Component->IsRegistered()
					&& Component->IsVisible() && Component->IsRenderStateCreated() && !It->IsHidden();
				if (!State->bBuilt || !Roads || !Terrain) continue;
				for (FIntPoint Coord : State->Quote.Path)
				{
					const FVector DeckCenter = Roads->RoadGridToWorld(Coord) - FVector(0, 0, 25);
					const FVector DeckScale(Terrain->CellSize * 1.01f / 100.f, Terrain->CellSize / 100.f, .45f);
					for (int32 I = 0; I < Component->GetInstanceCount(); ++I)
					{
						FTransform Transform;
						if (Component->GetInstanceTransform(I, Transform, true) && Transform.GetLocation().Equals(DeckCenter, .1)
							&& Transform.GetScale3D().Equals(DeckScale, .0001)) { ++MatchedDeckCells; break; }
					}
				}
			}
		}
		const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PresentationValidation"));
		IFileManager::Get().MakeDirectory(*Folder, true);
		const FString ScenePath = Folder / TEXT("studio-bridge-runtime.png");
		bool bSceneSaved = false;
		FMinimalViewInfo View;
		if (Bounds.IsValid && State->bBuilt && CaptureScene)
		{
			const FVector Target = Bounds.GetCenter();
			const double Span = FMath::Max(2500., Bounds.GetSize().Size2D() + 1800.);
			View.Location = Target + FVector(-.78, -.94, .95).GetSafeNormal() * Span * 1.65;
			View.Rotation = (Target - View.Location).Rotation();
			View.FOV = 48.f; View.AspectRatio = 1.6f; View.ProjectionMode = ECameraProjectionMode::Perspective;
			// Do not accept a PNG left by a previous process after a failed capture.
			IFileManager::Get().Delete(*ScenePath, false, true);
			bSceneSaved = CaptureScene(*W, View, ScenePath) && IsFreshBridgePNG(ScenePath);
		}
		const bool bOneBridge = Roads && State->bBuilt && Roads->GetBridgeCount() == State->BridgesBefore + 1;
		const bool bExactRoadDelta = Roads && State->bBuilt && Roads->CountRoadCells() == State->RoadsBefore + State->Quote.NewRoadCells;
		const bool bCostStillExact = Economy && State->bExactCharge && State->TreasuryBefore - Economy->TreasuryTL == State->Quote.CostTL;
		const bool bDeckMatches = State->bBuilt && MatchedDeckCells == State->Quote.Path.Num()
			&& StructureInstances >= State->StructureInstancesBefore + State->Quote.Path.Num() * 3;
		const bool bPassed = State->bBuilt && State->PurchaseCalls == 1 && State->bQuoteReadOnly && State->bOriginalPathSafe
			&& State->bReturnedQuoteEqual && bCostStillExact && bOneBridge && bExactRoadDelta && bTerrainPreserved && bRiverPreserved
			&& bSurfaceEqual && Hosts == 1 && bRenderReady && bDeckMatches && bSceneSaved;
		const uint32 BeforeHash = State->RiverBefore.Num() ? FCrc::MemCrc32(State->RiverBefore.GetData(), State->RiverBefore.Num()) : 0;
		const uint32 AfterHash = CurrentRiver.Num() ? FCrc::MemCrc32(CurrentRiver.GetData(), CurrentRiver.Num()) : 0;
		const FString JSON = FString::Printf(TEXT("{\n\"schema_version\":1,\"created_utc\":\"%s\",\"passed\":%s,\"scope\":\"Opt-in real startup terrain bridge purchase, scene-only 1600x1000 capture and render/native surface contract. No pedestrian or traffic crossing, HUD, performance or hazard accuracy claim.\",\"world_seconds\":%.3f,\"failure\":\"%s\",\"candidate_runs\":%d,\"valid_quotes\":%d,\"purchase_calls\":%d,\"built\":%s,\"bridge_id\":%d,\"quoted_cost_tl\":%d,\"actual_charge_tl\":%lld,\"quote_read_only\":%s,\"original_path_safe\":%s,\"returned_quote_equal\":%s,\"exact_cost\":%s,\"one_bridge_added\":%s,\"exact_road_delta\":%s,\"non_road_terrain_bytes_preserved\":%s,\"river_bytes_preserved\":%s,\"river_hash_before\":\"%08x\",\"river_hash_after\":\"%08x\",\"canonical_surface_matches_quote\":%s,\"path_cells\":%d,\"river_cells\":%d,\"approach_cells\":%d,\"presentation_hosts\":%d,\"structure_instances_before\":%d,\"structure_instances_after\":%d,\"matched_deck_cells\":%d,\"render_ready\":%s,\"mesh\":\"%s\",\"material\":\"%s\",\"scene_saved_1600x1000\":%s,\"scene\":\"%s\",\"camera_location\":\"%s\",\"camera_rotation\":\"%s\"\n}\n"),
			*FDateTime::UtcNow().ToIso8601(), bPassed ? TEXT("true") : TEXT("false"), W->GetTimeSeconds(), *EscapeBridgeJson(State->Failure),
			State->Candidates, State->ValidQuotes, State->PurchaseCalls, State->bBuilt ? TEXT("true") : TEXT("false"), State->BridgeID,
			State->Quote.CostTL, State->TreasuryBefore - State->TreasuryAfter, State->bQuoteReadOnly ? TEXT("true") : TEXT("false"),
			State->bOriginalPathSafe ? TEXT("true") : TEXT("false"), State->bReturnedQuoteEqual ? TEXT("true") : TEXT("false"), bCostStillExact ? TEXT("true") : TEXT("false"),
			bOneBridge ? TEXT("true") : TEXT("false"), bExactRoadDelta ? TEXT("true") : TEXT("false"), bTerrainPreserved ? TEXT("true") : TEXT("false"), bRiverPreserved ? TEXT("true") : TEXT("false"), BeforeHash, AfterHash,
			bSurfaceEqual ? TEXT("true") : TEXT("false"), State->Quote.Path.Num(), State->Quote.BridgeRiverCells, State->Quote.BridgeApproachCells,
			Hosts, State->StructureInstancesBefore, StructureInstances, MatchedDeckCells, bRenderReady ? TEXT("true") : TEXT("false"), *EscapeBridgeJson(MeshPath), *EscapeBridgeJson(MaterialPath),
			bSceneSaved ? TEXT("true") : TEXT("false"), *EscapeBridgeJson(ScenePath), *EscapeBridgeJson(View.Location.ToCompactString()), *EscapeBridgeJson(View.Rotation.ToCompactString()));
		const FString ReportPath = Folder / TEXT("bridge_runtime_audit.json");
		const bool bReportSaved = FFileHelper::SaveStringToFile(JSON, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (bPassed && bReportSaved)
		{
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_BRIDGE_RUNTIME_RESULT passed=1 bridgeID=%d costTL=%d purchaseCalls=1 pathCells=%d riverCells=%d matchedDeckCells=%d structureInstances=%d riverHash=%08x sceneSaved=1 scene=%s report=%s trafficCrossingClaim=0"),
				State->BridgeID, State->Quote.CostTL, State->Quote.Path.Num(), State->Quote.BridgeRiverCells, MatchedDeckCells, StructureInstances, AfterHash, *ScenePath, *ReportPath);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_BRIDGE_RUNTIME_RESULT passed=0 built=%d exactCost=%d oneBridge=%d roadDelta=%d terrainPreserved=%d riverPreserved=%d surface=%d hosts=%d renderReady=%d deckMatches=%d sceneSaved=%d reportSaved=%d reason=%s report=%s"),
				State->bBuilt, bCostStillExact, bOneBridge, bExactRoadDelta, bTerrainPreserved, bRiverPreserved, bSurfaceEqual, Hosts, bRenderReady, bDeckMatches, bSceneSaved, bReportSaved, *State->Failure, *ReportPath);
		}
	}), 12.f, false);
#endif
}
