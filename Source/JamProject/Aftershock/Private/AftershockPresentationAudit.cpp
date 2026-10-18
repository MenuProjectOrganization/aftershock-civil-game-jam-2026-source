#include "AftershockPresentationAudit.h"
#include "AftershockCityLifeRuntimeAudit.h"
#include "AftershockRubbleRuntimeAudit.h"
#include "AftershockBridgeRuntimeAudit.h"
#include "AftershockChunkRuntimeAudit.h"
#include "AftershockNarrativeRuntimeAudit.h"
#include "AftershockHydraulicRuntimeAudit.h"
#include "AftershockFireRuntimeAudit.h"

#if !UE_BUILD_SHIPPING
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/DisasterPresentationSubsystem.h"
#include "Aftershock/Public/DisasterAudioBridgeSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/AftershockRegionEnvironmentProfile.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/CityPresentationSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/AftershockMainHUDWidget.h"
#include "Aftershock/Game/AftershockPlayerController.h"
#include "AftershockGlobeSurface.h"
#include "AftershockTerrainSurface.h"
#include "AftershockPresentationBudget.h"
#include "AftershockEnvironmentQuality.h"
#include "AftershockArchitecturalRenderer.h"
#include "AftershockArchitecturalAudit.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "GameFramework/GameUserSettings.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "CoreGlobals.h"
#include "Components/SkyLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/PostProcessComponent.h"
#include "Engine/TextureCube.h"
#include "Math/SHMath.h"
#include "RenderCommandFence.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "EngineUtils.h"
#include "GeometryCollection/GeometryCollectionActor.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "ImageUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Misc/DefaultValueHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Serialization/Csv/CsvParser.h"
#include "TextureResource.h"
#include "SceneInterface.h"
#include "RenderUtils.h"
#include "StaticMeshResources.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameters.h"
#include "TimerManager.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "Math/InverseRotationMatrix.h"
#include <atomic>
#include "UnrealClient.h"

namespace
{
	bool IsActiveAuditWorld(UWorld* World)
	{
		if (!IsValid(World) || World->bIsTearingDown || !GEngine) return false;
		const FWorldContext* Context = GEngine->GetWorldContextFromWorld(World);
		return Context && Context->World() == World;
	}
	bool SaveRenderAuditJson(const TSharedRef<FJsonObject>& Object,const FString& Name);
	TSharedRef<FJsonObject> ReadRenderAuditMetadata(UWorld& World,const FString& Stage);
	struct FFramePacingAuditSnapshot
	{
		bool MarkerApplied=false,Confirmed=false;
		float EngineLimit=-1.f,CVarLimit=-1.f;
		int32 VSync=-1;
		const TCHAR* Policy() const{return Confirmed?TEXT("UncappedEphemeralNoSettingsWrite"):TEXT("NotConfirmedByEphemeralAuditMarker");}
	};
	FFramePacingAuditSnapshot ReadFramePacingAudit(const UWorld* World)
	{
		FFramePacingAuditSnapshot Snapshot;
		const auto* Controller=World?Cast<AAftershockPlayerController>(World->GetFirstPlayerController()):nullptr;
		Snapshot.MarkerApplied=FParse::Param(FCommandLine::Get(),TEXT("AftershockVisualAudit"))
			&&Controller&&Controller->HasAppliedEphemeralAuditFramePacing();
		Snapshot.EngineLimit=GEngine?GEngine->GetMaxFPS():-1.f;
		if(const auto* Limit=IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS")))Snapshot.CVarLimit=Limit->GetFloat();
		if(const auto* VSync=IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync")))Snapshot.VSync=VSync->GetInt();
		Snapshot.Confirmed=Snapshot.MarkerApplied&&FMath::IsNearlyZero(Snapshot.EngineLimit,.0001f)
			&&FMath::IsNearlyZero(Snapshot.CVarLimit,.0001f)&&Snapshot.VSync==0;
		return Snapshot;
	}

	struct FRenderProfileAuditState
	{
		int32 Agents=0,RequestedFrames=300,ActualCsvFrames=INDEX_NONE;
		float RequestedDuration=14.f,MinimumRuntime=14.f,DeadlineSeconds=180.f;
		bool bTimedCapture=false,bCaptureRequested=false,bEndRequested=false,bFinished=false;
		double StartedAt=0.,CaptureStartedAt=0.,CaptureEndedAt=0.,CapturedFrameSeconds=0.;
		FDateTime CaptureRequestedUtc;
		FString CsvPath;
		FTimerHandle MonitorTimer;
#if CSV_PROFILER
		TSharedFuture<FString> Completion;
#endif
	};

	void ReadCompletedProfileCsv(FRenderProfileAuditState& State)
	{
		FString Text;State.ActualCsvFrames=0;State.CapturedFrameSeconds=0.;
		if(!FFileHelper::LoadFileToString(Text,*State.CsvPath))return;
		const FCsvParser Parser(MoveTemp(Text));const auto& Rows=Parser.GetRows();
		if(Rows.IsEmpty())return;
		int32 FrameTimeColumn=INDEX_NONE;
		for(int32 I=0;I<Rows[0].Num();++I)if(FCString::Strcmp(Rows[0][I],TEXT("FrameTime"))==0){FrameTimeColumn=I;break;}
		if(FrameTimeColumn==INDEX_NONE)return;
		for(int32 RowIndex=1;RowIndex<Rows.Num();++RowIndex)
		{
			const auto& Row=Rows[RowIndex];
			// FCsvParser preserves quoted commas in EVENTS and metadata. Unreal's
			// repeated header and bracketed metadata footer are not frame samples.
			if(!Row.IsValidIndex(FrameTimeColumn)||(!Row.IsEmpty()&&FCString::Strcmp(Row[0],TEXT("[HasHeaderRowAtEnd]"))==0))continue;
			double Milliseconds=0.;
			if(FDefaultValueHelper::ParseDouble(FString(Row[FrameTimeColumn]),Milliseconds)&&FMath::IsFinite(Milliseconds)&&Milliseconds>0.)
			{++State.ActualCsvFrames;State.CapturedFrameSeconds+=Milliseconds/1000.;}
		}
	}

	void PollRenderProfile(TWeakObjectPtr<UWorld> WeakWorld,TSharedRef<FRenderProfileAuditState> State)
	{
		UWorld* World=WeakWorld.Get();
		if(State->bFinished||!IsActiveAuditWorld(World))return;
		const double Now=FPlatformTime::Seconds(),Runtime=Now-State->StartedAt;
		bool bCsvReady=false;int32 ActualFrames=0;
#if CSV_PROFILER
		FCsvProfiler* Profiler=FCsvProfiler::Get();
		if(State->bCaptureRequested&&State->bTimedCapture&&!State->bEndRequested&&FCsvProfiler::IsCapturing()
			&&Now-State->CaptureStartedAt>=State->RequestedDuration)
		{
			State->Completion=Profiler->EndCapture();State->bEndRequested=true;State->CaptureEndedAt=Now;
		}
		const bool bFreshFile=State->bCaptureRequested&&IFileManager::Get().FileSize(*State->CsvPath)>0
			&&IFileManager::Get().GetTimeStamp(*State->CsvPath)>=State->CaptureRequestedUtc;
		const bool bCaptureIdle=!FCsvProfiler::IsCapturing()&&!Profiler->IsWritingFile()&&!Profiler->IsEndCapturePending();
		// The profiler deliberately clears GetOutputFilename() after completion.
		// The timed future retains its completed filename; the frame capture uses
		// its explicitly requested path plus a new timestamp and idle-writer check.
		bCsvReady=bFreshFile&&bCaptureIdle
			&&(!State->bTimedCapture||(State->Completion.IsValid()&&State->Completion.IsReady()&&State->Completion.Get()==State->CsvPath));
		if(bCsvReady)
		{
			if(State->CaptureEndedAt==0.)State->CaptureEndedAt=Now;
			// GetCaptureFrameNumber includes the RT catch-up frame after Stop.
			// Read the completed CSV once to report the actual emitted samples.
			if(State->ActualCsvFrames==INDEX_NONE)ReadCompletedProfileCsv(*State);
			ActualFrames=State->ActualCsvFrames;
		}
#endif
		const bool bDeadline=Runtime>=State->DeadlineSeconds;
		if((Runtime<State->MinimumRuntime||!bCsvReady)&&!bDeadline)return;
		State->bFinished=true;World->GetTimerManager().ClearTimer(State->MonitorTimer);
		float RequestedOverview=0.f;
		if(FParse::Value(FCommandLine::Get(),TEXT("AftershockOverviewAudit="),RequestedOverview))
		{
			// Observe the final unchanged camera after CSV writing completes. No
			// screenshot/ReadPixels or metadata file I/O is injected into this capture.
			auto Metadata=ReadRenderAuditMetadata(*World,TEXT("profile-final"));
			Metadata->SetNumberField(TEXT("requested_overview"),RequestedOverview);
			SaveRenderAuditJson(Metadata,TEXT("profile-view-metadata.json"));
		}
		// Recheck live positions after the entire capture/session, not merely the
		// initial screenshot. This fixture still runs the normal80ms ISM timer.
		if(const UCrowdRenderSubsystem* Crowd=World->GetSubsystem<UCrowdRenderSubsystem>())Crowd->AuditCrowdVisibility();
		const bool bCountValid=State->bTimedCapture?ActualFrames>=300:ActualFrames==State->RequestedFrames;
		const bool bDurationValid=!State->bTimedCapture||State->CapturedFrameSeconds>=State->RequestedDuration;
		const double ObservedCaptureSeconds=State->CaptureEndedAt>0.?State->CaptureEndedAt-State->CaptureStartedAt:0.;
		const auto FramePacing=ReadFramePacingAudit(World);
		const FString Sidecar=FString::Printf(TEXT("{\"schema_version\":2,\"agents\":%d,\"capture_mode\":\"%s\",\"requested_frames\":%d,\"actual_frames\":%d,\"requested_capture_seconds\":%.3f,\"captured_frame_seconds\":%.3f,\"capture_wall_seconds_observed\":%.3f,\"minimum_runtime_seconds\":%.3f,\"actual_runtime_seconds\":%.3f,\"capture_starts_after_seconds\":3,\"synthetic_setup_after_seconds\":2,\"capture_started_after_seconds\":%.3f,\"completion_deadline_seconds\":%.3f,\"csv_ready\":%s,\"frame_count_valid\":%s,\"timed_out\":%s,\"capture_policy\":\"%s\",\"frame_pacing_marker_applied\":%s,\"max_fps_engine\":%.6f,\"frame_rate_limit_cvar_fps\":%.6f,\"vsync_cvar\":%d,\"uncapped_ephemeral_audit_confirmed\":%s}"),
			State->Agents,State->bTimedCapture?TEXT("duration"):TEXT("frames"),State->bTimedCapture?0:State->RequestedFrames,ActualFrames,
			State->bTimedCapture?State->RequestedDuration:0.f,State->CapturedFrameSeconds,ObservedCaptureSeconds,State->MinimumRuntime,Runtime,
			State->CaptureStartedAt>0.?State->CaptureStartedAt-State->StartedAt:0.,State->DeadlineSeconds,
			bCsvReady?TEXT("true"):TEXT("false"),bCountValid?TEXT("true"):TEXT("false"),bDeadline?TEXT("true"):TEXT("false"),
			FramePacing.Policy(),FramePacing.MarkerApplied?TEXT("true"):TEXT("false"),FramePacing.EngineLimit,FramePacing.CVarLimit,FramePacing.VSync,FramePacing.Confirmed?TEXT("true"):TEXT("false"));
		const bool bJsonSaved=FFileHelper::SaveStringToFile(Sidecar,*(FPaths::ChangeExtension(State->CsvPath,TEXT("profile.json"))),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if(bCsvReady&&bCountValid&&bDurationValid&&bJsonSaved&&!bDeadline)
		{
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_RENDER_PROFILE_COMPLETE agents=%d frames=%d requestedFrames=%d captureMode=%s requestedCaptureSeconds=%.3f capturedFrameSeconds=%.3f observedCaptureSeconds=%.3f runtimeSeconds=%.3f csvReady=1 jsonSaved=1 finalVisibilityImmediatelyAbove=1 scope=WholeScene_NotIsolatedCrowdGpuCost"),
				State->Agents,ActualFrames,State->bTimedCapture?0:State->RequestedFrames,State->bTimedCapture?TEXT("duration"):TEXT("frames"),State->bTimedCapture?State->RequestedDuration:0.f,State->CapturedFrameSeconds,ObservedCaptureSeconds,Runtime);
		}
		else
		{
			UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_RENDER_PROFILE_FAIL agents=%d frames=%d csvReady=%d frameCountValid=%d durationValid=%d capturedFrameSeconds=%.3f jsonSaved=%d deadlineReached=%d runtimeSeconds=%.3f"),State->Agents,ActualFrames,bCsvReady,bCountValid,bDurationValid,State->CapturedFrameSeconds,bJsonSaved,bDeadline,Runtime);
		}
		FPlatformMisc::RequestExit(false);
	}

	// Development fixture bookkeeping only. No UObject/world pointers or gameplay
	// authority survive travel; each stage reads the new world's actual subsystems.
	struct FCampaignTravelAuditState
	{
		FString InitialMapPackage;
		int32 ExpectedScenario = 0;
		int32 ArrivedWorlds = 0;
		bool bFailed = false;
	};
	FCampaignTravelAuditState CampaignTravelState;
	struct FRestartRuntimeAuditState
	{
		FString MapPackage;
		int32 ScenarioIndex = INDEX_NONE;
		int32 InitialSeed = 0;
		int32 InitialYear = 0;
		int64 InitialBudget = 0;
		int64 InitialPopulation = 0;
		int32 InitialAgents = 0;
		bool bRequested = false;
		bool bFinished = false;
	};
	FRestartRuntimeAuditState RestartRuntimeState;

	void RunRestartRuntimeStage(UWorld& World)
	{
		if (!IsActiveAuditWorld(&World) || RestartRuntimeState.bFinished) return;
		UAftershockScenarioSubsystem* Scenario = World.GetSubsystem<UAftershockScenarioSubsystem>();
		UTerrainGridSubsystem* Terrain = World.GetSubsystem<UTerrainGridSubsystem>();
		UBuildingManagerSubsystem* Buildings = World.GetSubsystem<UBuildingManagerSubsystem>();
		UEconomyAndStateSubsystem* Economy = World.GetSubsystem<UEconomyAndStateSubsystem>();
		UCrowdRenderSubsystem* Crowd = World.GetSubsystem<UCrowdRenderSubsystem>();
		URoadNetworkSubsystem* Roads = World.GetSubsystem<URoadNetworkSubsystem>();
		AAftershockPlayerController* PC = Cast<AAftershockPlayerController>(World.GetFirstPlayerController());
		UAftershockMainHUDWidget* HUD = PC ? PC->GetMainHUD() : nullptr;
		const auto Fail = [](const TCHAR* Reason)
		{
			RestartRuntimeState.bFinished = true;
			UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_RESTART_AUDIT_RESULT passed=0 reason=%s realHUDRestart=1 debugFixture=1"), Reason);
		};
		if (!Scenario || !Terrain || !Buildings || !Economy || !Crowd || !Roads || !HUD)
		{
			Fail(TEXT("Current-world subsystem or real HUD missing")); return;
		}
		int32 CLIIndex = INDEX_NONE;
		FParse::Value(FCommandLine::Get(), TEXT("-AftershockScenario="), CLIIndex);
		const int32 URLIndex = FCString::Atoi(World.URL.GetOption(TEXT("AftershockScenario="), TEXT("-1")));
		const FString MapPackage = World.GetOutermost()->GetName();
		const FAftershockScenarioDefinition& Definition = Scenario->GetDefinition();
		const int32 ExpectedIndex = RestartRuntimeState.bRequested ? RestartRuntimeState.ScenarioIndex : URLIndex;
		const int32 ExpectedSeed = RestartRuntimeState.bRequested ? RestartRuntimeState.InitialSeed : Definition.Seed;
		const int32 ExpectedYear = RestartRuntimeState.bRequested ? RestartRuntimeState.InitialYear : Definition.StartYear;
		const int64 ExpectedBudget = RestartRuntimeState.bRequested ? RestartRuntimeState.InitialBudget : Definition.StartingBudgetTL;
		TArray<FBuildingInstance> Records; Buildings->GetAllBuildings(Records);
		int64 Population = 0;
		UBuildingData* House = nullptr;
		for (const FBuildingInstance& B : Records)
		{
			if (!B.BuildingData) continue;
			Population += FMath::Max(0, B.BuildingData->Residents);
			if (B.BuildingData->Type == EBuildingType::House) House = B.BuildingData;
		}
		const bool bStartup = Scenario->IsPrepared() && (ExpectedIndex == 1 || ExpectedIndex == 2)
			&& Scenario->GetScenarioIndex() == ExpectedIndex && CLIIndex == 0 && URLIndex == ExpectedIndex
			&& Terrain->TerrainSeed == ExpectedSeed && Economy->TreasuryTL == ExpectedBudget && Economy->CurrentYear == ExpectedYear && Economy->CurrentTurn == 1
			&& Records.Num() == Scenario->GetDesignedLots().Num() && Roads->CountRoadCells() == Scenario->GetStarterRoadCellCount() && !World.IsPaused()
			&& Crowd->GetCurrentCrowdCount() == FMath::Min<int64>(1500, Population);
		if (!bStartup)
		{
			Fail(TEXT("Expected scenario1/2 URL to override stale CLI0 with reset seed/budget/year/turn/population/roads and no pause")); return;
		}
		if (RestartRuntimeState.bRequested)
		{
			const bool bRestored = MapPackage == RestartRuntimeState.MapPackage
				&& Population == RestartRuntimeState.InitialPopulation && Crowd->GetCurrentCrowdCount() == RestartRuntimeState.InitialAgents;
			RestartRuntimeState.bFinished = true;
			if (!bRestored) { Fail(TEXT("Restart changed map or canonical initial population")); return; }
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_RESTART_AUDIT_RESULT passed=1 scenario=%d seed=%d year=%d turn=%d budget=%lld buildings=%d population=%lld agents=%d roads=%d paused=%d cliIndex=%d urlIndex=%d sameMap=1 realHUDRestart=1 dirtyStateReset=1 debugFixture=1"),
				ExpectedIndex, Terrain->TerrainSeed, Economy->CurrentYear, Economy->CurrentTurn, Economy->TreasuryTL, Records.Num(), Population,
				Crowd->GetCurrentCrowdCount(), Roads->CountRoadCells(), World.IsPaused(), CLIIndex, URLIndex);
			return;
		}
		RestartRuntimeState.MapPackage = MapPackage;
		RestartRuntimeState.ScenarioIndex = ExpectedIndex;
		RestartRuntimeState.InitialSeed = ExpectedSeed;
		RestartRuntimeState.InitialYear = ExpectedYear;
		RestartRuntimeState.InitialBudget = ExpectedBudget;
		RestartRuntimeState.InitialPopulation = Population;
		RestartRuntimeState.InitialAgents = Crowd->GetCurrentCrowdCount();
		FAftershockRoadQuote Quote;
		bool bRoad=false;
		for(const FGridCell& Cell:Terrain->Cells)
		{
			if(Cell.bHasRoad||!Cell.bIsPlayable||Cell.bIsRiverbed||Cell.OccupyingBuildingID!=INDEX_NONE)continue;
			if(Roads->BuildRoad(Scenario->GetAssemblyCells()[0],Cell.Coords,Quote)){bRoad=true;break;}
		}
		bool bHouse = false;
		if (House)
		{
			for (const FGridCell& Cell : Terrain->Cells)
			{
				if (Buildings->CanPlaceBuildingAt(Cell.Coords, House) && Economy->BuildBuildingWithBudget(Cell.Coords, House, EFoundationQuality::Medium, House->FloorsMin))
				{ bHouse = true; break; }
			}
		}
		for (int32 I = 0; I < 9; ++I) Economy->AdvanceTurn();
		const bool bChanged = bRoad && bHouse && Roads->CountRoadCells() > Scenario->GetStarterRoadCellCount() && Buildings->GetActiveBuildingsCount() == Scenario->GetDesignedLots().Num()+1
			&& Economy->CurrentYear == ExpectedYear + 1 && Economy->CurrentTurn == 10 && Economy->TreasuryTL != ExpectedBudget
			&& Crowd->GetCurrentCrowdCount() > RestartRuntimeState.InitialAgents && !Economy->bIsGameOver;
		if (!bChanged) { Fail(TEXT("Real paid road/house and nine-turn fixture did not alter the initial state")); return; }
		HUD->TogglePauseMenu();
		if (!World.IsPaused()) { Fail(TEXT("Real HUD pause did not enter paused state before restart")); return; }
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_RESTART_AUDIT_REQUEST fromScenario=%d seed=%d dirtyBudget=%lld dirtyYear=%d dirtyTurn=%d dirtyBuildings=%d dirtyAgents=%d dirtyRoads=%d pausedBeforeRestart=1 realHUDRestart=1 debugFixture=1"),
			ExpectedIndex, Terrain->TerrainSeed, Economy->TreasuryTL, Economy->CurrentYear, Economy->CurrentTurn, Buildings->GetActiveBuildingsCount(), Crowd->GetCurrentCrowdCount(), Roads->CountRoadCells());
		RestartRuntimeState.bRequested = true;
		HUD->RestartCurrentLevel();
		// No pointers from this old world are retained or dereferenced after travel.
	}

	struct FCaptureFinalPPObservation
	{
		std::atomic<bool> Seen{false};
		int32 GI=INDEX_NONE,Reflection=INDEX_NONE,ExposureMethod=INDEX_NONE,Width=0,Height=0;
		float ExposureBias=0.f;
		bool bPhysicalCamera=false,bViewState=false,bLighting=false,bGIShowFlag=false;
		FVector Location=FVector::ZeroVector;
	};
	// Component-local extension: it is not registered globally. It observes the
	// actual capture view on RT AFTER engine volume/camera/default resolution.
	class FCaptureFinalPPObserver final:public ISceneViewExtension
	{
	public:
		FCaptureFinalPPObserver(FRenderTarget* InTarget,TSharedRef<FCaptureFinalPPObservation,ESPMode::ThreadSafe> InObservation)
			:Target(InTarget),Observation(InObservation){}
		virtual void PreRenderView_RenderThread(FRDGBuilder&,FSceneView& V) override
		{
			if(!V.Family||V.Family->RenderTarget!=Target||Observation->Seen.load(std::memory_order_acquire))return;
			const auto& P=V.FinalPostProcessSettings;
			Observation->GI=static_cast<int32>(P.DynamicGlobalIlluminationMethod);
			Observation->Reflection=static_cast<int32>(P.ReflectionMethod);
			Observation->ExposureMethod=static_cast<int32>(P.AutoExposureMethod);
			Observation->ExposureBias=P.AutoExposureBias;Observation->bPhysicalCamera=P.AutoExposureApplyPhysicalCameraExposure;
			Observation->bViewState=V.State!=nullptr;Observation->bLighting=V.Family->EngineShowFlags.Lighting;
			Observation->bGIShowFlag=V.Family->EngineShowFlags.GlobalIllumination;
			Observation->Width=V.UnconstrainedViewRect.Width();Observation->Height=V.UnconstrainedViewRect.Height();
			Observation->Location=V.ViewMatrices.GetViewOrigin();
			Observation->Seen.store(true,std::memory_order_release);
		}
	private:
		FRenderTarget* Target=nullptr;
		TSharedRef<FCaptureFinalPPObservation,ESPMode::ThreadSafe> Observation;
	};
	bool ResolvePinnedCapturePostProcess(UWorld& World,const FMinimalViewInfo& Pinned,int32 Width,int32 Height,
		FRenderTarget* Target,TUniquePtr<FSceneViewFamilyContext>& OutFamily,FString& Error)
	{
		auto* PC=World.GetFirstPlayerController();auto* LP=PC?PC->GetLocalPlayer():nullptr;
		auto* GameViewport=World.GetGameViewport();auto* Viewport=GameViewport?GameViewport->Viewport:nullptr;
		if(!PC||!LP||!GameViewport||!Viewport||!World.Scene||!Target)
		{Error=TEXT("MissingActualLocalPlayerViewportForFinalPostProcess");return false;}
		if(GEngine&&GEngine->IsStereoscopic3D(Viewport))
		{Error=TEXT("StereoCapturePostProcessNotValidated");return false;}
		auto Candidate=MakeUnique<FSceneViewFamilyContext>(FSceneViewFamily::ConstructionValues(Target,World.Scene,GameViewport->EngineShowFlags).SetRealtimeUpdate(true));
		FSceneViewInitOptions Init;
		if(!LP->CalcSceneViewInitOptions(Init,Viewport,nullptr,INDEX_NONE))
		{Error=TEXT("ActualLocalPlayerViewInitializationFailed");return false;}
		Init.ViewFamily=Candidate.Get();
		// Resolve PP only. NEVER share/mutate the player's temporal/exposure state.
		Init.SceneViewStateInterface=nullptr;Init.ExposureSceneViewStateInterface=nullptr;
		Init.ViewOrigin=Pinned.Location;Init.ViewLocation=Pinned.Location;Init.ViewRotation=Pinned.Rotation;
		Init.FOV=Pinned.FOV;Init.DesiredFOV=Pinned.DesiredFOV;
		Init.SetViewRectangle(FIntRect(0,0,Width,Height));
		Init.ViewRotationMatrix=FInverseRotationMatrix(Pinned.Rotation)*FMatrix(
			FPlane(0,0,1,0),FPlane(1,0,0,0),FPlane(0,1,0,0),FPlane(0,0,0,1));
		FMinimalViewInfo ProjectionView=Pinned;ProjectionView.AspectRatio=static_cast<float>(Width)/Height;
		ProjectionView.bConstrainAspectRatio=false;Init.ProjectionMatrix=ProjectionView.CalculateProjectionMatrix();
		auto* Resolved=new FSceneView(Init);Candidate->Views.Add(Resolved);
		// Same non-stereo order as ULocalPlayer::CalcSceneView. World volumes are
		// evaluated at PINNED location; project CVar defaults are engine inputs,
		// not our inference of final GI when volumes/camera overrides exist.
		Resolved->StartFinalPostprocessSettings(Pinned.Location);
		const TArray<FPostProcessSettings>* Blends=nullptr;const TArray<float>* Weights=nullptr;
		const TArray<EViewTargetBlendOrder>* Orders=nullptr;
		if(PC->PlayerCameraManager)PC->PlayerCameraManager->GetCachedPostProcessBlends(Blends,Weights,Orders);
		if((Blends||Weights||Orders)&&(!Blends||!Weights||!Orders||Blends->Num()!=Weights->Num()||Blends->Num()!=Orders->Num()))
		{Error=TEXT("InvalidActualCameraPostProcessBlendShape");return false;}
		if(Blends)for(int32 I=0;I<Blends->Num();++I)if((*Orders)[I]==VTBlendOrder_Base)
			Resolved->OverridePostProcessSettings((*Blends)[I],(*Weights)[I]);
		Resolved->OverridePostProcessSettings(Pinned.PostProcessSettings,Pinned.PostProcessBlendWeight);
		if(Blends)for(int32 I=0;I<Blends->Num();++I)if((*Orders)[I]==VTBlendOrder_Override)
			Resolved->OverridePostProcessSettings((*Blends)[I],(*Weights)[I]);
		if(PC->PlayerCameraManager)PC->PlayerCameraManager->UpdatePhotographyPostProcessing(Resolved->FinalPostProcessSettings);
		Resolved->EndFinalPostprocessSettings(Init);
		const auto& PP=Resolved->FinalPostProcessSettings;
		if(!FMath::IsFinite(PP.AutoExposureBias))
		{Error=TEXT("NonFiniteResolvedExposure");return false;}
		OutFamily=MoveTemp(Candidate);return true;
	}
	bool SaveCaptureFinalPPReceipt(const FString& Path,const FMinimalViewInfo& Pinned,int32 Width,int32 Height,
		ESceneCaptureSource Source,const FFinalPostProcessSettings& Resolved,const FCaptureFinalPPObservation& Actual,
		bool bPixelsSaved)
	{
		const bool Seen=Actual.Seen.load(std::memory_order_acquire);
		const bool MaterialBuffer=Source==ESceneCaptureSource::SCS_BaseColor||Source==ESceneCaptureSource::SCS_Normal;
		const bool RequireLumen=!MaterialBuffer&&FParse::Param(FCommandLine::Get(),TEXT("AftershockCaptureRequireLumen"));
		const bool Matches=Seen&&Actual.GI==static_cast<int32>(Resolved.DynamicGlobalIlluminationMethod)
			&&Actual.Reflection==static_cast<int32>(Resolved.ReflectionMethod)
			&&Actual.ExposureMethod==static_cast<int32>(Resolved.AutoExposureMethod)
			&&FMath::IsNearlyEqual(Actual.ExposureBias,Resolved.AutoExposureBias,1.e-5f)
			&&Actual.bPhysicalCamera==Resolved.AutoExposureApplyPhysicalCameraExposure;
		const bool Pose=Seen&&Actual.Location.Equals(Pinned.Location,1.e-3)&&Actual.Width==Width&&Actual.Height==Height;
		const bool Lumen=Seen&&Actual.GI==static_cast<int32>(EDynamicGlobalIlluminationMethod::Lumen)
			&&Actual.bViewState&&Actual.bGIShowFlag&&Actual.bLighting;
		const bool Valid=bPixelsSaved&&Matches&&Pose&&(!RequireLumen||Lumen);
		auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("schema_version"),1);J->SetBoolField(TEXT("valid"),Valid);
		J->SetStringField(TEXT("scope"),TEXT("Actual capture FinalPostProcessSettings read on render thread; engine-resolved world volumes at pinned pose plus actual camera blends. Not measured indirect-light contribution, Lumen convergence, GPU timing, main-view temporal AA or pixel-quality acceptance."));
		J->SetNumberField(TEXT("capture_source"),static_cast<int32>(Source));J->SetBoolField(TEXT("material_buffer"),MaterialBuffer);
		J->SetBoolField(TEXT("pixels_saved"),bPixelsSaved);J->SetBoolField(TEXT("render_thread_observed"),Seen);
		J->SetBoolField(TEXT("resolved_to_actual_key_settings_match"),Matches);J->SetBoolField(TEXT("pinned_pose_resolution_preserved"),Pose);
		J->SetBoolField(TEXT("raw_camera_settings_applied_once_in_resolver"),true);
		J->SetBoolField(TEXT("inherit_final_settings_without_main_camera_or_resolution"),true);
		J->SetBoolField(TEXT("player_temporal_state_shared"),false);J->SetBoolField(TEXT("lumen_required"),RequireLumen);
		J->SetBoolField(TEXT("actual_capture_lumen_configured"),Lumen);J->SetBoolField(TEXT("lumen_convergence_verified"),false);
		J->SetNumberField(TEXT("resolved_gi_method"),static_cast<int32>(Resolved.DynamicGlobalIlluminationMethod));
		J->SetNumberField(TEXT("resolved_reflection_method"),static_cast<int32>(Resolved.ReflectionMethod));
		J->SetNumberField(TEXT("resolved_exposure_method"),static_cast<int32>(Resolved.AutoExposureMethod));
		J->SetNumberField(TEXT("resolved_exposure_bias"),Resolved.AutoExposureBias);
		J->SetNumberField(TEXT("actual_gi_method"),Actual.GI);J->SetNumberField(TEXT("actual_reflection_method"),Actual.Reflection);
		J->SetNumberField(TEXT("actual_exposure_method"),Actual.ExposureMethod);J->SetNumberField(TEXT("actual_exposure_bias"),Actual.ExposureBias);
		J->SetBoolField(TEXT("actual_apply_physical_camera"),Actual.bPhysicalCamera);J->SetBoolField(TEXT("actual_view_state_present"),Actual.bViewState);
		J->SetBoolField(TEXT("actual_lighting_showflag"),Actual.bLighting);J->SetBoolField(TEXT("actual_gi_showflag"),Actual.bGIShowFlag);
		J->SetNumberField(TEXT("actual_width"),Actual.Width);J->SetNumberField(TEXT("actual_height"),Actual.Height);
		FString JSON;const bool Written=FJsonSerializer::Serialize(J,TJsonWriterFactory<>::Create(&JSON))
			&&FFileHelper::SaveStringToFile(JSON,*(Path+TEXT(".capture.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CAPTURE_FINAL_PP source=%d valid=%d seenRT=%d match=%d pinned=%d GI=%d reflection=%d exposureMethod=%d exposureBias=%.6f lumenRequired=%d lumenConfigured=%d saved=%d"),
			static_cast<int32>(Source),Valid,Seen,Matches,Pose,Actual.GI,Actual.Reflection,Actual.ExposureMethod,Actual.ExposureBias,RequireLumen,Lumen,Written);
		return Valid&&Written;
	}

