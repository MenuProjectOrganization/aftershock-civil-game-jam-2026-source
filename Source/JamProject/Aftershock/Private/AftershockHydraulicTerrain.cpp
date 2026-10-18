#include "AftershockHydraulicTerrain.h"
#include <cmath>

namespace AftershockHydraulicTerrainPrivate
{
	constexpr int32 HydraulicCoordLimit = 1000000;
	constexpr int64 HydraulicCoordBias = 1048576;
	constexpr double HydraulicCmToM = .01;
	const FIntPoint HydraulicSteps[4] = { FIntPoint(0,-1),FIntPoint(1,0),FIntPoint(0,1),FIntPoint(-1,0) };
	bool HydraulicFinite(double V) { return std::isfinite(V); }
	bool HydraulicAxis(FIntPoint S) { return (S.X==0 && (S.Y==1||S.Y==-1)) || (S.Y==0 && (S.X==1||S.X==-1)); }
	bool HydraulicValidCoord(FIntPoint C) { return FMath::Abs(static_cast<int64>(C.X))<=HydraulicCoordLimit && FMath::Abs(static_cast<int64>(C.Y))<=HydraulicCoordLimit; }
	bool HydraulicFail(FText& Error,const FString& Reason) { Error=FText::FromString(Reason);return false; }
	FString HydraulicAt(FIntPoint C) { return FString::Printf(TEXT("(%d,%d)"),C.X,C.Y); }
	// One ID per geometric face, shared by the two cells. Encoding includes the
	// lower corner and axis; it does not depend on append/storage order.
	int64 HydraulicFaceID(FIntPoint C,FIntPoint S)
	{
		if(S.X<0)--C.X; if(S.Y<0)--C.Y;
		return ((static_cast<int64>(C.X)+HydraulicCoordBias)<<22) | ((static_cast<int64>(C.Y)+HydraulicCoordBias)<<1) | (S.Y!=0?1ll:0ll);
	}
	bool HydraulicStateValid(const FAftershockHydraulicPersistentCell& P,double Area)
	{
		const double H=P.VolumeM3/Area;
		return HydraulicFinite(P.VolumeM3)&&P.VolumeM3>=0.&&HydraulicFinite(P.HuM2PerSecond)&&HydraulicFinite(P.HvM2PerSecond)
			&&HydraulicFinite(P.CapturedCellAreaM2)&&FMath::Abs(P.CapturedCellAreaM2-Area)<=FMath::Max(1e-10,Area*1e-12)
			&&HydraulicFinite(H)&&HydraulicFinite(AftershockWater::Gravity*H*H)
			&&(H>0.||(P.HuM2PerSecond==0.&&P.HvM2PerSecond==0.));
	}
	bool HydraulicSampleMaterialBank(const UTerrainGridSubsystem& Terrain,double GX,double GY,double& Out,FText& Error)
	{
		float Original=0.f;
		if(!Terrain.TrySampleResidentElevation(static_cast<float>(GX),static_cast<float>(GY),Original)||!HydraulicFinite(Original))return HydraulicFail(Error,TEXT("Missing finite canonical bank knot"));
		const FIntRect Bounds=Terrain.GetResidentCellBounds();if(Bounds.IsEmpty())return HydraulicFail(Error,TEXT("No resident material bank"));
		GX=FMath::Clamp(GX,static_cast<double>(Bounds.Min.X),static_cast<double>(Bounds.Max.X-1));
		GY=FMath::Clamp(GY,static_cast<double>(Bounds.Min.Y),static_cast<double>(Bounds.Max.Y-1));
		const int32 X0=FMath::FloorToInt(GX),Y0=FMath::FloorToInt(GY),X1=FMath::Min(X0+1,Bounds.Max.X-1),Y1=FMath::Min(Y0+1,Bounds.Max.Y-1);
		const FIntPoint Coords[]={FIntPoint(X0,Y0),FIntPoint(X1,Y0),FIntPoint(X0,Y1),FIntPoint(X1,Y1)};
		FGridCell C[4];bool Valid[4];double Offset[4]={0.,0.,0.,0.};
		for(int32 I=0;I<4;++I)
		{
			Valid[I]=Terrain.TryGetCell(Coords[I],C[I]);if(!Valid[I])continue;
			double MaterialCm=0.;if(!TryGetAftershockMaterialGroundCm(C[I],true,MaterialCm,Error))return false;
			Offset[I]=MaterialCm-static_cast<double>(C[I].Elevation);
		}
		const double FX=GX-X0,FY=GY-Y0;
		double W[]={(1.-FX)*(1.-FY),FX*(1.-FY),(1.-FX)*FY,FX*FY};
		// Match the existing canonical bank sampler's river-diagonal weights;
		// apply ONLY the mineral offset. DEM and its original interpolation stay.
		if(Valid[0]&&Valid[1]&&Valid[2]&&Valid[3])
		{
			if(C[0].bIsPlayable&&C[3].bIsPlayable&&C[0].bIsRiverbed&&C[3].bIsRiverbed&&!C[1].bIsRiverbed&&!C[2].bIsRiverbed)
			{if(FX>=FY){W[0]=1.-FX;W[1]=FX-FY;W[2]=0.;W[3]=FY;}else{W[0]=1.-FY;W[1]=0.;W[2]=FY-FX;W[3]=FX;}}
			else if(C[1].bIsPlayable&&C[2].bIsPlayable&&C[1].bIsRiverbed&&C[2].bIsRiverbed&&!C[0].bIsRiverbed&&!C[3].bIsRiverbed)
			{if(FX+FY<=1.){W[0]=1.-FX-FY;W[1]=FX;W[2]=FY;W[3]=0.;}else{W[0]=0.;W[1]=1.-FY;W[2]=1.-FX;W[3]=FX+FY-1.;}}
		}
		double Sum=0.,Weight=0.,Nearest=0.,NearestDistance=TNumericLimits<double>::Max();bool Any=false;
		for(int32 I=0;I<4;++I)if(Valid[I])
		{
			Any=true;Sum+=W[I]*Offset[I];Weight+=W[I];const double D=FMath::Square(GX-Coords[I].X)+FMath::Square(GY-Coords[I].Y);
			if(D<NearestDistance){NearestDistance=D;Nearest=Offset[I];}
		}
		if(!Any)return HydraulicFail(Error,TEXT("No resident material bank neighbours"));
		const double Candidate=Original+(Weight>SMALL_NUMBER?Sum/Weight:Nearest);
		if(!HydraulicFinite(Candidate))return HydraulicFail(Error,TEXT("Material bank knot overflows"));
		Out=Candidate;return true;
	}
}
using namespace AftershockHydraulicTerrainPrivate;

