#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AftershockMassMovement.h"
#include "AftershockMassMovementVisual.generated.h"

class UInstancedStaticMeshComponent;
class UStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class UMaterialInstanceDynamic;

/** Immutable accepted replay, owned by the existing disaster event authority.
 * WorldCm = WorldOffsetCm + PhysicalMetres *100. No additional elevation model.
 * Parent supplies the SAME staged cell inventory and obstacle footprints. */
struct FAftershockMassVisualEvent
{
    FGuid EventID;
    double StartedAtWorldSeconds=0.;
    double CellSizeM=10.;
    FVector2D GridOriginM=FVector2D::ZeroVector;
    FVector WorldOffsetCm=FVector::ZeroVector;
    TArray<FAftershockMassCell> SupportCells;
    /** Resolved blocking footprints only; exclude callbacks that explicitly allow flow. */
    TArray<FAftershockMassObstacle> Obstacles;
    FAftershockMassResult Result;
};

struct FAftershockMassVisualStats
{
    FGuid EventID;
    uint64 TerrainRevision=0;
    int32 FramePair=INDEX_NONE,SurfaceCells=0,SurfaceTriangles=0,VisibleClasts=0,VisiblePowderCards=0;
    int32 UnsafeChordIntervals=0,UnsupportedSurfaceCells=0,MeshBuilds=0,EffectsQuality=0;
    double MovingVolumeM3=0.,DepositedVolumeM3=0.,SuppressedVolumeM3=0.,RenderedVolumeM3=0.;
    double AnalyticSurfaceVolumeM3=0.,PlaybackSeconds=0.;
    bool bBound=false,bComplete=false,bMaterialsReady=false,bFinalDepositRetained=false,bReplayGeometryValid=false;
    /** Interpolation between measured samples is display-only, not a new solve. */
    bool bSampledTimeInterpolation=true;
    FBox EventWorldBounds=FBox(ForceInit);
};

/** One host/event, one shared surface and two shared ISM batches. No actor,
 * Tick, skeletal mesh, component or MID per parcel. No game-state writes.
 * Flow/deposit skins use actual cell volumes. Only a frame-pair transition
 * builds a mesh; 12.5Hz animation changes a shared WPO scalar and ISM transforms.
 * Completed deposit stays visible until its canonical aftermath owner explicitly
 * clears/transfers it. It has no weather fade or automatic expiry. */
UCLASS(NotBlueprintable, Transient)
class JAMPROJECT_API AAftershockMassMovementVisualActor final : public AActor
{
    GENERATED_BODY()
public:
    AAftershockMassMovementVisualActor();
    bool BeginAcceptedEvent(const FAftershockMassVisualEvent& InEvent,FString& OutFailure);
    /** World simulation clock, not wall-clock; loading resumes without Solve. */
    void SetPlaybackWorldSeconds(double WorldSeconds);
    void SetExternallyClocked(bool bValue=true) { bExternallyClocked=bValue; }
    void FinishMotionKeepDeposit();
    /** Explicit aftermath hand-off/world teardown only; clears retained deposit. */
    void ClearEvent();
    const FAftershockMassVisualStats& GetVisualStats()const{return Stats;}
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason)override;
private:
    void UpdatePresentation();
    bool ValidateEvent(const FAftershockMassVisualEvent& Value,FString& Why);
    bool SampleFrozenSupport(const FVector2D& PositionM,double& Z,FVector& Normal)const;
    bool ChordSupported(const FAftershockMassParcelFrame& A,const FAftershockMassParcelFrame& B,int64& Work)const;
    bool BuildFramePair(int32 Pair);
    void UpdateDetails(int32 Pair,double Alpha);
    void ApplyVisibility();
    FVector ToWorld(const FVector& Metres)const{return Event.WorldOffsetCm+Metres*100.;}
    UPROPERTY() TObjectPtr<UStaticMeshComponent> FlowAndDepositSurface;
    UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> Clasts;
    UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> PowderCards;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> SurfaceMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> ClastMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> PowderMaterial;
    /** Soft CDO references must be included in the canonical cook manifest. */
    UPROPERTY() TSoftObjectPtr<UMaterialInterface> SnowSurfaceAsset;
    UPROPERTY() TSoftObjectPtr<UMaterialInterface> SoilSurfaceAsset;
    UPROPERTY() TSoftObjectPtr<UMaterialInterface> SnowPowderAsset;
    UPROPERTY() TSoftObjectPtr<UMaterialInterface> SoilDustAsset;
    UPROPERTY() TSoftObjectPtr<UStaticMesh> SnowClastAsset;
    UPROPERTY() TSoftObjectPtr<UStaticMesh> RockClastAsset;
    UPROPERTY() TSoftObjectPtr<UStaticMesh> PowderCardAsset;
    FAftershockMassVisualEvent Event;
    FAftershockMassVisualStats Stats;
    TMap<FIntPoint,int32> SupportIndex;
    TMap<FIntPoint,double> FrozenBedM;
    TArray<TMap<int32,int32>> ParcelIndices;
    TArray<int32> ParcelIDs;
    TArray<FTransform> ClastTransforms,PowderTransforms;
    TArray<double> PairSuppressedA,PairSuppressedB,PairVisibleA,PairVisibleB;
    FTimerHandle PresentationTimer;
    int32 CurrentPair=INDEX_NONE;
    double CurrentWorldSeconds=0.;
    bool bExternallyClocked=false;
    float ClastNativeRadiusCm=1.;
};
