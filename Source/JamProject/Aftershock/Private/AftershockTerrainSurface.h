#pragma once

#include "CoreMinimal.h"

class AActor;
class UTerrainGridSubsystem;
class UStaticMeshComponent;
class UWorld;
class UStaticMesh;
class UMaterialInterface;
struct FAftershockDisasterVisualSnapshot;


/** Display-only mesh data; no collision or simulation-authoritative heights. */
struct FAftershockPresentationMeshVertex
{
    FVector Position;
    FVector Normal=FVector::UpVector;
    FVector2f UV=FVector2f::ZeroVector;
    FLinearColor Color=FLinearColor::White;
};
UStaticMesh* BuildAftershockPresentationMesh(AActor* Owner,const TCHAR* Name,
    const TArray<FAftershockPresentationMeshVertex>& Vertices,const TArray<int32>& Indices,
    UMaterialInterface* Material,const TArray<TArray<int32>>* ExtraLODIndices=nullptr);

/** Presentation window only. Initial dense frame is exact; expanded worlds draw at most 3x3 chunks. */
FIntRect GetAftershockVisibleCellBounds(const UTerrainGridSubsystem& Terrain);
/** Draw-only requested/installed ground intersection. No population/navigation semantics. */
FIntRect GetAftershockDrawableCellBounds(const UTerrainGridSubsystem& Terrain);
/** Read-only elevation: canonical river diagonals use matching triangles; other quads remain bilinear. Missing neighbours never create phantom zero-height terrain. */
float SampleAftershockResidentElevation(const UTerrainGridSubsystem& Terrain,float GridX,float GridY);
/** Exact geometry inputs only: occupancy/roads/economy revisions never invalidate an unchanged ground mesh. */
uint64 GetAftershockTerrainGeometrySignature(const UTerrainGridSubsystem& Terrain, const FIntRect& Bounds);

/** Bounded weak-cache telemetry. Counts actual work, not inferred memory savings. */
struct FAftershockTerrainSurfaceBuildStats
{
	uint64 GroundBuildAttempts = 0;
	uint64 SuccessfulGroundBuilds = 0;
	uint64 CollisionCookAttempts = 0;
	uint64 GroundCacheHits = 0;
	int32 CachedGroundComponents = 0;
	int32 ActiveGroundComponents = 0;
	int32 LastLOD0Triangles = 0;
	int32 LastRenderLODs = 0;
};

FAftershockTerrainSurfaceBuildStats GetAftershockTerrainSurfaceBuildStats(const UWorld* World);
/** Applies an existing render LOD only; exact LOD0 remains the collision/picking mesh. */
void ApplyAftershockTerrainPresentationBudget(UStaticMeshComponent* GroundComponent, const UWorld* World);

/** Sea geometry is clipped to canonical ocean cells, which are not playable land. */
bool BuildAftershockCoastalSeaSurface(AActor* Owner, const UTerrainGridSubsystem* Terrain,
	UStaticMeshComponent* SurfaceComponent);

struct FAftershockTsunamiSurfaceData
{
	TMap<FIntPoint, float> PeakDepthCm;
	TMap<FIntPoint, float> ArrivalFraction;
	int32 OceanCells = 0;
	int32 AffectedLandCells = 0;
	int32 Triangles = 0;
};

/** Event-time visual mesh; canonical severity is never modified. */
bool BuildAftershockTsunamiSurface(AActor* Owner, const UTerrainGridSubsystem* Terrain,
	const FAftershockDisasterVisualSnapshot& Snapshot, UStaticMeshComponent* SurfaceComponent,
	FAftershockTsunamiSurfaceData& OutData);

/** Builds presentation geometry from the canonical grid without writing simulation data. */
bool BuildAftershockTerrainSurface(AActor* Owner, const UTerrainGridSubsystem* Terrain,
	UStaticMeshComponent* GroundComponent, UStaticMeshComponent* WaterComponent);

bool BuildAftershockGlobeVisual(AActor* Owner, const UTerrainGridSubsystem* Terrain,
	UStaticMeshComponent* GlobeComponent, UStaticMeshComponent* TransitionComponent, UStaticMeshComponent* AtmosphereComponent);

struct FAftershockFloodSurfaceStats
{
	int32 CanonicalPositiveCells = 0;
	int32 ReconstructedWetCells = 0;
	int32 ReconstructedLandCells = 0;
	int32 SurfaceTriangles = 0;
	int32 FrontTriangles = 0;
	float MaximumDisplayDepthCm = 0.f;
	float EncodingCm = 512.f;
	double InitialDisplayColumnSumCm = 0.0;
	double FinalDisplayColumnSumCm = 0.0;
};

/**
 * Bounded hydrostatic DISPLAY reconstruction, not a new gameplay flood solver.
 * The input is dimensionless canonical severity. Art calibration: land runoff
 * = max(severity-6,0)*0.18 cm; river column = 12 + 2.2*max(severity-
 * RiverCapacity*18.75,0) cm. That gate matches the canonical .75 flow-to-severity
 * and capacity*25 overflow condition. 160 conservative
 * four-neighbour relaxation iterations redistribute that display volume over
 * canonical elevations, only within positive, playable input cells. No seconds,
 * rainfall-mm, physical water-depth or velocity claim follows from these values.
 * Residual columns below 12 cm do not create an inundation sheet. The continuous
 * surface clips against terrain at shores; boundary faces close exposed fronts.
 * Vertex RGB = (rise/512, displayDepth/512, topFaceMask), gamma precompensated.
 * A clipped mesh is built at event start and when its chunk window changes;
 * shader FloodEnvelope controls rise/drain at the original age, without collision.
 */
bool BuildAftershockFloodSurface(AActor* Owner, const UTerrainGridSubsystem* Terrain,
	const TMap<FIntPoint, float>& LocalSeverity, UStaticMeshComponent* SurfaceComponent,
	FAftershockFloodSurfaceStats* OutStats = nullptr,
	TMap<FIntPoint, float>* OutVisibleDisplayDepthCm = nullptr,
	const FAftershockDisasterVisualSnapshot* PhysicalSnapshot = nullptr);
