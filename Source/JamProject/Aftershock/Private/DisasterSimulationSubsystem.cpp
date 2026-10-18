// AFTERSHOCK — Civil Game Jam 2026
// Dinamik Afet Simülasyonu, Sel Yayılımı ve Chaos Yıkım Motoru Alt Sistemi Uygulaması

#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Narrative/CasualtyLedgerGenerator.h"
#include "Aftershock/Public/AftershockScaledChaos.h"
#include "AftershockTerrainSurface.h"
#include "AftershockHydraulicTerrain.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GeometryCollection/GeometryCollectionActor.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/Facades/CollectionAnchoringFacade.h"
#include "GeometryCollectionProxyData.h"
#include "Chaos/ImplicitObject.h"
#include "PhysicsProxy/GeometryCollectionPhysicsProxy.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Async/ParallelFor.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarParallelFlood(
 TEXT("r.Aftershock.ParallelFlood"),1,
 TEXT("0=serial legacy V3 pulse; 1=parallel identical four-pass Jacobi kernel. Production hydraulic Model2 uses its own solver."),ECVF_Default);

namespace
{
	void LogChaosFoundationParticlePoses(UGeometryCollectionComponent& Component,const TCHAR* Phase)
	{
		if (!FParse::Param(FCommandLine::Get(),TEXT("AftershockChaosAudit"))) return;
		// Read the game-thread particle API before proxy teardown. Render transforms
		// alone cannot distinguish a physical escape from mass/component-space
		// conversion under nonuniform actor scale. This never writes either pose.
		const FGeometryCollectionPhysicsProxy* Proxy=Component.GetPhysicsProxy();
		const UGeometryCollection* Rest=Component.GetRestCollection();
		const TSharedPtr<FGeometryCollection,ESPMode::ThreadSafe> Collection=Rest ? Rest->GetGeometryCollection() : nullptr;
		const FGeometryDynamicCollection* Dynamic=Component.GetDynamicCollection();
		if (!Proxy || !Collection || !Dynamic) return;
		const TManagedArray<FTransform>* MassToLocal=Collection->FindAttribute<FTransform>(TEXT("MassToLocal"),FGeometryCollection::TransformGroup);
		TArray<FTransform> RestGlobal; GeometryCollectionAlgo::GlobalMatrices(Collection->Transform,Collection->Parent,RestGlobal);
		const TArray<FTransform3f>& RenderTransforms=Component.GetComponentSpaceTransforms3f();
		const Chaos::Facades::FCollectionAnchoringFacade Anchoring(*Dynamic);
		int32 Anchored=0,AvailableParticles=0,NativeKinematic=0,EscapedParticles=0,EscapedRenderCentres=0;
		double MaxParticlePositionDelta=0.,MaxParticleRotationDeltaDegrees=0.,MaxRenderCentreDelta=0.;
		for (int32 Index=0;Index<Collection->SimulationType.Num();++Index)
		{
			if (Collection->SimulationType[Index]!=FGeometryCollection::FST_Rigid || !Anchoring.IsAnchored(Index)) continue;
			++Anchored;
			if (!MassToLocal || !MassToLocal->IsValidIndex(Index) || !RestGlobal.IsValidIndex(Index)) continue;
			const Chaos::FPBDRigidParticle* Particle=Proxy->GetParticleByIndex_External(Index);
			if (!Particle) continue;
			++AvailableParticles;
			NativeKinematic+=Particle->ObjectState()==Chaos::EObjectStateType::Kinematic ? 1 : 0;
			// This is the same mass-frame composition used by proxy initialization.
			const FTransform ExpectedParticle=(*MassToLocal)[Index]*RestGlobal[Index]*Component.GetComponentTransform();
			const FVector ParticlePosition(Particle->X());
			const FQuat ParticleRotation(Particle->R());
			const double PositionDelta=FVector::Dist(ParticlePosition,ExpectedParticle.GetTranslation());
			const double RotationDeltaDegrees=FMath::RadiansToDegrees(ParticleRotation.AngularDistance(ExpectedParticle.GetRotation()));
			double RenderCentreDelta=-1.;
			const int32 Geometry=Collection->TransformToGeometryIndex.IsValidIndex(Index) ? Collection->TransformToGeometryIndex[Index] : INDEX_NONE;
			if (RenderTransforms.IsValidIndex(Index) && Geometry!=INDEX_NONE && Collection->BoundingBox.IsValidIndex(Geometry))
			{
				const FBox RenderBounds=Collection->BoundingBox[Geometry].TransformBy(FTransform(RenderTransforms[Index])*Component.GetComponentTransform());
				const FBox ExpectedBounds=Collection->BoundingBox[Geometry].TransformBy(RestGlobal[Index]*Component.GetComponentTransform());
				RenderCentreDelta=FVector::Dist(RenderBounds.GetCenter(),ExpectedBounds.GetCenter());
				MaxRenderCentreDelta=FMath::Max(MaxRenderCentreDelta,RenderCentreDelta);
				EscapedRenderCentres+=RenderCentreDelta>3. ? 1 : 0;
			}
			EscapedParticles+=PositionDelta>3. ? 1 : 0;
			MaxParticlePositionDelta=FMath::Max(MaxParticlePositionDelta,PositionDelta);
			MaxParticleRotationDeltaDegrees=FMath::Max(MaxParticleRotationDeltaDegrees,RotationDeltaDegrees);
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHAOS_FOUNDATION_NATIVE_POSE actor=%s phase=%s leaf=%d nativeState=%d particlePosition=(%.3f,%.3f,%.3f) expectedMassPosition=(%.3f,%.3f,%.3f) nativePositionDeltaCm=%.4f nativeRotationDeltaDegrees=%.4f renderCentreDeltaCm=%.4f"),
				*GetNameSafe(Component.GetOwner()),Phase,Index,static_cast<int32>(Particle->ObjectState()),ParticlePosition.X,ParticlePosition.Y,ParticlePosition.Z,
				ExpectedParticle.GetTranslation().X,ExpectedParticle.GetTranslation().Y,ExpectedParticle.GetTranslation().Z,PositionDelta,RotationDeltaDegrees,RenderCentreDelta);
		}
		const FVector Scale=Component.GetComponentScale();
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHAOS_FOUNDATION_NATIVE_SUMMARY actor=%s phase=%s anchored=%d availableGTParticles=%d nativeKinematic=%d escapedNativeParticles=%d escapedRenderCentres=%d maxNativePositionDeltaCm=%.4f maxNativeRotationDeltaDegrees=%.4f maxRenderCentreDeltaCm=%.4f componentScale=(%.5f,%.5f,%.5f) toleranceCm=3 readOnly=1"),
			*GetNameSafe(Component.GetOwner()),Phase,Anchored,AvailableParticles,NativeKinematic,EscapedParticles,EscapedRenderCentres,MaxParticlePositionDelta,MaxParticleRotationDeltaDegrees,MaxRenderCentreDelta,Scale.X,Scale.Y,Scale.Z);
	}

	bool GetBuiltChaosGroundWindow(UWorld* World,FIntRect& OutBounds)
	{
		// Use the ACTUALLY installed collision window, not a predicted camera
		// rectangle that may not have finished its runtime mesh build yet.
		for (TActorIterator<AAftershockCrowdHostActor> It(World);It;++It)
		{
			const UStaticMeshComponent* Ground=It->TerrainSurface;
			if (It->IsActorBeingDestroyed() || !Ground || !Ground->GetStaticMesh() || !Ground->IsPhysicsStateCreated()
				|| !CollisionEnabledHasPhysics(Ground->GetCollisionEnabled()) || It->BuiltVisibleCellBounds.IsEmpty()) continue;
			OutBounds=It->BuiltVisibleCellBounds; return true;
		}
		return false;
	}

	bool IsChaosBoundsSupported(const UTerrainGridSubsystem& Terrain,const FIntRect& Window,const FBox& WorldBounds)
	{
		if (!WorldBounds.IsValid || Window.IsEmpty()) return false;
		// Two cells exceed the existing 160cm/s cosmetic kick's four-second
		// horizontal travel. A proxy near an unloaded edge is frozen before the
		// ground is removed. This is only a presentation/solver allocation gate.
		const FBox Padded=WorldBounds.ExpandBy(FVector(Terrain.CellSize*2.f,Terrain.CellSize*2.f,0));
		const FVector Center=Padded.GetCenter();
		const FVector Samples[]={Center,FVector(Padded.Min.X,Padded.Min.Y,Center.Z),FVector(Padded.Max.X,Padded.Min.Y,Center.Z),
			FVector(Padded.Min.X,Padded.Max.Y,Center.Z),FVector(Padded.Max.X,Padded.Max.Y,Center.Z),
			FVector(Padded.Min.X,Center.Y,Center.Z),FVector(Padded.Max.X,Center.Y,Center.Z),FVector(Center.X,Padded.Min.Y,Center.Z),FVector(Center.X,Padded.Max.Y,Center.Z)};
		for (const FVector& Sample:Samples)
		{
			const FIntPoint Coord=Terrain.WorldToGrid(Sample);
			if (!Window.Contains(Coord) || !Terrain.IsPlayableCoord(Coord)) return false;
		}
		return true;
	}

	FBox CollectionWorldBounds(const FGeometryCollection& Collection,const FTransform& ActorTransform)
	{
		TArray<FTransform> Global; GeometryCollectionAlgo::GlobalMatrices(Collection.Transform,Collection.Parent,Global);
		FBox Bounds(ForceInit);
		for (int32 I=0;I<Collection.SimulationType.Num();++I)
		{
			if (Collection.SimulationType[I]!=FGeometryCollection::FST_Rigid || !Global.IsValidIndex(I)) continue;
			const int32 Geometry=Collection.TransformToGeometryIndex[I];
			if (Geometry!=INDEX_NONE && Collection.BoundingBox.IsValidIndex(Geometry)) Bounds+=Collection.BoundingBox[Geometry].TransformBy(Global[I]*ActorTransform);
		}
		return Bounds;
	}

	TArray<FTransform> BuildAuthoredOffscreenRubblePose(const FGeometryCollection& Collection,const FTransform& ActorTransform,
		const UTerrainGridSubsystem& Terrain,int32 BuildingID)
	{
		TArray<FTransform> Global; GeometryCollectionAlgo::GlobalMatrices(Collection.Transform,Collection.Parent,Global);
		const TManagedArray<FTransform>* MassToLocal=Collection.FindAttribute<FTransform>(TEXT("MassToLocal"),FGeometryCollection::TransformGroup);
		TArray<FTransform> Poses; Poses.Reserve(Global.Num());
		for (int32 I=0;I<Global.Num();++I)
		{
			FTransform Pose=Global[I];
			const int32 Geometry=Collection.TransformToGeometryIndex.IsValidIndex(I) ? Collection.TransformToGeometryIndex[I] : INDEX_NONE;
			if (Collection.SimulationType[I]==FGeometryCollection::FST_Rigid && Geometry!=INDEX_NONE && Collection.BoundingBox.IsValidIndex(Geometry))
			{
				const FBox RestBounds=Collection.BoundingBox[Geometry].TransformBy(Global[I]);
				// Keep the real buried foundation skirt in its authored position.
				// Upper fragments receive a deterministic settled art pose; this is
				// explicitly NOT a reconstruction of an unobserved physics result.
				if (RestBounds.IsValid && RestBounds.Min.Z>=-20.f)
				{
					FRandomStream Random(static_cast<int32>(HashCombine(GetTypeHash(BuildingID),GetTypeHash(I))));
					Pose.SetRotation(FRotator(Random.FRandRange(65.f,85.f),Random.FRandRange(-180.f,180.f),0).Quaternion()*Pose.GetRotation());
					const FBox Rotated=Collection.BoundingBox[Geometry].TransformBy(Pose);
					const FVector Target(RestBounds.GetCenter().X*.65f+Random.FRandRange(-80.f,80.f),RestBounds.GetCenter().Y*.65f+Random.FRandRange(-80.f,80.f),0);
					Pose.AddToTranslation(Target-Rotated.GetCenter());
					const FBox WorldBounds=Collection.BoundingBox[Geometry].TransformBy(Pose*ActorTransform);
					const FVector Center=WorldBounds.GetCenter();
					const FVector Samples[]={Center,FVector(WorldBounds.Min.X,WorldBounds.Min.Y,Center.Z),FVector(WorldBounds.Max.X,WorldBounds.Min.Y,Center.Z),
						FVector(WorldBounds.Min.X,WorldBounds.Max.Y,Center.Z),FVector(WorldBounds.Max.X,WorldBounds.Max.Y,Center.Z)};
					float Ground=-MAX_flt;
					for (const FVector& Sample:Samples)
						Ground=FMath::Max(Ground,SampleAftershockResidentElevation(Terrain,(Sample.X-Terrain.GridOrigin.X)/Terrain.CellSize-.5f,(Sample.Y-Terrain.GridOrigin.Y)/Terrain.CellSize-.5f));
					const FVector WorldLift(0,0,Ground+4.f+(I%3)*12.f-WorldBounds.Min.Z);
					Pose.AddToTranslation(ActorTransform.InverseTransformVector(WorldLift));
				}
			}
			// Paired native API convention; SetLocalRestTransforms removes this
			// mass-space compensation before applying component-space poses.
			Poses.Add((MassToLocal && MassToLocal->IsValidIndex(I) ? (*MassToLocal)[I] : FTransform::Identity)*Pose);
		}
		return Poses;
	}
}

AAftershockFoundationGeometryCollectionActor::AAftershockFoundationGeometryCollectionActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UAftershockFoundationGeometryCollectionComponent>(TEXT("GeometryCollectionComponent0")))
{
}

void UAftershockFoundationGeometryCollectionComponent::OnCreatePhysicsState()
{
	// UGeometryCollectionComponent::OnRegister has just reset DynamicCollection.
	// Populate its initial anchor data before Super constructs and copies it into
	// the solver proxy. Post-creation anchor flags alone are insufficient for
	// independent (unclustered) dynamic leaves.
	if (FGeometryDynamicCollection* Dynamic=GetDynamicCollection())
	{
		const TSharedPtr<FGeometryCollection,ESPMode::ThreadSafe> Rest=GetRestCollection() ? GetRestCollection()->GetGeometryCollection() : nullptr;
		for (int32 Index=0;Index<Dynamic->GetNumTransforms();++Index)
		{
			const int32 Initial=Rest && Rest->InitialDynamicState.IsValidIndex(Index) ? Rest->InitialDynamicState[Index] : static_cast<int32>(Chaos::EObjectStateType::Uninitialized);
			Dynamic->DynamicState[Index]=Initial==static_cast<int32>(Chaos::EObjectStateType::Uninitialized)
				? static_cast<uint8>(EObjectStateTypeEnum::Chaos_Object_Dynamic) : static_cast<uint8>(Initial);
		}
		Chaos::Facades::FCollectionAnchoringFacade Anchoring(*Dynamic);
		if (!Anchoring.HasAnchoredAttribute()) Anchoring.AddAnchoredAttribute();
		for (int32 Index:FoundationAnchorIndices)
			if (Index>=0 && Index<Dynamic->GetNumTransforms())
			{
				Anchoring.SetAnchored(Index,true);
				Dynamic->DynamicState[Index]=static_cast<uint8>(EObjectStateTypeEnum::Chaos_Object_Kinematic);
			}
	}
	// Native OnCreatePhysicsState normally replaces every DynamicState from the
	// shared asset. User_Defined preserves this instance's explicitly prepared
	// array while it creates the proxy; restore the component setting afterwards.
	const EObjectStateTypeEnum PreviousObjectType=ObjectType;
	ObjectType=EObjectStateTypeEnum::Chaos_Object_UserDefined;
	if (GetRestCollection()) UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHAOS_PREPROXY_STATE component=%s dynamicTransforms=%d requestedFoundationAnchors=%d objectType=UserDefined"),
		*GetClass()->GetName(),GetDynamicCollection() ? GetDynamicCollection()->GetNumTransforms() : 0,FoundationAnchorIndices.Num());
	Super::OnCreatePhysicsState();
	ObjectType=PreviousObjectType;
	// Super may copy the asset's existing all-false Anchored attribute. Preserve
	// the instance metadata too; the solver already received explicit Kinematic
	// states above, which do not depend on clustering or this metadata flag.
	if (FGeometryDynamicCollection* Dynamic=GetDynamicCollection())
	{
		Chaos::Facades::FCollectionAnchoringFacade Anchoring(*Dynamic);
		if (!Anchoring.HasAnchoredAttribute()) Anchoring.AddAnchoredAttribute();
		for (int32 Index:FoundationAnchorIndices)
			if (Index>=0 && Index<Dynamic->GetNumTransforms()) Anchoring.SetAnchored(Index,true);
	}
}

void UDisasterSimulationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ActiveChaosActors.Empty();
	CollapsedRubbleActors.Empty();
	bHasRubbleDrawBounds=false;
	LatestVisualSnapshot = FAftershockDisasterVisualSnapshot();
	// One world callback, at most fifteen proxies, no citizen/building Tick and
	// no resident-cell scan. A camera-window replacement must not leave a live
	// debris solver running where its ground collision has been unloaded.
	FWorldDelegates::OnWorldPreActorTick.AddWeakLambda(this,[this](UWorld* TickWorld,ELevelTick,float)
	{
		if (TickWorld!=GetWorld() || ActiveChaosActors.IsEmpty()) return;
		ActiveChaosActors.RemoveAll([](const TWeakObjectPtr<AActor>& Actor) { return !Actor.IsValid(); });
		if (ActiveChaosActors.IsEmpty()) return;
		const UTerrainGridSubsystem* Terrain=TickWorld->GetSubsystem<UTerrainGridSubsystem>();
		FIntRect Window; const bool bHasGround=Terrain && GetBuiltChaosGroundWindow(TickWorld,Window);
		const TArray<TWeakObjectPtr<AActor>> Active=ActiveChaosActors;
		for (const TWeakObjectPtr<AActor>& Actor:Active)
		{
			const UGeometryCollectionComponent* Component=Actor.IsValid() ? Actor->FindComponentByClass<UGeometryCollectionComponent>() : nullptr;
			if (!Component || !bHasGround || !IsChaosBoundsSupported(*Terrain,Window,Component->Bounds.GetBox()))
			{
				if (Actor.IsValid()) UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHAOS_WINDOW_EXIT actor=%s action=FreezeExistingNativePose beforeNextPhysicsStep=1 damageChanged=0"),*Actor->GetName());
				FreezeChaosActor(Actor);
			}
		}
	});
}

void UDisasterSimulationSubsystem::Deinitialize()
{
	ShutdownPhysicalRuntime();
	FWorldDelegates::OnWorldPreActorTick.RemoveAll(this);
	for (auto& Pair : CollapsedRubbleActors)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->Destroy();
		}
	}
	CollapsedRubbleActors.Empty();
	ActiveChaosActors.Empty();
	OnDisasterCompleted.Clear();
	OnDisasterVisualSnapshotChanged.Clear();
	OnDamInfrastructureChanged.Clear();

	Super::Deinitialize();
}

void UDisasterSimulationSubsystem::RefreshVisibleRubblePresentation(bool bForce)
{
	const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	if(!Terrain)return;
	const FIntRect Drawable=GetAftershockDrawableCellBounds(*Terrain);
	if(!bForce&&bHasRubbleDrawBounds&&RubbleDrawBounds==Drawable)return;
	RubbleDrawBounds=Drawable;bHasRubbleDrawBounds=true;
	for(auto& Pair:CollapsedRubbleActors)
	{
		auto* Component=IsValid(Pair.Value)?Pair.Value->FindComponentByClass<UGeometryCollectionComponent>():nullptr;
		if(!Component)continue;
		const bool bDraw=Drawable.Contains(Pair.Key);
		// Visibility is a derived presentation property. In particular, hiding an
		// active collection must not tear down/recreate its proxy or alter its pose.
		if(Component->IsVisible()!=bDraw)Component->SetVisibility(bDraw,true);
		if(Component->bHiddenInGame==bDraw)Component->SetHiddenInGame(!bDraw,true);
	}
}

TMap<FIntPoint, float> UDisasterSimulationSubsystem::SimulateFloodMap(float RainfallIntensity)
{
	// Public previews are repeatable and never consume reservoir storage.
	return BuildFloodMapWithDams(RainfallIntensity, false);
}

TMap<FIntPoint,float> UDisasterSimulationSubsystem::SimulateWindMap(float BaseSeverity) const
{
	TMap<FIntPoint,float> Result;const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	if(!Terrain||!FMath::IsFinite(BaseSeverity)||BaseSeverity<=0.f)return Result;
	FVector2D Wind(ScenarioWindDirection.X,ScenarioWindDirection.Y);
	if(Wind.ContainsNaN()||Wind.IsNearlyZero())Wind=FVector2D(1.,.35);Wind.Normalize();
	const FIntPoint Upwind(-FMath::RoundToInt(Wind.X*4.),-FMath::RoundToInt(Wind.Y*4.));
	const FIntPoint Crosswind(-FMath::RoundToInt(Wind.Y*4.),FMath::RoundToInt(Wind.X*4.));
	const double FetchCm=FMath::Max(1.,static_cast<double>(Terrain->CellSize)*4.);
	for(const auto& Cell:Terrain->Cells)
	{
		if(!Cell.bIsPlayable||Cell.bIsOcean)continue;
		FGridCell Up,Left,Right;double Shelter=1.,Alignment=1.;
		if(Terrain->TryGetCell(Cell.Coords+Upwind,Up))Shelter=FMath::Clamp(1.-(Up.Elevation-Cell.Elevation)/FetchCm*.75,.55,1.30);
		if(Terrain->ContinuationGeneratorVersion>=3)
		{
			Alignment=.25;
			if(Terrain->TryGetCell(Cell.Coords+Crosswind,Left)&&Terrain->TryGetCell(Cell.Coords-Crosswind,Right))
				Alignment+=.75*FMath::Clamp((FMath::Min(Left.Elevation,Right.Elevation)-Cell.Elevation)/FetchCm*4.,0.,1.);
		}
		const double Channel=1.+(FMath::Clamp(static_cast<double>(Cell.ValleyWindMultiplier),1.,1.5)-1.)*Alignment;
		const double ReferenceGustMps=FMath::Clamp(static_cast<double>(BaseSeverity),0.,100.)*.5;
		const double LocalGust=ReferenceGustMps*Channel*Shelter;
		// q=.5*rho*v^2, rho=1.225kg/m3. q=1531.25Pa corresponds to this
		// scenario's100-severity50m/s reference gust; not a building-code check.
		Result.Add(Cell.Coords,static_cast<float>(FMath::Clamp(.5*1.225*LocalGust*LocalGust/1531.25*100.,0.,100.)));
	}
	return Result;
}

bool UDisasterSimulationSubsystem::CanTriggerDisaster(EDisasterType Type, FText& OutReason) const
{
	const UWorld* World = GetWorld();
	const UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!World || !Terrain) { OutReason = NSLOCTEXT("Aftershock", "DisasterWorldMissing", "Afet dünyası hazır değil."); return false; }
	if(Type==EDisasterType::Avalanche||Type==EDisasterType::Landslide)
	{
		if(GetPhysicalHazardWeight(Type)<=0.f)
		{OutReason=NSLOCTEXT("Aftershock","MassRequiresFiniteStock","Bu yerel bölgede yeterli bağlı kar/toprak stoku ve uygun eğim yok ya da zemin henüz kararsız değil.");return false;}
	}
	if(Type==EDisasterType::Flood&&Terrain->ContinuationGeneratorVersion>=3)
	{
		int32 Active=0;for(const auto& Cell:Terrain->Cells)if(Cell.bIsPlayable&&!Cell.bIsOcean)++Active;
		if(Active>65536){OutReason=NSLOCTEXT("Aftershock","FloodDomainBudget","Fiziksel sel senaryosu en fazla65.536 etkin arazi hücresi destekliyor; bu alan önce bölgesel simülasyon parçalarına ayrılmalı.");return false;}
	}
	if (Type == EDisasterType::Tsunami && !Terrain->IsCoastalWorld())
	{
		OutReason = NSLOCTEXT("Aftershock", "TsunamiRequiresSea", "Tsunami yalnızca denize açılan kıyı senaryosunda oluşabilir."); return false;
	}
	if (Type==EDisasterType::Volcano && (!Terrain->IsPlayableCoord(Terrain->VolcanicVentCoord) || !Terrain->GetCell(Terrain->VolcanicVentCoord).bVolcanicVent))
	{ OutReason=NSLOCTEXT("Aftershock","VolcanoMappedOnly","Volkan yalnız Etna gibi haritalanmış etkin volkan kaynağı olan bölgede kullanılabilir; sıcak hava bir patlama tetiklemez."); return false; }
	if (Type==EDisasterType::Fire)
	{
		bool bHasFuel=false;
		for (const FGridCell& Cell : Terrain->Cells) if (Cell.bIsPlayable && !Cell.bIsOcean && Cell.ForestFuel01>.15f && Cell.SoilMoisture01<.92f) { bHasFuel=true; break; }
		if (!bHasFuel) { OutReason=NSLOCTEXT("Aftershock","FireRequiresFuel","Bu bölgede yayılabilir orman yakıtı kalmadı."); return false; }
	}
	OutReason = FText::GetEmpty(); return true;
}

bool UDisasterSimulationSubsystem::SurveyGround(FIntPoint Coord,int32& OutCostTL)
{
	OutCostTL=0; UWorld* World=GetWorld();
	UTerrainGridSubsystem* Terrain=World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	UEconomyAndStateSubsystem* Economy=World ? World->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	if (!World || (World->WorldType!=EWorldType::Game && World->WorldType!=EWorldType::PIE) || !Terrain || !Economy || Economy->bIsGameOver || !Terrain->IsPlayableCoord(Coord)) return false;
	FGridCell Cell=Terrain->GetCell(Coord);
	if (Cell.bGroundSurveyed || Cell.bIsOcean || Cell.bIsRiverbed || !Economy->CanAfford(500)) return false;
	const FGridCell Original=Cell; Cell.bGroundSurveyed=true;
	if (!Terrain->SetCell(Coord,Cell)) return false;
	if (!Economy->TrySpendMoney(500)) { Terrain->SetCell(Coord,Original); return false; }
	OutCostTL=500;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_GROUND_SURVEY coord=%s substrate=%s costTL=500 authoredScenarioClassNotRealGeotechnicalReport=1"),*Coord.ToString(),Cell.SoilType==EAftershockSoilType::Alluvium ? TEXT("Alluvium") : TEXT("Bedrock"));
	return true;
}

TMap<FIntPoint,float> UDisasterSimulationSubsystem::SimulateForestFireMap(float BaseSeverity) const
{
	TMap<FIntPoint,float> Result;
	const UWorld* World=GetWorld(); const UTerrainGridSubsystem* Terrain=World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || !FMath::IsFinite(BaseSeverity) || BaseSeverity<=0.f) return Result;
	const float Strength=FMath::Clamp(BaseSeverity,0.f,100.f);
	FIntPoint Origin(INDEX_NONE,INDEX_NONE); float BestScore=-MAX_flt;
	// Authored ignition at a reproducible forest/settlement edge; the event is
	// hypothetical, not a weather forecast or automatic temperature ignition.
	const FIntPoint Desired(Terrain->GridWidth/4,Terrain->GridHeight/3);
	for (const FGridCell& Cell : Terrain->Cells)
	{
		if (!Cell.bIsPlayable || Cell.bIsOcean || Cell.bIsRiverbed || Cell.ForestFuel01<.15f || Cell.SoilMoisture01>=.92f) continue;
		const float Score=Cell.ForestFuel01*8.f-FVector2D::Distance(FVector2D(Cell.Coords),FVector2D(Desired));
		if (Score>BestScore || (Score==BestScore && (Cell.Coords.Y<Origin.Y || (Cell.Coords.Y==Origin.Y&&Cell.Coords.X<Origin.X)))) { BestScore=Score; Origin=Cell.Coords; }
	}
	if (Origin.X==INDEX_NONE) return Result;
	FVector Wind(ScenarioWindDirection.X,ScenarioWindDirection.Y,0.f);
	if (Wind.ContainsNaN() || Wind.IsNearlyZero()) Wind=FVector(1,.35f,0);
	Wind.Normalize();
	TArray<float> Energy; Energy.Init(0.f,Terrain->Cells.Num());
	TArray<FIntPoint> Queue; Queue.Add(Origin); Energy[Terrain->CoordToIndex(Origin)]=Strength;
	const FIntPoint Steps[]={FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1)};
	for (int32 Head=0;Head<Queue.Num();++Head)
	{
		const FIntPoint Current=Queue[Head]; const FGridCell Cell=Terrain->GetCell(Current);
		const float CurrentEnergy=Energy[Terrain->CoordToIndex(Current)];
		Result.Add(Current,CurrentEnergy);
		for (FIntPoint Step : Steps)
		{
			const FIntPoint Next=Current+Step; if (!Terrain->IsPlayableCoord(Next)) continue;
			const FGridCell N=Terrain->GetCell(Next);
			if (N.bIsOcean || N.bIsRiverbed || N.SoilMoisture01>=.95f || (N.ForestFuel01<.08f && N.OccupyingBuildingID==INDEX_NONE)) continue;
			const float Along=FVector::DotProduct(Wind,FVector(Step.X,Step.Y,0));
			const float Fuel=FMath::Max(N.ForestFuel01,N.OccupyingBuildingID!=INDEX_NONE ? .4f : 0.f);
			const float Grade=(N.Elevation-Cell.Elevation)/FMath::Max(1.f,Terrain->CellSize);
			const float UphillSpread=FMath::Clamp(FMath::Exp(Grade*2.f),.4f,2.2f);
			const float Loss=(3.2f+N.SoilMoisture01*9.f+(1.f-Fuel)*5.f+(N.bHasRoad ? 12.f : 0.f))
				*FMath::Max(.1f,Terrain->CellSize/500.f)/(FMath::Clamp(1.f+Along*.55f,.4f,1.6f)*UphillSpread);
			const float Candidate=CurrentEnergy-Loss;
			const int32 I=Terrain->CoordToIndex(Next);
			if (Candidate>1.f && Candidate>Energy[I]+.01f) { Energy[I]=Candidate; Queue.Add(Next); }
		}
	}
	// Connected, powered, staffed stations have bounded LOCAL suppression.
	// A station does not erase the event or protect every forest in the world.
	const UBuildingManagerSubsystem* Buildings=World->GetSubsystem<UBuildingManagerSubsystem>();
	const UCityLifeSubsystem* Life=World->GetSubsystem<UCityLifeSubsystem>();
	TArray<FBuildingInstance> Records; if (Buildings) Buildings->GetAllBuildings(Records);
	struct FOperationalFireStation {FIntPoint Coord;float Operation;};
	TArray<FOperationalFireStation> Stations;
	for(const FBuildingInstance& B:Records)
	{
		if(!B.BuildingData||B.BuildingData->Type!=EBuildingType::FireStation||B.DamageState>=EDamageState::Damaged)continue;
		Stations.Add({B.GridCoords,Life&&Life->IsCityInitialized()?Life->GetBuildingOperation01(B.InstanceID):1.f});
	}
	for (TPair<FIntPoint,float>& Pair:Result)
	{
		float Coverage=0.f;
		for (const FOperationalFireStation& Station:Stations)
		{
			const float DistanceCm=FVector2D::Distance(FVector2D(Pair.Key),FVector2D(Station.Coord))*Terrain->CellSize;
			Coverage=FMath::Max(Coverage,Station.Operation*FMath::Clamp(1.f-DistanceCm/6000.f,0.f,1.f));
		}
		Pair.Value*=1.f-.55f*Coverage;
	}
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_FOREST_FIRE_FIELD source=%s connectedCells=%d wind=(%.2f,%.2f) fuelMoistureRoadBreaks=1 units=AuthoredGameplaySeverity noGlobalFire=1"),*Origin.ToString(),Result.Num(),Wind.X,Wind.Y);
	return Result;
}

TMap<FIntPoint,float> UDisasterSimulationSubsystem::SimulateVolcanoMap(float BaseSeverity,TMap<FIntPoint,float>& OutLava,TMap<FIntPoint,float>& OutAsh) const
{
	TMap<FIntPoint,float> Arrival;return BuildVolcanoField(BaseSeverity,OutLava,OutAsh,Arrival);
}

