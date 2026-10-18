#pragma once

// Derived instanced building presentation; canonical gameplay data remains separate.
// One renderer is owned by the existing BuildingManager host; one optional
// preview renderer is owned by the existing camera. No new actor/timer/tick.
#include "AftershockArchitecturalRecipe.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectPtr.h"

struct FAftershockArchitectureAudit;
class AActor;
class UStaticMesh;
class UMaterialInterface;
class UHierarchicalInstancedStaticMeshComponent;

struct FAftershockArchitecturalCandidate
{
	UBuildingData* Definition=nullptr; // read-only grouping; never creates/changes definitions
	FAftershockArchitecturalInput Input;
};
struct FAftershockArchitectureWork
{
	uint64 MembershipRebuilds=0,CustomDataUpdates=0;
	uint64 ChangedBatches=0,ReusedBatches=0,InstanceAppends=0,InstanceReuses=0,TransformUpdates=0,RetiredSlots=0,TreeBuilds=0,Compactions=0;
	int32 AllocatedInstances=0;
	int32 Buildings=0,DetailedBuildings=0,Instances=0,ActiveBatches=0,RejectedBuildings=0;
};

/** Derived presentation cache, intentionally absent from save/physics/city life.
 * Whole-height facade assets REQUIRE offline-baked real windows. A missing
 * facade is an explicit whole-definition fallback, never a stretched window. */
class FAftershockArchitecturalRenderer final:public FGCObject
{
public:
	static constexpr int32 MaxDetailedBuildings=512,MaxBatches=192,MaxInstances=32768;
	static constexpr int32 DerivedCustomDataFloats=11;
	static constexpr float DetailRadiusCm=30000.f;
	FAftershockArchitecturalRenderer()=default;
	virtual ~FAftershockArchitecturalRenderer() override;
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override{return TEXT("FAftershockArchitecturalRenderer");}
	/** Stable topology is rebuilt only for descriptor/tier/focus-bucket changes.
	 * CPD values are excluded from its hash and updated through SyncBuilding. */
	bool Refresh(AActor& Host,const TArray<FAftershockArchitecturalCandidate>& Candidates,
		EAftershockArchitecturalTier Tier,FVector Focus,bool bForce=false);
	bool OwnsDefinition(const UBuildingData* Definition) const;
	bool NeedsRefresh(EAftershockArchitecturalTier Tier,FVector Focus) const;
	void SyncBuilding(int32 BuildingID,const FAftershockArchitecturalInput* Input);
	/** Same recipe and shape seed as placement; only one shared red/green MID. */
	bool ShowPreview(AActor& Host,const FAftershockArchitecturalInput& Input,UMaterialInterface* GhostMaterial);
	void HidePreview();
	void Reset();
	const FAftershockArchitectureWork& GetWork() const{return Work;}
	/** Read-only actual ID-to-instance fan-out evidence; never updates the renderer. */
	void ReadCanonicalCPDAudit(const TArray<FAftershockArchitecturalCandidate>& Inputs,FAftershockArchitectureAudit& Out) const;

private:
	struct FBatchKey
	{
		UStaticMesh* Mesh=nullptr;
		UMaterialInterface* Material=nullptr;
		UMaterialInterface* Accent=nullptr;
		uint8 LOD=0;
        bool bNativeMaterials=false;
		bool operator==(const FBatchKey& B) const{return Mesh==B.Mesh&&Material==B.Material&&Accent==B.Accent&&LOD==B.LOD&&bNativeMaterials==B.bNativeMaterials;}
		friend uint32 GetTypeHash(const FBatchKey& B)
		{return HashCombineFast(HashCombineFast(HashCombineFast(GetTypeHash(B.Mesh),GetTypeHash(B.Material)),GetTypeHash(B.Accent)),HashCombineFast(B.LOD,GetTypeHash(B.bNativeMaterials)));}
	};
	struct FInstanceRef{UHierarchicalInstancedStaticMeshComponent* Batch=nullptr;int32 Index=INDEX_NONE;};
	struct FPrepared
	{
		const FAftershockArchitecturalCandidate* Candidate=nullptr;
		FAftershockArchitecturalRecipe Recipe;
		EAftershockArchitecturalTier Tier=EAftershockArchitecturalTier::Regional;
	};
	struct FPendingBatch
	{
		TArray<FTransform> Transforms;
		TArray<float> CustomData;
		TArray<int32> BuildingIDs;
		TArray<uint64> InstanceKeys; // (stableBuildingID<<32)|recipePartOrdinal
	};
	struct FBatchMembership
	{
		TMap<uint64,int32> IndexByKey;
		TArray<int32> FreeIndices;
	};
	TMap<EAftershockArchitecturalPart,TObjectPtr<UStaticMesh>> CoreMeshes;
	TMap<EAftershockArchitecturalFinish,TObjectPtr<UMaterialInterface>> SharedMaterials;
	TMap<FString,TObjectPtr<UStaticMesh>> OptionalFacades;
	TMap<FBatchKey,TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> Batches;
	TMap<FBatchKey,FBatchMembership> BatchMembership;
	TMap<int32,TArray<FInstanceRef>> ReferencesByID;
	TSet<const UBuildingData*> OwnedDefinitions;
	uint64 TopologyHash=0;
	int32 LastQuality=INDEX_NONE;
	EAftershockArchitecturalTier LastTier=EAftershockArchitecturalTier::Globe;
	FIntPoint LastFocusBucket=FIntPoint(MAX_int32,MAX_int32);
	bool bTriedAssets=false,bAssetsReady=false,bPreview=false,bWarnedMissingFacade=false;
	FAftershockArchitectureWork Work;
	bool EnsureAssets();
	bool HasNativeFacadeFamily(int32 Floors);
	UStaticMesh* ResolveMesh(const FAftershockArchitecturalPlacement& Part,int32 Floors,uint8 Bays);
	bool AppendPrepared(const FPrepared& Prepared,UMaterialInterface* OverrideMaterial,TMap<FBatchKey,FPendingBatch>& Pending);
	bool Submit(AActor& Host,TMap<FBatchKey,FPendingBatch>& Pending,bool bIsPreview);
	void ClearInstances();
};
