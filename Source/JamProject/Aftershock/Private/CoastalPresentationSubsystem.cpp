#include "Aftershock/Public/CoastalPresentationSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "AftershockTerrainSurface.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

AAftershockCoastalPresentationActor::AAftershockCoastalPresentationActor()
{
	PrimaryActorTick.bCanEverTick = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("CoastalRoot")));
	SeaSurface = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CanonicalOceanSurface"));
	SeaSurface->SetupAttachment(GetRootComponent());
	SeaSurface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SeaSurface->SetCanEverAffectNavigation(false); SeaSurface->SetCastShadow(false);
	ConcreteParts = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("DamConcreteParts"));
	SteelParts = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("DamGateParts"));
	ReservoirParts = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("DamReservoirAndSpillway"));
	for (UInstancedStaticMeshComponent* Batch : {ConcreteParts.Get(), SteelParts.Get(), ReservoirParts.Get()})
	{
		Batch->SetupAttachment(GetRootComponent()); Batch->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Batch->SetCanEverAffectNavigation(false); Batch->SetMobility(EComponentMobility::Movable);
		Batch->SetCullDistances(0, 65000);
	}
	ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	ConstructorHelpers::FObjectFinder<UStaticMesh> Plane(TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (Cube.Succeeded()) { ConcreteParts->SetStaticMesh(Cube.Object); SteelParts->SetStaticMesh(Cube.Object); }
	if (Plane.Succeeded()) ReservoirParts->SetStaticMesh(Plane.Object);
	ConstructorHelpers::FObjectFinder<UMaterialInterface> Sea(TEXT("/Game/Environment/Materials/Presentation/M_CoastalSea.M_CoastalSea"));
	ConstructorHelpers::FObjectFinder<UMaterialInterface> Concrete(TEXT("/Game/Environment/Materials/Presentation/M_DamConcrete.M_DamConcrete"));
	ConstructorHelpers::FObjectFinder<UMaterialInterface> Steel(TEXT("/Game/Environment/Materials/Presentation/M_DamSteel.M_DamSteel"));
	if (Sea.Succeeded()) { SeaSurface->SetMaterial(0, Sea.Object); ReservoirParts->SetMaterial(0, Sea.Object); }
	if (Concrete.Succeeded()) ConcreteParts->SetMaterial(0, Concrete.Object);
	if (Steel.Succeeded()) SteelParts->SetMaterial(0, Steel.Object);
}