TMap<FIntPoint,float> UDisasterSimulationSubsystem::BuildVolcanoField(float BaseSeverity,TMap<FIntPoint,float>& OutLava,TMap<FIntPoint,float>& OutAsh,TMap<FIntPoint,float>& OutArrival) const
{
	OutLava.Reset(); OutAsh.Reset();OutArrival.Reset(); TMap<FIntPoint,float> Result;
	const UWorld* World=GetWorld(); const UTerrainGridSubsystem* Terrain=World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || !FMath::IsFinite(BaseSeverity) || BaseSeverity<=0.f || !Terrain->IsPlayableCoord(Terrain->VolcanicVentCoord) || !Terrain->GetCell(Terrain->VolcanicVentCoord).bVolcanicVent) return Result;
	const float Strength=FMath::Clamp(BaseSeverity,0.f,100.f);
	TArray<float> Energy; Energy.Init(0.f,Terrain->Cells.Num());
	TArray<FIntPoint> Queue; Queue.Add(Terrain->VolcanicVentCoord); Energy[Terrain->CoordToIndex(Terrain->VolcanicVentCoord)]=Strength;
	TArray<float> Travel;Travel.Init(0.f,Terrain->Cells.Num());
	const FIntPoint Steps[]={FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1)};
	for (int32 Head=0;Head<Queue.Num();++Head)
	{
		const FIntPoint Current=Queue[Head]; const FGridCell Cell=Terrain->GetCell(Current);
		const float CurrentEnergy=Energy[Terrain->CoordToIndex(Current)]; OutLava.Add(Current,CurrentEnergy);
		TArray<FIntPoint> Downhill;
		for (FIntPoint Step:Steps)
		{
			const FIntPoint Next=Current+Step;
			if (Terrain->IsPlayableCoord(Next) && !Terrain->GetCell(Next).bIsOcean && Terrain->GetCell(Next).Elevation<=Cell.Elevation+.001f) Downhill.Add(Next);
		}
		Downhill.Sort([Terrain](FIntPoint A,FIntPoint B){ const float EA=Terrain->GetCell(A).Elevation,EB=Terrain->GetCell(B).Elevation; return EA==EB ? Terrain->CoordToIndex(A)<Terrain->CoordToIndex(B) : EA<EB; });
		const int32 Branches=FMath::Min(2,Downhill.Num());
		for (int32 I=0;I<Branches;++I)
		{
			const FIntPoint Next=Downhill[I];
			// Shared directional transport intensity, not duplicated source volume.
			const float Loss=4.5f*FMath::Max(.1f,Terrain->CellSize/500.f);
			const float Candidate=FMath::Max(0.f,CurrentEnergy-Loss)*(Branches==1?1.f:I==0?.65f:.35f);
			const int32 NI=Terrain->CoordToIndex(Next);
			if (Candidate>1.f && Candidate>Energy[NI]+.01f) { Energy[NI]=Candidate;Travel[NI]=Travel[Terrain->CoordToIndex(Current)]+Terrain->CellSize; Queue.Add(Next); }
		}
	}
	float MaximumTravel=1.f;for(const auto& Pair:OutLava)MaximumTravel=FMath::Max(MaximumTravel,Travel[Terrain->CoordToIndex(Pair.Key)]);
	for(const auto& Pair:OutLava)OutArrival.Add(Pair.Key,FMath::Clamp(Travel[Terrain->CoordToIndex(Pair.Key)]/MaximumTravel,0.f,1.f));
	FVector Direction(ScenarioWindDirection.X,ScenarioWindDirection.Y,0.f); if (Direction.ContainsNaN() || Direction.IsNearlyZero()) Direction=FVector(1,.35f,0); Direction.Normalize();
	for (const FGridCell& Cell:Terrain->Cells)
	{
		if (!Cell.bIsPlayable || Cell.bIsOcean) continue;
		// Authored plume envelope uses5m reference lengths, independent of grid resolution.
		const FVector Delta=FVector(Cell.Coords.X-Terrain->VolcanicVentCoord.X,Cell.Coords.Y-Terrain->VolcanicVentCoord.Y,0)*(Terrain->CellSize/500.f);
		const float Along=FVector::DotProduct(Delta,Direction), Across=FMath::Abs(Delta.X*Direction.Y-Delta.Y*Direction.X);
		if (Along>=-2.f && Along<28.f && Across<3.f+FMath::Max(0.f,Along)*.3f)
		{
			const float Ash=Strength*.6f*FMath::Clamp(1.f-FMath::Max(0.f,Along)/28.f,0.f,1.f)*FMath::Clamp(1.f-Across/(3.f+FMath::Max(0.f,Along)*.3f),0.f,1.f);
			if (Ash>.1f) OutAsh.Add(Cell.Coords,Ash);
		}
		const float DamageSeverity=FMath::Max(OutLava.FindRef(Cell.Coords),OutAsh.FindRef(Cell.Coords)*.32f);
		if (DamageSeverity>0.f) Result.Add(Cell.Coords,DamageSeverity);
	}
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_VOLCANIC_FIELD mappedVent=%s lavaCells=%d ashCells=%d damageCells=%d downhillOnly=1 directionalAsh=1 hotWeatherTrigger=0 hypotheticalNotForecast=1"),*Terrain->VolcanicVentCoord.ToString(),OutLava.Num(),OutAsh.Num(),Result.Num());
	return Result;
}

TMap<FIntPoint,float> UDisasterSimulationSubsystem::SimulateDroughtMap(float BaseSeverity) const
{
	TMap<FIntPoint,float> Result; const UWorld* World=GetWorld();
	const UTerrainGridSubsystem* Terrain=World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || !FMath::IsFinite(BaseSeverity) || BaseSeverity<=0.f) return Result;
	for (const FGridCell& Cell:Terrain->Cells)
	{
		if (!Cell.bIsPlayable || Cell.bIsOcean) continue;
		const float Stress=FMath::Clamp(BaseSeverity*(.45f+.55f*(1.f-Cell.SoilMoisture01))*(Cell.bIsRiverbed ? .65f : 1.f),0.f,100.f);
		if (Stress>0.f) Result.Add(Cell.Coords,Stress);
	}
	return Result;
}

TMap<FIntPoint, float> UDisasterSimulationSubsystem::SimulateTsunamiMap(float BaseSeverity) const
{
	const double BuildStart = FPlatformTime::Seconds();
	TMap<FIntPoint, float> Result;
	const UWorld* World = GetWorld();
	const UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || !Terrain->IsCoastalWorld() || !FMath::IsFinite(BaseSeverity) || BaseSeverity <= 0.f) return Result;
	const float Strength = FMath::Clamp(BaseSeverity, 0.f, 100.f);
	// Explicit game-jam run-up calibration, not a tsunami forecast or CFD model.
	// A connected route must stay below this authored crest and loses energy per
	// cell/uphill step. Dams, river capacity, street drains and parks are not sea defences.
	const float PeakHeadCm = Terrain->CoastalSeaLevelCm + Strength * 6.f;
	TArray<float> Best; Best.Init(0.f, Terrain->Cells.Num());
	TArray<FIntPoint> Queue; Queue.Reserve(Terrain->Cells.Num());
	const FIntPoint Directions[] = { FIntPoint(0,-1), FIntPoint(1,0), FIntPoint(-1,0), FIntPoint(0,1) };
	for (const FGridCell& Sea : Terrain->Cells)
	{
		if (!Sea.bIsOcean) continue;
		Result.Add(Sea.Coords, Strength);
		for (FIntPoint Direction : Directions)
		{
			const FIntPoint Coord = Sea.Coords + Direction;
			if (!Terrain->IsPlayableCoord(Coord)) continue;
			const FGridCell Land = Terrain->GetCell(Coord);
			if (Land.bIsOcean || !FMath::IsFinite(Land.Elevation) || Land.Elevation >= PeakHeadCm) continue;
			const float Energy = Strength - 4.f - FMath::Max(0.f, Land.Elevation - Terrain->CoastalSeaLevelCm) * .07f;
			const int32 I = Terrain->CoordToIndex(Coord);
			if (Energy <= Best[I]) continue;
			Best[I] = Energy; Queue.Add(Coord);
		}
	}
	for (int32 Cursor = 0; Cursor < Queue.Num(); ++Cursor)
	{
		const FIntPoint Coord = Queue[Cursor];
		const FGridCell Cell = Terrain->GetCell(Coord);
		const float Energy = Best[Terrain->CoordToIndex(Coord)];
		if (Energy <= 0.f) continue;
		Result.Add(Coord, Energy);
		for (FIntPoint Direction : Directions)
		{
			const FIntPoint Next = Coord + Direction;
			if (!Terrain->IsPlayableCoord(Next)) continue;
			const FGridCell Neighbor = Terrain->GetCell(Next);
			if (Neighbor.bIsOcean || !FMath::IsFinite(Neighbor.Elevation) || Neighbor.Elevation >= PeakHeadCm) continue;
			const float StepLoss = (Terrain->ContinuationGeneratorVersion>=3?(Neighbor.bIsRiverbed?.7f:1.f):(Neighbor.bIsRiverbed?2.8f:4.f))*FMath::Max(.1f,Terrain->CellSize/500.f);
			const float Candidate = Energy - StepLoss - FMath::Max(0.f, Neighbor.Elevation - Cell.Elevation) * .07f;
			const int32 I = Terrain->CoordToIndex(Next);
			if (Candidate <= 0.f || Candidate <= Best[I] + .001f) continue;
			Best[I] = Candidate; Queue.Add(Next);
		}
	}
	int32 ReachedLand = 0;
	for (const TPair<FIntPoint, float>& Pair : Result) if (Terrain->IsPlayableCoord(Pair.Key)) ++ReachedLand;
	const FVector InlandDirection=Terrain->GetCoastInlandDirection();
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_TSUNAMI_FIELD coastStartRow=%d oceanAndLandCells=%d reachedLandCells=%d baseSeverity=%.2f authoredCrestHeadCm=%.2f inlandDirection=(%.1f,%.1f) workQueueEntries=%d cpuMs=%.3f units=DimensionlessGameplaySeverity runupCalibrationNotCFD=1 damProtection=0 drainProtection=0"),
		Terrain->CoastStartRow, Result.Num(), ReachedLand, Strength, PeakHeadCm, InlandDirection.X, InlandDirection.Y, Queue.Num(), (FPlatformTime::Seconds() - BuildStart) * 1000.0);
	return Result;
}

FAftershockDamQuote UDisasterSimulationSubsystem::QuoteDam(FIntPoint Coords) const
{
	FAftershockDamQuote Quote;
	const UWorld* World = GetWorld();
	const UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	const UEconomyAndStateSubsystem* Economy = World ? World->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	if (!World || (World->WorldType != EWorldType::Game && World->WorldType != EWorldType::PIE) || !Terrain || !Economy || Economy->bIsGameOver)
	{
		Quote.Feedback = NSLOCTEXT("Aftershock", "DamWorldUnavailable", "Baraj yatırımı için etkin oyun dünyası gerekli."); return Quote;
	}
	if (!Terrain->IsPlayableCoord(Coords)) { Quote.Feedback = NSLOCTEXT("Aftershock", "DamOutsideLand", "Baraj oynanabilir nehir yatağına kurulmalı; denize veya harita dışına kurulamaz."); return Quote; }
	const FGridCell Cell = Terrain->GetCell(Coords);
	if (Cell.bIsOcean || !Cell.bIsRiverbed || Cell.OccupyingBuildingID != INDEX_NONE || Cell.bHasRoad || Cell.bIsEmergencyAssemblyArea)
	{
		Quote.Feedback = NSLOCTEXT("Aftershock", "DamRiverOnly", "Baraj için boş bir nehir yatağı hücresi seç."); return Quote;
	}
	if (Cell.bHasDam) { Quote.Feedback = NSLOCTEXT("Aftershock", "DamAlreadyBuilt", "Bu hücrede zaten baraj var."); return Quote; }
	if (bDamTransactionInProgress || !Economy->CanAfford(Quote.CostTL))
	{
		Quote.Feedback = NSLOCTEXT("Aftershock", "DamInsufficientFunds", "Baraj 12.000 TL gerektirir; henüz ödeme alınmadı."); return Quote;
	}
	Quote.bCanBuild = true;
	Quote.Feedback = NSLOCTEXT("Aftershock", "DamReady", "12.000 TL | 180 olay birimi depolama. Yalnız bağlı mansap nehir taşkınını azaltır; tsunami savunması değildir.");
	return Quote;
}

bool UDisasterSimulationSubsystem::CanBuildDam(FIntPoint Coords, FText& OutReason) const
{
	const FAftershockDamQuote Quote = QuoteDam(Coords); OutReason = Quote.Feedback; return Quote.bCanBuild;
}

FAftershockDamStatus UDisasterSimulationSubsystem::GetDamStatus(FIntPoint Coords) const
{
	FAftershockDamStatus Status;
	const UWorld* World = GetWorld();
	const UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || !Terrain->IsValidCoord(Coords)) return Status;
	const FGridCell Cell = Terrain->GetCell(Coords);
	Status.bHasDam = Cell.bHasDam;
	if (!Status.bHasDam) return Status;
	Status.StorageCapacityUnits = FMath::Max(0.f, FMath::IsFinite(Cell.DamStorageCapacityUnits) ? Cell.DamStorageCapacityUnits : 0.f);
	Status.StoredWaterUnits = FMath::Clamp(FMath::IsFinite(Cell.DamStoredWaterUnits) ? Cell.DamStoredWaterUnits : 0.f, 0.f, Status.StorageCapacityUnits);
	Status.RemainingStorageUnits = Status.StorageCapacityUnits - Status.StoredWaterUnits;
	Status.LastRetainedUnits = FMath::Max(0.f, FMath::IsFinite(Cell.DamLastRetainedUnits) ? Cell.DamLastRetainedUnits : 0.f);
	Status.LastOvertoppingUnits = FMath::Max(0.f, FMath::IsFinite(Cell.DamLastOvertoppingUnits) ? Cell.DamLastOvertoppingUnits : 0.f);
	Status.bOvertopped = Status.LastOvertoppingUnits > .001f;
	return Status;
}

bool UDisasterSimulationSubsystem::InvestInDam(FIntPoint Coords, int32& OutCostTL)
{
	OutCostTL = 0;
	const FAftershockDamQuote Quote = QuoteDam(Coords);
	if (!Quote.bCanBuild) return false;
	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>();
	UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>();
	TGuardValue<bool> Guard(bDamTransactionInProgress, true);
	const FGridCell Original = Terrain->GetCell(Coords);
	FGridCell Updated = Original;
	Updated.bHasDam = true; Updated.DamStorageCapacityUnits = Quote.StorageCapacityUnits;
	Updated.DamStoredWaterUnits = 0.f; Updated.DamLastRetainedUnits = 0.f; Updated.DamLastOvertoppingUnits = 0.f;
	if(IsHydraulicModel2Active())
	{
		const FIntPoint Steps[]={FIntPoint(0,-1),FIntPoint(1,0),FIntPoint(0,1),FIntPoint(-1,0)};
		double Lowest=TNumericLimits<double>::Max();
		for(FIntPoint Step:Steps)
		{
			FGridCell N;double Bed;FText Error;
			if(Terrain->TryGetCell(Coords+Step,N)&&(N.bIsRiverbed||N.bIsOcean)&&TryGetAftershockHydraulicBedCm(N,true,Bed,Error)&&Bed<Lowest)
			{Lowest=Bed;Updated.DamOutflowStep=Step;}
		}
		if(Updated.DamOutflowStep==FIntPoint::ZeroValue)return false;
		TArray<FGridCell> Candidate=Terrain->Cells;Candidate[Terrain->CoordToIndex(Coords)]=Updated;
		TMap<FIntPoint,const FAftershockHydraulicCellSaveRecord*> ByCoord;
		for(const auto& Cell:PhysicalState.WaterCells)ByCoord.Add(Cell.Coord,&Cell);
		TArray<FAftershockHydraulicPersistentCell> Water;Water.Reserve(Candidate.Num());
		for(const auto& Cell:Candidate)
		{
			FAftershockHydraulicPersistentCell P;P.Coord=Cell.Coords;P.CapturedCellAreaM2=FMath::Square(Terrain->CellSize*.01);
			if(const auto* S=ByCoord.Find(Cell.Coords)){P.VolumeM3=(*S)->VolumeM3;P.HuM2PerSecond=(*S)->DischargeXM2PerSecond;P.HvM2PerSecond=(*S)->DischargeYM2PerSecond;}Water.Add(P);
		}
		FAftershockHydraulicTerrainInput Input;Input.Cells=Candidate;Input.PersistentCells=Water;Input.CellSizeCm=Terrain->CellSize;
		Input.CoastalSeaLevelCm=Terrain->CoastalSeaLevelCm;Input.SourceTerrainVersion=Terrain->ContinuationGeneratorVersion;Input.bModel2Active=true;
		FAftershockHydraulicTerrainBuild Geometry;FText Error;
		if(!BuildAftershockHydraulicTerrain(Input,Geometry,Error))
		{UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_DAM_PREFLIGHT_REJECTED costTL=0 protectionInstalled=0 reason=%s"),*Error.ToString());return false;}
	}
	if (!Terrain->SetCell(Coords, Updated)) return false;
	if (!Economy->TrySpendMoney(Quote.CostTL)) { Terrain->SetCell(Coords, Original); return false; }
	OutCostTL = Quote.CostTL;
	OnDamInfrastructureChanged.Broadcast(Coords);
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_DAM_BUILT coord=%s cost=%d capacityUnits=%.1f units=AuthoredEventPulse_NotCubicMetres downstreamAxis=PositiveGridY tsunamiProtection=0"), *Coords.ToString(), OutCostTL, Quote.StorageCapacityUnits);
	return true;
}

TMap<FIntPoint, float> UDisasterSimulationSubsystem::BuildDamRetentionMap(float RainfallIntensity, bool bCommitDamStorage)
{
	TMap<FIntPoint, float> Retention;
	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || !FMath::IsFinite(RainfallIntensity) || RainfallIntensity <= 0.f) return Retention;
	TArray<FIntPoint> Dams;
	for (const FGridCell& Cell : Terrain->Cells) if (Cell.bHasDam && Cell.bIsPlayable && Cell.bIsRiverbed && !Cell.bIsOcean) Dams.Add(Cell.Coords);
	Dams.Sort([](FIntPoint A, FIntPoint B) { return A.Y == B.Y ? A.X < B.X : A.Y < B.Y; });
	for (FIntPoint Coord : Dams)
	{
		FGridCell Dam = Terrain->GetCell(Coord);
		const FAftershockDamStatus Status = GetDamStatus(Coord);
		const float Incoming = FMath::Max(0.f, RainfallIntensity * (1.f - FMath::Clamp(Dam.WaterFlowDamping, 0.f, 1.f)) - Retention.FindRef(Coord));
		const float DesiredRetention = Incoming * .65f;
		const float Retained = FMath::Min(Status.RemainingStorageUnits, DesiredRetention);
		const float Overtopping = FMath::Max(0.f, DesiredRetention - Retained);
		TArray<FIntPoint> Queue; Queue.Add(Coord);
		TSet<FIntPoint> Visited; Visited.Add(Coord);
		// The existing river generator is parameterised north->south. This is an
		// explicit authored downstream convention, not an inferred CFD velocity.
		const FIntPoint Directions[] = { FIntPoint(1,0), FIntPoint(-1,0), FIntPoint(0,1) };
		for (int32 Cursor = 0; Cursor < Queue.Num(); ++Cursor)
		{
			const FIntPoint Current = Queue[Cursor];
			Retention.FindOrAdd(Current) += Retained;
			for (FIntPoint Direction : Directions)
			{
				const FIntPoint Next = Current + Direction;
				if (!Terrain->IsPlayableCoord(Next) || Visited.Contains(Next)) continue;
				const FGridCell Cell = Terrain->GetCell(Next);
				if (!Cell.bIsRiverbed || Cell.bIsOcean) continue;
				Visited.Add(Next); Queue.Add(Next);
			}
		}
		if (bCommitDamStorage)
		{
			Dam.DamStoredWaterUnits = FMath::Min(Status.StorageCapacityUnits, Status.StoredWaterUnits + Retained);
			Dam.DamLastRetainedUnits = Retained; Dam.DamLastOvertoppingUnits = Overtopping;
			Terrain->SetCell(Coord, Dam);
			OnDamInfrastructureChanged.Broadcast(Coord);
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_DAM_EVENT coord=%s incomingUnits=%.3f retainedUnits=%.3f remainingUnits=%.3f overtoppingUnits=%.3f downstreamRiverCells=%d storageCommitted=1 localRainPondingUnchanged=1 tsunamiProtection=0"),
				*Coord.ToString(), Incoming, Retained, Status.StorageCapacityUnits - Dam.DamStoredWaterUnits, Overtopping, Queue.Num());
		}
	}
	return Retention;
}

