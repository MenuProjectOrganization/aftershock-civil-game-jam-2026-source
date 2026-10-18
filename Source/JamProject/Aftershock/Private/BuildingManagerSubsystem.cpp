// AFTERSHOCK — Civil Game Jam 2026
// Optimize Bina Render ve İnşaat Yöneticisi Alt Sistemi Uygulaması

#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/AftershockBuildingCatalog.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "AftershockPhysicalLifecycle.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "Engine/Texture.h"
#include "Misc/PackageName.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/TransportSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Narrative/CasualtyLedgerGenerator.h"
#include "AftershockTerrainSurface.h"
#include "AftershockArchitecturalRenderer.h"
#include "AftershockPresentationBudget.h"
#include "Math/RandomStream.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "TimerManager.h"
#include "Algo/Reverse.h"

namespace
{
    bool BuildingInputsFrozenByGranularEvent(const UWorld* World)
    {
        const auto* Simulation=World?World->GetSubsystem<UDisasterSimulationSubsystem>():nullptr;
        if(!Simulation)return false;
        const auto& Episode=Simulation->GetPhysicalEpisodeState();
        return AftershockPhysicalLifecycle::LocksGranularBuildingInputs(Episode.bActive,Episode.Type);
    }
	void ApplyBuildingLOD(UStaticMeshComponent& Component,int32 Floor)
	{
		const int32 Count=Component.GetStaticMesh()?Component.GetStaticMesh()->GetNumLODs():0;
		const int32 Forced=Floor>0&&Count>0?FMath::Clamp(Floor,1,Count):0;
		if(Component.GetForcedLodModel()!=Forced)Component.SetForcedLodModel(Forced);
	}
	void SubmitDecorationBatch(UInstancedStaticMeshComponent& Batch,const TArray<FTransform>& Transforms,const TArray<float>* Strengths=nullptr)
	{
		bool Changed=Batch.GetInstanceCount()!=Transforms.Num();
		if(Changed)
		{
			if(Batch.GetInstanceCount()>0)Batch.ClearInstances();
			if(!Transforms.IsEmpty())Batch.AddInstances(Transforms,false,true,false);
		}
		else
		{
			bool TransformChanged=false;
			for(int32 I=0;I<Transforms.Num()&&!TransformChanged;++I){FTransform Current;TransformChanged=!Batch.GetInstanceTransform(I,Current,true)||!Current.Equals(Transforms[I],0.f);}
			if(TransformChanged){Batch.BatchUpdateInstancesTransforms(0,Transforms,true,false,true);Changed=true;}
		}
		if(Strengths)for(int32 I=0;I<Strengths->Num();++I)
			if(!Batch.PerInstanceSMCustomData.IsValidIndex(I)||Batch.PerInstanceSMCustomData[I]!=(*Strengths)[I]){Batch.SetCustomDataValue(I,0,(*Strengths)[I],false);Changed=true;}
		if(Changed)Batch.MarkRenderStateDirty();
	}
	void SetBuildingVisibilityTraceResponse(UInstancedStaticMeshComponent& Source,ECollisionResponse Response)
	{
		// INDEX_NONE returns the component template in UE5.8. Existing ISM bodies
		// have copied responses, so a template-only update leaves stale query filters.
		FBodyInstance* Template=Source.GetBodyInstance(NAME_None,false,INDEX_NONE);
		if(!Template||Template->GetResponseToChannel(ECC_Visibility)==Response)return;
		Template->SetResponseToChannel(ECC_Visibility,Response);
		for(FBodyInstance* Body:Source.GetInstanceBodies())
			if(Body&&Body->IsValidBodyInstance()&&Body->GetResponseToChannel(ECC_Visibility)!=Response)
				Body->SetResponseToChannel(ECC_Visibility,Response);
	}
}

TSoftObjectPtr<UGeometryCollection> UBuildingManagerSubsystem::GetDefaultFracturedMesh(EBuildingType Type)
{
	const TCHAR* Path = nullptr;
	switch (Type)
	{
	case EBuildingType::House: Path = TEXT("/Game/Buildings/Fractured/GC_House_01.GC_House_01"); break;
	case EBuildingType::Apartment: Path = TEXT("/Game/Buildings/Fractured/GC_Apartment_01.GC_Apartment_01"); break;
	case EBuildingType::Office: Path = TEXT("/Game/Buildings/Fractured/GC_Office_01.GC_Office_01"); break;
	case EBuildingType::Factory: Path = TEXT("/Game/Buildings/Fractured/GC_Factory_01.GC_Factory_01"); break;
	case EBuildingType::Hospital: Path = TEXT("/Game/Buildings/Fractured/GC_Hospital_01.GC_Hospital_01"); break;
	default: break;
	}
	return Path ? TSoftObjectPtr<UGeometryCollection>(FSoftObjectPath(Path)) : TSoftObjectPtr<UGeometryCollection>();
}

UBuildingData* UBuildingManagerSubsystem::GetCatalogData(EBuildingType Type)
{
	if (const TObjectPtr<UBuildingData>* Existing = CatalogDefinitions.Find(Type)) return Existing->Get();
	UBuildingData* Data = FAftershockBuildingCatalog::CreateData(this, Type);
	if (Data) CatalogDefinitions.Add(Type, Data);
	return Data;
}

void UBuildingManagerSubsystem::SetBuildingOperationalVisual(int32 BuildingID, float PowerCoverage01, float Operation01)
{
	const FBuildingInstance* Building = ActiveBuildings.Find(BuildingID);
	if (!Building || !Building->BuildingData || Building->ISMInstanceIndex == INDEX_NONE) return;
	if (UInstancedStaticMeshComponent* ISM = GetOrCreateISM(Building->BuildingData))
	{
		const float Values[]={FMath::Clamp(PowerCoverage01,0.f,1.f),FMath::Clamp(Operation01,0.f,1.f)};bool Changed=false;
		for(int32 C=0;C<2;++C){const int32 At=Building->ISMInstanceIndex*ISM->NumCustomDataFloats+2+C;
			if(!ISM->PerInstanceSMCustomData.IsValidIndex(At)||ISM->PerInstanceSMCustomData[At]!=Values[C]){ISM->SetCustomDataValue(Building->ISMInstanceIndex,2+C,Values[C],false);Changed=true;}}
		if(Changed){ISM->MarkRenderStateDirty();SyncPresentedBuilding(BuildingID);}
	}
}

void UBuildingManagerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	NextBuildingID = 1;
	ActiveBuildings.Empty();
	BuildingISMs.Empty();
	FreeBuildingInstanceIndices.Reset();
	CatalogDefinitions.Empty();
	PresentationISMs.Empty();PresentationComponentByID.Empty();PresentationIndexByID.Empty();
	CanonicalVisibilityResponses.Empty();
	bUseDerivedBuildingPresentation=false;bBuildingPresentationDirty=true;
	BuildingPresentationBounds=FIntRect(0,0,0,0);
	ISMHostActor = nullptr;
	PresentationWork={};bDamageDecorationsDirty=false;bMemorialDecorationsDirty=false;bFlushingDecorations=false;
	bConstructionTransactionInProgress=false;
}

void UBuildingManagerSubsystem::Deinitialize()
{
	ArchitecturalRenderer.Reset();bArchitecturalLayoutDirty=true;
	if(UWorld* World=GetWorld())World->GetTimerManager().ClearTimer(DecorationRefreshTimer);
	bDamageDecorationsDirty=false;bMemorialDecorationsDirty=false;
	NextBuildingID = 1;
	ActiveBuildings.Empty();
	BuildingISMs.Empty();
	FreeBuildingInstanceIndices.Reset();
	CatalogDefinitions.Empty();
	PresentationISMs.Empty();PresentationComponentByID.Empty();PresentationIndexByID.Empty();
	CanonicalVisibilityResponses.Empty();
	MemorialCandles=nullptr;MemorialFlowers=nullptr;DamagedBuildingSmokeISM=nullptr;DamagedBuildingDebrisISM=nullptr;

	if (IsValid(ISMHostActor))
	{
		ISMHostActor->Destroy();
		ISMHostActor = nullptr;
	}

	Super::Deinitialize();
}