void AAftershockCoastalPresentationActor::Rebuild(const UTerrainGridSubsystem& Terrain)
{
	ConcreteParts->ClearInstances(); SteelParts->ClearInstances(); ReservoirParts->ClearInstances();
	OceanCells = 0; InstalledDams = 0; OvertoppedDams = 0;
	const FIntRect Visible=GetAftershockDrawableCellBounds(Terrain);
	const auto* Simulation=GetWorld()?GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>():nullptr;
	const bool Physical=Simulation&&Simulation->IsHydraulicModel2Active();
	if(!Physical)BuildAftershockCoastalSeaSurface(this, &Terrain, SeaSurface);
	else {SeaSurface->SetStaticMesh(nullptr);SeaSurface->SetVisibility(false);}
	for (const FGridCell& Cell : Terrain.Cells)
	{
		OceanCells += Cell.bIsOcean ? 1 : 0;
		if (!Cell.bHasDam || !Cell.bIsRiverbed || !Cell.bIsPlayable) continue;
		++InstalledDams;
		OvertoppedDams += Cell.DamLastOvertoppingUnits > 0.f ? 1 : 0;
		if(!Visible.Contains(Cell.Coords))continue;
		FVector Upstream = FVector::ZeroVector, Downstream = FVector::ZeroVector;
		int32 UpCount = 0, DownCount = 0;
		for (int32 DY = -2; DY <= 2; ++DY) for (int32 DX = -2; DX <= 2; ++DX)
		{
			const FIntPoint Coord = Cell.Coords + FIntPoint(DX,DY);
			if (!Terrain.IsValidCoord(Coord) || !Terrain.GetCell(Coord).bIsRiverbed) continue;
			if (DY < 0) { Upstream += Terrain.GridToWorld(Coord); ++UpCount; }
			if (DY > 0) { Downstream += Terrain.GridToWorld(Coord); ++DownCount; }
		}
		FVector Tangent = UpCount > 0 && DownCount > 0 ? Downstream / DownCount - Upstream / UpCount : FVector(0,1,0);
		if(Physical&&Cell.DamOutflowStep!=FIntPoint::ZeroValue)Tangent=FVector(Cell.DamOutflowStep.X,Cell.DamOutflowStep.Y,0);
		Tangent.Z = 0.f; Tangent.Normalize();
		if (Tangent.IsNearlyZero()) Tangent = FVector(0,1,0);
		const FVector Across(Tangent.Y, -Tangent.X, 0);
		const FQuat Rotation = Across.Rotation().Quaternion();
		int32 Left = Cell.Coords.X, Right = Cell.Coords.X;
		while (Terrain.IsValidCoord(FIntPoint(Left-1,Cell.Coords.Y)) && Terrain.GetCell(FIntPoint(Left-1,Cell.Coords.Y)).bIsRiverbed) --Left;
		while (Terrain.IsValidCoord(FIntPoint(Right+1,Cell.Coords.Y)) && Terrain.GetCell(FIntPoint(Right+1,Cell.Coords.Y)).bIsRiverbed) ++Right;
		const float Width = (Right-Left+1)*Terrain.CellSize + 110.f;
		FVector Center = Terrain.GridToWorld(Cell.Coords);
		Center.X = Terrain.GridOrigin.X + (Left + Right + 1) * Terrain.CellSize * .5f;
		float WallWidth=Width;
		if(Physical)
		{
			const FIntPoint AcrossStep(Cell.DamOutflowStep.Y,-Cell.DamOutflowStep.X);
			FIntPoint A=Cell.Coords,B=Cell.Coords;
			for(int32 I=0;I<64;++I){FGridCell N;if(!Terrain.TryGetCell(A-AcrossStep,N)||!N.bIsRiverbed||N.bIsOcean)break;A-=AcrossStep;}
			for(int32 I=0;I<64;++I){FGridCell N;if(!Terrain.TryGetCell(B+AcrossStep,N)||!N.bIsRiverbed||N.bIsOcean)break;B+=AcrossStep;}
			Center=(Terrain.GridToWorld(A)+Terrain.GridToWorld(B))*.5+Tangent*(Terrain.CellSize*.5);
			Center.Z=Cell.Elevation;WallWidth=(FVector2D::Distance(FVector2D(A),FVector2D(B))+1.)*Terrain.CellSize;
		}
		const auto ConcreteBox = [&](FVector Offset, FVector Size)
		{ ConcreteParts->AddInstance(FTransform(Rotation, Center + Offset, Size / 100.f), true); };
		const float CrestCm=Physical?Cell.DamCrestHeightM*100.f:210.f;
		const float SkirtCm=Physical?Cell.HydraulicExcavationCm:0.f;
		ConcreteBox(FVector(0,0,(CrestCm-SkirtCm)*.5f), FVector(WallWidth,100,CrestCm+SkirtCm));
		ConcreteBox(FVector(0,0,CrestCm+15.f), FVector(WallWidth+65,140,30));
		for (float Side : {-1.f,1.f}) ConcreteBox(Across * (Width*.5f) + FVector(0,0,135), FVector(125,220,270));
		for (int32 Gate = -1; Gate <= 1; ++Gate)
		{
			const FVector GateCenter = Center + Across * (Gate * 95.f) + Tangent * 53.f + FVector(0,0,95);
			SteelParts->AddInstance(FTransform(Rotation, GateCenter, FVector(.68f,.08f,1.35f)), true);
		}
		const float Fill = Cell.DamStorageCapacityUnits > 0.f ? FMath::Clamp(Cell.DamStoredWaterUnits / Cell.DamStorageCapacityUnits, 0.f, 1.f) : 0.f;
		if (!Physical && Fill > .01f)
		{
			TArray<FIntPoint> Reach; Reach.Add(Cell.Coords);
			TSet<FIntPoint> Seen; Seen.Add(Cell.Coords);
			for(int32 Read=0;Read<Reach.Num();++Read)
			{
				for(FIntPoint Offset:{FIntPoint(0,-1),FIntPoint(-1,0),FIntPoint(1,0)})
				{
					const FIntPoint Coord=Reach[Read]+Offset;
					if(!Visible.Contains(Coord)||!Terrain.IsPlayableCoord(Coord) || Seen.Contains(Coord) || Coord.Y>Cell.Coords.Y || Cell.Coords.Y-Coord.Y>3 || FMath::Abs(Coord.X-Cell.Coords.X)>4) continue;
					const FGridCell WaterCell=Terrain.GetCell(Coord);
					if(!WaterCell.bIsRiverbed) continue;
					Seen.Add(Coord); Reach.Add(Coord);
					if(Coord.Y==Cell.Coords.Y) continue;
					FVector P=Terrain.GridToWorld(Coord);
					P.Z=FMath::Max(P.Z+13.f,Center.Z+Fill*105.f);
					ReservoirParts->AddInstance(FTransform(FQuat::Identity,P,FVector(Terrain.CellSize/100.f,Terrain.CellSize/100.f,1)),true);
				}
			}
		}
		if (!Physical && Cell.DamLastOvertoppingUnits > 0.f)
		{
			const FQuat SpillRotation = FRotationMatrix::MakeFromXZ(Across, Tangent).ToQuat();
			ReservoirParts->AddInstance(FTransform(SpillRotation,Center+Tangent*65.f+FVector(0,0,125),FVector(2.8f,2.1f,1)),true);
		}
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_DAM_VISUAL coord=(%d,%d) storedUnits=%.2f capacityUnits=%.2f fill=%.3f lastRetainedUnits=%.2f lastOvertoppingUnits=%.2f displayHeightIsArtOnly=1"),
			Cell.Coords.X,Cell.Coords.Y,Cell.DamStoredWaterUnits,Cell.DamStorageCapacityUnits,Fill,Cell.DamLastRetainedUnits,Cell.DamLastOvertoppingUnits);
	}
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_COASTAL_PRESENTATION %s"),*DescribeVisuals());
}

