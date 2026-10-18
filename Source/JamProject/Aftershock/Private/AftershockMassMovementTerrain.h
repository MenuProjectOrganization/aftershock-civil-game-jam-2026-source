#pragma once
#include "CoreMinimal.h"
#include "AftershockMassMovement.h"

class UTerrainGridSubsystem;
class UBuildingManagerSubsystem;
struct FGridCell;
class FJsonObject;

/** Authored antecedent weather, NOT observed daily NASA weather. All precip is
 * liquid-water equivalent mm/day. Temperature is at ReferenceElevationM. */
struct FAftershockMaterialWeatherSegment
{
    int32 Days=1;
    double AirTemperatureC=0.,PrecipitationMmPerDay=0.;
    /** -1 partitions by local temperature; nonnegative is explicitly supplied
     * snowfall water equivalent, still converted to liquid above AllRainC. */
    double SuppliedSnowWaterMmPerDay=-1.;
};
struct FAftershockMaterialWeatherHistory
{
    int32 ScenarioVersion=1;
    FName SourceCode=NAME_None;
    FString ContextSourceURL,Assumptions;
    double ReferenceElevationM=0.,LapseRateCPerKm=6.5;
    double AllSnowC=0.,AllRainC=2.,MeltBaseC=0.,MeltMmPerDegreeDay=2.5;
    double SnowBulkDensityKgM3=300.,SnowWeakLayerStrengthPa=1000.;
    TArray<FAftershockMaterialWeatherSegment> Segments;
};
/** Optional climate context is supplied from the existing verified cache by the
 * owner. It is a monthly mean, never a daily storm or snow inventory. */
struct FAftershockMaterialClimateContext
{
    bool bVerifiedContext=false;
    double ColdestMonthlyMeanTemperatureC=0.,ReferenceElevationM=0.;
    FString PrimarySourceURL;
};
struct FAftershockAuthoredWinterOptions
{
    bool bExplicitlyRequestAuthoredWinter=false;
    double ColdSpellAnomalyC=-4.,DailyPrecipitationMm=12.;
    int32 Days=12;
    double SnowWeakLayerStrengthPa=80.;
};
/** Explicit local authored mantle, not SoilType/Biome=>a universal1m deposit. */
struct FAftershockAuthoredRegolithPatch
{
    FName SourceCode=NAME_None;
    TArray<FIntPoint> Cells;
    double DepthM=0.,Porosity01=.35,DryBulkDensityKgM3=1500.;
    double CohesionPa=3000.,FrictionAngleDegrees=32.,InitialPoreWaterDepthM=0.;
    bool bExplicitWeatheredMantleOverScenarioBedrock=false;
};
struct FAftershockMaterialInitializationReceipt
{
    bool bApplied=false,bWeatherAuthoredNotObserved=true;
    uint64 BeforeTerrainRevision=0,AfterTerrainRevision=0;
    int32 CellsVisited=0,SnowCells=0,RegolithCells=0;
    double SnowWaterKg=0.,DrySoilKg=0.,PoreWaterM3=0.,AntecedentLiquidOutflowM3=0.;
    FAftershockMaterialWeatherHistory WeatherHistory;
    TArray<FAftershockAuthoredRegolithPatch> RegolithPatches;
    FString Failure;
};
/** Complete saved continuation contract. An authored material profile is not
 * observed weather. Mantle.Cells must be empty here; the canonical owner selects
 * new resident coordinates using these saved bounded eligibility rules. */