AActor* UBuildingManagerSubsystem::GetOrCreateISMHostActor()
{
	if (IsValid(ISMHostActor))
	{
		return ISMHostActor;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = TEXT("Aftershock_ISMHostActor");
	SpawnParams.ObjectFlags |= RF_Transient;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ISMHostActor = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (ISMHostActor)
	{
		ISMHostActor->PrimaryActorTick.bCanEverTick = false;
		ISMHostActor->SetActorTickEnabled(false);

		USceneComponent* RootComp = NewObject<USceneComponent>(ISMHostActor, TEXT("ISMRoot"));
		ISMHostActor->SetRootComponent(RootComp);
		RootComp->RegisterComponent();
	}

	return ISMHostActor;
}

UInstancedStaticMeshComponent* UBuildingManagerSubsystem::GetOrCreateISM(UBuildingData* Data)
{
	if (!Data)
	{
		return nullptr;
	}

	if (TObjectPtr<UInstancedStaticMeshComponent>* FoundISM = BuildingISMs.Find(Data))
	{
		if (IsValid(*FoundISM))
		{
			return *FoundISM;
		}
	}

	AActor* Host = GetOrCreateISMHostActor();
	if (!Host)
	{
		return nullptr;
	}

	const FName CompName = MakeUniqueObjectName(
		Host,
		UInstancedStaticMeshComponent::StaticClass(),
		*FString::Printf(TEXT("ISM_%s"), *Data->GetName())
	);

	UInstancedStaticMeshComponent* NewISM = NewObject<UInstancedStaticMeshComponent>(Host, CompName);
	if (NewISM)
	{
		NewISM->AttachToComponent(Host->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);

		// Mesh Intact görselini yükle; atanmamışsa /Engine/BasicShapes/Cube.Cube fallback'i kullan
		UStaticMesh* Mesh = Data->MeshIntact.LoadSynchronous();
		if (!Mesh)
		{
			UE_LOG(LogTemp, Warning, TEXT("AFTERSHOCK_BUILDING_MESH_FALLBACK type=%d data=%s path=%s"),
				static_cast<int32>(Data->Type),
				*Data->GetName(),
				*Data->MeshIntact.ToSoftObjectPath().ToString());
			Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		}
		if (Mesh)
		{
			NewISM->SetStaticMesh(Mesh);
		}

		// Restrained shared type tint. Gameplay construction material and all
		// per-instance damage/power values remain owned by their existing systems.
		FLinearColor BuildingColor(.67f,.65f,.60f);
		switch (Data->Type)
		{
		case EBuildingType::House:       BuildingColor=FLinearColor(.70f,.60f,.45f); break;
		case EBuildingType::Apartment:   BuildingColor=FLinearColor(.62f,.64f,.62f); break;
		case EBuildingType::Office:      BuildingColor=FLinearColor(.30f,.40f,.45f); break;
		case EBuildingType::Factory:     BuildingColor=FLinearColor(.46f,.38f,.31f); break;
		case EBuildingType::Hospital:    BuildingColor=FLinearColor(.78f,.79f,.75f); break;
		case EBuildingType::School:      BuildingColor=FLinearColor(.70f,.65f,.52f); break;
		case EBuildingType::FireStation: BuildingColor=FLinearColor(.57f,.28f,.23f); break;
		case EBuildingType::PowerPlant:  BuildingColor=FLinearColor(.35f,.38f,.38f); break;
		case EBuildingType::GreenSpace:  BuildingColor=FLinearColor(.24f,.40f,.26f); break;
		default: break;
		}
		const bool bWood=Data->Material==EMaterialType::Wood;
		const bool bBrick=Data->Material==EMaterialType::Brick;
		const bool bEquipment=Data->Type==EBuildingType::Generator||Data->Type==EBuildingType::WindTurbine
			||Data->Type==EBuildingType::BusStop||Data->Type==EBuildingType::Substation;
		const bool bPhotoFacade=!bWood&&!bEquipment&&Data->Type!=EBuildingType::GreenSpace;
		// Weak cache contains only art resources, never a second city/population
		// owner. Live shared MIDs keep the textures alive while a world uses them.
		const auto LoadFacadeTexture=[](const TCHAR* Name)->UTexture*
		{
			static TMap<FString,TWeakObjectPtr<UTexture>> Textures;
			const FString Package=FString::Printf(TEXT("/Game/Aftershock/CitySample/Textures/%s"),Name);
			if(const TWeakObjectPtr<UTexture>* Cached=Textures.Find(Package))
				if(Cached->IsValid())return Cached->Get();
			// Old fixtures and checkouts without licensed art retain the authored
			// material. Do not emit missing-package load errors or blank facades.
			if(!FPackageName::DoesPackageExist(Package))return nullptr;
			const FString Object=Package+TEXT(".")+Name;
			UTexture* Texture=LoadObject<UTexture>(nullptr,*Object);
			if(Texture)Textures.Add(Package,Texture);
			return Texture;
		};
		TMap<UMaterialInterface*,UMaterialInstanceDynamic*> SharedSlots;
		int32 FacadeSlots=0;
		for(int32 Slot=0;Slot<NewISM->GetNumMaterials();++Slot)
		{
			UMaterialInterface* Source=NewISM->GetMaterial(Slot);
			if(!Source)continue;
			const UMaterial* Master=Source->GetMaterial();
			const FString MasterPath=Master?Master->GetPathName():FString();
			if(MasterPath!=TEXT("/Game/Buildings/Materials/M_Building_Base.M_Building_Base")
				&&MasterPath!=TEXT("/Game/Buildings/Materials/M_ServiceBuilding.M_ServiceBuilding"))continue;
			if(UMaterialInstanceDynamic** Existing=SharedSlots.Find(Source))
			{NewISM->SetMaterial(Slot,*Existing);continue;}
			UMaterialInstanceDynamic* Shared=UMaterialInstanceDynamic::Create(Source,NewISM);
			if(!Shared)continue;
			Shared->SetVectorParameterValue(TEXT("BaseColorDefault"),BuildingColor);
			float DeclaredStrength=0.f;
			const bool bHasFacadeRecipe=Source->GetScalarParameterValue(
				FHashedMaterialParameterInfo(FName(TEXT("CitySampleFacadeStrength"))),DeclaredStrength);
			UTexture* Albedo=nullptr;UTexture* Normal=nullptr;UTexture* Packed=nullptr;
			if(bPhotoFacade&&bHasFacadeRecipe)
			{
				Albedo=LoadFacadeTexture(bBrick?TEXT("T_Brick_C"):TEXT("T_Concrete_C"));
				Normal=LoadFacadeTexture(bBrick?TEXT("T_Brick_N"):TEXT("T_Concrete_N"));
				if(bBrick)Packed=LoadFacadeTexture(TEXT("T_Brick_M"));
			}
			const bool bReady=bPhotoFacade&&bHasFacadeRecipe&&Albedo&&Normal;
			Shared->SetScalarParameterValue(TEXT("CitySampleFacadeStrength"),bReady?(bBrick?.82f:.72f):0.f);
			if(bReady)
			{
				Shared->SetTextureParameterValue(TEXT("CitySampleFacadeAlbedo"),Albedo);
				Shared->SetTextureParameterValue(TEXT("CitySampleFacadeNormal"),Normal);
				Shared->SetScalarParameterValue(TEXT("CitySampleFacadeNormalStrength"),bBrick?.42f:.28f);
				Shared->SetScalarParameterValue(TEXT("CitySampleFacadeTileCm"),bBrick?200.f:280.f);
				if(Packed)Shared->SetTextureParameterValue(TEXT("CitySampleFacadePacked"),Packed);
				++FacadeSlots;
			}
			// Brick inherits only the author's natively proven channel and enable
			// default. Concrete has no proven packed map: retain old roughness.
			if(!bReady||!bBrick||!Packed)
				Shared->SetScalarParameterValue(TEXT("CitySampleFacadeUsePackedRoughness"),0.f);
			SharedSlots.Add(Source,Shared);
			NewISM->SetMaterial(Slot,Shared);
		}
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_BUILDING_FACADE_BIND type=%d family=%s sharedMaterialCount=%d photoSlots=%d woodUnchanged=%d noPerBuildingMID=1"),
			static_cast<int32>(Data->Type),bWood?TEXT("OriginalWood"):bBrick?TEXT("EpicBrick"):TEXT("EpicConcrete"),
			SharedSlots.Num(),FacadeSlots,bWood);

		// Four per-instance values; no per-building material or Actor:
		// Index 2 = actual city-life power coverage; 3 = operating fraction.
		// Index 0 = Hasar Oranı [0.0 - 1.0]
		// Index 1 = Retrofit Durumu [0.0 veya 1.0]
		NewISM->NumCustomDataFloats = 4;

		NewISM->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		NewISM->SetCollisionResponseToAllChannels(ECR_Block);
		NewISM->SetVisibility(!bUseDerivedBuildingPresentation);
		if(bUseDerivedBuildingPresentation){CanonicalVisibilityResponses.Add(NewISM,NewISM->GetCollisionResponseToChannel(ECC_Visibility));SetBuildingVisibilityTraceResponse(*NewISM,ECR_Ignore);}
		NewISM->RegisterComponent();

		BuildingISMs.Add(Data, NewISM);
	}

	return NewISM;
}

