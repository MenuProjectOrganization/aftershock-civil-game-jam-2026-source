#include "AftershockNativeFacadeTangentRepair.h"
#if WITH_EDITOR
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshResources.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "Serialization/MemoryWriter.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "Misc/Guid.h"
#include "Misc/DateTime.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/MetaData.h"
#include "UObject/SavePackage.h"

namespace AftershockNativeFacadeRepairPrivate
{
constexpr const TCHAR* RepairTag=TEXT("AftershockNativeFacadeExplicitTangents");
constexpr const TCHAR* RepairVersion=TEXT("metric_uv_seam_mikk_v1");
constexpr double TileCm=200.;
bool Finite(const FVector3f& V){return FMath::IsFinite(V.X)&&FMath::IsFinite(V.Y)&&FMath::IsFinite(V.Z);}
bool Frame(const FVector3f& N,const FVector3f& T,float S)
{
    if(!Finite(N)||!Finite(T)||!FMath::IsFinite(S)||N.SizeSquared()<=.01f||T.SizeSquared()<=.01f||FMath::Abs(S)<.5f)return false;
    return FVector3f::CrossProduct(N,T).SizeSquared()>.01f&&
        FMath::Abs(FVector3f::DotProduct(N,T))/FMath::Sqrt(N.SizeSquared()*T.SizeSquared())<=.05f;
}
bool Write(const TSharedRef<FJsonObject>& R,const FString& Path)
{
    FString Text;auto W=TJsonWriterFactory<>::Create(&Text);if(!FJsonSerializer::Serialize(R,W))return false;
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);const FString Temp=Path+TEXT(".tmp");
    return FFileHelper::SaveStringToFile(Text,*Temp,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)&&
        IFileManager::Get().Move(*Path,*Temp,true,false,false,true);
}
FString Digest(const TArray<uint8>& B){FSHAHash H;FSHA1::HashBuffer(B.GetData(),B.Num(),H.Hash);return H.ToString();}
double Det(const FVector2f& A,const FVector2f& B,const FVector2f& C)
{return double(B.X-A.X)*(C.Y-A.Y)-double(B.Y-A.Y)*(C.X-A.X);}

