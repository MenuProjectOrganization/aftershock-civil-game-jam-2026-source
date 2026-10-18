#include "AftershockMassMovementVisual.h"
#include "AftershockEnvironmentQuality.h"
#include "AftershockPresentationBudget.h"
#include "AftershockRuntimeMeshUV.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include <cmath>
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "TimerManager.h"

namespace
{
constexpr int32 MaxSupportCells=65536,MaxSkinCells=8192,MaxClastsAndCards=512;
constexpr int64 MaximumChordWork=16000000;
constexpr float UpdateSeconds=.08f;
static inline bool MassVisualFinite(double X){return FMath::IsFinite(X);}
static inline bool MassVisualFinite(const FVector& V){return MassVisualFinite(V.X)&&MassVisualFinite(V.Y)&&MassVisualFinite(V.Z);}
static inline bool MassVisualFinite(const FVector2D& V){return MassVisualFinite(V.X)&&MassVisualFinite(V.Y);}
bool Ordered(FIntPoint A,FIntPoint B){return A.Y==B.Y?A.X<B.X:A.Y<B.Y;}
struct FSkinVertex
{
    FVector Position=FVector::ZeroVector;
    FVector2f UV=FVector2f::ZeroVector;
    FVector2f DepthsCm=FVector2f::ZeroVector;
};
UStaticMesh* MakeSkin(AActor* Owner,const TArray<FSkinVertex>& Vertices,const TArray<int32>& Indices,
    UMaterialInterface* Material,double MaximumHeightCm)
{
    if(!Owner||!Material||Vertices.IsEmpty()||Indices.IsEmpty()||Vertices.Num()>MaxSkinCells*5||Indices.Num()>MaxSkinCells*12)return nullptr;
    FMeshDescription D;FStaticMeshAttributes Attributes(D);Attributes.Register();
    auto P=Attributes.GetVertexPositions();auto N=Attributes.GetVertexInstanceNormals();auto T=Attributes.GetVertexInstanceTangents();
    auto B=Attributes.GetVertexInstanceBinormalSigns();auto UV=Attributes.GetVertexInstanceUVs();UV.SetNumChannels(2);
    auto C=Attributes.GetVertexInstanceColors();auto Group=D.CreatePolygonGroup();Attributes.GetPolygonGroupMaterialSlotNames()[Group]=TEXT("MassMovementSkin");
    TArray<FVertexID> IDs;IDs.Reserve(Vertices.Num());
    for(const auto& V:Vertices){auto ID=D.CreateVertex();P[ID]=FVector3f(V.Position);IDs.Add(ID);}
    for(int32 I=0;I+2<Indices.Num();I+=3)
    {
        const FVector Normal=FVector::CrossProduct(Vertices[Indices[I+2]].Position-Vertices[Indices[I]].Position,
            Vertices[Indices[I+1]].Position-Vertices[Indices[I]].Position).GetSafeNormal();
        if(Normal.IsNearlyZero())return nullptr;
        FVector Tangent=FVector::VectorPlaneProject(FVector::ForwardVector,Normal).GetSafeNormal();
        if(Tangent.IsNearlyZero())Tangent=FVector::VectorPlaneProject(FVector::RightVector,Normal).GetSafeNormal();
        FVertexInstanceID VI[3];for(int32 J=0;J<3;++J)
        {
            const int32 At=Indices[I+J];VI[J]=D.CreateVertexInstance(IDs[At]);N[VI[J]]=FVector3f(Normal);T[VI[J]]=FVector3f(Tangent);
            B[VI[J]]=1.f;C[VI[J]]=FVector4f(1,1,1,1);UV.Set(VI[J],0,Vertices[At].UV);UV.Set(VI[J],1,Vertices[At].DepthsCm);
        }
        D.CreateTriangle(Group,MakeArrayView(VI));
    }
    auto* Mesh=NewObject<UStaticMesh>(Owner,NAME_None,RF_Transient);Mesh->GetStaticMaterials().Add(FStaticMaterial(Material,TEXT("MassMovementSkin")));
    UStaticMesh::FBuildMeshDescriptionsParams Parameters;Parameters.bFastBuild=true;Parameters.bBuildSimpleCollision=false;
    Parameters.bCommitMeshDescription=false;Parameters.bMarkPackageDirty=false;Parameters.bAllowCpuAccess=false;
    UStaticMesh::FBuildMeshDescriptionsLODParams LOD;LOD.bUseFullPrecisionUVs=true;Parameters.PerLODOverrides.Add(LOD);
    TArray<const FMeshDescription*> Descriptions{&D};if(!Mesh->BuildFromMeshDescriptions(Descriptions,Parameters))return nullptr;
    InitializeAftershockRuntimeMeshUV(*Mesh,Vertices,Indices,false,2);
    // Material WPO is actual replay depth. Extend native bounds by that exact
    // maximum rather than rely on a giant BoundsScale or risk frustum clipping.
    Mesh->SetPositiveBoundsExtension(FVector(0,0,MaximumHeightCm+2.));Mesh->CalculateExtendedBounds();return Mesh;
}
/** Conservative swept disc vs the same oriented rectangular building footprint.
 * The narrow renderer never invents a curved route around a missing trajectory. */
bool SweptObstacle(const FVector2D& A,const FVector2D& B,double Radius,const FAftershockMassObstacle& O)
{
    const double C=FMath::Cos(O.YawRadians),S=FMath::Sin(O.YawRadians);
    const auto Local=[&](FVector2D P){P-=O.CenterM;return FVector2D(C*P.X+S*P.Y,-S*P.X+C*P.Y);};
    const FVector2D L=Local(A),Delta=Local(B)-L;
    const FVector2D Extent=O.HalfExtentM+FVector2D(FMath::Max(0.,Radius-1.e-4));
    double Enter=0.,Leave=1.;for(int32 Axis=0;Axis<2;++Axis)
    {
        const double Start=Axis?L.Y:L.X,D=Axis?Delta.Y:Delta.X,E=Axis?Extent.Y:Extent.X;
        if(FMath::Abs(D)<1.e-12){if(FMath::Abs(Start)>=E)return false;continue;}
        double T0=(-E-Start)/D,T1=(E-Start)/D;if(T0>T1)Swap(T0,T1);
        Enter=FMath::Max(Enter,T0);Leave=FMath::Min(Leave,T1);if(Enter>=Leave-1.e-10)return false;
    }
    return Leave>1.e-8&&Enter<1.-1.e-8;
}
}