bool UBuildingManagerSubsystem::IsBuildingInPresentationWindow(int32 BuildingID) const
{
	const auto* Building=ActiveBuildings.Find(BuildingID);const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	return Building&&Terrain&&GetAftershockDrawableCellBounds(*Terrain).Contains(Building->GridCoords)
		&&(!bUseDerivedBuildingPresentation||BuildingPresentationBounds.Contains(Building->GridCoords));
}
bool UBuildingManagerSubsystem::GetPresentedBuildingTransform(int32 BuildingID,FTransform& OutTransform) const
{
	const auto* B=ActiveBuildings.Find(BuildingID);if(!B||!B->BuildingData||B->DamageState>=EDamageState::Collapsed||!IsBuildingInPresentationWindow(BuildingID))return false;
	if(bUseDerivedBuildingPresentation)
	{
		const auto* Component=PresentationComponentByID.Find(BuildingID);const auto* Index=PresentationIndexByID.Find(BuildingID);
		return Component&&IsValid(*Component)&&Index&&(*Component)->GetInstanceTransform(*Index,OutTransform,true)&&!OutTransform.GetScale3D().IsNearlyZero();
	}
	const auto* Component=BuildingISMs.Find(B->BuildingData);
	return Component&&IsValid(*Component)&&(*Component)->GetInstanceTransform(B->ISMInstanceIndex,OutTransform,true)&&!OutTransform.GetScale3D().IsNearlyZero();
}
int32 UBuildingManagerSubsystem::GetPresentedBuildingCount() const
{int32 Count=0;FTransform T;for(const auto& Pair:ActiveBuildings)Count+=GetPresentedBuildingTransform(Pair.Key,T)?1:0;return Count;}
bool UBuildingManagerSubsystem::SyncPresentedBuilding(int32 BuildingID,bool bMarkRenderStateDirty)
{
	SyncArchitecturalBuilding(BuildingID);
	if(!bUseDerivedBuildingPresentation||bRefreshingBuildingPresentation)return false;
	const auto* B=ActiveBuildings.Find(BuildingID);const auto* ViewPtr=PresentationComponentByID.Find(BuildingID);const auto* Index=PresentationIndexByID.Find(BuildingID);
	if(!B||!B->BuildingData)return false;
	if(!ViewPtr||!IsValid(*ViewPtr)||!Index){if(B->DamageState<EDamageState::Collapsed&&BuildingPresentationBounds.Contains(B->GridCoords))bBuildingPresentationDirty=true;return false;}
	const auto* SourcePtr=BuildingISMs.Find(B->BuildingData);if(!SourcePtr||!IsValid(*SourcePtr))return false;
	const auto* ExpectedView=PresentationISMs.Find(B->BuildingData);if(!ExpectedView||*ExpectedView!=*ViewPtr){bBuildingPresentationDirty=true;return false;}
	FTransform Transform;if(!(*SourcePtr)->GetInstanceTransform(B->ISMInstanceIndex,Transform,true))return false;
	if(B->DamageState>=EDamageState::Collapsed||!BuildingPresentationBounds.Contains(B->GridCoords))Transform.SetScale3D(FVector::ZeroVector);
	bool Changed=false;FTransform Current;if(!(*ViewPtr)->GetInstanceTransform(*Index,Current,true)||!Current.Equals(Transform,0.f)){(*ViewPtr)->UpdateInstanceTransform(*Index,Transform,true,false,true);Changed=true;}
	for(int32 C=0;C<4;++C){const int32 At=B->ISMInstanceIndex*(*SourcePtr)->NumCustomDataFloats+C,ViewAt=*Index*(*ViewPtr)->NumCustomDataFloats+C;if((*SourcePtr)->PerInstanceSMCustomData.IsValidIndex(At)&&(!(*ViewPtr)->PerInstanceSMCustomData.IsValidIndex(ViewAt)||(*ViewPtr)->PerInstanceSMCustomData[ViewAt]!=(*SourcePtr)->PerInstanceSMCustomData[At])){(*ViewPtr)->SetCustomDataValue(*Index,C,(*SourcePtr)->PerInstanceSMCustomData[At],false);Changed=true;}}
	if(Changed&&bMarkRenderStateDirty)(*ViewPtr)->MarkRenderStateDirty();return Changed;
}
void UBuildingManagerSubsystem::RefreshVisibleBuildingPresentation(bool bForce)
{
	if(bRefreshingBuildingPresentation)return;
	const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;if(!Terrain)return;
	const auto Budget=GetAftershockPresentationBudget(GetWorld());
	if(bDrawBuildingDecorations!=Budget.bDrawDecorations){bDrawBuildingDecorations=Budget.bDrawDecorations;QueueDecorationRefresh(true);}
	const FIntRect Bounds=GetAftershockDrawableCellBounds(*Terrain),Initial(0,0,Terrain->GridWidth,Terrain->GridHeight);
	const bool UseViews=Bounds!=Initial;
	const bool bWindowChanged=Bounds!=BuildingPresentationBounds;
	const bool bStructureChanged=bForce||bBuildingPresentationDirty||UseViews!=bUseDerivedBuildingPresentation||bWindowChanged;
	const bool Rebuild=bStructureChanged;
	bUseDerivedBuildingPresentation=UseViews;BuildingPresentationBounds=Bounds;
	for(auto& Pair:BuildingISMs)if(IsValid(Pair.Value))
	{
		auto* Source=Pair.Value.Get();
		// Preserve the previous complete architectural replacement while checking
		// the new window. RefreshArchitecturalPresentation below reconciles new
		// ownership/fallback; unchanged hidden proxies must not toggle each pass.
		Source->SetVisibility(!UseViews&&!(ArchitecturalRenderer.IsValid()&&ArchitecturalRenderer->OwnsDefinition(Pair.Key)));
		ApplyBuildingLOD(*Source,Budget.PropLODFloor);
		if(UseViews){if(!CanonicalVisibilityResponses.Contains(Source))CanonicalVisibilityResponses.Add(Source,Source->GetCollisionResponseToChannel(ECC_Visibility));SetBuildingVisibilityTraceResponse(*Source,ECR_Ignore);}
		else if(const auto* Original=CanonicalVisibilityResponses.Find(Source)){SetBuildingVisibilityTraceResponse(*Source,*Original);CanonicalVisibilityResponses.Remove(Source);}
	}
	if(!UseViews)
	{
		if(!PresentationComponentByID.IsEmpty())for(auto& Pair:PresentationISMs)if(IsValid(Pair.Value)){Pair.Value->SetVisibility(false);Pair.Value->ClearInstances();}
		PresentationComponentByID.Reset();PresentationIndexByID.Reset();bBuildingPresentationDirty=false;
		if(bWindowChanged)QueueDecorationRefresh(true);
		RefreshArchitecturalPresentation(bStructureChanged);FlushPendingPresentationUpdates();return;
	}
	bool AnyViewChanged=Rebuild;
	if(Rebuild)
	{
		++PresentationWork.ViewRebuilds;
		TGuardValue<bool> Guard(bRefreshingBuildingPresentation,true);
		for(auto& Pair:PresentationISMs)if(IsValid(Pair.Value)){Pair.Value->ClearInstances();Pair.Value->SetVisibility(true);}
		PresentationComponentByID.Reset();PresentationIndexByID.Reset();
		for(const auto& Pair:ActiveBuildings)
		{
			const auto& B=Pair.Value;if(!B.BuildingData||B.DamageState>=EDamageState::Collapsed||!Bounds.Contains(B.GridCoords))continue;
			auto* Source=GetOrCreateISM(B.BuildingData);FTransform Transform;if(!Source||!Source->GetInstanceTransform(B.ISMInstanceIndex,Transform,true)||Transform.GetScale3D().IsNearlyZero())continue;
			auto& View=PresentationISMs.FindOrAdd(B.BuildingData);
			if(!IsValid(View))
			{
				auto* Host=GetOrCreateISMHostActor();if(!Host)continue;
				View=NewObject<UInstancedStaticMeshComponent>(Host,MakeUniqueObjectName(Host,UInstancedStaticMeshComponent::StaticClass(),TEXT("AftershockBuildingView")));
				View->SetupAttachment(Host->GetRootComponent());View->SetStaticMesh(Source->GetStaticMesh());View->NumCustomDataFloats=4;
				for(int32 M=0;M<Source->GetNumMaterials();++M)View->SetMaterial(M,Source->GetMaterial(M));
				View->SetCollisionEnabled(ECollisionEnabled::NoCollision);View->SetComponentTickEnabled(false);View->SetCastShadow(Source->CastShadow);Host->AddInstanceComponent(View);View->RegisterComponent();
			}
			View->SetVisibility(true);const int32 Index=View->AddInstance(Transform,true);PresentationComponentByID.Add(B.InstanceID,View);PresentationIndexByID.Add(B.InstanceID,Index);
			for(int32 C=0;C<4;++C){const int32 At=B.ISMInstanceIndex*Source->NumCustomDataFloats+C;if(Source->PerInstanceSMCustomData.IsValidIndex(At))View->SetCustomDataValue(Index,C,Source->PerInstanceSMCustomData[At],false);}
		}
		bBuildingPresentationDirty=false;
		for(auto& Pair:PresentationISMs)if(IsValid(Pair.Value))Pair.Value->MarkRenderStateDirty();
	}
	else
	{
		for(const auto& Pair:ActiveBuildings)if(PresentationIndexByID.Contains(Pair.Key))AnyViewChanged|=SyncPresentedBuilding(Pair.Key,false);
	}
	for(auto& Pair:PresentationISMs)if(IsValid(Pair.Value))ApplyBuildingLOD(*Pair.Value,Budget.PropLODFloor);
	if(Rebuild)QueueDecorationRefresh(true);
	RefreshArchitecturalPresentation(bStructureChanged);
	FlushPendingPresentationUpdates();
}

int32 UBuildingManagerSubsystem::CalculateConstructionCost(
	const UBuildingData* Data,
	EFoundationQuality Foundation,
	int32 Floors) const
{
	if (!Data)
	{
		return 0;
	}

	const float BaseCost = static_cast<float>(Data->CostTL);

	// BUILDING_STATS.md Bölüm 3 — Temel Maliyet Çarpanı
	float FoundationMultiplier = 1.3f;
	switch (Foundation)
	{
	case EFoundationQuality::Weak:
		FoundationMultiplier = 1.0f;
		break;
	case EFoundationQuality::Medium:
		FoundationMultiplier = 1.3f;
		break;
	case EFoundationQuality::Strong:
		FoundationMultiplier = 1.8f;
		break;
	}

	// Kat sayısı FloorsMin değerinden fazlaysa her ekstra kat için maliyet %15 artar
	const int32 BaseFloors = FMath::Max(1, Data->FloorsMin);
	const int32 ExtraFloors = FMath::Max(0, Floors - BaseFloors);
	const float FloorMultiplier = 1.0f + static_cast<float>(ExtraFloors) * 0.15f;

	return FMath::RoundToInt(BaseCost * FoundationMultiplier * FloorMultiplier);
}

