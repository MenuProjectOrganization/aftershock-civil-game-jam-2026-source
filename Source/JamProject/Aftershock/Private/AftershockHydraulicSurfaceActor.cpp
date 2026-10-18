#include "AftershockHydraulicSurfaceActor.h"

#include "AftershockTerrainSurface.h"
#include "AftershockHydraulicTerrain.h"
#include "AftershockEnvironmentQuality.h"
#include "AftershockPresentationBudget.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Async/Async.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "PixelFormat.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Templates/Atomic.h"
#include "TextureResource.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/GCObject.h"

namespace AftershockHydraulicSurfacePrivate
{
constexpr int32 HydraulicSurfaceFieldSize = 256, HydraulicSurfaceMaximumDrawableCellsPerAxis = HydraulicSurfaceFieldSize - 1;
constexpr int32 HydraulicSurfaceMaximumSurfaceCells = 16384;
constexpr double HydraulicSurfaceMinimumUploadIntervalSeconds = .1;
constexpr TCHAR HydraulicSurfaceMaterialPath[] = TEXT("/Game/Environment/Water/M_HydraulicSurface.M_HydraulicSurface");
static_assert(sizeof(FVector4f) == 16, "PF_A32B32G32R32F payload needs four contiguous float32 channels");
FText HydraulicSurfaceHydraulicError(const TCHAR* Message) { return FText::FromString(Message); }
}
using namespace AftershockHydraulicSurfacePrivate;

struct AAftershockHydraulicSurfaceActor::FUploadPacket
{
    TArray<FVector4f> Pixels;
    TArray<uint8> Connectivity;
    FUpdateTextureRegion2D Region = FUpdateTextureRegion2D(0, 0, 0, 0, HydraulicSurfaceFieldSize, HydraulicSurfaceFieldSize);
    TAtomic<bool> bConsumed{false};
    TAtomic<bool> bRhiUploadSucceeded{false};
    uint64 Epoch = 0, Revision = 0;
    uint64 ConnectivityRevision = 0;
    double PhysicalSeconds = 0.;
    FVector2f Wind = FVector2f(1.f, 0.f);
    float WindStrength = 0.f;
    int32 WetCells = 0, ValidLocalCells = 0;
    bool bPresentationApplied = false; // GT only.
    bool bUploadConnectivity = false;
};

AAftershockHydraulicSurfaceActor::AAftershockHydraulicSurfaceActor()
{
    PrimaryActorTick.bCanEverTick = false; PrimaryActorTick.bStartWithTickEnabled = false;
    Surface = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PersistentHydraulicSurface"));
    SetRootComponent(Surface); Surface->SetMobility(EComponentMobility::Movable);
    Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Surface->SetCollisionResponseToAllChannels(ECR_Ignore);
    Surface->SetCanEverAffectNavigation(false); Surface->SetCastShadow(false);
    Surface->SetAffectDistanceFieldLighting(false); Surface->SetVisibility(false);
    Surface->SetComponentTickEnabled(false);
    // Author this one material before the new source is integrated/cooked.
    // CDO reference makes the dependency visible to the cooker, not only a
    // dynamic LoadObject string. A missing asset still fails Configure clearly.
    ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(HydraulicSurfaceMaterialPath);
    if (Material.Succeeded()) SurfaceMaterialTemplate = Material.Object;
}

bool AAftershockHydraulicSurfaceActor::EnsureResources(FText& Error)
{
    bool bCreatedResource = false;
    if (!FieldTexture)
    {
        if (!GPixelFormats[PF_A32B32G32R32F].Supported)
        { Error = HydraulicSurfaceHydraulicError(TEXT("Platform PF_A32B32G32R32F su veri dokusunu desteklemiyor; ayrı doğrulanmış görsel fallback gerekli.")); return false; }
        // No image argument: CreateTransient does not auto-UpdateResource yet.
        // Set all data/sampling properties, initialize exact zeroes, then one init.
        FieldTexture = UTexture2D::CreateTransient(HydraulicSurfaceFieldSize, HydraulicSurfaceFieldSize, PF_A32B32G32R32F,
            MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), TEXT("AftershockHydraulicData")));
        if (!FieldTexture || !FieldTexture->GetPlatformData() || FieldTexture->GetPlatformData()->Mips.IsEmpty())
        { Error = HydraulicSurfaceHydraulicError(TEXT("Kalıcı float32 su veri dokusu oluşturulamadı.")); return false; }
        FieldTexture->SRGB = false; FieldTexture->NeverStream = true;
        FieldTexture->Filter = TF_Nearest; FieldTexture->AddressX = TA_Clamp; FieldTexture->AddressY = TA_Clamp;
