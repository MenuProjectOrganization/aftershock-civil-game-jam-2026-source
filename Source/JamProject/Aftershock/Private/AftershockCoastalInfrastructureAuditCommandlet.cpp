#include "AftershockCoastalInfrastructureAuditCommandlet.h"

#if WITH_EDITOR
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	struct FCoastalCheck { FString Name; bool Passed; FString Detail; };
	struct FScopedCoastalWorld
	{
		UWorld* World = nullptr;
		~FScopedCoastalWorld()
		{
			if (!World) return;
			World->DestroyWorld(false);
			if (GEngine) GEngine->DestroyWorldContext(World);
			World->RemoveFromRoot();
		}
	};
	FString CoastalJsonEscape(FString Value)
	{
		Value.ReplaceInline(TEXT("\\"), TEXT("\\\\")); Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Value.ReplaceInline(TEXT("\n"), TEXT("\\n")); Value.ReplaceInline(TEXT("\r"), TEXT("\\r")); Value.ReplaceInline(TEXT("\t"), TEXT("\\t"));
		return Value;
	}
	bool EqualSeverityMaps(const TMap<FIntPoint, float>& A, const TMap<FIntPoint, float>& B)
	{
		if (A.Num() != B.Num()) return false;
		for (const TPair<FIntPoint, float>& Pair : A)
		{
			const float* Found = B.Find(Pair.Key);
			if (!Found || !FMath::IsNearlyEqual(Pair.Value, *Found, .0001f)) return false;
		}
		return true;
	}
}