bool UBuildingManagerSubsystem::CanPlaceBuildingAt(FIntPoint GridCoords, const UBuildingData* Data) const
{
	if (!Data)
	{
		return false;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// Owner-level gate also covers direct Blueprint/native callers. No fee, ID
	// allocation or terrain edit may precede this frozen-obstacle check.
	if (BuildingInputsFrozenByGranularEvent(World)) return false;

	UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>();
	if (!Terrain || !Terrain->IsPlayableCoord(GridCoords))
	{
		return false;
	}

	const FGridCell Cell = Terrain->GetCell(GridCoords);
	if (Cell.bIsEmergencyAssemblyArea) return false;
	if (Cell.bVolcanicVent) return false;
	if (Cell.bIsOcean||Cell.bBurning||Cell.LavaCoverage01>.1f||!FMath::IsFinite(Cell.Elevation)) return false;

	// Hücrede başka bina olmamalı ve nehir yatağı olmamalı
	if (Cell.OccupyingBuildingID != -1)
	{
		return false;
	}
	if (Cell.bHasRoad)
	{
		return false;
	}

	if (Cell.bIsRiverbed)
	{
		return false;
	}

	return true;
}

FAftershockBuildingRoadAccessQuote UBuildingManagerSubsystem::QuoteAutomaticRoadAccess(
	FIntPoint GridCoords,const UBuildingData* Data) const
{
	FAftershockBuildingRoadAccessQuote Quote;
	const UWorld* World=GetWorld();
	const auto* Terrain=World?World->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const auto* Roads=World?World->GetSubsystem<URoadNetworkSubsystem>():nullptr;
	if(!CanPlaceBuildingAt(GridCoords,Data)||!Terrain||!Roads)
	{
		Quote.Feedback=NSLOCTEXT("Aftershock","BuildingAccessInvalidPlot","Bu parsel yapı veya güvenli yol erişimi için uygun değil.");
		return Quote;
	}
	// Starting a city without any network is an explicit bootstrap, not a free
	// disconnected access-road stub. Keep the canonical quote usable by the same
	// preview and guarded purchase path; the player must draw the first road.
	if(Roads->CountRoadCells()==0)
	{
		Quote.bRouteValid=true;
		Quote.Feedback=NSLOCTEXT("Aftershock","BuildingAccessRoadlessBootstrap","Şehirde henüz yol ağı yok. Bina kurulabilir; ilk yolu elle çizin. Otomatik yol oluşturulmaz ve yol ücreti alınmaz.");
		return Quote;
	}
	// Coordinate-keyed storage is essential: canonical resident cells may include
	// negative global coordinates and non-contiguous continuation chunks. This is
	// bounded by work, never by total world/DEM size, and never allocates terrain.
	constexpr int32 MaximumSearchCells=16384,MaximumAccessSteps=512;
	struct FAccessNode {FIntPoint Coord;int32 Parent;int32 Depth;};
	TArray<FAccessNode> Queue;Queue.Reserve(256);
	TMap<FIntPoint,int32> Visited;Visited.Reserve(256);
	Queue.Add({GridCoords,INDEX_NONE,0});Visited.Add(GridCoords,0);
	const FIntPoint Steps[]={FIntPoint(1,0),FIntPoint(0,1),FIntPoint(-1,0),FIntPoint(0,-1)};
	int32 Found=INDEX_NONE;bool bLimitReached=false;
	for(int32 At=0;At<Queue.Num()&&Found==INDEX_NONE&&!bLimitReached;++At)
	{
		// Copy before Add: queue growth can invalidate a reference to this node.
		const FAccessNode Node=Queue[At];
		if(Node.Depth>=MaximumAccessSteps)continue;
		for(const FIntPoint Step:Steps)
		{
			const FIntPoint Next=Node.Coord+Step;
			// The terminal existing road must obey the same surface-grade bound
			// as a new extension; ordinary frontage alone does not check grade.
			if(Visited.Contains(Next)||!Terrain->IsPlayableCoord(Next)
				||!Roads->CanBuildAutomaticAccessEdge(Node.Coord,Next))continue;
			const bool bExistingRoad=Roads->HasRoad(Next);
			if(bExistingRoad)
			{
				// Only an open, usable road with a genuine bank/frontage entrance
				// terminates the extension. Closed/elevated bridge interiors do not.
				if(!Roads->IsRoadTraversable(Next)||!Roads->CanAccessRoadFromCell(Node.Coord,Next))continue;
			}
			else
			{
				if(!Roads->CanBuildAutomaticAccessCell(Next))continue;
			}
			if(Queue.Num()>=MaximumSearchCells){bLimitReached=true;break;}
			const int32 Added=Queue.Add({Next,At,Node.Depth+1});Visited.Add(Next,Added);
			if(bExistingRoad){Found=Added;break;}
		}
	}
	if(Found==INDEX_NONE)
	{
		Quote.Feedback=bLimitReached
			?NSLOCTEXT("Aftershock","BuildingAccessSearchBound","Yakındaki yola güvenli bağlantı arama sınırına ulaşıldı. Önce bu bölgeye bir ana yol uzatın.")
			:NSLOCTEXT("Aftershock","BuildingAccessNoRoad","Güvenli bir yol bağlantısı bulunamadı. Önce yol kurun; otomatik bağlantı nehir, dik yamaç veya ayrılmış parselleri aşamaz. Arama en fazla 512 adım sürer.");
		return Quote;
	}
	for(int32 Index=Found;Queue[Index].Parent!=INDEX_NONE;Index=Queue[Index].Parent)
		Quote.RoadPath.Add(Queue[Index].Coord);
	Algo::Reverse(Quote.RoadPath);
	Quote.NewRoadCells=Quote.RoadPath.Num()-1;
	Quote.RoadCostTL=Quote.NewRoadCells*Roads->GetRoadCellCostTL();
	Quote.bRouteValid=true;
	Quote.Feedback=Quote.NewRoadCells>0
		?FText::Format(NSLOCTEXT("Aftershock","BuildingAccessQuote","Otomatik erişim: {0} yol hücresi, +{1} TL."),FText::AsNumber(Quote.NewRoadCells),FText::AsNumber(Quote.RoadCostTL))
		:NSLOCTEXT("Aftershock","BuildingAccessExisting","Mevcut yola doğrudan erişim var.");
	return Quote;
}

bool UBuildingManagerSubsystem::PlaceBuildingWithRoadAccess(FIntPoint GridCoords,UBuildingData* Data,
	EFoundationQuality Foundation,int32 Floors,int32& OutAssignedID)
{
	OutAssignedID=INDEX_NONE;
	UWorld* World=GetWorld();
	auto* Terrain=World?World->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	auto* Economy=World?World->GetSubsystem<UEconomyAndStateSubsystem>():nullptr;
	auto* Roads=World?World->GetSubsystem<URoadNetworkSubsystem>():nullptr;
	if(bConstructionTransactionInProgress||!Data||!Terrain||!Economy||!Roads||Economy->bIsGameOver
		||Data->CostTL<0||Data->FloorsMin<1||Data->FloorsMax<Data->FloorsMin
		||static_cast<uint8>(Foundation)>static_cast<uint8>(EFoundationQuality::Strong))
	{
		return false;
	}
	const int32 ClampedFloors=FMath::Clamp(Floors,Data->FloorsMin,Data->FloorsMax);
	const int32 BuildingCost=CalculateConstructionCost(Data,Foundation,ClampedFloors);
	// Requote synchronously; no preview/cached coordinates authorize a purchase.
	const FAftershockBuildingRoadAccessQuote Quote=QuoteAutomaticRoadAccess(GridCoords,Data);
	const int64 TotalCost=static_cast<int64>(BuildingCost)+Quote.RoadCostTL;
	if(BuildingCost<0||!Quote.bRouteValid||!Economy->CanAfford(TotalCost))
	{
		return false;
	}
	TGuardValue<bool> Guard(bConstructionTransactionInProgress,true);
	TArray<FGridCell> OriginalRoadCells;OriginalRoadCells.Reserve(Quote.NewRoadCells);
	const auto RollbackRoads=[&](){for(const FGridCell& Cell:OriginalRoadCells)Terrain->SetCell(Cell.Coords,Cell);};
	// The terminal road and all bridge metadata remain untouched. Each preceding
	// cell was quoted as empty resident land, so no bridge or free road is hidden.
	for(int32 I=0;I<Quote.NewRoadCells;++I)
	{
		const FIntPoint Coord=Quote.RoadPath[I];
		if(!Roads->CanBuildAutomaticAccessCell(Coord)){RollbackRoads();return false;}
		FGridCell Cell=Terrain->GetCell(Coord);OriginalRoadCells.Add(Cell);Cell.bHasRoad=true;
		if(!Terrain->SetCell(Coord,Cell)){RollbackRoads();return false;}
	}
	const int32 PreviousNextID=NextBuildingID;
	if(!PlaceBuilding(GridCoords,Data,Foundation,ClampedFloors,OutAssignedID))
	{
		RollbackRoads();OutAssignedID=INDEX_NONE;return false;
	}
	// No gameplay delegate has fired since CanAfford: this debit cannot normally
	// fail. Keep the rollback for a future owner change that can reject spending.
	if(!Economy->TrySpendMoney(TotalCost))
	{
		RemoveBuilding(OutAssignedID);NextBuildingID=PreviousNextID;
		RollbackRoads();OutAssignedID=INDEX_NONE;return false;
	}
	if(Quote.NewRoadCells>0)Roads->NotifyBuildingAccessCommitted();
	else if(auto* Life=World->GetSubsystem<UCityLifeSubsystem>();Life&&Life->IsCityInitialized())Life->RefreshCity();
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_BUILDING_ACCESS_PURCHASE id=%d plot=%d,%d accessCells=%d buildingCost=%d roadCost=%d totalCost=%lld debitCount=1"),
		OutAssignedID,GridCoords.X,GridCoords.Y,Quote.NewRoadCells,BuildingCost,Quote.RoadCostTL,TotalCost);
	return true;
}

