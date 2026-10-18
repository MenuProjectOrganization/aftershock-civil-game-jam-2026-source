#include "AftershockMassMovement.h"
#include "AftershockMassSparseFieldAudit.h"
#include "Containers/BitArray.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace AftershockMassKernelDetail
{
constexpr double G=FAftershockMassSettings::GravityMps2;
bool Finite(double V){return FMath::IsFinite(V);}
bool Finite(const FVector2D& V){return Finite(V.X)&&Finite(V.Y);}
bool Finite(const FVector& V){return Finite(V.X)&&Finite(V.Y)&&Finite(V.Z);}
bool FrictionValid(const FAftershockMassFriction& F)
{return Finite(F.Mu)&&F.Mu>=0.&&F.Mu<=2.&&Finite(F.XiMps2)&&F.XiMps2>0.&&F.XiMps2<=1.e12;}
bool ResponseValid(const FAftershockMassObstacleResponse& R)
{return Finite(R.NormalRestitution)&&R.NormalRestitution>=0.&&R.NormalRestitution<=1.
    &&Finite(R.TangentialRetention)&&R.TangentialRetention>=0.&&R.TangentialRetention<=1.
    &&Finite(R.ImpactCoefficient)&&R.ImpactCoefficient>=0.&&R.ImpactCoefficient<=10.;}
struct FWorkCell
{
    FAftershockMassCell Source;
    FAftershockMassFriction Friction;
    double AvailableKg=0.,Density=0.,FrozenBedM=0.;
};
struct FParcel
{
    int32 ID=0;
    FVector2D XY=FVector2D::ZeroVector;
    FVector Velocity=FVector::ZeroVector;
    double Mass=0.,Volume=0.,Area=0.,SurfaceZ=0.,HeldSeconds=0.;
    bool bMoving=true;
    // Derived diagnostics only; not serialized and never read by integration.
    bool bLastCanHold=false;
    FVector LastSupportNormal=FVector::UpVector;
    int32 LastSupport[4]={INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE};
    double Depth()const{return Volume/Area;}
    double Density()const{return Mass/Volume;}
    double Radius()const{return FMath::Sqrt(Area/UE_DOUBLE_PI);}
    double Kinetic()const{return .5*Mass*Velocity.SizeSquared();}
};
struct FSurface
{
    double Z=0.;
    FVector Normal=FVector::UpVector;
    int32 Indices[4]={INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE};
    double Weights[4]={0.,0.,0.,0.};
};
struct FObstacle
{
    FAftershockMassObstacle Source;
    FAftershockMassObstacleResponse Response;
    double C=1.,S=0.;
    FVector2D Local(const FVector2D& P)const
    {const auto D=P-Source.CenterM;return FVector2D(C*D.X+S*D.Y,-S*D.X+C*D.Y);}
    FVector2D WorldDirection(const FVector2D& P)const{return FVector2D(C*P.X-S*P.Y,S*P.X+C*P.Y);}
};
struct FHit
{int32 Obstacle=INDEX_NONE;double Fraction=1.;FVector Normal=FVector::ZeroVector;};

class FKernel
{
public:
    const FAftershockMassInput& In;
    FAftershockMassResult Out;
    TArray<FWorkCell> Cells;
    TMap<FIntPoint,int32> ByCoord;
    TArray<FObstacle> Obstacles;
    TMap<FIntPoint,TArray<int32>> ObstacleBins;
    TMap<int32,int32> ImpactIndices;
    TArray<FParcel> Parcels;
    TArray<double> MassScratch,VolumeScratch,PeakSpeedScratch;
    TArray<FVector> FastVelocityScratch;
    TBitArray<> FieldTouchedMask;
    TArray<int32> ActiveFieldIndices;
    FAftershockMassFieldWorkAudit FieldWork;
#if WITH_EDITOR
    bool bDenseFieldsReferenceForAudit=false;
#endif
    double Area=0.,Time=0.;
    FString Failure;

    explicit FKernel(const FAftershockMassInput& Value):In(Value)
    {Out.Material=In.Material;Out.HazardTypeTag=In.HazardTypeTag;Out.SourceTerrainRevision=In.TerrainRevision;}
    bool SpendWork(int64 Units)
    {
        if(Units<0||Units>In.Settings.MaxWorkUnits-Out.Budget.WorkUnits)
        {Failure=TEXT("Deterministic numerical work budget exhausted; do not commit");return false;}
        Out.Budget.WorkUnits+=Units;return true;
    }
    bool Initialize()
    {
        const auto& S=In.Settings;
        if(!Finite(In.CellSizeM)||In.CellSizeM<.5||In.CellSizeM>1000.||!Finite(In.GridOriginM)||In.GridOriginM.GetAbsMax()>1.e12
            ||In.Cells.IsEmpty()||S.MaxCells<1||S.MaxCells>65536||In.Cells.Num()>S.MaxCells
            ||S.MaxParcels<1||S.MaxParcels>2048||S.MaxObstacles<0||S.MaxObstacles>1024||In.Obstacles.Num()>S.MaxObstacles
            ||S.MaxReleaseCells<1||S.MaxReleaseCells>512||In.Release.Num()>S.MaxReleaseCells
            ||S.ParcelsPerReleaseCell<1||S.ParcelsPerReleaseCell>16
            ||S.MaxSubsteps<1||S.MaxSubsteps>20000||S.MaxParticleSteps<1||S.MaxParticleSteps>10000000
            ||S.MaxWorkUnits<1||S.MaxWorkUnits>250000000
            ||S.MaxFrames<2||S.MaxFrames>64||!Finite(S.MaximumSeconds)||S.MaximumSeconds<=0.||S.MaximumSeconds>300.
            ||!Finite(S.MaxStepSeconds)||S.MaxStepSeconds<=0.||S.MaxStepSeconds>.1
            ||!Finite(S.CFL)||S.CFL<=0.||S.CFL>.25||!Finite(S.MaxSupportedSpeedMps)||S.MaxSupportedSpeedMps<=0.||S.MaxSupportedSpeedMps>250.
            ||!Finite(S.MaxParcelDepthM)||S.MaxParcelDepthM<=0.||S.MaxParcelDepthM>100.
            ||!Finite(S.StopSpeedMps)||S.StopSpeedMps<0.||S.StopSpeedMps>2.
            ||!Finite(S.StopHoldSeconds)||S.StopHoldSeconds<0.||S.StopHoldSeconds>10.
            ||!Finite(S.MinimumSnowDepthM)||S.MinimumSnowDepthM<.01||S.MinimumSnowDepthM>5.
            ||!Finite(S.MinimumSoilDepthM)||S.MinimumSoilDepthM<.01||S.MinimumSoilDepthM>5.
            ||!Finite(S.MinimumSnowSlopeDegrees)||!Finite(S.MaximumSnowSlopeDegrees)||S.MinimumSnowSlopeDegrees<1.
            ||S.MaximumSnowSlopeDegrees<=S.MinimumSnowSlopeDegrees||S.MaximumSnowSlopeDegrees>=85.
            ||!Finite(S.MinimumSoilSlopeDegrees)||S.MinimumSoilSlopeDegrees<1.||S.MinimumSoilSlopeDegrees>=85.
            ||!Finite(S.EntrainFractionPerSecond)||S.EntrainFractionPerSecond<0.||S.EntrainFractionPerSecond>1.
            ||!FrictionValid(S.SnowFriction)||!FrictionValid(S.SoilFriction)
            ||(In.Material!=EAftershockMassMaterial::Snow&&In.Material!=EAftershockMassMaterial::SoilDebris)
            ||In.HazardTypeTag!=(In.Material==EAftershockMassMaterial::Snow?7:8))
        {Failure=TEXT("Invalid numeric input, material tag or bounded work settings");return false;}
        Area=In.CellSizeM*In.CellSizeM;
        if(!SpendWork(In.Cells.Num()))return false;
        TArray<FAftershockMassCell> Ordered=In.Cells;
        Ordered.Sort([](const auto& A,const auto& B){return A.Coord.Y!=B.Coord.Y?A.Coord.Y<B.Coord.Y:A.Coord.X<B.Coord.X;});
        Cells.Reserve(Ordered.Num());Out.Cells.Reserve(Ordered.Num());
        for(const auto& C:Ordered)
        {
            if(FMath::Abs(static_cast<int64>(C.Coord.X))>1000000000ll||FMath::Abs(static_cast<int64>(C.Coord.Y))>1000000000ll
                ||ByCoord.Contains(C.Coord)||!Finite(C.GroundElevationM)||FMath::Abs(C.GroundElevationM)>1.e7
                ||!Finite(C.SnowWaterEquivalentMm)||C.SnowWaterEquivalentMm<0.||C.SnowWaterEquivalentMm>10000.
                ||!Finite(C.SnowDensityKgM3)||C.SnowDensityKgM3<50.||C.SnowDensityKgM3>900.
                ||!Finite(C.SnowWeakLayerStrengthPa)||C.SnowWeakLayerStrengthPa<0.||C.SnowWeakLayerStrengthPa>1.e7
                ||!Finite(C.RegolithVerticalDepthM)||C.RegolithVerticalDepthM<0.||C.RegolithVerticalDepthM>30.
                ||!Finite(C.SoilDensityKgM3)||C.SoilDensityKgM3<500.||C.SoilDensityKgM3>3500.
                ||!Finite(C.CohesionPa)||C.CohesionPa<0.||C.CohesionPa>1.e7
                ||!Finite(C.FrictionAngleDegrees)||C.FrictionAngleDegrees<0.||C.FrictionAngleDegrees>=80.
                ||!Finite(C.EffectiveSaturation01)||C.EffectiveSaturation01<0.||C.EffectiveSaturation01>1.)
            {Failure=TEXT("Invalid, duplicate or unsupported material cell");return false;}
            FWorkCell W;W.Source=C;W.Density=In.Material==EAftershockMassMaterial::Snow?C.SnowDensityKgM3:C.SoilDensityKgM3;
            W.AvailableKg=C.bLand?(In.Material==EAftershockMassMaterial::Snow?C.SnowWaterEquivalentMm*Area:C.RegolithVerticalDepthM*Area*C.SoilDensityKgM3):0.;
            W.FrozenBedM=C.GroundElevationM+C.SnowWaterEquivalentMm/C.SnowDensityKgM3;
            W.Friction=In.Material==EAftershockMassMaterial::Snow?S.SnowFriction:S.SoilFriction;
            if(In.MaterialFriction)
            {++Out.Budget.MaterialCallbackCalls;if(!In.MaterialFriction(C,In.Material,W.Friction)||!FrictionValid(W.Friction))
                {Failure=TEXT("Immutable material callback rejected cell");return false;}}
            const int32 Index=Cells.Add(W);ByCoord.Add(C.Coord,Index);FAftershockMassCellResult R;R.Coord=C.Coord;Out.Cells.Add(R);
        }
        // Four arrays and a membership bitset are initialized ONCE per solve.
        // Capacity is bounded by the validated resident/parcel limits above.
        if(!SpendWork(static_cast<int64>(Cells.Num())*5))return false;
        MassScratch.Init(0.,Cells.Num());VolumeScratch.Init(0.,Cells.Num());
        PeakSpeedScratch.Init(0.,Cells.Num());FastVelocityScratch.Init(FVector::ZeroVector,Cells.Num());
        FieldTouchedMask.Init(false,Cells.Num());
        ActiveFieldIndices.Reserve(FMath::Min(Cells.Num(),In.Settings.MaxParcels*4));
        FieldWork.ScratchInitializedCells=Cells.Num();
        TArray<FAftershockMassObstacle> Sorted=In.Obstacles;
        Sorted.Sort([](const auto& A,const auto& B){return A.BuildingID<B.BuildingID;});
        int32 PreviousID=INDEX_NONE;
        for(const auto& O:Sorted)
        {
            if(O.BuildingID<0||O.BuildingID==PreviousID||!Finite(O.CenterM)||!Finite(O.HalfExtentM)
                ||O.HalfExtentM.X<=0.||O.HalfExtentM.Y<=0.||O.HalfExtentM.GetMax()>1000.
                ||!Finite(O.YawRadians)||!Finite(O.HeightM)||O.HeightM<=0.||O.HeightM>1000.)
            {Failure=TEXT("Invalid or duplicate obstacle footprint");return false;}
            PreviousID=O.BuildingID;FObstacle W;W.Source=O;W.C=FMath::Cos(O.YawRadians);W.S=FMath::Sin(O.YawRadians);
            if(In.FoundationResponse)
            {++Out.Budget.FoundationCallbackCalls;if(!In.FoundationResponse(O,W.Response)||!ResponseValid(W.Response))
                {Failure=TEXT("Immutable foundation callback rejected obstacle");return false;}}
            const int32 OIndex=Obstacles.Add(W);
            // Spatial bins use the largest possible parcel radius. Only resident
            // cells are visited once per obstacle, bounded by MaxCells*MaxObstacles.
            const double Margin=In.CellSizeM/FMath::Sqrt(static_cast<double>(S.ParcelsPerReleaseCell)*UE_DOUBLE_PI)+.01;
            const double EX=FMath::Abs(W.C)*O.HalfExtentM.X+FMath::Abs(W.S)*O.HalfExtentM.Y+Margin;
            const double EY=FMath::Abs(W.S)*O.HalfExtentM.X+FMath::Abs(W.C)*O.HalfExtentM.Y+Margin;
            for(const auto& C:Cells)
            {if(!SpendWork(1))return false;const FVector2D Center=CellCenter(C.Source.Coord);
                if(FMath::Abs(Center.X-O.CenterM.X)<=EX+In.CellSizeM*.5&&FMath::Abs(Center.Y-O.CenterM.Y)<=EY+In.CellSizeM*.5)
                    ObstacleBins.FindOrAdd(C.Source.Coord).Add(OIndex);}
        }
        return true;
    }
    FVector2D CellCenter(FIntPoint C)const
    {return In.GridOriginM+FVector2D((static_cast<double>(C.X)+.5)*In.CellSizeM,(static_cast<double>(C.Y)+.5)*In.CellSizeM);}
    bool PositionCoord(const FVector2D& P,FIntPoint& C)const
    {
        const double X=(P.X-In.GridOriginM.X)/In.CellSizeM,Y=(P.Y-In.GridOriginM.Y)/In.CellSizeM;
        if(!Finite(P)||!Finite(X)||!Finite(Y)||X<MIN_int32+4.||X>MAX_int32-4.||Y<MIN_int32+4.||Y>MAX_int32-4.)return false;
        C=FIntPoint(FMath::FloorToInt(X),FMath::FloorToInt(Y));return true;
    }
    bool Surface(const FVector2D& P,FSurface& S)
    {
        if(!SpendWork(4))return false;
        ++Out.Budget.SurfaceSamples;
        const FVector2D Shift=P-FVector2D(In.CellSizeM*.5,In.CellSizeM*.5);
        FIntPoint C;if(!PositionCoord(Shift,C))return false;
        const double U=(P.X-In.GridOriginM.X)/In.CellSizeM-.5-C.X;
        const double V=(P.Y-In.GridOriginM.Y)/In.CellSizeM-.5-C.Y;
        const FIntPoint At[4]={C,C+FIntPoint(1,0),C+FIntPoint(0,1),C+FIntPoint(1,1)};
        for(int32 J=0;J<4;++J)
        {const int32* Index=ByCoord.Find(At[J]);if(!Index||!Cells[*Index].Source.bLand)return false;S.Indices[J]=*Index;}
        const double CU=FMath::Clamp(U,0.,1.),CV=FMath::Clamp(V,0.,1.);
        S.Weights[0]=(1.-CU)*(1.-CV);S.Weights[1]=CU*(1.-CV);S.Weights[2]=(1.-CU)*CV;
        // Nonnegative partition with a final exact remainder; no negative tiny
        // deposit and no independently renormalized mass/volume inventories.
        double Remaining=1.;for(int32 J=0;J<3;++J){S.Weights[J]=FMath::Clamp(S.Weights[J],0.,Remaining);Remaining-=S.Weights[J];}S.Weights[3]=Remaining;
        double H[4];for(int32 J=0;J<4;++J){if(S.Weights[J]<-1.e-12)return false;H[J]=Cells[S.Indices[J]].FrozenBedM;}
        S.Z=H[0]*S.Weights[0]+H[1]*S.Weights[1]+H[2]*S.Weights[2]+H[3]*S.Weights[3];
        const double DX=((1.-V)*(H[1]-H[0])+V*(H[3]-H[2]))/In.CellSizeM;
        const double DY=((1.-U)*(H[2]-H[0])+U*(H[3]-H[1]))/In.CellSizeM;
        S.Normal=FVector(-DX,-DY,1.).GetSafeNormal();return Finite(S.Z)&&Finite(S.Normal)&&S.Normal.Z>0.;
    }
    FAftershockMassEligibility Eligibility(const FAftershockMassRelease& R)
    {
        FAftershockMassEligibility E;const int32* Index=ByCoord.Find(R.Coord);
        if(!Index||!Cells[*Index].Source.bLand){E.Reason=TEXT("Release requires supported land");return E;}
        if(!Finite(R.AvailableMassFraction)||R.AvailableMassFraction<=0.||R.AvailableMassFraction>1.||!Finite(R.InitialVelocityMps)
            ||R.InitialVelocityMps.Size()>In.Settings.MaxSupportedSpeedMps)
        {E.Reason=TEXT("Invalid release fraction or explicit initial velocity");return E;}
        const auto& W=Cells[*Index];const auto& C=W.Source;
        FSurface S;const auto Center=CellCenter(R.Coord);
        if(!Surface(Center,S)){E.Reason=TEXT("Source lacks valid local slope samples");return E;}
        // Initial subcell parcel placement requires these same supported samples.
        for(double Y:{-.24,.24})for(double X:{-.24,.24})
        {FSurface Test;if(!Surface(Center+FVector2D(X,Y)*In.CellSizeM,Test)){E.Reason=TEXT("Release patch touches unsupported boundary");return E;}}
        E.SlopeDegrees=FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(S.Normal.Z,0.,1.)));
        E.AvailableMassKg=W.AvailableKg;E.AvailableVolumeM3=W.AvailableKg/W.Density;
        if(In.Material==EAftershockMassMaterial::Snow)
        {
            const double Depth=C.SnowWaterEquivalentMm/C.SnowDensityKgM3;
            if(Depth<In.Settings.MinimumSnowDepthM||W.AvailableKg<=0.){E.Reason=TEXT("No sufficient finite snow inventory");return E;}
            const double Theta=FMath::DegreesToRadians(E.SlopeDegrees);
            const double Shear=C.SnowDensityKgM3*G*Depth*FMath::Sin(Theta)*FMath::Cos(Theta);
            E.FactorOfSafety=Shear>1.e-9?C.SnowWeakLayerStrengthPa/Shear:1.e12;
            if(!R.bForceRelease&&(E.SlopeDegrees<In.Settings.MinimumSnowSlopeDegrees||E.SlopeDegrees>In.Settings.MaximumSnowSlopeDegrees||E.FactorOfSafety>=1.))
            {E.Reason=TEXT("Snow slab slope/weak-layer gate is stable or unsupported");return E;}
        }
        else
        {
            if(C.RegolithVerticalDepthM<In.Settings.MinimumSoilDepthM||W.AvailableKg<=0.){E.Reason=TEXT("No finite erodible regolith");return E;}
            E.FactorOfSafety=FAftershockMassMovementSolver::ShallowSoilFactorOfSafety(FMath::DegreesToRadians(E.SlopeDegrees),
                C.RegolithVerticalDepthM,C.SoilDensityKgM3,C.CohesionPa,FMath::DegreesToRadians(C.FrictionAngleDegrees),C.EffectiveSaturation01);
            if(!R.bForceRelease&&(E.SlopeDegrees<In.Settings.MinimumSoilSlopeDegrees||E.FactorOfSafety>=1.))
            {E.Reason=TEXT("Shallow-soil stability gate is stable");return E;}
        }
        if(E.AvailableVolumeM3*R.AvailableMassFraction/Area>In.Settings.MaxParcelDepthM)
        {E.Reason=TEXT("Release thickness exceeds numerical material budget");return E;}
        E.bEligible=true;return E;
    }
    FHit Sweep(const FVector2D& A,const FVector2D& B,double Radius)
    {
        FHit Hit;FIntPoint CA,CB;if(!PositionCoord(A,CA)||!PositionCoord(B,CB))return Hit;
        TArray<int32,TInlineAllocator<32>> Candidates;
        for(int32 Y=FMath::Min(CA.Y,CB.Y);Y<=FMath::Max(CA.Y,CB.Y);++Y)
            for(int32 X=FMath::Min(CA.X,CB.X);X<=FMath::Max(CA.X,CB.X);++X)
                if(const auto* Bin=ObstacleBins.Find(FIntPoint(X,Y)))for(int32 O:*Bin)Candidates.AddUnique(O);
        Candidates.Sort();
        for(int32 Index:Candidates)
        {
            if(!SpendWork(1))return Hit;
            ++Out.Budget.ObstacleTests;const auto& O=Obstacles[Index];if(!O.Response.bBlocksFlow)continue;
            const FVector2D P=O.Local(A),D=O.Local(B)-P,Ext=O.Source.HalfExtentM+FVector2D(Radius,Radius);
            double Enter=0.,Exit=1.;FVector2D Normal=FVector2D::ZeroVector;bool Good=true;
            const bool Inside=FMath::Abs(P.X)<Ext.X-1.e-8&&FMath::Abs(P.Y)<Ext.Y-1.e-8;
            for(int32 Axis=0;Axis<2&&Good;++Axis)
            {
                if(FMath::Abs(D[Axis])<1.e-12){if(P[Axis]<-Ext[Axis]||P[Axis]>Ext[Axis])Good=false;continue;}
                double T0=(-Ext[Axis]-P[Axis])/D[Axis],T1=(Ext[Axis]-P[Axis])/D[Axis];
                FVector2D N=FVector2D::ZeroVector;N[Axis]=-1.;if(T0>T1){Swap(T0,T1);N[Axis]=1.;}
                if(T0>Enter){Enter=T0;Normal=N;}Exit=FMath::Min(Exit,T1);Good=Enter<=Exit;
            }
            if(Inside){Enter=0.;Normal=(-D).GetSafeNormal();if(Normal.IsNearlyZero())Normal=FVector2D(1.,0.);}
            const bool Approaching=FVector2D::DotProduct(D,Normal)<-1.e-10;
            if(Good&&Enter>=0.&&Enter<=Hit.Fraction&&(Inside||Approaching))
            {Hit.Obstacle=Index;Hit.Fraction=Enter;const auto N=O.WorldDirection(Normal);Hit.Normal=FVector(N.X,N.Y,0.);}
        }
        return Hit;
    }
    bool ReleaseParcels()
    {
        TArray<FAftershockMassRelease> Requests=In.Release;
        Requests.Sort([](const auto& A,const auto& B){return A.Coord.Y!=B.Coord.Y?A.Coord.Y<B.Coord.Y:A.Coord.X<B.Coord.X;});
        if(Requests.IsEmpty()||static_cast<int64>(Requests.Num())*In.Settings.ParcelsPerReleaseCell>In.Settings.MaxParcels)
        {Failure=TEXT("Empty release or parcel allocation budget exceeded");return false;}
        TSet<FIntPoint> Seen;
        for(const auto& R:Requests)
        {if(Seen.Contains(R.Coord)){Failure=TEXT("Duplicate release cell");return false;}Seen.Add(R.Coord);
            const auto E=Eligibility(R);if(!E.bEligible){Failure=E.Reason;return false;}}
        // All original eligibility checks finish before any staged withdrawal.
        for(const auto& R:Requests)
        {
            const int32 Index=ByCoord.FindChecked(R.Coord);auto& W=Cells[Index];auto& C=Out.Cells[Index];
            const double Mass=W.AvailableKg*R.AvailableMassFraction,Volume=Mass/W.Density;
            W.AvailableKg-=Mass;C.ReleasedMassKg+=Mass;C.RemovedVolumeM3+=Volume;Out.Budget.ReleasedKg+=Mass;
            W.FrozenBedM-=Volume/Area;
        }
        for(int32 I=0;I<Cells.Num();++I)Out.Cells[I].RunoutBedElevationM=Cells[I].FrozenBedM;
        for(const auto& R:Requests)
        {
            const int32 Index=ByCoord.FindChecked(R.Coord);const auto& W=Cells[Index];const auto& C=Out.Cells[Index];
            const int32 Count=In.Settings.ParcelsPerReleaseCell;
            const uint32 Seed=HashCombine(HashCombine(In.Settings.Seed,GetTypeHash(R.Coord)),static_cast<uint32>(In.HazardTypeTag));
            FRandomStream Random(static_cast<int32>(Seed));
            for(int32 J=0;J<Count;++J)
            {
                FParcel P;P.ID=Parcels.Num();P.Area=Area/Count;P.Mass=C.ReleasedMassKg/Count;P.Volume=P.Mass/W.Density;
                P.XY=CellCenter(R.Coord)+FVector2D(Random.FRandRange(-.22f,.22f),Random.FRandRange(-.22f,.22f))*In.CellSizeM;
                FSurface S;if(!Surface(P.XY,S)){Failure=TEXT("Staged release support unavailable");return false;}
                if(Sweep(P.XY,P.XY,P.Radius()).Obstacle!=INDEX_NONE)
                {Failure=TEXT("Release intersects a solid foundation footprint; source-undermining model is not enabled");return false;}
                P.SurfaceZ=S.Z;P.Velocity=R.InitialVelocityMps-S.Normal*FVector::DotProduct(R.InitialVelocityMps,S.Normal);
                P.LastSupportNormal=S.Normal;for(int32 SupportSlot=0;SupportSlot<4;++SupportSlot)P.LastSupport[SupportSlot]=S.Indices[SupportSlot];
                Out.Budget.InitialKineticJ+=P.Kinetic();Out.Budget.MaximumSpeedMps=FMath::Max(Out.Budget.MaximumSpeedMps,P.Velocity.Size());Parcels.Add(P);
            }
        }
        Out.Budget.InitialParcels=Parcels.Num();Out.Budget.PeakActiveParcels=Parcels.Num();return true;
    }
    void Deposit(FParcel& P,const FSurface& S)
    {
        double RemainingMass=P.Mass,RemainingVolume=P.Volume;
        for(int32 J=0;J<4;++J)
        {
            const double M=J==3?RemainingMass:FMath::Min(RemainingMass,P.Mass*S.Weights[J]),V=J==3?RemainingVolume:FMath::Min(RemainingVolume,P.Volume*S.Weights[J]);
            Out.Cells[S.Indices[J]].DepositedMassKg+=M;Out.Cells[S.Indices[J]].DepositedVolumeM3+=V;
            RemainingMass-=M;RemainingVolume-=V;
        }
        Out.Budget.DissipatedJ+=P.Kinetic();P.Velocity=FVector::ZeroVector;P.bMoving=false;
    }
    void Entrain(FParcel& P,double DT)
    {
        if(In.Settings.EntrainFractionPerSecond<=0.)return;
        FIntPoint C;if(!PositionCoord(P.XY,C))return;const int32* Index=ByCoord.Find(C);if(!Index)return;
        auto& W=Cells[*Index];if(W.AvailableKg<=0.||!W.Source.bLand)return;
        const double VolumeRoom=FMath::Max(0.,P.Area*In.Settings.MaxParcelDepthM-P.Volume);
        const double Added=FMath::Min3(W.AvailableKg,W.AvailableKg*In.Settings.EntrainFractionPerSecond*DT,VolumeRoom*W.Density);
        if(Added<=0.)return;const double OldK=P.Kinetic(),OldMass=P.Mass;
        P.Mass+=Added;P.Volume+=Added/W.Density;P.Velocity*=OldMass/P.Mass;
        Out.Budget.DissipatedJ+=FMath::Max(0.,OldK-P.Kinetic());Out.Budget.EntrainedKg+=Added;W.AvailableKg-=Added;
        auto& R=Out.Cells[*Index];R.EntrainedMassKg+=Added;R.RemovedVolumeM3+=Added/W.Density;
        // Deliberate frozen-bed closure during an event: inventories change, but
        // newly entrained/deposited geometry is committed only after the solve.
    }
    bool StepParcel(FParcel& P,double DT)
    {
        if(++Out.Budget.ParticleSteps>In.Settings.MaxParticleSteps){Failure=TEXT("Particle work budget exhausted");return false;}
        Entrain(P,DT);FSurface Old;if(!Surface(P.XY,Old)){Failure=TEXT("Active parcel lost its supported surface");return false;}
        FIntPoint C;if(!PositionCoord(P.XY,C)){Failure=TEXT("Nonfinite parcel coordinate");return false;}
        const int32* Cell=ByCoord.Find(C);if(!Cell){Failure=TEXT("Missing parcel material cell");return false;}
        const auto& F=Cells[*Cell].Friction;
        const double OldK=P.Kinetic();const FVector Gravity(0.,0.,-G);
        FVector V=P.Velocity-Old.Normal*FVector::DotProduct(P.Velocity,Old.Normal);
        V+=(Gravity-Old.Normal*FVector::DotProduct(Gravity,Old.Normal))*DT;
        const double Speed=V.Size();
        if(Speed>0.)
        {const double NewSpeed=FMath::Max(0.,Speed-F.Mu*G*Old.Normal.Z*DT)/(1.+DT*G*Speed/(F.XiMps2*FMath::Max(P.Depth(),.01)));
            V*=NewSpeed/Speed;}
        const FVector Average=(P.Velocity+V)*.5;
        FVector2D Target=P.XY+FVector2D(Average.X,Average.Y)*DT;
        if(!Finite(Target)||!Finite(V)){Failure=TEXT("Nonfinite velocity integration");return false;}
        const double Fraction=(Target-P.XY).Size()/In.CellSizeM;
        if(Fraction>In.Settings.CFL+1.e-6){Failure=TEXT("CFL displacement bound violated");return false;}
        Out.Budget.MaximumDisplacementCells=FMath::Max(Out.Budget.MaximumDisplacementCells,Fraction);
        FSurface Next;bool Export=!Surface(Target,Next);
        if(!Failure.IsEmpty())return false;
        if(Export)
        {
            // Locate the actual support edge without inventing terrain outside.
            FVector2D Low=P.XY,High=Target;Next=Old;
            for(int32 I=0;I<20;++I){const auto Mid=(Low+High)*.5;FSurface Test;
                if(Surface(Mid,Test)){Low=Mid;Next=Test;}else High=Mid;}
            Target=Low;
        }
        FHit Hit=Sweep(P.XY,Target,P.Radius());
        if(!Failure.IsEmpty())return false;
        if(Hit.Obstacle!=INDEX_NONE)
        {
            Target=P.XY+(Target-P.XY)*FMath::Max(0.,Hit.Fraction-1.e-5);
            if(!Surface(Target,Next)){Failure=TEXT("Obstacle contact lost terrain support");return false;}Export=false;
        }
        double Allowed=OldK+P.Mass*G*(P.SurfaceZ-Next.Z);
        if(Allowed<0.)
        {
            // Inertial uphill motion spends kinetic energy. Bisection shortens
            // the current subcell segment; no jumping/clamping across a ridge.
            FVector2D Low=P.XY,High=Target;FSurface Supported=Old;
            for(int32 I=0;I<24;++I){const auto Mid=(Low+High)*.5;FSurface Test;
                if(Surface(Mid,Test)&&OldK+P.Mass*G*(P.SurfaceZ-Test.Z)>=0.){Low=Mid;Supported=Test;}else High=Mid;}
            Target=Low;Next=Supported;V=FVector::ZeroVector;Hit=FHit();Export=false;
            Allowed=FMath::Max(0.,OldK+P.Mass*G*(P.SurfaceZ-Next.Z));
        }
        V-=Next.Normal*FVector::DotProduct(V,Next.Normal);
        const double TrialK=.5*P.Mass*V.SizeSquared();if(TrialK>Allowed&&TrialK>0.)V*=FMath::Sqrt(FMath::Max(0.,Allowed)/TrialK);
        if(Hit.Obstacle!=INDEX_NONE)
        {
            const auto& O=Obstacles[Hit.Obstacle];
            if(P.Depth()>O.Source.HeightM)
            {Failure=TEXT("Flow overtops a footprint; full-height obstacle closure unsupported for this event, do not commit");return false;}
            const FVector WallN=(Hit.Normal-Next.Normal*FVector::DotProduct(Hit.Normal,Next.Normal)).GetSafeNormal();
            const double Into=FMath::Min(0.,FVector::DotProduct(V,WallN));const FVector Before=V;
            V=(V-WallN*Into)*O.Response.TangentialRetention-WallN*Into*O.Response.NormalRestitution;
            const int32* FoundImpact=ImpactIndices.Find(O.Source.BuildingID);
            FAftershockMassImpact* Impact=nullptr;
            if(!FoundImpact){const int32 Index=Out.Impacts.AddDefaulted();ImpactIndices.Add(O.Source.BuildingID,Index);Impact=&Out.Impacts[Index];Impact->BuildingID=O.Source.BuildingID;}
            else Impact=&Out.Impacts[*FoundImpact];
            const double ContactSeconds=Time+DT*FMath::Clamp(Hit.Fraction,0.,1.);
            const double Pressure=O.Response.ImpactCoefficient*P.Density()*Into*Into;
            if(Impact->FirstContactSeconds<0.)Impact->FirstContactSeconds=ContactSeconds;
            else Impact->FirstContactSeconds=FMath::Min(Impact->FirstContactSeconds,ContactSeconds);
            if(Impact->PeakPressureSeconds<0.||Pressure>Impact->PeakNormalPressurePa)Impact->PeakPressureSeconds=ContactSeconds;
            Impact->PeakNormalPressurePa=FMath::Max(Impact->PeakNormalPressurePa,Pressure);
            Impact->ImpulseMagnitudeNs+=P.Mass*(Before-V).Size();Impact->PeakFlowDepthM=FMath::Max(Impact->PeakFlowDepthM,P.Depth());++Impact->Contacts;
        }
        const double NewK=.5*P.Mass*V.SizeSquared();
        if(!Finite(NewK)||!Finite(Allowed)||NewK>Allowed+FMath::Max(1.e-7,FMath::Abs(Allowed)*1.e-10))
        {Failure=TEXT("Kinetic energy gate failed");return false;}
        Out.Budget.GravityWorkJ+=P.Mass*G*(P.SurfaceZ-Next.Z);
        Out.Budget.DissipatedJ+=FMath::Max(0.,Allowed-NewK);
        P.XY=Target;P.SurfaceZ=Next.Z;P.Velocity=V;
        const double CurrentSpeed=V.Size();Out.Budget.MaximumSpeedMps=FMath::Max(Out.Budget.MaximumSpeedMps,CurrentSpeed);
        if(CurrentSpeed>In.Settings.MaxSupportedSpeedMps){Failure=TEXT("Supported velocity budget exceeded");return false;}
        if(Export)
        {
            if(In.Settings.bExportAtDomainBoundary)
            {Out.Budget.ExportedKg+=P.Mass;Out.Budget.ExportedKineticJ+=P.Kinetic();P.Velocity=FVector::ZeroVector;P.bMoving=false;}
            else {Failure=TEXT("Runout reaches unsupported domain boundary");return false;}
            return true;
        }
        const double TangentGravity=G*FMath::Sqrt(FMath::Max(0.,1.-Next.Normal.Z*Next.Normal.Z));
        const bool CanHold=TangentGravity<=F.Mu*G*Next.Normal.Z+1.e-6||Hit.Obstacle!=INDEX_NONE;
        P.bLastCanHold=CanHold;P.LastSupportNormal=Next.Normal;for(int32 J=0;J<4;++J)P.LastSupport[J]=Next.Indices[J];
        P.HeldSeconds=CurrentSpeed<=In.Settings.StopSpeedMps&&CanHold?P.HeldSeconds+DT:0.;
        if(P.HeldSeconds>=In.Settings.StopHoldSeconds)Deposit(P,Next);
        return true;
    }
    void RecordFields()
    {
#if WITH_EDITOR
        if(bDenseFieldsReferenceForAudit){RecordFieldsDenseReferenceForAudit();return;}
#endif
        ++FieldWork.RecordCalls;
        // Only last pass's touched cells can retain a nonzero scratch value or
        // FinalMovingMassKg. Deposited/arrival/peak/source history is untouched.
        if(!SpendWork(static_cast<int64>(ActiveFieldIndices.Num())*6))return;
        FieldWork.SparseClearedIndices+=ActiveFieldIndices.Num();
        for(int32 Index:ActiveFieldIndices)
        {
            MassScratch[Index]=VolumeScratch[Index]=PeakSpeedScratch[Index]=0.;
            FastVelocityScratch[Index]=FVector::ZeroVector;
            Out.Cells[Index].FinalMovingMassKg=0.;FieldTouchedMask[Index]=false;
        }
        ActiveFieldIndices.Reset();
        // Keep EXACT original parcel order, four-neighbour order, residual split
        // and additions. Sorting never changes floating-point accumulation.
        for(const auto& P:Parcels)if(P.bMoving)
        {
            FSurface S;if(!Surface(P.XY,S))continue;
            if(!SpendWork(4))return;
            double LeftM=P.Mass,LeftV=P.Volume;
            for(int32 J=0;J<4;++J)
            {const double M=J==3?LeftM:FMath::Min(LeftM,P.Mass*S.Weights[J]),V=J==3?LeftV:FMath::Min(LeftV,P.Volume*S.Weights[J]);
                const int32 Index=S.Indices[J];++FieldWork.SparseScatteredIndices;
                if(!FieldTouchedMask[Index]){FieldTouchedMask[Index]=true;ActiveFieldIndices.Add(Index);}
                MassScratch[Index]+=M;VolumeScratch[Index]+=V;
                if(M>0.&&P.Velocity.Size()>PeakSpeedScratch[Index]){PeakSpeedScratch[Index]=P.Velocity.Size();FastVelocityScratch[Index]=P.Velocity;}
                LeftM-=M;LeftV-=V;}
        }
        FieldWork.PeakTouchedIndices=FMath::Max(FieldWork.PeakTouchedIndices,ActiveFieldIndices.Num());
        // Cells/Out.Cells were canonically sorted once during Initialize.
        // A conservative nlogn charge bounds this sort; actual comparator calls
        // are also separately counted, not hidden as constant-time work.
        int32 Passes=0;for(int32 N=ActiveFieldIndices.Num();N>1;N=(N+1)/2)++Passes;
        const int64 SortUnits=static_cast<int64>(ActiveFieldIndices.Num())*FMath::Max(1,Passes*4);
        if(!SpendWork(SortUnits))return;FieldWork.SparseSortReservedUnits+=SortUnits;
        ActiveFieldIndices.Sort([this](int32 A,int32 B){++FieldWork.SparseSortComparisons;return A<B;});
        if(!SpendWork(static_cast<int64>(ActiveFieldIndices.Num())*4))return;
        FieldWork.SparseUpdatedIndices+=ActiveFieldIndices.Num();
        for(int32 I:ActiveFieldIndices)
        {
            auto& C=Out.Cells[I];C.FinalMovingMassKg=MassScratch[I];if(MassScratch[I]<=1.e-12)continue;
            const double Speed=PeakSpeedScratch[I],Depth=VolumeScratch[I]/Area,Density=MassScratch[I]/VolumeScratch[I];
            if(C.FirstArrivalSeconds<0.)C.FirstArrivalSeconds=Time;
            C.PeakFlowDepthM=FMath::Max(C.PeakFlowDepthM,Depth);C.PeakDynamicPressurePa=FMath::Max(C.PeakDynamicPressurePa,Density*Speed*Speed);
            if(Speed>C.PeakSpeedMps){C.PeakSpeedMps=Speed;C.PeakVelocityMps=FastVelocityScratch[I];}
        }
    }