bool TryGetAftershockMaterialGroundCm(const FGridCell& Cell,bool bUsePhysicalOverlays,double& OutGroundCm,FText& OutError)
{
	if(!HydraulicFinite(Cell.Elevation))return HydraulicFail(OutError,TEXT("Non-finite canonical bank elevation at ")+HydraulicAt(Cell.Coords));
	double Candidate=Cell.Elevation;
	if(bUsePhysicalOverlays)
	{
		if(!HydraulicFinite(Cell.GroundErodedDepthM)||!HydraulicFinite(Cell.GroundDepositedDepthM)||Cell.GroundErodedDepthM<0.f||Cell.GroundDepositedDepthM<0.f)
			return HydraulicFail(OutError,TEXT("Invalid canonical mineral overlay at ")+HydraulicAt(Cell.Coords));
		Candidate+=100.*(static_cast<double>(Cell.GroundDepositedDepthM)-Cell.GroundErodedDepthM);
	}
	if(!HydraulicFinite(Candidate))return HydraulicFail(OutError,TEXT("Material ground overflows at ")+HydraulicAt(Cell.Coords));
	OutGroundCm=Candidate;OutError=FText::GetEmpty();return true;
}

bool TryGetAftershockHydraulicBedCm(const FGridCell& Cell,bool bModel2Active,double& OutBedCm,FText& OutError)
{
	double Candidate=0.;if(!TryGetAftershockMaterialGroundCm(Cell,bModel2Active,Candidate,OutError))return false;
	if(bModel2Active)
	{
		if(Cell.HydraulicProfileVersion<0||Cell.HydraulicProfileVersion>1 || !HydraulicFinite(Cell.HydraulicExcavationCm)||Cell.HydraulicExcavationCm<0.f)
			return HydraulicFail(OutError,TEXT("Unsupported or invalid authored hydraulic profile at ")+HydraulicAt(Cell.Coords));
		if(Cell.HydraulicProfileVersion==0 && Cell.HydraulicExcavationCm!=0.f)
			return HydraulicFail(OutError,TEXT("Excavation has no explicit hydraulic profile version at ")+HydraulicAt(Cell.Coords));
		if(Cell.HydraulicExcavationCm>0.f && !Cell.bIsRiverbed && !Cell.bIsOcean)
			return HydraulicFail(OutError,TEXT("Excavated cell is not a canonical channel/ocean cell at ")+HydraulicAt(Cell.Coords));
		Candidate-=static_cast<double>(Cell.HydraulicExcavationCm);
	}
	if(!HydraulicFinite(Candidate))return HydraulicFail(OutError,TEXT("Hydraulic bed overflows at ")+HydraulicAt(Cell.Coords));
	OutBedCm=Candidate;OutError=FText::GetEmpty();return true;
}