FTransform UBuildingManagerSubsystem::GetBuildingVisualTransform(FIntPoint Coord, UBuildingData* Data, int32 Floors, float Yaw) const
{
	const UTerrainGridSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || !Data) return FTransform::Identity;
	UStaticMesh* Mesh = Data->MeshIntact.LoadSynchronous();
	const bool bCube = !Mesh || Mesh->GetName().Contains(TEXT("Cube"));
	const FVector Ground = Terrain->GridToWorld(Coord, true);
	const int32 Count = FMath::Clamp(Floors, Data->FloorsMin, Data->FloorsMax);
	if (Terrain->CellSize < 999.f)
	{
		if (bCube)
		{
			const float XY = Terrain->CellSize * .0065f, Z = Count * 1.5f;
			return FTransform(FRotator(0,Yaw,0), Ground + FVector(0,0,Z*50.f), FVector(XY,XY,Z));
		}
		return FTransform(FRotator(0,Yaw,0), Ground + FVector(0,0,0.5f), FVector(1,1,static_cast<float>(Count)/FMath::Max(1,Data->FloorsMin)));
	}
	const FBox Bounds = bCube ? FBox(FVector(-50),FVector(50)) : Mesh->GetBoundingBox();
	const FVector Size = Bounds.GetSize();
	// A road-connected city lot has margins; utility equipment keeps its native
	// human scale. Multi-cell civic campuses remain a separate parcel feature.
	const bool bEquipment = Data->Type == EBuildingType::Generator || Data->Type == EBuildingType::BusStop
		|| Data->Type == EBuildingType::Substation || Data->Type == EBuildingType::WindTurbine;
	const float XY = bEquipment && !bCube ? 1.f : Terrain->CellSize * .76f / FMath::Max(1.f,FMath::Max(Size.X,Size.Y));
	float Height = FMath::Max(1.f,Size.Z) * static_cast<float>(Count)/FMath::Max(1,Data->FloorsMin);
	switch (Data->Type)
	{
	case EBuildingType::House: Height = Count*300.f+100.f; break;
	case EBuildingType::Apartment: case EBuildingType::Office: Height = Count*310.f+60.f; break;
	case EBuildingType::Factory: case EBuildingType::Warehouse: Height = Count*450.f; break;
	case EBuildingType::Hospital: case EBuildingType::School: case EBuildingType::FireStation:
	case EBuildingType::Shelter: case EBuildingType::Bakery: case EBuildingType::Restaurant:
	case EBuildingType::TaxOffice: case EBuildingType::Courthouse: case EBuildingType::PoliceStation:
		Height = Count*330.f+60.f; break;
	default: break;
	}
	// The imported core buildings have a footing below local Z0. Their authored
	// grade is the pivot, not the bottom of that buried footing. Keep the source
	// grade at terrain height and size the visible structure above it; otherwise
	// the foundation becomes a visible extra storey and GC anchors leave ground.
	const bool bHasBuriedFooting=!bCube&&Bounds.Min.Z<-.5&&Bounds.Max.Z>.5;
	const float VisibleSourceHeight=bHasBuriedFooting?Bounds.Max.Z:Size.Z;
	const FVector Scale(XY,XY,Height/FMath::Max(1.f,VisibleSourceHeight));
	const FVector Pivot=bHasBuriedFooting?Ground:Ground-FVector(0,0,Bounds.Min.Z*Scale.Z);
	return FTransform(FRotator(0,Yaw,0),Pivot,Scale);
}

bool UBuildingManagerSubsystem::RaycastBuilding(const FVector& Origin, const FVector& Direction,
	float MaxDistance, FBuildingInstance& OutBuilding, float& OutDistance) const
{
	OutBuilding={};OutDistance=FMath::Max(0.f,MaxDistance);
	if (Origin.ContainsNaN() || Direction.ContainsNaN() || !FMath::IsFinite(MaxDistance) || MaxDistance<=0) return false;
	const FVector Ray=Direction.GetSafeNormal(); if (Ray.IsNearlyZero()) return false;
	bool Found=false;
	for (const auto& Pair:ActiveBuildings)
	{
		const auto& Building=Pair.Value;
		if (!Building.BuildingData || Building.DamageState==EDamageState::Collapsed || !IsBuildingInPresentationWindow(Building.InstanceID)) continue;
		const auto* Batch=BuildingISMs.Find(Building.BuildingData);
		if (!Batch || !IsValid(*Batch) || !(*Batch)->GetStaticMesh()) continue;
		FTransform Transform;FBox Box;
		if(Building.DamageState==EDamageState::Memorial)
		{
			const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;if(!Terrain)continue;
			Transform=FTransform(Terrain->GridToWorld(Building.GridCoords,true));Box=FBox(FVector(-90,-65,0),FVector(90,65,30));
		}
		else
		{
			if (!GetPresentedBuildingTransform(Building.InstanceID,Transform) || Transform.GetScale3D().GetAbsMin()<KINDA_SMALL_NUMBER) continue;
			Box=(*Batch)->GetStaticMesh()->GetBoundingBox().ExpandBy(1.f);
		}
		const FVector Start=Transform.InverseTransformPosition(Origin), Step=Transform.InverseTransformVector(Ray);
		double Enter=0, Exit=OutDistance;
		for (int32 Axis=0;Axis<3&&Enter<=Exit;++Axis)
		{
			if (FMath::Abs(Step[Axis])<UE_DOUBLE_SMALL_NUMBER)
			{if (Start[Axis]<Box.Min[Axis] || Start[Axis]>Box.Max[Axis]) Exit=-1;continue;}
			double A=(Box.Min[Axis]-Start[Axis])/Step[Axis],B=(Box.Max[Axis]-Start[Axis])/Step[Axis];
			if (A>B) Swap(A,B); Enter=FMath::Max(Enter,A);Exit=FMath::Min(Exit,B);
		}
		if (Enter>Exit || Exit<0 || Enter>MaxDistance) continue;
		if (!Found || Enter<OutDistance-.01 || (FMath::Abs(Enter-OutDistance)<=.01&&Building.InstanceID<OutBuilding.InstanceID))
		{Found=true;OutDistance=static_cast<float>(Enter);OutBuilding=Building;}
	}
	return Found;
}

bool UBuildingManagerSubsystem::PlaceBuilding(
	FIntPoint GridCoords,
	UBuildingData* Data,
	EFoundationQuality Foundation,
	int32 Floors,
	int32& OutAssignedID)
{
	OutAssignedID = -1;

	if (!CanPlaceBuildingAt(GridCoords, Data))
	{
		return false;
	}
	if(NextBuildingID<1||NextBuildingID==MAX_int32)return false;

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>();
	if (!Terrain)
	{
		return false;
	}

	UInstancedStaticMeshComponent* ISM = GetOrCreateISM(Data);
	if (!ISM)
	{
		return false;
	}

	const int32 ProposedID=NextBuildingID;

	const int32 ClampedFloors = FMath::Clamp(Floors, Data->FloorsMin, Data->FloorsMax);

	FBuildingInstance NewBuilding;
	NewBuilding.InstanceID = ProposedID;
	NewBuilding.GridCoords = GridCoords;
	NewBuilding.BuildingData = Data;
	NewBuilding.CurrentFloors = ClampedFloors;
	NewBuilding.Foundation = Foundation;
	NewBuilding.FoundationQuality = Foundation;
	NewBuilding.StructuralQuality = 1.0f;
	NewBuilding.RoofQuality = 1.0f;
	NewBuilding.Preparedness = 0.0f;
	NewBuilding.AgeInYears = 0;
	NewBuilding.bIsRetrofitted = false;
	NewBuilding.CurrentDamage = 0.0f;
	NewBuilding.DamageState = EDamageState::Intact;

	const FTransform InstanceTransform = GetBuildingVisualTransform(GridCoords,Data,ClampedFloors);

	// ISM'e ekle
	auto& FreeSlots=FreeBuildingInstanceIndices.FindOrAdd(Data);
	int32 ISMIndex=INDEX_NONE;
	while(!FreeSlots.IsEmpty())
	{
		const int32 Candidate=FreeSlots.Pop(EAllowShrinking::No);
		if(Candidate>=0&&Candidate<ISM->GetInstanceCount()){ISMIndex=Candidate;break;}
	}
	if(ISMIndex==INDEX_NONE)ISMIndex=ISM->AddInstance(InstanceTransform,true);
	else if(!ISM->UpdateInstanceTransform(ISMIndex,InstanceTransform,true,false,true))
	{
		FreeSlots.Add(ISMIndex);return false;
	}
	if(ISMIndex<0||ISMIndex>=ISM->GetInstanceCount())return false;
	NewBuilding.ISMInstanceIndex = ISMIndex;
	FGridCell Cell=Terrain->GetCell(GridCoords);Cell.OccupyingBuildingID=ProposedID;
	if(!Terrain->SetCell(GridCoords,Cell))
	{
		FTransform Hidden;Hidden.SetScale3D(FVector::ZeroVector);
		ISM->UpdateInstanceTransform(ISMIndex,Hidden,true,true,true);FreeSlots.Add(ISMIndex);
		return false;
	}
	OutAssignedID=ProposedID;++NextBuildingID;

	// Custom Primitive Data: Index 0 = Hasar (0.0), Index 1 = Retrofit (0.0)
	ISM->SetCustomDataValue(ISMIndex, 0, 0.0f, true);
	ISM->SetCustomDataValue(ISMIndex, 1, 0.0f, false);
	ISM->SetCustomDataValue(ISMIndex, 2, 1.0f, false);
	ISM->SetCustomDataValue(ISMIndex, 3, 1.0f, true);

	// Kayıtları güncelle
	ActiveBuildings.Add(OutAssignedID, NewBuilding);
	bBuildingPresentationDirty=true;

	bArchitecturalLayoutDirty=true;QueueDecorationRefresh();
	if(bUseDerivedBuildingPresentation)RefreshVisibleBuildingPresentation();

	if (UCityLifeSubsystem* Life = World->GetSubsystem<UCityLifeSubsystem>())
	{
		if (Life->IsCityInitialized()&&!bConstructionTransactionInProgress)
		{
			Life->RefreshCity();
		}
	}

	return true;
}