#if WITH_EDITOR
    void RecordFieldsDenseReferenceForAudit()
    {
        ++FieldWork.RecordCalls;FieldWork.DenseResetAndScanUnits+=static_cast<int64>(Cells.Num())*8;
        if(!SpendWork(static_cast<int64>(Cells.Num())*8))return;
        // Reuse bounded scratch capacity; do not heap-allocate per parcel/step.
        MassScratch.Init(0.,Cells.Num());VolumeScratch.Init(0.,Cells.Num());
        PeakSpeedScratch.Init(0.,Cells.Num());FastVelocityScratch.Init(FVector::ZeroVector,Cells.Num());
        for(const auto& P:Parcels)if(P.bMoving)
        {
            FSurface S;if(!Surface(P.XY,S))continue;
            double LeftM=P.Mass,LeftV=P.Volume;
            for(int32 J=0;J<4;++J)
            {const double M=J==3?LeftM:FMath::Min(LeftM,P.Mass*S.Weights[J]),V=J==3?LeftV:FMath::Min(LeftV,P.Volume*S.Weights[J]);
                const int32 Index=S.Indices[J];MassScratch[Index]+=M;VolumeScratch[Index]+=V;
                if(M>0.&&P.Velocity.Size()>PeakSpeedScratch[Index]){PeakSpeedScratch[Index]=P.Velocity.Size();FastVelocityScratch[Index]=P.Velocity;}
                LeftM-=M;LeftV-=V;}
        }
        for(int32 I=0;I<Cells.Num();++I)
        {
            auto& C=Out.Cells[I];C.FinalMovingMassKg=MassScratch[I];if(MassScratch[I]<=1.e-12)continue;
            const double Speed=PeakSpeedScratch[I],Depth=VolumeScratch[I]/Area,Density=MassScratch[I]/VolumeScratch[I];
            if(C.FirstArrivalSeconds<0.)C.FirstArrivalSeconds=Time;
            C.PeakFlowDepthM=FMath::Max(C.PeakFlowDepthM,Depth);C.PeakDynamicPressurePa=FMath::Max(C.PeakDynamicPressurePa,Density*Speed*Speed);
            if(Speed>C.PeakSpeedMps){C.PeakSpeedMps=Speed;C.PeakVelocityMps=FastVelocityScratch[I];}
        }
    }