AAftershockMassMovementVisualActor::AAftershockMassMovementVisualActor()
{
    PrimaryActorTick.bCanEverTick=false;SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("MassMovementEventRoot")));
    FlowAndDepositSurface=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("AcceptedFlowAndPersistentDeposit"));
    Clasts=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("AcceptedMassClasts"));
    PowderCards=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("AcceptedFrontPowder"));
    for(auto* C:{static_cast<UStaticMeshComponent*>(Clasts.Get()),static_cast<UStaticMeshComponent*>(PowderCards.Get()),FlowAndDepositSurface.Get()})
    {C->SetupAttachment(GetRootComponent());C->SetMobility(EComponentMobility::Movable);C->SetCollisionEnabled(ECollisionEnabled::NoCollision);C->SetCanEverAffectNavigation(false);C->SetCastShadow(false);}
    SnowSurfaceAsset=TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/FX/Aftershock/MassMovement/M_AvalancheSnow.M_AvalancheSnow")));
    SoilSurfaceAsset=TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/FX/Aftershock/MassMovement/M_LandslideDebris.M_LandslideDebris")));
    SnowPowderAsset=TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/FX/Aftershock/MassMovement/M_AvalanchePowder.M_AvalanchePowder")));
    SoilDustAsset=TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/FX/Aftershock/MassMovement/M_LandslideDust.M_LandslideDust")));
    SnowClastAsset=TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/FX/Aftershock/MassMovement/SM_SnowClast.SM_SnowClast")));
    RockClastAsset=TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/FX/Aftershock/MassMovement/SM_RockClast.SM_RockClast")));
    PowderCardAsset=TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Plane.Plane")));
}

bool AAftershockMassMovementVisualActor::SampleFrozenSupport(const FVector2D& P,double& Z,FVector& Normal)const
{
    if(!MassVisualFinite(P))return false;
    const FVector2D At=(P-Event.GridOriginM)/Event.CellSizeM-FVector2D(.5);
    if(FMath::Abs(At.X)>1.e9||FMath::Abs(At.Y)>1.e9)return false;
    const FIntPoint C(FMath::FloorToInt(At.X),FMath::FloorToInt(At.Y));const double U=At.X-C.X,V=At.Y-C.Y;
    const FIntPoint Cells[4]={C,C+FIntPoint(1,0),C+FIntPoint(0,1),C+FIntPoint(1,1)};double H[4];
    for(int32 I=0;I<4;++I)
    {const int32* Index=SupportIndex.Find(Cells[I]);const double* Bed=FrozenBedM.Find(Cells[I]);if(!Index||!Bed||!Event.SupportCells[*Index].bLand)return false;H[I]=*Bed;}
    Z=(1.-U)*(1.-V)*H[0]+U*(1.-V)*H[1]+(1.-U)*V*H[2]+U*V*H[3];
    Normal=FVector(-((1.-V)*(H[1]-H[0])+V*(H[3]-H[2]))/Event.CellSizeM,
        -((1.-U)*(H[2]-H[0])+U*(H[3]-H[1]))/Event.CellSizeM,1.).GetSafeNormal();return MassVisualFinite(Z)&&MassVisualFinite(Normal);
}

