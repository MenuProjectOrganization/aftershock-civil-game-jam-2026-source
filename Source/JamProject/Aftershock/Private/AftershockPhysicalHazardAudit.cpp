#include "AftershockPhysicalHazardAudit.h"
#include "AftershockPhysicalReplay.h"
#include "AftershockMassMovementVisual.h"
#if WITH_EDITOR
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/AftershockRegionRules.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/AftershockDisasterDirectorSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Aftershock/Public/AftershockSaveGame.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Async/TaskGraphInterfaces.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Misc/CommandLine.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

// Root explicitly authorizes only these fixture-world controls. No simulation
// state is pre-filled or damage/report forged. Mass replay is NEVER shortened.
struct FAftershockPhysicalHazardAuditDriver
{
	static void ShortenWaterSource(UDisasterSimulationSubsystem& Sim)
	{
		auto& E=Sim.PhysicalState.Episode;
		check(E.bActive&&(E.Type==EDisasterType::Flood||E.Type==EDisasterType::Tsunami));
		E.SourceDurationSeconds=10.;E.SimulationTimeScale=20.;
	}
};

namespace AftershockPhysicalAuditPrivate
{
	struct FWorldFixture
	{
		TStrongObjectPtr<UGameInstance> Instance;
		UWorld* World=nullptr;
		~FWorldFixture()
		{
			if(Instance.IsValid())Instance->Shutdown();
			if(World){World->DestroyWorld(false);if(GEngine)GEngine->DestroyWorldContext(World);World->RemoveFromRoot();}
		}
	};
	struct FCaseSpec { const TCHAR* Key;const TCHAR* Region;const TCHAR* Focus;EDisasterType Type;bool bNeedsSettlement=true; };
	const FCaseSpec Cases[]={
		{TEXT("flood"),TEXT("dereyolu"),TEXT("dereyolu"),EDisasterType::Flood},
		{TEXT("tsunami"),TEXT("sendai"),TEXT("sendai_coast"),EDisasterType::Tsunami},
		{TEXT("avalanche"),TEXT("etna"),TEXT("etna"),EDisasterType::Avalanche,false},
		{TEXT("landslide"),TEXT("dereyolu"),TEXT("dereyolu"),EDisasterType::Landslide},
		// Explicit optional production-length regression; never joins the default four.
		{TEXT("sendai_flood"),TEXT("sendai"),TEXT("sendai_coast"),EDisasterType::Flood}
	};
	bool EnsureActualSourceOcean(UTerrainGridSubsystem& Terrain,FJsonObject& Row,FText& Error)
	{
		for(const auto& C:Terrain.Cells)if(C.bIsOcean){Row.SetBoolField(TEXT("fixture_ocean_corridor_expanded"),false);return true;}
		const FIntRect Source=Terrain.GetSupportedGeographicCellBounds(),Initial=Terrain.GetResidentCellBounds();
		const int32 K=UTerrainGridSubsystem::ChunkSizeCells;
		const FIntPoint MinChunk(FMath::CeilToInt(double(Source.Min.X)/K),FMath::CeilToInt(double(Source.Min.Y)/K));
		const FIntPoint EndChunk(FMath::FloorToInt(double(Source.Max.X)/K),FMath::FloorToInt(double(Source.Max.Y)/K));
		const int64 Candidates=int64(EndChunk.X-MinChunk.X)*(EndChunk.Y-MinChunk.Y);
		if(Source.IsEmpty()||EndChunk.X<=MinChunk.X||EndChunk.Y<=MinChunk.Y||Candidates<=0||Candidates>16384)
		{Error=FText::FromString(TEXT("Actual source chunk-centre search exceeds its explicit16384-query bound."));return false;}
		FIntPoint TargetCell=FIntPoint::ZeroValue;bool Found=false;double Best=MAX_dbl;int32 Queries=0;
		for(int32 Y=MinChunk.Y;Y<EndChunk.Y;++Y)for(int32 X=MinChunk.X;X<EndChunk.X;++X)
		{
			const FIntPoint Cell(X*K+K/2,Y*K+K/2);
			const FVector2D Metres((Cell.X+.5-Terrain.GridWidth*.5)*Terrain.CellSize*.01,(Cell.Y+.5-Terrain.GridHeight*.5)*Terrain.CellSize*.01);
			double Latitude=0.,Longitude=0.;FAftershockGeographicTerrainSample Sample;++Queries;
			if(!AftershockFocusLocalMetresToLatLon(Terrain.GeographyIndex,Terrain.GeographyFocusIndex,Metres,Latitude,Longitude)
				||!TrySampleAftershockFocusDetailGeodata(Terrain.GeographyIndex,Terrain.GeographyFocusIndex,Latitude,Longitude,Sample)||!Sample.bOcean)continue;
			FIntPoint Mapped;if(!Terrain.TryGeographicToCell(Latitude,Longitude,Mapped)||!Source.Contains(Mapped))continue;
			const double DX=FMath::Max(0.,FMath::Max(double(Initial.Min.X-Mapped.X),double(Mapped.X-(Initial.Max.X-1))));
			const double DY=FMath::Max(0.,FMath::Max(double(Initial.Min.Y-Mapped.Y),double(Mapped.Y-(Initial.Max.Y-1))));
			const double Distance=DX*DX+DY*DY;
			if(!Found||Distance<Best||(Distance==Best&&(Mapped.Y<TargetCell.Y||(Mapped.Y==TargetCell.Y&&Mapped.X<TargetCell.X))))
			{Found=true;Best=Distance;TargetCell=Mapped;}
		}
		Row.SetNumberField(TEXT("fixture_ocean_source_queries"),Queries);
		if(!Found){Error=FText::FromString(TEXT("No genuine ocean chunk centre exists in the selected cached detail source."));return false;}
		const FIntPoint Target=UTerrainGridSubsystem::CellToChunk(TargetCell);
		FIntPoint Current=FIntPoint::ZeroValue;int64 Shortest=MAX_int64;bool HasStart=false;
		TSet<FIntPoint> Resident;
		for(const auto& C:Terrain.GetResidentChunks())
		{
			Resident.Add(C.ChunkCoord);const int64 D=FMath::Abs(int64(C.ChunkCoord.X)-Target.X)+FMath::Abs(int64(C.ChunkCoord.Y)-Target.Y);
			if(!HasStart||D<Shortest||(D==Shortest&&(C.ChunkCoord.Y<Current.Y||(C.ChunkCoord.Y==Current.Y&&C.ChunkCoord.X<Current.X))))
			{HasStart=true;Shortest=D;Current=C.ChunkCoord;}
		}
		if(!HasStart||Shortest>128){Error=FText::FromString(TEXT("Actual ocean corridor has no bounded resident start."));return false;}
		TArray<FIntPoint> Corridor;FIntPoint At=Current;
		while(At!=Target)
		{
			if(At.X!=Target.X)At.X+=At.X<Target.X?1:-1;else At.Y+=At.Y<Target.Y?1:-1;
			if(!Resident.Contains(At))Corridor.Add(At);
		}
		if(int64(Terrain.Cells.Num())+int64(Corridor.Num())*K*K>UTerrainGridSubsystem::MaximumResidentCells)
		{Error=FText::FromString(TEXT("Actual ocean corridor exceeds the65536 canonical resident-cell cap."));return false;}
		for(FIntPoint Chunk:Corridor)if(!Terrain.EnsureChunkGenerated(Chunk,Error))return false;
		FGridCell Actual;const bool Ocean=Terrain.TryGetCell(TargetCell,Actual)&&Actual.bIsOcean&&Actual.bHasRealElevationSample;
		Row.SetBoolField(TEXT("fixture_ocean_corridor_expanded"),true);Row.SetNumberField(TEXT("fixture_ocean_added_chunks"),Corridor.Num());
		Row.SetNumberField(TEXT("fixture_ocean_target_x"),TargetCell.X);Row.SetNumberField(TEXT("fixture_ocean_target_y"),TargetCell.Y);
		Row.SetBoolField(TEXT("fixture_ocean_target_has_real_source_sample"),Ocean);
		if(!Ocean)Error=FText::FromString(TEXT("Generated target did not retain its genuine ocean/detail-source sample."));
		return Ocean;
	}
	bool SameAcceptedPacket(const FAftershockPhysicalHazardSaveState& A,const FAftershockPhysicalHazardSaveState& B)
	{
		if(A.PendingPhysicalSeconds!=B.PendingPhysicalSeconds||A.PendingAcceptedSeconds!=B.PendingAcceptedSeconds
			||A.PendingForcingCells.Num()!=B.PendingForcingCells.Num()||A.PendingForcingBoundaries.Num()!=B.PendingForcingBoundaries.Num())return false;
		TMap<FIntPoint,const FAftershockHydraulicForcingCellSaveRecord*> Cells;
		for(const auto& C:A.PendingForcingCells){if(Cells.Contains(C.Coord))return false;Cells.Add(C.Coord,&C);}
		for(const auto& C:B.PendingForcingCells){const auto* Prior=Cells.Find(C.Coord);if(!Prior||!FAftershockHydraulicForcingCellSaveRecord::StaticStruct()->CompareScriptStruct(*Prior,&C,0))return false;Cells.Remove(C.Coord);}
		TMap<int64,const FAftershockHydraulicForcingBoundarySaveRecord*> Faces;
		for(const auto& F:A.PendingForcingBoundaries){if(Faces.Contains(F.FaceStableID))return false;Faces.Add(F.FaceStableID,&F);}
		for(const auto& F:B.PendingForcingBoundaries){const auto* Prior=Faces.Find(F.FaceStableID);if(!Prior||!FAftershockHydraulicForcingBoundarySaveRecord::StaticStruct()->CompareScriptStruct(*Prior,&F,0))return false;Faces.Remove(F.FaceStableID);}
		return Cells.IsEmpty()&&Faces.IsEmpty();
	}