TMap<FIntPoint, float> UDisasterSimulationSubsystem::BuildFloodMapWithDams(float RainfallIntensity, bool bCommitDamStorage)
{
	if(const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr)
		if(Terrain->ContinuationGeneratorVersion>=3)return BuildPhysicalFloodMap(RainfallIntensity,bCommitDamStorage);
	TMap<FIntPoint, float> FloodSeverityMap;

	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || !FMath::IsFinite(RainfallIntensity) || RainfallIntensity<0.f)
	{
		return FloodSeverityMap;
	}

	// Global coordinates define the traversal order, independent of the order
	// in which neighbours were generated or their stable storage indices.
	TArray<FIntPoint> Traversal;
	Traversal.Reserve(Terrain->Cells.Num());
	for(const FGridCell& Cell:Terrain->Cells) Traversal.Add(Cell.Coords);
	Traversal.Sort([](FIntPoint A,FIntPoint B){return A.Y==B.Y?A.X<B.X:A.Y<B.Y;});
	const TMap<FIntPoint, float> DamRetention = BuildDamRetentionMap(RainfallIntensity, bCommitDamStorage);

	// 1) Nehir Yatağı Taşkın Hesabı ve Sokak Yağış Birikintisi
	for (const FIntPoint Coord : Traversal)
	{
		const int32 X=Coord.X, Y=Coord.Y;
		const FGridCell Cell = Terrain->GetCell(Coord);
		if (Cell.bIsOcean) continue;

		if (Cell.bIsRiverbed)
		{
			// Nehir yatağındaki etkin akış ve taşma
			const float EffectiveFlow = FMath::Max(0.f, RainfallIntensity * (1.0f - FMath::Clamp(Cell.WaterFlowDamping, 0.0f, 1.0f)) - DamRetention.FindRef(Coord));
			const float BaseCapacity = Cell.RiverCapacity * 25.0f;
			const float Overflow = FMath::Max(0.0f, EffectiveFlow - BaseCapacity);
			if (bCommitDamStorage)
			{
				FGridCell Filled=Cell;
				Filled.RiverFill01=FMath::Clamp(EffectiveFlow/FMath::Max(1.f,BaseCapacity),0.f,1.f);
				Terrain->SetCell(Coord,Filled);
			}
			// Capacity fills in the river first. Only its positive excess
			// reaches adjacent land; independent street rain is a separate source.

			// Nehir içi su şiddeti
			FloodSeverityMap.Add(Coord, FMath::Clamp(EffectiveFlow * 0.75f, 0.0f, 100.0f));

			// Taşma varsa komşulara yay
			if (Overflow > 0.0f)
			{
				const TArray<FIntPoint> Neighbors = {
					FIntPoint(X + 1, Y), FIntPoint(X - 1, Y),
					FIntPoint(X, Y + 1), FIntPoint(X, Y - 1)
				};

				for (const FIntPoint& NeighborCoord : Neighbors)
				{
					if (!Terrain->IsValidCoord(NeighborCoord))
					{
						continue;
					}

					const FGridCell Neighbor = Terrain->GetCell(NeighborCoord);
					if (Neighbor.bIsOcean) continue;
					const float ElevDiff = Neighbor.Elevation - Cell.Elevation;

					if (ElevDiff < 120.0f) // Yakın veya alçak kot
					{
						const float Attenuation = FMath::Clamp(1.0f - (ElevDiff / 120.0f), 0.25f, 1.0f);
						const float WaterSpread = Overflow * Attenuation * (1.0f - Neighbor.WaterFlowDamping * 0.5f);
						const float StreetDrainageRelief = Neighbor.DrainageQuality * 35.0f;
						const float LocalWater = FMath::Clamp(WaterSpread - StreetDrainageRelief, 0.0f, 100.0f);

						float& Existing = FloodSeverityMap.FindOrAdd(NeighborCoord);
						Existing = FMath::Max(Existing, LocalWater);
					}
				}
			}
		}
		else
		{
			// Şehir içi doğrudan sokak yağışı birikintisi
			const float LocalPonding = RainfallIntensity * 0.45f;
			const float DrainageRelief = Cell.DrainageQuality * 40.0f;
			const float StreetOverflow = FMath::Max(0.0f, LocalPonding - DrainageRelief);

			if (StreetOverflow > 0.0f)
			{
				float& Existing = FloodSeverityMap.FindOrAdd(Coord);
				Existing = FMath::Max(Existing, StreetOverflow);
			}
		}
	}

	// 2) İkinci Adım: Alçak kotlara süzülme ve hidrolik yayılım (Smoothing)
	for (const FIntPoint Coord : Traversal)
	{
		const int32 X=Coord.X, Y=Coord.Y;
		const float CurrentWater = FloodSeverityMap.FindRef(Coord);

		if (CurrentWater > 20.0f)
		{
			const FGridCell Cell = Terrain->GetCell(Coord);
			const TArray<FIntPoint> Neighbors = {
				FIntPoint(X + 1, Y), FIntPoint(X - 1, Y),
				FIntPoint(X, Y + 1), FIntPoint(X, Y - 1)
			};

			for (const FIntPoint& NeighborCoord : Neighbors)
			{
				if (!Terrain->IsValidCoord(NeighborCoord))
				{
					continue;
				}

				const FGridCell Neighbor = Terrain->GetCell(NeighborCoord);
				if (Neighbor.bIsOcean) continue;
				if (Neighbor.Elevation < Cell.Elevation)
				{
					const float SpreadWater = (CurrentWater * 0.25f) * (1.0f - Neighbor.DrainageQuality * 0.4f);
					float& TargetWater = FloodSeverityMap.FindOrAdd(NeighborCoord);
					TargetWater = FMath::Clamp(FMath::Max(TargetWater, SpreadWater), 0.0f, 100.0f);
				}
			}
		}
	}

	return FloodSeverityMap;
}

TMap<FIntPoint,float> UDisasterSimulationSubsystem::BuildPhysicalFloodMap(float RainfallIntensity,bool bCommitStorage)
{
	TMap<FIntPoint,float> Result;
	ComputedWaterDepthCm.Reset();ComputedWaterHeadCm.Reset();ComputedWaterArrival.Reset();ComputedInfiltrationCm.Reset();ComputedVelX.Reset();ComputedVelY.Reset();LastFloodBudget={};
	auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	if(!Terrain||!FMath::IsFinite(RainfallIntensity)||RainfallIntensity<=0.f||!FMath::IsFinite(Terrain->CellSize)||Terrain->CellSize<=0.f)return Result;
	// One authored intensity unit represents2mm over a one-hour event pulse.
	// These are explicit scenario inputs, not observed rain/river bathymetry.
	constexpr int32 Steps=48,MaximumCells=65536;
	constexpr double DamUnitM3=.25; // Legacy event storage unit =25m2 *1cm calibration.
	const double RainCm=FMath::Clamp(static_cast<double>(RainfallIntensity),0.,100.)*.2;
	const double AreaM2=FMath::Square(Terrain->CellSize*.01),CmToM3=AreaM2*.01;
	TArray<int32> Ordered;
	for(int32 I=0;I<Terrain->Cells.Num();++I)if(Terrain->Cells[I].bIsPlayable&&!Terrain->Cells[I].bIsOcean)Ordered.Add(I);
	if(Ordered.Num()>MaximumCells){UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_PHYSICAL_FLOOD_DOMAIN_REJECTED activeCells=%d cap=65536"),Ordered.Num());return Result;}
	Ordered.Sort([Terrain](int32 A,int32 B){const auto& P=Terrain->Cells[A].Coords;const auto& Q=Terrain->Cells[B].Coords;return P.Y==Q.Y?P.X<Q.X:P.Y<Q.Y;});
	const int32 Count=Ordered.Num();if(Count==0)return Result;
	TMap<FIntPoint,int32> Lookup;Lookup.Reserve(Count);
	for(int32 I=0;I<Count;++I)Lookup.Add(Terrain->Cells[Ordered[I]].Coords,I);
	TArray<double> Bed,Depth,Delta,Infiltration,Drainage,Recharge,DamRemaining,DamRetained;
	for(auto* Array:{&Bed,&Depth,&Delta,&Infiltration,&Drainage,&Recharge,&DamRemaining,&DamRetained})Array->Init(0.,Count);
	TArray<float> Arrival;Arrival.Init(-1.f,Count);TArray<int32> Neighbours;Neighbours.Init(-3,Count*4);
	const FIntPoint Directions[]={FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1)};
	const FIntRect Resident=Terrain->GetResidentCellBounds();
	TSet<FIntPoint> ParkParcels;
	if(const auto* Buildings=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>())
	{
		TArray<FBuildingInstance> Records;Buildings->GetAllBuildings(Records);
		for(const auto& B:Records)if(B.BuildingData&&B.BuildingData->Type==EBuildingType::GreenSpace&&B.CurrentDamage<50.f)ParkParcels.Add(B.GridCoords);
	}
	for(int32 I=0;I<Count;++I)
	{
		const auto& Cell=Terrain->Cells[Ordered[I]];
		const double CapacityCm=Cell.bIsRiverbed?FMath::Max(0.,static_cast<double>(Cell.RiverCapacity))*100.:0.;
		Bed[I]=Cell.Elevation-CapacityCm;
		Depth[I]=CapacityCm*FMath::Clamp(static_cast<double>(Cell.RiverFill01),0.,1.);
		LastFloodBudget.InitialRiverStorageM3+=Depth[I]*CmToM3;
		const bool Park=ParkParcels.Contains(Cell.Coords);
		const double Impervious=Park?.05:Cell.bHasRoad?.90:Cell.OccupyingBuildingID!=INDEX_NONE?.75:Cell.bAgriculturalLand?.15:.10;
		// Infiltration/drain capacities are centimetres per one-hour pulse. Saturated
		// soil and burned cover absorb less; urban cover does not remove rainfall.
		Infiltration[I]=Cell.bIsRiverbed?0.:(1.-Impervious)*(.6+1.8*(1.-FMath::Clamp(static_cast<double>(Cell.SoilMoisture01),0.,1.))
			+.9*FMath::Clamp(static_cast<double>(Cell.ForestFuel01),0.,1.)+(Park?1.5:0.))*(Cell.bBurning?.4:1.);
		Drainage[I]=Cell.bIsRiverbed?0.:8.*FMath::Clamp(static_cast<double>(Cell.DrainageQuality),0.,1.);
		if(Cell.bHasDam&&Cell.bIsRiverbed)DamRemaining[I]=FMath::Max(0.,static_cast<double>(Cell.DamStorageCapacityUnits-Cell.DamStoredWaterUnits))*DamUnitM3;
		for(int32 D=0;D<4;++D)
		{
			const FIntPoint Coord=Cell.Coords+Directions[D];
			if(const int32* Found=Lookup.Find(Coord))Neighbours[I*4+D]=*Found;
			else {FGridCell N;if(Terrain->TryGetCell(Coord,N)&&N.bIsOcean)Neighbours[I*4+D]=-1;else if(!Resident.Contains(Coord))Neighbours[I*4+D]=-2;}
		}
	}
	// A single kernel serves both execution modes. Every pass has a barrier;
	// neighbours are read only after every cell has completed rain/storage.
	// Workers own POD array slots and never call terrain/UObject methods.
	TArray<double> Transfers,DrainAccum,OutletAccum,FlowDamping,SurfaceElevation;
	Transfers.Init(0.,Count*4);DrainAccum.Init(0.,Count);OutletAccum.Init(0.,Count);
	FlowDamping.SetNumUninitialized(Count);SurfaceElevation.SetNumUninitialized(Count);
	for(int32 I=0;I<Count;++I)
	{
		const auto& Cell=Terrain->Cells[Ordered[I]];
		FlowDamping[I]=FMath::Clamp(static_cast<double>(Cell.WaterFlowDamping),0.,1.);
		SurfaceElevation[I]=Cell.Elevation;
	}
	const double CoastalHead=Terrain->CoastalSeaLevelCm;
	const EParallelForFlags Execution=CVarParallelFlood.GetValueOnGameThread()!=0
		?EParallelForFlags::None:EParallelForFlags::ForceSingleThread;
	for(int32 Step=0;Step<Steps;++Step)
	{
		ParallelFor(Count,[&](int32 I)
		{
			Depth[I]+=RainCm/Steps;
			const double Soak=FMath::Min(Depth[I],Infiltration[I]/Steps);Depth[I]-=Soak;Recharge[I]+=Soak;
			const double Drain=FMath::Min(Depth[I],Drainage[I]/Steps);Depth[I]-=Drain;DrainAccum[I]+=Drain;
			const double Retained=FMath::Min(DamRemaining[I],Depth[I]*CmToM3*.65);
			Depth[I]-=Retained/CmToM3;DamRemaining[I]-=Retained;DamRetained[I]+=Retained;
		},Execution);
		ParallelFor(Count,[&](int32 I)
		{
			const double Head=Bed[I]+Depth[I];double Desired[4]={0.,0.,0.,0.},Sum=0.;
			for(int32 D=0;D<4;++D)
			{
				const int32 N=Neighbours[I*4+D];if(N==-3)continue;
				const double OtherHead=N>=0?Bed[N]+Depth[N]:N==-1?CoastalHead:Bed[I];
				Desired[D]=FMath::Max(0.,Head-OtherHead)*.24*(1.-.70*FlowDamping[I]);Sum+=Desired[D];
			}
			const double Scale=Sum>0.?FMath::Min(1.,Depth[I]*.65/Sum):0.;
			for(int32 D=0;D<4;++D)Transfers[I*4+D]=Desired[D]*Scale;
		},Execution);
		ParallelFor(Count,[&](int32 I)
		{
			double LocalDelta=0.,LocalOutlet=0.;
			for(int32 D=0;D<4;++D)
			{
				const int32 N=Neighbours[I*4+D];if(N==-3)continue;
				const double Out=Transfers[I*4+D];LocalDelta-=Out;
				if(N>=0)LocalDelta+=Transfers[N*4+(D^1)];else LocalOutlet+=Out;
			}
			Delta[I]=LocalDelta;OutletAccum[I]+=LocalOutlet;
		},Execution);
		ParallelFor(Count,[&](int32 I)
		{
			Depth[I]=FMath::Max(0.,Depth[I]+Delta[I]);
			if(Arrival[I]<0.f&&Depth[I]>.1&&Bed[I]+Depth[I]>SurfaceElevation[I]+.1)Arrival[I]=static_cast<float>(Step+1)/Steps;
		},Execution);
	}
	ComputedVelX.Reset(); ComputedVelY.Reset();
	for (int32 I = 0; I < Count; ++I)
	{
		if (Depth[I] > 0.1)
		{
			const float Vx = static_cast<float>(Transfers[I*4+0] - Transfers[I*4+1]);
			const float Vy = static_cast<float>(Transfers[I*4+2] - Transfers[I*4+3]);
			if (FMath::Abs(Vx) > 0.0001f || FMath::Abs(Vy) > 0.0001f)
			{
				const FIntPoint Coord = Terrain->Cells[Ordered[I]].Coords;
				ComputedVelX.Add(Coord, Vx);
				ComputedVelY.Add(Coord, Vy);
			}
		}
	}
	// Ordered main-thread reduction avoids shared atomic/cache-line contention.
	// EdgeVisits preserves the original attempted-face work metric, including walls.
	LastFloodBudget.RainfallM3=RainCm*Count*CmToM3;
	LastFloodBudget.EdgeVisits=static_cast<int64>(Steps)*Count*4;
	for(int32 I=0;I<Count;++I)
	{
		LastFloodBudget.InfiltrationM3+=Recharge[I]*CmToM3;
		LastFloodBudget.DrainageM3+=DrainAccum[I]*CmToM3;
		LastFloodBudget.DamRetainedM3+=DamRetained[I];
		LastFloodBudget.OutletM3+=OutletAccum[I]*CmToM3;
	}
	for(int32 I=0;I<Count;++I)
	{
		const FGridCell Before=Terrain->Cells[Ordered[I]];const FIntPoint Coord=Before.Coords;
		LastFloodBudget.FinalStorageM3+=Depth[I]*CmToM3;ComputedInfiltrationCm.Add(Coord,static_cast<float>(Recharge[I]));
		if(Depth[I]>.001)
		{
			const float Head=static_cast<float>(Bed[I]+Depth[I]);ComputedWaterDepthCm.Add(Coord,static_cast<float>(Depth[I]));ComputedWaterHeadCm.Add(Coord,Head);
			ComputedWaterArrival.Add(Coord,Arrival[I]<0.f?1.f:Arrival[I]);
			const double Exposure=Before.bIsRiverbed?Depth[I]:FMath::Max(0.,Bed[I]+Depth[I]-Before.Elevation);
			const float Severity=static_cast<float>(100.*(1.-FMath::Exp(-Exposure/80.)));if(Severity>.001f)Result.Add(Coord,Severity);
		}
		if(bCommitStorage&&(Before.bIsRiverbed||Before.bHasDam))
		{
			FGridCell After=Before;
			if(Before.bIsRiverbed)After.RiverFill01=static_cast<float>(FMath::Clamp(Depth[I]/FMath::Max(1.,static_cast<double>(Before.RiverCapacity)*100.),0.,1.));
			if(Before.bHasDam){After.DamStoredWaterUnits=FMath::Clamp(Before.DamStoredWaterUnits+static_cast<float>(DamRetained[I]/DamUnitM3),0.f,Before.DamStorageCapacityUnits);After.DamLastRetainedUnits=static_cast<float>(DamRetained[I]/DamUnitM3);After.DamLastOvertoppingUnits=DamRemaining[I]<=1.e-6?static_cast<float>(Depth[I]*CmToM3/DamUnitM3):0.f;}
			Terrain->SetCell(Coord,After);if(Before.bHasDam)OnDamInfrastructureChanged.Broadcast(Coord);
		}
	}
	LastFloodBudget.ActiveCells=Count;LastFloodBudget.Iterations=Steps;LastFloodBudget.bCommittedStorage=bCommitStorage;
	LastFloodBudget.MassResidualM3=LastFloodBudget.InitialRiverStorageM3+LastFloodBudget.RainfallM3-LastFloodBudget.InfiltrationM3-LastFloodBudget.DrainageM3-LastFloodBudget.OutletM3-LastFloodBudget.DamRetainedM3-LastFloodBudget.FinalStorageM3;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_FLOOD version=1 activeCells=%d iterations=48 edgeVisits=%lld rainMm=%.3f pulseSeconds=3600 initialM3=%.6f rainM3=%.6f infiltrationM3=%.6f drainM3=%.6f outletM3=%.6f damM3=%.6f finalM3=%.6f residualM3=%.9f openResidentBoundary=1 authoredChannelDepthCm=100*capacity damUnitM3=.25 forecast=0"),
		Count,LastFloodBudget.EdgeVisits,RainCm*10.,LastFloodBudget.InitialRiverStorageM3,LastFloodBudget.RainfallM3,LastFloodBudget.InfiltrationM3,LastFloodBudget.DrainageM3,LastFloodBudget.OutletM3,LastFloodBudget.DamRetainedM3,LastFloodBudget.FinalStorageM3,LastFloodBudget.MassResidualM3);
	return Result;
}

