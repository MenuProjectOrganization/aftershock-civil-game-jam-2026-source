#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RenderCommandFence.h"
#include "AftershockHydraulicSurfaceActor.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMeshComponent;
class UStaticMesh;
class UTexture2D;
class UTerrainGridSubsystem;
class FReferenceCollector;

// Borrowed, read-only presentation DTO. No UPROPERTY/save or simulation authority.
struct FAftershockHydraulicVisualCell
{
    static constexpr uint8 North = 1, East = 2, South = 4, West = 8;
    FIntPoint Coord = FIntPoint::ZeroValue;
    float BedElevationCm = 0.f;
    float SurfaceElevationCm = 0.f;
    float DepthCm = 0.f;
    FVector2f VelocityCmPerSecond = FVector2f::ZeroVector;
    // Owner supplies FREE-SURFACE BLEND connectivity, N=-Y,E=+X,S=+Y,W=-X.
    // A transmitting orifice can retain different surface heads: gate/dam
    // structure faces must remain blocked here even while hydraulic Q flows.
    // Both sides of a shared face must agree; unknown defaults safely closed.
    uint8 BlockedFaces = 0x0f;
};

struct FAftershockHydraulicVisualFrame
{
    TConstArrayView<FAftershockHydraulicVisualCell> Cells;
    double PhysicalSeconds = 0.;
    uint64 ConnectivityRevision = 0;
    // Root's authoritative bridge supplies only visual wind inputs. Wind forces
    // belong in the canonical physics model; this actor never adds them there.
    FVector2f VisualWindDirection = FVector2f(1.f, 0.f);
    float VisualWindStrength01 = 0.f;
};

enum class EAftershockHydraulicUploadResult : uint8
{
    Queued, Unchanged, Throttled, Busy, NeedsConfigure, Rejected
};

struct FAftershockHydraulicRenderStats
{
    uint64 ConfigurationEpoch = 0, GeometryBuilds = 0, SubmittedUploads = 0;
    uint64 LastQueuedRevision = 0, LastConsumedRevision = 0, LastConnectivityRevision = 0;
    uint64 SubmittedMaskUploads = 0, FailedRhiUploads = 0;
    uint64 GeometrySignatureScans = 0, ConfigurationCacheHits = 0;
    double ConsumedPhysicalSeconds = 0., LastPackingCpuMs = 0.;
    int32 TextureWidth = 256, TextureHeight = 256, PendingPackets = 0;
    int32 LastWetCells = 0, LastValidLocalCells = 0, ExpectedLocalCells = 0, SurfaceVertices = 0, SurfaceTriangles = 0;
    FIntRect DrawableBounds = FIntRect(0, 0, 0, 0);
};

/** One persistent visual actor. No Tick, timers, collision, physics or canonical writes. */
UCLASS(NotBlueprintable, Transient)
class JAMPROJECT_API AAftershockHydraulicSurfaceActor final : public AActor
{
    GENERATED_BODY()
public:
    AAftershockHydraulicSurfaceActor();
    // Owner calls on actual geometry/window install, not each solver substep.
    // Height reserve bounds only; it never creates/changes a hydraulic bed.
    bool Configure(const UTerrainGridSubsystem& Terrain, FText& Error, float HeightReserveCm = 10000.f);
    EAftershockHydraulicUploadResult Update(const FAftershockHydraulicVisualFrame& Frame,
        uint64 Revision, FText& Error);
    // Optional owner heartbeat. RHI cleanup also schedules one weak GT callback;
    // no per-actor/per-cell Tick is needed, including for a paused scene.
    void PumpUploads();
    void RefreshQuality();
    const FAftershockHydraulicRenderStats& GetRenderStats() const { return Stats; }
    UTexture2D* GetDataTextureForAudit() const { return FieldTexture; }
    UTexture2D* GetConnectivityTextureForAudit() const { return ConnectivityTexture; }
    UStaticMeshComponent* GetSurfaceForAudit() const { return Surface; }
    static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void BeginDestroy() override;
    virtual bool IsReadyForFinishDestroy() override;
    virtual void FinishDestroy() override;
private:
    struct FUploadPacket;
    bool EnsureResources(FText& Error);
    bool CanRetirePending() const;
    UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> Surface;
    UPROPERTY(Transient) TObjectPtr<UTexture2D> FieldTexture;
    UPROPERTY(Transient) TObjectPtr<UTexture2D> ConnectivityTexture;
    UPROPERTY(Transient) TObjectPtr<UStaticMesh> SurfaceMesh;
    UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> WaterMaterial;
    UPROPERTY() TObjectPtr<UMaterialInterface> SurfaceMaterialTemplate;
    TWeakObjectPtr<const UTerrainGridSubsystem> TerrainSource;
    TSharedPtr<FUploadPacket, ESPMode::ThreadSafe> PendingPacket;
    TArray<uint8> LastConsumedConnectivity;
    FRenderCommandFence ResourceFence, UploadFence, DestroyFence;
    FAftershockHydraulicRenderStats Stats;
    FVector ConfiguredGridOrigin = FVector::ZeroVector;
    float ConfiguredCellSize = 0.f, ConfiguredHeightReserveCm = 0.f, MinSupportedHeadCm = 0.f, MaxSupportedHeadCm = 0.f;
    uint64 GeometrySignature = 0;
    uint64 ConfiguredTerrainRevision = MAX_uint64;
    uint32 QualitySignature = MAX_uint32;
    double LastSubmissionRealSeconds = -1.e30;
    bool bConfigured = false, bResourceFenceStarted = false, bUploadFenceStarted = false;
    bool bDestroyFenceStarted = false, bShuttingDown = false, bLocalPresentationVisible = true;
};
