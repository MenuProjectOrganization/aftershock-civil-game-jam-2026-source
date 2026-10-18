#include "AftershockRubbleRuntimeAudit.h"
#if !UE_BUILD_SHIPPING
#include "AftershockRubblePersistenceAuditCommandlet.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GeometryCollection/GeometryCollectionActor.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	struct FRubbleRuntimeState
	{
		TArray<FAftershockRubbleSaveRecord> Poses;
		TMap<int32,float> Damage;
		FAftershockEconomySaveState Economy;
		int32 Deaths=0,Population=0;
		TStrongObjectPtr<UAftershockRubbleAuditListener> Listener;
		bool bCollapsedFive=false,bRestored=false,bEqual=false,bNoGameplayChange=false;
	};
	bool ValidRubbleWorld(UWorld* World)
	{
		return IsValid(World) && !World->bIsTearingDown && GEngine && GEngine->GetWorldContextFromWorld(World);
	}
	bool SameRuntimePoses(const TArray<FAftershockRubbleSaveRecord>& A,const TArray<FAftershockRubbleSaveRecord>& B)
	{
		if (A.Num()!=B.Num()) return false;
		for (int32 I=0;I<A.Num();++I)
		{
			if (A[I].BuildingID!=B[I].BuildingID || A[I].Coord!=B[I].Coord || !A[I].ActorTransform.Equals(B[I].ActorTransform,.00001) || A[I].LocalRestTransforms.Num()!=B[I].LocalRestTransforms.Num()) return false;
			for (int32 J=0;J<A[I].LocalRestTransforms.Num();++J)
			{
				const FTransform& X=A[I].LocalRestTransforms[J]; const FTransform& Y=B[I].LocalRestTransforms[J];
				if (!X.GetTranslation().Equals(Y.GetTranslation(),.05) || !X.GetRotation().Equals(Y.GetRotation(),.00001) || !X.GetScale3D().Equals(Y.GetScale3D(),.00001)) return false;
			}
		}
		return true;
	}
	bool NoRuntimeGameplayChange(UWorld& World,const FRubbleRuntimeState& State)
	{
		const auto* Economy=World.GetSubsystem<UEconomyAndStateSubsystem>(); const auto* Buildings=World.GetSubsystem<UBuildingManagerSubsystem>();
		const auto* Life=World.GetSubsystem<UCityLifeSubsystem>();
		if (!Economy || !Buildings || !Life || !State.Listener.IsValid()) return false;
		FAftershockEconomySaveState Current; Economy->CaptureSaveState(Current);
		bool Same=Current.Treasury==State.Economy.Treasury && Current.Turn==State.Economy.Turn && Current.Satisfaction==State.Economy.Satisfaction
			&& Current.PendingAid==State.Economy.PendingAid && Current.SurvivedDisasters==State.Economy.SurvivedDisasters && State.Listener->Completions==0
			&& Life->GetState().TotalDeaths==State.Deaths && Life->GetState().LivingPopulation==State.Population;
		for (const auto& Pair:State.Damage) { FBuildingInstance Record; Same&=Buildings->GetBuildingByID(Pair.Key,Record) && Record.CurrentDamage==Pair.Value; }
		return Same;
	}
}
#endif