FDisasterReport UDisasterSimulationSubsystem::TriggerDisaster(
	EDisasterType Type,
	float BaseSeverity,
	float CityAwarenessLevel,
	bool bIsDaytime)
{
	FDisasterReport Report;
	Report.DisasterType = Type;
	Report.BaseSeverity = BaseSeverity;
	// Reject invalid public API input before snapshots, terrain, casualties or
	// economy delegates can change. Zero intensity remains a valid audit input.
	if (!FMath::IsFinite(BaseSeverity) || BaseSeverity < 0.f || !FMath::IsFinite(CityAwarenessLevel))
	{
		Report.bWasRejected = true;
		Report.RejectionReason = NSLOCTEXT("Aftershock", "InvalidDisasterInput", "Afet şiddeti sıfır veya daha büyük, farkındalık ise geçerli bir sayı olmalı.");
		UE_LOG(LogTemp, Warning, TEXT("AFTERSHOCK_DISASTER_REJECTED type=%d invalidNumericInput=1 completionEventEmitted=0 snapshotChanged=0 economyChanged=0"), static_cast<int32>(Type));
		return Report;
	}
	BaseSeverity=FMath::Clamp(BaseSeverity,0.f,100.f);
	CityAwarenessLevel=FMath::Clamp(CityAwarenessLevel,0.f,1.f);
	Report.BaseSeverity=BaseSeverity;

	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	UBuildingManagerSubsystem* BuildingMgr = World ? World->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
	// Live geographical worlds use a persistent SI frontier. Legacy isolated
	// fixtures keep their versioned contract rather than silently changing units.
	if ((Type==EDisasterType::Avalanche || Type==EDisasterType::Landslide)
		|| ((Type==EDisasterType::Flood || Type==EDisasterType::Tsunami)
			&& Terrain && Terrain->ContinuationGeneratorVersion==3
			&& World && (World->WorldType==EWorldType::Game || World->WorldType==EWorldType::PIE)))
		return BeginPhysicalDisaster(Type,BaseSeverity,CityAwarenessLevel,bIsDaytime);
	if (IsPhysicalEventRunning())
	{ Report.bWasRejected=true;Report.RejectionReason=NSLOCTEXT("Aftershock","PhysicalBusy","Önce sürmekte olan fiziksel afetin tamamlanmasını bekle.");return Report; }

	if (!Terrain || !BuildingMgr)
	{
		if (Type == EDisasterType::Tsunami)
		{
			Report.bWasRejected = true;
			Report.RejectionReason = NSLOCTEXT("Aftershock", "TsunamiWorldUnavailable", "Tsunami için kıyı dünyası ve şehir yöneticisi hazır olmalı.");
		}
		return Report;
	}
	if (Type == EDisasterType::Flood || Type == EDisasterType::Tsunami || Type==EDisasterType::Volcano || Type==EDisasterType::Fire || Type==EDisasterType::Drought)
	{
		FText Reason;
		if (!CanTriggerDisaster(Type, Reason) || !FMath::IsFinite(BaseSeverity))
		{
			Report.bWasRejected = true;
			Report.RejectionReason = Reason.IsEmpty() ? NSLOCTEXT("Aftershock", "TsunamiInvalidInput", "Tsunami şiddeti geçerli bir sayı olmalı.") : Reason;
			UE_LOG(LogTemp, Warning, TEXT("AFTERSHOCK_TSUNAMI_REJECTED reason=%s completionEventEmitted=0 snapshotChanged=0 economyChanged=0"), *Report.RejectionReason.ToString());
			return Report;
		}
		BaseSeverity = FMath::Clamp(BaseSeverity, 0.f, 100.f);
		Report.BaseSeverity = BaseSeverity;
	}

	FAftershockScopedCrowdRefresh CrowdRefreshBatch(World->GetSubsystem<UCrowdRenderSubsystem>());
	TArray<FBuildingInstance> AllBuildings;
	BuildingMgr->GetAllBuildings(AllBuildings);
	UCityLifeSubsystem* Life=World->GetSubsystem<UCityLifeSubsystem>();
	if (Life && !Life->IsCityInitialized()) Life->InitializeForCity();
	if (Life && Life->IsCityInitialized()) Life->RefreshCity();
	FAftershockScopedOccupancyBatch OccupancyBatch(Life);
	Report.DisasterEventID=FGuid::NewGuid();
	const TArray<FCityResidentRecord> BeforeResidents=Life?Life->GetResidents():TArray<FCityResidentRecord>();
	const TArray<FBuildingInstance> BeforeBuildings=AllBuildings;
	TMap<int32,TArray<int32>> ActualOccupants;
	for(const auto& Resident:BeforeResidents)if(Resident.bAlive)
		ActualOccupants.FindOrAdd(bIsDaytime&&Resident.WorkBuildingID!=INDEX_NONE?Resident.WorkBuildingID:Resident.HomeBuildingID).Add(Resident.ResidentID);
	TSet<int32> ExposedIDs;

	// 1) Hastane Kurtarma Bonusu (BUILDING_STATS.md Bölüm 1: Hastane afet sonrası kurtarma hızını artırır)
	float ActiveHospitals = 0.f;
	int32 ActiveWindTurbines = 0;
	int32 ActiveGreenSpaces = 0;

	for (const FBuildingInstance& B : AllBuildings)
	{
		if (B.BuildingData && B.CurrentDamage < 50.0f)
		{
			if (B.BuildingData->Type == EBuildingType::Hospital)
			{
				ActiveHospitals+=Life && Life->IsCityInitialized() ? Life->GetBuildingOperation01(B.InstanceID) : 1.f;
			}
			else if (B.BuildingData->Type == EBuildingType::WindTurbine)
			{
				ActiveWindTurbines++;
			}
			else if (B.BuildingData->Type == EBuildingType::GreenSpace)
			{
				ActiveGreenSpaces++;
			}
		}
	}

	// Hastane başına kurtarma hızı x2 (ölüm oranında düşüş)
	const float HospitalRescueMultiplier = 1.0f / (1.0f + static_cast<float>(ActiveHospitals) * 0.5f);

	// Turbines supply the canonical power/CO2 economy; they do NOT weaken
	// incoming meteorological storms. Keep the explicit zero across the existing
	// shared damage/display paths rather than inventing a weather benefit.
	const float WindTurbineProtection = 0.f;
	// Existing park retention is an authored city-scale gameplay abstraction,
	// not measured rainfall removal, a forecast or a local hydraulic solver.
	const float GreenSpaceFloodProtection = Terrain->ContinuationGeneratorVersion>=3?0.f:FMath::Min(0.40f, static_cast<float>(ActiveGreenSpaces) * 0.08f);
	if (Type==EDisasterType::Wind) UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_WIND_TURBINES_POWER_ONLY turbines=%d incomingStormReduction=0 units=AuthoredGameplayIntensity"),ActiveWindTurbines);

	// 2) Sel ise Dinamik Sel Haritası Üret
	TMap<FIntPoint, float> FloodMap;
	TMap<FIntPoint,float> HazardMap,LavaMap,AshMap,LavaArrival;
	TMap<FIntPoint,float> RiverFillBefore;
	if (Type == EDisasterType::Flood)
	{
		for (const FGridCell& Cell:Terrain->Cells) if (Cell.bIsPlayable && Cell.bIsRiverbed && !Cell.bIsOcean) RiverFillBefore.Add(Cell.Coords,FMath::Clamp(Cell.RiverFill01,0.f,1.f));
		const float EffectiveRainfall = BaseSeverity * (1.0f - GreenSpaceFloodProtection);
		FloodMap = BuildFloodMapWithDams(EffectiveRainfall, true);
		// The SAME rainfall input replenishes soil/source storage and breaks a
		// persistent dry spell. A marine tsunami is saltwater, never drought relief.
		if (FMath::IsFinite(EffectiveRainfall) && EffectiveRainfall>0.f)
		{
			const float Rain01=FMath::Clamp(EffectiveRainfall/100.f,0.f,1.f);
			if(Terrain->ContinuationGeneratorVersion<3)Terrain->ActiveDroughtSeverity=FMath::Max(0.f,Terrain->ActiveDroughtSeverity-EffectiveRainfall*.9f);
			float TotalRecharge=0.f;int32 RechargedCells=0;
			for (FGridCell& Cell:Terrain->Cells)
			{
				if (!Cell.bIsPlayable || Cell.bIsOcean) continue;
				const float Recharge=Terrain->ContinuationGeneratorVersion>=3?ComputedInfiltrationCm.FindRef(Cell.Coords)/10.f:Rain01*.42f;
				Cell.SoilMoisture01=FMath::Min(1.f,Cell.SoilMoisture01+Recharge);TotalRecharge+=Recharge;++RechargedCells;
				Cell.DroughtStress01=FMath::Min(Cell.DroughtStress01,1.f-Cell.SoilMoisture01);
				if (!Cell.bIsRiverbed) Cell.RiverFill01=FMath::Min(1.f,Cell.RiverFill01+Rain01*.28f);
			}
			if(Terrain->ContinuationGeneratorVersion>=3&&RechargedCells>0)Terrain->ActiveDroughtSeverity=FMath::Max(0.f,Terrain->ActiveDroughtSeverity-TotalRecharge/RechargedCells*100.f);
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_RAINFALL_RECOVERY sameRainfallInput=%.2f activeDroughtSeverity=%.2f soilReplenished=1 cropRegrowthRequiresFutureTurns=1 tsunamiRecovery=0"),EffectiveRainfall,Terrain->ActiveDroughtSeverity);
		}
	}
	else if (Type == EDisasterType::Tsunami)
	{
		FloodMap = SimulateTsunamiMap(BaseSeverity);
	}
	else if (Type==EDisasterType::Fire) HazardMap=SimulateForestFireMap(BaseSeverity);
	else if (Type==EDisasterType::Volcano) HazardMap=BuildVolcanoField(BaseSeverity,LavaMap,AshMap,LavaArrival);
	else if (Type==EDisasterType::Drought) HazardMap=SimulateDroughtMap(BaseSeverity);
	if (Type==EDisasterType::Fire || Type==EDisasterType::Volcano || Type==EDisasterType::Drought)
	{
		float CropLossSum=0.f,WaterStressSum=0.f; int32 LandCount=0;
		for (FGridCell& Cell:Terrain->Cells)
		{
			if (!Cell.bIsPlayable || Cell.bIsOcean) continue;
			const float Severity01=FMath::Clamp(HazardMap.FindRef(Cell.Coords)/100.f,0.f,1.f);
			if (Type==EDisasterType::Fire && Severity01>0.f)
			{
				Cell.bBurning=true; Cell.BurningTurns=3;
				Cell.ForestFuel01=FMath::Max(0.f,Cell.ForestFuel01-.35f*Severity01);
				Cell.SoilMoisture01=FMath::Max(0.f,Cell.SoilMoisture01-.18f*Severity01);
				++Report.ForestCellsBurned;
			}
			if (Type==EDisasterType::Volcano)
			{
				Cell.LavaCoverage01=FMath::Clamp(LavaMap.FindRef(Cell.Coords)/100.f,0.f,1.f);
				Cell.AshLoad01=FMath::Clamp(AshMap.FindRef(Cell.Coords)/100.f,0.f,1.f);
				if (Cell.LavaCoverage01>0.f) { Cell.ForestFuel01=FMath::Max(0.f,Cell.ForestFuel01-Cell.LavaCoverage01); Cell.CropHealth01*=1.f-Cell.LavaCoverage01; }
			}
			if (Type==EDisasterType::Drought && Severity01>0.f)
			{
				// Starts a dry-spell pulse; further depletion occurs on the existing
				// environmental turns. No instant season of water loss or collapse.
				Cell.SoilMoisture01=FMath::Max(0.f,Cell.SoilMoisture01-.025f*Severity01);
				Cell.RiverFill01=FMath::Max(0.f,Cell.RiverFill01-.035f*Severity01);
				Cell.DroughtStress01=FMath::Max(Cell.DroughtStress01,1.f-Cell.SoilMoisture01);
				if (Cell.bAgriculturalLand) Cell.CropHealth01=FMath::Max(0.f,Cell.CropHealth01-.025f*Severity01);
			}
			WaterStressSum+=Cell.DroughtStress01; CropLossSum+=1.f-Cell.CropHealth01; ++LandCount;
		}
		if (Type==EDisasterType::Drought) Terrain->ActiveDroughtSeverity=FMath::Max(Terrain->ActiveDroughtSeverity,BaseSeverity);
		Report.WaterSupplyStress01=LandCount>0 ? WaterStressSum/LandCount : 0.f;
		Report.CropLoss01=LandCount>0 ? CropLossSum/LandCount : 0.f;
	}

	// Cache the SAME map used below, so the visible flood cannot roll another event
	// or invent a different drainage/river result. Values are severity, not metres.
	const int32 NextVisualRevision = LatestVisualSnapshot.Revision + 1;
	LatestVisualSnapshot = FAftershockDisasterVisualSnapshot();
	LatestVisualSnapshot.Revision = NextVisualRevision;
	LatestVisualSnapshot.Type = Type;
	LatestVisualSnapshot.BaseSeverity = BaseSeverity;
	LatestVisualSnapshot.StartedAtWorldSeconds = World->GetTimeSeconds();
	LatestVisualSnapshot.DurationSeconds = Type == EDisasterType::Tsunami || Type==EDisasterType::Volcano ? 32.f : ((Type == EDisasterType::Flood || Type == EDisasterType::Wind || Type==EDisasterType::Fire || Type==EDisasterType::Drought) ? 24.f : (Type == EDisasterType::Earthquake ? 14.f : 0.f));
	LatestVisualSnapshot.EffectiveIntensity = BaseSeverity;
	FVector DisplayDirection(ScenarioWindDirection.X, ScenarioWindDirection.Y, 0.f);
	if (DisplayDirection.ContainsNaN() || DisplayDirection.IsNearlyZero()) DisplayDirection = FVector(1.f, 0.35f, 0.f);
	LatestVisualSnapshot.DominantWindDirection = DisplayDirection.GetSafeNormal();
	if (Type == EDisasterType::Flood)
	{
		LatestVisualSnapshot.EffectiveIntensity = BaseSeverity * (1.0f - GreenSpaceFloodProtection);
		LatestVisualSnapshot.LocalSeverity = FloodMap;
		if(Terrain->ContinuationGeneratorVersion>=3)
		{
			LatestVisualSnapshot.FloodModelVersion=1;LatestVisualSnapshot.RainfallPulseMm=FMath::Clamp(BaseSeverity,0.f,100.f)*2.f;
			LatestVisualSnapshot.FloodPulseDurationSeconds=3600.f;LatestVisualSnapshot.WaterDepthCm=MoveTemp(ComputedWaterDepthCm);
			LatestVisualSnapshot.WaterSurfaceElevationCm=MoveTemp(ComputedWaterHeadCm);LatestVisualSnapshot.WaterArrivalFraction=MoveTemp(ComputedWaterArrival);
			ComputedInfiltrationCm.Empty();
		}
		LatestVisualSnapshot.RiverFillBefore=MoveTemp(RiverFillBefore);
		for (const FGridCell& Cell:Terrain->Cells) if (Cell.bIsPlayable && Cell.bIsRiverbed && !Cell.bIsOcean) LatestVisualSnapshot.RiverFillAfter.Add(Cell.Coords,FMath::Clamp(Cell.RiverFill01,0.f,1.f));
		float PeakFloodScore = -1.f;
		FIntPoint PeakFloodCoord(INDEX_NONE, INDEX_NONE);
		for (const auto& Pair : FloodMap)
		{
			if (Pair.Value <= 0.f || !Terrain->IsPlayableCoord(Pair.Key)) continue;
			const FGridCell& Cell = Terrain->GetCell(Pair.Key);
			// Heavily prioritize inundated buildings and city streets over uninhabited wilderness riverbeds
			float Weight = 0.05f;
			if (Cell.OccupyingBuildingID != INDEX_NONE) Weight = 25.0f;
			else if (Cell.bHasRoad) Weight = 8.0f;
			else if (!Cell.bIsRiverbed && !Cell.bIsOcean) Weight = 3.0f;
			const float Score = Pair.Value * Weight;
			if (Score > PeakFloodScore)
			{
				PeakFloodScore = Score;
				PeakFloodCoord = Pair.Key;
			}
		}
		if (PeakFloodCoord == FIntPoint(INDEX_NONE, INDEX_NONE))
		{
			// Fallback: pick river cell closest to city buildings
			float MinDistSq = MAX_flt;
			for (const auto& Pair : FloodMap)
			{
				if (Pair.Value <= 0.f || !Terrain->IsPlayableCoord(Pair.Key)) continue;
				for (const FBuildingInstance& B : AllBuildings)
				{
					const float DistSq = FVector2D::DistSquared(FVector2D(Pair.Key), FVector2D(B.GridCoords));
					if (DistSq < MinDistSq)
					{
						MinDistSq = DistSq;
						PeakFloodCoord = Pair.Key;
					}
				}
			}
		}
		LatestVisualSnapshot.SourceCoord = PeakFloodCoord;
	}
	else if (Type == EDisasterType::Tsunami)
	{
		LatestVisualSnapshot.LocalSeverity = FloodMap;
		LatestVisualSnapshot.PropagationDirection = Terrain->GetCoastInlandDirection();
		LatestVisualSnapshot.SurgePeakHeadCm = Terrain->CoastalSeaLevelCm + BaseSeverity * 6.f;
		float PeakTsunamiScore = -1.f;
		FIntPoint PeakTsunamiCoord(INDEX_NONE, INDEX_NONE);
		for (const auto& Pair : FloodMap)
		{
			if (Pair.Value <= 0.f || !Terrain->IsPlayableCoord(Pair.Key)) continue;
			const FGridCell& Cell = Terrain->GetCell(Pair.Key);
			if (Cell.bIsOcean) continue;
			float Weight = (Cell.OccupyingBuildingID != INDEX_NONE) ? 20.f : (Cell.bHasRoad ? 6.f : 2.f);
			const float Score = Pair.Value * Weight;
			if (Score > PeakTsunamiScore)
			{
				PeakTsunamiScore = Score;
				PeakTsunamiCoord = Pair.Key;
			}
		}
		LatestVisualSnapshot.SourceCoord = (PeakTsunamiCoord != FIntPoint(INDEX_NONE, INDEX_NONE))
			? PeakTsunamiCoord : FIntPoint(Terrain->GridWidth / 2, FMath::Max(0, Terrain->CoastStartRow));
	}
	else if (Type == EDisasterType::Wind)
	{
		LatestVisualSnapshot.EffectiveIntensity = BaseSeverity * (1.0f - WindTurbineProtection);
		LatestVisualSnapshot.LocalSeverity=SimulateWindMap(LatestVisualSnapshot.EffectiveIntensity);
		float PeakWindScore = -1.f;
		FIntPoint PeakWindCoord(INDEX_NONE, INDEX_NONE);
		for (const FGridCell& Cell : Terrain->Cells)
		{
			const float CellWind = LatestVisualSnapshot.LocalSeverity.FindRef(Cell.Coords);
			if (!Cell.bIsPlayable || Cell.bIsOcean) continue;
			float Weight = 0.2f;
			if (Cell.OccupyingBuildingID != INDEX_NONE) Weight = 15.0f;
			else if (Cell.bHasRoad) Weight = 5.0f;
			else if (!Cell.bIsRiverbed) Weight = 2.0f;
			const float Score = CellWind * Weight;
			if (Score > PeakWindScore)
			{
				PeakWindScore = Score;
				PeakWindCoord = Cell.Coords;
			}
		}
		LatestVisualSnapshot.SourceCoord = PeakWindCoord;
	}
	else if (Type == EDisasterType::Earthquake)
	{
		LatestVisualSnapshot.EffectiveIntensity = BaseSeverity;
		float PeakQuakeScore = -1.f;
		FIntPoint PeakQuakeCoord(INDEX_NONE, INDEX_NONE);
		for (const FGridCell& Cell : Terrain->Cells)
		{
			if (!Cell.bIsPlayable) continue;
			// Mirror the existing canonical terrain evaluation for presentation.
			// Building evaluation below receives BaseSeverity and applies this once.
			const float CellQuakeSeverity = BaseSeverity * (1.0f + Cell.FaultLineProximity * 0.5f)
				* (Cell.SoilType == EAftershockSoilType::Alluvium ? 1.18f : 1.0f);
			LatestVisualSnapshot.LocalSeverity.Add(Cell.Coords, CellQuakeSeverity);
			// Prioritize populated urban parcels and roads along fault line; never pick empty riverbed!
			float UrbanWeight = 0.05f;
			if (Cell.OccupyingBuildingID != INDEX_NONE) UrbanWeight = 20.0f;
			else if (Cell.bHasRoad) UrbanWeight = 6.0f;
			else if (!Cell.bIsRiverbed && !Cell.bIsOcean) UrbanWeight = 2.0f;
			const float Score = CellQuakeSeverity * UrbanWeight;
			if (Score > PeakQuakeScore)
			{
				PeakQuakeScore = Score;
				PeakQuakeCoord = Cell.Coords;
			}
		}
		LatestVisualSnapshot.SourceCoord = PeakQuakeCoord;
	}
	else if (Type==EDisasterType::Fire || Type==EDisasterType::Volcano || Type==EDisasterType::Drought)
	{
		LatestVisualSnapshot.LocalSeverity=HazardMap;
		LatestVisualSnapshot.LavaSeverity=LavaMap; LatestVisualSnapshot.AshSeverity=AshMap;
		LatestVisualSnapshot.LavaArrivalFraction=LavaArrival;
		LatestVisualSnapshot.SourceCoord=Type==EDisasterType::Volcano ? Terrain->VolcanicVentCoord : FIntPoint(INDEX_NONE,INDEX_NONE);
		LatestVisualSnapshot.DroughtStress01=Report.WaterSupplyStress01; LatestVisualSnapshot.CropLoss01=Report.CropLoss01;
		if (Type==EDisasterType::Fire)
		{
			float PeakFireScore = -1.f;
			FIntPoint PeakFireCoord(INDEX_NONE, INDEX_NONE);
			for (const FGridCell& Cell : Terrain->Cells)
			{
				const float Haz = HazardMap.FindRef(Cell.Coords);
				if (Haz <= 0.f || !Cell.bIsPlayable) continue;
				float Weight = (Cell.OccupyingBuildingID != INDEX_NONE) ? 25.f : (Cell.bHasRoad ? 6.f : (!Cell.bIsRiverbed ? 2.f : 0.05f));
				const float Score = Haz * Weight;
				if (Score > PeakFireScore)
				{
					PeakFireScore = Score;
					PeakFireCoord = Cell.Coords;
				}
			}
			LatestVisualSnapshot.SourceCoord = PeakFireCoord;
		}
		else if (Type==EDisasterType::Drought)
		{
			float PeakStressScore = -1.f;
			FIntPoint PeakDroughtCoord(INDEX_NONE, INDEX_NONE);
			for (const FGridCell& Cell : Terrain->Cells)
			{
				if (!Cell.bIsPlayable || Cell.bIsOcean || Cell.bIsRiverbed) continue;
				float Weight = Cell.bAgriculturalLand ? 8.f : (Cell.OccupyingBuildingID != INDEX_NONE ? 3.f : 1.f);
				const float Score = Cell.DroughtStress01 * Weight;
				if (Score > PeakStressScore)
				{
					PeakStressScore = Score;
					PeakDroughtCoord = Cell.Coords;
				}
			}
			LatestVisualSnapshot.SourceCoord = PeakDroughtCoord;
		}
	}
	// 3) Binaları Değerlendir
	for (FBuildingInstance& Building : AllBuildings)
	{
		if(Building.DamageState==EDamageState::Memorial || Building.DamageState==EDamageState::Collapsed)continue;
		float LocalSeverity = BaseSeverity;
		if (Type == EDisasterType::Tsunami)
		{
			LocalSeverity = FloodMap.FindRef(Building.GridCoords);
			if (LocalSeverity <= 0.f)
			{
				// Unreached inland lots are survivors, never evaluated with a global
				// fallback severity or charged another round of existing damage.
				continue;
			}
		}

		if (Type == EDisasterType::Flood)
		{
			if (const float* Found = FloodMap.Find(Building.GridCoords))
			{
				LocalSeverity = *Found;
			}
			else
			{
				// Rain does not expose an otherwise dry/unreached building.
				LocalSeverity = 0.f;
			}
		}
		else if (Type == EDisasterType::Wind)
		{
			LocalSeverity = LatestVisualSnapshot.LocalSeverity.FindRef(Building.GridCoords);
		}
		// Earthquake keeps BaseSeverity: EvaluateBuildingInDisaster owns its
		// canonical terrain amplification. The visual field is already amplified.
		else if (Type==EDisasterType::Fire || Type==EDisasterType::Volcano || Type==EDisasterType::Drought) LocalSeverity=HazardMap.FindRef(Building.GridCoords);
		if(LocalSeverity>0.f)if(const auto* At=ActualOccupants.Find(Building.InstanceID))for(int32 ID:*At)ExposedIDs.Add(ID);
		if (Type==EDisasterType::Drought || LocalSeverity<=0.f)
		{
			continue;
		}

		// Oyuncunun yatırımları olmasaydı (Weak temel, retrofit yok, 0 drenaj) oluşacak teorik kayıp
		const int32 TheoreticalZeroPrevDeaths = CalculateTheoreticalZeroPreventionDeaths(
			Building,
			Type == EDisasterType::Tsunami ? EDisasterType::Flood : Type==EDisasterType::Volcano ? EDisasterType::Fire : Type,
			LocalSeverity,
			CityAwarenessLevel,
			bIsDaytime
		);

		const bool bWasAlreadyCollapsed = (Building.DamageState == EDamageState::Collapsed);

		// Afet hasarını ve kayıplarını hesapla
		FDisasterOutcome Outcome = Terrain->EvaluateBuildingInDisaster(
			Building,
			Type,
			LocalSeverity,
			CityAwarenessLevel,
			bIsDaytime,
			Type==EDisasterType::Wind || (Type==EDisasterType::Flood&&Terrain->ContinuationGeneratorVersion>=3)
		);

		// Hastane kurtarma bonusunu uygula
		const int32 RequestedDeaths = FMath::RoundToInt(static_cast<float>(Outcome.Deaths) * HospitalRescueMultiplier);
		const TArray<int32> AssignedDeaths=Life && Life->IsCityInitialized()?Life->ApplyCasualties(Building.InstanceID,RequestedDeaths,bIsDaytime):TArray<int32>();
		Report.ActualDeathResidentIDs.Append(AssignedDeaths);
		const int32 AdjustedDeaths=AssignedDeaths.Num();
		Outcome.Survivors += (Outcome.Deaths - AdjustedDeaths);
		Outcome.Deaths = AdjustedDeaths;
		Outcome.SatisfactionChange = -static_cast<float>(AdjustedDeaths) * 0.3f;
		Outcome.StateAidNextTurn = 0; // Canonical event settlement uses actual IDs below.
		if (Life && Life->IsCityInitialized()) Outcome.Survivors=Life->GetLivingOccupants(Building.InstanceID,bIsDaytime);

		// Önlenen can kaybı farkı
		const int32 Prevented = FMath::Max(0, TheoreticalZeroPrevDeaths - AdjustedDeaths);
		Report.PreventedDeathsEstimate += Prevented;

		if (Outcome.FinalDamage > 0.0f)
		{
			Report.TotalBuildingsAffected++;
		}

		// Binanın durumunu güncelle
		BuildingMgr->UpdateBuildingInstance(Building);
		BuildingMgr->UpdateBuildingVisualState(Building.InstanceID);

		// 4) Chaos Destruction Kontrolü (Sadece yeni çöken binalar)
		if (Outcome.NewDamageState == EDamageState::Collapsed && !bWasAlreadyCollapsed)
		{
			Report.TotalBuildingsCollapsed++;
			SpawnChaosDestructionForBuilding(Building);
		}

		// Rapor toplamları
		Report.TotalDeaths += Outcome.Deaths;
		Report.TotalSurvivors += Outcome.Survivors;
		Report.TotalSatisfactionChange += Outcome.SatisfactionChange;
	}
	Report.TotalDeaths=Report.ActualDeathResidentIDs.Num();Report.ActualDeathResidentIDs.Sort();
	for(int32 ID:ExposedIDs){FCityResidentRecord Current;if(Life&&Life->GetResident(ID,Current)&&Current.bAlive)Report.ExposedSurvivorResidentIDs.Add(ID);}
	Report.ExposedSurvivorResidentIDs.Sort();Report.TotalSurvivors=Report.ExposedSurvivorResidentIDs.Num();
	Report.TotalDeathCostTL=static_cast<int64>(Report.TotalDeaths)*500;
	Report.TotalStateAidGranted=static_cast<int64>(Report.TotalSurvivors)*150;
	const auto* Economy=World->GetSubsystem<UEconomyAndStateSubsystem>();
	if(auto* Ledger=World->GetSubsystem<UCasualtyLedgerGenerator>())
		if(!Ledger->RecordDisaster(Report.DisasterEventID,Type,Economy?Economy->CurrentTurn:1,bIsDaytime,BeforeResidents,BeforeBuildings,Report.ActualDeathResidentIDs,Report.ExposedSurvivorResidentIDs,false,BaseSeverity))
			UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_CASUALTY_LEDGER_FAILED event=%s actualDeaths=%d"),*Report.DisasterEventID.ToString(),Report.TotalDeaths);

	// 5) Gameplay Niagara Bağlantısı (Limitler: Earthquake: 64, Flood: 32, Wind: 32)
	if (UCrowdRenderSubsystem* Crowd = World->GetSubsystem<UCrowdRenderSubsystem>(); Crowd && Type!=EDisasterType::Drought && Type!=EDisasterType::Fire && Type!=EDisasterType::Volcano)
	{
		EAftershockCosmeticFx FxType = EAftershockCosmeticFx::EarthquakeDust;
		int32 MaxFxSpawns = 64;

		if (Type == EDisasterType::Flood || Type == EDisasterType::Tsunami)
		{
			FxType = EAftershockCosmeticFx::FloodSpray;
			MaxFxSpawns = 32;
		}
		else if (Type == EDisasterType::Wind)
		{
			FxType = EAftershockCosmeticFx::WindDebris;
			MaxFxSpawns = 32;
		}

		int32 SpawnedFxCount = 0;
		for (const FBuildingInstance& Building : AllBuildings)
		{
			if (Type == EDisasterType::Tsunami && FloodMap.FindRef(Building.GridCoords) <= 0.f) continue;
			if (SpawnedFxCount >= MaxFxSpawns)
			{
				break;
			}

			if (Building.CurrentDamage > 0.0f || Building.DamageState != EDamageState::Intact)
			{
				const FVector FxPos = Terrain->GridToWorld(Building.GridCoords, true) + FVector(0.f, 0.f, 60.f);
				Crowd->SpawnCosmeticFx(FxType, FxPos, 1.5f);
				SpawnedFxCount++;
			}
		}

		if (SpawnedFxCount == 0 && Type != EDisasterType::Tsunami)
		{
			const FVector CenterPos = Terrain->GridToWorld(FIntPoint(Terrain->GridWidth / 2, Terrain->GridHeight / 2), true) + FVector(0.f, 0.f, 60.f);
			Crowd->SpawnCosmeticFx(FxType, CenterPos, 2.0f);
		}
	}

	// 6) Presentation starts before the outcome UI receives the same event.
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_DISASTER_SNAPSHOT revision=%d type=%d base=%.2f effective=%.2f cells=%d duration=%.1f units=DimensionlessGameplaySeverity_NoDepthMeters windDirectionScenarioDefault=%d"),
		LatestVisualSnapshot.Revision, static_cast<int32>(Type), BaseSeverity, LatestVisualSnapshot.EffectiveIntensity,
		LatestVisualSnapshot.LocalSeverity.Num(), LatestVisualSnapshot.DurationSeconds, LatestVisualSnapshot.bWindDirectionIsScenarioDefault);
	OnDisasterVisualSnapshotChanged.Broadcast(LatestVisualSnapshot);
	if (Life && Life->IsCityInitialized()) Life->RefreshCity();
	OnDisasterCompleted.Broadcast(Report);

	return Report;
}

