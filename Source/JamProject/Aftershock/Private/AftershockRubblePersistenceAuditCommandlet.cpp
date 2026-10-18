#include "AftershockRubblePersistenceAuditCommandlet.h"
#if WITH_EDITOR
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Narrative/CasualtyLedgerGenerator.h"
#include "AftershockTerrainSurface.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionActor.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

namespace
{
	struct FRubbleCheck { FString Name; bool bPassed; };
	struct FRubbleWorld
	{
		UWorld* World=nullptr;
		~FRubbleWorld() { if (World) { World->DestroyWorld(false); if (GEngine) GEngine->DestroyWorldContext(World); World->RemoveFromRoot(); } }
	};
	bool EqualPose(const FTransform& A,const FTransform& B)
	{
		return A.GetTranslation().Equals(B.GetTranslation(),.05) && A.GetRotation().Equals(B.GetRotation(),.00001) && A.GetScale3D().Equals(B.GetScale3D(),.00001);
	}
	bool EqualRubble(const TArray<FAftershockRubbleSaveRecord>& A,const TArray<FAftershockRubbleSaveRecord>& B)
	{
		if (A.Num()!=B.Num()) return false;
		for (int32 I=0;I<A.Num();++I)
		{
			if (A[I].BuildingID!=B[I].BuildingID || A[I].Coord!=B[I].Coord || !EqualPose(A[I].ActorTransform,B[I].ActorTransform) || A[I].LocalRestTransforms.Num()!=B[I].LocalRestTransforms.Num()) return false;
			for (int32 J=0;J<A[I].LocalRestTransforms.Num();++J) if (!EqualPose(A[I].LocalRestTransforms[J],B[I].LocalRestTransforms[J])) return false;
		}
		return true;
	}
	bool ExactRubblePayload(const TArray<FAftershockRubbleSaveRecord>& A,const TArray<FAftershockRubbleSaveRecord>& B)
	{
		if(A.Num()!=B.Num())return false;
		for(int32 I=0;I<A.Num();++I)
		{
			if(A[I].BuildingID!=B[I].BuildingID||A[I].Coord!=B[I].Coord||A[I].FracturedAssetPath!=B[I].FracturedAssetPath
				||!A[I].ActorTransform.Equals(B[I].ActorTransform,0.f)||A[I].LocalRestTransforms.Num()!=B[I].LocalRestTransforms.Num())return false;
			for(int32 J=0;J<A[I].LocalRestTransforms.Num();++J)if(!A[I].LocalRestTransforms[J].Equals(B[I].LocalRestTransforms[J],0.f))return false;
		}
		return true;
	}
}

UAftershockRubblePersistenceAuditCommandlet::UAftershockRubblePersistenceAuditCommandlet()
{ IsClient=false; IsServer=false; IsEditor=true; LogToConsole=true; }