void BeginAftershockRubbleRuntimeAudit(UWorld& World,FAftershockRubbleSceneCapture CaptureScene)
{
#if !UE_BUILD_SHIPPING
	const TWeakObjectPtr<UWorld> WeakWorld(&World); const auto State=MakeShared<FRubbleRuntimeState>();
	FTimerHandle CollapseTimer;
	World.GetTimerManager().SetTimer(CollapseTimer,FTimerDelegate::CreateLambda([WeakWorld,State]()
	{
		UWorld* W=WeakWorld.Get(); if (!ValidRubbleWorld(W)) return;
		auto* Buildings=W->GetSubsystem<UBuildingManagerSubsystem>(); auto* Disaster=W->GetSubsystem<UDisasterSimulationSubsystem>(); if (!Buildings || !Disaster) return;
		TArray<FBuildingInstance> Records; Buildings->GetAllBuildings(Records); Records.Sort([](const FBuildingInstance& A,const FBuildingInstance& B) { return A.InstanceID<B.InstanceID; });
		TSet<FSoftObjectPath> Paths;
		for (FBuildingInstance& Record:Records)
		{
			if (!Record.BuildingData || Record.BuildingData->MeshFractured.IsNull() || Paths.Contains(Record.BuildingData->MeshFractured.ToSoftObjectPath())) continue;
			Buildings->ApplyDamageToBuilding(Record.InstanceID,95.f); Buildings->GetBuildingByID(Record.InstanceID,Record); Disaster->SpawnChaosDestructionForBuilding(Record);
			Paths.Add(Record.BuildingData->MeshFractured.ToSoftObjectPath()); if (Paths.Num()==5) break;
		}
		State->bCollapsedFive=Paths.Num()==5;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_RUBBLE_RUNTIME_COLLAPSE uniqueCollections=%d inducedVisualFixture=1 hazardMathChanged=0"),Paths.Num());
	}),9.f,false);
	FTimerHandle RestoreTimer;
	World.GetTimerManager().SetTimer(RestoreTimer,FTimerDelegate::CreateLambda([WeakWorld,State]()
	{
		UWorld* W=WeakWorld.Get(); if (!ValidRubbleWorld(W)) return;
		auto* Disaster=W->GetSubsystem<UDisasterSimulationSubsystem>(); auto* Economy=W->GetSubsystem<UEconomyAndStateSubsystem>(); auto* Buildings=W->GetSubsystem<UBuildingManagerSubsystem>();
		if (!Disaster || !Economy || !Buildings) return;
		Disaster->CaptureRubbleSaveState(State->Poses); Economy->CaptureSaveState(State->Economy);
		if (const auto* Life=W->GetSubsystem<UCityLifeSubsystem>()) { State->Deaths=Life->GetState().TotalDeaths; State->Population=Life->GetState().LivingPopulation; }
		TArray<FBuildingInstance> Records; Buildings->GetAllBuildings(Records); for (const auto& Record:Records) State->Damage.Add(Record.InstanceID,Record.CurrentDamage);
		State->Listener.Reset(NewObject<UAftershockRubbleAuditListener>(W)); Disaster->OnDisasterCompleted.AddDynamic(State->Listener.Get(),&UAftershockRubbleAuditListener::Completed);
		FText Error; State->bRestored=State->Poses.Num()==5 && Disaster->RestoreRubbleSaveState(State->Poses,Error);
		TArray<FAftershockRubbleSaveRecord> After; Disaster->CaptureRubbleSaveState(After); State->bEqual=SameRuntimePoses(State->Poses,After); State->bNoGameplayChange=NoRuntimeGameplayChange(*W,*State);
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_RUBBLE_RUNTIME_RESTORE capturedActors=%d restored=%d equalNativePoses=%d gameplayUnchanged=%d error=%s"),State->Poses.Num(),State->bRestored,State->bEqual,State->bNoGameplayChange,*Error.ToString());
	}),14.f,false);
	FTimerHandle CaptureTimer;
	World.GetTimerManager().SetTimer(CaptureTimer,FTimerDelegate::CreateLambda([WeakWorld,State,CaptureScene=MoveTemp(CaptureScene)]()
	{
		UWorld* W=WeakWorld.Get(); if (!ValidRubbleWorld(W)) return;
		auto* Disaster=W->GetSubsystem<UDisasterSimulationSubsystem>(); if (!Disaster) return;
		TArray<FAftershockRubbleSaveRecord> Again; Disaster->CaptureRubbleSaveState(Again);
		int32 Actors=0,Fragments=0; bool Frozen=true;
		for (TActorIterator<AGeometryCollectionActor> It(W);It;++It)
		{
			if (It->IsActorBeingDestroyed()) continue; ++Actors; UGeometryCollectionComponent* Component=It->GetGeometryCollectionComponent();
			Frozen&=Component && !It->IsActorTickEnabled() && !Component->IsComponentTickEnabled() && !Component->IsSimulatingPhysics() && !Component->IsPhysicsStateCreated() && Component->GetPhysicsProxy()==nullptr;
			if (Component) Fragments+=Component->GetCurrentTransforms().Num();
		}
		const FString Folder=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation")); IFileManager::Get().MakeDirectory(*Folder,true);
		APlayerController* PC=W->GetFirstPlayerController(); const FString Path=Folder/TEXT("studio-rubble-restored.png");
		const bool SceneSaved=PC && PC->PlayerCameraManager && CaptureScene(*W,PC->PlayerCameraManager->GetCameraCacheView(),Path);
		const bool Passed=State->bCollapsedFive && State->bRestored && State->bEqual && State->bNoGameplayChange && Actors==5 && Frozen && SameRuntimePoses(State->Poses,Again) && NoRuntimeGameplayChange(*W,*State) && SceneSaved;
		if (Passed) { UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_RUBBLE_RUNTIME_RESULT passed=1 actors=%d fragmentTransforms=%d nativePoseEqual=1 actorTicks=0 componentTicks=0 proxies=0 gameplayUnchanged=1 sceneSaved=1 scene=%s"),Actors,Fragments,*Path); }
		else { UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_RUBBLE_RUNTIME_RESULT passed=0 actors=%d fragmentTransforms=%d collapsedFive=%d restored=%d nativePoseEqual=%d frozen=%d gameplayUnchanged=%d sceneSaved=%d"),Actors,Fragments,State->bCollapsedFive,State->bRestored,SameRuntimePoses(State->Poses,Again),Frozen,NoRuntimeGameplayChange(*W,*State),SceneSaved); }
	}),15.f,false);
#endif
}
