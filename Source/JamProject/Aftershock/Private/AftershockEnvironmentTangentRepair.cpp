#include "AftershockEnvironmentTangentRepair.h"

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
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/MetaData.h"
#include "UObject/SavePackage.h"

namespace
{
constexpr const TCHAR* ExplicitTag=TEXT("AftershockExplicitTangentsLOD0");
constexpr const TCHAR* ExplicitVersion=TEXT("mikk_uv_derivative_orthogonal_v1");

bool Finite(const FVector3f& V)
{return FMath::IsFinite(V.X)&&FMath::IsFinite(V.Y)&&FMath::IsFinite(V.Z);}

bool FrameValid(const FVector3f& N,const FVector3f& T,float Sign)
{
    return Finite(N)&&Finite(T)&&FMath::IsFinite(Sign)&&N.SizeSquared()>.01f&&
        T.SizeSquared()>.01f&&FVector3f::CrossProduct(N,T).SizeSquared()>.01f&&FMath::Abs(Sign)>.5f;
}

// Exact protected source values; deliberately excludes tangent and binormal sign.
TArray<uint8> ProtectedSource(const FMeshDescription& D)
{
    TArray<uint8> Bytes; FMemoryWriter W(Bytes,true);
    FStaticMeshConstAttributes A(D);
    const auto Positions=A.GetVertexPositions();const auto Normals=A.GetVertexInstanceNormals();
    const auto Colors=A.GetVertexInstanceColors();const auto UV=A.GetVertexInstanceUVs();
    const auto Hardness=A.GetEdgeHardnesses();const auto Slots=A.GetPolygonGroupMaterialSlotNames();
    int32 Counts[]={D.Vertices().Num(),D.VertexInstances().Num(),D.Edges().Num(),D.Triangles().Num(),D.Polygons().Num(),D.PolygonGroups().Num(),UV.GetNumChannels()};
    for(int32 C:Counts)W<<C;
    for(const FVertexID ID:D.Vertices().GetElementIDs())
    {int32 I=ID.GetValue();FVector3f P=Positions[ID];W<<I<<P;}
    for(const FVertexInstanceID ID:D.VertexInstances().GetElementIDs())
    {
        int32 I=ID.GetValue(),V=D.GetVertexInstanceVertex(ID).GetValue();
        FVector3f N=Normals[ID];FVector4f C=Colors[ID];W<<I<<V<<N<<C;
        for(int32 Channel=0;Channel<UV.GetNumChannels();++Channel){FVector2f U=UV.Get(ID,Channel);W<<U;}
    }
    for(const FEdgeID ID:D.Edges().GetElementIDs())
    {int32 I=ID.GetValue(),V0=D.GetEdgeVertex(ID,0).GetValue(),V1=D.GetEdgeVertex(ID,1).GetValue();bool H=Hardness[ID];W<<I<<V0<<V1<<H;}
    for(const FTriangleID ID:D.Triangles().GetElementIDs())
    {
        int32 I=ID.GetValue(),P=D.GetTrianglePolygon(ID).GetValue(),G=D.GetTrianglePolygonGroup(ID).GetValue();W<<I<<P<<G;
        for(const FVertexInstanceID VI:D.GetTriangleVertexInstances(ID)){int32 V=VI.GetValue();W<<V;}
    }
    for(const FPolygonGroupID ID:D.PolygonGroups().GetElementIDs())
    {int32 I=ID.GetValue();FString S=Slots[ID].ToString();W<<I<<S;}
    return Bytes;
}

FString Digest(const TArray<uint8>& Bytes)
{FSHAHash H;FSHA1::HashBuffer(Bytes.GetData(),Bytes.Num(),H.Hash);return H.ToString();}

bool SourceFramesValid(const FMeshDescription& D)
{
    const FStaticMeshConstAttributes A(D);const auto N=A.GetVertexInstanceNormals();
    const auto T=A.GetVertexInstanceTangents();const auto S=A.GetVertexInstanceBinormalSigns();
    for(const FVertexInstanceID ID:D.VertexInstances().GetElementIDs())if(!FrameValid(N[ID],T[ID],S[ID]))return false;
    return true;
}

bool DerivativeFrame(const FMeshDescription& D,FVertexInstanceID Corner,FVector3f& OutT,float& OutSign)
{
    const FStaticMeshConstAttributes A(D);const auto P=A.GetVertexPositions();const auto UV=A.GetVertexInstanceUVs();
    const FVector3f N=A.GetVertexInstanceNormals()[Corner].GetSafeNormal();
    TArray<FTriangleID> Triangles;
    for(const FTriangleID ID:D.GetVertexInstanceConnectedTriangleIDs(Corner))Triangles.Add(ID);
    Triangles.Sort([](const FTriangleID& L,const FTriangleID& R){return L.GetValue()<R.GetValue();});
    float BestArea=-1.f;
    for(const FTriangleID Triangle:Triangles)
    {
        const auto V=D.GetTriangleVertexInstances(Triangle);
        if(V.Num()!=3)continue;
        const FVector3f E1=P[D.GetVertexInstanceVertex(V[1])]-P[D.GetVertexInstanceVertex(V[0])];
        const FVector3f E2=P[D.GetVertexInstanceVertex(V[2])]-P[D.GetVertexInstanceVertex(V[0])];
        const FVector2f U1=UV.Get(V[1],0)-UV.Get(V[0],0),U2=UV.Get(V[2],0)-UV.Get(V[0],0);
        const double Det=double(U1.X)*U2.Y-double(U1.Y)*U2.X;
        if(!FMath::IsFinite(Det)||FMath::Abs(Det)<=1.e-12)continue;
        const FVector3f T=(E1*U2.Y-E2*U1.Y)/float(Det);
        const FVector3f B=(E2*U1.X-E1*U2.X)/float(Det);
        FVector3f Projected=T-N*FVector3f::DotProduct(T,N);
        if(!Finite(T)||!Finite(B)||!Projected.Normalize())continue;
        const float Handedness=FVector3f::DotProduct(FVector3f::CrossProduct(N,Projected),B);
        const float Area=FVector3f::CrossProduct(E1,E2).SizeSquared();
        if(!FMath::IsFinite(Handedness)||FMath::Abs(Handedness)<=1.e-8f||Area<=BestArea)continue;
        OutT=Projected;OutSign=Handedness<0.f?-1.f:1.f;BestArea=Area;
    }
    // No arbitrary world-axis substitute: invalid source must remain a failure.
    return BestArea>=0.f;
}

bool RenderFramesValid(UStaticMesh* Mesh,int32 LODIndex,int32& Invalid)
{
    Invalid=0;const auto* Render=Mesh->GetRenderData();
    if(!Render||!Render->LODResources.IsValidIndex(LODIndex))return false;
    const auto& V=Render->LODResources[LODIndex].VertexBuffers.StaticMeshVertexBuffer;
    if(V.GetNumVertices()==0||V.GetTangentData()==nullptr)return false;
    for(uint32 I=0;I<V.GetNumVertices();++I)
    {
        const FVector4f X4=V.VertexTangentX(I),Z4=V.VertexTangentZ(I);
        const FVector3f X(X4.X,X4.Y,X4.Z),Z(Z4.X,Z4.Y,Z4.Z),Y=V.VertexTangentY(I);
        if(!FrameValid(Z,X,1.f)||!Finite(Y)||Y.SizeSquared()<=.01f)++Invalid;
    }
    return Invalid==0;
}

struct FPlan
{
    UStaticMesh* Mesh=nullptr;FMeshDescription Original,Working;
    FMeshBuildSettings OriginalBuild;TArray<uint8> Protected;
    TSharedPtr<FJsonObject> Row=MakeShared<FJsonObject>();
    int32 Repaired=0;bool bInstalled=false;
};
}