#if WITH_EDITORONLY_DATA
        FieldTexture->MipGenSettings = TMGS_NoMipmaps;
#endif
        auto& Mip = FieldTexture->GetPlatformData()->Mips[0];
        void* InitialData = Mip.BulkData.Lock(LOCK_READ_WRITE);
        FMemory::Memzero(InitialData, HydraulicSurfaceFieldSize * HydraulicSurfaceFieldSize * sizeof(FVector4f));
        Mip.BulkData.Unlock(); FieldTexture->UpdateResource();
        bCreatedResource = true;
    }
    if (!ConnectivityTexture)
    {
        if (!GPixelFormats[PF_G8].Supported)
        { Error = HydraulicSurfaceHydraulicError(TEXT("Platform su bağlantı maskesi PF_G8 formatını desteklemiyor.")); return false; }
        ConnectivityTexture = UTexture2D::CreateTransient(HydraulicSurfaceFieldSize, HydraulicSurfaceFieldSize, PF_G8,
            MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), TEXT("AftershockHydraulicConnectivity")));
        if (!ConnectivityTexture || !ConnectivityTexture->GetPlatformData() || ConnectivityTexture->GetPlatformData()->Mips.Num() == 0)
        { Error = HydraulicSurfaceHydraulicError(TEXT("Kalıcı su bağlantı dokusu oluşturulamadı.")); return false; }
        ConnectivityTexture->SRGB = false; ConnectivityTexture->NeverStream = true;
        ConnectivityTexture->Filter = TF_Nearest; ConnectivityTexture->AddressX = TA_Clamp; ConnectivityTexture->AddressY = TA_Clamp;
#if WITH_EDITORONLY_DATA
        ConnectivityTexture->MipGenSettings = TMGS_NoMipmaps;
#endif
        auto& Mip = ConnectivityTexture->GetPlatformData()->Mips[0];
        void* InitialData = Mip.BulkData.Lock(LOCK_READ_WRITE);
        FMemory::Memset(InitialData, 0x0f, HydraulicSurfaceFieldSize * HydraulicSurfaceFieldSize);
        Mip.BulkData.Unlock(); ConnectivityTexture->UpdateResource(); bCreatedResource = true;
    }
    if (bCreatedResource) { ResourceFence.BeginFence(FRenderCommandFence::ESyncDepth::RHIThread); bResourceFenceStarted = true; }
    if (!WaterMaterial)
    {
        UMaterialInterface* Base = SurfaceMaterialTemplate.Get();
        if (!Base) Base = LoadObject<UMaterialInterface>(nullptr, HydraulicSurfaceMaterialPath);
        if (!Base) { Error = HydraulicSurfaceHydraulicError(TEXT("M_HydraulicSurface gerçek proje materyali bulunamadı; authoring/cook gerekli.")); return false; }
        SurfaceMaterialTemplate = Base;
        WaterMaterial = UMaterialInstanceDynamic::Create(Base, this);
        if (!WaterMaterial) { Error = HydraulicSurfaceHydraulicError(TEXT("Tek ortak su materyali oluşturulamadı.")); return false; }
        WaterMaterial->SetTextureParameterValue(TEXT("HydraulicField"), FieldTexture);
        WaterMaterial->SetTextureParameterValue(TEXT("HydraulicConnectivity"), ConnectivityTexture);
        WaterMaterial->SetScalarParameterValue(TEXT("HydraulicTextureSize"), HydraulicSurfaceFieldSize);
        Surface->SetMaterial(0, WaterMaterial); QualitySignature = MAX_uint32;
    }
    return true;
}

bool AAftershockHydraulicSurfaceActor::CanRetirePending() const
{
    return !PendingPacket.IsValid() || (PendingPacket->bConsumed.Load()
        && (!bUploadFenceStarted || UploadFence.IsFenceComplete()));
}

