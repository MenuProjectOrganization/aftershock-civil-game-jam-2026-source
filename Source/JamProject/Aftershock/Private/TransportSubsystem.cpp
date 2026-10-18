#include "Aftershock/Public/TransportSubsystem.h"
#include "AftershockRoadCpuScope.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/DisasterPresentationSubsystem.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "AftershockTerrainSurface.h"
#include "AftershockPresentationBudget.h"
#include "CoreGlobals.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Materials/MaterialInterface.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr int32 MaxCars=80,MaxBuses=8,MaxMunicipalFleet=24,MaxMunicipalUnits=1024,MaxRenderedCars=MaxCars+MaxMunicipalFleet,MaxWheels=MaxRenderedCars*4+MaxBuses*6;
	constexpr float TransportStep=.08f;
	const FIntPoint Adjacent[]={FIntPoint(1,0),FIntPoint(0,1),FIntPoint(-1,0),FIntPoint(0,-1)};
	bool ActiveTrip(EAftershockPassengerState State)
	{ return State==EAftershockPassengerState::WalkingToPickup || State==EAftershockPassengerState::Waiting || State==EAftershockPassengerState::Riding; }
	bool IsMunicipalVehicle(const FAftershockRoadVehicle& Vehicle) { return Vehicle.VehicleID<=-1000; }
	EAftershockMunicipalService MunicipalRole(EBuildingType Type)
	{
		switch(Type)
		{
		case EBuildingType::FireStation:return EAftershockMunicipalService::FireResponse;
		case EBuildingType::Hospital:return EAftershockMunicipalService::Ambulance;
		case EBuildingType::PoliceStation:return EAftershockMunicipalService::PolicePatrol;
		case EBuildingType::Courthouse:return EAftershockMunicipalService::CourtTransport;
		case EBuildingType::TaxOffice:case EBuildingType::WaterPlant:case EBuildingType::PowerPlant:
		case EBuildingType::Generator:case EBuildingType::Substation:return EAftershockMunicipalService::MunicipalUtility;
		default:return EAftershockMunicipalService::None;
		}
	}
	float CruiseSpeed(const FAftershockRoadVehicle& Vehicle)
	{
		uint32 Seed=static_cast<uint32>(Vehicle.bBus?Vehicle.VehicleID:Vehicle.ResidentOwnerID)*0x9e3779b9u;
		Seed^=Seed>>16;Seed*=0x7feb352du;Seed^=Seed>>15;
		const float Unit=(Seed&65535u)/65535.f;
		return Vehicle.bBus?400.f+40.f*Unit:540.f+110.f*Unit;
	}

    struct FSidewalkPassengerPlaces
    {
        TArray<FVector,TInlineAllocator<12>> Waiting;
        TArray<FVector,TInlineAllocator<2>> Exits;
    };
    // Same nine rectangles and cardinal bit order as conforming-road rendering.
    // This is a read-only shape predicate, never a road/terrain authority.
    bool IsConformingSidewalkDisc(double X,double Y,double Radius,int32 Mask,double Cell)
    {
        if(!FMath::IsFinite(X)||!FMath::IsFinite(Y)||!FMath::IsFinite(Radius)||Radius<=0.||Cell<=0.||
            X-Radius<0.||Y-Radius<0.||X+Radius>Cell||Y+Radius>Cell)return false;
        const double B[]={0.,.12,.88,1.};
        for(int32 PY=0;PY<3;++PY)for(int32 PX=0;PX<3;++PX)
        {
            const bool Asphalt=(PX==1&&PY==1)||(PY==1&&((PX==2&&(Mask&1))||(PX==0&&(Mask&4))))||
                (PX==1&&((PY==2&&(Mask&2))||(PY==0&&(Mask&8))));
            if(!Asphalt)continue;
            const double DX=X-FMath::Clamp(X,B[PX]*Cell,B[PX+1]*Cell);
            const double DY=Y-FMath::Clamp(Y,B[PY]*Cell,B[PY+1]*Cell);
            if(DX*DX+DY*DY<Radius*Radius-1.e-6)return false;
        }
        return true;
    }
    bool BuildConformingSidewalkPlaces(const UTerrainGridSubsystem& T,const URoadNetworkSubsystem& Roads,
        FIntPoint Road,FIntPoint Building,FSidewalkPassengerPlaces& Out)
    {
        Out.Waiting.Reset();Out.Exits.Reset();FGridCell Cell;
        if(!FMath::IsFinite(T.CellSize)||T.CellSize<900.f||!T.TryGetCell(Road,Cell)||!Cell.bIsPlayable||
            !Cell.bHasRoad||Cell.bIsRiverbed||Cell.bIsOcean||Cell.BridgeID!=INDEX_NONE||
            Cell.OccupyingBuildingID!=INDEX_NONE||!Roads.IsPublicWalkingCell(Road))return false;
        int32 Mask=0;
        for(int32 D=0;D<4;++D)
        {
            FGridCell N;if(!T.TryGetCell(Road+Adjacent[D],N)||!N.bIsPlayable||!N.bHasRoad||N.OccupyingBuildingID!=INDEX_NONE||
                (N.bIsRiverbed&&N.BridgeID==INDEX_NONE))continue;
            if(N.BridgeID!=INDEX_NONE&&!N.bBridgeEndpoint&&((N.BridgeAxis==1&&Adjacent[D].Y!=0)||(N.BridgeAxis==2&&Adjacent[D].X!=0)))continue;
            Mask|=1<<D;
        }
        FVector Along=(Mask&5)?FVector::ForwardVector:FVector::RightVector;
        FVector Across(-Along.Y,Along.X,0.);
        const FVector Centre=T.GridToWorld(Road),BuildingCentre=T.GridToWorld(Building);
        if(FVector::DotProduct(BuildingCentre-Centre,Across)<0.)Across*=-1.f;
        // 32cm is the existing movement-clearance disc, not a newly invented NPC.
        constexpr double BodyRadius=32.,Margin=2.,PersonalSpacing=80.;
        const double SafeRadius=BodyRadius+Margin,HalfRow=T.CellSize*.06-SafeRadius;
        if(HalfRow<=0.)return false;
        const double RowDistance=HalfRow*2.;
        const double Stagger=RowDistance>=PersonalSpacing?0.:FMath::CeilToDouble(FMath::Sqrt(FMath::Square(PersonalSpacing)-FMath::Square(RowDistance)))+1.;
        const double Step=FMath::Max(PersonalSpacing,Stagger*2.);
        const double Limit=T.CellSize*.5-SafeRadius;
        const int32 Columns=FMath::Clamp(FMath::FloorToInt((Limit*2.-Stagger)/Step)+1,0,8);
        TArray<FVector,TInlineAllocator<16>> Raw;
        for(int32 Column=0;Column<Columns;++Column)for(int32 Row=0;Row<2;++Row)
        {
            const double Longitudinal=(Column-(Columns-1)*.5)*Step+(Row?Stagger*.5:-Stagger*.5);
            const double Lateral=T.CellSize*.44+(Row?HalfRow:-HalfRow);
            FVector Position=Centre+Along*Longitudinal+Across*Lateral;
            const double X=Position.X-(T.GridOrigin.X+Road.X*static_cast<double>(T.CellSize));
            const double Y=Position.Y-(T.GridOrigin.Y+Road.Y*static_cast<double>(T.CellSize));
            if(!IsConformingSidewalkDisc(X,Y,SafeRadius,Mask,T.CellSize))continue;
            bool Separate=true;for(const FVector& Existing:Raw)Separate&=FVector::DistSquared2D(Position,Existing)>=FMath::Square(PersonalSpacing);
            if(!Separate||T.WorldToGrid(Position)!=Road)continue;
            Position.Z=Roads.SampleRoadSurfaceElevation(Position)+5.f;
            if(!Position.ContainsNaN())Raw.Add(Position);
        }
        if(Raw.Num()<3)return false;
        // Reserve the final two actual spaces for exits before allocating the
        // waiting roster. Narrow/masked frontages honestly expose fewer slots.
        Out.Exits.Add(Raw[Raw.Num()-2]);Out.Exits.Add(Raw.Last());
        const int32 Count=FMath::Min(12,Raw.Num()-2);
        for(int32 I=0;I<Count;++I)Out.Waiting.Add(Raw[I]);
        return !Out.Waiting.IsEmpty();
    }

	bool BodyBoxesOverlap(const FVector& A,float AYaw,const FVector& ASize,const FVector& B,float BYaw,const FVector& BSize,float Margin=3.f)
	{
		if(A.Z+ASize.Z<B.Z||B.Z+BSize.Z<A.Z)return false;
		const FVector AX=FRotator(0,AYaw,0).Vector(),AY(-AX.Y,AX.X,0),BX=FRotator(0,BYaw,0).Vector(),BY(-BX.Y,BX.X,0);
		const FVector Delta=B-A;
		for(const FVector Axis:{AX,AY,BX,BY})
		{
			const float AH=(FMath::Abs(FVector::DotProduct(AX,Axis))*ASize.X+FMath::Abs(FVector::DotProduct(AY,Axis))*ASize.Y)*.5f;
			const float BH=(FMath::Abs(FVector::DotProduct(BX,Axis))*BSize.X+FMath::Abs(FVector::DotProduct(BY,Axis))*BSize.Y)*.5f;
			if(FMath::Abs(FVector::DotProduct(Delta,Axis))>=AH+BH+Margin)return false;
		}
		return true;
	}
}

