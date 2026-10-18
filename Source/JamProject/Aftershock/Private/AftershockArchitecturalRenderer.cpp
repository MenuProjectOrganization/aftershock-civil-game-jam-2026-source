#include "AftershockArchitecturalRenderer.h"
#include "AftershockArchitecturalAudit.h"
#include "GameFramework/GameUserSettings.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	int32 ArchitectureQuality()
    {const auto* S=UGameUserSettings::GetGameUserSettings();return S?FMath::Clamp(FMath::Min(S->GetShadingQuality(),S->GetViewDistanceQuality()),0,4):3;}
	using P=EAftershockArchitecturalPart;
	using F=EAftershockArchitecturalFinish;
	uint64 Combine(uint64 A,uint64 B){return A^(B+0x9e3779b97f4a7c15ull+(A<<6)+(A>>2));}
	uint64 DescriptorHash(const FAftershockArchitecturalInput& I)
	{
		uint64 H=static_cast<uint32>(I.BuildingID);
		H=Combine(H,GetTypeHash(I.Coord));H=Combine(H,static_cast<uint32>(I.Type));H=Combine(H,I.Floors);
		H=Combine(H,GetTypeHash(I.RegionID.ToString()));H=Combine(H,GetTypeHash(I.FocusID.ToString()));H=Combine(H,I.TerrainSeed);
		H=Combine(H,GetTypeHash(I.Frame.GetTranslation()));H=Combine(H,GetTypeHash(I.Frame.GetRotation()));
        H=Combine(H,static_cast<uint8>(I.RegionalBiome));H=Combine(H,I.bParkGroundNearlyLevel?1u:0u);
		return Combine(H,GetTypeHash(I.EnvelopeCm)); // CPD/damage excluded; SyncBuilding updates them directly.
	}
	float PartCategory(P Part)
	{
		if(Part==P::FacadeStrip)return 1.f;
		if(Part==P::RoofFlat||Part==P::RoofHip||Part==P::RoofGable||Part==P::RoofShed)return 2.f;
		if(Part==P::Door)return 3.f;if(Part==P::Canopy)return 4.f;return 0.f;
	}
	bool ValidMesh(const UStaticMesh* Mesh)
	{
		if(!Mesh)return false;
		const FBox B=Mesh->GetBoundingBox();
		return B.IsValid&&!B.Min.ContainsNaN()&&!B.Max.ContainsNaN()&&B.GetSize().GetMin()>.01;
	}
}