	bool CaptureScene(UWorld& World, const FMinimalViewInfo& View, const FString& Path,int32 Width=1600,int32 Height=1000,ESceneCaptureSource Source=ESceneCaptureSource::SCS_FinalColorLDR,UPrimitiveComponent* HiddenForCapture=nullptr)
	{
		const bool MaterialBuffer=Source==ESceneCaptureSource::SCS_BaseColor||Source==ESceneCaptureSource::SCS_Normal;
		if(Source!=ESceneCaptureSource::SCS_FinalColorLDR&&!MaterialBuffer)return false;
		if(MaterialBuffer)
		{
			// UE silently falls back to SceneColorHDR for these sources under
			// Forward Shading. Refuse a mislabeled albedo/normal diagnostic.
			if(!World.Scene||IsForwardShadingEnabled(World.Scene->GetShaderPlatform()))
			{UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_MATERIAL_BUFFER unsupported=1 reason=DeferredRendererNotConfirmed requestedSource=%d"),static_cast<int32>(Source));return false;}
		}
		ASceneCapture2D* CaptureActor = World.SpawnActor<ASceneCapture2D>();
		if (!CaptureActor) return false;
		USceneCaptureComponent2D* Capture = CaptureActor->GetCaptureComponent2D();
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(CaptureActor);
		Target->RenderTargetFormat = MaterialBuffer?RTF_RGBA16f:RTF_RGBA8;
		Target->TargetGamma = MaterialBuffer?1.f:2.2f;
		Target->InitAutoFormat(Width, Height);
		Target->UpdateResourceImmediate(true);
		Capture->TextureTarget = Target;
		Capture->CaptureSource = Source;
		if(HiddenForCapture)Capture->HideComponent(HiddenForCapture);
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		Capture->bAlwaysPersistRenderingState = true;
		Capture->ProjectionType = View.ProjectionMode;
		Capture->OrthoWidth = View.OrthoWidth;
		Capture->FOVAngle = View.FOV;
		Capture->SetWorldLocationAndRotation(View.Location, View.Rotation);
		TUniquePtr<FSceneViewFamilyContext> ResolvedPPFamily;
		FString PPError;
		FRenderTarget* CaptureTarget=Target->GameThread_GetRenderTargetResource();
		if(!ResolvePinnedCapturePostProcess(World,View,Width,Height,CaptureTarget,ResolvedPPFamily,PPError))
		{
			UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_CAPTURE_FINAL_PP valid=0 reason=%s"),*PPError);
			CaptureActor->Destroy();return false;
		}
		const auto Observation=MakeShared<FCaptureFinalPPObservation,ESPMode::ThreadSafe>();
		const auto Observer=MakeShared<FCaptureFinalPPObserver,ESPMode::ThreadSafe>(CaptureTarget,Observation);
		const TSharedPtr<ISceneViewExtension,ESPMode::ThreadSafe> ObserverInterface=Observer;
		Capture->SceneViewExtensions.Add(ObserverInterface);
		// Camera settings were blended exactly once by the engine-order resolver.
		// Inherit its final PP, without borrowing the player's temporal state,
		// replacing the pinned camera or changing render-target resolution.
		Capture->PostProcessSettings=View.PostProcessSettings;
		Capture->PostProcessBlendWeight=0.f;
		Capture->MainViewFamily=ResolvedPPFamily.Get();
		Capture->bInheritMainViewCameraPostProcessSettings=true;
		Capture->bMainViewCamera=false;Capture->bMainViewResolution=false;Capture->bMainViewFamily=false;
		Capture->CaptureScene();
		TArray<FColor> Pixels;
		bool bSaved = false;
		bool Read=false;
		if(MaterialBuffer)
		{
			TArray<FLinearColor> LinearPixels;
			FReadSurfaceDataFlags Flags(RCM_MinMax);Flags.SetLinearToGamma(false);
			Read=Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(LinearPixels,Flags)&&LinearPixels.Num()==Width*Height;
			if(Read)
			{
				Pixels.Reserve(LinearPixels.Num());
				for(const FLinearColor& Linear:LinearPixels)
				{
					if(!FMath::IsFinite(Linear.R)||!FMath::IsFinite(Linear.G)||!FMath::IsFinite(Linear.B)){Read=false;break;}
					// BaseColor is linear GBuffer albedo, encoded to sRGB8 for PNG.
					// WorldNormal is [-1,1], remapped to [0,1] and encoded linearly.
					Pixels.Add(Source==ESceneCaptureSource::SCS_Normal
						?FLinearColor(Linear.R*.5f+.5f,Linear.G*.5f+.5f,Linear.B*.5f+.5f,1.f).ToFColor(false)
						:FLinearColor(Linear.R,Linear.G,Linear.B,1.f).ToFColorSRGB());
				}
			}
		}
		else Read=Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
		if (Read)
		{
			for (FColor& Pixel : Pixels) Pixel.A = 255;
			TArray64<uint8> PNG;
			FImageUtils::PNGCompressImageArray(Width, Height, Pixels, PNG);
			bSaved = FFileHelper::SaveArrayToFile(PNG, *Path);
		}
		// Keep the borrowed family, target and local observer alive until the
		// queued render has consumed them and the RT receipt is visible on GT.
		FRenderCommandFence CaptureFence;CaptureFence.BeginFence();CaptureFence.Wait();
		const bool bPPValid=SaveCaptureFinalPPReceipt(Path,View,Width,Height,Source,
			ResolvedPPFamily->Views[0]->FinalPostProcessSettings,*Observation,bSaved);
		Capture->MainViewFamily=nullptr;
		Capture->SceneViewExtensions.Reset();
		CaptureActor->Destroy();
		return bSaved&&bPPValid;
	}

	TSharedRef<FJsonObject> CaptureGroundOcclusionDiagnostics(UWorld& World,const FMinimalViewInfo& View,const FString& Stem,int32 Width,int32 Height)
	{
		auto Result=MakeShared<FJsonObject>();
		UStaticMeshComponent* Ground=nullptr;int32 GroundCount=0;
		for(TActorIterator<AAftershockCrowdHostActor> It(&World);It;++It)
			if(IsValid(It->TerrainSurface)){Ground=It->TerrainSurface;++GroundCount;}
		Result->SetNumberField(TEXT("intended_ground_candidates"),GroundCount);
		TArray<TSharedPtr<FJsonValue>> Inventory;
		for(TActorIterator<AActor> It(&World);It;++It)
		{
			TInlineComponentArray<UStaticMeshComponent*> Components;It->GetComponents(Components);
			for(const UStaticMeshComponent* Component:Components)
			{
				const UStaticMesh* Mesh=Component->GetStaticMesh();if(!Mesh)continue;
				const FString Name=(Component->GetName()+TEXT(" ")+Mesh->GetName()).ToLower();
				const FBox Box=Component->Bounds.GetBox();const FVector Size=Box.GetSize();
				const bool GroundLike=Component==Ground||Name.Contains(TEXT("terrain"))||Name.Contains(TEXT("ground"))||Name.Contains(TEXT("landscape"))
					||((Name.Contains(TEXT("plane"))||Name.Contains(TEXT("cube")))&&FMath::Min(Size.X,Size.Y)>10000.&&Size.Z<FMath::Max(Size.X,Size.Y)*.05);
				if(!GroundLike)continue;
				auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("component"),Component->GetPathName());Row->SetStringField(TEXT("actor"),It->GetPathName());
				Row->SetStringField(TEXT("mesh"),Mesh->GetPathName());Row->SetBoolField(TEXT("is_intended_ground"),Component==Ground);
				Row->SetBoolField(TEXT("visible_flag"),Component->GetVisibleFlag());Row->SetBoolField(TEXT("hidden_in_game"),Component->bHiddenInGame);
				Row->SetBoolField(TEXT("actor_hidden"),It->IsHidden());Row->SetBoolField(TEXT("registered"),Component->IsRegistered());
				Row->SetNumberField(TEXT("collision_enabled_enum"),static_cast<int32>(Component->GetCollisionEnabled()));
				Row->SetNumberField(TEXT("forced_lod_model"),Component->GetForcedLodModel());
				TArray<TSharedPtr<FJsonValue>> Bounds;
				for(double Value:{Box.Min.X,Box.Min.Y,Box.Min.Z,Box.Max.X,Box.Max.Y,Box.Max.Z})Bounds.Add(MakeShared<FJsonValueNumber>(Value));
				Row->SetArrayField(TEXT("world_bounds_min_max_cm"),Bounds);
				TArray<TSharedPtr<FJsonValue>> Materials;for(int32 Slot=0;Slot<Component->GetNumMaterials();++Slot)
				{const UMaterialInterface* Material=Component->GetMaterial(Slot);Materials.Add(MakeShared<FJsonValueString>(Material?Material->GetPathName():FString()));}
				Row->SetArrayField(TEXT("actual_material_paths"),Materials);Inventory.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
		Result->SetArrayField(TEXT("ground_like_static_meshes"),Inventory);
		const bool Candidate=GroundCount==1&&Ground&&Ground->GetFName()==FName(TEXT("ContinuousTerrainSurface"))&&Ground->GetStaticMesh()
			&&Ground->IsRegistered()&&Ground->GetVisibleFlag()&&!Ground->bHiddenInGame&&Ground->GetOwner()&&!Ground->GetOwner()->IsHidden();
		Result->SetBoolField(TEXT("expected_ground_enabled"),Candidate);
		Result->SetBoolField(TEXT("valid"),false);
		if(!Candidate){Result->SetStringField(TEXT("failure"),TEXT("Expected exactly one enabled native ContinuousTerrainSurface"));return Result;}
		const bool VisibleBefore=Ground->GetVisibleFlag(),HiddenBefore=Ground->bHiddenInGame;
		const auto CollisionBefore=Ground->GetCollisionEnabled();const FTransform TransformBefore=Ground->GetComponentTransform();
		UStaticMesh* MeshBefore=Ground->GetStaticMesh();TArray<UMaterialInterface*> MaterialsBefore;
		for(int32 Slot=0;Slot<Ground->GetNumMaterials();++Slot)MaterialsBefore.Add(Ground->GetMaterial(Slot));
		const FString VisiblePath=Stem+TEXT("-ground-visible.png"),HiddenPath=Stem+TEXT("-ground-hidden.png");
		const bool VisibleSaved=CaptureScene(World,View,VisiblePath,Width,Height);
		// Capture-local hidden primitive list: the game world's visibility,
		// collision, actor, transform, material and canonical data stay untouched.
		const bool HiddenSaved=CaptureScene(World,View,HiddenPath,Width,Height,ESceneCaptureSource::SCS_FinalColorLDR,Ground);
		bool MaterialsSame=MaterialsBefore.Num()==Ground->GetNumMaterials();
		for(int32 Slot=0;Slot<MaterialsBefore.Num();++Slot)MaterialsSame&=MaterialsBefore[Slot]==Ground->GetMaterial(Slot);
		const bool Unchanged=VisibleBefore==Ground->GetVisibleFlag()&&HiddenBefore==Ground->bHiddenInGame&&CollisionBefore==Ground->GetCollisionEnabled()
			&&MeshBefore==Ground->GetStaticMesh()&&TransformBefore.Equals(Ground->GetComponentTransform(),0.)&&MaterialsSame;
		Result->SetBoolField(TEXT("valid"),VisibleSaved&&HiddenSaved&&Unchanged);
		Result->SetBoolField(TEXT("visible_capture_saved"),VisibleSaved);Result->SetBoolField(TEXT("hidden_capture_saved"),HiddenSaved);
		Result->SetStringField(TEXT("visible_capture_path"),VisiblePath);Result->SetStringField(TEXT("hidden_capture_path"),HiddenPath);
		Result->SetStringField(TEXT("hidden_component"),Ground->GetPathName());Result->SetBoolField(TEXT("game_component_state_unchanged"),Unchanged);
		Result->SetBoolField(TEXT("capture_local_hide_only"),true);Result->SetBoolField(TEXT("pixel_attribution_acceptance_complete"),false);
		Result->SetStringField(TEXT("scope"),TEXT("Same pinned View FinalLDR pair, only intended ground hidden in second SceneCapture. World visibility/collision/material unchanged. JSON valid means capture/state checks only; compare actual pixels to determine contribution/occlusion. No temporal-noise-free or geometric occlusion verdict."));
		return Result;
	}

	TSharedRef<FJsonObject> CaptureMaterialBufferDiagnostics(UWorld& World,const FMinimalViewInfo& View,const FString& Stem,int32 Width=1600,int32 Height=900)
	{
		auto Result=MakeShared<FJsonObject>();
		const bool Albedo=FParse::Param(FCommandLine::Get(),TEXT("AftershockAlbedoAudit"));
		const bool Normals=FParse::Param(FCommandLine::Get(),TEXT("AftershockNormalAudit"));
		const bool GroundOcclusion=FParse::Param(FCommandLine::Get(),TEXT("AftershockGroundOcclusionAudit"));
		Result->SetBoolField(TEXT("requested"),Albedo||Normals||GroundOcclusion);Result->SetBoolField(TEXT("valid"),true);
		if(!Albedo&&!Normals&&!GroundOcclusion)return Result;
		Result->SetNumberField(TEXT("width"),Width);Result->SetNumberField(TEXT("height"),Height);
		Result->SetStringField(TEXT("scope"),TEXT("Diagnostic opaque GBuffer material attributes, separate pinned-view capture. Not FinalColor, not translucent water appearance, not calibrated irradiance. No gameplay/material/settings mutation."));
		bool Valid=true;
		if(Albedo)
		{
			const FString Path=Stem+TEXT("-basecolor.png");
			const bool Saved=CaptureScene(World,View,Path,Width,Height,ESceneCaptureSource::SCS_BaseColor);Valid&=Saved;
			Result->SetBoolField(TEXT("basecolor_saved"),Saved);Result->SetStringField(TEXT("basecolor_path"),Path);
			Result->SetStringField(TEXT("basecolor_encoding"),TEXT("Opaque GBuffer linear BaseColor -> explicit sRGB8 PNG; no lighting/exposure inference"));
		}
		if(Normals)
		{
			const FString Path=Stem+TEXT("-worldnormal.png");
			const bool Saved=CaptureScene(World,View,Path,Width,Height,ESceneCaptureSource::SCS_Normal);Valid&=Saved;
			Result->SetBoolField(TEXT("worldnormal_saved"),Saved);Result->SetStringField(TEXT("worldnormal_path"),Path);
			Result->SetStringField(TEXT("worldnormal_encoding"),TEXT("Opaque GBuffer WorldNormal remapped n*.5+.5 -> linearUNorm8 PNG"));
		}
		if(GroundOcclusion)
		{
			auto GroundDiagnostic=CaptureGroundOcclusionDiagnostics(World,View,Stem,Width,Height);
			Result->SetObjectField(TEXT("ground_occlusion_diagnostic"),GroundDiagnostic);Valid&=GroundDiagnostic->GetBoolField(TEXT("valid"));
		}
		Result->SetBoolField(TEXT("valid"),Valid);
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MATERIAL_BUFFER valid=%d baseColorRequested=%d worldNormalRequested=%d groundOcclusionRequested=%d captureOnly=1 noGameplayWrites=1"),Valid,Albedo,Normals,GroundOcclusion);
		return Result;
	}

	bool SaveRenderAuditJson(const TSharedRef<FJsonObject>& Object,const FString& Name)
	{
		const FString Folder=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation"));
		IFileManager::Get().MakeDirectory(*Folder,true);FString Text;
		const auto Writer=TJsonWriterFactory<>::Create(&Text);
		return FJsonSerializer::Serialize(Object,Writer)
			&&FFileHelper::SaveStringToFile(Text,*(Folder/Name),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool SetRenderAuditZoom(UWorld& World,double TargetZoom)
	{
		auto* Controller=World.GetFirstPlayerController();
		auto* Camera=Controller?Cast<AAftershockCameraPawn>(Controller->GetPawn()):nullptr;
		const auto* Terrain=World.GetSubsystem<UTerrainGridSubsystem>();
		if(!Camera||!Terrain||!FMath::IsFinite(TargetZoom)||TargetZoom<1500.)return false;
		FAftershockCameraSaveState View;Camera->CaptureSaveState(View);
		const auto Layout=GetAftershockGlobeLayout(*Terrain);
		const double Start=Layout.GridSpanCm*.6;
		const double Linear=FMath::Clamp((TargetZoom-Start)/FMath::Max(1.,Layout.RadiusCm*1.6-Start),0.,1.);
		View.Zoom=static_cast<float>(TargetZoom);View.OverviewBlend=static_cast<float>(Linear*Linear*(3.-2.*Linear));
		View.Location=FMath::Lerp(View.LocalFocus,Layout.Center,View.OverviewBlend);
		FText Error;const bool Applied=Camera->RestoreSaveState(View,Error);
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_RENDER_AUDIT_CAMERA targetZoomCm=%.1f applied=%d cameraOnly=1 canonicalWrites=0 error=%s"),TargetZoom,Applied,*Error.ToString());
		return Applied;
	}
	void SetRenderAuditVector(const TSharedRef<FJsonObject>& Object,const TCHAR* Key,const FVector& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Values;for(double Coordinate:{Value.X,Value.Y,Value.Z})Values.Add(MakeShared<FJsonValueNumber>(Coordinate));Object->SetArrayField(Key,Values);
	}
	TSharedRef<FJsonObject> ReadRenderMaterialSlot(const UStaticMeshComponent& Component,int32 Slot)
	{
		auto Object=MakeShared<FJsonObject>();const UMaterialInterface* Material=Component.GetMaterial(Slot);
		Object->SetNumberField(TEXT("slot"),Slot);Object->SetBoolField(TEXT("material_present"),Material!=nullptr);
		Object->SetStringField(TEXT("material"),Material?Material->GetPathName():FString());
		if(!Material)return Object;
		for(const TCHAR* Name:{TEXT("RegionalStyleEnabled"),TEXT("RegionalStyleTexturesReady"),TEXT("RegionalSourceMaskEnabled"),TEXT("RegionalMeanTemperatureC"),TEXT("RegionalClimateElevationM"),TEXT("RegionalVisualMonth"),TEXT("RegionalWetness"),TEXT("RegionalBiomeSlot"),TEXT("RegionalVolcanic"),TEXT("RegionalVolcanicActualStyle"),TEXT("RegionalMacroReady"),TEXT("GlobalImageryReady"),TEXT("RegionalSurroundReady"),TEXT("FloodEncodingCm"),TEXT("FloodPhysicalArrivalEnabled"),TEXT("FloodPhysicalDataEnabled"),TEXT("FloodArrivalProgress"),TEXT("FloodEnvelope")})
		{
			float Value=0.f;const bool Present=Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(Name)),Value);
			Object->SetBoolField(FString(Name)+TEXT("_present"),Present);Object->SetBoolField(FString(Name)+TEXT("_finite"),Present&&FMath::IsFinite(Value));
			if(Present&&FMath::IsFinite(Value))Object->SetNumberField(Name,Value);
		}
		for(const TCHAR* Name:{TEXT("RegionalSourceBoundsMin"),TEXT("RegionalSourceBoundsMax"),TEXT("RegionalMacroBoundsMin"),TEXT("RegionalMacroBoundsMax"),TEXT("GlobalRegionWorldMin"),TEXT("GlobalRegionWorldMax"),TEXT("GlobalRegionWGS84Min"),TEXT("GlobalRegionWGS84Max"),TEXT("RegionalSurroundGeoMin"),TEXT("RegionalSurroundGeoMax")})
		{
			FLinearColor Value;const bool Present=Material->GetVectorParameterValue(FHashedMaterialParameterInfo(FName(Name)),Value);
			Object->SetBoolField(FString(Name)+TEXT("_present"),Present);
			Object->SetBoolField(FString(Name)+TEXT("_finite"),Present&&FMath::IsFinite(Value.R)&&FMath::IsFinite(Value.G)&&FMath::IsFinite(Value.B));
			if(Present)SetRenderAuditVector(Object,Name,FVector(Value.R,Value.G,Value.B));
		}
		for(const TCHAR* Name:{TEXT("RegionalSourceMask"),TEXT("RegionalBaseDiffuse"),TEXT("RegionalBaseNormalDX"),TEXT("RegionalBaseRoughness"),TEXT("RegionalRockDiffuse"),TEXT("RegionalRockNormalDX"),TEXT("RegionalRockRoughness"),TEXT("RegionalMacroTexture"),TEXT("GlobalImageryTexture"),TEXT("RegionalSurroundTexture")})
		{
			UTexture* Texture=nullptr;const bool Present=Material->GetTextureParameterValue(FHashedMaterialParameterInfo(FName(Name)),Texture);auto TextureObject=MakeShared<FJsonObject>();
			TextureObject->SetBoolField(TEXT("parameter_present"),Present);TextureObject->SetStringField(TEXT("texture"),Texture?Texture->GetPathName():FString());
			if(const auto* Texture2D=Cast<UTexture2D>(Texture))
			{TextureObject->SetNumberField(TEXT("width"),Texture2D->GetSizeX());TextureObject->SetNumberField(TEXT("height"),Texture2D->GetSizeY());TextureObject->SetNumberField(TEXT("mips"),Texture2D->GetNumMips());TextureObject->SetBoolField(TEXT("linear"),!Texture2D->SRGB);TextureObject->SetBoolField(TEXT("streaming_enabled"),!Texture2D->NeverStream);}
			Object->SetObjectField(Name,TextureObject);
		}
		return Object;
	}
	TSharedRef<FJsonObject> ReadRenderVolcanoStats(const AActor& Actor,const FAftershockVolcanoVisualStats& Stats,const TCHAR* Kind)
	{
		auto Object=MakeShared<FJsonObject>();Object->SetStringField(TEXT("kind"),Kind);Object->SetStringField(TEXT("actor"),Actor.GetPathName());Object->SetBoolField(TEXT("actor_hidden"),Actor.IsHidden());
		Object->SetNumberField(TEXT("canonical_lava_cells"),Stats.CanonicalLavaCells);Object->SetNumberField(TEXT("drawn_lava_cells"),Stats.DrawnLavaCells);
		Object->SetNumberField(TEXT("lava_top_triangles"),Stats.LavaTopTriangles);Object->SetNumberField(TEXT("lava_skirt_triangles"),Stats.LavaSkirtTriangles);Object->SetNumberField(TEXT("vent_triangles"),Stats.VentTriangles);
		Object->SetNumberField(TEXT("plume_triangles"),Stats.PlumeTriangles);Object->SetNumberField(TEXT("spatter_instances"),Stats.SpatterInstances);Object->SetNumberField(TEXT("minimum_ground_clearance_cm"),Stats.MinimumGroundClearanceCm);
		Object->SetBoolField(TEXT("source_resident"),Stats.bSourceResident);Object->SetBoolField(TEXT("source_in_city_draw_window"),Stats.bSourceInDrawableWindow);Object->SetBoolField(TEXT("source_context_backed"),Stats.bSourceContextBacked);
		Object->SetBoolField(TEXT("requested_materials_loaded"),Stats.bMaterialsReady);SetRenderAuditVector(Object,TEXT("source_world_cm"),Stats.SourceWorld);
		TArray<TSharedPtr<FJsonValue>> Surfaces;TInlineComponentArray<UStaticMeshComponent*> Components;Actor.GetComponents(Components);
		for(const UStaticMeshComponent* Surface:Components)
		{
			if(!Surface->GetName().Contains(TEXT("Volcano"))&&!Surface->GetName().Contains(TEXT("LavaVolume")))continue;
			auto View=MakeShared<FJsonObject>();const UStaticMesh* Mesh=Surface->GetStaticMesh();const FStaticMeshRenderData* Data=Mesh?Mesh->GetRenderData():nullptr;
			View->SetStringField(TEXT("component"),Surface->GetPathName());View->SetStringField(TEXT("mesh"),Mesh?Mesh->GetPathName():FString());View->SetBoolField(TEXT("visible"),Surface->IsVisible());
			View->SetNumberField(TEXT("actual_lod0_triangles"),Data&&!Data->LODResources.IsEmpty()?Data->LODResources[0].GetNumTriangles():0);
			const auto* Instances=Cast<UInstancedStaticMeshComponent>(Surface);View->SetNumberField(TEXT("actual_instance_count"),Instances?Instances->GetInstanceCount():Mesh?1:0);
			View->SetObjectField(TEXT("material_slot0"),ReadRenderMaterialSlot(*Surface,0));SetRenderAuditVector(View,TEXT("world_bounds_min_cm"),Surface->Bounds.GetBox().Min);SetRenderAuditVector(View,TEXT("world_bounds_max_cm"),Surface->Bounds.GetBox().Max);
			Surfaces.Add(MakeShared<FJsonValueObject>(View));
		}
		Object->SetArrayField(TEXT("actual_surfaces"),Surfaces);return Object;
	}

	TSharedRef<FJsonObject> ReadUserSettingsDestinationAudit()
	{
		auto Out=MakeShared<FJsonObject>();
		FString Requested;
		const bool HasOverride=FParse::Value(FCommandLine::Get(),TEXT("GameUserSettingsINI="),Requested);
		const auto Normalize=[](FString Path)
		{
			if(Path.IsEmpty())return Path;
			Path=FPaths::ConvertRelativePathToFull(Path);FPaths::NormalizeFilename(Path);FPaths::CollapseRelativeDirectories(Path);
#if PLATFORM_WINDOWS
			Path=Path.ToLower();
#endif
			return Path;
		};
		const auto Fingerprint=[](const FString& Path)
		{
			const FTCHARToUTF8 Bytes(*Path);
			return FSHA1::HashBuffer(Bytes.Get(),Bytes.Length()).ToString().ToLower();
		};
        // UE5.8 known configs use a logical cache alias ("GameUserSettings").
        // Resolve the ALREADY-LOADED branch; do not compute a destination from
        // the CLI, create a branch, reload it, or confuse the alias with a path.
		const FConfigBranch* Branch=GConfig?GConfig->FindBranchWithNoReload(NAME_None,GGameUserSettingsIni):nullptr;
		const FString Physical=Branch?Branch->IniPath:FString();
		const FString Actual=Normalize(Physical),Expected=HasOverride?Normalize(Requested):FString();
		Out->SetNumberField(TEXT("schema_version"),2);Out->SetBoolField(TEXT("override_requested"),HasOverride);
		Out->SetBoolField(TEXT("config_branch_present"),Branch!=nullptr);
		Out->SetStringField(TEXT("config_branch_name"),Branch?Branch->IniName.ToString():FString());
		Out->SetStringField(TEXT("config_cache_key_filename_only"),FPaths::GetCleanFilename(GGameUserSettingsIni));
		Out->SetStringField(TEXT("effective_destination_source"),TEXT("LoadedGConfigBranch.IniPath"));
		Out->SetBoolField(TEXT("effective_destination_present"),!Actual.IsEmpty());
		Out->SetBoolField(TEXT("effective_destination_matches_override"),HasOverride&&!Actual.IsEmpty()&&Actual==Expected);
		Out->SetStringField(TEXT("effective_filename"),FPaths::GetCleanFilename(Physical));
		Out->SetStringField(TEXT("effective_normalized_path_sha1"),Actual.IsEmpty()?FString():Fingerprint(Actual));
		Out->SetStringField(TEXT("requested_normalized_path_sha1"),Expected.IsEmpty()?FString():Fingerprint(Expected));
#if PLATFORM_WINDOWS
		Out->SetStringField(TEXT("normalization"),TEXT("absolute-slashes-collapse-windows-lower-utf8-sha1"));
#else
		Out->SetStringField(TEXT("normalization"),TEXT("absolute-slashes-collapse-case-sensitive-utf8-sha1"));
#endif
		Out->SetStringField(TEXT("scope"),TEXT("Live loaded GConfig branch IniPath resolved from the GGameUserSettingsIni logical cache key. CLI is comparison-only; missing branch/path fails closed. No personal absolute path, branch creation/reload, config mutation or restore. Parent separately verifies original bytes after engine exit."));
		return Out;
	}

	TSharedRef<FJsonObject> ReadGraphicsQualityAudit(UWorld& World)
	{
		auto Out=MakeShared<FJsonObject>();
		const auto Quality=GetAftershockEnvironmentQuality(&World);
		const auto Camera=GetAftershockPresentationBudget(&World);
		const auto Environment=MakeAftershockEnvironmentBudget(Quality,Camera);
		const auto* Settings=UGameUserSettings::GetGameUserSettings();
		int32 Requested=INDEX_NONE;const bool RequestedPresent=FParse::Value(FCommandLine::Get(),TEXT("AftershockGraphicsQuality="),Requested);
		Out->SetNumberField(TEXT("schema_version"),1);Out->SetBoolField(TEXT("user_settings_present"),Settings!=nullptr);
		Out->SetBoolField(TEXT("fixture_requested"),RequestedPresent);
		if(RequestedPresent)Out->SetNumberField(TEXT("requested_quality"),Requested);
		Out->SetNumberField(TEXT("actual_matching_preset_index"),Quality.GetPresetIndex());
		Out->SetNumberField(TEXT("actual_quality_signature"),Quality.GetSignature());
		auto Groups=MakeShared<FJsonObject>();
		Groups->SetNumberField(TEXT("view_distance"),Quality.ViewDistance);Groups->SetNumberField(TEXT("shadows"),Quality.Shadows);
		Groups->SetNumberField(TEXT("global_illumination"),Quality.GlobalIllumination);Groups->SetNumberField(TEXT("reflections"),Quality.Reflections);
		Groups->SetNumberField(TEXT("anti_aliasing"),Quality.AntiAliasing);Groups->SetNumberField(TEXT("textures"),Quality.Textures);
		Groups->SetNumberField(TEXT("effects"),Quality.Effects);Groups->SetNumberField(TEXT("post_process"),Quality.PostProcess);
		Groups->SetNumberField(TEXT("foliage"),Quality.Foliage);Groups->SetNumberField(TEXT("shading"),Quality.Shading);
		Out->SetObjectField(TEXT("groups"),Groups);
		Out->SetObjectField(TEXT("user_settings_destination"),ReadUserSettingsDestinationAudit());
		auto CVars=MakeShared<FJsonObject>();
		for(const TCHAR* Name:{TEXT("r.ReflectionMethod"),TEXT("r.SSR.Quality"),TEXT("r.SSR.Temporal"),TEXT("r.ViewDistanceScale"),
			TEXT("r.Streaming.PoolSize"),TEXT("r.Streaming.MipBias"),TEXT("r.Streaming.FullyLoadUsedTextures"),
			TEXT("r.DynamicGlobalIlluminationMethod"),TEXT("r.ShadowQuality"),TEXT("foliage.DensityScale"),
            TEXT("r.DistanceFieldAO"),TEXT("r.AOQuality"),TEXT("r.AOObjectDistanceField"),TEXT("r.AOApplyToStaticIndirect"),
            TEXT("r.SkylightIntensityMultiplier"),TEXT("r.AmbientOcclusionLevels")})
		{
			auto Value=MakeShared<FJsonObject>();const IConsoleVariable* Variable=IConsoleManager::Get().FindConsoleVariable(Name);
			Value->SetBoolField(TEXT("found"),Variable!=nullptr);
			if(Variable){Value->SetNumberField(TEXT("value"),Variable->GetFloat());Value->SetNumberField(TEXT("set_by_mask"),static_cast<uint32>(Variable->GetFlags()&ECVF_SetByMask));}
			CVars->SetObjectField(Name,Value);
		}
		Out->SetObjectField(TEXT("actual_console_variables"),CVars);
		Out->SetStringField(TEXT("streaming_pool_scope"),TEXT("Live configured streaming-pool CVar, not measured allocated/used VRAM"));
		auto Limits=MakeShared<FJsonObject>();
		Limits->SetNumberField(TEXT("tree_cap"),Environment.TreeInstanceCap);Limits->SetNumberField(TEXT("shrub_cap"),Environment.ShrubInstanceCap);
		Limits->SetNumberField(TEXT("grass_cap"),Environment.GrassInstanceCap);Limits->SetNumberField(TEXT("tree_cull_end_cm"),Environment.TreeCullEndCm);
		Limits->SetNumberField(TEXT("grass_cull_end_cm"),Environment.GrassCullEndCm);
		Out->SetObjectField(TEXT("environment_policy"),Limits);
		TArray<TSharedPtr<FJsonValue>> Foliage,Materials;
		int32 FamilyInstances[5]={0,0,0,0,0},FamilyVisible[5]={0,0,0,0,0};
		const TCHAR* FamilyNames[]={TEXT("GrassClump"),TEXT("Shrub"),TEXT("Conifer"),TEXT("Palm"),TEXT("Broadleaf")};
		for(TActorIterator<AActor> It(&World);It;++It)
		{
			TInlineComponentArray<UStaticMeshComponent*> Components(*It);
			for(const auto* Component:Components)
			{
				const FString Name=Component->GetName();
				if(const auto* HISM=Cast<UHierarchicalInstancedStaticMeshComponent>(Component);HISM&&Name.StartsWith(TEXT("Woodland_")))
				{
					int32 Family=INDEX_NONE;for(int32 I=0;I<5;++I)if(Name.StartsWith(FString(TEXT("Woodland_"))+FamilyNames[I]+TEXT("_"))){Family=I;break;}
					if(Family!=INDEX_NONE)
					{
						const UStaticMesh* Mesh=HISM->GetStaticMesh();const auto* Data=Mesh?Mesh->GetRenderData():nullptr;
						auto Row=MakeShared<FJsonObject>();int32 Nonzero=0;
						for(int32 I=0;I<HISM->GetInstanceCount();++I){FTransform T;if(HISM->GetInstanceTransform(I,T,true)&&!T.GetScale3D().IsNearlyZero())++Nonzero;}
						Row->SetStringField(TEXT("family"),FamilyNames[Family]);Row->SetStringField(TEXT("component"),HISM->GetPathName());
						Row->SetBoolField(TEXT("mesh_present"),Mesh!=nullptr);if(Mesh)Row->SetStringField(TEXT("mesh"),Mesh->GetPathName());
						Row->SetNumberField(TEXT("source_instances"),HISM->GetInstanceCount());Row->SetNumberField(TEXT("nonzero_instances"),Nonzero);
						Row->SetBoolField(TEXT("component_visible"),HISM->IsVisible());Row->SetBoolField(TEXT("cast_shadow"),HISM->CastShadow!=0);
						Row->SetBoolField(TEXT("cast_dynamic_shadow"),HISM->bCastDynamicShadow!=0);
						Row->SetNumberField(TEXT("native_source_lod_count"),Mesh?Mesh->GetNumLODs():0);
						Row->SetNumberField(TEXT("actual_render_lod_count"),Data?Data->LODResources.Num():0);
						Row->SetNumberField(TEXT("forced_lod_model"),HISM->GetForcedLodModel());
						Row->SetNumberField(TEXT("cull_start_cm"),HISM->InstanceStartCullDistance);Row->SetNumberField(TEXT("cull_end_cm"),HISM->InstanceEndCullDistance);
						TArray<TSharedPtr<FJsonValue>> Triangles;if(Data)for(const auto& LOD:Data->LODResources)Triangles.Add(MakeShared<FJsonValueNumber>(LOD.GetNumTriangles()));
						Row->SetArrayField(TEXT("triangles_per_render_lod"),Triangles);
						Foliage.Add(MakeShared<FJsonValueObject>(Row));FamilyInstances[Family]+=Nonzero;if(HISM->IsVisible())FamilyVisible[Family]+=Nonzero;
					}
				}
				for(int32 Slot=0;Slot<Component->GetNumMaterials();++Slot)
				{
					const auto* Material=Component->GetMaterial(Slot);if(!Material)continue;
					auto Values=MakeShared<FJsonObject>();bool Any=false;
					for(const TCHAR* Parameter:{TEXT("GlobalGroundNormalQuality"),TEXT("GlobalWaterNormalQuality"),TEXT("GlobalWaterFoamQuality")})
					{
						float Value=0.f;const bool Present=Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(Parameter)),Value);
						auto Scalar=MakeShared<FJsonObject>();Scalar->SetBoolField(TEXT("parameter_present"),Present);
						if(Present&&FMath::IsFinite(Value)){Scalar->SetNumberField(TEXT("value"),Value);Any=true;}
						Values->SetObjectField(Parameter,Scalar);
					}
					if(Any)
					{
						Values->SetStringField(TEXT("component"),Component->GetPathName());Values->SetNumberField(TEXT("slot"),Slot);
						Values->SetStringField(TEXT("actual_material"),Material->GetPathName());
						Values->SetBoolField(TEXT("is_dynamic_material_instance"),Material->IsA<UMaterialInstanceDynamic>());
						Materials.Add(MakeShared<FJsonValueObject>(Values));
					}
				}
			}
		}
		Out->SetArrayField(TEXT("foliage_batches"),Foliage);Out->SetNumberField(TEXT("observed_foliage_hism_count"),Foliage.Num());
		auto Families=MakeShared<FJsonObject>();for(int32 I=0;I<5;++I)
		{auto Count=MakeShared<FJsonObject>();Count->SetNumberField(TEXT("nonzero_instances"),FamilyInstances[I]);Count->SetNumberField(TEXT("visible_component_nonzero_instances"),FamilyVisible[I]);Families->SetObjectField(FamilyNames[I],Count);}
		Out->SetObjectField(TEXT("foliage_families"),Families);Out->SetArrayField(TEXT("actual_environment_material_parameters"),Materials);
		auto Architecture=MakeShared<FJsonObject>();
		const auto* Manager=World.GetSubsystem<UBuildingManagerSubsystem>();const auto* Work=Manager?Manager->GetArchitecturalPresentationWork():nullptr;
		Architecture->SetBoolField(TEXT("observed"),Work!=nullptr);
		if(Work)
		{
			Architecture->SetNumberField(TEXT("buildings"),Work->Buildings);Architecture->SetNumberField(TEXT("detailed_buildings"),Work->DetailedBuildings);
			Architecture->SetNumberField(TEXT("instances"),Work->Instances);Architecture->SetNumberField(TEXT("active_batches"),Work->ActiveBatches);
			Architecture->SetNumberField(TEXT("allocated_instances"),Work->AllocatedInstances);
			Architecture->SetNumberField(TEXT("membership_rebuilds"),static_cast<double>(Work->MembershipRebuilds));
		}
		Out->SetObjectField(TEXT("architecture"),Architecture);
		Out->SetStringField(TEXT("scope"),TEXT("Read-only actual production settings/CVars/component/material/renderer telemetry. Component visibility/instance counts do not prove GPU visibility, occlusion, FPS, VRAM use or visual quality."));
		return Out;
	}