bool AAftershockMassMovementVisualActor::ChordSupported(const FAftershockMassParcelFrame& A,const FAftershockMassParcelFrame& B,int64& Work)const
{
    const FVector2D From(A.PositionM.X,A.PositionM.Y),To(B.PositionM.X,B.PositionM.Y);
    const double Distance=FVector2D::Distance(From,To);const int32 Samples=FMath::Max(1,FMath::CeilToInt(Distance/(Event.CellSizeM*.2)));
    if(Samples>256||Work+Samples+Event.Obstacles.Num()>MaximumChordWork)return false;Work+=Samples+Event.Obstacles.Num();
    for(int32 I=0;I<=Samples;++I){double Z;FVector Normal;if(!SampleFrozenSupport(FMath::Lerp(From,To,static_cast<double>(I)/Samples),Z,Normal))return false;}
    const double Radius=FMath::Sqrt(FMath::Max(A.FootprintAreaM2,B.FootprintAreaM2)/UE_DOUBLE_PI);
    for(const auto& O:Event.Obstacles)if(SweptObstacle(From,To,Radius,O))return false;return true;
}

bool AAftershockMassMovementVisualActor::ValidateEvent(const FAftershockMassVisualEvent& V,FString& Why)
{
    const auto Reject=[&](const TCHAR* Reason){Why=Reason;return false;};const auto& R=V.Result;
    if(!V.EventID.IsValid()||!MassVisualFinite(V.StartedAtWorldSeconds)||!MassVisualFinite(V.CellSizeM)||V.CellSizeM<.5||V.CellSizeM>1000.
        ||!MassVisualFinite(V.GridOriginM)||!MassVisualFinite(V.WorldOffsetCm)||V.SupportCells.IsEmpty()||V.SupportCells.Num()>MaxSupportCells
        ||V.Obstacles.Num()>1024||R.Cells.Num()!=V.SupportCells.Num()||!R.bAccepted||!R.bComplete||!R.bMassConserved||!R.bEnergyClosed
        ||!R.bFrameExportComplete||R.Frames.IsEmpty()||R.Frames.Num()>64||!MassVisualFinite(R.SimulatedSeconds)||R.SimulatedSeconds<0.)
        return Reject(TEXT("Invalid or unaccepted bounded event/replay; presentation will not solve or fabricate it"));
    if(R.Material!=EAftershockMassMaterial::Snow&&R.Material!=EAftershockMassMaterial::SoilDebris)return Reject(TEXT("Unknown mass material"));
    FBox Bounds(ForceInit);for(int32 I=0;I<Event.SupportCells.Num();++I)
    {
        const auto& C=Event.SupportCells[I];if(SupportIndex.Contains(C.Coord))return Reject(TEXT("Duplicate support cell"));SupportIndex.Add(C.Coord,I);
    }
    for(const auto& C:R.Cells)
    {if(!SupportIndex.Contains(C.Coord)||FrozenBedM.Contains(C.Coord)||!MassVisualFinite(C.RunoutBedElevationM))return Reject(TEXT("Missing or duplicate exact runout bed"));
        FrozenBedM.Add(C.Coord,C.RunoutBedElevationM);Bounds+=ToWorld(FVector(Event.GridOriginM.X+(C.Coord.X+.5)*Event.CellSizeM,Event.GridOriginM.Y+(C.Coord.Y+.5)*Event.CellSizeM,C.RunoutBedElevationM));}
    if(!Bounds.IsValid||Bounds.GetSize().GetAbsMax()>1000000.)return Reject(TEXT("Display stage exceeds10km local bounds"));
    double Previous=-1.;int32 CellRecords=0,ParcelRecords=0;TSet<int32> IDs;
    for(const auto& F:R.Frames)
    {
        if(!MassVisualFinite(F.Seconds)||F.Seconds<Previous||F.Seconds<0.||F.Seconds>R.SimulatedSeconds+1.e-6||F.Parcels.Num()>512||F.Cells.Num()>MaxSupportCells)
        {
            return Reject(TEXT("Invalid sampled replay time or record count"));
        }
        Previous=F.Seconds;
        TMap<int32,int32> Indices;for(int32 I=0;I<F.Parcels.Num();++I)
        {
            const auto& P=F.Parcels[I];if(P.ParcelID<0||Indices.Contains(P.ParcelID)||!MassVisualFinite(P.PositionM)||!MassVisualFinite(P.VelocityMps)
                ||!MassVisualFinite(P.MassKg)||!MassVisualFinite(P.VolumeM3)||!MassVisualFinite(P.FootprintAreaM2)||P.MassKg<=0.||P.VolumeM3<=0.||P.FootprintAreaM2<=0.)return Reject(TEXT("Invalid parcel replay"));
            Indices.Add(P.ParcelID,I);IDs.Add(P.ParcelID);Bounds+=ToWorld(P.PositionM);
        }
        TSet<FIntPoint> Seen;for(const auto& C:F.Cells)
        {
            if(!SupportIndex.Contains(C.Coord)||Seen.Contains(C.Coord)||!MassVisualFinite(C.MovingVolumeM3)||!MassVisualFinite(C.DepositedVolumeM3)
                ||C.MovingVolumeM3<0.||C.DepositedVolumeM3<0.||!MassVisualFinite(C.FlowDepthM)||C.FlowDepthM<0.)
            {
                return Reject(TEXT("Invalid exact moving/deposit sample"));
            }
            Seen.Add(C.Coord);
        }
        ParcelIndices.Add(MoveTemp(Indices));CellRecords+=F.Cells.Num();ParcelRecords+=F.Parcels.Num();
    }
    if(IDs.Num()>512||CellRecords>FAftershockMassResult::MaximumFrameCellRecords||ParcelRecords>FAftershockMassResult::MaximumFrameParcelRecords
        ||CellRecords!=R.ExportedFrameCellRecords||ParcelRecords!=R.ExportedFrameParcelRecords||FMath::Abs(Previous-R.SimulatedSeconds)>1.e-6)
        return Reject(TEXT("Replay export budget or final-time proof mismatch"));
    ParcelIDs=IDs.Array();ParcelIDs.Sort();int64 Work=0;
    for(int32 F=0;F+1<R.Frames.Num();++F)for(const auto& A:R.Frames[F].Parcels)
        if(const int32* B=ParcelIndices[F+1].Find(A.ParcelID))if(!ChordSupported(A,R.Frames[F+1].Parcels[*B],Work))++Stats.UnsafeChordIntervals;
    if(Stats.UnsafeChordIntervals>0)return Reject(TEXT("Sparse replay chords cross unsupported land/solid footprint or exceed work bound; add trajectory samples, never invent detours"));
    Bounds=Bounds.ExpandBy(Event.CellSizeM*100.);Stats.EventWorldBounds=Bounds;return true;
}