UAftershockCoastalInfrastructureAuditCommandlet::UAftershockCoastalInfrastructureAuditCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UAftershockCoastalInfrastructureAuditCommandlet::Main(const FString& Params)
{
	TArray<FCoastalCheck> Checks;
	int32 Failed = 0;
	const auto Check = [&](const TCHAR* Name, bool Passed, const FString& Detail)
	{
		Checks.Add({ Name, Passed, Detail }); Failed += Passed ? 0 : 1;
		if (Passed) { UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_COASTAL_AUDIT_PASS %s: %s"), Name, *Detail); }
		else { UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_COASTAL_AUDIT_FAIL %s: %s"), Name, *Detail); }
	};
	const auto Finish = [&]()
	{
		const FString Directory = FPaths::ProjectSavedDir() / TEXT("PresentationValidation");
		IFileManager::Get().MakeDirectory(*Directory, true);
		const FString Path = Directory / TEXT("coastal_infrastructure_audit.json");
		FString Json = FString::Printf(TEXT("{\n\"schema_version\":1,\n\"utc\":\"%s\",\n\"scope\":\"Actual canonical Engine APIs on an isolated12x16 authored fixture. Tsunami run-up and reservoir pulse units are game calibrations, not CFD, measured wave prediction, or cubic metres. No scene, GPU, UI, package or scientific safety claim.\",\n\"passed\":%d,\n\"failed\":%d,\n\"checks\":[\n"),
			*FDateTime::UtcNow().ToIso8601(), Checks.Num() - Failed, Failed);
		for (int32 I = 0; I < Checks.Num(); ++I)
		{
			const FCoastalCheck& C = Checks[I];
			Json += FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s,\"detail\":\"%s\"}%s\n"),
				*CoastalJsonEscape(C.Name), C.Passed ? TEXT("true") : TEXT("false"), *CoastalJsonEscape(C.Detail), I + 1 < Checks.Num() ? TEXT(",") : TEXT(""));
		}
		Json += TEXT("]\n}\n");
		if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return 2;
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_COASTAL_AUDIT_RESULT passed=%d failed=%d report=%s"), Checks.Num() - Failed, Failed, *Path);
		return Failed == 0 ? 0 : 1;
	};
	Check(TEXT("engine_initialized"), GEngine != nullptr, TEXT("Calls the real canonical C++ APIs, not a parallel numerical model."));
	if (!GEngine) return Finish();
	FScopedCoastalWorld Fixture;
	Fixture.World = UWorld::CreateWorld(EWorldType::Game, false, FName(TEXT("AftershockCoastalFixture")), GetTransientPackage(), true, ERHIFeatureLevel::SM5, nullptr, false);
	Check(TEXT("isolated_world_created"), Fixture.World != nullptr, TEXT("No existing map or authored asset is saved or edited."));
	if (!Fixture.World) return Finish();
	UWorld* World = Fixture.World;
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game); Context.SetCurrentWorld(World);
	UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>();
	UBuildingManagerSubsystem* Buildings = World->GetSubsystem<UBuildingManagerSubsystem>();
	UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>();
	UDisasterSimulationSubsystem* Simulation = World->GetSubsystem<UDisasterSimulationSubsystem>();
	Check(TEXT("canonical_subsystems_ready"), Terrain && Buildings && Economy && Simulation, TEXT("All data and transactions belong to this one isolated world."));
	if (!Terrain || !Buildings || !Economy || !Simulation) return Finish();
	constexpr int32 W = 12, H = 16;
	Terrain->GenerateProceduralTerrain(W,H,500.f,1337); Terrain->GridOrigin = FVector::ZeroVector;
	for (int32 Y = 0; Y < H; ++Y) for (int32 X = 0; X < W; ++X)
	{
		FGridCell C; C.Coords = FIntPoint(X, Y); C.bIsPlayable = true; C.bIsRiverbed = X == 3 || X == 9;
		C.Elevation = C.bIsRiverbed ? 30.f : 80.f; C.RiverCapacity = 1.f;
		Terrain->Cells[X + Y * W] = C;
	}
	TStrongObjectPtr<UAftershockCoastalAuditListener> Listener(NewObject<UAftershockCoastalAuditListener>(World));
	Simulation->OnDisasterCompleted.AddDynamic(Listener.Get(), &UAftershockCoastalAuditListener::RecordCompleted);
	Simulation->OnDamInfrastructureChanged.AddDynamic(Listener.Get(), &UAftershockCoastalAuditListener::RecordDamChanged);
	int32 SnapshotEvents = 0;
	const FDelegateHandle SnapshotHandle = Simulation->OnDisasterVisualSnapshotChanged.AddLambda([&SnapshotEvents](const FAftershockDisasterVisualSnapshot&) { ++SnapshotEvents; });
	const int64 InitialTreasury = Economy->TreasuryTL;
	const float InitialSatisfaction = Economy->PublicSatisfaction;
	FText Reason;
	const bool InlandPreflight = Simulation->CanTriggerDisaster(EDisasterType::Tsunami, Reason);
	const FDisasterReport Rejected = Simulation->TriggerDisaster(EDisasterType::Tsunami, 70.f, 1.f, true);
	Check(TEXT("inland_tsunami_rejected_without_any_outcome_event"), !InlandPreflight && !Reason.IsEmpty() && Rejected.bWasRejected
		&& Listener->Completions == 0 && SnapshotEvents == 0 && Simulation->GetLatestVisualSnapshot().Revision == 0
		&& Economy->TreasuryTL == InitialTreasury && Economy->PublicSatisfaction == InitialSatisfaction && Economy->PendingStateAidTL == 0,
		TEXT("No sea means no tsunami, snapshot, completion reward/aid or satisfaction change."));
	const TMap<FIntPoint, float> Baseline = Simulation->SimulateFloodMap(80.f);
	const FIntPoint DamCoord(3, 3);
	const FAftershockDamQuote Quote = Simulation->QuoteDam(DamCoord);
	int32 Cost = 0;
	const bool Bought = Simulation->InvestInDam(DamCoord, Cost);
	FAftershockDamStatus Status = Simulation->GetDamStatus(DamCoord);
	Check(TEXT("dam_one_atomic_payment_one_canonical_installation"), Quote.bCanBuild && Bought && Cost == Quote.CostTL && Cost == 12000
		&& InitialTreasury - Economy->TreasuryTL == Cost && Terrain->GetCell(DamCoord).bHasDam && Status.StorageCapacityUnits == 180.f && Status.StoredWaterUnits == 0.f && Listener->DamEvents == 1,
		TEXT("Quote and actual purchase agree; InvestInDam already charges the economy once and broadcasts after commit."));
	const int64 AfterPurchase = Economy->TreasuryTL;
	int32 DuplicateCost = -1, LandCost = -1, OutsideCost = -1;
	Check(TEXT("duplicate_land_outside_dam_requests_do_not_charge"), !Simulation->InvestInDam(DamCoord, DuplicateCost) && DuplicateCost == 0
		&& !Simulation->InvestInDam(FIntPoint(0,0), LandCost) && LandCost == 0 && !Simulation->InvestInDam(FIntPoint(-1,0), OutsideCost) && OutsideCost == 0
		&& Economy->TreasuryTL == AfterPurchase && Listener->DamEvents == 1,
		TEXT("Rejected placements preserve both treasury and canonical installation count."));
	Economy->TreasuryTL = Quote.CostTL - 1;
	int32 PoorCost = -1;
	const bool PoorAccepted = Simulation->InvestInDam(FIntPoint(3,5), PoorCost);
	Check(TEXT("unaffordable_dam_is_not_partially_installed"), !PoorAccepted && PoorCost == 0 && !Terrain->GetCell(FIntPoint(3,5)).bHasDam && Economy->TreasuryTL == Quote.CostTL - 1,
		TEXT("Explicit isolated treasury input probe is restored immediately after the rejected purchase."));
	Economy->TreasuryTL = AfterPurchase;
	const EWorldType::Type OriginalWorldType = World->WorldType; World->WorldType = EWorldType::Editor;
	const bool EditorAccepted = Simulation->QuoteDam(FIntPoint(3,5)).bCanBuild;
	World->WorldType = OriginalWorldType;
	Check(TEXT("non_game_world_dam_preflight_rejected"), !EditorAccepted && Economy->TreasuryTL == AfterPurchase,
		TEXT("Explicit restored WorldType input probe exercises the public preflight guard, not a separate editor world lifecycle."));
	const int32 EventsBeforePreview = Listener->DamEvents;
	const TMap<FIntPoint, float> PreviewA = Simulation->SimulateFloodMap(80.f), PreviewB = Simulation->SimulateFloodMap(80.f);
	Check(TEXT("flood_preview_is_repeatable_and_never_consumes_storage"), EqualSeverityMaps(PreviewA, PreviewB) && Simulation->GetDamStatus(DamCoord).StoredWaterUnits == 0.f && Listener->DamEvents == EventsBeforePreview,
		TEXT("Two actual public previews leave reservoir storage and infrastructure events unchanged."));
	Check(TEXT("dam_reduces_only_connected_downstream_river_pulse"), PreviewA.FindRef(FIntPoint(3,8)) < Baseline.FindRef(FIntPoint(3,8))
		&& FMath::IsNearlyEqual(PreviewA.FindRef(FIntPoint(3,1)), Baseline.FindRef(FIntPoint(3,1)))
		&& FMath::IsNearlyEqual(PreviewA.FindRef(FIntPoint(9,8)), Baseline.FindRef(FIntPoint(9,8)))
		&& FMath::IsNearlyEqual(PreviewA.FindRef(FIntPoint(0,5)), Baseline.FindRef(FIntPoint(0,5))),
		FString::Printf(TEXT("downstream %.3f->%.3f; upstream %.3f->%.3f; disconnected river %.3f->%.3f; direct rain ponding %.3f->%.3f"),
			Baseline.FindRef(FIntPoint(3,8)), PreviewA.FindRef(FIntPoint(3,8)), Baseline.FindRef(FIntPoint(3,1)), PreviewA.FindRef(FIntPoint(3,1)),
			Baseline.FindRef(FIntPoint(9,8)), PreviewA.FindRef(FIntPoint(9,8)), Baseline.FindRef(FIntPoint(0,5)), PreviewA.FindRef(FIntPoint(0,5))));
	Simulation->TriggerDisaster(EDisasterType::Flood, 80.f, 1.f, true);
	Status = Simulation->GetDamStatus(DamCoord);
	Check(TEXT("real_flood_commits_storage_once_and_caches_same_map"), FMath::IsNearlyEqual(Status.StoredWaterUnits, 52.f) && FMath::IsNearlyEqual(Status.LastRetainedUnits, 52.f)
		&& EqualSeverityMaps(PreviewA, Simulation->GetLatestVisualSnapshot().LocalSeverity) && Listener->DamEvents == EventsBeforePreview + 1,
		TEXT("80*.65=52 authored pulse units are retained once by the actual event; its hazard and visual snapshot share the preview map."));
	bool BoundedStorage = true;
	for (int32 Event = 0; Event < 4; ++Event)
	{
		Simulation->TriggerDisaster(EDisasterType::Flood, 100.f, 1.f, true);
		Status = Simulation->GetDamStatus(DamCoord);
		BoundedStorage &= FMath::IsFinite(Status.StoredWaterUnits) && FMath::IsFinite(Status.RemainingStorageUnits)
			&& Status.StoredWaterUnits >= 0.f && Status.StoredWaterUnits <= Status.StorageCapacityUnits && Status.RemainingStorageUnits >= 0.f;
	}
	Check(TEXT("finite_capacity_exhausts_and_overtops"), BoundedStorage && FMath::IsNearlyEqual(Status.StoredWaterUnits, 180.f)
		&& FMath::IsNearlyZero(Status.RemainingStorageUnits) && Status.bOvertopped && Status.LastOvertoppingUnits > 0.f && FMath::IsNearlyZero(Status.LastRetainedUnits),
		TEXT("Repeated real events fill the finite180-unit reservoir. A full dam retains zero additional pulse and reports overtopping; no infinite protection."));
	FGridCell OccupiedSea = Terrain->GetCell(FIntPoint(0,14)); OccupiedSea.OccupyingBuildingID = 777; Terrain->SetCell(OccupiedSea.Coords, OccupiedSea);
	const bool ErasedOccupiedBand = Terrain->ConfigureCoastFromSouthernEdge(12);
	Check(TEXT("coast_configuration_cannot_erase_existing_occupancy"), !ErasedOccupiedBand && !Terrain->IsCoastalWorld() && Terrain->GetCell(OccupiedSea.Coords).OccupyingBuildingID == 777,
		TEXT("Explicit isolated occupancy probe prevents the entire coast transaction before any ocean flags change."));
	OccupiedSea.OccupyingBuildingID = INDEX_NONE; Terrain->SetCell(OccupiedSea.Coords, OccupiedSea);
	const bool CoastConfigured = Terrain->ConfigureCoastFromSouthernEdge(12);
	bool OceanValid = CoastConfigured && Terrain->IsCoastalWorld();
	for (const FGridCell& Cell : Terrain->Cells) if (Cell.Coords.Y >= 12) OceanValid &= Cell.bIsOcean && !Cell.bIsPlayable && !Cell.bIsRiverbed;
	int32 SeaCost = -1;
	Check(TEXT("canonical_ocean_rejects_dams_and_land_prevention"), OceanValid && !Simulation->InvestInDam(FIntPoint(3,14), SeaCost) && SeaCost == 0
		&& !Terrain->UpgradeCellDrainage(FIntPoint(3,14), .25f) && !Terrain->UpgradeRiverPrevention(FIntPoint(3,14), 1.f, 0.f),
		TEXT("Ocean is one canonical nonplayable/nonriver source, not a second renderer-only sea or a buildable/dam parcel."));
	// A full-width high ridge makes reachability/elevation shielding unambiguous.
	for (int32 X = 0; X < W; ++X) { FGridCell C = Terrain->GetCell(FIntPoint(X,7)); C.Elevation = 1000.f; Terrain->SetCell(C.Coords, C); }
	const TMap<FIntPoint, float> TsunamiMap = Simulation->SimulateTsunamiMap(60.f);
	bool MapFinite = !TsunamiMap.IsEmpty();
	for (const TPair<FIntPoint, float>& Pair : TsunamiMap) MapFinite &= FMath::IsFinite(Pair.Value) && Pair.Value > 0.f && Pair.Value <= 60.f;
	Check(TEXT("tsunami_attentuates_and_cannot_cross_high_ridge"), MapFinite && TsunamiMap.FindRef(FIntPoint(5,11)) > TsunamiMap.FindRef(FIntPoint(5,9))
		&& TsunamiMap.FindRef(FIntPoint(5,9)) > 0.f && !TsunamiMap.Contains(FIntPoint(5,7)) && !TsunamiMap.Contains(FIntPoint(5,5)),
		TEXT("Only ocean-connected land below the authored crest is reached; inland distance attenuates the pulse and a1000cm ridge blocks it."));
	FGridCell DamState = Terrain->GetCell(DamCoord), DamDisabled = DamState; DamDisabled.bHasDam = false; Terrain->SetCell(DamCoord, DamDisabled);
	const TMap<FIntPoint, float> WithoutDamTsunami = Simulation->SimulateTsunamiMap(60.f);
	Terrain->SetCell(DamCoord, DamState);
	Check(TEXT("inland_dam_provides_no_tsunami_protection"), EqualSeverityMaps(TsunamiMap, WithoutDamTsunami), TEXT("Explicit restored installed-flag probe leaves the actual tsunami field identical."));
	UBuildingData* Data = NewObject<UBuildingData>(Buildings);
	Data->DisplayName = FText::FromString(TEXT("Coastal audit house")); Data->Type = EBuildingType::House; Data->Residents = 4; Data->Workers = 0;
	Data->Material = EMaterialType::Wood; Data->MeshIntact = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	int32 CoastalID = INDEX_NONE, SafeID = INDEX_NONE;
	const bool CoastalPlaced = Buildings->PlaceBuilding(FIntPoint(5,11), Data, EFoundationQuality::Medium, 1, CoastalID);
	const bool SafePlaced = Buildings->PlaceBuilding(FIntPoint(5,5), Data, EFoundationQuality::Medium, 1, SafeID);
	Check(TEXT("coastal_and_shielded_building_fixtures_exist"), CoastalPlaced && SafePlaced, TEXT("Both records use canonical BuildingManager placement; no parallel casualty/population registry."));
	FBuildingInstance Coastal; Buildings->GetBuildingByID(CoastalID, Coastal);
	FBuildingInstance DryDrainProbe = Coastal, GoodDrainProbe = Coastal;
	const FDisasterOutcome NoDrainOutcome = Terrain->EvaluateBuildingInDisaster(DryDrainProbe, EDisasterType::Tsunami, 50.f, 1.f, true);
	FGridCell CoastalCell = Terrain->GetCell(FIntPoint(5,11)), DrainedCell = CoastalCell; DrainedCell.DrainageQuality = 1.f; Terrain->SetCell(DrainedCell.Coords, DrainedCell);
	const FDisasterOutcome DrainOutcome = Terrain->EvaluateBuildingInDisaster(GoodDrainProbe, EDisasterType::Tsunami, 50.f, 1.f, true);
	Terrain->SetCell(CoastalCell.Coords, CoastalCell);
	Check(TEXT("tsunami_reuses_flood_susceptibility_without_fake_drain_defence"), FMath::IsNearlyEqual(Data->GetBaseResistance(EDisasterType::Tsunami), Data->GetBaseResistance(EDisasterType::Flood))
		&& FMath::IsNearlyEqual(NoDrainOutcome.FinalDamage, DrainOutcome.FinalDamage),
		TEXT("Flood material/foundation susceptibility is shared, while ordinary storm-drain quality does not reduce the new tsunami damage."));
	const FAftershockDamStatus BeforeTsunami = Simulation->GetDamStatus(DamCoord);
	const int32 DamEventsBeforeTsunami = Listener->DamEvents;
	const FDisasterReport CoastalReport = Simulation->TriggerDisaster(EDisasterType::Tsunami, 60.f, 1.f, true);
	FBuildingInstance CoastalAfter, SafeAfter; Buildings->GetBuildingByID(CoastalID, CoastalAfter); Buildings->GetBuildingByID(SafeID, SafeAfter);
	const FAftershockDisasterVisualSnapshot& Snapshot = Simulation->GetLatestVisualSnapshot();
	Check(TEXT("actual_tsunami_damages_reached_lot_only_and_retains_event_identity"), !CoastalReport.bWasRejected && CoastalReport.DisasterType == EDisasterType::Tsunami
		&& CoastalAfter.CurrentDamage > 0.f && SafeAfter.CurrentDamage == 0.f && CoastalReport.TotalBuildingsAffected == 1 && CoastalReport.TotalDeaths == 0
		&& Snapshot.Type == EDisasterType::Tsunami && Snapshot.DurationSeconds == 32.f && Snapshot.PropagationDirection.Equals(FVector(0,-1,0))
		&& FMath::IsNearlyEqual(Snapshot.SurgePeakHeadCm, Terrain->CoastalSeaLevelCm + 360.f) && EqualSeverityMaps(Snapshot.LocalSeverity, TsunamiMap),
		TEXT("Actual damage consumes the same coast-connected map as the typed32-second snapshot; the shielded inland lot remains untouched."));
	Check(TEXT("tsunami_does_not_consume_inland_reservoir_storage"), Simulation->GetDamStatus(DamCoord).StoredWaterUnits == BeforeTsunami.StoredWaterUnits && Listener->DamEvents == DamEventsBeforeTsunami,
		TEXT("A tsunami is not processed as a rainfall/reservoir pulse."));
	Simulation->OnDisasterCompleted.RemoveDynamic(Listener.Get(), &UAftershockCoastalAuditListener::RecordCompleted);
	Simulation->OnDamInfrastructureChanged.RemoveDynamic(Listener.Get(), &UAftershockCoastalAuditListener::RecordDamChanged);
	Simulation->OnDisasterVisualSnapshotChanged.Remove(SnapshotHandle);
	return Finish();
}
#else
UAftershockCoastalInfrastructureAuditCommandlet::UAftershockCoastalInfrastructureAuditCommandlet() {}
int32 UAftershockCoastalInfrastructureAuditCommandlet::Main(const FString& Params) { return 1; }
#endif