void AAftershockHydraulicSurfaceActor::PumpUploads()
{
    check(IsInGameThread());
    if (!PendingPacket.IsValid()) return;
    if (PendingPacket->bConsumed.Load() && !PendingPacket->bPresentationApplied)
    {
        // Callback means RHI has consumed the update command's source bytes,
        // not that a particular screen frame/texel was read back successfully.
        PendingPacket->bPresentationApplied = true;
        if (!bShuttingDown && bConfigured && PendingPacket->Epoch == Stats.ConfigurationEpoch && WaterMaterial && PendingPacket->bRhiUploadSucceeded.Load())
        {
            WaterMaterial->SetScalarParameterValue(TEXT("HydraulicPhysicalSeconds"), static_cast<float>(PendingPacket->PhysicalSeconds));
            WaterMaterial->SetVectorParameterValue(TEXT("HydraulicWind"), FLinearColor(PendingPacket->Wind.X, PendingPacket->Wind.Y, PendingPacket->WindStrength, 0.f));
            Stats.LastConsumedRevision = PendingPacket->Revision; Stats.ConsumedPhysicalSeconds = PendingPacket->PhysicalSeconds;
            Stats.LastConnectivityRevision = PendingPacket->ConnectivityRevision;
            if (PendingPacket->bUploadConnectivity) LastConsumedConnectivity = MoveTemp(PendingPacket->Connectivity);
            Stats.LastWetCells = PendingPacket->WetCells; Stats.LastValidLocalCells = PendingPacket->ValidLocalCells;
            Surface->SetVisibility(bLocalPresentationVisible && PendingPacket->WetCells > 0);
        }
        else if (!PendingPacket->bRhiUploadSucceeded.Load())
        {
            Stats.FailedRhiUploads++; Stats.LastQueuedRevision = Stats.LastConsumedRevision;
            if (Surface) Surface->SetVisibility(false);
            UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_HYDRAULIC_UPLOAD_FAILED revision=%llu epoch=%llu exactTextureResourceMissing=1 sourceStateChanged=0"), PendingPacket->Revision, PendingPacket->Epoch);
        }
    }
    if (CanRetirePending()) { PendingPacket.Reset(); bUploadFenceStarted = false; Stats.PendingPackets = 0; }
}