bool AAftershockMassMovementVisualActor::BeginAcceptedEvent(const FAftershockMassVisualEvent& InEvent,FString& OutFailure)
{
    if(!IsInGameThread()||!GetWorld()){OutFailure=TEXT("Presentation requires a live game thread/world");return false;}
    if(Stats.bBound)
    {if(Event.EventID==InEvent.EventID)return true;OutFailure=TEXT("Existing event/deposit must be explicitly handed off before replacing it");return false;}
    ClearEvent();Event=InEvent;Stats.EventID=Event.EventID;Stats.TerrainRevision=Event.Result.SourceTerrainRevision;
    if(!ValidateEvent(Event,OutFailure)){UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_MASS_VISUAL_REJECT event=%s reason=%s unsafeChords=%d"),*Event.EventID.ToString(),*OutFailure,Stats.UnsafeChordIntervals);return false;}
    const bool Snow=Event.Result.Material==EAftershockMassMaterial::Snow;
    auto* Base=(Snow?SnowSurfaceAsset:SoilSurfaceAsset).LoadSynchronous();auto* Detail=(Snow?SnowClastAsset:RockClastAsset).LoadSynchronous();
    auto* PowderBase=(Snow?SnowPowderAsset:SoilDustAsset).LoadSynchronous();auto* Card=PowderCardAsset.LoadSynchronous();
    if(!Base||!Detail||!PowderBase||!Card){OutFailure=TEXT("Mass-movement native material/mesh assets missing; no unverified fallback");return false;}
    SetActorLocation(Stats.EventWorldBounds.GetCenter());SetActorRotation(FRotator::ZeroRotator);SetActorScale3D(FVector::OneVector);
    SurfaceMaterial=UMaterialInstanceDynamic::Create(Base,this);ClastMaterial=UMaterialInstanceDynamic::Create(Base,this);PowderMaterial=UMaterialInstanceDynamic::Create(PowderBase,this);
    // A clast uses the same lit shader with no height displacement/mask.
    ClastMaterial->SetScalarParameterValue(TEXT("MassClastMode"),1.f);
    FlowAndDepositSurface->SetMaterial(0,SurfaceMaterial);Clasts->SetStaticMesh(Detail);Clasts->SetMaterial(0,ClastMaterial);
    PowderCards->SetStaticMesh(Card);PowderCards->SetMaterial(0,PowderMaterial);
    ClastNativeRadiusCm=FMath::Max(1.f,Detail->GetBounds().BoxExtent.GetMax());
    Stats.bMaterialsReady=true;Stats.bBound=true;CurrentPair=INDEX_NONE;
    SetPlaybackWorldSeconds(GetWorld()->GetTimeSeconds());
    if(!Stats.bReplayGeometryValid||((Stats.MovingVolumeM3+Stats.DepositedVolumeM3)>1.e-9&&Stats.RenderedVolumeM3<=1.e-9))
    {OutFailure=TEXT("Accepted replay has no supported native surface coverage; widen its staged support or repair geometry");ClearEvent();return false;}
    GetWorldTimerManager().SetTimer(PresentationTimer,this,&AAftershockMassMovementVisualActor::UpdatePresentation,UpdateSeconds,true);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MASS_VISUAL_BOUND event=%s frames=%d cells=%d parcels=%d material=%s depositionFromKernel=1 noGameWrites=1"),
        *Event.EventID.ToString(),Event.Result.Frames.Num(),Event.Result.Cells.Num(),ParcelIDs.Num(),Snow?TEXT("snow"):TEXT("soil_debris"));return true;
}