// Protect actual triangle corners, not VI IDs: UV-only seams may fork a corner
// while preserving the same vertex, triangle diagonal, normal and vertex colour.
// UV0 changes are permitted ONLY on explicitly identified invalid polygons.
TArray<uint8> Protected(const FMeshDescription& D,const TSet<FPolygonID>& UVAllowed)
{
    TArray<uint8> B;FMemoryWriter W(B,true);FStaticMeshConstAttributes A(D);
    const auto P=A.GetVertexPositions();const auto N=A.GetVertexInstanceNormals();
    const auto C=A.GetVertexInstanceColors();const auto UV=A.GetVertexInstanceUVs();
    const auto H=A.GetEdgeHardnesses();const auto Slots=A.GetPolygonGroupMaterialSlotNames();
    int32 Counts[]={D.Vertices().Num(),D.Edges().Num(),D.Triangles().Num(),D.Polygons().Num(),D.PolygonGroups().Num(),UV.GetNumChannels()};
    for(int32 X:Counts)W<<X;
    for(FVertexID ID:D.Vertices().GetElementIDs()){int32 I=ID.GetValue();FVector3f V=P[ID];W<<I<<V;}
    for(FEdgeID ID:D.Edges().GetElementIDs())
    {int32 I=ID.GetValue(),V0=D.GetEdgeVertex(ID,0).GetValue(),V1=D.GetEdgeVertex(ID,1).GetValue();bool Hard=H[ID];W<<I<<V0<<V1<<Hard;}
    for(FTriangleID ID:D.Triangles().GetElementIDs())
    {
        int32 I=ID.GetValue(),G=D.GetTrianglePolygonGroup(ID).GetValue(),Poly=D.GetTrianglePolygon(ID).GetValue();W<<I<<G<<Poly;
        for(FVertexInstanceID VI:D.GetTriangleVertexInstances(ID))
        {
            int32 Vertex=D.GetVertexInstanceVertex(VI).GetValue();FVector3f Normal=N[VI];FVector4f Color=C[VI];W<<Vertex<<Normal<<Color;
            for(int32 Channel=UVAllowed.Contains(FPolygonID(Poly))?1:0;Channel<UV.GetNumChannels();++Channel){FVector2f U=UV.Get(VI,Channel);W<<U;}
        }
    }
    for(FPolygonGroupID ID:D.PolygonGroups().GetElementIDs()){int32 I=ID.GetValue();FString S=Slots[ID].ToString();W<<I<<S;}
    return B;
}
bool FindInvalidUV(const FMeshDescription& D,TSet<FPolygonID>& Bad,FString& Error)
{
    Bad.Reset();const FStaticMeshConstAttributes A(D);const auto P=A.GetVertexPositions();const auto UV=A.GetVertexInstanceUVs();const auto N=A.GetVertexInstanceNormals();
    bool KnownAttributes=true;
    D.VertexInstanceAttributes().ForEach([&](FName Name,auto Ref)
    {
        static_cast<void>(Ref);
        KnownAttributes&=Name==MeshAttribute::VertexInstance::VertexIndex||Name==MeshAttribute::VertexInstance::TextureCoordinate||
            Name==MeshAttribute::VertexInstance::Normal||Name==MeshAttribute::VertexInstance::Tangent||
            Name==MeshAttribute::VertexInstance::BinormalSign||Name==MeshAttribute::VertexInstance::Color;
    });
    if(!KnownAttributes){Error=TEXT("Unknown source VI attributes cannot be safely copied by this bounded repair");return false;}
    if(UV.GetNumChannels()<1||UV.GetNumChannels()>8){Error=TEXT("Require1..8 bounded source UV channels");return false;}
    for(FTriangleID Triangle:D.Triangles().GetElementIDs())
    {
        const auto V=D.GetTriangleVertexInstances(Triangle);
        for(FVertexInstanceID VI:V)if(!Finite(P[D.GetVertexInstanceVertex(VI)])||!Finite(N[VI])||N[VI].SizeSquared()<=.01f)
        {Error=TEXT("Refuse to change invalid source position/normal");return false;}
        const FVector3f E1=P[D.GetVertexInstanceVertex(V[1])]-P[D.GetVertexInstanceVertex(V[0])];
        const FVector3f E2=P[D.GetVertexInstanceVertex(V[2])]-P[D.GetVertexInstanceVertex(V[0])];
        if(FVector3f::CrossProduct(E1,E2).SizeSquared()<=1.e-14f)
        {Error=TEXT("Geometrically degenerate source triangle; seam repair cannot invent geometry");return false;}
        for(int32 Channel=0;Channel<UV.GetNumChannels();++Channel)for(FVertexInstanceID VI:V)
        {const auto U=UV.Get(VI,Channel);if(!FMath::IsFinite(U.X)||!FMath::IsFinite(U.Y)){Error=TEXT("Nonfinite existing UV");return false;}}
        if(FMath::Abs(Det(UV.Get(V[0],0),UV.Get(V[1],0),UV.Get(V[2],0)))<=1.e-12)Bad.Add(D.GetTrianglePolygon(Triangle));
    }
    return true;
}
bool SplitProjectionSeams(FMeshDescription& D,const TSet<FPolygonID>& Bad,int32& Splits,FString& Error)
{
    Splits=0;TArray<FPolygonID> Ordered=Bad.Array();Ordered.Sort([](FPolygonID A,FPolygonID B){return A.GetValue()<B.GetValue();});
    for(FPolygonID Poly:Ordered)
    {
        FStaticMeshAttributes A(D);const auto P=A.GetVertexPositions();auto N=A.GetVertexInstanceNormals(),T=A.GetVertexInstanceTangents();
        auto S=A.GetVertexInstanceBinormalSigns();auto C=A.GetVertexInstanceColors();auto UV=A.GetVertexInstanceUVs();
        FVector3f Sum=FVector3f::ZeroVector;
        for(FTriangleID Triangle:D.GetPolygonTriangles(Poly))
        {
            const auto V=D.GetTriangleVertexInstances(Triangle);
            Sum+=FVector3f::CrossProduct(P[D.GetVertexInstanceVertex(V[1])]-P[D.GetVertexInstanceVertex(V[0])],
                P[D.GetVertexInstanceVertex(V[2])]-P[D.GetVertexInstanceVertex(V[0])]);
        }
        if(!Finite(Sum)||Sum.SizeSquared()<=1.e-14f){Error=TEXT("Invalid polygon geometric projection plane");return false;}
        const FVector3f Abs=Sum.GetAbs();const int32 Axis=Abs.X>=Abs.Y&&Abs.X>=Abs.Z?0:Abs.Y>=Abs.Z?1:2;
        const TArray<FVertexInstanceID> Old=D.GetPolygonVertexInstances(Poly);
        TMap<FVertexInstanceID,FVector2f> Desired;
        for(FVertexInstanceID VI:Old)
        {
            const auto Pos=P[D.GetVertexInstanceVertex(VI)];
            const FVector2f U=Axis==0?FVector2f(Pos.Y*(Sum.X<0?-1.f:1.f)/TileCm,Pos.Z/TileCm):
                Axis==1?FVector2f(Pos.X*(Sum.Y<0?1.f:-1.f)/TileCm,Pos.Z/TileCm):
                        FVector2f(Pos.X*(Sum.Z<0?-1.f:1.f)/TileCm,Pos.Y/TileCm);
            Desired.Add(VI,U);
        }
        // Do not re-triangulate or switch a quad's diagonal. If one polygon
        // genuinely cannot use a single nondegenerate plane, reject it.
        for(FTriangleID Triangle:D.GetPolygonTriangles(Poly))
        {
            const auto V=D.GetTriangleVertexInstances(Triangle);
            const auto* U0=Desired.Find(V[0]);const auto* U1=Desired.Find(V[1]);const auto* U2=Desired.Find(V[2]);
            if(!U0||!U1||!U2||FMath::Abs(Det(*U0,*U1,*U2))<=1.e-12)
            {Error=TEXT("Nonplanar/invalid polygon cannot be safely seam-repaired without changing geometry");return false;}
        }
        for(FVertexInstanceID From:Old)
        {
            // Capture values before any VI container mutation/reallocation.
            const FVertexID Vertex=D.GetVertexInstanceVertex(From);const FVector3f NN=N[From],TT=T[From];
            const FVector4f CC=C[From];const float SS=S[From];TArray<FVector2f> Channels;
            for(int32 I=0;I<UV.GetNumChannels();++I)Channels.Add(UV.Get(From,I));
            const FVertexInstanceID To=D.CreateVertexInstance(Vertex);
            N[To]=NN;T[To]=TT;C[To]=CC;S[To]=SS;
            for(int32 I=0;I<Channels.Num();++I)UV.Set(To,I,I==0?Desired.FindChecked(From):Channels[I]);
            // SetPolygonVertexInstance updates the existing triangles in place;
            // SetPolygonVertexInstances would delete/re-triangulate them.
            const auto Perimeter=D.GetPolygonVertexInstances(Poly);const int32 At=Perimeter.Find(From);
            if(At==INDEX_NONE){Error=TEXT("Source polygon perimeter changed unexpectedly");return false;}
            D.SetPolygonVertexInstance(Poly,At,To);
            if(D.GetVertexInstanceConnectedTriangleIDs(From).Num()==0)D.DeleteVertexInstance(From);
            if(++Splits>20000){Error=TEXT("Bounded20k seam-corner repair budget exceeded");return false;}
        }
    }
    return true;
}
bool Derivative(const FMeshDescription& D,FVertexInstanceID VI,FVector3f& Out,float& Sign)
{
    const FStaticMeshConstAttributes A(D);const auto P=A.GetVertexPositions();const auto UV=A.GetVertexInstanceUVs();
    const FVector3f N=A.GetVertexInstanceNormals()[VI].GetSafeNormal();float Best=-1.f;
    TArray<FTriangleID> Triangles;for(auto ID:D.GetVertexInstanceConnectedTriangleIDs(VI))Triangles.Add(ID);
    Triangles.Sort([](FTriangleID A,FTriangleID B){return A.GetValue()<B.GetValue();});
    for(FTriangleID ID:Triangles)
    {
        const auto V=D.GetTriangleVertexInstances(ID);
        const FVector3f E1=P[D.GetVertexInstanceVertex(V[1])]-P[D.GetVertexInstanceVertex(V[0])],E2=P[D.GetVertexInstanceVertex(V[2])]-P[D.GetVertexInstanceVertex(V[0])];
        const FVector2f U1=UV.Get(V[1],0)-UV.Get(V[0],0),U2=UV.Get(V[2],0)-UV.Get(V[0],0);
        const double DUV=double(U1.X)*U2.Y-double(U1.Y)*U2.X;if(FMath::Abs(DUV)<=1.e-12)continue;
        const FVector3f Raw=(E1*U2.Y-E2*U1.Y)/float(DUV),B=(E2*U1.X-E1*U2.X)/float(DUV);
        FVector3f Projected=Raw-N*FVector3f::DotProduct(Raw,N);
        if(!Finite(Raw)||!Finite(B)||!Projected.Normalize())continue;
        const float H=FVector3f::DotProduct(FVector3f::CrossProduct(N,Projected),B),Area=FVector3f::CrossProduct(E1,E2).SizeSquared();
        if(!FMath::IsFinite(H)||FMath::Abs(H)<=1.e-8f||Area<=Best)continue;
        Out=Projected;Sign=H<0.f?-1.f:1.f;Best=Area;
    }
    return Best>=0.f;
}
bool SourceFrames(const FMeshDescription& D)
{
    FStaticMeshConstAttributes A(D);const auto N=A.GetVertexInstanceNormals(),T=A.GetVertexInstanceTangents();const auto S=A.GetVertexInstanceBinormalSigns();
    for(auto Triangle:D.Triangles().GetElementIDs())for(auto VI:D.GetTriangleVertexInstances(Triangle))if(!Frame(N[VI],T[VI],S[VI]))return false;
    return true;
}
bool RenderFrames(UStaticMesh* Mesh,TArray<int32>& Triangles,int32& Invalid)
{
    Invalid=0;Triangles.Reset();const auto* R=Mesh->GetRenderData();if(!R||R->LODResources.Num()!=3)return false;
    int32 Glass=INDEX_NONE;const auto& Slots=Mesh->GetStaticMaterials();if(Slots.Num()!=3)return false;
    for(int32 I=0;I<Slots.Num();++I)if(Slots[I].MaterialSlotName==TEXT("Glass"))Glass=I;
    if(Glass==INDEX_NONE)return false;
    const int32 Caps[]={50000,18000,6000};
    for(int32 L=0;L<3;++L)
    {
        const auto& Data=R->LODResources[L];Triangles.Add(Data.GetNumTriangles());
        if(Triangles[L]<=0||Triangles[L]>Caps[L])return false;
        const auto& V=Data.VertexBuffers.StaticMeshVertexBuffer;
        if(V.GetNumVertices()==0||V.GetNumVertices()>200000||!V.GetTangentData())return false;
        bool HasGlass=false;
        for(const auto& Section:Data.Sections)HasGlass|=Section.MaterialIndex==Glass&&Section.NumTriangles>0;
        if(!HasGlass)return false;
        for(uint32 I=0;I<V.GetNumVertices();++I)
        {
            const FVector4f X4=V.VertexTangentX(I),Z4=V.VertexTangentZ(I);
            const FVector3f X(X4.X,X4.Y,X4.Z),Z(Z4.X,Z4.Y,Z4.Z),Y=V.VertexTangentY(I);
            if(!Frame(Z,X,Z4.W)||!Finite(Y)||Y.SizeSquared()<=.01f){++Invalid;continue;}
            const float H=FVector3f::DotProduct(FVector3f::CrossProduct(Z,X),Y)/FMath::Sqrt(X.SizeSquared()*Y.SizeSquared()*Z.SizeSquared());
            if(!FMath::IsFinite(H)||H*Z4.W<.9f)++Invalid;
        }
    }
    return Invalid==0;
}
struct FPlan
{
    UStaticMesh* Mesh=nullptr;FMeshDescription Original,Working;TArray<FMeshBuildSettings> Builds;TArray<uint8> ProtectedBytes;
    TSet<FPolygonID> Bad;TArray<int32> Triangles;TSharedPtr<FJsonObject> Row=MakeShared<FJsonObject>();
    FBox OriginalBounds=FBox(ForceInit);int32 Splits=0,Residuals=0;bool Installed=false;
};
}