bool AAftershockHydraulicSurfaceActor::Configure(const UTerrainGridSubsystem& Terrain, FText& Error, float HeightReserveCm)
{
    check(IsInGameThread()); Error = FText::GetEmpty(); PumpUploads();
    if (bShuttingDown || !FMath::IsFinite(Terrain.CellSize) || Terrain.CellSize <= 0.f
        || Terrain.GridOrigin.ContainsNaN() || !FMath::IsFinite(HeightReserveCm) || HeightReserveCm < 0.f || HeightReserveCm > 100000.f)
    { Error = HydraulicSurfaceHydraulicError(TEXT("Su görünümü için geçerli arazi ve bounded yükseklik rezervi gerekli.")); return false; }
    const FIntRect Bounds = GetAftershockDrawableCellBounds(Terrain);
    if (Bounds.Width() <= 0 || Bounds.Height() <= 0 || Bounds.Width() > HydraulicSurfaceMaximumDrawableCellsPerAxis || Bounds.Height() > HydraulicSurfaceMaximumDrawableCellsPerAxis
        || static_cast<int64>(Bounds.Width()) * Bounds.Height() > HydraulicSurfaceMaximumSurfaceCells)
    { Surface->SetVisibility(false); Error = HydraulicSurfaceHydraulicError(TEXT("Su görünüm penceresi 1..255 hücre/eksen olmalı; fizik resident alanı kırpılmadı.")); return false; }
    const bool SameFrame=bConfigured&&Stats.DrawableBounds==Bounds&&ConfiguredGridOrigin==Terrain.GridOrigin
        &&ConfiguredCellSize==Terrain.CellSize&&ConfiguredHeightReserveCm==HeightReserveCm;
    if(SameFrame&&ConfiguredTerrainRevision==Terrain.GetTerrainRevision())
    {++Stats.ConfigurationCacheHits;RefreshQuality();return true;}
    ++Stats.GeometrySignatureScans;
    const uint64 NewSignature = GetAftershockTerrainGeometrySignature(Terrain, Bounds);
    if (bConfigured && GeometrySignature == NewSignature && Stats.DrawableBounds == Bounds
        && ConfiguredGridOrigin == Terrain.GridOrigin && ConfiguredCellSize == Terrain.CellSize && ConfiguredHeightReserveCm == HeightReserveCm)
    { ConfiguredTerrainRevision=Terrain.GetTerrainRevision();++Stats.ConfigurationCacheHits;RefreshQuality();return true; }
    Surface->SetVisibility(false);
    if (!CanRetirePending())
    { Error = HydraulicSurfaceHydraulicError(TEXT("Önceki tek su upload paketi sürüyor; pencere kurulumu tekrar denenmeli.")); return false; }
    if (!EnsureResources(Error)) return false;
    float MinGround = MAX_flt, MaxGround = -MAX_flt; int32 ExpectedLocalCells = 0;
    for (const FGridCell& Cell : Terrain.Cells) if (Bounds.Contains(Cell.Coords))
    {
        double BedCm=0.,BankCm=0.;
        if(!TryGetAftershockHydraulicBedCm(Cell,true,BedCm,Error)||!TryGetAftershockMaterialGroundCm(Cell,true,BankCm,Error))return false;
        MinGround=FMath::Min(MinGround,static_cast<float>(BedCm));MaxGround=FMath::Max(MaxGround,static_cast<float>(BankCm));ExpectedLocalCells++;
    }
    if (MinGround == MAX_flt || !FMath::IsFinite(MinGround) || !FMath::IsFinite(MaxGround))
    { Error = HydraulicSurfaceHydraulicError(TEXT("Görünür su penceresinde sonlu zemin yok.")); return false; }
    const float NewMinHead = MinGround - HeightReserveCm, NewMaxHead = MaxGround + HeightReserveCm;
    if (!FMath::IsFinite(NewMinHead) || !FMath::IsFinite(NewMaxHead))
    { Error = HydraulicSurfaceHydraulicError(TEXT("Su bounds rezervi sayısal sınırı aşıyor.")); return false; }
    TArray<FAftershockPresentationMeshVertex> Vertices; TArray<int32> Indices;
    // Persistent 2x2 patches retain a canonical centre sample as well as face
    // midpoints. A corner-only quad cannot represent a cell-centred head and
    // produces two large flat triangles even when the uploaded field varies.
    // Topology is built only at Configure, never per hydraulic publication.
    constexpr int32 Subdivisions = 2, VerticesPerAxis = Subdivisions + 1;
    Vertices.Reserve(Bounds.Width() * Bounds.Height() * VerticesPerAxis * VerticesPerAxis + 2);
    Indices.Reserve(Bounds.Width() * Bounds.Height() * Subdivisions * Subdivisions * 6);
    constexpr float UVEpsilon = .0001f;
    for (int32 Y = 0; Y < Bounds.Height(); ++Y) for (int32 X = 0; X < Bounds.Width(); ++X)
    {
        const int32 A = Vertices.Num();
        for (int32 VY = 0; VY <= Subdivisions; ++VY) for (int32 VX = 0; VX <= Subdivisions; ++VX)
        {
            const float FX = static_cast<float>(VX) / Subdivisions, FY = static_cast<float>(VY) / Subdivisions;
            auto& V = Vertices.AddDefaulted_GetRef();
            V.Position = FVector((X + FX) * Terrain.CellSize, (Y + FY) * Terrain.CellSize, 0.);
            V.Normal = FVector::UpVector;
            // Coincident edge vertices belong to just one patch on each side;
            // the shader joins open faces and preserves a real wall head jump.
            // Inset UVs preserve ownership, without moving the geometry itself.
            V.UV = FVector2f((X + FMath::Clamp(FX, UVEpsilon, 1.f - UVEpsilon)) / HydraulicSurfaceFieldSize,
                (Y + FMath::Clamp(FY, UVEpsilon, 1.f - UVEpsilon)) / HydraulicSurfaceFieldSize);
        }
        for (int32 QY = 0; QY < Subdivisions; ++QY) for (int32 QX = 0; QX < Subdivisions; ++QX)
        {
            const int32 TL = A + QX + QY * VerticesPerAxis, BL = TL + VerticesPerAxis;
            Indices.Append({TL, BL, TL + 1, TL + 1, BL, BL + 1});
        }
    }
    // The shared fresh-resource helper derives bounds from all source vertices
    // and compacts only indexed vertices. Two unreferenced bounds witnesses
    // give WPO a conservative Z range without mutating live mesh buffers.
    for (float Z : {NewMinHead, NewMaxHead})
    { auto& V = Vertices.AddDefaulted_GetRef(); V.Position = FVector(0., 0., Z - Terrain.GridOrigin.Z); }
    UStaticMesh* NewMesh = BuildAftershockPresentationMesh(this, TEXT("AftershockPersistentHydraulicGrid"), Vertices, Indices, WaterMaterial);
    if (!NewMesh) { Error = HydraulicSurfaceHydraulicError(TEXT("Kalıcı su grid görünümü kurulamadı.")); return false; }
    SurfaceMesh = NewMesh; Surface->SetStaticMesh(SurfaceMesh);
    SetActorTransform(FTransform(FQuat::Identity,
        FVector(Terrain.GridOrigin.X + Bounds.Min.X * Terrain.CellSize, Terrain.GridOrigin.Y + Bounds.Min.Y * Terrain.CellSize, Terrain.GridOrigin.Z)));
    WaterMaterial->SetScalarParameterValue(TEXT("HydraulicCellSizeCm"), Terrain.CellSize);
    WaterMaterial->SetScalarParameterValue(TEXT("HydraulicOriginZCm"), static_cast<float>(Terrain.GridOrigin.Z));
    WaterMaterial->SetVectorParameterValue(TEXT("HydraulicWindowSize"), FLinearColor(Bounds.Width(), Bounds.Height(), 0.f, 0.f));
    TerrainSource = &Terrain; ConfiguredGridOrigin = Terrain.GridOrigin; ConfiguredCellSize = Terrain.CellSize;
    ConfiguredHeightReserveCm = HeightReserveCm; MinSupportedHeadCm = NewMinHead; MaxSupportedHeadCm = NewMaxHead; GeometrySignature = NewSignature;
    ConfiguredTerrainRevision=Terrain.GetTerrainRevision();
    Stats.ConfigurationEpoch++; Stats.GeometryBuilds++; Stats.DrawableBounds = Bounds;
    Stats.SurfaceVertices = Bounds.Width() * Bounds.Height() * VerticesPerAxis * VerticesPerAxis;
    Stats.SurfaceTriangles = Indices.Num() / 3;
    Stats.ExpectedLocalCells = ExpectedLocalCells;
    Stats.LastQueuedRevision = Stats.LastConsumedRevision = 0; Stats.ConsumedPhysicalSeconds = 0.;
    Stats.LastConnectivityRevision = 0; LastConsumedConnectivity.Empty();
    Stats.LastWetCells = Stats.LastValidLocalCells = 0; LastSubmissionRealSeconds = -1.e30; bConfigured = true;
    QualitySignature = MAX_uint32; RefreshQuality(); return true;
}