bool IsAftershockHydraulicExcavatedCell(const FGridCell& Cell,bool bModel2Active)
{
	return bModel2Active && Cell.HydraulicProfileVersion==1 && HydraulicFinite(Cell.HydraulicExcavationCm) && Cell.HydraulicExcavationCm>0.f;
}

bool BuildAftershockHydraulicTerrain(const FAftershockHydraulicTerrainInput& Input,FAftershockHydraulicTerrainBuild& OutBuild,FText& OutError)
{
	if(!Input.bModel2Active || Input.SourceTerrainVersion!=3)
		return HydraulicFail(OutError,TEXT("Model2 requires an explicit V3 world-centimetre activation/migration transaction"));
	const double SizeM=Input.CellSizeCm*HydraulicCmToM,Area=SizeM*SizeM;
	if(Input.Cells.IsEmpty()||Input.Cells.Num()>static_cast<int32>(AftershockWater::MaximumCells)
		||Input.PersistentCells.Num()!=Input.Cells.Num()||!HydraulicFinite(SizeM)||SizeM<=0.||!HydraulicFinite(Area)||Area<=0.
		||!HydraulicFinite(Input.CoastalSeaLevelCm)||!HydraulicFinite(Input.DefaultManningN)||Input.DefaultManningN<0.
		||(!Input.ManningNByStorageIndex.IsEmpty()&&Input.ManningNByStorageIndex.Num()!=Input.Cells.Num())
		||Input.ExteriorBoundaries.Num()>Input.Cells.Num()*4
		||!HydraulicFinite(Input.AuthoredGateHeightM)||Input.AuthoredGateHeightM<0.
		||!HydraulicFinite(Input.AuthoredWeirCoefficientM05PerS)||Input.AuthoredWeirCoefficientM05PerS<0.
		||!HydraulicFinite(Input.AuthoredGateDischargeCoefficient)||Input.AuthoredGateDischargeCoefficient<0.)
		return HydraulicFail(OutError,TEXT("Invalid hydraulic terrain dimensions, counts, sea level or authored coefficients"));
	TMap<FIntPoint,int32> IndexByCoord,PersistentByCoord;IndexByCoord.Reserve(Input.Cells.Num());PersistentByCoord.Reserve(Input.Cells.Num());
	for(int32 I=0;I<Input.Cells.Num();++I)
	{
		const FIntPoint C=Input.Cells[I].Coords;
		if(!HydraulicValidCoord(C)||IndexByCoord.Contains(C))return HydraulicFail(OutError,TEXT("Invalid/duplicate resident coordinate ")+HydraulicAt(C));
		IndexByCoord.Add(C,I);
	}
	for(int32 I=0;I<Input.PersistentCells.Num();++I)
	{
		const auto& P=Input.PersistentCells[I];
		if(!IndexByCoord.Contains(P.Coord)||PersistentByCoord.Contains(P.Coord)||!HydraulicStateValid(P,Area))
			return HydraulicFail(OutError,TEXT("Missing/duplicate/non-finite volume or incompatible cell area at ")+HydraulicAt(P.Coord));
		PersistentByCoord.Add(P.Coord,I);
	}
	FAftershockHydraulicTerrainBuild Candidate;Candidate.Domain.CellWidthM=SizeM;Candidate.Domain.CellHeightM=SizeM;
	Candidate.Domain.Cells.reserve(Input.Cells.Num());Candidate.InitialCells.reserve(Input.Cells.Num());
	Candidate.Domain.Faces.reserve(Input.Cells.Num()*2+4);Candidate.StorageIndexToCoord.Reserve(Input.Cells.Num());
	Candidate.DefaultForcing.Cells.resize(Input.Cells.Num());
	Candidate.Diagnostics.ResidentCells=Input.Cells.Num();
	for(int32 I=0;I<Input.Cells.Num();++I)
	{
		const FGridCell& C=Input.Cells[I];double BedCm=0.;
		if(!TryGetAftershockHydraulicBedCm(C,true,BedCm,OutError))return false;
		if(C.bIsOcean && BedCm>=Input.CoastalSeaLevelCm)
			return HydraulicFail(OutError,TEXT("Ocean cell needs explicit authored below-sea hydraulic bathymetry; DEM is not bathymetry at ")+HydraulicAt(C.Coords));
		const double N=Input.ManningNByStorageIndex.IsEmpty()?Input.DefaultManningN:Input.ManningNByStorageIndex[I];
		if(!HydraulicFinite(N)||N<0.)return HydraulicFail(OutError,TEXT("Invalid Manning coefficient at ")+HydraulicAt(C.Coords));
		const auto& P=Input.PersistentCells[PersistentByCoord.FindChecked(C.Coords)];
		const double H=P.VolumeM3/Area;
		if(!HydraulicFinite(BedCm*HydraulicCmToM+H))return HydraulicFail(OutError,TEXT("Initial surface elevation overflows at ")+HydraulicAt(C.Coords));
		Candidate.Domain.Cells.push_back({C.Coords.X,C.Coords.Y,BedCm*HydraulicCmToM,N});
		Candidate.InitialCells.push_back({H,P.HuM2PerSecond,P.HvM2PerSecond});Candidate.StorageIndexToCoord.Add(C.Coords);
		Candidate.Diagnostics.NonPlayableCells+=C.bIsPlayable?0:1;Candidate.Diagnostics.ExcavatedCells+=IsAftershockHydraulicExcavatedCell(C,true)?1:0;
		Candidate.Diagnostics.InitialVolumeM3+=P.VolumeM3;
		if(!HydraulicFinite(Candidate.Diagnostics.InitialVolumeM3))return HydraulicFail(OutError,TEXT("Initial hydraulic volume sum overflows"));
	}
	TMap<int64,int32> ExplicitBoundaryByFace;
	for(int32 I=0;I<Input.ExteriorBoundaries.Num();++I)
	{
		const auto& B=Input.ExteriorBoundaries[I];const int32* CellIndex=IndexByCoord.Find(B.Coord);
		if(!CellIndex||!HydraulicAxis(B.OutwardStep)||IndexByCoord.Contains(B.Coord+B.OutwardStep)
			||!HydraulicFinite(B.StageElevationCm)||!HydraulicFinite(B.VelocityCmPerSecond.X)||!HydraulicFinite(B.VelocityCmPerSecond.Y)
			||!HydraulicFinite(B.InflowM3PerSecond)||B.InflowM3PerSecond<0.
			||static_cast<uint8>(B.Kind)>static_cast<uint8>(EAftershockHydraulicBoundary::DeclaredCellInflow))
			return HydraulicFail(OutError,TEXT("Invalid or non-exterior declared boundary at ")+HydraulicAt(B.Coord));
		const int64 Key=HydraulicFaceID(B.Coord,B.OutwardStep);
		if(ExplicitBoundaryByFace.Contains(Key))return HydraulicFail(OutError,TEXT("Duplicate exterior boundary at ")+HydraulicAt(B.Coord));
		const auto& C=Input.Cells[*CellIndex];
		if((B.Kind==EAftershockHydraulicBoundary::SeaStage&&!C.bIsOcean)
			||((B.Kind==EAftershockHydraulicBoundary::RiverStage||B.Kind==EAftershockHydraulicBoundary::DeclaredCellInflow)&&!C.bIsRiverbed)
			||(B.Kind!=EAftershockHydraulicBoundary::DeclaredCellInflow&&B.InflowM3PerSecond!=0.))
			return HydraulicFail(OutError,TEXT("Boundary classification/source disagrees with canonical cell at ")+HydraulicAt(B.Coord));
		ExplicitBoundaryByFace.Add(Key,I);
	}
	TMap<int64,int32> DamByFace;
	for(int32 I=0;I<Input.Cells.Num();++I)
	{
		const auto& C=Input.Cells[I];if(!C.bHasDam)continue;
		const int32* Other=HydraulicAxis(C.DamOutflowStep)?IndexByCoord.Find(C.Coords+C.DamOutflowStep):nullptr;
		if(!C.bIsRiverbed||!Other||(!Input.Cells[*Other].bIsRiverbed&&!Input.Cells[*Other].bIsOcean)
			||!HydraulicFinite(C.DamCrestHeightM)||C.DamCrestHeightM<0.f||!HydraulicFinite(C.DamGateOpen01)||C.DamGateOpen01<0.f||C.DamGateOpen01>1.f)
			return HydraulicFail(OutError,TEXT("Dam requires a migrated cardinal resident channel outflow, crest and gate at ")+HydraulicAt(C.Coords));
		// One physical dam spans the contiguous channel cross-section, not only
		// its owner cell. A missing resident bank is UNKNOWN geometry, never a
		// silent open end/outlet. Cardinal owner direction is the sole axis.
		const FIntPoint Cross(-C.DamOutflowStep.Y,C.DamOutflowStep.X);
		int32 Segments=0;
		const auto AddSegment=[&](FIntPoint At)
		{
			const int32* Upstream=IndexByCoord.Find(At);
			const int32* Downstream=IndexByCoord.Find(At+C.DamOutflowStep);
			if(!Upstream||!Downstream||!Input.Cells[*Upstream].bIsRiverbed||Input.Cells[*Upstream].bIsOcean
				||(!Input.Cells[*Downstream].bIsRiverbed&&!Input.Cells[*Downstream].bIsOcean))
				return HydraulicFail(OutError,TEXT("Dam cross-section has missing or non-channel resident outflow at ")+HydraulicAt(At));
			if(++Segments>64)return HydraulicFail(OutError,TEXT("Dam cross-section exceeds64 resident channel segments at ")+HydraulicAt(C.Coords));
			const int64 Key=HydraulicFaceID(At,C.DamOutflowStep);
			if(DamByFace.Contains(Key))return HydraulicFail(OutError,TEXT("Two dam owners claim the same cross-section face at ")+HydraulicAt(At));
			DamByFace.Add(Key,I);return true;
		};
		if(!AddSegment(C.Coords))return false;
		for(int32 Sign:{-1,1})
		{
			bool BankFound=false;
			for(int32 Offset=1;Offset<=65;++Offset)
			{
				const FIntPoint At=C.Coords+Cross*(Sign*Offset);
				const int32* Index=IndexByCoord.Find(At);
				if(!Index||Input.Cells[*Index].bIsOcean)
					return HydraulicFail(OutError,TEXT("Dam cross-section requires a known resident non-ocean bank at ")+HydraulicAt(At));
				if(!Input.Cells[*Index].bIsRiverbed){BankFound=true;break;}
				if(!AddSegment(At))return false;
			}
			if(!BankFound)return HydraulicFail(OutError,TEXT("Dam cross-section has no bounded resident bank at ")+HydraulicAt(C.Coords));
		}
	}
	for(int32 I=0;I<Input.Cells.Num();++I)
	{
		const auto& C=Input.Cells[I];
		for(FIntPoint Step:HydraulicSteps)
		{
			const int32* Other=IndexByCoord.Find(C.Coords+Step);
			if(Other && (Step.X<0||Step.Y<0))continue; // Interior face exactly once.
			AftershockWater::FaceGeometry F;F.Left=I;F.Right=Other?*Other:-1;
			F.NormalX=Step.X;F.NormalY=Step.Y;F.LengthM=SizeM;F.StableID=HydraulicFaceID(C.Coords,Step);
			if(Other)
			{
				F.Kind=AftershockWater::FaceKind::Interior;++Candidate.Diagnostics.InteriorFaces;
				if(const int32* DamIndex=DamByFace.Find(F.StableID))
				{
					const auto& D=Input.Cells[*DamIndex];F.Kind=AftershockWater::FaceKind::HydraulicStructure;
					F.WeirCrestM=static_cast<double>(D.Elevation)*HydraulicCmToM+D.DamCrestHeightM;
					// The shared orifice cannot conduct below either adjacent local
					// bed. Lateral channel cuts may differ from the owner's bed.
					F.GateInvertM=FMath::Max(Candidate.Domain.Cells[I].BedM,Candidate.Domain.Cells[*Other].BedM);
					F.GateHeightM=FMath::Min(Input.AuthoredGateHeightM,F.WeirCrestM-F.GateInvertM);
					F.GateOpenFraction=D.DamGateOpen01;F.WeirCoefficientM05PerS=Input.AuthoredWeirCoefficientM05PerS;
					F.GateDischargeCoefficient=Input.AuthoredGateDischargeCoefficient;
					if(!HydraulicFinite(F.WeirCrestM)||!HydraulicFinite(F.GateHeightM)||F.GateHeightM<0.)return HydraulicFail(OutError,TEXT("Dam head/geometry overflows at ")+HydraulicAt(D.Coords));
					++Candidate.Diagnostics.StructureFaces;
				}
			}
			else
			{
				const int32* SpecIndex=ExplicitBoundaryByFace.Find(F.StableID);
				const auto Kind=SpecIndex?Input.ExteriorBoundaries[*SpecIndex].Kind:(C.bIsOcean?EAftershockHydraulicBoundary::SeaStage:EAftershockHydraulicBoundary::Wall);
				const auto* B=SpecIndex?&Input.ExteriorBoundaries[*SpecIndex]:nullptr;
				F.Kind=AftershockWater::FaceKind::Wall;
				if(Kind==EAftershockHydraulicBoundary::SeaStage||Kind==EAftershockHydraulicBoundary::RiverStage)
				{
					F.Kind=AftershockWater::FaceKind::StageBoundary;F.StageHeadM=(B?B->StageElevationCm:Input.CoastalSeaLevelCm)*HydraulicCmToM;
					F.StageVelocityXMps=B?B->VelocityCmPerSecond.X*HydraulicCmToM:0.;F.StageVelocityYMps=B?B->VelocityCmPerSecond.Y*HydraulicCmToM:0.;
					if(Kind==EAftershockHydraulicBoundary::SeaStage)++Candidate.Diagnostics.SeaStageFaces;else ++Candidate.Diagnostics.RiverStageFaces;
				}
				else if(Kind==EAftershockHydraulicBoundary::FreeOutflow){F.Kind=AftershockWater::FaceKind::FreeOutflow;++Candidate.Diagnostics.FreeOutflowFaces;}
				else
				{
					++Candidate.Diagnostics.WallFaces;
					if(Kind==EAftershockHydraulicBoundary::DeclaredCellInflow)
					{
						// Multiple declared inflows into one cell combine momentum-weighted.
						auto& In=Candidate.DefaultForcing.Cells[I];const double Old=In.ExternalInflowM3PerS,Added=B->InflowM3PerSecond,Total=Old+Added;
						if(!HydraulicFinite(Total)||!HydraulicFinite(Old*In.InflowVelocityXMps+Added*B->VelocityCmPerSecond.X*HydraulicCmToM)
							||!HydraulicFinite(Old*In.InflowVelocityYMps+Added*B->VelocityCmPerSecond.Y*HydraulicCmToM))return HydraulicFail(OutError,TEXT("Declared river inflow overflows at ")+HydraulicAt(C.Coords));
						In.InflowVelocityXMps=Total>0.?(Old*In.InflowVelocityXMps+Added*B->VelocityCmPerSecond.X*HydraulicCmToM)/Total:0.;
						In.InflowVelocityYMps=Total>0.?(Old*In.InflowVelocityYMps+Added*B->VelocityCmPerSecond.Y*HydraulicCmToM)/Total:0.;
						In.ExternalInflowM3PerS=Total;++Candidate.Diagnostics.DeclaredInflowFaces;Candidate.Diagnostics.DeclaredInflowM3PerSecond+=Added;
						if(!HydraulicFinite(Candidate.Diagnostics.DeclaredInflowM3PerSecond))return HydraulicFail(OutError,TEXT("Declared inflow sum overflows"));
					}
				}
			}
			Candidate.Domain.Faces.push_back(F);
		}
	}
	// Core initialization performs its independent full topology/state checks.
	// This adapter deliberately does not construct/reset a live solver here.
	OutBuild=MoveTemp(Candidate);OutError=FText::GetEmpty();return true;
}