bool UBuildingManagerSubsystem::RetrofitBuilding(int32 BuildingID, int32& OutCostTL)
{
	OutCostTL = 0;
	if (BuildingInputsFrozenByGranularEvent(GetWorld())) return false;

	FBuildingInstance* FoundBuilding = ActiveBuildings.Find(BuildingID);
	if (!FoundBuilding || FoundBuilding->bIsRetrofitted || FoundBuilding->DamageState>=EDamageState::Collapsed)
	{
		return false;
	}

	// BUILDING_STATS.md Bölüm 4:
	// Retrofit maliyeti 1.000 TL, yıpranma sıfırlanır
	FoundBuilding->bIsRetrofitted = true;
	FoundBuilding->AgeInYears = 0;
	if(auto* Ledger=GetWorld()->GetSubsystem<UCasualtyLedgerGenerator>())Ledger->RecordRetrofitInvestment(BuildingID);
	OutCostTL = 1000;

	// ISM Custom Primitive Data Index 1 = 1.0f (Retrofit yapıldı)
	if (FoundBuilding->BuildingData && FoundBuilding->ISMInstanceIndex != -1)
	{
		if (UInstancedStaticMeshComponent* ISM = GetOrCreateISM(FoundBuilding->BuildingData))
		{
			ISM->SetCustomDataValue(FoundBuilding->ISMInstanceIndex, 1, 1.0f, true);
			SyncPresentedBuilding(BuildingID);
		}
	}

	return true;
}

bool UBuildingManagerSubsystem::RepairBuilding(int32 BuildingID, int32& OutCostTL)
{
	OutCostTL = 0;
	if (BuildingInputsFrozenByGranularEvent(GetWorld())) return false;

	FBuildingInstance* FoundBuilding = ActiveBuildings.Find(BuildingID);
	if (!FoundBuilding)
	{
		return false;
	}

	// BUILDING_STATS.md Bölüm 5:
	// 20-50: Hasarlı (geçici kapalı, onarım 2.000 TL)
	// 50-80: Ağır hasar (onarım 5.000 TL)
	if (FoundBuilding->CurrentDamage >= 20.0f && FoundBuilding->CurrentDamage <= 50.0f)
	{
		OutCostTL = 2000;
	}
	else if (FoundBuilding->CurrentDamage > 50.0f && FoundBuilding->CurrentDamage < 80.0f)
	{
		OutCostTL = 5000;
	}
	else if (FoundBuilding->CurrentDamage > 0.0f && FoundBuilding->CurrentDamage < 20.0f)
	{
		OutCostTL = 500;
	}
	else
	{
		return false;
	}

	FoundBuilding->CurrentDamage = 0.0f;
	FoundBuilding->DamageState = EDamageState::Intact;

	UpdateBuildingVisualState(BuildingID);

	return true;
}

void UBuildingManagerSubsystem::UpdateBuildingVisualState(int32 BuildingID)
{
	FBuildingInstance* FoundBuilding = ActiveBuildings.Find(BuildingID);
	if (!FoundBuilding || !FoundBuilding->BuildingData || FoundBuilding->ISMInstanceIndex == -1)
	{
		return;
	}

	UInstancedStaticMeshComponent* ISM = GetOrCreateISM(FoundBuilding->BuildingData);
	if (!ISM)
	{
		return;
	}

	const float DamageRatio = FMath::Clamp(FoundBuilding->CurrentDamage / 100.0f, 0.0f, 1.0f);
	ISM->SetCustomDataValue(FoundBuilding->ISMInstanceIndex, 0, DamageRatio, false);
	ISM->SetCustomDataValue(FoundBuilding->ISMInstanceIndex, 1, FoundBuilding->bIsRetrofitted ? 1.0f : 0.0f, true);

	// Eğer bina EDamageState::Collapsed (80-100) olduysa instance ölçeğini FVector::ZeroVector yaparak gizle
	// (Swap-and-pop yüzünden diğer binaların ISMInstanceIndex sırasının bozulmaması için instance silinmez)
	if (FoundBuilding->DamageState == EDamageState::Collapsed || FoundBuilding->DamageState == EDamageState::Memorial)
	{
		FTransform CurrentTransform;
		if (ISM->GetInstanceTransform(FoundBuilding->ISMInstanceIndex, CurrentTransform, true))
		{
			CurrentTransform.SetScale3D(FVector::ZeroVector);
			ISM->UpdateInstanceTransform(FoundBuilding->ISMInstanceIndex, CurrentTransform, true, true, true);
		}
		if(FoundBuilding->DamageState==EDamageState::Memorial)QueueDecorationRefresh(true);
	}
	else
	{
		FTransform CurrentTransform;
		if (ISM->GetInstanceTransform(FoundBuilding->ISMInstanceIndex, CurrentTransform, true) && CurrentTransform.GetScale3D().IsNearlyZero())
		{
			CurrentTransform = GetBuildingVisualTransform(FoundBuilding->GridCoords,FoundBuilding->BuildingData,FoundBuilding->CurrentFloors,CurrentTransform.Rotator().Yaw);
			ISM->UpdateInstanceTransform(FoundBuilding->ISMInstanceIndex, CurrentTransform, true, true, true);
		}
	}
	SyncPresentedBuilding(BuildingID);
	QueueDecorationRefresh();
}

void UBuildingManagerSubsystem::ApplyDamageToBuilding(int32 BuildingID, float NewDamage)
{
	if (FBuildingInstance* Found = ActiveBuildings.Find(BuildingID))
	{
		if(Found->DamageState==EDamageState::Memorial)return;
		if(!FMath::IsFinite(NewDamage))return;
		Found->CurrentDamage = FMath::Clamp(NewDamage, 0.0f, 100.0f);
		if (Found->CurrentDamage <= 0.0f)
		{
			Found->DamageState = EDamageState::Intact;
		}
		else if (Found->CurrentDamage <= 20.0f)
		{
			Found->DamageState = EDamageState::Cracked;
		}
		else if (Found->CurrentDamage <= 50.0f)
		{
			Found->DamageState = EDamageState::Damaged;
		}
		else if (Found->CurrentDamage < 80.0f)
		{
			Found->DamageState = EDamageState::Severe;
		}
		else
		{
			Found->DamageState = EDamageState::Collapsed;
		}

		UpdateBuildingVisualState(BuildingID);
	}
}

bool UBuildingManagerSubsystem::GetBuildingByID(int32 BuildingID, FBuildingInstance& OutBuilding) const
{
	if (const FBuildingInstance* Found = ActiveBuildings.Find(BuildingID))
	{
		OutBuilding = *Found;
		return true;
	}
	return false;
}

bool UBuildingManagerSubsystem::GetBuildingAtCoords(FIntPoint GridCoords, FBuildingInstance& OutBuilding) const
{
	for (const auto& Pair : ActiveBuildings)
	{
		if (Pair.Value.GridCoords == GridCoords)
		{
			OutBuilding = Pair.Value;
			return true;
		}
	}
	return false;
}

void UBuildingManagerSubsystem::GetAllBuildings(TArray<FBuildingInstance>& OutBuildings) const
{
	OutBuildings.Reset(ActiveBuildings.Num());
	for (const auto& Pair : ActiveBuildings)
	{
		OutBuildings.Add(Pair.Value);
	}
}

bool UBuildingManagerSubsystem::UpdateBuildingInstance(const FBuildingInstance& UpdatedBuilding)
{
	if (FBuildingInstance* Found = ActiveBuildings.Find(UpdatedBuilding.InstanceID))
	{
		if(BuildingInputsFrozenByGranularEvent(GetWorld())
            &&!AftershockPhysicalLifecycle::PreservesFrozenBuildingInputs(*Found,UpdatedBuilding))return false;
		if(Found->DamageState==EDamageState::Memorial&&UpdatedBuilding.DamageState!=EDamageState::Memorial)return false;
		const bool StructureChanged=Found->BuildingData!=UpdatedBuilding.BuildingData||Found->GridCoords!=UpdatedBuilding.GridCoords||Found->ISMInstanceIndex!=UpdatedBuilding.ISMInstanceIndex;
		*Found = UpdatedBuilding;
		bBuildingPresentationDirty|=StructureChanged;
		return true;
	}
	return false;
}