int32 UDisasterSimulationSubsystem::CalculateTheoreticalZeroPreventionDeaths(
	const FBuildingInstance& Building,
	EDisasterType DisasterType,
	float LocalSeverity,
	float CityAwarenessLevel,
	bool bIsDaytime) const
{
	const float BaseResistance = Building.BuildingData
		? Building.BuildingData->GetBaseResistance(DisasterType)
		: 0.3f;

	// Zayıf (Weak) temel: Sel için -0.1, Deprem için 0.0
	const float WeakFoundationBonus = (DisasterType == EDisasterType::Flood) ? -0.1f : 0.0f;
	const float HeightPenalty = static_cast<float>(FMath::Max(1, Building.CurrentFloors) - 1) * 0.03f;
	const float AgeDecay = FMath::Max(0.5f, 1.0f - (static_cast<float>(Building.AgeInYears) / 10.0f) * 0.05f);

	// Sıfır güçlendirme (Retrofit = 0.0)
	const float ZeroPrevResistance = FMath::Clamp(
		(BaseResistance + WeakFoundationBonus - HeightPenalty) * AgeDecay,
		0.05f,
		0.98f
	);

	// Şehir önlemi yok (Drainage = 0)
	const float TheoDamage = FMath::Clamp(LocalSeverity * (1.0f - ZeroPrevResistance), 0.0f, 100.0f);

	float DeathCoeff = 0.0f;
	if (TheoDamage <= 20.0f) DeathCoeff = 0.02f;
	else if (TheoDamage <= 50.0f) DeathCoeff = 0.10f;
	else if (TheoDamage <= 80.0f) DeathCoeff = 0.30f;
	else DeathCoeff = 0.60f;

	const float ClampedAwareness = FMath::Clamp(CityAwarenessLevel, 0.0f, 1.0f);
	const float Panic = (TheoDamage / 100.0f) * (1.0f - ClampedAwareness) * 0.5f;

	const int32 Residents = Building.BuildingData ? Building.BuildingData->Residents : 4;
	const int32 Workers = Building.BuildingData ? Building.BuildingData->Workers : 0;
	const UCityLifeSubsystem* Life=GetWorld() ? GetWorld()->GetSubsystem<UCityLifeSubsystem>() : nullptr;
	const int32 AtRisk = Life && Life->IsCityInitialized() ? Life->GetLivingOccupants(Building.InstanceID,bIsDaytime) : Residents+(bIsDaytime ? Workers : 0);

	return FMath::Clamp(
		FMath::RoundToInt(static_cast<float>(AtRisk) * Panic * DeathCoeff),
		0,
		AtRisk
	);
}

