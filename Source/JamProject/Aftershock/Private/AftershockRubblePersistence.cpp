#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/AftershockScaledChaos.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "Engine/World.h"

void UDisasterSimulationSubsystem::CaptureRubbleSaveState(TArray<FAftershockRubbleSaveRecord>& OutState) const
{
	OutState.Reset();
	const UBuildingManagerSubsystem* Buildings=GetWorld()?GetWorld()->GetSubsystem<UBuildingManagerSubsystem>():nullptr;
	if(!Buildings)return;
	for(const auto& Pair:CollapsedRubbleActors)
	{
		const AActor* Actor=Pair.Value;
		UGeometryCollectionComponent* Component=IsValid(Actor)?Actor->FindComponentByClass<UGeometryCollectionComponent>():nullptr;
		FBuildingInstance Building;
		if(!Component||!Buildings->GetBuildingAtCoords(Pair.Key,Building)||Building.DamageState!=EDamageState::Collapsed)continue;
		FAftershockRubbleSaveRecord& Record=OutState.AddDefaulted_GetRef();
		Record.BuildingID=Building.InstanceID;Record.Coord=Pair.Key;Record.ActorTransform=Actor->GetActorTransform();
		if(const UGeometryCollection* Rest=Component->GetRestCollection())Record.FracturedAssetPath=FSoftObjectPath(Rest);
		// Native paired APIs include MassToLocal compensation and preserve all
		// actual fragment poses, instead of reapplying the disaster after loading.
		Record.LocalRestTransforms=Component->GetCurrentTransforms();
	}
	OutState.Sort([](const auto& A,const auto& B){return A.BuildingID<B.BuildingID;});
}