	TSharedRef<FJsonObject> ReadArchitectureCPDAudit(UWorld& World)
	{
		auto Object=MakeShared<FJsonObject>();FAftershockArchitectureAudit R;
		if(const auto* Manager=World.GetSubsystem<UBuildingManagerSubsystem>())Manager->ReadArchitecturalCPDAudit(R);
		Object->SetNumberField(TEXT("schema_version"),1);
		Object->SetBoolField(TEXT("valid"),R.IsValid());Object->SetBoolField(TEXT("has_visual_samples"),R.HasVisualSamples());
		Object->SetBoolField(TEXT("cpd_visual_acceptance_complete"),R.IsValid()&&R.HasVisualSamples());
		Object->SetBoolField(TEXT("renderer_present"),R.RendererPresent);
		Object->SetBoolField(TEXT("globe_suppressed"),R.GlobeSuppressed);
		Object->SetBoolField(TEXT("canonical_stride_valid"),R.CanonicalStrideValid);
		Object->SetBoolField(TEXT("derived_stride_valid"),R.DerivedStrideValid);
		Object->SetBoolField(TEXT("allocation_within_budget"),R.AllocationWithinBudget);
		Object->SetNumberField(TEXT("canonical_buildings"),R.CanonicalBuildings);
		Object->SetNumberField(TEXT("outside_window_buildings"),R.OutsideWindowBuildings);
		Object->SetNumberField(TEXT("collapsed_or_memorial_buildings"),R.CollapsedOrMemorialBuildings);
		Object->SetNumberField(TEXT("unsupported_buildings"),R.UnsupportedBuildings);
		Object->SetNumberField(TEXT("canonical_input_failures"),R.CanonicalInputFailures);
		Object->SetNumberField(TEXT("fallback_buildings"),R.FallbackBuildings);
		Object->SetNumberField(TEXT("expected_rendered_buildings"),R.ExpectedRenderedBuildings);
		Object->SetNumberField(TEXT("rendered_buildings"),R.RenderedBuildings);
		Object->SetNumberField(TEXT("missing_expected_buildings"),R.MissingExpectedBuildings);
		Object->SetNumberField(TEXT("unexpected_rendered_buildings"),R.UnexpectedRenderedBuildings);
		Object->SetNumberField(TEXT("referenced_modules"),R.ReferencedModules);
		Object->SetNumberField(TEXT("checked_module_instances"),R.CheckedModuleInstances);
		Object->SetNumberField(TEXT("checked_canonical_values"),R.CheckedCanonicalValues);
		Object->SetNumberField(TEXT("mismatched_canonical_values"),R.MismatchedCanonicalValues);
		Object->SetNumberField(TEXT("nonfinite_values"),R.NonfiniteValues);
		Object->SetNumberField(TEXT("invalid_references"),R.InvalidReferences);
		Object->SetNumberField(TEXT("duplicate_references"),R.DuplicateReferences);
		Object->SetNumberField(TEXT("unexpected_hidden_modules"),R.UnexpectedHiddenModules);
		Object->SetNumberField(TEXT("allocated_slots"),R.AllocatedSlots);
		Object->SetNumberField(TEXT("active_nonzero_slots"),R.ActiveNonzeroSlots);
		Object->SetNumberField(TEXT("active_membership_keys"),R.ActiveMembershipKeys);
		Object->SetNumberField(TEXT("pooled_inactive_slots"),R.PooledInactiveSlots);
		Object->SetNumberField(TEXT("visible_pooled_slots"),R.VisiblePooledSlots);
		Object->SetNumberField(TEXT("unreferenced_nonzero_slots"),R.UnreferencedNonzeroSlots);
		TArray<TSharedPtr<FJsonValue>> IDs;for(int32 ID:R.SampleProblemBuildingIDs)IDs.Add(MakeShared<FJsonValueNumber>(ID));
		Object->SetArrayField(TEXT("sample_problem_building_ids"),IDs);
		Object->SetStringField(TEXT("scope"),TEXT("Actual canonical4CPD to derived11CPD ID/component/index readback. Unsupported/fallback/outside-window/collapsed records explicit. Pooled storage is not visible population; no GPU visibility, timing or art certification."));return Object;
	}

	// Opt-in audit snapshots only. A bounded render-thread fence protects the CPU
	// SH copy: UE's UpdateSkyCaptureContents queues writes on the render thread.
	// This does not wait for a future deferred capture, recapture a light, read
	// GPU pixels, or establish the amount of indirect light on a building facade.