	bool RecordActualMassEvidence(const FAftershockPhysicalHazardSaveState& State,FJsonObject& Row,
		bool& Moving,bool& HasContacts,bool& HasPressure,FString& Detail)
	{
		Moving=HasContacts=HasPressure=false;FAftershockMassVisualEvent Replay;FText Error;
		if(!DecodeAftershockMassReplay(State.Episode.MassReplay,Replay,Error)||Replay.Result.Frames.IsEmpty())
		{Detail=Error.ToString();return false;}
		const auto& Result=Replay.Result;const auto& Budget=Result.Budget;const auto& Last=Result.Frames.Last();
		FVector2D SourceSum=FVector2D::ZeroVector,FinalSum=FVector2D::ZeroVector;double SourceKg=0.,FinalKg=0.;
		for(const auto& C:Result.Cells)if(C.ReleasedMassKg>0.)
		{SourceSum+=(Replay.GridOriginM+FVector2D(C.Coord.X+.5,C.Coord.Y+.5)*Replay.CellSizeM)*C.ReleasedMassKg;SourceKg+=C.ReleasedMassKg;}
		for(const auto& C:Last.Cells)
		{const double Kg=C.MovingMassKg+C.DepositedMassKg;if(Kg>0.){FinalSum+=(Replay.GridOriginM+FVector2D(C.Coord.X+.5,C.Coord.Y+.5)*Replay.CellSizeM)*Kg;FinalKg+=Kg;}}
		const bool HasCentroids=SourceKg>0.&&FinalKg>0.;const FVector2D Source=SourceKg>0.?SourceSum/SourceKg:FVector2D::ZeroVector;
		const FVector2D Final=FinalKg>0.?FinalSum/FinalKg:FVector2D::ZeroVector;const double DistributionShift=HasCentroids?FVector2D::Distance(Source,Final):0.;
		TMap<int32,FVector> FirstSamples;double MaxSampledParcelRunoutM=0.;
		for(const auto& Frame:Result.Frames)for(const auto& P:Frame.Parcels)
		{if(const FVector* First=FirstSamples.Find(P.ParcelID))MaxSampledParcelRunoutM=FMath::Max(MaxSampledParcelRunoutM,FVector::Dist2D(*First,P.PositionM));else FirstSamples.Add(P.ParcelID,P.PositionM);}
		int32 ActualContacts=0;double PeakPressurePa=0.,MaxImpulseNs=0.;
		for(const auto& Impact:Last.Impacts)
		{ActualContacts+=Impact.Contacts;PeakPressurePa=FMath::Max(PeakPressurePa,Impact.PeakNormalPressurePa);MaxImpulseNs=FMath::Max(MaxImpulseNs,Impact.ImpulseMagnitudeNs);}
		Moving=FMath::IsFinite(Budget.MaximumSpeedMps)&&Budget.MaximumSpeedMps>1.e-6
			&&(Budget.MaximumDisplacementCells>1.e-9||DistributionShift>1.e-6||MaxSampledParcelRunoutM>1.e-6);
		HasContacts=ActualContacts>0;HasPressure=PeakPressurePa>0.||MaxImpulseNs>0.;
		Row.SetNumberField(TEXT("mass_replay_frames"),Result.Frames.Num());Row.SetNumberField(TEXT("mass_simulated_seconds"),Result.SimulatedSeconds);
		Row.SetNumberField(TEXT("mass_max_speed_mps"),Budget.MaximumSpeedMps);Row.SetNumberField(TEXT("mass_max_substep_displacement_cells"),Budget.MaximumDisplacementCells);
		Row.SetNumberField(TEXT("mass_released_kg"),Budget.ReleasedKg);Row.SetNumberField(TEXT("mass_entrained_kg"),Budget.EntrainedKg);
		Row.SetNumberField(TEXT("mass_deposited_kg"),Budget.DepositedKg);Row.SetNumberField(TEXT("mass_exported_kg"),Budget.ExportedKg);
		Row.SetBoolField(TEXT("mass_centroids_available"),HasCentroids);Row.SetNumberField(TEXT("mass_source_centroid_x_m"),Source.X);Row.SetNumberField(TEXT("mass_source_centroid_y_m"),Source.Y);
		Row.SetNumberField(TEXT("mass_final_centroid_x_m"),Final.X);Row.SetNumberField(TEXT("mass_final_centroid_y_m"),Final.Y);
		Row.SetNumberField(TEXT("mass_source_to_final_distribution_centroid_shift_m"),DistributionShift);Row.SetNumberField(TEXT("mass_max_sampled_same_id_parcel_runout_m"),MaxSampledParcelRunoutM);
		Row.SetStringField(TEXT("mass_runout_scope"),TEXT("Grid-distribution centroid includes entrainment and excludes exported mass; parcel runout covers retained replay samples only. MaximumDisplacementCells is a per-substep bound, not total runout."));
		Row.SetNumberField(TEXT("mass_actual_building_contacts"),ActualContacts);Row.SetBoolField(TEXT("mass_building_pressure_applicable"),HasContacts);
		Row.SetNumberField(TEXT("mass_peak_actual_building_pressure_pa"),PeakPressurePa);Row.SetNumberField(TEXT("mass_max_actual_building_impulse_ns"),MaxImpulseNs);
		Row.SetBoolField(TEXT("mass_nonzero_motion"),Moving);Row.SetBoolField(TEXT("mass_visual_acceptance_not_proven_by_numerical_motion"),true);
		Detail=FString::Printf(TEXT("seconds=%.6f speedMps=%.9f maxSubstepCells=%.9f distributionShiftM=%.6f sampledParcelRunoutM=%.6f releasedKg=%.3f entrainedKg=%.3f contacts=%d"),
			Result.SimulatedSeconds,Budget.MaximumSpeedMps,Budget.MaximumDisplacementCells,DistributionShift,MaxSampledParcelRunoutM,Budget.ReleasedKg,Budget.EntrainedKg,ActualContacts);
		return FMath::IsFinite(Result.SimulatedSeconds)&&FMath::IsFinite(DistributionShift)&&FMath::IsFinite(MaxSampledParcelRunoutM)
			&&FMath::IsFinite(Budget.MaximumSpeedMps)&&FMath::IsFinite(Budget.MaximumDisplacementCells)&&Budget.ReleasedKg>0.;
	}

