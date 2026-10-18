#include "AftershockPhysicalReplay.h"
#include "AftershockMassMovementVisual.h"
#include "Misc/Compression.h"
#include "Misc/SecureHash.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
THIRD_PARTY_INCLUDES_START
#include "zlib.h"
THIRD_PARTY_INCLUDES_END

namespace AftershockPhysicalReplayDetail
{
constexpr uint32 Magic=0x41534D46,Version=1;
constexpr int32 MaximumRawBytes=32*1024*1024,MaximumStoredBytes=8*1024*1024;
voidpf ZAllocate(voidpf,uInt Items,uInt Size)
{
    const uint64 Bytes=static_cast<uint64>(Items)*Size;
    return Bytes>0&&Bytes<=1024*1024?FMemory::Malloc(static_cast<SIZE_T>(Bytes)):nullptr;
}
void ZFree(voidpf,voidpf Address){FMemory::Free(Address);}
bool InflateExact(const uint8* Compressed,int32 Count,TArray<uint8>& Raw)
{
    // FCompression's public wrapper logs Error for ordinary corrupt user bytes
    // and does not check consumed input. Own bounded inflate rejects both
    // malformed/truncated streams and trailing compressed data, silently.
    if(!Compressed||Count<1||Count>MaximumStoredBytes||Raw.IsEmpty()||Raw.Num()>MaximumRawBytes)return false;
    z_stream S{};S.zalloc=&ZAllocate;S.zfree=&ZFree;
    S.next_in=const_cast<Bytef*>(Compressed);S.avail_in=static_cast<uInt>(Count);
    S.next_out=Raw.GetData();S.avail_out=static_cast<uInt>(Raw.Num());
    if(inflateInit(&S)!=Z_OK)return false;
    const int Status=inflate(&S,Z_FINISH);
    const bool Complete=Status==Z_STREAM_END&&S.total_in==static_cast<uLong>(Count)
        &&S.total_out==static_cast<uLong>(Raw.Num())&&S.avail_in==0;
    const int End=inflateEnd(&S);return Complete&&End==Z_OK;
}
template<class...T> void Fields(FArchive& A,T&... Values){((A<<Values),...);}
void Point(FArchive&A,FIntPoint&V){Fields(A,V.X,V.Y);}
void Vec(FArchive&A,FVector&V){Fields(A,V.X,V.Y,V.Z);}
void Vec2(FArchive&A,FVector2D&V){Fields(A,V.X,V.Y);}
// Version1 deliberately matches UE's legacy4byte bool wire representation,
// but rejects noncanonical values without logging an engine-level Error for a
// normal malformed-file rejection/native negative fixture.
void Bool(FArchive& A,bool& V)
{uint32 B=V?1u:0u;A<<B;if(A.IsError()||B>1u){A.SetError();return;}if(A.IsLoading())V=B!=0;}
struct FFrameCounts {int32 Parcels=0,Cells=0,Impacts=0;};
template<class T,class F> bool Array(FArchive&A,TArray<T>&Values,int32 Cap,int32 MinimumWireBytes,F&& Serialize)
{
    if(A.IsError()||Cap<0||MinimumWireBytes<1){A.SetError();return false;}
    int32 Count=Values.Num();A<<Count;
    if(A.IsError()||Count<0||Count>Cap){A.SetError();return false;}
    if(A.IsLoading())
    {
        if(static_cast<int64>(Count)*MinimumWireBytes>A.TotalSize()-A.Tell()){A.SetError();return false;}
        Values.SetNum(Count);
    }
    for(auto&V:Values){Serialize(A,V);if(A.IsError()||A.Tell()>MaximumRawBytes){A.SetError();return false;}}
    return true;
}
void Cell(FArchive&A,FAftershockMassCell&V)
{
    Point(A,V.Coord);Fields(A,V.GroundElevationM);Bool(A,V.bLand);
    Fields(A,V.SnowWaterEquivalentMm,V.SnowDensityKgM3,V.SnowWeakLayerStrengthPa,
        V.RegolithVerticalDepthM,V.SoilDensityKgM3,V.CohesionPa,V.FrictionAngleDegrees,V.EffectiveSaturation01);
}
void Obstacle(FArchive&A,FAftershockMassObstacle&V)
{Fields(A,V.BuildingID);Vec2(A,V.CenterM);Vec2(A,V.HalfExtentM);Fields(A,V.YawRadians,V.HeightM,V.StructuralMaterialTag,V.FoundationTag);}
void Impact(FArchive&A,FAftershockMassImpact&V)
{Fields(A,V.BuildingID,V.PeakNormalPressurePa,V.ImpulseMagnitudeNs,V.PeakFlowDepthM,V.Contacts,V.FirstContactSeconds,V.PeakPressureSeconds);}
void ResultCell(FArchive&A,FAftershockMassCellResult&V)
{
    Point(A,V.Coord);Fields(A,V.RunoutBedElevationM,V.RemainingSourceMaterialKg,V.ReleasedMassKg,V.EntrainedMassKg,V.RemovedVolumeM3,
        V.DepositedMassKg,V.DepositedVolumeM3,V.FinalMovingMassKg,V.FirstArrivalSeconds,V.PeakFlowDepthM,V.PeakSpeedMps,V.PeakDynamicPressurePa);
    Vec(A,V.PeakVelocityMps);
}
void Parcel(FArchive&A,FAftershockMassParcelFrame&V)
{Fields(A,V.ParcelID);Vec(A,V.PositionM);Vec(A,V.VelocityMps);Fields(A,V.MassKg,V.VolumeM3,V.FootprintAreaM2);}
void FrameCell(FArchive&A,FAftershockMassFrameCell&V)
{Point(A,V.Coord);Fields(A,V.MovingMassKg,V.MovingVolumeM3,V.FlowDepthM,V.DepositedMassKg,V.DepositedVolumeM3);}
void Frame(FArchive&A,FAftershockMassFrame&V,FFrameCounts& Counts)
{
    Fields(A,V.Seconds);
    if(!Array(A,V.Parcels,FMath::Min(512,FAftershockMassResult::MaximumFrameParcelRecords-Counts.Parcels),76,Parcel))return;
    Counts.Parcels+=V.Parcels.Num();
    if(!Array(A,V.Cells,FMath::Min(65536,FAftershockMassResult::MaximumFrameCellRecords-Counts.Cells),48,FrameCell))return;
    Counts.Cells+=V.Cells.Num();
    if(!Array(A,V.Impacts,FMath::Min(8192,FAftershockMassResult::MaximumFrameImpactRecords-Counts.Impacts),48,Impact))return;
    Counts.Impacts+=V.Impacts.Num();
}
void Budget(FArchive&A,FAftershockMassBudget&V)
{
    Fields(A,V.ReleasedKg,V.EntrainedKg,V.MovingKg,V.DepositedKg,V.ExportedKg,V.MassResidualKg,
        V.InitialKineticJ,V.GravityWorkJ,V.DissipatedJ,V.MovingKineticJ,V.ExportedKineticJ,V.EnergyResidualJ,
        V.MaximumSpeedMps,V.MaximumDisplacementCells,V.Substeps,V.InitialParcels,V.PeakActiveParcels,
        V.MaterialCallbackCalls,V.FoundationCallbackCalls,V.ParticleSteps,V.SurfaceSamples,V.ObstacleTests,V.WorkUnits,V.ComputeMilliseconds);
}
bool Body(FArchive&A,FAftershockMassVisualEvent&V)
{
    Fields(A,V.EventID,V.StartedAtWorldSeconds,V.CellSizeM);Vec2(A,V.GridOriginM);Vec(A,V.WorldOffsetCm);
    if(!Array(A,V.SupportCells,65536,84,Cell)||!Array(A,V.Obstacles,8192,60,Obstacle))return false;
    auto&R=V.Result;uint8 Material=static_cast<uint8>(R.Material);Fields(A,Material,R.HazardTypeTag,R.SourceTerrainRevision,R.SimulatedSeconds);
    if(Material>1){A.SetError();return false;}R.Material=static_cast<EAftershockMassMaterial>(Material);
    Bool(A,R.bAccepted);Bool(A,R.bComplete);Bool(A,R.bMassConserved);Bool(A,R.bEnergyClosed);Bool(A,R.bFrozenBedDuringEvent);
    Bool(A,R.bLateralGranularPressureModeled);Bool(A,R.bOvertoppingModeled);Bool(A,R.bFrameExportComplete);
    Budget(A,R.Budget);
    FFrameCounts Counts;
    if(!Array(A,R.Cells,65536,128,ResultCell)||!Array(A,R.Impacts,8192,48,Impact)
        ||!Array(A,R.Frames,64,20,[&](FArchive& Wire,FAftershockMassFrame& F){Frame(Wire,F,Counts);})
        ||A.IsError()||A.Tell()>MaximumRawBytes)return false;
    if(A.IsLoading())
    {R.ExportedFrameCellRecords=Counts.Cells;R.ExportedFrameParcelRecords=Counts.Parcels;R.ExportedFrameImpactRecords=Counts.Impacts;}
    return true;
}
bool FiniteNonnegative(double V){return FMath::IsFinite(V)&&V>=0.;}
bool Range(double V,double Lo,double Hi){return FMath::IsFinite(V)&&V>=Lo&&V<=Hi;}
bool Near(double A,double B,double Scale=1.)
{return FMath::IsFinite(A)&&FMath::IsFinite(B)&&FMath::Abs(A-B)<=FMath::Max(1.e-6,FMath::Max3(1.,FMath::Abs(Scale),FMath::Max(FMath::Abs(A),FMath::Abs(B)))*1.e-8);}
bool CoordValid(FIntPoint C)
{return FMath::Abs(static_cast<int64>(C.X))<=1000000&&FMath::Abs(static_cast<int64>(C.Y))<=1000000;}
bool VectorValid(FVector V,double Bound)
{return Range(V.X,-Bound,Bound)&&Range(V.Y,-Bound,Bound)&&Range(V.Z,-Bound,Bound);}
bool Valid(const FAftershockMassVisualEvent&V)
{
    const auto&R=V.Result;const auto&B=R.Budget;
    if(!V.EventID.IsValid()||!Range(V.StartedAtWorldSeconds,-1.e12,1.e12)||!Range(V.CellSizeM,.5,1000.)
        ||!Range(V.GridOriginM.X,-1.e12,1.e12)||!Range(V.GridOriginM.Y,-1.e12,1.e12)||!VectorValid(V.WorldOffsetCm,1.e12)
        ||!R.bAccepted||!R.bComplete||!R.bMassConserved||!R.bEnergyClosed||!R.bFrameExportComplete||!R.bFrozenBedDuringEvent
        ||R.bLateralGranularPressureModeled||R.bOvertoppingModeled||!R.Failure.IsEmpty()||!R.FrameExportFailure.IsEmpty()
        ||(R.Material!=EAftershockMassMaterial::Snow&&R.Material!=EAftershockMassMaterial::SoilDebris)
        ||R.HazardTypeTag!=(R.Material==EAftershockMassMaterial::Snow?7:8)||!Range(R.SimulatedSeconds,0.,300.)
        ||V.SupportCells.IsEmpty()||V.SupportCells.Num()>65536||V.Obstacles.Num()>8192
        ||R.Cells.Num()!=V.SupportCells.Num()||R.Impacts.Num()>8192||R.Frames.IsEmpty()||R.Frames.Num()>64)return false;
    const double Area=V.CellSizeM*V.CellSizeM;
    const double MinimumDensity=R.Material==EAftershockMassMaterial::Snow?50.:500.;
    const double MaximumDensity=R.Material==EAftershockMassMaterial::Snow?900.:3500.;
    const auto InventoryVolumeValid=[&](double MassKg,double VolumeM3)
    {
        if(!FiniteNonnegative(MassKg)||!FiniteNonnegative(VolumeM3))return false;
        // Independent exact-remainder ledgers can differ at submicrogram scale.
        if(MassKg<=1.e-6&&VolumeM3<=1.e-6/MinimumDensity)return true;
        return VolumeM3>0.&&Range(MassKg/VolumeM3,MinimumDensity-1.e-6,MaximumDensity+1.e-6);
    };
    TMap<FIntPoint,const FAftershockMassCell*> Support;
    for(const auto&C:V.SupportCells)
    {
        if(!CoordValid(C.Coord)||Support.Contains(C.Coord)||!Range(C.GroundElevationM,-1.e7,1.e7)
            ||!Range(C.SnowWaterEquivalentMm,0.,10000.)||!Range(C.SnowDensityKgM3,50.,900.)||!Range(C.SnowWeakLayerStrengthPa,0.,1.e7)
            ||!Range(C.RegolithVerticalDepthM,0.,30.)||!Range(C.SoilDensityKgM3,500.,3500.)||!Range(C.CohesionPa,0.,1.e7)
            ||!Range(C.FrictionAngleDegrees,0.,80.)||C.FrictionAngleDegrees>=80.||!Range(C.EffectiveSaturation01,0.,1.))return false;
        Support.Add(C.Coord,&C);
    }
    TSet<int32> Buildings;
    for(const auto&O:V.Obstacles)
    {
        if(O.BuildingID<1||Buildings.Contains(O.BuildingID)||!Range(O.CenterM.X,-1.e12,1.e12)||!Range(O.CenterM.Y,-1.e12,1.e12)
            ||!Range(O.HalfExtentM.X,SMALL_NUMBER,1000.)||!Range(O.HalfExtentM.Y,SMALL_NUMBER,1000.)
            ||!Range(O.YawRadians,-1.e12,1.e12)||!Range(O.HeightM,SMALL_NUMBER,1000.)
            ||O.StructuralMaterialTag<0||O.StructuralMaterialTag>4||O.FoundationTag<0||O.FoundationTag>2)return false;
        Buildings.Add(O.BuildingID);
    }
    const auto CheckImpact=[&](const FAftershockMassImpact&I,double Clock)
    {return Buildings.Contains(I.BuildingID)&&FiniteNonnegative(I.PeakNormalPressurePa)&&FiniteNonnegative(I.ImpulseMagnitudeNs)
        &&FiniteNonnegative(I.PeakFlowDepthM)&&I.Contacts>0&&I.Contacts<=10000000&&FiniteNonnegative(I.FirstContactSeconds)
        &&Range(I.PeakPressureSeconds,I.FirstContactSeconds,Clock+1.e-8);};
    TMap<int32,const FAftershockMassImpact*> FinalImpacts;
    for(const auto&I:R.Impacts)
    {if(FinalImpacts.Contains(I.BuildingID)||!CheckImpact(I,R.SimulatedSeconds))return false;FinalImpacts.Add(I.BuildingID,&I);}
    TMap<FIntPoint,const FAftershockMassCellResult*> ResultCells;
    double Deposited=0.,Moving=0.,Released=0.,Entrained=0.;
    for(const auto&C:R.Cells)
    {
        const auto* Found=Support.Find(C.Coord);
        if(!Found||ResultCells.Contains(C.Coord)||!Range(C.RunoutBedElevationM,-1.e7,1.e7)
            ||!FiniteNonnegative(C.RemainingSourceMaterialKg)||!FiniteNonnegative(C.ReleasedMassKg)||!FiniteNonnegative(C.EntrainedMassKg)
            ||!FiniteNonnegative(C.RemovedVolumeM3)||!FiniteNonnegative(C.DepositedMassKg)||!FiniteNonnegative(C.DepositedVolumeM3)
            ||!FiniteNonnegative(C.FinalMovingMassKg)||!Range(C.FirstArrivalSeconds,-1.,R.SimulatedSeconds+1.e-8)
            ||(C.FirstArrivalSeconds<0.&&C.FirstArrivalSeconds!=-1.)||!FiniteNonnegative(C.PeakFlowDepthM)||!Range(C.PeakSpeedMps,0.,250.)
            ||!FiniteNonnegative(C.PeakDynamicPressurePa)||!VectorValid(C.PeakVelocityMps,250.)
            ||!InventoryVolumeValid(C.DepositedMassKg,C.DepositedVolumeM3))return false;
        const auto& S=**Found;const double Density=R.Material==EAftershockMassMaterial::Snow?S.SnowDensityKgM3:S.SoilDensityKgM3;
        const double Initial=S.bLand?(R.Material==EAftershockMassMaterial::Snow?S.SnowWaterEquivalentMm*Area:S.RegolithVerticalDepthM*Area*S.SoilDensityKgM3):0.;
        if(!Near(Initial,C.RemainingSourceMaterialKg+C.ReleasedMassKg+C.EntrainedMassKg)
            ||!Near(C.RemovedVolumeM3,(C.ReleasedMassKg+C.EntrainedMassKg)/Density)
            ||!Near(C.RunoutBedElevationM,S.GroundElevationM+S.SnowWaterEquivalentMm/S.SnowDensityKgM3-C.ReleasedMassKg/Density/Area))return false;
        ResultCells.Add(C.Coord,&C);Deposited+=C.DepositedMassKg;Moving+=C.FinalMovingMassKg;Released+=C.ReleasedMassKg;Entrained+=C.EntrainedMassKg;
    }
    const double Mass=B.ReleasedKg+B.EntrainedKg;
    const double EnergyScale=FMath::Max(1.,FMath::Abs(B.GravityWorkJ)+B.InitialKineticJ);
    if(!FiniteNonnegative(B.ReleasedKg)||B.ReleasedKg<=0.||!FiniteNonnegative(B.EntrainedKg)||!FiniteNonnegative(B.MovingKg)||B.MovingKg>1.e-9
        ||!FiniteNonnegative(B.DepositedKg)||!FiniteNonnegative(B.ExportedKg)||!FMath::IsFinite(B.MassResidualKg)
        ||!FiniteNonnegative(B.InitialKineticJ)||!FMath::IsFinite(B.GravityWorkJ)||!FiniteNonnegative(B.DissipatedJ)
        ||!FiniteNonnegative(B.MovingKineticJ)||!FiniteNonnegative(B.ExportedKineticJ)||!FMath::IsFinite(B.EnergyResidualJ)
        ||!Range(B.MaximumSpeedMps,0.,250.)||!Range(B.MaximumDisplacementCells,0.,.250001)||!FiniteNonnegative(B.ComputeMilliseconds)
        ||B.Substeps<0||B.Substeps>20000||B.InitialParcels<1||B.InitialParcels>512||B.PeakActiveParcels<B.InitialParcels||B.PeakActiveParcels>512
        ||B.MaterialCallbackCalls<0||B.MaterialCallbackCalls>65536||B.FoundationCallbackCalls<0||B.FoundationCallbackCalls>8192
        ||B.ParticleSteps<0||B.ParticleSteps>10000000||B.SurfaceSamples<0||B.ObstacleTests<0||B.WorkUnits<1||B.WorkUnits>250000000)return false;
    const double MassResidual=Mass-B.DepositedKg-B.ExportedKg-B.MovingKg;
    const double EnergyResidual=B.InitialKineticJ+B.GravityWorkJ-B.DissipatedJ-B.MovingKineticJ-B.ExportedKineticJ;
    if(!Near(MassResidual,0.,Mass)||!Near(B.MassResidualKg,MassResidual,Mass)||!Near(EnergyResidual,0.,EnergyScale)
        ||!Near(B.EnergyResidualJ,EnergyResidual,EnergyScale)||!Near(Released,B.ReleasedKg)||!Near(Entrained,B.EntrainedKg)
        ||!Near(Deposited,B.DepositedKg)||!Near(Moving,B.MovingKg))return false;
    const auto SupportedPosition=[&](const FAftershockMassParcelFrame& P)
    {
        const FVector2D Q=(FVector2D(P.PositionM)-V.GridOriginM)/V.CellSizeM-FVector2D(.5);
        if(!Range(Q.X,-1000001.,1000001.)||!Range(Q.Y,-1000001.,1000001.))return false;
        const FIntPoint C(FMath::FloorToInt(Q.X),FMath::FloorToInt(Q.Y));const double U=Q.X-C.X,W=Q.Y-C.Y;
        const FIntPoint At[4]={C,C+FIntPoint(1,0),C+FIntPoint(0,1),C+FIntPoint(1,1)};double H[4];
        for(int32 J=0;J<4;++J)
        {const auto* S=Support.Find(At[J]);const auto* Bed=ResultCells.Find(At[J]);if(!S||!Bed||!(**S).bLand)return false;H[J]=(**Bed).RunoutBedElevationM;}
        const double Z=(1.-U)*(1.-W)*H[0]+U*(1.-W)*H[1]+(1.-U)*W*H[2]+U*W*H[3];
        return Near(P.PositionM.Z,Z+P.VolumeM3/P.FootprintAreaM2*.5);
    };
    double Previous=-1.;int64 ParcelRecords=0,CellRecords=0,ImpactRecords=0;TSet<int32> OriginalIDs;
    TMap<FIntPoint,const FAftershockMassFrameCell*> LastDeposits;TMap<int32,const FAftershockMassImpact*> LastImpacts;
    TMap<int32,const FAftershockMassParcelFrame*> LastParcels;
    for(int32 FI=0;FI<R.Frames.Num();++FI)
    {
        const auto& F=R.Frames[FI];if(!Range(F.Seconds,0.,R.SimulatedSeconds+1.e-8)||F.Seconds<Previous||F.Parcels.Num()>512||F.Cells.Num()>65536||F.Impacts.Num()>8192)return false;
        if(FI==0&&F.Seconds!=0.)return false;Previous=F.Seconds;
        ParcelRecords+=F.Parcels.Num();CellRecords+=F.Cells.Num();ImpactRecords+=F.Impacts.Num();
        if(ParcelRecords>FAftershockMassResult::MaximumFrameParcelRecords||CellRecords>FAftershockMassResult::MaximumFrameCellRecords||ImpactRecords>FAftershockMassResult::MaximumFrameImpactRecords)return false;
        TSet<int32> IDs;TMap<FIntPoint,const FAftershockMassFrameCell*> Current;TMap<int32,const FAftershockMassImpact*> CurrentImpacts;
        TMap<int32,const FAftershockMassParcelFrame*> CurrentParcels;double ParcelMass=0.,ParcelVolume=0.,ParcelKinetic=0.,CellMass=0.,CellVolume=0.,DepositMass=0.;
        for(const auto&P:F.Parcels)
        {
            if(P.ParcelID<0||P.ParcelID>=B.InitialParcels||IDs.Contains(P.ParcelID)||!VectorValid(P.PositionM,1.e12)||!VectorValid(P.VelocityMps,250.)
                ||!FiniteNonnegative(P.MassKg)||P.MassKg<=0.||!FiniteNonnegative(P.VolumeM3)||P.VolumeM3<=0.
                ||!Range(P.FootprintAreaM2,SMALL_NUMBER,Area)||P.VolumeM3/P.FootprintAreaM2>100.000001||!SupportedPosition(P))return false;
            const double Density=P.MassKg/P.VolumeM3;
            if(!Range(Density,MinimumDensity-1.e-6,MaximumDensity+1.e-6))return false;
            if(FI==0)OriginalIDs.Add(P.ParcelID);else if(!OriginalIDs.Contains(P.ParcelID)||!LastParcels.Contains(P.ParcelID))return false;
            if(const auto* Old=LastParcels.Find(P.ParcelID))if(P.MassKg+1.e-7<(**Old).MassKg||P.VolumeM3+1.e-7<(**Old).VolumeM3)return false;
            IDs.Add(P.ParcelID);CurrentParcels.Add(P.ParcelID,&P);ParcelMass+=P.MassKg;ParcelVolume+=P.VolumeM3;ParcelKinetic+=.5*P.MassKg*P.VelocityMps.SizeSquared();
        }
        for(const auto&C:F.Cells)
        {
            const auto* S=Support.Find(C.Coord);
            if(!S||!(**S).bLand||Current.Contains(C.Coord)||!FiniteNonnegative(C.MovingMassKg)||!FiniteNonnegative(C.MovingVolumeM3)
                ||!FiniteNonnegative(C.FlowDepthM)||!FiniteNonnegative(C.DepositedMassKg)||!FiniteNonnegative(C.DepositedVolumeM3)
                ||!Near(C.FlowDepthM,C.MovingVolumeM3/Area)
                ||!InventoryVolumeValid(C.MovingMassKg,C.MovingVolumeM3)||!InventoryVolumeValid(C.DepositedMassKg,C.DepositedVolumeM3))return false;
            if(const auto* Old=LastDeposits.Find(C.Coord))if(C.DepositedMassKg+1.e-7<(**Old).DepositedMassKg||C.DepositedVolumeM3+1.e-7<(**Old).DepositedVolumeM3)return false;
            const auto& Final=**ResultCells.Find(C.Coord);if(C.DepositedMassKg>Final.DepositedMassKg+FMath::Max(1.e-6,Mass*1.e-8))return false;
            Current.Add(C.Coord,&C);CellMass+=C.MovingMassKg;CellVolume+=C.MovingVolumeM3;DepositMass+=C.DepositedMassKg;
        }
        for(const auto& Old:LastDeposits)if(Old.Value->DepositedMassKg>0.&&!Current.Contains(Old.Key))return false;
        for(const auto&I:F.Impacts)
        {
            if(CurrentImpacts.Contains(I.BuildingID)||!CheckImpact(I,F.Seconds))return false;
            const auto* Final=FinalImpacts.Find(I.BuildingID);if(!Final||I.PeakNormalPressurePa>(**Final).PeakNormalPressurePa+1.e-6
                ||I.ImpulseMagnitudeNs>(**Final).ImpulseMagnitudeNs+1.e-6||I.Contacts>(**Final).Contacts)return false;
            if(const auto* Old=LastImpacts.Find(I.BuildingID))if(I.Contacts<(**Old).Contacts||I.ImpulseMagnitudeNs+1.e-6<(**Old).ImpulseMagnitudeNs
                ||I.PeakNormalPressurePa+1.e-6<(**Old).PeakNormalPressurePa||!Near(I.FirstContactSeconds,(**Old).FirstContactSeconds))return false;
            CurrentImpacts.Add(I.BuildingID,&I);
        }
        for(const auto& Old:LastImpacts)if(!CurrentImpacts.Contains(Old.Key))return false;
        if(!Near(ParcelMass,CellMass)||!Near(ParcelVolume,CellVolume)||ParcelMass+DepositMass>Mass+FMath::Max(1.e-6,Mass*1.e-8))return false;
        if(FI==0&&(!Near(ParcelMass,B.ReleasedKg)||!Near(ParcelKinetic,B.InitialKineticJ)||DepositMass!=0.||!F.Impacts.IsEmpty()||F.Parcels.Num()!=B.InitialParcels))return false;
        if(FI==R.Frames.Num()-1)
        {
            if(!Near(F.Seconds,R.SimulatedSeconds)||!Near(ParcelMass,B.MovingKg)||!Near(ParcelKinetic,B.MovingKineticJ)||!Near(DepositMass,B.DepositedKg)||CurrentImpacts.Num()!=FinalImpacts.Num())return false;
            for(const auto& Final:ResultCells)
            {const auto* C=Current.Find(Final.Key);if(Final.Value->DepositedMassKg>0.&&(!C||!Near((**C).DepositedMassKg,Final.Value->DepositedMassKg)||!Near((**C).DepositedVolumeM3,Final.Value->DepositedVolumeM3)))return false;}
            for(const auto& Final:FinalImpacts)
            {const auto* C=CurrentImpacts.Find(Final.Key);if(!C||(**C).Contacts!=Final.Value->Contacts||!Near((**C).ImpulseMagnitudeNs,Final.Value->ImpulseMagnitudeNs)
                ||!Near((**C).PeakNormalPressurePa,Final.Value->PeakNormalPressurePa)||!Near((**C).FirstContactSeconds,Final.Value->FirstContactSeconds)||!Near((**C).PeakPressureSeconds,Final.Value->PeakPressureSeconds))return false;}
        }
        LastDeposits=MoveTemp(Current);LastImpacts=MoveTemp(CurrentImpacts);LastParcels=MoveTemp(CurrentParcels);
    }
    return ParcelRecords==R.ExportedFrameParcelRecords&&CellRecords==R.ExportedFrameCellRecords&&ImpactRecords==R.ExportedFrameImpactRecords;
}
bool Fail(FText&Error){Error=NSLOCTEXT("Aftershock","PhysicalReplayInvalid","Afet hareket kaydı sürüm, boyut, sayısal değer veya kütle kontrolünü geçemedi; dosya korunuyor.");return false;}
}