void UDisasterSimulationSubsystem::SpawnChaosDestructionForBuilding(const FBuildingInstance& Building)
{
	if (!Building.BuildingData || Building.BuildingData->MeshFractured.IsNull())
	{
		return;
	}

	UGeometryCollection* FracturedMesh = Building.BuildingData->MeshFractured.LoadSynchronous();
	if (!FracturedMesh)
	{
		return;
	}

	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!World || !Terrain)
	{
		return;
	}

	if (const TObjectPtr<AActor>* Existing=CollapsedRubbleActors.Find(Building.GridCoords); Existing && IsValid(*Existing)) return;

	const FVector SpawnLoc = Terrain->GridToWorld(Building.GridCoords, true);
	FRotator BuildingRotation = FRotator::ZeroRotator;
	if (UBuildingManagerSubsystem* Buildings = World->GetSubsystem<UBuildingManagerSubsystem>())
	{
		if (UInstancedStaticMeshComponent* ISM = Buildings->GetOrCreateISM(Building.BuildingData))
		{
			FTransform IntactTransform;
			if (ISM->GetInstanceTransform(Building.ISMInstanceIndex, IntactTransform, true)) BuildingRotation = IntactTransform.Rotator();
		}
	}
	const float BaseFloors = static_cast<float>(FMath::Max(1, Building.BuildingData->FloorsMin));
	const float ZScale = static_cast<float>(Building.CurrentFloors) / BaseFloors;
	FTransform SpawnTransform(BuildingRotation, SpawnLoc, FVector(1.f, 1.f, ZScale));
	if (Terrain->CellSize>=999.f)
		if (const UBuildingManagerSubsystem* Buildings=World->GetSubsystem<UBuildingManagerSubsystem>())
			SpawnTransform=Buildings->GetBuildingVisualTransform(Building.GridCoords,Building.BuildingData,Building.CurrentFloors,BuildingRotation.Yaw);
	FText SelectionError;
	FracturedMesh=AftershockScaledChaos::ResolveFracturedAsset(Building,Terrain->CellSize,SpawnTransform,SelectionError);
	if (!FracturedMesh)
	{
		UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_CHAOS_ASSET_SELECTION_FAILED building=%d reason=%s canonicalDamageChanged=0"),Building.InstanceID,*SelectionError.ToString());
		return;
	}
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHAOS_ASSET_SELECTED building=%d asset=%s scale=%s canonicalDefinitionUnchanged=1"),Building.InstanceID,*FracturedMesh->GetPathName(),*SpawnTransform.GetScale3D().ToString());
	const TSharedPtr<FGeometryCollection,ESPMode::ThreadSafe> PresentationCollection=FracturedMesh->GetGeometryCollection();
	if (!PresentationCollection) return;
	FIntRect GroundWindow;
	const bool bCanSimulateVisible=GetBuiltChaosGroundWindow(World,GroundWindow)
		&& IsChaosBoundsSupported(*Terrain,GroundWindow,CollectionWorldBounds(*PresentationCollection,SpawnTransform));
	if (!bCanSimulateVisible)
	{
		// Canonical collapse/damage already happened for every resident building.
		// A distant building has no loaded ground collider: retain its actual GC
		// asset and one saved native transform per leaf, using explicit settled art
		// rather than starting invisible bodies that would fall through the world.
		AGeometryCollectionActor* Rubble=World->SpawnActorDeferred<AGeometryCollectionActor>(AGeometryCollectionActor::StaticClass(),SpawnTransform,nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		UGeometryCollectionComponent* Component=Rubble ? Rubble->GetGeometryCollectionComponent() : nullptr;
		if (!Rubble || !Component) { if (Rubble) Rubble->Destroy(); return; }
		Rubble->SetActorEnableCollision(false); Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetSimulatePhysics(false); Component->ObjectType=EObjectStateTypeEnum::Chaos_Object_Kinematic;
		Component->EnableClustering=FracturedMesh->EnableClustering; Component->ClusterGroupIndex=FracturedMesh->ClusterGroupIndex; Component->MaxClusterLevel=FracturedMesh->MaxClusterLevel;
		Component->bAllowRemovalOnSleep=false; Component->bAllowRemovalOnBreak=false;
		Component->SetRestCollection(FracturedMesh,true); Rubble->FinishSpawning(SpawnTransform);
		const TArray<FTransform> Poses=BuildAuthoredOffscreenRubblePose(*PresentationCollection,SpawnTransform,*Terrain,Building.InstanceID);
		Component->SetLocalRestTransforms(Poses,false); Component->SetSimulatePhysics(false); Component->DestroyPhysicsState();
		const bool bDrawRubble=GetAftershockDrawableCellBounds(*Terrain).Contains(Building.GridCoords);
		Component->SetVisibility(bDrawRubble,true); Component->SetHiddenInGame(!bDrawRubble,true); Component->SetCastShadow(true);
		Component->SetComponentTickEnabled(false); Rubble->SetActorTickEnabled(false);
		// Same operation as GeometryCollection's debug-only convenience wrapper,
		// using the public runtime API so the final rubble pose also ships.
		Component->MarkRenderDynamicDataDirty(); Rubble->Tags.Add(TEXT("Aftershock.AuthoredOffscreenRubble"));
		CollapsedRubbleActors.Add(Building.GridCoords,Rubble);
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHAOS_OFFSCREEN_STATIC building=%d coord=(%d,%d) asset=%s nativeTransforms=%d proxyReleased=%d actorTick=%d componentTick=%d poseSource=DeterministicAuthoredSettledRubble physicsReconstructed=0 canonicalDamageChanged=0"),
			Building.InstanceID,Building.GridCoords.X,Building.GridCoords.Y,*FracturedMesh->GetPathName(),Poses.Num(),Component->GetPhysicsProxy()==nullptr,Rubble->IsActorTickEnabled(),Component->IsComponentTickEnabled());
		return;
	}
	// Only visible, collision-supported actors occupy the existing 15-proxy pool.
	ActiveChaosActors.RemoveAll([](const TWeakObjectPtr<AActor>& Actor) { return !Actor.IsValid(); });
	if (ActiveChaosActors.Num()>=15 && ActiveChaosActors.IsValidIndex(0)) FreezeChaosActor(ActiveChaosActors[0]);
	// Set scale/asset/body defaults BEFORE component registration creates the proxy.
	AGeometryCollectionActor* ChaosActor = World->SpawnActorDeferred<AGeometryCollectionActor>(
		AAftershockFoundationGeometryCollectionActor::StaticClass(), SpawnTransform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);

	if (ChaosActor)
	{
		UGeometryCollectionComponent* GCComp = ChaosActor->GetGeometryCollectionComponent();
		if (GCComp)
		{
			// ApplyAssetDefaults copies damage settings but does NOT copy clustering.
			GCComp->EnableClustering = FracturedMesh->EnableClustering;
			GCComp->ClusterGroupIndex = FracturedMesh->ClusterGroupIndex;
			GCComp->MaxClusterLevel = FracturedMesh->MaxClusterLevel;
			GCComp->ObjectType = EObjectStateTypeEnum::Chaos_Object_Dynamic;
			GCComp->bAllowRemovalOnSleep = false;
			GCComp->bAllowRemovalOnBreak = false;
			// Source art contains a buried foundation skirt down to local Z=-150cm.
			// Its fractured convexes begin embedded in soil, so treating them as
			// free debris ejects them downwards. Anchor only ground-embedded base
			// pieces in this INSTANCE before the first solver step. The component's
			// OnCreatePhysicsState hook applies these after OnRegister resets the
			// dynamic collection and before it is copied into the physics proxy.
			TArray<int32> FoundationAnchorIndices;
			if (const TSharedPtr<FGeometryCollection,ESPMode::ThreadSafe> Rest=FracturedMesh->GetGeometryCollection())
			{
				TArray<FTransform> RestTransforms; GeometryCollectionAlgo::GlobalMatrices(Rest->Transform,Rest->Parent,RestTransforms);
				for (int32 Index=0;Index<Rest->SimulationType.Num();++Index)
				{
					if (Rest->SimulationType[Index]!=FGeometryCollection::FST_Rigid || !Rest->Children[Index].IsEmpty() || !RestTransforms.IsValidIndex(Index)) continue;
					const int32 Geometry=Rest->TransformToGeometryIndex[Index];
					if (Geometry==INDEX_NONE || !Rest->BoundingBox.IsValidIndex(Geometry)) continue;
					const FBox LocalBounds=Rest->BoundingBox[Geometry].TransformBy(FTransform(RestTransforms[Index]));
					if (!LocalBounds.IsValid || LocalBounds.Min.Z>=-20.f) continue;
					const FBox WorldBounds=LocalBounds.TransformBy(SpawnTransform);
					const FIntPoint Coord=Terrain->WorldToGrid(WorldBounds.GetCenter());
					if (!Terrain->IsPlayableCoord(Coord) || WorldBounds.Min.Z>=Terrain->GetCell(Coord).Elevation-20.f) continue;
					FoundationAnchorIndices.Add(Index);
				}
			}
			UAftershockFoundationGeometryCollectionComponent* FoundationComponent=CastChecked<UAftershockFoundationGeometryCollectionComponent>(GCComp);
			FoundationComponent->FoundationAnchorIndices=FoundationAnchorIndices;
			// Thin fractured walls/roof convexes can cross the runtime terrain's
			// triangle surface between discrete steps. The GC proxy reads this
			// flag at proxy creation, so enable CCD before SetRestCollection.
			GCComp->BodyInstance.bUseCCD = true;
			GCComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			GCComp->SetCollisionResponseToAllChannels(ECR_Block);
			const bool bDrawRubble=GetAftershockDrawableCellBounds(*Terrain).Contains(Building.GridCoords);
			GCComp->SetVisibility(bDrawRubble, true);
			GCComp->SetHiddenInGame(!bDrawRubble, true);
			GCComp->SetCastShadow(true);
			GCComp->SetEnableGravity(true);
			// SpawnActorDeferred already registers native default components.
			// SetRestCollection can therefore create the proxy immediately: every
			// boundary condition, CCD flag and shape filter MUST precede this call.
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHAOS_BEFORE_SETREST componentRegistered=%d proxyAlreadyExists=%d requestedFoundationAnchors=%d ccd=%d"),GCComp->IsRegistered(),GCComp->GetPhysicsProxy()!=nullptr,FoundationAnchorIndices.Num(),GCComp->BodyInstance.bUseCCD);
			GCComp->SetRestCollection(FracturedMesh,true);
			ChaosActor->FinishSpawning(SpawnTransform);
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHAOS_FOUNDATION_ANCHORS actor=%s anchoredLeaves=%d source=BuriedRestGeometryAndCanonicalGround instanceOnly=1 poseSnapping=0 appliedBeforePhysicsProxyCreation=1"),*ChaosActor->GetName(),FoundationAnchorIndices.Num());
			GCComp->SetSimulatePhysics(true);
			LogChaosFoundationParticlePoses(*GCComp,TEXT("PostSpawnBeforeFirstSolverStep"));
			// Inspect cooked per-leaf collision before the first solver step. A
			// visible leaf can have no cooked implicit, which CCD cannot repair.
			if (const TSharedPtr<FGeometryCollection,ESPMode::ThreadSafe> RestCollection=FracturedMesh->GetGeometryCollection();
				RestCollection&&FParse::Param(FCommandLine::Get(),TEXT("AftershockChaosAudit")))
			{
				const TManagedArray<Chaos::FImplicitObjectPtr>* Implicits=RestCollection->FindAttribute<Chaos::FImplicitObjectPtr>(TEXT("Implicits"),FGeometryCollection::TransformGroup);
				const TArray<FTransform3f>& RestTransforms=GCComp->GetComponentSpaceTransforms3f();
				int32 InitialRigid=0,MissingImplicit=0,InitiallyBelowGround=0,InitiallyPenetratingGround=0;
				for (int32 Index=0;Index<RestCollection->SimulationType.Num();++Index)
				{
					if (RestCollection->SimulationType[Index]!=FGeometryCollection::FST_Rigid || !RestTransforms.IsValidIndex(Index)) continue;
					++InitialRigid;
					const bool HasImplicit=Implicits && Implicits->IsValidIndex(Index) && (*Implicits)[Index];
					MissingImplicit+=HasImplicit ? 0 : 1;
					const int32 GeometryIndex=RestCollection->TransformToGeometryIndex[Index];
					FBox WorldBounds(ForceInit);
					if (GeometryIndex!=INDEX_NONE && RestCollection->BoundingBox.IsValidIndex(GeometryIndex))
						WorldBounds=RestCollection->BoundingBox[GeometryIndex].TransformBy(FTransform(RestTransforms[Index])*GCComp->GetComponentTransform());
					const FIntPoint Coord=Terrain->WorldToGrid(WorldBounds.IsValid ? WorldBounds.GetCenter() : SpawnLoc);
					const float Ground=Terrain->IsPlayableCoord(Coord) ? Terrain->GetCell(Coord).Elevation : SpawnLoc.Z;
					InitiallyBelowGround+=WorldBounds.IsValid && WorldBounds.Max.Z<Ground-10.f ? 1 : 0;
					InitiallyPenetratingGround+=WorldBounds.IsValid && WorldBounds.Min.Z<Ground-20.f ? 1 : 0;
					const FVector ImplicitExtent=HasImplicit && (*Implicits)[Index]->HasBoundingBox() ? FVector((*Implicits)[Index]->BoundingBox().Extents()) : FVector::ZeroVector;
					UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHAOS_INITIAL_LEAF actor=%s leaf=%d cookedImplicit=%d implicitType=%d implicitSize=(%.2f,%.2f,%.2f) worldGeometryMinZ=%.2f worldGeometryMaxZ=%.2f sampledGroundZ=%.2f"),
						*ChaosActor->GetName(),Index,HasImplicit,HasImplicit ? static_cast<int32>((*Implicits)[Index]->GetType()) : -1,ImplicitExtent.X,ImplicitExtent.Y,ImplicitExtent.Z,
						WorldBounds.IsValid ? WorldBounds.Min.Z : 0.f,WorldBounds.IsValid ? WorldBounds.Max.Z : 0.f,Ground);
				}
				UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHAOS_INITIAL_COLLISION actor=%s rigid=%d missingCookedImplicits=%d initiallyWholeBelowGround=%d initiallyGroundPenetrating=%d"),
					*ChaosActor->GetName(),InitialRigid,MissingImplicit,InitiallyBelowGround,InitiallyPenetratingGround);
			}
			// bVelChange maps directly to cm/s. The previous 250000 assigned 2.5km/s
			// and threw every chunk out of the city. This is a cosmetic debris kick.
			World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(GCComp, [GCComp, SpawnLoc]()
			{
				GCComp->AddRadialImpulse(SpawnLoc + FVector(0.f, 0.f, 80.f), 1600.f, 160.f, RIF_Linear, true);
			}));
		}
		else { ChaosActor->Destroy(); return; }

		ActiveChaosActors.Add(ChaosActor);
		CollapsedRubbleActors.Add(Building.GridCoords, ChaosActor);
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CHAOS_SPAWN building=%d asset=%s active=%d cluster=%d proxy=%d ccd=%d visualKickCmPerSec=160"),
			Building.InstanceID, *FracturedMesh->GetPathName(), ActiveChaosActors.Num(), GCComp->EnableClustering, GCComp->GetPhysicsProxy() != nullptr,GCComp->BodyInstance.bUseCCD);

		// 4 Saniye Kuralı: 4.0 sn sonra fiziği kapatıp enkazı dondur
		FTimerHandle FreezeTimerHandle;
		TWeakObjectPtr<AActor> WeakActor = ChaosActor;
		World->GetTimerManager().SetTimer(
			FreezeTimerHandle,
			FTimerDelegate::CreateUObject(this, &UDisasterSimulationSubsystem::FreezeChaosActor, WeakActor),
			4.0f,
			false
		);
	}
}