	// One-shot, opt-in A/B only. No map/config save and no shipping behavior change.
	struct FPhysicalSunPreviewReceipt
	{
		bool bAttempted=false,bApplied=false;
		int32 SunCandidates=0,SkyCandidates=0,GradeCandidates=0,AtmosphereCandidates=0,RecaptureRequests=0;
		float SunBefore=0.f,SkyBefore=0.f,BiasBefore=0.f,BiasExpected=0.f;
		FVector SunDirectionBefore=FVector::ZeroVector;
		FVector SkySHL0Before=FVector::ZeroVector;
		FLinearColor SunColorBefore=FLinearColor::Black,SkyColorBefore=FLinearColor::Black,LowerBefore=FLinearColor::Black;
		bool bLowerBlackBefore=false;
		FString Failure;
		TWeakObjectPtr<UDirectionalLightComponent> Sun;
		TWeakObjectPtr<USkyLightComponent> Sky;
		TWeakObjectPtr<UPostProcessComponent> Grade;
	};
	TMap<TWeakObjectPtr<UWorld>,FPhysicalSunPreviewReceipt> PhysicalSunPreviewReceipts;
	void ApplyPhysicalSunPreview(UWorld& World)
	{
		if(!FParse::Param(FCommandLine::Get(),TEXT("AftershockPhysicalSunPreview")))return;
		for(auto It=PhysicalSunPreviewReceipts.CreateIterator();It;++It)if(!It.Key().IsValid())It.RemoveCurrent();
		const TWeakObjectPtr<UWorld> Key(&World);
		if(PhysicalSunPreviewReceipts.Contains(Key))return;
		if(PhysicalSunPreviewReceipts.Num()>=8)
		{UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_PHYSICAL_SUN_PREVIEW applied=0 reason=ReceiptCapacity diagnosticOnly=1"));return;}
		auto& R=PhysicalSunPreviewReceipts.Add(Key);
		R.bAttempted=true;
		for(TActorIterator<AActor> It(&World);It;++It)
		{
			if(It->IsHidden())continue;
			TInlineComponentArray<UActorComponent*> Components(*It);
			for(UActorComponent* Component:Components)
			{
				if(auto* Sun=Cast<UDirectionalLightComponent>(Component))
					if(Sun->IsRegistered()&&Sun->IsVisible()&&Sun->bAffectsWorld&&Sun->Intensity>0.f)
					{R.Sun=Sun;++R.SunCandidates;}
				if(auto* Sky=Cast<USkyLightComponent>(Component))
					if(Sky->IsRegistered()&&Sky->IsVisible()&&Sky->bAffectsWorld&&Sky->Intensity>0.f)
					{R.Sky=Sky;++R.SkyCandidates;}
				if(auto* Atmosphere=Cast<USkyAtmosphereComponent>(Component))
					if(Atmosphere->IsRegistered()&&Atmosphere->IsVisible())++R.AtmosphereCandidates;
				if(auto* Grade=Cast<UPostProcessComponent>(Component))
					if(It->IsA<AAftershockCityPresentationActor>()&&Grade->GetFName()==FName(TEXT("PresentationGrade"))
						&&Grade->IsRegistered()&&Grade->bEnabled&&Grade->bUnbound&&Grade->BlendWeight>=.999f)
					{R.Grade=Grade;++R.GradeCandidates;}
			}
		}
		auto* Sun=R.Sun.Get();auto* Sky=R.Sky.Get();auto* Grade=R.Grade.Get();
		if(R.SunCandidates!=1||R.SkyCandidates!=1||R.GradeCandidates!=1||R.AtmosphereCandidates<1||!Sun||!Sky||!Grade)
			R.Failure=TEXT("ExpectedOneActiveSunSkyAndPresentationGradePlusAtmosphere");
		else if(Sun->GetMobility()!=EComponentMobility::Movable||!Sun->IsUsedAsAtmosphereSunLight()||Sun->GetAtmosphereSunLightIndex()!=0
			||Sky->GetMobility()!=EComponentMobility::Movable||Sky->SourceType!=SLS_CapturedScene||Sky->IsRealTimeCaptureEnabled())
			R.Failure=TEXT("ExpectedMovablePrimaryAtmosphereSunAndNonRealtimeCapturedSky");
		else if(!Grade->Settings.bOverride_AutoExposureMethod||Grade->Settings.AutoExposureMethod!=EAutoExposureMethod::AEM_Manual
			||!Grade->Settings.bOverride_AutoExposureBias||!Grade->Settings.bOverride_AutoExposureApplyPhysicalCameraExposure
			||Grade->Settings.AutoExposureApplyPhysicalCameraExposure||!FMath::IsFinite(Sun->Intensity)||Sun->Intensity<=.01f||!FMath::IsFinite(Sky->Intensity)
			||!FMath::IsFinite(Grade->Settings.AutoExposureBias))
			R.Failure=TEXT("ExpectedExistingFiniteManualExposureCalibration");
		else
		{
			R.SunBefore=Sun->Intensity;R.SkyBefore=Sky->Intensity;R.BiasBefore=Grade->Settings.AutoExposureBias;
			R.SunDirectionBefore=Sun->GetForwardVector();R.SunColorBefore=Sun->GetLightColor();R.SkyColorBefore=Sky->GetLightColor();
			R.LowerBefore=Sky->LowerHemisphereColor;R.bLowerBlackBefore=Sky->bLowerHemisphereIsBlack;
			FRenderCommandFence BeforeCaptureReadFence;BeforeCaptureReadFence.BeginFence();BeforeCaptureReadFence.Wait();
			const FSHVectorRGB3 BeforeSH=Sky->GetIrradianceEnvironmentMap();
			R.SkySHL0Before=FVector(BeforeSH.R.V[0],BeforeSH.G.V[0],BeforeSH.B.V[0]);
			// Retain the project's existing manual direct-light normalization; this
			// is not an independent exposure boost. +1 exposure bias doubles brightness.
			R.BiasExpected=R.BiasBefore-FMath::Log2(50000.f/R.SunBefore);
			if(!FMath::IsFinite(R.BiasExpected))R.Failure=TEXT("NonFiniteRebasedExposure");
			else
			{
				Sun->SetIntensity(50000.f);
				Grade->Settings.AutoExposureBias=R.BiasExpected;
				Grade->MarkRenderStateDirty();
				Sky->RecaptureSky();++R.RecaptureRequests;
				R.bApplied=FMath::IsNearlyEqual(Sun->Intensity,50000.f,.01f)
					&&FMath::IsNearlyEqual(Grade->Settings.AutoExposureBias,R.BiasExpected,.0001f);
				if(!R.bApplied)R.Failure=TEXT("PropertyApplicationDidNotReadBack");
			}
		}
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_SUN_PREVIEW applied=%d sunCandidates=%d skyCandidates=%d gradeCandidates=%d atmosphereCandidates=%d sunLuxBefore=%.6f sunLuxAfter=%.6f skyIntensityBefore=%.6f skyIntensityAfter=%.6f biasBefore=%.6f biasAfter=%.6f recaptureRequests=%d reason=%s diagnosticOnly=1 mapConfigSave=0"),
			R.bApplied,R.SunCandidates,R.SkyCandidates,R.GradeCandidates,R.AtmosphereCandidates,R.SunBefore,Sun?Sun->Intensity:0.f,
			R.SkyBefore,Sky?Sky->Intensity:0.f,R.BiasBefore,Grade?Grade->Settings.AutoExposureBias:0.f,R.RecaptureRequests,*R.Failure);
	}
	TSharedRef<FJsonObject> ReadPhysicalSunPreview(UWorld& World)
	{
		auto Out=MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("schema_version"),1);
		Out->SetBoolField(TEXT("requested"),FParse::Param(FCommandLine::Get(),TEXT("AftershockPhysicalSunPreview")));
		Out->SetBoolField(TEXT("diagnostic_only"),true);
		Out->SetBoolField(TEXT("map_or_config_save_requested"),false);
		const auto* R=PhysicalSunPreviewReceipts.Find(TWeakObjectPtr<UWorld>(&World));
		Out->SetBoolField(TEXT("attempted"),R&&R->bAttempted);Out->SetBoolField(TEXT("applied"),R&&R->bApplied);
		if(!R)return Out;
		Out->SetStringField(TEXT("failure"),R->Failure);
		Out->SetNumberField(TEXT("sun_candidates"),R->SunCandidates);Out->SetNumberField(TEXT("sky_candidates"),R->SkyCandidates);
		Out->SetNumberField(TEXT("grade_candidates"),R->GradeCandidates);Out->SetNumberField(TEXT("atmosphere_candidates"),R->AtmosphereCandidates);
		Out->SetNumberField(TEXT("recapture_requests"),R->RecaptureRequests);
		Out->SetNumberField(TEXT("sun_lux_before"),R->SunBefore);Out->SetNumberField(TEXT("sky_intensity_before"),R->SkyBefore);
		Out->SetNumberField(TEXT("manual_bias_before"),R->BiasBefore);Out->SetNumberField(TEXT("manual_bias_expected"),R->BiasExpected);
		const auto* Sun=R->Sun.Get();auto* Sky=R->Sky.Get();const auto* Grade=R->Grade.Get();
		Out->SetBoolField(TEXT("selected_components_still_valid"),Sun&&Sky&&Grade);
		if(Sun)
		{
			Out->SetNumberField(TEXT("sun_lux_observed"),Sun->Intensity);
			Out->SetBoolField(TEXT("sun_direction_unchanged"),Sun->GetForwardVector().Equals(R->SunDirectionBefore,1.e-6));
			Out->SetBoolField(TEXT("sun_color_unchanged"),Sun->GetLightColor().Equals(R->SunColorBefore,1.e-6f));
		}
		if(Sky)
		{
			Out->SetNumberField(TEXT("sky_intensity_observed"),Sky->Intensity);
			SetRenderAuditVector(Out,TEXT("sky_sh_l0_before"),R->SkySHL0Before);
			const FSHVectorRGB3 ObservedSH=Sky->GetIrradianceEnvironmentMap();
			const FVector ObservedL0(ObservedSH.R.V[0],ObservedSH.G.V[0],ObservedSH.B.V[0]);
			SetRenderAuditVector(Out,TEXT("sky_sh_l0_observed"),ObservedL0);
			Out->SetNumberField(TEXT("sky_sh_l0_max_abs_change"),(ObservedL0-R->SkySHL0Before).GetAbsMax());
			Out->SetBoolField(TEXT("sky_color_unchanged"),Sky->GetLightColor().Equals(R->SkyColorBefore,1.e-6f));
			Out->SetBoolField(TEXT("sky_lower_hemisphere_unchanged"),Sky->LowerHemisphereColor.Equals(R->LowerBefore,1.e-6f)&&Sky->bLowerHemisphereIsBlack==R->bLowerBlackBefore);
		}
		if(Grade)
		{
			Out->SetNumberField(TEXT("manual_bias_observed"),Grade->Settings.AutoExposureBias);
			Out->SetBoolField(TEXT("manual_mode_preserved"),Grade->Settings.bOverride_AutoExposureMethod&&Grade->Settings.AutoExposureMethod==EAutoExposureMethod::AEM_Manual
				&&Grade->Settings.bOverride_AutoExposureBias&&Grade->Settings.bOverride_AutoExposureApplyPhysicalCameraExposure&&!Grade->Settings.AutoExposureApplyPhysicalCameraExposure);
		}
		Out->SetStringField(TEXT("scope"),TEXT("50k lux matches the existing fallback comparison target, not a universal daylight prescription. Paired manual-bias rebase preserves direct-light normalization. One recapture request does not prove a finished sky capture or acceptable facade pixels; review the later processed SH and same-camera image. Sky intensity/color, lower hemisphere, sun direction/color and GI method are not modified."));
		return Out;
	}

	TSharedRef<FJsonObject> ReadLightingAudit(UWorld& World)
	{
		check(IsInGameThread());
		FRenderCommandFence LightingReadFence;
		LightingReadFence.BeginFence();
		LightingReadFence.Wait();
		auto Object=MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Skies,Suns,PresentationGrades;
		int32 EnabledSkies=0,EnabledSuns=0,NonRealtimeReview=0;
		for(TActorIterator<AActor> ActorIt(&World);ActorIt;++ActorIt)
		{

			if(ActorIt->IsA<AAftershockCityPresentationActor>())
			{
				TInlineComponentArray<UPostProcessComponent*> Grades;ActorIt->GetComponents(Grades);
				for(const auto* Grade:Grades)if(Grade->GetFName()==FName(TEXT("PresentationGrade")))
				{
					auto Row=MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("component"),Grade->GetPathName());
					Row->SetBoolField(TEXT("registered"),Grade->IsRegistered());Row->SetBoolField(TEXT("actor_hidden"),ActorIt->IsHidden());
					Row->SetBoolField(TEXT("enabled"),Grade->bEnabled);Row->SetBoolField(TEXT("unbound"),Grade->bUnbound);
					Row->SetNumberField(TEXT("blend_weight"),Grade->BlendWeight);Row->SetNumberField(TEXT("priority"),Grade->Priority);
					Row->SetNumberField(TEXT("auto_exposure_method"),static_cast<int32>(Grade->Settings.AutoExposureMethod.GetValue()));
					Row->SetNumberField(TEXT("auto_exposure_bias"),Grade->Settings.AutoExposureBias);
					Row->SetBoolField(TEXT("override_method"),Grade->Settings.bOverride_AutoExposureMethod);
					Row->SetBoolField(TEXT("override_bias"),Grade->Settings.bOverride_AutoExposureBias);
					Row->SetBoolField(TEXT("apply_physical_camera_exposure"),Grade->Settings.AutoExposureApplyPhysicalCameraExposure);
					PresentationGrades.Add(MakeShared<FJsonValueObject>(Row));
				}
			}
			TInlineComponentArray<ULightComponentBase*> Lights;
			ActorIt->GetComponents(Lights);
			for(ULightComponentBase* Light:Lights)
			{
				auto* Sky=Cast<USkyLightComponent>(Light);
				auto* Sun=Cast<UDirectionalLightComponent>(Light);
				if(!Sky&&!Sun)continue;
				auto Row=MakeShared<FJsonObject>();
				const bool Enabled=Light->IsRegistered()&&Light->IsVisible()&&!ActorIt->IsHidden()&&Light->bAffectsWorld&&Light->Intensity>0.f;
				Row->SetStringField(TEXT("actor"),ActorIt->GetPathName());
				Row->SetStringField(TEXT("component"),Light->GetPathName());
				Row->SetBoolField(TEXT("actor_hidden"),ActorIt->IsHidden());
				Row->SetBoolField(TEXT("component_visible"),Light->IsVisible());
				Row->SetBoolField(TEXT("component_registered"),Light->IsRegistered());
				Row->SetBoolField(TEXT("affects_world"),Light->bAffectsWorld!=0);
				Row->SetBoolField(TEXT("component_enabled_observed"),Enabled);
				Row->SetNumberField(TEXT("mobility"),static_cast<int32>(Light->GetMobility()));
				Row->SetNumberField(TEXT("intensity"),Light->Intensity);
				Row->SetNumberField(TEXT("indirect_lighting_intensity"),Light->IndirectLightingIntensity);
				Row->SetNumberField(TEXT("volumetric_scattering_intensity"),Light->VolumetricScatteringIntensity);
				Row->SetBoolField(TEXT("casts_shadows"),Light->CastShadows!=0);
				const FLinearColor Color=Light->GetLightColor();
				SetRenderAuditVector(Row,TEXT("light_color_linear_rgb"),FVector(Color.R,Color.G,Color.B));
				if(Sun)
				{
					EnabledSuns+=Enabled?1:0;
					Row->SetNumberField(TEXT("light_units_enum"),static_cast<int32>(Sun->GetLightUnits()));
					Row->SetStringField(TEXT("directional_intensity_units"),TEXT("lux (UE directional API returns Unitless enum by design)"));
					Row->SetBoolField(TEXT("atmosphere_sun_light"),Sun->IsUsedAsAtmosphereSunLight());
					SetRenderAuditVector(Row,TEXT("forward_direction"),Sun->GetForwardVector());
					Suns.Add(MakeShared<FJsonValueObject>(Row));
				}
				if(Sky)
				{
					EnabledSkies+=Enabled?1:0;
					const bool Realtime=Sky->IsRealTimeCaptureEnabled();
					const FTexture* Processed=Sky->GetProcessedSkyTexture();
					const bool ResourceInitialized=Processed&&Processed->IsInitialized();
					const bool RHIHandlePresent=Processed&&Processed->TextureRHI.IsValid();
					const uint32 Width=Processed?Processed->GetSizeX():0;
					const uint32 Height=Processed?Processed->GetSizeY():0;
					const bool Ready=ResourceInitialized&&RHIHandlePresent&&Width>0&&Height>0;
					const FSHVectorRGB3 SH=Sky->GetIrradianceEnvironmentMap();
					bool SHFinite=true;double SumSquares=0.,SumAbs=0.;
					for(int32 Basis=0;Basis<FSHVector3::MaxSHBasis;++Basis)
					{
						for(float Value:{SH.R.V[Basis],SH.G.V[Basis],SH.B.V[Basis]})
						{SHFinite&=FMath::IsFinite(Value);if(FMath::IsFinite(Value)){SumSquares+=static_cast<double>(Value)*Value;SumAbs+=FMath::Abs(static_cast<double>(Value));}}
					}
					const bool SHNonzero=SHFinite&&SumAbs>1.e-8;
					const bool Review=Enabled&&!Realtime&&(!Ready||!SHFinite||!SHNonzero);
					NonRealtimeReview+=Review?1:0;
					Row->SetNumberField(TEXT("source_type_enum"),static_cast<int32>(Sky->SourceType.GetValue()));
					Row->SetStringField(TEXT("specified_cubemap"),Sky->Cubemap?Sky->Cubemap->GetPathName():FString());
					Row->SetBoolField(TEXT("real_time_capture_requested"),Sky->bRealTimeCapture);
					Row->SetBoolField(TEXT("real_time_capture_enabled"),Realtime);
					Row->SetNumberField(TEXT("requested_cubemap_resolution"),Sky->CubemapResolution);
					Row->SetBoolField(TEXT("processed_texture_present"),Processed!=nullptr);
					Row->SetBoolField(TEXT("processed_resource_initialized"),ResourceInitialized);
					Row->SetBoolField(TEXT("processed_rhi_handle_present"),RHIHandlePresent);
					Row->SetNumberField(TEXT("processed_width"),Width);
					Row->SetNumberField(TEXT("processed_height"),Height);
					Row->SetBoolField(TEXT("processed_resource_ready_observed"),Ready);
					Row->SetBoolField(TEXT("cpu_sh_finite"),SHFinite);
					Row->SetNumberField(TEXT("cpu_sh_coefficients_per_channel"),FSHVector3::MaxSHBasis);
					Row->SetNumberField(TEXT("cpu_sh_rgb_l2_magnitude"),SHFinite?FMath::Sqrt(SumSquares):0.);
					Row->SetNumberField(TEXT("cpu_sh_rgb_abs_sum"),SHFinite?SumAbs:0.);
					Row->SetBoolField(TEXT("cpu_sh_nonzero_observed"),SHNonzero);
					if(SHFinite)SetRenderAuditVector(Row,TEXT("cpu_sh_l0_rgb"),FVector(SH.R.V[0],SH.G.V[0],SH.B.V[0]));
					Row->SetBoolField(TEXT("non_realtime_capture_requires_diagnostic_review"),Review);
					Row->SetNumberField(TEXT("sky_distance_threshold_cm"),Sky->SkyDistanceThreshold);
					Row->SetBoolField(TEXT("capture_emissive_only"),Sky->bCaptureEmissiveOnly);
					Row->SetBoolField(TEXT("lower_hemisphere_is_black"),Sky->bLowerHemisphereIsBlack);
					SetRenderAuditVector(Row,TEXT("lower_hemisphere_linear_rgb"),FVector(Sky->LowerHemisphereColor.R,Sky->LowerHemisphereColor.G,Sky->LowerHemisphereColor.B));
					Row->SetNumberField(TEXT("occlusion_max_distance_cm"),Sky->OcclusionMaxDistance);
					Row->SetNumberField(TEXT("minimum_occlusion"),Sky->MinOcclusion);
					Skies.Add(MakeShared<FJsonValueObject>(Row));
				}
			}
		}
		Object->SetNumberField(TEXT("schema_version"),1);
		Object->SetBoolField(TEXT("render_thread_fence_completed"),true);
		Object->SetBoolField(TEXT("gpu_pixels_read"),false);
		Object->SetBoolField(TEXT("recapture_requested_by_this_readback"),false);
		Object->SetObjectField(TEXT("physical_sun_preview"),ReadPhysicalSunPreview(World));
        Object->SetBoolField(TEXT("earlier_ground_bounce_preview_flag_requested"),FParse::Param(FCommandLine::Get(),TEXT("AftershockGroundBouncePreview")));
        Object->SetStringField(TEXT("recapture_scope"),TEXT("This readback does not request capture. Earlier GroundBouncePreview may request one recapture; its actual applied/request count is reported by AFTERSHOCK_GROUND_BOUNCE_PREVIEW, not inferred here."));
		Object->SetBoolField(TEXT("shadowed_surface_illuminance_measured"),false);
		Object->SetNumberField(TEXT("enabled_directional_components"),EnabledSuns);
		Object->SetNumberField(TEXT("enabled_sky_components"),EnabledSkies);
		Object->SetNumberField(TEXT("non_realtime_sky_review_count"),NonRealtimeReview);
		Object->SetArrayField(TEXT("directional_lights"),Suns);
		Object->SetArrayField(TEXT("sky_lights"),Skies);
		Object->SetArrayField(TEXT("presentation_grades"),PresentationGrades);
		Object->SetStringField(TEXT("exposure_readback_scope"),TEXT("Actual owned volume inputs, not measured final blended camera/view exposure."));
		Object->SetStringField(TEXT("scope"),TEXT("Actual light component properties, processed resource handles, and fenced CPU SH coefficients. Nonzero SH is not calibrated lux or facade illumination. Real-time GPU capture may not populate this CPU SH/processed texture; its absence alone is not failure. No capture, exposure/intensity modification, gameplay writes or GPU pixel readback."));
		return Object;
	}

	TSharedRef<FJsonObject> ReadRenderAuditMetadata(UWorld& World,const FString& Stage)
	{
		auto Result=MakeShared<FJsonObject>();
		const auto* Terrain=World.GetSubsystem<UTerrainGridSubsystem>();
		const auto* Life=World.GetSubsystem<UCityLifeSubsystem>();
		const auto* Crowd=World.GetSubsystem<UCrowdRenderSubsystem>();
		const auto* Buildings=World.GetSubsystem<UBuildingManagerSubsystem>();
		auto* Controller=World.GetFirstPlayerController();
		const auto* Camera=Controller?Cast<AAftershockCameraPawn>(Controller->GetPawn()):nullptr;
		const auto Budget=GetAftershockPresentationBudget(&World);
		const auto GroundStats=GetAftershockTerrainSurfaceBuildStats(&World);
		const auto PlanetStats=GetAftershockRegionalMeshBuildStats(&World);
		const auto GeoStats=GetAftershockGeodataCacheStats();
		UStaticMeshComponent* Ground=nullptr;UInstancedStaticMeshComponent* Citizens=nullptr;
		for(TActorIterator<AAftershockCrowdHostActor> HostIt(&World);HostIt;++HostIt)
		{Ground=HostIt->TerrainSurface;Citizens=HostIt->CitizenISM;break;}
		const auto* GroundData=Ground&&Ground->GetStaticMesh()?Ground->GetStaticMesh()->GetRenderData():nullptr;
		const int32 GroundLOD=Ground?Ground->GetForcedLodModel():0;
		const bool HasGround=GroundData&&GroundData->LODResources.Num()>0;
		int32 Markers=0;for(TActorIterator<AAftershockCityPresentationActor> CityIt(&World);CityIt;++CityIt){Markers=CityIt->GetGeographicFocusMarkerCount();break;}
		bool ContextPresent=false,PlanetClipPresent=false,PlanetMasked=false,SourceMaskPresent=false,SourceMaskLinear=false;
		int32 VehicleNonzero=0,SourceMaskWidth=0,SourceMaskHeight=0,SourceMaskMips=0;float PlanetClip=-1.f,SourceMaskEnabled=0.f;
		FLinearColor PlanetClipMin=FLinearColor::Black,PlanetClipMax=FLinearColor::Black;
		TArray<TSharedPtr<FJsonValue>> Batches,ContextMaterialSlots,ContextSections,VolcanoPresentations,FloodPresentations;
		TSharedPtr<FJsonObject> PlanetMaterialSlot;
		int32 ContextTriangles=0,ContextSlotCount=0;FBox ContextWorldBounds(ForceInit),ContextLocalBounds(ForceInit);
		for(TActorIterator<AActor> ActorIt(&World);ActorIt;++ActorIt)
		{
			if(const auto* ActiveVolcano=Cast<AAftershockDisasterPresentationActor>(*ActorIt))
			{
				VolcanoPresentations.Add(MakeShared<FJsonValueObject>(ReadRenderVolcanoStats(*ActiveVolcano,ActiveVolcano->GetVolcanoVisualStats(),TEXT("active_event"))));
				auto Flood=MakeShared<FJsonObject>();const FString Description=ActiveVolcano->DescribeCurrentVisuals();uint64 ArrivalBytes=0;int32 PhysicalArrival=0;float Progress=0.f;
				const bool HasArrivalBytes=FParse::Value(*Description,TEXT("floodArrivalMapBytes="),ArrivalBytes);const bool HasPhysical=FParse::Value(*Description,TEXT("floodPhysicalArrival="),PhysicalArrival);FParse::Value(*Description,TEXT("floodArrivalProgress="),Progress);
				Flood->SetStringField(TEXT("actor"),ActiveVolcano->GetPathName());Flood->SetBoolField(TEXT("arrival_map_bytes_observed"),HasArrivalBytes);Flood->SetNumberField(TEXT("arrival_map_allocated_bytes"),static_cast<double>(ArrivalBytes));
				Flood->SetBoolField(TEXT("physical_arrival_observed"),HasPhysical);Flood->SetBoolField(TEXT("physical_arrival_enabled"),PhysicalArrival==1);Flood->SetNumberField(TEXT("arrival_progress"),Progress);
				TInlineComponentArray<UStaticMeshComponent*> FloodComponents;ActiveVolcano->GetComponents(FloodComponents);for(const UStaticMeshComponent* Surface:FloodComponents)if(Surface->GetFName()==FName(TEXT("CanonicalFloodSurfaceISM")))Flood->SetObjectField(TEXT("actual_material_slot0"),ReadRenderMaterialSlot(*Surface,0));
				FloodPresentations.Add(MakeShared<FJsonValueObject>(Flood));
			}
			if(const auto* PassiveVolcano=Cast<AAftershockEcologyStateActor>(*ActorIt))VolcanoPresentations.Add(MakeShared<FJsonValueObject>(ReadRenderVolcanoStats(*PassiveVolcano,PassiveVolcano->GetRegionalVolcanoVisualStats(),TEXT("persistent_terrain"))));
			TInlineComponentArray<UStaticMeshComponent*> Components;ActorIt->GetComponents(Components);
			for(const auto* Component:Components)
			{
				// V2 intentionally has no planet mesh in Street/City, but its
				// material defaults and disabled clip still belong to this real scene.
				if(Component->GetFName()==FName(TEXT("FictionalGlobe")))
				{
					if(const auto* PlanetMaterial=Component->GetMaterial(0))
					{
						PlanetMaterialSlot=ReadRenderMaterialSlot(*Component,0);
						PlanetMasked=PlanetMaterial->GetBlendMode()==BLEND_Masked;
						PlanetClipPresent=PlanetMaterial->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(TEXT("RegionalContextClipEnabled"))),PlanetClip);
						PlanetClipPresent&=PlanetMaterial->GetVectorParameterValue(FHashedMaterialParameterInfo(FName(TEXT("RegionalContextBoundsMin"))),PlanetClipMin);
						PlanetClipPresent&=PlanetMaterial->GetVectorParameterValue(FHashedMaterialParameterInfo(FName(TEXT("RegionalContextBoundsMax"))),PlanetClipMax);
					}
				}
				const UStaticMesh* Mesh=Component->GetStaticMesh();if(!Mesh)continue;
				const FStaticMeshRenderData* Data=Mesh->GetRenderData();
				const auto* Instances=Cast<UInstancedStaticMeshComponent>(Component);
				int32 Nonzero=0;if(Instances)for(int32 InstanceIndex=0;InstanceIndex<Instances->GetInstanceCount();++InstanceIndex)
				{FTransform Transform;if(Instances->GetInstanceTransform(InstanceIndex,Transform,true)&&!Transform.GetScale3D().IsNearlyZero())++Nonzero;}
				if(ActorIt->IsA<AAftershockTransportHost>()&&Component->IsVisible())VehicleNonzero+=Nonzero;
				if(Component->GetFName()==FName(TEXT("CityToGlobeLandscape")))
				{
					ContextPresent=Data&&Data->LODResources.Num()>0;
					ContextWorldBounds=Component->Bounds.GetBox();ContextLocalBounds=Mesh->GetBoundingBox();ContextSlotCount=Component->GetNumMaterials();ContextTriangles=ContextPresent?Data->LODResources[0].GetNumTriangles():0;
					for(int32 Slot=0;Slot<ContextSlotCount;++Slot)ContextMaterialSlots.Add(MakeShared<FJsonValueObject>(ReadRenderMaterialSlot(*Component,Slot)));
					if(ContextPresent)for(const FStaticMeshSection& Section:Data->LODResources[0].Sections)
					{auto ActualSection=MakeShared<FJsonObject>();ActualSection->SetNumberField(TEXT("material_index"),Section.MaterialIndex);ActualSection->SetNumberField(TEXT("first_index"),Section.FirstIndex);ActualSection->SetNumberField(TEXT("triangles"),Section.NumTriangles);ContextSections.Add(MakeShared<FJsonValueObject>(ActualSection));}
					if(const auto* Material=Component->GetMaterial(0))
					{
						UTexture* Texture=nullptr;
						const bool HasEnabled=Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(TEXT("RegionalSourceMaskEnabled"))),SourceMaskEnabled);
						const bool HasTexture=Material->GetTextureParameterValue(FHashedMaterialParameterInfo(FName(TEXT("RegionalSourceMask"))),Texture);
						if(const auto* Mask=Cast<UTexture2D>(Texture))
						{
							SourceMaskWidth=Mask->GetSizeX();SourceMaskHeight=Mask->GetSizeY();SourceMaskMips=Mask->GetNumMips();SourceMaskLinear=!Mask->SRGB;
							SourceMaskPresent=HasEnabled&&HasTexture&&SourceMaskEnabled>.5f&&SourceMaskWidth==257&&SourceMaskHeight==257&&SourceMaskMips==9&&SourceMaskLinear;
						}
					}
				}
				auto Batch=MakeShared<FJsonObject>();Batch->SetStringField(TEXT("component"),Component->GetPathName());
				Batch->SetStringField(TEXT("mesh"),Mesh->GetPathName());Batch->SetBoolField(TEXT("component_visible"),Component->IsVisible());
				Batch->SetNumberField(TEXT("forced_lod_model"),Component->GetForcedLodModel());
				Batch->SetNumberField(TEXT("source_instances"),Instances?Instances->GetInstanceCount():1);
				Batch->SetNumberField(TEXT("nonzero_transform_instances"),Instances?Nonzero:1);
				TArray<TSharedPtr<FJsonValue>> Triangles;if(Data)for(const auto& LOD:Data->LODResources)Triangles.Add(MakeShared<FJsonValueNumber>(static_cast<double>(LOD.GetNumTriangles())));
				Batch->SetArrayField(TEXT("triangles_per_render_lod"),Triangles);Batches.Add(MakeShared<FJsonValueObject>(Batch));
			}
		}
		uint32 Roster=2166136261u;if(Life)for(const auto& Resident:Life->GetResidents())
		{Roster=HashCombine(Roster,GetTypeHash(Resident.ResidentID));Roster=HashCombine(Roster,GetTypeHash(Resident.bAlive));}
		Result->SetNumberField(TEXT("schema_version"),1);Result->SetNumberField(TEXT("regional_surround_schema_version"),1);Result->SetStringField(TEXT("stage"),Stage);Result->SetBoolField(TEXT("metadata_valid"),Terrain&&Camera&&HasGround);
		const auto FramePacing=ReadFramePacingAudit(&World);
		Result->SetStringField(TEXT("capture_policy"),FramePacing.Policy());Result->SetBoolField(TEXT("frame_pacing_marker_applied"),FramePacing.MarkerApplied);
		Result->SetNumberField(TEXT("max_fps_engine"),FramePacing.EngineLimit);Result->SetNumberField(TEXT("frame_rate_limit_cvar_fps"),FramePacing.CVarLimit);
		Result->SetNumberField(TEXT("vsync_cvar"),FramePacing.VSync);Result->SetBoolField(TEXT("uncapped_ephemeral_audit_confirmed"),FramePacing.Confirmed);
		Result->SetStringField(TEXT("utc"),FDateTime::UtcNow().ToIso8601());Result->SetNumberField(TEXT("world_time_seconds"),World.GetTimeSeconds());
		Result->SetNumberField(TEXT("frame_counter"),static_cast<double>(GFrameCounter));
		Result->SetNumberField(TEXT("terrain_seed"),Terrain?Terrain->TerrainSeed:0);Result->SetNumberField(TEXT("cell_size_cm"),Terrain?Terrain->CellSize:0.f);
		Result->SetNumberField(TEXT("region_index"),Terrain?Terrain->GeographyIndex:INDEX_NONE);Result->SetNumberField(TEXT("focus_index"),Terrain?Terrain->GeographyFocusIndex:INDEX_NONE);
		if(Terrain)if(const auto* Anchor=TryGetAftershockGeographyFocus(Terrain->GeographyIndex,Terrain->GeographyFocusIndex))
		{Result->SetStringField(TEXT("region_id"),Anchor->Id);Result->SetStringField(TEXT("focus_id"),Anchor->FocusId);Result->SetStringField(TEXT("focus_label"),Anchor->FocusLabel);Result->SetNumberField(TEXT("anchor_latitude"),Anchor->Latitude);Result->SetNumberField(TEXT("anchor_longitude"),Anchor->Longitude);}
		Result->SetBoolField(TEXT("real_geography"),Terrain&&Terrain->IsUsingRealGeography());Result->SetNumberField(TEXT("continuation_version"),Terrain?Terrain->ContinuationGeneratorVersion:0);
		Result->SetNumberField(TEXT("tier"),static_cast<uint8>(Budget.Tier));Result->SetNumberField(TEXT("view_distance_cm"),Budget.ViewDistanceCm);
		Result->SetNumberField(TEXT("overview_blend"),Camera?Camera->GetPlanetOverviewBlend():-1.f);Result->SetNumberField(TEXT("geometry_stride"),Budget.GeometryStride);
		Result->SetBoolField(TEXT("budget_draw_citizens"),Budget.bDrawCitizens);Result->SetBoolField(TEXT("budget_draw_decorations"),Budget.bDrawDecorations);
		Result->SetBoolField(TEXT("citizen_component_visible"),Citizens&&Citizens->IsVisible());Result->SetNumberField(TEXT("vehicle_visible_component_nonzero_transforms"),VehicleNonzero);
		Result->SetNumberField(TEXT("visual_agents"),Crowd?Crowd->GetCurrentCrowdCount():0);Result->SetNumberField(TEXT("canonical_living_residents"),Life?Life->GetState().LivingPopulation:0);
		Result->SetStringField(TEXT("canonical_roster_hash"),FString::Printf(TEXT("%08x"),Roster));Result->SetNumberField(TEXT("canonical_buildings"),Buildings?Buildings->GetActiveBuildingsCount():0);
		Result->SetNumberField(TEXT("ground_forced_lod_model"),GroundLOD);Result->SetNumberField(TEXT("ground_lod0_triangles"),HasGround?GroundData->LODResources[0].GetNumTriangles():0);
		Result->SetNumberField(TEXT("ground_forced_lod_triangles"),HasGround&&GroundLOD>0?static_cast<double>(GroundData->LODResources[FMath::Clamp(GroundLOD-1,0,GroundData->LODResources.Num()-1)].GetNumTriangles()):-1.);
		Result->SetBoolField(TEXT("automatic_lod_actual_selection_unknown"),GroundLOD==0);Result->SetBoolField(TEXT("source_context_mesh_present"),ContextPresent);
		Result->SetBoolField(TEXT("source_surface_mask_present"),SourceMaskPresent);Result->SetBoolField(TEXT("source_surface_mask_linear"),SourceMaskLinear);
		Result->SetNumberField(TEXT("source_surface_mask_enabled"),SourceMaskEnabled);Result->SetNumberField(TEXT("source_surface_mask_width"),SourceMaskWidth);
		Result->SetNumberField(TEXT("source_surface_mask_height"),SourceMaskHeight);Result->SetNumberField(TEXT("source_surface_mask_mips"),SourceMaskMips);
		Result->SetNumberField(TEXT("source_context_material_slot_count"),ContextSlotCount);Result->SetNumberField(TEXT("source_context_actual_lod0_triangles"),ContextTriangles);
		Result->SetArrayField(TEXT("source_context_material_slots"),ContextMaterialSlots);Result->SetArrayField(TEXT("source_context_lod0_sections"),ContextSections);
		if(ContextPresent){SetRenderAuditVector(Result,TEXT("source_context_world_bounds_min_cm"),ContextWorldBounds.Min);SetRenderAuditVector(Result,TEXT("source_context_world_bounds_max_cm"),ContextWorldBounds.Max);SetRenderAuditVector(Result,TEXT("source_context_local_bounds_min_cm"),ContextLocalBounds.Min);SetRenderAuditVector(Result,TEXT("source_context_local_bounds_max_cm"),ContextLocalBounds.Max);}
		if(Ground)Result->SetObjectField(TEXT("ground_material_slot0"),ReadRenderMaterialSlot(*Ground,0));
		if(PlanetMaterialSlot.IsValid())Result->SetObjectField(TEXT("planet_material_slot0"),PlanetMaterialSlot.ToSharedRef());
		Result->SetArrayField(TEXT("volcano_presentations"),VolcanoPresentations);Result->SetArrayField(TEXT("flood_presentations"),FloodPresentations);
		Result->SetBoolField(TEXT("planet_clip_parameter_present"),PlanetClipPresent);Result->SetBoolField(TEXT("planet_material_masked"),PlanetMasked);
		Result->SetNumberField(TEXT("planet_regional_clip_enabled"),PlanetClip);
		TArray<TSharedPtr<FJsonValue>> ClipBounds;for(double Value:{static_cast<double>(PlanetClipMin.R),static_cast<double>(PlanetClipMin.G),static_cast<double>(PlanetClipMax.R),static_cast<double>(PlanetClipMax.G)})ClipBounds.Add(MakeShared<FJsonValueNumber>(Value));
		Result->SetArrayField(TEXT("planet_clip_world_xy_bounds"),ClipBounds);
		Result->SetNumberField(TEXT("geographic_focus_markers"),Markers);Result->SetNumberField(TEXT("ground_builds"),static_cast<double>(GroundStats.SuccessfulGroundBuilds));
		Result->SetNumberField(TEXT("ground_collision_cooks"),static_cast<double>(GroundStats.CollisionCookAttempts));Result->SetNumberField(TEXT("ground_cache_hits"),static_cast<double>(GroundStats.GroundCacheHits));
		Result->SetNumberField(TEXT("ground_cached_components"),GroundStats.CachedGroundComponents);Result->SetNumberField(TEXT("ground_live_component_meshes"),GroundStats.ActiveGroundComponents);
		Result->SetNumberField(TEXT("planet_builds"),PlanetStats.PlanetBuilds);Result->SetNumberField(TEXT("regional_context_builds"),PlanetStats.ContextBuilds);
		Result->SetNumberField(TEXT("regional_live_weak_mesh_references"),PlanetStats.ActiveMeshReferences);
		Result->SetNumberField(TEXT("normal_route_cache_entries"),Crowd?Crowd->GetNormalRouteCacheEntryCount():0);
		Result->SetNumberField(TEXT("normal_route_cache_allocated_bytes"),Crowd?static_cast<double>(Crowd->GetNormalRouteCacheAllocatedBytes()):0.);
		Result->SetNumberField(TEXT("geodata_ready_tiles"),GeoStats.ReadyTiles);Result->SetNumberField(TEXT("geodata_decoded_allocated_bytes"),static_cast<double>(GeoStats.DecodedAllocatedBytes));
		Result->SetNumberField(TEXT("geodata_compiled_compressed_bytes"),static_cast<double>(GeoStats.CompiledCompressedBytes));
		Result->SetNumberField(TEXT("geodata_detail_ready_tiles"),GeoStats.DetailReadyTiles);Result->SetNumberField(TEXT("geodata_detail_decoded_allocated_bytes"),static_cast<double>(GeoStats.DetailDecodedAllocatedBytes));Result->SetNumberField(TEXT("geodata_detail_compiled_compressed_bytes"),static_cast<double>(GeoStats.DetailCompiledCompressedBytes));
		Result->SetNumberField(TEXT("geodata_overview_resident_tiles"),GeoStats.OverviewResidentTiles);Result->SetNumberField(TEXT("geodata_overview_decoded_allocated_bytes"),static_cast<double>(GeoStats.OverviewDecodedAllocatedBytes));Result->SetNumberField(TEXT("geodata_overview_compiled_compressed_bytes"),static_cast<double>(GeoStats.OverviewCompiledCompressedBytes));
		int32 ProfileCount=0;Result->SetBoolField(TEXT("synthetic_render_profile"),FParse::Value(FCommandLine::Get(),TEXT("AftershockProfileAgents="),ProfileCount));
		Result->SetArrayField(TEXT("render_components"),Batches);
		Result->SetObjectField(TEXT("graphics_quality"),ReadGraphicsQualityAudit(World));
		Result->SetObjectField(TEXT("architecture_cpd_audit"),ReadArchitectureCPDAudit(World));
		Result->SetObjectField(TEXT("lighting_audit"),ReadLightingAudit(World));
		Result->SetStringField(TEXT("scope"),TEXT("Actual scene components and forced LOD metadata; instance transforms are not GPU visibility or occlusion proof. No measured VRAM or RAM saving claim."));
		return Result;
	}

	// Separate opt-in V3 acceptance fixture. It observes the ordinary event/timer
	// path; it never edits DEM, drainage, water maps or the population to force a picture.
	uint32 PhysicalFloodTerrainGeometryHash(const UTerrainGridSubsystem& Terrain)
	{
		uint32 Hash=HashCombine(GetTypeHash(Terrain.CellSize),GetTypeHash(Terrain.GridOrigin));
		for(const FGridCell& Cell:Terrain.Cells)
		{
			Hash=HashCombine(Hash,GetTypeHash(Cell.Coords));Hash=HashCombine(Hash,GetTypeHash(Cell.Elevation));
			Hash=HashCombine(Hash,GetTypeHash(Cell.RealElevationMetres));Hash=HashCombine(Hash,GetTypeHash(Cell.bIsPlayable));
			Hash=HashCombine(Hash,GetTypeHash(Cell.bIsRiverbed));Hash=HashCombine(Hash,GetTypeHash(Cell.bIsOcean));
		}
		return Hash;
	}
	uint32 PhysicalFloodSnapshotHash(const FAftershockDisasterVisualSnapshot& Snapshot,const UTerrainGridSubsystem& Terrain)
	{
		uint32 Hash=HashCombine(GetTypeHash(Snapshot.Revision),GetTypeHash(Snapshot.FloodModelVersion));
		for(const FGridCell& Cell:Terrain.Cells)for(const auto* Map:{&Snapshot.WaterDepthCm,&Snapshot.WaterSurfaceElevationCm,&Snapshot.WaterArrivalFraction})
		{const float* Value=Map->Find(Cell.Coords);Hash=HashCombine(Hash,GetTypeHash(Value!=nullptr));if(Value)Hash=HashCombine(Hash,GetTypeHash(*Value));}
		return Hash;
	}
	struct FPhysicalFloodRenderAuditState
	{
		bool Valid=true,Triggered=false;uint32 GeometryHash=0,SnapshotHash=0;int32 CellCount=0,Revision=0;
		FIntPoint FocusCoord=FIntPoint::ZeroValue;
		FAftershockFloodBudgetStats Budget;TArray<TSharedPtr<FJsonValue>> Frames;
	};
	void BeginPhysicalFloodRenderAudit(UWorld& World)
	{
		const TWeakObjectPtr<UWorld> WeakWorld(&World);auto State=MakeShared<FPhysicalFloodRenderAuditState>();
		FTimerHandle EventTimer;World.GetTimerManager().SetTimer(EventTimer,FTimerDelegate::CreateLambda([WeakWorld,State]()
		{
			auto* Active=WeakWorld.Get();if(!IsActiveAuditWorld(Active))return;
			auto* Terrain=Active->GetSubsystem<UTerrainGridSubsystem>();auto* Simulation=Active->GetSubsystem<UDisasterSimulationSubsystem>();
			if(!Terrain||!Simulation||Terrain->ContinuationGeneratorVersion!=3||Terrain->GeographyIndex!=1||Terrain->GeographyFocusIndex!=0)
			{State->Valid=false;UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_PHYSICAL_FLOOD_RHI_PRECONDITION_FAIL requiresSourceVersion=3 region=1 focus=0 canonicalDataUnchanged=1"));return;}
			State->GeometryHash=PhysicalFloodTerrainGeometryHash(*Terrain);State->CellCount=Terrain->Cells.Num();
			const FDisasterReport EventReport=Simulation->TriggerDisaster(EDisasterType::Flood,70.f,.35f,true);
			const auto& Snapshot=Simulation->GetLatestVisualSnapshot();State->Budget=Simulation->GetLastPhysicalFloodBudget();
			State->Triggered=!EventReport.bWasRejected&&Snapshot.Type==EDisasterType::Flood&&Snapshot.FloodModelVersion==1;
			State->Revision=Snapshot.Revision;State->SnapshotHash=PhysicalFloodSnapshotHash(Snapshot,*Terrain);State->Valid&=State->Triggered;
			// Observe the wettest real non-river cell already inside installed ground.
			// Camera-only framing cannot manufacture ponding or expand canonical cells.
			const FIntRect Drawable=GetAftershockDrawableCellBounds(*Terrain);float Best=-1.f;FIntPoint BestCoord=FIntPoint::ZeroValue;
			for(const FGridCell& Cell:Terrain->Cells)if(Drawable.Contains(Cell.Coords)&&Cell.bIsPlayable&&!Cell.bIsOcean&&!Cell.bIsRiverbed)
				if(const float* Head=Snapshot.WaterSurfaceElevationCm.Find(Cell.Coords);Head&&*Head-Cell.Elevation>Best){Best=*Head-Cell.Elevation;BestCoord=Cell.Coords;}
			auto* Controller=Active->GetFirstPlayerController();auto* Camera=Controller?Cast<AAftershockCameraPawn>(Controller->GetPawn()):nullptr;
			if(Camera&&Best>1.f)
			{
				State->FocusCoord=BestCoord;
				FAftershockCameraSaveState View;Camera->CaptureSaveState(View);View.LocalFocus=Terrain->GridToWorld(BestCoord);View.Location=View.LocalFocus;View.OverviewBlend=0.f;
				FText Error;State->Valid&=Camera->RestoreSaveState(View,Error)&&SetRenderAuditZoom(*Active,30000.);
			}
			else State->Valid=false;
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_FLOOD_RHI_EVENT accepted=%d sourceVersion=%d model=%d revision=%d rainfallMm=%.3f eventPulseSeconds=%.1f bestRealLandDepthCm=%.3f geometryUnchanged=%d terrainEditsByFixture=0"),State->Triggered,Terrain->ContinuationGeneratorVersion,Snapshot.FloodModelVersion,Snapshot.Revision,Snapshot.RainfallPulseMm,Snapshot.FloodPulseDurationSeconds,Best,State->GeometryHash==PhysicalFloodTerrainGeometryHash(*Terrain));
			const float CaptureAges[]={.25f,10.f,14.4f};
			for(int32 FrameIndex=0;FrameIndex<3;++FrameIndex)
			{
				FTimerHandle CaptureTimer;Active->GetTimerManager().SetTimer(CaptureTimer,FTimerDelegate::CreateLambda([WeakWorld,State,FrameIndex]()
				{
					auto* TestWorld=WeakWorld.Get();if(!IsActiveAuditWorld(TestWorld))return;
					const auto* Grid=TestWorld->GetSubsystem<UTerrainGridSubsystem>();const auto* Sim=TestWorld->GetSubsystem<UDisasterSimulationSubsystem>();
					const auto* Presentation=TestWorld->GetSubsystem<UDisasterPresentationSubsystem>();auto* PC=TestWorld->GetFirstPlayerController();
					if(!Grid||!Sim||!Presentation||!PC||!PC->PlayerCameraManager){State->Valid=false;return;}
					const auto& Current=Sim->GetLatestVisualSnapshot();const TCHAR* Stages[]={TEXT("arrival-start"),TEXT("arrival-intermediate"),TEXT("fully-arrived-before-drain")};
					const FString Stage=Stages[FrameIndex],Stem=TEXT("physical-flood-")+Stage;
					const FString Path=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation")/(Stem+TEXT("-scene.png")));
					const bool Saved=CaptureScene(*TestWorld,PC->PlayerCameraManager->GetCameraCacheView(),Path,1600,900);
					auto Frame=ReadRenderAuditMetadata(*TestWorld,Stage);Frame->SetBoolField(TEXT("scene_saved"),Saved);
					const float Age=TestWorld->GetTimeSeconds()-Current.StartedAtWorldSeconds;
					Frame->SetNumberField(TEXT("event_age_seconds"),Age);Frame->SetNumberField(TEXT("event_duration_seconds"),Current.DurationSeconds);
					Frame->SetNumberField(TEXT("snapshot_revision"),Current.Revision);Frame->SetNumberField(TEXT("flood_model_version"),Current.FloodModelVersion);
					Frame->SetNumberField(TEXT("rainfall_pulse_mm"),Current.RainfallPulseMm);Frame->SetNumberField(TEXT("physical_pulse_duration_seconds"),Current.FloodPulseDurationSeconds);
					const bool GeometryUnchanged=Grid->Cells.Num()==State->CellCount&&PhysicalFloodTerrainGeometryHash(*Grid)==State->GeometryHash;
					const bool SnapshotUnchanged=Current.Revision==State->Revision&&PhysicalFloodSnapshotHash(Current,*Grid)==State->SnapshotHash;
					Frame->SetBoolField(TEXT("source_geometry_unchanged"),GeometryUnchanged);Frame->SetBoolField(TEXT("canonical_water_snapshot_unchanged"),SnapshotUnchanged);
					Frame->SetStringField(TEXT("geometry_hash"),FString::Printf(TEXT("%08x"),PhysicalFloodTerrainGeometryHash(*Grid)));
					Frame->SetStringField(TEXT("water_snapshot_hash"),FString::Printf(TEXT("%08x"),PhysicalFloodSnapshotHash(Current,*Grid)));
					Frame->SetStringField(TEXT("immutability_scope"),TEXT("Source coordinates/elevations/playable/river/ocean only. Canonical flood depth, fill, damage, casualties and economy may legitimately change at event commit."));
					UStaticMeshComponent* Surface=nullptr;for(TActorIterator<AAftershockDisasterPresentationActor> It(TestWorld);It;++It)
					{TInlineComponentArray<UStaticMeshComponent*> Components;It->GetComponents(Components);for(auto* Component:Components)if(Component->GetFName()==FName(TEXT("CanonicalFloodSurfaceISM")))Surface=Component;}
					const UMaterialInterface* Material=Surface?Surface->GetMaterial(0):nullptr;float Encoding=0.f,ArrivalEnabled=0.f,PhysicalData=0.f,Progress=-1.f,Envelope=-1.f;
					const bool Parameters=Material&&Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(TEXT("FloodEncodingCm"))),Encoding)
						&&Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(TEXT("FloodPhysicalArrivalEnabled"))),ArrivalEnabled)
						&&Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(TEXT("FloodPhysicalDataEnabled"))),PhysicalData)
						&&Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(TEXT("FloodArrivalProgress"))),Progress)
						&&Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(TEXT("FloodEnvelope"))),Envelope);
					const UStaticMesh* Mesh=Surface?Surface->GetStaticMesh():nullptr;const FStaticMeshRenderData* Data=Mesh?Mesh->GetRenderData():nullptr;
					const int32 Triangles=Data&&!Data->LODResources.IsEmpty()?Data->LODResources[0].GetNumTriangles():0;
					const int32 UVChannels=Data&&!Data->LODResources.IsEmpty()?Data->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords():0;
					const bool FullPrecisionUVs=Data&&!Data->LODResources.IsEmpty()&&Data->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetUseFullPrecisionUVs();
					const auto* FloodInstances=Cast<UInstancedStaticMeshComponent>(Surface);FTransform FloodTransform;
					const bool ActualFloodInstance=FloodInstances&&FloodInstances->GetInstanceCount()==1&&FloodInstances->GetInstanceTransform(0,FloodTransform,true)&&!FloodTransform.GetScale3D().IsNearlyZero();
					Frame->SetBoolField(TEXT("actual_flood_component_visible"),Surface&&Surface->IsVisible());Frame->SetNumberField(TEXT("actual_flood_lod0_triangles"),Triangles);
					Frame->SetBoolField(TEXT("actual_flood_single_nonzero_instance"),ActualFloodInstance);
					Frame->SetNumberField(TEXT("actual_flood_uv_channels"),UVChannels);Frame->SetBoolField(TEXT("actual_flood_full_precision_uvs"),FullPrecisionUVs);
					FVector2D FocusPixel=FVector2D::ZeroVector;const bool FocusProjected=UGameplayStatics::ProjectWorldToScreen(PC,Grid->GridToWorld(State->FocusCoord),FocusPixel,false);
					const bool FocusInFrame=FocusProjected&&FocusPixel.X>=0.&&FocusPixel.X<=1600.&&FocusPixel.Y>=0.&&FocusPixel.Y<=900.;
					Frame->SetNumberField(TEXT("focus_cell_x"),State->FocusCoord.X);Frame->SetNumberField(TEXT("focus_cell_y"),State->FocusCoord.Y);Frame->SetBoolField(TEXT("real_wet_land_focus_in_frame"),FocusInFrame);Frame->SetNumberField(TEXT("focus_pixel_x"),FocusPixel.X);Frame->SetNumberField(TEXT("focus_pixel_y"),FocusPixel.Y);Frame->SetNumberField(TEXT("focus_cpu_displayed_cm"),Presentation->GetDisplayedFloodDepthCm(State->FocusCoord));
					Frame->SetBoolField(TEXT("actual_material_parameters_read"),Parameters);if(Surface)Frame->SetObjectField(TEXT("actual_flood_material"),ReadRenderMaterialSlot(*Surface,0));
					Frame->SetNumberField(TEXT("depth_map_count"),Current.WaterDepthCm.Num());Frame->SetNumberField(TEXT("head_map_count"),Current.WaterSurfaceElevationCm.Num());Frame->SetNumberField(TEXT("arrival_map_count"),Current.WaterArrivalFraction.Num());
					Frame->SetNumberField(TEXT("water_map_allocated_bytes"),static_cast<double>(Current.WaterDepthCm.GetAllocatedSize()+Current.WaterSurfaceElevationCm.GetAllocatedSize()+Current.WaterArrivalFraction.GetAllocatedSize()));
					bool Paired=!Current.WaterDepthCm.IsEmpty()&&Current.WaterDepthCm.Num()==Current.WaterSurfaceElevationCm.Num()&&Current.WaterDepthCm.Num()==Current.WaterArrivalFraction.Num(),CpuMatches=true;
					int32 PositiveLand=0,DisplayedLand=0,DrawableDisplayedLand=0;double MaxColumn=0.,MaxDisplayed=0.,HeadMin=DBL_MAX,HeadMax=-DBL_MAX;
					TArray<TSharedPtr<FJsonValue>> Samples;const FIntRect Draw=GetAftershockDrawableCellBounds(*Grid);
					for(const FGridCell& Cell:Grid->Cells)if(const float* Depth=Current.WaterDepthCm.Find(Cell.Coords))
					{
						const float* Head=Current.WaterSurfaceElevationCm.Find(Cell.Coords);const float* Arrival=Current.WaterArrivalFraction.Find(Cell.Coords);
						if(!Head||!Arrival||!FMath::IsFinite(*Depth)||!FMath::IsFinite(*Head)||!FMath::IsFinite(*Arrival)||*Depth<0.f||*Arrival<0.f||*Arrival>1.f){Paired=false;continue;}
						const float Bed=Cell.Elevation-(Cell.bIsRiverbed?Cell.RiverCapacity*100.f:0.f);Paired&=FMath::IsNearlyEqual(*Head,Bed+*Depth,.05f);
						const float Peak=FMath::Max(0.f,*Head-Cell.Elevation),Actual=Presentation->GetDisplayedFloodDepthCm(Cell.Coords);
						const bool Meaningful=Cell.bIsPlayable&&Peak>=(Cell.bIsRiverbed?3.f:1.f);
						const float Weight=Progress>=1.f?1.f:FMath::SmoothStep(FMath::Max(0.f,*Arrival-.05f),FMath::Min(1.f,*Arrival+.05f),Progress);
						const float Expected=Meaningful?FMath::Max(0.f,(Peak+4.f)*Envelope*Weight-4.f):0.f;
						CpuMatches&=FMath::IsFinite(Actual)&&FMath::IsNearlyEqual(Actual,Expected,.05f);MaxColumn=FMath::Max(MaxColumn,static_cast<double>(Peak));MaxDisplayed=FMath::Max(MaxDisplayed,static_cast<double>(Actual));HeadMin=FMath::Min(HeadMin,static_cast<double>(*Head));HeadMax=FMath::Max(HeadMax,static_cast<double>(*Head));
						if(!Cell.bIsRiverbed&&!Cell.bIsOcean&&Peak>=1.f){++PositiveLand;if(Actual>0.f){++DisplayedLand;if(Draw.Contains(Cell.Coords))++DrawableDisplayedLand;}}
						if(Samples.Num()<32&&Draw.Contains(Cell.Coords)&&!Cell.bIsRiverbed&&Peak>=1.f)
						{auto Sample=MakeShared<FJsonObject>();Sample->SetNumberField(TEXT("x"),Cell.Coords.X);Sample->SetNumberField(TEXT("y"),Cell.Coords.Y);Sample->SetNumberField(TEXT("ground_cm"),Cell.Elevation);Sample->SetNumberField(TEXT("bed_column_cm"),*Depth);Sample->SetNumberField(TEXT("absolute_head_cm"),*Head);Sample->SetNumberField(TEXT("arrival_fraction"),*Arrival);Sample->SetNumberField(TEXT("above_ground_peak_cm"),Peak);Sample->SetNumberField(TEXT("cpu_displayed_cm"),Actual);Sample->SetNumberField(TEXT("expected_from_actual_material_cm"),Expected);Samples.Add(MakeShared<FJsonValueObject>(Sample));}
					}
					Frame->SetBoolField(TEXT("water_maps_paired_finite_bed_head_consistent"),Paired);Frame->SetBoolField(TEXT("cpu_depth_matches_actual_material_timeline"),CpuMatches);
					Frame->SetNumberField(TEXT("canonical_above_ground_land_cells"),PositiveLand);Frame->SetNumberField(TEXT("cpu_displayed_land_cells"),DisplayedLand);Frame->SetNumberField(TEXT("drawable_displayed_land_cells"),DrawableDisplayedLand);
					Frame->SetNumberField(TEXT("maximum_above_ground_peak_cm"),MaxColumn);Frame->SetNumberField(TEXT("maximum_cpu_displayed_cm"),MaxDisplayed);
					if(HeadMin<=HeadMax){Frame->SetNumberField(TEXT("minimum_absolute_head_cm"),HeadMin);Frame->SetNumberField(TEXT("maximum_absolute_head_cm"),HeadMax);}Frame->SetArrayField(TEXT("bounded_actual_land_samples"),Samples);
					const auto& B=State->Budget;auto Budget=MakeShared<FJsonObject>();bool FiniteBudget=true;
					const TCHAR* Names[]={TEXT("rainfall_m3"),TEXT("initial_river_storage_m3"),TEXT("infiltration_m3"),TEXT("drainage_m3"),TEXT("outlet_m3"),TEXT("dam_retained_m3"),TEXT("final_storage_m3"),TEXT("mass_residual_m3")};
					const double Values[]={B.RainfallM3,B.InitialRiverStorageM3,B.InfiltrationM3,B.DrainageM3,B.OutletM3,B.DamRetainedM3,B.FinalStorageM3,B.MassResidualM3};
					for(int32 I=0;I<UE_ARRAY_COUNT(Names);++I){FiniteBudget&=FMath::IsFinite(Values[I]);if(FMath::IsFinite(Values[I]))Budget->SetNumberField(Names[I],Values[I]);}
					const double Recomputed=B.InitialRiverStorageM3+B.RainfallM3-B.InfiltrationM3-B.DrainageM3-B.OutletM3-B.DamRetainedM3-B.FinalStorageM3;
					const double Tolerance=FMath::Max(1.e-5,(B.InitialRiverStorageM3+B.RainfallM3)*1.e-7);
					const bool MassConserved=FiniteBudget&&FMath::Abs(B.MassResidualM3)<=Tolerance&&FMath::Abs(Recomputed-B.MassResidualM3)<=Tolerance;
					Budget->SetNumberField(TEXT("active_cells"),B.ActiveCells);Budget->SetNumberField(TEXT("iterations"),B.Iterations);Budget->SetNumberField(TEXT("edge_visits"),static_cast<double>(B.EdgeVisits));Budget->SetBoolField(TEXT("committed_storage"),B.bCommittedStorage);Budget->SetNumberField(TEXT("tolerance_m3"),Tolerance);Budget->SetBoolField(TEXT("mass_conserved"),MassConserved);Frame->SetObjectField(TEXT("canonical_event_mass_budget"),Budget);
					bool StageValid=FrameIndex==0?Progress==0.f&&Envelope==0.f&&MaxDisplayed==0.:FrameIndex==1?Progress>0.f&&Progress<1.f&&Envelope>.99f&&DrawableDisplayedLand>0:Progress==1.f&&Envelope>.99f&&DrawableDisplayedLand>0&&Age<Current.DurationSeconds-8.f;
					const bool Valid=Saved&&Frame->GetBoolField(TEXT("metadata_valid"))&&GeometryUnchanged&&SnapshotUnchanged&&Current.FloodModelVersion==1&&Paired&&CpuMatches&&MassConserved&&B.bCommittedStorage&&B.ActiveCells>0&&Triangles>0&&ActualFloodInstance&&UVChannels>=3&&FullPrecisionUVs&&Parameters&&Encoding>=512.f&&ArrivalEnabled==1.f&&PhysicalData==1.f&&PositiveLand>0&&FocusInFrame&&StageValid;
					Frame->SetBoolField(TEXT("valid"),Valid);State->Valid&=Valid;State->Frames.Add(MakeShared<FJsonValueObject>(Frame));
					UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_FLOOD_RHI_FRAME stage=%s valid=%d age=%.3f model=%d arrival=%.3f envelope=%.3f actualTriangles=%d canonicalLandCells=%d drawnLandCells=%d cpuTimelineMatches=%d immutableGeometry=%d immutableSnapshot=%d massConserved=%d sceneSaved=%d"),*Stage,Valid,Age,Current.FloodModelVersion,Progress,Envelope,Triangles,PositiveLand,DrawableDisplayedLand,CpuMatches,GeometryUnchanged,SnapshotUnchanged,MassConserved,Saved);
					if(FrameIndex==2)
					{
						auto Report=MakeShared<FJsonObject>();const bool Complete=State->Triggered&&State->Valid&&State->Frames.Num()==3;
						Report->SetNumberField(TEXT("schema_version"),1);Report->SetBoolField(TEXT("valid"),Complete);Report->SetNumberField(TEXT("region_index"),1);Report->SetNumberField(TEXT("focus_index"),0);Report->SetNumberField(TEXT("source_version"),3);Report->SetNumberField(TEXT("flood_model_version"),1);Report->SetBoolField(TEXT("terrain_or_water_edits_by_fixture"),false);Report->SetArrayField(TEXT("frames"),State->Frames);
						Report->SetStringField(TEXT("scope"),TEXT("Real production V3 source/event and normal presentation timers. Canonical mass budget is independent of rendered water. PNG/geometry/timeline evidence does not prove CFD calibration or zero flicker."));
						const bool JsonSaved=SaveRenderAuditJson(Report,TEXT("physical_flood_runtime_audit.json"));
						UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_FLOOD_RHI_RESULT valid=%d frames=%d jsonSaved=%d sourceVersion=3 model=1"),Complete&&JsonSaved,State->Frames.Num(),JsonSaved);
					}
				}),CaptureAges[FrameIndex],false);
			}
		}),9.f,false);
	}

	struct FGeographyViewAuditState
	{
		TArray<TSharedPtr<FJsonValue>> Snapshots;
		TArray<TSharedPtr<FJsonValue>> TemporalSnapshots;
		TArray<TSharedPtr<FJsonValue>> RegionalSnapshots;
		FBox RegionalROI=FBox(ForceInit);
		bool bRegionalRequested=false,bRegionalValid=true,bRegionalMeshCoversROI=false;
		double RegionalTargetZoomCm=0.;
		FMinimalViewInfo TemporalCamera;
		bool bTemporalRequested=false,bHasTemporalCamera=false,bTemporalValid=true;
		uint64 TemporalFirstFrame=0;double TemporalFirstWorldSeconds=0.;
		bool Valid=true,SameRoster=true,SameGroundWork=true;
		FTimerHandle ReadinessTimer;double ReadinessStartedAt=0.,ReadinessWaitSeconds=0.;int32 ReadinessPolls=0;
		bool bReadinessRequired=false,bReadinessPassed=false,bReadinessTimedOut=false,bTimelineScheduled=false;
		FString Roster;double Population=0.,VisualAgents=0.,GroundBuilds=0.,GroundCooks=0.;
	};
	bool AreGeographySurroundMaterialsReady(UWorld& World,bool& Required)
	{
		int32 ExpectedVersion=3,ExpectedRegion=0,ExpectedFocus=0;
		FParse::Value(FCommandLine::Get(),TEXT("AftershockAuditSourceVersion="),ExpectedVersion);
		const bool ExplicitLegacy=FParse::Param(FCommandLine::Get(),TEXT("AftershockLegacyGeographyAudit"));
		Required=!ExplicitLegacy&&ExpectedVersion!=2;
		const auto* Terrain=World.GetSubsystem<UTerrainGridSubsystem>();
		if(!Terrain||Terrain->GridWidth<=0||Terrain->GridHeight<=0||Terrain->Cells.IsEmpty()||!FMath::IsFinite(Terrain->CellSize)||Terrain->CellSize<=0.f)return false;
		if(ExplicitLegacy||ExpectedVersion==2)return true;
		if(ExpectedVersion!=3||Terrain->ContinuationGeneratorVersion!=3)return false;
		const bool HasExpectedRegion=FParse::Value(FCommandLine::Get(),TEXT("AftershockAuditRegion="),ExpectedRegion);
		const bool HasExpectedFocus=FParse::Value(FCommandLine::Get(),TEXT("AftershockAuditFocus="),ExpectedFocus);
		if((HasExpectedRegion&&Terrain->GeographyIndex!=ExpectedRegion)||(HasExpectedFocus&&Terrain->GeographyFocusIndex!=ExpectedFocus))return false;
		const auto* Profile=TryGetAftershockRegionEnvironmentProfile(Terrain->GeographyIndex);if(!Profile||!Profile->RegionId||!*Profile->RegionId)return false;
		const FString Package=FString(TEXT("/Game/Environment/Textures/Regional/Surround/T_RegionalSurround_"))+Profile->RegionId;
		const FString ExpectedPath=Package+TEXT(".")+FString(TEXT("T_RegionalSurround_"))+Profile->RegionId;
		const UMaterialInterface* Materials[4]={nullptr,nullptr,nullptr,nullptr};
		for(TActorIterator<AAftershockCityPresentationActor> CityIt(&World);CityIt;++CityIt)
		{
			TInlineComponentArray<UStaticMeshComponent*> Components;CityIt->GetComponents(Components);
			for(const UStaticMeshComponent* Surface:Components)
			{
				if(Surface->GetFName()==FName(TEXT("FictionalGlobe")))Materials[0]=Surface->GetMaterial(0);
				if(Surface->GetFName()==FName(TEXT("CityToGlobeLandscape"))&&Surface->GetNumMaterials()>=2){Materials[2]=Surface->GetMaterial(0);Materials[3]=Surface->GetMaterial(1);}
			}
			break;
		}
		for(TActorIterator<AAftershockCrowdHostActor> CrowdIt(&World);CrowdIt;++CrowdIt){if(CrowdIt->TerrainSurface)Materials[1]=CrowdIt->TerrainSurface->GetMaterial(0);break;}
		FLinearColor SharedMin=FLinearColor::Black,SharedMax=FLinearColor::Black;
		for(int32 MaterialIndex=0;MaterialIndex<4;++MaterialIndex)
		{
			const UMaterialInterface* Material=Materials[MaterialIndex];if(!Material)return false;
			float Ready=0.f;UTexture* Texture=nullptr;FLinearColor GeoMin,GeoMax;
			if(!Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(TEXT("RegionalSurroundReady"))),Ready)||Ready!=1.f
				||!Material->GetTextureParameterValue(FHashedMaterialParameterInfo(FName(TEXT("RegionalSurroundTexture"))),Texture)
				||!Material->GetVectorParameterValue(FHashedMaterialParameterInfo(FName(TEXT("RegionalSurroundGeoMin"))),GeoMin)
				||!Material->GetVectorParameterValue(FHashedMaterialParameterInfo(FName(TEXT("RegionalSurroundGeoMax"))),GeoMax))return false;
			const auto* Texture2D=Cast<UTexture2D>(Texture);
			if(!Texture2D||Texture2D->GetPathName()!=ExpectedPath||Texture2D->GetSizeX()!=2048||Texture2D->GetSizeY()!=2048
				||Texture2D->GetNumMips()!=12||!Texture2D->SRGB||Texture2D->NeverStream)return false;
			if(!FMath::IsFinite(GeoMin.R)||!FMath::IsFinite(GeoMin.G)||!FMath::IsFinite(GeoMin.B)
				||!FMath::IsFinite(GeoMax.R)||!FMath::IsFinite(GeoMax.G)||!FMath::IsFinite(GeoMax.B)
				||GeoMin.R>=GeoMax.R||GeoMin.G>=GeoMax.G)return false;
			if(MaterialIndex==0){SharedMin=GeoMin;SharedMax=GeoMax;}else if(GeoMin!=SharedMin||GeoMax!=SharedMax)return false;
		}
		return true;
	}
	TSharedRef<FJsonObject> GeographyReadinessReceipt(const FGeographyViewAuditState& State)
	{
		auto Receipt=MakeShared<FJsonObject>();Receipt->SetNumberField(TEXT("schema_version"),1);
		Receipt->SetBoolField(TEXT("required"),State.bReadinessRequired);Receipt->SetBoolField(TEXT("passed"),State.bReadinessPassed);
		Receipt->SetBoolField(TEXT("timed_out"),State.bReadinessTimedOut);Receipt->SetNumberField(TEXT("wait_seconds"),State.ReadinessWaitSeconds);
		Receipt->SetNumberField(TEXT("polls"),State.ReadinessPolls);Receipt->SetNumberField(TEXT("deadline_seconds"),15.);
		Receipt->SetBoolField(TEXT("capture_timeline_scheduled_once"),State.bTimelineScheduled);
		Receipt->SetStringField(TEXT("scope"),TEXT("Opt-in geography warmup only. Actual four-MID source path/2048/12-mip/sRGB/streaming readiness; no forced readiness, normal settings writes or gameplay pause. Full source bbox validation remains in runner."));return Receipt;
	}
	void BeginSingleRoadConstructionAudit(UWorld& World)
	{
		const TWeakObjectPtr<UWorld> Weak(&World);
		FTimerHandle ViewTimer;World.GetTimerManager().SetTimer(ViewTimer,FTimerDelegate::CreateLambda([Weak]()
		{
			auto* W=Weak.Get();if(!IsActiveAuditWorld(W))return;
			SetRenderAuditZoom(*W,8000.);
			ApplyPhysicalSunPreview(*W);
			if(FParse::Param(FCommandLine::Get(),TEXT("AftershockGroundBouncePreview")))
			{
				USkyLightComponent* Candidate=nullptr;int32 Candidates=0;
				for(TActorIterator<AActor> It(W);It;++It)
				{
					if(It->IsHidden())continue;
					TInlineComponentArray<USkyLightComponent*> Skies;It->GetComponents(Skies);
					for(USkyLightComponent* Sky:Skies)
						if(Sky->IsRegistered()&&Sky->IsVisible()&&Sky->bAffectsWorld&&Sky->Intensity>0.f&&Sky->GetMobility()==EComponentMobility::Movable
							&&Sky->SourceType==SLS_CapturedScene&&!Sky->IsRealTimeCaptureEnabled()&&Sky->bLowerHemisphereIsBlack
							&&FMath::IsNearlyZero(Sky->LowerHemisphereColor.R)&&FMath::IsNearlyZero(Sky->LowerHemisphereColor.G)&&FMath::IsNearlyZero(Sky->LowerHemisphereColor.B))
						{Candidate=Sky;++Candidates;}
				}
				bool Applied=false,Recaptured=false;float IntensityBefore=0.f,IntensityAfter=0.f;
				if(Candidates==1&&Candidate)
				{
					IntensityBefore=Candidate->Intensity;
					const FLinearColor Bounce(.10f,.085f,.065f,1.f);
					Candidate->SetLowerHemisphereColor(Bounce);
					Applied=Candidate->LowerHemisphereColor.Equals(Bounce,UE_SMALL_NUMBER);
					if(Applied){Candidate->RecaptureSky();Recaptured=true;}
					IntensityAfter=Candidate->Intensity;
				}
				UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_GROUND_BOUNCE_PREVIEW candidates=%d applied=%d recaptureRequests=%d intensityBefore=%.6f intensityAfter=%.6f lowerLinearRGB=0.10,0.085,0.065 diagnosticOnly=1 defaultShippingChange=0 sunExposureMaterialGameplayWrites=0"),
					Candidates,Applied,Recaptured?1:0,IntensityBefore,IntensityAfter);
			}
		}),2.f,false);
		FTimerHandle TransactionTimer;World.GetTimerManager().SetTimer(TransactionTimer,FTimerDelegate::CreateLambda([Weak]()
		{
			auto* W=Weak.Get();if(!IsActiveAuditWorld(W))return;
			auto Report=MakeShared<FJsonObject>();Report->SetNumberField(TEXT("schema_version"),1);
			Report->SetBoolField(TEXT("valid"),false);Report->SetBoolField(TEXT("performance_accepted"),false);
			Report->SetStringField(TEXT("scope"),TEXT("Opt-in fresh-process real paid one-cell road transaction through production owners; no physics edits, save/load/settings writes, budget refund, fake network event or manual mesh refresh. CPU call time is not GPU frame time."));
			Report->SetBoolField(TEXT("persistent_save_api_called"),false);
			const auto Fail=[&](const TCHAR* Reason)
			{Report->SetStringField(TEXT("failure"),Reason);SaveRenderAuditJson(Report,TEXT("single_road_construction_audit.json"));UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_SINGLE_ROAD_AUDIT_FAIL reason=%s"),Reason);};
			auto* Terrain=W->GetSubsystem<UTerrainGridSubsystem>();auto* Roads=W->GetSubsystem<URoadNetworkSubsystem>();
			auto* Buildings=W->GetSubsystem<UBuildingManagerSubsystem>();auto* Economy=W->GetSubsystem<UEconomyAndStateSubsystem>();
			auto* City=W->GetSubsystem<UCityPresentationSubsystem>();auto* PC=W->GetFirstPlayerController();
			const auto* Camera=PC?Cast<AAftershockCameraPawn>(PC->GetPawn()):nullptr;
			if(!Terrain||!Roads||!Buildings||!Economy||!City||!Camera||!PC->PlayerCameraManager){Fail(TEXT("Production owners/camera unavailable"));return;}
			// Settle the ordinary representation BEFORE timing; transaction itself
			// must cause its own real OnRoadNetworkChanged refresh.
			City->RefreshVisiblePresentation();const auto Bounds=GetAftershockDrawableCellBounds(*Terrain);
			TArray<FIntPoint> Candidates;const FIntPoint Dir[]={FIntPoint(1,0),FIntPoint(0,1),FIntPoint(-1,0),FIntPoint(0,-1)};
			for(int32 Y=Bounds.Min.Y;Y<Bounds.Max.Y;++Y)for(int32 X=Bounds.Min.X;X<Bounds.Max.X;++X)
			{
				const FIntPoint C(X,Y);FGridCell Cell;
				if(!Terrain->TryGetCell(C,Cell)||!Cell.bIsPlayable||Cell.bHasRoad||Cell.bIsRiverbed||Cell.bIsOcean||Cell.BridgeID!=INDEX_NONE||Cell.OccupyingBuildingID!=INDEX_NONE||Cell.bIsEmergencyAssemblyArea||Cell.bVolcanicVent||Cell.bBurning||Cell.LavaCoverage01>.1f)continue;
				const auto Local=UTerrainGridSubsystem::CellToChunkLocal(C);
				if(Local.X<5||Local.X>19||Local.Y<5||Local.Y>19)continue; // interior: one existing chunk only
				bool Adjacent=false;for(const auto& D:Dir)Adjacent|=Roads->HasRoad(C+D)&&Roads->CanAccessRoadFromCell(C,C+D);
				if(!Adjacent)continue;
				Candidates.Add(C);
			}
			const FVector Focus=Camera->GetLocalMapFocus();
			Candidates.Sort([&](const auto& A,const auto& B)
			{const double DA=FVector::DistSquared2D(Terrain->GridToWorld(A),Focus),DB=FVector::DistSquared2D(Terrain->GridToWorld(B),Focus);return DA!=DB?DA<DB:(A.Y==B.Y?A.X<B.X:A.Y<B.Y);});
			if(Candidates.IsEmpty()){Fail(TEXT("No valid connected single-cell interior paid road candidate"));return;}
			FIntPoint C(MAX_int32,MAX_int32);
			for(const auto& Candidate:Candidates)
			{const auto Q=Roads->QuoteRoad(Candidate,Candidate);if(Q.bRouteValid&&!Q.bIncludesBridge&&Q.NewRoadCells==1&&Q.CostTL==Roads->GetRoadCellCostTL()){C=Candidate;break;}}
			if(C.X==MAX_int32){Fail(TEXT("Real one-cell quote rejected all nearby eligible candidates"));return;}
			const auto CollectMeshes=[W]()
			{
				TMap<FString,TWeakObjectPtr<UStaticMesh>> Result;
				for(TActorIterator<AAftershockCityPresentationActor> It(W);It;++It)
				{TInlineComponentArray<UStaticMeshComponent*> Components;It->GetComponents(Components);for(auto* M:Components)if(M->GetName().StartsWith(TEXT("ConformingRoadChunk_")))Result.Add(M->GetPathName(),M->GetStaticMesh());}
				return Result;
			};
			const auto BeforeMeshes=CollectMeshes();if(BeforeMeshes.Num()==0){Fail(TEXT("No actual chunked road components; wrong binary or renderer not ready"));return;}
			FAftershockBuildingsSaveState BeforeBuildings,AfterBuildings;Buildings->CaptureSaveState(BeforeBuildings);
			auto Before=ReadRenderAuditMetadata(*W,TEXT("single-road-before"));
			const auto GeometryBefore=GetAftershockTerrainGeometrySignature(*Terrain,Bounds);const auto GroundBefore=GetAftershockTerrainSurfaceBuildStats(W);
			const int32 RoadsBefore=Roads->CountRoadCells();const int64 TreasuryBefore=Economy->TreasuryTL;const uint64 Frame=GFrameCounter;
			FAftershockRoadQuote Actual;const double Start=FPlatformTime::Seconds();
			const bool Built=Roads->BuildRoad(C,C,Actual);
			const double CallMs=(FPlatformTime::Seconds()-Start)*1000.; // includes every synchronous production event listener
			const auto AfterMeshes=CollectMeshes();const auto GroundAfter=GetAftershockTerrainSurfaceBuildStats(W);
			auto After=ReadRenderAuditMetadata(*W,TEXT("single-road-after"));Buildings->CaptureSaveState(AfterBuildings);
			int32 Changed=0,Stable=0,Missing=0;
			for(const auto& P:BeforeMeshes)
			{const auto* Now=AfterMeshes.Find(P.Key);if(!Now)++Missing;else if(Now->Get()==P.Value.Get())++Stable;else++Changed;}
			const bool BuildingsSame=FAftershockBuildingsSaveState::StaticStruct()->CompareScriptStruct(&BeforeBuildings,&AfterBuildings,0);
			const bool GroundSame=GeometryBefore==GetAftershockTerrainGeometrySignature(*Terrain,Bounds)&&GroundBefore.SuccessfulGroundBuilds==GroundAfter.SuccessfulGroundBuilds&&GroundBefore.CollisionCookAttempts==GroundAfter.CollisionCookAttempts;
			const bool RosterSame=Before->GetStringField(TEXT("canonical_roster_hash"))==After->GetStringField(TEXT("canonical_roster_hash"))&&Before->GetNumberField(TEXT("canonical_living_residents"))==After->GetNumberField(TEXT("canonical_living_residents"));
			const bool Valid=Built&&Actual.NewRoadCells==1&&!Actual.bIncludesBridge&&Roads->CountRoadCells()==RoadsBefore+1
				&&TreasuryBefore-Economy->TreasuryTL==Actual.CostTL&&Actual.CostTL==Roads->GetRoadCellCostTL()
				&&BeforeMeshes.Num()==AfterMeshes.Num()&&Changed>0&&Changed<=3&&Missing==0&&Stable>=BeforeMeshes.Num()-3
				&&GroundSame&&BuildingsSame&&RosterSame&&After->GetObjectField(TEXT("architecture_cpd_audit"))->GetBoolField(TEXT("valid"));
			Report->SetBoolField(TEXT("valid"),Valid);Report->SetBoolField(TEXT("performance_accepted"),Valid&&CallMs<=16.);
			Report->SetNumberField(TEXT("budget_ms"),16.);Report->SetNumberField(TEXT("production_transaction_cpu_ms"),CallMs);
			Report->SetNumberField(TEXT("transaction_frame_counter"),static_cast<double>(Frame));Report->SetNumberField(TEXT("coord_x"),C.X);Report->SetNumberField(TEXT("coord_y"),C.Y);
			Report->SetNumberField(TEXT("road_cells_before"),RoadsBefore);Report->SetNumberField(TEXT("road_cells_after"),Roads->CountRoadCells());
			Report->SetNumberField(TEXT("cost_tl"),Actual.CostTL);Report->SetNumberField(TEXT("treasury_delta_tl"),TreasuryBefore-Economy->TreasuryTL);
			Report->SetNumberField(TEXT("road_components_before"),BeforeMeshes.Num());Report->SetNumberField(TEXT("road_components_after"),AfterMeshes.Num());
			Report->SetNumberField(TEXT("changed_mesh_components"),Changed);Report->SetNumberField(TEXT("unchanged_mesh_components"),Stable);Report->SetNumberField(TEXT("missing_components"),Missing);
			Report->SetBoolField(TEXT("canonical_ground_and_cooks_unchanged"),GroundSame);Report->SetBoolField(TEXT("canonical_buildings_unchanged"),BuildingsSame);Report->SetBoolField(TEXT("canonical_roster_unchanged"),RosterSame);
			Report->SetObjectField(TEXT("before"),Before);Report->SetObjectField(TEXT("after"),After);
			const FString Stem=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/single-road-after"));
			const FMinimalViewInfo PinnedView=PC->PlayerCameraManager->GetCameraCacheView();
			Report->SetBoolField(TEXT("scene_saved"),CaptureScene(*W,PinnedView,Stem+TEXT("-scene.png"),1600,900));
			auto Diagnostic=CaptureMaterialBufferDiagnostics(*W,PinnedView,Stem);Report->SetObjectField(TEXT("material_buffer_diagnostics"),Diagnostic);
			if(Diagnostic->GetBoolField(TEXT("requested")))Report->SetBoolField(TEXT("valid"),Valid&&Diagnostic->GetBoolField(TEXT("valid")));
			SaveRenderAuditJson(Report,TEXT("single_road_construction_audit.json"));
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SINGLE_ROAD_AUDIT valid=%d performanceAccepted=%d cpuCallMs=%.3f budgetMs=16 changedMeshComponents=%d stableMeshComponents=%d sourceGroundUnchanged=%d oneRealPaidTransaction=1 persistentSaveCalls=0 noGpuFrameTimeClaim=1"),Valid,Valid&&CallMs<=16.,CallMs,Changed,Stable,GroundSame);
		}),6.f,false);
		FTimerHandle ExitTimer;World.GetTimerManager().SetTimer(ExitTimer,FTimerDelegate::CreateLambda([Weak]()
		{if(auto* W=Weak.Get();IsActiveAuditWorld(W)){UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_VISUAL_AUDIT_END singleRoadConstruction=1"));FPlatformMisc::RequestExit(false);}}),12.f,false);
	}

	void ScheduleGeographyViewAudit(UWorld& World,TSharedRef<FGeographyViewAuditState> State)
	{
		const TWeakObjectPtr<UWorld> WeakWorld(&World);
		for(int32 StageIndex=0;StageIndex<4;++StageIndex)
		{
			FTimerHandle ZoomTimer;World.GetTimerManager().SetTimer(ZoomTimer,FTimerDelegate::CreateLambda([WeakWorld,State,StageIndex]()
			{
				if(auto* Active=WeakWorld.Get();IsActiveAuditWorld(Active))
				{
					const auto* Terrain=Active->GetSubsystem<UTerrainGridSubsystem>();
					const double Zooms[]={8000.,125000.,500000.,Terrain?GetAftershockGlobeLayout(*Terrain).RadiusCm*2.2:0.};
					State->Valid&=SetRenderAuditZoom(*Active,Zooms[StageIndex]);
				}
			}),1.f+StageIndex*4.f,false);
			FTimerHandle FrameTimer;World.GetTimerManager().SetTimer(FrameTimer,FTimerDelegate::CreateLambda([WeakWorld,State,StageIndex]()
			{
				auto* Active=WeakWorld.Get();if(!IsActiveAuditWorld(Active))return;
				auto* Controller=Active->GetFirstPlayerController();if(!Controller||!Controller->PlayerCameraManager){State->Valid=false;return;}
				const TCHAR* Names[]={TEXT("street"),TEXT("1250m"),TEXT("5000m"),TEXT("globe")};const FString Stage=Names[StageIndex];
				const FString Stem=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation")/(TEXT("geo-view-")+Stage));
				const FMinimalViewInfo PinnedView=Controller->PlayerCameraManager->GetCameraCacheView();
				const bool Saved=CaptureScene(*Active,PinnedView,Stem+TEXT("-scene.png"),1600,900);
				FScreenshotRequest::RequestScreenshot(Stem+TEXT("-hud.png"),true,false,false,FIntRect(),true);
				auto Snapshot=ReadRenderAuditMetadata(*Active,Stage);Snapshot->SetBoolField(TEXT("scene_saved"),Saved);
				if(StageIndex==0)
				{
					auto Diagnostic=CaptureMaterialBufferDiagnostics(*Active,PinnedView,Stem);
					Snapshot->SetObjectField(TEXT("material_buffer_diagnostics"),Diagnostic);
					State->Valid&=Diagnostic->GetBoolField(TEXT("valid"));
				}
				const int32 ExpectedTiers[]={0,2,2,3};
				State->Valid&=Saved&&Snapshot->GetBoolField(TEXT("metadata_valid"))&&Snapshot->GetBoolField(TEXT("real_geography"))
					&&Snapshot->GetBoolField(TEXT("source_context_mesh_present"))&&Snapshot->GetBoolField(TEXT("source_surface_mask_present"))&&Snapshot->GetNumberField(TEXT("tier"))==ExpectedTiers[StageIndex]
					&&Snapshot->GetBoolField(TEXT("planet_clip_parameter_present"))&&Snapshot->GetBoolField(TEXT("planet_material_masked"))
					&&Snapshot->GetNumberField(TEXT("planet_regional_clip_enabled"))==((StageIndex==1||StageIndex==2)?1.:0.);
				if(StageIndex==0)
				{
					State->Roster=Snapshot->GetStringField(TEXT("canonical_roster_hash"));State->Population=Snapshot->GetNumberField(TEXT("canonical_living_residents"));
					State->VisualAgents=Snapshot->GetNumberField(TEXT("visual_agents"));State->GroundBuilds=Snapshot->GetNumberField(TEXT("ground_builds"));State->GroundCooks=Snapshot->GetNumberField(TEXT("ground_collision_cooks"));
				}
				else
				{
					State->SameRoster&=State->Roster==Snapshot->GetStringField(TEXT("canonical_roster_hash"))&&State->Population==Snapshot->GetNumberField(TEXT("canonical_living_residents"))&&State->VisualAgents==Snapshot->GetNumberField(TEXT("visual_agents"));
					State->SameGroundWork&=State->GroundBuilds==Snapshot->GetNumberField(TEXT("ground_builds"))&&State->GroundCooks==Snapshot->GetNumberField(TEXT("ground_collision_cooks"));
				}
				State->Snapshots.Add(MakeShared<FJsonValueObject>(Snapshot));
				UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_GEOGRAPHIC_VIEW_FRAME stage=%s sceneSaved=%d actualTier=%d actualSceneMetadata=1"),*Stage,Saved,static_cast<int32>(Snapshot->GetNumberField(TEXT("tier"))));
			}),4.f+StageIndex*4.f,false);
		}
		if(State->bTemporalRequested)for(int32 PairIndex=0;PairIndex<2;++PairIndex)
		{
			FTimerHandle PairTimer;World.GetTimerManager().SetTimer(PairTimer,FTimerDelegate::CreateLambda([WeakWorld,State,PairIndex]()
			{
				auto* Active=WeakWorld.Get();if(!IsActiveAuditWorld(Active))return;
				auto* PairController=Active->GetFirstPlayerController();if(!PairController||!PairController->PlayerCameraManager){State->bTemporalValid=false;return;}
				const FMinimalViewInfo ActualView=PairController->PlayerCameraManager->GetCameraCacheView();
				if(PairIndex==0){State->TemporalCamera=ActualView;State->bHasTemporalCamera=true;State->TemporalFirstFrame=GFrameCounter;State->TemporalFirstWorldSeconds=Active->GetTimeSeconds();}
				const FString PairStage=FString::Printf(TEXT("street-static-%d"),PairIndex);
				const FString PairStem=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation")/(TEXT("geo-view-")+PairStage));
				const bool PairSaved=State->bHasTemporalCamera&&CaptureScene(*Active,State->TemporalCamera,PairStem+TEXT("-scene.png"),1600,900);
				auto PairMetadata=ReadRenderAuditMetadata(*Active,PairStage);
				const bool SameCamera=State->bHasTemporalCamera&&ActualView.Location.Equals(State->TemporalCamera.Location,.01)
					&&ActualView.Rotation.Equals(State->TemporalCamera.Rotation,.001f)&&FMath::IsNearlyEqual(ActualView.FOV,State->TemporalCamera.FOV,.001f);
				PairMetadata->SetBoolField(TEXT("scene_saved"),PairSaved);PairMetadata->SetBoolField(TEXT("actual_camera_matches_pinned_capture_view"),SameCamera);
				PairMetadata->SetNumberField(TEXT("requested_pair_interval_seconds"),.5);PairMetadata->SetNumberField(TEXT("elapsed_since_pair_first_world_seconds"),Active->GetTimeSeconds()-State->TemporalFirstWorldSeconds);
				SetRenderAuditVector(PairMetadata,TEXT("pinned_capture_location_cm"),State->TemporalCamera.Location);
				SetRenderAuditVector(PairMetadata,TEXT("pinned_capture_rotation_degrees"),FVector(State->TemporalCamera.Rotation.Pitch,State->TemporalCamera.Rotation.Yaw,State->TemporalCamera.Rotation.Roll));
				PairMetadata->SetNumberField(TEXT("pinned_capture_fov_degrees"),State->TemporalCamera.FOV);PairMetadata->SetBoolField(TEXT("moving_world_was_not_frozen"),true);
				PairMetadata->SetBoolField(TEXT("flicker_free_claim"),false);PairMetadata->SetBoolField(TEXT("isolated_gpu_timing_claim"),false);
				const bool DistinctFrame=PairIndex==0||(GFrameCounter>State->TemporalFirstFrame&&Active->GetTimeSeconds()>State->TemporalFirstWorldSeconds);
				State->bTemporalValid&=PairSaved&&SameCamera&&DistinctFrame&&PairMetadata->GetBoolField(TEXT("metadata_valid"))&&PairMetadata->GetNumberField(TEXT("tier"))==0
					&&PairMetadata->GetStringField(TEXT("canonical_roster_hash"))==State->Roster
					&&PairMetadata->GetNumberField(TEXT("ground_builds"))==State->GroundBuilds&&PairMetadata->GetNumberField(TEXT("ground_collision_cooks"))==State->GroundCooks;
				State->TemporalSnapshots.Add(MakeShared<FJsonValueObject>(PairMetadata));
				UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_GEOGRAPHIC_TEMPORAL_FRAME pairIndex=%d sceneSaved=%d samePinnedCamera=%d distinctFrame=%d requestedIntervalSeconds=.5 actualElapsedSeconds=%.3f worldNotFrozen=1 noFlickerFreeClaim=1"),PairIndex,PairSaved,SameCamera,DistinctFrame,Active->GetTimeSeconds()-State->TemporalFirstWorldSeconds);
			}),4.2f+PairIndex*.5f,false);
		}
		if(State->bRegionalRequested)
		{
			FTimerHandle RegionalZoomTimer;World.GetTimerManager().SetTimer(RegionalZoomTimer,FTimerDelegate::CreateLambda([WeakWorld,State]()
			{
				auto* Active=WeakWorld.Get();if(!IsActiveAuditWorld(Active))return;
				auto* RegionController=Active->GetFirstPlayerController();const auto* RegionCamera=RegionController?Cast<AAftershockCameraPawn>(RegionController->GetPawn()):nullptr;
				const auto* Grid=Active->GetSubsystem<UTerrainGridSubsystem>();FAftershockGeographicBounds SourceBounds;FVector NorthWest=FVector::ZeroVector,SouthEast=FVector::ZeroVector;FBox ActualContext(ForceInit);
				for(TActorIterator<AAftershockCityPresentationActor> It(Active);It;++It)
				{TInlineComponentArray<UStaticMeshComponent*> Components;It->GetComponents(Components);for(const UStaticMeshComponent* Component:Components)if(Component->GetFName()==FName(TEXT("CityToGlobeLandscape"))&&Component->GetStaticMesh())ActualContext=Component->Bounds.GetBox();}
				const bool Ready=Grid&&Grid->ContinuationGeneratorVersion==3&&RegionCamera&&RegionController->PlayerCameraManager&&ActualContext.IsValid
					&&GetAftershockRegionalContextBounds(Grid->GeographyIndex,SourceBounds)
					&&AftershockGeographicPresentationWorld(*Grid,SourceBounds.North,SourceBounds.West,NorthWest)&&AftershockGeographicPresentationWorld(*Grid,SourceBounds.South,SourceBounds.East,SouthEast);
				if(!Ready){State->bRegionalValid=false;return;}
				State->RegionalROI=FBox(FVector(NorthWest.X,NorthWest.Y,ActualContext.Min.Z),FVector(SouthEast.X,SouthEast.Y,ActualContext.Max.Z));
				State->bRegionalMeshCoversROI=ActualContext.Min.X<=State->RegionalROI.Min.X+1.&&ActualContext.Min.Y<=State->RegionalROI.Min.Y+1.
					&&ActualContext.Max.X>=State->RegionalROI.Max.X-1.&&ActualContext.Max.Y>=State->RegionalROI.Max.Y-1.;
				const auto View=RegionController->PlayerCameraManager->GetCameraCacheView();const double HorizontalHalf=FMath::DegreesToRadians(static_cast<double>(View.FOV)*.5);
				const double VerticalHalf=FMath::Atan(FMath::Tan(HorizontalHalf)/(1600./900.));const double Sine=FMath::Sin(FMath::Min(HorizontalHalf,VerticalHalf));
				if(!FMath::IsFinite(Sine)||Sine<=.05){State->bRegionalValid=false;return;}
				double Radius=0.;const FVector Focus=RegionCamera->GetLocalMapFocus();
				for(int32 Corner=0;Corner<8;++Corner){const FVector Point(Corner&1?State->RegionalROI.Max.X:State->RegionalROI.Min.X,Corner&2?State->RegionalROI.Max.Y:State->RegionalROI.Min.Y,Corner&4?State->RegionalROI.Max.Z:State->RegionalROI.Min.Z);Radius=FMath::Max(Radius,FVector::Distance(Point,Focus));}
				const double Span=FMath::Max(State->RegionalROI.GetSize().X,State->RegionalROI.GetSize().Y);
				// Bounding sphere around the existing city focus, not merely an
				// arbitrary5km zoom or a shifted/cropped source. Narrow FOVs may
				// legitimately require more than2*span. Actual projected corners
				// are validated at capture; no geometry/canonical camera focus edit.
				State->RegionalTargetZoomCm=FMath::Max(Span*1.4,Radius/Sine*1.12);
				State->bRegionalValid&=State->bRegionalMeshCoversROI&&SetRenderAuditZoom(*Active,State->RegionalTargetZoomCm);
			}),18.f,false);
			FTimerHandle RegionalFrameTimer;World.GetTimerManager().SetTimer(RegionalFrameTimer,FTimerDelegate::CreateLambda([WeakWorld,State]()
			{
				auto* Active=WeakWorld.Get();if(!IsActiveAuditWorld(Active))return;auto* RegionController=Active->GetFirstPlayerController();
				if(!RegionController||!RegionController->PlayerCameraManager||!State->RegionalROI.IsValid){State->bRegionalValid=false;return;}
				const FString Stem=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/geo-view-regional"));
				const bool ImageSaved=CaptureScene(*Active,RegionController->PlayerCameraManager->GetCameraCacheView(),Stem+TEXT("-scene.png"),1600,900);
				FScreenshotRequest::RequestScreenshot(Stem+TEXT("-hud.png"),true,false,false,FIntRect(),true);
				auto Metadata=ReadRenderAuditMetadata(*Active,TEXT("regional"));int32 ViewportX=0,ViewportY=0;RegionController->GetViewportSize(ViewportX,ViewportY);TArray<TSharedPtr<FJsonValue>> ProjectedCorners;bool AllInFrame=ViewportX==1600&&ViewportY==900;
				for(int32 Corner=0;Corner<8;++Corner)
				{
					const FVector Point(Corner&1?State->RegionalROI.Max.X:State->RegionalROI.Min.X,Corner&2?State->RegionalROI.Max.Y:State->RegionalROI.Min.Y,Corner&4?State->RegionalROI.Max.Z:State->RegionalROI.Min.Z);FVector2D Pixel;
					const bool Projected=UGameplayStatics::ProjectWorldToScreen(RegionController,Point,Pixel,true);const bool InFrame=Projected&&!Pixel.ContainsNaN()&&Pixel.X>=0.&&Pixel.Y>=0.&&Pixel.X<=ViewportX&&Pixel.Y<=ViewportY;AllInFrame&=InFrame;
					auto Projection=MakeShared<FJsonObject>();Projection->SetNumberField(TEXT("corner"),Corner);Projection->SetBoolField(TEXT("projected"),Projected);Projection->SetBoolField(TEXT("inside_frame"),InFrame);if(Projected&&!Pixel.ContainsNaN()){Projection->SetNumberField(TEXT("pixel_x"),Pixel.X);Projection->SetNumberField(TEXT("pixel_y"),Pixel.Y);}ProjectedCorners.Add(MakeShared<FJsonValueObject>(Projection));
				}
				Metadata->SetBoolField(TEXT("scene_saved"),ImageSaved);Metadata->SetBoolField(TEXT("whole_source_roi_in_frame"),AllInFrame);Metadata->SetBoolField(TEXT("actual_mesh_covers_source_roi_xy"),State->bRegionalMeshCoversROI);
				Metadata->SetNumberField(TEXT("requested_regional_zoom_cm"),State->RegionalTargetZoomCm);Metadata->SetNumberField(TEXT("projection_viewport_width"),ViewportX);Metadata->SetNumberField(TEXT("projection_viewport_height"),ViewportY);
				Metadata->SetArrayField(TEXT("source_roi_projected_corners"),ProjectedCorners);SetRenderAuditVector(Metadata,TEXT("requested_source_roi_min_cm"),State->RegionalROI.Min);SetRenderAuditVector(Metadata,TEXT("requested_source_roi_max_cm"),State->RegionalROI.Max);
				State->bRegionalValid&=ImageSaved&&AllInFrame&&Metadata->GetBoolField(TEXT("metadata_valid"))&&Metadata->GetNumberField(TEXT("tier"))==2&&Metadata->GetNumberField(TEXT("overview_blend"))<.2
					&&Metadata->GetStringField(TEXT("canonical_roster_hash"))==State->Roster&&Metadata->GetNumberField(TEXT("canonical_living_residents"))==State->Population
					&&Metadata->GetNumberField(TEXT("ground_builds"))==State->GroundBuilds&&Metadata->GetNumberField(TEXT("ground_collision_cooks"))==State->GroundCooks;
				State->RegionalSnapshots.Add(MakeShared<FJsonValueObject>(Metadata));
				UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_GEOGRAPHIC_REGIONAL_FRAME sceneSaved=%d wholeSourceRoiInFrame=%d actualMeshCoversRoi=%d targetZoomCm=%.1f tier=%d oldFourViewCadenceUnchanged=1"),ImageSaved,AllInFrame,State->bRegionalMeshCoversROI,State->RegionalTargetZoomCm,static_cast<int32>(Metadata->GetNumberField(TEXT("tier"))));
			}),22.f,false);
		}
		FTimerHandle EndTimer;World.GetTimerManager().SetTimer(EndTimer,FTimerDelegate::CreateLambda([WeakWorld,State]()
		{
			auto* Active=WeakWorld.Get();if(!IsActiveAuditWorld(Active))return;const auto* Terrain=Active->GetSubsystem<UTerrainGridSubsystem>();
			auto Report=MakeShared<FJsonObject>();Report->SetNumberField(TEXT("schema_version"),1);const bool TemporalComplete=!State->bTemporalRequested||(State->bTemporalValid&&State->TemporalSnapshots.Num()==2);
			const bool RegionalComplete=!State->bRegionalRequested||(State->bRegionalValid&&State->RegionalSnapshots.Num()==1);
			const bool Valid=State->Valid&&State->Snapshots.Num()==4&&State->SameRoster&&State->SameGroundWork&&TemporalComplete&&RegionalComplete;
			Report->SetBoolField(TEXT("valid"),Valid);Report->SetNumberField(TEXT("region_index"),Terrain?Terrain->GeographyIndex:INDEX_NONE);Report->SetNumberField(TEXT("focus_index"),Terrain?Terrain->GeographyFocusIndex:INDEX_NONE);
			Report->SetObjectField(TEXT("surround_readiness"),GeographyReadinessReceipt(*State));
			Report->SetBoolField(TEXT("same_canonical_roster_across_views"),State->SameRoster);Report->SetBoolField(TEXT("unchanged_ground_builds_and_cooks_across_views"),State->SameGroundWork);Report->SetArrayField(TEXT("snapshots"),State->Snapshots);
			Report->SetBoolField(TEXT("temporal_pair_requested"),State->bTemporalRequested);Report->SetBoolField(TEXT("temporal_pair_complete"),TemporalComplete);Report->SetArrayField(TEXT("temporal_snapshots"),State->TemporalSnapshots);
			Report->SetBoolField(TEXT("temporal_pair_proves_zero_flicker"),false);Report->SetBoolField(TEXT("temporal_pair_is_gpu_benchmark"),false);
			Report->SetBoolField(TEXT("regional_overview_requested"),State->bRegionalRequested);Report->SetBoolField(TEXT("regional_overview_complete"),RegionalComplete);Report->SetArrayField(TEXT("regional_snapshots"),State->RegionalSnapshots);
			Report->SetStringField(TEXT("scope"),TEXT("Normal production geography/roster and camera-only development views. Scene-only PNG plus requested viewport HUD; not mouse input, package, certified geographic accuracy or isolated GPU benchmark."));
			const bool Saved=SaveRenderAuditJson(Report,TEXT("geographic_view_audit.json"));
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_GEOGRAPHIC_VIEW_RESULT valid=%d snapshots=%d jsonSaved=%d"),Valid,State->Snapshots.Num(),Saved);
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_VISUAL_AUDIT_END geographyViews=1"));FPlatformMisc::RequestExit(false);
		}),State->bRegionalRequested?25.f:19.f,false);
	}

	void PollGeographyReadiness(TWeakObjectPtr<UWorld> WeakWorld,TSharedRef<FGeographyViewAuditState> State)
	{
		auto* Active=WeakWorld.Get();if(!IsActiveAuditWorld(Active)){if(Active)Active->GetTimerManager().ClearTimer(State->ReadinessTimer);return;}
		if(State->bTimelineScheduled||State->bReadinessTimedOut)return;
		++State->ReadinessPolls;State->ReadinessWaitSeconds=FPlatformTime::Seconds()-State->ReadinessStartedAt;
		const bool Ready=AreGeographySurroundMaterialsReady(*Active,State->bReadinessRequired);
		if(Ready)
		{
			Active->GetTimerManager().ClearTimer(State->ReadinessTimer);State->bReadinessPassed=true;State->bTimelineScheduled=true;
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_GEOGRAPHIC_READINESS_READY required=%d waitSeconds=%.3f polls=%d actualFourMidSourceAnd12Mips=%d timelineScheduledOnce=1"),State->bReadinessRequired,State->ReadinessWaitSeconds,State->ReadinessPolls,State->bReadinessRequired?1:0);
			ScheduleGeographyViewAudit(*Active,State);return;
		}
		if(State->ReadinessWaitSeconds<15.)return;
		Active->GetTimerManager().ClearTimer(State->ReadinessTimer);State->bReadinessTimedOut=true;State->Valid=false;
		const auto* Terrain=Active->GetSubsystem<UTerrainGridSubsystem>();auto Report=MakeShared<FJsonObject>();
		Report->SetNumberField(TEXT("schema_version"),1);Report->SetBoolField(TEXT("valid"),false);
		Report->SetNumberField(TEXT("region_index"),Terrain?Terrain->GeographyIndex:INDEX_NONE);Report->SetNumberField(TEXT("focus_index"),Terrain?Terrain->GeographyFocusIndex:INDEX_NONE);
		Report->SetArrayField(TEXT("snapshots"),State->Snapshots);Report->SetObjectField(TEXT("surround_readiness"),GeographyReadinessReceipt(*State));
		Report->SetStringField(TEXT("scope"),TEXT("Opt-in geography readiness timed out before any camera timeline/capture was scheduled. Missing or pending assets are not accepted as ready."));
		const bool Saved=SaveRenderAuditJson(Report,TEXT("geographic_view_audit.json"));
		UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_GEOGRAPHIC_READINESS_TIMEOUT waitSeconds=%.3f polls=%d deadlineSeconds=15 expectedAllFourMidReady12=1 jsonSaved=%d capturesScheduled=0"),State->ReadinessWaitSeconds,State->ReadinessPolls,Saved);
		FPlatformMisc::RequestExit(false);
	}
	void BeginGeographyViewAudit(UWorld& World)
	{
		if(!FParse::Param(FCommandLine::Get(),TEXT("AftershockVisualAudit"))||!FParse::Param(FCommandLine::Get(),TEXT("AftershockGeographyViewAudit")))return;
		auto State=MakeShared<FGeographyViewAuditState>();State->ReadinessStartedAt=FPlatformTime::Seconds();
		State->bTemporalRequested=FParse::Param(FCommandLine::Get(),TEXT("AftershockGeographyTemporalAudit"));
		State->bRegionalRequested=FParse::Param(FCommandLine::Get(),TEXT("AftershockRegionalOverviewAudit"));
		const TWeakObjectPtr<UWorld> WeakWorld(&World);PollGeographyReadiness(WeakWorld,State);
		if(State->bTimelineScheduled||State->bReadinessTimedOut)return;
		World.GetTimerManager().SetTimer(State->ReadinessTimer,FTimerDelegate::CreateLambda([WeakWorld,State](){PollGeographyReadiness(WeakWorld,State);}),.25f,true);
	}

	void RunCampaignTravelStage(UWorld& World)
	{
		if (!IsActiveAuditWorld(&World)) return;
		UAftershockScenarioSubsystem* Scenario = World.GetSubsystem<UAftershockScenarioSubsystem>();
		UTerrainGridSubsystem* Terrain = World.GetSubsystem<UTerrainGridSubsystem>();
		UBuildingManagerSubsystem* Buildings = World.GetSubsystem<UBuildingManagerSubsystem>();
		UEconomyAndStateSubsystem* Economy = World.GetSubsystem<UEconomyAndStateSubsystem>();
		URoadNetworkSubsystem* Roads = World.GetSubsystem<URoadNetworkSubsystem>();
		UDisasterSimulationSubsystem* Simulation = World.GetSubsystem<UDisasterSimulationSubsystem>();
		UCrowdRenderSubsystem* Crowd = World.GetSubsystem<UCrowdRenderSubsystem>();
		const auto Fail = [](const TCHAR* Reason)
		{
			CampaignTravelState.bFailed = true;
			UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_CAMPAIGN_TRAVEL_FAIL reason=%s debugFixture=1 zeroIntensity=1 scopeOnlyTravelAndGates=1 noHazardAccuracyClaim=1"), Reason);
		};
		if (!Scenario || !Terrain || !Buildings || !Economy || !Roads || !Simulation || !Crowd)
		{
			Fail(TEXT("Current-world subsystem missing")); return;
		}
		const int32 Index = Scenario->GetScenarioIndex();
		const FString MapPackage = World.GetOutermost()->GetName();
		int32 CLIIndex = 0;
		FParse::Value(FCommandLine::Get(), TEXT("AftershockScenario="), CLIIndex);
		const bool HasURLIndex = World.URL.HasOption(TEXT("AftershockScenario="));
		const int32 URLIndex = FCString::Atoi(World.URL.GetOption(TEXT("AftershockScenario="), TEXT("-1")));
		if (CampaignTravelState.InitialMapPackage.IsEmpty()) CampaignTravelState.InitialMapPackage = MapPackage;
		const bool SameMap = CampaignTravelState.InitialMapPackage == MapPackage;
		const bool SelectionCorrect = Index == CampaignTravelState.ExpectedScenario && CLIIndex == 0
			&& (Index == 0 || (HasURLIndex && URLIndex == Index));
		const bool StartupReady = Scenario->IsPrepared() && Scenario->GetAssemblyCells().Num() == 3
			&& Buildings->GetActiveBuildingsCount() == Scenario->GetDesignedLots().Num()
			&& Terrain->TerrainSeed == Scenario->GetDefinition().Seed && Crowd->GetCurrentCrowdCount() > 0;
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CAMPAIGN_TRAVEL_ARRIVAL index=%d expected=%d map=%s sameMap=%d cliIndex=%d hasURL=%d urlIndex=%d startupReady=%d seed=%d buildings=%d agents=%d paused=%d debugFixture=1 scopeOnlyTravelAndGates=1"),
			Index, CampaignTravelState.ExpectedScenario, *MapPackage, SameMap, CLIIndex, HasURLIndex, URLIndex, StartupReady,
			Terrain->TerrainSeed, Buildings->GetActiveBuildingsCount(), Crowd->GetCurrentCrowdCount(), World.IsPaused());
		if (CampaignTravelState.bFailed || !SameMap || !SelectionCorrect || !StartupReady || World.IsPaused())
		{
			Fail(TEXT("Unexpected stage/map, URL did not override CLI, unready settlement, or pause leaked into new world")); return;
		}
		++CampaignTravelState.ArrivedWorlds;

		const FIntPoint Root = Scenario->GetAssemblyCells()[0];
		bool RoadsReady = true;
		for (int32 I = 1; I < Scenario->GetAssemblyCells().Num(); ++I)
		{
			FAftershockRoadQuote Quote;
			const int64 Before = Economy->TreasuryTL;
			const FAftershockRoadQuote Existing = Roads->QuoteRoad(Root, Scenario->GetAssemblyCells()[I]);
			if (Existing.bRouteValid && Existing.NewRoadCells == 0) continue;
			const bool Built = Roads->BuildRoad(Root, Scenario->GetAssemblyCells()[I], Quote);
			RoadsReady &= Built && Before - Economy->TreasuryTL == Quote.CostTL;
		}
		TArray<FIntPoint> NearbyCells;
		for (const FGridCell& Cell : Terrain->Cells)
		{
			if (Cell.bIsPlayable && !Cell.bIsRiverbed && Cell.OccupyingBuildingID == INDEX_NONE) NearbyCells.Add(Cell.Coords);
		}
		NearbyCells.Sort([Root](FIntPoint A, FIntPoint B)
		{
			const int32 DA = FMath::Abs(A.X - Root.X) + FMath::Abs(A.Y - Root.Y), DB = FMath::Abs(B.X - Root.X) + FMath::Abs(B.Y - Root.Y);
			return DA == DB ? (A.Y == B.Y ? A.X < B.X : A.Y < B.Y) : DA < DB;
		});
		for (FIntPoint End : NearbyCells)
		{
			if (Roads->CountRoadCells() >= 9) break;
			const FAftershockRoadQuote Preview = Roads->QuoteRoad(Root, End);
			if (!Preview.bRouteValid || Preview.NewRoadCells == 0) continue;
			FAftershockRoadQuote Actual;
			const int64 Before = Economy->TreasuryTL;
			const bool Built = Roads->BuildRoad(Root, End, Actual);
			RoadsReady &= Built && Before - Economy->TreasuryTL == Actual.CostTL;
			if (!Built) break;
		}
		TArray<FBuildingInstance> Records; Buildings->GetAllBuildings(Records);
		bool PreparednessReady = false;
		if (Index == 0)
		{
			for (const FBuildingInstance& B : Records)
			{
				if (B.BuildingData && B.BuildingData->Type == EBuildingType::House)
				{
					PreparednessReady = Economy->RetrofitWithBudget(B.InstanceID); break;
				}
			}
		}
		else if (Index == 1)
		{
			int32 Upgraded = 0;
			for (FIntPoint Coord : NearbyCells)
			{
				if (Upgraded >= 6) break;
				if (Terrain->GetCell(Coord).DrainageQuality >= .2f) continue;
				int32 ActualCost = 0;
				if (!Economy->TrySpendMoney(400)) break;
				if (Simulation->InvestInStreetDrainage(Coord, 0, ActualCost) && ActualCost == 400) ++Upgraded;
			}
			PreparednessReady = Upgraded == 6;
		}
		else if (Index == 2)
		{
			UBuildingData* Park = nullptr;
			for (const FBuildingInstance& B : Records) if (B.BuildingData && B.BuildingData->Type == EBuildingType::GreenSpace) { Park = B.BuildingData; break; }
			int32 Added = 0;
			for (FIntPoint Coord : NearbyCells)
			{
				if (!Park || Added >= 2) break;
				if (Buildings->CanPlaceBuildingAt(Coord, Park) && Economy->BuildBuildingWithBudget(Coord, Park, EFoundationQuality::Medium, 1)) ++Added;
			}
			PreparednessReady = Added == 2;
		}
		else if(Index==3)
		{
			PreparednessReady = Terrain->IsCoastalWorld();
			for (FIntPoint Assembly : Scenario->GetAssemblyCells()) PreparednessReady &= Roads->HasRoad(Assembly);
		}
		else if(Index==4||Index==6)
		{
			const EBuildingType Required=Index==4?EBuildingType::FireStation:EBuildingType::Farm;
			UBuildingData* Data=Buildings->GetCatalogData(Required);
			for(FIntPoint Coord:NearbyCells)
			{
				if(Buildings->CanPlaceBuildingAt(Coord,Data)&&Economy->BuildBuildingWithBudget(Coord,Data,EFoundationQuality::Medium,Data->FloorsMin))
				{
					const FIntPoint Directions[]={FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1)};
					for(FIntPoint Direction:Directions)
					{
						const FAftershockRoadQuote Existing=Roads->QuoteRoad(Root,Coord+Direction);
						FAftershockRoadQuote Actual;
						if(Existing.bRouteValid&&(Existing.NewRoadCells==0||Roads->BuildRoad(Root,Coord+Direction,Actual))){PreparednessReady=true;break;}
					}
					break;
				}
			}
		}
		else if(Index==5)
		{
			int32 Surveyed=0;
			for(const FGridCell& Cell:Terrain->Cells){int32 Cost=0;if(Surveyed<3&&Simulation->SurveyGround(Cell.Coords,Cost)&&Cost==500)++Surveyed;}
			PreparednessReady=Surveyed==3;
		}
		if (!RoadsReady || Roads->CountRoadCells() < 9 || !PreparednessReady)
		{
			Fail(TEXT("Real paid road/preparedness fixture could not satisfy scenario objectives")); return;
		}
		const EDisasterType Hazard = Scenario->GetDefinition().Hazard;
		Crowd->BeginEmergencyEvacuation(Hazard);
		const FDisasterReport Report = Simulation->TriggerDisaster(Hazard, 0.f, 1.f, true);
		const bool CanAdvance = Scenario->CanAdvanceScenario();
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CAMPAIGN_TRAVEL_STAGE_READY index=%d roads=%d preparednessReady=%d primaryType=%d deaths=%d canAdvance=%d pausedBeforeTravel=%d progress=%s debugFixture=1 zeroIntensity=1 scopeOnlyTravelAndGates=1 noHazardAccuracyClaim=1"),
			Index, Roads->CountRoadCells(), PreparednessReady, static_cast<int32>(Report.DisasterType), Report.TotalDeaths, CanAdvance, World.IsPaused(), *Scenario->GetProgressText().ToString().Replace(TEXT("\n"), TEXT(" | ")));
		if (!CanAdvance || Report.TotalDeaths != 0)
		{
			Fail(TEXT("Actual zero-intensity primary event did not open the real objective gate")); return;
		}
		if (Scenario->IsFinalScenario())
		{
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CAMPAIGN_TRAVEL_RESULT passed=%d arrivals=%d finalScenario=%d map=%s actualOpenLevelSequence=0_1_2_3_4_5_6 urlOverridesCLI=1 debugFixture=1 zeroIntensity=1 scopeOnlyTravelAndGates=1 noHazardAccuracyClaim=1 noFinalTravelRequested=1"),
				CampaignTravelState.ArrivedWorlds == UAftershockScenarioSubsystem::GetScenarioCount(), CampaignTravelState.ArrivedWorlds, Index, *MapPackage);
			return;
		}
		CampaignTravelState.ExpectedScenario = Index + 1;
		const bool RequestedTravel = Scenario->AdvanceToNextScenario();
		// OpenLevel may now destroy this world: only copied values/static fixture
		// bookkeeping are read after the request; no old subsystem pointer is retained.
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CAMPAIGN_TRAVEL_REQUEST from=%d to=%d requested=%d map=%s realOpenLevel=1 debugFixture=1"), Index, Index + 1, RequestedTravel, *MapPackage);
		if (!RequestedTravel) Fail(TEXT("AdvanceToNextScenario rejected a completed stage"));
	}
}
#endif

