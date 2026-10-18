#pragma once

#include "CoreMinimal.h"

/** Original bounded game approximation, not a geotechnical or avalanche forecast.
 * Every distance is METRES, time SECONDS, mass KG, density KG/M3, pressure PA.
 * Pure DTO/math: no UObject, Actor, world, damage, resident or economy authority.
 * Root must finish V4 save migration before adding reflected hazard/state fields. */
enum class EAftershockMassMaterial : uint8 { Snow, SoilDebris };

struct FAftershockMassCell
{
    FIntPoint Coord=FIntPoint::ZeroValue;
    /** Effective mineral/soil surface BEFORE this event; previous canonical
     * soil erosion/deposition already included. Current snow is separate below. */
    double GroundElevationM=0.;
    bool bLand=true;
    double SnowWaterEquivalentMm=0.,SnowDensityKgM3=300.,SnowWeakLayerStrengthPa=1000.;
    double RegolithVerticalDepthM=0.,SoilDensityKgM3=1800.;
    double CohesionPa=3000.,FrictionAngleDegrees=32.,EffectiveSaturation01=0.;
};

struct FAftershockMassFriction
{
    /** Dimensionless basal friction; Xi is m/s2. Authored calibration. */
    double Mu=.24,XiMps2=1000.;
};

struct FAftershockMassObstacle
{
    int32 BuildingID=INDEX_NONE;
    FVector2D CenterM=FVector2D::ZeroVector,HalfExtentM=FVector2D(3.,3.);
    double YawRadians=0.,HeightM=3.;
    int32 StructuralMaterialTag=0,FoundationTag=0;
};

struct FAftershockMassObstacleResponse
{
    bool bBlocksFlow=true;
    double NormalRestitution=0.,TangentialRetention=.55,ImpactCoefficient=1.;
};

struct FAftershockMassRelease
{
    FIntPoint Coord=FIntPoint::ZeroValue;
    double AvailableMassFraction=.5;
    /** Explicit scenario/God-Mode release only; does not create material,
     * bypass numerical limits, ocean gates or the caller's session permission. */
    bool bForceRelease=false;
    FVector InitialVelocityMps=FVector::ZeroVector;
};

struct FAftershockMassSettings
{
    static constexpr double GravityMps2=9.81;
    int32 MaxCells=4096,MaxParcels=512,MaxObstacles=256,MaxReleaseCells=128;
    int32 ParcelsPerReleaseCell=4,MaxSubsteps=4096,MaxParticleSteps=1500000,MaxFrames=32;
    int64 MaxWorkUnits=64000000;
    double MaximumSeconds=60.,MaxStepSeconds=.05,CFL=.23;
    double MaxSupportedSpeedMps=100.,MaxParcelDepthM=20.;
    double StopSpeedMps=.12,StopHoldSeconds=.35;
    double MinimumSnowDepthM=.15,MinimumSnowSlopeDegrees=30.,MaximumSnowSlopeDegrees=45.;
    double MinimumSoilDepthM=.05,MinimumSoilSlopeDegrees=5.;
    double EntrainFractionPerSecond=0.;
    bool bExportAtDomainBoundary=true;
    uint32 Seed=1337;
    FAftershockMassFriction SnowFriction{.24,1000.},SoilFriction{.45,300.};
};

struct FAftershockMassInput
{
    EAftershockMassMaterial Material=EAftershockMassMaterial::Snow;
    /** Native int only: proposed integration Avalanche7 / Landslide8.
     * This helper deliberately does not modify EDisasterType. */
    int32 HazardTypeTag=7;
    uint64 TerrainRevision=0;
    double CellSizeM=10.;
    FVector2D GridOriginM=FVector2D::ZeroVector;
    TArray<FAftershockMassCell> Cells;
    TArray<FAftershockMassRelease> Release;
    TArray<FAftershockMassObstacle> Obstacles;
    FAftershockMassSettings Settings;
    /** Optional immutable DTO callbacks, evaluated ONCE per staged cell/obstacle.
     * Do not capture mutable worlds/UObjects or mutate authoritative state. */
    TFunction<bool(const FAftershockMassCell&,EAftershockMassMaterial,FAftershockMassFriction&)> MaterialFriction;
    TFunction<bool(const FAftershockMassObstacle&,FAftershockMassObstacleResponse&)> FoundationResponse;
};

struct FAftershockMassEligibility
{
    bool bEligible=false;
    double SlopeDegrees=0.,AvailableMassKg=0.,AvailableVolumeM3=0.,FactorOfSafety=0.;
    FString Reason;
};