	bool Near(double A,double B,double T=1e-8){return FMath::Abs(A-B)<=T*FMath::Max(1.,FMath::Max(FMath::Abs(A),FMath::Abs(B)));}
	bool SameWaterFrontier(const FAftershockPhysicalHazardSaveState& A,const FAftershockPhysicalHazardSaveState& B)
	{
		if(A.WaterCells.Num()!=B.WaterCells.Num()||A.PublishedStep!=B.PublishedStep||A.PhysicalSeconds!=B.PhysicalSeconds
			||!SameAcceptedPacket(A,B)||A.Episode.StartPhysicalSeconds!=B.Episode.StartPhysicalSeconds
			||A.Episode.EventID!=B.Episode.EventID||A.Episode.SourceElapsedSeconds!=B.Episode.SourceElapsedSeconds||A.Episode.PlaybackSeconds!=B.Episode.PlaybackSeconds)return false;
		for(int32 I=0;I<A.WaterCells.Num();++I)
		{
			const auto& X=A.WaterCells[I];const auto& Y=B.WaterCells[I];
			if(X.Coord!=Y.Coord||X.BedElevationCm!=Y.BedElevationCm||X.VolumeM3!=Y.VolumeM3||X.DischargeXM2PerSecond!=Y.DischargeXM2PerSecond||X.DischargeYM2PerSecond!=Y.DischargeYM2PerSecond||X.CumulativeInfiltrationM3!=Y.CumulativeInfiltrationM3)return false;
		}
		return FAftershockHydraulicBudgetSaveRecord::StaticStruct()->CompareScriptStruct(&A.WaterBudget,&B.WaterBudget,0);
	}
	void TickWorld(UWorld& World,float Delta=.08f)
	{
		// TimerManager's one-tick-per-engine-frame guard is genuine. A native
		// commandlet has no UEngine::Tick loop, so advance its explicit frame here.
		++GFrameCounter;
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		World.Tick(LEVELTICK_All,Delta);
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FPlatformProcess::SleepNoStats(.001f); // yield to the real bounded worker, not simulated task results.
	}
	template<class Predicate> bool PumpUntil(UWorld& World,Predicate&& Ready,double MaximumWallSeconds)
	{
		const double End=FPlatformTime::Seconds()+MaximumWallSeconds;
		while(FPlatformTime::Seconds()<End)
		{
			if(Ready())return true;
			TickWorld(World);
		}
		return Ready();
	}
}