#endif
    void Frame(bool Final=false)
    {
        // Actual retained full-grid replay scans are independent diagnostics.
        // MaxFrames<=64 bounds this to <=65 calls including final replacement.
        // Never spend numerical work or alter moving mass for export detail.
        ++FieldWork.ReplayFrameCalls;FieldWork.ReplayCellScanIndices+=Out.Cells.Num();
        FAftershockMassFrame F;F.Seconds=Time;
        for(const auto& P:Parcels)if(P.bMoving)
        {FAftershockMassParcelFrame V;V.ParcelID=P.ID;V.PositionM=FVector(P.XY.X,P.XY.Y,P.SurfaceZ+P.Depth()*.5);
            V.VelocityMps=P.Velocity;V.MassKg=P.Mass;V.VolumeM3=P.Volume;V.FootprintAreaM2=P.Area;F.Parcels.Add(V);}
        for(int32 I=0;I<Out.Cells.Num();++I)
        {
            const auto& C=Out.Cells[I];
            if(MassScratch[I]<=0.&&C.DepositedMassKg<=0.)continue;
            FAftershockMassFrameCell V;V.Coord=C.Coord;V.MovingMassKg=MassScratch[I];
            V.MovingVolumeM3=VolumeScratch[I];V.FlowDepthM=VolumeScratch[I]/Area;
            V.DepositedMassKg=C.DepositedMassKg;V.DepositedVolumeM3=C.DepositedVolumeM3;F.Cells.Add(V);
        }
        F.Impacts=Out.Impacts;F.Impacts.Sort([](const auto& A,const auto& B){return A.BuildingID<B.BuildingID;});
        const bool Replace=Final&&Out.Frames.Num()>=In.Settings.MaxFrames;
        const int32 OldCells=Replace?Out.Frames.Last().Cells.Num():0,OldParcels=Replace?Out.Frames.Last().Parcels.Num():0;
        const int32 OldImpacts=Replace?Out.Frames.Last().Impacts.Num():0;
        if(Out.ExportedFrameCellRecords-OldCells+F.Cells.Num()>FAftershockMassResult::MaximumFrameCellRecords
            ||Out.ExportedFrameParcelRecords-OldParcels+F.Parcels.Num()>FAftershockMassResult::MaximumFrameParcelRecords
            ||Out.ExportedFrameImpactRecords-OldImpacts+F.Impacts.Num()>FAftershockMassResult::MaximumFrameImpactRecords)
        {
            Out.bFrameExportComplete=false;
            Out.FrameExportFailure=TEXT("Bounded replay export exhausted; physics unchanged, do not claim complete replay");
            return;
        }
        if(Out.Frames.Num()<In.Settings.MaxFrames||Replace)
        {
            Out.ExportedFrameCellRecords+=F.Cells.Num()-OldCells;Out.ExportedFrameParcelRecords+=F.Parcels.Num()-OldParcels;
            Out.ExportedFrameImpactRecords+=F.Impacts.Num()-OldImpacts;
            if(Replace)Out.Frames.Last()=MoveTemp(F);else Out.Frames.Add(MoveTemp(F));
        }
    }
    void Finish()
    {
        Out.Budget.MovingKg=0.;Out.Budget.MovingKineticJ=0.;Out.Budget.DepositedKg=0.;
        for(const auto& P:Parcels)if(P.bMoving){Out.Budget.MovingKg+=P.Mass;Out.Budget.MovingKineticJ+=P.Kinetic();}
        for(int32 I=0;I<Out.Cells.Num();++I){Out.Budget.DepositedKg+=Out.Cells[I].DepositedMassKg;Out.Cells[I].RemainingSourceMaterialKg=Cells[I].AvailableKg;}
        const double Source=Out.Budget.ReleasedKg+Out.Budget.EntrainedKg;
        Out.Budget.MassResidualKg=Source-Out.Budget.MovingKg-Out.Budget.DepositedKg-Out.Budget.ExportedKg;
        Out.Budget.EnergyResidualJ=Out.Budget.InitialKineticJ+Out.Budget.GravityWorkJ-Out.Budget.DissipatedJ-Out.Budget.MovingKineticJ-Out.Budget.ExportedKineticJ;
        Out.bMassConserved=Finite(Out.Budget.MassResidualKg)&&FMath::Abs(Out.Budget.MassResidualKg)<=FMath::Max(1.e-6,Source*1.e-8);
        const double EnergyScale=FMath::Max(FMath::Abs(Out.Budget.GravityWorkJ)+Out.Budget.InitialKineticJ,1.);
        Out.bEnergyClosed=Finite(Out.Budget.EnergyResidualJ)&&FMath::Abs(Out.Budget.EnergyResidualJ)<=FMath::Max(1.e-6,EnergyScale*1.e-8);
        Out.bComplete=Out.Budget.MovingKg<=1.e-9&&Failure.IsEmpty();
        Out.bAccepted=Out.bComplete&&Out.bMassConserved&&Out.bEnergyClosed;
        if(Failure.IsEmpty()&&!Out.bComplete)Failure=TEXT("Time/substep budget ended with explicitly retained moving mass; do not commit");
        if(Failure.IsEmpty()&&(!Out.bMassConserved||!Out.bEnergyClosed))Failure=TEXT("Conservation acceptance gate failed");
        Out.Failure=Failure;Out.SimulatedSeconds=Time;Out.Impacts.Sort([](const auto& A,const auto& B){return A.BuildingID<B.BuildingID;});
        if(!Out.bAccepted&&FParse::Param(FCommandLine::Get(),TEXT("AftershockMassDiagnostics")))
        {
            int32 Moving=0,LowSpeed=0,CannotHold=0,ZeroSpeedCannotHold=0,ValidSupport=0;
            double EndMaxSpeed=0.,EndMinSpeed=TNumericLimits<double>::Max(),MaxHeld=0.,MinNormalZ=1.;
            for(const auto& P:Parcels)if(P.bMoving)
            {
                ++Moving;const double Speed=P.Velocity.Size();EndMaxSpeed=FMath::Max(EndMaxSpeed,Speed);EndMinSpeed=FMath::Min(EndMinSpeed,Speed);
                LowSpeed+=Speed<=In.Settings.StopSpeedMps;CannotHold+=!P.bLastCanHold;ZeroSpeedCannotHold+=Speed<=1.e-12&&!P.bLastCanHold;
                MaxHeld=FMath::Max(MaxHeld,P.HeldSeconds);MinNormalZ=FMath::Min(MinNormalZ,P.LastSupportNormal.Z);
                bool Valid=true;for(int32 J=0;J<4;++J)Valid&=Cells.IsValidIndex(P.LastSupport[J]);ValidSupport+=Valid;
            }
            UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_MASS_TERMINAL_DIAGNOSTICS simulatedSeconds=%.17g limitSeconds=%.17g substeps=%d limitSubsteps=%d particleSteps=%lld limitParticleSteps=%d workUnits=%lld limitWorkUnits=%lld movingKg=%.17g depositedKg=%.17g exportedKg=%.17g massResidualKg=%.17g energyResidualJ=%.17g movingParcels=%d lowSpeedParcels=%d cannotHoldParcels=%d zeroSpeedCannotHold=%d validFourCellSupport=%d finalMinSpeedMps=%.17g finalMaxSpeedMps=%.17g maxHeldSeconds=%.17g minSupportNormalZ=%.17g muUnchanged=1 forcedSettlement=0 reason=%s"),
                Time,In.Settings.MaximumSeconds,Out.Budget.Substeps,In.Settings.MaxSubsteps,Out.Budget.ParticleSteps,In.Settings.MaxParticleSteps,Out.Budget.WorkUnits,In.Settings.MaxWorkUnits,
                Out.Budget.MovingKg,Out.Budget.DepositedKg,Out.Budget.ExportedKg,Out.Budget.MassResidualKg,Out.Budget.EnergyResidualJ,Moving,LowSpeed,CannotHold,ZeroSpeedCannotHold,ValidSupport,
                Moving?EndMinSpeed:0.,EndMaxSpeed,MaxHeld,MinNormalZ,*Failure);
            int32 Logged=0;
            for(const auto& P:Parcels)if(P.bMoving&&Logged++<16)
            {
                FIntPoint Coord(MAX_int32,MAX_int32);const bool Positioned=PositionCoord(P.XY,Coord);const int32* Index=Positioned?ByCoord.Find(Coord):nullptr;
                const double Mu=Index?Cells[*Index].Friction.Mu:-1.,Xi=Index?Cells[*Index].Friction.XiMps2:-1.;
                const auto SupportCoord=[&](int32 J){return Cells.IsValidIndex(P.LastSupport[J])?Cells[P.LastSupport[J]].Source.Coord:FIntPoint(MAX_int32,MAX_int32);};
                const auto A=SupportCoord(0),B=SupportCoord(1),C=SupportCoord(2),D=SupportCoord(3);
                UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_MASS_TERMINAL_PARCEL id=%d cell=(%d,%d) xM=%.17g yM=%.17g zM=%.17g vxMps=%.17g vyMps=%.17g vzMps=%.17g massKg=%.17g heldSeconds=%.17g canHold=%d normal=(%.17g,%.17g,%.17g) mu=%.17g xiMps2=%.17g support=(%d,%d);(%d,%d);(%d,%d);(%d,%d) maximumLogged=16 diagnosticOnly=1"),
                    P.ID,Coord.X,Coord.Y,P.XY.X,P.XY.Y,P.SurfaceZ,P.Velocity.X,P.Velocity.Y,P.Velocity.Z,P.Mass,P.HeldSeconds,P.bLastCanHold,
                    P.LastSupportNormal.X,P.LastSupportNormal.Y,P.LastSupportNormal.Z,Mu,Xi,A.X,A.Y,B.X,B.Y,C.X,C.Y,D.X,D.Y);
            }
        }
    }
    FAftershockMassResult Run()
    {
        const double Started=FPlatformTime::Seconds();
        if(!Initialize()||!ReleaseParcels())
        {Out.Failure=Failure;Out.Budget.ComputeMilliseconds=(FPlatformTime::Seconds()-Started)*1000.;return MoveTemp(Out);}
        RecordFields();Frame();double NextFrame=In.Settings.MaximumSeconds/(In.Settings.MaxFrames-1);
        while(Time<In.Settings.MaximumSeconds&&Out.Budget.Substeps<In.Settings.MaxSubsteps&&Failure.IsEmpty())
        {
            double Speed=0.,Depth=0.;int32 Active=0;
            for(const auto& P:Parcels)if(P.bMoving){++Active;Speed=FMath::Max(Speed,P.Velocity.Size());Depth=FMath::Max(Depth,P.Depth());}
            if(Active==0)break;
            const double StableDT=FMath::Min(In.Settings.MaxStepSeconds,
                In.Settings.CFL*In.CellSizeM/FMath::Max(.01,Speed+G*In.Settings.MaxStepSeconds+FMath::Sqrt(G*Depth)));
            // The numerical stability bound and the final duration remainder
            // are different constraints. Integrate the final representable
            // remainder, including a few ulps after repeated decimal steps.
            // Retained moving mass still fails the independent completion gate.
            if(!Finite(StableDT)||StableDT<1.e-7){Failure=TEXT("Minimum stable timestep budget exceeded");break;}
            const double DT=FMath::Min(StableDT,In.Settings.MaximumSeconds-Time);
            if(!Finite(DT)||DT<=0.||Time+DT<=Time){Failure=TEXT("Physical mass timestep cannot represent progress");break;}
            ++Out.Budget.Substeps;
            // A full substep is pre-budgeted so no parcel advances beyond a peer
            // solely because the work budget ended midway through this pass.
            if(Out.Budget.ParticleSteps+Active>In.Settings.MaxParticleSteps){Failure=TEXT("Particle work budget exhausted");break;}
            bool Advanced=true;for(auto& P:Parcels)if(P.bMoving&&!StepParcel(P,DT)){Advanced=false;break;}
            if(!Advanced)break;Time+=DT;RecordFields();
            if(Time+1.e-10>=NextFrame){Frame();NextFrame+=In.Settings.MaximumSeconds/(In.Settings.MaxFrames-1);}
        }
        RecordFields();Frame(true);Finish();Out.Budget.ComputeMilliseconds=(FPlatformTime::Seconds()-Started)*1000.;return MoveTemp(Out);
    }
};
}