EAftershockHydraulicUploadResult AAftershockHydraulicSurfaceActor::Update(
    const FAftershockHydraulicVisualFrame& Frame, uint64 Revision, FText& Error)
{
    check(IsInGameThread()); Error = FText::GetEmpty(); PumpUploads();
    if (bShuttingDown || !bConfigured || !TerrainSource.IsValid()) return EAftershockHydraulicUploadResult::NeedsConfigure;
    const auto* Terrain = TerrainSource.Get();
    if (GetAftershockDrawableCellBounds(*Terrain) != Stats.DrawableBounds
        || Terrain->GridOrigin != ConfiguredGridOrigin || Terrain->CellSize != ConfiguredCellSize)
    { Surface->SetVisibility(false); return EAftershockHydraulicUploadResult::NeedsConfigure; }
    if (Revision == 0 || Frame.ConnectivityRevision == 0 || Frame.ConnectivityRevision < Stats.LastConnectivityRevision
        || !FMath::IsFinite(Frame.PhysicalSeconds) || Frame.PhysicalSeconds < 0.
        || Frame.PhysicalSeconds < Stats.ConsumedPhysicalSeconds || Frame.Cells.Num() > UTerrainGridSubsystem::MaximumResidentCells
        || Frame.VisualWindDirection.ContainsNaN() || !FMath::IsFinite(Frame.VisualWindStrength01)
        || Frame.VisualWindStrength01 < 0.f || Frame.VisualWindStrength01 > 1.f)
    { Error = HydraulicSurfaceHydraulicError(TEXT("Su görünüm frame kimliği, saati veya girdisi geçersiz.")); return EAftershockHydraulicUploadResult::Rejected; }
    if (Revision <= Stats.LastQueuedRevision) return EAftershockHydraulicUploadResult::Unchanged;
    if (!CanRetirePending() || (bResourceFenceStarted && !ResourceFence.IsFenceComplete())) return EAftershockHydraulicUploadResult::Busy;
    if (!FieldTexture || !FieldTexture->GetResource() || FieldTexture->IsCurrentlyVirtualTextured() || FieldTexture->IsStreamable()
        || !ConnectivityTexture || !ConnectivityTexture->GetResource() || ConnectivityTexture->IsCurrentlyVirtualTextured() || ConnectivityTexture->IsStreamable())
    { Error = HydraulicSurfaceHydraulicError(TEXT("Su veri dokusu güncelleme kaynağı hazır veya non-streaming değil.")); return EAftershockHydraulicUploadResult::Rejected; }
    const double Now = FPlatformTime::Seconds();
    if (Now - LastSubmissionRealSeconds < HydraulicSurfaceMinimumUploadIntervalSeconds) return EAftershockHydraulicUploadResult::Throttled;
    const double Started = FPlatformTime::Seconds();
    auto Packet = MakeShared<FUploadPacket, ESPMode::ThreadSafe>();
    Packet->Pixels.Init(FVector4f(0.f, 0.f, 0.f, 0.f), HydraulicSurfaceFieldSize * HydraulicSurfaceFieldSize);
    Packet->Connectivity.Init(0x0f, HydraulicSurfaceFieldSize * HydraulicSurfaceFieldSize); Packet->ConnectivityRevision = Frame.ConnectivityRevision;
    Packet->Epoch = Stats.ConfigurationEpoch; Packet->Revision = Revision; Packet->PhysicalSeconds = Frame.PhysicalSeconds;
    Packet->Wind = Frame.VisualWindDirection.GetSafeNormal(); Packet->WindStrength = Frame.VisualWindStrength01;
    TBitArray<> Seen(false, Stats.DrawableBounds.Width() * Stats.DrawableBounds.Height());
    for (const auto& Cell : Frame.Cells)
    {
        if (!Stats.DrawableBounds.Contains(Cell.Coord)) continue; // full resident DTO, bounded pack.
        const FIntPoint Local = Cell.Coord - Stats.DrawableBounds.Min;
        const int32 Compact = Local.X + Local.Y * Stats.DrawableBounds.Width();
        if (Seen[Compact] || (Cell.BlockedFaces & 0xf0) != 0 || !Terrain->IsValidCoord(Cell.Coord) || !FMath::IsFinite(Cell.BedElevationCm) || !FMath::IsFinite(Cell.SurfaceElevationCm)
            || !FMath::IsFinite(Cell.DepthCm) || Cell.DepthCm < 0.f || Cell.VelocityCmPerSecond.ContainsNaN()
            || FMath::Abs(Cell.VelocityCmPerSecond.X) > 100000.f || FMath::Abs(Cell.VelocityCmPerSecond.Y) > 100000.f
            || FMath::Abs((static_cast<double>(Cell.SurfaceElevationCm) - Cell.BedElevationCm) - Cell.DepthCm) > .1)
        { Error = HydraulicSurfaceHydraulicError(TEXT("Yerel su DTO'su duplicate, sonlu olmayan veya head=bed+depth sözleşmesine aykırı.")); return EAftershockHydraulicUploadResult::Rejected; }
        if (Cell.SurfaceElevationCm < MinSupportedHeadCm || Cell.SurfaceElevationCm > MaxSupportedHeadCm)
        { Error = HydraulicSurfaceHydraulicError(TEXT("Su kotu kurulu WPO bounds rezervini aşıyor; owner daha geniş Configure yapmalı.")); return EAftershockHydraulicUploadResult::NeedsConfigure; }
        Seen[Compact] = true; Packet->ValidLocalCells++; Packet->WetCells += Cell.DepthCm > .2f ? 1 : 0;
        Packet->Connectivity[Local.X + Local.Y * HydraulicSurfaceFieldSize] = Cell.BlockedFaces;
        Packet->Pixels[Local.X + Local.Y * HydraulicSurfaceFieldSize] = FVector4f(
            static_cast<float>((static_cast<double>(Cell.SurfaceElevationCm) - ConfiguredGridOrigin.Z) * .01),
            Cell.DepthCm * .01f, Cell.VelocityCmPerSecond.X * .01f, Cell.VelocityCmPerSecond.Y * .01f);
    }
    if (Packet->ValidLocalCells != Stats.ExpectedLocalCells)
    { Error = HydraulicSurfaceHydraulicError(TEXT("Su frame'i kurulu zeminin tüm resident hücrelerini taşımıyor; eksik alan kuru gibi gösterilmedi.")); return EAftershockHydraulicUploadResult::Rejected; }
    for (int32 Y = 0; Y < Stats.DrawableBounds.Height(); ++Y) for (int32 X = 0; X < Stats.DrawableBounds.Width(); ++X)
    {
        const int32 At = X + Y * HydraulicSurfaceFieldSize; const uint8 Mask = Packet->Connectivity[At];
        if ((X + 1 < Stats.DrawableBounds.Width() && ((Mask & 2) != 0) != ((Packet->Connectivity[At + 1] & 8) != 0))
            || (Y + 1 < Stats.DrawableBounds.Height() && ((Mask & 4) != 0) != ((Packet->Connectivity[At + HydraulicSurfaceFieldSize] & 1) != 0)))
        { Error = HydraulicSurfaceHydraulicError(TEXT("Canonical su bağlantı maskesinin komşu yüzleri simetrik değil.")); return EAftershockHydraulicUploadResult::Rejected; }
    }
    Packet->bUploadConnectivity = LastConsumedConnectivity.Num() != Packet->Connectivity.Num()
        || FMemory::Memcmp(LastConsumedConnectivity.GetData(), Packet->Connectivity.GetData(), Packet->Connectivity.Num()) != 0;
    if (Packet->bUploadConnectivity && Frame.ConnectivityRevision <= Stats.LastConnectivityRevision)
    { Error = HydraulicSurfaceHydraulicError(TEXT("Bağlantı maskesi değişti fakat canonical connectivity revision ilerlemedi.")); return EAftershockHydraulicUploadResult::Rejected; }
    if (!Packet->bUploadConnectivity) Packet->Connectivity.Reset();
    PendingPacket = Packet; Stats.PendingPackets = 1;
    const TWeakObjectPtr<AAftershockHydraulicSurfaceActor> WeakThis(this);
    FTextureResource* FieldResource = FieldTexture->GetResource();
    FTextureResource* ConnectivityResource = ConnectivityTexture->GetResource();
    // Both payloads live in ONE render command, so a draw cannot consume the
    // new eta with an old wall mask between two independently queued updates.
    ENQUEUE_RENDER_COMMAND(AftershockUploadHydraulicFrame)(
        [FieldResource, ConnectivityResource, Packet, WeakThis](FRHICommandListImmediate& RHICmdList)
        {
            FRHITexture* FieldRHI = FieldResource ? FieldResource->GetTexture2DRHI() : nullptr;
            FRHITexture* MaskRHI = ConnectivityResource ? ConnectivityResource->GetTexture2DRHI() : nullptr;
            if (FieldRHI && MaskRHI)
            {
                RHICmdList.UpdateTexture2D(FieldRHI, 0, Packet->Region, HydraulicSurfaceFieldSize * sizeof(FVector4f), reinterpret_cast<const uint8*>(Packet->Pixels.GetData()));
                if (Packet->bUploadConnectivity) RHICmdList.UpdateTexture2D(MaskRHI, 0, Packet->Region, HydraulicSurfaceFieldSize, Packet->Connectivity.GetData());
                Packet->bRhiUploadSucceeded.Store(true);
            }
            // UE5.8 Texture2D.cpp also defers SrcData lifetime to an RHI lambda.
            RHICmdList.EnqueueLambda([Packet, WeakThis](FRHICommandList&)
            {
                Packet->bConsumed.Store(true); // POD only on render/RHI threads.
                AsyncTask(ENamedThreads::GameThread, [WeakThis]()
                { if (auto* Self = WeakThis.Get()) Self->PumpUploads(); });
            });
        });
    UploadFence.BeginFence(FRenderCommandFence::ESyncDepth::RHIThread); bUploadFenceStarted = true;
    Stats.LastQueuedRevision = Revision; Stats.SubmittedUploads++; LastSubmissionRealSeconds = Now;
    Stats.SubmittedMaskUploads += Packet->bUploadConnectivity ? 1 : 0;
    Stats.LastPackingCpuMs = (FPlatformTime::Seconds() - Started) * 1000.;
    return EAftershockHydraulicUploadResult::Queued;
}