FString AAftershockCoastalPresentationActor::DescribeVisuals() const
{
	return FString::Printf(TEXT("oceanCells=%d installedDams=%d overtoppedDams=%d concreteInstances=%d gateInstances=%d reservoirPieces=%d perCellActors=0 actorTick=0"),
		OceanCells,InstalledDams,OvertoppedDams,ConcreteParts->GetInstanceCount(),SteelParts->GetInstanceCount(),ReservoirParts->GetInstanceCount());
}

bool UCoastalPresentationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return Super::ShouldCreateSubsystem(Outer) && World && (World->WorldType==EWorldType::Game || World->WorldType==EWorldType::PIE);
}

void UCoastalPresentationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection); Collection.InitializeDependency<UTerrainGridSubsystem>(); Collection.InitializeDependency<UDisasterSimulationSubsystem>();
	if (UDisasterSimulationSubsystem* D = GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>()) D->OnDamInfrastructureChanged.AddUniqueDynamic(this,&UCoastalPresentationSubsystem::HandleDamChanged);
}

void UCoastalPresentationSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	InWorld.GetTimerManager().SetTimer(RefreshTimer,this,&UCoastalPresentationSubsystem::Refresh,1.f,true,.2f);
}

void UCoastalPresentationSubsystem::HandleDamChanged(FIntPoint Coord) { bHasSignature=false; Refresh(); }

void UCoastalPresentationSubsystem::Refresh()
{
	UWorld* World=GetWorld(); const UTerrainGridSubsystem* T=World?World->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	if(!T) return;
	uint32 Signature=HashCombine(GetTypeHash(T->TerrainSeed),GetTypeHash(T->CoastalSeaLevelCm));
	const auto* Simulation=World->GetSubsystem<UDisasterSimulationSubsystem>();
	Signature=HashCombine(Signature,GetTypeHash(Simulation&&Simulation->IsHydraulicModel2Active()));
	Signature=HashCombine(Signature,GetTypeHash(T->GridOrigin));
	Signature=HashCombine(Signature,GetTypeHash(T->CellSize));
	const FIntRect Visible=GetAftershockDrawableCellBounds(*T);
	Signature=HashCombine(Signature,GetTypeHash(Visible.Min));Signature=HashCombine(Signature,GetTypeHash(Visible.Max));
	for(const FGridCell& C:T->Cells)
	{
		if (!Visible.Contains(C.Coords)) continue;
		Signature=HashCombine(Signature,GetTypeHash(C.Coords));
		Signature=HashCombine(Signature,GetTypeHash(C.Elevation));
		Signature=HashCombine(Signature,GetTypeHash(C.bIsRiverbed));
		Signature=HashCombine(Signature,GetTypeHash(C.bIsPlayable));
		Signature=HashCombine(Signature,GetTypeHash(C.bIsOcean)); Signature=HashCombine(Signature,GetTypeHash(C.bHasDam));
		if(C.bHasDam) { Signature=HashCombine(Signature,GetTypeHash(C.DamStoredWaterUnits)); Signature=HashCombine(Signature,GetTypeHash(C.DamStorageCapacityUnits)); Signature=HashCombine(Signature,GetTypeHash(C.DamLastOvertoppingUnits)); Signature=HashCombine(Signature,GetTypeHash(C.DamOutflowStep));Signature=HashCombine(Signature,GetTypeHash(C.DamCrestHeightM)); }
	}
	if(bHasSignature && Signature==LastSignature) return;
	if(!IsValid(PresentationActor)) { FActorSpawnParameters P; P.ObjectFlags|=RF_Transient; PresentationActor=World->SpawnActor<AAftershockCoastalPresentationActor>(FVector::ZeroVector,FRotator::ZeroRotator,P); }
	if(!PresentationActor) return;
	PresentationActor->Rebuild(*T); LastSignature=Signature; bHasSignature=true;
}

FString UCoastalPresentationSubsystem::DescribeCurrentVisuals() const { return IsValid(PresentationActor)?PresentationActor->DescribeVisuals():TEXT("coastalPresentationPending"); }

void UCoastalPresentationSubsystem::Deinitialize()
{
	if(UWorld* W=GetWorld()) { W->GetTimerManager().ClearTimer(RefreshTimer); if(UDisasterSimulationSubsystem* D=W->GetSubsystem<UDisasterSimulationSubsystem>()) D->OnDamInfrastructureChanged.RemoveDynamic(this,&UCoastalPresentationSubsystem::HandleDamChanged); }
	if(IsValid(PresentationActor)) PresentationActor->Destroy(); PresentationActor=nullptr; Super::Deinitialize();
}
