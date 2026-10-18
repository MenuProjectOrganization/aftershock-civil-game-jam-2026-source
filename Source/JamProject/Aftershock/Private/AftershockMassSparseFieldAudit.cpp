#include "AftershockMassSparseFieldAudit.h"
#if WITH_EDITOR
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"

namespace AftershockSparseMassAuditPrivate
{
bool Same(double A,double B){return FMemory::Memcmp(&A,&B,sizeof(double))==0;}
bool Same(const FVector& A,const FVector& B){return Same(A.X,B.X)&&Same(A.Y,B.Y)&&Same(A.Z,B.Z);}
bool SameImpact(const FAftershockMassImpact& A,const FAftershockMassImpact& B)
{
    return A.BuildingID==B.BuildingID&&A.Contacts==B.Contacts&&Same(A.PeakNormalPressurePa,B.PeakNormalPressurePa)
        &&Same(A.ImpulseMagnitudeNs,B.ImpulseMagnitudeNs)&&Same(A.PeakFlowDepthM,B.PeakFlowDepthM)
        &&Same(A.FirstContactSeconds,B.FirstContactSeconds)&&Same(A.PeakPressureSeconds,B.PeakPressureSeconds);
}
bool ExactOutputs(const FAftershockMassResult& A,const FAftershockMassResult& B)
{
    if(A.bAccepted!=B.bAccepted||A.bComplete!=B.bComplete||A.bMassConserved!=B.bMassConserved||A.bEnergyClosed!=B.bEnergyClosed
        ||A.bFrameExportComplete!=B.bFrameExportComplete||A.ExportedFrameCellRecords!=B.ExportedFrameCellRecords
        ||A.ExportedFrameParcelRecords!=B.ExportedFrameParcelRecords||A.ExportedFrameImpactRecords!=B.ExportedFrameImpactRecords
        ||A.FrameExportFailure!=B.FrameExportFailure||A.bFrozenBedDuringEvent!=B.bFrozenBedDuringEvent
        ||A.bLateralGranularPressureModeled!=B.bLateralGranularPressureModeled||A.bOvertoppingModeled!=B.bOvertoppingModeled
        ||A.Material!=B.Material||A.HazardTypeTag!=B.HazardTypeTag||A.SourceTerrainRevision!=B.SourceTerrainRevision
        ||!Same(A.SimulatedSeconds,B.SimulatedSeconds)||A.Failure!=B.Failure
        ||A.Cells.Num()!=B.Cells.Num()||A.Impacts.Num()!=B.Impacts.Num()||A.Frames.Num()!=B.Frames.Num())return false;
    const auto& X=A.Budget;const auto& Y=B.Budget;
    // WorkUnits intentionally changes with the implementation. Compute time is
    // nondeterministic. ALL physical ledger/counter outputs below are exact.
    const double AX[]={X.ReleasedKg,X.EntrainedKg,X.MovingKg,X.DepositedKg,X.ExportedKg,X.MassResidualKg,
        X.InitialKineticJ,X.GravityWorkJ,X.DissipatedJ,X.MovingKineticJ,X.ExportedKineticJ,X.EnergyResidualJ,X.MaximumSpeedMps,X.MaximumDisplacementCells};
    const double BY[]={Y.ReleasedKg,Y.EntrainedKg,Y.MovingKg,Y.DepositedKg,Y.ExportedKg,Y.MassResidualKg,
        Y.InitialKineticJ,Y.GravityWorkJ,Y.DissipatedJ,Y.MovingKineticJ,Y.ExportedKineticJ,Y.EnergyResidualJ,Y.MaximumSpeedMps,Y.MaximumDisplacementCells};
    for(int I=0;I<UE_ARRAY_COUNT(AX);++I)if(!Same(AX[I],BY[I]))return false;
    if(X.Substeps!=Y.Substeps||X.InitialParcels!=Y.InitialParcels||X.PeakActiveParcels!=Y.PeakActiveParcels
        ||X.MaterialCallbackCalls!=Y.MaterialCallbackCalls||X.FoundationCallbackCalls!=Y.FoundationCallbackCalls
        ||X.ParticleSteps!=Y.ParticleSteps||X.SurfaceSamples!=Y.SurfaceSamples||X.ObstacleTests!=Y.ObstacleTests)return false;
    for(int I=0;I<A.Cells.Num();++I)
    {
        const auto& C=A.Cells[I];const auto& D=B.Cells[I];if(C.Coord!=D.Coord||!Same(C.PeakVelocityMps,D.PeakVelocityMps))return false;
        const double CValues[]={C.RunoutBedElevationM,C.RemainingSourceMaterialKg,C.ReleasedMassKg,C.EntrainedMassKg,C.RemovedVolumeM3,
            C.DepositedMassKg,C.DepositedVolumeM3,C.FinalMovingMassKg,C.FirstArrivalSeconds,C.PeakFlowDepthM,C.PeakSpeedMps,C.PeakDynamicPressurePa};
        const double DValues[]={D.RunoutBedElevationM,D.RemainingSourceMaterialKg,D.ReleasedMassKg,D.EntrainedMassKg,D.RemovedVolumeM3,
            D.DepositedMassKg,D.DepositedVolumeM3,D.FinalMovingMassKg,D.FirstArrivalSeconds,D.PeakFlowDepthM,D.PeakSpeedMps,D.PeakDynamicPressurePa};
        for(int K=0;K<UE_ARRAY_COUNT(CValues);++K)if(!Same(CValues[K],DValues[K]))return false;
    }
    for(int I=0;I<A.Impacts.Num();++I)if(!SameImpact(A.Impacts[I],B.Impacts[I]))return false;
    for(int I=0;I<A.Frames.Num();++I)
    {
        const auto& F=A.Frames[I];const auto& G=B.Frames[I];
        if(!Same(F.Seconds,G.Seconds)||F.Parcels.Num()!=G.Parcels.Num()||F.Cells.Num()!=G.Cells.Num()||F.Impacts.Num()!=G.Impacts.Num())return false;
        for(int K=0;K<F.Parcels.Num();++K)
        {
            const auto& P=F.Parcels[K];const auto& Q=G.Parcels[K];
            if(P.ParcelID!=Q.ParcelID||!Same(P.PositionM,Q.PositionM)||!Same(P.VelocityMps,Q.VelocityMps)
                ||!Same(P.MassKg,Q.MassKg)||!Same(P.VolumeM3,Q.VolumeM3)||!Same(P.FootprintAreaM2,Q.FootprintAreaM2))return false;
        }
        for(int K=0;K<F.Cells.Num();++K)
        {
            const auto& C=F.Cells[K];const auto& D=G.Cells[K];
            if(C.Coord!=D.Coord||!Same(C.MovingMassKg,D.MovingMassKg)||!Same(C.MovingVolumeM3,D.MovingVolumeM3)
                ||!Same(C.FlowDepthM,D.FlowDepthM)||!Same(C.DepositedMassKg,D.DepositedMassKg)||!Same(C.DepositedVolumeM3,D.DepositedVolumeM3))return false;
        }
        for(int K=0;K<F.Impacts.Num();++K)if(!SameImpact(F.Impacts[K],G.Impacts[K]))return false;
    }
    return true;
}
FAftershockMassInput Fixture(EAftershockMassMaterial Material=EAftershockMassMaterial::Snow,bool Flat=false)
{
    FAftershockMassInput I;I.Material=Material;I.HazardTypeTag=Material==EAftershockMassMaterial::Snow?7:8;
    I.TerrainRevision=123;I.CellSizeM=10.;I.Settings.MaximumSeconds=60.;
    const double Gradient=Flat?0.:FMath::Tan(FMath::DegreesToRadians(35.));
    for(int Y=0;Y<24;++Y)for(int X=0;X<20;++X)
    {
        FAftershockMassCell C;C.Coord=FIntPoint(X,Y);C.GroundElevationM=-FMath::Min(Y,12)*10.*Gradient;
        C.SnowWaterEquivalentMm=Material==EAftershockMassMaterial::Snow?180.:0.;C.SnowDensityKgM3=300.;C.SnowWeakLayerStrengthPa=50.;
        C.RegolithVerticalDepthM=1.;C.SoilDensityKgM3=1800.;C.CohesionPa=50.;C.FrictionAngleDegrees=25.;C.EffectiveSaturation01=1.;I.Cells.Add(C);
    }
    FAftershockMassRelease R;R.Coord=FIntPoint(8,4);R.AvailableMassFraction=.5;R.bForceRelease=Flat;
    if(Flat)R.InitialVelocityMps=FVector(0.,4.,0.);I.Release.Add(R);return I;
}
TSharedRef<FJsonObject> WorkJSON(const FAftershockMassFieldWorkAudit& W,const FAftershockMassResult& R)
{
    auto J=MakeShared<FJsonObject>();
    J->SetNumberField(TEXT("scratch_initialized_cells"),static_cast<double>(W.ScratchInitializedCells));
    J->SetNumberField(TEXT("record_calls"),static_cast<double>(W.RecordCalls));
    J->SetNumberField(TEXT("sparse_cleared_indices"),static_cast<double>(W.SparseClearedIndices));
    J->SetNumberField(TEXT("sparse_scattered_indices"),static_cast<double>(W.SparseScatteredIndices));
    J->SetNumberField(TEXT("sparse_updated_indices"),static_cast<double>(W.SparseUpdatedIndices));
    J->SetNumberField(TEXT("sparse_sort_comparisons"),static_cast<double>(W.SparseSortComparisons));
    J->SetNumberField(TEXT("sparse_sort_reserved_units"),static_cast<double>(W.SparseSortReservedUnits));
    J->SetNumberField(TEXT("dense_reset_scan_units"),static_cast<double>(W.DenseResetAndScanUnits));
    J->SetNumberField(TEXT("replay_frame_calls"),static_cast<double>(W.ReplayFrameCalls));
    J->SetNumberField(TEXT("replay_cell_scan_indices"),static_cast<double>(W.ReplayCellScanIndices));
    J->SetNumberField(TEXT("peak_touched_indices"),W.PeakTouchedIndices);
    J->SetNumberField(TEXT("numerical_work_units"),static_cast<double>(R.Budget.WorkUnits));
    J->SetNumberField(TEXT("simulated_seconds"),R.SimulatedSeconds);J->SetNumberField(TEXT("moving_kg"),R.Budget.MovingKg);
    J->SetBoolField(TEXT("accepted"),R.bAccepted);J->SetBoolField(TEXT("complete"),R.bComplete);J->SetStringField(TEXT("failure"),R.Failure);return J;
}
}

