#include "AftershockEnvironmentAuditCommandlet.h"
#include "AftershockEnvironmentFrameReadback.h"
#include "AftershockEnvironmentTangentRepair.h"
#include "AftershockNativeFacadeTangentRepair.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#if WITH_EDITOR
#include "AftershockMassMovementAudit.h"
#include "AftershockMassSparseFieldAudit.h"
#include "AftershockPhysicalReplayAudit.h"
#include "AftershockPhysicalHazardAudit.h"
#include "AftershockHydrologySolverReferenceTests.h"
#include "AftershockHydrologyAsyncStepperReferenceTests.h"
#include "AftershockHydraulicTerrainReferenceTests.h"
#include "AftershockHydraulicObstructionsReferenceTests.h"
#include "Materials/Material.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "StaticMeshCompiler.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "MaterialShared.h"
#include "UnrealEngine.h"
#include "RHIGlobals.h"
#include "ShaderCore.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/CommandLine.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"

bool RunAftershockMaterialExtensionFixtures(const FString& AbsoluteReportPath);
bool RunAftershockPhysicalLifecycleFixtures(const FString& AbsoluteReportPath);
bool RunAftershockMassSupportPreparationFixtures(const FString& AbsoluteReportPath);
bool RunAftershockHydraulicGroundInstallFixtures(const FString& AbsoluteReportPath);