struct FAftershockMassCellResult
{
    FIntPoint Coord=FIntPoint::ZeroValue;
    /** Exact staged support used by this solve, after initial release but before
     * later entrainment/deposition. No renderer re-derives another path bed. */
    double RunoutBedElevationM=0.,RemainingSourceMaterialKg=0.;
    double ReleasedMassKg=0.,EntrainedMassKg=0.,RemovedVolumeM3=0.;
    double DepositedMassKg=0.,DepositedVolumeM3=0.,FinalMovingMassKg=0.;
    double FirstArrivalSeconds=-1.,PeakFlowDepthM=0.,PeakSpeedMps=0.,PeakDynamicPressurePa=0.;
    FVector PeakVelocityMps=FVector::ZeroVector;
};

struct FAftershockMassImpact
{
    int32 BuildingID=INDEX_NONE;
    double PeakNormalPressurePa=0.,ImpulseMagnitudeNs=0.,PeakFlowDepthM=0.;
    /** Substep contact time, not event start or delayed presentation-frame time. */
    double FirstContactSeconds=-1.,PeakPressureSeconds=-1.;
    int32 Contacts=0;
};

struct FAftershockMassParcelFrame
{
    int32 ParcelID=INDEX_NONE;
    FVector PositionM=FVector::ZeroVector,VelocityMps=FVector::ZeroVector;
    double MassKg=0.,VolumeM3=0.,FootprintAreaM2=0.;
};
/** Sparse same-state sample; no severity proxy or independent deposit envelope. */
struct FAftershockMassFrameCell
{
    FIntPoint Coord=FIntPoint::ZeroValue;
    double MovingMassKg=0.,MovingVolumeM3=0.,FlowDepthM=0.;
    double DepositedMassKg=0.,DepositedVolumeM3=0.;
};
struct FAftershockMassFrame
{
    double Seconds=0.;
    TArray<FAftershockMassParcelFrame> Parcels;
    TArray<FAftershockMassFrameCell> Cells;
    /** Cumulative contacts as of this frame ONLY; never the future final peak. */
    TArray<FAftershockMassImpact> Impacts;
};

struct FAftershockMassBudget
{
    double ReleasedKg=0.,EntrainedKg=0.,MovingKg=0.,DepositedKg=0.,ExportedKg=0.,MassResidualKg=0.;
    /** Kinetic + resolved base-height work closure. Does not claim thermal,
     * pore-fluid or granular dilatancy energy of a full continuum solver. */
    double InitialKineticJ=0.,GravityWorkJ=0.,DissipatedJ=0.,MovingKineticJ=0.,ExportedKineticJ=0.,EnergyResidualJ=0.;
    double MaximumSpeedMps=0.,MaximumDisplacementCells=0.;
    int32 Substeps=0,InitialParcels=0,PeakActiveParcels=0,MaterialCallbackCalls=0,FoundationCallbackCalls=0;
    int64 ParticleSteps=0,SurfaceSamples=0,ObstacleTests=0,WorkUnits=0;
    double ComputeMilliseconds=0.;
};

struct FAftershockMassResult
{
    static constexpr int32 SolverVersion=1;
    bool bAccepted=false,bComplete=false,bMassConserved=false,bEnergyClosed=false;
    /** Export budgets never change numerical stepping or acceptance. A caller
     * requiring replay must additionally require bFrameExportComplete. */
    static constexpr int32 MaximumFrameCellRecords=131072,MaximumFrameParcelRecords=32768,MaximumFrameImpactRecords=16384;
    bool bFrameExportComplete=true;
    int32 ExportedFrameCellRecords=0,ExportedFrameParcelRecords=0,ExportedFrameImpactRecords=0;
    FString FrameExportFailure;
    bool bFrozenBedDuringEvent=true,bLateralGranularPressureModeled=false,bOvertoppingModeled=false;
    EAftershockMassMaterial Material=EAftershockMassMaterial::Snow;
    int32 HazardTypeTag=7;
    uint64 SourceTerrainRevision=0;
    double SimulatedSeconds=0.;
    FString Failure;
    FAftershockMassBudget Budget;
    TArray<FAftershockMassCellResult> Cells;
    TArray<FAftershockMassImpact> Impacts;
    TArray<FAftershockMassFrame> Frames;
};

class FAftershockMassMovementSolver final
{
public:
    static FAftershockMassEligibility EvaluateRelease(const FAftershockMassInput& Input,const FAftershockMassRelease& Release);
    static bool EvaluateReleaseCandidates(const FAftershockMassInput& Input,const TArray<FAftershockMassRelease>& Requests,
        TArray<FAftershockMassEligibility>& Out,FString& Failure);
    static FAftershockMassResult Solve(const FAftershockMassInput& Input);
    static double ShallowSoilFactorOfSafety(double SlopeRadians,double VerticalDepthM,double DensityKgM3,
        double CohesionPa,double FrictionAngleRadians,double Saturation01);
    /** Frozen hydrostatic effective-stress approximation, not a pore-pressure solver. */
    static double EffectiveSoilCoulombCoefficient(double DryMu,double Saturation01,double BulkDensityKgM3);
};