bool AAftershockMassMovementVisualActor::BuildFramePair(int32 Pair)
{
    const auto& Frames=Event.Result.Frames;const auto& A=Frames[Pair];const auto& B=Frames[FMath::Min(Pair+1,Frames.Num()-1)];
    TMap<FIntPoint,double> DepthA,DepthB;TSet<FIntPoint> Union;double VolumeA=0.,VolumeB=0.;
    for(const auto& C:A.Cells){const double V=C.MovingVolumeM3+C.DepositedVolumeM3;DepthA.Add(C.Coord,V/(Event.CellSizeM*Event.CellSizeM));if(V>0.)Union.Add(C.Coord);VolumeA+=V;}
    for(const auto& C:B.Cells){const double V=C.MovingVolumeM3+C.DepositedVolumeM3;DepthB.Add(C.Coord,V/(Event.CellSizeM*Event.CellSizeM));if(V>0.)Union.Add(C.Coord);VolumeB+=V;}
    TArray<FIntPoint> Cells=Union.Array();Cells.Sort(Ordered);
    if(Cells.Num()>MaxSkinCells){Stats.bReplayGeometryValid=false;return false;}
    TArray<FSkinVertex> Vertices;TArray<int32> Indices;
    double SuppressedA=0.,SuppressedB=0.,AnalyticA=0.,AnalyticB=0.,MaxHeight=0.;int32 Unsupported=0;
    const FIntPoint CornerOffset[4]={FIntPoint(0,0),FIntPoint(1,0),FIntPoint(1,1),FIntPoint(0,1)};
    const auto CornerDepth=[](const TMap<FIntPoint,double>& Map,FIntPoint Corner)
    {double H=MAX_dbl;for(int32 Y=-1;Y<=0;++Y)for(int32 X=-1;X<=0;++X)H=FMath::Min(H,Map.FindRef(Corner+FIntPoint(X,Y)));return FMath::Max(0.,H);};
    const double Area=Event.CellSizeM*Event.CellSizeM;
    for(FIntPoint C:Cells)
    {
        FVector P[5];double H0[5],H1[5];bool Valid=true;
        for(int32 K=0;K<5;++K)
        {
            const FVector2D XY=Event.GridOriginM+(K==4?FVector2D(C.X+.5,C.Y+.5):FVector2D(C+CornerOffset[K]))*Event.CellSizeM;
            // Cosmetic5mm support translation prevents the retained deposit skin
            // fighting the canonical deposited terrain. Thickness/volume below
            // remain relative to this translated support; no stock is added.
            double Z=0.;FVector Normal;Valid&=SampleFrozenSupport(XY,Z,Normal);P[K]=ToWorld(FVector(XY.X,XY.Y,Z+.005))-GetActorLocation();
            if(K<4){H0[K]=CornerDepth(DepthA,C+CornerOffset[K]);H1[K]=CornerDepth(DepthB,C+CornerOffset[K]);}
        }
        // Do not paint skins through a solid building. Full-cell conservatism
        // suppresses (and reports) a small boundary volume, never relocates it.
        const FVector2D Center=Event.GridOriginM+FVector2D(C.X+.5,C.Y+.5)*Event.CellSizeM;
        for(const auto& O:Event.Obstacles)if(SweptObstacle(Center,Center,Event.CellSizeM*.707107,O)){Valid=false;break;}
        if(!Valid){SuppressedA+=DepthA.FindRef(C)*Area;SuppressedB+=DepthB.FindRef(C)*Area;++Unsupported;continue;}
        H0[4]=3.*DepthA.FindRef(C)-.5*(H0[0]+H0[1]+H0[2]+H0[3]);H1[4]=3.*DepthB.FindRef(C)-.5*(H1[0]+H1[1]+H1[2]+H1[3]);
        const int32 Base=Vertices.Num();for(int32 K=0;K<5;++K)
        {
            FSkinVertex V;V.Position=P[K];V.UV=FVector2f(static_cast<float>((P[K].X+GetActorLocation().X)*.005),static_cast<float>((P[K].Y+GetActorLocation().Y)*.005));
            V.DepthsCm=FVector2f(static_cast<float>(H0[K]*100.),static_cast<float>(H1[K]*100.));Vertices.Add(V);MaxHeight=FMath::Max3(MaxHeight,H0[K]*100.,H1[K]*100.);
        }
        for(int32 K=0;K<4;++K)Indices.Append({Base+4,Base+(K+1)%4,Base+K});
        AnalyticA+=Area*(H0[4]/3.+(H0[0]+H0[1]+H0[2]+H0[3])/6.);
        AnalyticB+=Area*(H1[4]/3.+(H1[0]+H1[1]+H1[2]+H1[3])/6.);
    }
    UStaticMesh* Mesh=Indices.IsEmpty()?nullptr:MakeSkin(this,Vertices,Indices,SurfaceMaterial,MaxHeight);
    FlowAndDepositSurface->SetStaticMesh(Mesh);++Stats.MeshBuilds;Stats.FramePair=Pair;
    Stats.SurfaceCells=Indices.Num()/12;Stats.SurfaceTriangles=Indices.Num()/3;Stats.UnsupportedSurfaceCells=Unsupported;
    PairSuppressedA={SuppressedA};PairSuppressedB={SuppressedB};PairVisibleA={VolumeA-SuppressedA};PairVisibleB={VolumeB-SuppressedB};
    // Integral is exact relative to the represented horizontal cell volumes,
    // apart from the explicitly reported sub5mm material clip strip.
    Stats.AnalyticSurfaceVolumeM3=AnalyticA;
    const bool VolumeOK=FMath::Abs(AnalyticA-(VolumeA-SuppressedA))<=FMath::Max(1.e-6,VolumeA*1.e-7)
        &&FMath::Abs(AnalyticB-(VolumeB-SuppressedB))<=FMath::Max(1.e-6,VolumeB*1.e-7);
    Stats.bReplayGeometryValid=VolumeOK&&(Indices.IsEmpty()||Mesh!=nullptr);
    if(!Stats.bReplayGeometryValid){UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_MASS_VISUAL_SKIN_REJECT event=%s volumeOK=%d"),*Event.EventID.ToString(),VolumeOK);FlowAndDepositSurface->SetVisibility(false);return false;}
    CurrentPair=Pair;return true;
}

void AAftershockMassMovementVisualActor::UpdateDetails(int32 Pair,double Alpha)
{
    const auto Quality=GetAftershockEnvironmentQuality(GetWorld());const auto Camera=GetAftershockPresentationBudget(GetWorld());const int32 Q=FMath::Clamp(Quality.Effects,0,4);
    Stats.EffectsQuality=Q;const int32 DetailCaps[]={64,128,224,352,448};const int32 PowderCaps[]={0,12,24,48,64};
    const int32 Count=Camera.bDrawDecorations?FMath::Min(ParcelIDs.Num(),DetailCaps[Q]):0;
    const int32 CardCap=Camera.bDrawDecorations?FMath::Min(PowderCaps[Q],MaxClastsAndCards-Count):0;
    if(Clasts->GetInstanceCount()!=Count)
    {Clasts->ClearInstances();ClastTransforms.Init(FTransform(FQuat::Identity,FVector::ZeroVector,FVector::ZeroVector),Count);for(const auto& T:ClastTransforms)Clasts->AddInstance(T);}
    if(PowderCards->GetInstanceCount()!=CardCap)
    {PowderCards->ClearInstances();PowderTransforms.Init(FTransform(FQuat::Identity,FVector::ZeroVector,FVector::ZeroVector),CardCap);for(const auto& T:PowderTransforms)PowderCards->AddInstance(T);}
    const int32 Next=FMath::Min(Pair+1,Event.Result.Frames.Num()-1);const auto& A=Event.Result.Frames[Pair];const auto& B=Event.Result.Frames[Next];
    FVector CameraWorld=GetActorLocation()+FVector(0,0,20000.);if(auto* PC=GetWorld()->GetFirstPlayerController())if(PC->PlayerCameraManager)CameraWorld=PC->PlayerCameraManager->GetCameraLocation();
    int32 Visible=0,Cards=0;const bool Snow=Event.Result.Material==EAftershockMassMaterial::Snow;
    for(int32 Slot=0;Slot<Count;++Slot)
    {
        auto& Transform=ClastTransforms[Slot];Transform.SetScale3D(FVector::ZeroVector);const int32 ID=ParcelIDs[Slot];
        const int32* IA=ParcelIndices[Pair].Find(ID);const int32* IB=ParcelIndices[Next].Find(ID);if(!IA&&!IB)continue;
        // No generated parcel trajectories: common IDs interpolate measured
        // endpoints; a terminal unmatched sample dissolves into its exact cell
        // ledger. It never gets an invented destination or second impulse.
        const auto& P=IA?A.Parcels[*IA]:B.Parcels[*IB];FVector Position=P.PositionM,Velocity=P.VelocityMps;
        double Depth=P.VolumeM3/P.FootprintAreaM2,Presence=IA?(IB?1.:1.-Alpha):Alpha;
        if(IA&&IB){const auto& E=B.Parcels[*IB];Position=FMath::Lerp(P.PositionM,E.PositionM,Alpha);Velocity=FMath::Lerp(P.VelocityMps,E.VelocityMps,Alpha);Depth=FMath::Lerp(Depth,E.VolumeM3/E.FootprintAreaM2,Alpha);}
        double Bed;FVector Normal;if(Presence<=.001||!SampleFrozenSupport(FVector2D(Position.X,Position.Y),Bed,Normal))continue;
        const double RadiusM=FMath::Min(FMath::Clamp(std::cbrt(P.VolumeM3)*.07,.12,Snow?.55:.7),FMath::Max(.02,Depth*.45));
        // Preserve the kernel's sampled centre/support convention; do not lift
        // buried parcels above accumulated bulk simply to claim visibility.
        Position.Z=Bed+Depth*.5;FVector Forward=FVector::VectorPlaneProject(Velocity,Normal).GetSafeNormal();if(Forward.IsNearlyZero())Forward=FVector::VectorPlaneProject(FVector::ForwardVector,Normal).GetSafeNormal();
        const FQuat Rotation=FQuat(Normal,ID*2.3999632297)*FRotationMatrix::MakeFromXZ(Forward,Normal).ToQuat();const float Scale=static_cast<float>(RadiusM*100./ClastNativeRadiusCm*Presence);
        Transform=FTransform(Rotation,ToWorld(Position)-GetActorLocation(),FVector(Scale*.85f,Scale,Scale*.7f));++Visible;
        if(Cards<CardCap&&Velocity.Size()>(Snow?3.:6.))
        {
            FVector Front=Position;Front.Z=Bed+Depth+RadiusM*(Snow?2.:1.2);const FVector World=ToWorld(Front);
            const FQuat Facing=FRotationMatrix::MakeFromZ((CameraWorld-World).GetSafeNormal()).ToQuat();
            const float SizeCm=static_cast<float>(FMath::Clamp(FMath::Sqrt(P.FootprintAreaM2)*100.,80.,Snow?450.:220.));
            PowderTransforms[Cards++]=FTransform(Facing,World-GetActorLocation(),FVector(SizeCm*.01f*Presence,SizeCm*.01f*Presence,1.f));
        }
    }
    for(int32 I=Cards;I<CardCap;++I)PowderTransforms[I].SetScale3D(FVector::ZeroVector);
    if(Count>0)Clasts->BatchUpdateInstancesTransforms(0,ClastTransforms,false,true,true);
    if(CardCap>0)PowderCards->BatchUpdateInstancesTransforms(0,PowderTransforms,false,true,true);
    const float FarCull[]={25000.f,35000.f,45000.f,55000.f,65000.f};Clasts->SetCullDistances(static_cast<int32>(FarCull[Q]*.8f),static_cast<int32>(FarCull[Q]));PowderCards->SetCullDistances(static_cast<int32>(FarCull[Q]*.6f),static_cast<int32>(FarCull[Q]*.85f));
    Clasts->SetCastShadow(Quality.Shadows>=2&&Camera.bDrawDecorations);Stats.VisibleClasts=Visible;Stats.VisiblePowderCards=Cards;
}

void AAftershockMassMovementVisualActor::SetPlaybackWorldSeconds(double WorldSeconds)
{
    if(!Stats.bBound||!MassVisualFinite(WorldSeconds))return;CurrentWorldSeconds=WorldSeconds;
    const double Age=FMath::Clamp(WorldSeconds-Event.StartedAtWorldSeconds,0.,Event.Result.SimulatedSeconds);Stats.PlaybackSeconds=Age;
    const auto& Frames=Event.Result.Frames;int32 Pair=0;
    while(Pair+1<Frames.Num()&&Frames[Pair+1].Seconds<=Age+1.e-9)++Pair;
    const int32 Next=FMath::Min(Pair+1,Frames.Num()-1);const double Span=Frames[Next].Seconds-Frames[Pair].Seconds;
    const double Alpha=Span>1.e-9?FMath::Clamp((Age-Frames[Pair].Seconds)/Span,0.,1.):0.;
    if(CurrentPair!=Pair&&!BuildFramePair(Pair)){Clasts->SetVisibility(false);PowderCards->SetVisibility(false);GetWorldTimerManager().ClearTimer(PresentationTimer);return;}SurfaceMaterial->SetScalarParameterValue(TEXT("MassFrameAlpha"),static_cast<float>(Alpha));
    double MovingA=0.,MovingB=0.,DepositA=0.,DepositB=0.;
    for(const auto& C:Frames[Pair].Cells){MovingA+=C.MovingVolumeM3;DepositA+=C.DepositedVolumeM3;}
    for(const auto& C:Frames[Next].Cells){MovingB+=C.MovingVolumeM3;DepositB+=C.DepositedVolumeM3;}
    Stats.MovingVolumeM3=FMath::Lerp(MovingA,MovingB,Alpha);Stats.DepositedVolumeM3=FMath::Lerp(DepositA,DepositB,Alpha);
    Stats.SuppressedVolumeM3=FMath::Lerp(PairSuppressedA[0],PairSuppressedB[0],Alpha);Stats.RenderedVolumeM3=FMath::Lerp(PairVisibleA[0],PairVisibleB[0],Alpha);
    Stats.AnalyticSurfaceVolumeM3=Stats.RenderedVolumeM3;UpdateDetails(Pair,Alpha);
    Stats.bComplete=Age>=Event.Result.SimulatedSeconds-1.e-8;Stats.bFinalDepositRetained=Stats.bComplete&&DepositA>0.&&FlowAndDepositSurface->GetStaticMesh()!=nullptr;
    if(Stats.bComplete){Clasts->SetVisibility(false);PowderCards->SetVisibility(false);}ApplyVisibility();
}

void AAftershockMassMovementVisualActor::ApplyVisibility()
{
    const auto Camera=GetAftershockPresentationBudget(GetWorld());const bool DrawSurface=Camera.Tier!=EAftershockPresentationTier::Globe;
    FlowAndDepositSurface->SetVisibility(Stats.bBound&&Stats.bReplayGeometryValid&&DrawSurface&&FlowAndDepositSurface->GetStaticMesh()!=nullptr);
    Clasts->SetVisibility(Stats.bBound&&!Stats.bComplete&&Camera.bDrawDecorations);PowderCards->SetVisibility(Stats.bBound&&!Stats.bComplete&&Camera.bDrawDecorations);
}
void AAftershockMassMovementVisualActor::UpdatePresentation()
{
    if(!Stats.bBound||!GetWorld())return;
    if(Stats.bComplete){ApplyVisibility();return;}
    SetPlaybackWorldSeconds(bExternallyClocked?CurrentWorldSeconds:GetWorld()->GetTimeSeconds());
    if(Stats.bComplete)GetWorldTimerManager().SetTimer(PresentationTimer,this,&AAftershockMassMovementVisualActor::UpdatePresentation,1.f,true);
}
void AAftershockMassMovementVisualActor::FinishMotionKeepDeposit()
{
    if(!Stats.bBound)return;SetPlaybackWorldSeconds(Event.StartedAtWorldSeconds+Event.Result.SimulatedSeconds);
    GetWorldTimerManager().SetTimer(PresentationTimer,this,&AAftershockMassMovementVisualActor::UpdatePresentation,1.f,true);
}
void AAftershockMassMovementVisualActor::ClearEvent()
{
    if(GetWorld())GetWorldTimerManager().ClearTimer(PresentationTimer);
    FlowAndDepositSurface->SetStaticMesh(nullptr);FlowAndDepositSurface->SetMaterial(0,nullptr);Clasts->ClearInstances();PowderCards->ClearInstances();
    Clasts->SetStaticMesh(nullptr);PowderCards->SetStaticMesh(nullptr);Clasts->SetMaterial(0,nullptr);PowderCards->SetMaterial(0,nullptr);
    SurfaceMaterial=nullptr;ClastMaterial=nullptr;PowderMaterial=nullptr;SupportIndex.Reset();FrozenBedM.Reset();ParcelIndices.Reset();ParcelIDs.Reset();
    ClastTransforms.Reset();PowderTransforms.Reset();PairSuppressedA.Reset();PairSuppressedB.Reset();PairVisibleA.Reset();PairVisibleB.Reset();
    Event=FAftershockMassVisualEvent{};Stats=FAftershockMassVisualStats{};CurrentPair=INDEX_NONE;CurrentWorldSeconds=0.;
}
void AAftershockMassMovementVisualActor::EndPlay(const EEndPlayReason::Type Reason)
{ClearEvent();Super::EndPlay(Reason);}