bool RepairAftershockBroadleafExplicitTangents(const FString& AbsoluteReportPath)
{
    if(!FParse::Param(FCommandLine::Get(),TEXT("NullRHI"))||FParse::Param(FCommandLine::Get(),TEXT("AllowCommandletRendering")))
    {UE_LOG(LogTemp,Error,TEXT("Explicit tangent source repair requires -NullRHI without -AllowCommandletRendering"));return false;}
    TArray<TUniquePtr<FPlan>> Plans;TArray<TSharedPtr<FJsonValue>> Rows;
    bool bValid=true;FString Error;
    for(int32 Variant:{1,3})
    {
        auto Plan=MakeUnique<FPlan>();const FString Stem=FString::Printf(TEXT("SM_Broadleaf_%02d"),Variant);
        Plan->Mesh=LoadObject<UStaticMesh>(nullptr,*(TEXT("/Game/Environment/Meshes/Realistic/")+Stem+TEXT(".")+Stem));
        if(!Plan->Mesh||Plan->Mesh->GetNumSourceModels()!=3||!Plan->Mesh->CloneMeshDescription(0,Plan->Original))
        {Error=TEXT("Missing exact target sourceLOD0 ")+Stem;bValid=false;break;}
        Plan->OriginalBuild=Plan->Mesh->GetSourceModel(0).BuildSettings;
        Plan->Protected=ProtectedSource(Plan->Original);Plan->Working=Plan->Original;
        const FStaticMeshConstAttributes OriginalAttributes(Plan->Original);
        for(const FVertexInstanceID ID:Plan->Original.VertexInstances().GetElementIDs())
        {
            const FVector3f N=OriginalAttributes.GetVertexInstanceNormals()[ID];
            if(!Finite(N)||N.SizeSquared()<=.01f){Error=TEXT("Refuse to alter invalid imported normal ")+Stem;bValid=false;break;}
        }
        if(!bValid)break;
        // Imported normals remain untouched. Mikk establishes every ordinary
        // corner first; only its invalid residuals get incident-UV repair.
        FStaticMeshOperations::ComputeTriangleTangentsAndNormals(Plan->Working,0.f,*Stem);
        FStaticMeshOperations::ComputeTangentsAndNormals(Plan->Working,EComputeNTBsFlags::Tangents|EComputeNTBsFlags::UseMikkTSpace);
        FStaticMeshAttributes A(Plan->Working);auto N=A.GetVertexInstanceNormals();auto T=A.GetVertexInstanceTangents();auto S=A.GetVertexInstanceBinormalSigns();
        TArray<int32> Changed;
        for(const FVertexInstanceID ID:Plan->Working.VertexInstances().GetElementIDs())
        {
            if(FrameValid(N[ID],T[ID],S[ID]))continue;
            FVector3f Tangent;float Sign=1.f;
            if(!DerivativeFrame(Plan->Working,ID,Tangent,Sign)){Error=TEXT("No valid incident UV derivative for ")+Stem;bValid=false;break;}
            T[ID]=Tangent;S[ID]=Sign;Changed.Add(ID.GetValue());++Plan->Repaired;
        }
        if(!bValid)break;
        if(Plan->Repaired>128||!SourceFramesValid(Plan->Working)||ProtectedSource(Plan->Working)!=Plan->Protected)
        {Error=TEXT("Source preservation/frame/narrow repair budget gate failed ")+Stem;bValid=false;break;}
        Plan->Row->SetStringField(TEXT("asset"),Plan->Mesh->GetPathName());Plan->Row->SetNumberField(TEXT("lod"),0);
        Plan->Row->SetNumberField(TEXT("repaired_vertex_instances"),Plan->Repaired);
        TArray<TSharedPtr<FJsonValue>> IDs;for(int32 I:Changed)IDs.Add(MakeShared<FJsonValueNumber>(I));
        Plan->Row->SetArrayField(TEXT("repaired_vertex_instance_ids"),IDs);
        Plan->Row->SetStringField(TEXT("protected_source_sha1"),Digest(Plan->Protected));
        Plan->Row->SetBoolField(TEXT("geometry_normals_colors_uv_material_topology_preserved"),true);
        Rows.Add(MakeShared<FJsonValueObject>(Plan->Row));Plans.Add(MoveTemp(Plan));
    }
    // All targets preflight before either native source package is modified.
    if(bValid&&Plans.Num()==2)for(auto& Plan:Plans)
    {
        UStaticMesh* Mesh=Plan->Mesh;Mesh->Modify();
        Mesh->CreateMeshDescription(0,Plan->Working);Mesh->CommitMeshDescription(0);
        auto& Build=Mesh->GetSourceModel(0).BuildSettings;
        Build.bRecomputeNormals=false;Build.bRecomputeTangents=false;Build.bUseMikkTSpace=true;
        Plan->bInstalled=true;TArray<FText> BuildErrors;Mesh->Build(true,&BuildErrors);
        FStaticMeshCompilingManager::Get().FinishAllCompilation();
        FMeshDescription Readback;int32 Invalid=0;
        const bool bSource=Mesh->CloneMeshDescription(0,Readback)&&ProtectedSource(Readback)==Plan->Protected&&SourceFramesValid(Readback);
        const bool bRender=RenderFramesValid(Mesh,0,Invalid);
        Plan->Row->SetNumberField(TEXT("rebuilt_render_invalid_frames"),Invalid);
        Plan->Row->SetBoolField(TEXT("rebuilt_render_frames_valid"),bRender);
        Plan->Row->SetBoolField(TEXT("recompute_tangents"),Build.bRecomputeTangents);
        if(!bSource||!bRender||!BuildErrors.IsEmpty())
        {Error=TEXT("Rebuilt source/render basis gate failed ")+Mesh->GetName();bValid=false;break;}
    }
    if(!bValid)
    {
        // A failed attempt is never persisted. Restore in-memory source/settings
        // so a later operation in this commandlet cannot accidentally save it.
        for(auto& Plan:Plans)if(Plan->bInstalled)
        {Plan->Mesh->CreateMeshDescription(0,Plan->Original);Plan->Mesh->CommitMeshDescription(0);Plan->Mesh->GetSourceModel(0).BuildSettings=Plan->OriginalBuild;}
    }
    else for(auto& Plan:Plans)
    {
        UPackage* Package=Plan->Mesh->GetOutermost();Package->GetMetaData().SetValue(Plan->Mesh,ExplicitTag,ExplicitVersion);
        Package->MarkPackageDirty();FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;Args.SaveFlags=SAVE_NoError;
        const FString Filename=FPackageName::LongPackageNameToFilename(Package->GetName(),FPackageName::GetAssetPackageExtension());
        if(!UPackage::SavePackage(Package,Plan->Mesh,*Filename,Args))
        {Error=TEXT("Native package save failed ")+Plan->Mesh->GetPathName();bValid=false;break;}
        Plan->Row->SetBoolField(TEXT("saved"),true);
        UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_EXPLICIT_TANGENT_REPAIR asset=%s lod=0 residualCorners=%d protectedSource=%s renderInvalid=0 saved=1"),
            *Plan->Mesh->GetPathName(),Plan->Repaired,*Digest(Plan->Protected));
    }
    auto Report=MakeShared<FJsonObject>();Report->SetBoolField(TEXT("valid"),bValid);
    Report->SetStringField(TEXT("policy"),ExplicitVersion);Report->SetStringField(TEXT("error"),Error);
    Report->SetStringField(TEXT("proof_scope"),TEXT("Two exact LOD0 explicit source tangent bases and rebuilt CPU native frames. Separate fresh-process 45LOD render readback remains required."));
    Report->SetArrayField(TEXT("results"),Rows);FString Output;
    const auto Writer=TJsonWriterFactory<>::Create(&Output);FJsonSerializer::Serialize(Report,Writer);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsoluteReportPath),true);
    const bool bSaved=FFileHelper::SaveStringToFile(Output,*AbsoluteReportPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    if(!bValid)UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_EXPLICIT_TANGENT_REPAIR_FAILED %s"),*Error);
    return bValid&&bSaved;
}
#else
bool RepairAftershockBroadleafExplicitTangents(const FString&){return false;}
#endif
