#pragma once
#include "CoreMinimal.h"

class AActor;
class UStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class UTerrainGridSubsystem;

/** Exact-ground, collision-free roads cached per canonical25x25cell chunk.
 * Initial125x125frame: at most25chunks/75components. General arbitrary128cell
 * window: at most49global chunks/147components. No per-road actor/tick/save. */
class FAftershockConformingRoads
{
public:
	explicit FAftershockConformingRoads(AActor& InOwner);
	~FAftershockConformingRoads();
	bool Refresh(const UTerrainGridSubsystem& Terrain,UStaticMeshComponent* InstalledGround);
	void ApplyPresentationBudget();
	bool HasRoadTile(FIntPoint Coord) const{return bReady&&RoadTiles.Contains(Coord);}
	int32 GetRoadTileCount() const{return bReady?RoadTiles.Num():0;}
private:
	struct FSourceChunk{TArray<TArray<int32>> TriangleOffsets;};
	struct FPresentedChunk
	{
		TWeakObjectPtr<UStaticMeshComponent> Batches[3];
		TSet<FIntPoint> Tiles;
		uint64 Signature=0;
		bool bHasSignature=false;
	};
	TWeakObjectPtr<AActor> Owner;
	TWeakObjectPtr<UStaticMeshComponent> Ground;
	TWeakObjectPtr<UStaticMesh> CachedGround;
	TWeakObjectPtr<UMaterialInterface> Materials[3];
	TMap<FIntPoint,FSourceChunk> SourceChunks;
	TMap<FIntPoint,FPresentedChunk> PresentedChunks;
	TSet<FIntPoint> RoadTiles;
	uint64 GeometrySignature=0;
	bool bHasGeometry=false,bReady=false;
	void DestroyChunk(FPresentedChunk& Chunk);
	void ResetPresentation();
	UMaterialInterface* ResolveMaterial(int32 Batch);
};
