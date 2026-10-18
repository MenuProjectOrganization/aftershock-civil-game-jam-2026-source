#include "AftershockMassMovementAudit.h"
#include "AftershockMassMovement.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include <limits>

namespace AftershockMassNumericalAuditDetail
{
FAftershockMassInput Fixture(EAftershockMassMaterial Material,double Degrees=35.,bool Flatten=true)
{
    FAftershockMassInput I;I.Material=Material;I.HazardTypeTag=Material==EAftershockMassMaterial::Snow?7:8;
    I.TerrainRevision=123;I.CellSizeM=10.;I.Settings.MaximumSeconds=60.;
    const double Gradient=FMath::Tan(FMath::DegreesToRadians(Degrees));
    for(int32 Y=0;Y<24;++Y)for(int32 X=0;X<20;++X)
    {
        FAftershockMassCell C;C.Coord=FIntPoint(X,Y);C.GroundElevationM=-(Flatten?FMath::Min(Y,12):Y)*I.CellSizeM*Gradient;
        C.SnowWaterEquivalentMm=Material==EAftershockMassMaterial::Snow?180.:0.;C.SnowDensityKgM3=300.;C.SnowWeakLayerStrengthPa=50.;
        C.RegolithVerticalDepthM=1.;C.SoilDensityKgM3=1800.;C.CohesionPa=50.;C.FrictionAngleDegrees=25.;C.EffectiveSaturation01=1.;
        I.Cells.Add(C);
    }
    FAftershockMassRelease R;R.Coord=FIntPoint(8,4);R.AvailableMassFraction=.5;I.Release.Add(R);return I;
}
FAftershockMassCell* Cell(FAftershockMassInput& I,FIntPoint Coord)
{return I.Cells.FindByPredicate([&](const auto& C){return C.Coord==Coord;});}
bool Close(double A,double B,double Tolerance=1.e-7)
{return FMath::IsFinite(A)&&FMath::IsFinite(B)&&FMath::Abs(A-B)<=Tolerance*FMath::Max3(1.,FMath::Abs(A),FMath::Abs(B));}
bool Ledger(const FAftershockMassResult& R)
{return R.bMassConserved&&R.bEnergyClosed&&Close(R.Budget.ReleasedKg+R.Budget.EntrainedKg,R.Budget.MovingKg+R.Budget.DepositedKg+R.Budget.ExportedKg)
    &&Close(R.Budget.InitialKineticJ+R.Budget.GravityWorkJ,R.Budget.DissipatedJ+R.Budget.MovingKineticJ+R.Budget.ExportedKineticJ)
    &&R.Budget.DissipatedJ>=-1.e-8;}
bool PositiveFields(const FAftershockMassResult& R)
{
    for(const auto& C:R.Cells)
        if(!FMath::IsFinite(C.DepositedMassKg)||!FMath::IsFinite(C.RemovedVolumeM3)||C.DepositedMassKg<0.||C.DepositedVolumeM3<0.
            ||C.ReleasedMassKg<0.||C.EntrainedMassKg<0.||C.FinalMovingMassKg<0.||C.PeakFlowDepthM<0.||C.PeakSpeedMps<0.)return false;
    return true;
}
bool SamePhysics(const FAftershockMassResult& A,const FAftershockMassResult& B)
{
    if(A.bAccepted!=B.bAccepted||A.Failure!=B.Failure||A.Cells.Num()!=B.Cells.Num()
        ||!Close(A.Budget.ReleasedKg,B.Budget.ReleasedKg,1.e-12)||!Close(A.Budget.DepositedKg,B.Budget.DepositedKg,1.e-12)
        ||!Close(A.Budget.ExportedKg,B.Budget.ExportedKg,1.e-12)||!Close(A.Budget.GravityWorkJ,B.Budget.GravityWorkJ,1.e-12)
        ||!Close(A.Budget.DissipatedJ,B.Budget.DissipatedJ,1.e-12))return false;
    for(int32 K=0;K<A.Cells.Num();++K)
    {const auto& X=A.Cells[K];const auto& Y=B.Cells[K];if(X.Coord!=Y.Coord||!Close(X.DepositedMassKg,Y.DepositedMassKg,1.e-12)
        ||!Close(X.PeakFlowDepthM,Y.PeakFlowDepthM,1.e-12)||!Close(X.PeakSpeedMps,Y.PeakSpeedMps,1.e-12))return false;}
    return true;
}
TSharedRef<FJsonObject> BudgetJSON(const FAftershockMassResult& R)
{
    auto V=MakeShared<FJsonObject>();V->SetBoolField(TEXT("accepted"),R.bAccepted);V->SetBoolField(TEXT("complete"),R.bComplete);
    V->SetStringField(TEXT("failure"),R.Failure);V->SetNumberField(TEXT("released_kg"),R.Budget.ReleasedKg);V->SetNumberField(TEXT("entrained_kg"),R.Budget.EntrainedKg);
    V->SetNumberField(TEXT("moving_kg"),R.Budget.MovingKg);V->SetNumberField(TEXT("deposited_kg"),R.Budget.DepositedKg);V->SetNumberField(TEXT("exported_kg"),R.Budget.ExportedKg);
    V->SetNumberField(TEXT("mass_residual_kg"),R.Budget.MassResidualKg);V->SetNumberField(TEXT("initial_kinetic_j"),R.Budget.InitialKineticJ);
    V->SetNumberField(TEXT("gravity_bed_work_j"),R.Budget.GravityWorkJ);V->SetNumberField(TEXT("dissipated_j"),R.Budget.DissipatedJ);
    V->SetNumberField(TEXT("moving_kinetic_j"),R.Budget.MovingKineticJ);V->SetNumberField(TEXT("exported_kinetic_j"),R.Budget.ExportedKineticJ);V->SetNumberField(TEXT("energy_residual_j"),R.Budget.EnergyResidualJ);
    V->SetNumberField(TEXT("maximum_speed_mps"),R.Budget.MaximumSpeedMps);V->SetNumberField(TEXT("maximum_displacement_cells"),R.Budget.MaximumDisplacementCells);
    V->SetNumberField(TEXT("substeps"),R.Budget.Substeps);V->SetNumberField(TEXT("particle_steps"),static_cast<double>(R.Budget.ParticleSteps));
    V->SetNumberField(TEXT("work_units"),static_cast<double>(R.Budget.WorkUnits));
    V->SetNumberField(TEXT("surface_samples"),static_cast<double>(R.Budget.SurfaceSamples));V->SetNumberField(TEXT("compute_ms"),R.Budget.ComputeMilliseconds);return V;
}
}