bool UDisasterSimulationSubsystem::RestoreRubbleSaveState(const TArray<FAftershockRubbleSaveRecord>& State,FText& OutError)
{
	UWorld* World=GetWorld();UBuildingManagerSubsystem* Buildings=World?World->GetSubsystem<UBuildingManagerSubsystem>():nullptr;
	TSet<FIntPoint> Seen;TArray<UGeometryCollection*> Assets;
	bool bValid=World&&Buildings&&State.Num()<=4096;
	for(const FAftershockRubbleSaveRecord& Record:State)
	{
		FBuildingInstance Building;
		bool bRecordValid=Buildings&&Buildings->GetBuildingByID(Record.BuildingID,Building)&&Building.GridCoords==Record.Coord
			&& Building.DamageState==EDamageState::Collapsed&&Building.BuildingData&&!Seen.Contains(Record.Coord)&&Record.ActorTransform.IsValid();
		FText AssetError;
		if(bRecordValid)bRecordValid=AftershockScaledChaos::IsValidSavedFracturedAssetReference(Building,Record.FracturedAssetPath,Record.ActorTransform,AssetError);
		UGeometryCollection* Asset=bRecordValid?AftershockScaledChaos::ResolveSavedFracturedAsset(Building,Record.FracturedAssetPath,AssetError):nullptr;
		const TSharedPtr<FGeometryCollection,ESPMode::ThreadSafe> Collection=Asset?Asset->GetGeometryCollection():nullptr;
		bRecordValid &= Collection&&Record.LocalRestTransforms.Num()==Collection->Transform.Num()&&Record.LocalRestTransforms.Num()<=4096;
		for(const FTransform& Transform:Record.LocalRestTransforms)bRecordValid &= Transform.IsValid();
		bValid &= bRecordValid;Seen.Add(Record.Coord);Assets.Add(Asset);
	}
	if(!bValid){OutError=NSLOCTEXT("Aftershock","InvalidSavedRubble","Enkaz kaydı yapı veya parçalanmış model ile uyuşmuyor.");return false;}
	TMap<FIntPoint,TObjectPtr<AActor>> Staged;
	for(int32 I=0;I<State.Num();++I)
	{
		const FAftershockRubbleSaveRecord& Record=State[I];
		AGeometryCollectionActor* Actor=World->SpawnActorDeferred<AGeometryCollectionActor>(AGeometryCollectionActor::StaticClass(),Record.ActorTransform,nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		UGeometryCollectionComponent* Component=Actor?Actor->GetGeometryCollectionComponent():nullptr;
		if(!Actor||!Component)
		{
			if(Actor)Actor->Destroy();for(auto& Pair:Staged)if(IsValid(Pair.Value))Pair.Value->Destroy();
			OutError=NSLOCTEXT("Aftershock","RubbleRestoreFailed","Enkaz görselleri oluşturulamadı; önceki enkaz korunuyor.");return false;
		}
		Actor->SetActorHiddenInGame(true);Actor->SetActorEnableCollision(false);
		Component->SetSimulatePhysics(false);Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->ObjectType=EObjectStateTypeEnum::Chaos_Object_Kinematic;
		Component->SetRestCollection(Assets[I],true);
		Actor->FinishSpawning(Record.ActorTransform);
		Component->SetLocalRestTransforms(Record.LocalRestTransforms,false);
		Component->SetSimulatePhysics(false);Component->DestroyPhysicsState();
		Component->SetComponentTickEnabled(false);Actor->SetActorTickEnabled(false);
		Component->MarkRenderDynamicDataDirty();Staged.Add(Record.Coord,Actor);
	}
	for(auto& Pair:CollapsedRubbleActors)if(IsValid(Pair.Value))Pair.Value->Destroy();
	ActiveChaosActors.Reset();CollapsedRubbleActors=MoveTemp(Staged);
	for(auto& Pair:CollapsedRubbleActors)Pair.Value->SetActorHiddenInGame(false);
	RefreshVisibleRubblePresentation(true);
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_RUBBLE_RESTORED actors=%d physicsResimulated=0 damageReapplied=0 actorTicks=0 componentTicks=0 poseSource=SavedNativeTransforms"),State.Num());
	return true;
}

bool UDisasterSimulationSubsystem::RestoreVisualSaveState(const FAftershockDisasterVisualSnapshot& State,float RemainingSeconds)
{
	if(!GetWorld()||!FMath::IsFinite(RemainingSeconds)||RemainingSeconds<0.f||RemainingSeconds>3600.f
		||!FMath::IsFinite(State.DurationSeconds)||State.DurationSeconds<0.f||State.DurationSeconds>3600.f
		||State.Revision<0||static_cast<uint8>(State.Type)>static_cast<uint8>(EDisasterType::Landslide)
		||!FMath::IsFinite(State.BaseSeverity)||!FMath::IsFinite(State.EffectiveIntensity)||State.DominantWindDirection.ContainsNaN()
		||State.PropagationDirection.ContainsNaN()||!FMath::IsFinite(State.SurgePeakHeadCm)
		||!FMath::IsFinite(State.DroughtStress01)||State.DroughtStress01<0.f||State.DroughtStress01>1.f
		||!FMath::IsFinite(State.CropLoss01)||State.CropLoss01<0.f||State.CropLoss01>1.f)return false;
	const UTerrainGridSubsystem* Terrain=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
	for(const auto* Map:{&State.LocalSeverity,&State.LavaSeverity,&State.AshSeverity,&State.RiverFillBefore,&State.RiverFillAfter})
		for(const auto& Pair:*Map)if(!Terrain||!Terrain->IsValidCoord(Pair.Key)||!FMath::IsFinite(Pair.Value))return false;
	// Tagged fields default to zero/empty for every pre-V3 save. Validate this
	// public restore entry point too; callers need not pass through SaveSubsystem.
	if(State.FloodModelVersion<0||State.FloodModelVersion>2
		||!FMath::IsFinite(State.RainfallPulseMm)||State.RainfallPulseMm<0.f||State.RainfallPulseMm>200.f
		||!FMath::IsFinite(State.FloodPulseDurationSeconds)||State.FloodPulseDurationSeconds<0.f||State.FloodPulseDurationSeconds>3600.f)return false;
	if(State.FloodModelVersion==0)
	{
		if(State.RainfallPulseMm!=0.f||State.FloodPulseDurationSeconds!=0.f||!State.WaterDepthCm.IsEmpty()
			||!State.WaterSurfaceElevationCm.IsEmpty()||!State.WaterArrivalFraction.IsEmpty())return false;
	}
	else if(State.FloodModelVersion==1)
	{
		if(State.Type!=EDisasterType::Flood||State.FloodPulseDurationSeconds<=0.f
			||State.WaterDepthCm.Num()>UTerrainGridSubsystem::MaximumResidentCells
			||State.WaterDepthCm.Num()!=State.WaterSurfaceElevationCm.Num()
			||State.WaterDepthCm.Num()!=State.WaterArrivalFraction.Num())return false;
		for(const auto& Pair:State.WaterDepthCm)
		{
			const float* Head=State.WaterSurfaceElevationCm.Find(Pair.Key);const float* Arrival=State.WaterArrivalFraction.Find(Pair.Key);
			if(!Terrain||!Terrain->IsValidCoord(Pair.Key)||!FMath::IsFinite(Pair.Value)||Pair.Value<0.f
				||!Head||!FMath::IsFinite(*Head)||!Arrival||!FMath::IsFinite(*Arrival)||*Arrival<0.f||*Arrival>1.f)return false;
		}
	}
	else
	{
		// Persistent hydraulic state is restored by its canonical owner first.
		// Model2 deliberately carries no obsolete event-depth/arrival maps.
		if(!IsHydraulicModel2Active()||(State.Type!=EDisasterType::Flood&&State.Type!=EDisasterType::Tsunami)
			||!State.WaterDepthCm.IsEmpty()||!State.WaterSurfaceElevationCm.IsEmpty()||!State.WaterArrivalFraction.IsEmpty())return false;
	}
	if(State.LavaArrivalFraction.Num()>UTerrainGridSubsystem::MaximumResidentCells
		||(!State.LavaArrivalFraction.IsEmpty()&&(State.Type!=EDisasterType::Volcano||State.LavaArrivalFraction.Num()!=State.LavaSeverity.Num())))return false;
	for(const auto& Pair:State.LavaArrivalFraction)
		if(!Terrain||!Terrain->IsValidCoord(Pair.Key)||!State.LavaSeverity.Contains(Pair.Key)||!FMath::IsFinite(Pair.Value)||Pair.Value<0.f||Pair.Value>1.f)return false;
	// Preserve the phase/clock of the old event in the new world. Completed
	// events also remain completed, including the legacy ten-second quake bed.
	const float EffectiveDuration=State.GetEffectiveDurationSeconds();
	if(RemainingSeconds>EffectiveDuration)return false;
	LatestVisualSnapshot=State;
	LatestVisualSnapshot.StartedAtWorldSeconds=GetWorld()->GetTimeSeconds()-FMath::Max(0.f,EffectiveDuration-FMath::Min(RemainingSeconds,EffectiveDuration));
	OnDisasterVisualSnapshotChanged.Broadcast(LatestVisualSnapshot);
	return true;
}
