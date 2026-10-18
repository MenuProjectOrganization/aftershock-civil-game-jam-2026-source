#include "AftershockChunkSaveAuditCommandlet.h"

#if WITH_EDITOR
#include "Aftershock/Public/AftershockSaveGame.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/CityPresentationSubsystem.h"
#include "AftershockTerrainSurface.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

namespace
{
struct FChunkSaveWorld
{
	TStrongObjectPtr<UGameInstance> Instance;
	UWorld* World = nullptr;
	~FChunkSaveWorld()
	{
		if (Instance.IsValid()) Instance->Shutdown();
		if (World)
		{
			World->DestroyWorld(false);
			if (GEngine) GEngine->DestroyWorldContext(World);
			World->RemoveFromRoot();
		}
	}
};

FString EscapeChunkSaveJson(FString Value)
{
	Value.ReplaceInline(TEXT("\\"), TEXT("\\\\")); Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Value.ReplaceInline(TEXT("\n"), TEXT("\\n")); Value.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Value.ReplaceInline(TEXT("\t"), TEXT("\\t")); return Value;
}

TArray<uint8> ChunkCellBytes(FGridCell Cell, bool bIgnorePlayable = false, bool bIgnoreNewProvenance = false)
{
	if (bIgnorePlayable) Cell.bIsPlayable = false;
	if (bIgnoreNewProvenance) { Cell.bAuthoredContinuation = false; Cell.bHasRealElevationSample = false; }
	TArray<uint8> Bytes; FMemoryWriter Writer(Bytes, true); FObjectAndNameAsStringProxyArchive Archive(Writer, false);
	FGridCell::StaticStruct()->SerializeItem(Archive, &Cell, nullptr); return Bytes;
}

TArray<uint8> ChunkWorldBytes(UAftershockSaveSubsystem& Saves, UWorld& World)
{
	FText Error; TStrongObjectPtr<UAftershockSaveGame> Snapshot(Saves.CaptureWorld(&World, Error));
	TArray<uint8> Bytes;
	if (Snapshot.IsValid()) { Snapshot->SavedAt = FDateTime(0); UGameplayStatics::SaveGameToMemory(Snapshot.Get(), Bytes); }
	return Bytes;
}
TArray<uint8> ChunkBuildingBytes(UBuildingManagerSubsystem& Buildings)
{
	FAftershockBuildingsSaveState State;Buildings.CaptureSaveState(State);TArray<uint8> Bytes;FMemoryWriter Writer(Bytes,true);FObjectAndNameAsStringProxyArchive Archive(Writer,false);
	FAftershockBuildingsSaveState::StaticStruct()->SerializeItem(Archive,&State,nullptr);return Bytes;
}

void RemoveChunkAuditSlot()
{
	const FString Path = UAftershockSaveSubsystem::SlotPath(TEXT("Audit"));
	for (const FString& File : { Path, Path + TEXT(".bak"), Path + TEXT(".tmp"), Path + TEXT(".bak.tmp") })
		IFileManager::Get().Delete(*File, false, true);
}

struct FLegacyChunkWireReceipt
{
	int32 PayloadBytes = 0, RawEnvelopeBytes = 0, StoredEnvelopeBytes = 0, EnvelopeVersion = 0;
	bool bRawWithinDiskLimit = false, bRawDiskBytesExact = false, bRawDiskAccepted = false, bTaggedPayloadDecoded = false, bDiskBytesExact = false;
	FString Describe(int32 Schema) const
	{
		return FString::Printf(TEXT("schema=%d generatedCurrentGVAS=1 payloadBytes=%d rawEnvelopeBytes=%d rawWithin64MiB=%d rawDiskBytesExact=%d rawDiskAccepted=%d envelopeVersion=%d storedEnvelopeBytes=%d diskBytesExact=%d taggedPayloadDecoded=%d"),
			Schema, PayloadBytes, RawEnvelopeBytes, bRawWithinDiskLimit, bRawDiskBytesExact, bRawDiskAccepted, EnvelopeVersion, StoredEnvelopeBytes, bDiskBytesExact, bTaggedPayloadDecoded);
	}
};

// Deliberately bypass WriteSnapshot's eager migration so the disk reader sees
// a checksum-valid schema-1/2 typed payload with legacy-field defaults. These
// are generated current-GVAS fixtures, not archived historical executables.
// A modern dense city's tagged fields can exceed the supported raw envelope's
// 64MiB disk limit. Exercise that rejection, then use the already supported
// envelope2/zlib transport; SessionSaveAudit separately retains raw1 coverage.
bool WriteLegacyChunkAuditPayload(UAftershockSaveSubsystem& Saves, UAftershockSaveGame& Legacy, FLegacyChunkWireReceipt& Receipt)
{
	constexpr int32 MaximumDiskBytes = 64 * 1024 * 1024, MaximumPayloadBytes = 256 * 1024 * 1024;
	TArray<uint8> Payload;
	if (!UGameplayStatics::SaveGameToMemory(&Legacy, Payload) || Payload.IsEmpty()) return false;
	Receipt.PayloadBytes = Payload.Num();
	if (Payload.Num() > MaximumPayloadBytes) return false;
	{
		TStrongObjectPtr<UAftershockSaveGame> TaggedDecoded(Cast<UAftershockSaveGame>(UGameplayStatics::LoadGameFromMemory(Payload)));
		Receipt.bTaggedPayloadDecoded = TaggedDecoded.IsValid() && TaggedDecoded->SchemaVersion == Legacy.SchemaVersion
			&& TaggedDecoded->Terrain.Cells.Num() == Legacy.Terrain.Cells.Num();
	}
	uint8 Digest[FSHA1::DigestSize];
	FSHA1::HashBuffer(Payload.GetData(), Payload.Num(), Digest);
	const auto MakeEnvelope = [&](const TArray<uint8>& Body, int32 Version, TArray<uint8>& Out)
	{
		FMemoryWriter Writer(Out, true); uint32 Magic = 0x41465348; int32 Count = Body.Num(), Uncompressed = Payload.Num();
		Writer << Magic << Version << Count; if (Version == 2) Writer << Uncompressed;
		Writer.Serialize(Digest, sizeof(Digest)); Writer.Serialize(const_cast<uint8*>(Body.GetData()), Body.Num()); Writer.Close();
		return !Writer.IsError();
	};
	TArray<uint8> Envelope;
	if (!MakeEnvelope(Payload, 1, Envelope)) return false;
	Receipt.RawEnvelopeBytes = Envelope.Num(); Receipt.bRawWithinDiskLimit = Envelope.Num() <= MaximumDiskBytes;
	const FString Path = UAftershockSaveSubsystem::SlotPath(TEXT("Audit"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	if (!FFileHelper::SaveArrayToFile(Envelope, *Path)) return false;
	{
		TArray<uint8> RawDiskBytes;
		Receipt.bRawDiskBytesExact = FFileHelper::LoadFileToArray(RawDiskBytes, *Path) && RawDiskBytes == Envelope;
		if (!Receipt.bRawDiskBytesExact) return false;
		// Observe the real PUBLIC disk reader, including its size guard. Do not
		// infer oversized-file rejection merely from a duplicate fixture constant.
		FText RawError; TStrongObjectPtr<UAftershockSaveGame> RawRead(Saves.ReadSnapshot(TEXT("Audit"), RawError));
		Receipt.bRawDiskAccepted = RawRead.IsValid();
	}
	Receipt.EnvelopeVersion = 1;
	if (!Receipt.bRawWithinDiskLimit)
	{
		Envelope.Reset(); int32 CompressedSize = FCompression::CompressMemoryBound(NAME_Zlib, Payload.Num());
		TArray<uint8> Compressed; Compressed.SetNumUninitialized(CompressedSize);
		if (!FCompression::CompressMemory(NAME_Zlib, Compressed.GetData(), CompressedSize, Payload.GetData(), Payload.Num(), COMPRESS_BiasSpeed)) return false;
		Compressed.SetNum(CompressedSize, EAllowShrinking::No);
		if (!MakeEnvelope(Compressed, 2, Envelope)) return false;
		Receipt.EnvelopeVersion = 2;
	}
	Receipt.StoredEnvelopeBytes = Envelope.Num();
	if (Envelope.Num() > MaximumDiskBytes) return false;
	if (!FFileHelper::SaveArrayToFile(Envelope, *Path)) return false;
	TArray<uint8> DiskBytes; Receipt.bDiskBytesExact = FFileHelper::LoadFileToArray(DiskBytes, *Path) && DiskBytes == Envelope;
	return Receipt.bDiskBytesExact;
}

void ResetChunkMetadataForLegacy(UAftershockSaveGame& Snapshot, int32 Schema)
{
	Snapshot.SchemaVersion = Schema; Snapshot.Terrain.TerrainStorageVersion = 1;
	Snapshot.Terrain.ChunkSize = 25; Snapshot.Terrain.ContinuationGeneratorVersion = 1;
	Snapshot.Terrain.TerrainRevision = 0; Snapshot.Terrain.Chunks.Reset();
	Snapshot.Terrain.bUsesRegionalLayout = false; Snapshot.Terrain.bInitialMaskUnlocked = false;
	// Explicit old-wire fixture, matching the SessionSave legacy fixture: V1/2
	// contained none of the V5 conserved physical state. Merely relabelling a
	// current snapshot is correctly rejected by the production migration guard.
	Snapshot.Physics = {};
	for (auto& Cell : Snapshot.Terrain.Cells)
	{
		Cell.bAuthoredContinuation = false; Cell.bHasRealElevationSample = false;
		Cell.SnowWaterEquivalentMm = 0.f; Cell.RegolithDepthM = 0.f; Cell.SoilPoreWaterDepthM = 0.f;
		Cell.GroundErodedDepthM = 0.f; Cell.GroundDepositedDepthM = 0.f; Cell.MaterialScenarioVersion = 0;
	}
}

bool FindRealChunkBridge(const UTerrainGridSubsystem& Terrain, const URoadNetworkSubsystem& Roads,
	const UEconomyAndStateSubsystem& Economy, FIntPoint& Start, FIntPoint& End, FAftershockRoadQuote& Best)
{
	const FIntPoint Directions[] = { FIntPoint(1, 0), FIntPoint(0, 1) };
	for (const FGridCell& Cell : Terrain.Cells)
	{
		if (!Terrain.IsPlayableCoord(Cell.Coords) || Cell.bIsRiverbed) continue;
		for (FIntPoint Direction : Directions)
		{
			FIntPoint Other = Cell.Coords + Direction; int32 RiverCells = 0;
			while (Terrain.IsValidCoord(Other) && Terrain.GetCell(Other).bIsRiverbed && RiverCells <= 8) { Other += Direction; ++RiverCells; }
			if (RiverCells < 1 || RiverCells > 8 || !Terrain.IsPlayableCoord(Other)) continue;
			const auto Quote = Roads.QuoteRoad(Cell.Coords, Other);
			if (!Quote.bRouteValid || !Quote.bIncludesBridge || Quote.Path.Num() < 3 || !Economy.CanAfford(Quote.CostTL)) continue;
			bool bCrossesChunk = false;
			for (FIntPoint Coord : Quote.Path) bCrossesChunk |= UTerrainGridSubsystem::CellToChunk(Coord) != UTerrainGridSubsystem::CellToChunk(Quote.Path[0]);
			if (!bCrossesChunk || (Best.bRouteValid && Best.CostTL <= Quote.CostTL)) continue;
			Best = Quote; Start = Cell.Coords; End = Other;
		}
	}
	return Best.bRouteValid;
}
}

UAftershockChunkSaveAuditCommandlet::UAftershockChunkSaveAuditCommandlet()
{ IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true; }

int32 UAftershockChunkSaveAuditCommandlet::Main(const FString& Params)
{
	int32 Passed = 0, Failed = 0, UnlockedCells = 0, NativeBridgeID = INDEX_NONE;
	TArray<FString> Results;
	const auto Check = [&](const TCHAR* Name, bool bGood, const FString& Detail = FString())
	{
		bGood ? ++Passed : ++Failed;
		Results.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s,\"detail\":\"%s\"}"), Name, bGood ? TEXT("true") : TEXT("false"), *EscapeChunkSaveJson(Detail)));
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CHUNK_SAVE_CHECK %s passed=%d %s"), Name, bGood, *Detail);
	};
	const auto Finish = [&]()
	{
		const FString Folder = FPaths::ProjectSavedDir() / TEXT("PresentationValidation"); IFileManager::Get().MakeDirectory(*Folder, true);
		const FString Report = FString::Printf(TEXT("{\"schema_version\":1,\"created_utc\":\"%s\",\"scope\":\"Real standalone GameInstance native world expansion, paid cross-boundary roads/building, canonical transport, checksum disk roundtrip and schema-tagged legacy payload migration. No visual, live traffic crossing, archived historical-binary compatibility, performance or infinite-world claim.\",\"save_schema\":%d,\"unlocked_initial_mask_cells\":%d,\"native_cross_chunk_bridge_id\":%d,\"passed\":%d,\"failed\":%d,\"checks\":[%s]}"),
			*FDateTime::UtcNow().ToIso8601(), UAftershockSaveGame::CurrentVersion, UnlockedCells, NativeBridgeID, Passed, Failed, *FString::Join(Results, TEXT(",")));
		const bool bSaved = FFileHelper::SaveStringToFile(Report, *(Folder / TEXT("chunk_save_audit.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CHUNK_SAVE_RESULT passed=%d failed=%d reportSaved=%d"), Passed, Failed, bSaved);
		return Failed == 0 && bSaved ? 0 : 1;
	};
	Check(TEXT("engine_available"), GEngine != nullptr); if (!GEngine) return Finish();
	const int32 ContextsBefore = GEngine->GetWorldContexts().Num();
	{
		FChunkSaveWorld Fixture; Fixture.Instance.Reset(NewObject<UGameInstance>(GEngine));
		Fixture.Instance->InitializeStandalone(TEXT("AftershockChunkSaveFixture")); Fixture.World = Fixture.Instance->GetWorld();
		UWorld* W = Fixture.World; Check(TEXT("real_game_instance_world"), W != nullptr); if (!W) return Finish();
		auto* Session = Fixture.Instance->GetSubsystem<UAftershockOpeningSessionSubsystem>();
		auto* Saves = Fixture.Instance->GetSubsystem<UAftershockSaveSubsystem>();
		auto* Terrain = W->GetSubsystem<UTerrainGridSubsystem>(); auto* Buildings = W->GetSubsystem<UBuildingManagerSubsystem>();
		auto* Roads = W->GetSubsystem<URoadNetworkSubsystem>(); auto* Economy = W->GetSubsystem<UEconomyAndStateSubsystem>();
		auto* Crowd = W->GetSubsystem<UCrowdRenderSubsystem>(); auto* Life = W->GetSubsystem<UCityLifeSubsystem>();
		auto* Transport = W->GetSubsystem<UTransportSubsystem>(); auto* Scenario = W->GetSubsystem<UAftershockScenarioSubsystem>();
		Check(TEXT("canonical_owners_available"), Session && Saves && Terrain && Buildings && Roads && Economy && Crowd && Life && Transport && Scenario);
		if (!Session || !Saves || !Terrain || !Buildings || !Roads || !Economy || !Crowd || !Life || !Transport || !Scenario) return Finish();
		FText Error; FAftershockSessionSettings Settings; Settings.Mode = EAftershockPlayMode::GeographicSimulation;
		Check(TEXT("configure_geographic_session"), Session->ConfigureNewSession(Settings, Error), Error.ToString());
		Scenario->PrepareScenarioBeforeSettlement(); Buildings->SeedInitialSettlement(); Crowd->SynchronizeCanonicalPopulation(); Roads->NotifySaveRestored();
		TStrongObjectPtr<UAftershockSaveGame> Initial(Saves->CaptureWorld(W, Error));
		Check(TEXT("initial_current_schema_snapshot_valid"), Initial.IsValid() && Initial->SchemaVersion == UAftershockSaveGame::CurrentVersion && Initial->Terrain.TerrainStorageVersion == 2, Error.ToString());
		if (!Initial.IsValid()) return Finish();
		const int32 InitialCount = Terrain->Cells.Num(), InitialWidth = Terrain->GridWidth, InitialHeight = Terrain->GridHeight;
		TMap<UBuildingData*,ECollisionResponse> InitialVisibilityResponses,InitialCameraResponses;
		TArray<FBuildingInstance> InitialBuildingRecords;Buildings->GetAllBuildings(InitialBuildingRecords);
		for(const auto& B:InitialBuildingRecords)if(B.BuildingData){auto* Batch=Buildings->GetOrCreateISM(B.BuildingData);if(Batch){InitialVisibilityResponses.FindOrAdd(B.BuildingData)=Batch->GetCollisionResponseToChannel(ECC_Visibility);InitialCameraResponses.FindOrAdd(B.BuildingData)=Batch->GetCollisionResponseToChannel(ECC_Camera);}}
		const auto HasDenseVisibilityPositiveControl=[&]()
		{
			for(const auto& InitialBuilding:InitialBuildingRecords)
			{
				FBuildingInstance Current;if(!Buildings->GetBuildingByID(InitialBuilding.InstanceID,Current)||!Current.BuildingData)continue;
				auto* Batch=Buildings->GetOrCreateISM(Current.BuildingData);if(!Batch||InitialVisibilityResponses.FindRef(Current.BuildingData)!=ECR_Block)continue;
				const auto* Body=Batch->GetBodyInstance(NAME_None,false,Current.ISMInstanceIndex);FTransform Pose;
				if(!Body||!Body->IsValidBodyInstance()||Body->GetResponseToChannel(ECC_Visibility)!=ECR_Block||!Batch->GetInstanceTransform(Current.ISMInstanceIndex,Pose,true))continue;
				FHitResult Hit;W->LineTraceSingleByChannel(Hit,Pose.GetLocation()+FVector(0,0,100000),Pose.GetLocation()-FVector(0,0,10000),ECC_Visibility);
				if(Hit.GetComponent()==Batch)return true;
			}
			return false;
		};
		Check(TEXT("initial_dense_real_query_body_and_visibility_trace_positive_control"),HasDenseVisibilityPositiveControl());
		const int32 FirstEastChunk=FMath::DivideAndRoundUp(InitialWidth,UTerrainGridSubsystem::ChunkSizeCells);
		const int32 FirstNorthChunk=FMath::DivideAndRoundUp(InitialHeight,UTerrainGridSubsystem::ChunkSizeCells);
		const FIntPoint Chunks[] = { FIntPoint(-1, 0), FIntPoint(-1, -1), FIntPoint(FirstEastChunk, 0), FIntPoint(0, FirstNorthChunk) };
		bool bGenerated = true;
		for (FIntPoint Chunk : Chunks)
		{
			const bool bGood = Terrain->EnsureChunkGenerated(Chunk, Error); bGenerated &= bGood;
			Check(TEXT("append_real_global_chunk"), bGood, FString::Printf(TEXT("chunk=%d,%d %s"), Chunk.X, Chunk.Y, *Error.ToString()));
		}
		bool bOldCellsPreserved = Terrain->Cells.Num() >= InitialCount, bStableIndices = true;
		for (int32 I = 0; I < InitialCount && I < Terrain->Cells.Num(); ++I)
		{
			const auto& Before = Initial->Terrain.Cells[I]; const auto& After = Terrain->Cells[I];
			bOldCellsPreserved &= ChunkCellBytes(Before, true) == ChunkCellBytes(After, true);
			bStableIndices &= Terrain->CoordToIndex(Before.Coords) == I && Terrain->IndexToCoord(I) == Before.Coords;
			if (Before.bIsPlayable != After.bIsPlayable) { ++UnlockedCells; bOldCellsPreserved &= !Before.bIsPlayable && After.bIsPlayable && !Before.bIsOcean; }
		}
		Check(TEXT("initial_dimensions_and_all_old_indices_remain_stable"), bStableIndices && Terrain->GridWidth == InitialWidth && Terrain->GridHeight == InitialHeight);
		Check(TEXT("only_explicit_initial_mask_policy_changed"), bOldCellsPreserved && Terrain->bInitialMaskUnlocked && UnlockedCells > 0, FString::Printf(TEXT("unlocked=%d"), UnlockedCells));
		Check(TEXT("negative_minus_one_minus_one_is_real_cell"), bGenerated && Terrain->IsValidCoord(FIntPoint(-1, -1)) && Terrain->GetCell(FIntPoint(-1, -1)).Coords == FIntPoint(-1, -1));
		Check(TEXT("four_chunks_append_625_cells_each"), Terrain->Cells.Num() == InitialCount + 4 * 625);
		const int32 BeforeRepeat = Terrain->Cells.Num();
		Check(TEXT("repeated_chunk_request_is_idempotent"), Terrain->EnsureChunkGenerated(Chunks[0], Error) && Terrain->Cells.Num() == BeforeRepeat);

		// Buy one actual road connection from the old city into a new western
		// chunk, then buy its house using the normal economy entry point.
		UBuildingData* House = Buildings->GetCatalogData(EBuildingType::House);
		FIntPoint HouseCoord(MAX_int32, MAX_int32), RoadStart(MAX_int32, MAX_int32), RoadEnd(MAX_int32, MAX_int32); FAftershockRoadQuote BestRoad;
		if (House && !Scenario->GetAssemblyCells().IsEmpty())
		{
			RoadStart = Scenario->GetAssemblyCells()[0];
			for (const auto& Cell : Terrain->Cells)
			{
				if (Cell.Coords.X < -3 || Cell.Coords.X > -2 || Cell.Coords.Y < 0 || Cell.Coords.Y >= 25 || !Buildings->CanPlaceBuildingAt(Cell.Coords, House)) continue;
				const FIntPoint Goal = Cell.Coords + FIntPoint(1, 0);
				const auto Quote = Roads->QuoteRoad(RoadStart, Goal);
				if (!Quote.bRouteValid || Quote.bIncludesBridge || Quote.NewRoadCells < 1 || !Economy->CanAfford(Quote.CostTL + 10000)) continue;
				if (!BestRoad.bRouteValid || Quote.CostTL < BestRoad.CostTL) { BestRoad = Quote; RoadEnd = Goal; HouseCoord = Cell.Coords; }
			}
		}
		FAftershockRoadQuote BoughtRoad; const int64 BeforeRoad = Economy->TreasuryTL;
		const bool bRoadBuilt = BestRoad.bRouteValid && Roads->BuildRoad(RoadStart, RoadEnd, BoughtRoad);
		bool bCrossesInitialBoundary = false, bTouchesOld = false, bTouchesNew = false;
		for (FIntPoint C : BoughtRoad.Path) { bTouchesOld |= C.X >= 0 && C.X < InitialWidth && C.Y >= 0 && C.Y < InitialHeight; bTouchesNew |= C.X < 0; }
		bCrossesInitialBoundary = bTouchesOld && bTouchesNew;
		Check(TEXT("paid_native_road_crosses_initial_frame_to_negative_chunk"), bRoadBuilt && bCrossesInitialBoundary && BeforeRoad - Economy->TreasuryTL == BoughtRoad.CostTL, BoughtRoad.Feedback.ToString());
		const bool bHouseBuilt = bRoadBuilt && Economy->BuildBuildingWithBudget(HouseCoord, House, EFoundationQuality::Medium, House->FloorsMin);
		const int32 NewHouseID = bHouseBuilt ? Terrain->GetCell(HouseCoord).OccupyingBuildingID : INDEX_NONE;
		Crowd->SynchronizeCanonicalPopulation();
		Check(TEXT("normal_economy_builds_real_negative_coordinate_house"), bHouseBuilt && NewHouseID > 0 && HouseCoord.X < 0);
		if (!bHouseBuilt) return Finish();

		FIntPoint BridgeStart, BridgeEnd; FAftershockRoadQuote BridgePreview, BridgeActual;
		const bool bQuotedBridge = FindRealChunkBridge(*Terrain, *Roads, *Economy, BridgeStart, BridgeEnd, BridgePreview);
		const int64 BeforeBridge = Economy->TreasuryTL;
		const bool bBridgeBuilt = bQuotedBridge && Roads->BuildRoad(BridgeStart, BridgeEnd, BridgeActual);
		if (bBridgeBuilt) NativeBridgeID = Terrain->GetCell(BridgeActual.Path[0]).BridgeID;
		Check(TEXT("natural_cross_chunk_bridge_native_purchase"), bBridgeBuilt && NativeBridgeID != INDEX_NONE && BeforeBridge - Economy->TreasuryTL == BridgeActual.CostTL, BridgeActual.Feedback.ToString());

		// The city's canonical roster is larger than the 1,500-body draw cap.
		// Explicitly stage one actual drawn resident at the purchased western road;
		// do not replace their canonical home/job or invent a second person.
		const TArray<FCitizenAgent> DrawnResidents=Crowd->GetCitizenDebugRoster();
		const int32 PassengerID=DrawnResidents.IsEmpty()?INDEX_NONE:DrawnResidents[0].ResidentID;
		FAftershockPassengerTrip Trip;FBuildingInstance TripDestination;bool bPlanned=false;
		TArray<FBuildingInstance> ExistingDestinations;Buildings->GetAllBuildings(ExistingDestinations);
		for(const auto& Destination:ExistingDestinations)
		{
			if(Destination.GridCoords.X<0||Destination.DamageState>=EDamageState::Damaged||UTransportSubsystem::GetOffstreetParkingCapacity(Destination)<1)continue;
			if(Transport->PlanTrip(PassengerID,Terrain->GridToWorld(RoadEnd),Terrain->GridToWorld(Destination.GridCoords),Destination.InstanceID,true,Trip))
			{TripDestination=Destination;bPlanned=true;break;}
		}
		Check(TEXT("native_private_car_trip_has_negative_pickup"), bPlanned && Trip.Mode == EAftershockTravelMode::PrivateCar && Trip.PickupRoadCell.X < 0);
		TStrongObjectPtr<UAftershockSaveGame> Reservation(Saves->CaptureWorld(W, Error));
		Check(TEXT("native_private_car_preboarding_reservation_is_saveable"), bPlanned&&Reservation.IsValid(), Error.ToString());
		const bool bBoarded = bPlanned && Transport->MarkPassengerAtPickup(Trip.TripID, Trip.ResidentID, Trip.PickupLocation);
		FAftershockCrowdSaveState CrowdState; Crowd->CaptureSaveState(CrowdState);bool bLinkedRider=false,bNegativeWalkFixture=false;
		if (bBoarded)
		{
			Transport->GetTrip(Trip.TripID, Trip);
			if (auto* Agent = CrowdState.Agents.FindByPredicate([&](const auto& A) { return A.ResidentID == Trip.ResidentID; }))
			{
				Agent->TransportTripID = Trip.TripID; Agent->bInsideVehicle = true; Agent->CurrentLocation = Trip.CurrentLocation;
				Agent->TargetLocation = Trip.CurrentLocation; Agent->TransportFinalDestination = Terrain->GridToWorld(TripDestination.GridCoords);
				Agent->FinalDestinationLocation=Agent->TransportFinalDestination;Agent->Activity=EAftershockCitizenActivity::RidingTransport;Agent->MoveSpeed=0.f;
				Agent->bWaitingForOutdoorSpace=false;Agent->OutdoorDepartureBuildingID=INDEX_NONE;Agent->OutdoorDepartureLocation=FVector::ZeroVector;Agent->RouteGridCoords.Reset();Agent->NextRouteCoordIndex=0;
				bLinkedRider=true;
			}
		}
		if (CrowdState.Agents.Num() > 1)
		{
			auto& Agent = CrowdState.Agents[0].ResidentID == Trip.ResidentID ? CrowdState.Agents[1] : CrowdState.Agents[0];
			for(const auto& Cell:Terrain->Cells)
			{
				const FIntPoint Next=Cell.Coords+FIntPoint(1,0);
				if(Cell.Coords.X>=-1||!Roads->CanTraverseWalkingEdge(Cell.Coords,Next)||!Crowd->CanAdmitCitizenAt(Terrain->GridToWorld(Cell.Coords),Agent.ResidentID))continue;
				Agent.TransportTripID=INDEX_NONE;Agent.bInsideVehicle=false;Agent.CurrentLocation=Terrain->GridToWorld(Cell.Coords);
				Agent.TargetLocation=Terrain->GridToWorld(Next);Agent.FinalDestinationLocation=Agent.TargetLocation;Agent.Activity=EAftershockCitizenActivity::Commuting;Agent.MoveSpeed=120.f;
				Agent.bWaitingForOutdoorSpace=false;Agent.OutdoorDepartureBuildingID=INDEX_NONE;Agent.OutdoorDepartureLocation=FVector::ZeroVector;
				Agent.RouteGridCoords={Cell.Coords,Next};Agent.NextRouteCoordIndex=1;bNegativeWalkFixture=true;break;
			}
		}
		Check(TEXT("typed_crowd_restores_negative_route_and_native_passenger"), bBoarded&&bLinkedRider&&bNegativeWalkFixture&&Crowd->RestoreSaveState(CrowdState, Error), Error.ToString());
		TStrongObjectPtr<UAftershockSaveGame> Expanded(Saves->CaptureWorld(W, Error));
		Check(TEXT("expanded_current_schema_snapshot_with_bridge_trip_and_negative_routes_valid"), Expanded.IsValid()&&Expanded->SchemaVersion==UAftershockSaveGame::CurrentVersion, Error.ToString());
		if (!Expanded.IsValid()) return Finish();
		// Camera-window masking is a derived view. Prove its canonical source and
		// saved transforms never become zero-scale merely because they are offscreen.
		{
			const TArray<uint8> BeforeMask=ChunkBuildingBytes(*Buildings);
			TArray<FBuildingInstance> BeforeRecords;Buildings->GetAllBuildings(BeforeRecords);
			TMap<int32,FTransform> SourceTransforms;TMap<int32,TArray<float>> SourceCustom;
			TMap<UBuildingData*,ECollisionResponse> BeforeMaskCameraResponses;
			for(const auto& B:BeforeRecords)if(auto* Batch=Buildings->GetOrCreateISM(B.BuildingData))
			{BeforeMaskCameraResponses.FindOrAdd(B.BuildingData)=Batch->GetCollisionResponseToChannel(ECC_Camera);FTransform T;Batch->GetInstanceTransform(B.ISMInstanceIndex,T,true);SourceTransforms.Add(B.InstanceID,T);TArray<float> Values;for(int32 C=0;C<4;++C){const int32 At=B.ISMInstanceIndex*Batch->NumCustomDataFloats+C;Values.Add(Batch->PerInstanceSMCustomData.IsValidIndex(At)?Batch->PerInstanceSMCustomData[At]:0.f);}SourceCustom.Add(B.InstanceID,MoveTemp(Values));}
			auto* ViewController=W->SpawnActor<APlayerController>();auto* ViewCamera=W->SpawnActor<AAftershockCameraPawn>();
			if(ViewController)W->AddController(ViewController);
			Check(TEXT("render_window_uses_real_camera_and_canonical_sources"),ViewController&&ViewCamera&&!BeforeMask.IsEmpty());
			Check(TEXT("render_window_controller_is_registered_as_world_first"),ViewController&&W->GetFirstPlayerController()==ViewController);
			if(ViewController&&ViewCamera)
			{
				ViewController->Possess(ViewCamera);ViewCamera->InitializeCameraForGrid(Terrain->GridWidth,Terrain->GridHeight,Terrain->CellSize);
				FAftershockCameraSaveState InitialView;ViewCamera->CaptureSaveState(InitialView);InitialView.Zoom=8000;InitialView.SavedLocalZoom=8000;InitialView.OverviewBlend=0;
				Crowd->EnsureTerrainVisualBuilt();Buildings->RefreshVisibleBuildingPresentation(true);
				const FIntRect FirstWindow=Buildings->GetBuildingPresentationBounds();
				const auto* Other=BeforeRecords.FindByPredicate([&](const auto& B){return B.BuildingData&&B.DamageState<EDamageState::Collapsed&&!FirstWindow.Contains(B.GridCoords);});
				bool Moved=false;if(Other){auto View=InitialView;View.LocalFocus=Terrain->GridToWorld(Other->GridCoords);View.Location=View.LocalFocus;Moved=ViewCamera->RestoreSaveState(View,Error);Crowd->EnsureTerrainVisualBuilt();W->GetSubsystem<UCityPresentationSubsystem>()->RefreshVisiblePresentation();Buildings->RefreshVisibleBuildingPresentation(true);}
				const FIntRect Window=GetAftershockDrawableCellBounds(*Terrain);int32 Expected=0,OutsideID=INDEX_NONE,InsideID=INDEX_NONE;
				for(const auto& B:BeforeRecords)if(B.DamageState<EDamageState::Collapsed){if(Window.Contains(B.GridCoords)){++Expected;if(InsideID==INDEX_NONE)InsideID=B.InstanceID;}else OutsideID=B.InstanceID;}
				Check(TEXT("expanded_static_view_matches_installed_ground_window"),Moved&&Buildings->UsesDerivedBuildingPresentation()&&Buildings->GetBuildingPresentationBounds()==Window&&Window.Width()<=75&&Window.Height()<=75&&Buildings->GetPresentedBuildingCount()==Expected&&OutsideID!=INDEX_NONE&&InsideID!=INDEX_NONE);
				bool ExactSources=true;for(const auto& B:BeforeRecords){FBuildingInstance Current;ExactSources&=Buildings->GetBuildingByID(B.InstanceID,Current)&&FBuildingInstance::StaticStruct()->CompareScriptStruct(&B,&Current,0);auto* Batch=Buildings->GetOrCreateISM(B.BuildingData);FTransform T;ExactSources&=Batch&&Batch->GetInstanceTransform(B.ISMInstanceIndex,T,true)&&T.Equals(SourceTransforms.FindChecked(B.InstanceID),0.f);if(Batch)for(int32 C=0;C<4;++C){const int32 At=B.ISMInstanceIndex*Batch->NumCustomDataFloats+C;ExactSources&=Batch->PerInstanceSMCustomData.IsValidIndex(At)&&Batch->PerInstanceSMCustomData[At]==SourceCustom.FindChecked(B.InstanceID)[C];}}
				Check(TEXT("window_mask_preserves_exact_saved_bytes_ids_indices_transforms_custom_data"),ExactSources&&BeforeMask==ChunkBuildingBytes(*Buildings));
				FTransform InsideTransform,OutsideTransform;FBuildingInstance Hit;float HitDistance=0;
				const bool InsidePresented=Buildings->GetPresentedBuildingTransform(InsideID,InsideTransform);const FTransform* OutsideSource=SourceTransforms.Find(OutsideID);
				Check(TEXT("only_drawn_building_obb_is_pickable"),InsidePresented&&OutsideSource&&Buildings->RaycastBuilding(InsideTransform.GetLocation()+FVector(0,0,100000),FVector::DownVector,200000,Hit,HitDistance)&&Hit.InstanceID==InsideID&&!Buildings->RaycastBuilding(OutsideSource->GetLocation()+FVector(0,0,100000),FVector::DownVector,200000,Hit,HitDistance)&&!Buildings->GetPresentedBuildingTransform(OutsideID,OutsideTransform));
				FBuildingInstance OutsideRecord;auto* OutsideBatch=Buildings->GetBuildingByID(OutsideID,OutsideRecord)?Buildings->GetOrCreateISM(OutsideRecord.BuildingData):nullptr;
				FHitResult VisibilityHit,CameraHit;bool VisibilityOccluded=false,CameraRayHitsSource=false;
				if(OutsideBatch&&OutsideSource){const FVector Start=OutsideSource->GetLocation()+FVector(0,0,100000),End=OutsideSource->GetLocation()-FVector(0,0,10000);W->LineTraceSingleByChannel(VisibilityHit,Start,End,ECC_Visibility);VisibilityOccluded=VisibilityHit.GetComponent()==OutsideBatch;W->LineTraceSingleByChannel(CameraHit,Start,End,ECC_Camera);CameraRayHitsSource=CameraHit.GetComponent()==OutsideBatch;}
				const bool CameraBaselineExists=BeforeMaskCameraResponses.Contains(OutsideRecord.BuildingData),CameraUnchanged=OutsideBatch&&CameraBaselineExists&&OutsideBatch->GetCollisionResponseToChannel(ECC_Camera)==BeforeMaskCameraResponses.FindChecked(OutsideRecord.BuildingData);
				const auto* OutsideBody=OutsideBatch?OutsideBatch->GetBodyInstance(NAME_None,false,OutsideRecord.ISMInstanceIndex):nullptr;
				const bool BodyValid=OutsideBody&&OutsideBody->IsValidBodyInstance(),BodyVisibilityIgnored=BodyValid&&OutsideBody->GetResponseToChannel(ECC_Visibility)==ECR_Ignore,BodyCameraPreserved=BodyValid&&CameraBaselineExists&&OutsideBody->GetResponseToChannel(ECC_Camera)==BeforeMaskCameraResponses.FindChecked(OutsideRecord.BuildingData);
				Check(TEXT("hidden_canonical_visibility_trace_ignores_off_window_building_only"),OutsideBatch&&OutsideSource&&!VisibilityOccluded&&OutsideBatch->GetCollisionResponseToChannel(ECC_Visibility)==ECR_Ignore&&OutsideBatch->GetCollisionEnabled()==ECollisionEnabled::QueryOnly&&CameraUnchanged&&BodyVisibilityIgnored&&BodyCameraPreserved&&CameraRayHitsSource,FString::Printf(TEXT("batch=%d source=%d occluded=%d visibility=%d collisionEnabled=%d cameraBaselineExists=%d cameraUnchanged=%d bodyValid=%d bodyVisibilityIgnored=%d bodyCameraPreserved=%d cameraRayHitsSource=%d hitComponent=%s"),OutsideBatch!=nullptr,OutsideSource!=nullptr,VisibilityOccluded,OutsideBatch?static_cast<int32>(OutsideBatch->GetCollisionResponseToChannel(ECC_Visibility)):-1,OutsideBatch?static_cast<int32>(OutsideBatch->GetCollisionEnabled()):-1,CameraBaselineExists,CameraUnchanged,BodyValid,BodyVisibilityIgnored,BodyCameraPreserved,CameraRayHitsSource,*GetNameSafe(VisibilityHit.GetComponent())));
				TStrongObjectPtr<UAftershockSaveGame> Clipped(Saves->CaptureWorld(W,Error));Check(TEXT("save_while_static_view_clipped_keeps_valid_real_transforms"),Clipped.IsValid()&&Clipped->Buildings.Buildings.Num()==BeforeRecords.Num()&&BeforeMask==ChunkBuildingBytes(*Buildings),Error.ToString());
				FAftershockCameraSaveState SecondView;ViewCamera->CaptureSaveState(SecondView);ViewCamera->RestoreSaveState(InitialView,Error);Crowd->EnsureTerrainVisualBuilt();Buildings->RefreshVisibleBuildingPresentation(true);
				Check(TEXT("window_return_preserves_exact_building_save_bytes"),BeforeMask==ChunkBuildingBytes(*Buildings));
				ViewCamera->RestoreSaveState(SecondView,Error);Crowd->EnsureTerrainVisualBuilt();Buildings->RefreshVisibleBuildingPresentation(true);
				Buildings->ApplyDamageToBuilding(InsideID,90.f);const bool Memorial=Buildings->BeginMemorial(InsideID,Economy->CurrentTurn);Buildings->RefreshVisibleBuildingPresentation(true);FTransform Hidden;
				Check(TEXT("collapsed_memorial_view_does_not_resurrect_intact_mesh"),Memorial&&!Buildings->GetPresentedBuildingTransform(InsideID,Hidden));
				Buildings->ApplyDamageToBuilding(OutsideID,25.f);int32 RepairCost=0;const bool Repaired=Buildings->RepairBuilding(OutsideID,RepairCost);Buildings->RefreshVisibleBuildingPresentation();
				Check(TEXT("outside_window_repair_updates_source_without_showing_proxy"),Repaired&&RepairCost==2000&&!Buildings->GetPresentedBuildingTransform(OutsideID,Hidden));
				FIntPoint NewLot(MAX_int32,MAX_int32);for(const auto& Cell:Terrain->Cells)if(!Window.Contains(Cell.Coords)&&Buildings->CanPlaceBuildingAt(Cell.Coords,House)){NewLot=Cell.Coords;break;}
				const auto NewAccess=Buildings->QuoteAutomaticRoadAccess(NewLot,House);
				const int32 NewBuildingCost=Buildings->CalculateConstructionCost(House,EFoundationQuality::Medium,House->FloorsMin);
				const int64 BeforePlacement=Economy->TreasuryTL,NewCost=static_cast<int64>(NewBuildingCost)+NewAccess.RoadCostTL;
				const bool NewPlaced=Economy->BuildBuildingWithBudget(NewLot,House,EFoundationQuality::Medium,House->FloorsMin);const int32 NewID=NewPlaced?Terrain->GetCell(NewLot).OccupyingBuildingID:INDEX_NONE;FBuildingInstance NewRecord;
				if(NewPlaced&&Buildings->GetBuildingByID(NewID,NewRecord)){auto* Batch=Buildings->GetOrCreateISM(NewRecord.BuildingData);FTransform T;if(Batch&&Batch->GetInstanceTransform(NewRecord.ISMInstanceIndex,T,true)){T.SetRotation(FRotator(0,90,0).Quaternion());Batch->UpdateInstanceTransform(NewRecord.ISMInstanceIndex,T,true,true,true);}}
				Buildings->RefreshVisibleBuildingPresentation();
				Check(TEXT("outside_window_new_building_charges_once_and_stays_hidden"),NewAccess.bRouteValid&&NewPlaced&&BeforePlacement-Economy->TreasuryTL==NewCost&&!Buildings->GetPresentedBuildingTransform(NewID,Hidden),
					FString::Printf(TEXT("buildingCost=%d accessCells=%d roadCost=%d expectedTotal=%lld actualCharge=%lld"),NewBuildingCost,NewAccess.NewRoadCells,NewAccess.RoadCostTL,NewCost,BeforePlacement-Economy->TreasuryTL));
				if(NewPlaced){auto NewView=SecondView;NewView.LocalFocus=Terrain->GridToWorld(NewLot);NewView.Location=NewView.LocalFocus;ViewCamera->RestoreSaveState(NewView,Error);Crowd->EnsureTerrainVisualBuilt();Buildings->RefreshVisibleBuildingPresentation(true);}
				Check(TEXT("newly_visible_placement_preserves_logical_id_and_yaw"),NewPlaced&&Buildings->GetPresentedBuildingTransform(NewID,Hidden)&&FMath::IsNearlyEqual(FRotator::NormalizeAxis(Hidden.Rotator().Yaw),90.f,.01f));
				ViewCamera->RestoreSaveState(InitialView,Error);Crowd->EnsureTerrainVisualBuilt();Buildings->RefreshVisibleBuildingPresentation(true);
				Check(TEXT("return_window_keeps_memorial_state_and_hidden_intact"),!Buildings->GetPresentedBuildingTransform(InsideID,Hidden)&&Buildings->GetMemorialTurnsRemaining(InsideID)==3);
				Check(TEXT("mask_fixture_restores_original_complete_owners"),Saves->RestoreWorld(W,Expanded.Get(),Error),Error.ToString());
			}
			if(ViewController)ViewController->Destroy();if(ViewCamera)ViewCamera->Destroy();
			Buildings->RefreshVisibleBuildingPresentation(true);
		}
		RemoveChunkAuditSlot();
		Check(TEXT("expanded_checksum_disk_write"), Saves->WriteSnapshot(Expanded.Get(), TEXT("Audit"), Error), Error.ToString());
		TStrongObjectPtr<UAftershockSaveGame> Disk(Saves->ReadSnapshot(TEXT("Audit"), Error));
		Check(TEXT("expanded_checksum_disk_read_preserves_chunk_count_and_schema"), Disk.IsValid() && Disk->SchemaVersion == UAftershockSaveGame::CurrentVersion && Disk->Terrain.Cells.Num() == Expanded->Terrain.Cells.Num() && Disk->Terrain.Chunks.Num() == Expanded->Terrain.Chunks.Num(), Error.ToString());
		Check(TEXT("restore_smaller_initial_world_before_cold_expanded_preflight"), Saves->RestoreWorld(W, Initial.Get(), Error) && !Terrain->IsValidCoord(HouseCoord), Error.ToString());
		Buildings->RefreshVisibleBuildingPresentation(true);bool DenseChannelsRestored=!Buildings->UsesDerivedBuildingPresentation();
		for(const auto& Pair:InitialVisibilityResponses){auto* Batch=Buildings->GetOrCreateISM(Pair.Key);DenseChannelsRestored&=Batch&&Batch->GetCollisionResponseToChannel(ECC_Visibility)==Pair.Value&&Batch->GetCollisionResponseToChannel(ECC_Camera)==InitialCameraResponses.FindChecked(Pair.Key)&&Batch->GetCollisionEnabled()==ECollisionEnabled::QueryOnly;}
		Check(TEXT("dense_presentation_restores_each_source_original_visibility_channel"),DenseChannelsRestored);
		Check(TEXT("restored_dense_real_query_body_and_visibility_trace_positive_control"),HasDenseVisibilityPositiveControl());
		Check(TEXT("expanded_preflight_reads_saved_cells_not_smaller_current_world"), Disk.IsValid() && Saves->ValidateSnapshot(*Disk, Error), Error.ToString());
		Check(TEXT("expanded_disk_snapshot_restores_all_owners"), Disk.IsValid() && Saves->RestoreWorld(W, Disk.Get(), Error), Error.ToString());
		bool bCellsEqual = Terrain->Cells.Num() == Expanded->Terrain.Cells.Num();
		for (int32 I = 0; bCellsEqual && I < Terrain->Cells.Num(); ++I) bCellsEqual &= ChunkCellBytes(Terrain->Cells[I]) == ChunkCellBytes(Expanded->Terrain.Cells[I]) && Terrain->CoordToIndex(Terrain->Cells[I].Coords) == I;
		FBuildingInstance LoadedHouse; FAftershockPassengerTrip LoadedTrip;
		Check(TEXT("disk_restore_preserves_all_cell_fields_and_global_indices"), bCellsEqual);
		Check(TEXT("disk_restore_preserves_negative_building_and_riding_trip"), Buildings->GetBuildingByID(NewHouseID, LoadedHouse) && LoadedHouse.GridCoords == HouseCoord && Transport->GetTrip(Trip.TripID, LoadedTrip) && LoadedTrip.State == EAftershockPassengerState::Riding && LoadedTrip.PickupRoadCell.X < 0);
		FAftershockCrowdSaveState RestoredCrowd;Crowd->CaptureSaveState(RestoredCrowd);
		Check(TEXT("disk_restore_preserves_actual_drawn_rider_and_negative_walk_route"),RestoredCrowd.Agents.ContainsByPredicate([&](const auto& A){return A.ResidentID==PassengerID&&A.bInsideVehicle&&A.TransportTripID==Trip.TripID;})&&RestoredCrowd.Agents.ContainsByPredicate([](const auto& A){return !A.bInsideVehicle&&A.CurrentLocation.X<0&&A.RouteGridCoords.ContainsByPredicate([](FIntPoint C){return C.X<0;});}));
		Check(TEXT("disk_restore_preserves_native_bridge_membership_and_deck"), bBridgeBuilt && Roads->GetBridgeCount() == Expanded->Terrain.Cells.FilterByPredicate([](const auto& C) { return C.BridgeID != INDEX_NONE && C.bBridgeEndpoint; }).Num() / 2 && Terrain->GetCell(BridgeActual.Path[0]).BridgeID == NativeBridgeID);

		TStrongObjectPtr<UAftershockSaveGame> Invalid(DuplicateObject<UAftershockSaveGame>(Expanded.Get(), Fixture.Instance.Get()));
		const auto RejectBeforeMutation = [&](const TCHAR* Name)
		{
			const TArray<uint8> Before = ChunkWorldBytes(*Saves, *W);
			const bool bRejected = !Saves->RestoreWorld(W, Invalid.Get(), Error);
			Check(Name, !Before.IsEmpty() && bRejected && Before == ChunkWorldBytes(*Saves, *W), Error.ToString());
			Invalid->Terrain = Expanded->Terrain; Invalid->Crowd = Expanded->Crowd; Invalid->Transport = Expanded->Transport; Invalid->Camera = Expanded->Camera; Invalid->SchemaVersion = UAftershockSaveGame::CurrentVersion;
		};
		Invalid->Terrain.Cells.Last().Coords = Invalid->Terrain.Cells[0].Coords; RejectBeforeMutation(TEXT("duplicate_global_cell_rejected_before_any_owner_mutates"));
		Invalid->Terrain.Cells.Last().Coords = FIntPoint(1000001, 0); RejectBeforeMutation(TEXT("unbounded_global_coord_rejected_before_mutation"));
		Invalid->Camera.LocalFocus.X = std::numeric_limits<double>::max(); RejectBeforeMutation(TEXT("finite_unrepresentable_camera_focus_rejected_before_chunk_math"));
		Invalid->Camera.LocalFocus = Terrain->GridToWorld(FIntPoint(900000, 900000)); RejectBeforeMutation(TEXT("camera_focus_requires_resident_global_cell"));
		Invalid->Terrain.Chunks[0].CellCount += 1; RejectBeforeMutation(TEXT("wrong_chunk_manifest_count_rejected_before_mutation"));
		Invalid->Terrain.Chunks.RemoveAt(0); RejectBeforeMutation(TEXT("missing_chunk_manifest_record_rejected_before_mutation"));
		const FAftershockTerrainChunkState DuplicateChunk = Invalid->Terrain.Chunks[0];
		Invalid->Terrain.Chunks.Add(DuplicateChunk); RejectBeforeMutation(TEXT("duplicate_chunk_manifest_record_rejected_before_mutation"));
		Invalid->Terrain.ChunkSize = 26; RejectBeforeMutation(TEXT("unknown_chunk_size_rejected_before_mutation"));
		Invalid->Terrain.ContinuationGeneratorVersion = 99; RejectBeforeMutation(TEXT("unknown_generator_version_rejected_before_mutation"));
		Invalid->Terrain.TerrainStorageVersion = 99; RejectBeforeMutation(TEXT("unknown_storage_version_rejected_before_mutation"));
		Invalid->Terrain.bInitialMaskUnlocked = false; RejectBeforeMutation(TEXT("expanded_snapshot_requires_serialized_initial_mask_unlock"));
		Invalid->Terrain.bUsesRegionalLayout = false; RejectBeforeMutation(TEXT("geographic_continuation_policy_must_match_saved_mode"));
		Invalid->Terrain.Cells[InitialCount].bHasRealElevationSample = true; RejectBeforeMutation(TEXT("authored_continuation_cannot_claim_real_dem_sample"));
		Invalid->Crowd.Agents[0].RouteGridCoords.Add(FIntPoint(900000, 900000)); RejectBeforeMutation(TEXT("missing_global_pedestrian_route_cell_rejected_before_mutation"));
		Invalid->Crowd.Agents[0].CurrentLocation.X = std::numeric_limits<double>::max(); RejectBeforeMutation(TEXT("finite_unrepresentable_world_coord_rejected_before_integer_conversion"));
		if (!Invalid->Transport.Vehicles.IsEmpty()) { Invalid->Transport.Vehicles[0].RoadPath.Add(FIntPoint(900000, 900000)); RejectBeforeMutation(TEXT("missing_global_vehicle_route_cell_rejected_before_mutation")); }
		if (!Invalid->Transport.Trips.IsEmpty()) { Invalid->Transport.Trips.Last().PickupRoadCell = FIntPoint(900000, 900000); RejectBeforeMutation(TEXT("active_private_trip_requires_resident_global_pickup_cell")); }
		if (bBridgeBuilt)
		{
			const int32 BridgeIndex = Invalid->Terrain.Cells.IndexOfByPredicate([&](const auto& Cell) { return Cell.Coords == BridgeActual.Path[0]; });
			if (Invalid->Terrain.Cells.IsValidIndex(BridgeIndex)) Invalid->Terrain.Cells[BridgeIndex].RoadSurfaceElevationCm -= 1000.f;
			RejectBeforeMutation(TEXT("corrupt_cross_chunk_bridge_deck_rejected_before_mutation"));
		}
		ResetChunkMetadataForLegacy(*Invalid, 2); RejectBeforeMutation(TEXT("expanded_cells_cannot_be_downgraded_to_legacy_dense_schema"));

		for (int32 Schema : { 1, 2 })
		{
			TStrongObjectPtr<UAftershockSaveGame> Legacy(DuplicateObject<UAftershockSaveGame>(Initial.Get(), Fixture.Instance.Get()));
			ResetChunkMetadataForLegacy(*Legacy, Schema);
			if (Schema == 1) Legacy->Settings.FocusID = TEXT("legacy-default-focus-migration");
			const auto OldCells = Legacy->Terrain.Cells;
			// Validate a COPY so the payload written below still carries the old
			// schema and exercises the actual checksum/disk reader migration path.
			TStrongObjectPtr<UAftershockSaveGame> LegacyPreflight(DuplicateObject<UAftershockSaveGame>(Legacy.Get(), Fixture.Instance.Get()));
			const bool LegacyPreflightValid=LegacyPreflight.IsValid()&&Saves->ValidateSnapshot(*LegacyPreflight,Error);
			Check(TEXT("legacy_old_wire_fixture_passes_production_migration_preflight"),LegacyPreflightValid,
				FString::Printf(TEXT("schema=%d %s"),Schema,*Error.ToString()));
			RemoveChunkAuditSlot();
			FLegacyChunkWireReceipt WireReceipt;
			const bool bLegacyWritten = WriteLegacyChunkAuditPayload(*Saves, *Legacy, WireReceipt);
			const FString WireDetail = WireReceipt.Describe(Schema);
			Check(TEXT("legacy_schema_tagged_checksum_payload_written_to_disk"), bLegacyWritten, WireDetail);
			Check(TEXT("legacy_raw_envelope_limit_is_enforced_without_relaxing_reader"), WireReceipt.PayloadBytes > 0
				&& WireReceipt.bRawDiskBytesExact
				&& (WireReceipt.bRawWithinDiskLimit ? WireReceipt.bRawDiskAccepted : !WireReceipt.bRawDiskAccepted && WireReceipt.EnvelopeVersion == 2), WireDetail);
			Check(TEXT("legacy_tagged_payload_roundtrips_before_schema_migration"), bLegacyWritten && WireReceipt.bTaggedPayloadDecoded, WireDetail);
			TStrongObjectPtr<UAftershockSaveGame> Migrated(Saves->ReadSnapshot(TEXT("Audit"), Error));
			bool bOldFieldsEqual = Migrated.IsValid() && Migrated->Terrain.Cells.Num() == OldCells.Num();
			for (int32 I = 0; bOldFieldsEqual && I < OldCells.Num(); ++I) bOldFieldsEqual &= ChunkCellBytes(OldCells[I], false, true) == ChunkCellBytes(Migrated->Terrain.Cells[I], false, true);
			Check(TEXT("legacy_disk_schema_migrates_to_current_without_changing_old_cell_fields"), bOldFieldsEqual && Migrated->SchemaVersion == UAftershockSaveGame::CurrentVersion && Migrated->Terrain.TerrainStorageVersion == 2 && !Migrated->Terrain.Chunks.IsEmpty(), Error.ToString());
			Check(TEXT("legacy_disk_migration_preserves_economy_and_roster"), Migrated.IsValid() && Migrated->Economy.Treasury == Initial->Economy.Treasury && Migrated->CityLife.Residents.Num() == Initial->CityLife.Residents.Num() && Migrated->Transport.NextTripID == Initial->Transport.NextTripID && Migrated->Terrain.bUsesRegionalLayout);
			Check(TEXT("legacy_disk_migration_has_safe_default_focus"), Migrated.IsValid() && Migrated->Settings.FocusID.IsNone() && Migrated->Terrain.GeographyFocusIndex == 0);
			Legacy->Terrain.Cells[0].Coords = FIntPoint(-1, -1);
			Check(TEXT("invalid_legacy_shape_fails_without_partial_schema_migration"), !Saves->ValidateSnapshot(*Legacy, Error) && Legacy->SchemaVersion == Schema && Legacy->Terrain.TerrainStorageVersion == 1 && Legacy->Terrain.Chunks.IsEmpty());
		}
		RemoveChunkAuditSlot();
	}
	Check(TEXT("standalone_context_cleaned_up"), GEngine->GetWorldContexts().Num() == ContextsBefore);
	return Finish();
}
#else
UAftershockChunkSaveAuditCommandlet::UAftershockChunkSaveAuditCommandlet() {}
int32 UAftershockChunkSaveAuditCommandlet::Main(const FString&) { return 1; }
#endif