bool CaptureAftershockHydraulicPersistentCells(const AftershockWater::Domain& Geometry,const std::vector<AftershockWater::CellState>& Cells,TArray<FAftershockHydraulicPersistentCell>& OutCells,FText& OutError)
{
	const double Area=Geometry.CellWidthM*Geometry.CellHeightM;
	if(Cells.size()!=Geometry.Cells.size()||Cells.empty()||Cells.size()>AftershockWater::MaximumCells||!HydraulicFinite(Area)||Area<=0.)return HydraulicFail(OutError,TEXT("Invalid committed hydraulic capture geometry"));
	TArray<FAftershockHydraulicPersistentCell> Candidate;Candidate.Reserve(static_cast<int32>(Cells.size()));TSet<FIntPoint> Seen;
	for(std::size_t I=0;I<Cells.size();++I)
	{
		const auto& G=Geometry.Cells[I];const auto& S=Cells[I];FAftershockHydraulicPersistentCell P;
		P.Coord=FIntPoint(G.X,G.Y);P.VolumeM3=S.DepthM*Area;P.HuM2PerSecond=S.DischargeXM2PerS;P.HvM2PerSecond=S.DischargeYM2PerS;P.CapturedCellAreaM2=Area;
		if(!HydraulicValidCoord(P.Coord)||Seen.Contains(P.Coord)||!HydraulicStateValid(P,Area))return HydraulicFail(OutError,TEXT("Invalid committed hydraulic state at ")+HydraulicAt(P.Coord));
		Seen.Add(P.Coord);Candidate.Add(P);
	}
	OutCells=MoveTemp(Candidate);OutError=FText::GetEmpty();return true;
}