bool RepairAftershockNativeFacadeTangents(const FString& Path)
{
    using namespace AftershockNativeFacadeRepairPrivate;
    auto Report=MakeShared<FJsonObject>();Report->SetBoolField(TEXT("valid"),false);Report->SetBoolField(TEXT("in_progress"),true);
    Report->SetStringField(TEXT("run_id"),FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
    Report->SetStringField(TEXT("started_utc"),FDateTime::UtcNow().ToIso8601());Report->SetStringField(TEXT("policy"),RepairVersion);
    Report->SetStringField(TEXT("scope"),TEXT("Exact16 owned native facade derivatives only; UV0 seam split on invalid polygons, imported normals/positions/colours/other UVs/triangle diagonals/materials protected. Fresh48LOD process audit still required."));
    if(!Write(Report,Path))return false;
    FString Error;bool Valid=true;TArray<TUniquePtr<FPlan>> Plans;TArray<TSharedPtr<FJsonValue>> Rows;int64 Bytes=0;
    if(!FParse::Param(FCommandLine::Get(),TEXT("NullRHI"))||FParse::Param(FCommandLine::Get(),TEXT("AllowCommandletRendering")))
    {Error=TEXT("Repair requires NullRHI without AllowCommandletRendering");Valid=false;}
    for(int32 F=1;Valid&&F<=4;++F)for(int32 B=1;Valid&&B<=4;++B)
    {
        auto P=MakeUnique<FPlan>();const FString Stem=FString::Printf(TEXT("SM_CS_Facade_B%d_F%02d"),B,F);
        const FString Package=TEXT("/Game/Aftershock/CitySample/Architecture/Baked/")+Stem;
        P->Mesh=LoadObject<UStaticMesh>(nullptr,*(Package+TEXT(".")+Stem));
        if(!P->Mesh||P->Mesh->GetOutermost()->GetName()!=Package||P->Mesh->GetNumSourceModels()!=3||
            FString(P->Mesh->GetOutermost()->GetMetaData().GetValue(P->Mesh,TEXT("AftershockNativeFacadeVersion")))!=TEXT("1")||
            !P->Mesh->CloneMeshDescription(0,P->Original)||P->Original.Triangles().Num()>50000)
        {Error=TEXT("Require exact owned Version1 native facade sourceLOD0: ")+Stem;Valid=false;break;}
        // This producer stores only sourceLOD0; LOD1/2 are classic reductions.
        if(P->Mesh->IsMeshDescriptionValid(1)||P->Mesh->IsMeshDescriptionValid(2))
        {Error=TEXT("Unexpected explicit sourceLOD1/2; preserve these assets for separate review");Valid=false;break;}
        P->OriginalBounds=P->Mesh->GetBoundingBox();
        for(int32 L=0;L<3;++L)P->Builds.Add(P->Mesh->GetSourceModel(L).BuildSettings);
        P->Working=P->Original;
        if(!FindInvalidUV(P->Working,P->Bad,Error)){Valid=false;break;}
        P->ProtectedBytes=Protected(P->Original,P->Bad);Bytes+=P->ProtectedBytes.Num();
        if(Bytes>192ll*1024*1024){Error=TEXT("Bounded192MiB protected snapshot budget exceeded");Valid=false;break;}
        if(!SplitProjectionSeams(P->Working,P->Bad,P->Splits,Error)){Valid=false;break;}
        TSet<FPolygonID> Remaining;if(!FindInvalidUV(P->Working,Remaining,Error)||!Remaining.IsEmpty()){Error=TEXT("UV repair left invalid triangles ")+Error;Valid=false;break;}
        FStaticMeshOperations::ComputeTriangleTangentsAndNormals(P->Working,0.f,*Stem);
        FStaticMeshOperations::ComputeTangentsAndNormals(P->Working,EComputeNTBsFlags::Tangents|EComputeNTBsFlags::UseMikkTSpace);
        FStaticMeshAttributes A(P->Working);auto N=A.GetVertexInstanceNormals(),T=A.GetVertexInstanceTangents();auto S=A.GetVertexInstanceBinormalSigns();
        TSet<FVertexInstanceID> Used;for(auto ID:P->Working.Triangles().GetElementIDs())for(auto VI:P->Working.GetTriangleVertexInstances(ID))Used.Add(VI);
        TArray<FVertexInstanceID> Ordered=Used.Array();Ordered.Sort([](FVertexInstanceID L,FVertexInstanceID R){return L.GetValue()<R.GetValue();});
        for(auto VI:Ordered)
        {
            if(Frame(N[VI],T[VI],S[VI]))continue;FVector3f Tangent;float Sign=1.f;
            if(++P->Residuals>4096||!Derivative(P->Working,VI,Tangent,Sign)){Error=TEXT("No finite incident-UV residual frame or residual4096 cap exceeded");Valid=false;break;}
            T[VI]=Tangent;S[VI]=Sign;
        }
        if(!Valid)break;
        if(!SourceFrames(P->Working)||Protected(P->Working,P->Bad)!=P->ProtectedBytes)
        {Error=TEXT("Protected source corner invariants/frame validation failed ")+Stem;Valid=false;break;}
        P->Row->SetStringField(TEXT("asset"),Package);P->Row->SetNumberField(TEXT("invalid_uv_polygons"),P->Bad.Num());
        P->Row->SetNumberField(TEXT("new_projection_seam_corners"),P->Splits);P->Row->SetNumberField(TEXT("mikk_residual_frames_repaired"),P->Residuals);
        P->Row->SetStringField(TEXT("protected_corner_sha1"),Digest(P->ProtectedBytes));P->Row->SetBoolField(TEXT("protected_geometry_normal_color_other_uv_materials"),true);
        Rows.Add(MakeShared<FJsonValueObject>(P->Row));Plans.Add(MoveTemp(P));
    }
    Report->SetNumberField(TEXT("preflight_meshes"),Plans.Num());Report->SetArrayField(TEXT("results"),Rows);Write(Report,Path);
    if(Valid&&Plans.Num()==16)for(auto& P:Plans)
    {
        auto* M=P->Mesh;M->Modify();M->CreateMeshDescription(0,P->Working);M->CommitMeshDescription(0);P->Installed=true;
        for(int32 L=0;L<3;++L){auto& S=M->GetSourceModel(L).BuildSettings;S.bRecomputeNormals=false;S.bRecomputeTangents=false;S.bUseMikkTSpace=true;}
        TArray<FText> BuildErrors;M->Build(true,&BuildErrors);FStaticMeshCompilingManager::Get().FinishAllCompilation();
        FMeshDescription Back;int32 Invalid=0;const FBox Box=M->GetBoundingBox();
        const bool Source=M->CloneMeshDescription(0,Back)&&Protected(Back,P->Bad)==P->ProtectedBytes&&SourceFrames(Back);
        const bool Frames=RenderFrames(M,P->Triangles,Invalid);
        const bool Shape=Box.Min.Equals(P->OriginalBounds.Min,.01)&&Box.Max.Equals(P->OriginalBounds.Max,.01)&&M->GetNumTriangles(0)==P->Original.Triangles().Num();
        P->Row->SetBoolField(TEXT("rebuilt_source_preserved"),Source);P->Row->SetBoolField(TEXT("rebuilt48_policy_frames_valid"),Frames);
        P->Row->SetNumberField(TEXT("rebuilt_invalid_frames"),Invalid);P->Row->SetBoolField(TEXT("bounds_lod0_triangle_count_preserved"),Shape);
        TArray<TSharedPtr<FJsonValue>> Ts;for(int32 N:P->Triangles)Ts.Add(MakeShared<FJsonValueNumber>(N));P->Row->SetArrayField(TEXT("triangles_per_lod"),Ts);
        if(!Source||!Frames||!Shape||!BuildErrors.IsEmpty()){Error=TEXT("Rebuilt native source/frame/bounds gate failed ")+M->GetName();Valid=false;break;}
    }
    else Valid=false;
    if(!Valid)
    {
        // No package saved on preflight/build failure. Restore in-memory source
        // so another operation cannot accidentally persist an unaccepted plan.
        for(auto& P:Plans)if(P->Installed)
        {P->Mesh->CreateMeshDescription(0,P->Original);P->Mesh->CommitMeshDescription(0);for(int32 L=0;L<3;++L)P->Mesh->GetSourceModel(L).BuildSettings=P->Builds[L];}
    }
    else for(auto& P:Plans)
    {
        UPackage* Package=P->Mesh->GetOutermost();Package->GetMetaData().SetValue(P->Mesh,RepairTag,RepairVersion);Package->MarkPackageDirty();
        FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;Args.SaveFlags=SAVE_NoError;
        const FString File=FPackageName::LongPackageNameToFilename(Package->GetName(),FPackageName::GetAssetPackageExtension());
        if(!UPackage::SavePackage(Package,P->Mesh,*File,Args)){Error=TEXT("Native owned derivative package save failed ")+File;Valid=false;break;}
        TArray<uint8> FileBytes;if(!FFileHelper::LoadFileToArray(FileBytes,*File)){Error=TEXT("Cannot hash newly saved native derivative");Valid=false;break;}
        P->Row->SetBoolField(TEXT("saved"),true);P->Row->SetStringField(TEXT("saved_package_sha1"),Digest(FileBytes));
        UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_NATIVE_FACADE_REPAIR asset=%s invalidUVpolygons=%d splitCorners=%d residualFrames=%d all3LODsValid=1 saved=1"),*Package->GetName(),P->Bad.Num(),P->Splits,P->Residuals);
    }
    Report->SetBoolField(TEXT("valid"),Valid);Report->SetBoolField(TEXT("in_progress"),false);Report->SetStringField(TEXT("error"),Error);
    Report->SetNumberField(TEXT("expected_meshes"),16);Report->SetNumberField(TEXT("expected_render_lods"),48);
    Report->SetBoolField(TEXT("fresh_process_48lod_audit_still_required"),true);Report->SetStringField(TEXT("completed_utc"),FDateTime::UtcNow().ToIso8601());
    const bool Saved=Write(Report,Path);if(!Valid)UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_NATIVE_FACADE_REPAIR_FAILED %s"),*Error);
    return Valid&&Saved;
}
#else
bool RepairAftershockNativeFacadeTangents(const FString&){return false;}
#endif
