#include "AftershockHydraulicTerrainReferenceTests.h"
#include "AftershockHydraulicTerrain.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/EngineTypes.h"
#include "CollisionQueryParams.h"
#include "UObject/UnrealType.h"
#if WITH_EDITOR
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/StrongObjectPtr.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#endif

namespace AftershockHydraulicTerrainReferencePrivate
{
	bool HydraulicTestNear(double A,double B,double T=1e-9){return FMath::Abs(A-B)<=T;}
	bool HydraulicTestSameCells(const TArray<FGridCell>& A,const TArray<FGridCell>& B)
	{
		if(A.Num()!=B.Num())return false;
		for(int32 I=0;I<A.Num();++I)if(!FGridCell::StaticStruct()->CompareScriptStruct(&A[I],&B[I],0))return false;
		return true;
	}
	void AppendHydraulicWideDamChecks(TArray<FAftershockHydraulicTerrainCheck>& Checks)
	{
		const auto Check=[&](const TCHAR* Name,bool Pass){Checks.Add({Name,Pass});};
		TArray<FGridCell> Cells;TArray<FAftershockHydraulicPersistentCell> Persistent;
		for(int32 Y=0;Y<4;++Y)for(int32 X=0;X<5;++X)
		{
			FGridCell C;C.Coords=FIntPoint(X,Y);C.bIsPlayable=true;C.bIsRiverbed=X>=1&&X<=3;C.Elevation=C.bIsRiverbed?(X==1?1050.f:X==2?1000.f:1100.f):2000.f;
			C.HydraulicProfileVersion=C.bIsRiverbed?1:0;C.HydraulicExcavationCm=C.bIsRiverbed?(Y<=1?250.f:300.f):0.f;
			if(C.Coords==FIntPoint(2,1)){C.bHasDam=true;C.DamOutflowStep=FIntPoint(0,1);C.DamCrestHeightM=2.f;C.DamGateOpen01=0.f;}
			Cells.Add(C);const double Bed=(C.Elevation-C.HydraulicExcavationCm)*.01;
			Persistent.Add({C.Coords,C.bIsRiverbed&&Y<=1?(11.-Bed)*100.:0.,0.,0.,100.});
		}
		FAftershockHydraulicTerrainInput Input;Input.Cells=Cells;Input.PersistentCells=Persistent;Input.bModel2Active=true;
		FAftershockHydraulicTerrainBuild B;FText Error;std::string CoreError;
		const bool Built=BuildAftershockHydraulicTerrain(Input,B,Error);
		Check(TEXT("wide_dam_has_three_shared_channel_faces_one_owner"),Built&&B.Diagnostics.StructureFaces==3);
		bool Correct=Built;TSet<int64> Shared;int32 Touches[5]={0,0,0,0,0};
		for(const auto& F:B.Domain.Faces)if(F.Kind==AftershockWater::FaceKind::HydraulicStructure)
		{
			const auto& L=B.Domain.Cells[F.Left];const auto& R=B.Domain.Cells[F.Right];
			Correct&=L.X==R.X&&L.Y==1&&R.Y==2&&F.NormalX==0.&&F.NormalY==1.&&!Shared.Contains(F.StableID)
				&&HydraulicTestNear(F.WeirCrestM,12.)&&HydraulicTestNear(F.GateInvertM,FMath::Max(L.BedM,R.BedM))
				&&HydraulicTestNear(F.GateOpenFraction,0.);Shared.Add(F.StableID);
			if(L.X>=0&&L.X<5)++Touches[L.X];
		}
		Check(TEXT("wide_dam_reciprocal_face_ids_crest_and_local_inverts"),Correct&&Touches[1]==1&&Touches[2]==1&&Touches[3]==1);
		const auto Downstream=[&](const AftershockWater::Solver& Solver)
		{double Volume=0.;for(std::size_t I=0;I<Solver.Cells().size();++I)if(Solver.Geometry().Cells[I].Y>=2)Volume+=Solver.Cells()[I].DepthM*100.;return Volume;};
		AftershockWater::Settings Settings;Settings.MaximumStepsPerAdvance=512;Settings.MaximumStepSeconds=.05;
		AftershockWater::Solver Closed;const bool ClosedReady=Built&&Closed.Initialize(B.Domain,B.InitialCells,{},Settings,CoreError);
		const auto ClosedStep=ClosedReady?Closed.Advance(2.,{}):AftershockWater::AdvanceResult{};
		Check(TEXT("wide_closed_gate_below_crest_has_no_side_channel_bypass"),ClosedStep.Success&&ClosedStep.RemainingSeconds<1.e-9
			&&Downstream(Closed)<1.e-8&&FMath::Abs(Closed.VolumeBudget().ResidualM3)<1.e-7);
		// The same single owner's control opens every segment; no per-cell
		// duplicate dam record or invented reservoir volume is required.
		for(auto& C:Cells)if(C.bHasDam)C.DamGateOpen01=.5f;
		const bool OpenBuilt=BuildAftershockHydraulicTerrain(Input,B,Error);AftershockWater::Solver Open;
		const bool OpenReady=OpenBuilt&&Open.Initialize(B.Domain,B.InitialCells,{},Settings,CoreError);
		const auto OpenStep=OpenReady?Open.Advance(.5,{}):AftershockWater::AdvanceResult{};
		bool AllOpen=OpenBuilt;for(const auto& F:B.Domain.Faces)if(F.Kind==AftershockWater::FaceKind::HydraulicStructure)AllOpen&=HydraulicTestNear(F.GateOpenFraction,.5);
		Check(TEXT("wide_open_gate_passes_subcrest_water_and_conserves_volume"),AllOpen&&OpenStep.Success&&Downstream(Open)>1.e-4&&FMath::Abs(Open.VolumeBudget().ResidualM3)<1.e-7);
		for(auto& C:Cells)if(C.bHasDam)C.DamGateOpen01=0.f;
		for(auto& P:Persistent)
		{
			const auto& C=Cells[P.Coord.Y*5+P.Coord.X];const double Bed=(C.Elevation-C.HydraulicExcavationCm)*.01;
			P.VolumeM3=C.bIsRiverbed&&P.Coord.Y<=1?(13.-Bed)*100.:0.;
		}
		const bool CrestBuilt=BuildAftershockHydraulicTerrain(Input,B,Error);AftershockWater::Solver Over;
		const bool OverReady=CrestBuilt&&Over.Initialize(B.Domain,B.InitialCells,{},Settings,CoreError);
		const auto OverStep=OverReady?Over.Advance(.5,{}):AftershockWater::AdvanceResult{};
		Check(TEXT("wide_closed_gate_still_allows_actual_overcrest_weir_flow"),OverStep.Success&&Downstream(Over)>1.e-4&&FMath::Abs(Over.VolumeBudget().ResidualM3)<1.e-7);
		const auto OriginalCells=Cells;const auto OriginalPersistent=Persistent;const auto PreservedFaces=B.Domain.Faces.size();
		bool Cardinal=true;
		for(int32 Turns=0;Turns<4;++Turns)
		{
			auto RotatedCells=OriginalCells;auto RotatedWater=OriginalPersistent;
			const auto Rotate=[&](FIntPoint P){for(int32 T=0;T<Turns;++T)P=FIntPoint(-P.Y,P.X);return P;};
			for(auto& C:RotatedCells){C.Coords=Rotate(C.Coords);C.DamOutflowStep=Rotate(C.DamOutflowStep);}
			for(auto& P:RotatedWater)P.Coord=Rotate(P.Coord);
			auto RotatedInput=Input;RotatedInput.Cells=RotatedCells;RotatedInput.PersistentCells=RotatedWater;FAftershockHydraulicTerrainBuild RotatedBuild;
			Cardinal&=BuildAftershockHydraulicTerrain(RotatedInput,RotatedBuild,Error)&&RotatedBuild.Diagnostics.StructureFaces==3;
			for(const auto& F:RotatedBuild.Domain.Faces)if(F.Kind==AftershockWater::FaceKind::HydraulicStructure)
				Cardinal&=HydraulicTestNear(F.WeirCrestM,12.)&&HydraulicTestNear(F.GateOpenFraction,0.);
		}
		Check(TEXT("wide_cross_section_all_four_owner_directions_and_negative_coords"),Cardinal);
		Cells[2*5+2].bHasDam=true;Cells[2*5+2].DamOutflowStep=FIntPoint(0,-1);
		Check(TEXT("wide_reciprocal_second_owner_same_faces_rejected_atomically"),!BuildAftershockHydraulicTerrain(Input,B,Error)&&B.Domain.Faces.size()==PreservedFaces);Cells=OriginalCells;Input.Cells=Cells;
		Cells.RemoveAt(1*5);Persistent.RemoveAt(1*5);Input.Cells=Cells;Input.PersistentCells=Persistent;
		Check(TEXT("wide_missing_bank_never_becomes_hidden_outlet"),!BuildAftershockHydraulicTerrain(Input,B,Error));
		Cells=OriginalCells;Persistent=OriginalPersistent;Cells[1*5].bIsOcean=true;Cells[1*5].HydraulicProfileVersion=1;Cells[1*5].HydraulicExcavationCm=2100.f;Input.Cells=Cells;Input.PersistentCells=Persistent;
		Check(TEXT("wide_ocean_is_not_an_assumed_lateral_dam_bank"),!BuildAftershockHydraulicTerrain(Input,B,Error));
		Cells=OriginalCells;Persistent=OriginalPersistent;Cells.RemoveAt(2*5+1);Persistent.RemoveAt(2*5+1);Input.Cells=Cells;Input.PersistentCells=Persistent;
		Check(TEXT("wide_missing_lateral_segment_outflow_rejected"),!BuildAftershockHydraulicTerrain(Input,B,Error));
		Cells=OriginalCells;Persistent=OriginalPersistent;Input.Cells=Cells;Input.PersistentCells=Persistent;
		Cells[1*5+1].HydraulicExcavationCm=0.f;Cells[1*5+1].Elevation=1300.f;
		Check(TEXT("wide_face_bed_above_common_crest_rejected"),!BuildAftershockHydraulicTerrain(Input,B,Error));
		Cells.Reset();Persistent.Reset();
		for(int32 Y=0;Y<2;++Y)for(int32 X=0;X<66;++X)
		{
			FGridCell C;C.Coords=FIntPoint(X,Y);C.bIsRiverbed=X>=1&&X<=64;C.Elevation=C.bIsRiverbed?1000.f:2000.f;
			C.HydraulicProfileVersion=C.bIsRiverbed?1:0;C.HydraulicExcavationCm=C.bIsRiverbed?200.f:0.f;
			if(X==32&&Y==0){C.bHasDam=true;C.DamOutflowStep=FIntPoint(0,1);C.DamCrestHeightM=2.f;}
			Cells.Add(C);Persistent.Add({C.Coords,0.,0.,0.,100.});
		}
		Input.Cells=Cells;Input.PersistentCells=Persistent;
		Check(TEXT("wide_exact64_segment_cross_section_is_bounded_and_valid"),BuildAftershockHydraulicTerrain(Input,B,Error)&&B.Diagnostics.StructureFaces==64);
		// Replace the right bank by a65th channel and add a real new bank;
		// rejected before any output changes rather than truncating the barrier.
		for(int32 Y=0;Y<2;++Y)
		{
			Cells[Y*66+65].bIsRiverbed=true;Cells[Y*66+65].Elevation=1000.f;Cells[Y*66+65].HydraulicProfileVersion=1;Cells[Y*66+65].HydraulicExcavationCm=200.f;
			FGridCell C;C.Coords=FIntPoint(66,Y);C.Elevation=2000.f;Cells.Add(C);Persistent.Add({C.Coords,0.,0.,0.,100.});
		}
		Input.Cells=Cells;Input.PersistentCells=Persistent;
		Check(TEXT("wide_65_segments_fail_closed_without_partial_barrier"),!BuildAftershockHydraulicTerrain(Input,B,Error)&&B.Diagnostics.StructureFaces==64);
	}
}
using namespace AftershockHydraulicTerrainReferencePrivate;