namespace
{
bool WriteMaterialResourceReceipt(const TSharedRef<FJsonObject>& Report, const FString& Path)
{
    FString Output;
    const auto Writer = TJsonWriterFactory<>::Create(&Output);
    if (!FJsonSerializer::Serialize(Report, Writer)) return false;
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
    const FString Temporary = Path + TEXT(".tmp");
    return FFileHelper::SaveStringToFile(Output, *Temporary,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
        && IFileManager::Get().Move(*Path, *Temporary, true, false, false, true);
}

bool RunHydrologyAsyncStepperAudit()
{
    const FString Path=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/hydrology_async_stepper.json"));
    auto Report=MakeShared<FJsonObject>();
    Report->SetNumberField(TEXT("schema_version"),1);
    Report->SetStringField(TEXT("scope"),TEXT("Isolated real ThreadPool ownership/frontier/debt lifecycle fixtures; no world, historical-save, render or calibrated flood acceptance."));
    Report->SetBoolField(TEXT("valid"),false);Report->SetBoolField(TEXT("in_progress"),true);
    if(!WriteMaterialResourceReceipt(Report,Path))return false;
    const double Started=FPlatformTime::Seconds();
    const auto Checks=AftershockWater::RunHydrologyAsyncStepperReferenceChecks();
    TArray<TSharedPtr<FJsonValue>> Rows;int32 Failed=0;
    for(const auto& Check:Checks)
    {
        auto Row=MakeShared<FJsonObject>();const FString Name=UTF8_TO_TCHAR(Check.Name.c_str());
        Row->SetStringField(TEXT("name"),Name);Row->SetBoolField(TEXT("passed"),Check.Passed);
        Rows.Add(MakeShared<FJsonValueObject>(Row));Failed+=Check.Passed?0:1;
        if(!Check.Passed)UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_HYDROLOGY_ASYNC_FAIL %s"),*Name);
    }
    const bool Valid=Checks.size()==46&&Failed==0;
    Report->SetArrayField(TEXT("checks"),Rows);Report->SetNumberField(TEXT("passed"),static_cast<int32>(Checks.size())-Failed);
    Report->SetNumberField(TEXT("failed"),Failed);Report->SetNumberField(TEXT("compute_ms"),(FPlatformTime::Seconds()-Started)*1000.);
    Report->SetBoolField(TEXT("valid"),Valid);Report->SetBoolField(TEXT("in_progress"),false);
    const bool Saved=WriteMaterialResourceReceipt(Report,Path);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_HYDROLOGY_ASYNC checked=%d failed=%d saved=%d"),static_cast<int32>(Checks.size()),Failed,Saved);
    return Valid&&Saved;
}


bool RunHydraulicObstructionAudit()
{
    constexpr int32 ExpectedChecks=29;
    const FString Path=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/hydraulic_obstructions.json"));
    auto Report=MakeShared<FJsonObject>();
    Report->SetNumberField(TEXT("schema_version"),1);
    Report->SetStringField(TEXT("scope"),TEXT("Isolated pure canonical-obstruction adapter fixtures only. No runtime binding, building transaction, typed-save migration, street-flow, RHI or calibrated-flood acceptance."));
    Report->SetNumberField(TEXT("expected_checks"),ExpectedChecks);
    Report->SetBoolField(TEXT("valid"),false);Report->SetBoolField(TEXT("in_progress"),true);
    if(!WriteMaterialResourceReceipt(Report,Path))return false;
    const double Started=FPlatformTime::Seconds();
    const auto Checks=AftershockWater::RunHydraulicObstructionReferenceChecks();
    TArray<TSharedPtr<FJsonValue>> Rows;int32 Failed=0;
    for(const auto& Check:Checks)
    {
        auto Row=MakeShared<FJsonObject>();const FString Name=UTF8_TO_TCHAR(Check.Name.c_str());
        Row->SetStringField(TEXT("name"),Name);Row->SetBoolField(TEXT("passed"),Check.Passed);
        Rows.Add(MakeShared<FJsonValueObject>(Row));Failed+=Check.Passed?0:1;
        if(!Check.Passed){UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_HYDRAULIC_OBSTRUCTIONS_FAIL %s"),*Name);}
    }
    const int32 Checked=static_cast<int32>(Checks.size());const bool Valid=Checked==ExpectedChecks&&Failed==0;
    Report->SetArrayField(TEXT("checks"),Rows);Report->SetNumberField(TEXT("checked"),Checked);
    Report->SetNumberField(TEXT("passed"),Checked-Failed);Report->SetNumberField(TEXT("failed"),Failed);
    Report->SetNumberField(TEXT("compute_ms"),(FPlatformTime::Seconds()-Started)*1000.);
    Report->SetBoolField(TEXT("valid"),Valid);Report->SetBoolField(TEXT("in_progress"),false);
    const bool Saved=WriteMaterialResourceReceipt(Report,Path);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_HYDRAULIC_OBSTRUCTIONS checked=%d passed=%d failed=%d expected=%d saved=%d"),Checked,Checked-Failed,Failed,ExpectedChecks,Saved);
    return Valid&&Saved;
}

bool RunHydraulicNumericalAudit()
{
    const FString Path=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/hydrology_numerics.json"));
    auto Report=MakeShared<FJsonObject>();
    Report->SetNumberField(TEXT("schema_version"),1);
    Report->SetStringField(TEXT("scope"),TEXT("Isolated synthetic SI finite-volume hydrology fixtures. No gameplay, render, historical-save or real flood forecast acceptance."));
    Report->SetBoolField(TEXT("valid"),false);Report->SetBoolField(TEXT("in_progress"),true);
    if(!WriteMaterialResourceReceipt(Report,Path))return false;
    const double Started=FPlatformTime::Seconds();
    const auto Checks=AftershockWater::RunHydrologyReferenceChecks();
    TArray<TSharedPtr<FJsonValue>> Rows;int32 Failed=0;
    for(const auto& Check:Checks)
    {
        auto Row=MakeShared<FJsonObject>();const FString Name=UTF8_TO_TCHAR(Check.Name.c_str());
        Row->SetStringField(TEXT("name"),Name);Row->SetBoolField(TEXT("passed"),Check.Passed);
        Rows.Add(MakeShared<FJsonValueObject>(Row));Failed+=Check.Passed?0:1;
        if(!Check.Passed)UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_HYDROLOGY_NUMERICS_FAIL %s"),*Name);
    }
    const bool Valid=Checks.size()==52&&Failed==0;
    Report->SetArrayField(TEXT("checks"),Rows);Report->SetNumberField(TEXT("passed"),static_cast<int32>(Checks.size())-Failed);
    Report->SetNumberField(TEXT("failed"),Failed);Report->SetNumberField(TEXT("compute_ms"),(FPlatformTime::Seconds()-Started)*1000.);
    Report->SetBoolField(TEXT("valid"),Valid);Report->SetBoolField(TEXT("in_progress"),false);
    const bool Saved=WriteMaterialResourceReceipt(Report,Path);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_HYDROLOGY_NUMERICS checked=%d failed=%d saved=%d"),static_cast<int32>(Checks.size()),Failed,Saved);
    return Valid&&Saved;
}


bool ReadNativeCitySampleFacadeFrames()
{
    const FString Path=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/citysample_facade_render_frames.json"));
    auto Report=MakeShared<FJsonObject>();
    Report->SetBoolField(TEXT("valid"),false);Report->SetBoolField(TEXT("in_progress"),true);
    Report->SetStringField(TEXT("run_id"),FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
    Report->SetStringField(TEXT("started_utc"),FDateTime::UtcNow().ToIso8601());
    Report->SetStringField(TEXT("proof_scope"),TEXT("Read-only actual compiled tangentX/binormalY/normalZ, handedness, finite positions and retained glass sections on exactly16 native facade meshes/48LODs. Source UV determinants are diagnostic. No asset writes, shader/cook/visual/performance acceptance."));
    if(!WriteMaterialResourceReceipt(Report,Path))return false;
    TArray<UStaticMesh*> Meshes;TArray<TSharedPtr<FJsonValue>> Missing,Rows;
    bool All=true;uint64 TotalVertices=0;int32 LODCount=0;
    for(int32 Floors=1;Floors<=4;++Floors)for(int32 Bays=1;Bays<=4;++Bays)
    {
        const FString Stem=FString::Printf(TEXT("SM_CS_Facade_B%d_F%02d"),Bays,Floors);
        const FString Object=TEXT("/Game/Aftershock/CitySample/Architecture/Baked/")+Stem+TEXT(".")+Stem;
        UStaticMesh* Mesh=LoadObject<UStaticMesh>(nullptr,*Object);
        if(Mesh)Meshes.Add(Mesh);else{All=false;Missing.Add(MakeShared<FJsonValueString>(Object));}
    }
    FStaticMeshCompilingManager::Get().FinishAllCompilation();
    for(UStaticMesh* Mesh:Meshes)
    {
        const auto* Render=Mesh->GetRenderData();
        int32 Glass=INDEX_NONE;
        const auto& Slots=Mesh->GetStaticMaterials();
        for(int32 I=0;I<Slots.Num();++I)if(Slots[I].MaterialSlotName==TEXT("Glass"))Glass=I;
        const bool MeshValid=Render&&Render->LODResources.Num()==3&&Slots.Num()==3&&Glass!=INDEX_NONE;
        if(!MeshValid){All=false;continue;}
        const FMeshDescription* Source=Mesh->GetMeshDescription(0);
        int32 UVZero=0,UVNonFinite=0,GeometryZero=0;
        double MinimumUV=MAX_dbl;
        if(Source)
        {
            const FStaticMeshConstAttributes A(*Source);const auto UV=A.GetVertexInstanceUVs();const auto P=A.GetVertexPositions();
            if(UV.GetNumChannels()>0)for(const FTriangleID Triangle:Source->Triangles().GetElementIDs())
            {
                const auto V=Source->GetTriangleVertexInstances(Triangle);
                const FVector2f A0=UV.Get(V[0],0),A1=UV.Get(V[1],0),A2=UV.Get(V[2],0);
                const double Det=double(A1.X-A0.X)*(A2.Y-A0.Y)-double(A1.Y-A0.Y)*(A2.X-A0.X);
                if(!FMath::IsFinite(Det))++UVNonFinite;
                else{MinimumUV=FMath::Min(MinimumUV,FMath::Abs(Det));UVZero+=FMath::Abs(Det)<=1.e-12?1:0;}
                const FVector3f E1=P[Source->GetVertexInstanceVertex(V[1])]-P[Source->GetVertexInstanceVertex(V[0])];
                const FVector3f E2=P[Source->GetVertexInstanceVertex(V[2])]-P[Source->GetVertexInstanceVertex(V[0])];
                GeometryZero+=FVector3f::CrossProduct(E1,E2).SizeSquared()<=1.e-14f?1:0;
            }
        }
        for(int32 L=0;L<3;++L)
        {
            ++LODCount;const auto& Data=Render->LODResources[L];
            const auto& V=Data.VertexBuffers.StaticMeshVertexBuffer;const auto& P=Data.VertexBuffers.PositionVertexBuffer;
            const uint32 Count=V.GetNumVertices();
            const bool CPU=Count>0&&Count<=200000&&V.GetTangentData()!=nullptr&&P.GetNumVertices()==Count&&P.GetVertexData()!=nullptr;
            uint32 NonFinite=0,Zero=0,Parallel=0,NonOrthogonal=0,BadHandedness=0;
            float MinimumX=MAX_flt,MinimumY=MAX_flt,MinimumZ=MAX_flt,MinimumCross=MAX_flt,MinimumHand=MAX_flt;
            const auto Finite=[](const FVector3f& A){return FMath::IsFinite(A.X)&&FMath::IsFinite(A.Y)&&FMath::IsFinite(A.Z);};
            if(CPU)for(uint32 I=0;I<Count;++I)
            {
                const FVector4f X4=V.VertexTangentX(I),Z4=V.VertexTangentZ(I);
                const FVector3f X(X4.X,X4.Y,X4.Z),Z(Z4.X,Z4.Y,Z4.Z),Y=V.VertexTangentY(I),Position=P.VertexPosition(I);
                if(!Finite(X)||!Finite(Y)||!Finite(Z)||!Finite(Position)||!FMath::IsFinite(Z4.W)){++NonFinite;continue;}
                const float LX=X.SizeSquared(),LY=Y.SizeSquared(),LZ=Z.SizeSquared(),Cross=FVector3f::CrossProduct(X,Z).SizeSquared();
                MinimumX=FMath::Min(MinimumX,LX);MinimumY=FMath::Min(MinimumY,LY);MinimumZ=FMath::Min(MinimumZ,LZ);MinimumCross=FMath::Min(MinimumCross,Cross);
                if(LX<.01f||LY<.01f||LZ<.01f){++Zero;continue;}
                if(Cross<.01f){++Parallel;continue;}
                const float Dot=FMath::Abs(FVector3f::DotProduct(X,Z)/FMath::Sqrt(LX*LZ));
                if(Dot>.05f)++NonOrthogonal;
                const float Hand=FVector3f::DotProduct(FVector3f::CrossProduct(Z,X),Y)/FMath::Sqrt(LX*LY*LZ);
                MinimumHand=FMath::Min(MinimumHand,FMath::Abs(Hand));
                if(!FMath::IsFinite(Hand)||FMath::Abs(Z4.W)<.5f||Hand*Z4.W<.9f)++BadHandedness;
            }
            bool HasGlass=false;bool Sections=Data.Sections.Num()>0;
            for(const auto& Section:Data.Sections)
            {
                Sections&=Section.MaterialIndex>=0&&Section.MaterialIndex<Slots.Num()&&Section.NumTriangles>0;
                HasGlass|=Section.MaterialIndex==Glass&&Section.NumTriangles>0;
            }
            const uint32 Caps[]={50000,18000,6000};
            const bool Triangles=Data.GetNumTriangles()>0&&static_cast<uint32>(Data.GetNumTriangles())<=Caps[L];
            const bool Valid=CPU&&NonFinite==0&&Zero==0&&Parallel==0&&NonOrthogonal==0&&BadHandedness==0&&Sections&&HasGlass&&Triangles;
            All&=Valid;TotalVertices+=Count;
            auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("asset"),Mesh->GetPathName());
            Row->SetNumberField(TEXT("lod"),L);Row->SetBoolField(TEXT("valid"),Valid);Row->SetBoolField(TEXT("cpu_render_data_available"),CPU);
            Row->SetNumberField(TEXT("vertices"),Count);Row->SetNumberField(TEXT("triangles"),Data.GetNumTriangles());
            Row->SetNumberField(TEXT("nonfinite_frames_or_positions"),NonFinite);Row->SetNumberField(TEXT("zero_frames"),Zero);
            Row->SetNumberField(TEXT("parallel_frames"),Parallel);Row->SetNumberField(TEXT("nonorthogonal_frames"),NonOrthogonal);Row->SetNumberField(TEXT("invalid_handedness"),BadHandedness);
            Row->SetNumberField(TEXT("minimum_tangent_length_squared"),CPU?MinimumX:0.f);Row->SetNumberField(TEXT("minimum_binormal_length_squared"),CPU?MinimumY:0.f);
            Row->SetNumberField(TEXT("minimum_normal_length_squared"),CPU?MinimumZ:0.f);Row->SetNumberField(TEXT("minimum_cross_length_squared"),CPU?MinimumCross:0.f);
            Row->SetNumberField(TEXT("minimum_abs_normalized_handedness"),CPU&&MinimumHand<MAX_flt?MinimumHand:0.f);
            Row->SetBoolField(TEXT("retained_native_glass_section"),HasGlass);Row->SetBoolField(TEXT("source_lod0_description_present"),Source!=nullptr);
            Row->SetNumberField(TEXT("source_lod0_zero_uv_triangles"),UVZero);Row->SetNumberField(TEXT("source_lod0_nonfinite_uv_triangles"),UVNonFinite);
            Row->SetNumberField(TEXT("source_lod0_geometrically_degenerate_triangles"),GeometryZero);
            Row->SetNumberField(TEXT("source_lod0_minimum_uv_double_area"),MinimumUV<MAX_dbl?MinimumUV:0.);
            Rows.Add(MakeShared<FJsonValueObject>(Row));
            UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_NATIVE_FACADE_FRAME asset=%s lod=%d valid=%d vertices=%u nonfinite=%u zero=%u parallel=%u nonorthogonal=%u handedness=%u sourceUVzero=%d sourceGeometryZero=%d"),
                *Mesh->GetPathName(),L,Valid,Count,NonFinite,Zero,Parallel,NonOrthogonal,BadHandedness,UVZero,GeometryZero);
            Report->SetArrayField(TEXT("results"),Rows);if(!WriteMaterialResourceReceipt(Report,Path))return false;
        }
    }
    All&=Meshes.Num()==16&&LODCount==48;
    Report->SetNumberField(TEXT("expected_meshes"),16);Report->SetNumberField(TEXT("expected_lods"),48);
    Report->SetNumberField(TEXT("meshes"),Meshes.Num());Report->SetNumberField(TEXT("lods"),LODCount);
    Report->SetNumberField(TEXT("vertices"),static_cast<double>(TotalVertices));Report->SetArrayField(TEXT("missing"),Missing);
    Report->SetBoolField(TEXT("valid"),All);Report->SetBoolField(TEXT("in_progress"),false);
    Report->SetStringField(TEXT("completed_utc"),FDateTime::UtcNow().ToIso8601());
    const bool Saved=WriteMaterialResourceReceipt(Report,Path);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_NATIVE_FACADE_FRAMES valid=%d meshes=%d lods=%d saved=%d"),All,Meshes.Num(),LODCount,Saved);
    return All&&Saved;
}

bool ReadCurrentMaterialResources()
{
    const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
        / TEXT("PresentationValidation/environment_material_resources.json"));
    const EShaderPlatform Platform = GMaxRHIShaderPlatform;
    const ERHIFeatureLevel::Type Feature = GMaxRHIFeatureLevel;
    const EMaterialQualityLevel::Type Quality = GetCachedScalabilityCVars().MaterialQualityLevel;
    auto Report = MakeShared<FJsonObject>();
    Report->SetBoolField(TEXT("valid"), false);
    Report->SetBoolField(TEXT("in_progress"), true);
    Report->SetStringField(TEXT("run_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
    Report->SetStringField(TEXT("started_utc"), FDateTime::UtcNow().ToIso8601());
    Report->SetStringField(TEXT("proof_scope"), TEXT("Own native FMaterialResource and game-thread shader maps at the current RHI/feature/material quality only; no proxy fallback, package writes, cook/platform coverage, GPU timing or visual acceptance."));
    Report->SetNumberField(TEXT("requested_shader_platform"), static_cast<int32>(Platform));
    Report->SetNumberField(TEXT("requested_feature_level"), static_cast<int32>(Feature));
    Report->SetNumberField(TEXT("requested_material_quality"), static_cast<int32>(Quality));
    if (const auto* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MaterialQualityLevel")))
        Report->SetNumberField(TEXT("material_quality_cvar"), CVar->GetInt());
    // A previous successful receipt is invalidated before any asset load.
    if (!WriteMaterialResourceReceipt(Report, Path))
    {
        UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_MATERIAL_RESOURCES cannot invalidate receipt %s"), *Path);
        return false;
    }
    if (GUsingNullRHI || !FParse::Param(FCommandLine::Get(), TEXT("AllowCommandletRendering")))
    {
        Report->SetBoolField(TEXT("in_progress"), false);
        Report->SetStringField(TEXT("error"), TEXT("MaterialResources requires a real RHI and -AllowCommandletRendering; do not use -NullRHI."));
        WriteMaterialResourceReceipt(Report, Path);
        UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_MATERIAL_RESOURCES requires real commandlet rendering"));
        return false;
    }

    // Explicit bounded production list: the original finalizer set plus the
    // hydraulic surface and four physical mass-movement materials.
    TArray<FString> Packages;
    for (const TCHAR* Family : {TEXT("Plaster"), TEXT("Wood"), TEXT("Roof"), TEXT("Concrete"), TEXT("Brick"), TEXT("Metal"), TEXT("Glass")})
        Packages.Add(FString(TEXT("/Game/Buildings/Architecture/Materials/M_Arch_")) + Family);
    for (const TCHAR* Family : {TEXT("Bark"), TEXT("Grass"), TEXT("Shrub"), TEXT("Conifer"), TEXT("Palm"), TEXT("Broadleaf"), TEXT("Asphalt"), TEXT("LaneMarkings"), TEXT("Sidewalk")})
        Packages.Add(FString(TEXT("/Game/Environment/Materials/Realistic/M_Environment")) + Family);
    for (const TCHAR* Name : {TEXT("M_PresentationGroundPBR"), TEXT("M_AftershockRegionalBlend"), TEXT("M_PresentationWater"), TEXT("M_CoastalSea"), TEXT("M_EmergencyAssembly")})
        Packages.Add(FString(TEXT("/Game/Environment/Materials/Presentation/")) + Name);
    Packages.Add(TEXT("/Game/FX/Aftershock/Materials/M_DisasterFloodVolume"));
    Packages.Add(TEXT("/Game/FX/Aftershock/Materials/M_DisasterTsunamiWave"));
    Packages.Add(TEXT("/Game/Buildings/Materials/M_BuildingGhost"));
    Packages.Add(TEXT("/Game/Environment/Water/M_HydraulicSurface"));
    for (const TCHAR* Name : {TEXT("M_AvalanchePowder"), TEXT("M_AvalancheSnow"), TEXT("M_LandslideDebris"), TEXT("M_LandslideDust")})
        Packages.Add(FString(TEXT("/Game/FX/Aftershock/MassMovement/")) + Name);

    constexpr int32 ExpectedMaterialCount = 29;
    bool bAllValid = Packages.Num() == ExpectedMaterialCount;
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FString& Package : Packages)
    {
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("asset"), Package);
        UMaterial* Material = LoadObject<UMaterial>(nullptr, *(Package + TEXT(".") + FPaths::GetCleanFilename(Package)));
        FMaterialResource* Resource = Material ? Material->GetMaterialResource(Platform, Quality) : nullptr;
        Row->SetBoolField(TEXT("loaded_material"), Material != nullptr);
        Row->SetBoolField(TEXT("own_resource_present"), Resource != nullptr);
        bool bValid = false;
        if (Resource)
        {
            const bool bOwn = Resource->GetMaterial() == Material
                && Resource->GetMaterialInterface() == Material && !Resource->IsDefaultMaterial();
            const bool bSubmitted = !Resource->IsGameThreadShaderMapComplete();
            if (bSubmitted) Resource->SubmitCompileJobs_GameThread(EShaderCompileJobPriority::High);
            Resource->FinishCompilation();
            // Read errors only AFTER the blocking completion, directly from this resource.
            TArray<TSharedPtr<FJsonValue>> Errors;
            for (const FString& Error : Resource->GetCompileErrors())
                Errors.Add(MakeShared<FJsonValueString>(Error));
            FMaterialShaderMap* Map = Resource->GetGameThreadShaderMap();
            const bool bFinished = Resource->IsCompilationFinished();
            const bool bHasValid = Resource->HasValidGameThreadShaderMap();
            const bool bComplete = Resource->IsGameThreadShaderMapComplete();
            const bool bFinalized = Map && Map->IsCompilationFinalized();
            const bool bSucceeded = Map && Map->CompiledSuccessfully();
            const bool bPlatform = Resource->GetShaderPlatform() == Platform
                && Map && Map->GetShaderPlatform() == Platform;
            const bool bFeature = Resource->GetFeatureLevel() == Feature
                && Map && Map->GetShaderMapId().FeatureLevel == Feature;
            const auto ResourceQuality = Resource->GetQualityLevel();
            // Num represents a quality-independent permutation of the SAME material.
            const bool bQuality = (ResourceQuality == Quality || ResourceQuality == EMaterialQualityLevel::Num)
                && Map && Map->GetShaderMapId().QualityLevel == ResourceQuality;
            const bool bState = Map && Map->GetShaderMapId().BaseMaterialId == Material->StateId;
            const int32 Samplers = Map ? Resource->GetSamplerUsage() : -1;
            bValid = bOwn && Errors.IsEmpty() && bFinished && bHasValid && bComplete
                && bFinalized && bSucceeded && bPlatform && bFeature && bQuality && bState
                && Samplers >= 0 && Samplers <= 16;
            Row->SetStringField(TEXT("resource_owner"), GetPathNameSafe(Resource->GetMaterialInterface()));
            Row->SetBoolField(TEXT("own_material_not_default"), bOwn);
            Row->SetBoolField(TEXT("submitted_missing_compile_jobs"), bSubmitted);
            Row->SetBoolField(TEXT("errors_read_after_finish"), true);
            Row->SetArrayField(TEXT("compile_errors"), Errors);
            Row->SetBoolField(TEXT("compilation_finished"), bFinished);
            Row->SetBoolField(TEXT("own_shader_map_present"), Map != nullptr);
            Row->SetBoolField(TEXT("own_shader_map_valid"), bHasValid);
            Row->SetBoolField(TEXT("own_shader_map_complete"), bComplete);
            Row->SetBoolField(TEXT("map_compilation_finalized"), bFinalized);
            Row->SetBoolField(TEXT("map_compiled_successfully"), bSucceeded);
            Row->SetBoolField(TEXT("platform_matches"), bPlatform);
            Row->SetBoolField(TEXT("feature_level_matches"), bFeature);
            Row->SetBoolField(TEXT("quality_matches"), bQuality);
            Row->SetBoolField(TEXT("material_state_id_matches"), bState);
            Row->SetNumberField(TEXT("resource_shader_platform"), static_cast<int32>(Resource->GetShaderPlatform()));
            Row->SetNumberField(TEXT("resource_feature_level"), static_cast<int32>(static_cast<ERHIFeatureLevel::Type>(Resource->GetFeatureLevel())));
            Row->SetNumberField(TEXT("resource_material_quality"), static_cast<int32>(ResourceQuality));
            Row->SetBoolField(TEXT("quality_independent_permutation"), ResourceQuality == EMaterialQualityLevel::Num);
            Row->SetNumberField(TEXT("samplers"), Samplers);
        }
        Row->SetBoolField(TEXT("valid"), bValid);
        Rows.Add(MakeShared<FJsonValueObject>(Row));
        bAllValid &= bValid;
        UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_MATERIAL_RESOURCE asset=%s valid=%d"), *Package, bValid);
        Report->SetArrayField(TEXT("results"), Rows);
        // Persist partial failures too; a crash can never leave the old success.
        if (!WriteMaterialResourceReceipt(Report, Path)) return false;
    }
    Report->SetNumberField(TEXT("expected_materials"), ExpectedMaterialCount);
    Report->SetNumberField(TEXT("checked_materials"), Rows.Num());
    Report->SetBoolField(TEXT("valid"), bAllValid);
    Report->SetBoolField(TEXT("in_progress"), false);
    Report->SetStringField(TEXT("completed_utc"), FDateTime::UtcNow().ToIso8601());
    const bool bSaved = WriteMaterialResourceReceipt(Report, Path);
    UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_MATERIAL_RESOURCES complete valid=%d checked=%d saved=%d"), bAllValid, Rows.Num(), bSaved);
    return bAllValid && bSaved;
}
}
#endif

UAftershockEnvironmentAuditCommandlet::UAftershockEnvironmentAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}
int32 UAftershockEnvironmentAuditCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
 if(FParse::Param(*Params,TEXT("MassSupportPreparation")))
  return RunAftershockMassSupportPreparationFixtures(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/mass_support_preparation.json")))?0:1;
 if(FParse::Param(*Params,TEXT("HydraulicGroundInstall")))
  return RunAftershockHydraulicGroundInstallFixtures(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/hydraulic_ground_install.json")))?0:1;
 if(FParse::Param(*Params,TEXT("PhysicalLifecycle")))
  return RunAftershockPhysicalLifecycleFixtures(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/physical_lifecycle_clock.json")))?0:1;
 if(FParse::Param(*Params,TEXT("MaterialStockExtension")))
  return RunAftershockMaterialExtensionFixtures(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/material_stock_extension.json")))?0:1;
 if(FParse::Param(*Params,TEXT("PhysicalHazards")))
  return RunAftershockPhysicalHazardFixtures(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/physical_hazards.json")))?0:1;
 if(FParse::Param(*Params,TEXT("PhysicalReplay")))
  return RunAftershockPhysicalReplayFixtures(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/physical_replay_codec.json")))?0:1;
 if(FParse::Param(*Params,TEXT("HydraulicTerrain")))
  return RunAftershockHydraulicTerrainFixtures(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/hydraulic_terrain.json")))?0:1;
 if(FParse::Param(*Params,TEXT("HydrologyAsyncStepper")))return RunHydrologyAsyncStepperAudit()?0:1;
 if(FParse::Param(*Params,TEXT("HydraulicObstructions")))return RunHydraulicObstructionAudit()?0:1;
 if(FParse::Param(*Params,TEXT("HydrologyNumerics")))return RunHydraulicNumericalAudit()?0:1;
 if(FParse::Param(*Params,TEXT("MassSparseFields")))
  return RunAftershockMassSparseFieldFixtures(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/mass_sparse_fields.json")))?0:1;
 if(FParse::Param(*Params,TEXT("MassMovement")))
  return RunAftershockMassMovementFixtures(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/mass_movement_numerics.json")))?0:1;
 if(FParse::Param(*Params,TEXT("RepairNativeFacadeTangents")))return RepairAftershockNativeFacadeTangents(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/citysample_facade_tangent_repair.json")))?0:1;
 if(FParse::Param(*Params,TEXT("CitySampleFacadeFrames")))return ReadNativeCitySampleFacadeFrames()?0:1;
 if(FParse::Param(*Params,TEXT("MaterialResources")))
  return ReadCurrentMaterialResources()?0:1;
 if(FParse::Param(*Params,TEXT("RepairBroadleafTangents")))
  return RepairAftershockBroadleafExplicitTangents(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/environment_explicit_tangent_repair.json")))?0:1;
 return ReadAftershockEnvironmentRenderFrames(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/environment_render_frames.json")))?0:1;
#else
 return 1;
#endif
}