int32 UAftershockRubblePersistenceAuditCommandlet::Main(const FString& Params)
{
	TArray<FRubbleCheck> Checks; int32 Failed=0;
	const auto Check=[&](const TCHAR* Name,bool Passed)
	{
		Checks.Add({Name,Passed}); Failed+=Passed ? 0 : 1;
		if (Passed) { UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_RUBBLE_AUDIT_PASS %s"),Name); }
		else { UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_RUBBLE_AUDIT_FAIL %s"),Name); }
	};
	const auto Finish=[&]()
	{
		const FString Folder=FPaths::ProjectSavedDir()/TEXT("PresentationValidation"); IFileManager::Get().MakeDirectory(*Folder,true);
		FString Json=FString::Printf(TEXT("{\n\"schema_version\":1,\n\"scope\":\"Native five authored Geometry Collections, canonical collapsed buildings, explicit finite posed-fragment fixture, save/restore equality, proxy/tick and side effects. Does not run a collapse solver, inspect pixels, time GPU, or prove packaged load.\",\n\"passed\":%d,\n\"failed\":%d,\n\"checks\":[\n"),Checks.Num()-Failed,Failed);
		for (int32 I=0;I<Checks.Num();++I) Json+=FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}%s\n"),*Checks[I].Name,Checks[I].bPassed ? TEXT("true") : TEXT("false"),I+1<Checks.Num() ? TEXT(",") : TEXT(""));
		Json+=TEXT("]\n}\n");
		if (!FFileHelper::SaveStringToFile(Json,*(Folder/TEXT("rubble_persistence_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return 2;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_RUBBLE_AUDIT_RESULT passed=%d failed=%d"),Checks.Num()-Failed,Failed); return Failed==0 ? 0 : 1;
	};
	Check(TEXT("engine_initialized"),GEngine!=nullptr); if (!GEngine) return Finish();
	FRubbleWorld Fixture;
	Fixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("AftershockRubblePersistenceFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("isolated_world_created"),Fixture.World!=nullptr); if (!Fixture.World) return Finish();
	UWorld* World=Fixture.World; GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	UTerrainGridSubsystem* Terrain=World->GetSubsystem<UTerrainGridSubsystem>(); UBuildingManagerSubsystem* Buildings=World->GetSubsystem<UBuildingManagerSubsystem>();
	UDisasterSimulationSubsystem* Disaster=World->GetSubsystem<UDisasterSimulationSubsystem>(); UEconomyAndStateSubsystem* Economy=World->GetSubsystem<UEconomyAndStateSubsystem>();
	Check(TEXT("canonical_subsystems_ready"),Terrain && Buildings && Disaster && Economy); if (!Terrain || !Buildings || !Disaster || !Economy) return Finish();
	Terrain->GenerateProceduralTerrain(20,12,500.f,1337);
	for (FGridCell& Cell:Terrain->Cells) { const FIntPoint Coord=Cell.Coords; Cell=FGridCell(); Cell.Coords=Coord; Cell.bIsPlayable=true; Cell.Elevation=100.f; }
	const EBuildingType Types[]={EBuildingType::House,EBuildingType::Apartment,EBuildingType::Office,EBuildingType::Factory,EBuildingType::Hospital};
	TArray<FAftershockRubbleSaveRecord> Requested; bool bSetup=true; TSet<FSoftObjectPath> Collections;
	for (int32 I=0;I<5;++I)
	{
		UBuildingData* Data=Buildings->GetCatalogData(Types[I]); int32 ID=INDEX_NONE; const FIntPoint Coord(3+I*3,5);
		UGeometryCollection* Asset=Data ? Data->MeshFractured.LoadSynchronous() : nullptr;
		const TSharedPtr<FGeometryCollection,ESPMode::ThreadSafe> Collection=Asset ? Asset->GetGeometryCollection() : nullptr;
		if (!Data || !Collection || !Buildings->PlaceBuilding(Coord,Data,EFoundationQuality::Medium,Data->FloorsMin,ID)) { bSetup=false; continue; }
		Buildings->ApplyDamageToBuilding(ID,95.f);
		FAftershockRubbleSaveRecord Record; Record.BuildingID=ID; Record.Coord=Coord; Record.ActorTransform=FTransform(Terrain->GridToWorld(Coord));
		TArray<FTransform> Global; GeometryCollectionAlgo::GlobalMatrices(Collection->Transform,Collection->Parent,Global);
		const TManagedArray<FTransform>* MassToLocal=Collection->FindAttribute<FTransform>("MassToLocal",FGeometryCollection::TransformGroup);
		for (int32 J=0;J<Global.Num();++J)
		{
			FTransform Pose=(MassToLocal ? (*MassToLocal)[J] : FTransform::Identity)*Global[J];
			if (Collection->SimulationType[J]==FGeometryCollection::FST_Rigid) Pose.AddToTranslation(FVector((J%3-1)*11.f,(J%5-2)*7.f,(J%2)*4.f));
			Record.LocalRestTransforms.Add(Pose);
		}
		Requested.Add(MoveTemp(Record)); Collections.Add(Data->MeshFractured.ToSoftObjectPath());
	}
	Check(TEXT("five_distinct_authored_gc_assets_have_canonical_collapsed_buildings"),bSetup && Requested.Num()==5 && Collections.Num()==5);
	if (Requested.Num()!=5) return Finish();
	TStrongObjectPtr<UAftershockRubbleAuditListener> Listener(NewObject<UAftershockRubbleAuditListener>(World));
	Disaster->OnDisasterCompleted.AddDynamic(Listener.Get(),&UAftershockRubbleAuditListener::Completed);
	FAftershockEconomySaveState EconomyBefore; Economy->CaptureSaveState(EconomyBefore);
	const UCityLifeSubsystem* Life=World->GetSubsystem<UCityLifeSubsystem>();
	const int32 DeathsBefore=Life ? Life->GetState().TotalDeaths : 0,PopulationBefore=Life ? Life->GetState().LivingPopulation : 0;
	TArray<FBuildingInstance> BuildingsBefore; Buildings->GetAllBuildings(BuildingsBefore);
	const auto UnchangedGameplay=[&]()
	{
		FAftershockEconomySaveState Now; Economy->CaptureSaveState(Now);
		bool Same=Now.Treasury==EconomyBefore.Treasury && Now.Turn==EconomyBefore.Turn && Now.Satisfaction==EconomyBefore.Satisfaction && Now.PendingAid==EconomyBefore.PendingAid && Now.SurvivedDisasters==EconomyBefore.SurvivedDisasters && Now.AppliedDisasterEventIDs==EconomyBefore.AppliedDisasterEventIDs && Listener->Completions==0
			&& Life && Life->GetState().TotalDeaths==DeathsBefore && Life->GetState().LivingPopulation==PopulationBefore;
		for (const FBuildingInstance& Before:BuildingsBefore) { FBuildingInstance Current; Same&=Buildings->GetBuildingByID(Before.InstanceID,Current) && Current.CurrentDamage==Before.CurrentDamage && Current.DamageState==Before.DamageState; }
		return Same;
	};
	FText Error;
	Check(TEXT("valid_five_collection_pose_restore_succeeds"),Disaster->RestoreRubbleSaveState(Requested,Error));
	TArray<FAftershockRubbleSaveRecord> Captured; Disaster->CaptureRubbleSaveState(Captured);
	Check(TEXT("native_get_current_transforms_matches_every_requested_fragment"),EqualRubble(Requested,Captured));
	bool Frozen=true; int32 ActorCount=0;
	for (TActorIterator<AGeometryCollectionActor> It(World);It;++It)
	{
		if (It->IsActorBeingDestroyed()) continue; ++ActorCount;
		const UGeometryCollectionComponent* Component=It->GetGeometryCollectionComponent();
		Frozen&=Component && !It->IsActorTickEnabled() && !Component->IsComponentTickEnabled() && !Component->IsSimulatingPhysics() && Component->GetPhysicsProxy()==nullptr && !Component->IsPhysicsStateCreated();
	}
	Check(TEXT("five_persistent_actors_have_no_tick_or_physics_proxy"),ActorCount==5 && Frozen);
	Check(TEXT("restore_does_not_reapply_damage_deaths_money_or_completion"),UnchangedGameplay());
	Check(TEXT("capture_restore_recapture_is_idempotent"),Disaster->RestoreRubbleSaveState(Captured,Error));
	TArray<FAftershockRubbleSaveRecord> Again; Disaster->CaptureRubbleSaveState(Again);
	Check(TEXT("second_native_roundtrip_preserves_all_frozen_poses"),EqualRubble(Captured,Again));
	{
		// A real camera and installed terrain host exercise render out/in/out.
		// No hazard is triggered and no fragment transform is manufactured here.
		FText WindowError;const bool Expanded=Terrain->EnsureChunkGenerated(FIntPoint(-3,0),WindowError);
		auto* PC=World->SpawnActor<APlayerController>();auto* Camera=World->SpawnActor<AAftershockCameraPawn>();if(PC){World->AddController(PC);if(Camera){PC->Possess(Camera);Camera->InitializeCameraForGrid(Terrain->GridWidth,Terrain->GridHeight,Terrain->CellSize);}}
		Check(TEXT("headless_rubble_fixture_uses_registered_actual_first_controller"),PC&&World->GetFirstPlayerController()==PC);
		auto* Crowd=World->GetSubsystem<UCrowdRenderSubsystem>();auto* Ledger=World->GetSubsystem<UCasualtyLedgerGenerator>();
		FAftershockCasualtyLedgerSaveState LedgerBefore;if(Ledger)Ledger->CaptureSaveState(LedgerBefore);
		TArray<FAftershockRubbleSaveRecord> MaskBaseline=Again;
		const auto StableFrozenAndPayload=[&]()
		{
			TArray<FAftershockRubbleSaveRecord> Now;Disaster->CaptureRubbleSaveState(Now);bool Stable=ExactRubblePayload(MaskBaseline,Now)&&UnchangedGameplay();
			FAftershockCasualtyLedgerSaveState Current;if(Ledger)Ledger->CaptureSaveState(Current);Stable&=Ledger&&FAftershockCasualtyLedgerSaveState::StaticStruct()->CompareScriptStruct(&LedgerBefore,&Current,0);
			for(TActorIterator<AGeometryCollectionActor> It(World);It;++It)if(!It->IsActorBeingDestroyed()){auto* Component=It->GetGeometryCollectionComponent();Stable&=Component&&Component->GetPhysicsProxy()==nullptr&&!Component->IsPhysicsStateCreated()&&!Component->IsSimulatingPhysics()&&Component->GetCollisionEnabled()==ECollisionEnabled::NoCollision&&!It->IsActorTickEnabled()&&!Component->IsComponentTickEnabled();}
			return Stable;
		};
		const auto MoveDrawWindow=[&](FIntPoint Focus,bool bExpectedDraw)
		{
			if(!Expanded||!PC||!Camera||!Crowd)return false;
			FAftershockCameraSaveState View;Camera->CaptureSaveState(View);View.LocalFocus=Terrain->GridToWorld(Focus);View.Location=View.LocalFocus;
			if(!Camera->RestoreSaveState(View,WindowError))return false;Crowd->EnsureTerrainVisualBuilt();Disaster->RefreshVisibleRubblePresentation();
			const FIntRect Drawable=GetAftershockDrawableCellBounds(*Terrain);bool Correct=true;int32 Count=0;
			for(TActorIterator<AGeometryCollectionActor> It(World);It;++It)if(!It->IsActorBeingDestroyed())
			{
				++Count;auto* Component=It->GetGeometryCollectionComponent();const FIntPoint Coord=Terrain->WorldToGrid(It->GetActorLocation());
				Correct&=Drawable.Contains(Coord)==bExpectedDraw&&Component&&Component->IsVisible()==bExpectedDraw&&Component->bHiddenInGame!=bExpectedDraw&&!It->IsHidden();
			}
			return Count==5&&Correct&&StableFrozenAndPayload();
		};
		Check(TEXT("off_window_frozen_rubble_hidden_without_pose_save_or_financial_changes"),MoveDrawWindow(FIntPoint(-65,5),false));
		Check(TEXT("installed_window_return_shows_same_native_rubble_without_resimulation"),MoveDrawWindow(FIntPoint(10,5),true));
		Check(TEXT("second_window_exit_hides_same_five_rubble_without_canonical_changes"),MoveDrawWindow(FIntPoint(-65,5),false));
		const bool RestoredOffscreen=Disaster->RestoreRubbleSaveState(Again,WindowError);bool AllHidden=RestoredOffscreen;int32 HiddenCount=0;
		for(TActorIterator<AGeometryCollectionActor> It(World);It;++It)if(!It->IsActorBeingDestroyed()){++HiddenCount;auto* Component=It->GetGeometryCollectionComponent();AllHidden&=Component&&!Component->IsVisible()&&Component->bHiddenInGame&&!It->IsHidden();}
		TArray<FAftershockRubbleSaveRecord> Restored;Disaster->CaptureRubbleSaveState(Restored);bool SameAssets=Restored.Num()==Again.Num();for(int32 I=0;SameAssets&&I<Restored.Num();++I)SameAssets&=Restored[I].FracturedAssetPath==Again[I].FracturedAssetPath&&Restored[I].ActorTransform.Equals(Again[I].ActorTransform,0.f);
		// Native paired mass/local transforms cross float render storage on import;
		// retain the existing roundtrip tolerance. Subsequent visibility-only work
		// must preserve the post-import payload EXACTLY, not within that tolerance.
		const bool SameNativeRestore=EqualRubble(Again,Restored)&&SameAssets;MaskBaseline=MoveTemp(Restored);
		Check(TEXT("off_window_restore_masks_first_published_frame_and_preserves_native_save_pose"),AllHidden&&HiddenCount==5&&SameNativeRestore&&StableFrozenAndPayload());
		Check(TEXT("restored_off_window_rubble_reappears_when_ground_is_installed"),MoveDrawWindow(FIntPoint(10,5),true));
		if(PC)PC->Destroy();if(Camera)Camera->Destroy();Disaster->RefreshVisibleRubblePresentation(true);
	}
	const auto RejectWithoutMutation=[&](const TCHAR* Name,const TArray<FAftershockRubbleSaveRecord>& Invalid)
	{
		const bool Rejected=!Disaster->RestoreRubbleSaveState(Invalid,Error); TArray<FAftershockRubbleSaveRecord> After; Disaster->CaptureRubbleSaveState(After);
		Check(Name,Rejected && !Error.IsEmpty() && EqualRubble(Again,After) && UnchangedGameplay());
	};
	TArray<FAftershockRubbleSaveRecord> Invalid=Again; Invalid[0].BuildingID=MAX_int32; RejectWithoutMutation(TEXT("unknown_building_id_rejected_before_mutation"),Invalid);
	Invalid=Again; Invalid[0].Coord+=FIntPoint(1,0); RejectWithoutMutation(TEXT("mismatched_canonical_coordinate_rejected_before_mutation"),Invalid);
	Invalid=Again; Invalid[0].LocalRestTransforms.RemoveAt(0); RejectWithoutMutation(TEXT("wrong_fragment_count_rejected_before_mutation"),Invalid);
	Invalid=Again; const FAftershockRubbleSaveRecord Duplicate=Invalid[0]; Invalid.Add(Duplicate); RejectWithoutMutation(TEXT("duplicate_building_coordinate_rejected_before_mutation"),Invalid);
	Invalid=Again; FMemory::Memset(&Invalid[0].LocalRestTransforms[0],0xFF,sizeof(FTransform)); RejectWithoutMutation(TEXT("nonfinite_fragment_transform_rejected_before_mutation"),Invalid);
	Invalid=Again; FMemory::Memset(&Invalid[0].ActorTransform,0xFF,sizeof(FTransform)); RejectWithoutMutation(TEXT("nonfinite_actor_transform_rejected_before_mutation"),Invalid);
	int32 VisualEvents=0; const FDelegateHandle Handle=Disaster->OnDisasterVisualSnapshotChanged.AddLambda([&](const FAftershockDisasterVisualSnapshot&) { ++VisualEvents; });
	FAftershockDisasterVisualSnapshot Visual; Visual.Revision=42; Visual.Type=EDisasterType::Flood; Visual.DurationSeconds=24.f; Visual.BaseSeverity=30.f; Visual.EffectiveIntensity=30.f; Visual.LocalSeverity.Add(FIntPoint(3,5),20.f);
	const bool bVisual=Disaster->RestoreVisualSaveState(Visual,0.f); const auto Expired=Disaster->GetLatestVisualSnapshot();
	Check(TEXT("expired_visual_is_remapped_expired_without_completion_or_rewards"),bVisual && VisualEvents==1 && Expired.Revision==42 && World->GetTimeSeconds()-Expired.StartedAtWorldSeconds>=24.f && UnchangedGameplay());
	Visual.Type=EDisasterType::Earthquake; Visual.DurationSeconds=0.f;
	Check(TEXT("expired_quake_audio_window_stays_expired"),Disaster->RestoreVisualSaveState(Visual,0.f) && World->GetTimeSeconds()-Disaster->GetLatestVisualSnapshot().StartedAtWorldSeconds>=10.f && UnchangedGameplay());
	Visual.DurationSeconds=14.f;
	const bool bLongQuake=Disaster->RestoreVisualSaveState(Visual,12.f);
	const FAftershockDisasterVisualSnapshot LongQuake=Disaster->GetLatestVisualSnapshot();
	const float LongQuakeRemaining=LongQuake.GetEffectiveDurationSeconds()-(World->GetTimeSeconds()-LongQuake.StartedAtWorldSeconds);
	Check(TEXT("fourteen_second_quake_accepts_twelve_seconds_remaining"),bLongQuake && LongQuake.DurationSeconds==14.f && FMath::IsNearlyEqual(LongQuake.GetEffectiveDurationSeconds(),14.f) && FMath::IsNearlyEqual(LongQuakeRemaining,12.f) && UnchangedGameplay());
	const bool bLongQuakeRoundtrip=Disaster->RestoreVisualSaveState(LongQuake,LongQuakeRemaining);
	const FAftershockDisasterVisualSnapshot RoundtripQuake=Disaster->GetLatestVisualSnapshot();
	Check(TEXT("fourteen_second_quake_roundtrip_preserves_event_clock_without_replay"),bLongQuakeRoundtrip && RoundtripQuake.Revision==LongQuake.Revision && RoundtripQuake.DurationSeconds==14.f && FMath::IsNearlyEqual(RoundtripQuake.StartedAtWorldSeconds,LongQuake.StartedAtWorldSeconds) && FMath::IsNearlyEqual(RoundtripQuake.GetEffectiveDurationSeconds()-(World->GetTimeSeconds()-RoundtripQuake.StartedAtWorldSeconds),12.f) && UnchangedGameplay());
	Visual.DurationSeconds=0.f;
	const bool bLegacyQuake=Disaster->RestoreVisualSaveState(Visual,10.f);
	const FAftershockDisasterVisualSnapshot LegacyQuake=Disaster->GetLatestVisualSnapshot();
	Check(TEXT("legacy_zero_duration_quake_retains_full_ten_second_window"),bLegacyQuake && LegacyQuake.DurationSeconds==0.f && LegacyQuake.GetEffectiveDurationSeconds()==10.f && FMath::IsNearlyEqual(LegacyQuake.StartedAtWorldSeconds,static_cast<float>(World->GetTimeSeconds())) && UnchangedGameplay());
	const int32 BeforeInvalidVisual=VisualEvents; const float NaN=std::numeric_limits<float>::quiet_NaN();
	const bool bTimeRejected=!Disaster->RestoreVisualSaveState(Visual,NaN);
	Visual.LocalSeverity[FIntPoint(3,5)]=NaN;
	Check(TEXT("nonfinite_visual_input_rejected_without_broadcast_or_revision"),bTimeRejected && !Disaster->RestoreVisualSaveState(Visual,1.f) && VisualEvents==BeforeInvalidVisual && Disaster->GetLatestVisualSnapshot().Revision==42 && UnchangedGameplay());
	Disaster->OnDisasterVisualSnapshotChanged.Remove(Handle);
	return Finish();
}
#else
UAftershockRubblePersistenceAuditCommandlet::UAftershockRubblePersistenceAuditCommandlet() {}
int32 UAftershockRubblePersistenceAuditCommandlet::Main(const FString&) { return 1; }
#endif