TArray<FAftershockHydraulicTerrainCheck> RunAftershockHydraulicTerrainReferenceChecks(UTerrainGridSubsystem& FixtureTerrain)
{
	TArray<FAftershockHydraulicTerrainCheck> Checks;
	const auto Check=[&](const TCHAR* Name,bool Passed){Checks.Add({Name,Passed});};
	TArray<FGridCell> Cells;
	for(int32 Y=-1;Y<=0;++Y)for(int32 X=-1;X<=0;++X)
	{
		FGridCell C;C.Coords=FIntPoint(X,Y);C.Elevation=1000.f;C.bIsPlayable=(Y==0);C.bIsRiverbed=(Y==-1);
		C.HydraulicProfileVersion=Y==-1?1:0;C.HydraulicExcavationCm=Y==-1?200.f:0.f;Cells.Add(C);
	}
	TArray<FAftershockHydraulicPersistentCell> Persistent;
	for(const auto& C:Cells)Persistent.Add({C.Coords,100.,.125,-.0625,100.});
	FAftershockHydraulicTerrainInput Input;Input.Cells=Cells;Input.PersistentCells=Persistent;Input.bModel2Active=true;
	FAftershockHydraulicTerrainBuild B;FText Error;
	bool Ok=BuildAftershockHydraulicTerrain(Input,B,Error);
	Check(TEXT("negative_global_coords_and_nonplayable_land_resident"),Ok&&B.Domain.Cells.size()==4&&B.Diagnostics.NonPlayableCells==2&&B.StorageIndexToCoord[0]==FIntPoint(-1,-1));
	Check(TEXT("world_centimetres_to_metres_exact_bed"),Ok&&HydraulicTestNear(B.Domain.Cells[0].BedM,8.)&&HydraulicTestNear(B.Domain.Cells[2].BedM,10.)&&HydraulicTestNear(B.InitialCells[0].DepthM,1.));
	bool Complete=Ok;TSet<int64> FaceIDs;int32 PerCell[4]={0,0,0,0};
	for(const auto& F:B.Domain.Faces){Complete&=!FaceIDs.Contains(F.StableID);FaceIDs.Add(F.StableID);++PerCell[F.Left];if(F.Right>=0)++PerCell[F.Right];}
	for(int32 Count:PerCell)Complete&=Count==4;
	Check(TEXT("complete_four_axis_faces_and_unique_stable_ids"),Complete&&B.Domain.Faces.size()==12);
	Check(TEXT("unknown_exterior_river_is_wall_not_hidden_source"),Ok&&B.Diagnostics.WallFaces==8&&B.Diagnostics.RiverStageFaces==0&&B.Diagnostics.DeclaredInflowM3PerSecond==0.);
	std::string CoreError;AftershockWater::Solver Solver;
	Check(TEXT("domain_passes_independent_core_topology_validation"),Ok&&Solver.Initialize(B.Domain,B.InitialCells,{},AftershockWater::Settings{},CoreError));
	FAftershockHydraulicBoundarySpec River;River.Coord=FIntPoint(-1,-1);River.OutwardStep=FIntPoint(0,-1);River.Kind=EAftershockHydraulicBoundary::RiverStage;River.StageElevationCm=1100.;River.VelocityCmPerSecond=FVector2d(0.,100.);
	TArray<FAftershockHydraulicBoundarySpec> Boundaries={River};Input.ExteriorBoundaries=Boundaries;
	Check(TEXT("explicit_river_stage_only"),BuildAftershockHydraulicTerrain(Input,B,Error)&&B.Diagnostics.RiverStageFaces==1&&B.Diagnostics.SeaStageFaces==0);
	Boundaries[0].Kind=EAftershockHydraulicBoundary::DeclaredCellInflow;Boundaries[0].InflowM3PerSecond=.4;
	Check(TEXT("declared_point_inflow_has_exact_si_source_and_wall_face"),BuildAftershockHydraulicTerrain(Input,B,Error)&&HydraulicTestNear(B.DefaultForcing.Cells[0].ExternalInflowM3PerS,.4)&&HydraulicTestNear(B.DefaultForcing.Cells[0].InflowVelocityYMps,1.)&&B.Diagnostics.WallFaces==8);
	const auto BeforeDomainCount=B.Domain.Cells.size();const double BeforeVolume=B.Diagnostics.InitialVolumeM3;
	const FAftershockHydraulicBoundarySpec DuplicateBoundary=Boundaries[0];
	Boundaries.Add(DuplicateBoundary);Input.ExteriorBoundaries=Boundaries;
	Check(TEXT("duplicate_exterior_boundary_rejected_before_output_mutation"),!BuildAftershockHydraulicTerrain(Input,B,Error)&&B.Domain.Cells.size()==BeforeDomainCount&&B.Diagnostics.InitialVolumeM3==BeforeVolume);
	Input.ExteriorBoundaries=TConstArrayView<FAftershockHydraulicBoundarySpec>();Persistent[0].CapturedCellAreaM2=25.;
	Check(TEXT("grid_resize_cannot_silently_reproject_saved_volume"),!BuildAftershockHydraulicTerrain(Input,B,Error));Persistent[0].CapturedCellAreaM2=100.;
	Persistent[0].VolumeM3=-.1;
	Check(TEXT("negative_persisted_volume_rejected"),!BuildAftershockHydraulicTerrain(Input,B,Error));Persistent[0].VolumeM3=100.;
	const auto Original=Cells;
	Cells[2].bIsOcean=true;Cells[2].Elevation=0.f;
	Check(TEXT("ocean_dem_without_authored_bathymetry_rejected"),!BuildAftershockHydraulicTerrain(Input,B,Error));
	Cells[2].HydraulicProfileVersion=1;Cells[2].HydraulicExcavationCm=100.f;
	Check(TEXT("explicit_ocean_bathymetry_and_declared_sea_stage"),BuildAftershockHydraulicTerrain(Input,B,Error)&&B.Diagnostics.SeaStageFaces==2&&HydraulicTestNear(B.Domain.Cells[2].BedM,-1.));
	Cells=Original;Input.Cells=Cells;Cells[0].bHasDam=true;
	Check(TEXT("unmigrated_zero_dam_outflow_fails_closed"),!BuildAftershockHydraulicTerrain(Input,B,Error));
	Cells[0].DamOutflowStep=FIntPoint(1,0);Cells[0].DamCrestHeightM=2.f;Cells[0].DamGateOpen01=.5f;
	// This existing single-segment fixture now supplies explicit resident banks
	// on both lateral sides; an absent bank is no longer silently accepted.
	for(int32 X=-1;X<=0;++X){FGridCell Bank;Bank.Coords=FIntPoint(X,-2);Bank.Elevation=2000.f;Cells.Add(Bank);Persistent.Add({Bank.Coords,0.,0.,0.,100.});}
	Input.Cells=Cells;Input.PersistentCells=Persistent;
	Ok=BuildAftershockHydraulicTerrain(Input,B,Error);const AftershockWater::FaceGeometry* Dam=nullptr;
	for(const auto& F:B.Domain.Faces)if(F.Kind==AftershockWater::FaceKind::HydraulicStructure)Dam=&F;
	Check(TEXT("dam_axis_crest_bank_relative_and_gate_invert_shared_bed"),Ok&&Dam&&B.Diagnostics.StructureFaces==1&&HydraulicTestNear(Dam->WeirCrestM,12.)&&HydraulicTestNear(Dam->GateInvertM,8.)&&HydraulicTestNear(Dam->GateOpenFraction,.5));
	Cells.SetNum(4);Persistent.SetNum(4);Input.Cells=Cells;Input.PersistentCells=Persistent;
	Cells[0].bHasDam=false;Cells[0].HydraulicExcavationCm=300.f;
	Check(TEXT("capacity_profile_rebind_preserves_m3_hu_hv"),BuildAftershockHydraulicTerrain(Input,B,Error)&&HydraulicTestNear(B.Domain.Cells[0].BedM,7.)&&HydraulicTestNear(B.InitialCells[0].DepthM,1.)&&HydraulicTestNear(B.InitialCells[0].DischargeXM2PerS,.125)&&HydraulicTestNear(B.InitialCells[0].DischargeYM2PerS,-.0625)&&HydraulicTestNear(B.Diagnostics.InitialVolumeM3,400.));
	TArray<FAftershockHydraulicPersistentCell> Captured;
	Check(TEXT("committed_capture_is_h_times_area_not_legacy_fill"),CaptureAftershockHydraulicPersistentCells(B.Domain,B.InitialCells,Captured,Error)&&Captured.Num()==4&&HydraulicTestNear(Captured[0].VolumeM3,100.)&&HydraulicTestNear(Captured[0].HuM2PerSecond,.125));
	const TSet<int64> BeforeIDs=FaceIDs;
	Swap(Cells[0],Cells[3]);Input.Cells=Cells;Ok=BuildAftershockHydraulicTerrain(Input,B,Error);TSet<int64> ReorderedIDs;
	for(const auto& F:B.Domain.Faces)ReorderedIDs.Add(F.StableID);
	Check(TEXT("storage_order_can_change_without_face_id_or_coordinate_alias"),Ok&&ReorderedIDs.Difference(BeforeIDs).IsEmpty()&&BeforeIDs.Difference(ReorderedIDs).IsEmpty()&&B.StorageIndexToCoord[0]==Cells[0].Coords);
	double Bed=0.;FGridCell Legacy=Original[0];Legacy.HydraulicProfileVersion=99;Legacy.HydraulicExcavationCm=500.f;
	Check(TEXT("inactive_legacy_bed_remains_bank_even_with_future_profile"),TryGetAftershockHydraulicBedCm(Legacy,false,Bed,Error)&&HydraulicTestNear(Bed,1000.));
	Check(TEXT("active_unknown_profile_rejected"),!TryGetAftershockHydraulicBedCm(Legacy,true,Bed,Error));

	// Actual terrain lookup is exercised here; no fabricated row-major lookup.
	FixtureTerrain.GenerateProceduralTerrain(5,5,1000.f,91);
	for(const FGridCell& Old:FixtureTerrain.Cells)
	{
		FGridCell C=Old;C.Elevation=1000.f;C.bIsPlayable=true;C.bIsRiverbed=false;C.bIsOcean=false;C.bHasDam=false;
		C.HydraulicProfileVersion=0;C.HydraulicExcavationCm=0.f;
		if((C.Coords==FIntPoint(1,2))||(C.Coords==FIntPoint(2,2)))
		{C.bIsRiverbed=true;C.HydraulicProfileVersion=1;C.HydraulicExcavationCm=C.Coords.X==1?200.f:400.f;}
		FixtureTerrain.SetCell(C.Coords,C);
	}
	const TArray<FGridCell> BeforeTerrain=FixtureTerrain.Cells;
	double H0=0.,H1=0.,Shared=0.,Bank=0.,Corner=0.,LandCentre=0.;
	Check(TEXT("actual_terrain_profile_centres_exact_and_shared_edge_continuous"),
		TryGetAftershockHydraulicGroundKnotCm(FixtureTerrain,FIntPoint(2,4),true,H0,Error)
		&&TryGetAftershockHydraulicGroundKnotCm(FixtureTerrain,FIntPoint(4,4),true,H1,Error)
		&&TryGetAftershockHydraulicGroundKnotCm(FixtureTerrain,FIntPoint(3,4),true,Shared,Error)
		&&HydraulicTestNear(H0,800.)&&HydraulicTestNear(H1,600.)&&HydraulicTestNear(Shared,700.));
	Check(TEXT("mixed_shore_edge_and_corner_interpolate_authored_cut_land_centre_exact"),
		TryGetAftershockHydraulicGroundKnotCm(FixtureTerrain,FIntPoint(2,3),true,Bank,Error)
		&&TryGetAftershockHydraulicGroundKnotCm(FixtureTerrain,FIntPoint(3,3),true,Corner,Error)
		&&TryGetAftershockHydraulicGroundKnotCm(FixtureTerrain,FIntPoint(2,2),true,LandCentre,Error)
		&&HydraulicTestNear(Bank,900.)&&HydraulicTestNear(Corner,850.)&&HydraulicTestNear(LandCentre,1000.));
	FAftershockHydraulicGroundPatch Patch;
	Ok=BuildAftershockHydraulicGroundPatch(FixtureTerrain,FIntRect(0,0,5,5),true,Patch,Error);
	int32 SharedCount=0;bool HasCentreA=false,HasCentreB=false;
	for(const auto& V:Patch.Vertices)
	{
		const double GX=(V.Position.X-FixtureTerrain.GridOrigin.X)/1000.-.5,GY=(V.Position.Y-FixtureTerrain.GridOrigin.Y)/1000.-.5;
		if(HydraulicTestNear(GX,1.5)&&HydraulicTestNear(GY,2.))++SharedCount;
		if(HydraulicTestNear(GX,1.)&&HydraulicTestNear(GY,2.)&&HydraulicTestNear(V.Position.Z,800.))HasCentreA=true;
		if(HydraulicTestNear(GX,2.)&&HydraulicTestNear(GY,2.)&&HydraulicTestNear(V.Position.Z,600.))HasCentreB=true;
	}
	Check(TEXT("continuous_mesh_shares_edge_vertices_no_vertical_cell_slabs"),Ok&&Patch.ProfileCells==2&&Patch.Indices.Num()==48&&Patch.Vertices.Num()==15&&SharedCount==1&&HasCentreA&&HasCentreB);
	Check(TEXT("read_only_adapter_does_not_mutate_dem_flags_or_bank_records"),HydraulicTestSameCells(BeforeTerrain,FixtureTerrain.Cells));
	FAftershockHydraulicGroundPatch Inactive;
	Check(TEXT("inactive_model_does_not_add_any_profile_geometry"),BuildAftershockHydraulicGroundPatch(FixtureTerrain,FIntRect(0,0,5,5),false,Inactive,Error)&&Inactive.Vertices.IsEmpty()&&Inactive.Indices.IsEmpty());
	for(FIntPoint Coord:{FIntPoint(1,3),FIntPoint(2,3)})
	{
		FGridCell C=FixtureTerrain.GetCell(Coord);C.bIsRiverbed=true;C.HydraulicProfileVersion=1;
		C.HydraulicExcavationCm=Coord.X==1?300.f:500.f;FixtureTerrain.SetCell(Coord,C);
	}
	double FourWay=0.;
	Check(TEXT("four_profile_corner_is_one_continuous_shared_value"),TryGetAftershockHydraulicGroundKnotCm(FixtureTerrain,FIntPoint(3,5),true,FourWay,Error)&&HydraulicTestNear(FourWay,650.));
	const bool Expanded=FixtureTerrain.EnsureChunkGenerated(FIntPoint(-1,0),Error);
	if(Expanded)for(FIntPoint Coord:{FIntPoint(-1,2),FIntPoint(0,2)})
	{
		FGridCell C=FixtureTerrain.GetCell(Coord);C.Elevation=1000.f;C.bIsRiverbed=true;C.bIsOcean=false;
		C.HydraulicProfileVersion=1;C.HydraulicExcavationCm=Coord.X==-1?200.f:400.f;FixtureTerrain.SetCell(Coord,C);
	}
	double NegativeShared=0.;
	Check(TEXT("negative_half_grid_floor_division_and_actual_terrain_lookup"),Expanded&&TryGetAftershockHydraulicGroundKnotCm(FixtureTerrain,FIntPoint(-1,4),true,NegativeShared,Error)&&HydraulicTestNear(NegativeShared,700.));
	FGridCell Mineral=FixtureTerrain.GetCell(FIntPoint(1,2));
	Mineral.GroundErodedDepthM=.1f;Mineral.GroundDepositedDepthM=.25f;FixtureTerrain.SetCell(Mineral.Coords,Mineral);
	double MaterialGround=0.,MineralBed=0.,MineralKnot=0.;
	Check(TEXT("mineral_overlay_domain_and_render_knot_share_one_immutable_base"),
		TryGetAftershockMaterialGroundCm(Mineral,true,MaterialGround,Error)&&HydraulicTestNear(MaterialGround,1015.,1e-4)
		&&TryGetAftershockHydraulicBedCm(Mineral,true,MineralBed,Error)&&HydraulicTestNear(MineralBed,815.,1e-4)
		&&TryGetAftershockHydraulicGroundKnotCm(FixtureTerrain,FIntPoint(2,4),true,MineralKnot,Error)&&HydraulicTestNear(MineralKnot,MineralBed)
		&&FixtureTerrain.GetCell(Mineral.Coords).Elevation==1000.f);
	Mineral.GroundErodedDepthM=-.1f;
	Check(TEXT("negative_mineral_overlay_is_rejected"),!TryGetAftershockHydraulicBedCm(Mineral,true,MineralBed,Error));
	Mineral.GroundErodedDepthM=Mineral.GroundDepositedDepthM=0.f;FixtureTerrain.SetCell(Mineral.Coords,Mineral);
	AppendHydraulicWideDamChecks(Checks);
	return Checks;
}

