#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/AftershockStartupPreparationSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Aftershock/Public/AftershockRegionEnvironmentProfile.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Narrative/CasualtyLedgerGenerator.h"
#include "AftershockHydraulicTerrain.h"
#include "AftershockHydrologyAsyncStepper.h"
#include "AftershockHydraulicSurfaceActor.h"
#include "AftershockMassMovementTerrain.h"
#include "AftershockMassMovementVisual.h"
#include "AftershockPhysicalReplay.h"
#include "AftershockPhysicalLifecycle.h"
#include "AftershockMassSupportPreparation.h"
#include "AftershockTerrainSurface.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/MemoryWriter.h"
#include "Misc/SecureHash.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "TimerManager.h"

namespace AftershockPhysicalDetail
{
constexpr float Heartbeat=.08f;
const FIntPoint Steps[4]={FIntPoint(0,-1),FIntPoint(1,0),FIntPoint(0,1),FIntPoint(-1,0)};
bool IsWater(EDisasterType T){return T==EDisasterType::Flood||T==EDisasterType::Tsunami;}
bool IsMass(EDisasterType T){return T==EDisasterType::Avalanche||T==EDisasterType::Landslide;}
FText Failure(const FString& S){return FText::FromString(S);}
double Slope(const UTerrainGridSubsystem& T,const FGridCell& C)
{
    double X=0.,Y=0.;FGridCell A,B;
    if(T.TryGetCell(C.Coords+FIntPoint(1,0),A)&&T.TryGetCell(C.Coords-FIntPoint(1,0),B))X=(A.Elevation-B.Elevation)/(2.*T.CellSize);
    if(T.TryGetCell(C.Coords+FIntPoint(0,1),A)&&T.TryGetCell(C.Coords-FIntPoint(0,1),B))Y=(A.Elevation-B.Elevation)/(2.*T.CellSize);
    return FMath::RadiansToDegrees(FMath::Atan(FMath::Sqrt(X*X+Y*Y)));
}
FString BuildingSignature(const UBuildingManagerSubsystem& B)
{
    FAftershockBuildingsSaveState S;B.CaptureSaveState(S);
    S.Buildings.Sort([](const auto& A,const auto& C){return A.State.InstanceID<C.State.InstanceID;});
    TArray<uint8> Bytes;FMemoryWriter W(Bytes);
    for(auto& R:S.Buildings){W<<R.State.InstanceID<<R.State.GridCoords<<R.WorldTransform<<R.State.CurrentDamage;}
    uint8 Hash[FSHA1::DigestSize];FSHA1::HashBuffer(Bytes.GetData(),Bytes.Num(),Hash);return BytesToHex(Hash,FSHA1::DigestSize);
}
void ToSaved(const AftershockWater::Budget& B,FAftershockHydraulicBudgetSaveRecord& S)
{
    S.InitialSurfaceM3=B.InitialSurfaceM3;S.InitialStorageM3=B.InitialStorageM3;S.RainM3=B.RainM3;S.ExternalInflowM3=B.ExternalInflowM3;
    S.BoundaryInflowM3=B.BoundaryInflowM3;S.BoundaryOutflowM3=B.BoundaryOutflowM3;S.InfiltrationM3=B.InfiltrationM3;
    S.ExternalDrainageM3=B.ExternalDrainageM3;S.EvaporationM3=B.EvaporationM3;S.StorageExternalOutflowM3=B.StorageExternalOutflowM3;
    S.FinalSurfaceM3=B.FinalSurfaceM3;S.FinalStorageM3=B.FinalStorageM3;S.NumericalVolumeCorrectionM3=B.NumericalVolumeCorrectionM3;
    S.ResidualM3=B.ResidualM3;S.AcceptedSteps=B.AcceptedSteps;S.FaceEvaluations=B.FaceEvaluations;
}
AftershockWater::Budget FromSaved(const FAftershockHydraulicBudgetSaveRecord& S)
{
    AftershockWater::Budget B;B.InitialSurfaceM3=S.InitialSurfaceM3;B.InitialStorageM3=S.InitialStorageM3;B.RainM3=S.RainM3;B.ExternalInflowM3=S.ExternalInflowM3;
    B.BoundaryInflowM3=S.BoundaryInflowM3;B.BoundaryOutflowM3=S.BoundaryOutflowM3;B.InfiltrationM3=S.InfiltrationM3;
    B.ExternalDrainageM3=S.ExternalDrainageM3;B.EvaporationM3=S.EvaporationM3;B.StorageExternalOutflowM3=S.StorageExternalOutflowM3;
    B.FinalSurfaceM3=S.FinalSurfaceM3;B.FinalStorageM3=S.FinalStorageM3;B.NumericalVolumeCorrectionM3=S.NumericalVolumeCorrectionM3;
    B.ResidualM3=S.ResidualM3;B.AcceptedSteps=S.AcceptedSteps;B.FaceEvaluations=S.FaceEvaluations;return B;
}
// This v1 owner accepts exactly ONE immutable water quantum at a time. Rebind
// its pending controls by canonical keys, never by a new episode time/terrain.
bool BindSavedAcceptedForcing(const FAftershockPhysicalHazardSaveState& Saved,
    const AftershockWater::Domain& Domain,AftershockWater::Forcing& Out,FText& Error)
{
    if(!FMath::IsFinite(Saved.PendingAcceptedSeconds)||Saved.PendingAcceptedSeconds<=1.e-12
        ||!FMath::IsFinite(Saved.PendingPhysicalSeconds)||Saved.PendingAcceptedSeconds>Saved.PendingPhysicalSeconds
        ||!Domain.Storages.empty()||!Domain.Exchanges.empty()
        ||Domain.Cells.empty()||Domain.Cells.size()>AftershockWater::MaximumCells
        ||Saved.PendingForcingCells.Num()!=static_cast<int32>(Domain.Cells.size())
        ||Saved.PendingForcingBoundaries.Num()>static_cast<int32>(Domain.Cells.size()*4))
    {Error=Failure(TEXT("Accepted hydraulic forcing has invalid time, cardinality or unsupported storage nodes."));return false;}
    TMap<FIntPoint,int32> Cells;Cells.Reserve(Domain.Cells.size());
    for(int32 I=0;I<static_cast<int32>(Domain.Cells.size());++I)
    {
        const FIntPoint Coord(Domain.Cells[I].X,Domain.Cells[I].Y);
        if(Cells.Contains(Coord)){Error=Failure(TEXT("Hydraulic domain has duplicate cell keys."));return false;}Cells.Add(Coord,I);
    }
    AftershockWater::Forcing Candidate;Candidate.Cells.resize(Domain.Cells.size());Candidate.Boundaries.resize(Domain.Faces.size());
    TSet<FIntPoint> SeenCells;SeenCells.Reserve(Saved.PendingForcingCells.Num());
    for(const auto& C:Saved.PendingForcingCells)
    {
        const int32* Index=Cells.Find(C.Coord);
        if(!Index||SeenCells.Contains(C.Coord)||!FMath::IsFinite(C.InflowVelocityXMps)||!FMath::IsFinite(C.InflowVelocityYMps))
        {Error=Failure(TEXT("Accepted hydraulic forcing cell key or velocity is invalid."));return false;}
        for(double V:{C.RainMPerSecond,C.InfiltrationCapacityMPerSecond,C.ExternalDrainCapacityM3PerSecond,C.EvaporationMPerSecond,C.ExternalInflowM3PerSecond})
            if(!FMath::IsFinite(V)||V<0.){Error=Failure(TEXT("Accepted hydraulic forcing has non-finite/negative rates."));return false;}
        SeenCells.Add(C.Coord);auto& D=Candidate.Cells[*Index];D.RainMPerS=C.RainMPerSecond;
        D.InfiltrationCapacityMPerS=C.InfiltrationCapacityMPerSecond;D.ExternalDrainCapacityM3PerS=C.ExternalDrainCapacityM3PerSecond;
        D.EvaporationMPerS=C.EvaporationMPerSecond;D.ExternalInflowM3PerS=C.ExternalInflowM3PerSecond;
        D.InflowVelocityXMps=C.InflowVelocityXMps;D.InflowVelocityYMps=C.InflowVelocityYMps;
    }
    TMap<int64,int32> Stages;
    for(int32 I=0;I<static_cast<int32>(Domain.Faces.size());++I)if(Domain.Faces[I].Kind==AftershockWater::FaceKind::StageBoundary)
    {
        const int64 ID=Domain.Faces[I].StableID;
        if(Stages.Contains(ID)){Error=Failure(TEXT("Hydraulic domain has duplicate stage face IDs."));return false;}Stages.Add(ID,I);
    }
    if(Saved.PendingForcingBoundaries.Num()!=Stages.Num())
    {Error=Failure(TEXT("Saved accepted controls do not exactly cover the immutable stage-boundary set."));return false;}
    TSet<int64> SeenStages;SeenStages.Reserve(Stages.Num());
    for(const auto& B:Saved.PendingForcingBoundaries)
    {
        const int32* Index=Stages.Find(B.FaceStableID);
        if(!Index||SeenStages.Contains(B.FaceStableID)||!FMath::IsFinite(B.HeadM)||!FMath::IsFinite(B.VelocityXMps)||!FMath::IsFinite(B.VelocityYMps))
        {Error=Failure(TEXT("Accepted stage controls contain an extra, non-stage, duplicate or non-finite record."));return false;}
        SeenStages.Add(B.FaceStableID);auto& D=Candidate.Boundaries[*Index];D.HeadM=B.HeadM;D.VelocityXMps=B.VelocityXMps;D.VelocityYMps=B.VelocityYMps;
    }
    Out=MoveTemp(Candidate);return true; // Non-stage boundary records remain zero.
}

bool CaptureAcceptedForcing(const AftershockWater::Domain& Domain,const AftershockWater::Forcing& Sources,
    double Seconds,FAftershockPhysicalHazardSaveState& Out,FText& Error)
{
    if(!FMath::IsFinite(Seconds)||Seconds<=1.e-12||Seconds>Out.PendingPhysicalSeconds
        ||Domain.Cells.size()!=static_cast<size_t>(Out.WaterCells.Num())||Domain.Cells.size()>AftershockWater::MaximumCells
        ||(!Sources.Cells.empty()&&Sources.Cells.size()!=Domain.Cells.size())
        ||(!Sources.Boundaries.empty()&&Sources.Boundaries.size()!=Domain.Faces.size())
        ||!Sources.StorageExternalOutflowM3PerS.empty()||!Domain.Storages.empty()||!Domain.Exchanges.empty())
    {Error=Failure(TEXT("Cannot capture an unsupported or unschedulable accepted hydraulic quantum."));return false;}
    Out.PendingAcceptedSeconds=Seconds;Out.PendingForcingCells.Reserve(Domain.Cells.size());
    TSet<FIntPoint> WaterCoords;WaterCoords.Reserve(Out.WaterCells.Num());for(const auto& C:Out.WaterCells)WaterCoords.Add(C.Coord);
    for(int32 I=0;I<static_cast<int32>(Domain.Cells.size());++I)
    {
        const auto& G=Domain.Cells[I];const FIntPoint Coord(G.X,G.Y);
        if(!WaterCoords.Contains(Coord)){Error=Failure(TEXT("Accepted hydraulic quantum does not match saved water coordinates."));return false;}
        const AftershockWater::CellForcing C=Sources.Cells.empty()?AftershockWater::CellForcing{}:Sources.Cells[I];
        auto& D=Out.PendingForcingCells.AddDefaulted_GetRef();D.Coord=Coord;D.RainMPerSecond=C.RainMPerS;
        D.InfiltrationCapacityMPerSecond=C.InfiltrationCapacityMPerS;D.ExternalDrainCapacityM3PerSecond=C.ExternalDrainCapacityM3PerS;
        D.EvaporationMPerSecond=C.EvaporationMPerS;D.ExternalInflowM3PerSecond=C.ExternalInflowM3PerS;
        D.InflowVelocityXMps=C.InflowVelocityXMps;D.InflowVelocityYMps=C.InflowVelocityYMps;
    }
    for(int32 I=0;I<static_cast<int32>(Domain.Faces.size());++I)if(Domain.Faces[I].Kind==AftershockWater::FaceKind::StageBoundary)
    {
        const auto& G=Domain.Faces[I];const AftershockWater::BoundaryForcing C=Sources.Boundaries.empty()
            ?AftershockWater::BoundaryForcing{G.StageHeadM,G.StageVelocityXMps,G.StageVelocityYMps}:Sources.Boundaries[I];
        auto& D=Out.PendingForcingBoundaries.AddDefaulted_GetRef();D.FaceStableID=G.StableID;
        D.HeadM=C.HeadM;D.VelocityXMps=C.VelocityXMps;D.VelocityYMps=C.VelocityYMps;
    }
    return true;
}

}

/** GT owns this bridge. Worker closures contain numerical DTOs only. */
struct FAftershockPhysicalHazardRuntime
{
    FAftershockPhysicalHazardRuntime():Water([]
    {
        AftershockWater::StepperLimits Limits;Limits.MaximumWorkerSteps=32;
        return Limits;
    }()){}
    AftershockWater::AsyncStepper Water;
    TWeakObjectPtr<AAftershockHydraulicSurfaceActor> WaterActor;
    TWeakObjectPtr<AAftershockMassMovementVisualActor> MassActor;
    TFuture<FAftershockMassResult> MassJob;
    TUniquePtr<FAftershockTerrainChunkExpansion> MassExpansion;
    TSharedPtr<FAftershockMassTerrainSnapshot> MassSource;
    TSharedPtr<FAftershockMassVisualEvent> MassReplay;
    FString MassBuildingSignature,LastFailure;
    TMap<FIntPoint,int32> WaterIndex;
    TMap<FIntPoint,double> AppliedDepositM3;
    TMap<FIntPoint,double> AppliedInfiltrationM3;
    TArray<FAftershockHydraulicVisualCell> VisualCells;
    std::shared_ptr<const AftershockWater::ForcingPacket> Forcing;
    // Decoded once against the exact restored Domain. Retained until its first
    // published initialization allows submission, before any new controls.
    AftershockWater::Forcing RestoredAcceptedSources;
    double RestoredAcceptedSeconds=0.;
    uint64 TerrainRevision=MAX_uint64,ForcingRevision=0,LastPublication=0,WeightTerrainRevision=MAX_uint64,CoastDirectionRevision=MAX_uint64;
    uint64 MaterialCheckedRevision=MAX_uint64;
    FVector CoastInlandDirection=FVector::ZeroVector;
    double CallerDebt=0.,LastWorldSeconds=0.,LastExposureSeconds=0.,NextRiskRefreshWorldSeconds=0.;
    float SnowWeight=0.f,SoilWeight=0.f;
    bool bInitializing=false,bInitialized=false,bRestored=false,bMassWaiting=false,bMassAwaitingBoundary=false,bRiskDirty=true,bStopped=false;
    bool bMassSupportExpanded=false,bMassSupportPrepared=false;
};

