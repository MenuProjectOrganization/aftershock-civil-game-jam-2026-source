#include "AftershockHydraulicObstructions.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace AftershockWater
{
namespace ObstructionDetail
{
constexpr std::int64_t MaximumWorldMM=2000000000000ll;
constexpr std::int64_t MaximumCellMM=1000000ll;
constexpr std::int32_t MaximumGlobalCoord=1000000;
constexpr std::uint64_t HashInitial=14695981039346656037ull;
std::uint64_t Key(std::int32_t X,std::int32_t Y)
{return (std::uint64_t(std::uint32_t(X))<<32)|std::uint32_t(Y);}
void Hash(std::uint64_t& H,std::uint64_t V)
{for(int I=0;I<8;++I){H^=(V>>(8*I))&255u;H*=1099511628211ull;}}
std::uint64_t Bytes(std::uint64_t N,std::uint64_t S)
{return S&&N>UINT64_MAX/S?UINT64_MAX:N*S;}
bool Add(std::uint64_t& A,std::uint64_t B)
{if(B>UINT64_MAX-A)return false;A+=B;return true;}
std::int64_t FloorDiv(std::int64_t A,std::int64_t B)
{const auto Q=A/B,R=A%B;return Q-(R<0);}
bool Finite(double A){return std::isfinite(A);}
bool Fail(std::string& Error,const char* Why){Error=Why;return false;}
int Population(std::uint64_t Bits)
{int N=0;while(Bits){Bits&=Bits-1;++N;}return N;}
int Side(const FaceGeometry& F)
{
    if(F.NormalX==0.&&F.NormalY==-1.)return 0;
    if(F.NormalX==1.&&F.NormalY==0.)return 1;
    if(F.NormalX==0.&&F.NormalY==1.)return 2;
    if(F.NormalX==-1.&&F.NormalY==0.)return 3;
    return -1;
}
int Opposite(int SideIndex){return (SideIndex+2)%4;}
std::int64_t Cross(ObstructionPointMM A,ObstructionPointMM B,ObstructionPointMM C)
{return (B.X-A.X)*(C.Y-A.Y)-(B.Y-A.Y)*(C.X-A.X);}
// Arguments are LOCAL mm*64. The width/span admission bounds cap cross products
// below 2* (9*1e6*64)^2 = 6.64e17, safely inside signed int64 (no MSVC __int128).
bool IntersectsTile(const std::array<ObstructionPointMM,4>& P,
    std::int64_t X0,std::int64_t Y0,std::int64_t X1,std::int64_t Y1)
{
    std::int64_t MinX=P[0].X,MaxX=MinX,MinY=P[0].Y,MaxY=MinY;
    for(const auto& V:P){MinX=std::min(MinX,V.X);MaxX=std::max(MaxX,V.X);MinY=std::min(MinY,V.Y);MaxY=std::max(MaxY,V.Y);}
    if(MaxX<X0||MinX>X1||MaxY<Y0||MinY>Y1)return false;
    const std::array<ObstructionPointMM,4> R{{{X0,Y0},{X1,Y0},{X1,Y1},{X0,Y1}}};
    for(int Edge=0;Edge<4;++Edge)
    {
        bool AllOutside=true;
        for(const auto& V:R)if(Cross(P[Edge],P[(Edge+1)%4],V)>=0){AllOutside=false;break;}
        if(AllOutside)return false;
    }
    return true; // Touching tiles are blocked: conservative supercover.
}
bool Connected(const std::array<std::uint64_t,64>& Blocked,int OpenCount)
{
    if(OpenCount==0)return false;
    std::array<std::uint64_t,64> Seen{};
    std::array<std::uint16_t,4096> Queue{};
    int Head=0,Tail=0;
    for(int Y=0;Y<64&&Tail==0;++Y)for(int X=0;X<64;++X)
        if(!(Blocked[Y]&(std::uint64_t(1)<<X))){Queue[Tail++]=std::uint16_t(Y*64+X);Seen[Y]|=std::uint64_t(1)<<X;break;}
    const int DX[4]={0,1,0,-1},DY[4]={-1,0,1,0};
    while(Head<Tail)
    {
        const int Position=Queue[Head++],X=Position%64,Y=Position/64;
        for(int S=0;S<4;++S)
        {
            const int NX=X+DX[S],NY=Y+DY[S];
            if(NX<0||NX>=64||NY<0||NY>=64)continue;
            const auto Bit=std::uint64_t(1)<<NX;
            if((Blocked[NY]|Seen[NY])&Bit)continue;
            Seen[NY]|=Bit;Queue[Tail++]=std::uint16_t(NY*64+NX);
        }
    }
    return Tail==OpenCount;
}
struct CellCandidates
{
    std::array<std::uint32_t,MaximumFootprintsPerCell> Indices{};
    std::uint8_t Count=0;
};
bool Topology(const Domain& D,std::unordered_map<std::uint64_t,std::size_t>& Lookup,
    std::uint64_t& Fingerprint,std::string& Error)
{
    if(D.Cells.empty()||D.Cells.size()>MaximumCells||D.Faces.size()>4*MaximumCells)
        return Fail(Error,"Obstruction resident/face count exceeds explicit bound");
    Lookup.reserve(D.Cells.size());Fingerprint=HashInitial;
    for(std::size_t I=0;I<D.Cells.size();++I)
    {
        const auto& C=D.Cells[I];
        if(C.X<-MaximumGlobalCoord||C.X>MaximumGlobalCoord||C.Y<-MaximumGlobalCoord||C.Y>MaximumGlobalCoord
            ||!Lookup.emplace(Key(C.X,C.Y),I).second)return Fail(Error,"Duplicate/out-of-range global obstruction coordinate");
        Hash(Fingerprint,Key(C.X,C.Y));
    }
    std::unordered_map<std::int64_t,bool> IDs;IDs.reserve(D.Faces.size());
    std::vector<std::uint8_t> Seen(D.Cells.size(),0);
    const int DX[4]={0,1,0,-1},DY[4]={-1,0,1,0};
    for(const auto& F:D.Faces)
    {
        const int S=Side(F);
        if(S<0||F.Left<0||std::size_t(F.Left)>=D.Cells.size()||F.Right<-1
            ||(F.Right>=0&&std::size_t(F.Right)>=D.Cells.size())||!IDs.emplace(F.StableID,true).second)
            return Fail(Error,"Invalid obstruction face identity/index/normal");
        if(Seen[F.Left]&(1u<<S))return Fail(Error,"Duplicate obstruction half-face");
        Seen[F.Left]|=std::uint8_t(1u<<S);
        const auto& L=D.Cells[F.Left];
        if(F.Right>=0)
        {
            const auto& R=D.Cells[F.Right];
            if(R.X!=L.X+DX[S]||R.Y!=L.Y+DY[S]||(Seen[F.Right]&(1u<<Opposite(S))))
                return Fail(Error,"Obstruction shared face does not connect canonical neighbours");
            Seen[F.Right]|=std::uint8_t(1u<<Opposite(S));
        }
        else if(Lookup.count(Key(L.X+DX[S],L.Y+DY[S])))
            return Fail(Error,"Boundary face hides a resident neighbour");
        Hash(Fingerprint,std::uint64_t(F.StableID));Hash(Fingerprint,std::uint64_t(F.Left));
        Hash(Fingerprint,std::uint64_t(F.Right));Hash(Fingerprint,std::uint64_t(S));
        Hash(Fingerprint,std::uint64_t(F.Kind));
    }
    for(auto Bits:Seen)if(Bits!=15)return Fail(Error,"Obstruction domain is missing a canonical half-face");
    return true;
}
std::uint64_t GeometryHash(const ObstructionGeometry& G)
{
    std::uint64_t H=HashInitial;const auto& B=G.Binding;
    Hash(H,B.ModelVersion);Hash(H,B.RasterResolution);Hash(H,B.FormatFingerprint);
    Hash(H,std::uint64_t(B.GridOriginXMM));Hash(H,std::uint64_t(B.GridOriginYMM));
    Hash(H,std::uint64_t(B.CellWidthMM));Hash(H,std::uint64_t(B.CellHeightMM));
    Hash(H,B.SourceTerrainRevision);Hash(H,B.SourceBuildingRevision);Hash(H,G.TopologyFingerprint);
    for(const auto& P:B.Footprints)
    {
        Hash(H,P.BuildingID);Hash(H,Key(P.OwnerX,P.OwnerY));Hash(H,P.DefinitionType);Hash(H,P.SourceTransformBinding);
        for(const auto& C:P.Corners){Hash(H,std::uint64_t(C.X));Hash(H,std::uint64_t(C.Y));}
    }
    for(const auto& C:G.Cells){Hash(H,Key(C.X,C.Y));for(auto P:C.OpenPorts)Hash(H,P);
        Hash(H,std::uint64_t(std::llround(C.StoragePorosity*4096.)));}
    for(const auto& F:G.Faces){Hash(H,std::uint64_t(F.StableID));Hash(H,std::uint64_t(std::llround(F.OpenFraction*64.)));}
    return H;
}
}

bool IsSupportedObstructionFormat(std::uint32_t Version,std::uint32_t Raster,std::uint64_t Fingerprint)
{return (Version==0||Version==ObstructionModelVersion)&&Raster==64&&Fingerprint==ObstructionFormatFingerprint;}

bool BuildHydraulicObstructions(const Domain& D,const ObstructionInput& In,
    ObstructionGeometry& Out,std::string& Error,const ObstructionBuildLimits& Limits)
{
    using namespace ObstructionDetail;Error.clear();
    if(!IsSupportedObstructionFormat(In.ModelVersion,In.RasterResolution,In.FormatFingerprint))
        return Fail(Error,"Unsupported explicit obstruction format/model/raster fingerprint");
    if(In.Footprints.size()>MaximumObstructionFootprints||(!In.ModelVersion&&!In.Footprints.empty()))
        return Fail(Error,"Obstruction footprint count/model0 descriptor mismatch");
    if(In.CellWidthMM<1||In.CellWidthMM>MaximumCellMM||In.CellHeightMM<1||In.CellHeightMM>MaximumCellMM
        ||In.GridOriginXMM<-MaximumWorldMM||In.GridOriginXMM>MaximumWorldMM
        ||In.GridOriginYMM<-MaximumWorldMM||In.GridOriginYMM>MaximumWorldMM
        ||!Finite(D.CellWidthM)||!Finite(D.CellHeightM)
        ||std::abs(D.CellWidthM*1000.-double(In.CellWidthMM))>1e-7
        ||std::abs(D.CellHeightM*1000.-double(In.CellHeightMM))>1e-7)
        return Fail(Error,"Obstruction integer-mm grid binding differs from SI domain");
    // Includes peak hash-map node allowance, candidate lists, both staged outputs,
    // one 4KiB raster/BFS scratch, and copied descriptors. Not process RSS.
    std::uint64_t Owned=16384;
    if(!Add(Owned,Bytes(D.Cells.size(),sizeof(ObstructionCellOpening)+sizeof(CellCandidates)+96))
        ||!Add(Owned,Bytes(D.Faces.size(),sizeof(ObstructionFaceOpening)+64))
        ||!Add(Owned,Bytes(In.Footprints.size(),sizeof(ObstructionFootprint)+16))
        ||Owned>std::min(Limits.MaximumOwnedBytes,MaximumObstructionOwnedBytes))return Fail(Error,"Obstruction adapter exceeds bounded peak payload budget");
    std::unordered_map<std::uint64_t,std::size_t> Lookup;std::uint64_t TopologyID=0;
    if(!Topology(D,Lookup,TopologyID,Error))return false;
    ObstructionGeometry Candidate;Candidate.Binding=In;Candidate.TopologyFingerprint=TopologyID;
    std::sort(Candidate.Binding.Footprints.begin(),Candidate.Binding.Footprints.end(),
        [](const auto& A,const auto& B){return A.BuildingID<B.BuildingID;});
    Candidate.Cells.resize(D.Cells.size());Candidate.Faces.reserve(D.Faces.size());
    Candidate.Diagnostics.EstimatedOwnedBytes=Owned;Candidate.Diagnostics.Footprints=In.Footprints.size();
    std::vector<CellCandidates> Candidates(D.Cells.size());
    std::int32_t PreviousID=0;
    for(std::size_t I=0;I<Candidate.Binding.Footprints.size();++I)
    {
        const auto& P=Candidate.Binding.Footprints[I];
        if(P.BuildingID<=PreviousID||P.DefinitionType>23||P.SourceTransformBinding==0||!Lookup.count(Key(P.OwnerX,P.OwnerY)))
            return Fail(Error,"Duplicate/invalid obstruction building identity/source binding");
        PreviousID=P.BuildingID;
        std::int64_t MinX=P.Corners[0].X,MaxX=MinX,MinY=P.Corners[0].Y,MaxY=MinY;
        for(const auto& C:P.Corners)
        {
            if(C.X<-MaximumWorldMM||C.X>MaximumWorldMM||C.Y<-MaximumWorldMM||C.Y>MaximumWorldMM)
                return Fail(Error,"Footprint world-mm coordinate exceeds supported bound");
            MinX=std::min(MinX,C.X);MaxX=std::max(MaxX,C.X);MinY=std::min(MinY,C.Y);MaxY=std::max(MaxY,C.Y);
        }
        if(MaxX-MinX>std::int64_t(MaximumFootprintSpanCells)*In.CellWidthMM
            ||MaxY-MinY>std::int64_t(MaximumFootprintSpanCells)*In.CellHeightMM)
            return Fail(Error,"Footprint exceeds supported eight-cell envelope; no building omitted");
        for(int C=0;C<4;++C)if(Cross(P.Corners[C],P.Corners[(C+1)%4],P.Corners[(C+2)%4])<=0)
            return Fail(Error,"Footprint is not a strictly convex counter-clockwise quad");
        // Include touching neighbour at an exact boundary, matching closed SAT.
        const auto X0=FloorDiv(MinX-In.GridOriginXMM-1,In.CellWidthMM);
        const auto X1=FloorDiv(MaxX-In.GridOriginXMM,In.CellWidthMM);
        const auto Y0=FloorDiv(MinY-In.GridOriginYMM-1,In.CellHeightMM);
        const auto Y1=FloorDiv(MaxY-In.GridOriginYMM,In.CellHeightMM);
        if(X0<-MaximumGlobalCoord-1ll||X1>MaximumGlobalCoord+1ll
            ||Y0<-MaximumGlobalCoord-1ll||Y1>MaximumGlobalCoord+1ll)
            return Fail(Error,"Footprint coordinate lies outside supported global frame");
        bool IntersectsResident=false;
        for(auto Y=Y0;Y<=Y1;++Y)for(auto X=X0;X<=X1;++X)
        {
            ++Candidate.Diagnostics.CandidateVisits;
            const auto It=Lookup.find(Key(std::int32_t(X),std::int32_t(Y)));if(It==Lookup.end())continue;
            auto& List=Candidates[It->second];
            if(List.Count==MaximumFootprintsPerCell)return Fail(Error,"More than sixteen footprints touch one cell; no omission allowed");
            List.Indices[List.Count++]=std::uint32_t(I);IntersectsResident=true;
            Candidate.Diagnostics.MaximumCandidatesInCell=std::max(Candidate.Diagnostics.MaximumCandidatesInCell,std::size_t(List.Count));
        }
        if(!IntersectsResident)return Fail(Error,"Footprint does not intersect any resident cell");
    }
    for(std::size_t I=0;I<D.Cells.size();++I)
    {
        const auto& C=D.Cells[I];auto& Opening=Candidate.Cells[I];Opening.X=C.X;Opening.Y=C.Y;
        if(!Candidates[I].Count)continue;
        ++Candidate.Diagnostics.AffectedCells;std::array<std::uint64_t,64> Blocked{};
        const auto OriginX=In.GridOriginXMM+std::int64_t(C.X)*In.CellWidthMM;
        const auto OriginY=In.GridOriginYMM+std::int64_t(C.Y)*In.CellHeightMM;
        for(std::size_t K=0;K<Candidates[I].Count;++K)
        {
            const auto& P=Candidate.Binding.Footprints[Candidates[I].Indices[K]];
            std::array<ObstructionPointMM,4> Local{};
            for(int V=0;V<4;++V)Local[V]={(P.Corners[V].X-OriginX)*64,(P.Corners[V].Y-OriginY)*64};
            for(int Y=0;Y<64;++Y)for(int X=0;X<64;++X)
            {
                if(++Candidate.Diagnostics.RasterTileTests>std::min(Limits.MaximumRasterTileTests,MaximumObstructionRasterTileTests))
                    return Fail(Error,"Obstruction raster CPU work budget exceeded; no building omitted");
                const auto Bit=std::uint64_t(1)<<X;if(Blocked[Y]&Bit)continue;
                if(IntersectsTile(Local,X*In.CellWidthMM,Y*In.CellHeightMM,(X+1)*In.CellWidthMM,(Y+1)*In.CellHeightMM))Blocked[Y]|=Bit;
            }
        }
        int BlockedCount=0;for(auto Row:Blocked)BlockedCount+=Population(Row);
        const int OpenCount=4096-BlockedCount;Opening.StoragePorosity=OpenCount/4096.;
        if(Opening.StoragePorosity<MinimumSupportedStoragePorosity)
            return Fail(Error,"Unsupported fully/near-filled cell; porosity was not clamped");
        if(!Connected(Blocked,OpenCount))return Fail(Error,"Open cell contains disconnected water voids; refinement required");
        Opening.OpenPorts[0]=~Blocked[0];Opening.OpenPorts[2]=~Blocked[63];
        Opening.OpenPorts[1]=Opening.OpenPorts[3]=0;
        for(int Y=0;Y<64;++Y)
        {
            if(!(Blocked[Y]&(std::uint64_t(1)<<63)))Opening.OpenPorts[1]|=std::uint64_t(1)<<Y;
            if(!(Blocked[Y]&1))Opening.OpenPorts[3]|=std::uint64_t(1)<<Y;
        }
    }
    for(const auto& F:D.Faces)
    {
        const int S=Side(F);auto Port=Candidate.Cells[F.Left].OpenPorts[S];
        if(F.Right>=0)Port&=Candidate.Cells[F.Right].OpenPorts[Opposite(S)];
        Candidate.Faces.push_back({F.StableID,Population(Port)/64.});
    }
    Candidate.GeometryFingerprint=GeometryHash(Candidate);Out=std::move(Candidate);return true;
}

bool ValidateHydraulicObstructions(const Domain& D,const ObstructionGeometry& G,std::string& Error)
{
    using namespace ObstructionDetail;
    if(G.Cells.size()!=D.Cells.size()||G.Faces.size()!=D.Faces.size())return Fail(Error,"Saved obstruction shape differs from domain");
    ObstructionGeometry Expected;if(!BuildHydraulicObstructions(D,G.Binding,Expected,Error))return false;
    if(G.TopologyFingerprint!=Expected.TopologyFingerprint||G.GeometryFingerprint!=Expected.GeometryFingerprint)
        return Fail(Error,"Saved obstruction geometry/topology fingerprint mismatch");
    for(std::size_t I=0;I<G.Cells.size();++I)
    {
        const auto& A=G.Cells[I];const auto& B=Expected.Cells[I];
        if(A.X!=B.X||A.Y!=B.Y||A.StoragePorosity!=B.StoragePorosity||A.OpenPorts!=B.OpenPorts)
            return Fail(Error,"Saved exact cell porosity/ports differ from stored footprint geometry");
    }
    for(std::size_t I=0;I<G.Faces.size();++I)
        if(G.Faces[I].StableID!=Expected.Faces[I].StableID||G.Faces[I].OpenFraction!=Expected.Faces[I].OpenFraction)
            return Fail(Error,"Saved exact shared-face opening differs from stored footprint geometry");
    return true;
}

bool ReprojectHydraulicObstructions(const Domain& Old,const std::vector<CellState>& Cells,
    const ObstructionGeometry& New,std::vector<CellState>& Out,std::string& Error)
{
    using namespace ObstructionDetail;
    if(Cells.size()!=Old.Cells.size()||!ValidateHydraulicObstructions(Old,New,Error))return false;
    const double Area=Old.CellWidthM*Old.CellHeightM;
    if(!Finite(Area)||Area<=0.)return Fail(Error,"Invalid obstruction reprojection plan area");
    std::vector<CellState> Candidate;Candidate.reserve(Cells.size());
    double BeforeV=0.,AfterV=0.,BeforeX=0.,AfterX=0.,BeforeY=0.,AfterY=0.;
    for(std::size_t I=0;I<Cells.size();++I)
    {
        const auto& S=Cells[I];const auto& G=Old.Cells[I];const double P=New.Cells[I].StoragePorosity;
        if(!Finite(G.StoragePorosity)||G.StoragePorosity<=0.||G.StoragePorosity>1.
            ||!Finite(S.DepthM)||S.DepthM<0.||!Finite(S.DischargeXM2PerS)||!Finite(S.DischargeYM2PerS)
            ||(S.DepthM==0.&&(S.DischargeXM2PerS!=0.||S.DischargeYM2PerS!=0.)))
            return Fail(Error,"Invalid captured obstruction depth/discharge/old storage area");
        const double Ratio=G.StoragePorosity/P;
        CellState T{S.DepthM*Ratio,S.DischargeXM2PerS*Ratio,S.DischargeYM2PerS*Ratio};
        if(!Finite(T.DepthM)||!Finite(T.DischargeXM2PerS)||!Finite(T.DischargeYM2PerS))
            return Fail(Error,"Obstruction reprojection overflows; transaction not committed");
        BeforeV+=S.DepthM*G.StoragePorosity*Area;AfterV+=T.DepthM*P*Area;
        BeforeX+=S.DischargeXM2PerS*G.StoragePorosity*Area;AfterX+=T.DischargeXM2PerS*P*Area;
        BeforeY+=S.DischargeYM2PerS*G.StoragePorosity*Area;AfterY+=T.DischargeYM2PerS*P*Area;
        Candidate.push_back(T);
    }
    const auto Close=[](double A,double B){return Finite(A)&&Finite(B)&&std::abs(A-B)<=1e-10*std::max(1.,std::max(std::abs(A),std::abs(B)));};
    if(!Close(BeforeV,AfterV)||!Close(BeforeX,AfterX)||!Close(BeforeY,AfterY))
        return Fail(Error,"Obstruction reprojection does not conserve volume/integrated momentum");
    Out=std::move(Candidate);return true;
}
}