#if WITH_EDITOR
bool RunAftershockHydraulicTerrainFixtures(const FString& AbsoluteReportPath)
{
	auto Report=MakeShared<FJsonObject>();Report->SetNumberField(TEXT("schema_version"),1);
	Report->SetBoolField(TEXT("valid"),false);Report->SetBoolField(TEXT("in_progress"),true);
	Report->SetStringField(TEXT("scope"),TEXT("Isolated synthetic canonical terrain adapter/domain/shared-knot fixtures; not live hydraulic lifecycle, historical-save migration, real RHI or cooked collision acceptance."));
	const auto Write=[&]()
	{
		FString Output;const auto Writer=TJsonWriterFactory<>::Create(&Output);
		if(!FJsonSerializer::Serialize(Report,Writer))return false;
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsoluteReportPath),true);
		return FFileHelper::SaveStringToFile(Output,*AbsoluteReportPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	};
	if(!Write())return false;
	struct FHydraulicTerrainFixtureWorld
	{
		UWorld* World=nullptr;
		~FHydraulicTerrainFixtureWorld(){if(World){World->DestroyWorld(false);if(GEngine)GEngine->DestroyWorldContext(World);World->RemoveFromRoot();}}
	} Fixture;
	const double Started=FPlatformTime::Seconds();TArray<FAftershockHydraulicTerrainCheck> Checks;
	if(GEngine)
	{
		Fixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("AftershockHydraulicTerrainFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
		if(Fixture.World)
		{
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(Fixture.World);
			TStrongObjectPtr<UTerrainGridSubsystem> Terrain(NewObject<UTerrainGridSubsystem>(Fixture.World));
			if(Terrain.Get())Checks=RunAftershockHydraulicTerrainReferenceChecks(*Terrain.Get());
		}
	}
	int32 Failed=0;TArray<TSharedPtr<FJsonValue>> Rows;
	for(const auto& Check:Checks)
	{
		auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("name"),Check.Name);Row->SetBoolField(TEXT("passed"),Check.bPassed);Rows.Add(MakeShared<FJsonValueObject>(Row));
		Failed+=Check.bPassed?0:1;
		if(!Check.bPassed){UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_HYDRAULIC_TERRAIN_FAIL %s"),*Check.Name);}
	}
	const bool Valid=Checks.Num()==41&&Failed==0;
	Report->SetArrayField(TEXT("checks"),Rows);Report->SetNumberField(TEXT("passed"),Checks.Num()-Failed);Report->SetNumberField(TEXT("failed"),Failed);
	Report->SetNumberField(TEXT("compute_ms"),(FPlatformTime::Seconds()-Started)*1000.);Report->SetBoolField(TEXT("valid"),Valid);Report->SetBoolField(TEXT("in_progress"),false);
	Report->SetBoolField(TEXT("actual_collision_trace_executed"),false);Report->SetNumberField(TEXT("actual_collision_checks_pending"),4);
	const bool Saved=Write();UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_HYDRAULIC_TERRAIN_RESULT checked=%d failed=%d valid=%d saved=%d"),Checks.Num(),Failed,Valid,Saved);
	return Valid&&Saved;
}
#endif

