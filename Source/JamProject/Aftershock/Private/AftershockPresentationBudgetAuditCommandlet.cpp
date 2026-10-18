#include "AftershockPresentationBudgetAuditCommandlet.h"

#if WITH_EDITOR
#include "AftershockPresentationBudget.h"
#include "AftershockTerrainSurface.h"
#include "AftershockGlobeSurface.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/CityPresentationSubsystem.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameters.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "StaticMeshResources.h"
#include "UObject/GarbageCollection.h"
#include <limits>

namespace
{
	struct FRenderBudgetAuditWorld
	{
		UWorld* World=nullptr;
		~FRenderBudgetAuditWorld(){if(World){World->DestroyWorld(false);if(GEngine)GEngine->DestroyWorldContext(World);World->RemoveFromRoot();}}
	};
}

UAftershockPresentationBudgetAuditCommandlet::UAftershockPresentationBudgetAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}

int32 UAftershockPresentationBudgetAuditCommandlet::Main(const FString&)
{
	int32 Passed=0,Failed=0;TArray<FString> Checks;
	const auto Check=[&](const TCHAR* Name,bool Good)
	{
		Good?++Passed:++Failed;Checks.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"),Name,Good?TEXT("true"):TEXT("false")));
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PRESENTATION_BUDGET_CHECK %s passed=%d"),Name,Good);
	};
	const auto Finish=[&]()
	{
		const FString Folder=FPaths::ProjectSavedDir()/TEXT("PresentationValidation");IFileManager::Get().MakeDirectory(*Folder,true);
		const FString Json=FString::Printf(TEXT("{\"schema_version\":1,\"scope\":\"Pure render-budget thresholds and isolated native actual mesh LOD/collision, no-op geometry cache, incremental HISM and weak-cache garbage collection fixtures. No GPU timing, measured RAM saving, real gameplay, geography or crowd safety claim.\",\"passed\":%d,\"failed\":%d,\"checks\":[%s]}"),Passed,Failed,*FString::Join(Checks,TEXT(",")));
		if(!FFileHelper::SaveStringToFile(Json,*(Folder/TEXT("presentation_budget_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return 2;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PRESENTATION_BUDGET_RESULT passed=%d failed=%d"),Passed,Failed);return Failed?1:0;
	};
	const auto Street=MakeAftershockPresentationBudget(15999.f,0.f),City=MakeAftershockPresentationBudget(16000.f,0.f);
	const auto Regional=MakeAftershockPresentationBudget(100000.f,.199f),Globe=MakeAftershockPresentationBudget(100000.f,.2f);
	Check(TEXT("pure_street_city_threshold_preserves_near_people_and_details"),Street.Tier==EAftershockPresentationTier::Street&&City.Tier==EAftershockPresentationTier::City&&Street.bDrawCitizens&&City.bDrawCitizens&&City.bDrawVehicleDetails);
	Check(TEXT("pure_regional_globe_reduces_draw_only_details"),Regional.Tier==EAftershockPresentationTier::Regional&&Globe.Tier==EAftershockPresentationTier::Globe&&!Regional.bDrawCitizens&&!Globe.bDrawDecorations&&Globe.GeometryStride==8);
	Check(TEXT("invalid_numeric_budget_is_safe_full_street_fallback"),MakeAftershockPresentationBudget(std::numeric_limits<float>::quiet_NaN(),0.f).Tier==EAftershockPresentationTier::Street&&MakeAftershockPresentationBudget(-1.f,0.f).bDrawDecorations);
	Check(TEXT("no_world_budget_is_full_street"),GetAftershockPresentationBudget(nullptr).GeometryStride==1);
	TArray<double> RealAngles,LegacyAngles;GetAftershockGlobeLatitudeAngles(true,RealAngles);GetAftershockGlobeLatitudeAngles(false,LegacyAngles);
	bool AnalyticRings=RealAngles.Num()==74,SortedAngles=true,LegacyBands=LegacyAngles.Num()==97;
	constexpr double EarthRadiusMetres=6371000.;
	for(double ArcKm:{1.,2.,4.,8.,16.,32.,64.,128.,256.})
		AnalyticRings&=RealAngles.ContainsByPredicate([ArcKm](double Theta){return FMath::Abs(Theta-ArcKm*1000./6371000.)<1.e-12;});
	for(int32 I=1;I<RealAngles.Num();++I)SortedAngles&=FMath::IsFinite(RealAngles[I])&&RealAngles[I]>RealAngles[I-1];
	for(int32 I=0;I<LegacyAngles.Num();++I)LegacyBands&=LegacyAngles[I]==static_cast<double>(PI*I/96);
	Check(TEXT("real_earth_adds_nine_sorted_analytic_near_cap_rings"),AnalyticRings&&SortedAngles&&RealAngles[0]==0.&&RealAngles.Last()==UE_DOUBLE_PI);
	Check(TEXT("legacy_globe_preserves_all96_float_latitude_bands"),LegacyBands);
	double MaximumNearCapSeaErrorMetres=0.;int32 CheckedCapFaces=0;
	const auto UnitSphere=[](double Theta,double Phi){return FVector(FMath::Sin(Theta)*FMath::Cos(Phi),FMath::Sin(Theta)*FMath::Sin(Phi),FMath::Cos(Theta));};
	for(int32 I=1;I<RealAngles.Num()&&RealAngles[I]<=8000./EarthRadiusMetres+1.e-12;++I)
	{
		const FVector A=UnitSphere(RealAngles[I-1],0.),B=UnitSphere(RealAngles[I-1],2.*UE_DOUBLE_PI/128.);
		const FVector C=UnitSphere(RealAngles[I],0.),D=UnitSphere(RealAngles[I],2.*UE_DOUBLE_PI/128.);
		const auto FaceError=[&](const FVector& P,const FVector& Q,const FVector& R)
		{
			const FVector Cross=FVector::CrossProduct(Q-P,R-P);const double Size=Cross.Size();if(Size<1.e-20)return;
			MaximumNearCapSeaErrorMetres=FMath::Max(MaximumNearCapSeaErrorMetres,EarthRadiusMetres*(1.-FMath::Abs(FVector::DotProduct(Cross/Size,P))));++CheckedCapFaces;
		};
		FaceError(A,B,C);FaceError(B,D,C);
	}
	Check(TEXT("analytic_sphere_faces_within8km_cap_have_below1metre_sea_level_error"),CheckedCapFaces>=7&&MaximumNearCapSeaErrorMetres<1.);
	Check(TEXT("refined_earth_topology_stays_below20000_triangles"),(RealAngles.Num()-1)*128*2<20000);
	Check(TEXT("engine_initialized"),GEngine!=nullptr);if(!GEngine)return Finish();
	const auto* PlanetMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Environment/Materials/Presentation/M_AftershockPlanetSurface.M_AftershockPlanetSurface"));
	float DefaultRegionalClip=-1.f;FLinearColor SourceMin,SourceMax;
	Check(TEXT("actual_planet_material_is_masked_with_legacy_safe_clip_default0"),PlanetMaterial&&PlanetMaterial->GetBlendMode()==BLEND_Masked
		&&PlanetMaterial->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(TEXT("RegionalContextClipEnabled"))),DefaultRegionalClip)&&DefaultRegionalClip==0.f);
	Check(TEXT("actual_planet_material_exposes_real_source_world_xy_clip_bounds"),PlanetMaterial
		&&PlanetMaterial->GetVectorParameterValue(FHashedMaterialParameterInfo(FName(TEXT("RegionalContextBoundsMin"))),SourceMin)
		&&PlanetMaterial->GetVectorParameterValue(FHashedMaterialParameterInfo(FName(TEXT("RegionalContextBoundsMax"))),SourceMax));
	FRenderBudgetAuditWorld Fixture;
	Fixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("PresentationBudgetFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("isolated_fixture_world"),Fixture.World!=nullptr);if(!Fixture.World)return Finish();
	UWorld* World=Fixture.World;GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();auto* Buildings=World->GetSubsystem<UBuildingManagerSubsystem>();
	Check(TEXT("canonical_owners_available"),Terrain&&Buildings);if(!Terrain||!Buildings)return Finish();
	Terrain->GenerateProceduralTerrain(12,12,500.f,1337);Terrain->GridOrigin=FVector::ZeroVector;
	for(int32 Y=0;Y<12;++Y)for(int32 X=0;X<12;++X)
	{
		FGridCell Cell;Cell.Coords=FIntPoint(X,Y);Cell.bIsPlayable=true;Cell.Elevation=50.f;Cell.bHasRoad=Y==3;
		Terrain->Cells[Terrain->CoordToIndex(Cell.Coords)]=Cell;
	}
	const FIntRect SparseCoverageBounds(0,0,14,14);
	Check(TEXT("context_coverage_does_not_exclude_missing_resident_cells_inside_rectangle"),
		!IsAftershockInstalledGroundCoverage(*Terrain,SparseCoverageBounds,FIntPoint(12,5)));
	const int32 CoverageProbe=Terrain->CoordToIndex(FIntPoint(6,6));
	Terrain->Cells[CoverageProbe].bIsPlayable=false;
	Check(TEXT("context_coverage_keeps_unrendered_hole_between_installed_ground"),
		IsAftershockInstalledGroundCoverage(*Terrain,SparseCoverageBounds,FIntPoint(5,6))
		&&!IsAftershockInstalledGroundCoverage(*Terrain,SparseCoverageBounds,FIntPoint(6,6))
		&&IsAftershockInstalledGroundCoverage(*Terrain,SparseCoverageBounds,FIntPoint(7,6)));
	Terrain->Cells[CoverageProbe].bIsPlayable=true;
	Check(TEXT("context_coverage_respects_actual_installed_draw_window"),
		!IsAftershockInstalledGroundCoverage(*Terrain,FIntRect(0,0,5,5),FIntPoint(6,6)));
	AActor* Owner=World->SpawnActor<AActor>();
	auto* Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	Check(TEXT("isolated_host_and_material"),Owner&&Material);if(!Owner||!Material)return Finish();
	const auto MakePair=[&](AActor* Host,UStaticMeshComponent*& Ground,UStaticMeshComponent*& Water)
	{
		Ground=NewObject<UStaticMeshComponent>(Host);Water=NewObject<UStaticMeshComponent>(Host);
		Host->AddInstanceComponent(Ground);Host->AddInstanceComponent(Water);Host->SetRootComponent(Ground);Water->SetupAttachment(Ground);
		Ground->SetMaterial(0,Material);Water->SetMaterial(0,Material);Ground->RegisterComponent();Water->RegisterComponent();
	};
	UStaticMeshComponent *Ground=nullptr,*Water=nullptr;MakePair(Owner,Ground,Water);
	const auto CellsBefore=Terrain->Cells;
	Check(TEXT("actual_ground_build_succeeds"),BuildAftershockTerrainSurface(Owner,Terrain,Ground,Water));
	const UStaticMesh* ExactMesh=Ground->GetStaticMesh();
	const auto* Data=ExactMesh?ExactMesh->GetRenderData():nullptr;
	bool CoarseValid=Data&&Data->LODResources.Num()==4;
	if(CoarseValid)for(int32 I=1;I<4;++I)CoarseValid&=Data->LODResources[I].GetNumTriangles()>0&&Data->LODResources[I].GetNumTriangles()<=Data->LODResources[I-1].GetNumTriangles();
	Check(TEXT("real_four_render_lods_keep_exact_lod0_collision"),CoarseValid&&ExactMesh->LODForCollision==0);
	Ground->SetForcedLodModel(4);FHitResult Hit;const FVector Centre=Terrain->GridToWorld(FIntPoint(6,6));
	Check(TEXT("coarsest_render_selection_keeps_exact_ground_query"),Ground->LineTraceComponent(Hit,Centre+FVector(0,0,2000),Centre-FVector(0,0,2000),FCollisionQueryParams(SCENE_QUERY_STAT(RenderBudgetAudit),true))&&FMath::Abs(Hit.ImpactPoint.Z-Centre.Z)<.5f);
	bool Unchanged=CellsBefore.Num()==Terrain->Cells.Num();for(int32 I=0;I<CellsBefore.Num();++I)Unchanged&=FGridCell::StaticStruct()->CompareScriptStruct(&CellsBefore[I],&Terrain->Cells[I],0);
	Check(TEXT("render_build_does_not_write_canonical_cells"),Unchanged);
	const auto Before=GetAftershockTerrainSurfaceBuildStats(World);
	FGridCell Occupied=Terrain->GetCell(FIntPoint(7,7));Occupied.OccupyingBuildingID=31415;Occupied.bHasRoad=true;Terrain->SetCell(Occupied.Coords,Occupied);
	Check(TEXT("occupancy_and_road_change_reuses_real_mesh"),BuildAftershockTerrainSurface(Owner,Terrain,Ground,Water)&&Ground->GetStaticMesh()==ExactMesh);
	const auto After=GetAftershockTerrainSurfaceBuildStats(World);
	Check(TEXT("unrelated_edit_has_zero_actual_ground_build_and_cook_delta"),After.SuccessfulGroundBuilds==Before.SuccessfulGroundBuilds&&After.CollisionCookAttempts==Before.CollisionCookAttempts&&After.GroundCacheHits==Before.GroundCacheHits+1);
	TWeakObjectPtr<UStaticMesh> OldMesh(Ground->GetStaticMesh());Occupied.Elevation+=100.f;Terrain->SetCell(Occupied.Coords,Occupied);
	Check(TEXT("actual_elevation_change_rebuilds_ground"),BuildAftershockTerrainSurface(Owner,Terrain,Ground,Water)&&Ground->GetStaticMesh()!=OldMesh.Get());
	const auto Edited=GetAftershockTerrainSurfaceBuildStats(World);
	Check(TEXT("elevation_edit_has_one_ground_build_and_collision_cook"),Edited.SuccessfulGroundBuilds==After.SuccessfulGroundBuilds+1&&Edited.CollisionCookAttempts==After.CollisionCookAttempts+1);
	TWeakObjectPtr<UStaticMesh> ActiveMesh(Ground->GetStaticMesh());CollectGarbage(RF_NoFlags,true);
	Check(TEXT("weak_cache_does_not_retain_replaced_mesh_after_gc"),!OldMesh.IsValid());
	Check(TEXT("active_component_keeps_current_ground_alive_after_gc"),ActiveMesh.IsValid()&&Ground->GetStaticMesh()==ActiveMesh.Get());
	AAftershockCityPresentationActor* CityVisual=World->SpawnActor<AAftershockCityPresentationActor>();
	Check(TEXT("city_hism_host_created"),CityVisual!=nullptr);
	if(CityVisual)
	{
		CityVisual->RebuildCosmeticDistrict(*Terrain,*Buildings);const int32 Roads=CityVisual->GetRenderedRoadCount();
		CityVisual->RebuildCosmeticDistrict(*Terrain,*Buildings);
		Check(TEXT("same_cosmetic_input_does_not_upload_or_rebuild_any_hism"),CityVisual->GetLastUpdatedBatchCount()==0&&CityVisual->GetLastChangedInstanceCount()==0&&CityVisual->GetRenderedRoadCount()==Roads);
		Check(TEXT("legacy_fixture_has_no_geographic_globe_markers"),CityVisual->GetGeographicFocusMarkerCount()==0);
	}
	// Synthetic V2 geometry regression: inspect the actual built water positions
	// against LOD0 ground collision, including points between canonical centres.
	// No real geodata or gameplay state is inferred from this isolated fixture.
	const auto SavedCells=Terrain->Cells;const int32 SavedVersion=Terrain->ContinuationGeneratorVersion;
	Terrain->ContinuationGeneratorVersion=2;Owner->Tags.AddUnique(TEXT("Aftershock_MeshCpuAudit"));
	const auto RiverClearance=[&](const TArray<float>& Heights,bool Gap,bool NegativeMSL)
	{
		for(auto& Cell:Terrain->Cells){Cell.Elevation=Heights[0];Cell.bIsRiverbed=false;Cell.bIsPlayable=true;}
		for(int32 I=0;I<Heights.Num();++I)
		{
			const FIntPoint Coord(4+I*(Gap?2:1),6);auto& Cell=Terrain->Cells[Terrain->CoordToIndex(Coord)];
			Cell.Elevation=Heights[I];Cell.bIsRiverbed=true;
		}
		if(!BuildAftershockTerrainSurface(Owner,Terrain,Ground,Water))return false;
		const UStaticMesh* WaterMesh=Water->GetStaticMesh();const FStaticMeshRenderData* WaterData=WaterMesh?WaterMesh->GetRenderData():nullptr;
		if(!WaterData||WaterData->LODResources.IsEmpty())return false;
		const auto& WaterLOD=WaterData->LODResources[0];const auto WaterTriangles=WaterLOD.IndexBuffer.GetArrayView();
		bool Valid=WaterLOD.GetNumTriangles()==Heights.Num()*8;bool HasNegativeHeight=false;int32 Probes=0;
		for(int32 I=0;I<WaterTriangles.Num();I+=3)
		{
			const FVector A(WaterLOD.VertexBuffers.PositionVertexBuffer.VertexPosition(WaterTriangles[I]));
			const FVector B(WaterLOD.VertexBuffers.PositionVertexBuffer.VertexPosition(WaterTriangles[I+1]));
			const FVector C(WaterLOD.VertexBuffers.PositionVertexBuffer.VertexPosition(WaterTriangles[I+2]));
			const FVector Centre=(A+B+C)/3.;FGridCell Cell;
			Valid&=Terrain->TryGetCell(Terrain->WorldToGrid(Centre),Cell)&&Cell.bIsPlayable&&Cell.bIsRiverbed;
			for(const FVector& Point:{A,B,C,Centre,A*.2+B*.3+C*.5})
			{
				FHitResult SurfaceHit;
				const bool HitGround=Ground->LineTraceComponent(SurfaceHit,Point+FVector(0,0,10000),Point-FVector(0,0,10000),
					FCollisionQueryParams(SCENE_QUERY_STAT(RiverWaterClearanceAudit),true));
				Valid&=HitGround&&FMath::IsNearlyEqual(Point.Z-SurfaceHit.ImpactPoint.Z,6.,.15);
				HasNegativeHeight|=Point.Z<0.;++Probes;
			}
		}
		return Valid&&Probes>0&&(!NegativeMSL||HasNegativeHeight);
	};
	Check(TEXT("v2_river_water_clears_actual_ground_on_two_cell_rising_bank"),RiverClearance({100.f,200.f},false,false));
	Check(TEXT("v2_river_water_clears_three_cell_interior_ridge"),RiverClearance({10.f,30.f,10.f},false,false));
	Check(TEXT("v2_river_water_keeps_negative_msl_without_flattening"),RiverClearance({-300.f,-200.f},false,true));
	Check(TEXT("v2_river_water_never_spans_the_dry_cell_between_runs"),RiverClearance({10.f,30.f},true,false));
	Terrain->Cells=SavedCells;Terrain->ContinuationGeneratorVersion=SavedVersion;Owner->Tags.Remove(TEXT("Aftershock_MeshCpuAudit"));
	// Empty valid display windows exercise the cache bound cheaply; these are
	// isolated fixture mutations, never generated gameplay or a terrain authority.
	for(auto& Cell:Terrain->Cells){Cell.bIsPlayable=false;Cell.bIsRiverbed=false;}
	for(int32 I=0;I<65;++I)
	{
		AActor* Host=World->SpawnActor<AActor>();UStaticMeshComponent *G=nullptr,*W=nullptr;MakePair(Host,G,W);BuildAftershockTerrainSurface(Host,Terrain,G,W);
	}
	Check(TEXT("ground_cache_is_bounded_to64_live_component_entries"),GetAftershockTerrainSurfaceBuildStats(World).CachedGroundComponents<=64);
	return Finish();
}
#else
UAftershockPresentationBudgetAuditCommandlet::UAftershockPresentationBudgetAuditCommandlet(){}
int32 UAftershockPresentationBudgetAuditCommandlet::Main(const FString&){return 1;}
#endif
