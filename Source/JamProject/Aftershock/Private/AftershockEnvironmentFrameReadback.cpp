#include "AftershockEnvironmentFrameReadback.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "Rendering/ColorVertexBuffer.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#if WITH_EDITOR
#include "StaticMeshCompiler.h"
#endif

bool ReadAftershockEnvironmentRenderFrames(const FString& AbsoluteReportPath)
{
    TArray<TSharedPtr<FJsonValue>> Rows;
    const TCHAR* Families[]={TEXT("GrassClump"),TEXT("Shrub"),TEXT("Conifer"),TEXT("Palm"),TEXT("Broadleaf")};
    TArray<UStaticMesh*> Meshes;
    bool bAllValid=true;int32 TotalLODs=0;uint64 TotalVertices=0;
    for(const TCHAR* Family:Families)for(int32 Variant=1;Variant<=3;++Variant)
    {
        const FString Stem=FString::Printf(TEXT("SM_%s_%02d"),Family,Variant);
        const FString Package=TEXT("/Game/Environment/Meshes/Realistic/")+Stem+TEXT(".")+Stem;
        UStaticMesh* Mesh=LoadObject<UStaticMesh>(nullptr,*Package);
        if(!Mesh){bAllValid=false;UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_ENVIRONMENT_FRAME_MISSING %s"),*Package);}
        else Meshes.Add(Mesh);
    }
#if WITH_EDITOR
    FStaticMeshCompilingManager::Get().FinishAllCompilation();
#endif
    for(UStaticMesh* Mesh:Meshes)
    {
        const auto* Render=Mesh->GetRenderData();
        if(!Render||Render->LODResources.Num()!=3)
        {bAllValid=false;UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_ENVIRONMENT_FRAME_LOD_MISSING %s"),*Mesh->GetPathName());continue;}
        for(int32 LODIndex=0;LODIndex<Render->LODResources.Num();++LODIndex)
        {
            ++TotalLODs;
            const auto& LOD=Render->LODResources[LODIndex];
            const auto& Vertices=LOD.VertexBuffers.StaticMeshVertexBuffer;
            const auto& Colors=LOD.VertexBuffers.ColorVertexBuffer;
            const uint32 Count=Vertices.GetNumVertices();
            const bool bCPUAvailable=Count>0&&Vertices.GetTangentData()!=nullptr;
            uint32 NonFinite=0,ZeroFrame=0,ParallelFrame=0;
            float MinimumX=MAX_flt,MinimumY=MAX_flt,MinimumZ=MAX_flt,MinimumCross=MAX_flt;
            uint8 WindMin=255,WindMax=0,LeafMin=255,LeafMax=0;
            if(bCPUAvailable)for(uint32 Index=0;Index<Count;++Index)
            {
                const FVector4f X4=Vertices.VertexTangentX(Index),Z4=Vertices.VertexTangentZ(Index);
                const FVector3f X(X4.X,X4.Y,X4.Z),Z(Z4.X,Z4.Y,Z4.Z),Y=Vertices.VertexTangentY(Index);
                const auto Finite=[](const FVector3f& V){return FMath::IsFinite(V.X)&&FMath::IsFinite(V.Y)&&FMath::IsFinite(V.Z);};
                if(!Finite(X)||!Finite(Y)||!Finite(Z)){++NonFinite;continue;}
                const float LX=X.SizeSquared(),LY=Y.SizeSquared(),LZ=Z.SizeSquared();
                const float Cross=FVector3f::CrossProduct(X,Z).SizeSquared();
                MinimumX=FMath::Min(MinimumX,LX);MinimumY=FMath::Min(MinimumY,LY);MinimumZ=FMath::Min(MinimumZ,LZ);MinimumCross=FMath::Min(MinimumCross,Cross);
                if(LX<.01f||LY<.01f||LZ<.01f)++ZeroFrame;
                if(Cross<.01f)++ParallelFrame;
            }
            const bool bColorsAvailable=Colors.GetNumVertices()==Count&&Count>0&&Colors.GetVertexData()!=nullptr;
            if(bColorsAvailable)for(uint32 Index=0;Index<Count;++Index)
            {
                const FColor C=Colors.VertexColor(Index);
                WindMin=FMath::Min(WindMin,C.R);WindMax=FMath::Max(WindMax,C.R);
                LeafMin=FMath::Min(LeafMin,C.G);LeafMax=FMath::Max(LeafMax,C.G);
            }
            const bool bValid=bCPUAvailable&&NonFinite==0&&ZeroFrame==0&&ParallelFrame==0&&bColorsAvailable;
            bAllValid&=bValid;TotalVertices+=Count;
            auto Row=MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("asset"),Mesh->GetPathName());Row->SetNumberField(TEXT("lod"),LODIndex);
            Row->SetBoolField(TEXT("valid"),bValid);Row->SetBoolField(TEXT("render_cpu_tangent_data_available"),bCPUAvailable);
            Row->SetNumberField(TEXT("render_vertices"),Count);Row->SetNumberField(TEXT("nonfinite_frames"),NonFinite);
            Row->SetNumberField(TEXT("zero_frames"),ZeroFrame);Row->SetNumberField(TEXT("parallel_frames"),ParallelFrame);
            Row->SetNumberField(TEXT("minimum_tangent_length_squared"),bCPUAvailable?MinimumX:0.f);
            Row->SetNumberField(TEXT("minimum_binormal_length_squared"),bCPUAvailable?MinimumY:0.f);
            Row->SetNumberField(TEXT("minimum_normal_length_squared"),bCPUAvailable?MinimumZ:0.f);
            Row->SetNumberField(TEXT("minimum_tangent_cross_normal_length_squared"),bCPUAvailable?MinimumCross:0.f);
            Row->SetBoolField(TEXT("color_buffer_present"),bColorsAvailable);
            Row->SetNumberField(TEXT("wind_r_min"),WindMin);Row->SetNumberField(TEXT("wind_r_max"),WindMax);
            Row->SetNumberField(TEXT("leaf_g_min"),LeafMin);Row->SetNumberField(TEXT("leaf_g_max"),LeafMax);
            Rows.Add(MakeShared<FJsonValueObject>(Row));
            UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_ENVIRONMENT_FRAME_READBACK asset=%s lod=%d valid=%d vertices=%u nonfinite=%u zero=%u parallel=%u colorPresent=%d"),
                *Mesh->GetPathName(),LODIndex,bValid,Count,NonFinite,ZeroFrame,ParallelFrame,bColorsAvailable);
        }
    }
    bAllValid&=Meshes.Num()==15&&TotalLODs==45;
    auto Report=MakeShared<FJsonObject>();
    Report->SetBoolField(TEXT("valid"),bAllValid);Report->SetStringField(TEXT("proof_scope"),TEXT("Actual compiled native render-buffer tangentX/binormalY/normalZ and vertexcolour availability on every environment LOD; not screenshot or GPU timing proof"));
    Report->SetNumberField(TEXT("meshes"),Meshes.Num());Report->SetNumberField(TEXT("lods"),TotalLODs);
    Report->SetNumberField(TEXT("render_vertices"),static_cast<double>(TotalVertices));Report->SetArrayField(TEXT("results"),Rows);
    FString Output;const auto Writer=TJsonWriterFactory<>::Create(&Output);FJsonSerializer::Serialize(Report,Writer);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsoluteReportPath),true);
    const bool bSaved=FFileHelper::SaveStringToFile(Output,*AbsoluteReportPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    return bAllValid&&bSaved;
}