bool UBuildingManagerSubsystem::RemoveBuilding(int32 BuildingID)
{
    // Includes automatic memorial expiry: defer removal until the event ends.
    // Progressive physical damage and final settlement remain enabled.
    if(BuildingInputsFrozenByGranularEvent(GetWorld()))return false;
	if (FBuildingInstance* Found = ActiveBuildings.Find(BuildingID))
	{
		if(Found->DamageState==EDamageState::Memorial && GetMemorialTurnsRemaining(BuildingID)>0)return false;
		// Cars cannot materialise on an occupied sidewalk when their actual garage
		// is demolished. A blocked exit keeps the plot until a safe entry exists.
		if(auto* Transport=GetWorld()->GetSubsystem<UTransportSubsystem>())if(!Transport->EvictBuildingParking(BuildingID))return false;
		const FIntPoint Coords = Found->GridCoords;
		if (Found->BuildingData && Found->ISMInstanceIndex != -1)
		{
			if (UInstancedStaticMeshComponent* ISM = GetOrCreateISM(Found->BuildingData))
			{
				FTransform ZeroTransform;
				ZeroTransform.SetScale3D(FVector::ZeroVector);
				ISM->UpdateInstanceTransform(Found->ISMInstanceIndex, ZeroTransform, true, true, true);
				auto& FreeSlots=FreeBuildingInstanceIndices.FindOrAdd(Found->BuildingData.Get());FreeSlots.Add(Found->ISMInstanceIndex);
				if(FreeSlots.Num()==ISM->GetInstanceCount()){ISM->ClearInstances();FreeSlots.Empty();}
			}
		}

		UWorld* World = GetWorld();
		if (World)
		{
			if (UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>())
			{
				if (Terrain->IsValidCoord(Coords))
				{
					FGridCell Cell = Terrain->GetCell(Coords);
					if (Cell.OccupyingBuildingID == BuildingID)
					{
						Cell.OccupyingBuildingID = -1;
						Terrain->SetCell(Coords, Cell);
					}
				}
			}
		}

		if(const auto* View=PresentationComponentByID.Find(BuildingID))if(IsValid(*View))if(const auto* Index=PresentationIndexByID.Find(BuildingID)){FTransform Hidden;Hidden.SetScale3D(FVector::ZeroVector);(*View)->UpdateInstanceTransform(*Index,Hidden,true,true,true);}
		PresentationComponentByID.Remove(BuildingID);PresentationIndexByID.Remove(BuildingID);bBuildingPresentationDirty=true;
		ActiveBuildings.Remove(BuildingID);
		SyncArchitecturalBuilding(BuildingID);bArchitecturalLayoutDirty=true;
		QueueDecorationRefresh(true);
		if (World)
		{
			if (UCityLifeSubsystem* Life = World->GetSubsystem<UCityLifeSubsystem>())
			{
				if (Life->IsCityInitialized()&&!bAdvancingMemorials&&!bConstructionTransactionInProgress)
				{
					Life->RefreshCity();
				}
			}
		}
		return true;
	}
	return false;
}

