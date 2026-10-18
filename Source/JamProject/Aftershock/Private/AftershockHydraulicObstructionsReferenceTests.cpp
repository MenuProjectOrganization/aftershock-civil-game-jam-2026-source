#include "AftershockHydraulicObstructionsReferenceTests.h"
#include "AftershockHydraulicObstructions.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace AftershockWater
{
namespace ObstructionTests
{
Domain Grid(int NX=1,int NY=1,int X0=0,int Y0=0)
{
    Domain D;D.CellWidthM=D.CellHeightM=10.;
    for(int Y=0;Y<NY;++Y)for(int X=0;X<NX;++X)D.Cells.push_back({X+X0,Y+Y0,0.,0.});
    const auto Add=[&](int L,int R,int DX,int DY)
    {FaceGeometry F;F.Left=L;F.Right=R;F.NormalX=DX;F.NormalY=DY;F.LengthM=10.;F.Kind=R<0?FaceKind::Wall:FaceKind::Interior;
        F.StableID=std::int64_t(D.Faces.size()+1);D.Faces.push_back(F);};
    for(int Y=0;Y<NY;++Y)for(int X=0;X<NX;++X)
    {
        const int I=Y*NX+X;
        if(Y==0)Add(I,-1,0,-1);if(X==0)Add(I,-1,-1,0);
        Add(I,X+1<NX?I+1:-1,1,0);Add(I,Y+1<NY?I+NX:-1,0,1);
    }
    return D;
}
ObstructionFootprint Box(int ID,int OX,int OY,std::int64_t X0,std::int64_t Y0,std::int64_t X1,std::int64_t Y1)
{
    ObstructionFootprint P;P.BuildingID=ID;P.OwnerX=OX;P.OwnerY=OY;P.SourceTransformBinding=std::uint64_t(ID)+100;
    P.Corners={{{X0,Y0},{X1,Y0},{X1,Y1},{X0,Y1}}};return P;
}
ObstructionInput Input(std::vector<ObstructionFootprint> P={})
{ObstructionInput I;I.ModelVersion=1;I.Footprints=std::move(P);return I;}
bool Close(double A,double B){return std::abs(A-B)<=1e-10*std::max(1.,std::max(std::abs(A),std::abs(B)));}
}
std::vector<ReferenceCheck> RunHydraulicObstructionReferenceChecks()
{
    using namespace ObstructionTests;std::vector<ReferenceCheck> R;std::string E;
    const auto Check=[&](const char* Name,bool Value){R.push_back({Name,Value});};
    {
        auto D=Grid(2,2,-1,-1);ObstructionInput I;ObstructionGeometry G;
        bool OK=BuildHydraulicObstructions(D,I,G,E);
        for(const auto& C:G.Cells)OK&=C.StoragePorosity==1.;for(const auto& F:G.Faces)OK&=F.OpenFraction==1.;
        Check("obstruction_model0_explicit_phi_theta_one_negative_coords",OK);
        I.Footprints.push_back(Box(1,-1,-1,-8800,-8800,-1200,-1200));
        Check("obstruction_model0_never_silently_accepts_new_footprints",!BuildHydraulicObstructions(D,I,G,E));
    }
    const auto Central=Input({Box(1,0,0,1200,1200,8800,8800)});
    {
        auto D=Grid();ObstructionGeometry G;
        const bool OK=BuildHydraulicObstructions(D,Central,G,E);
        Check("obstruction_real_760cm_box_conservative_storage_raster",OK&&G.Cells[0].StoragePorosity==1596./4096.);
        bool FullPorts=OK;for(const auto& F:G.Faces)FullPorts&=F.OpenFraction==1.;
        Check("obstruction_central_box_edge_ports_not_fabricated_internal_streamlines",FullPorts);
        Check("obstruction_stored_geometry_exact_revalidation",OK&&ValidateHydraulicObstructions(D,G,E));
        auto Tampered=G;if(!Tampered.Cells.empty())Tampered.Cells[0].StoragePorosity=1.;
        Check("obstruction_corrupt_phi_rejected_despite_saved_digest",!ValidateHydraulicObstructions(D,Tampered,E));
        Tampered=G;if(!Tampered.Faces.empty())Tampered.Faces[0].OpenFraction=.5;
        Check("obstruction_corrupt_shared_theta_rejected",!ValidateHydraulicObstructions(D,Tampered,E));
        Tampered=G;++Tampered.Binding.FormatFingerprint;
        Check("obstruction_explicit_format_fingerprint_mismatch_rejected",!ValidateHydraulicObstructions(D,Tampered,E));
    }
    {
        auto D=Grid();auto A=Central;A.Footprints.push_back(Box(2,0,0,2000,2000,8000,8000));
        ObstructionGeometry G0,G1,G2;bool OK=BuildHydraulicObstructions(D,Central,G0,E)&&BuildHydraulicObstructions(D,A,G1,E);
        std::reverse(A.Footprints.begin(),A.Footprints.end());OK&=BuildHydraulicObstructions(D,A,G2,E);
        Check("obstruction_union_no_double_count_and_order_determinism",OK&&G0.Cells[0].StoragePorosity==G1.Cells[0].StoragePorosity
            &&G1.GeometryFingerprint==G2.GeometryFingerprint&&G1.Cells[0].OpenPorts==G2.Cells[0].OpenPorts);
    }
    {
        auto D=Grid(2);auto I=Input({Box(1,0,0,8000,3000,12000,7000)});ObstructionGeometry G;
        bool OK=BuildHydraulicObstructions(D,I,G,E);bool Shared=false;
        for(std::size_t J=0;OK&&J<D.Faces.size();++J)if(D.Faces[J].Right>=0)
        {
            Shared=G.Faces[J].OpenFraction>0.&&G.Faces[J].OpenFraction<1.;
            OK&=G.Cells[0].OpenPorts[1]==G.Cells[1].OpenPorts[3];
        }
        Check("obstruction_two_cells_use_one_aligned_shared_open_fraction",OK&&Shared);
        auto Broken=D;Broken.Faces.pop_back();ObstructionGeometry O;
        Check("obstruction_missing_canonical_half_face_fails_preflight",!BuildHydraulicObstructions(Broken,I,O,E));
    }
    {
        auto D=Grid(1,1,-1,-1);auto I=Input({Box(1,-1,-1,-8800,-8800,-1200,-1200)});ObstructionGeometry G;
        Check("obstruction_negative_cell_grid_origin_is_corner_exact",BuildHydraulicObstructions(D,I,G,E)
            &&G.Cells[0].StoragePorosity==1596./4096.&&G.Cells[0].X==-1&&G.Cells[0].Y==-1);
        I.GridOriginXMM=500;
        ObstructionGeometry Shifted;Check("obstruction_origin_binding_changes_footprint_occupancy",BuildHydraulicObstructions(D,I,Shifted,E)
            &&Shifted.GeometryFingerprint!=G.GeometryFingerprint);
    }
    {
        auto D=Grid();ObstructionGeometry G;G.GeometryFingerprint=123;
        const auto Wall=Input({Box(1,0,0,4900,-1000,5100,11000)});
        Check("obstruction_disconnected_void_rejected_before_output_mutation",!BuildHydraulicObstructions(D,Wall,G,E)&&G.GeometryFingerprint==123);
        const auto Solid=Input({Box(1,0,0,-1000,-1000,11000,11000)});
        Check("obstruction_fully_solid_cell_rejected_without_phi_clamp",!BuildHydraulicObstructions(D,Solid,G,E)&&G.GeometryFingerprint==123);
        const auto TinyVoid=Input({Box(1,0,0,0,0,9800,10000)});
        Check("obstruction_near_solid_cell_rejected_without_phi_clamp",!BuildHydraulicObstructions(D,TinyVoid,G,E));
    }
    {
        auto D=Grid();auto I=Central;I.Footprints[0].SourceTransformBinding=0;ObstructionGeometry G;
        Check("obstruction_missing_canonical_source_binding_rejected",!BuildHydraulicObstructions(D,I,G,E));
        I=Central;I.Footprints[0].Corners[2]=I.Footprints[0].Corners[0];
        Check("obstruction_nonconvex_degenerate_descriptor_rejected",!BuildHydraulicObstructions(D,I,G,E));
        I=Central;I.CellWidthMM=9999;
        Check("obstruction_cm_to_si_grid_scale_mismatch_rejected",!BuildHydraulicObstructions(D,I,G,E));
        I=Central;I.Footprints[0]=Box(1,0,0,-1000,-1000,90000,9000);
        Check("obstruction_oversized_building_fails_never_omitted",!BuildHydraulicObstructions(D,I,G,E));
        I=Central;I.Footprints.assign(MaximumObstructionFootprints+1,I.Footprints[0]);
        Check("obstruction_explicit_maximum_building_count_failure",!BuildHydraulicObstructions(D,I,G,E));
        I=Input();for(int J=1;J<=17;++J)I.Footprints.push_back(Box(J,0,0,1200,1200,8800,8800));
        Check("obstruction_cell_candidate_cap_fails_no_partial_union",!BuildHydraulicObstructions(D,I,G,E));
        ObstructionBuildLimits Limits;Limits.MaximumOwnedBytes=1024;G.GeometryFingerprint=123;
        Check("obstruction_peak_memory_budget_fails_before_output_mutation",!BuildHydraulicObstructions(D,Central,G,E,Limits)&&G.GeometryFingerprint==123);
        Limits={};Limits.MaximumRasterTileTests=128;
        Check("obstruction_raster_work_budget_fails_without_dropping_buildings",!BuildHydraulicObstructions(D,Central,G,E,Limits)&&G.GeometryFingerprint==123);
    }
    {
        auto D=Grid();ObstructionGeometry G;const bool OK=BuildHydraulicObstructions(D,Central,G,E);
        std::vector<CellState> S{{2.,3.,-4.}},T;
        const bool Reprojected=OK&&ReprojectHydraulicObstructions(D,S,G,T,E);const double P=OK?G.Cells[0].StoragePorosity:1.;
        Check("obstruction_construction_reprojection_conserves_volume_and_integrated_q",Reprojected&&Close(T[0].DepthM*P,2.)
            &&Close(T[0].DischargeXM2PerS*P,3.)&&Close(T[0].DischargeYM2PerS*P,-4.));
        Check("obstruction_construction_reprojection_preserves_velocity",Reprojected&&Close(T[0].DischargeXM2PerS/T[0].DepthM,1.5));
        if(OK)D.Cells[0].StoragePorosity=P;
        ObstructionInput Empty;ObstructionGeometry Removed;bool Undo=Reprojected&&BuildHydraulicObstructions(D,Empty,Removed,E);
        std::vector<CellState> Restored;Undo=Undo&&ReprojectHydraulicObstructions(D,T,Removed,Restored,E);
        Check("obstruction_demolition_rebind_conserves_same_water_and_q",Undo&&Close(Restored[0].DepthM,S[0].DepthM)
            &&Close(Restored[0].DischargeXM2PerS,S[0].DischargeXM2PerS)&&Close(Restored[0].DischargeYM2PerS,S[0].DischargeYM2PerS));
        T={{123.,0.,0.}};S={{0.,1.,0.}};
        Check("obstruction_invalid_dry_momentum_rejected_without_mutation",!ReprojectHydraulicObstructions(D,S,Removed,T,E)&&T[0].DepthM==123.);
    }
    {
        auto D=Grid();ObstructionGeometry G;ObstructionInput I;std::vector<CellState> S{{.125,.25,-.5}},T;
        const bool OK=BuildHydraulicObstructions(D,I,G,E)&&ReprojectHydraulicObstructions(D,S,G,T,E);
        Check("obstruction_legacy_one_path_preserves_exact_state_values",OK&&T[0].DepthM==S[0].DepthM
            &&T[0].DischargeXM2PerS==S[0].DischargeXM2PerS&&T[0].DischargeYM2PerS==S[0].DischargeYM2PerS);
    }
    return R;
}
}