bool RunAftershockMassMovementFixtures(const FString& AbsoluteReportPath)
{
    using namespace AftershockMassNumericalAuditDetail;
    auto Report=MakeShared<FJsonObject>();Report->SetBoolField(TEXT("valid"),false);Report->SetBoolField(TEXT("in_progress"),true);
    Report->SetStringField(TEXT("run_id"),FGuid::NewGuid().ToString());Report->SetStringField(TEXT("started_utc"),FDateTime::UtcNow().ToIso8601());
    Report->SetStringField(TEXT("scope"),TEXT("34 isolated numerical-kernel fixtures on fixed480-cell DTOs; not world, building damage, CityLife, GodMode permissions, save migration, GPU, RHI or packaged acceptance."));
    const auto Save=[&]()
    {FString Text;const auto Writer=TJsonWriterFactory<>::Create(&Text);if(!FJsonSerializer::Serialize(Report,Writer))return false;
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsoluteReportPath),true);const FString Temp=AbsoluteReportPath+TEXT(".tmp");
        return FFileHelper::SaveStringToFile(Text,*Temp,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
            &&IFileManager::Get().Move(*AbsoluteReportPath,*Temp,true,false,false,true);};
    if(!Save())return false;
    TArray<TSharedPtr<FJsonValue>> Rows;int32 Failed=0;
    const auto Check=[&](const TCHAR* Name,bool Pass,const FAftershockMassResult* R=nullptr)
    {auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("name"),Name);Row->SetBoolField(TEXT("passed"),Pass);
        if(R)Row->SetObjectField(TEXT("actual_kernel_result"),BudgetJSON(*R));Rows.Add(MakeShared<FJsonValueObject>(Row));Failed+=Pass?0:1;
        UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MASS_FIXTURE name=%s passed=%d"),Name,Pass);};
    using K=FAftershockMassMovementSolver;
    const auto Snow=Fixture(EAftershockMassMaterial::Snow),Soil=Fixture(EAftershockMassMaterial::SoilDebris);
    auto Zero=Snow;for(auto& C:Zero.Cells)C.SnowWaterEquivalentMm=0.;
    const auto ZeroR=K::Solve(Zero);Check(TEXT("01_zero_snow_rejected_no_release"),!ZeroR.bAccepted&&ZeroR.Budget.ReleasedKg==0.,&ZeroR);
    auto Flat=Fixture(EAftershockMassMaterial::Snow,0.);const auto FlatE=K::EvaluateRelease(Flat,Flat.Release[0]);
    Check(TEXT("02_flat_snow_not_spontaneous_slab"),!FlatE.bEligible&&FlatE.SlopeDegrees<1.e-6);
    const auto SnowE=K::EvaluateRelease(Snow,Snow.Release[0]);
    Check(TEXT("03_weak_snow_release_eligible_exact9000kg"),SnowE.bEligible&&Close(SnowE.AvailableMassKg*Snow.Release[0].AvailableMassFraction,9000.));
    auto Strong=Snow;for(auto& C:Strong.Cells)C.SnowWeakLayerStrengthPa=1.e6;
    Check(TEXT("04_stronger_layer_stable"),!K::EvaluateRelease(Strong,Strong.Release[0]).bEligible);
    Strong.Release[0].bForceRelease=true;
    Check(TEXT("05_explicit_force_only_bypasses_stability_not_stock"),K::EvaluateRelease(Strong,Strong.Release[0]).bEligible
        &&!K::EvaluateRelease(Zero,Strong.Release[0]).bEligible);
    auto Rock=Soil;for(auto& C:Rock.Cells)C.RegolithVerticalDepthM=0.;const auto RockR=K::Solve(Rock);
    Check(TEXT("06_zero_regolith_no_debris_created"),!RockR.bAccepted&&RockR.Budget.ReleasedKg==0.,&RockR);
    auto StableSoil=Soil;for(auto& C:StableSoil.Cells){C.CohesionPa=8000.;C.FrictionAngleDegrees=40.;C.EffectiveSaturation01=0.;}
    Check(TEXT("07_dry_coherent_soil_stable"),!K::EvaluateRelease(StableSoil,StableSoil.Release[0]).bEligible);
    const double T=FMath::DegreesToRadians(35.),Phi=FMath::DegreesToRadians(30.);
    const double DryFS=K::ShallowSoilFactorOfSafety(T,1.,1800.,1000.,Phi,0.);
    const double WetFS=K::ShallowSoilFactorOfSafety(T,1.,1800.,1000.,Phi,1.);
    Check(TEXT("08_saturation_reduces_fixed_parameter_FS"),WetFS<DryFS&&WetFS>=0.);
    Check(TEXT("09_cohesion_raises_FS"),K::ShallowSoilFactorOfSafety(T,1.,1800.,5000.,Phi,.5)>K::ShallowSoilFactorOfSafety(T,1.,1800.,1000.,Phi,.5));
    Check(TEXT("10_friction_angle_raises_FS"),K::ShallowSoilFactorOfSafety(T,1.,1800.,1000.,FMath::DegreesToRadians(40.),.5)>K::ShallowSoilFactorOfSafety(T,1.,1800.,1000.,Phi,.5));
    auto Bad=Snow;Bad.Cells[0].GroundElevationM=std::numeric_limits<double>::quiet_NaN();const auto BadR=K::Solve(Bad);
    Check(TEXT("11_nonfinite_input_fails_before_release"),!BadR.bAccepted&&BadR.Budget.ReleasedKg==0.,&BadR);
    auto Over=Snow;Over.Settings.MaxCells=10;const auto OverR=K::Solve(Over);
    Check(TEXT("12_cell_budget_fails_before_allocation_release"),!OverR.bAccepted&&OverR.Cells.IsEmpty()&&OverR.Budget.ReleasedKg==0.,&OverR);
    const auto SnowR=K::Solve(Snow),SoilR=K::Solve(Soil);
    Check(TEXT("13_snow_runout_mass_conserved"),SnowR.bAccepted&&Ledger(SnowR)&&Close(SnowR.Budget.ReleasedKg,9000.)&&PositiveFields(SnowR),&SnowR);
    Check(TEXT("14_soil_runout_mass_conserved"),SoilR.bAccepted&&Ledger(SoilR)&&Close(SoilR.Budget.ReleasedKg,90000.)&&PositiveFields(SoilR),&SoilR);
    Check(TEXT("15_gravity_kinetic_dissipation_energy_closure"),Ledger(SnowR)&&Ledger(SoilR)&&SnowR.Budget.GravityWorkJ>0.&&SnowR.Budget.DissipatedJ>0.,&SnowR);
    auto MovingFlat=Flat;MovingFlat.Release[0].bForceRelease=true;MovingFlat.Release[0].InitialVelocityMps=FVector(0.,4.,0.);
    const auto FlatR=K::Solve(MovingFlat);
    Check(TEXT("16_friction_spends_initial_energy"),FlatR.bAccepted&&FlatR.Budget.DissipatedJ>0.&&Ledger(FlatR),&FlatR);
    Check(TEXT("17_flat_runout_persistent_deposit"),FlatR.bAccepted&&FlatR.Budget.DepositedKg>0.&&FlatR.Budget.MovingKg==0.&&PositiveFields(FlatR),&FlatR);
    auto Plane=Fixture(EAftershockMassMaterial::Snow,35.,false);Plane.Settings.MaximumSeconds=.5;Plane.Settings.SnowFriction={0.,1.e12};
    const auto PlaneR=K::Solve(Plane);
    Check(TEXT("18_plane_gravity_positive_velocity_CFL_energy"),PlaneR.Budget.MaximumSpeedMps>.1&&PlaneR.Budget.MaximumDisplacementCells<=Plane.Settings.CFL+1.e-6&&Ledger(PlaneR),&PlaneR);
    auto Ridge=MovingFlat;for(auto& C:Ridge.Cells)if(C.Coord.Y>=5)C.GroundElevationM=(C.Coord.Y-4)*20.;
    const auto RidgeR=K::Solve(Ridge);
    TMap<int32,double> InitialRidgeEnergy;bool RidgeNoGain=!RidgeR.Frames.IsEmpty();
    if(RidgeNoGain)for(const auto& P:RidgeR.Frames[0].Parcels)
        InitialRidgeEnergy.Add(P.ParcelID,.5*P.MassKg*P.VelocityMps.SizeSquared()+P.MassKg*FAftershockMassSettings::GravityMps2*P.PositionM.Z);
    for(const auto& F:RidgeR.Frames)for(const auto& P:F.Parcels)
    {const double E=.5*P.MassKg*P.VelocityMps.SizeSquared()+P.MassKg*FAftershockMassSettings::GravityMps2*P.PositionM.Z;
        const double* Initial=InitialRidgeEnergy.Find(P.ParcelID);RidgeNoGain&=Initial&&E<=*Initial+FMath::Max(1.e-6,FMath::Abs(*Initial)*1.e-8);}
    Check(TEXT("19_ridge_cannot_gain_free_kinetic_energy"),Ledger(RidgeR)&&RidgeNoGain,&RidgeR);
    auto Outlet=Fixture(EAftershockMassMaterial::Snow,35.,false);Outlet.Settings.SnowFriction={.05,1.e9};const auto OutletR=K::Solve(Outlet);
    Check(TEXT("20_supported_domain_exit_mass_and_energy_export"),OutletR.bAccepted&&OutletR.Budget.ExportedKg>0.&&Ledger(OutletR),&OutletR);
    auto Pair=MovingFlat;const FAftershockMassRelease SecondRelease=Pair.Release[0];Pair.Release.Add(SecondRelease);Pair.Release.Last().Coord=FIntPoint(9,4);const auto PairR=K::Solve(Pair);
    double DepositedSum=0.;for(const auto& C:PairR.Cells)DepositedSum+=C.DepositedMassKg;
    Check(TEXT("21_two_sources_add_deposition_not_overwrite"),PairR.bAccepted&&Close(PairR.Budget.ReleasedKg,18000.)&&Close(DepositedSum,PairR.Budget.DepositedKg)&&Ledger(PairR),&PairR);
    auto Entrain=MovingFlat;Entrain.Settings.EntrainFractionPerSecond=.8;const auto EntrainR=K::Solve(Entrain);
    bool FiniteSupply=true;for(const auto& C:EntrainR.Cells)
    {const auto* Before=Entrain.Cells.FindByPredicate([&](const auto& V){return V.Coord==C.Coord;});
        FiniteSupply&=Before&&C.ReleasedMassKg+C.EntrainedMassKg<=Before->SnowWaterEquivalentMm*100.+1.e-5;}
    Check(TEXT("22_entrainment_withdraws_only_finite_stock"),EntrainR.Budget.EntrainedKg>0.&&FiniteSupply&&Ledger(EntrainR)&&PositiveFields(EntrainR),&EntrainR);
    Check(TEXT("23_disabled_entrainment_never_adds_mass"),FlatR.Budget.EntrainedKg==0.&&Close(FlatR.Budget.ReleasedKg,9000.),&FlatR);
    auto Light=Snow,Dense=Snow;for(auto& C:Light.Cells)C.SnowDensityKgM3=150.;for(auto& C:Dense.Cells)C.SnowDensityKgM3=450.;
    const auto LightE=K::EvaluateRelease(Light,Light.Release[0]),DenseE=K::EvaluateRelease(Dense,Dense.Release[0]);
    Check(TEXT("24_SWE_mass_same_density_changes_volume_threefold"),Close(LightE.AvailableMassKg,DenseE.AvailableMassKg)&&Close(LightE.AvailableVolumeM3,3.*DenseE.AvailableVolumeM3));
    const auto Repeat=K::Solve(Snow);
    Check(TEXT("25_same_seed_same_order_same_physics"),SamePhysics(SnowR,Repeat),&Repeat);
    auto Callbacks=MovingFlat;int32 MaterialCalls=0,FoundationCalls=0;
    FAftershockMassObstacle Wall;Wall.BuildingID=44;Wall.CenterM=FVector2D(85.,105.);Wall.HalfExtentM=FVector2D(25.,1.);Wall.HeightM=5.;Wall.FoundationTag=2;
    Callbacks.Obstacles.Add(Wall);Callbacks.MaterialFriction=[&](const auto&,auto,auto&){++MaterialCalls;return true;};
    Callbacks.FoundationResponse=[&](const auto& O,auto& R){++FoundationCalls;R.TangentialRetention=O.FoundationTag==2?0.:.8;return true;};
    const auto CallbackR=K::Solve(Callbacks);
    Check(TEXT("26_callbacks_only_once_per_staged_DTO"),MaterialCalls==Callbacks.Cells.Num()&&FoundationCalls==1
        &&CallbackR.Budget.MaterialCallbackCalls==MaterialCalls&&CallbackR.Budget.FoundationCallbackCalls==1,&CallbackR);
    auto Blocked=Snow;Blocked.Obstacles.Add(Wall);Blocked.FoundationResponse=[](const auto&,auto& R){R.TangentialRetention=0.;return true;};
    const auto BlockedR=K::Solve(Blocked);
    Check(TEXT("27_real_footprint_collision_pressure_impulse"),BlockedR.bAccepted&&Ledger(BlockedR)&&BlockedR.Impacts.Num()==1
        &&BlockedR.Impacts[0].BuildingID==44&&BlockedR.Impacts[0].PeakNormalPressurePa>0.&&BlockedR.Impacts[0].ImpulseMagnitudeNs>0.
        &&BlockedR.Impacts[0].FirstContactSeconds>0.&&BlockedR.Impacts[0].FirstContactSeconds<=BlockedR.SimulatedSeconds
        &&BlockedR.Impacts[0].PeakPressureSeconds>=BlockedR.Impacts[0].FirstContactSeconds
        &&BlockedR.Impacts[0].PeakPressureSeconds<=BlockedR.SimulatedSeconds,&BlockedR);
    Check(TEXT("28_immutable_snapshot_no_material_foundation_authority_write"),Callbacks.Cells.Num()==480&&Callbacks.Obstacles[0].FoundationTag==2
        &&Callbacks.Cells[0].SnowWaterEquivalentMm==180.&&Callbacks.TerrainRevision==123&&Close(Cell(Callbacks,FIntPoint(8,4))->SnowWaterEquivalentMm,180.));
    auto Limited=Snow;Limited.Settings.MaxParticleSteps=1;const auto LimitedR=K::Solve(Limited);
    Check(TEXT("29_work_timeout_retains_mass_and_rejects_commit"),!LimitedR.bAccepted&&!LimitedR.bComplete&&LimitedR.Budget.ParticleSteps==0
        &&Close(LimitedR.Budget.MovingKg,LimitedR.Budget.ReleasedKg)&&Ledger(LimitedR),&LimitedR);
    Check(TEXT("30_zero_slope_and_depth_divisions_failclosed"),K::ShallowSoilFactorOfSafety(0.,1.,1800.,0.,Phi,0.)>=1.e10
        &&K::ShallowSoilFactorOfSafety(T,0.,1800.,0.,Phi,0.)<0.&&K::ShallowSoilFactorOfSafety(T,1.,0.,0.,Phi,0.)<0.);
    auto FewFrames=Snow;FewFrames.Settings.MaxFrames=2;const auto FewFramesR=K::Solve(FewFrames);
    const auto FrameLedger=[&](const FAftershockMassResult& R)
    {
        if(!R.bFrameExportComplete||R.Frames.IsEmpty()||R.ExportedFrameCellRecords>FAftershockMassResult::MaximumFrameCellRecords
            ||R.ExportedFrameParcelRecords>FAftershockMassResult::MaximumFrameParcelRecords)return false;
        TMap<FIntPoint,double> PreviousDeposits;TMap<int32,FAftershockMassImpact> PreviousImpacts;
        int32 CellRecords=0,ParcelRecords=0,ImpactRecords=0;
        for(const auto& F:R.Frames)
        {
            if(F.Parcels.Num()>Snow.Settings.MaxParcels||F.Cells.Num()>Snow.Settings.MaxCells)return false;
            double ParcelM=0.,CellM=0.;for(const auto& P:F.Parcels)ParcelM+=P.MassKg;
            TSet<FIntPoint> Seen;for(const auto& C:F.Cells)
            {
                if(Seen.Contains(C.Coord)||C.MovingMassKg<0.||C.MovingVolumeM3<0.||C.DepositedMassKg<0.||C.DepositedVolumeM3<0.
                    ||!Close(C.FlowDepthM,C.MovingVolumeM3/100.)||C.DepositedMassKg+1.e-7<PreviousDeposits.FindRef(C.Coord))return false;
                Seen.Add(C.Coord);PreviousDeposits.Add(C.Coord,C.DepositedMassKg);CellM+=C.MovingMassKg;
            }
            for(const auto& I:F.Impacts)
            {
                if(I.FirstContactSeconds<0.||I.FirstContactSeconds>F.Seconds+1.e-8||I.PeakPressureSeconds<I.FirstContactSeconds
                    ||I.PeakPressureSeconds>F.Seconds+1.e-8)return false;
                if(const auto* Before=PreviousImpacts.Find(I.BuildingID))if(I.Contacts<Before->Contacts||I.ImpulseMagnitudeNs+1.e-7<Before->ImpulseMagnitudeNs
                    ||I.PeakNormalPressurePa+1.e-7<Before->PeakNormalPressurePa)return false;
                PreviousImpacts.Add(I.BuildingID,I);
            }
            if(!Close(ParcelM,CellM))return false;CellRecords+=F.Cells.Num();ParcelRecords+=F.Parcels.Num();ImpactRecords+=F.Impacts.Num();
        }
        double FinalM=0.,FinalV=0.;const auto& Last=R.Frames.Last();
        for(const auto& C:Last.Cells)
        {
            const auto* Canonical=R.Cells.FindByPredicate([&](const auto& V){return V.Coord==C.Coord;});
            if(!Canonical||!Close(C.DepositedMassKg,Canonical->DepositedMassKg)||!Close(C.DepositedVolumeM3,Canonical->DepositedVolumeM3))return false;
            FinalM+=C.DepositedMassKg;FinalV+=C.DepositedVolumeM3;
        }
        if(Last.Impacts.Num()!=R.Impacts.Num())return false;
        for(const auto& I:R.Impacts)
        {
            const auto* End=Last.Impacts.FindByPredicate([&](const auto& V){return V.BuildingID==I.BuildingID;});
            if(!End||End->Contacts!=I.Contacts||!Close(End->PeakNormalPressurePa,I.PeakNormalPressurePa)
                ||!Close(End->ImpulseMagnitudeNs,I.ImpulseMagnitudeNs)||!Close(End->FirstContactSeconds,I.FirstContactSeconds)
                ||!Close(End->PeakPressureSeconds,I.PeakPressureSeconds))return false;
        }
        double CanonicalV=0.;for(const auto& C:R.Cells)CanonicalV+=C.DepositedVolumeM3;
        return Close(FinalM,R.Budget.DepositedKg)&&Close(FinalV,CanonicalV)&&CellRecords==R.ExportedFrameCellRecords
            &&ParcelRecords==R.ExportedFrameParcelRecords&&ImpactRecords==R.ExportedFrameImpactRecords
            &&ImpactRecords<=FAftershockMassResult::MaximumFrameImpactRecords;
    };
    Check(TEXT("31_visual_frame_ledger_and_budget_do_not_change_physics"),SnowR.Frames.Num()<=Snow.Settings.MaxFrames&&FewFramesR.Frames.Num()<=2
        &&FrameLedger(SnowR)&&FrameLedger(FewFramesR)&&FrameLedger(FlatR)&&FrameLedger(BlockedR)&&SamePhysics(SnowR,FewFramesR),&FewFramesR);
    Check(TEXT("32_pore_pressure_reduces_basal_effective_stress_and_rejects_invalid_inputs"),
        Close(K::EffectiveSoilCoulombCoefficient(.45,0.,1800.),.45)
        &&Close(K::EffectiveSoilCoulombCoefficient(.45,1.,1800.),.2)
        &&K::EffectiveSoilCoulombCoefficient(.45,2.,1800.)<0.);
    auto Drained=Fixture(EAftershockMassMaterial::SoilDebris,20.);
    Drained.Release[0].bForceRelease=true;
    for(auto& C:Drained.Cells)C.EffectiveSaturation01=0.;
    Drained.MaterialFriction=[](const FAftershockMassCell& C,EAftershockMassMaterial,FAftershockMassFriction& F)
    {F.Mu=K::EffectiveSoilCoulombCoefficient(F.Mu,C.EffectiveSaturation01,C.SoilDensityKgM3);return F.Mu>=0.;};
    auto Saturated=Drained;for(auto& C:Saturated.Cells)C.EffectiveSaturation01=1.;
    const auto DrainedR=K::Solve(Drained),SaturatedR=K::Solve(Saturated);
    Check(TEXT("33_same_slope_wet_basal_resistance_changes_actual_runout_without_creating_mass"),
        DrainedR.bAccepted&&SaturatedR.bAccepted&&Ledger(DrainedR)&&Ledger(SaturatedR)
        &&DrainedR.Budget.MaximumSpeedMps<.01&&SaturatedR.Budget.MaximumSpeedMps>.1
        &&Close(DrainedR.Budget.ReleasedKg,SaturatedR.Budget.ReleasedKg),&SaturatedR);
    auto DecimalEnd=Fixture(EAftershockMassMaterial::Snow,0.);
    DecimalEnd.Release[0].bForceRelease=true;DecimalEnd.Release[0].InitialVelocityMps=FVector(.001,0.,0.);
    DecimalEnd.Settings.SnowFriction.Mu=0.;DecimalEnd.Settings.SnowFriction.XiMps2=1.e12;DecimalEnd.Settings.StopSpeedMps=0.;
    const auto DecimalEndR=K::Solve(DecimalEnd);
    Check(TEXT("34_decimal_duration_remainder_is_integrated_without_forging_stationary_mass"),
        !DecimalEndR.bAccepted&&!DecimalEndR.bComplete&&DecimalEndR.SimulatedSeconds==60.
        &&DecimalEndR.Budget.MovingKg>0.&&Ledger(DecimalEndR)
        &&DecimalEndR.Failure.StartsWith(TEXT("Time/substep budget ended")),&DecimalEndR);
    Report->SetNumberField(TEXT("expected_checks"),34);Report->SetNumberField(TEXT("passed"),Rows.Num()-Failed);Report->SetNumberField(TEXT("failed"),Failed);
    Report->SetBoolField(TEXT("valid"),Rows.Num()==34&&Failed==0);Report->SetBoolField(TEXT("in_progress"),false);Report->SetArrayField(TEXT("checks"),Rows);
    Report->SetObjectField(TEXT("snow_reference_actual"),BudgetJSON(SnowR));Report->SetObjectField(TEXT("soil_reference_actual"),BudgetJSON(SoilR));
    Report->SetStringField(TEXT("analytic_mass_goldens"),TEXT("One snowcell:180kg/m2*100m2*0.5=9000kg; one soilcell:1m*100m2*1800kg/m3*0.5=90000kg. Remaining outputs are measured native results, not preclaimed goldens."));
    Report->SetBoolField(TEXT("world_save_casualty_permissions_render_acceptance_pending"),true);
    Report->SetStringField(TEXT("completed_utc"),FDateTime::UtcNow().ToIso8601());const bool Saved=Save();
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MASS_FIXTURES complete checked=%d failed=%d saved=%d"),Rows.Num(),Failed,Saved);return Rows.Num()==34&&Failed==0&&Saved;
}