void AAftershockHydraulicSurfaceActor::RefreshQuality()
{
    check(IsInGameThread()); if (!WaterMaterial || bShuttingDown) return;
    const auto Quality = GetAftershockEnvironmentQuality(GetWorld());
    const auto Camera = GetAftershockPresentationBudget(GetWorld());
    // Local planar water follows the same globe hand-off as local terrain.
    // Camera visibility never changes resident physics or the conserved field.
    bLocalPresentationVisible = Camera.Tier != EAftershockPresentationTier::Globe;
    if (Surface) Surface->SetVisibility(bConfigured && bLocalPresentationVisible && Stats.LastWetCells > 0);
    const uint32 Signature = HashCombine(Quality.GetSignature(), GetTypeHash(static_cast<uint8>(Camera.Tier)));
    if (Signature == QualitySignature) return;
    QualitySignature = Signature; const auto Budget = MakeAftershockEnvironmentBudget(Quality, Camera);
    WaterMaterial->SetScalarParameterValue(TEXT("GlobalWaterNormalQuality"), Budget.WaterNormalStrength);
    WaterMaterial->SetScalarParameterValue(TEXT("GlobalWaterFoamQuality"), Budget.WaterFoamStrength);
    // SSR capability is compiled in the material; actual project method/CVar
    // remains the Settings owner's responsibility and must be read back in RHI.
    WaterMaterial->SetScalarParameterValue(TEXT("HydraulicWaterRoughness"), Budget.bRequestWaterSSR ? .18f : .27f);
}