bool EncodeAftershockMassReplay(const FAftershockMassVisualEvent&Event,TArray<uint8>&Out,FText&Error)
{
    using namespace AftershockPhysicalReplayDetail;if(!Valid(Event))return Fail(Error);
    FAftershockMassVisualEvent Copy=Event;TArray<uint8> Raw;FMemoryWriter Writer(Raw,true);
    if(!Body(Writer,Copy)||Raw.IsEmpty()||Raw.Num()>MaximumRawBytes)return Fail(Error);
    int32 Count=FCompression::CompressMemoryBound(NAME_Zlib,Raw.Num());if(Count<=0||Count>MaximumRawBytes+1024*1024)return Fail(Error);
    TArray<uint8> Stored;Stored.SetNumUninitialized(Count);
    if(!FCompression::CompressMemory(NAME_Zlib,Stored.GetData(),Count,Raw.GetData(),Raw.Num(),COMPRESS_BiasSpeed)||Count>MaximumStoredBytes-36)return Fail(Error);
    Stored.SetNum(Count,EAllowShrinking::No);TArray<uint8> Candidate;FMemoryWriter Envelope(Candidate,true);uint32 M=Magic,V=Version;int32 N=Raw.Num();uint8 Digest[20];FSHA1::HashBuffer(Raw.GetData(),Raw.Num(),Digest);
    Fields(Envelope,M,V,N,Count);Envelope.Serialize(Digest,20);Envelope.Serialize(Stored.GetData(),Count);
    if(Envelope.IsError())return Fail(Error);Out=MoveTemp(Candidate);Error=FText::GetEmpty();return true;
}
bool DecodeAftershockMassReplay(const TArray<uint8>&Bytes,FAftershockMassVisualEvent&Out,FText&Error)
{
    using namespace AftershockPhysicalReplayDetail;if(Bytes.Num()<36||Bytes.Num()>MaximumStoredBytes)return Fail(Error);
    FMemoryReader Header(Bytes,true);uint32 M=0,V=0;int32 N=0,Count=0;Fields(Header,M,V,N,Count);uint8 Digest[20];Header.Serialize(Digest,20);
    if(Header.IsError()||M!=Magic||V!=Version||N<1||N>MaximumRawBytes||Count<1||Count!=Bytes.Num()-36)return Fail(Error);
    TArray<uint8> Raw;Raw.SetNumUninitialized(N);if(!InflateExact(Bytes.GetData()+36,Count,Raw))return Fail(Error);
    uint8 Actual[20];FSHA1::HashBuffer(Raw.GetData(),N,Actual);if(FMemory::Memcmp(Actual,Digest,20))return Fail(Error);
    FAftershockMassVisualEvent Candidate;FMemoryReader Reader(Raw,true);
    if(!Body(Reader,Candidate)||Reader.IsError()||Reader.Tell()!=Raw.Num()||!Valid(Candidate))return Fail(Error);
    Out=MoveTemp(Candidate);Error=FText::GetEmpty();return true;
}
bool ValidateAftershockMassReplayBytes(const TArray<uint8>&Bytes,FText&Error)
{FAftershockMassVisualEvent Checked;return DecodeAftershockMassReplay(Bytes,Checked,Error);}