void UDisasterSimulationSubsystem::OnWorldBeginPlay(UWorld& World)
{
    Super::OnWorldBeginPlay(World);
    if(World.WorldType!=EWorldType::Game&&World.WorldType!=EWorldType::PIE)return;
    PhysicalRuntime=MakeShared<FAftershockPhysicalHazardRuntime>();
    PhysicalRuntime->LastWorldSeconds=World.GetTimeSeconds();
    World.GetTimerManager().SetTimer(PhysicalTimer,this,&UDisasterSimulationSubsystem::AdvancePhysicalHazards,AftershockPhysicalDetail::Heartbeat,true);
}

void UDisasterSimulationSubsystem::ShutdownPhysicalRuntime()
{
    if(GetWorld())GetWorld()->GetTimerManager().ClearTimer(PhysicalTimer);
    if(!PhysicalRuntime)return;
    PhysicalRuntime->Water.Deinitialize();
    // Teardown only: one bounded worker, no UObject callbacks or detached jobs.
    if(PhysicalRuntime->MassJob.IsValid())PhysicalRuntime->MassJob.Wait();
    if(PhysicalRuntime->WaterActor.IsValid())PhysicalRuntime->WaterActor->Destroy();
    if(PhysicalRuntime->MassActor.IsValid())PhysicalRuntime->MassActor->Destroy();
    PhysicalRuntime.Reset();
}

namespace AftershockPhysicalDetail
{
FString MaterialSourceDigest(const UTerrainGridSubsystem& Terrain,const FAftershockMaterialContinuationProfile& P,
    const TArray<FIntPoint>& Ordered)
{
    TArray<uint8> Bytes;FMemoryWriter W(Bytes);int32 Seed=Terrain.TerrainSeed,Region=Terrain.GeographyIndex,Focus=Terrain.GeographyFocusIndex;
    double CellSize=Terrain.CellSize;W<<Seed<<Region<<Focus<<CellSize;
    const auto WriteNumber=[&](double Value){W<<Value;};
    const auto WriteText=[&](FString Value){W<<Value;};
    WriteNumber(P.ProfileVersion);const auto& H=P.Weather;WriteNumber(H.ScenarioVersion);WriteText(H.SourceCode.ToString());
    WriteText(H.ContextSourceURL);WriteText(H.Assumptions);WriteNumber(H.ReferenceElevationM);WriteNumber(H.LapseRateCPerKm);
    WriteNumber(H.AllSnowC);WriteNumber(H.AllRainC);WriteNumber(H.MeltBaseC);WriteNumber(H.MeltMmPerDegreeDay);
    WriteNumber(H.SnowBulkDensityKgM3);WriteNumber(H.SnowWeakLayerStrengthPa);WriteNumber(H.Segments.Num());
    for(const auto& S:H.Segments){WriteNumber(S.Days);WriteNumber(S.AirTemperatureC);WriteNumber(S.PrecipitationMmPerDay);WriteNumber(S.SuppliedSnowWaterMmPerDay);}
    const auto& M=P.Mantle;WriteText(M.SourceCode.ToString());WriteNumber(M.DepthM);WriteNumber(M.Porosity01);
    WriteNumber(M.DryBulkDensityKgM3);WriteNumber(M.CohesionPa);WriteNumber(M.FrictionAngleDegrees);WriteNumber(M.InitialPoreWaterDepthM);
    WriteNumber(M.bExplicitWeatheredMantleOverScenarioBedrock?1.:0.);WriteNumber(P.MinimumForestFuel01);
    WriteNumber(P.MinimumSlopeDegrees);WriteNumber(P.MaximumSlopeDegrees);WriteNumber(Ordered.Num());
    for(FIntPoint Coord:Ordered)
    {
        const auto& C=Terrain.GetCell(Coord);W<<Coord;WriteNumber(C.Elevation);WriteNumber(C.RealElevationMetres);
        WriteNumber(C.bHasRealElevationSample?1.:0.);WriteNumber(static_cast<int32>(C.SoilType));WriteNumber(C.ForestFuel01);
        WriteNumber(C.bIsOcean?1.:0.);WriteNumber(C.bIsRiverbed?1.:0.);WriteNumber(C.bVolcanicVent?1.:0.);
        // The saved eligibility rule uses canonical immutable-neighbour slope.
        // Record the actual support used, including missing boundary neighbours.
        for(FIntPoint Step:Steps){FGridCell N;const bool Found=Terrain.TryGetCell(Coord+Step,N);WriteNumber(Found?1.:0.);if(Found)WriteNumber(N.Elevation);}
    }
    uint8 Digest[FSHA1::DigestSize];FSHA1::HashBuffer(Bytes.GetData(),Bytes.Num(),Digest);return BytesToHex(Digest,FSHA1::DigestSize);
}

bool AppendMaterialReceipt(const TSharedPtr<FJsonObject>& Root,const UTerrainGridSubsystem& Terrain,
    const FAftershockMaterialContinuationProfile& Profile,const TArray<FIntPoint>& Ordered,
    const FAftershockMaterialInitializationReceipt& Receipt,FString& Out,FString& Why)
{
    if(!Root.IsValid()||Ordered.IsEmpty()||Ordered.Num()!=Receipt.CellsVisited)
    {Why=TEXT("Invalid material provenance staging inputs");return false;}
    const TArray<TSharedPtr<FJsonValue>>* Existing=nullptr;TArray<TSharedPtr<FJsonValue>> Records;
    if(Root->HasField(TEXT("material_extensions")))
    {
        if(!Root->TryGetArrayField(TEXT("material_extensions"),Existing)||!Existing||Existing->Num()>=4096)
        {Why=TEXT("Material provenance extension array is malformed or exceeds its bounded history");return false;}
        Records=*Existing;
    }
    auto Entry=MakeShared<FJsonObject>();Entry->SetNumberField(TEXT("sequence"),Records.Num());
    Entry->SetNumberField(TEXT("new_cells"),Ordered.Num());Entry->SetNumberField(TEXT("snow_cells"),Receipt.SnowCells);
    Entry->SetNumberField(TEXT("mantle_cells"),Receipt.RegolithCells);Entry->SetNumberField(TEXT("snow_water_kg_added"),Receipt.SnowWaterKg);
    Entry->SetNumberField(TEXT("dry_soil_kg_added"),Receipt.DrySoilKg);Entry->SetNumberField(TEXT("pore_water_m3_added"),Receipt.PoreWaterM3);
    Entry->SetNumberField(TEXT("antecedent_liquid_outflow_m3"),Receipt.AntecedentLiquidOutflowM3);
    Entry->SetStringField(TEXT("source_sha1"),MaterialSourceDigest(Terrain,Profile,Ordered));
    Entry->SetStringField(TEXT("source_hash_scope"),TEXT("saved full authored profile, seed/focus, exact cell coordinates, immutable DEM/land flags and neighbour slope support; checksum, not source-provider signature"));
    FIntPoint Min=Ordered[0],Max=Ordered[0];TArray<TSharedPtr<FJsonValue>> Runs;
    int32 FirstX=Ordered[0].X,LastX=FirstX,Row=Ordered[0].Y;
    const auto AddRun=[&]()
    {TArray<TSharedPtr<FJsonValue>> R;R.Add(MakeShared<FJsonValueNumber>(Row));R.Add(MakeShared<FJsonValueNumber>(FirstX));R.Add(MakeShared<FJsonValueNumber>(LastX));Runs.Add(MakeShared<FJsonValueArray>(R));};
    for(int32 I=0;I<Ordered.Num();++I)
    {
        const FIntPoint Coord=Ordered[I];Min.X=FMath::Min(Min.X,Coord.X);Min.Y=FMath::Min(Min.Y,Coord.Y);Max.X=FMath::Max(Max.X,Coord.X);Max.Y=FMath::Max(Max.Y,Coord.Y);
        if(I==0)continue;
        if(Coord.Y==Row&&static_cast<int64>(Coord.X)==static_cast<int64>(LastX)+1)LastX=Coord.X;
        else{AddRun();Row=Coord.Y;FirstX=LastX=Coord.X;}
    }
    AddRun();Entry->SetArrayField(TEXT("cell_runs_y_xfirst_xlast_inclusive"),Runs);
    Entry->SetNumberField(TEXT("min_x"),Min.X);Entry->SetNumberField(TEXT("min_y"),Min.Y);
    Entry->SetNumberField(TEXT("max_x"),Max.X);Entry->SetNumberField(TEXT("max_y"),Max.Y);
    Records.Add(MakeShared<FJsonValueObject>(Entry));Root->SetArrayField(TEXT("material_extensions"),Records);
    Root->SetStringField(TEXT("material_continuation_scope"),TEXT("Only newly resident zero-stock Version 0 cells; same saved authored antecedent history. Existing consumed/deposited stocks are preserved. Antecedent liquid is not injected into live surface water."));
    FString Candidate;if(!FJsonSerializer::Serialize(Root,TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Candidate)))
    {Why=TEXT("Could not serialize material provenance before commit");return false;}
    FTCHARToUTF8 UTF8(*Candidate);if(UTF8.Length()>256*1024)
    {Why=TEXT("Material provenance exceeds the 256 KiB save bound; no material cells initialized");return false;}
    Out=MoveTemp(Candidate);return true;
}
}