TArray<FAftershockHydraulicTerrainCheck> RunAftershockHydraulicGroundCollisionChecks(const UTerrainGridSubsystem& FixtureTerrain,UStaticMeshComponent& BuiltGround)
{
	TArray<FAftershockHydraulicTerrainCheck> Checks;
	const FIntPoint Targets[]={FIntPoint(1,2),FIntPoint(2,2),FIntPoint(1,1)};
	for(const FIntPoint Coord:Targets)
	{
		FText Error;double Expected=0.;FHitResult Hit;FCollisionQueryParams Query(SCENE_QUERY_STAT(AftershockHydraulicGroundAudit),true);
		const FVector Bank=FixtureTerrain.GridToWorld(Coord);
		const bool HasExpected=TryGetAftershockHydraulicCellGroundCm(FixtureTerrain,Coord,true,Expected,Error);
		const bool Passed=HasExpected&&BuiltGround.LineTraceComponent(Hit,FVector(Bank.X,Bank.Y,Expected+10000.),FVector(Bank.X,Bank.Y,Expected-10000.),Query)&&HydraulicTestNear(Hit.ImpactPoint.Z,Expected,.5);
		Checks.Add({FString::Printf(TEXT("actual_cooked_ground_trace_matches_shared_profile_or_bank_%d_%d"),Coord.X,Coord.Y),Passed});
	}
	FText Error;double Shared=0.;FHitResult Hit;FCollisionQueryParams Query(SCENE_QUERY_STAT(AftershockHydraulicGroundSeamAudit),true);
	const double WX=FixtureTerrain.GridOrigin.X+2.*FixtureTerrain.CellSize,WY=FixtureTerrain.GridOrigin.Y+2.5*FixtureTerrain.CellSize;
	const bool SharedPassed=TryGetAftershockHydraulicGroundKnotCm(FixtureTerrain,FIntPoint(3,4),true,Shared,Error)
		&&BuiltGround.LineTraceComponent(Hit,FVector(WX,WY,Shared+10000.),FVector(WX,WY,Shared-10000.),Query)&&HydraulicTestNear(Hit.ImpactPoint.Z,Shared,.5);
	Checks.Add({TEXT("actual_cooked_ground_shared_channel_edge_has_no_collision_seam"),SharedPassed});
	return Checks;
}
