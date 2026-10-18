#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "RoadNetworkSubsystem.generated.h"

/** Read-only quote derived from canonical grid occupancy. No cells are reserved by a preview. */
USTRUCT(BlueprintType)
struct JAMPROJECT_API FAftershockRoadQuote
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Aftershock|Roads")
	bool bRouteValid = false;
	UPROPERTY(BlueprintReadOnly, Category="Aftershock|Roads")
	TArray<FIntPoint> Path;
	UPROPERTY(BlueprintReadOnly, Category="Aftershock|Roads")
	int32 NewRoadCells = 0;
	UPROPERTY(BlueprintReadOnly, Category="Aftershock|Roads")
	int32 CostTL = 0;
	UPROPERTY(BlueprintReadOnly, Category="Aftershock|Roads")
	FText Feedback;
	/** Path-parallel road/deck/ramp world Z. Preview never writes terrain. */
	UPROPERTY(BlueprintReadOnly, Category="Aftershock|Roads")
	TArray<float> SurfaceElevationsCm;
	UPROPERTY(BlueprintReadOnly, Category="Aftershock|Roads")
	bool bIncludesBridge = false;
	UPROPERTY(BlueprintReadOnly, Category="Aftershock|Roads")
	uint8 BridgeAxis = 0;
	UPROPERTY(BlueprintReadOnly, Category="Aftershock|Roads")
	int32 BridgeRiverCells = 0;
	UPROPERTY(BlueprintReadOnly, Category="Aftershock|Roads")
	int32 BridgeApproachCells = 0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnAftershockRoadNetworkChanged);

/** Player-authored, four-neighbour roads. FGridCell::bHasRoad is the sole road authority. */
UCLASS(BlueprintType)
class JAMPROJECT_API URoadNetworkSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	UPROPERTY(BlueprintAssignable, Category="Aftershock|Roads")
	FOnAftershockRoadNetworkChanged OnRoadNetworkChanged;

	UFUNCTION(BlueprintPure, Category="Aftershock|Roads")
	int32 GetRoadCellCostTL() const { return 200; }

	/** Orthogonal bank selections crossing a river quote a complete bridge and ramps automatically. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Roads")
	FAftershockRoadQuote QuoteRoad(FIntPoint Start, FIntPoint End) const;

	/** Requotes, verifies funds, commits all new cells and spends exactly once. No partial road on failure. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Roads")
	bool BuildRoad(FIntPoint Start, FIntPoint End, FAftershockRoadQuote& OutQuote);

	/** Removes one existing road cell. Demolition is free and does not refund construction. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Roads")
	bool RemoveRoadAt(FIntPoint Coords);

	UFUNCTION(BlueprintPure, Category="Aftershock|Roads")
	bool HasRoad(FIntPoint Coords) const;

	UFUNCTION(BlueprintPure, Category="Aftershock|Roads")
	int32 CountRoadCells() const;

	/** Startup-only authored infrastructure. Validates the entire empty-land set,
	 * commits once without spending the player's starting treasury, and emits the
	 * same network event as a paid extension. Not exposed as a player action. */
	bool SeedInitialRoads(const TArray<FIntPoint>& AuthoredCells);
	/** Shared traversal contract: closed bridges never connect roads, services or pedestrians. */
	bool IsRoadTraversable(FIntPoint Coords) const;
	bool CanTraverseRoadEdge(FIntPoint From, FIntPoint To) const;
	bool IsPublicWalkingCell(FIntPoint Coords) const;
	bool CanTraverseWalkingEdge(FIntPoint From, FIntPoint To) const;
    /** Geometry-only shared query for a canonical walkability snapshot. Caller
     * MUST gate both cells with captured public-cell costs; movement uses the
     * full live CanTraverseWalkingEdge. No hazard or profile formula duplicated. */
    bool CanTraverseWalkingProfileEdge(FIntPoint From,FIntPoint To) const{return IsProfileEdgeAllowed(From,To);}
	/** Building frontage/kerb access: elevated bridge interiors have no side entrance. */
	bool CanAccessRoadFromCell(FIntPoint From, FIntPoint Road) const;
	/** Construction-only queries shared with automatic building access. They
	 * reject reserved parcels, water, active hazards and unbuildable grades. */
	bool CanBuildAutomaticAccessCell(FIntPoint Coord) const;
	bool CanBuildAutomaticAccessEdge(FIntPoint From,FIntPoint To) const;
	/** Called once after a complete paid building/access purchase, never per cell. */
	void NotifyBuildingAccessCommitted();
	float GetRoadSurfaceElevation(FIntPoint Coords) const;
	/** Canonical displayed water above the actual road/deck, not the river bed. */
	float GetRoadFloodDepthCm(FIntPoint Coords) const;
	float SampleRoadSurfaceElevation(FVector WorldLocation) const;
	FVector RoadGridToWorld(FIntPoint Coords, bool bCenter = true) const;
	bool IsBridgeClosed(int32 BridgeID) const { return ClosedBridgeIDs.Contains(BridgeID); }
	int32 GetBridgeCount() const { return BridgeCellsByID.Num(); }
	/** Save owner calls after all canonical subsystems have restored, without charging or rebuilding roads. */
	void NotifySaveRestored();

private:
	bool TryQuoteBridge(FIntPoint Start, FIntPoint End, FAftershockRoadQuote& OutQuote) const;
	bool IsCellHazardSafe(FIntPoint Coord, bool bVehicle) const;
	bool IsProfileEdgeAllowed(FIntPoint From, FIntPoint To) const;
	void RebuildBridgeMembership();
	void RefreshBridgeClosures();
	void NotifyNetworkCommitted();
	void HandleTerrainStorageChanged(uint64 Revision);
	FDelegateHandle TerrainStorageDelegateHandle;
	TMap<int32,TArray<FIntPoint>> BridgeCellsByID;
	TSet<int32> ClosedBridgeIDs;
	FTimerHandle BridgeClosureTimer;
	bool bTransactionInProgress = false;
};
