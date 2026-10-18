// AFTERSHOCK — actual Voronoi cuts through public Engine editor APIs.
#include "AftershockFractureSetupCommandlet.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/GeometryCollectionClusteringUtility.h"
#include "GeometryCollection/GeometryCollectionConvexUtility.h"
#include "GeometryCollection/GeometryCollectionEngineConversion.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PlanarCut.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "Voronoi/Voronoi.h"

namespace AftershockFractureAuthoring
{
	struct FBuildingSpec
	{
		const TCHAR* Name;
		int32 Seed;
	};
	const FBuildingSpec Specs[] = {
		{ TEXT("House"), 1337 }, { TEXT("Apartment"), 1434 }, { TEXT("Office"), 1531 },
		{ TEXT("Factory"), 1628 }, { TEXT("Hospital"), 1725 }
	};

	int32 CountRigidLeaves(const UGeometryCollection* Asset)
	{
		const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Collection = Asset ? Asset->GetGeometryCollection() : nullptr;
		if (!Collection) return 0;
		int32 Count = 0;
		for (int32 Index = 0; Index < Collection->SimulationType.Num(); ++Index)
		{
			const int32 Geo = Collection->TransformToGeometryIndex[Index];
			if (Collection->SimulationType[Index] == FGeometryCollection::FST_Rigid && Collection->Children[Index].IsEmpty()
				&& Geo >= 0 && Geo < Collection->FaceCount.Num() && Collection->FaceCount[Geo] > 0)
			{
				++Count;
			}
		}
		return Count;
	}