void UDisasterSimulationSubsystem::InitializePhysicalMaterials()
{
    using namespace AftershockPhysicalDetail;
    auto* T=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
    if(!PhysicalRuntime||!T||T->ContinuationGeneratorVersion!=3||T->Cells.IsEmpty()||T->Cells.Num()>65536)return;
    auto& R=*PhysicalRuntime;const uint64 Revision=T->GetTerrainRevision();if(R.MaterialCheckedRevision==Revision)return;
    if(PhysicalState.bNeedsLegacyWaterSeed)return;
    // A V4 restore has no authored weather receipt. The water-migration flag
    // later becomes false; it must never grant permission to invent old weather.
    if(R.bRestored&&PhysicalState.MaterialScenarioReceiptJSON.IsEmpty())
    {R.MaterialCheckedRevision=Revision;return;}
    const auto Frontier=R.Water.CaptureSaveFrontier();
    // The ONLY active-mass exception is explicit pre-replay preparation after
    // every older accepted/caller quantum drained. No worker/replay sees edits.
    const bool PreparingMass=R.bMassAwaitingBoundary&&R.bMassSupportExpanded&&!R.bMassWaiting&&!R.MassReplay
        &&PhysicalState.Episode.bActive&&IsMass(PhysicalState.Episode.Type)
        &&!R.bInitializing&&R.RestoredAcceptedSeconds==0.&&R.CallerDebt<=1.e-12
        &&!Frontier.WorkerInFlight&&Frontier.Unconsumed.empty();
    if(R.bInitializing||R.bMassWaiting||R.RestoredAcceptedSeconds>0.
        ||((R.bMassAwaitingBoundary||(PhysicalState.Episode.bActive&&IsMass(PhysicalState.Episode.Type)))&&!PreparingMass))return;
    if(Frontier.WorkerInFlight||!Frontier.Unconsumed.empty())return;
    TArray<FIntPoint> Targets;Targets.Reserve(T->Cells.Num());bool HasInitialized=false;
    for(const auto& C:T->Cells){if(C.MaterialScenarioVersion==0)Targets.Add(C.Coords);else HasInitialized=true;}
    if(Targets.IsEmpty()){R.MaterialCheckedRevision=Revision;return;}
    Targets.Sort([](FIntPoint A,FIntPoint B){return A.Y==B.Y?A.X<B.X:A.Y<B.Y;});
    FAftershockMaterialContinuationProfile Profile;TSharedPtr<FJsonObject> JSON;FString Why;
    const bool Fresh=PhysicalState.MaterialScenarioReceiptJSON.IsEmpty();
    if(Fresh)
    {
        if(R.bRestored||HasInitialized){R.MaterialCheckedRevision=Revision;return;}
        FAftershockMaterialClimateContext Context;Context.PrimarySourceURL=TEXT("https://power.larc.nasa.gov/docs/methodology/citations/");
        FAftershockMonthlyClimate Cold;Cold.T2MC=100.f;
        for(int32 Month=1;Month<=12;++Month)
        {FAftershockMonthlyClimate C;if(TryGetAftershockFocusMonthlyClimate(T->GeographyIndex,T->GeographyFocusIndex,Month,C)&&C.T2MC<Cold.T2MC)Cold=C;}
        Context.bVerifiedContext=Cold.T2MC<100.f;Context.ColdestMonthlyMeanTemperatureC=Cold.T2MC;Context.ReferenceElevationM=Cold.SourceElevationM;
        FAftershockAuthoredWinterOptions Winter;Winter.bExplicitlyRequestAuthoredWinter=Context.bVerifiedContext;
        if(!AftershockMassTerrain::MakeAuthoredWinterHistory(Context,Winter,Profile.Weather,Why))
        {R.MaterialCheckedRevision=Revision;UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_MATERIAL_SCENARIO_REJECTED %s"),*Why);return;}
        auto& M=Profile.Mantle;M.SourceCode=TEXT("AuthoredWeatheredMantleV1");M.DepthM=.8;M.Porosity01=.4;
        M.DryBulkDensityKgM3=1500.;M.CohesionPa=800.;M.FrictionAngleDegrees=30.;M.InitialPoreWaterDepthM=.12;
        M.bExplicitWeatheredMantleOverScenarioBedrock=true;
        JSON=MakeShared<FJsonObject>();JSON->SetNumberField(TEXT("version"),1);JSON->SetNumberField(TEXT("seed"),T->TerrainSeed);
        JSON->SetNumberField(TEXT("region"),T->GeographyIndex);JSON->SetNumberField(TEXT("focus"),T->GeographyFocusIndex);
        const auto& H=Profile.Weather;JSON->SetStringField(TEXT("weather_source"),H.SourceCode.ToString());JSON->SetStringField(TEXT("context_url"),H.ContextSourceURL);
        JSON->SetStringField(TEXT("assumptions"),H.Assumptions+TEXT(" Explicit hypothetical 0.8 m weathered mantle on mapped vegetated 8-60 degree slopes, not a soil survey. Porosity 0.4, dry density 1500 kg/m3, cohesion 800 Pa, friction 30 degrees, initial pore-water depth 0.12 m. Antecedent liquid leaves the initial scenario; it is not surface-water input."));
        JSON->SetNumberField(TEXT("reference_elevation_m"),H.ReferenceElevationM);JSON->SetNumberField(TEXT("lapse_c_per_km"),H.LapseRateCPerKm);
        JSON->SetNumberField(TEXT("snow_density_kg_m3"),H.SnowBulkDensityKgM3);JSON->SetNumberField(TEXT("weak_layer_pa"),H.SnowWeakLayerStrengthPa);
        TSharedPtr<FJsonObject> Contract;if(!AftershockMassTerrain::EncodeMaterialContinuationProfile(Profile,Contract,Why))
        {R.MaterialCheckedRevision=Revision;return;}JSON->SetObjectField(TEXT("continuation_profile"),Contract);
    }
    else
    {
        double Seed=0.,Region=0.,Focus=0.,Version=0.;const TSharedPtr<FJsonObject>* Contract=nullptr;
        const auto Reader=TJsonReaderFactory<>::Create(PhysicalState.MaterialScenarioReceiptJSON);
        if(!FJsonSerializer::Deserialize(Reader,JSON)||!JSON.IsValid()||!JSON->TryGetNumberField(TEXT("version"),Version)||Version!=1.
            ||!JSON->TryGetNumberField(TEXT("seed"),Seed)||Seed!=T->TerrainSeed||!JSON->TryGetNumberField(TEXT("region"),Region)||Region!=T->GeographyIndex
            ||!JSON->TryGetNumberField(TEXT("focus"),Focus)||Focus!=T->GeographyFocusIndex
            ||!JSON->TryGetObjectField(TEXT("continuation_profile"),Contract)||!Contract
            ||!AftershockMassTerrain::DecodeMaterialContinuationProfile(*Contract,Profile,Why))
        {
            R.MaterialCheckedRevision=Revision;
            UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_MATERIAL_EXTENSION_DEFERRED newCells=%d existingStocksUnchanged=1 noWeatherGuessed=1 reason=%s"),Targets.Num(),Why.IsEmpty()?TEXT("Receipt lacks exact saved continuation profile or terrain identity"):*Why);return;
        }
    }
    TArray<FAftershockAuthoredRegolithPatch> Patches;
    if(!AftershockMassTerrain::BuildAuthoredMantlePatches(*T,Profile,Targets,Patches,Why))
    {R.MaterialCheckedRevision=Revision;UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_MATERIAL_EXTENSION_REJECTED %s"),*Why);return;}
    FString StagedJSON;
    const auto Receipt=AftershockMassTerrain::InitializeAuthoredMaterialSubset(*T,Profile.Weather,Patches,Targets,
        [&](const FAftershockMaterialInitializationReceipt& Prepared,FString& Failure)
        {
            if(Fresh)
            {
                JSON->SetNumberField(TEXT("snow_water_kg"),Prepared.SnowWaterKg);JSON->SetNumberField(TEXT("dry_soil_kg"),Prepared.DrySoilKg);
                JSON->SetNumberField(TEXT("pore_water_m3"),Prepared.PoreWaterM3);JSON->SetNumberField(TEXT("antecedent_liquid_outflow_m3"),Prepared.AntecedentLiquidOutflowM3);
            }
            return AppendMaterialReceipt(JSON,*T,Profile,Targets,Prepared,StagedJSON,Failure);
        });
    R.MaterialCheckedRevision=T->GetTerrainRevision();
    if(!Receipt.bApplied){UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_MATERIAL_EXTENSION_REJECTED existingStocksUnchanged=1 %s"),*Receipt.Failure);return;}
    PhysicalState.MaterialScenarioReceiptJSON=MoveTemp(StagedJSON);R.bRiskDirty=true;
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_AUTHORED_MATERIALS mode=%s newCells=%d preservedCells=%d snowCells=%d mantleCells=%d stockWaterKgAdded=%.3f drySoilKgAdded=%.3f observedWeather=0 surfaceMeltwaterInjected=0"),
        Fresh?TEXT("new-game"):TEXT("saved-profile-extension"),Receipt.CellsVisited,T->Cells.Num()-Receipt.CellsVisited,Receipt.SnowCells,Receipt.RegolithCells,Receipt.SnowWaterKg,Receipt.DrySoilKg);
}


bool UDisasterSimulationSubsystem::RebuildPhysicalWaterDomain(FText& Error)
{
    using namespace AftershockPhysicalDetail;
    auto* T=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
    if(!PhysicalRuntime||!T||T->ContinuationGeneratorVersion!=3||T->Cells.IsEmpty()||T->Cells.Num()>65536)
    {Error=Failure(TEXT("Persistent water needs a bounded V3 world-centimetre terrain."));return false;}
    auto& R=*PhysicalRuntime;
    const auto Existing=R.Water.CaptureSaveFrontier();
    if(Existing.WorkerInFlight||Existing.UnconsumedSeconds>1.e-10){Error=Failure(TEXT("Hydraulic topology update waits for the accepted frontier."));return false;}
    bool Edited=false;
    // Explicit authored sub-grid hydraulic geometry. Provider land DEM does not
    // contain channel bathymetry. These offsets are saved separately from DEM.
    for(auto& C:T->Cells)
    {
        if(C.HydraulicProfileVersion==0)
        {
            if(C.bIsRiverbed&&!C.bIsOcean)C.HydraulicExcavationCm=FMath::Clamp(C.RiverCapacity*100.f,100.f,400.f);
            if(C.bIsOcean&&C.Elevation>=T->CoastalSeaLevelCm)
                C.HydraulicExcavationCm=C.Elevation-T->CoastalSeaLevelCm+200.f;
            C.HydraulicProfileVersion=1;Edited=true;
        }
    }
    for(auto& C:T->Cells)if(C.bHasDam&&C.DamOutflowStep==FIntPoint::ZeroValue)
    {
        double Lowest=TNumericLimits<double>::Max();FIntPoint Chosen=FIntPoint::ZeroValue;
        for(auto D:Steps){FGridCell N;double Bed;FText E;if(T->TryGetCell(C.Coords+D,N)&&(N.bIsRiverbed||N.bIsOcean)&&TryGetAftershockHydraulicBedCm(N,true,Bed,E)&&Bed<Lowest){Lowest=Bed;Chosen=D;}}
        if(Chosen==FIntPoint::ZeroValue){Error=Failure(TEXT("Dam has no canonical resident channel outflow face."));return false;}
        C.DamOutflowStep=Chosen;Edited=true;
    }
    if(Edited)T->MarkTerrainEdited();
    const double Area=FMath::Square(T->CellSize*.01);
    TMap<FIntPoint,const FAftershockHydraulicCellSaveRecord*> Saved;
    for(const auto& C:PhysicalState.WaterCells)Saved.Add(C.Coord,&C);
    TArray<FAftershockHydraulicPersistentCell> State;State.Reserve(T->Cells.Num());
    TArray<FAftershockHydraulicBoundarySpec> Boundaries;
    for(const auto& C:T->Cells)
    {
        FAftershockHydraulicPersistentCell P;P.Coord=C.Coords;P.CapturedCellAreaM2=Area;double Bed=0.;
        if(!TryGetAftershockHydraulicBedCm(C,true,Bed,Error))return false;
        if(const auto* Old=Saved.Find(C.Coords)){P.VolumeM3=(*Old)->VolumeM3;P.HuM2PerSecond=(*Old)->DischargeXM2PerSecond;P.HvM2PerSecond=(*Old)->DischargeYM2PerSecond;}
        else if(!PhysicalState.bWaterInitialized)
        {
            double Head=C.bIsOcean?T->CoastalSeaLevelCm:Bed;
            if(C.bIsRiverbed&&!C.bIsOcean)Head=Bed+C.HydraulicExcavationCm*FMath::Clamp(C.RiverFill01,0.f,1.f);
            if(PhysicalState.bNeedsLegacyWaterSeed)
            {if(const auto* Prior=LatestVisualSnapshot.WaterSurfaceElevationCm.Find(C.Coords))Head=FMath::Max(Head,static_cast<double>(*Prior));else Head=FMath::Max(Head,C.Elevation+static_cast<double>(C.FloodDepth));}
            P.VolumeM3=FMath::Max(0.,Head-Bed)*.01*Area;
        }
        // Newly resident dry cells start dry. New ocean volume is an explicit
        // sea-boundary influx through the solver, never invisible initialization.
        State.Add(P);
        if(C.bIsRiverbed&&!C.bIsOcean)
        {
            for(auto D:Steps)if(!T->IsValidCoord(C.Coords+D))
            {FAftershockHydraulicBoundarySpec B;B.Coord=C.Coords;B.OutwardStep=D;B.Kind=EAftershockHydraulicBoundary::RiverStage;B.StageElevationCm=Bed+C.HydraulicExcavationCm*FMath::Clamp(C.RiverFill01,.25f,.8f);Boundaries.Add(B);}
        }
    }
    FAftershockHydraulicTerrainInput Input;Input.Cells=T->Cells;Input.PersistentCells=State;Input.ExteriorBoundaries=Boundaries;
    Input.CellSizeCm=T->CellSize;Input.CoastalSeaLevelCm=T->CoastalSeaLevelCm;Input.SourceTerrainVersion=T->ContinuationGeneratorVersion;Input.bModel2Active=true;
    FAftershockHydraulicTerrainBuild Built;if(!BuildAftershockHydraulicTerrain(Input,Built,Error))return false;
    if(PhysicalState.Episode.bActive&&IsMass(PhysicalState.Episode.Type)&&PhysicalState.bWaterInitialized)
    {
        // Explicit bounded operator split: liquid support stays frozen while
        // the accepted granular replay is moving. Preserve this SAME support
        // through save/load; rebind to deposited ground when motion completes.
        for(auto& G:Built.Domain.Cells)if(const auto* Prior=Saved.Find(FIntPoint(G.X,G.Y)))G.BedM=(*Prior)->BedElevationCm*.01;
        for(auto& F:Built.Domain.Faces)
        {
            if(F.Kind==AftershockWater::FaceKind::HydraulicStructure)
            {
                F.GateInvertM=FMath::Max(Built.Domain.Cells[F.Left].BedM,Built.Domain.Cells[F.Right].BedM);
                F.GateHeightM=FMath::Max(0.,FMath::Min(1.5,F.WeirCrestM-F.GateInvertM));
            }
            if(F.Kind==AftershockWater::FaceKind::StageBoundary)
            {
                const auto& G=Built.Domain.Cells[F.Left];const auto& Cell=T->GetCell(FIntPoint(G.X,G.Y));
                if(!Cell.bIsOcean)F.StageHeadM=G.BedM+Cell.HydraulicExcavationCm*.01*FMath::Clamp(Cell.RiverFill01,.25f,.8f);
            }
        }
    }
    AftershockWater::Forcing RestoredSources;
    if(PhysicalState.PendingAcceptedSeconds>0.
        &&!BindSavedAcceptedForcing(PhysicalState,Built.Domain,RestoredSources,Error))return false;
    AftershockWater::InitializeInput Init;Init.Geometry=MoveTemp(Built.Domain);Init.Cells=MoveTemp(Built.InitialCells);
    Init.CumulativeInfiltrationM3.reserve(T->Cells.Num());
    for(const auto& Cell:T->Cells)
    {
        const auto* Prior=Saved.Find(Cell.Coords);const double Total=Prior?(*Prior)->CumulativeInfiltrationM3:0.;
        Init.CumulativeInfiltrationM3.push_back(Total);R.AppliedInfiltrationM3.Add(Cell.Coords,Total);
    }
    // Thirty-two off-thread steps amortize the 80ms owner/publication cadence.
    // The former eight-step quantum throttled a20x storm below real time even
    // with idle CPU. The exact solver, CFL, source duration and ledger remain.
    Init.NumericalSettings.MaximumStepsPerAdvance=32;Init.NumericalSettings.MaximumStepSeconds=1.;
    if(PhysicalState.bWaterInitialized)
    {
        Init.Origin.HasLedger=true;Init.Origin.Ledger=FromSaved(PhysicalState.WaterBudget);Init.Origin.SimulatedSeconds=PhysicalState.PhysicalSeconds;
        if(Existing.Published)Init.ExternalSurfaceSourceM3=FMath::Max(0.,PhysicalState.WaterBudget.ExternalInflowM3-Existing.Published->Ledger.ExternalInflowM3);
    }
    std::string Why;if(!R.Water.BeginInitialize(std::move(Init),Existing.Published?Existing.Published->Stamp.Publication:0,Why))
    {Error=Failure(UTF8_TO_TCHAR(Why.c_str()));return false;}
    if(PhysicalState.PendingAcceptedSeconds>0.)
    {
        R.RestoredAcceptedSeconds=PhysicalState.PendingAcceptedSeconds;R.RestoredAcceptedSources=MoveTemp(RestoredSources);
        PhysicalState.PendingAcceptedSeconds=0.;PhysicalState.PendingForcingCells.Reset();PhysicalState.PendingForcingBoundaries.Reset();
    }
    R.bInitializing=true;R.TerrainRevision=T->GetTerrainRevision();R.Forcing.reset();R.LastPublication=0;
    return true;
}

void UDisasterSimulationSubsystem::CapturePhysicalSaveState(FAftershockPhysicalHazardSaveState& Out) const
{
    using namespace AftershockPhysicalDetail;
    Out=PhysicalState;Out.PendingAcceptedSeconds=0.;Out.PendingForcingCells.Reset();Out.PendingForcingBoundaries.Reset();
    if(!PhysicalRuntime)return;
    const auto& R=*PhysicalRuntime;const auto Frontier=R.Water.CaptureSaveFrontier();
    Out.PendingPhysicalSeconds=R.CallerDebt+Frontier.UnconsumedSeconds+R.RestoredAcceptedSeconds;
    const auto Defer=[&](const FString& Why)
    {Out.bWaterInitialized=false;UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_PHYSICAL_SAVE_DEFERRED %s"),*Why);};
    if(R.bMassAwaitingBoundary)
    {Defer(TEXT("Granular support/material/water preparation has not reached an accepted replay frontier."));return;}
    if(Frontier.DesiredInitialization&&!Frontier.Published)
    {
        Out.ModelVersion=2;
        // Observational capture before the first restore Poll still exposes the
        // exact latched packet. It is NOT yet a valid resavable world frontier.
        if(R.RestoredAcceptedSeconds>0.&&Frontier.DesiredInitialization->Geometry)
        {
            FText PendingError;
            if(!CaptureAcceptedForcing(*Frontier.DesiredInitialization->Geometry,R.RestoredAcceptedSources,R.RestoredAcceptedSeconds,Out,PendingError))
                Defer(PendingError.ToString());
        }
        Defer(TEXT("No completed hydraulic initialization frontier."));return;
    }
    if(const auto* T=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
        T&&R.TerrainRevision!=T->GetTerrainRevision()&&!(Out.Episode.bActive&&IsMass(Out.Episode.Type)))
        Defer(TEXT("Canonical topology has not reached the hydraulic frontier."));
    if(Frontier.DesiredInitialization&&Frontier.Published&&Frontier.DesiredGeneration!=Frontier.Published->Stamp.Generation)
        Defer(TEXT("Hydraulic topology transition is still unpublished."));
    if(R.bMassWaiting)Out.Episode.MassReplay.Reset();
    if(Frontier.Unconsumed.size()>1||(R.RestoredAcceptedSeconds>0.&&!Frontier.Unconsumed.empty()))
    {Defer(TEXT("V5 owner permits one accepted immutable forcing quantum, not multiple history segments."));return;}
    FText Error;
    if(R.RestoredAcceptedSeconds>0.)
    {
        if(!Frontier.Published||!CaptureAcceptedForcing(*Frontier.Published->Geometry,R.RestoredAcceptedSources,R.RestoredAcceptedSeconds,Out,Error))
            Defer(Error.IsEmpty()?TEXT("Restored accepted source has no matching published geometry."):Error.ToString());
    }
    else if(!Frontier.Unconsumed.empty())
    {
        const auto& Debt=Frontier.Unconsumed.front();
        if(!Frontier.Published||!Debt.Sources||Debt.Sources->Generation()!=Frontier.Published->Stamp.Generation
            ||!CaptureAcceptedForcing(*Frontier.Published->Geometry,Debt.Sources->Values(),Debt.RemainingSeconds,Out,Error))
            Defer(Error.IsEmpty()?TEXT("Accepted source packet has no matching published geometry."):Error.ToString());
    }
}

bool UDisasterSimulationSubsystem::RestorePhysicalSaveState(const FAftershockPhysicalHazardSaveState& Saved,FText& Error)
{
    using namespace AftershockPhysicalDetail;
    auto* T=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
    if(!T){Error=Failure(TEXT("Terrain must restore before physical hazards."));return false;}
    if(!Saved.MaterialScenarioReceiptJSON.IsEmpty())
    {
        TSharedPtr<FJsonObject> J;const auto Reader=TJsonReaderFactory<>::Create(Saved.MaterialScenarioReceiptJSON);
        double Version=0.,Seed=0.,Region=0.,Focus=0.;
        if(!FJsonSerializer::Deserialize(Reader,J)||!J.IsValid()||!J->TryGetNumberField(TEXT("version"),Version)||Version!=1.
            ||!J->TryGetNumberField(TEXT("seed"),Seed)||Seed!=T->TerrainSeed||!J->TryGetNumberField(TEXT("region"),Region)||Region!=T->GeographyIndex
            ||!J->TryGetNumberField(TEXT("focus"),Focus)||Focus!=T->GeographyFocusIndex)
        {Error=Failure(TEXT("Authored material provenance belongs to another terrain/seed."));return false;}
    }
    FAftershockMassVisualEvent Replay;
    if(Saved.Episode.bActive&&IsMass(Saved.Episode.Type))
    {
        if(!DecodeAftershockMassReplay(Saved.Episode.MassReplay,Replay,Error)||Replay.EventID!=Saved.Episode.EventID||Replay.Result.HazardTypeTag!=static_cast<int32>(Saved.Episode.Type))
        {Error=Failure(TEXT("Physical replay event identity does not match the active episode."));return false;}
    }
    if(!FMath::IsFinite(Saved.PendingAcceptedSeconds)||Saved.PendingAcceptedSeconds<0.
        ||!FMath::IsFinite(Saved.PendingPhysicalSeconds)||Saved.PendingPhysicalSeconds<Saved.PendingAcceptedSeconds
        ||(Saved.PendingAcceptedSeconds>0.&&(Saved.ModelVersion!=2||!Saved.bWaterInitialized||Saved.PendingAcceptedSeconds<=1.e-12))
        ||(Saved.PendingAcceptedSeconds==0.&&(!Saved.PendingForcingCells.IsEmpty()||!Saved.PendingForcingBoundaries.IsEmpty())))
    {Error=Failure(TEXT("Saved accepted/caller hydraulic debt is inconsistent."));return false;}
    ShutdownPhysicalRuntime();PhysicalState=Saved;PhysicalRuntime=MakeShared<FAftershockPhysicalHazardRuntime>();
    auto& R=*PhysicalRuntime;R.bRestored=true;R.CallerDebt=Saved.PendingPhysicalSeconds-Saved.PendingAcceptedSeconds;R.LastWorldSeconds=GetWorld()->GetTimeSeconds();
    R.LastExposureSeconds=Saved.PhysicalSeconds;
    R.WaterIndex.Reserve(Saved.WaterCells.Num());for(int32 I=0;I<Saved.WaterCells.Num();++I)R.WaterIndex.Add(Saved.WaterCells[I].Coord,I);
    if(Saved.ModelVersion==2&&!RebuildPhysicalWaterDomain(Error))return false;
    if(Saved.Episode.bActive&&IsMass(Saved.Episode.Type))
    {
        Replay.StartedAtWorldSeconds=GetWorld()->GetTimeSeconds()-Saved.Episode.PlaybackSeconds;
        R.MassReplay=MakeShared<FAftershockMassVisualEvent>(MoveTemp(Replay));
        R.MassActor=GetWorld()->SpawnActor<AAftershockMassMovementVisualActor>();FString Why;
        if(R.MassActor.IsValid())R.MassActor->SetExternallyClocked();
        if(!R.MassActor.IsValid()||!R.MassActor->BeginAcceptedEvent(*R.MassReplay,Why)){Error=Failure(Why);return false;}
        R.MassActor->SetPlaybackWorldSeconds(GetWorld()->GetTimeSeconds());
        // Saved mineral/pore stocks already include this prefix. Derive only
        // bookkeeping from the frozen replay; never apply it a second time.
        for(const auto& F:R.MassReplay->Result.Frames)if(F.Seconds<=Saved.Episode.PlaybackSeconds)
            for(const auto& C:F.Cells)R.AppliedDepositM3.Add(C.Coord,C.DepositedVolumeM3);
    }
    GetWorld()->GetTimerManager().SetTimer(PhysicalTimer,this,&UDisasterSimulationSubsystem::AdvancePhysicalHazards,Heartbeat,true);
    return true;
}

bool UDisasterSimulationSubsystem::TryGetPhysicalWaterSample(FIntPoint Coord,double& HeadCm,double& DepthCm,FVector2D& VelocityCmPerSecond) const
{
    if(PhysicalState.ModelVersion!=2||!PhysicalState.bWaterInitialized||!PhysicalRuntime)return false;
    const int32* I=PhysicalRuntime->WaterIndex.Find(Coord);const auto* T=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
    FGridCell C;double Bed=0.;
    if(!I||!PhysicalState.WaterCells.IsValidIndex(*I)||!T||!T->TryGetCell(Coord,C))return false;
    const auto Published=PhysicalRuntime->Water.LastPublished();
    if(Published&&Published->Cells.size()>static_cast<size_t>(*I)&&Published->Cells[*I].X==Coord.X&&Published->Cells[*I].Y==Coord.Y)Bed=Published->Cells[*I].BedM*100.;
    else Bed=PhysicalState.WaterCells[*I].BedElevationCm; // Restored frozen support before the first async publication.
    const auto& S=PhysicalState.WaterCells[*I];DepthCm=S.VolumeM3/PhysicalState.CapturedCellAreaM2*100.;HeadCm=Bed+DepthCm;
    const double H=DepthCm*.01;VelocityCmPerSecond=H>1.e-7?FVector2D(S.DischargeXM2PerSecond,S.DischargeYM2PerSecond)*(100./H):FVector2D::ZeroVector;
    return FMath::IsFinite(HeadCm)&&FMath::IsFinite(DepthCm)&&!VelocityCmPerSecond.ContainsNaN();
}

float UDisasterSimulationSubsystem::GetPhysicalHazardWeight(EDisasterType Type) const
{
    if(!AftershockPhysicalDetail::IsMass(Type))return 1.f;
    if(!PhysicalRuntime||PhysicalRuntime->bStopped)return 0.f;
    return Type==EDisasterType::Avalanche?PhysicalRuntime->SnowWeight:PhysicalRuntime->SoilWeight;
}

FDisasterReport UDisasterSimulationSubsystem::BeginPhysicalDisaster(EDisasterType Type,float Severity,float Awareness,bool bDaytime)
{
    using namespace AftershockPhysicalDetail;
    FDisasterReport Report;Report.DisasterType=Type;Report.BaseSeverity=Severity;
    auto Reject=[&](const FText& Why){Report.bWasRejected=true;Report.RejectionReason=Why;return Report;};
    auto* World=GetWorld();auto* T=World?World->GetSubsystem<UTerrainGridSubsystem>():nullptr;
    auto* B=World?World->GetSubsystem<UBuildingManagerSubsystem>():nullptr;
    if(!PhysicalRuntime||!T||!B||T->ContinuationGeneratorVersion!=3||PhysicalRuntime->bStopped)
        return Reject(Failure(TEXT("Fiziksel afet için V3 coğrafya ve etkin oyun dünyası gerekli.")));
    if(PhysicalState.Episode.bActive)return Reject(Failure(TEXT("Sürmekte olan fiziksel afet henüz tamamlanmadı.")));
    if(PhysicalRuntime->RestoredAcceptedSeconds>0.)return Reject(Failure(TEXT("Kayıttaki kabul edilmiş su adımı henüz yeniden bağlanıyor.")));
    FText Why;if(!CanTriggerDisaster(Type,Why))return Reject(Why);
    if(!PhysicalState.bWaterInitialized||PhysicalRuntime->bInitializing)
        return Reject(Failure(TEXT("Su alanı hazırlanıyor; fizik sınırının tamamlanmasını bekle.")));
    // A completed worker can wait for the next timer Poll. Consume it here so
    // background flow never makes every button appear permanently busy.
    const auto Ready=PhysicalRuntime->Water.Poll();
    if(Ready.Failed)
    {
        PhysicalRuntime->bStopped=true;PhysicalRuntime->LastFailure=UTF8_TO_TCHAR(Ready.Error.c_str());
        return Reject(Failure(PhysicalRuntime->LastFailure));
    }
    if(Ready.Published)PublishPhysicalWater();
    const auto WaterFront=PhysicalRuntime->Water.CaptureSaveFrontier();
    auto& R=*PhysicalRuntime;
    // Flush the quiet interval since the last timer callback before changing
    // source scale/phase. This prefix belongs before the new event boundary.
    double CalmDelta=0.;FString ClockFailure;
    if(!AftershockPhysicalLifecycle::AccrueCallerTime(World->GetTimeSeconds(),PhysicalState.PhysicalSeconds,
        WaterFront.UnconsumedSeconds+R.RestoredAcceptedSeconds,false,1.,R.LastWorldSeconds,R.CallerDebt,CalmDelta,ClockFailure))
    {
        R.bStopped=true;R.LastFailure=ClockFailure;return Reject(Failure(ClockFailure));
    }
    if(R.MassActor.IsValid()){R.MassActor->ClearEvent();R.MassActor->Destroy();R.MassActor.Reset();}
    R.MassReplay.Reset();R.AppliedDepositM3.Reset();R.Forcing.reset();
    R.MassExpansion.Reset();R.bMassSupportExpanded=false;R.bMassSupportPrepared=false;
    auto Episode=FAftershockPhysicalEpisodeSaveState();Episode.bActive=true;Episode.EventID=FGuid::NewGuid();Episode.Type=Type;
    Episode.BaseSeverity=Severity;Episode.CityAwareness01=Awareness;Episode.bDaytime=bDaytime;
    // Previously accepted/owed calm seconds retain their ORIGINAL controls.
    // The new source starts only after that exact physical frontier is reached.
    Episode.StartPhysicalSeconds=PhysicalState.PhysicalSeconds+WaterFront.UnconsumedSeconds+R.CallerDebt;
    auto* Life=World->GetSubsystem<UCityLifeSubsystem>();
    if(Life&&!Life->IsCityInitialized())Life->InitializeForCity();
    if(Life)Life->RefreshCity();
    Episode.BeforeResidents=Life?Life->GetResidents():TArray<FCityResidentRecord>();
    TArray<FBuildingInstance> Records;B->GetAllBuildings(Records);
    Records.Sort([](const auto& A,const auto& C){return A.InstanceID<C.InstanceID;});
    for(const auto& Record:Records)
    {
        FAftershockPhysicalBuildingBaseline Baseline;Baseline.State=Record;
        Baseline.DefinitionType=Record.BuildingData?Record.BuildingData->Type:EBuildingType::House;
        Baseline.State.BuildingData=nullptr;Baseline.State.ISMInstanceIndex=INDEX_NONE;Episode.BeforeBuildings.Add(Baseline);
    }
    if(IsMass(Type))
    {
        R.bMassAwaitingBoundary=true;Episode.SourceDurationSeconds=180.;Episode.SimulationTimeScale=1.;
    }
    else
    {
        Episode.SourceDurationSeconds=Type==EDisasterType::Flood?3600.:120.;
        Episode.SimulationTimeScale=Type==EDisasterType::Flood?20.:1.;
        // Explicit authored storm:30..150mm/hour, compressed game clock. The
        // solver receives m/s; severity never masquerades as centimetres.
        Episode.RainMPerSecond=Type==EDisasterType::Flood?(30.+1.2*Severity)/3600000.:0.;
    }
    PhysicalState.Episode=MoveTemp(Episode);R.LastExposureSeconds=PhysicalState.PhysicalSeconds;
    LatestVisualSnapshot=FAftershockDisasterVisualSnapshot();LatestVisualSnapshot.Revision=FMath::Max(1,static_cast<int32>(++R.ForcingRevision));
    LatestVisualSnapshot.Type=Type;LatestVisualSnapshot.BaseSeverity=Severity;LatestVisualSnapshot.EffectiveIntensity=Severity;
    LatestVisualSnapshot.StartedAtWorldSeconds=World->GetTimeSeconds();
    LatestVisualSnapshot.DurationSeconds=PhysicalState.Episode.SourceDurationSeconds/PhysicalState.Episode.SimulationTimeScale;
    LatestVisualSnapshot.FloodModelVersion=IsWater(Type)?2:0;
    LatestVisualSnapshot.DominantWindDirection=ScenarioWindDirection;
    if(IsMass(Type)&&R.MassSource)
    {
        LatestVisualSnapshot.SourceCoord=R.MassSource->Input.Release[0].Coord;
        for(const auto& Pair:R.MassSource->Instability01)if(Pair.Value>0.)LatestVisualSnapshot.LocalSeverity.Add(Pair.Key,Pair.Value*Severity);
    }
    OnDisasterVisualSnapshotChanged.Broadcast(LatestVisualSnapshot);
    Report.DisasterEventID=PhysicalState.Episode.EventID;Report.bIsPending=true;
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_ACCEPTED event=%s hazard=%d pending=1 finalCasualtiesCommitted=0 rainMPerS=%.9f sourceSeconds=%.3f clockScale=%.1f"),
        *Report.DisasterEventID.ToString(),static_cast<int32>(Type),PhysicalState.Episode.RainMPerSecond,PhysicalState.Episode.SourceDurationSeconds,PhysicalState.Episode.SimulationTimeScale);
    return Report;
}

bool UDisasterSimulationSubsystem::StagePhysicalMassJob(FText& Error)
{
    using namespace AftershockPhysicalDetail;
    auto& R=*PhysicalRuntime;auto& E=PhysicalState.Episode;auto* World=GetWorld();
    auto* T=World->GetSubsystem<UTerrainGridSubsystem>();const auto* B=World->GetSubsystem<UBuildingManagerSubsystem>();
    const auto Boundary=R.Water.CaptureSaveFrontier();
    if(!T||!B||!E.bActive||!IsMass(E.Type)||!R.bMassAwaitingBoundary||R.bMassWaiting||R.MassReplay
        ||Boundary.WorkerInFlight||!Boundary.Unconsumed.empty()||R.RestoredAcceptedSeconds>0.||R.CallerDebt>1.e-12)
    {Error=Failure(TEXT("Granular support preparation requires a drained, idle pre-replay hydraulic boundary."));return false;}
    if(!R.bMassSupportExpanded)
    {
        if(!R.MassExpansion)
        {
            // Never manufacture a weather history for a legacy load or infer a
            // new profile from regional display data during expansion.
            TSharedPtr<FJsonObject> Receipt;const TSharedPtr<FJsonObject>* Contract=nullptr;
            double Seed=0.,Region=0.,Focus=0.;FString Why;FAftershockMaterialContinuationProfile Profile;
            const auto Reader=TJsonReaderFactory<>::Create(PhysicalState.MaterialScenarioReceiptJSON);
            if(PhysicalState.MaterialScenarioReceiptJSON.IsEmpty()
                ||!FJsonSerializer::Deserialize(Reader,Receipt)||!Receipt.IsValid()
                ||!Receipt->TryGetNumberField(TEXT("seed"),Seed)||Seed!=T->TerrainSeed
                ||!Receipt->TryGetNumberField(TEXT("region"),Region)||Region!=T->GeographyIndex
                ||!Receipt->TryGetNumberField(TEXT("focus"),Focus)||Focus!=T->GeographyFocusIndex
                ||!Receipt->TryGetObjectField(TEXT("continuation_profile"),Contract)
                ||!AftershockMassTerrain::DecodeMaterialContinuationProfile(*Contract,Profile,Why))
            {Error=Failure(TEXT("Granular support needs the exact saved authored continuation profile; legacy weather was not invented."));return false;}
            TArray<FIntPoint> Coords;Coords.Reserve(T->Cells.Num());for(const auto& C:T->Cells)Coords.Add(C.Coords);
            AftershockMassSupportPreparation::FPlan Plan;
            if(!AftershockMassSupportPreparation::BuildPlan(FIntPoint(T->GridWidth,T->GridHeight),
                T->GetSupportedGeographicCellBounds(),Coords,Plan,Why)){Error=Failure(Why);return false;}
            R.MassExpansion=MakeUnique<FAftershockTerrainChunkExpansion>();
            const double Started=FPlatformTime::Seconds();
            if(!T->BeginChunkExpansion(Plan.MissingChunks,*R.MassExpansion,Error)){R.MassExpansion.Reset();return false;}
            UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MASS_SUPPORT_PLAN oldCells=%d addedCells=%d finalCells=%d requestedBounds=(%d,%d)-(%d,%d) missingChunks=%d clippedToRealSource=%d preflightMs=%.3f maxGeneratedPerHeartbeat=1000 residentCap=65536 terrainMutated=0"),
                Plan.ExistingCells,Plan.AdditionalCells,Plan.FinalCells,Plan.RequestedCells.Min.X,Plan.RequestedCells.Min.Y,
                Plan.RequestedCells.Max.X,Plan.RequestedCells.Max.Y,Plan.MissingChunks.Num(),Plan.bClippedToSource,(FPlatformTime::Seconds()-Started)*1000.);
        }
        const double StageStarted=FPlatformTime::Seconds();
        if(!T->ContinueChunkExpansion(*R.MassExpansion,1000,Error)){R.MassExpansion.Reset();return false;}
        UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MASS_SUPPORT_STAGED preparedCells=%d requiredCells=%d generationMs=%.3f liveTerrainUnchanged=1"),
            R.MassExpansion->GetPreparedCells(),R.MassExpansion->GetRequiredCells(),(FPlatformTime::Seconds()-StageStarted)*1000.);
        if(!R.MassExpansion->IsReady())return true; // Pending, not accepted physical motion.
        const double CommitStarted=FPlatformTime::Seconds();
        if(!T->CommitChunkExpansion(*R.MassExpansion,Error)){R.MassExpansion.Reset();return false;}
        R.MassExpansion.Reset();R.bMassSupportExpanded=true;
        UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MASS_SUPPORT_COMMIT residentCells=%d commitAndSubscribersMs=%.3f newSettlementRecords=0"),
            T->Cells.Num(),(FPlatformTime::Seconds()-CommitStarted)*1000.);
    }
    if(!R.bMassSupportPrepared)
    {
        const double MaterialsStarted=FPlatformTime::Seconds();InitializePhysicalMaterials();
        for(const auto& C:T->Cells)if(C.MaterialScenarioVersion!=1)
        {Error=Failure(TEXT("Expanded support lacks the exact authored material profile; no granular stock was withdrawn."));return false;}
        UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MASS_SUPPORT_MATERIALS residentCells=%d extensionMs=%.3f priorStocksReset=0"),
            T->Cells.Num(),(FPlatformTime::Seconds()-MaterialsStarted)*1000.);
        const auto Published=R.Water.LastPublished();
        const bool FullPublished=Published&&Published->Geometry&&R.bInitialized&&!R.bInitializing
            &&Published->Stamp.Generation==R.Water.DesiredGeneration()
            &&R.TerrainRevision==T->GetTerrainRevision()&&PhysicalState.DomainRevision==T->GetTerrainRevision()
            &&PhysicalState.WaterCells.Num()==T->Cells.Num()&&R.WaterIndex.Num()==T->Cells.Num()
            &&Published->Cells.size()==static_cast<size_t>(T->Cells.Num());
        if(!FullPublished)
        {
            if(R.bInitializing)return true;
            const double RebindStarted=FPlatformTime::Seconds();
            if(!RebuildPhysicalWaterDomain(Error))return false;
            UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MASS_SUPPORT_WATER_REBIND residentCells=%d oldPublishedCells=%d rebindBuildMs=%.3f awaitingPublication=1"),
                T->Cells.Num(),PhysicalState.WaterCells.Num(),(FPlatformTime::Seconds()-RebindStarted)*1000.);
            return true; // Stage resumes only AFTER the immutable expanded domain publishes.
        }
        for(const auto& C:T->Cells)if(!R.WaterIndex.Contains(C.Coords))
        {Error=Failure(TEXT("Expanded hydraulic frontier does not cover every canonical support cell."));return false;}
        R.bMassSupportPrepared=true;
        UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MASS_SUPPORT_READY residentCells=%d waterCells=%d materialCells=%d exactCoordinateCoverage=1 physicalSeconds=%.17g callerSeconds=%.17g stockWithdrawalBeforeReady=0"),
            T->Cells.Num(),PhysicalState.WaterCells.Num(),T->Cells.Num(),PhysicalState.PhysicalSeconds,R.CallerDebt);
    }
    FAftershockMassTerrainOptions Options;Options.Material=E.Type==EDisasterType::Avalanche?EAftershockMassMaterial::Snow:EAftershockMassMaterial::SoilDebris;
    Options.Seed=static_cast<uint32>(T->TerrainSeed);Options.ReleaseFraction=.15+.45*FMath::Clamp(E.BaseSeverity*.01,0.,1.);
    const auto* Session=World->GetGameInstance()?World->GetGameInstance()->GetSubsystem<UAftershockOpeningSessionSubsystem>():nullptr;
    Options.bCallerAuthorizedGodMode=Session&&Session->GetSettings().Mode==EAftershockPlayMode::Sandbox;Options.bForceRelease=Options.bCallerAuthorizedGodMode;
    auto Snapshot=AftershockMassTerrain::BuildSnapshot(*T,*B,Options);
    if(!Snapshot.bReady){Error=Failure(Snapshot.Failure);return false;}
    // Longer OBSERVATION window for the same friction, gravity and finite stock.
    // The sparse kernel remains bounded; unfinished/unsupported motion still fails.
    Snapshot.Input.Settings.MaximumSeconds=180.;Snapshot.Input.Settings.MaxSubsteps=12000;
    Snapshot.Input.Settings.MaxParticleSteps=3000000;Snapshot.Input.Settings.MaxWorkUnits=250000000;
    FIntPoint ReleaseMin=Snapshot.Input.Release[0].Coord,ReleaseMax=ReleaseMin;
    for(const auto& Source:Snapshot.Input.Release)
    {ReleaseMin.X=FMath::Min(ReleaseMin.X,Source.Coord.X);ReleaseMin.Y=FMath::Min(ReleaseMin.Y,Source.Coord.Y);
     ReleaseMax.X=FMath::Max(ReleaseMax.X,Source.Coord.X);ReleaseMax.Y=FMath::Max(ReleaseMax.Y,Source.Coord.Y);}
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MASS_DOMAIN_PREPARED residentCells=%d releaseCells=%d releaseBounds=(%d,%d)-(%d,%d) minSupportedMarginCells=%d excludedNearBoundary=%d supportGuardOnly=1 fullRunoutGuarantee=0 exportUnknown=0 solveSeconds=180 maxSubsteps=12000 maxParcelSteps=3000000 maxWorkUnits=250000000"),
        Snapshot.Input.Cells.Num(),Snapshot.Input.Release.Num(),ReleaseMin.X,ReleaseMin.Y,ReleaseMax.X,ReleaseMax.Y,
        Snapshot.MinimumSelectedSupportCells,Snapshot.BoundaryExcludedSourceCells);
    R.MassSource=MakeShared<FAftershockMassTerrainSnapshot>(MoveTemp(Snapshot));R.MassBuildingSignature=BuildingSignature(*B);
    auto Input=R.MassSource->Input;
    R.MassJob=Async(EAsyncExecution::ThreadPool,[Input=MoveTemp(Input)](){return FAftershockMassMovementSolver::Solve(Input);});
    R.bMassWaiting=true;R.bMassAwaitingBoundary=false;return true;
}