AAftershockTransportHost::AAftershockTransportHost()
{
	PrimaryActorTick.bCanEverTick=false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("TransportRoot")));
	Cars=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ResidentCars"));
	Hatchbacks=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ResidentHatchbacks"));
	Utilities=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ResidentUtilities"));
	Buses=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ServiceBuses"));
	Wheels=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("VehicleWheels"));
	ServiceEquipment=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("MunicipalVehicleEquipment"));
	for(UInstancedStaticMeshComponent* Batch:{Cars.Get(),Hatchbacks.Get(),Utilities.Get(),Buses.Get(),Wheels.Get(),ServiceEquipment.Get()})
	{
		Batch->SetupAttachment(GetRootComponent());Batch->SetMobility(EComponentMobility::Movable);
		Batch->SetCollisionEnabled(ECollisionEnabled::NoCollision);Batch->SetCanEverAffectNavigation(false);
		Batch->SetCullDistances(0,50000);Batch->SetCastShadow(true);Batch->NumCustomDataFloats=3;
	}
	ConstructorHelpers::FObjectFinder<UStaticMesh> Car(TEXT("/Game/Environment/Transport/SM_ResidentCar.SM_ResidentCar"));
	ConstructorHelpers::FObjectFinder<UStaticMesh> Bus(TEXT("/Game/Environment/Transport/SM_CityMinibus.SM_CityMinibus"));
	if(Car.Succeeded())Cars->SetStaticMesh(Car.Object);if(Bus.Succeeded())Buses->SetStaticMesh(Bus.Object);
	UStaticMesh* Hatch=LoadObject<UStaticMesh>(nullptr,TEXT("/Game/Environment/Transport/SM_ResidentHatchback.SM_ResidentHatchback"));
	UStaticMesh* Utility=LoadObject<UStaticMesh>(nullptr,TEXT("/Game/Environment/Transport/SM_ResidentUtility.SM_ResidentUtility"));
	UStaticMesh* DefaultCar=Cars->GetStaticMesh();
	Hatchbacks->SetStaticMesh(Hatch?Hatch:DefaultCar);Utilities->SetStaticMesh(Utility?Utility:DefaultCar);
	UStaticMesh* Wheel=LoadObject<UStaticMesh>(nullptr,TEXT("/Game/Environment/Transport/SM_VehicleWheel.SM_VehicleWheel"));
	if(!Wheel)Wheel=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if(Wheel)Wheels->SetStaticMesh(Wheel);
	if(auto* EquipmentMesh=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")))ServiceEquipment->SetStaticMesh(EquipmentMesh);
	// This existing native material reads RGB across the whole instance. The
	// vehicle palette material requires authored atlas UVs, which a cube lacks.
	if(auto* EquipmentMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Environment/Materials/M_TerrainCell.M_TerrainCell")))ServiceEquipment->SetMaterial(0,EquipmentMaterial);
	// The shared wheel mesh has its own tyre/rim palette. A rubber override
	// would erase the rim and spoke material of the authored model.
}

FVector AAftershockTransportHost::GetVehicleDimensionsCm(const FAftershockRoadVehicle& Vehicle,float CellSize) const
{
	const uint32 Variant=IsMunicipalVehicle(Vehicle)?2u:static_cast<uint32>(FMath::Max(0,Vehicle.ResidentOwnerID))%3u;
	const UInstancedStaticMeshComponent* Body=Vehicle.bBus?Buses.Get():(Variant==1?Hatchbacks.Get():(Variant==2?Utilities.Get():Cars.Get()));
	FVector Size=Vehicle.bBus?FVector(650,230,270):FVector(450,211,170);
	if(Body&&Body->GetStaticMesh())
	{
		const FBox Bounds=Body->GetStaticMesh()->GetBoundingBox();
		// Include any asymmetric bumper/mirror around the actual ground pivot.
		Size=FVector(FMath::Max(FMath::Abs(Bounds.Min.X),FMath::Abs(Bounds.Max.X))*2.,FMath::Max(FMath::Abs(Bounds.Min.Y),FMath::Abs(Bounds.Max.Y))*2.,Bounds.GetSize().Z);
	}
	if(IsMunicipalVehicle(Vehicle))Size.Z+=45.f;
	return Size*(CellSize>=900.f?1.f:GetVehicleRenderScale(Vehicle,CellSize));
}

float AAftershockTransportHost::GetVehicleRenderScale(const FAftershockRoadVehicle& Vehicle,float CellSize) const
{
	if(CellSize>=900.f)return 1.f;
	const FVector Native=GetVehicleDimensionsCm(Vehicle,1000.f);
	// Legacy five-metre tiles encode a compressed city. Reserve a32cm human
	// radius and8cm clear gap at the actual .42-tile footway, then fit both the
	// model's mirrors and its turning tail to the available lane/stop bay.
	const float Lane=CellSize*.22f,FootwayInner=CellSize*.42f-32.f;
	const float UsableHalfWidth=FMath::Max(8.f,FMath::Min(Lane-8.f,FootwayInner-Lane-8.f));
	return FMath::Clamp(FMath::Min(UsableHalfWidth*2.f/FMath::Max(1.f,static_cast<float>(Native.Y)),CellSize*.84f/FMath::Max(1.f,static_cast<float>(Native.X))),.05f,1.f);
}

void AAftershockTransportHost::DrawFleet(const TArray<FAftershockRoadVehicle>& Fleet,const TMap<int32,EAftershockMunicipalService>* MunicipalRoles)
{
	const FAftershockPresentationBudget Budget = GetAftershockPresentationBudget(GetWorld());
	for (UInstancedStaticMeshComponent* Batch : {Cars.Get(), Hatchbacks.Get(), Utilities.Get(), Buses.Get(), Wheels.Get(),ServiceEquipment.Get()})
	{
		const bool bVisible = Budget.bDrawVehicleDetails;
		if (Batch->IsVisible() != bVisible) Batch->SetVisibility(bVisible);
		const int32 LODs = Batch->GetStaticMesh() ? Batch->GetStaticMesh()->GetNumLODs() : 1;
		const int32 ForcedLOD = FMath::Min(Budget.PropLODFloor, LODs);
		if (Batch->GetForcedLodModel() != ForcedLOD) Batch->SetForcedLodModel(ForcedLOD);
	}
	if (!Budget.bDrawVehicleDetails) return; // Canonical vehicles/trips keep advancing in UpdateTransport.
	const FTransform Hidden(FQuat::Identity,FVector::ZeroVector,FVector::ZeroVector);
	auto& CarTransforms = CarTransformScratch;
	auto& BusTransforms = BusTransformScratch;
	auto& WheelTransforms = WheelTransformScratch;
	auto& CarColors = CarColorScratch;
	auto& BusColors = BusColorScratch;
	for(int32 I=0;I<3;++I){CarTransforms[I].Init(Hidden,MaxRenderedCars);CarColors[I].Init(FLinearColor::White,MaxRenderedCars);}
	BusTransforms.Init(Hidden,MaxBuses);BusColors.Init(FLinearColor::White,MaxBuses);WheelTransforms.Init(Hidden,MaxWheels);
	ServiceEquipmentScratch.Reset();ServiceEquipmentColours.Reset();
	const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const FIntRect Drawable=Terrain?GetAftershockDrawableCellBounds(*Terrain):FIntRect(0,0,0,0);
	int32 CarIndices[3]={0,0,0},TotalCars=0,BusIndex=0,WheelIndex=0;
	for(const FAftershockRoadVehicle& V:Fleet)
	{
		if(!V.bActive||V.ParkedAtBuildingID!=INDEX_NONE)continue;
		if(Terrain&&!Drawable.Contains(Terrain->WorldToGrid(V.Location)))continue;
		const FQuat Rotation=FRotator(0,V.Yaw,0).Quaternion();
		const float Fit=GetVehicleRenderScale(V,Terrain?Terrain->CellSize:1000.f);
		const int32 Variant=IsMunicipalVehicle(V)?2:static_cast<uint32>(FMath::Max(0,V.ResidentOwnerID))%3u;
		const int32 Index=V.bBus?BusIndex++:CarIndices[Variant]++;
		if(V.bBus){if(Index>=MaxBuses)continue;BusTransforms[Index]=FTransform(Rotation,V.Location,FVector(Fit));}
		else{if(TotalCars++>=MaxRenderedCars)continue;CarTransforms[Variant][Index]=FTransform(Rotation,V.Location,FVector(Fit));}
		{
			const FLinearColor Palette[]={FLinearColor(.025f,.03f,.038f),FLinearColor(.80f,.82f,.82f),FLinearColor(.42f,.46f,.50f),FLinearColor(.10f,.115f,.13f),
				FLinearColor(.065f,.14f,.24f),FLinearColor(.46f,.065f,.045f),FLinearColor(.065f,.16f,.13f),FLinearColor(.44f,.36f,.25f)};
			// Car preference samples canonical IDs in regular strides. Mix all bits
			// before palette selection so owners4,8,12,... do not all become blue.
			uint32 PaintSeed=static_cast<uint32>(V.ResidentOwnerID)^0x68bc21ebu;
			PaintSeed^=PaintSeed>>16;PaintSeed*=0x7feb352du;PaintSeed^=PaintSeed>>15;PaintSeed*=0x846ca68bu;PaintSeed^=PaintSeed>>16;
			FLinearColor C=V.bBus?FLinearColor(.78f,.48f,.055f):Palette[PaintSeed%UE_ARRAY_COUNT(Palette)];
			const EAftershockMunicipalService MunicipalService=MunicipalRoles?MunicipalRoles->FindRef(V.VehicleID):EAftershockMunicipalService::None;
			if(MunicipalService!=EAftershockMunicipalService::None)
			{
				C=MunicipalService==EAftershockMunicipalService::FireResponse?FLinearColor(.72f,.025f,.012f):MunicipalService==EAftershockMunicipalService::Ambulance?FLinearColor(.88f,.89f,.84f):MunicipalService==EAftershockMunicipalService::MunicipalUtility?FLinearColor(.78f,.38f,.035f):FLinearColor(.022f,.055f,.12f);
				const FVector Native=GetVehicleDimensionsCm(V,1000.f);const float Roof=Native.Z-25.f;
				const auto Equipment=[&](FVector Offset,FVector Size,FLinearColor Colour,float LocalYaw=0.f)
				{ServiceEquipmentScratch.Emplace(Rotation*FRotator(0,LocalYaw,0).Quaternion(),V.Location+Rotation.RotateVector(Offset*Fit),Size*.01f*Fit);ServiceEquipmentColours.Add(Colour);};
				Equipment(FVector(35,0,Roof),FVector(28,118,16),FLinearColor(.055f,.06f,.07f));
				Equipment(FVector(35,-40,Roof+10),FVector(24,28,14),FLinearColor(.86f,.015f,.005f));
				Equipment(FVector(35,40,Roof+10),FVector(24,28,14),MunicipalService==EAftershockMunicipalService::MunicipalUtility?FLinearColor(1.f,.43f,.015f):FLinearColor(.005f,.16f,.95f));
				if(MunicipalService==EAftershockMunicipalService::FireResponse)
				{
					for(float Side:{-1.f,1.f})Equipment(FVector(-72,Side*30,Roof+12),FVector(210,8,8),FLinearColor(.48f,.5f,.52f));
					for(int32 Rung=0;Rung<5;++Rung)Equipment(FVector(-156+Rung*42,0,Roof+12),FVector(6,66,6),FLinearColor(.48f,.5f,.52f));
				}
				else if(MunicipalService==EAftershockMunicipalService::Ambulance)
				{
					for(float Arm:{0.f,60.f,120.f})Equipment(FVector(-78,0,Roof+5),FVector(85,22,5),FLinearColor(.025f,.18f,.6f),Arm);
				}
			}
			if(V.bBus)BusColors[Index]=C;else CarColors[Variant][Index]=C;
		}
		const float Radius=V.bBus?43.f:31.f,HalfWidth=V.bBus?107.f:78.f;
		const float Axles[]={V.bBus?210.f:120.f,V.bBus?-180.f:-120.f};
		for(int32 Axle=0;Axle<2;++Axle)for(float Side:{-1.f,1.f})
		{
			const FVector Offset(Axles[Axle],Side*HalfWidth,Radius);
			const FQuat Q=Rotation*FQuat(FVector::RightVector,V.WheelAngle)*FQuat(FVector::ForwardVector,HALF_PI);
			if(WheelIndex<MaxWheels)WheelTransforms[WheelIndex++]=FTransform(Q,V.Location+Rotation.RotateVector(Offset*Fit),FVector(Radius/50.f,Radius/50.f,.20f)*Fit);
		}
	}
	const auto Submit=[this](UInstancedStaticMeshComponent* Batch,const TArray<FTransform>& Transforms,int32 Slot)
	{
		auto& Last = LastSubmittedTransforms[Slot];
		bool bChanged = Batch->GetInstanceCount() != Transforms.Num() || Last.Num() != Transforms.Num();
		for (int32 I = 0; !bChanged && I < Transforms.Num(); ++I) bChanged = !Last[I].Equals(Transforms[I], .001f);
		if (!bChanged) return false;
		if(Batch->GetInstanceCount()!=Transforms.Num())
		{
			if (Slot < 4) LastSubmittedColors[Slot].Reset();
			Batch->ClearInstances();
			Batch->AddInstances(Transforms,false,true,false);
		}
		else if(!Transforms.IsEmpty())
		{
			Batch->BatchUpdateInstancesTransforms(0,Transforms,true,false,true);
		}
		Last = Transforms;
		return true;
	};
	bool Dirty[5] = {Submit(Cars,CarTransforms[0],0),Submit(Hatchbacks,CarTransforms[1],1),Submit(Utilities,CarTransforms[2],2),Submit(Buses,BusTransforms,3),Submit(Wheels,WheelTransforms,4)};
	const auto SetColors=[this](UInstancedStaticMeshComponent* Batch,const TArray<FLinearColor>& Colors,int32 Slot)
	{
		auto& Last = LastSubmittedColors[Slot];
		bool bChanged = Last.Num() != Colors.Num();
		for (int32 I = 0; !bChanged && I < Colors.Num(); ++I) bChanged = Last[I] != Colors[I];
		if (!bChanged) return false;
		for(int32 I=0;I<Colors.Num();++I)for(int32 Channel=0;Channel<3;++Channel)
			Batch->SetCustomDataValue(I,Channel,Channel==0?Colors[I].R:(Channel==1?Colors[I].G:Colors[I].B),false);
		Last = Colors;
		return true;
	};
	Dirty[0] |= SetColors(Cars,CarColors[0],0); Dirty[1] |= SetColors(Hatchbacks,CarColors[1],1);
	Dirty[2] |= SetColors(Utilities,CarColors[2],2); Dirty[3] |= SetColors(Buses,BusColors,3);
	UInstancedStaticMeshComponent* Batches[] = {Cars.Get(), Hatchbacks.Get(), Utilities.Get(), Buses.Get(), Wheels.Get()};
	for (int32 I = 0; I < 5; ++I) if (Dirty[I]) Batches[I]->MarkRenderInstancesDirty();
	bool EquipmentChanged=ServiceEquipmentScratch.Num()!=LastServiceEquipment.Num()||ServiceEquipmentColours!=LastServiceEquipmentColours;
	for(int32 I=0;!EquipmentChanged&&I<ServiceEquipmentScratch.Num();++I)EquipmentChanged=!ServiceEquipmentScratch[I].Equals(LastServiceEquipment[I],.001f);
	if(EquipmentChanged)
	{
		if(ServiceEquipment->GetInstanceCount()!=ServiceEquipmentScratch.Num())
		{ServiceEquipment->ClearInstances();ServiceEquipment->AddInstances(ServiceEquipmentScratch,false,true,false);}
		else if(!ServiceEquipmentScratch.IsEmpty())ServiceEquipment->BatchUpdateInstancesTransforms(0,ServiceEquipmentScratch,true,false,true);
		for(int32 I=0;I<ServiceEquipmentColours.Num();++I)
		{const auto& C=ServiceEquipmentColours[I];ServiceEquipment->SetCustomDataValue(I,0,C.R,false);ServiceEquipment->SetCustomDataValue(I,1,C.G,false);ServiceEquipment->SetCustomDataValue(I,2,C.B,false);}
		ServiceEquipment->MarkRenderInstancesDirty();LastServiceEquipment=ServiceEquipmentScratch;LastServiceEquipmentColours=ServiceEquipmentColours;
	}
}

void UTransportSubsystem::DrawAllFleets()
{
	if(!IsValid(Host))return;
	FleetRenderScratch.Reset();FleetRenderScratch.Reserve(Vehicles.Num()+MaxMunicipalFleet);MunicipalRenderRoles.Reset();
	const auto AppendPose=[this](const FAftershockRoadVehicle& Vehicle)
	{
		// Rendering does not need paths, stops or passengers. Copying every
		// city's road-path arrays on each 80 ms update would create avoidable
		// allocations and memory traffic merely to add a few municipal poses.
		auto& Pose=FleetRenderScratch.AddDefaulted_GetRef();Pose.VehicleID=Vehicle.VehicleID;Pose.ResidentOwnerID=Vehicle.ResidentOwnerID;
		Pose.bBus=Vehicle.bBus;Pose.bActive=Vehicle.bActive;Pose.ParkedAtBuildingID=Vehicle.ParkedAtBuildingID;
		Pose.Location=Vehicle.Location;Pose.Yaw=Vehicle.Yaw;Pose.WheelAngle=Vehicle.WheelAngle;
	};
	for(const auto& Vehicle:Vehicles)AppendPose(Vehicle);
	for(const auto& Unit:MunicipalFleet)if(Unit.bDeployed)
	{AppendPose(Unit.Vehicle);MunicipalRenderRoles.Add(Unit.Vehicle.VehicleID,Unit.Service);}
	Host->DrawFleet(FleetRenderScratch,&MunicipalRenderRoles);
}

void UTransportSubsystem::RefreshVisiblePresentation()
{
	DrawAllFleets();
}

bool UTransportSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* W=Cast<UWorld>(Outer);return Super::ShouldCreateSubsystem(Outer)&&W&&(W->WorldType==EWorldType::Game||W->WorldType==EWorldType::PIE);
}
void UTransportSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);Collection.InitializeDependency<UTerrainGridSubsystem>();Collection.InitializeDependency<UBuildingManagerSubsystem>();
	Collection.InitializeDependency<URoadNetworkSubsystem>();Collection.InitializeDependency<UCityLifeSubsystem>();
	if(URoadNetworkSubsystem* R=GetWorld()->GetSubsystem<URoadNetworkSubsystem>())R->OnRoadNetworkChanged.AddUniqueDynamic(this,&UTransportSubsystem::HandleRoadsChanged);
	if(UCityLifeSubsystem* L=GetWorld()->GetSubsystem<UCityLifeSubsystem>())L->OnCityLifeUpdated.AddUniqueDynamic(this,&UTransportSubsystem::HandleCityLifeUpdated);
}
void UTransportSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);InWorld.GetTimerManager().SetTimer(UpdateTimer,this,&UTransportSubsystem::UpdateTransport,TransportStep,true,.25f);
}
void UTransportSubsystem::Deinitialize()
{
	if(UWorld* W=GetWorld())
	{
		W->GetTimerManager().ClearTimer(UpdateTimer);
		if(URoadNetworkSubsystem* R=W->GetSubsystem<URoadNetworkSubsystem>())R->OnRoadNetworkChanged.RemoveDynamic(this,&UTransportSubsystem::HandleRoadsChanged);
		if(UCityLifeSubsystem* L=W->GetSubsystem<UCityLifeSubsystem>())L->OnCityLifeUpdated.RemoveDynamic(this,&UTransportSubsystem::HandleCityLifeUpdated);
	}
	if(IsValid(Host))Host->Destroy();Host=nullptr;Trips.Empty();Vehicles.Empty();Stops.Empty();MunicipalFleet.Empty();MunicipalFleetByResident.Empty();FleetRenderScratch.Empty();Super::Deinitialize();
}
void UTransportSubsystem::HandleRoadsChanged()
{
	bInfrastructureDirty=true;
	const UGameInstance* Instance=GetWorld()?GetWorld()->GetGameInstance():nullptr;
	const UAftershockSaveSubsystem* Saves=Instance?Instance->GetSubsystem<UAftershockSaveSubsystem>():nullptr;
	if (!Saves || !Saves->IsRestoring()) RefreshInfrastructure();
}
void UTransportSubsystem::HandleCityLifeUpdated(){bInfrastructureDirty=true;}

bool UTransportSubsystem::IsRoadSafe(FIntPoint Coord) const
{
	const UWorld* W=GetWorld();const UTerrainGridSubsystem* T=W?W->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const auto* Roads=W?W->GetSubsystem<URoadNetworkSubsystem>():nullptr;
	return T&&Roads&&Roads->IsRoadTraversable(Coord)&&Roads->GetRoadFloodDepthCm(Coord)<=35.f;
}
FVector UTransportSubsystem::RoadLocation(FIntPoint Coord,FVector Direction) const
{
	const UTerrainGridSubsystem* T=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;if(!T)return FVector::ZeroVector;
	Direction=Direction.GetSafeNormal2D();if(Direction.IsNearlyZero())Direction=FVector::ForwardVector;
	const auto* Roads=GetWorld()->GetSubsystem<URoadNetworkSubsystem>();
	const FVector Base=(Roads?Roads->RoadGridToWorld(Coord):T->GridToWorld(Coord));
	const float OffsetMag=T->CellSize*(T->CellSize>=900.f?.165f:.22f);
	FVector Offset=FVector(-Direction.Y,Direction.X,0)*OffsetMag;
	const FIntPoint Neighbor=T->WorldToGrid(Base+Offset*1.5f);
	if(Neighbor!=Coord&&T->IsValidCoord(Neighbor)&&T->GetCell(Neighbor).OccupyingBuildingID!=INDEX_NONE)
	{
		const auto* Buildings=GetWorld()?GetWorld()->GetSubsystem<UBuildingManagerSubsystem>():nullptr;
		FBuildingInstance Bld;
		if(Buildings&&Buildings->GetBuildingByID(T->GetCell(Neighbor).OccupyingBuildingID,Bld)&&Bld.BuildingData&&Bld.BuildingData->Type!=EBuildingType::GreenSpace)
		{
			Offset=-Offset;
		}
	}
	return Base+Offset+FVector(0,0,3);
}

FVector UTransportSubsystem::VehicleDimensions(const FAftershockRoadVehicle& Vehicle) const
{
	const AAftershockTransportHost* Visual=IsValid(Host)?Host.Get():GetDefault<AAftershockTransportHost>();
	const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	return Visual->GetVehicleDimensionsCm(Vehicle,Terrain?Terrain->CellSize:1000.f);
}