void BeginAftershockPresentationAudit(UWorld& World)
{
#if !UE_BUILD_SHIPPING
	if (!FParse::Param(FCommandLine::Get(), TEXT("AftershockVisualAudit"))) return;
	if(FParse::Param(FCommandLine::Get(),TEXT("AftershockFireRuntimeAudit")))
	{
		BeginAftershockFireRuntimeAudit(World,
			[](UWorld& W,const FMinimalViewInfo& View,const FString& Path,UPrimitiveComponent* Hidden)
			{return CaptureScene(W,View,Path,1600,900,ESceneCaptureSource::SCS_FinalColorLDR,Hidden);},
			[](UWorld& W,const FString& Stage){return ReadRenderAuditMetadata(W,Stage);});
		return; // This opt-in audit owns its startup/Model2 gate, real fire clock and wall deadline.
	}

    if(FParse::Param(FCommandLine::Get(),TEXT("AftershockHydraulicRuntimeAudit")))
    {
        BeginAftershockHydraulicRuntimeAudit(World,
            [](UWorld& W,const FMinimalViewInfo& View,const FString& Path,UPrimitiveComponent* Hidden)
            {return CaptureScene(W,View,Path,1600,1000,ESceneCaptureSource::SCS_FinalColorLDR,Hidden);},
            [](UWorld& W,const FString& Stage){return ReadRenderAuditMetadata(W,Stage);});
        return; // Own real-time300s deadline; the generic14s exit must not run.
    }

	if(FParse::Param(FCommandLine::Get(),TEXT("AftershockSingleRoadConstructionAudit")))
	{
		BeginSingleRoadConstructionAudit(World);
		return; // Fresh throwaway world; owns its transaction/captures/deadline.
	}
	if(FParse::Param(FCommandLine::Get(),TEXT("AftershockGeographyViewAudit")))
	{
		BeginGeographyViewAudit(World);
		return; // This camera-only fixture owns four frames and its completion deadline.
	}
	if(FParse::Param(FCommandLine::Get(),TEXT("AftershockNarrativeAudit")))
	{
		BeginAftershockNarrativeRuntimeAudit(World);
		return; // Intro/epilogue pause gameplay; this fixture owns a real-time exit.
	}
	if(FParse::Param(FCommandLine::Get(),TEXT("AftershockChunkVisualAudit")))
	{
		BeginAftershockChunkRuntimeAudit(World,[](UWorld& W,const FMinimalViewInfo& View,const FString& Path){return CaptureScene(W,View,Path);});
		return; // The chunk fixture owns its deadline; the generic14s exit is too early.
	}
	const TWeakObjectPtr<UWorld> WeakWorld(&World);
	BeginAftershockCityLifeRuntimeAudit(World);
	if(FParse::Param(FCommandLine::Get(),TEXT("AftershockBridgeVisualAudit")))
		BeginAftershockBridgeRuntimeAudit(World,[](UWorld& W,const FMinimalViewInfo& View,const FString& Path){return CaptureScene(W,View,Path);});
	if (FParse::Param(FCommandLine::Get(), TEXT("AftershockRestartAudit")))
	{
		FTimerHandle RestartAuditTimer;
		World.GetTimerManager().SetTimer(RestartAuditTimer, FTimerDelegate::CreateLambda([WeakWorld]()
		{
			if (UWorld* Current = WeakWorld.Get(); IsActiveAuditWorld(Current)) RunRestartRuntimeStage(*Current);
		}), 3.f, false);
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("AftershockCampaignTravelAudit")))
	{
		FTimerHandle CampaignTravelTimer;
		World.GetTimerManager().SetTimer(CampaignTravelTimer, FTimerDelegate::CreateLambda([WeakWorld]()
		{
			if (UWorld* Current = WeakWorld.Get(); IsActiveAuditWorld(Current)) RunCampaignTravelStage(*Current);
		}), 3.f, false);
	}
	int32 ProfileAgents = 0;
	TSharedPtr<FRenderProfileAuditState> RenderProfile;
	if (FParse::Value(FCommandLine::Get(), TEXT("AftershockProfileAgents="), ProfileAgents))
	{
		ProfileAgents = FMath::Clamp(ProfileAgents, 1, 1500);
		RenderProfile=MakeShared<FRenderProfileAuditState>();RenderProfile->Agents=ProfileAgents;RenderProfile->StartedAt=FPlatformTime::Seconds();
		const bool bExplicitFrames=FParse::Value(FCommandLine::Get(),TEXT("AftershockProfileFrames="),RenderProfile->RequestedFrames);
		const bool bExplicitDuration=FParse::Value(FCommandLine::Get(),TEXT("AftershockProfileDuration="),RenderProfile->RequestedDuration);
		RenderProfile->RequestedFrames=FMath::Clamp(RenderProfile->RequestedFrames,300,12000);
		if(!FMath::IsFinite(RenderProfile->RequestedDuration))RenderProfile->RequestedDuration=14.f;
		RenderProfile->RequestedDuration=FMath::Clamp(RenderProfile->RequestedDuration,14.f,180.f);
		RenderProfile->bTimedCapture=bExplicitDuration&&!bExplicitFrames;
		RenderProfile->MinimumRuntime=RenderProfile->bTimedCapture?3.f+RenderProfile->RequestedDuration:RenderProfile->RequestedDuration;
		RenderProfile->DeadlineSeconds=RenderProfile->bTimedCapture?3.f+RenderProfile->RequestedDuration+5.f:FMath::Max(180.f,RenderProfile->MinimumRuntime+5.f);
		RenderProfile->CsvPath=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation")/FString::Printf(TEXT("crowd-render-%d.csv"),ProfileAgents));
		float RequestedOverview=0.f;
		if(FParse::Value(FCommandLine::Get(),TEXT("AftershockOverviewAudit="),RequestedOverview))
		{
			FTimerHandle ProfileViewTimer;World.GetTimerManager().SetTimer(ProfileViewTimer,FTimerDelegate::CreateLambda([WeakWorld,RequestedOverview]()
			{
				auto* Active=WeakWorld.Get();if(!IsActiveAuditWorld(Active))return;
				const auto* Terrain=Active->GetSubsystem<UTerrainGridSubsystem>();
				if(!Terrain||!FMath::IsFinite(RequestedOverview)||RequestedOverview<0.f||RequestedOverview>1.f)return;
				const auto Layout=GetAftershockGlobeLayout(*Terrain);
				double Low=0.,High=1.;for(int32 Iteration=0;Iteration<40;++Iteration)
				{const double Mid=(Low+High)*.5;if(Mid*Mid*(3.-2.*Mid)<RequestedOverview)Low=Mid;else High=Mid;}
				const double Start=Layout.GridSpanCm*.6;
				const double Target=RequestedOverview<=0.f?8000.:RequestedOverview>=1.f?Layout.RadiusCm*2.2:Start+(Low+High)*.5*(Layout.RadiusCm*1.6-Start);
				SetRenderAuditZoom(*Active,Target);
			}),2.25f,false);
		}
		FTimerHandle ProfilePopulationTimer;
		World.GetTimerManager().SetTimer(ProfilePopulationTimer, FTimerDelegate::CreateLambda([WeakWorld, ProfileAgents]()
		{
			if (UWorld* TestWorld = WeakWorld.Get())
			{
				if (UCrowdRenderSubsystem* Crowd = TestWorld->GetSubsystem<UCrowdRenderSubsystem>())
				{
					const bool Ready=Crowd->BeginSyntheticRenderProfile(ProfileAgents);
					UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_RENDER_PROFILE_SETUP requested=%d agents=%d ready=%d explicitSyntheticVisualPopulation=1 economyPopulationUnchanged=1"), ProfileAgents,Crowd->GetCurrentCrowdCount(),Ready);
				}
			}
		}), 2.f, false);
#if CSV_PROFILER
		FTimerHandle ProfileCaptureTimer;
		World.GetTimerManager().SetTimer(ProfileCaptureTimer, FTimerDelegate::CreateLambda([WeakWorld,RenderProfile]()
		{
			if(!IsActiveAuditWorld(WeakWorld.Get()))return;
			if(FCsvProfiler::IsCapturing()||FCsvProfiler::Get()->IsWritingFile()||FCsvProfiler::Get()->IsEndCapturePending())
			{UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_RENDER_PROFILE_CAPTURE_BUSY anotherCaptureOwnsProfiler=1"));return;}
			const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PresentationValidation"));
			IFileManager::Get().MakeDirectory(*Folder, true);
			RenderProfile->bCaptureRequested=true;RenderProfile->CaptureStartedAt=FPlatformTime::Seconds();RenderProfile->CaptureRequestedUtc=FDateTime::UtcNow();
			// World timers can begin late while the startup city/materials are
			// installed. Measure the full requested capture from its actual start;
			// a nominal t=3 deadline must not abort a valid sixty-second capture.
			if(RenderProfile->bTimedCapture)
			{
				const float ActualStartSeconds=static_cast<float>(RenderProfile->CaptureStartedAt-RenderProfile->StartedAt);
				RenderProfile->MinimumRuntime=FMath::Max(RenderProfile->MinimumRuntime,ActualStartSeconds+RenderProfile->RequestedDuration);
				RenderProfile->DeadlineSeconds=FMath::Max(RenderProfile->DeadlineSeconds,RenderProfile->MinimumRuntime+5.f);
			}
			FCsvProfiler::Get()->BeginCapture(RenderProfile->bTimedCapture?-1:RenderProfile->RequestedFrames, Folder, FPaths::GetCleanFilename(RenderProfile->CsvPath));
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_RENDER_PROFILE_CAPTURE agents=%d frames=%d captureMode=%s requestedCaptureSeconds=%.3f minimumRuntimeSeconds=%.3f deadlineSeconds=%.3f setupAtSeconds=2 captureAtSeconds=3 explicitFramesTakePriority=1 scope=WholeScene_NotIsolatedCrowdGpuCost"),
				RenderProfile->Agents,RenderProfile->bTimedCapture?-1:RenderProfile->RequestedFrames,RenderProfile->bTimedCapture?TEXT("duration"):TEXT("frames"),RenderProfile->bTimedCapture?RenderProfile->RequestedDuration:0.f,RenderProfile->MinimumRuntime,RenderProfile->DeadlineSeconds);
		}), 3.f, false);
#endif
	}
	FString WeatherAudit;
	FParse::Value(FCommandLine::Get(), TEXT("AftershockWeatherAudit="), WeatherAudit);
	const bool bTsunamiAudit = WeatherAudit.Equals(TEXT("Tsunami"), ESearchCase::IgnoreCase);
	const bool bWeatherAudit = WeatherAudit.Equals(TEXT("Flood"), ESearchCase::IgnoreCase) || WeatherAudit.Equals(TEXT("Wind"), ESearchCase::IgnoreCase) || bTsunamiAudit
		|| WeatherAudit.Equals(TEXT("Fire"),ESearchCase::IgnoreCase)||WeatherAudit.Equals(TEXT("Volcano"),ESearchCase::IgnoreCase)||WeatherAudit.Equals(TEXT("Drought"),ESearchCase::IgnoreCase);
	const bool bGlobeAudit = FParse::Param(FCommandLine::Get(), TEXT("AftershockGlobeAudit"));
	const bool bPhysicalFloodAudit=FParse::Param(FCommandLine::Get(),TEXT("AftershockPhysicalFloodAudit"));
	if(bPhysicalFloodAudit)BeginPhysicalFloodRenderAudit(World);
	if (FParse::Param(FCommandLine::Get(), TEXT("AftershockRoadAudit")))
	{
		FTimerHandle RoadTimer;
		World.GetTimerManager().SetTimer(RoadTimer, FTimerDelegate::CreateLambda([WeakWorld]()
		{
			UWorld* TestWorld = WeakWorld.Get();
			URoadNetworkSubsystem* Roads = TestWorld ? TestWorld->GetSubsystem<URoadNetworkSubsystem>() : nullptr;
			UBuildingManagerSubsystem* Buildings = TestWorld ? TestWorld->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
			if (!Roads || !Buildings) return;
			TArray<FBuildingInstance> Records; Buildings->GetAllBuildings(Records);
			int32 MinX = MAX_int32, MinY = MAX_int32, MaxX = 0, MaxY = 0;
			for (const FBuildingInstance& B : Records)
			{
				MinX = FMath::Min(MinX, B.GridCoords.X); MaxX = FMath::Max(MaxX, B.GridCoords.X);
				MinY = FMath::Min(MinY, B.GridCoords.Y); MaxY = FMath::Max(MaxY, B.GridCoords.Y);
			}
			const int32 InitialRoads = Roads->CountRoadCells();
			FAftershockRoadQuote Quote;
			const bool A = Roads->BuildRoad(FIntPoint(MinX - 1, MinY - 1), FIntPoint(MaxX + 1, MinY - 1), Quote);
			const bool B = Roads->BuildRoad(FIntPoint(MinX - 1, MinY - 1), FIntPoint(MinX - 1, MaxY + 1), Quote);
			const bool C = Roads->BuildRoad(FIntPoint(MinX - 1, MaxY + 1), FIntPoint(MaxX + 1, MaxY + 1), Quote);
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_ROAD_VISUAL_AUDIT explicitTestTransactions=1 initialRoads=%d finalRoads=%d segments=%d/%d/%d"), InitialRoads, Roads->CountRoadCells(), A, B, C);
		}), 1.f, false);
	}
	float ProfileOverviewValue=0.f;
	const bool bProfileViewOnly=RenderProfile.IsValid()&&FParse::Value(FCommandLine::Get(),TEXT("AftershockOverviewAudit="),ProfileOverviewValue);
	if(!bProfileViewOnly)
	{
	FTimerHandle CaptureTimer;
	World.GetTimerManager().SetTimer(CaptureTimer, FTimerDelegate::CreateLambda([WeakWorld]()
	{
		UWorld* AuditWorld = WeakWorld.Get();
		if (!IsActiveAuditWorld(AuditWorld)) return;
		APlayerController* PC = AuditWorld ? AuditWorld->GetFirstPlayerController() : nullptr;
		if (!PC || !PC->PlayerCameraManager)
		{
			UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_VISUAL_AUDIT_FAIL no runtime camera"));
			return;
		}
		const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PresentationValidation"));
		IFileManager::Get().MakeDirectory(*Folder, true);
		const FMinimalViewInfo RuntimeView = PC->PlayerCameraManager->GetCameraCacheView();
		const bool bSceneSaved = CaptureScene(*AuditWorld, RuntimeView, Folder / TEXT("studio-runtime-scene.png"));
		const UTerrainGridSubsystem* Terrain = AuditWorld->GetSubsystem<UTerrainGridSubsystem>();
		const UBuildingManagerSubsystem* Buildings = AuditWorld->GetSubsystem<UBuildingManagerSubsystem>();
		const UCrowdRenderSubsystem* Crowd = AuditWorld->GetSubsystem<UCrowdRenderSubsystem>();
		const UAftershockScenarioSubsystem* Scenario = AuditWorld->GetSubsystem<UAftershockScenarioSubsystem>();
		if (Crowd) Crowd->AuditCrowdVisibility();
		int32 ColliderSamples = 0;
		for (TActorIterator<AAftershockCrowdHostActor> It(AuditWorld); It && Terrain; ++It)
		{
			for (int32 Sample = 0; Sample < 5; ++Sample)
			{
				const FIntPoint Coord((Terrain->GridWidth - 1) * (Sample + 1) / 6, (Terrain->GridHeight - 1) * (5 - Sample) / 6);
				const FVector Center = Terrain->GridToWorld(Coord);
				FHitResult Hit;
				FCollisionQueryParams Query(SCENE_QUERY_STAT(AftershockRuntimeTerrainAudit), true);
				if (It->TerrainSurface->LineTraceComponent(Hit, Center + FVector(0, 0, 2000), Center - FVector(0, 0, 2000), Query)
					&& FMath::Abs(Hit.ImpactPoint.Z - Center.Z) < 0.5f) ++ColliderSamples;
			}
			break;
		}
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_TERRAIN_RUNTIME_COLLISION canonicalCentreSamples=%d/5"), ColliderSamples);
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_VISUAL_AUDIT sceneSaved=%s scene=%s camera=%s rotation=%s buildings=%d agents=%d seed=%d"),
			bSceneSaved ? TEXT("true") : TEXT("false"), *(Folder / TEXT("studio-runtime-scene.png")),
			*RuntimeView.Location.ToCompactString(), *RuntimeView.Rotation.ToCompactString(),
			Buildings ? Buildings->GetActiveBuildingsCount() : -1, Crowd ? Crowd->GetCurrentCrowdCount() : -1,
			Terrain ? Terrain->TerrainSeed : -1);
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_RUNTIME_SCENARIO_AUDIT prepared=%d index=%d authoredLots=%d assemblyCells=%d expectedSeed=%d actualSeed=%d startupLifecycleObserved=1"),
			Scenario && Scenario->IsPrepared(), Scenario ? Scenario->GetScenarioIndex() : -1,
			Scenario ? Scenario->GetDesignedLots().Num() : -1, Scenario ? Scenario->GetAssemblyCells().Num() : -1,
			Scenario ? Scenario->GetDefinition().Seed : -1, Terrain ? Terrain->TerrainSeed : -1);
		// This second file is a real game viewport capture including UMG when the
		// platform provides it. SceneCapture above is explicitly labelled scene-only.
		FScreenshotRequest::RequestScreenshot(Folder / TEXT("studio-runtime-hud.png"), true, false, false, FIntRect(), true);
	}), 8.0f, false);
	}
	else UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PROFILE_VIEW_CAPTURE_POLICY screenshotReadPixelsDuringCsv=0 finalMetadataAfterCsv=1 syntheticAgentIDsAndPlanningUnchanged=1"));
	if (bWeatherAudit || bGlobeAudit)
	{
		FTimerHandle ScenarioTimer;
		World.GetTimerManager().SetTimer(ScenarioTimer, FTimerDelegate::CreateLambda([WeakWorld, WeatherAudit, bWeatherAudit, bGlobeAudit]()
		{
			UWorld* TestWorld = WeakWorld.Get();
			if (!IsActiveAuditWorld(TestWorld)) return;
			if (bWeatherAudit)
			{
				const EDisasterType Type = WeatherAudit.Equals(TEXT("Flood"), ESearchCase::IgnoreCase) ? EDisasterType::Flood : WeatherAudit.Equals(TEXT("Tsunami"), ESearchCase::IgnoreCase) ? EDisasterType::Tsunami
					:WeatherAudit.Equals(TEXT("Fire"),ESearchCase::IgnoreCase)?EDisasterType::Fire:WeatherAudit.Equals(TEXT("Volcano"),ESearchCase::IgnoreCase)?EDisasterType::Volcano:WeatherAudit.Equals(TEXT("Drought"),ESearchCase::IgnoreCase)?EDisasterType::Drought:EDisasterType::Wind;
				if (UCrowdRenderSubsystem* Crowd = TestWorld->GetSubsystem<UCrowdRenderSubsystem>()) Crowd->BeginEmergencyEvacuation(Type);
				if (UDisasterSimulationSubsystem* Simulation = TestWorld->GetSubsystem<UDisasterSimulationSubsystem>())
				{
					Simulation->TriggerDisaster(Type, Type == EDisasterType::Tsunami ? 60.f : 70.f, 0.35f, true);
					if(Type==EDisasterType::Volcano)
					{
						// Only the opt-in render fixture moves: the mapped vent may be
						// kilometres outside the initial city camera's field of view.
						const auto* Grid=TestWorld->GetSubsystem<UTerrainGridSubsystem>();
						auto* Controller=TestWorld->GetFirstPlayerController();
						auto* Camera=Controller?Cast<AAftershockCameraPawn>(Controller->GetPawn()):nullptr;
						const auto& Snapshot=Simulation->GetLatestVisualSnapshot();
						double Latitude=0.,Longitude=0.;FText Error;
						const bool Mapped=Grid&&Snapshot.Revision>0&&Snapshot.Type==EDisasterType::Volcano
							&&Snapshot.SourceCoord==Grid->VolcanicVentCoord&&Grid->IsPlayableCoord(Snapshot.SourceCoord);
						const bool Focused=Mapped&&Camera&&Grid->TryWorldToGeographic(Grid->GridToWorld(Snapshot.SourceCoord),Latitude,Longitude)
							&&Camera->FocusGeographicLocation(Latitude,Longitude,Error)&&SetRenderAuditZoom(*TestWorld,120000.);
						UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_VOLCANO_AUDIT_CAMERA mapped=%d focused=%d source=(%d,%d) zoomCm=120000 fixtureOnly=1 canonicalWrites=0"),Mapped,Focused,Snapshot.SourceCoord.X,Snapshot.SourceCoord.Y);
					}
				}
			}
			if (bGlobeAudit)
			{
				APlayerController* PC = TestWorld->GetFirstPlayerController();
				if (AAftershockCameraPawn* Camera = PC ? Cast<AAftershockCameraPawn>(PC->GetPawn()) : nullptr) Camera->HandleToggleOverview();
			}
		}), 9.f, false);
		FTimerHandle PeakTimer;
		World.GetTimerManager().SetTimer(PeakTimer, FTimerDelegate::CreateLambda([WeakWorld, WeatherAudit, bGlobeAudit]()
		{
			UWorld* TestWorld = WeakWorld.Get();
			if (!IsActiveAuditWorld(TestWorld)) return;
			APlayerController* PC = TestWorld ? TestWorld->GetFirstPlayerController() : nullptr;
			if (!PC || !PC->PlayerCameraManager) return;
			const FString Name = bGlobeAudit ? TEXT("globe-overview") : WeatherAudit.ToLower()+TEXT("-peak");
			const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PresentationValidation"));
			const bool VolcanoAudit=WeatherAudit.Equals(TEXT("Volcano"),ESearchCase::IgnoreCase)&&!bGlobeAudit;
			const bool Saved = CaptureScene(*TestWorld, PC->PlayerCameraManager->GetCameraCacheView(), Folder / (Name + TEXT("-scene.png")),1600,VolcanoAudit?900:1000);
			FScreenshotRequest::RequestScreenshot(Folder / (Name + TEXT("-hud.png")), true, false, false, FIntRect(), true);
			const UDisasterPresentationSubsystem* Presentation = TestWorld->GetSubsystem<UDisasterPresentationSubsystem>();
			if(VolcanoAudit)
			{
				auto Metadata=ReadRenderAuditMetadata(*TestWorld,TEXT("volcano-peak"));
				const auto* Grid=TestWorld->GetSubsystem<UTerrainGridSubsystem>();
				const auto* Simulation=TestWorld->GetSubsystem<UDisasterSimulationSubsystem>();
				const auto* Snapshot=Simulation?&Simulation->GetLatestVisualSnapshot():nullptr;
				const bool Mapped=Grid&&Snapshot&&Snapshot->Revision>0&&Snapshot->Type==EDisasterType::Volcano&&Snapshot->SourceCoord==Grid->VolcanicVentCoord;
				const FVector Source=Mapped?Grid->GridToWorld(Snapshot->SourceCoord):FVector::ZeroVector;
				int32 ViewportWidth=0,ViewportHeight=0;PC->GetViewportSize(ViewportWidth,ViewportHeight);
				FVector2D VentPixel=FVector2D::ZeroVector,PlumePixel=FVector2D::ZeroVector;
				const bool VentProjected=Mapped&&UGameplayStatics::ProjectWorldToScreen(PC,Source,VentPixel,false);
				const bool PlumeProjected=Mapped&&UGameplayStatics::ProjectWorldToScreen(PC,Source+FVector(0.,0.,30000.),PlumePixel,false);
				const auto InFrame=[ViewportWidth,ViewportHeight](const FVector2D& Pixel){return !Pixel.ContainsNaN()&&ViewportWidth>0&&ViewportHeight>0&&Pixel.X>=0.&&Pixel.Y>=0.&&Pixel.X<=ViewportWidth&&Pixel.Y<=ViewportHeight;};
				const bool VentInFrame=VentProjected&&InFrame(VentPixel),PlumeInFrame=PlumeProjected&&InFrame(PlumePixel);
				bool VentMesh=false,LavaMesh=false,PlumeMesh=false;
				for(TActorIterator<AActor> It(TestWorld);It;++It)
				{
					const auto* Active=Cast<AAftershockDisasterPresentationActor>(*It);
					const auto* Passive=Cast<AAftershockEcologyStateActor>(*It);
					const auto* Stats=Active?&Active->GetVolcanoVisualStats():Passive?&Passive->GetRegionalVolcanoVisualStats():nullptr;
					if(!Mapped||!Stats||!Stats->SourceWorld.Equals(Source,5.f)||It->IsHidden())continue;
					TInlineComponentArray<UStaticMeshComponent*> Components;It->GetComponents(Components);
					for(const auto* Surface:Components)
					{
						const UStaticMesh* Mesh=Surface->GetStaticMesh().Get();const FStaticMeshRenderData* Data=Mesh?Mesh->GetRenderData():nullptr;
						if(!Surface->IsVisible()||!Data||Data->LODResources.IsEmpty()||Data->LODResources[0].GetNumTriangles()==0)continue;
						VentMesh|=Surface->GetName().Contains(TEXT("VolcanoVent"));LavaMesh|=Surface->GetName().Contains(TEXT("LavaVolume"));PlumeMesh|=Surface->GetName().Contains(TEXT("VolcanoWindBentPlume"));
					}
				}
				const bool ActualGeometry=VentMesh&&LavaMesh&&PlumeMesh;
				Metadata->SetBoolField(TEXT("scene_saved"),Saved);Metadata->SetBoolField(TEXT("actual_mapped_vent"),Mapped);
				Metadata->SetBoolField(TEXT("mapped_vent_in_frame"),VentInFrame);Metadata->SetBoolField(TEXT("plume_300m_top_in_frame"),PlumeInFrame);
				Metadata->SetBoolField(TEXT("actual_vent_lava_plume_meshes"),ActualGeometry);
				Metadata->SetNumberField(TEXT("vent_pixel_x"),VentPixel.X);Metadata->SetNumberField(TEXT("vent_pixel_y"),VentPixel.Y);
				Metadata->SetNumberField(TEXT("viewport_width"),ViewportWidth);Metadata->SetNumberField(TEXT("viewport_height"),ViewportHeight);
				const bool Valid=Saved&&Mapped&&VentInFrame&&PlumeInFrame&&ActualGeometry&&ViewportWidth==1600&&ViewportHeight==900;
				Metadata->SetBoolField(TEXT("valid"),Valid);const bool JsonSaved=SaveRenderAuditJson(Metadata,TEXT("volcano_runtime_audit.json"));
				UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_VOLCANO_FRAME_AUDIT valid=%d sceneSaved=%d ventInFrame=%d plumeTopInFrame=%d actualMeshes=%d jsonSaved=%d fixtureOnly=1 canonicalWrites=0"),Valid,Saved,VentInFrame,PlumeInFrame,ActualGeometry,JsonSaved);
			}
			if (bGlobeAudit)
			{
				if (const UTerrainGridSubsystem* Terrain = TestWorld->GetSubsystem<UTerrainGridSubsystem>())
				{
					const FAftershockGlobeLayout Layout = GetAftershockGlobeLayout(*Terrain);
					const double Distance = FVector::Distance(PC->PlayerCameraManager->GetCameraLocation(), Layout.Center);
					const AAftershockCameraPawn* Camera = Cast<AAftershockCameraPawn>(PC->GetPawn());
					UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_GLOBE_CAMERA_AUDIT cameraCentreDistanceCm=%.0f radiusCm=%.0f centre=%s outsideSphere=%d overviewBlend=%.3f sharedLayout=1"),
						Distance, Layout.RadiusCm, *Layout.Center.ToCompactString(), Distance > Layout.RadiusCm, Camera ? Camera->GetPlanetOverviewBlend() : -1.f);
				}
			}
			if (const UCrowdRenderSubsystem* Crowd = TestWorld->GetSubsystem<UCrowdRenderSubsystem>()) { Crowd->AuditCrowdVisibility(); Crowd->AuditCitizenPlanning(); }
			if (const UDisasterAudioBridgeSubsystem* Audio = TestWorld->GetSubsystem<UDisasterAudioBridgeSubsystem>())
			{
				UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_SCENARIO_AUDIO_AUDIT %s"), *Audio->DescribeSoundscape());
			}
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_SCENARIO_RENDER_AUDIT scenario=%s sceneSaved=%d camera=%s state=%s"), *Name, Saved,
				*PC->PlayerCameraManager->GetCameraLocation().ToCompactString(), Presentation ? *Presentation->DescribeCurrentVisuals() : TEXT("NO_PRESENTATION"));
		// Flood deliberately fills the river before its land-water envelope starts
		// at event age 6s. Capture at age 11s, not the zero-envelope 6s frame.
		// Only this opt-in audit timer changes; production presentation is untouched.
		}), bTsunamiAudit ? 26.f : WeatherAudit.Equals(TEXT("Flood"), ESearchCase::IgnoreCase) ? 20.f : 15.f, false);
	}
	const bool bRubbleSaveAudit=FParse::Param(FCommandLine::Get(),TEXT("AftershockRubbleSaveAudit"));
	if (bRubbleSaveAudit) BeginAftershockRubbleRuntimeAudit(World,[](UWorld& W,const FMinimalViewInfo& View,const FString& Path) { return CaptureScene(W,View,Path); });
	const bool bChaosAudit = FParse::Param(FCommandLine::Get(), TEXT("AftershockChaosAudit")) && !bRubbleSaveAudit;
	if (bChaosAudit)
	{
		FTimerHandle CollapseTimer;
		World.GetTimerManager().SetTimer(CollapseTimer, FTimerDelegate::CreateLambda([WeakWorld]()
		{
			UWorld* TestWorld = WeakWorld.Get();
			if (!IsActiveAuditWorld(TestWorld)) return;
			UBuildingManagerSubsystem* Buildings = TestWorld ? TestWorld->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
			UDisasterSimulationSubsystem* Disaster = TestWorld ? TestWorld->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
			if (!Buildings || !Disaster) return;
			TArray<FBuildingInstance> Records;
			Buildings->GetAllBuildings(Records);
			Records.Sort([](const FBuildingInstance& A, const FBuildingInstance& B) { return A.InstanceID < B.InstanceID; });
			const bool bAllTypes = FParse::Param(FCommandLine::Get(), TEXT("AftershockChaosAuditAllTypes"));
			TSet<FSoftObjectPath> TestedCollections;
			for (FBuildingInstance& Record : Records)
			{
				if (!Record.BuildingData || Record.BuildingData->MeshFractured.IsNull()) continue;
				const FSoftObjectPath CollectionPath = Record.BuildingData->MeshFractured.ToSoftObjectPath();
				if (TestedCollections.Contains(CollectionPath)) continue;
				// Explicit development test flag: this is an induced collapse to test
				// presentation, NOT evidence for hazard severity or casualty formulas.
				Buildings->ApplyDamageToBuilding(Record.InstanceID, 95.0f);
				Buildings->GetBuildingByID(Record.InstanceID, Record);
				Disaster->SpawnChaosDestructionForBuilding(Record);
				TestedCollections.Add(CollectionPath);
				UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CHAOS_AUDIT inducedCollapse=1 building=%d"), Record.InstanceID);
				if (!bAllTypes || TestedCollections.Num() >= 5) break;
			}
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CHAOS_AUDIT_COLLECTIONS uniqueAssets=%d allFiveRequested=%d"), TestedCollections.Num(), bAllTypes);
		}), 9.0f, false);
		FTimerHandle RubbleTimer;
		World.GetTimerManager().SetTimer(RubbleTimer, FTimerDelegate::CreateLambda([WeakWorld]()
		{
			UWorld* TestWorld = WeakWorld.Get();
			if (!IsActiveAuditWorld(TestWorld)) return;
			APlayerController* PC = TestWorld ? TestWorld->GetFirstPlayerController() : nullptr;
			if (!PC || !PC->PlayerCameraManager) return;
			const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PresentationValidation"));
			CaptureScene(*TestWorld, PC->PlayerCameraManager->GetCameraCacheView(), Folder / TEXT("studio-induced-collapse.png"));
			int32 Actors = 0;
			int32 Frozen = 0;
			for (TActorIterator<AGeometryCollectionActor> It(TestWorld); It; ++It)
			{
				++Actors;
				if (!It->GetGeometryCollectionComponent()->IsSimulatingPhysics()) ++Frozen;
			}
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CHAOS_AUDIT_RESULT geometryActors=%d frozenAfter4Seconds=%d"), Actors, Frozen);
		}), 14.5f, false);
	}
	FTimerHandle ExitTimer;
	const float ExistingExitSeconds=FParse::Param(FCommandLine::Get(),TEXT("AftershockCityLifeRuntimeAudit"))?95.f:(bTsunamiAudit ?45.f:(bWeatherAudit||bPhysicalFloodAudit)?36.f:((bGlobeAudit||bChaosAudit||bRubbleSaveAudit)?18.f:14.f));
	if(RenderProfile)
	{
		RenderProfile->MinimumRuntime=FMath::Max(RenderProfile->MinimumRuntime,ExistingExitSeconds);
		RenderProfile->DeadlineSeconds=FMath::Max(RenderProfile->DeadlineSeconds,RenderProfile->MinimumRuntime+5.f);
		const TSharedRef<FRenderProfileAuditState> State=RenderProfile.ToSharedRef();
		World.GetTimerManager().SetTimer(RenderProfile->MonitorTimer,FTimerDelegate::CreateLambda([WeakWorld,State](){PollRenderProfile(WeakWorld,State);}),.25f,true);
		return; // The monitor waits for completed CSV output before requesting exit.
	}
	World.GetTimerManager().SetTimer(ExitTimer, FTimerDelegate::CreateLambda([WeakWorld]()
	{
		// GameInstance timers can outlive level travel. An old world's deadline
		// must never terminate the new scenario before its own capture/exit time.
		if (!IsActiveAuditWorld(WeakWorld.Get())) return;
		const FString Path = FPaths::ProjectSavedDir() / TEXT("PresentationValidation/studio-runtime-hud.png");
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_VISUAL_AUDIT_END viewportCaptureSaved=%s"),
			IFileManager::Get().FileExists(*Path) ? TEXT("true") : TEXT("false"));
		FPlatformMisc::RequestExit(false);
	}), ExistingExitSeconds, false);
#endif
}