void AAftershockHydraulicSurfaceActor::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
    auto* Self = CastChecked<AAftershockHydraulicSurfaceActor>(InThis);
    Collector.AddReferencedObject(Self->FieldTexture); Collector.AddReferencedObject(Self->SurfaceMesh);
    Collector.AddReferencedObject(Self->ConnectivityTexture);
    Collector.AddReferencedObject(Self->WaterMaterial);
    Collector.AddReferencedObject(Self->SurfaceMaterialTemplate);
    Super::AddReferencedObjects(InThis, Collector);
}

void AAftershockHydraulicSurfaceActor::EndPlay(const EEndPlayReason::Type Reason)
{
    bShuttingDown = true; if (Surface) Surface->SetVisibility(false);
    // Keep UObject references and packet storage until destruction fences retire.
    Super::EndPlay(Reason);
}
void AAftershockHydraulicSurfaceActor::BeginDestroy()
{
    bShuttingDown = true;
    DestroyFence.BeginFence(FRenderCommandFence::ESyncDepth::RHIThread); bDestroyFenceStarted = true;
    Super::BeginDestroy();
}
bool AAftershockHydraulicSurfaceActor::IsReadyForFinishDestroy()
{
    return (!bDestroyFenceStarted || DestroyFence.IsFenceComplete())
        && (!bResourceFenceStarted || ResourceFence.IsFenceComplete())
        && (!bUploadFenceStarted || UploadFence.IsFenceComplete()) && Super::IsReadyForFinishDestroy();
}
void AAftershockHydraulicSurfaceActor::FinishDestroy()
{
    PendingPacket.Reset(); LastConsumedConnectivity.Empty(); FieldTexture = nullptr; ConnectivityTexture = nullptr; SurfaceMesh = nullptr; WaterMaterial = nullptr;
    Super::FinishDestroy();
}