bool UDisasterSimulationSubsystem::GetPhysicalRuntimeFailure(FText& Reason) const
{
    Reason=FText::GetEmpty();
    if(!PhysicalRuntime||!PhysicalRuntime->bStopped)return false;
    Reason=FText::FromString(PhysicalRuntime->LastFailure.IsEmpty()
        ?TEXT("Fizik işçisi durdu; olay sonuçlandırılmadı."):PhysicalRuntime->LastFailure);
    return true;
}

bool UDisasterSimulationSubsystem::TryGetPhysicalAudioProgress(double& Playback,double& Source,double& Duration,float& Moving) const
{
    using namespace AftershockPhysicalDetail;
    const auto& E=PhysicalState.Episode;Moving=0.;
    if(!E.EventID.IsValid()||!PhysicalRuntime||PhysicalState.ModelVersion!=2)return false;
    Playback=E.PlaybackSeconds;Source=E.SourceElapsedSeconds;Duration=E.SourceDurationSeconds;
    if(IsMass(E.Type))
    {
        if(!PhysicalRuntime->MassReplay||PhysicalRuntime->bMassWaiting||PhysicalRuntime->bMassAwaitingBoundary)return false;
        const auto& Result=PhysicalRuntime->MassReplay->Result;const FAftershockMassFrame* Frame=nullptr;
        for(const auto& F:Result.Frames)if(F.Seconds<=E.PlaybackSeconds)Frame=&F;
        double Mass=0.;if(Frame)for(const auto& C:Frame->Cells)Mass+=C.MovingMassKg;
        const double Released=Result.Budget.ReleasedKg+Result.Budget.EntrainedKg;
        Moving=Released>0.?FMath::Clamp(Mass/Released,0.,1.):0.;
        return true;
    }
    return IsWater(E.Type)&&PhysicalState.bWaterInitialized;
}

