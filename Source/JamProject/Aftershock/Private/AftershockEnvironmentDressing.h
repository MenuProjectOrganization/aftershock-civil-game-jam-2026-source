#pragma once

#include "CoreMinimal.h"
#include "AftershockEnvironmentQuality.h"

class AActor;
class UHierarchicalInstancedStaticMeshComponent;
class UTerrainGridSubsystem;

/** Read-only derived presentation telemetry. Never population, land-use or fuel authority. */
struct FAftershockEnvironmentDressingStats
{
	int32 ScannedCells=0,EligibleCells=0,Grass=0,Shrubs=0,Conifers=0,Palms=0,Broadleaf=0;
	int32 ChangedBatches=0,ChangedInstances=0,ReadyMeshes=0,MissingMeshes=0;
	int32 RejectedFootprints=0,RejectedGround=0;
	uint32 QualitySignature=0;
	bool bOverviewDisabled=false,bRequiredTreeFamilyMissing=false,bReplacesLegacyWoodland=false;
	FIntRect ScanBounds=FIntRect(0,0,0,0);
	int32 GetTrees() const {return Conifers+Palms+Broadleaf;}
};

/**
 * One existing city actor owns every component. This ordinary C++ helper owns
 * weak component references and derived render slots only: no UObject subsystem,
 * timer, Actor/Tick/MID per plant, collision, save state or canonical mutation.
 * Call Refresh from the existing1Hz city poll, and after canonical road/building
 * changes. A10m view bucket/quality/terrain signature gates bounded diff rebuilds.
 */
class FAftershockEnvironmentDressing final
{
public:
	explicit FAftershockEnvironmentDressing(AActor& InOwner);
	bool Refresh(const UTerrainGridSubsystem& Terrain,bool bForce=false);
	/** Cheap camera-tier/quality visibility changes; safe from existing0.1s budget callback. */
	void ApplyPresentationBudget();
	bool ShouldReplaceLegacyWoodland() const {return Stats.bReplacesLegacyWoodland;}
	const FAftershockEnvironmentDressingStats& GetStats() const {return Stats;}

private:
	enum class EFamily:uint8 {Grass,Shrub,Conifer,Palm,Broadleaf};
	struct FEntry
	{
		EFamily Family=EFamily::Grass;
		TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent> Component;
		TMap<FIntVector,int32> IndexByKey;
		TArray<FIntVector> KeyByIndex;
		FBox NativeBounds=FBox(ForceInit);
		bool bReady=false;
	};
	struct FDesired {FIntVector Key;FTransform Transform;float Phase=0.f;};
	TWeakObjectPtr<AActor> Owner;
	TArray<FEntry> Entries;
	FAftershockEnvironmentDressingStats Stats;
	uint64 LastSignature=0;
	bool bHasSignature=false;
	uint32 LastAppliedQuality=MAX_uint32;
	int32 LastAppliedTier=INDEX_NONE;
	int32 ReadyVariant(EFamily Family,uint32 Seed) const;
	bool FamilyReady(EFamily Family) const;
	bool Commit(const TArray<TArray<FDesired>>& Desired);
};