double FAftershockMassMovementSolver::ShallowSoilFactorOfSafety(double Theta,double Z,double Density,double Cohesion,double Phi,double Saturation)
{
    using namespace AftershockMassKernelDetail;
    if(!Finite(Theta)||Theta<0.||Theta>=UE_DOUBLE_PI*.5||!Finite(Z)||Z<=0.||!Finite(Density)||Density<=0.
        ||!Finite(Cohesion)||Cohesion<0.||!Finite(Phi)||Phi<0.||Phi>=UE_DOUBLE_PI*.5||!Finite(Saturation)||Saturation<0.||Saturation>1.)return -1.;
    const double C=FMath::Cos(Theta),S=FMath::Sin(Theta),Driving=Density*G*Z*S*C;
    if(Driving<=1.e-10)return 1.e12;
    const double Normal=Density*G*Z*C*C,Pore=1000.*G*Saturation*Z*C*C;
    return (Cohesion+FMath::Max(0.,Normal-Pore)*FMath::Tan(Phi))/Driving;
}

double FAftershockMassMovementSolver::EffectiveSoilCoulombCoefficient(double DryMu,double Saturation,double Density)
{
    if(!FMath::IsFinite(DryMu)||DryMu<0.||!FMath::IsFinite(Saturation)||Saturation<0.||Saturation>1.
        ||!FMath::IsFinite(Density)||Density<=0.)return -1.;
    // The same hydrostatic u/sigma = rho_water*S/rho_bulk assumption as the
    // infinite-slope release gate. A wet slope must not trigger using reduced
    // effective stress, then run out as if its basal resistance were dry.
    return DryMu*FMath::Clamp(1.-1000.*Saturation/Density,0.,1.);
}