void UDisasterSimulationSubsystem::PublishPhysicalWater()
{
    using namespace AftershockPhysicalDetail;
    if(!PhysicalRuntime)return;auto& R=*PhysicalRuntime;
    const auto Frontier=R.Water.LastPublished();if(!Frontier||Frontier->Stamp.Publication==R.LastPublication)return;
    auto* T=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();if(!T)return;
    const bool InstallingGround=R.bInitializing||PhysicalState.ModelVersion!=2;
    const bool FirstModel2=PhysicalState.ModelVersion!=2;
    const double Before=PhysicalState.PhysicalSeconds;R.LastPublication=Frontier->Stamp.Publication;
    PhysicalState.ModelVersion=2;PhysicalState.bWaterInitialized=true;PhysicalState.bNeedsLegacyWaterSeed=false;
    PhysicalState.CapturedCellAreaM2=Frontier->Geometry->CellWidthM*Frontier->Geometry->CellHeightM;
    PhysicalState.PhysicalSeconds=Frontier->Stamp.SimulatedSeconds;PhysicalState.PublishedStep=Frontier->Stamp.CompletedStep;
    PhysicalState.DomainRevision=R.TerrainRevision;ToSaved(Frontier->Ledger,PhysicalState.WaterBudget);
    PhysicalState.WaterCells.SetNum(Frontier->Cells.size());R.WaterIndex.Reset();R.WaterIndex.Reserve(Frontier->Cells.size());
    R.VisualCells.Reset(Frontier->Cells.size());const auto Drawable=GetAftershockDrawableCellBounds(*T);
    // Hydraulic structures may transmit an orifice flux without equal surface
    // heads. Never blur those heads across a dam/gate face in the GPU material.
    TMap<int32,uint8> StructureMasks;
    for(const auto& F:Frontier->Geometry->Faces)if(F.Kind==AftershockWater::FaceKind::HydraulicStructure)
    {
        const uint8 Mask=F.NormalX>0.?2:F.NormalX<0.?8:F.NormalY>0.?4:1;
        StructureMasks.FindOrAdd(F.Left)|=Mask;
        if(F.Right>=0)StructureMasks.FindOrAdd(F.Right)|=Mask==1?4:Mask==2?8:Mask==4?1:2;
    }
    for(int32 I=0;I<static_cast<int32>(Frontier->Cells.size());++I)
    {
        const auto& C=Frontier->Cells[I];auto& S=PhysicalState.WaterCells[I];S.Coord=FIntPoint(C.X,C.Y);
        S.VolumeM3=C.DepthM*PhysicalState.CapturedCellAreaM2;S.BedElevationCm=C.BedM*100.;S.DischargeXM2PerSecond=C.DischargeXM2PerS;S.DischargeYM2PerSecond=C.DischargeYM2PerS;
        S.CumulativeInfiltrationM3=C.CumulativeInfiltrationM3;R.WaterIndex.Add(S.Coord,I);
        const int32 TI=T->CoordToIndex(S.Coord);if(T->Cells.IsValidIndex(TI))
        {
            auto& Cell=T->Cells[TI];Cell.FloodDepth=FMath::Max(0.,100.*(C.BedM+C.DepthM)-Cell.Elevation);
            const double Added=FMath::Max(0.,C.CumulativeInfiltrationM3-R.AppliedInfiltrationM3.FindRef(S.Coord));
            Cell.SoilPoreWaterDepthM+=Added/PhysicalState.CapturedCellAreaM2;
            if(Added>1.e-10)R.bRiskDirty=true;
            R.AppliedInfiltrationM3.Add(S.Coord,C.CumulativeInfiltrationM3);
        }
        if(Drawable.Contains(S.Coord))
        {
            auto& V=R.VisualCells.AddDefaulted_GetRef();V.Coord=S.Coord;V.BedElevationCm=C.BedM*100.;V.DepthCm=C.DepthM*100.;V.SurfaceElevationCm=V.BedElevationCm+V.DepthCm;
            V.VelocityCmPerSecond=C.DepthM>1.e-7?FVector2f(C.DischargeXM2PerS*100./C.DepthM,C.DischargeYM2PerS*100./C.DepthM):FVector2f::ZeroVector;
            V.BlockedFaces=(~C.ConnectedMask&15)|StructureMasks.FindRef(I);
        }
    }
    const double Advanced=FMath::Max(0.,PhysicalState.PhysicalSeconds-Before);
    if(PhysicalState.Episode.bActive&&IsWater(PhysicalState.Episode.Type))
    {auto& E=PhysicalState.Episode;E.PlaybackSeconds=FMath::Max(0.,PhysicalState.PhysicalSeconds-E.StartPhysicalSeconds);E.SourceElapsedSeconds=FMath::Min(E.SourceDurationSeconds,E.PlaybackSeconds);}
    R.bInitializing=false;R.bInitialized=true;
    if(!R.WaterActor.IsValid())R.WaterActor=GetWorld()->SpawnActor<AAftershockHydraulicSurfaceActor>();
    if(R.WaterActor.IsValid())
    {
        FText Error;
        if(R.WaterActor->Configure(*T,Error))
        {
            FAftershockHydraulicVisualFrame Frame;Frame.Cells=R.VisualCells;Frame.PhysicalSeconds=PhysicalState.PhysicalSeconds;
            Frame.ConnectivityRevision=Frontier->Stamp.Publication;Frame.VisualWindDirection=FVector2f(ScenarioWindDirection.X,ScenarioWindDirection.Y).GetSafeNormal();
            R.WaterActor->Update(Frame,Frontier->Stamp.Publication,Error);R.WaterActor->PumpUploads();
        }
        if(!Error.IsEmpty()&&R.LastFailure!=Error.ToString()){R.LastFailure=Error.ToString();UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_HYDRAULIC_RENDER %s"),*R.LastFailure);}
    }
    if(InstallingGround)
    {
        // A model 0 -> 2 initialization can finish without a new cell revision.
        // Install its SAME physical ground now, not on the player's next road
        // purchase. Ordinary 80 ms flow publications never enter this branch.
        const double Started=FPlatformTime::Seconds();const auto BeforeGround=GetAftershockTerrainSurfaceBuildStats(GetWorld());
        if(auto* Crowd=GetWorld()->GetSubsystem<UCrowdRenderSubsystem>())Crowd->EnsureTerrainVisualBuilt();
        const auto AfterGround=GetAftershockTerrainSurfaceBuildStats(GetWorld());
        UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_HYDRAULIC_GROUND_INSTALLED firstModel2=%d publication=%llu terrainRevision=%llu groundBuildDelta=%llu collisionCookDelta=%llu installAndSubscribersMs=%.3f ordinaryFlowPublication=0 canonicalWrites=0"),
            FirstModel2,Frontier->Stamp.Publication,T->GetTerrainRevision(),AfterGround.SuccessfulGroundBuilds-BeforeGround.SuccessfulGroundBuilds,
            AfterGround.CollisionCookAttempts-BeforeGround.CollisionCookAttempts,(FPlatformTime::Seconds()-Started)*1000.);
    }
}