bool TryGetAftershockHydraulicCellGroundCm(const UTerrainGridSubsystem& Terrain,FIntPoint Coord,bool bModel2Active,double& OutGroundCm,FText& OutError)
{
	FGridCell Cell;if(!Terrain.TryGetCell(Coord,Cell))return HydraulicFail(OutError,TEXT("Ground query has no resident hydraulic cell at ")+HydraulicAt(Coord));
	return TryGetAftershockHydraulicBedCm(Cell,bModel2Active,OutGroundCm,OutError);
}

bool TryGetAftershockHydraulicGroundKnotCm(const UTerrainGridSubsystem& Terrain,FIntPoint HalfGrid,bool bModel2Active,double& OutGroundCm,FText& OutError)
{
    const double GX=HalfGrid.X*.5,GY=HalfGrid.Y*.5;
    const auto OriginalBank=[&]()
    {
        if(bModel2Active)return HydraulicSampleMaterialBank(Terrain,GX,GY,OutGroundCm,OutError);
        float H=0.f;
        if(!Terrain.TrySampleResidentElevation(static_cast<float>(GX),static_cast<float>(GY),H)||!HydraulicFinite(H))
            return HydraulicFail(OutError,TEXT("Missing finite canonical bank knot"));
        OutGroundCm=H;OutError=FText::GetEmpty();return true;
    };
    if(!bModel2Active)return OriginalBank();
    const bool OddX=(HalfGrid.X%2)!=0,OddY=(HalfGrid.Y%2)!=0;
    const int32 BaseX=FMath::FloorToInt(GX),BaseY=FMath::FloorToInt(GY);
    const int32 CountX=OddX?2:1,CountY=OddY?2:1;
    double Sum=0.,ExcavationSum=0.;bool AllProfile=true,AllKnown=true,AnyProfile=false;
    for(int32 Y=0;Y<CountY;++Y)for(int32 X=0;X<CountX;++X)
    {
        FGridCell C;
        if(!Terrain.TryGetCell(FIntPoint(BaseX+X,BaseY+Y),C)){AllProfile=false;AllKnown=false;continue;}
        double Bed=0.;if(!TryGetAftershockHydraulicBedCm(C,true,Bed,OutError))return false;
        if(!IsAftershockHydraulicExcavatedCell(C,true))AllProfile=false;
        else {AnyProfile=true;ExcavationSum+=static_cast<double>(C.HydraulicExcavationCm);}
        Sum+=Bed;
    }
    if(!AllProfile)
    {
        // Interpolate the EXISTING authored excavation at a complete mixed
        // shore stencil. Requiring every incident cell to be channel retained
        // full-height grid corners and produced square opaque bank notches.
        // Keep the original DEM/mineral interpolation; only its known cut
        // offset is averaged. Land centres, river centres and solver beds stay
        // exact. Render and collision consume this same shared knot function.
        if(!AllKnown||!AnyProfile)return OriginalBank();
        if(!OriginalBank())return false;
        const double Candidate=OutGroundCm-ExcavationSum/(CountX*CountY);
        if(!HydraulicFinite(Candidate))return HydraulicFail(OutError,TEXT("Hydraulic shared shore knot overflows"));
        OutGroundCm=Candidate;OutError=FText::GetEmpty();return true;
    }
    const double Candidate=Sum/(CountX*CountY);
    if(!HydraulicFinite(Candidate))return HydraulicFail(OutError,TEXT("Hydraulic shared ground knot overflows"));
    OutGroundCm=Candidate;OutError=FText::GetEmpty();return true;
}