bool UTransportSubsystem::VehicleSweepIntersectsDisc(const FVector& From,float FromYaw,const FVector& To,float ToYaw,
	const FVector& Dimensions,const FVector& DiscFrom,const FVector& DiscTo,float RadiusCm)
{
	if(From.ContainsNaN()||To.ContainsNaN()||DiscFrom.ContainsNaN()||DiscTo.ContainsNaN()||Dimensions.ContainsNaN()||RadiusCm<0.f)return true;
	const float Angle=FMath::FindDeltaAngleDegrees(FromYaw,ToYaw);
	const float RelativeDistance=FVector::Dist2D(From,To)+FVector::Dist2D(DiscFrom,DiscTo);
	const int32 Steps=FMath::Max(1,FMath::CeilToInt(FMath::Max(RelativeDistance/12.f,FMath::Abs(Angle)/3.f)));
	if(Steps>128)return true; // A discontinuous import must not become a tunnelling move.
	const float ArcMargin=Dimensions.Size2D()*.5f*FMath::Sin(FMath::DegreesToRadians(FMath::Abs(Angle)/Steps*.5f));
	const float SweepMargin=RelativeDistance/(2.f*Steps)+ArcMargin+.5f;
	for(int32 Step=0;Step<=Steps;++Step)
	{
		const float Alpha=static_cast<float>(Step)/Steps;
		const FVector Centre=FMath::Lerp(From,To,Alpha),Person=FMath::Lerp(DiscFrom,DiscTo,Alpha);
		if(Person.Z>Centre.Z+Dimensions.Z||Person.Z+175.f<Centre.Z)continue;
		const FVector Forward=FRotator(0,FromYaw+Angle*Alpha,0).Vector(),Side(-Forward.Y,Forward.X,0),D=Person-Centre;
		const float DX=FMath::Max(0.f,FMath::Abs(FVector::DotProduct(D,Forward))-Dimensions.X*.5f);
		const float DY=FMath::Max(0.f,FMath::Abs(FVector::DotProduct(D,Side))-Dimensions.Y*.5f);
		if(DX*DX+DY*DY<FMath::Square(RadiusCm+SweepMargin))return true;
	}
	return false;
}

bool UTransportSubsystem::IsPedestrianSpaceClear(const FVector& From,const FVector& To,float RadiusCm,bool bYieldToApproaching) const
{
	const auto ClearOf=[&](const FAftershockRoadVehicle& Vehicle)
	{
		if(!Vehicle.bActive||Vehicle.ParkedAtBuildingID!=INDEX_NONE)return true;
		const FVector Size=VehicleDimensions(Vehicle);
		if(VehicleSweepIntersectsDisc(Vehicle.Location,Vehicle.Yaw,Vehicle.Location,Vehicle.Yaw,Size,From,To,RadiusCm))return false;
		if(!bYieldToApproaching||Vehicle.bStalled||Vehicle.DwellSeconds>0.f||!Vehicle.RoadPath.IsValidIndex(Vehicle.NextPathStep))return true;
		const FVector Forward=FRotator(0,Vehicle.Yaw,0).Vector(),Forecast=Vehicle.Location+Forward*CruiseSpeed(Vehicle)*.4f;
		// A crossing pedestrian already in the forecast corridor retains priority.
		// New entrants wait on the footway; cars test the real person before moving.
		if(!VehicleSweepIntersectsDisc(Vehicle.Location,Vehicle.Yaw,Forecast,Vehicle.Yaw,Size,From,From,RadiusCm)
			&&VehicleSweepIntersectsDisc(Vehicle.Location,Vehicle.Yaw,Forecast,Vehicle.Yaw,Size,From,To,RadiusCm))return false;
		return true;
	};
	for(const FAftershockRoadVehicle& Vehicle:Vehicles)if(!ClearOf(Vehicle))return false;
	for(const auto& Unit:MunicipalFleet)if(!ClearOf(Unit.Vehicle))return false;
	return true;
}

bool UTransportSubsystem::IsMunicipalBodyClear(const FAftershockRoadVehicle& Vehicle,const FVector& To,float ToYaw) const
{
	const FVector Size=VehicleDimensions(Vehicle);const float Distance=FVector::Dist2D(Vehicle.Location,To);
	const float Angle=FMath::FindDeltaAngleDegrees(Vehicle.Yaw,ToYaw);
	const int32 Steps=FMath::Max(1,FMath::CeilToInt(FMath::Max(Distance/10.f,FMath::Abs(Angle)/3.f)));
	if(Steps>128)return false;
	const float Margin=Distance/(2.f*Steps)+Size.Size2D()*.5f*FMath::Sin(FMath::DegreesToRadians(FMath::Abs(Angle)/Steps*.5f))+3.f;
	for(const auto& Unit:MunicipalFleet)
	{
		const auto& Other=Unit.Vehicle;if(!Other.bActive||Other.ParkedAtBuildingID!=INDEX_NONE||Other.VehicleID==Vehicle.VehicleID)continue;
		const FVector OtherSize=VehicleDimensions(Other);
		if(FVector::DistSquared2D(Vehicle.Location,Other.Location)>FMath::Square(Size.Size2D()*.5f+OtherSize.Size2D()*.5f+Distance+Margin))continue;
		for(int32 Step=0;Step<=Steps;++Step)
		{const float Alpha=static_cast<float>(Step)/Steps;if(BodyBoxesOverlap(FMath::Lerp(Vehicle.Location,To,Alpha),Vehicle.Yaw+Angle*Alpha,Size,Other.Location,Other.Yaw,OtherSize,Margin))return false;}
	}
	return true;
}

bool UTransportSubsystem::CanMoveVehicle(const FAftershockRoadVehicle& Vehicle,const FVector& To,float ToYaw,bool bCheckPedestrians) const
{
	if(To.ContainsNaN()||!FMath::IsFinite(ToYaw))return false;
	if(!IsMunicipalBodyClear(Vehicle,To,ToYaw))return false;
	const FVector Size=VehicleDimensions(Vehicle);const float Angle=FMath::FindDeltaAngleDegrees(Vehicle.Yaw,ToYaw);
	const float Distance=FVector::Dist2D(Vehicle.Location,To);
	const int32 Steps=FMath::Max(1,FMath::CeilToInt(FMath::Max(Distance/10.f,FMath::Abs(Angle)/3.f)));
	if(Steps>128)return false;
	const float Margin=Distance/(2.f*Steps)+Size.Size2D()*.5f*FMath::Sin(FMath::DegreesToRadians(FMath::Abs(Angle)/Steps*.5f))+3.f;
	for(const FAftershockRoadVehicle& Other:Vehicles)
	{
		if(!Other.bActive||Other.ParkedAtBuildingID!=INDEX_NONE||Other.VehicleID==Vehicle.VehicleID)continue;
		const FVector OtherSize=VehicleDimensions(Other);
		if(FVector::DistSquared2D(Vehicle.Location,Other.Location)>FMath::Square(Size.Size2D()*.5f+OtherSize.Size2D()*.5f+Distance+Margin))continue;
		for(int32 Step=0;Step<=Steps;++Step)
		{
			const float Alpha=static_cast<float>(Step)/Steps;
			if(BodyBoxesOverlap(FMath::Lerp(Vehicle.Location,To,Alpha),Vehicle.Yaw+Angle*Alpha,Size,Other.Location,Other.Yaw,OtherSize,Margin))return false;
		}
	}
	const UTerrainGridSubsystem* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const UBuildingManagerSubsystem* Buildings=GetWorld()?GetWorld()->GetSubsystem<UBuildingManagerSubsystem>():nullptr;
	if(Terrain&&Buildings)
	{
		const FIntPoint VehicleCell=Terrain->WorldToGrid(To);
		for(int32 DY=-1;DY<=1;++DY)
		{
			for(int32 DX=-1;DX<=1;++DX)
			{
				const FIntPoint CheckCoord=VehicleCell+FIntPoint(DX,DY);
				if(!Terrain->IsValidCoord(CheckCoord))continue;
				const FGridCell& Cell=Terrain->GetCell(CheckCoord);
				if(Cell.OccupyingBuildingID!=INDEX_NONE)
				{
					FBuildingInstance Building;
					if(Buildings->GetBuildingByID(Cell.OccupyingBuildingID,Building)&&Building.BuildingData&&Building.BuildingData->Type!=EBuildingType::GreenSpace)
					{
						const FVector BuildingCenter=Terrain->GridToWorld(CheckCoord,true);
						const FVector BuildingSize=FVector(Terrain->CellSize*0.48f,Terrain->CellSize*0.48f,300.f);
						if(BodyBoxesOverlap(To,ToYaw,Size,BuildingCenter,0.f,BuildingSize,Margin))return false;
					}
				}
			}
		}
	}
	const auto* Crowd=GetWorld()?GetWorld()->GetSubsystem<UCrowdRenderSubsystem>():nullptr;
	const float CrossingLookahead=Vehicle.RoadPath.IsValidIndex(Vehicle.NextPathStep)&&!Vehicle.bParking?CruiseSpeed(Vehicle)*.4f:0.f;
	return !bCheckPedestrians||!Crowd||Crowd->IsVehicleSpaceClear(Vehicle.Location,Vehicle.Yaw,To,ToYaw,Size,INDEX_NONE,CrossingLookahead);
}

bool UTransportSubsystem::TryMoveVehicle(FAftershockRoadVehicle& Vehicle,const FVector& To,float ToYaw)
{
	if(!CanMoveVehicle(Vehicle,To,ToYaw))return false;
	const auto* Terrain=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
	const auto* Visual=IsValid(Host)?Host.Get():GetDefault<AAftershockTransportHost>();
	const float WheelRadius=(Vehicle.bBus?43.f:31.f)*Visual->GetVehicleRenderScale(Vehicle,Terrain?Terrain->CellSize:1000.f);
	Vehicle.WheelAngle=FMath::Fmod(Vehicle.WheelAngle+FVector::Dist2D(Vehicle.Location,To)/FMath::Max(1.f,WheelRadius),2.f*PI);
	Vehicle.Location=To;Vehicle.Yaw=ToYaw;return true;
}

FVector UTransportSubsystem::VehicleDoorLocation(const FAftershockRoadVehicle& Vehicle,float Side) const
{
	const FVector Forward=FRotator(0,Vehicle.Yaw,0).Vector(),Across(-Forward.Y,Forward.X,0);
	FVector Result=Vehicle.Location+Across*Side*(VehicleDimensions(Vehicle).Y*.5f+40.f);
	if(const auto* Roads=GetWorld()->GetSubsystem<URoadNetworkSubsystem>())Result.Z=Roads->SampleRoadSurfaceElevation(Result)+5.f;
	return Result;
}

bool UTransportSubsystem::FindPassengerReleaseLocation(int32 TripID,FVector& OutLocation) const
{
	const auto* Trip=Trips.Find(TripID);const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const auto* Roads=GetWorld()?GetWorld()->GetSubsystem<URoadNetworkSubsystem>():nullptr;
	const auto* Crowd=GetWorld()?GetWorld()->GetSubsystem<UCrowdRenderSubsystem>():nullptr;
	if(!Trip||!Terrain||!Roads)return false;
	const auto* Vehicle=Vehicles.FindByPredicate([&](const auto& V){return V.VehicleID==Trip->VehicleID;});
	TArray<FVector,TInlineAllocator<8>> Candidates;
	if(Trip->State==EAftershockPassengerState::Arrived)Candidates.Add(Trip->CurrentLocation);
	else if(Vehicle)
	{
		if(Vehicle->bBus)for(int32 Slot=0;Slot<2;++Slot){FVector At;if(GetPassengerKerbLocation(Trip->DropoffStopID,Slot,true,At))Candidates.Add(At);}
		else for(float Side:{1.f,-1.f})Candidates.Add(VehicleDoorLocation(*Vehicle,Side));
		if(Vehicle->bBus&&(Trip->State==EAftershockPassengerState::Cancelled||Trip->State==EAftershockPassengerState::Stranded))
			for(float Side:{1.f,-1.f})Candidates.Add(VehicleDoorLocation(*Vehicle,Side));
	}
	if(Candidates.IsEmpty())Candidates.Add(Trip->CurrentLocation);
	for(const FVector& Candidate:Candidates)
	{
		const FIntPoint Coord=Terrain->WorldToGrid(Candidate);
		if(!Roads->IsPublicWalkingCell(Coord)||!IsPedestrianSpaceClear(Candidate,Candidate,32.f,false))continue;
		if(Crowd&&!Crowd->CanAdmitCitizenAt(Candidate,Trip->ResidentID))continue;
		OutLocation=Candidate;return true;
	}
	return false;
}

void UTransportSubsystem::RetainPendingPassengerHandoff(int32 TripID)
{
	if(auto* Trip=Trips.Find(TripID);Trip&&!ActiveTrip(Trip->State))Trip->TerminalObservedAtSeconds=HistoryClockSeconds;
}

int32 UTransportSubsystem::GetOffstreetParkingCapacity(const FBuildingInstance& Building)
{
	if(!Building.BuildingData)return 0;
	const int32 Floors=FMath::Clamp(Building.CurrentFloors,1,40);
	switch(Building.BuildingData->Type)
	{
	case EBuildingType::House:return 1;
	case EBuildingType::Apartment:return FMath::Min(40,Floors*2);
	case EBuildingType::Office:return FMath::Min(60,Floors*3);
	case EBuildingType::Factory:return FMath::Min(20,Floors*2);
	case EBuildingType::Hospital:return FMath::Min(40,Floors*4);
	case EBuildingType::Restaurant:return 2;
	case EBuildingType::Bakery:return 1;
	default:return 0;
	}
}

bool UTransportSubsystem::HasOffstreetParkingSpace(int32 BuildingID,int32 IgnoredOwnerID) const
{
	const auto* Buildings=GetWorld()?GetWorld()->GetSubsystem<UBuildingManagerSubsystem>():nullptr;FBuildingInstance Building;
	if(!Buildings||!Buildings->GetBuildingByID(BuildingID,Building)||Building.DamageState>=EDamageState::Damaged)return false;
	const int32 Capacity=GetOffstreetParkingCapacity(Building);if(Capacity<=0)return false;
	int32 Occupied=0;
	for(const auto& V:Vehicles)Occupied+=V.bActive&&!V.bBus&&V.ResidentOwnerID!=IgnoredOwnerID&&V.ParkedAtBuildingID==BuildingID?1:0;
	for(const auto& Pair:Trips)
	{
		const auto& P=Pair.Value;
		Occupied+=P.ResidentID!=IgnoredOwnerID&&P.Mode==EAftershockTravelMode::PrivateCar&&ActiveTrip(P.State)&&P.DestinationBuildingID==BuildingID?1:0;
	}
	return Occupied<Capacity;
}

bool UTransportSubsystem::CanParkOffstreet(const FAftershockRoadVehicle& Vehicle,int32 BuildingID) const
{
	const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const auto* Buildings=GetWorld()?GetWorld()->GetSubsystem<UBuildingManagerSubsystem>():nullptr;FBuildingInstance Building;FIntPoint Entry;
	return !Vehicle.bBus&&Vehicle.ResidentOwnerID!=INDEX_NONE&&Terrain&&Buildings&&Buildings->GetBuildingByID(BuildingID,Building)
		&&FindRoadNear(Building.GridCoords,Entry)&&Entry==Terrain->WorldToGrid(Vehicle.Location)&&HasOffstreetParkingSpace(BuildingID,Vehicle.ResidentOwnerID);
}