FAftershockArchitecturalRenderer::~FAftershockArchitecturalRenderer(){Reset();}
void FAftershockArchitecturalRenderer::AddReferencedObjects(FReferenceCollector& C)
{
	// TObjectPtr assignments participate in incremental reachability barriers.
	// UE5.8 raw-pointer AddReferencedObject overload is deprecated/unsafe here.
	C.AddReferencedObjects(CoreMeshes);C.AddReferencedObjects(SharedMaterials);
	C.AddReferencedObjects(OptionalFacades);C.AddReferencedObjects(Batches);
}
bool FAftershockArchitecturalRenderer::EnsureAssets()
{
	if(bTriedAssets)return bAssetsReady;bTriedAssets=true;bAssetsReady=true;
	const struct{P Part;const TCHAR* Name;} Meshes[]={
		{P::Mass,TEXT("Body")},{P::Door,TEXT("Door")},
		{P::RoofFlat,TEXT("RoofFlat")},{P::RoofGable,TEXT("RoofGable")},{P::RoofHip,TEXT("RoofHip")},
		{P::RoofShed,TEXT("RoofShed")},{P::Canopy,TEXT("Canopy")}};
	for(const auto& M:Meshes)
	{
		const FString Path=FString::Printf(TEXT("/Game/Buildings/Architecture/Meshes/SM_Arch_%s.SM_Arch_%s"),M.Name,M.Name);
		auto* Asset=LoadObject<UStaticMesh>(nullptr,*Path);CoreMeshes.Add(M.Part,Asset);
		if(!ValidMesh(Asset)){bAssetsReady=false;UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_ARCH_ASSET_MISSING mesh=%s originalRendererRetained=1"),*Path);}
	}
	const struct{F Finish;const TCHAR* Name;} Materials[]={
		{F::PlasterCladding,TEXT("Plaster")},{F::TimberCladding,TEXT("Wood")},{F::BrickVeneer,TEXT("Brick")},
		{F::MetalCladding,TEXT("Metal")},{F::GlassScreen,TEXT("Glass")},{F::RoofTile,TEXT("Roof")},{F::RoofMetal,TEXT("Metal")},{F::ConcreteCladding,TEXT("Concrete")}};
	for(const auto& M:Materials)
	{
		const FString Path=FString::Printf(TEXT("/Game/Buildings/Architecture/Materials/M_Arch_%s.M_Arch_%s"),M.Name,M.Name);
		auto* Asset=LoadObject<UMaterialInterface>(nullptr,*Path);SharedMaterials.Add(M.Finish,Asset);
		if(!Asset){bAssetsReady=false;UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_ARCH_ASSET_MISSING material=%s originalRendererRetained=1"),*Path);}
	}
	return bAssetsReady;
}
bool FAftershockArchitecturalRenderer::HasNativeFacadeFamily(int32 Floors)
{
    if(Floors<1||Floors>4)return false;
    for(int32 Bays=1;Bays<=4;++Bays)
    {
        const FString Name=FString::Printf(TEXT("SM_CS_Facade_B%d_F%02d"),Bays,Floors);
        TObjectPtr<UStaticMesh>* Slot=OptionalFacades.Find(Name);
        if(!Slot)
        {
            const FString Path=FString::Printf(TEXT("/Game/Aftershock/CitySample/Architecture/Baked/%s.%s"),*Name,*Name);
            Slot=&OptionalFacades.Add(Name,LoadObject<UStaticMesh>(nullptr,*Path));
            bool Valid=ValidMesh(Slot->Get());
            if(Valid)
            {
                const auto* Mesh=Slot->Get();const FBox B=Mesh->GetBoundingBox();const FVector S=B.GetSize();
                bool Wall=false,Glass=false,Metal=false;
                for(const auto& M:Mesh->GetStaticMaterials())
                {const auto N=M.MaterialSlotName;Wall|=N==TEXT("Wall");Glass|=N==TEXT("Glass");Metal|=N==TEXT("Metal");}
                Valid=Mesh->GetNumLODs()==3&&Mesh->GetStaticMaterials().Num()==3&&Wall&&Glass&&Metal
                    &&FMath::Abs(S.X-Bays*300.)<.5&&FMath::Abs(S.Y-75.)<.5&&FMath::Abs(S.Z-Floors*300.)<.5
                    &&FMath::Abs(B.Min.Z)<.25&&FMath::Abs(B.Min.Y)<.25&&FMath::Abs(B.Min.X+Bays*150.)<.25;
                if(Valid)for(int32 LOD=0;LOD<3;++LOD)
                {const int32 Caps[]={50000,18000,6000};const int32 Triangles=Mesh->GetNumTriangles(LOD);Valid&=Triangles>0&&Triangles<=Caps[LOD];}
            }
            if(!Valid){*Slot=nullptr;UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_NATIVE_FACADE_FALLBACK path=%s wholeLegacyRecipeRetained=1"),*Path);}
        }
        if(!Slot->Get())return false;
    }
    return true;
}
UStaticMesh* FAftershockArchitecturalRenderer::ResolveMesh(const FAftershockArchitecturalPlacement& Part,int32 Floors,uint8 Bays)
{
    if(Part.Part==P::ParkPath)return CoreMeshes.FindRef(P::Mass).Get();
    if(Part.Part>=P::ParkBench&&Part.Part<=P::ParkBroadleaf)
    {
        const TCHAR* Names[]={TEXT("Bench"),TEXT("GrassClump"),TEXT("Shrub"),TEXT("Conifer"),TEXT("Palm"),TEXT("Broadleaf")};
        const int32 Index=static_cast<int32>(Part.Part)-static_cast<int32>(P::ParkBench);
        const FString Name=Index==0?TEXT("SM_StreetBench"):FString::Printf(TEXT("SM_%s_%02d"),Names[Index],Part.Variant%3+1);
        const FString CacheName=TEXT("ParkNative_")+Name;
        TObjectPtr<UStaticMesh>* Cached=OptionalFacades.Find(CacheName);
        if(!Cached)
        {
            const FString Root=Index==0?TEXT("/Game/Aftershock/CitySample/Meshes/"):TEXT("/Game/Environment/Meshes/Realistic/");
            Cached=&OptionalFacades.Add(CacheName,LoadObject<UStaticMesh>(nullptr,*(Root+Name+TEXT(".")+Name)));
            if(!ValidMesh(Cached->Get()))UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_PARK_ASSET_MISSING mesh=%s originalParkDefinitionFallback=1"),*Name);
        }
        return ValidMesh(Cached->Get())?Cached->Get():nullptr;
    }
	if(Part.Part==P::FacadeStrip)
	{
        if(Part.Variant==240)
            return OptionalFacades.FindRef(FString::Printf(TEXT("SM_CS_Facade_B%d_F%02d"),Bays,Floors)).Get();
		const FString Name=FString::Printf(TEXT("SM_Arch_Facade_B%d_F%02d"),Bays,Floors);
		TObjectPtr<UStaticMesh>* Cached=OptionalFacades.Find(Name);
		if(!Cached)
		{
			const FString Path=FString::Printf(TEXT("/Game/Buildings/Architecture/Meshes/%s.%s"),*Name,*Name);
			Cached=&OptionalFacades.Add(Name,LoadObject<UStaticMesh>(nullptr,*Path));
		}
		if(ValidMesh(Cached->Get()))return Cached->Get();
		if(!bWarnedMissingFacade)
		{
			bWarnedMissingFacade=true;
			UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_ARCH_FACADE_MISSING path=%s wholeDefinitionFallback=1 noLiteralWindowStretch=1"),*Name);
		}
		return nullptr; // Real baked window geometry is required, never stretch a one-floor window.
	}
	const TObjectPtr<UStaticMesh>* Found=CoreMeshes.Find(Part.Part);return Found?Found->Get():nullptr;
}
bool FAftershockArchitecturalRenderer::AppendPrepared(const FPrepared& Ready,UMaterialInterface* Override,TMap<FBatchKey,FPendingBatch>& Pending)
{
	const auto& I=Ready.Candidate->Input;
	for(int32 PartOrdinal=0;PartOrdinal<Ready.Recipe.Pieces.Num();++PartOrdinal)
	{
		const auto& Part=Ready.Recipe.Pieces[PartOrdinal];
		auto* Mesh=ResolveMesh(Part,I.Floors,Part.Bays);
        const bool Native=Part.Part>=P::ParkBench&&Part.Part<=P::ParkBroadleaf;
        const TObjectPtr<UMaterialInterface>* Material=SharedMaterials.Find(Part.Finish);
        UMaterialInterface* Selected=Override?Override:Native?(Mesh?Mesh->GetMaterial(0):nullptr):Material?Material->Get():nullptr;
        if(Part.Part==P::ParkPath&&!Override)
        {
            auto* Paving=SharedMaterials.Find(F::ParkPaving);
            if(!Paving)Paving=&SharedMaterials.Add(F::ParkPaving,LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Environment/Materials/Realistic/M_EnvironmentSidewalk.M_EnvironmentSidewalk")));
            Selected=Paving->Get();
        }
        if(!ValidMesh(Mesh)||!Selected)return false;
		// Keep native recessed windows in the ordinary city view, using their
		// real third LOD instead of forcing near-detail geometry at a distance.
		const bool NativeFacade=Part.Part==P::FacadeStrip&&Part.Variant==240;
		const uint8 LOD=Ready.Tier==EAftershockArchitecturalTier::Street?0:
			Ready.Tier==EAftershockArchitecturalTier::City?(NativeFacade?3:2):3;
		const bool Roof=PartCategory(Part.Part)==2.f;
		FBatchKey Key{Mesh,Selected,Roof?(Override?Override:SharedMaterials.FindRef(Part.AccentFinish).Get()):nullptr,LOD,Native&&!Override};
		const FBox B=Mesh->GetBoundingBox();const FVector Size=B.GetSize();
		// Normalize measured authored bounds once per instance, then apply the
		// recipe's REAL cm dimensions. Its final bound remains inside the saved
		// envelope. Imported CitySample prefabs are not accepted as these modules.
		const FVector Base(B.GetCenter().X,B.GetCenter().Y,B.Min.Z);
		FTransform ToUnit(FQuat::Identity,-Base/Size,FVector::OneVector/Size);
		// Blender FBX(+Y wall depth) imports through UE's RHS->LHS Y flip.
		// These measured negative-Y assets therefore face +Y. Restore the
		// recipe's front=-Y with a PROPER rotation, never a negative scale:
		// glass stays behind frames, and the door handle stays outside.
		const bool FrontSemantic=Part.Part==P::FacadeStrip||Part.Part==P::Door;
		if(FrontSemantic&&B.Max.Y<=.5&&B.Min.Y<-.5)
			ToUnit=ToUnit*FTransform(FRotator(0.f,180.f,0.f));
        FTransform Placement=Part.LocalTransform;
        if(Native)
        {
            const FVector Reservation=Placement.GetScale3D();
            const bool Plant=Part.Part>=P::ParkGrass&&Part.Part<=P::ParkBroadleaf;
            // Native flora is authored with its real root at XY=(0,0). An
            // asymmetric canopy's bbox centre is NOT a valid ground anchor;
            // especially on slopes, moving that root loses the sampled datum.
            const FVector RootedSize=Plant?FVector(
                2.*FMath::Max(FMath::Abs(B.Min.X),FMath::Abs(B.Max.X)),
                2.*FMath::Max(FMath::Abs(B.Min.Y),FMath::Abs(B.Max.Y)),Size.Z):Size;
            if(RootedSize.ContainsNaN()||RootedSize.GetMin()<=.01)return false;
            const double Uniform=(Reservation/RootedSize).GetMin();
            if(!FMath::IsFinite(Uniform)||Uniform<=0.)return false;
            const FVector Anchor=Plant?FVector(0.,0.,B.Min.Z):Base;
            ToUnit=FTransform(FQuat::Identity,-Anchor,FVector::OneVector);
            Placement.SetScale3D(FVector(Uniform));
            // Check the actual imported native bbox, not only the abstract
            // reservation. Keep centring the bench; keep all plant roots fixed.
            const FBox Actual=B.TransformBy(ToUnit*Placement);
            const FBox Allowed=Part.LocalConservativeBounds.ExpandBy(.25);
            if(!Actual.IsValid||Actual.Min.ContainsNaN()||Actual.Max.ContainsNaN()||
                !Allowed.IsInsideOrOn(Actual.Min)||!Allowed.IsInsideOrOn(Actual.Max))return false;
        }
        const FTransform World=ToUnit*Placement*I.Frame;
		if(World.ContainsNaN())return false;
		auto& Batch=Pending.FindOrAdd(Key);Batch.Transforms.Add(World);Batch.BuildingIDs.Add(I.BuildingID);
		Batch.InstanceKeys.Add((static_cast<uint64>(static_cast<uint32>(I.BuildingID))<<32)|static_cast<uint32>(PartOrdinal));
		for(float V:I.CanonicalCustomData)Batch.CustomData.Add(V);
		Batch.CustomData.Add(.92f+.16f*static_cast<float>(Ready.Recipe.AppearanceSeed%1001u)/1000.f);
		Batch.CustomData.Add(static_cast<float>(I.Floors));Batch.CustomData.Add(static_cast<float>(Part.Bays));Batch.CustomData.Add(PartCategory(Part.Part));
		Batch.CustomData.Add(Ready.Recipe.SurfaceTint.R);Batch.CustomData.Add(Ready.Recipe.SurfaceTint.G);Batch.CustomData.Add(Ready.Recipe.SurfaceTint.B);
	}
	return true;
}
void FAftershockArchitecturalRenderer::ClearInstances()
{
	for(auto& Pair:Batches)if(IsValid(Pair.Value.Get()))
	{
		Pair.Value->SetVisibility(false);if(Pair.Value->GetInstanceCount())Pair.Value->ClearInstances();
	}
	BatchMembership.Reset();ReferencesByID.Reset();Work.Buildings=Work.DetailedBuildings=Work.Instances=Work.ActiveBatches=0;Work.AllocatedInstances=0;
}
bool FAftershockArchitecturalRenderer::Submit(AActor& Host,TMap<FBatchKey,FPendingBatch>& Pending,bool IsPreview)
{
	int32 Total=0,ProjectedAllocated=0;
	for(const auto& Pair:Pending)
	{
		const auto& B=Pair.Value;Total+=B.Transforms.Num();
		if(B.InstanceKeys.Num()!=B.Transforms.Num()||B.BuildingIDs.Num()!=B.Transforms.Num()
			||B.CustomData.Num()!=B.Transforms.Num()*DerivedCustomDataFloats)return false;
		TSet<uint64> Unique;Unique.Reserve(B.InstanceKeys.Num());
		for(const uint64 Key:B.InstanceKeys)if(Unique.Contains(Key))return false;else Unique.Add(Key);
		const auto* Existing=Batches.Find(Pair.Key);
		ProjectedAllocated+=FMath::Max(Existing&&IsValid(Existing->Get())?(*Existing)->GetInstanceCount():0,B.Transforms.Num());
	}
	if(Pending.Num()>MaxBatches||Total>MaxInstances||!Host.GetRootComponent())
	{
		UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_ARCH_BUDGET_REJECT batches=%d instances=%d wholeDefinitionFallback=1"),Pending.Num(),Total);return false;
	}
	// Pooled zero-scale slots retain stable indices. If their high-water sum
	// would exceed the ORIGINAL allocation ceiling, compact only shrinking
	// buckets. This rare explicit fallback is not a claim of hitch-free rendering.
	const bool CompactShrinking=ProjectedAllocated>MaxInstances;
	if(CompactShrinking)UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_ARCH_POOL_COMPACTION projectedSlots=%d targetActive=%d cap=%d"),ProjectedAllocated,Total,MaxInstances);
	TArray<FBatchKey> Retired;
	for(const auto& Pair:Batches)if(!Pending.Contains(Pair.Key))Retired.Add(Pair.Key);
	for(const auto& Key:Retired)
	{
		if(auto* C=Batches.FindRef(Key).Get();IsValid(C))C->DestroyComponent();
		Batches.Remove(Key);BatchMembership.Remove(Key);
	}
	for(const auto& Pair:Pending)
	{
		if(auto* Existing=Batches.Find(Pair.Key);Existing&&IsValid(Existing->Get()))continue;
		auto* C=NewObject<UHierarchicalInstancedStaticMeshComponent>(&Host,MakeUniqueObjectName(&Host,UHierarchicalInstancedStaticMeshComponent::StaticClass(),TEXT("AftershockArchitectureBatch")));
		C->SetupAttachment(Host.GetRootComponent());C->SetAbsolute(true,true,true);C->SetWorldTransform(FTransform::Identity);
		C->SetStaticMesh(Pair.Key.Mesh);C->NumCustomDataFloats=DerivedCustomDataFloats;
		const bool Roof=Pair.Key.Accent!=nullptr;
		for(int32 Slot=0;Slot<FMath::Max(1,Pair.Key.Mesh->GetStaticMaterials().Num());++Slot)
		{
			UMaterialInterface* Mat=Pair.Key.Material;
			if(!IsPreview&&!Pair.Key.bNativeMaterials)
			{
				const FString Role=Pair.Key.Mesh->GetStaticMaterials()[Slot].MaterialSlotName.ToString();
				if(Role.Contains(TEXT("Metal")))Mat=SharedMaterials.FindChecked(F::MetalCladding).Get();
				else if(Role.Contains(TEXT("Glass")))Mat=SharedMaterials.FindChecked(F::GlassScreen).Get();
				else if(Roof&&Role.Contains(TEXT("Wall")))Mat=Pair.Key.Accent;
			}
            if(!IsPreview&&Pair.Key.bNativeMaterials)Mat=Pair.Key.Mesh->GetMaterial(Slot);
            if(!Mat)return false;
            C->SetMaterial(Slot,Mat);
		}
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);C->SetGenerateOverlapEvents(false);C->bDisableCollision=true;
		C->SetComponentTickEnabled(false);C->SetCastShadow(!IsPreview);C->bAutoRebuildTreeOnInstanceChanges=false;
		C->SetCullDistances(0,0);const int32 LODs=Pair.Key.Mesh->GetNumLODs();
		C->SetForcedLodModel(Pair.Key.LOD>0?FMath::Clamp<int32>(Pair.Key.LOD,1,FMath::Max(1,LODs)):0);
		Host.AddInstanceComponent(C);C->RegisterComponent();Batches.Add(Pair.Key,C);
	}
	// Reuse each living building's small reverse-index capacity; no historical
	// resident/building ledger is introduced, and retired IDs are removed below.
	for(auto& Pair:ReferencesByID)Pair.Value.Reset();
	int32 Allocated=0;
	for(auto& Pair:Pending)
	{
		auto* C=Batches.FindChecked(Pair.Key).Get();auto& B=Pair.Value;
		auto& State=BatchMembership.FindOrAdd(Pair.Key);
		bool GeometryChanged=false,DataChanged=false;
		if((CompactShrinking&&C->GetInstanceCount()>B.Transforms.Num())
			||State.IndexByKey.Num()+State.FreeIndices.Num()!=C->GetInstanceCount())
		{
			if(C->GetInstanceCount())C->ClearInstances();
			State.IndexByKey.Empty(B.Transforms.Num());State.FreeIndices.Empty();
			GeometryChanged=true;++Work.Compactions;
		}
		TSet<uint64> Wanted;Wanted.Reserve(B.InstanceKeys.Num());
		for(const uint64 Key:B.InstanceKeys)Wanted.Add(Key);
		TArray<uint64> Remove;
		for(const auto& Existing:State.IndexByKey)if(!Wanted.Contains(Existing.Key))Remove.Add(Existing.Key);
		for(const uint64 Key:Remove)
		{
			const int32 Index=State.IndexByKey.FindChecked(Key);FTransform Hidden;
			if(C->GetInstanceTransform(Index,Hidden,true)&&!Hidden.GetScale3D().IsNearlyZero())
			{
				Hidden.SetScale3D(FVector::ZeroVector);C->UpdateInstanceTransform(Index,Hidden,true,false,true);GeometryChanged=true;
			}
			State.IndexByKey.Remove(Key);State.FreeIndices.Add(Index);++Work.RetiredSlots;
		}
		for(int32 At=0;At<B.InstanceKeys.Num();++At)
		{
			const uint64 Key=B.InstanceKeys[At];int32 Index=INDEX_NONE;
			if(const int32* Existing=State.IndexByKey.Find(Key))Index=*Existing;
			else
			{
				if(!State.FreeIndices.IsEmpty())
				{
					Index=State.FreeIndices.Pop(EAllowShrinking::No);
					C->UpdateInstanceTransform(Index,B.Transforms[At],true,false,true);++Work.InstanceReuses;
				}
				else{Index=C->AddInstance(B.Transforms[At],true);++Work.InstanceAppends;}
				State.IndexByKey.Add(Key,Index);GeometryChanged=true;
			}
			FTransform Current;const auto& Desired=B.Transforms[At];
			// CPU instance matrices contain float32 values. A stable desired double
			// pose must not become a false change every frame through round trips.
			if(!C->GetInstanceTransform(Index,Current,true)
				||!Current.GetTranslation().Equals(Desired.GetTranslation(),.1)
				||!Current.GetRotation().Equals(Desired.GetRotation(),1.e-5)
				||!Current.GetScale3D().Equals(Desired.GetScale3D(),1.e-5))
			{
				C->UpdateInstanceTransform(Index,Desired,true,false,true);GeometryChanged=true;++Work.TransformUpdates;
			}
			for(int32 Ch=0;Ch<DerivedCustomDataFloats;++Ch)
			{
				const float Value=B.CustomData[At*DerivedCustomDataFloats+Ch];const int32 Target=Index*DerivedCustomDataFloats+Ch;
				if(!C->PerInstanceSMCustomData.IsValidIndex(Target)||C->PerInstanceSMCustomData[Target]!=Value)
				{C->SetCustomDataValue(Index,Ch,Value,false);DataChanged=true;}
			}
			ReferencesByID.FindOrAdd(B.BuildingIDs[At]).Add({C,Index});
		}
		C->SetVisibility(true);
		if(GeometryChanged){C->BuildTreeIfOutdated(false,true);++Work.TreeBuilds;}
		if(DataChanged)++Work.CustomDataUpdates;
		if(GeometryChanged||DataChanged){C->MarkRenderStateDirty();++Work.ChangedBatches;}else ++Work.ReusedBatches;
		Allocated+=C->GetInstanceCount();
	}
	TArray<int32> EmptyIDs;for(const auto& Pair:ReferencesByID)if(Pair.Value.IsEmpty())EmptyIDs.Add(Pair.Key);
	for(const int32 ID:EmptyIDs)ReferencesByID.Remove(ID);
	Work.Instances=Total;Work.AllocatedInstances=Allocated;Work.ActiveBatches=Pending.Num();++Work.MembershipRebuilds;
	ensureMsgf(Allocated<=MaxInstances,TEXT("Architectural pooled allocation exceeds bounded budget"));
	return true;
}


bool FAftershockArchitecturalRenderer::Refresh(AActor& Host,const TArray<FAftershockArchitecturalCandidate>& Candidates,EAftershockArchitecturalTier Tier,FVector Focus,bool Force)
{
	if(!EnsureAssets()){ClearInstances();OwnedDefinitions.Reset();return false;}
	bPreview=false;
	TArray<const FAftershockArchitecturalCandidate*> Ordered;Ordered.Reserve(Candidates.Num());
	for(const auto& C:Candidates)Ordered.Add(&C);
	Ordered.Sort([](const auto& A,const auto& B){return A.Input.BuildingID<B.Input.BuildingID;});
	const FIntPoint Bucket(FMath::FloorToInt(Focus.X/2500.),FMath::FloorToInt(Focus.Y/2500.));
	const int32 Quality=ArchitectureQuality();
	uint64 H=Combine(Combine(static_cast<uint64>(Tier),GetTypeHash(Bucket)),Quality);
	for(const auto* C:Ordered)H=Combine(H,DescriptorHash(C->Input));
	if(!Force&&H==TopologyHash)return !OwnedDefinitions.IsEmpty();
	TSet<const UBuildingData*> NewOwned,Failed;
	TArray<FPrepared> Ready;Ready.Reserve(Ordered.Num());
	if(Tier==EAftershockArchitecturalTier::Globe)
	{
		for(const auto* C:Ordered)if(SupportsAftershockArchitecturalType(C->Input.Type))NewOwned.Add(C->Definition);
		ClearInstances();OwnedDefinitions=MoveTemp(NewOwned);TopologyHash=H;LastFocusBucket=Bucket;LastTier=Tier;LastQuality=Quality;return !OwnedDefinitions.IsEmpty();
	}
	// Stable distance tie: coordinate/ID, independent of TMap iteration order.
	Ordered.Sort([&](const auto& A,const auto& B)
	{
		const double DA=FVector::DistSquared2D(A.Input.Frame.GetLocation(),Focus),DB=FVector::DistSquared2D(B.Input.Frame.GetLocation(),Focus);
		return DA!=DB?DA<DB:A.Input.BuildingID<B.Input.BuildingID;
	});
	const int32 DetailCaps[]={64,128,256,384,512};
	const float DetailRadii[]={10000.f,15000.f,20000.f,25000.f,30000.f};
	int32 DetailCount=0;
	for(const auto* C:Ordered)
	{
		if(!SupportsAftershockArchitecturalType(C->Input.Type))continue;
		const bool Detail=Tier!=EAftershockArchitecturalTier::Regional&&DetailCount<FMath::Min(MaxDetailedBuildings,DetailCaps[Quality])&&FVector::DistSquared2D(C->Input.Frame.GetLocation(),Focus)<=FMath::Square(DetailRadii[Quality]);
		const auto LocalTier=Detail?Tier:EAftershockArchitecturalTier::Regional;
        const bool CompatibleNative=LocalTier!=EAftershockArchitecturalTier::Regional
            &&(C->Input.RegionID==TEXT("cyprus")||C->Input.RegionID==TEXT("atakent"))
            &&(C->Input.Type==EBuildingType::House||C->Input.Type==EBuildingType::Apartment)
            &&HasNativeFacadeFamily(C->Input.Floors);
        auto Recipe=BuildAftershockArchitecturalRecipe(C->Input,LocalTier,CompatibleNative);
        if(!Recipe.IsReady()&&CompatibleNative)Recipe=BuildAftershockArchitecturalRecipe(C->Input,LocalTier,false);
		if(!Recipe.IsReady()){Failed.Add(C->Definition);continue;}
		if(Detail)++DetailCount;
		FPrepared P;P.Candidate=C;P.Recipe=MoveTemp(Recipe);P.Tier=LocalTier;Ready.Add(MoveTemp(P));NewOwned.Add(C->Definition);
	}
	for(const auto* Data:Failed)NewOwned.Remove(Data);
	for(const auto& P:Ready)if(NewOwned.Contains(P.Candidate->Definition))
		for(const auto& Part:P.Recipe.Pieces)if(!ValidMesh(ResolveMesh(Part,P.Candidate->Input.Floors,Part.Bays)))
			{Failed.Add(P.Candidate->Definition);break;}
	for(const auto* Data:Failed)NewOwned.Remove(Data);
	TMap<FBatchKey,FPendingBatch> Pending;
	for(const auto& P:Ready)if(NewOwned.Contains(P.Candidate->Definition))if(!AppendPrepared(P,nullptr,Pending))
	{
		ClearInstances();OwnedDefinitions.Reset();TopologyHash=0;return false;
	}
	if(!Submit(Host,Pending,false)){ClearInstances();OwnedDefinitions.Reset();TopologyHash=0;return false;}
	OwnedDefinitions=MoveTemp(NewOwned);TopologyHash=H;LastFocusBucket=Bucket;LastTier=Tier;LastQuality=Quality;Work.Buildings=0;Work.DetailedBuildings=0;Work.RejectedBuildings=0;
	for(const auto& P:Ready)if(OwnedDefinitions.Contains(P.Candidate->Definition)){++Work.Buildings;Work.DetailedBuildings+=P.Tier!=EAftershockArchitecturalTier::Regional;}
	for(const auto* C:Ordered)Work.RejectedBuildings+=Failed.Contains(C->Definition);
    int32 NativeBuildings=0,NativeModules=0;
    for(const auto& Prepared:Ready)if(OwnedDefinitions.Contains(Prepared.Candidate->Definition))
    {
        bool Any=false;for(const auto& Part:Prepared.Recipe.Pieces)if(Part.Part==P::FacadeStrip&&Part.Variant==240){++NativeModules;Any=true;}
        NativeBuildings+=Any?1:0;
    }
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_NATIVE_FACADE_PRESENTATION buildings=%d wholeHeightModules=%d floorPitchCm=300 wallDepthCm=75 canonicalWrites=0 pixelVisibilityNotMeasured=1"),NativeBuildings,NativeModules);
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_ARCH_PRESENTATION tier=%d buildings=%d detailed=%d instances=%d batches=%d rejected=%d authority=canonical_saved_envelope noPhysicsChange=1"),static_cast<int32>(Tier),Work.Buildings,Work.DetailedBuildings,Work.Instances,Work.ActiveBatches,Work.RejectedBuildings);
	return !OwnedDefinitions.IsEmpty();
}
bool FAftershockArchitecturalRenderer::OwnsDefinition(const UBuildingData* Data) const{return OwnedDefinitions.Contains(Data);}
bool FAftershockArchitecturalRenderer::NeedsRefresh(EAftershockArchitecturalTier Tier,FVector Focus) const
{
	return TopologyHash==0||LastQuality!=ArchitectureQuality()||LastTier!=Tier||LastFocusBucket!=FIntPoint(FMath::FloorToInt(Focus.X/2500.),FMath::FloorToInt(Focus.Y/2500.));
}
void FAftershockArchitecturalRenderer::SyncBuilding(int32 ID,const FAftershockArchitecturalInput* Input)
{
	const auto* Refs=ReferencesByID.Find(ID);if(!Refs)return;
	if(!Input||Input->DamageState>=EDamageState::Collapsed)TopologyHash=0; // repair must restore hidden transforms through the next derived rebuild
	TSet<UHierarchicalInstancedStaticMeshComponent*> Dirty;
	for(const auto& Ref:*Refs)if(IsValid(Ref.Batch)&&Ref.Index>=0&&Ref.Index<Ref.Batch->GetInstanceCount())
	{
		if(!Input||Input->DamageState>=EDamageState::Collapsed)
		{
			FTransform Current;if(Ref.Batch->GetInstanceTransform(Ref.Index,Current,true)&&!Current.GetScale3D().IsNearlyZero())
			{Current.SetScale3D(FVector::ZeroVector);Ref.Batch->UpdateInstanceTransform(Ref.Index,Current,true,false,true);Dirty.Add(Ref.Batch);}
		}
		else for(int32 Ch=0;Ch<4;++Ch)
		{
			const int32 At=Ref.Index*DerivedCustomDataFloats+Ch;
			if(!Ref.Batch->PerInstanceSMCustomData.IsValidIndex(At)||Ref.Batch->PerInstanceSMCustomData[At]!=Input->CanonicalCustomData[Ch])
			{Ref.Batch->SetCustomDataValue(Ref.Index,Ch,Input->CanonicalCustomData[Ch],false);Dirty.Add(Ref.Batch);}
		}
	}
	for(auto* C:Dirty)C->MarkRenderStateDirty();Work.CustomDataUpdates+=Dirty.Num();
}
bool FAftershockArchitecturalRenderer::ShowPreview(AActor& Host,const FAftershockArchitecturalInput& Input,UMaterialInterface* Material)
{
	if(!Material||!EnsureAssets()){HidePreview();return false;}
	const uint64 H=Combine(DescriptorHash(Input),GetTypeHash(Material));
	if(bPreview&&H==TopologyHash){for(auto& Pair:Batches)if(IsValid(Pair.Value.Get())&&Pair.Value->GetInstanceCount())Pair.Value->SetVisibility(true);return true;}
	FAftershockArchitecturalCandidate Candidate;Candidate.Input=Input;
	FPrepared P;P.Candidate=&Candidate;P.Recipe=BuildAftershockArchitecturalRecipe(Input,EAftershockArchitecturalTier::Street,(Input.RegionID==TEXT("cyprus")||Input.RegionID==TEXT("atakent"))&&(Input.Type==EBuildingType::House||Input.Type==EBuildingType::Apartment)&&HasNativeFacadeFamily(Input.Floors));if(!P.Recipe.IsReady())P.Recipe=BuildAftershockArchitecturalRecipe(Input,EAftershockArchitecturalTier::Street,false);P.Tier=EAftershockArchitecturalTier::Street;
	if(!P.Recipe.IsReady()){HidePreview();return false;}
	TMap<FBatchKey,FPendingBatch> Pending;
	if(!AppendPrepared(P,Material,Pending)||!Submit(Host,Pending,true)){HidePreview();return false;}
	TopologyHash=H;bPreview=true;return true;
}
void FAftershockArchitecturalRenderer::HidePreview(){for(auto& Pair:Batches)if(IsValid(Pair.Value.Get()))Pair.Value->SetVisibility(false);}
void FAftershockArchitecturalRenderer::Reset()
{
	for(auto& Pair:Batches)if(IsValid(Pair.Value.Get()))Pair.Value->DestroyComponent();
	Batches.Reset();BatchMembership.Reset();ReferencesByID.Reset();OwnedDefinitions.Reset();CoreMeshes.Reset();SharedMaterials.Reset();OptionalFacades.Reset();
	TopologyHash=0;bTriedAssets=bAssetsReady=bPreview=bWarnedMissingFacade=false;Work={};
}

// Append to current renderer source; private audit header is included separately.
void FAftershockArchitecturalRenderer::ReadCanonicalCPDAudit(const TArray<FAftershockArchitecturalCandidate>& Inputs,FAftershockArchitectureAudit& Out) const
{
	Out.RendererPresent=true;Out.GlobeSuppressed=LastTier==EAftershockArchitecturalTier::Globe;
	TMap<int32,const FAftershockArchitecturalCandidate*> ByID;
	TSet<UHierarchicalInstancedStaticMeshComponent*> OwnedBatches;
	for(const auto& P:Batches)if(IsValid(P.Value.Get()))OwnedBatches.Add(P.Value.Get());
	for(const auto& C:Inputs)
	{
		ByID.Add(C.Input.BuildingID,&C);
		if(!SupportsAftershockArchitecturalType(C.Input.Type)){++Out.UnsupportedBuildings;continue;}
		if(!OwnsDefinition(C.Definition)){++Out.FallbackBuildings;continue;}
		if(!Out.GlobeSuppressed)++Out.ExpectedRenderedBuildings;
	}
	TMap<UHierarchicalInstancedStaticMeshComponent*,TSet<int32>> Seen;
	TSet<int32> VisibleIDs;
	for(const auto& P:ReferencesByID)
	{
		const auto* Found=ByID.Find(P.Key);const auto* Canonical=Found?*Found:nullptr;
		const bool Expected=Canonical&&SupportsAftershockArchitecturalType(Canonical->Input.Type)&&OwnsDefinition(Canonical->Definition)&&!Out.GlobeSuppressed;
		bool Visible=false;
		for(const auto& Ref:P.Value)
		{
			++Out.ReferencedModules;
			if(!IsValid(Ref.Batch)||!OwnedBatches.Contains(Ref.Batch)||Ref.Index<0||Ref.Index>=Ref.Batch->GetInstanceCount())
			{++Out.InvalidReferences;Out.Problem(P.Key);continue;}
			auto& Indices=Seen.FindOrAdd(Ref.Batch);
			if(Indices.Contains(Ref.Index)){++Out.DuplicateReferences;Out.Problem(P.Key);}else Indices.Add(Ref.Index);
			FTransform T;if(!Ref.Batch->GetInstanceTransform(Ref.Index,T,true)||T.ContainsNaN())
			{++Out.InvalidReferences;Out.Problem(P.Key);continue;}
			const bool Nonzero=!T.GetScale3D().IsNearlyZero();
			if(Nonzero&&Ref.Batch->IsVisible())Visible=true;
			if(!Expected)continue; // excluded/collapsed IDs must have no visible module below
			if(!Nonzero||!Ref.Batch->IsVisible()){++Out.UnexpectedHiddenModules;Out.Problem(P.Key);continue;}
			if(Ref.Batch->NumCustomDataFloats!=DerivedCustomDataFloats){Out.DerivedStrideValid=false;Out.Problem(P.Key);continue;}
			++Out.CheckedModuleInstances;
			for(int32 Ch=0;Ch<DerivedCustomDataFloats;++Ch)
			{
				const int32 At=Ref.Index*DerivedCustomDataFloats+Ch;
				if(!Ref.Batch->PerInstanceSMCustomData.IsValidIndex(At)){++Out.InvalidReferences;Out.Problem(P.Key);continue;}
				const float Actual=Ref.Batch->PerInstanceSMCustomData[At];
				if(!FMath::IsFinite(Actual)){++Out.NonfiniteValues;Out.Problem(P.Key);}
				if(Ch<4)
				{
					++Out.CheckedCanonicalValues;const float ExpectedValue=Canonical->Input.CanonicalCustomData[Ch];
					if(!FMath::IsFinite(ExpectedValue)||!FMath::IsNearlyEqual(Actual,ExpectedValue,1.e-6f))
					{++Out.MismatchedCanonicalValues;Out.Problem(P.Key);}
				}
			}
		}
		if(Visible){VisibleIDs.Add(P.Key);if(!Expected){++Out.UnexpectedRenderedBuildings;Out.Problem(P.Key);}}
	}
	for(const auto& C:Inputs)if(SupportsAftershockArchitecturalType(C.Input.Type)&&OwnsDefinition(C.Definition)&&!Out.GlobeSuppressed)
	{
		if(VisibleIDs.Contains(C.Input.BuildingID))++Out.RenderedBuildings;
		else{++Out.MissingExpectedBuildings;Out.Problem(C.Input.BuildingID);}
	}
	for(const auto& P:Batches)
	{
		const auto* C=P.Value.Get();if(!IsValid(C)){++Out.InvalidReferences;continue;}
		Out.AllocatedSlots+=C->GetInstanceCount();Out.DerivedStrideValid&=C->NumCustomDataFloats==DerivedCustomDataFloats;
		const auto* State=BatchMembership.Find(P.Key);const auto* Refs=Seen.Find(P.Value.Get());
		if(State)
		{
			Out.ActiveMembershipKeys+=State->IndexByKey.Num();Out.PooledInactiveSlots+=State->FreeIndices.Num();
			if(State->IndexByKey.Num()+State->FreeIndices.Num()!=C->GetInstanceCount())++Out.InvalidReferences;
			for(const auto& Active:State->IndexByKey)if(Active.Value<0||Active.Value>=C->GetInstanceCount()||!Refs||!Refs->Contains(Active.Value))++Out.InvalidReferences;
			for(const int32 Index:State->FreeIndices)
			{
				FTransform T;if(Index<0||Index>=C->GetInstanceCount()||!C->GetInstanceTransform(Index,T,true)){++Out.InvalidReferences;continue;}
				if(!T.GetScale3D().IsNearlyZero())++Out.VisiblePooledSlots;
			}
		}
		else if(C->GetInstanceCount())++Out.InvalidReferences;
		for(int32 Index=0;Index<C->GetInstanceCount();++Index)
		{
			FTransform T;if(!C->GetInstanceTransform(Index,T,true)||T.ContainsNaN()){++Out.InvalidReferences;continue;}
			if(!T.GetScale3D().IsNearlyZero())
			{++Out.ActiveNonzeroSlots;if(!Refs||!Refs->Contains(Index))++Out.UnreferencedNonzeroSlots;}
		}
	}
	Out.AllocationWithinBudget=Out.AllocatedSlots<=MaxInstances&&Batches.Num()<=MaxBatches
		&&Out.ActiveNonzeroSlots<=Out.AllocatedSlots&&Out.ActiveMembershipKeys<=Out.AllocatedSlots;
}