bool BuildAftershockHydraulicGroundPatch(const UTerrainGridSubsystem& Terrain,const FIntRect& Visible,bool bModel2Active,FAftershockHydraulicGroundPatch& OutPatch,FText& OutError)
{
    FAftershockHydraulicGroundPatch Candidate;
    if(!bModel2Active){OutPatch=MoveTemp(Candidate);OutError=FText::GetEmpty();return true;}
    if(Visible.Width()<=0||Visible.Height()<=0||static_cast<int64>(Visible.Width())*Visible.Height()>65536||!HydraulicFinite(Terrain.CellSize)||Terrain.CellSize<=0.f)
        return HydraulicFail(OutError,TEXT("Invalid bounded hydraulic ground display window"));
    const double Size=Terrain.CellSize;
    TMap<FIntPoint,int32> SharedIndex;SharedIndex.Reserve(FMath::Min(Visible.Width()*Visible.Height()*4,262144));
    const auto VertexFor=[&](FIntPoint Half,int32& OutIndex)
    {
        if(const int32* Found=SharedIndex.Find(Half)){OutIndex=*Found;return true;}
        double H=0.,XP=0.,XM=0.,YP=0.,YM=0.;
        if(!TryGetAftershockHydraulicGroundKnotCm(Terrain,Half,true,H,OutError)
            ||!TryGetAftershockHydraulicGroundKnotCm(Terrain,Half+FIntPoint(1,0),true,XP,OutError)
            ||!TryGetAftershockHydraulicGroundKnotCm(Terrain,Half-FIntPoint(1,0),true,XM,OutError)
            ||!TryGetAftershockHydraulicGroundKnotCm(Terrain,Half+FIntPoint(0,1),true,YP,OutError)
            ||!TryGetAftershockHydraulicGroundKnotCm(Terrain,Half-FIntPoint(0,1),true,YM,OutError))return false;
        FAftershockHydraulicGroundVertex V;
        V.Position=FVector(Terrain.GridOrigin.X+(Half.X*.5+.5)*Size,Terrain.GridOrigin.Y+(Half.Y*.5+.5)*Size,H);
        V.Normal=FVector(-(XP-XM),-(YP-YM),Size).GetSafeNormal();
        V.UV=FVector2f(static_cast<float>(V.Position.X*.005),static_cast<float>(V.Position.Y*.005));
        OutIndex=Candidate.Vertices.Add(V);SharedIndex.Add(Half,OutIndex);return true;
    };
    for(int32 Y=Visible.Min.Y;Y<Visible.Max.Y;++Y)for(int32 X=Visible.Min.X;X<Visible.Max.X;++X)
    {
        FGridCell C;if(!Terrain.TryGetCell(FIntPoint(X,Y),C))continue;
        double Bed=0.;if(!TryGetAftershockHydraulicBedCm(C,true,Bed,OutError))return false;
        if(!IsAftershockHydraulicExcavatedCell(C,true))continue;
        int32 V[9];
        for(int32 J=0;J<3;++J)for(int32 I=0;I<3;++I)
            if(!VertexFor(FIntPoint(X*2-1+I,Y*2-1+J),V[I+J*3]))return false;
        for(int32 J=0;J<2;++J)for(int32 I=0;I<2;++I)
        {
            const int32 A=I+J*3;
            Candidate.Indices.Append({V[A],V[A+3],V[A+1],V[A+1],V[A+3],V[A+4]});
        }
        ++Candidate.ProfileCells;
    }
    Candidate.SharedKnots=Candidate.Vertices.Num();
    OutPatch=MoveTemp(Candidate);OutError=FText::GetEmpty();return true;
}