bool UTransportSubsystem::ReconcileVehiclePlacements(FAftershockTransportSaveState& InOutState,FText& Error) const
{
	Error=FText::GetEmpty();const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const auto* Roads=GetWorld()?GetWorld()->GetSubsystem<URoadNetworkSubsystem>():nullptr;
	const auto* Buildings=GetWorld()?GetWorld()->GetSubsystem<UBuildingManagerSubsystem>():nullptr;
	if(!Terrain||!Roads||!Buildings||InOutState.Vehicles.Num()>MaxCars+MaxBuses)return false;
	FAftershockTransportSaveState Candidate=InOutState;TArray<int32> Accepted,Order;TMap<int32,int32> GarageUse;
	for(int32 I=0;I<Candidate.Vehicles.Num();++I){Order.Add(I);if(Candidate.Vehicles[I].bActive&&Candidate.Vehicles[I].ParkedAtBuildingID!=INDEX_NONE)++GarageUse.FindOrAdd(Candidate.Vehicles[I].ParkedAtBuildingID);}
	Order.Sort([&](int32 A,int32 B){return Candidate.Vehicles[A].VehicleID<Candidate.Vehicles[B].VehicleID;});
	int32 Moved=0,GarageMigrated=0;float MaximumMove=0.f;
	const auto Clear=[&](const FAftershockRoadVehicle& V)
	{
		if(V.Location.ContainsNaN()||!Roads->IsRoadTraversable(Terrain->WorldToGrid(V.Location)))return false;
		const FVector Size=VehicleDimensions(V);
		for(int32 J:Accepted){const auto& Other=Candidate.Vehicles[J];if(BodyBoxesOverlap(V.Location,V.Yaw,Size,Other.Location,Other.Yaw,VehicleDimensions(Other),4.f))return false;}
		return true;
	};
	for(int32 I:Order)
	{
		auto& V=Candidate.Vehicles[I];if(!V.bActive||V.ParkedAtBuildingID!=INDEX_NONE)continue;
		// An old completed car may legitimately already be in its destination's
		// actual garage. This is evidenced by the same owner's completed trip.
		if(!V.bBus&&V.PassengerTripIDs.IsEmpty())
		{
			const FAftershockPassengerTrip* Arrival=Candidate.Trips.FindByPredicate([&](const auto& P){return P.VehicleID==V.VehicleID&&P.ResidentID==V.ResidentOwnerID&&P.State==EAftershockPassengerState::Arrived&&P.DestinationBuildingID!=INDEX_NONE;});
			FBuildingInstance Destination;FIntPoint Entry;
			if(Arrival&&Buildings->GetBuildingByID(Arrival->DestinationBuildingID,Destination)&&FindRoadNear(Destination.GridCoords,Entry)&&Entry==Terrain->WorldToGrid(V.Location)
				&&GarageUse.FindRef(Destination.InstanceID)<GetOffstreetParkingCapacity(Destination))
			{V.ParkedAtBuildingID=Destination.InstanceID;++GarageUse.FindOrAdd(Destination.InstanceID);++GarageMigrated;continue;}
		}
		if(!Clear(V))
		{
			const FVector Original=V.Location;const FIntPoint Start=Terrain->WorldToGrid(Original);
			const FIntPoint Goal=V.RoadPath.IsValidIndex(V.NextPathStep)?V.RoadPath.Last():Start;
			const bool MustKeepStop=V.bBus&&(!V.PendingAlightTripIDs.IsEmpty()||V.DwellSeconds>0.f);
			TArray<FIntPoint,TInlineAllocator<128>> Queue;TSet<FIntPoint> Seen;Queue.Add(Start);Seen.Add(Start);bool Found=false;
			for(int32 Read=0;Read<Queue.Num()&&Read<128&&!Found;++Read)
			{
				const FIntPoint Coord=Queue[Read];TArray<FIntPoint> Path;
				if((!MustKeepStop||Coord==Start)&&IsRoadSafe(Coord)&&(Coord==Goal||BuildRoadPath(Coord,Goal,Path)))
				{
					const FVector Heading=MustKeepStop||Path.IsEmpty()?FRotator(0,V.Yaw,0).Vector():(Terrain->GridToWorld(Path[0])-Terrain->GridToWorld(Coord)).GetSafeNormal2D();
					for(float Side:{1.f,-1.f})
					{
						if(Side<0.f&&!MustKeepStop)continue;
						FAftershockRoadVehicle At=V;At.Location=RoadLocation(Coord,Heading*Side);At.Yaw=(Heading*Side).Rotation().Yaw;
						if(Clear(At))
						{
							V.Location=At.Location;V.Yaw=At.Yaw;
							if(!MustKeepStop){V.SegmentOriginCell=Coord;V.RoadPath=MoveTemp(Path);V.NextPathStep=0;V.bSegmentLaneAligned=true;V.bParking=false;}
							Found=true;break;
						}
					}
				}
				for(FIntPoint Offset:Adjacent)
				{
					const FIntPoint Next=Coord+Offset;if(Queue.Num()>=128||Seen.Contains(Next)||!IsRoadSafe(Next)||!Roads->CanTraverseRoadEdge(Coord,Next))continue;
					Seen.Add(Next);Queue.Add(Next);
				}
			}
			if(!Found){Error=NSLOCTEXT("Aftershock","TrafficPhysicalCapacity","Kayıtlı araçlar için aynı yol ağı üzerinde çakışmasız alan yok; trafik değiştirilmedi.");return false;}
			++Moved;MaximumMove=FMath::Max(MaximumMove,static_cast<float>(FVector::Dist2D(Original,V.Location)));
			for(auto& P:Candidate.Trips)if(P.VehicleID==V.VehicleID)
			{
				if(P.State==EAftershockPassengerState::Riding)P.CurrentLocation=V.Location;
				else if(P.Mode==EAftershockTravelMode::PrivateCar&&(P.State==EAftershockPassengerState::Waiting||P.State==EAftershockPassengerState::WalkingToPickup))
				{P.PickupRoadCell=Terrain->WorldToGrid(V.Location);P.PickupLocation=VehicleDoorLocation(V);}
			}
		}
		Accepted.Add(I);
	}
	InOutState=MoveTemp(Candidate);
	if(Moved||GarageMigrated)UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_TRAFFIC_VISUAL_RECONCILED moved=%d garages=%d maxDistanceCm=%.2f vehicleIDsPreserved=1 passengerIDsPreserved=1 newVehicles=0"),Moved,GarageMigrated,MaximumMove);
	return true;
}