void UDisasterSimulationSubsystem::FreezeChaosActor(TWeakObjectPtr<AActor> ChaosActorPtr)
{
	if (ChaosActorPtr.IsValid())
	{
		if (AGeometryCollectionActor* GCActor = Cast<AGeometryCollectionActor>(ChaosActorPtr.Get()))
		{
			if (UGeometryCollectionComponent* GCComp = GCActor->GetGeometryCollectionComponent())
			{
				LogChaosFoundationParticlePoses(*GCComp,TEXT("PreFreeze"));
				const TArray<FTransform3f>& Transforms = GCComp->GetComponentSpaceTransforms3f();
				const UGeometryCollection* Rest = GCComp->GetRestCollection();
				const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Collection = Rest ? Rest->GetGeometryCollection() : nullptr;
				FBox LeafBounds(ForceInit);
				int32 Leaves = 0, NearSourceLeaves = 0;
				int32 TerrainCheckedLeaves=0,BelowTerrainLeaves=0,AirborneLeaves=0,OffPlayableLeaves=0;
				int32 AnchoredFoundationLeaves=0,KinematicFoundationLeaves=0;
				int32 AnchoredBelowTerrainLeaves=0,UnanchoredBelowTerrainLeaves=0,EscapedFoundationLeaves=0;
				const FGeometryDynamicCollection* Dynamic=GCComp->GetDynamicCollection();
				TArray<FTransform> RestGlobalTransforms;
				if (Collection) GeometryCollectionAlgo::GlobalMatrices(Collection->Transform,Collection->Parent,RestGlobalTransforms);
				float MinimumBottomClearance=MAX_flt;
				const UTerrainGridSubsystem* Terrain=GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
				for (int32 Index = 0; Collection && Index < Transforms.Num() && Index < Collection->SimulationType.Num(); ++Index)
				{
					if (Collection->SimulationType[Index] != FGeometryCollection::FST_Rigid) continue;
					const FVector Position = GCComp->GetComponentTransform().TransformPosition(FVector(Transforms[Index].GetTranslation()));
					LeafBounds += Position;
					++Leaves;
					bool bAnchoredFoundation=false;
					if (Dynamic)
					{
						const Chaos::Facades::FCollectionAnchoringFacade Anchoring(*Dynamic);
						bAnchoredFoundation=Anchoring.IsAnchored(Index);
						AnchoredFoundationLeaves+=bAnchoredFoundation ? 1 : 0;
						KinematicFoundationLeaves+=bAnchoredFoundation && Dynamic->DynamicState[Index]==static_cast<uint8>(EObjectStateTypeEnum::Chaos_Object_Kinematic) ? 1 : 0;
					}
					NearSourceLeaves += FVector::Dist2D(Position, GCActor->GetActorLocation()) < 2000.f ? 1 : 0;
					const int32 GeometryIndex=Collection->TransformToGeometryIndex.IsValidIndex(Index) ? Collection->TransformToGeometryIndex[Index] : INDEX_NONE;
					if (!Terrain || GeometryIndex==INDEX_NONE || !Collection->BoundingBox.IsValidIndex(GeometryIndex)) continue;
					const FBox ChunkBounds=Collection->BoundingBox[GeometryIndex].TransformBy(FTransform(Transforms[Index])*GCComp->GetComponentTransform());
					if (!ChunkBounds.IsValid) continue;
					if (bAnchoredFoundation && RestGlobalTransforms.IsValidIndex(Index))
					{
						const FBox ExpectedBounds=Collection->BoundingBox[GeometryIndex].TransformBy(RestGlobalTransforms[Index]*GCComp->GetComponentTransform());
						// An originally buried footing is legitimate. Moving it away
						// from its authored rest position is not: never hide escaped
						// foundations by excluding every anchored piece from the audit.
						EscapedFoundationLeaves+=ExpectedBounds.IsValid && FVector::Dist(ChunkBounds.GetCenter(),ExpectedBounds.GetCenter())>3.f ? 1 : 0;
					}
					const FVector Centre=ChunkBounds.GetCenter();
					const FVector Samples[]={Centre,FVector(ChunkBounds.Min.X,ChunkBounds.Min.Y,Centre.Z),FVector(ChunkBounds.Max.X,ChunkBounds.Min.Y,Centre.Z),
						FVector(ChunkBounds.Min.X,ChunkBounds.Max.Y,Centre.Z),FVector(ChunkBounds.Max.X,ChunkBounds.Max.Y,Centre.Z)};
					float MinGround=MAX_flt,MaxGround=-MAX_flt;
					for (const FVector& Sample:Samples)
					{
						const FIntPoint Coord=Terrain->WorldToGrid(Sample);
						if (!Terrain->IsPlayableCoord(Coord)) continue;
						const float Ground=Terrain->GetCell(Coord).Elevation;
						MinGround=FMath::Min(MinGround,Ground); MaxGround=FMath::Max(MaxGround,Ground);
					}
					if (MinGround==MAX_flt) { ++OffPlayableLeaves; continue; }
					++TerrainCheckedLeaves;
					// Whole transformed chunk bounds and five nearby canonical samples
					// make this a conservative diagnostic, not an exact contact test.
					const bool bWholeBelowTerrain=ChunkBounds.Max.Z<MinGround-10.f;
					BelowTerrainLeaves += bWholeBelowTerrain ? 1 : 0;
					AnchoredBelowTerrainLeaves+=bWholeBelowTerrain && bAnchoredFoundation ? 1 : 0;
					UnanchoredBelowTerrainLeaves+=bWholeBelowTerrain && !bAnchoredFoundation ? 1 : 0;
					AirborneLeaves += ChunkBounds.Min.Z>MaxGround+100.f ? 1 : 0;
					MinimumBottomClearance=FMath::Min(MinimumBottomClearance,static_cast<float>(ChunkBounds.Min.Z)-MaxGround);
				}
				GCComp->SetSimulatePhysics(false);
				GCComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				// UE's default SetSimulatePhysics(false) can keep its solver proxy alive.
				// Explicit teardown retains the dynamic collection's last render pose.
				GCComp->DestroyPhysicsState();
				GCComp->SetComponentTickEnabled(false);
				GCComp->MarkRenderDynamicDataDirty();
				UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CHAOS_FROZEN actor=%s leafTransforms=%d nearSourceLeaves=%d leafMinZ=%.1f leafMaxZ=%.1f proxyReleased=%d ccd=%d terrainCheckedLeaves=%d belowTerrainLeaves=%d airborneLeaves=%d offPlayableLeaves=%d minChunkBottomClearanceCm=%.1f contactScope=ApproximateWorldChunkBoundsVsFiveCanonicalTerrainSamples freezeSeconds=4 anchoredFoundationLeaves=%d kinematicFoundationLeaves=%d anchoredBelowTerrainLeaves=%d unanchoredBelowTerrainLeaves=%d escapedFoundationLeaves=%d foundationRestPoseToleranceCm=3"),
					*GCActor->GetName(), Leaves, NearSourceLeaves, LeafBounds.IsValid ? LeafBounds.Min.Z : 0.f,
					LeafBounds.IsValid ? LeafBounds.Max.Z : 0.f, GCComp->GetPhysicsProxy() == nullptr,GCComp->BodyInstance.bUseCCD,TerrainCheckedLeaves,
					BelowTerrainLeaves,AirborneLeaves,OffPlayableLeaves,MinimumBottomClearance==MAX_flt ? 0.f : MinimumBottomClearance,AnchoredFoundationLeaves,KinematicFoundationLeaves,
					AnchoredBelowTerrainLeaves,UnanchoredBelowTerrainLeaves,EscapedFoundationLeaves);
			}
			GCActor->SetActorTickEnabled(false);
		}
		ActiveChaosActors.Remove(ChaosActorPtr);
	}
}

bool UDisasterSimulationSubsystem::ClearCollapsedRubble(FIntPoint GridCoords, int32& OutCostTL)
{
	OutCostTL = 3000; // BUILDING_STATS.md Bölüm 5: Enkaz kaldırma 3.000 TL

	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	UBuildingManagerSubsystem* BuildingMgr = World ? World->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
	FBuildingInstance Building;
	const auto* Economy=World?World->GetSubsystem<UEconomyAndStateSubsystem>():nullptr;
	if(!Terrain||!BuildingMgr||!Economy||!BuildingMgr->GetBuildingAtCoords(GridCoords,Building)||Building.DamageState!=EDamageState::Collapsed
		||!BuildingMgr->BeginMemorial(Building.InstanceID,Economy->CurrentTurn)){OutCostTL=0;return false;}

	// Enkaz aktörünü dünyadan kaldır
	if (TObjectPtr<AActor>* FoundActor = CollapsedRubbleActors.Find(GridCoords))
	{
		if (IsValid(*FoundActor))
		{
			(*FoundActor)->Destroy();
		}
		CollapsedRubbleActors.Remove(GridCoords);
	}

	// The same canonical parcel/building ID remains an uninhabitable memorial
	// for three real turns. Only AdvanceMemorialTurn releases its occupancy.

	return true;
}

bool UDisasterSimulationSubsystem::InvestInRiverExpansion(FIntPoint Coords, int32& OutCostTL)
{
	OutCostTL = 2500;

	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain)
	{
		return false;
	}

	return Terrain->UpgradeRiverPrevention(Coords, 1.0f, 0.0f);
}

bool UDisasterSimulationSubsystem::InvestInWaterDamping(FIntPoint Coords, int32& OutCostTL)
{
	OutCostTL = 2000;

	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain)
	{
		return false;
	}

	return Terrain->UpgradeRiverPrevention(Coords, 0.0f, 0.25f);
}

bool UDisasterSimulationSubsystem::InvestInStreetDrainage(FIntPoint Coords, int32 Radius, int32& OutCostTL)
{
	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain)
	{
		OutCostTL = 0;
		return false;
	}

	int32 UpgradedCells = 0;
	const int32 ClampedRadius = FMath::Clamp(Radius, 0, 5);

	for (int32 Dy = -ClampedRadius; Dy <= ClampedRadius; ++Dy)
	{
		for (int32 Dx = -ClampedRadius; Dx <= ClampedRadius; ++Dx)
		{
			const FIntPoint TargetCoord(Coords.X + Dx, Coords.Y + Dy);
			if (Terrain->IsValidCoord(TargetCoord))
			{
				const float Dist = FMath::Sqrt(static_cast<float>(Dx * Dx + Dy * Dy));
				if (Dist <= static_cast<float>(ClampedRadius))
				{
					if (Terrain->UpgradeCellDrainage(TargetCoord, 0.25f))
					{
						UpgradedCells++;
					}
				}
			}
		}
	}

	OutCostTL = UpgradedCells * 400; // Hücre başı 400 TL drenaj temizleme maliyeti
	return UpgradedCells > 0;
}