	bool SaveAssetPackage(UObject* Asset)
	{
		UPackage* Package = Asset->GetOutermost();
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Asset, *Filename, Args);
	}

	bool EnsureGeometryCollectionMaterial(UMaterialInterface* Interface, TSet<UMaterial*>& ChangedMaterials)
	{
		if (!Interface) return false;
		UMaterial* Material = Interface->GetMaterial();
		if (!Material) return false;
		if (!Material->GetUsageByFlag(MATUSAGE_GeometryCollections))
		{
			// Do not alter a shared Engine material. Source buildings must use project art.
			if (!Material->GetOutermost()->GetName().StartsWith(TEXT("/Game/"))) return false;
			Material->Modify();
			if (!Material->SetMaterialUsage(MATUSAGE_GeometryCollections)) return false;
			ChangedMaterials.Add(Material);
		}
		return true;
	}

	bool ValidateCollection(const UGeometryCollection* Asset, const TCHAR* SourceMeshPath)
	{
		const int32 Count = CountRigidLeaves(Asset);
		if (!Asset || Count < 20 || Count > 40 || !Asset->HasVisibleGeometry()) return false;
		bool bHasSource = false;
		for (const FGeometryCollectionSource& Source : Asset->GeometrySource)
		{
			bHasSource |= Source.SourceGeometryObject.ToString() == SourceMeshPath;
		}
		if (!bHasSource) return false;
		const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Collection = Asset->GetGeometryCollection();
		for (int32 MaterialID : Collection->MaterialID)
		{
			if (MaterialID < 0 || MaterialID >= Asset->Materials.Num() || !Asset->Materials[MaterialID]) return false;
		}
		return true;
	}

	bool GenerateBuilding(const FBuildingSpec& Spec, UMaterialInterface* InteriorMaterial,
		TSet<UMaterial*>& ChangedMaterials, FString& OutReport)
	{
		const FString MeshName = FString::Printf(TEXT("SM_%s_01"), Spec.Name);
		const FString MeshPath = FString::Printf(TEXT("/Game/Buildings/Meshes/%s.%s"), *MeshName, *MeshName);
		const FString AssetName = FString::Printf(TEXT("GC_%s_01"), Spec.Name);
		const FString PackageName = TEXT("/Game/Buildings/Fractured/") + AssetName;
		const FString AssetPath = PackageName + TEXT(".") + AssetName;
		if (UGeometryCollection* Existing = LoadObject<UGeometryCollection>(nullptr, *AssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			if (!ValidateCollection(Existing, *MeshPath))
			{
				UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_GC_INVALID_EXISTING path=%s rigidLeaves=%d; authoring stops to preserve an existing asset"), *AssetPath, CountRigidLeaves(Existing));
				return false;
			}
			OutReport += FString::Printf(TEXT("%s,%s,%d,reused,0,0\n"), *AssetPath, *MeshPath, CountRigidLeaves(Existing));
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_GC_VALIDATED path=%s rigidLeaves=%d reused=1"), *AssetPath, CountRigidLeaves(Existing));
			return true;
		}
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
		if (!Mesh)
		{
			UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_GC_SOURCE_MISSING path=%s"), *MeshPath);
			return false;
		}
		FAssetCompilingManager::Get().FinishAllCompilation();
		TArray<UMaterialInterface*> Materials;
		TArray<TObjectPtr<UMaterialInterface>> SourceMaterials;
		for (const FStaticMaterial& StaticMaterial : Mesh->GetStaticMaterials())
		{
			if (!EnsureGeometryCollectionMaterial(StaticMaterial.MaterialInterface, ChangedMaterials))
			{
				UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_GC_SOURCE_MATERIAL_INVALID mesh=%s"), *MeshPath);
				return false;
			}
			Materials.Add(StaticMaterial.MaterialInterface);
			SourceMaterials.Add(StaticMaterial.MaterialInterface);
		}
		if (Materials.IsEmpty()) return false;
		const FBox Bounds = Mesh->GetBoundingBox();
		if (!Bounds.IsValid || Bounds.GetVolume() <= 0) return false;

		// Actual cut count can be smaller than the site count on concave/open art.
		// Each attempt starts from fresh source geometry; never repeatedly shatter fragments.
		for (int32 Attempt = 0; Attempt < 8; ++Attempt)
		{
			const int32 SiteCount = FMath::Min(40, 28 + Attempt * 4);
			const int32 Seed = Spec.Seed + Attempt * 7919;
			TStrongObjectPtr<UGeometryCollection> Candidate(NewObject<UGeometryCollection>(GetTransientPackage()));
			Candidate->bManualDataCreate = true;
			if (!FGeometryCollectionEngineConversion::AppendStaticMesh(Mesh, Materials, FTransform::Identity, Candidate.Get(), true, false, false, false)) return false;
			const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> Collection = Candidate->GetGeometryCollection();
			TArray<int32> SourceTransforms;
			for (int32 Index = 0; Index < Collection->SimulationType.Num(); ++Index)
			{
				if (Collection->SimulationType[Index] == FGeometryCollection::FST_Rigid && Collection->TransformToGeometryIndex[Index] >= 0) SourceTransforms.Add(Index);
			}
			if (SourceTransforms.IsEmpty()) return false;
			const int32 InteriorID = Candidate->Materials.Add(InteriorMaterial);
			FRandomStream Random(Seed);
			TArray<FVector> Sites;
			Sites.Reserve(SiteCount);
			for (int32 Index = 0; Index < SiteCount; ++Index)
			{
				Sites.Add(Bounds.Min + FVector(Random.FRand(), Random.FRand(), Random.FRand()) * Bounds.GetSize());
			}
			FVoronoiDiagram Diagram(Sites, Bounds, 0.1);
			FPlanarCells Cells(Sites, Diagram);
			Cells.InternalSurfaceMaterials.GlobalMaterialID = InteriorID;
			Cells.InternalSurfaceMaterials.GlobalUVScale = 0.01f;
			// No noise subdivision, no per-face collision samples, no island explosion.
			const int32 FirstNewGeometry = CutMultipleWithPlanarCells(Cells, *Collection, SourceTransforms,
				0.0, 0.0, Seed, TOptional<FTransform>(), true, false, nullptr, FVector::ZeroVector, FIslandSplitSettings(false));
			const int32 RigidLeaves = CountRigidLeaves(Candidate.Get());
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_GC_CUT_ATTEMPT building=%s attempt=%d seed=%d sites=%d rigidLeaves=%d firstNewGeometry=%d"),
				Spec.Name, Attempt + 1, Seed, SiteCount, RigidLeaves, FirstNewGeometry);
			if (FirstNewGeometry == INDEX_NONE || RigidLeaves < 20 || RigidLeaves > 40) continue;
			Candidate->GeometrySource.Add(FGeometryCollectionSource(FSoftObjectPath(MeshPath), FTransform::Identity, SourceMaterials, false, false));
			// This asset is instantiated only AFTER canonical gameplay declares collapse.
			// Independent leaves start as rubble immediately; no second damage threshold.
			Candidate->EnableClustering = false;
			Candidate->DamageThreshold = { 100.f };
			Candidate->EnableNanite = false;
			Candidate->bImportCollisionFromSource = false;
			Candidate->bRemoveOnMaxSleep = false; // Gameplay freezes rubble after four seconds.
			Candidate->bScaleOnRemoval = false;
			Candidate->SizeSpecificData.SetNum(1);
			Candidate->SizeSpecificData[0].CollisionShapes.SetNum(1);
			FGeometryCollectionCollisionTypeData& Collision = Candidate->SizeSpecificData[0].CollisionShapes[0];
			Collision.CollisionType = ECollisionTypeEnum::Chaos_Volumetric;
			Collision.ImplicitType = EImplicitTypeEnum::Chaos_Implicit_Convex;
			Collision.CollisionMarginFraction = 0.02f;
			// The interactive editor adds GUID metadata after cuts. Supply that
			// metadata here as well, with stable values for a reproducible build.
			const FName GuidAttribute(TEXT("GUID"));
			if (!Collection->HasAttribute(GuidAttribute, FGeometryCollection::TransformGroup))
			{
				Collection->AddAttribute<FGuid>(GuidAttribute, FGeometryCollection::TransformGroup);
			}
			TManagedArray<FGuid>& Guids = Collection->ModifyAttribute<FGuid>(GuidAttribute, FGeometryCollection::TransformGroup);
			for (int32 Index = 0; Index < Guids.Num(); ++Index)
			{
				Guids[Index] = FGuid(0xAF7E2501, static_cast<uint32>(Spec.Seed), static_cast<uint32>(Seed), static_cast<uint32>(Index + 1));
			}
			FGeometryCollectionClusteringUtility::UpdateHierarchyLevelOfChildren(Collection.Get(), INDEX_NONE);
			Collection->ReindexMaterials();
			Candidate->InitializeMaterials(false);
			GeometryCollectionAlgo::PrepareForSimulation(Collection.Get(), false);
			FGeometryCollectionConvexUtility::CreateNonOverlappingConvexHullData(Collection.Get(), 0.3, 2.0, 0.5, EConvexOverlapRemoval::OnlyClusters);
			if (!ValidateCollection(Candidate.Get(), *MeshPath)) return false;
			UPackage* Package = CreatePackage(*PackageName);
			if (!Candidate->Rename(*AssetName, Package, REN_DontCreateRedirectors | REN_NonTransactional)) return false;
			Candidate->ClearFlags(RF_Transient);
			Candidate->SetFlags(RF_Public | RF_Standalone);
			Candidate->InvalidateCollection();
			Candidate->CreateSimulationData();
			Candidate->RebuildRenderData();
			Candidate->bManualDataCreate = false;
			FAssetRegistryModule::AssetCreated(Candidate.Get());
			if (!SaveAssetPackage(Candidate.Get())) return false;
			OutReport += FString::Printf(TEXT("%s,%s,%d,created,%d,%d\n"), *AssetPath, *MeshPath, RigidLeaves, Seed, SiteCount);
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_GC_VALIDATED path=%s rigidLeaves=%d sites=%d seed=%d materials=%d visibleGeometry=%d renderData=%d collision=Convex clustering=0"),
				*AssetPath, RigidLeaves, SiteCount, Seed, Candidate->Materials.Num(), Candidate->HasVisibleGeometry(), Candidate->HasMeshData());
			return true;
		}
		UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_GC_FRACTURE_FAILED mesh=%s could not produce 20-40 rigid leaves in eight deterministic attempts"), *MeshPath);
		return false;
	}
}
#endif