void UDisasterSimulationSubsystem::AdvancePhysicalHazards()
{
    using namespace AftershockPhysicalDetail;
    UWorld* World=GetWorld();auto* T=World?World->GetSubsystem<UTerrainGridSubsystem>():nullptr;
    if(!World||!T||T->Cells.IsEmpty()||T->ContinuationGeneratorVersion!=3||!PhysicalRuntime)return;
    const auto* Saves=World->GetGameInstance()?World->GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>():nullptr;
    if(Saves&&(Saves->IsRestoring()||Saves->HasPendingLoad()||Saves->IsWorldLoadFailed(World)))return;
    auto& R=*PhysicalRuntime;if(R.bStopped)return;
    const double Now=World->GetTimeSeconds();const auto ClockFrontier=R.Water.CaptureSaveFrontier();
    const auto& EntryEpisode=PhysicalState.Episode;
    const auto* Startup=World->GetGameInstance()?World->GetGameInstance()->GetSubsystem<UAftershockStartupPreparationSubsystem>():nullptr;
    const bool StartupBlocking=Startup&&Startup->IsGameplayBlocked(World);
    const bool PauseWater=StartupBlocking||AftershockPhysicalLifecycle::LocksGranularBuildingInputs(EntryEpisode.bActive,EntryEpisode.Type);
    const double EntryRate=EntryEpisode.bActive&&IsWater(EntryEpisode.Type)?EntryEpisode.SimulationTimeScale:1.;
    double Delta=0.;FString ClockFailure;
    // Accrue caller-owned time exactly once BEFORE every initialize/topology
    // early return. Accepted immutable forcing stays in the async frontier.
    if(!AftershockPhysicalLifecycle::AccrueCallerTime(Now,PhysicalState.PhysicalSeconds,
        ClockFrontier.UnconsumedSeconds+R.RestoredAcceptedSeconds,PauseWater,EntryRate,
        R.LastWorldSeconds,R.CallerDebt,Delta,ClockFailure))
    {
        R.bStopped=true;R.LastFailure=ClockFailure;
        UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_PHYSICAL_CLOCK_STOPPED ownerTimeUnchanged=1 %s"),*ClockFailure);return;
    }
    InitializePhysicalMaterials();
    if(!R.bInitializing&&!R.bInitialized)
    {FText Error;if(!RebuildPhysicalWaterDomain(Error)){if(R.LastFailure!=Error.ToString()){R.LastFailure=Error.ToString();UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_HYDRAULIC_INITIALIZE %s"),*R.LastFailure);}return;}}
    const auto Poll=R.Water.Poll();
    if(Poll.Failed){R.bStopped=true;R.LastFailure=UTF8_TO_TCHAR(Poll.Error.c_str());UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_HYDRAULIC_STOPPED noTimeDropped=1 %s"),*R.LastFailure);return;}
    if(Poll.Published)PublishPhysicalWater();
    // A completed job becomes an idle frontier only after Poll. New-cell stock
    // initialization must get this boundary before the next forcing submission.
    InitializePhysicalMaterials();
    // Initial immutable-domain work and a previously accepted worker may publish.
    // Warmup elapsed time is paused via AccrueCallerTime above. No new accepted
    // source, replay, exposure or fiscal progression is submitted behind the UI.
    if(StartupBlocking)return;
    if(!R.bInitialized)return;
    auto& E=PhysicalState.Episode;
    if(R.RestoredAcceptedSeconds>0.)
    {
        // Do not sample a new tsunami stage, infiltration capacity or rain
        // phase while the original saved accepted quantum is still pending.
        const auto Waiting=R.Water.CaptureSaveFrontier();
        if(Waiting.WorkerInFlight||!Waiting.Unconsumed.empty())
        {R.bStopped=true;R.LastFailure=TEXT("Restored accepted forcing encountered another accepted job.");return;}
        // Keep the latched source intact if packet allocation/acceptance fails.
        auto PendingSources=R.RestoredAcceptedSources;
        std::string Why;auto Packet=AftershockWater::AsyncStepper::MakeForcingPacket(
            std::move(PendingSources),R.Water.DesiredGeneration(),++R.ForcingRevision,Why);
        if(!Packet||!R.Water.BeginAdvance(R.RestoredAcceptedSeconds,Packet,Why))
        {R.bStopped=true;R.LastFailure=UTF8_TO_TCHAR(Why.c_str());UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_HYDRAULIC_RESTORE_SOURCE_FAILED %s"),*R.LastFailure);return;}
        UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_HYDRAULIC_RESTORE_SOURCE acceptedSeconds=%.17g callerSeconds=%.17g stageControlsRestoredExactly=1 sourcePhaseResampled=0"),R.RestoredAcceptedSeconds,R.CallerDebt);
        R.Forcing=Packet;R.RestoredAcceptedSeconds=0.;R.RestoredAcceptedSources={};
    }
    // Refresh bounded risk weights only on canonical topology/material edits.
    if((R.WeightTerrainRevision!=T->GetTerrainRevision()||R.bRiskDirty)&&Now>=R.NextRiskRefreshWorldSeconds)
    {
        R.WeightTerrainRevision=T->GetTerrainRevision();R.bRiskDirty=false;R.NextRiskRefreshWorldSeconds=Now+1.;int32 Snow=0,Soil=0;
        const auto* Session=World->GetGameInstance()?World->GetGameInstance()->GetSubsystem<UAftershockOpeningSessionSubsystem>():nullptr;
        const bool God=Session&&Session->GetSettings().Mode==EAftershockPlayMode::Sandbox;
        for(const auto& C:T->Cells)
        {
            if(C.bIsOcean||C.bIsRiverbed||C.MaterialScenarioVersion!=1||C.OccupyingBuildingID!=INDEX_NONE)continue;
            const double Grade=Slope(*T,C),Theta=FMath::DegreesToRadians(Grade);
            const double SnowDepth=C.SnowWaterEquivalentMm/C.SnowBulkDensityKgM3;
            const double Driving=C.SnowBulkDensityKgM3*9.81*SnowDepth*FMath::Sin(Theta)*FMath::Cos(Theta);
            if(Grade>=30.&&Grade<=45.&&SnowDepth>=.15&&(God||Driving>C.SnowWeakLayerStrengthPa))++Snow;
            const double Saturation=C.RegolithDepthM>0.?C.SoilPoreWaterDepthM/(C.SoilPorosity01*C.RegolithDepthM):0.;
            const double Density=C.SoilDryBulkDensityKgM3+(C.RegolithDepthM>0.?1000.*C.SoilPoreWaterDepthM/C.RegolithDepthM:0.);
            const double FS=FAftershockMassMovementSolver::ShallowSoilFactorOfSafety(Theta,C.RegolithDepthM,Density,C.SoilCohesionPa,FMath::DegreesToRadians(C.SoilFrictionAngleDegrees),Saturation);
            if(Grade>=5.&&C.RegolithDepthM>=.05&&(God||(FS>=0.&&FS<1.)))++Soil;
        }
        R.SnowWeight=Snow>=16?FMath::Min(1.f,Snow/128.f):0.f;R.SoilWeight=Soil>=16?FMath::Min(1.f,Soil/128.f):0.f;
    }
    if(E.bActive&&IsMass(E.Type)&&R.bMassAwaitingBoundary)
    {
        const auto Boundary=R.Water.CaptureSaveFrontier();
        if(!Boundary.WorkerInFlight&&Boundary.Unconsumed.empty()&&R.RestoredAcceptedSeconds==0.&&R.CallerDebt<=1.e-12)
        {
            FText Error;if(!StagePhysicalMassJob(Error))
            {
                FDisasterReport Failed;Failed.bWasRejected=true;Failed.DisasterEventID=E.EventID;Failed.DisasterType=E.Type;Failed.RejectionReason=Error;
                E=FAftershockPhysicalEpisodeSaveState();R.bMassAwaitingBoundary=false;R.MassExpansion.Reset();R.bMassSupportExpanded=false;R.bMassSupportPrepared=false;OnDisasterCompleted.Broadcast(Failed);return;
            }
        }
    }
    if(R.bMassWaiting&&R.MassJob.IsReady())
    {
        auto Result=R.MassJob.Consume();R.bMassWaiting=false;
        const auto* B=World->GetSubsystem<UBuildingManagerSubsystem>();
        auto Reject=[&](const FString& Why)
        {
            FDisasterReport Report;Report.bWasRejected=true;Report.DisasterEventID=E.EventID;Report.DisasterType=E.Type;Report.RejectionReason=Failure(Why);
            E=FAftershockPhysicalEpisodeSaveState();R.MassSource.Reset();R.MassReplay.Reset();OnDisasterCompleted.Broadcast(Report);
            UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_MASS_REJECTED canonicalStocksChanged=0 %s"),*Why);
        };
        if(!Result.bAccepted||!Result.bComplete||!Result.bMassConserved||!Result.bEnergyClosed||!Result.bFrameExportComplete)
        {
            if(FParse::Param(FCommandLine::Get(),TEXT("AftershockMassDiagnostics")))
            {
                const auto* Snapshot=R.MassSource.Get();
                UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_MASS_REJECTED_RESULT event=%s hazard=%d sourceRevision=%llu currentRevision=%llu inputCells=%d releaseCells=%d initialParcels=%d frameCount=%d finalFrameParcels=%d supportMarginCells=%d accepted=%d complete=%d massConserved=%d energyClosed=%d frameExportComplete=%d simulatedSeconds=%.17g peakSpeedMps=%.17g movingKg=%.17g movingKineticJ=%.17g releasedKg=%.17g depositedKg=%.17g substeps=%d parcelSteps=%lld workUnits=%lld computeMs=%.3f sourceStockWithdrawal=0"),
                    *E.EventID.ToString(),static_cast<int32>(E.Type),Result.SourceTerrainRevision,T->GetTerrainRevision(),Snapshot?Snapshot->Input.Cells.Num():0,
                    Snapshot?Snapshot->Input.Release.Num():0,Result.Budget.InitialParcels,Result.Frames.Num(),Result.Frames.IsEmpty()?0:Result.Frames.Last().Parcels.Num(),
                    Snapshot?Snapshot->MinimumSelectedSupportCells:0,Result.bAccepted,Result.bComplete,Result.bMassConserved,Result.bEnergyClosed,Result.bFrameExportComplete,
                    Result.SimulatedSeconds,Result.Budget.MaximumSpeedMps,Result.Budget.MovingKg,Result.Budget.MovingKineticJ,Result.Budget.ReleasedKg,Result.Budget.DepositedKg,
                    Result.Budget.Substeps,Result.Budget.ParticleSteps,Result.Budget.WorkUnits,Result.Budget.ComputeMilliseconds);
            }
            Reject(Result.Failure.IsEmpty()?Result.FrameExportFailure:Result.Failure);return;
        }
        if(Result.SourceTerrainRevision!=T->GetTerrainRevision()||!B||BuildingSignature(*B)!=R.MassBuildingSignature)
        {Reject(TEXT("Terrain/buildings changed while the pure mass solve ran; stale result was not applied."));return;}
        const auto Water=AftershockMassTerrain::StageSourceWaterWithdrawals(*R.MassSource,Result);
        if(!Water.bValid){Reject(Water.Failure);return;}
        auto Replay=MakeShared<FAftershockMassVisualEvent>();Replay->EventID=E.EventID;Replay->StartedAtWorldSeconds=Now;
        Replay->CellSizeM=R.MassSource->Input.CellSizeM;Replay->GridOriginM=R.MassSource->Input.GridOriginM;
        Replay->SupportCells=R.MassSource->Input.Cells;Replay->Obstacles=R.MassSource->Input.Obstacles;Replay->Result=MoveTemp(Result);
        FText Error;if(!EncodeAftershockMassReplay(*Replay,E.MassReplay,Error)){Reject(Error.ToString());return;}
        R.MassActor=World->SpawnActor<AAftershockMassMovementVisualActor>();FString Why;
        if(R.MassActor.IsValid())R.MassActor->SetExternallyClocked();
        if(!R.MassActor.IsValid()||!R.MassActor->BeginAcceptedEvent(*Replay,Why)){Reject(Why);return;}
        const double Area=FMath::Square(T->CellSize*.01);
        for(const auto& C:Replay->Result.Cells)if(C.RemovedVolumeM3>0.)
        {
            const int32 I=T->CoordToIndex(C.Coord);if(!T->Cells.IsValidIndex(I))continue;auto& Cell=T->Cells[I];
            if(E.Type==EDisasterType::Avalanche)Cell.SnowWaterEquivalentMm=FMath::Max(0.,Cell.SnowWaterEquivalentMm-(C.ReleasedMassKg+C.EntrainedMassKg)/Area);
            else {Cell.RegolithDepthM=FMath::Max(0.,Cell.RegolithDepthM-C.RemovedVolumeM3/Area);Cell.GroundErodedDepthM+=C.RemovedVolumeM3/Area;}
        }
        if(E.Type==EDisasterType::Landslide)for(const auto& W:Water.Sources)
        {const int32 I=T->CoordToIndex(W.Coord);if(T->Cells.IsValidIndex(I))T->Cells[I].SoilPoreWaterDepthM=W.RemainingSourceWaterM3/Area;}
        E.ReleasedMixtureWaterM3=Water.ReleasedWaterM3+Water.EntrainedWaterM3;E.SourceDurationSeconds=Replay->Result.SimulatedSeconds;
        R.MassReplay=Replay;R.MassSource.Reset();T->MarkTerrainEdited();
        UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MASS_STARTED event=%s seconds=%.3f releasedKg=%.3f massResidualKg=%.9f energyResidualJ=%.9f workerMs=%.3f"),
            *E.EventID.ToString(),E.SourceDurationSeconds,Replay->Result.Budget.ReleasedKg,Replay->Result.Budget.MassResidualKg,Replay->Result.Budget.EnergyResidualJ,Replay->Result.Budget.ComputeMilliseconds);
    }
    if(E.bActive&&IsMass(E.Type)&&!R.bMassWaiting&&R.MassReplay)
    {
        E.PlaybackSeconds=FMath::Min(E.SourceDurationSeconds,E.PlaybackSeconds+Delta);E.SourceElapsedSeconds=E.PlaybackSeconds;
        if(R.MassActor.IsValid())R.MassActor->SetPlaybackWorldSeconds(R.MassReplay->StartedAtWorldSeconds+E.PlaybackSeconds);
        const FAftershockMassFrame* Frame=nullptr;for(const auto& F:R.MassReplay->Result.Frames)if(F.Seconds<=E.PlaybackSeconds)Frame=&F;
        bool Edited=false;const double Area=FMath::Square(T->CellSize*.01);
        const double TotalMass=R.MassReplay->Result.Budget.ReleasedKg+R.MassReplay->Result.Budget.EntrainedKg;
        const double WaterPerKg=TotalMass>0.?E.ReleasedMixtureWaterM3/TotalMass:0.;
        if(Frame)for(const auto& C:Frame->Cells)
        {
            const double Prior=R.AppliedDepositM3.FindRef(C.Coord),Added=FMath::Max(0.,C.DepositedVolumeM3-Prior);
            if(Added<=0.)continue;const int32 I=T->CoordToIndex(C.Coord);if(!T->Cells.IsValidIndex(I))continue;
            auto& Cell=T->Cells[I];
            if(E.Type==EDisasterType::Avalanche)
            {const double WaterKg=Added*(C.DepositedVolumeM3>0.?C.DepositedMassKg/C.DepositedVolumeM3:300.);Cell.SnowWaterEquivalentMm+=WaterKg/Area;E.TransferredMixtureWaterM3+=WaterKg*.001;}
            else
            {
                const double WaterM3=Added*(C.DepositedVolumeM3>0.?C.DepositedMassKg/C.DepositedVolumeM3:1800.)*WaterPerKg;
                const double PreviousVolume=Cell.RegolithDepthM*Area,PreviousDryKg=PreviousVolume*Cell.SoilDryBulkDensityKgM3;
                const double AddedMixtureKg=Added*(C.DepositedVolumeM3>0.?C.DepositedMassKg/C.DepositedVolumeM3:1800.);
                const double AddedDryKg=FMath::Max(0.,AddedMixtureKg-WaterM3*1000.);
                Cell.GroundDepositedDepthM+=Added/Area;Cell.RegolithDepthM+=Added/Area;
                Cell.SoilDryBulkDensityKgM3=(PreviousDryKg+AddedDryKg)/(PreviousVolume+Added);
                // Finite porosity: displaced/excess pore water returns to the
                // surface as an explicitly labelled source, never disappears.
                const double Capacity=FMath::Max(0.,Cell.RegolithDepthM*Cell.SoilPorosity01-Cell.SoilPoreWaterDepthM)*Area;
                const double Stored=FMath::Min(WaterM3,Capacity);Cell.SoilPoreWaterDepthM+=Stored/Area;
                const double Spill=WaterM3-Stored;
                if(Spill>0.)if(const int32* WI=R.WaterIndex.Find(C.Coord))
                {PhysicalState.WaterCells[*WI].VolumeM3+=Spill;PhysicalState.WaterBudget.ExternalInflowM3+=Spill;PhysicalState.WaterBudget.FinalSurfaceM3+=Spill;}
                E.TransferredMixtureWaterM3+=WaterM3;
            }
            R.AppliedDepositM3.Add(C.Coord,C.DepositedVolumeM3);Edited=true;
        }
        // Canonical inventory is saved at this prefix; the one topology mesh
        // rebuild is coalesced at motion completion, not one collision cook/frame.
        ApplyPhysicalExposure();
        if(E.PlaybackSeconds>=E.SourceDurationSeconds){if(R.MassActor.IsValid())R.MassActor->FinishMotionKeepDeposit();T->MarkTerrainEdited();CompletePhysicalEpisode();}
    }
    if(E.bActive&&IsWater(E.Type))ApplyPhysicalExposure();
    const auto Debt=R.Water.CaptureSaveFrontier();
    if(!(E.bActive&&IsMass(E.Type))&&!Debt.WorkerInFlight&&Debt.UnconsumedSeconds<=1.e-8&&!R.bInitializing&&R.TerrainRevision!=T->GetTerrainRevision())
    {FText Error;if(!RebuildPhysicalWaterDomain(Error))R.LastFailure=Error.ToString();return;}
    // Caller time was accrued at callback entry, including initialization and
    // topology waits. A mass completion cannot charge its same paused interval
    // a second time; ordinary water resumes on the next callback.
    if(!R.bInitializing&&!Debt.WorkerInFlight&&Debt.Unconsumed.empty()&&R.CallerDebt>1.e-12)
    {
        double Seconds=FMath::Min(4.,R.CallerDebt);
        AftershockPhysicalLifecycle::ESourceControlPhase SourcePhase=AftershockPhysicalLifecycle::ESourceControlPhase::Ended;
        double BoundaryGap=0.,ControlElapsed=0.,ControlTolerance=0.;FString PhaseFailure;
        // Only NEW controls use this comparison. An accepted prefix, including
        // a restored tsunami head, is never rebuilt or sampled at another phase.
        if(E.bActive&&!AftershockPhysicalLifecycle::ClassifySourceControls(PhysicalState.PhysicalSeconds,
            E.StartPhysicalSeconds,E.SourceDurationSeconds,SourcePhase,BoundaryGap,ControlElapsed,ControlTolerance,PhaseFailure))
        {
            R.bStopped=true;R.LastFailure=PhaseFailure;
            UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_PHYSICAL_PHASE_STOPPED clockAndDebtUnchanged=1 %s"),*PhaseFailure);return;
        }
        const bool SourceActive=E.bActive&&SourcePhase==AftershockPhysicalLifecycle::ESourceControlPhase::Active;
        if(SourcePhase==AftershockPhysicalLifecycle::ESourceControlPhase::BeforeStart||(SourceActive&&IsWater(E.Type)))
            Seconds=FMath::Min(Seconds,BoundaryGap);
        const auto Front=R.Water.LastPublished();
        AftershockWater::Forcing Sources;Sources.Cells.resize(Front->Cells.size());Sources.Boundaries.resize(Front->Geometry->Faces.size());
        for(int32 I=0;I<static_cast<int32>(Front->Cells.size());++I)
        {
            const auto& C=Front->Cells[I];const int32 Index=T->CoordToIndex(FIntPoint(C.X,C.Y));if(!T->Cells.IsValidIndex(Index))continue;
            const auto& Cell=T->Cells[Index];auto& S=Sources.Cells[I];
            if(!Cell.bIsOcean)
            {
                S.RainMPerS=SourceActive&&E.Type==EDisasterType::Flood?E.RainMPerSecond:0.;
                const double RemainingPore=FMath::Max(0.,Cell.RegolithDepthM*Cell.SoilPorosity01-Cell.SoilPoreWaterDepthM);
                S.InfiltrationCapacityMPerS=FMath::Min(RemainingPore/FMath::Max(1.,Seconds),5./3600000.);
                S.ExternalDrainCapacityM3PerS=Cell.bIsRiverbed?0.:FMath::Clamp(static_cast<double>(Cell.DrainageQuality),0.,1.)*.01;
            }
        }
        const double Wave=SourceActive&&E.Type==EDisasterType::Tsunami&&E.SourceDurationSeconds>0.
            ?FMath::Square(FMath::Sin(UE_DOUBLE_PI*ControlElapsed/E.SourceDurationSeconds))*(.5+.055*E.BaseSeverity):0.;
        if(Wave>0.&&R.CoastDirectionRevision!=T->GetTerrainRevision())
        {R.CoastInlandDirection=T->GetCoastInlandDirection().GetSafeNormal2D();R.CoastDirectionRevision=T->GetTerrainRevision();}
        const FVector Inland=R.CoastInlandDirection;
        for(int32 I=0;I<static_cast<int32>(Front->Geometry->Faces.size());++I)
        {
            const auto& F=Front->Geometry->Faces[I];auto& B=Sources.Boundaries[I];B.HeadM=F.StageHeadM;B.VelocityXMps=F.StageVelocityXMps;B.VelocityYMps=F.StageVelocityYMps;
            if(F.Kind==AftershockWater::FaceKind::StageBoundary)
            {
                const auto& C=Front->Cells[F.Left];FGridCell Cell;
                // A coastal incoming wave enters from the offshore sector;
                // lateral/back sea edges retain their ambient stage. The
                // canonical inland vector is geographic, never camera-facing.
                if(F.NormalX*Inland.X+F.NormalY*Inland.Y<-.5
                    &&T->TryGetCell(FIntPoint(C.X,C.Y),Cell)&&Cell.bIsOcean)B.HeadM+=Wave;
            }
        }
        std::string Why;auto Packet=AftershockWater::AsyncStepper::MakeForcingPacket(std::move(Sources),R.Water.DesiredGeneration(),++R.ForcingRevision,Why);
        if(Packet&&R.Water.BeginAdvance(Seconds,Packet,Why)){R.CallerDebt-=Seconds;R.Forcing=Packet;}
        else if(!Why.empty())
        {
            const FString Reason=UTF8_TO_TCHAR(Why.c_str());
            const FString FailureKey=FString::Printf(TEXT("advancePhase=%d: %s"),static_cast<int32>(SourcePhase),*Reason);
            if(R.LastFailure!=FailureKey)
            {
                R.LastFailure=FailureKey;
                UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_HYDRAULIC_BACKPRESSURE attemptSeconds=%.17g retainedCallerSeconds=%.17g physicalSeconds=%.17g phaseStart=%.17g phaseElapsed=%.17g boundaryGap=%.17g phaseTolerance=%.17g phase=%d %s"),
                    Seconds,R.CallerDebt,PhysicalState.PhysicalSeconds,E.StartPhysicalSeconds,ControlElapsed,BoundaryGap,ControlTolerance,static_cast<int32>(SourcePhase),*Reason);
            }
        }
    }
    else if(!Debt.WorkerInFlight&&Debt.UnconsumedSeconds>1.e-8){std::string Why;R.Water.RetryPending(Why);}
    if(E.bActive&&IsWater(E.Type)&&E.PlaybackSeconds>=E.SourceDurationSeconds+120.)CompletePhysicalEpisode();
}

void UDisasterSimulationSubsystem::ApplyPhysicalExposure()
{
    using namespace AftershockPhysicalDetail;
    if(!PhysicalRuntime||!PhysicalState.Episode.bActive)return;
    auto& R=*PhysicalRuntime;auto& E=PhysicalState.Episode;
    auto* B=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();auto* T=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
    if(!B||!T)return;
    TMap<int32,int32> ByID;for(int32 I=0;I<E.Exposures.Num();++I)ByID.Add(E.Exposures[I].BuildingID,I);
    TMap<int32,const FAftershockMassImpact*> Contacts;
    const FAftershockMassFrame* Frame=nullptr;
    if(IsMass(E.Type)&&R.MassReplay)
    {for(const auto& F:R.MassReplay->Result.Frames)if(F.Seconds<=E.PlaybackSeconds)Frame=&F;if(Frame)for(const auto& I:Frame->Impacts)Contacts.Add(I.BuildingID,&I);}
    const double WetStep=FMath::Max(0.,PhysicalState.PhysicalSeconds-R.LastExposureSeconds);R.LastExposureSeconds=PhysicalState.PhysicalSeconds;
    TSet<int32> ExposedIDs(E.ExposedResidentIDs);
    TMap<int32,TArray<int32>> Occupants;
    for(const auto& Person:E.BeforeResidents)if(Person.bAlive)
        Occupants.FindOrAdd(E.bDaytime&&Person.WorkBuildingID!=INDEX_NONE?Person.WorkBuildingID:Person.HomeBuildingID).Add(Person.ResidentID);
    for(const auto& Baseline:E.BeforeBuildings)
    {
        if(Baseline.State.DamageState>=EDamageState::Collapsed)continue;
        FBuildingInstance Current;if(!B->GetBuildingByID(Baseline.State.InstanceID,Current)||!Current.BuildingData||Current.DamageState==EDamageState::Memorial)continue;
        double Depth=0.,Speed=0.,Pressure=0.,Impulse=0.;
        if(IsWater(E.Type))
        {
            double Head=0.,Column=0.;FVector2D V;
            if(!TryGetPhysicalWaterSample(Current.GridCoords,Head,Column,V))continue;
            Depth=FMath::Max(0.,(Head-T->GetCell(Current.GridCoords).Elevation)*.01);Speed=V.Size()*.01;
            if(Depth<.03)continue;
            Pressure=1000.*9.80665*Depth*.5+500.*Speed*Speed;
        }
        else
        {
            const auto* Found=Contacts.Find(Current.InstanceID);if(!Found)continue;
            const auto& Contact=**Found;Depth=Contact.PeakFlowDepthM;Pressure=Contact.PeakNormalPressurePa;Impulse=Contact.ImpulseMagnitudeNs;
            if(Contact.FirstContactSeconds<0.||Contact.FirstContactSeconds>E.PlaybackSeconds)continue;
        }
        int32* Existing=ByID.Find(Current.InstanceID);int32 Index;
        if(Existing)Index=*Existing;else{Index=E.Exposures.AddDefaulted();E.Exposures[Index].BuildingID=Current.InstanceID;ByID.Add(Current.InstanceID,Index);}
        auto& Exposure=E.Exposures[Index];Exposure.PeakDepthM=FMath::Max(Exposure.PeakDepthM,Depth);Exposure.PeakSpeedMps=FMath::Max(Exposure.PeakSpeedMps,Speed);
        Exposure.PeakPressurePa=FMath::Max(Exposure.PeakPressurePa,Pressure);Exposure.ImpulseNs=FMath::Max(Exposure.ImpulseNs,Impulse);
        if(IsWater(E.Type))Exposure.WetSeconds+=WetStep;
        // Authored structural fragility, explicitly distinct from the numerical
        // flow model. Pressures are Pa, strength proxies are Pa; no damage/frame.
        double Capacity=30000.;
        switch(Current.BuildingData->Material)
        {case EMaterialType::Wood:Capacity=15000.;break;case EMaterialType::Prefab:Capacity=20000.;break;
         case EMaterialType::Concrete:Capacity=60000.;break;case EMaterialType::Steel:Capacity=80000.;break;default:break;}
        Capacity*=Current.StructuralQuality*(Current.Foundation==EFoundationQuality::Strong?1.25:Current.Foundation==EFoundationQuality::Weak?.65:1.);
        Capacity*=Current.bIsRetrofitted?1.35:1.;Capacity/=1.+.03*FMath::Max(0,Current.CurrentFloors-1);
        const double ExposureFactor=IsWater(E.Type)?FMath::Min(1.,.25+Exposure.WetSeconds/1200.):1.;
        const float Demand=FMath::Clamp(100.*(1.-FMath::Exp(-Exposure.PeakPressurePa/FMath::Max(1.,Capacity)))*ExposureFactor,0.,100.);
        const float Target=FMath::Clamp(Baseline.State.CurrentDamage+Demand,0.f,100.f);
        Exposure.AppliedDamageDemand=FMath::Max(Exposure.AppliedDamageDemand,Demand);
        if(Target>Current.CurrentDamage+.01f)
        {
            const bool WasCollapsed=Current.DamageState==EDamageState::Collapsed;
            B->ApplyDamageToBuilding(Current.InstanceID,Target);B->GetBuildingByID(Current.InstanceID,Current);
            if(!WasCollapsed&&Current.DamageState==EDamageState::Collapsed&&!Exposure.bCollapseCounted)
            {Exposure.bCollapseCounted=true;++E.CollapsedBuildings;SpawnChaosDestructionForBuilding(Current);}
        }
        const auto* At=Occupants.Find(Current.InstanceID);const int32 OccupantCount=At?At->Num():0;
        if(At)for(int32 ID:*At)ExposedIDs.Add(ID);
        const double Fatality=Current.DamageState==EDamageState::Collapsed?.6:Current.DamageState==EDamageState::Severe?.3:Current.DamageState==EDamageState::Damaged?.1:.02;
        const double Prepared=FMath::Clamp(static_cast<double>(E.CityAwareness01+Current.Preparedness*.5f),0.,1.);
        Exposure.AssignedDeaths=FMath::Max(Exposure.AssignedDeaths,FMath::Clamp(FMath::RoundToInt(OccupantCount*Fatality*(1.-Prepared)*Current.CurrentDamage*.005),0,OccupantCount));
    }
    E.ExposedResidentIDs=ExposedIDs.Array();E.ExposedResidentIDs.Sort();
}

void UDisasterSimulationSubsystem::CompletePhysicalEpisode()
{
    if(!PhysicalRuntime||!PhysicalState.Episode.bActive||PhysicalState.Episode.bOutcomeCommitted)return;
    auto& E=PhysicalState.Episode;auto* World=GetWorld();auto* Life=World->GetSubsystem<UCityLifeSubsystem>();auto* B=World->GetSubsystem<UBuildingManagerSubsystem>();
    auto* Ledger=World->GetSubsystem<UCasualtyLedgerGenerator>();auto* Economy=World->GetSubsystem<UEconomyAndStateSubsystem>();
    if(!Life||!B||!Ledger||!Economy)return;
    FDisasterReport Report;Report.DisasterEventID=E.EventID;Report.DisasterType=E.Type;Report.BaseSeverity=E.BaseSeverity;
    // This non-yielding GT transaction is the ONLY resident/ledger/fiscal
    // settlement. Progressive exposure above never changes living population.
    // RecordDisaster validates against the proposed post-casualty Life state;
    // retain its complete prior owner state so rejection cannot leave deaths.
    FAftershockCityLifeSaveState PriorLife;Life->CaptureSaveState(PriorLife);
    FAftershockScopedOccupancyBatch OccupancyBatch(Life);
    for(const auto& Exposure:E.Exposures)
    {
        const auto Deaths=Life->ApplyCasualties(Exposure.BuildingID,Exposure.AssignedDeaths,E.bDaytime);Report.ActualDeathResidentIDs.Append(Deaths);
        Report.TotalBuildingsAffected+=Exposure.AppliedDamageDemand>0.f?1:0;
    }
    Report.ActualDeathResidentIDs.Sort();
    for(int32 ID:E.ExposedResidentIDs){FCityResidentRecord Person;if(Life->GetResident(ID,Person)&&Person.bAlive)Report.ExposedSurvivorResidentIDs.Add(ID);}
    Report.ExposedSurvivorResidentIDs.Sort();Report.TotalDeaths=Report.ActualDeathResidentIDs.Num();Report.TotalSurvivors=Report.ExposedSurvivorResidentIDs.Num();
    Report.TotalDeathCostTL=static_cast<int64>(Report.TotalDeaths)*500;Report.TotalStateAidGranted=static_cast<int64>(Report.TotalSurvivors)*150;
    Report.TotalSatisfactionChange=-Report.TotalDeaths*.3f;Report.TotalBuildingsCollapsed=E.CollapsedBuildings;
    TArray<FBuildingInstance> Before;Before.Reserve(E.BeforeBuildings.Num());
    for(const auto& Saved:E.BeforeBuildings){auto State=Saved.State;State.BuildingData=B->GetCatalogData(Saved.DefinitionType);Before.Add(State);}
    const bool Recorded=Ledger->RecordDisaster(E.EventID,E.Type,Economy->CurrentTurn,E.bDaytime,E.BeforeResidents,Before,Report.ActualDeathResidentIDs,Report.ExposedSurvivorResidentIDs,false,E.BaseSeverity);
    if(!Recorded)
    {
        // A violated invariant is visible and blocks saving; never conceal a
        // ledger failure behind another event ID or a second fiscal payment.
        FText RollbackError;const bool Restored=Life->RestoreSaveState(PriorLife,RollbackError);
        PhysicalRuntime->bStopped=true;
        UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_PHYSICAL_SETTLEMENT_FAILED event=%s fatalitiesRolledBack=%d fiscalApplied=0 rollbackError=%s"),
            *E.EventID.ToString(),Restored,*RollbackError.ToString());return;
    }
    E.ActualDeathResidentIDs=Report.ActualDeathResidentIDs;E.bActive=false;E.bOutcomeCommitted=true;
    Life->RefreshCity();OnDisasterCompleted.Broadcast(Report);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_COMPLETED event=%s type=%d exposed=%d deaths=%d collapsed=%d aidTL=%lld costTL=%lld actualPhysicalSeconds=%.6f"),
        *E.EventID.ToString(),static_cast<int32>(E.Type),E.ExposedResidentIDs.Num(),Report.TotalDeaths,E.CollapsedBuildings,Report.TotalStateAidGranted,Report.TotalDeathCostTL,PhysicalState.PhysicalSeconds);
}