struct FAftershockMaterialContinuationProfile
{
    int32 ProfileVersion=1;
    FAftershockMaterialWeatherHistory Weather;
    FAftershockAuthoredRegolithPatch Mantle;
    double MinimumForestFuel01=.1,MinimumSlopeDegrees=8.,MaximumSlopeDegrees=60.;
};
struct FAftershockMassTerrainOptions
{
    EAftershockMassMaterial Material=EAftershockMassMaterial::Snow;
    uint32 Seed=1337;
    int32 MinimumReleaseCells=16,MaximumReleaseCells=64;
    /** Numerical preparation margin, NOT a predicted runout length. Sources
     * closer to missing/ocean support require domain expansion before admission.
     * Default 35 cells is 350 m at the current 10 m grid. */
    int32 MinimumResidentSupportCells=35;
    double ReleaseFraction=.35;
    bool bForceRelease=false,bCallerAuthorizedGodMode=false;
    bool bHasPreferredRelease=false;
    FIntPoint PreferredRelease=FIntPoint::ZeroValue;
};
/** Per-source wet mixture composition, kept separate from ecological moisture. */
struct FAftershockMassSourceInventory
{
    FIntPoint Coord=FIntPoint::ZeroValue;
    double InitialMixtureVolumeM3=0.,InitialMixtureMassKg=0.;
    double InitialDryMassKg=0.,InitialWaterM3=0.;
};
struct FAftershockMassTerrainSnapshot
{
    bool bReady=false;
    FAftershockMassInput Input;
    /** Dimensionless instability display [0,1], NOT a forecast probability. */
    TMap<FIntPoint,double> Instability01;
    TArray<FAftershockMassSourceInventory> SourceInventories;
    FString BuildingFingerprint,Failure;
    int32 NaturalEligibleCells=0,BlockedSourceCells=0,IncludedSolidBuildings=0;
    int32 BoundaryExcludedSourceCells=0,MinimumSelectedSupportCells=0;
};
struct FAftershockMassSourceWaterWithdrawal
{
    FIntPoint Coord=FIntPoint::ZeroValue;
    double ReleasedWaterM3=0.,EntrainedWaterM3=0.,RemainingSourceWaterM3=0.;
};
struct FAftershockMassWaterTransferMetadata
{
    bool bValid=false,bEventWideWellMixedApproximation=true;
    double ReleasedWaterM3=0.,EntrainedWaterM3=0.,WaterMassFraction=0.;
    TArray<FAftershockMassSourceWaterWithdrawal> Sources;
    FString Failure;
};

/** No subsystem, Tick, hazard trigger, casualty or budget authority. The ONLY
 * mutation is an explicit prevalidated material-stock transaction.
 * Continuation only accepts zero-stock Version 0 resident cells and a complete
 * previously saved authored profile; existing Version 1 state is never reset. */
namespace AftershockMassTerrain
{
    bool MakeAuthoredWinterHistory(const FAftershockMaterialClimateContext& Context,
        const FAftershockAuthoredWinterOptions& Options,FAftershockMaterialWeatherHistory& Out,FString& Failure);
    bool ComputeSnowInventory(const FAftershockMaterialWeatherHistory& History,double GroundElevationM,
        double& OutSnowWaterMm,double& OutLiquidOutflowMm,FString& Failure);
    /** Owner must call only for a newly created game, never a V4 load/restore.
     * Every existing stock/version must still be0. Receipt must be saved by the
     * canonical scenario/save owner; a renderer cannot choose another history. */
    FAftershockMaterialInitializationReceipt InitializeAuthoredMaterialScenario(UTerrainGridSubsystem& Terrain,
        const FAftershockMaterialWeatherHistory& History,const TArray<FAftershockAuthoredRegolithPatch>& Patches,bool bNewGameTransaction);
    /** Full finite material preflight includes preserved cells. Only unique
     * CellsToInitialize with Version 0 and zero inventories may change. The
     * receipt callback must stage/validate provenance without mutating terrain;
     * false rejects before the first SetCell. Existing cells are never written. */
    FAftershockMaterialInitializationReceipt InitializeAuthoredMaterialSubset(UTerrainGridSubsystem& Terrain,
        const FAftershockMaterialWeatherHistory& History,const TArray<FAftershockAuthoredRegolithPatch>& Patches,
        const TArray<FIntPoint>& CellsToInitialize,
        TFunctionRef<bool(const FAftershockMaterialInitializationReceipt&,FString&)> StageReceipt);
    bool EncodeMaterialContinuationProfile(const FAftershockMaterialContinuationProfile& Profile,
        TSharedPtr<FJsonObject>& Out,FString& Failure);
    bool DecodeMaterialContinuationProfile(const TSharedPtr<FJsonObject>& JSON,
        FAftershockMaterialContinuationProfile& Out,FString& Failure);
    bool BuildAuthoredMantlePatches(const UTerrainGridSubsystem& Terrain,
        const FAftershockMaterialContinuationProfile& Profile,const TArray<FIntPoint>& CellsToInitialize,
        TArray<FAftershockAuthoredRegolithPatch>& Out,FString& Failure);
    bool EffectiveGroundElevationM(const UTerrainGridSubsystem& Terrain,const FGridCell& Cell,double& Out,FString& Failure);
    /** Full resident canonical domain, not the camera's drawable window. Missing
     * outside support rejects flow; no chunk generation or invisible mass export. */
    FAftershockMassTerrainSnapshot BuildSnapshot(const UTerrainGridSubsystem& Terrain,
        const UBuildingManagerSubsystem& Buildings,const FAftershockMassTerrainOptions& Options);
    FAftershockMassWaterTransferMetadata StageSourceWaterWithdrawals(const FAftershockMassTerrainSnapshot& Snapshot,
        const FAftershockMassResult& Result);
}