bool RunAftershockPhysicalHazardFixtures(const FString& AbsoluteReportPath)
{
	using namespace AftershockPhysicalAuditPrivate;
	const bool FullWaterSource=FParse::Param(FCommandLine::Get(),TEXT("AftershockFullWaterSource"));
	auto Report=MakeShared<FJsonObject>();Report->SetNumberField(TEXT("schema_version"),1);
	Report->SetBoolField(TEXT("valid"),false);Report->SetBoolField(TEXT("in_progress"),true);
	Report->SetStringField(TEXT("scope"),FullWaterSource
		?TEXT("Optional full production water-source lifecycle on real V3 geography, real bounded async solver and explicit virtual UWorld ticks. Production source duration, rate and120-second drainage tail are unchanged; no physical-clock override. Includes active in-memory Save API restore and fiscal idempotency. No real-time throughput, GPU/pixel, cooked collision, disk or packaged acceptance.")
		:TEXT("Isolated real V3 geography startup (Etna summit deliberately unpopulated; Sendai may extend an explicit bounded real-source chunk corridor to ocean), real timers and async canonical hazard lifecycle, active in-memory Save API restore and fiscal replay idempotency. Water source shortened to10 physical seconds ONLY in this disposable fixture; production120-second drainage tail retained. No full authored-storm, GPU/pixel, cooked-collision, disk or packaged acceptance."));
	Report->SetBoolField(TEXT("player_slots_written"),false);Report->SetBoolField(TEXT("disk_roundtrip_executed"),false);
	Report->SetBoolField(TEXT("render_and_collision_acceptance_pending"),true);
	Report->SetBoolField(TEXT("fixture_water_source_duration_override"),!FullWaterSource);
	Report->SetBoolField(TEXT("full_authored_water_source"),FullWaterSource);
	Report->SetBoolField(TEXT("physical_clock_override"),false);
	const auto Write=[&]()
	{
		FString JSON;const auto Writer=TJsonWriterFactory<>::Create(&JSON);if(!FJsonSerializer::Serialize(Report,Writer))return false;
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsoluteReportPath),true);
		return FFileHelper::SaveStringToFile(JSON,*AbsoluteReportPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	};
	if(!Write())return false;
	int32 Passed=0,Failed=0,Visited=0;TArray<TSharedPtr<FJsonValue>> Checks,CaseRows;
	const auto Check=[&](const FString& Name,bool bPassed,const FString& Detail=FString())
	{
		auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("name"),Name);Row->SetBoolField(TEXT("passed"),bPassed);Row->SetStringField(TEXT("detail"),Detail);Checks.Add(MakeShared<FJsonValueObject>(Row));
		if(bPassed){++Passed;UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_AUDIT_PASS %s %s"),*Name,*Detail);}
		else{++Failed;UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_PHYSICAL_AUDIT_FAIL %s %s"),*Name,*Detail);}
	};
	const double Started=FPlatformTime::Seconds();FString Only;
	FParse::Value(FCommandLine::Get(),TEXT("AftershockPhysicalAuditOnly="),Only);
	Check(TEXT("engine_initialized"),GEngine!=nullptr);
	const int32 ContextsBefore=GEngine?GEngine->GetWorldContexts().Num():0;
	if(GEngine)for(const auto& Spec:Cases)
	{
		if(FCString::Strcmp(Spec.Key,TEXT("sendai_flood"))==0&&(!FullWaterSource||Only!=Spec.Key))continue;
		if(!Only.IsEmpty()&&Only!=Spec.Key)continue;
		++Visited;const FString Prefix=FString(Spec.Key)+TEXT("_");const int32 FailBefore=Failed;
		auto Case=MakeShared<FJsonObject>();Case->SetStringField(TEXT("case"),Spec.Key);Case->SetStringField(TEXT("region"),Spec.Region);Case->SetStringField(TEXT("focus"),Spec.Focus);
		Case->SetBoolField(TEXT("settlement_required"),Spec.bNeedsSettlement);
		Case->SetNumberField(TEXT("hazard"),static_cast<int32>(Spec.Type));Case->SetBoolField(TEXT("water_duration_override"),!FullWaterSource&&(Spec.Type==EDisasterType::Flood||Spec.Type==EDisasterType::Tsunami));
		Case->SetBoolField(TEXT("full_authored_water_source"),FullWaterSource&&(Spec.Type==EDisasterType::Flood||Spec.Type==EDisasterType::Tsunami));
		const double CaseStart=FPlatformTime::Seconds();
		{
			FWorldFixture Fixture;Fixture.Instance.Reset(NewObject<UGameInstance>(GEngine));
			Fixture.Instance->InitializeStandalone(FName(*FString::Printf(TEXT("PhysicalHazard_%s"),Spec.Key)));Fixture.World=Fixture.Instance->GetWorld();UWorld* World=Fixture.World;
			Check(Prefix+TEXT("real_game_instance_world"),World&&World->GetGameInstance()==Fixture.Instance.Get());
			if(World)
			{
				auto* Session=Fixture.Instance->GetSubsystem<UAftershockOpeningSessionSubsystem>();auto* Saves=Fixture.Instance->GetSubsystem<UAftershockSaveSubsystem>();
				auto* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();auto* Scenario=World->GetSubsystem<UAftershockScenarioSubsystem>();
				auto* Buildings=World->GetSubsystem<UBuildingManagerSubsystem>();auto* Life=World->GetSubsystem<UCityLifeSubsystem>();
				auto* Crowd=World->GetSubsystem<UCrowdRenderSubsystem>();auto* Economy=World->GetSubsystem<UEconomyAndStateSubsystem>();
				auto* Sim=World->GetSubsystem<UDisasterSimulationSubsystem>();auto* Director=World->GetSubsystem<UAftershockDisasterDirectorSubsystem>();
				const bool Owners=Session&&Saves&&Terrain&&Scenario&&Buildings&&Life&&Crowd&&Economy&&Sim&&Director;
				Check(Prefix+TEXT("canonical_owners"),Owners);
				if(Owners)
				{
					FAftershockSessionSettings Settings;Settings.Mode=EAftershockPlayMode::Sandbox;Settings.RegionID=FName(Spec.Region);Settings.FocusID=FName(Spec.Focus);Settings.Seed=77191;
					FText Error;const bool Configured=Session->ConfigureNewSession(Settings,Error);
					const bool Prepared=Configured&&Scenario->PrepareScenarioBeforeSettlement();
					const bool CoastReady=Prepared&&(Spec.Type!=EDisasterType::Tsunami||EnsureActualSourceOcean(*Terrain,*Case,Error));
					if(Spec.Type==EDisasterType::Tsunami)Check(Prefix+TEXT("real_source_ocean_corridor_before_simulation"),CoastReady,Error.ToString());
					if(CoastReady){Buildings->SeedInitialSettlement();Life->RefreshCity();Crowd->SynchronizeCanonicalPopulation();Session->MarkSessionStarted();}
					const int32 ActualBuildings=Buildings->GetActiveBuildingsCount(),ActualLiving=Life->GetState().LivingPopulation;
					Case->SetNumberField(TEXT("actual_buildings_at_guard"),ActualBuildings);Case->SetNumberField(TEXT("actual_living_at_guard"),ActualLiving);
					Case->SetNumberField(TEXT("actual_resident_records_at_guard"),Life->GetResidents().Num());Case->SetBoolField(TEXT("life_initialized_at_guard"),Life->IsCityInitialized());
					const bool SettlementContract=Spec.bNeedsSettlement?ActualBuildings>0&&ActualLiving>0:ActualBuildings==0&&ActualLiving==0&&Life->GetResidents().IsEmpty();
					const bool V3=CoastReady&&Terrain->ContinuationGeneratorVersion==3&&!Terrain->Cells.IsEmpty()&&Life->IsCityInitialized()&&SettlementContract;
					Check(Prefix+(Spec.bNeedsSettlement?TEXT("real_v3_city_buildings_and_life"):TEXT("real_v3_protected_source_world_has_initialized_zero_population")),V3,Error.ToString());
					if(V3)
					{
						int32 Rivers=0,Oceans=0;for(const auto& C:Terrain->Cells){Rivers+=C.bIsRiverbed?1:0;Oceans+=C.bIsOcean?1:0;}
						Case->SetNumberField(TEXT("resident_cells"),Terrain->Cells.Num());Case->SetNumberField(TEXT("river_cells"),Rivers);Case->SetNumberField(TEXT("ocean_cells"),Oceans);
						Case->SetNumberField(TEXT("buildings"),Buildings->GetActiveBuildingsCount());Case->SetNumberField(TEXT("living_before"),Life->GetState().LivingPopulation);
						Check(Prefix+TEXT("actual_expected_geography"),Spec.Type==EDisasterType::Flood?Rivers>0:Spec.Type==EDisasterType::Tsunami?Oceans>0&&Terrain->IsCoastalWorld():true);
						AAftershockCameraPawn* Camera=World->SpawnActor<AAftershockCameraPawn>();
						FAftershockCameraSaveState CameraState;CameraState.LocalFocus=Terrain->GridOrigin+FVector(Terrain->GridWidth*.5*Terrain->CellSize,Terrain->GridHeight*.5*Terrain->CellSize,0.);CameraState.Location=CameraState.LocalFocus;
						Check(Prefix+TEXT("camera_save_owner"),Camera&&Camera->RestoreSaveState(CameraState,Error));
						Economy->TreasuryTL=5000000; // Explicit fixture solvency, does not alter a player city.
						Sim->OnWorldBeginPlay(*World);Director->OnWorldBeginPlay(*World);
						FAftershockPhysicalHazardSaveState Initial;
						const bool Initialized=PumpUntil(*World,[&](){Sim->CapturePhysicalSaveState(Initial);return Initial.ModelVersion==2&&Initial.bWaterInitialized&&Initial.PublishedStep>=1;},60.);
						Check(Prefix+TEXT("persistent_hydraulic_owner_initializes_and_advances"),Initialized,FString::Printf(TEXT("cells=%d steps=%llu"),Initial.WaterCells.Num(),Initial.PublishedStep));
						const float Weight=Sim->GetPhysicalHazardWeight(Spec.Type);Case->SetNumberField(TEXT("physical_weight"),Weight);
						const bool Allowed=Initialized&&Sim->CanTriggerDisaster(Spec.Type,Error)&&FMath::IsFinite(Weight)&&Weight>0.f;
						Check(Prefix+TEXT("actual_canonical_material_and_region_gate"),Allowed,Error.ToString());
						if(Allowed)
						{
							const int64 TreasuryBefore=Economy->TreasuryTL;const int32 DeathsBefore=Life->GetState().TotalDeaths;
							const bool Warning=Director->RequestWarning(Spec.Type,3600.f,Error);
							const bool Accepted=Warning&&Director->TriggerPendingNow(Error);
							FAftershockPhysicalHazardSaveState Active;Sim->CapturePhysicalSaveState(Active);
							Check(Prefix+TEXT("actual_trigger_returns_pending_until_completion"),Warning&&Accepted&&Sim->IsPhysicalEventRunning()&&Active.Episode.EventID.IsValid()&&!Active.Episode.bOutcomeCommitted&&Economy->TreasuryTL==TreasuryBefore&&Life->GetState().TotalDeaths==DeathsBefore,Error.ToString());
							if(Accepted&&Active.Episode.bActive)
							{
								const FGuid EventID=Active.Episode.EventID;const bool Water=Spec.Type==EDisasterType::Flood||Spec.Type==EDisasterType::Tsunami;
								if(Water&&!FullWaterSource)FAftershockPhysicalHazardAuditDriver::ShortenWaterSource(*Sim);
								const auto& Source=Sim->GetPhysicalEpisodeState();
								Case->SetNumberField(TEXT("actual_source_duration_seconds"),Source.SourceDurationSeconds);
								Case->SetNumberField(TEXT("actual_simulation_time_scale"),Source.SimulationTimeScale);
								Case->SetNumberField(TEXT("production_drainage_tail_seconds"),Water?120.:0.);
								if(Water&&FullWaterSource)Check(Prefix+TEXT("full_production_source_duration_and_scale_are_unmodified"),
									Source.SourceDurationSeconds==(Spec.Type==EDisasterType::Flood?3600.:120.)&&Source.SimulationTimeScale==(Spec.Type==EDisasterType::Flood?20.:1.));
								TStrongObjectPtr<UAftershockSaveGame> ActiveSave;
								const bool Captured=PumpUntil(*World,[&]()
								{
									Sim->CapturePhysicalSaveState(Active);
									if(!Active.Episode.bActive||Active.Episode.PlaybackSeconds<=0.||(!Water&&Active.Episode.MassReplay.IsEmpty())
										||(Water&&(Active.PendingAcceptedSeconds<=1.e-12||Active.PendingForcingCells.IsEmpty())))return false;
									ActiveSave.Reset(Saves->CaptureWorld(World,Error));return ActiveSave.IsValid();
								},60.);
								Check(Prefix+TEXT("active_public_save_capture"),Captured&&ActiveSave.IsValid()&&ActiveSave->Physics.Episode.bActive&&ActiveSave->Physics.Episode.EventID==EventID,Error.ToString());
								if(!Water&&Captured)
								{
									bool Moving=false,Contacts=false,Pressure=false;FString Evidence;
									const bool Recorded=RecordActualMassEvidence(Active,*Case,Moving,Contacts,Pressure,Evidence);
									Check(Prefix+TEXT("accepted_replay_exposes_actual_mass_metrics"),Recorded,Evidence);
									Check(Prefix+TEXT("accepted_mass_has_nonzero_physical_motion"),Recorded&&Moving,Evidence);
									if(Contacts)Check(Prefix+TEXT("actual_building_contacts_carry_pressure_or_impulse"),Recorded&&Pressure,Evidence);
								}
								if(ActiveSave.IsValid())
								{
									const auto Before=ActiveSave->Physics;const int64 SavedTreasury=ActiveSave->Economy.Treasury;
									const bool Restored=Saves->RestoreWorld(World,ActiveSave.Get(),Error);FAftershockPhysicalHazardSaveState After;Sim->CapturePhysicalSaveState(After);
									Case->SetBoolField(TEXT("raw_restore_observation_is_resavable_frontier"),After.bWaterInitialized);
									Case->SetNumberField(TEXT("restored_accepted_seconds_before_tick"),After.PendingAcceptedSeconds);
									Check(Prefix+TEXT("active_public_restore_keeps_exact_frontier_and_original_pending_packet_before_tick"),Restored&&SameWaterFrontier(Before,After)&&Sim->IsPhysicalEventRunning()&&Economy->TreasuryTL==SavedTreasury,Error.ToString());
								}
								const bool Finished=PumpUntil(*World,[&](){FText Failure;return !Sim->IsPhysicalEventRunning()||(FullWaterSource&&Water&&Sim->GetPhysicalRuntimeFailure(Failure));},FullWaterSource&&Water?300.:120.)&&!Sim->IsPhysicalEventRunning();
								FAftershockPhysicalHazardSaveState Final;Sim->CapturePhysicalSaveState(Final);const FDisasterReport Settled=Director->GetState().LastReport;
								FText RuntimeFailure;const bool RuntimeStopped=Sim->GetPhysicalRuntimeFailure(RuntimeFailure);
								Case->SetBoolField(TEXT("terminal_runtime_stopped"),RuntimeStopped);Case->SetStringField(TEXT("terminal_runtime_failure"),RuntimeFailure.ToString());
								Case->SetNumberField(TEXT("terminal_world_seconds"),World->GetTimeSeconds());Case->SetNumberField(TEXT("terminal_physical_seconds"),Final.PhysicalSeconds);
								Case->SetNumberField(TEXT("terminal_pending_physical_seconds"),Final.PendingPhysicalSeconds);Case->SetNumberField(TEXT("terminal_pending_accepted_seconds"),Final.PendingAcceptedSeconds);
								Case->SetNumberField(TEXT("terminal_source_elapsed_seconds"),Final.Episode.SourceElapsedSeconds);Case->SetNumberField(TEXT("terminal_playback_seconds"),Final.Episode.PlaybackSeconds);
								Case->SetNumberField(TEXT("terminal_director_remaining_seconds"),Director->GetState().RemainingSeconds);
								if(Water&&FullWaterSource)
								{
									const auto& Visual=Sim->GetLatestVisualSnapshot();
									Check(Prefix+TEXT("full_water_source_and_production_drainage_tail_physically_completed"),Finished&&Final.Episode.PlaybackSeconds>=Final.Episode.SourceDurationSeconds+120.);
									Check(Prefix+TEXT("completed_expired_visual_has_no_second_director_countdown"),Finished&&World->GetTimeSeconds()>=Visual.StartedAtWorldSeconds+Visual.GetEffectiveDurationSeconds()&&Director->GetState().RemainingSeconds==0.f);
									Check(Prefix+TEXT("full_water_lifecycle_has_no_stopped_runtime"),!RuntimeStopped,RuntimeFailure.ToString());
								}
								Check(Prefix+TEXT("actual_completion_delivers_one_final_event"),Finished&&!Settled.bIsPending&&!Settled.bWasRejected&&Settled.DisasterEventID==EventID&&Final.Episode.bOutcomeCommitted);
								if(Water&&Finished&&Settled.DisasterEventID==EventID)
								{
									const double CompletedPhysicalSeconds=Final.PhysicalSeconds;
									const bool Persistent=PumpUntil(*World,[&](){Sim->CapturePhysicalSaveState(Final);return Final.PublishedStep>=Initial.PublishedStep+200&&Final.PhysicalSeconds>CompletedPhysicalSeconds;},60.);
									Check(Prefix+TEXT("water_continues_after_the_completed_episode"),Persistent&&!Final.Episode.bActive&&Final.Episode.EventID==EventID);
								}
								Check(Prefix+TEXT("water_budget_is_closed_and_not_numeric_correction_forged"),FMath::IsFinite(Final.WaterBudget.ResidualM3)&&FMath::Abs(Final.WaterBudget.ResidualM3)<=1e-6*FMath::Max(1.,Final.WaterBudget.InitialSurfaceM3+Final.WaterBudget.RainM3+Final.WaterBudget.BoundaryInflowM3)&&FMath::Abs(Final.WaterBudget.NumericalVolumeCorrectionM3)<=1e-8*FMath::Max(1.,Final.WaterBudget.InitialSurfaceM3+Final.WaterBudget.RainM3));
								if(Water)Check(Prefix+TEXT("persistent_water_runs_at_least_200_accepted_steps"),Final.PublishedStep>=Initial.PublishedStep+200);
								if(Spec.Type==EDisasterType::Flood)Check(Prefix+TEXT("rain_is_declared_in_actual_si_volume_ledger"),Final.WaterBudget.RainM3>Initial.WaterBudget.RainM3);
								TArray<FGuid> Events;Economy->CaptureAppliedDisasterEvents(Events);const int64 Balance=Economy->TreasuryTL;const int32 Deaths=Life->GetState().TotalDeaths;
								const int32 AppliedCount=Events.FilterByPredicate([&](const FGuid& ID){return ID==EventID;}).Num();
								Check(Prefix+TEXT("canonical_fiscal_event_consumed_exactly_once"),AppliedCount==1);
								if(Finished&&Settled.DisasterEventID==EventID)
								{
									Sim->OnDisasterCompleted.Broadcast(Settled);TArray<FGuid> Replayed;Economy->CaptureAppliedDisasterEvents(Replayed);
									Check(Prefix+TEXT("same_real_final_callback_cannot_charge_or_award_twice"),Economy->TreasuryTL==Balance&&Life->GetState().TotalDeaths==Deaths&&Replayed.Num()==Events.Num());
								}
								Case->SetNumberField(TEXT("physical_seconds"),Final.PhysicalSeconds);Case->SetNumberField(TEXT("published_steps"),static_cast<double>(Final.PublishedStep));Case->SetNumberField(TEXT("rain_m3"),Final.WaterBudget.RainM3);Case->SetNumberField(TEXT("residual_m3"),Final.WaterBudget.ResidualM3);
								Case->SetNumberField(TEXT("final_deaths"),Settled.TotalDeaths);Case->SetNumberField(TEXT("final_survivors"),Settled.TotalSurvivors);Case->SetStringField(TEXT("event_id"),EventID.ToString());
							}
						}
					}
				}
			}
		}
		Case->SetBoolField(TEXT("valid"),Failed==FailBefore);Case->SetNumberField(TEXT("wall_seconds"),FPlatformTime::Seconds()-CaseStart);CaseRows.Add(MakeShared<FJsonValueObject>(Case));
		Check(Prefix+TEXT("isolated_world_context_released"),GEngine->GetWorldContexts().Num()==ContextsBefore);
		Report->SetArrayField(TEXT("checks"),Checks);Report->SetArrayField(TEXT("cases"),CaseRows);Write();
	}
	const int32 Expected=Only.IsEmpty()?4:1;Check(TEXT("all_requested_cases_executed"),Visited==Expected);
	Report->SetArrayField(TEXT("checks"),Checks);Report->SetArrayField(TEXT("cases"),CaseRows);Report->SetNumberField(TEXT("passed"),Passed);Report->SetNumberField(TEXT("failed"),Failed);Report->SetNumberField(TEXT("case_count"),Visited);
	Report->SetStringField(TEXT("case_filter"),Only);Report->SetNumberField(TEXT("wall_seconds"),FPlatformTime::Seconds()-Started);Report->SetBoolField(TEXT("in_progress"),false);Report->SetBoolField(TEXT("valid"),Failed==0&&Visited==Expected);
	const bool Saved=Write();UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_HAZARD_AUDIT_RESULT cases=%d passed=%d failed=%d saved=%d"),Visited,Passed,Failed,Saved);
	return Saved&&Failed==0&&Visited==Expected;
}
#else
bool RunAftershockPhysicalHazardFixtures(const FString&){return false;}
#endif