bool RunAftershockMassSparseFieldFixtures(const FString& AbsoluteReportPath)
{
    using namespace AftershockSparseMassAuditPrivate;
    auto Report=MakeShared<FJsonObject>();Report->SetBoolField(TEXT("valid"),false);Report->SetBoolField(TEXT("in_progress"),true);
    Report->SetStringField(TEXT("scope"),TEXT("Sparse recorder vs exact old dense RecordFields on the same numerical kernel. All physical fields/replay/contacts/ledger doubles bit-equal except WorkUnits/ComputeMilliseconds. Synthetic51075-cell180s incomplete-moving work fixture, not real runout completion/world/RHI/save proof."));
    const auto Save=[&](){FString JSON;const auto Writer=TJsonWriterFactory<>::Create(&JSON);if(!FJsonSerializer::Serialize(Report,Writer))return false;
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsoluteReportPath),true);const FString Temp=AbsoluteReportPath+TEXT(".tmp");
        return FFileHelper::SaveStringToFile(JSON,*Temp,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
            &&IFileManager::Get().Move(*AbsoluteReportPath,*Temp,true,false,false,true);};
    if(!Save())return false;TArray<TSharedPtr<FJsonValue>> Rows;int Failed=0;
    const auto Check=[&](const TCHAR* Name,bool Passed){auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("name"),Name);J->SetBoolField(TEXT("passed"),Passed);
        Rows.Add(MakeShared<FJsonValueObject>(J));Failed+=Passed?0:1;UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MASS_SPARSE_CHECK name=%s passed=%d"),Name,Passed);};
    const auto Compare=[&](const TCHAR* Name,const FAftershockMassInput& I)
    {FAftershockMassFieldWorkAudit A,B;const auto Sparse=SolveAftershockMassFieldsForAudit(I,false,A),Dense=SolveAftershockMassFieldsForAudit(I,true,B);
        Check(Name,ExactOutputs(Sparse,Dense)&&A.DenseResetAndScanUnits==0&&B.DenseResetAndScanUnits>0);
        return Sparse;};
    const auto Snow=Fixture(),Soil=Fixture(EAftershockMassMaterial::SoilDebris);
    Compare(TEXT("01_snow_all_physical_fields_frames_contacts_ledgers_bit_equal"),Snow);
    Compare(TEXT("02_soil_all_physical_fields_frames_contacts_ledgers_bit_equal"),Soil);
    auto Flat=Fixture(EAftershockMassMaterial::Snow,true);Compare(TEXT("03_moving_to_deposit_clears_old_final_moving_history_exact"),Flat);
    auto Multi=Flat;auto Second=Multi.Release[0];Second.Coord=FIntPoint(9,4);Multi.Release.Add(Second);Multi.Settings.EntrainFractionPerSecond=.8;
    Compare(TEXT("04_overlapping_sources_entrainment_residual_order_bit_equal"),Multi);
    auto Blocked=Snow;FAftershockMassObstacle Wall;Wall.BuildingID=44;Wall.CenterM=FVector2D(85.,105.);Wall.HalfExtentM=FVector2D(25.,1.);Wall.HeightM=5.;Blocked.Obstacles.Add(Wall);
    Blocked.FoundationResponse=[](const auto&,auto& Response){Response.TangentialRetention=0.;return true;};
    const auto Contact=Compare(TEXT("05_foundation_contact_pressure_time_impulse_replay_bit_equal"),Blocked);
    Check(TEXT("06_contact_equivalence_contains_actual_nonzero_contact"),Contact.Impacts.Num()==1&&Contact.Impacts[0].Contacts>0&&Contact.Impacts[0].PeakNormalPressurePa>0.);
    auto Few=Snow;Few.Settings.MaxFrames=2;Compare(TEXT("07_final_replacement_two_frame_replay_bit_equal"),Few);
    auto Limited=Snow;Limited.Settings.MaxParticleSteps=1;Compare(TEXT("08_rejected_particle_budget_keeps_moving_mass_outputs_bit_equal"),Limited);
    FAftershockMassInput Large=Fixture(EAftershockMassMaterial::Snow,true);Large.Cells.Reset();Large.Settings.MaxCells=65536;
    Large.Settings.MaximumSeconds=180.;Large.Settings.MaxWorkUnits=250000000;Large.Settings.StopSpeedMps=0.;
    Large.Settings.SnowFriction={0.,1.e12};Large.Release[0].InitialVelocityMps=FVector(.001,0.,0.);
    for(int Y=0;Y<227;++Y)for(int X=0;X<225;++X)
    {FAftershockMassCell C;C.Coord=FIntPoint(X,Y);C.SnowWaterEquivalentMm=180.;C.SnowDensityKgM3=300.;C.SnowWeakLayerStrengthPa=50.;Large.Cells.Add(C);}
    FAftershockMassFieldWorkAudit SparseWork,DenseWork;
    const auto LargeSparse=SolveAftershockMassFieldsForAudit(Large,false,SparseWork);
    const auto LargeDense=SolveAftershockMassFieldsForAudit(Large,true,DenseWork);
    Check(TEXT("09_51075_cells_180_seconds_reached_without_dense_work_charge"),LargeSparse.SimulatedSeconds==180.
        &&LargeSparse.Budget.WorkUnits<Large.Settings.MaxWorkUnits&&!LargeSparse.Failure.Contains(TEXT("work budget")));
    Check(TEXT("10_incomplete_moving_mass_is_retained_never_forged_as_complete"),!LargeSparse.bAccepted&&!LargeSparse.bComplete&&LargeSparse.Budget.MovingKg>0.
        &&LargeSparse.bMassConserved&&LargeSparse.bEnergyClosed&&LargeSparse.Failure.StartsWith(TEXT("Time/substep budget ended")));
    Check(TEXT("11_old_dense_reference_reproduces_250m_work_budget_limit"),LargeDense.SimulatedSeconds<180.
        &&LargeDense.Failure.Contains(TEXT("work budget"))&&!LargeDense.bAccepted&&LargeDense.Budget.MovingKg>0.);
    const int64 TouchBound=static_cast<int64>(Large.Settings.MaxParcels)*4;
    Check(TEXT("12_sparse_scratch_once_and_touched_visits_bounded_by_parcels"),SparseWork.ScratchInitializedCells==51075&&SparseWork.DenseResetAndScanUnits==0
        &&SparseWork.PeakTouchedIndices<=TouchBound&&SparseWork.SparseScatteredIndices<=SparseWork.RecordCalls*LargeSparse.Budget.InitialParcels*4
        &&SparseWork.SparseClearedIndices<=SparseWork.RecordCalls*TouchBound&&SparseWork.SparseUpdatedIndices<=SparseWork.RecordCalls*TouchBound);
    Check(TEXT("13_replay_full_grid_scans_counted_separately_and_bounded"),SparseWork.ReplayCellScanIndices==SparseWork.ReplayFrameCalls*51075
        &&SparseWork.ReplayFrameCalls<=Large.Settings.MaxFrames+1&&LargeSparse.Frames.Num()<=Large.Settings.MaxFrames);
    Check(TEXT("14_large_canonical_final_moving_fields_sum_to_actual_mass"),[&](){double Sum=0.;for(const auto& C:LargeSparse.Cells)Sum+=C.FinalMovingMassKg;
        return FMath::IsNearlyEqual(Sum,LargeSparse.Budget.MovingKg,1.e-6);}());
    Report->SetArrayField(TEXT("checks"),Rows);Report->SetNumberField(TEXT("expected_checks"),14);Report->SetNumberField(TEXT("passed"),Rows.Num()-Failed);
    Report->SetNumberField(TEXT("failed"),Failed);Report->SetBoolField(TEXT("valid"),Rows.Num()==14&&Failed==0);Report->SetBoolField(TEXT("in_progress"),false);
    Report->SetObjectField(TEXT("large_sparse_actual"),WorkJSON(SparseWork,LargeSparse));Report->SetObjectField(TEXT("large_dense_actual"),WorkJSON(DenseWork,LargeDense));
    Report->SetStringField(TEXT("completed_utc"),FDateTime::UtcNow().ToIso8601());const bool Saved=Save();
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MASS_SPARSE_RESULT checked=%d failed=%d saved=%d"),Rows.Num(),Failed,Saved);
    return Rows.Num()==14&&Failed==0&&Saved;
}
#endif