UAftershockFractureSetupCommandlet::UAftershockFractureSetupCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UAftershockFractureSetupCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	using namespace AftershockFractureAuthoring;
	TSet<UMaterial*> ChangedMaterials;
	UMaterialInterface* Interior = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Environment/Materials/Presentation/M_PresentationRock.M_PresentationRock"));
	if (!EnsureGeometryCollectionMaterial(Interior, ChangedMaterials))
	{
		UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_GC_INTERIOR_MATERIAL_MISSING create /Game/Environment/Materials/Presentation/M_PresentationRock first"));
		return 1;
	}
	FString Report(TEXT("geometry_collection,source_mesh,rigid_leaves,status,seed,voronoi_sites\n"));
	int32 Completed = 0;
	for (const FBuildingSpec& Spec : Specs)
	{
		if (GenerateBuilding(Spec, Interior, ChangedMaterials, Report)) ++Completed;
	}
	FAssetCompilingManager::Get().FinishAllCompilation();
	bool bMaterialsSaved = true;
	for (UMaterial* Material : ChangedMaterials) bMaterialsSaved &= SaveAssetPackage(Material);
	const FString ReportPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PackageValidation/ChaosVoronoiAssetReport.csv"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true);
	const bool bReportSaved = FFileHelper::SaveStringToFile(Report, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_GC_SETUP_RESULT validated=%d required=5 materialsSaved=%d reportSaved=%d report=%s"), Completed, bMaterialsSaved, bReportSaved, *ReportPath);
	return Completed == UE_ARRAY_COUNT(Specs) && bMaterialsSaved && bReportSaved ? 0 : 1;
#else
	UE_LOG(LogTemp, Error, TEXT("AftershockFractureSetup is an offline editor commandlet."));
	return 1;
#endif
}