bool UTransportSubsystem::EvictBuildingParking(int32 BuildingID)
{
	if(BuildingID==INDEX_NONE)return false;
	const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	if(!Terrain)return false;
	TArray<int32> Indices;TArray<FAftershockRoadVehicle> Staged;
	for(int32 I=0;I<Vehicles.Num();++I)
	{
		const auto& Parked=Vehicles[I];if(!Parked.bActive||Parked.ParkedAtBuildingID!=BuildingID)continue;
		const FIntPoint Entry=Terrain->WorldToGrid(Parked.Location);if(!IsRoadSafe(Entry))return false;
		bool Found=false;
		for(FIntPoint Offset:Adjacent)
		{
			if(!IsRoadSafe(Entry+Offset))continue;const FVector Direction(Offset.X,Offset.Y,0);
			const float HalfLength=VehicleDimensions(Parked).X*.5f;
			const float AlongRoom=FMath::Max(0.f,Terrain->CellSize*.5f-HalfLength-8.f);
			for(float Along:{0.f,-AlongRoom,AlongRoom})
			{
				FAftershockRoadVehicle Emerging=Parked;Emerging.ParkedAtBuildingID=INDEX_NONE;Emerging.Location=RoadLocation(Entry,Direction)+Direction*Along;Emerging.Yaw=Direction.Rotation().Yaw;
				bool Clear=CanMoveVehicle(Emerging,Emerging.Location,Emerging.Yaw);
				for(const auto& Other:Staged)Clear&=!BodyBoxesOverlap(Emerging.Location,Emerging.Yaw,VehicleDimensions(Emerging),Other.Location,Other.Yaw,VehicleDimensions(Other),4.f);
				if(Clear){Emerging.RoadPath.Reset();Emerging.NextPathStep=0;Emerging.SegmentOriginCell=Entry;Emerging.bSegmentLaneAligned=false;Emerging.bParking=false;Emerging.DwellSeconds=0.f;Indices.Add(I);Staged.Add(Emerging);Found=true;break;}
			}
			if(Found)break;
		}
		if(!Found)return false;
	}
	for(int32 I=0;I<Indices.Num();++I)Vehicles[Indices[I]]=MoveTemp(Staged[I]);
	if(!Indices.IsEmpty()){if(Host)Host->DrawFleet(Vehicles);UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_GARAGE_EVICTED building=%d cars=%d clearRoadAdmission=1 IDsPreserved=1"),BuildingID,Indices.Num());}
	return true;
}

bool UTransportSubsystem::GetPassengerKerbLocation(int32 StopID,int32 Slot,bool bAlighting,FVector& OutLocation) const
{
	const UTerrainGridSubsystem* T=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const auto* Roads=GetWorld()?GetWorld()->GetSubsystem<URoadNetworkSubsystem>():nullptr;
	const FAftershockTransitStop* Stop=Stops.FindByPredicate([&](const FAftershockTransitStop& S){return S.BuildingID==StopID;});
	if(!T||!Roads||!Stop||!IsRoadSafe(Stop->RoadCell)||Slot<0)return false;
    if(T->CellSize>=900.f)
    {
        const auto* B=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();FBuildingInstance Building;
        FSidewalkPassengerPlaces Places;
        if(!B||!B->GetBuildingByID(StopID,Building)||!BuildConformingSidewalkPlaces(*T,*Roads,Stop->RoadCell,Building.GridCoords,Places))return false;
        if(bAlighting){if(!Places.Exits.IsValidIndex(Slot))return false;OutLocation=Places.Exits[Slot];}
        else{if(!Places.Waiting.IsValidIndex(Slot))return false;OutLocation=Places.Waiting[Slot];}
        return true;
    }
	const bool bWide=T->CellSize>=900.f;
	FVector Along=FVector::ForwardVector;
	const FGridCell Cell=T->GetCell(Stop->RoadCell);
	if(Cell.BridgeID!=INDEX_NONE)Along=Cell.BridgeAxis==1?FVector::ForwardVector:FVector::RightVector;
	else if(!IsRoadSafe(Stop->RoadCell+FIntPoint(1,0))&&!IsRoadSafe(Stop->RoadCell-FIntPoint(1,0)))Along=FVector::RightVector;
	FVector Across(-Along.Y,Along.X,0);
	FBuildingInstance Building;
	if(const auto* B=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>())
		if(B->GetBuildingByID(StopID,Building)&&FVector::DotProduct(T->GridToWorld(Building.GridCoords)-T->GridToWorld(Stop->RoadCell),Across)<0)Across*=-1.f;
	float Longitudinal=0.f,Lateral=0.f;
	if(bAlighting)
	{
		Longitudinal=T->CellSize*.36f;
		Lateral=T->CellSize*(bWide?.40f:.42f)+(bWide?(Slot%2?40.f:-40.f):0.f);
	}
	else
	{
		const int32 Columns=bWide?6:4,Rows=bWide?2:1;
		if(Slot>=Columns*Rows)return false;
		Longitudinal=(Slot%Columns-(Columns-1)*.5f)*80.f-(bWide?T->CellSize*.08f:0.f);
		Lateral=T->CellSize*(bWide?.40f:.42f)+(bWide?((Slot/Columns)*2-1)*40.f:0.f);
	}
	OutLocation=Roads->RoadGridToWorld(Stop->RoadCell)+Along*Longitudinal+Across*Lateral;
	OutLocation.Z=Roads->SampleRoadSurfaceElevation(OutLocation)+5.f;
	return T->WorldToGrid(OutLocation)==Stop->RoadCell&&Roads->IsPublicWalkingCell(Stop->RoadCell);
}
bool UTransportSubsystem::FindRoadNear(FIntPoint Start,FIntPoint& OutCoord,int32 Radius) const
{
	const UTerrainGridSubsystem* T=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;if(!T||!T->IsPlayableCoord(Start))return false;
	const auto* Roads=GetWorld()->GetSubsystem<URoadNetworkSubsystem>();if(!Roads)return false;
	TArray<FIntPoint> Q;TArray<int32>D;TSet<FIntPoint>S;Q.Add(Start);D.Add(0);S.Add(Start);
	for(int32 I=0;I<Q.Num();++I)
	{
		if(IsRoadSafe(Q[I])){OutCoord=Q[I];return true;}if(D[I]>=Radius)continue;
		for(FIntPoint O:Adjacent)
		{
			const FIntPoint N=Q[I]+O;if(!T->IsPlayableCoord(N)||S.Contains(N))continue;const FGridCell C=T->GetCell(N);
			if(IsRoadSafe(N)) {if(!Roads->CanAccessRoadFromCell(Q[I],N))continue;}
			else if(Q[I]==Start&&T->GetCell(Start).OccupyingBuildingID!=INDEX_NONE)
			{if(!Roads->IsPublicWalkingCell(N))continue;}
			else if(!Roads->CanTraverseWalkingEdge(Q[I],N))continue;
			S.Add(N);Q.Add(N);D.Add(D[I]+1);
		}
	}
	return false;
}
bool UTransportSubsystem::BuildRoadPath(FIntPoint Start,FIntPoint Goal,TArray<FIntPoint>& Out) const
{
	Out.Reset();const UTerrainGridSubsystem* T=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	if(!T||!IsRoadSafe(Start)||!IsRoadSafe(Goal))return false;
	const int32 ResidentCellCount=T->Cells.Num();
	TArray<FIntPoint> Q;Q.Add(Start);TMap<FIntPoint,FIntPoint> Parent;Parent.Add(Start,Start);
	for(int32 I=0;I<Q.Num()&&I<ResidentCellCount&&!Parent.Contains(Goal);++I)for(FIntPoint O:Adjacent)
	{const FIntPoint N=Q[I]+O;const auto* Roads=GetWorld()->GetSubsystem<URoadNetworkSubsystem>();if(!Parent.Contains(N)&&IsRoadSafe(N)&&Roads&&Roads->CanTraverseRoadEdge(Q[I],N)){Parent.Add(N,Q[I]);Q.Add(N);}}
	if(!Parent.Contains(Goal))return false;
	TArray<FIntPoint> Reverse;for(FIntPoint At=Goal;At!=Start;At=Parent.FindChecked(At))Reverse.Add(At);
	for(int32 I=Reverse.Num()-1;I>=0;--I)Out.Add(Reverse[I]);return true;
}

void UTransportSubsystem::RefreshMunicipalFleet(const TArray<FBuildingInstance>& Buildings)
{
	bMunicipalRebuildPending=false;
	const auto* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr;
	if(!Life||!Life->IsCityInitialized()){MunicipalFleet.Reset();MunicipalFleetByResident.Reset();return;}
	TMap<int32,const FBuildingInstance*> ByID;for(const auto& Building:Buildings)ByID.Add(Building.InstanceID,&Building);
	TMap<int32,int32> FirstStaffByBuilding;
	for(const auto& Resident:Life->GetResidents())if(Resident.bAlive&&Resident.bWorkingAge&&Resident.WorkBuildingID!=INDEX_NONE)
	{int32* Existing=FirstStaffByBuilding.Find(Resident.WorkBuildingID);if(!Existing||Resident.ResidentID<*Existing)FirstStaffByBuilding.Add(Resident.WorkBuildingID,Resident.ResidentID);}
	MunicipalFleet.RemoveAll([&](const auto& Unit)
	{
		const auto* Building=ByID.FindRef(Unit.OwnerBuildingID);FCityResidentRecord Staff;
		return !Building||!Building->BuildingData||Building->DamageState>=EDamageState::Damaged
			||!Life->GetResident(Unit.ResidentID,Staff)||!Staff.bAlive||Staff.WorkBuildingID!=Unit.OwnerBuildingID;
	});
	TMap<int32,int32> ExistingByBuilding;
	for(int32 I=0;I<MunicipalFleet.Num();++I)ExistingByBuilding.Add(MunicipalFleet[I].OwnerBuildingID,I);
	for(const auto& Building:Buildings)
	{
		if(!Building.BuildingData||Building.DamageState>=EDamageState::Damaged)continue;
		const auto MunicipalService=MunicipalRole(Building.BuildingData->Type);if(MunicipalService==EAftershockMunicipalService::None)continue;
		const auto Status=Life->GetBuildingStatus(Building.InstanceID);FIntPoint Entry;
		if(!Status.bOperating||Status.AssignedWorkers<1||!FindRoadNear(Building.GridCoords,Entry,1))continue;
		if(const int32* ExistingIndex=ExistingByBuilding.Find(Building.InstanceID))
		{
			auto* Existing=&MunicipalFleet[*ExistingIndex];
			Existing->HomeRoadCell=Entry;
			if(!Existing->bDeployed){Existing->Vehicle.Location=RoadLocation(Entry);Existing->Vehicle.SegmentOriginCell=Entry;}
			continue;
		}
		if(MunicipalFleet.Num()>=MaxMunicipalUnits)break;
		const int32* Staff=FirstStaffByBuilding.Find(Building.InstanceID);if(!Staff)continue;
		FAftershockMunicipalFleetUnit Unit;Unit.OwnerBuildingID=Building.InstanceID;Unit.ResidentID=*Staff;Unit.Service=MunicipalService;Unit.HomeRoadCell=Entry;
		// Negative IDs are reserved for derived display units and never enter the
		// frozen passenger/vehicle snapshot, fares or private ownership ledger.
		Unit.Vehicle.VehicleID=-1000-Building.InstanceID;Unit.Vehicle.ResidentOwnerID=Unit.ResidentID;
		Unit.Vehicle.ParkedAtBuildingID=Building.InstanceID;Unit.Vehicle.Location=RoadLocation(Entry);Unit.Vehicle.SegmentOriginCell=Entry;
		Unit.CooldownSeconds=static_cast<float>(Building.InstanceID%7)*.8f;MunicipalFleet.Add(MoveTemp(Unit));
	}
	MunicipalFleetByResident.Reset();for(int32 I=0;I<MunicipalFleet.Num();++I)MunicipalFleetByResident.Add(MunicipalFleet[I].ResidentID,I);
}

bool UTransportSubsystem::SelectMunicipalRoute(FAftershockMunicipalFleetUnit& Unit,TArray<FIntPoint>& OutRoute,FIntPoint& OutIncident) const
{
	OutRoute.Reset();const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const auto* Roads=GetWorld()?GetWorld()->GetSubsystem<URoadNetworkSubsystem>():nullptr;
	const auto* Buildings=GetWorld()?GetWorld()->GetSubsystem<UBuildingManagerSubsystem>():nullptr;
	const auto* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr;
	if(!Terrain||!Roads||!Buildings||!Life||!IsRoadSafe(Unit.HomeRoadCell))return false;
	TMap<FIntPoint,FIntPoint> Targets;
	const auto AddFrontages=[&](FIntPoint Incident)
	{for(const FIntPoint Offset:Adjacent){const FIntPoint Road=Incident+Offset;if(IsRoadSafe(Road)&&Road!=Unit.HomeRoadCell)Targets.FindOrAdd(Road,Incident);}};
	if(Unit.Service==EAftershockMunicipalService::FireResponse)
	{
		for(const auto& Cell:Terrain->Cells)if(Cell.bBurning)AddFrontages(Cell.Coords);
	}
	else
	{
		TSet<int32> SickHomes;if(Unit.Service==EAftershockMunicipalService::Ambulance)
			for(const auto& Resident:Life->GetResidents())if(Resident.bAlive&&Resident.Health01<.5f)SickHomes.Add(Resident.HomeBuildingID);
		TArray<FBuildingInstance> Records;Buildings->GetAllBuildings(Records);
		Records.Sort([](const auto& A,const auto& B){return A.InstanceID<B.InstanceID;});
		// A changing, deterministic patrol sector prevents every unit choosing
		// the same nearest house. It does not invent crimes, patients or fares.
		const int32 Sector=static_cast<int32>(HistoryClockSeconds/30.f)+Unit.OwnerBuildingID;
		for(int32 I=0;I<Records.Num();++I)
		{
			const auto& B=Records[I];if(!B.BuildingData||B.InstanceID==Unit.OwnerBuildingID||B.DamageState==EDamageState::Memorial)continue;
			const auto Type=B.BuildingData->Type;bool Target=false;
			if(Unit.Service==EAftershockMunicipalService::Ambulance)Target=SickHomes.Contains(B.InstanceID)||(bEmergencyActive&&B.CurrentDamage>10.f);
			else if(Unit.Service==EAftershockMunicipalService::PolicePatrol)Target=(Type==EBuildingType::House||Type==EBuildingType::Apartment||Type==EBuildingType::Office)&&((I+Sector)%5==0);
			else if(Unit.Service==EAftershockMunicipalService::CourtTransport)Target=Type==EBuildingType::PoliceStation||Type==EBuildingType::TaxOffice;
			else if(Unit.Service==EAftershockMunicipalService::MunicipalUtility)Target=(MunicipalRole(Type)==EAftershockMunicipalService::MunicipalUtility||Type==EBuildingType::Warehouse)&&((I+Sector)%3==0);
			if(Target)AddFrontages(B.GridCoords);
		}
	}
	if(Targets.IsEmpty())return false;
	TArray<FIntPoint> Queue;TMap<FIntPoint,FIntPoint> Parent;Queue.Add(Unit.HomeRoadCell);Parent.Add(Unit.HomeRoadCell,Unit.HomeRoadCell);
	FIntPoint Goal(MAX_int32,MAX_int32);constexpr int32 MaximumRouteSearchCells=4096;
	for(int32 Read=0;Read<Queue.Num()&&Read<MaximumRouteSearchCells;++Read)
	{
		const FIntPoint Current=Queue[Read];
		if(const auto* Incident=Targets.Find(Current)){Goal=Current;OutIncident=*Incident;break;}
		for(const FIntPoint Offset:Adjacent)
		{const FIntPoint Next=Current+Offset;if(Parent.Num()>=MaximumRouteSearchCells||Parent.Contains(Next)||!IsRoadSafe(Next)||!Roads->CanTraverseRoadEdge(Current,Next))continue;Parent.Add(Next,Current);Queue.Add(Next);}
	}
	if(Goal.X==MAX_int32)return false;
	TArray<FIntPoint> Reverse;for(FIntPoint At=Goal;At!=Unit.HomeRoadCell;At=Parent.FindChecked(At))Reverse.Add(At);
	if(Reverse.IsEmpty())return false;
	for(int32 I=Reverse.Num()-1;I>=0;--I)OutRoute.Add(Reverse[I]);
	Unit.OnScenePathStep=OutRoute.Num();
	// The same actual road edges in reverse make a connected return circuit.
	for(int32 I=1;I<Reverse.Num();++I)OutRoute.Add(Reverse[I]);
	OutRoute.Add(Unit.HomeRoadCell);return true;
}

bool UTransportSubsystem::TryBeginServiceDuty(int32 ResidentID,int32 WorkBuildingID)
{
	if(ResidentID<1||WorkBuildingID<1||!GetWorld())return false;
	const int32* UnitIndex=MunicipalFleetByResident.Find(ResidentID);auto* Unit=UnitIndex&&MunicipalFleet.IsValidIndex(*UnitIndex)?&MunicipalFleet[*UnitIndex]:nullptr;
	if(!Unit||Unit->OwnerBuildingID!=WorkBuildingID||Unit->bDeployed||Unit->CooldownSeconds>0.f)return false;
	if(GetMunicipalDeployedCount()>=MaxMunicipalFleet)return false;
	const auto* Life=GetWorld()->GetSubsystem<UCityLifeSubsystem>();FCityResidentRecord Resident;
	if(!Life||!Life->GetResident(ResidentID,Resident)||!Resident.bAlive||Resident.WorkBuildingID!=WorkBuildingID||!Life->GetBuildingStatus(WorkBuildingID).bOperating)return false;
	if(!TryAcquireMunicipalQueryBudget())return false;
	Unit->CooldownSeconds=8.f+static_cast<float>(WorkBuildingID%7);
	TArray<FIntPoint> Route;FIntPoint Incident;if(!SelectMunicipalRoute(*Unit,Route,Incident))return false;
	const auto* Terrain=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
	const FVector Direction=(Terrain->GridToWorld(Route[0])-Terrain->GridToWorld(Unit->HomeRoadCell)).GetSafeNormal2D();
	FAftershockRoadVehicle Proposed=Unit->Vehicle;Proposed.ParkedAtBuildingID=INDEX_NONE;Proposed.Location=RoadLocation(Unit->HomeRoadCell,Direction);Proposed.Yaw=Direction.Rotation().Yaw;
	if(!CanMoveVehicle(Proposed,Proposed.Location,Proposed.Yaw))return false;
	Unit->Vehicle=MoveTemp(Proposed);Unit->Vehicle.RoadPath=MoveTemp(Route);Unit->Vehicle.NextPathStep=0;Unit->Vehicle.bSegmentLaneAligned=false;Unit->Vehicle.bStalled=false;
	Unit->Vehicle.SegmentOriginCell=Unit->HomeRoadCell;Unit->bDeployed=true;Unit->bReturning=false;Unit->bCancellationRequested=false;Unit->IncidentCell=Incident;Unit->OnSceneSeconds=0.f;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MUNICIPAL_DISPATCH building=%d resident=%d service=%d routeSteps=%d populationAdded=0 outcomeWrites=0"),WorkBuildingID,ResidentID,static_cast<int32>(Unit->Service),Unit->Vehicle.RoadPath.Num());
	return true;
}

bool UTransportSubsystem::GetMunicipalDuty(int32 ResidentID,FAftershockMunicipalDutyView& OutView) const
{
	const int32* UnitIndex=MunicipalFleetByResident.Find(ResidentID);const auto* Unit=UnitIndex&&MunicipalFleet.IsValidIndex(*UnitIndex)?&MunicipalFleet[*UnitIndex]:nullptr;
	if(!Unit||!Unit->bDeployed)return false;
	OutView.OwnerBuildingID=Unit->OwnerBuildingID;OutView.ResidentID=ResidentID;OutView.Service=Unit->Service;
	OutView.VehicleLocation=Unit->Vehicle.Location;OutView.bRiding=true;OutView.bOnScene=Unit->OnSceneSeconds>0.f;return true;
}

bool UTransportSubsystem::IsMunicipalStaffResident(int32 ResidentID,int32 WorkBuildingID) const
{
	const int32* Index=MunicipalFleetByResident.Find(ResidentID);if(!Index||!MunicipalFleet.IsValidIndex(*Index))return false;
	const auto& Unit=MunicipalFleet[*Index];if(Unit.OwnerBuildingID!=WorkBuildingID||Unit.Service==EAftershockMunicipalService::CourtTransport)return false;
	const auto* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr;FCityResidentRecord Resident;
	return Life&&Life->GetResident(ResidentID,Resident)&&Resident.bAlive&&Resident.WorkBuildingID==WorkBuildingID;
}

void UTransportSubsystem::GetMunicipalStaffResidentIDs(TArray<int32>& OutResidentIDs) const
{
	OutResidentIDs.Reset();const auto* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr;
	const auto* Buildings=GetWorld()?GetWorld()->GetSubsystem<UBuildingManagerSubsystem>():nullptr;
	if(!Life||!Life->IsCityInitialized()||!Buildings)return;
	TSet<int32> SelectedBuildings,SelectedResidents;
	// Retain already dispatched representatives first; a city expansion must
	// never drop their display identity midway through a real return circuit.
	for(int32 Pass=0;Pass<2;++Pass)for(const auto& Unit:MunicipalFleet)
	{
		if(Unit.bDeployed!=(Pass==0)||OutResidentIDs.Num()>=MaxMunicipalUnits)continue;FCityResidentRecord Resident;
		if(!Life->GetResident(Unit.ResidentID,Resident)||!Resident.bAlive||Resident.WorkBuildingID!=Unit.OwnerBuildingID)continue;
		if(SelectedResidents.Contains(Resident.ResidentID))continue;
		OutResidentIDs.Add(Resident.ResidentID);SelectedResidents.Add(Resident.ResidentID);SelectedBuildings.Add(Unit.OwnerBuildingID);
	}
	TArray<FBuildingInstance> Records;Buildings->GetAllBuildings(Records);TSet<int32> EligibleBuildings;
	for(const auto& Building:Records)if(Building.BuildingData&&Building.DamageState<EDamageState::Damaged&&
		MunicipalRole(Building.BuildingData->Type)!=EAftershockMunicipalService::None&&!SelectedBuildings.Contains(Building.InstanceID)&&Life->GetBuildingStatus(Building.InstanceID).bOperating)EligibleBuildings.Add(Building.InstanceID);
	TMap<int32,int32> FirstStaff;
	// One canonical population pass, never one full resident scan per facility.
	for(const auto& Resident:Life->GetResidents())if(Resident.bAlive&&Resident.bWorkingAge&&EligibleBuildings.Contains(Resident.WorkBuildingID))
	{int32* Existing=FirstStaff.Find(Resident.WorkBuildingID);if(!Existing||Resident.ResidentID<*Existing)FirstStaff.Add(Resident.WorkBuildingID,Resident.ResidentID);}
	TArray<int32> Candidates;FirstStaff.GenerateValueArray(Candidates);Candidates.Sort();
	for(int32 ResidentID:Candidates)if(OutResidentIDs.Num()<MaxMunicipalUnits&&!SelectedResidents.Contains(ResidentID))
	{OutResidentIDs.Add(ResidentID);SelectedResidents.Add(ResidentID);}
}

void UTransportSubsystem::CancelMunicipalDutyForResident(int32 ResidentID)
{
	const int32* Index=MunicipalFleetByResident.Find(ResidentID);if(!Index||!MunicipalFleet.IsValidIndex(*Index))return;
	auto& Unit=MunicipalFleet[*Index];if(!Unit.bDeployed)return;
	const auto* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr;FCityResidentRecord Resident;
	if(!Life||!Life->GetResident(ResidentID,Resident)||!Resident.bAlive||Resident.WorkBuildingID!=Unit.OwnerBuildingID)
	{
		MunicipalFleet.RemoveAt(*Index);MunicipalFleetByResident.Reset();
		for(int32 I=0;I<MunicipalFleet.Num();++I)MunicipalFleetByResident.Add(MunicipalFleet[I].ResidentID,I);
		return;
	}
	if(Unit.bCancellationRequested)return;
	// The request is idempotent. The transport callback consumes its shared
	// route budget once instead of Crowd restarting the return every 80 ms.
	Unit.bCancellationRequested=true;Unit.OnSceneSeconds=0.f;Unit.CooldownSeconds=0.f;
}

void UTransportSubsystem::ResetMunicipalPresentationAfterRestore()
{
	// Presentation dispatch never enters the frozen V4/V5 descriptor or changes
	// the imported treasury, citizen anchor, private fleet, fare or timer state.
	MunicipalFleet.Reset();MunicipalFleetByResident.Reset();MunicipalRenderRoles.Reset();FleetRenderScratch.Reset();bMunicipalRebuildPending=true;
}

bool UTransportSubsystem::TryAcquireMunicipalQueryBudget()
{
	if(MunicipalQueryFrame!=GFrameCounter){MunicipalQueryFrame=GFrameCounter;MunicipalQueriesThisFrame=0;}
	if(MunicipalQueriesThisFrame>=2)return false;
	++MunicipalQueriesThisFrame;return true;
}

int32 UTransportSubsystem::GetMunicipalDeployedCount() const
{int32 Count=0;for(const auto& Unit:MunicipalFleet)Count+=Unit.bDeployed?1:0;return Count;}

void UTransportSubsystem::UpdateMunicipalFleet()
{
	const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const auto* Roads=GetWorld()?GetWorld()->GetSubsystem<URoadNetworkSubsystem>():nullptr;if(!Terrain||!Roads)return;
	for(auto& Unit:MunicipalFleet)
	{
		Unit.CooldownSeconds=FMath::Max(0.f,Unit.CooldownSeconds-TransportStep);if(!Unit.bDeployed)continue;
		auto& V=Unit.Vehicle;
		if(Unit.bCancellationRequested&&!Unit.bReturning)
		{
			const FIntPoint Actual=Terrain->WorldToGrid(V.Location);
			if(Actual==Unit.HomeRoadCell){V.RoadPath.Empty();V.NextPathStep=0;Unit.bReturning=true;}
			else
			{
				if(Unit.CooldownSeconds>0.f||!IsRoadSafe(Actual)||!TryAcquireMunicipalQueryBudget())continue;
				Unit.CooldownSeconds=3.f;TArray<FIntPoint> Return;
				if(BuildRoadPath(Actual,Unit.HomeRoadCell,Return)&&!Return.IsEmpty())
				{V.RoadPath=MoveTemp(Return);V.NextPathStep=0;V.SegmentOriginCell=Actual;V.bSegmentLaneAligned=false;Unit.bReturning=true;}
				else continue;
			}
		}
		if(Unit.OnSceneSeconds>0.f){Unit.OnSceneSeconds=FMath::Max(0.f,Unit.OnSceneSeconds-TransportStep);continue;}
		if(!Unit.bReturning&&V.NextPathStep==Unit.OnScenePathStep)
		{Unit.bReturning=true;Unit.OnSceneSeconds=Unit.Service==EAftershockMunicipalService::FireResponse?8.f:3.f;continue;}
		if(!V.RoadPath.IsValidIndex(V.NextPathStep))
		{
			if(Terrain->WorldToGrid(V.Location)==Unit.HomeRoadCell)
			{V.ParkedAtBuildingID=Unit.OwnerBuildingID;V.RoadPath.Empty();V.NextPathStep=0;Unit.bDeployed=false;Unit.CooldownSeconds=15.f+Unit.OwnerBuildingID%11;}
			else if(Unit.CooldownSeconds<=0.f)
			{
				if(!TryAcquireMunicipalQueryBudget())continue;
				Unit.CooldownSeconds=3.f;const FIntPoint Actual=Terrain->WorldToGrid(V.Location);TArray<FIntPoint> Return;
				if(IsRoadSafe(Actual)&&BuildRoadPath(Actual,Unit.HomeRoadCell,Return)&&!Return.IsEmpty())
				{V.RoadPath=MoveTemp(Return);V.NextPathStep=0;V.SegmentOriginCell=Actual;V.bSegmentLaneAligned=false;Unit.bReturning=true;}
			}
			continue;
		}
		const FIntPoint Current=Terrain->WorldToGrid(V.Location),Next=V.RoadPath[V.NextPathStep];
		if(!IsRoadSafe(Current)||!IsRoadSafe(Next)||!Roads->CanTraverseRoadEdge(V.SegmentOriginCell,Next))
		{
			// A changed hazard never teleports this vehicle through the blockage.
			// Replan only from its actual position, keeping its real crew reserved.
			if(Unit.CooldownSeconds<=0.f&&IsRoadSafe(Current))
			{
				if(!TryAcquireMunicipalQueryBudget())continue;
				Unit.CooldownSeconds=3.f;TArray<FIntPoint> Return;
				if(BuildRoadPath(Current,Unit.HomeRoadCell,Return)&&!Return.IsEmpty())
				{V.RoadPath=MoveTemp(Return);V.NextPathStep=0;V.SegmentOriginCell=Current;V.bSegmentLaneAligned=false;Unit.bReturning=true;}
			}
			continue;
		}
		const FVector Direction=(Terrain->GridToWorld(Next)-Terrain->GridToWorld(V.SegmentOriginCell)).GetSafeNormal2D();
		if(!V.bSegmentLaneAligned)
		{
			const FVector Lane=RoadLocation(V.SegmentOriginCell,Direction);
			if(!TryMoveVehicle(V,FMath::VInterpConstantTo(V.Location,Lane,TransportStep,350.f),FMath::FixedTurn(V.Yaw,Direction.Rotation().Yaw,180.f*TransportStep)))continue;
			V.bSegmentLaneAligned=FVector::DistSquared2D(V.Location,Lane)<1.f;if(!V.bSegmentLaneAligned)continue;
		}
		const FVector Goal=RoadLocation(Next,Direction);const float Distance=FVector::Dist2D(V.Location,Goal);
		const float Step=FMath::Min(Distance,500.f*TransportStep);FVector Candidate=V.Location+(Goal-V.Location).GetSafeNormal2D()*Step;
		Candidate.Z=Roads->SampleRoadSurfaceElevation(Candidate)+3.f;
		if(!TryMoveVehicle(V,Candidate,V.Yaw))continue;
		if(Distance<=Step+.01f){V.SegmentOriginCell=Next;++V.NextPathStep;V.bSegmentLaneAligned=false;}
	}
}

void UTransportSubsystem::RefreshInfrastructure()
{
    FAftershockRoadCpuScope Cpu(TEXT("Transport.RefreshInfrastructure"));
	if(!GetWorld())return;const UTerrainGridSubsystem* T=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
	UBuildingManagerSubsystem* B=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();const UCityLifeSubsystem* L=GetWorld()->GetSubsystem<UCityLifeSubsystem>();if(!T||!B)return;
	Stops.Reset();TArray<FBuildingInstance> Records;B->GetAllBuildings(Records);
	Records.Sort([](const FBuildingInstance&A,const FBuildingInstance&B){return A.InstanceID<B.InstanceID;});
	RefreshMunicipalFleet(Records);
	for(const FBuildingInstance& R:Records)
	{
		if(!R.BuildingData||R.DamageState>=EDamageState::Damaged||(R.BuildingData->Type!=EBuildingType::BusStop&&R.BuildingData->Type!=EBuildingType::BusTerminal))continue;
		if(L&&L->IsCityInitialized()&&!L->GetBuildingStatus(R.InstanceID).bOperating)continue;
		FIntPoint Road;if(!FindRoadNear(R.GridCoords,Road,2))continue;
		FAftershockTransitStop S;S.BuildingID=R.InstanceID;S.RoadCell=Road;S.bTerminal=R.BuildingData->Type==EBuildingType::BusTerminal;
		const float OperatingFraction=L&&L->IsCityInitialized()?L->GetBuildingStatus(R.InstanceID).Operation01:1.f;
		S.Capacity=S.bTerminal?FMath::Clamp(FMath::FloorToInt(R.BuildingData->TransitCapacity*OperatingFraction),0,40):FMath::Clamp(R.BuildingData->TransitCapacity,0,T->CellSize>=900.f?12:4);
        if(!S.bTerminal&&T->CellSize>=900.f)
        {
            const auto* Roads=GetWorld()->GetSubsystem<URoadNetworkSubsystem>();
            FSidewalkPassengerPlaces Best;FIntPoint Selected=Road;bool Found=false;
            TArray<FIntPoint,TInlineAllocator<5>> Candidates;Candidates.Add(Road);
            for(FIntPoint Delta:Adjacent)Candidates.AddUnique(R.GridCoords+Delta);
            for(FIntPoint Candidate:Candidates)
            {
                // A stop may use a different REAL adjacent frontage, never a
                // remote/disconnected road or a teleported passenger position.
                if(!Roads||FMath::Abs(Candidate.X-R.GridCoords.X)+FMath::Abs(Candidate.Y-R.GridCoords.Y)!=1||
                    !IsRoadSafe(Candidate)||!Roads->CanAccessRoadFromCell(R.GridCoords,Candidate))continue;
                TArray<FIntPoint> Connection;if(Candidate!=Road&&!BuildRoadPath(Road,Candidate,Connection))continue;
                FSidewalkPassengerPlaces Trial;
                if(!BuildConformingSidewalkPlaces(*T,*Roads,Candidate,R.GridCoords,Trial))continue;
                if(!Found||Trial.Waiting.Num()>Best.Waiting.Num()){Found=true;Selected=Candidate;Best=MoveTemp(Trial);}
            }
            if(!Found){UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_TRANSIT_FRONTAGE_UNSUPPORTED building=%d capacity=0 actualSidewalkRequired=1"),R.InstanceID);continue;}
            S.RoadCell=Selected;S.Capacity=FMath::Min(S.Capacity,Best.Waiting.Num());
            if(S.Capacity<=0)continue;
        }
        Stops.Add(S);
	}
	for(FAftershockRoadVehicle& V:Vehicles)
	{
		if(!V.bBus)
		{
			// A stopped private car remains at its real position. Once its road is
			// available again its owner may walk back and plan a new, connected trip.
			// Never clear a pickup reservation or resume the cancelled old route.
			if(V.bActive&&V.bStalled&&V.PassengerTripIDs.IsEmpty()&&IsRoadSafe(T->WorldToGrid(V.Location)))
			{V.bStalled=false;V.RoadPath.Reset();V.NextPathStep=0;V.DwellSeconds=0.f;V.bParking=false;V.bSegmentLaneAligned=false;}
			continue;
		}
		const FAftershockTransitStop* Terminal=Stops.FindByPredicate([&V](const FAftershockTransitStop&S){return S.bTerminal&&S.BuildingID==V.TerminalBuildingID;});
		if(!Terminal){StrandPassengers(V);V.bActive=false;continue;}
		V.ServiceStopIDs.Reset();TArray<FIntPoint> P;
		for(const FAftershockTransitStop&S:Stops)if(!S.bTerminal&&BuildRoadPath(Terminal->RoadCell,S.RoadCell,P))V.ServiceStopIDs.Add(S.BuildingID);
		V.Capacity=FMath::Clamp(Terminal->Capacity,1,40);
		while(V.PassengerTripIDs.Num()>V.Capacity)
		{
			const int32 ID=V.PassengerTripIDs.Pop(EAllowShrinking::No);
			V.PendingAlightTripIDs.Remove(ID);
			if(FAftershockPassengerTrip* ExcessPassenger=Trips.Find(ID)){ExcessPassenger->State=EAftershockPassengerState::Stranded;ExcessPassenger->CurrentLocation=V.Location;}
		}
		if(V.ServiceStopIDs.Num()<2||Terminal->Capacity<=0){StrandPassengers(V);V.bStalled=true;continue;}
		V.NextServiceStop=FMath::Clamp(V.NextServiceStop,0,V.ServiceStopIDs.Num()-1);
		V.PendingAlightTripIDs.RemoveAll([&](int32 ID)
		{
			const FAftershockPassengerTrip* Passenger=Trips.Find(ID);
			return !Passenger||!V.PassengerTripIDs.Contains(ID)||Passenger->State!=EAftershockPassengerState::Riding
				||Passenger->DropoffStopID!=V.ServiceStopIDs[V.NextServiceStop];
		});
		if(V.PendingAlightTripIDs.IsEmpty())V.AlightReleaseSeconds=0.f;
		else continue; // A service refresh cannot dispatch a bus through its unloading queue.
		const FAftershockTransitStop* Target=Stops.FindByPredicate([&](const FAftershockTransitStop&S){return S.BuildingID==V.ServiceStopIDs[V.NextServiceStop];});
		V.RoadPath.Reset();V.NextPathStep=0;
		V.bSegmentLaneAligned=false;
		V.SegmentOriginCell=T->WorldToGrid(V.Location);
		V.bStalled=!Target||!BuildRoadPath(T->WorldToGrid(V.Location),Target->RoadCell,V.RoadPath);
		if(V.bStalled)StrandPassengers(V);
	}
	PruneInactiveBuses();
	// A retired bus still owns its slot while a real passenger waits for a
	// finite curb handoff. Admission applies backpressure instead of appending
	// an unbounded graveyard of disabled bus records after terminal demolition.
	int32 BusCount=0;for(const FAftershockRoadVehicle&V:Vehicles)BusCount+=V.bBus?1:0;
	for(const FAftershockTransitStop& Terminal:Stops)
	{
		if(!Terminal.bTerminal||Terminal.Capacity<=0||BusCount>=MaxBuses)continue;
		if(Vehicles.ContainsByPredicate([&](const FAftershockRoadVehicle&V){return V.bBus&&V.bActive&&V.TerminalBuildingID==Terminal.BuildingID;}))continue;
		FAftershockRoadVehicle Bus;Bus.bBus=true;Bus.VehicleID=NextVehicleID++;Bus.TerminalBuildingID=Terminal.BuildingID;Bus.Capacity=FMath::Clamp(Terminal.Capacity,1,40);Bus.Location=RoadLocation(Terminal.RoadCell);
		TArray<FIntPoint> P;for(const FAftershockTransitStop&S:Stops)if(!S.bTerminal&&BuildRoadPath(Terminal.RoadCell,S.RoadCell,P))Bus.ServiceStopIDs.Add(S.BuildingID);
		if(Bus.ServiceStopIDs.Num()<2)continue;
		const FAftershockTransitStop* First=Stops.FindByPredicate([&](const FAftershockTransitStop&S){return S.BuildingID==Bus.ServiceStopIDs[0];});
		if(!First||!BuildRoadPath(Terminal.RoadCell,First->RoadCell,Bus.RoadPath))continue;
		const FVector Departure=Bus.RoadPath.IsEmpty()?FVector::ForwardVector
			:(T->GridToWorld(Bus.RoadPath[0])-T->GridToWorld(Terminal.RoadCell)).GetSafeNormal2D();
		Bus.Location=RoadLocation(Terminal.RoadCell,Departure);Bus.Yaw=Departure.Rotation().Yaw;
		Bus.SegmentOriginCell=Terminal.RoadCell;
		if(!CanMoveVehicle(Bus,Bus.Location,Bus.Yaw))continue;
		Vehicles.Add(Bus);++BusCount;
	}
	TArray<int32> Invalid;
	for(const TPair<int32,FAftershockPassengerTrip>&Pair:Trips)
	{
		const FAftershockPassengerTrip& P=Pair.Value;if(!ActiveTrip(P.State))continue;
		FBuildingInstance Destination;FCityResidentRecord Resident;
		const bool bPersonValid=!L||!L->IsCityInitialized()||(L->GetResident(P.ResidentID,Resident)&&Resident.bAlive);
		const bool bDestinationValid=P.DestinationBuildingID==INDEX_NONE||B->GetBuildingByID(P.DestinationBuildingID,Destination);
		const bool bStopsValid=P.Mode!=EAftershockTravelMode::Bus||(Stops.ContainsByPredicate([&](const FAftershockTransitStop&S){return S.BuildingID==P.PickupStopID;})&&Stops.ContainsByPredicate([&](const FAftershockTransitStop&S){return S.BuildingID==P.DropoffStopID;}));
		const bool bBusAvailable=P.Mode!=EAftershockTravelMode::Bus||Vehicles.ContainsByPredicate([&](const FAftershockRoadVehicle& V){return V.bBus&&V.bActive&&!V.bStalled&&V.ServiceStopIDs.Contains(P.PickupStopID)&&V.ServiceStopIDs.Contains(P.DropoffStopID);});
        bool PickupGeometryValid=true;
        if(P.Mode==EAftershockTravelMode::Bus&&T->CellSize>=900.f&&
            (P.State==EAftershockPassengerState::WalkingToPickup||P.State==EAftershockPassengerState::Waiting))
        {
            FVector CurrentSlot=FVector::ZeroVector;
            PickupGeometryValid=GetPassengerKerbLocation(P.PickupStopID,P.PickupQueueSlot,false,CurrentSlot)&&
                FVector::DistSquared2D(CurrentSlot,P.PickupLocation)<=1.&&T->WorldToGrid(CurrentSlot)==P.PickupRoadCell;
            if(P.State==EAftershockPassengerState::Waiting&&PickupGeometryValid)PickupGeometryValid=FVector::DistSquared2D(CurrentSlot,P.CurrentLocation)<=1.;
        }
        if(!bPersonValid||!bDestinationValid||!bStopsValid||!bBusAvailable||!PickupGeometryValid)Invalid.Add(Pair.Key);
	}
	for(int32 ID:Invalid)CancelTrip(ID);bInfrastructureDirty=false;
}

bool UTransportSubsystem::PlanTrip(int32 ResidentID,FVector Origin,FVector Destination,int32 DestinationBuildingID,bool bAllowPrivateCar,FAftershockPassengerTrip& Out)
{
	if(bEmergencyActive||!GetWorld()||ResidentID==INDEX_NONE)return false;if(bInfrastructureDirty)RefreshInfrastructure();
	const UTerrainGridSubsystem* T=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();const UCityLifeSubsystem* L=GetWorld()->GetSubsystem<UCityLifeSubsystem>();
	FCityResidentRecord Resident;if(L&&L->IsCityInitialized()&&(!L->GetResident(ResidentID,Resident)||!Resident.bAlive))return false;
	if(DestinationBuildingID!=INDEX_NONE)
	{
		const UBuildingManagerSubsystem* Buildings=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();FBuildingInstance Building;
		if(!Buildings||!Buildings->GetBuildingByID(DestinationBuildingID,Building)||Building.DamageState>=EDamageState::Damaged)return false;
	}
	for(const TPair<int32,FAftershockPassengerTrip>&P:Trips)if(P.Value.ResidentID==ResidentID&&ActiveTrip(P.Value.State))return false;
	FIntPoint From,To;if(!T||!FindRoadNear(T->WorldToGrid(Origin),From)||!FindRoadNear(T->WorldToGrid(Destination),To))return false;
	if(FMath::Abs(From.X-To.X)+FMath::Abs(From.Y-To.Y)<4)return false;
	TArray<FIntPoint> Path;if(!BuildRoadPath(From,To,Path))return false;
	FAftershockPassengerTrip Trip;Trip.TripID=NextTripID++;Trip.ResidentID=ResidentID;Trip.DestinationBuildingID=DestinationBuildingID;Trip.CurrentLocation=Origin;
	// Prefer a real stop pair in one terminal's service, with actual waiting capacity.
	int32 BestScore=MAX_int32;const FAftershockTransitStop* Pickup=nullptr;const FAftershockTransitStop* Dropoff=nullptr;
	for(const FAftershockRoadVehicle& Bus:Vehicles)
	{
		if(!Bus.bBus||!Bus.bActive||Bus.bStalled)continue;
		for(const FAftershockTransitStop& A:Stops)for(const FAftershockTransitStop& B:Stops)
		{
			if(A.bTerminal||B.bTerminal||A.BuildingID==B.BuildingID||!Bus.ServiceStopIDs.Contains(A.BuildingID)||!Bus.ServiceStopIDs.Contains(B.BuildingID))continue;
			const int32 StartWalk=FMath::Abs(A.RoadCell.X-From.X)+FMath::Abs(A.RoadCell.Y-From.Y),EndWalk=FMath::Abs(B.RoadCell.X-To.X)+FMath::Abs(B.RoadCell.Y-To.Y);
			if(StartWalk>4||EndWalk>4)continue;int32 Waiting=0;
			for(const TPair<int32,FAftershockPassengerTrip>&P:Trips)Waiting+=P.Value.PickupStopID==A.BuildingID&&(P.Value.State==EAftershockPassengerState::WalkingToPickup||P.Value.State==EAftershockPassengerState::Waiting)?1:0;
			if(A.Capacity<=Waiting)continue;
			if(StartWalk+EndWalk<BestScore){BestScore=StartWalk+EndWalk;Pickup=&A;Dropoff=&B;}
		}
	}
	if(Pickup&&Dropoff)
	{
		Trip.Mode=EAftershockTravelMode::Bus;Trip.PickupStopID=Pickup->BuildingID;Trip.DropoffStopID=Dropoff->BuildingID;
		Trip.PickupRoadCell=Pickup->RoadCell;Trip.DropoffRoadCell=Dropoff->RoadCell;
		TSet<int32> ReservedSlots;
		for(const TPair<int32,FAftershockPassengerTrip>& P:Trips)
			if(P.Value.PickupStopID==Pickup->BuildingID&&(P.Value.State==EAftershockPassengerState::WalkingToPickup||P.Value.State==EAftershockPassengerState::Waiting))ReservedSlots.Add(P.Value.PickupQueueSlot);
		for(int32 Slot=0;Slot<Pickup->Capacity;++Slot)if(!ReservedSlots.Contains(Slot)){Trip.PickupQueueSlot=Slot;break;}

	}
	else
	{
		if(!bAllowPrivateCar)return false;
		if(!HasOffstreetParkingSpace(DestinationBuildingID,ResidentID))return false;
		FAftershockRoadVehicle* Car=Vehicles.FindByPredicate([&](const FAftershockRoadVehicle&V){return !V.bBus&&V.bActive&&V.ResidentOwnerID==ResidentID;});
		if(Car)
		{
			if(!Car->PassengerTripIDs.IsEmpty()||Car->bStalled||T->WorldToGrid(Car->Location)!=From)return false;
			if(Car->ParkedAtBuildingID!=INDEX_NONE)
			{
				FIntPoint Entry;FBuildingInstance Garage;
				const auto* Buildings=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();
				if(!Buildings||!Buildings->GetBuildingByID(Car->ParkedAtBuildingID,Garage)||!FindRoadNear(Garage.GridCoords,Entry)||Entry!=From)return false;
				const FVector Departure=(T->GridToWorld(Path[0])-T->GridToWorld(From)).GetSafeNormal2D();
				FAftershockRoadVehicle Emerging=*Car;Emerging.Location=RoadLocation(From,Departure);Emerging.Yaw=Departure.Rotation().Yaw;
				if(!CanMoveVehicle(Emerging,Emerging.Location,Emerging.Yaw))return false;
				Car->ParkedAtBuildingID=INDEX_NONE;Car->Location=Emerging.Location;Car->Yaw=Emerging.Yaw;
			}
		}
		else
		{
			int32 Count=0;for(const FAftershockRoadVehicle&V:Vehicles)Count+=!V.bBus&&V.bActive?1:0;if(Count>=MaxCars)return false;
			const FVector Departure=(T->GridToWorld(Path[0])-T->GridToWorld(From)).GetSafeNormal2D();
			const FVector Kerb=RoadLocation(From,Departure);
			// One housing frontage can represent many real owners. Do not stack
			// their cars at the same point; unavailable curb space falls back to walking.
			FAftershockRoadVehicle Proposed;Proposed.ResidentOwnerID=ResidentID;Proposed.Location=Kerb;Proposed.Yaw=Departure.Rotation().Yaw;
			if(!CanMoveVehicle(Proposed,Kerb,Proposed.Yaw))return false;
			FAftershockRoadVehicle New;New.VehicleID=NextVehicleID++;New.ResidentOwnerID=ResidentID;New.Location=Kerb;New.Yaw=Departure.Rotation().Yaw;Vehicles.Add(New);Car=&Vehicles.Last();
		}
		Car->RoadPath=Path;Car->NextPathStep=0;Car->DwellSeconds=MAX_flt;
		Car->SegmentOriginCell=From;Car->bParking=false;Car->bSegmentLaneAligned=false;
		Trip.Mode=EAftershockTravelMode::PrivateCar;Trip.VehicleID=Car->VehicleID;Trip.PickupRoadCell=From;Trip.DropoffRoadCell=To;
		Car->PassengerTripIDs.Add(Trip.TripID);
	}
	Trip.PickupLocation=RoadLocation(Trip.PickupRoadCell);Trip.DropoffLocation=RoadLocation(Trip.DropoffRoadCell);
	if(Trip.Mode==EAftershockTravelMode::Bus)
	{
		if(!GetPassengerKerbLocation(Trip.PickupStopID,Trip.PickupQueueSlot,false,Trip.PickupLocation))return false;
		if(!GetPassengerKerbLocation(Trip.DropoffStopID,0,true,Trip.DropoffLocation))return false;
	}

	if(Trip.Mode==EAftershockTravelMode::PrivateCar)
		if(const FAftershockRoadVehicle* Car=Vehicles.FindByPredicate([&](const FAftershockRoadVehicle&V){return V.VehicleID==Trip.VehicleID;}))Trip.PickupLocation=VehicleDoorLocation(*Car);
	Trips.Add(Trip.TripID,Trip);Out=Trip;return true;
}

bool UTransportSubsystem::MarkPassengerAtPickup(int32 TripID,int32 ResidentID,FVector Location)
{
	FAftershockPassengerTrip* Trip=Trips.Find(TripID);const UTerrainGridSubsystem* T=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	if(!Trip||!T||Trip->ResidentID!=ResidentID||Trip->State!=EAftershockPassengerState::WalkingToPickup||T->WorldToGrid(Location)!=Trip->PickupRoadCell)return false;
    if(Trip->Mode==EAftershockTravelMode::Bus&&T->CellSize>=900.f)
    {
        FVector Reserved;
        if(!GetPassengerKerbLocation(Trip->PickupStopID,Trip->PickupQueueSlot,false,Reserved)||
            FVector::DistSquared2D(Location,Reserved)>1.)return false;
    }
	Trip->CurrentLocation=Location;Trip->State=EAftershockPassengerState::Waiting;
	if(Trip->Mode==EAftershockTravelMode::PrivateCar)
	{
		if(FAftershockRoadVehicle* V=Vehicles.FindByPredicate([&](const FAftershockRoadVehicle&V){return V.VehicleID==Trip->VehicleID&&V.bActive&&!V.bStalled;}))
		{Trip->State=EAftershockPassengerState::Riding;Trip->CurrentLocation=V->Location;V->DwellSeconds=0;return true;}
		Trip->State=EAftershockPassengerState::Stranded;return false;
	}
	return true;
}
bool UTransportSubsystem::GetTrip(int32 TripID,FAftershockPassengerTrip& Out)const
{if(const FAftershockPassengerTrip* P=Trips.Find(TripID)){Out=*P;return true;}return false;}
void UTransportSubsystem::CancelTrip(int32 TripID)
{
	FAftershockPassengerTrip* Trip=Trips.Find(TripID);if(!Trip||!ActiveTrip(Trip->State))return;
	for(FAftershockRoadVehicle&V:Vehicles)if(V.PassengerTripIDs.Remove(TripID)>0)
	{
		V.PendingAlightTripIDs.Remove(TripID);if(V.PendingAlightTripIDs.IsEmpty())V.AlightReleaseSeconds=0.f;
		if(Trip->State==EAftershockPassengerState::Riding)Trip->CurrentLocation=V.Location;if(!V.bBus){V.RoadPath.Reset();V.DwellSeconds=0;}
	}
	Trip->State=EAftershockPassengerState::Cancelled;
}
void UTransportSubsystem::StrandPassengers(FAftershockRoadVehicle& V)
{
	for(int32 ID:V.PassengerTripIDs)if(FAftershockPassengerTrip*P=Trips.Find(ID)){P->CurrentLocation=V.Location;P->State=EAftershockPassengerState::Stranded;}
	V.PassengerTripIDs.Reset();V.PendingAlightTripIDs.Reset();V.AlightReleaseSeconds=0.f;V.RoadPath.Reset();V.NextPathStep=0;V.bSegmentLaneAligned=false;
}
void UTransportSubsystem::SetEmergencyMode(bool bEmergency)
{
	bEmergencyActive=bEmergency;if(!bEmergency){bInfrastructureDirty=true;return;}
	for(FAftershockRoadVehicle&V:Vehicles)StrandPassengers(V);
	for(TPair<int32,FAftershockPassengerTrip>&P:Trips)if(ActiveTrip(P.Value.State))P.Value.State=EAftershockPassengerState::Cancelled;
}

void UTransportSubsystem::HandleBusStop(FAftershockRoadVehicle& Bus)
{
	if(!Bus.ServiceStopIDs.IsValidIndex(Bus.NextServiceStop))return;const int32 StopID=Bus.ServiceStopIDs[Bus.NextServiceStop];
	Bus.PendingAlightTripIDs.Reset();Bus.AlightReleaseSeconds=0.f;
	for(int32 ID:Bus.PassengerTripIDs)if(const FAftershockPassengerTrip*P=Trips.Find(ID))
		if(P->State==EAftershockPassengerState::Riding&&P->DropoffStopID==StopID)Bus.PendingAlightTripIDs.Add(ID);
	Bus.PendingAlightTripIDs.Sort();
	// Queued residents remain physically inside this bus and keep their Riding
	// state until one valid curb exit is available. Fare is never charged on exit.
	if(Bus.PendingAlightTripIDs.IsEmpty())AdvanceBusService(Bus);
}

void UTransportSubsystem::BoardBusPassenger(FAftershockRoadVehicle& Bus,int32 StopID)
{
	const UTerrainGridSubsystem* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const FAftershockTransitStop* Stop=Stops.FindByPredicate([StopID](const auto& Candidate){return Candidate.BuildingID==StopID;});
	// Infrastructure edits can reorder the service list during an existing dwell.
	// An inferred previous index is never proof that this bus reached that stop.
	if(!Terrain||!Stop||Stop->bTerminal||!Bus.bBus||!Bus.bActive||Bus.bStalled
		||!IsRoadSafe(Stop->RoadCell)||Terrain->WorldToGrid(Bus.Location)!=Stop->RoadCell)return;
	TArray<int32> Queue;for(const TPair<int32,FAftershockPassengerTrip>&Pair:Trips)if(Pair.Value.Mode==EAftershockTravelMode::Bus&&Pair.Value.PickupStopID==StopID&&Pair.Value.State==EAftershockPassengerState::Waiting)Queue.Add(Pair.Key);
	Queue.Sort();for(int32 ID:Queue)
	{
		if(Bus.PassengerTripIDs.Num()>=Bus.Capacity)break;FAftershockPassengerTrip&P=Trips.FindChecked(ID);
        if(Terrain->CellSize>=900.f)
        {
            FVector Reserved=FVector::ZeroVector;
            if(!GetPassengerKerbLocation(P.PickupStopID,P.PickupQueueSlot,false,Reserved)||FVector::DistSquared2D(P.CurrentLocation,Reserved)>1.)
            {P.State=EAftershockPassengerState::Cancelled;P.TerminalObservedAtSeconds=HistoryClockSeconds;continue;}
        }
		if(!Bus.ServiceStopIDs.Contains(P.DropoffStopID))continue;P.VehicleID=Bus.VehicleID;P.State=EAftershockPassengerState::Riding;P.CurrentLocation=Bus.Location;Bus.PassengerTripIDs.Add(ID);
		if(UCityLifeSubsystem* Life=GetWorld()->GetSubsystem<UCityLifeSubsystem>())Life->RecordTransitFare(P.ResidentID,P.TripID);
		break; // One actual boarding per callback, retaining all other finite queue reservations.
	}
}

void UTransportSubsystem::AdvanceBusService(FAftershockRoadVehicle& Bus)
{
	Bus.PendingAlightTripIDs.Reset();Bus.AlightReleaseSeconds=0.f;
	if(Bus.ServiceStopIDs.IsEmpty()){StrandPassengers(Bus);Bus.bStalled=true;return;}
	if(Bus.ServiceStopIDs.IsValidIndex(Bus.NextServiceStop))BoardBusPassenger(Bus,Bus.ServiceStopIDs[Bus.NextServiceStop]);
	Bus.DwellSeconds=1.6f;Bus.NextServiceStop=(Bus.NextServiceStop+1)%Bus.ServiceStopIDs.Num();
	const FAftershockTransitStop* Target=Stops.FindByPredicate([&](const FAftershockTransitStop&S){return S.BuildingID==Bus.ServiceStopIDs[Bus.NextServiceStop];});
	const UTerrainGridSubsystem* T=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();Bus.NextPathStep=0;
	Bus.SegmentOriginCell=T->WorldToGrid(Bus.Location);
	Bus.bSegmentLaneAligned=false;
	Bus.bStalled=!Target||!BuildRoadPath(T->WorldToGrid(Bus.Location),Target->RoadCell,Bus.RoadPath);
	if(Bus.bStalled)
	{
		StrandPassengers(Bus);
		for(TPair<int32,FAftershockPassengerTrip>& P:Trips)
			if(P.Value.Mode==EAftershockTravelMode::Bus&&P.Value.State==EAftershockPassengerState::Waiting
				&&Bus.ServiceStopIDs.Contains(P.Value.PickupStopID))P.Value.State=EAftershockPassengerState::Cancelled;
	}
}

void UTransportSubsystem::UpdateTransport()
{
	if(!GetWorld())return;
	HistoryClockSeconds+=TransportStep;HistoryRefreshSeconds+=TransportStep;
	if(HistoryRefreshSeconds>=5.f){HistoryRefreshSeconds=0.f;PruneTripHistory();}
	if(PruneInactiveBuses())bInfrastructureDirty=true;
	if(bInfrastructureDirty)RefreshInfrastructure();
	else if(bMunicipalRebuildPending)
	{
		TArray<FBuildingInstance> Buildings;
		if(const auto* Manager=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>())Manager->GetAllBuildings(Buildings);
		Buildings.Sort([](const auto& A,const auto& B){return A.InstanceID<B.InstanceID;});RefreshMunicipalFleet(Buildings);
	}
	if(!IsValid(Host)){FActorSpawnParameters P;P.ObjectFlags|=RF_Transient;Host=GetWorld()->SpawnActor<AAftershockTransportHost>(FVector::ZeroVector,FRotator::ZeroRotator,P);}
	UpdateMunicipalFleet();
	const UTerrainGridSubsystem* T=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
	if(T)for(FAftershockRoadVehicle&V:Vehicles)
	{
		if(!V.bActive||V.bStalled||V.ParkedAtBuildingID!=INDEX_NONE)continue;
		if(!V.PendingAlightTripIDs.IsEmpty())
		{
			V.AlightReleaseSeconds=FMath::Max(0.f,V.AlightReleaseSeconds-TransportStep);
			if(V.AlightReleaseSeconds<=0.f)
			{
				const int32 ID=V.PendingAlightTripIDs[0];FAftershockPassengerTrip* Passenger=Trips.Find(ID);
				FVector Exit;
				if(!Passenger||Passenger->State!=EAftershockPassengerState::Riding||!V.PassengerTripIDs.Contains(ID))
				{V.PendingAlightTripIDs.RemoveAt(0);if(V.PendingAlightTripIDs.IsEmpty())AdvanceBusService(V);}
				else if(FindPassengerReleaseLocation(ID,Exit))
				{
					Passenger->CurrentLocation=Exit;Passenger->DropoffLocation=Exit;Passenger->State=EAftershockPassengerState::Arrived;
					Passenger->TerminalObservedAtSeconds=HistoryClockSeconds;
					V.PassengerTripIDs.Remove(ID);V.PendingAlightTripIDs.RemoveAt(0);
					if(V.PendingAlightTripIDs.IsEmpty())AdvanceBusService(V);
					else V.AlightReleaseSeconds=T->CellSize>=900.f?.32f:.48f;
				}
				else V.AlightReleaseSeconds=.16f; // A busy curb holds the real passenger inside.
			}
			continue;
		}
		if(V.DwellSeconds>0)
		{
			if(V.DwellSeconds!=MAX_flt)
			{
				const float Remaining=FMath::Max(0.f,V.DwellSeconds-TransportStep);
				// Derive the doorway cadence from the already-saved dwell timer;
				// restore neither duplicates a boarding nor needs another clock authority.
				if(V.bBus&&!V.ServiceStopIDs.IsEmpty()
					&&FMath::FloorToInt((1.6f-V.DwellSeconds+.0001f)/.32f)!=FMath::FloorToInt((1.6f-Remaining+.0001f)/.32f))
					BoardBusPassenger(V,V.ServiceStopIDs[(V.NextServiceStop+V.ServiceStopIDs.Num()-1)%V.ServiceStopIDs.Num()]);
				V.DwellSeconds=Remaining;
			}
			continue;
		}
		if(!V.RoadPath.IsValidIndex(V.NextPathStep))
		{
			if(V.bBus)HandleBusStop(V);
			else if(!V.PassengerTripIDs.IsEmpty())
			{
				if(!V.bParking)
				{
					const FVector Forward=FRotator(0,V.Yaw,0).Vector();
					const float BayOffset=T->CellSize*(T->CellSize>=900.f?.205f:.22f);
					V.ParkingGoal=GetWorld()->GetSubsystem<URoadNetworkSubsystem>()->RoadGridToWorld(T->WorldToGrid(V.Location))+FVector(-Forward.Y,Forward.X,0)*BayOffset+FVector(0,0,3);
					V.bParking=true;
				}
				TryMoveVehicle(V,FMath::VInterpConstantTo(V.Location,V.ParkingGoal,TransportStep,120.f),V.Yaw);
				const bool Arrived=FVector::Dist2D(V.Location,V.ParkingGoal)<10.f;
				TArray<int32,TInlineAllocator<2>> Released;int32 ArrivedDestination=INDEX_NONE;
				for(int32 ID:V.PassengerTripIDs)if(FAftershockPassengerTrip*P=Trips.Find(ID))
				{
					FVector Exit;P->CurrentLocation=V.Location;
					if(Arrived&&FindPassengerReleaseLocation(ID,Exit)){P->CurrentLocation=Exit;P->DropoffLocation=Exit;P->State=EAftershockPassengerState::Arrived;P->TerminalObservedAtSeconds=HistoryClockSeconds;Released.Add(ID);if(P->ResidentID==V.ResidentOwnerID)ArrivedDestination=P->DestinationBuildingID;}
				}
				for(int32 ID:Released)V.PassengerTripIDs.Remove(ID);
				if(Arrived&&V.PassengerTripIDs.IsEmpty())
				{
					V.bParking=false;
					if(ArrivedDestination!=INDEX_NONE&&CanParkOffstreet(V,ArrivedDestination))V.ParkedAtBuildingID=ArrivedDestination;
				}
			}
			continue;
		}
		const FIntPoint Next=V.RoadPath[V.NextPathStep];
		if(!IsRoadSafe(T->WorldToGrid(V.Location))||!IsRoadSafe(Next))
		{
			V.bStalled=true;StrandPassengers(V);
			if(V.bBus)for(TPair<int32,FAftershockPassengerTrip>& P:Trips)
				if(P.Value.Mode==EAftershockTravelMode::Bus&&P.Value.State==EAftershockPassengerState::Waiting
					&&V.ServiceStopIDs.Contains(P.Value.PickupStopID))P.Value.State=EAftershockPassengerState::Cancelled;
			continue;
		}
		const FVector Direction=(T->GridToWorld(Next)-T->GridToWorld(V.SegmentOriginCell)).GetSafeNormal2D();
		if(!V.bSegmentLaneAligned)
		{
			// Changing heading must finish inside the cell already occupied. Testing
			// the next cell while still on the previous heading's offset created a
			// false collision between two otherwise separate, opposing lanes.
			const FVector LaneOrigin=RoadLocation(V.SegmentOriginCell,Direction);
			const FVector Before=V.Location;
			if(!TryMoveVehicle(V,FMath::VInterpConstantTo(V.Location,LaneOrigin,TransportStep,V.bBus?330.f:450.f),FMath::FixedTurn(V.Yaw,Direction.Rotation().Yaw,180.f*TransportStep)))continue;
			V.bSegmentLaneAligned=FVector::DistSquared2D(V.Location,LaneOrigin)<1.f;
			for(int32 ID:V.PassengerTripIDs)if(FAftershockPassengerTrip* P=Trips.Find(ID))if(P->State==EAftershockPassengerState::Riding)P->CurrentLocation=V.Location;
			if(!V.bSegmentLaneAligned||!Before.Equals(V.Location,.01f))continue;
		}
		const FVector Goal=RoadLocation(Next,Direction);const FVector Delta=Goal-V.Location;const float Distance=Delta.Size2D();
		int32 JunctionArms=0;FIntPoint ArmSum=FIntPoint::ZeroValue;
		for(FIntPoint Offset:Adjacent)if(IsRoadSafe(Next+Offset)){++JunctionArms;ArmSum+=Offset;}
		const bool IsCorner=JunctionArms==2&&ArmSum!=FIntPoint::ZeroValue;
		// A two-arm corner needs entry spacing too: otherwise following vehicles
		// catch a turning leader after its heading changes and stack at the corner.
		const bool ApproachingJunction=(JunctionArms>=3||IsCorner)&&T->WorldToGrid(V.Location)!=Next;
		const FVector Dimensions=VehicleDimensions(V);
		float AvailableStep=Distance;
		bool Following=false;for(const FAftershockRoadVehicle&Other:Vehicles)
		{
			if(!Other.bActive||Other.bStalled||Other.ParkedAtBuildingID!=INDEX_NONE||Other.VehicleID==V.VehicleID)continue;
			if(FVector::DistSquared2D(Other.Location,V.Location)>FMath::Square(T->CellSize*3.5f))continue;
			bool OtherDriving=Other.bBus;
			for(int32 ID:Other.PassengerTripIDs)if(const FAftershockPassengerTrip* Passenger=Trips.Find(ID))OtherDriving|=Passenger->State==EAftershockPassengerState::Riding;
			if(!OtherDriving)continue;
			const FVector D=Other.Location-V.Location;
			const FVector OtherDirection=Other.RoadPath.IsValidIndex(Other.NextPathStep)
				?(T->GridToWorld(Other.RoadPath[Other.NextPathStep])-T->GridToWorld(Other.SegmentOriginCell)).GetSafeNormal2D():FRotator(0,Other.Yaw,0).Vector();
			// Only cars going in the same direction form a following queue.
			// Treating crossing/oncoming traffic as a leader creates mutual waits.
			const FVector OtherDimensions=VehicleDimensions(Other);
			const float Ahead=FVector::DotProduct(D,Direction),Lateral=FMath::Abs(FVector::CrossProduct(D,Direction).Z);
			const FVector OtherFacing=FRotator(0,Other.Yaw,0).Vector(),Facing=FRotator(0,V.Yaw,0).Vector();
			const auto ProjectedHalf=[](const FVector& Size,const FVector& Forward,const FVector& Axis)->float
			{
				return (FMath::Abs(FVector::DotProduct(Forward,Axis))*Size.X+FMath::Abs(FVector::CrossProduct(Forward,Axis).Z)*Size.Y)*.5f;
			};
			const FVector LateralAxis(-Direction.Y,Direction.X,0);
			if(FVector::DotProduct(Direction,OtherDirection)>.5f&&Ahead>0
				&&Lateral<ProjectedHalf(Dimensions,Facing,LateralAxis)+ProjectedHalf(OtherDimensions,OtherFacing,LateralAxis)+8.f)
				AvailableStep=FMath::Min(AvailableStep,FMath::Max(0.f,Ahead-ProjectedHalf(Dimensions,Facing,Direction)-ProjectedHalf(OtherDimensions,OtherFacing,Direction)-45.f-CruiseSpeed(V)*.16f));
			if(!ApproachingJunction)continue;
			// Opposing, parallel lanes may occupy one junction cell concurrently.
			// Whole-cell exclusion deadlocks two cars exchanging adjacent junctions.
			// Require actual lateral separation as well as opposing headings; a
			// crossing/turning vehicle already inside the junction still clears first.
			const bool SeparatedOncomingLane=FVector::DotProduct(Direction,OtherDirection)<-.9f
				&&FMath::Abs(FVector::CrossProduct(D,Direction).Z)>=T->CellSize*.32f;
			if(SeparatedOncomingLane)continue;
			// Conflicting approaches use stable ID priority only while the junction
			// is empty. A vehicle inside always keeps priority over an entering one.
			const FVector JunctionDelta=Other.Location-T->GridToWorld(Next);
			const float HalfExtentX=ProjectedHalf(OtherDimensions,OtherFacing,FVector::ForwardVector);
			const float HalfExtentY=ProjectedHalf(OtherDimensions,OtherFacing,FVector::RightVector);
			const bool TailStillClearing=Other.SegmentOriginCell==Next&&T->WorldToGrid(Other.Location)!=Next
				&&FMath::Abs(JunctionDelta.X)<T->CellSize*.5f+HalfExtentX+20.f&&FMath::Abs(JunctionDelta.Y)<T->CellSize*.5f+HalfExtentY+20.f;
			if(T->WorldToGrid(Other.Location)==Next||TailStillClearing
				||(Other.RoadPath.IsValidIndex(Other.NextPathStep)&&Other.RoadPath[Other.NextPathStep]==Next&&Other.VehicleID<V.VehicleID&&FVector::DistSquared2D(Other.Location,T->GridToWorld(Next))<FMath::Square(T->CellSize*1.2f)))
			{Following=true;break;}
		}
		// A distance-clamped step can land exactly on this waypoint. Acknowledge
		// the completed segment before testing permission for further movement;
		// zero remaining distance is not a permanently blocked following gap.
		const bool bAtWaypoint=Distance<=.01f;
		if(!bAtWaypoint&&(Following||AvailableStep<.01f))continue;
		if(bAtWaypoint||(Distance<18.f&&AvailableStep>=Distance))
		{
			if(!TryMoveVehicle(V,Goal,V.Yaw))continue;
			V.SegmentOriginCell=Next;++V.NextPathStep;V.bSegmentLaneAligned=false;
		}
		else
		{
			const float Step=FMath::Min(AvailableStep,CruiseSpeed(V)*TransportStep);
			FVector Candidate=V.Location+Delta.GetSafeNormal2D()*Step;
			Candidate.Z=GetWorld()->GetSubsystem<URoadNetworkSubsystem>()->SampleRoadSurfaceElevation(Candidate)+3.f;
			if(!TryMoveVehicle(V,Candidate,FMath::FixedTurn(V.Yaw,Direction.Rotation().Yaw,180.f*TransportStep)))continue;
		}
		for(int32 ID:V.PassengerTripIDs)if(FAftershockPassengerTrip*P=Trips.Find(ID))if(P->State==EAftershockPassengerState::Riding)P->CurrentLocation=V.Location;
	}
	DrawAllFleets();
}

bool UTransportSubsystem::PruneInactiveBuses()
{
	TSet<int32> InactiveBusIDs;
	for(const auto& Vehicle:Vehicles)if(Vehicle.bBus&&!Vehicle.bActive)InactiveBusIDs.Add(Vehicle.VehicleID);
	if(InactiveBusIDs.IsEmpty())return false; // Normal active-only frames read no crowd/history arrays.
	TSet<int32> ProtectedVehicleIDs;
	for(const auto& Vehicle:Vehicles)if(InactiveBusIDs.Contains(Vehicle.VehicleID)&&
		(!Vehicle.PassengerTripIDs.IsEmpty()||!Vehicle.PendingAlightTripIDs.IsEmpty()))ProtectedVehicleIDs.Add(Vehicle.VehicleID);
	for(const auto& Pair:Trips)if(InactiveBusIDs.Contains(Pair.Value.VehicleID)&&ActiveTrip(Pair.Value.State))ProtectedVehicleIDs.Add(Pair.Value.VehicleID);
	if(const auto* Crowd=GetWorld()?GetWorld()->GetSubsystem<UCrowdRenderSubsystem>():nullptr)
	{
		// Read the canonical representatives by const reference. Copying their
		// complete walking routes just to inspect a handful of handoffs would
		// introduce allocation traffic during the very teardown being repaired.
		for(const auto& Agent:Crowd->Agents)if(Agent.bInsideVehicle)
		{
			const auto* Trip=Trips.Find(Agent.TransportTripID);
			if(!Trip)return false; // Defer an unresolved visual handoff until Crowd acknowledges it.
			if(InactiveBusIDs.Contains(Trip->VehicleID))ProtectedVehicleIDs.Add(Trip->VehicleID);
		}
	}
	const int32 Removed=Vehicles.RemoveAll([&](const auto& Vehicle)
	{return Vehicle.bBus&&!Vehicle.bActive&&Vehicle.PassengerTripIDs.IsEmpty()&&Vehicle.PendingAlightTripIDs.IsEmpty()&&!ProtectedVehicleIDs.Contains(Vehicle.VehicleID);});
	if(Removed>0)UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_TRANSIT_RETIRED buses=%d pendingPhysicalHandoffsPreserved=1 historicalTripIDsUnchanged=1 faresUnchanged=1"),Removed);
	return Removed>0;
}

void UTransportSubsystem::PruneTripHistory()
{
	constexpr int32 RetainedTerminalTrips=2048;
	TArray<int32> Finished;
	for(TPair<int32,FAftershockPassengerTrip>& Pair:Trips)
	{
		if(ActiveTrip(Pair.Value.State))continue;
		if(Pair.Value.TerminalObservedAtSeconds<0.f)Pair.Value.TerminalObservedAtSeconds=HistoryClockSeconds;
		Finished.Add(Pair.Key);
	}
	Finished.Sort();
	int32 Excess=Finished.Num()-RetainedTerminalTrips;
	for(int32 ID:Finished)
	{
		if(Excess<=0)break;
		const FAftershockPassengerTrip& Trip=Trips.FindChecked(ID);
		// A completion must remain readable by the crowd's following timer update.
		// Grace applies even to a long-running trip with an old numerical ID.
		if(HistoryClockSeconds-Trip.TerminalObservedAtSeconds<2.f)continue;
		RetiredTripStats.CompletedTrips+=Trip.State==EAftershockPassengerState::Arrived?1:0;
		RetiredTripStats.StrandedTrips+=Trip.State==EAftershockPassengerState::Stranded?1:0;
		RetiredTripStats.BusBoardings+=Trip.Mode==EAftershockTravelMode::Bus&&Trip.VehicleID!=INDEX_NONE?1:0;
		RetiredTripStats.BusArrivals+=Trip.Mode==EAftershockTravelMode::Bus&&Trip.State==EAftershockPassengerState::Arrived?1:0;
		RetiredTripStats.PrivateCarTrips+=Trip.Mode==EAftershockTravelMode::PrivateCar?1:0;
		Trips.Remove(ID);--Excess;
	}
}

TArray<FAftershockPassengerTrip> UTransportSubsystem::GetTripRoster()const
{
	TArray<FAftershockPassengerTrip> Result;Trips.GenerateValueArray(Result);Result.Sort([](const FAftershockPassengerTrip&A,const FAftershockPassengerTrip&B){return A.TripID<B.TripID;});return Result;
}
FAftershockTransportStats UTransportSubsystem::GetTransportStats()const
{
	FAftershockTransportStats S=RetiredTripStats;for(const FAftershockRoadVehicle&V:Vehicles)if(V.bActive){V.bBus?++S.Buses:++S.Cars;for(int32 I=V.NextPathStep;I<V.RoadPath.Num();++I)S.InvalidRoadSteps+=IsRoadSafe(V.RoadPath[I])?0:1;}
	for(const FAftershockTransitStop&Stop:Stops)S.Stops+=!Stop.bTerminal?1:0;
	for(const TPair<int32,FAftershockPassengerTrip>&P:Trips)
	{S.WaitingPassengers+=P.Value.State==EAftershockPassengerState::Waiting?1:0;S.RidingPassengers+=P.Value.State==EAftershockPassengerState::Riding?1:0;S.CompletedTrips+=P.Value.State==EAftershockPassengerState::Arrived?1:0;S.StrandedTrips+=P.Value.State==EAftershockPassengerState::Stranded?1:0;S.BusBoardings+=P.Value.Mode==EAftershockTravelMode::Bus&&P.Value.VehicleID!=INDEX_NONE?1:0;S.BusArrivals+=P.Value.Mode==EAftershockTravelMode::Bus&&P.Value.State==EAftershockPassengerState::Arrived?1:0;S.PrivateCarTrips+=P.Value.Mode==EAftershockTravelMode::PrivateCar?1:0;}
	return S;
}
void UTransportSubsystem::AuditTransport()const
{
	const FAftershockTransportStats S=GetTransportStats();UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_TRANSPORT cars=%d buses=%d stops=%d waiting=%d riding=%d completed=%d stranded=%d unsafePathSteps=%d populationOwner=CityLife vehicleActors=0 vehicleTick=0"),S.Cars,S.Buses,S.Stops,S.WaitingPassengers,S.RidingPassengers,S.CompletedTrips,S.StrandedTrips,S.InvalidRoadSteps);
}