FAftershockMassEligibility FAftershockMassMovementSolver::EvaluateRelease(const FAftershockMassInput& Input,const FAftershockMassRelease& Release)
{
    using namespace AftershockMassKernelDetail;
    FKernel Kernel(Input);if(!Kernel.Initialize()){FAftershockMassEligibility E;E.Reason=Kernel.Failure;return E;}
    return Kernel.Eligibility(Release);
}

FAftershockMassResult FAftershockMassMovementSolver::Solve(const FAftershockMassInput& Input)
{AftershockMassKernelDetail::FKernel Kernel(Input);return Kernel.Run();}

#if WITH_EDITOR
FAftershockMassResult SolveAftershockMassFieldsForAudit(const FAftershockMassInput& Input,
    bool bDenseReference,FAftershockMassFieldWorkAudit& OutWork)
{
    AftershockMassKernelDetail::FKernel Kernel(Input);Kernel.bDenseFieldsReferenceForAudit=bDenseReference;
    FAftershockMassResult Result=Kernel.Run();OutWork=Kernel.FieldWork;return Result;
}
#endif

bool FAftershockMassMovementSolver::EvaluateReleaseCandidates(const FAftershockMassInput& Input,
    const TArray<FAftershockMassRelease>& Requests,TArray<FAftershockMassEligibility>& Out,FString& Failure)
{
    Out.Reset();Failure.Reset();
    if(Requests.Num()>65536){Failure=TEXT("Eligibility request budget exceeded");return false;}
    AftershockMassKernelDetail::FKernel Kernel(Input);
    if(!Kernel.Initialize()){Failure=Kernel.Failure;return false;}
    Out.Reserve(Requests.Num());
    for(const auto& Request:Requests)
    {
        auto Eligibility=Kernel.Eligibility(Request);
        if(!Kernel.Failure.IsEmpty()){Failure=Kernel.Failure;Out.Reset();return false;}
        Out.Add(MoveTemp(Eligibility));
    }
    return true;
}