bool UBuildingManagerSubsystem::BeginMemorial(int32 ID,int32 Turn)
{
	auto* B=ActiveBuildings.Find(ID);if(!B||B->DamageState!=EDamageState::Collapsed||Turn<1||Turn>MAX_int32-3)return false;
	B->DamageState=EDamageState::Memorial;B->MemorialStartedTurn=Turn;B->MemorialUnlockTurn=Turn+3;
	if(const auto* Ledger=GetWorld()->GetSubsystem<UCasualtyLedgerGenerator>())B->MemorialCasualtyResidentIDs=Ledger->GetBuildingCasualtyIDs(ID);
	UpdateBuildingVisualState(ID);return true;
}
int32 UBuildingManagerSubsystem::GetMemorialTurnsRemaining(int32 ID) const
{
	const auto* B=ActiveBuildings.Find(ID);const auto* E=GetWorld()?GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>():nullptr;
	return B&&B->DamageState==EDamageState::Memorial?FMath::Max(0,B->MemorialUnlockTurn-(E?E->CurrentTurn:B->MemorialStartedTurn)):0;
}
TArray<int32> UBuildingManagerSubsystem::GetMemorialCasualtyIDs(int32 ID) const
{const auto* B=ActiveBuildings.Find(ID);return B&&B->DamageState==EDamageState::Memorial?B->MemorialCasualtyResidentIDs:TArray<int32>();}
void UBuildingManagerSubsystem::AdvanceMemorialTurn(int32 Turn)
{
	TArray<int32> Expired;for(const auto& Pair:ActiveBuildings)if(Pair.Value.DamageState==EDamageState::Memorial&&Turn>=Pair.Value.MemorialUnlockTurn)Expired.Add(Pair.Key);
	if(Expired.IsEmpty())return;
	{TGuardValue<bool> Guard(bAdvancingMemorials,true);for(int32 ID:Expired)RemoveBuilding(ID);}
	RebuildMemorialVisuals();if(auto* Life=GetWorld()->GetSubsystem<UCityLifeSubsystem>())if(Life->IsCityInitialized())Life->RefreshCity();
}
void UBuildingManagerSubsystem::QueueDecorationRefresh(bool bMemorial)
{
	++PresentationWork.DecorationRequests;bDamageDecorationsDirty=true;bMemorialDecorationsDirty|=bMemorial;
	if(UWorld* World=GetWorld())if(!World->GetTimerManager().IsTimerActive(DecorationRefreshTimer))
		DecorationRefreshTimer=World->GetTimerManager().SetTimerForNextTick(this,&UBuildingManagerSubsystem::FlushPendingPresentationUpdates);
}
void UBuildingManagerSubsystem::FlushPendingPresentationUpdates()
{
	if(bFlushingDecorations||(!bDamageDecorationsDirty&&!bMemorialDecorationsDirty))return;
	TGuardValue<bool> Guard(bFlushingDecorations,true);
	if(UWorld* World=GetWorld())World->GetTimerManager().ClearTimer(DecorationRefreshTimer);
	++PresentationWork.DecorationFlushes;
	const bool Damage=bDamageDecorationsDirty,Memorial=bMemorialDecorationsDirty;
	bDamageDecorationsDirty=false;bMemorialDecorationsDirty=false;
	if(Damage)RebuildDamagedBuildingVisuals();if(Memorial)RebuildMemorialVisuals();
	RefreshArchitecturalPresentation();
}
void UBuildingManagerSubsystem::RebuildMemorialVisuals()
{
	bMemorialDecorationsDirty=false;++PresentationWork.MemorialRebuilds;
	if(!GetAftershockPresentationBudget(GetWorld()).bDrawDecorations){if(MemorialCandles&&MemorialCandles->GetInstanceCount()>0)MemorialCandles->ClearInstances();if(MemorialFlowers&&MemorialFlowers->GetInstanceCount()>0)MemorialFlowers->ClearInstances();return;}
	bool HasMemorial=false;for(const auto& Pair:ActiveBuildings){++PresentationWork.MemorialBuildingsVisited;HasMemorial|=Pair.Value.DamageState==EDamageState::Memorial;}
	if(!HasMemorial){if(MemorialCandles)MemorialCandles->ClearInstances();if(MemorialFlowers)MemorialFlowers->ClearInstances();return;}
	AActor* Host=GetOrCreateISMHostActor();const auto* Terrain=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();if(!Host||!Terrain)return;
	const auto CreateBatch=[Host](const TCHAR* Name,const TCHAR* MeshPath)
	{auto* C=NewObject<UInstancedStaticMeshComponent>(Host,Name);C->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,MeshPath));C->SetCollisionEnabled(ECollisionEnabled::NoCollision);C->SetComponentTickEnabled(false);C->SetCastShadow(true);C->SetupAttachment(Host->GetRootComponent());Host->AddInstanceComponent(C);C->RegisterComponent();return C;};
	if(!MemorialCandles)MemorialCandles=CreateBatch(TEXT("AftershockMemorialCandles"),TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if(!MemorialFlowers)MemorialFlowers=CreateBatch(TEXT("AftershockMemorialFlowersAndToys"),TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	TArray<FTransform> CandleTransforms,FlowerTransforms;
	const auto* Ledger=GetWorld()->GetSubsystem<UCasualtyLedgerGenerator>();
	const FIntRect Visible=GetAftershockDrawableCellBounds(*Terrain);
	for(const auto& Pair:ActiveBuildings)
	{
		++PresentationWork.MemorialBuildingsVisited;
		if(Pair.Value.DamageState!=EDamageState::Memorial||!Visible.Contains(Pair.Value.GridCoords))continue;
		const FVector Ground=Terrain->GridToWorld(Pair.Value.GridCoords,true);const int32 Count=FMath::Clamp(Pair.Value.MemorialCasualtyResidentIDs.Num(),1,12);
		for(int32 I=0;I<Count;++I)
		{
			const FVector P=Ground+FVector((I%4-1.5f)*35.f,(I/4-1.f)*35.f,0);
			CandleTransforms.Emplace(FQuat::Identity,P+FVector(0,0,10),FVector(.07,.07,.20));
			FlowerTransforms.Emplace(FQuat::Identity,P+FVector(13,0,6),FVector(.18,.18,.06));
			if(Pair.Value.MemorialCasualtyResidentIDs.IsValidIndex(I)&&Ledger&&Ledger->HasFlowerTribute(Pair.Value.MemorialCasualtyResidentIDs[I]))FlowerTransforms.Emplace(FQuat::Identity,P+FVector(13,15,6),FVector(.18,.18,.06));
			FResidentIdentityCard Card;
			if(Pair.Value.MemorialCasualtyResidentIDs.IsValidIndex(I)&&Ledger&&Ledger->GetResidentIdentityCard(Pair.Value.MemorialCasualtyResidentIDs[I],Card)&&Card.Age<18)
			{
				// A tiny shared-mesh toy beside the candle, never a person actor.
				FlowerTransforms.Emplace(FQuat::Identity,P+FVector(-13,0,6),FVector(.12,.10,.12));
				FlowerTransforms.Emplace(FQuat::Identity,P+FVector(-13,0,14),FVector(.09,.09,.09));
			}
		}
	}
	SubmitDecorationBatch(*MemorialCandles,CandleTransforms);SubmitDecorationBatch(*MemorialFlowers,FlowerTransforms);
}

void UBuildingManagerSubsystem::RebuildDamagedBuildingVisuals()
{
	++PresentationWork.DamagedRebuilds;
	if(!GetAftershockPresentationBudget(GetWorld()).bDrawDecorations){if(DamagedBuildingSmokeISM&&DamagedBuildingSmokeISM->GetInstanceCount()>0)DamagedBuildingSmokeISM->ClearInstances();if(DamagedBuildingDebrisISM&&DamagedBuildingDebrisISM->GetInstanceCount()>0)DamagedBuildingDebrisISM->ClearInstances();return;}
	bool bHasDamaged = false;
	for (const auto& Pair : ActiveBuildings)
	{
		++PresentationWork.DamagedBuildingsVisited;
		if (Pair.Value.DamageState == EDamageState::Damaged || Pair.Value.DamageState == EDamageState::Severe)
		{
			bHasDamaged = true;
			break;
		}
	}
	if (!bHasDamaged)
	{
		if (DamagedBuildingSmokeISM) DamagedBuildingSmokeISM->ClearInstances();
		if (DamagedBuildingDebrisISM) DamagedBuildingDebrisISM->ClearInstances();
		return;
	}
	AActor* Host = GetOrCreateISMHostActor();
	const UTerrainGridSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Host || !Terrain) return;

	const auto CreateBatch = [Host](const TCHAR* Name, const TCHAR* MeshPath, const TCHAR* MatPath)
	{
		auto* C = NewObject<UInstancedStaticMeshComponent>(Host, Name);
		C->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, MeshPath));
		if (MatPath)
		{
			if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, MatPath)) C->SetMaterial(0, Mat);
		}
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetComponentTickEnabled(false);
		C->SetCastShadow(false);
		C->SetupAttachment(Host->GetRootComponent());
		Host->AddInstanceComponent(C);
		C->RegisterComponent();
		return C;
	};

	if (!DamagedBuildingSmokeISM)
	{
		DamagedBuildingSmokeISM = CreateBatch(TEXT("AftershockDamagedBuildingSmoke"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder"), TEXT("/Game/FX/Aftershock/Materials/M_DisasterVolcanicAsh.M_DisasterVolcanicAsh"));
		DamagedBuildingSmokeISM->SetNumCustomDataFloats(1);
	}
	if (!DamagedBuildingDebrisISM)
	{
		DamagedBuildingDebrisISM = CreateBatch(TEXT("AftershockDamagedBuildingDebris"), TEXT("/Engine/BasicShapes/Cube.Cube"), nullptr);
	}
	TArray<FTransform> SmokeTransforms,DebrisTransforms;TArray<float> SmokeStrengths;
	const FIntRect Visible=GetAftershockDrawableCellBounds(*Terrain);

	for (const auto& Pair : ActiveBuildings)
	{
		++PresentationWork.DamagedBuildingsVisited;
		const FBuildingInstance& Building = Pair.Value;
		if (Building.DamageState != EDamageState::Damaged && Building.DamageState != EDamageState::Severe) continue;
		if (!Building.BuildingData||!Visible.Contains(Building.GridCoords)) continue;

		const FVector Ground = Terrain->GridToWorld(Building.GridCoords, true);
		const float RoofHeight = Building.CurrentFloors * 300.0f;
		const FVector RoofCenter = Ground + FVector(0.f, 0.f, RoofHeight);

		const bool bSevere = Building.DamageState == EDamageState::Severe;
		const float SmokeScale = bSevere ? 3.5f : 1.8f;
		const float SmokeHeight = bSevere ? 800.f : 400.f;

		SmokeTransforms.Emplace(FRotator(0.f,Building.InstanceID*45.f,0.f),RoofCenter+FVector(0.f,0.f,SmokeHeight*.5f),FVector(SmokeScale,SmokeScale,SmokeHeight/100.f));
		SmokeStrengths.Add(bSevere?.9f:.5f);

		const bool bSlopedRoof = Building.BuildingData->Type == EBuildingType::House;
		const float DebrisOffset = bSlopedRoof ? 180.f : 120.f;
		for (int32 Corner = 0; Corner < (bSevere ? 4 : 2); ++Corner)
		{
			const float CX = (Corner % 2 == 0 ? 1.f : -1.f) * DebrisOffset;
			const float CY = (Corner / 2 == 0 ? 1.f : -1.f) * DebrisOffset;
			const FVector DebrisLoc = bSlopedRoof
				? (Ground + FVector(CX, CY, 12.f))
				: (RoofCenter + FVector(CX, CY, 20.f));
			DebrisTransforms.Emplace(FRotator(Corner*30.f,Corner*45.f,Corner*15.f),DebrisLoc,FVector(.35f,.35f,.25f));
		}
	}
	SubmitDecorationBatch(*DamagedBuildingSmokeISM,SmokeTransforms,&SmokeStrengths);SubmitDecorationBatch(*DamagedBuildingDebrisISM,DebrisTransforms);
}

void UBuildingManagerSubsystem::AgeAllBuildingsOneYear()
{
	for (auto& Pair : ActiveBuildings)
	{
		Pair.Value.AgeInYears++;
	}
}

void UBuildingManagerSubsystem::SeedInitialSettlement()
{
	if (ActiveBuildings.Num() > 0)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>();
	if (!Terrain)
	{
		return;
	}

	UBuildingData* DataHouse = GetCatalogData(EBuildingType::House);
	UBuildingData* DataApartment = GetCatalogData(EBuildingType::Apartment);
	UBuildingData* DataOffice = GetCatalogData(EBuildingType::Office);
	UBuildingData* DataHospital = GetCatalogData(EBuildingType::Hospital);
	UBuildingData* DataSchool = GetCatalogData(EBuildingType::School);

	if (UAftershockScenarioSubsystem* Scenario = World->GetSubsystem<UAftershockScenarioSubsystem>(); Scenario && Scenario->IsPrepared())
	{
		// Designed neighbourhoods use the same canonical purchase-free initial city
		// path as the old seed; there is no second building/population registry.
		int32 Residents = 0, Workers = 0, Placed = 0;
		for (const FAftershockScenarioLot& Lot : Scenario->GetDesignedLots())
		{
			UBuildingData* Data = GetCatalogData(Lot.Type);
			if (!Data) continue;
			int32 ID = INDEX_NONE;
			if (PlaceBuilding(Lot.Coord, Data, Lot.Foundation, Lot.Floors, ID)) { Residents += Data->Residents; Workers += Data->Workers; ++Placed; }
		}
		if (UCrowdRenderSubsystem* Crowd = World->GetSubsystem<UCrowdRenderSubsystem>()) Crowd->UpdatePopulationCount(Residents, Workers);
		if (UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>()) Economy->OnEconomyUpdated.Broadcast(Economy->TreasuryTL, Economy->PublicSatisfaction, Economy->CurrentTurn);
		Scenario->NotifySettlementSeeded();
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_AUTHORED_SETTLEMENT index=%d placed=%d residents=%d workers=%d starterRoadCells=%d"), Scenario->GetScenarioIndex(), Placed, Residents, Workers, Scenario->GetStarterRoadCellCount());
		return;
	}

	int32 SpawnedBuildings = 0;
	const int32 TargetInitialBuildings = 28;
	int32 TotalResidents = 0;
	int32 TotalWorkers = 0;

	FRandomStream Rnd(Terrain->TerrainSeed);

	for (int32 X = 18; X <= 32 && SpawnedBuildings < TargetInitialBuildings; ++X)
	{
		for (int32 Y = 18; Y <= 32 && SpawnedBuildings < TargetInitialBuildings; ++Y)
		{
			const FIntPoint Coord(X, Y);
			if (!Terrain->IsValidCoord(Coord)) continue;

			const FGridCell Cell = Terrain->GetCell(Coord);
			if (Cell.bIsRiverbed || Cell.OccupyingBuildingID != -1)
			{
				continue;
			}

			if (Rnd.FRand() < 0.35f)
			{
				UBuildingData* SelectedData = nullptr;
				int32 Floors = 1;

				if (SpawnedBuildings < 12)
				{
					SelectedData = DataHouse;
					Floors = Rnd.RandRange(1, 2);
				}
				else if (SpawnedBuildings < 20)
				{
					SelectedData = DataApartment;
					Floors = Rnd.RandRange(4, 7);
				}
				else if (SpawnedBuildings < 25)
				{
					SelectedData = DataOffice;
					Floors = Rnd.RandRange(5, 10);
				}
				else if (SpawnedBuildings == 25)
				{
					SelectedData = DataHospital;
					Floors = 4;
				}
				else
				{
					SelectedData = DataSchool;
					Floors = 3;
				}

				int32 AssignedID = -1;
				if (PlaceBuilding(Coord, SelectedData, EFoundationQuality::Medium, Floors, AssignedID))
				{
					SpawnedBuildings++;
					TotalResidents += SelectedData->Residents;
					TotalWorkers += SelectedData->Workers;
				}
			}
		}
	}

	// Görselleri güncelle ve Crowd sistemine başlangıç nüfusunu aktar
	if (UCrowdRenderSubsystem* CrowdSys = World->GetSubsystem<UCrowdRenderSubsystem>())
	{
		CrowdSys->UpdatePopulationCount(TotalResidents, TotalWorkers);
	}
	if (UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>())
	{
		// HUD construction can precede city seeding. Publish the initialized city
		// so projected taxes/maintenance do not remain zero until the first purchase.
		Economy->OnEconomyUpdated.Broadcast(Economy->TreasuryTL, Economy->PublicSatisfaction, Economy->CurrentTurn);
	}
}
