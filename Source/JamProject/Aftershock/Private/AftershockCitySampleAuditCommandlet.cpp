#include "AftershockCitySampleAuditCommandlet.h"
#if WITH_EDITOR
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshResources.h"
#endif
UAftershockCitySampleAuditCommandlet::UAftershockCitySampleAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;ShowErrorCount=true;}
int32 UAftershockCitySampleAuditCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	int32 Passed=0,Failed=0;TArray<TSharedPtr<FJsonValue>> Rows;
	const auto Check=[&](const FString& Name,bool Good)
	{Good?++Passed:++Failed;auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("name"),Name);Row->SetBoolField(TEXT("passed"),Good);Rows.Add(MakeShared<FJsonValueObject>(Row));UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITYSAMPLE_CHECK %s passed=%d"),*Name,Good);};
	for(const TCHAR* Name:{TEXT("T_Brick_C"),TEXT("T_Brick_N"),TEXT("T_Brick_M"),TEXT("T_Concrete_C"),TEXT("T_Concrete_N"),TEXT("T_Bench_C"),TEXT("T_Bench_N"),TEXT("T_Bench_M"),TEXT("T_Lamp_C")})
	{
		const FString Path=FString::Printf(TEXT("/Game/Aftershock/CitySample/Textures/%s.%s"),Name,Name);
		auto* Texture=LoadObject<UTexture2D>(nullptr,*Path);Check(FString(Name)+TEXT("_native_load"),Texture!=nullptr);if(!Texture)continue;
		const bool Normal=FString(Name).EndsWith(TEXT("_N")),Colour=FString(Name).EndsWith(TEXT("_C"));
		Check(FString(Name)+TEXT("_streaming_runtime_cap"),!Texture->NeverStream&&!Texture->VirtualTextureStreaming&&Texture->MaxTextureSize==2048);
		Check(FString(Name)+TEXT("_correct_colour_and_compression"),Texture->SRGB==Colour&&(!Normal||Texture->CompressionSettings==TC_Normalmap));
	}
	for(const TCHAR* Name:{TEXT("SM_StreetBench"),TEXT("SM_StreetLamp")})
	{
		const FString Path=FString::Printf(TEXT("/Game/Aftershock/CitySample/Meshes/%s.%s"),Name,Name);
		auto* Mesh=LoadObject<UStaticMesh>(nullptr,*Path);Check(FString(Name)+TEXT("_native_load"),Mesh!=nullptr);if(!Mesh)continue;
		const FVector Size=Mesh->GetBoundingBox().GetSize();const bool Bench=FString(Name).Contains(TEXT("Bench"));
		Check(FString(Name)+TEXT("_human_scale_centimetres"),Bench?(Size.X>230&&Size.X<250&&Size.Z>90&&Size.Z<110):(Size.Z>750&&Size.Z<790));
		const auto* Data=Mesh->GetRenderData();const int32 Triangles=Data&&!Data->LODResources.IsEmpty()?Data->LODResources[0].GetNumTriangles():0;
		Check(FString(Name)+TEXT("_bounded_classic_render_fallback"),Triangles>0&&Triangles<=5000);
		bool Owned=!Mesh->GetStaticMaterials().IsEmpty();for(const auto& Slot:Mesh->GetStaticMaterials())Owned&=Slot.MaterialInterface&&Slot.MaterialInterface->GetPathName().StartsWith(TEXT("/Game/Aftershock/CitySample/Materials/"));
		Check(FString(Name)+TEXT("_all_material_slots_resolve_to_compact_export"),Owned);
	}
	for(const TCHAR* Path:{TEXT("/Game/Buildings/Materials/M_Building_Base.M_Building_Base"),TEXT("/Game/Buildings/Materials/M_ServiceBuilding.M_ServiceBuilding")})
	{
		auto* Material=LoadObject<UMaterialInterface>(nullptr,Path);float Strength=-1,Channel=-1,Enable=-1;
		Check(FString(Path)+TEXT("_safe_opt_in_and_native_roughness"),Material&&Material->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("CitySampleFacadeStrength")),Strength)&&Strength==0.f
			&&Material->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("CitySampleFacadePackedRoughnessChannel")),Channel)&&Channel==0.f
			&&Material->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("CitySampleFacadeUsePackedRoughness")),Enable)&&Enable==1.f);
	}
	auto Report=MakeShared<FJsonObject>();Report->SetNumberField(TEXT("passed"),Passed);Report->SetNumberField(TEXT("failed"),Failed);Report->SetArrayField(TEXT("checks"),Rows);
	Report->SetStringField(TEXT("scope"),TEXT("Actual native asset load/policy, physical mesh bounds and classic-render fallback; no whole-scene GPU, source downsample, cook or art acceptance claim."));
	FString Json;auto Writer=TJsonWriterFactory<>::Create(&Json);FJsonSerializer::Serialize(Report,Writer);
	const FString File=FPaths::ProjectSavedDir()/TEXT("PresentationValidation/citysample_asset_audit.json");IFileManager::Get().MakeDirectory(*FPaths::GetPath(File),true);
	if(!FFileHelper::SaveStringToFile(Json,*File,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return 2;return Failed?1:0;
#else
	return 1;
#endif
}
