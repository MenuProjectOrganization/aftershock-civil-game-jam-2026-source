#include "Aftershock/Public/CityPresentationSubsystem.h"
#include "AftershockEnvironmentSurfaces.h"
// AFTERSHOCK — CC0 Kenney palette props, batched over the canonical city grid.
#include "AftershockEnvironmentDressing.h"
#include "AftershockConformingRoads.h"

#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "AftershockGlobeSurface.h"
#include "AftershockTerrainSurface.h"
#include "AftershockRegionalSurfaceStyle.h"
#include "Aftershock/Public/DisasterPresentationSubsystem.h"
#include "AftershockPresentationBudget.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/CoastalPresentationSubsystem.h"
#include "Aftershock/Public/WorldClimateSubsystem.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameUserSettings.h"
#include "Camera/PlayerCameraManager.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	enum ECityBatch : int32
	{
		Straight, LitStraight, Corner, Junction, Crossroads, Pavement, Fountain,
		Woodland, TallWoodland, BridgeStructure, PremiumStreetLamp, PremiumParkBench, BatchCount
	};

	// Stable cardinal order is also the native road-mesh connection mask.
	const FIntPoint Cardinal[4] = { FIntPoint(1, 0), FIntPoint(0, 1), FIntPoint(-1, 0), FIntPoint(0, -1) };
	int32 GetCityFoliageQuality()
	{
		const auto* Settings=UGameUserSettings::GetGameUserSettings();
		return Settings?FMath::Clamp(Settings->GetFoliageQuality(),0,4):3;
	}
	bool GetRoadDetailView(const UWorld* World,FVector& Out)
	{
		const APlayerController* PC=World?World->GetFirstPlayerController():nullptr;
		if(!PC||!PC->PlayerCameraManager)return false;
		Out=PC->PlayerCameraManager->GetCameraLocation();return !Out.ContainsNaN();
	}

	FTransform TileTransform(UStaticMesh* Mesh, const FVector& Surface, float Width,
		float Yaw, float GrassBaseDepthFraction = 0.045f)
	{
		const FBox Bounds = Mesh->GetBoundingBox();
		const FVector Size = Bounds.GetSize();
		const float Scale = Width / FMath::Max(1.0f, FMath::Max(Size.X, Size.Y));
		// Imported glTF units and pivot vary. Normalize their plan footprint, keep
		// proportions, and sink the kit's grass/pavement slab into the real surface.
		FVector Position = Surface;
		Position.Z -= Bounds.Min.Z * Scale + Width * GrassBaseDepthFraction;
		const FVector CenterOffset = FRotator(0.f, Yaw, 0.f).RotateVector(FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y, 0.f) * Scale);
		Position -= CenterOffset;
		return FTransform(FRotator(0.f, Yaw, 0.f), Position, FVector(Scale));
	}

	void ChooseRoadTile(int32 Mask, bool bLamppost, int32& OutBatch, float& OutYaw)
	{
		const int32 Count = static_cast<int32>((Mask & 1) != 0) + static_cast<int32>((Mask & 2) != 0)
			+ static_cast<int32>((Mask & 4) != 0) + static_cast<int32>((Mask & 8) != 0);
		OutYaw = 0.f;
		if (Count == 4) { OutBatch = Crossroads; return; }
		if (Count == 3)
		{
			OutBatch = Junction;
			OutYaw = Mask == 14 ? 90.f : Mask == 13 ? 180.f : Mask == 11 ? 270.f : 0.f;
			return;
		}
		if (Count == 2 && Mask != 5 && Mask != 10)
		{
			OutBatch = Corner;
			// Native corner exits at -X/+Y (glTF -X/+Z), not +X/+Y.
			OutYaw = Mask == 6 ? 0.f : Mask == 12 ? 90.f : Mask == 9 ? 180.f : 270.f;
			return;
		}
		OutBatch = bLamppost ? LitStraight : Straight;
		// GLTFCore ConvertVec3 is {X,Z,Y}: the native lane runs along UE Y.
		OutYaw = (Mask & 5) != 0 ? 90.f : 0.f;
	}
}

AAftershockCityPresentationActor::AAftershockCityPresentationActor()
{
	PrimaryActorTick.bCanEverTick = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("CityPresentationRoot")));
	const auto MakeWorldMesh = [this](const TCHAR* Name, const TCHAR* MaterialPath, bool bShadow)
	{
		UStaticMeshComponent* Component = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Component->SetupAttachment(GetRootComponent());
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCanEverAffectNavigation(false);
		Component->SetCastShadow(bShadow);
		ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(MaterialPath);
		if (Material.Succeeded()) Component->SetMaterial(0, Material.Object);
		return Component;
	};
	WorldGlobe = MakeWorldMesh(TEXT("FictionalGlobe"), TEXT("/Game/Environment/Materials/Presentation/M_AftershockPlanetSurface.M_AftershockPlanetSurface"), true);
	WorldTransition = MakeWorldMesh(TEXT("CityToGlobeLandscape"), TEXT("/Game/Environment/Materials/Presentation/M_AftershockRegionalBlend.M_AftershockRegionalBlend"), true);
	WorldAtmosphere = MakeWorldMesh(TEXT("GlobeAtmosphere"), TEXT("/Game/Environment/Materials/Presentation/M_AftershockPlanetAtmosphere.M_AftershockPlanetAtmosphere"), false);
	WorldClouds = MakeWorldMesh(TEXT("ScenarioClimateClouds"), TEXT("/Game/Environment/Materials/Presentation/M_AftershockClimateClouds.M_AftershockClimateClouds"), false);
	WorldClouds->TranslucencySortPriority = 1;
	WorldAtmosphere->TranslucencySortPriority = 2;
	GeographicFocusMarkers=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("GeographicFocusMarkers"));
	GeographicFocusMarkers->SetupAttachment(GetRootComponent());GeographicFocusMarkers->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GeographicFocusMarkers->SetCanEverAffectNavigation(false);GeographicFocusMarkers->SetCastShadow(false);GeographicFocusMarkers->SetVisibility(false);
	// Constructor defaults must not query the runtime material quality. The
	// texture-residency setter does so even for false and asserts during cook.
	GeographicFocusMarkers->SetNumCustomDataFloats(3);GeographicFocusMarkers->bForceMipStreaming=false;
	ConstructorHelpers::FObjectFinder<UStaticMesh> MarkerSphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if(MarkerSphere.Succeeded())GeographicFocusMarkers->SetStaticMesh(MarkerSphere.Object);
	ConstructorHelpers::FObjectFinder<UMaterialInterface> MarkerMaterial(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if(MarkerMaterial.Succeeded())GeographicFocusMarkers->SetMaterial(0,MarkerMaterial.Object);
	MeshBatches.SetNum(BatchCount);
	CosmeticBatchSlots.SetNum(BatchCount);
	// Imported glTF/Substrate material instances rendered white on the target RHI.
	// Every source prop shares the same palette/UV0, so use a project-owned lit
	// atlas directly instead of depending on the importer's transient parent graph.
	ConstructorHelpers::FObjectFinder<UMaterialInterface> PaletteFinder(TEXT("/Game/Environment/Materials/Presentation/M_PresentationKenneyAtlas.M_PresentationKenneyAtlas"));
	UMaterialInterface* PaletteMaterial = PaletteFinder.Succeeded() ? PaletteFinder.Object : nullptr;
	auto MakeBatch = [this, PaletteMaterial](int32 Index, const TCHAR* Name, const TCHAR* AssetName, bool bShadow)
	{
		UHierarchicalInstancedStaticMeshComponent* Batch = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(Name);
		Batch->SetupAttachment(GetRootComponent());
		Batch->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Batch->SetCanEverAffectNavigation(false);
		Batch->SetCastShadow(bShadow);
		Batch->SetMobility(EComponentMobility::Movable);
		Batch->bAutoRebuildTreeOnInstanceChanges = false;
		Batch->SetRemoveSwap();
		// Road surface is the only copy of these canonical tiles. It must remain
		// visible across the1250m initial city when buildings remain visible.
		// Lightposts are selected only within450m; decorative props keep600m.
		Batch->SetCullDistances(0, Index<=Crossroads||Index==Pavement?0:60000);
		Batch->bForceMipStreaming=false;
		const FString MeshPath = FString::Printf(TEXT("/Game/Aftershock/KenneyMeshes/Imported/%s/StaticMeshes/%s.%s"), AssetName, AssetName, AssetName);
		ConstructorHelpers::FObjectFinder<UStaticMesh> Mesh(*MeshPath);
		if (Mesh.Succeeded()) Batch->SetStaticMesh(Mesh.Object);
		if (PaletteMaterial) Batch->SetMaterial(0, PaletteMaterial);
		MeshBatches[Index] = Batch;
	};
	MakeBatch(Straight, TEXT("RoadStraightHISM"), TEXT("road-straight"), false);
	MakeBatch(LitStraight, TEXT("RoadLightpostsHISM"), TEXT("road-straight-lightposts"), true);
	MakeBatch(Corner, TEXT("RoadCornerHISM"), TEXT("road-corner"), false);
	MakeBatch(Junction, TEXT("RoadJunctionHISM"), TEXT("road-split"), false);
	MakeBatch(Crossroads, TEXT("RoadCrossroadsHISM"), TEXT("road-intersection"), false);
	MakeBatch(Pavement, TEXT("PavementHISM"), TEXT("pavement"), false);
	MakeBatch(Fountain, TEXT("FountainHISM"), TEXT("pavement-fountain"), true);
	MakeBatch(Woodland, TEXT("WoodlandHISM"), TEXT("grass-trees"), true);
	MakeBatch(TallWoodland, TEXT("TallWoodlandHISM"), TEXT("grass-trees-tall"), true);
	// Native-centimetre City Sample derivatives. Two shared batches, no lights,
	// per-prop actors or ticks; original materials and source proportions remain.
	const auto MakePremiumBatch=[this](int32 Index,const TCHAR* Name,const TCHAR* Path)
	{
		auto* Batch=CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(Name);
		Batch->SetupAttachment(GetRootComponent());Batch->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Batch->SetCanEverAffectNavigation(false);Batch->SetMobility(EComponentMobility::Movable);
		Batch->bAutoRebuildTreeOnInstanceChanges=false;Batch->SetRemoveSwap();
		Batch->SetCullDistances(35000,50000);Batch->bForceMipStreaming=false;Batch->SetCastShadow(true);
		ConstructorHelpers::FObjectFinder<UStaticMesh> Mesh(Path);if(Mesh.Succeeded())Batch->SetStaticMesh(Mesh.Object);
		MeshBatches[Index]=Batch;
	};
	MakePremiumBatch(PremiumStreetLamp,TEXT("CitySampleStreetLampHISM"),TEXT("/Game/Aftershock/CitySample/Meshes/SM_StreetLamp.SM_StreetLamp"));
	MakePremiumBatch(PremiumParkBench,TEXT("CitySampleParkBenchHISM"),TEXT("/Game/Aftershock/CitySample/Meshes/SM_StreetBench.SM_StreetBench"));
	UHierarchicalInstancedStaticMeshComponent* Bridge = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("BridgeStructureHISM"));
	Bridge->SetupAttachment(GetRootComponent());
	Bridge->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Bridge->SetCanEverAffectNavigation(false);
	Bridge->SetCastShadow(true);
	Bridge->SetMobility(EComponentMobility::Movable);
	Bridge->SetCullDistances(0,60000);
	Bridge->bAutoRebuildTreeOnInstanceChanges = false;
	Bridge->bForceMipStreaming=false;
	ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (Cube.Succeeded()) Bridge->SetStaticMesh(Cube.Object);
	ConstructorHelpers::FObjectFinder<UMaterialInterface> BridgeMaterial(TEXT("/Game/Environment/Materials/Presentation/M_PresentationSidewalk.M_PresentationSidewalk"));
	if (BridgeMaterial.Succeeded()) Bridge->SetMaterial(0, BridgeMaterial.Object);
	MeshBatches[BridgeStructure] = Bridge;

	FallbackSun = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("FallbackSun"));
	FallbackSun->SetupAttachment(GetRootComponent());
	FallbackSun->SetMobility(EComponentMobility::Movable);
	FallbackSun->SetRelativeRotation(FRotator(-42.f, -32.f, 0.f));
	FallbackSun->SetIntensity(50000.f);
	FallbackSun->SetLightColor(FLinearColor(1.f, 0.94f, 0.84f));
	FallbackSun->SetCastShadows(true);
	FallbackSun->bAtmosphereSunLight = true;
	FallbackSun->SetVisibility(false);
	FallbackSky = CreateDefaultSubobject<USkyLightComponent>(TEXT("FallbackSky"));
	FallbackSky->SetupAttachment(GetRootComponent());
	FallbackSky->SetMobility(EComponentMobility::Movable);
	FallbackSky->SetIntensity(0.8f);
	FallbackSky->SetVisibility(false);
	FallbackAtmosphere = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("FallbackAtmosphere"));
	FallbackAtmosphere->SetupAttachment(GetRootComponent());
	FallbackAtmosphere->SetVisibility(false);
	FallbackFog = CreateDefaultSubobject<UExponentialHeightFogComponent>(TEXT("FallbackDistanceHaze"));
	FallbackFog->SetupAttachment(GetRootComponent());
	FallbackFog->SetFogDensity(0.0015f);
	FallbackFog->SetVisibility(false);
	PresentationGrade = CreateDefaultSubobject<UPostProcessComponent>(TEXT("PresentationGrade"));
	PresentationGrade->SetupAttachment(GetRootComponent());
	PresentationGrade->bUnbound = true;
	// An authored map's explicit postprocess can override the fallback grade.
	PresentationGrade->Priority = 50.f;
}

void AAftershockCityPresentationActor::BuildGlobeVisual(const UTerrainGridSubsystem& Terrain)
{
	const FIntRect Visible=GetAftershockDrawableCellBounds(Terrain);
	uint64 Signature=GetAftershockTerrainGeometrySignature(Terrain,Visible);
	Signature^=static_cast<uint64>(GetTypeHash(Terrain.GeographyIndex))<<32;
	Signature^=static_cast<uint64>(GetTypeHash(Terrain.GeographyFocusIndex))<<16;
	Signature^=GetTypeHash(Terrain.CoastalSeaLevelCm);
	const auto Budget=GetAftershockPresentationBudget(GetWorld());
	RebuildGeographicFocusMarkers(Terrain);
	Signature^=static_cast<uint64>(Budget.GeometryStride)*1099511628211ull;
	const bool NeedPlanet=!Terrain.IsUsingRealGeography()||Budget.Tier==EAftershockPresentationTier::Regional||Budget.Tier==EAftershockPresentationTier::Globe;
	const bool Cached=WorldTransition->GetStaticMesh()&&(!NeedPlanet||(WorldGlobe->GetStaticMesh()&&WorldClouds->GetStaticMesh()&&WorldAtmosphere->GetStaticMesh()))&&GlobeSignature==Signature;
	if(!Cached&&BuildAftershockRegionalGlobe(this,Terrain,WorldGlobe,WorldTransition,WorldAtmosphere,WorldClouds))GlobeSignature=Signature;
	// The analytic Earth remains complete in globe mode. Only its near-side
	// source rectangle is clipped while real regional DEM context is installed:
	// otherwise a sea-level sphere can cover negative-MSL cities such as New Orleans.
	UMaterialInstanceDynamic* PlanetMaterial=Cast<UMaterialInstanceDynamic>(WorldGlobe->GetMaterial(0));
	if(!PlanetMaterial)PlanetMaterial=WorldGlobe->CreateAndSetMaterialInstanceDynamic(0);
	if(PlanetMaterial)
	{
		FAftershockGeographicBounds SourceBounds;FVector NorthWest=FVector::ZeroVector,SouthEast=FVector::ZeroVector;
		const bool SourceReady=Terrain.IsUsingRealGeography()&&GlobeSignature==Signature&&WorldTransition->GetStaticMesh()
			&&WorldTransition->GetStaticMesh()->HasValidRenderData()
			&&(Terrain.ContinuationGeneratorVersion==3?GetAftershockRegionalContextBounds(Terrain.GeographyIndex,SourceBounds):Terrain.TryGetGeographicSourceBounds(SourceBounds))
			&&AftershockGeographicPresentationWorld(Terrain,SourceBounds.North,SourceBounds.West,NorthWest)
			&&AftershockGeographicPresentationWorld(Terrain,SourceBounds.South,SourceBounds.East,SouthEast);
		const bool Clip=SourceReady&&Budget.Tier==EAftershockPresentationTier::Regional;
		PlanetMaterial->SetScalarParameterValue(TEXT("RegionalContextClipEnabled"),Clip?1.f:0.f);
		if(SourceReady)
		{
			if(Terrain.ContinuationGeneratorVersion==3)
			{
				// Leave the sea-level planet under the closed overview perimeter.
				// The exact detail/construction source remains fully clipped, including
				// below-MSL cities. No DEM value or gameplay boundary is changed.
				FAftershockGeographicBounds Detail;FVector DNW,DSE;
				if(Terrain.TryGetGeographicSourceBounds(Detail)
					&&AftershockGeographicPresentationWorld(Terrain,Detail.North,Detail.West,DNW)
					&&AftershockGeographicPresentationWorld(Terrain,Detail.South,Detail.East,DSE))
				{
					const double InsetX=(SouthEast.X-NorthWest.X)/128.,InsetY=(SouthEast.Y-NorthWest.Y)/128.;
					NorthWest.X+=FMath::Min(InsetX,FMath::Max(0.,(DNW.X-NorthWest.X)*.5));
					NorthWest.Y+=FMath::Min(InsetY,FMath::Max(0.,(DNW.Y-NorthWest.Y)*.5));
					SouthEast.X-=FMath::Min(InsetX,FMath::Max(0.,(SouthEast.X-DSE.X)*.5));
					SouthEast.Y-=FMath::Min(InsetY,FMath::Max(0.,(SouthEast.Y-DSE.Y)*.5));
				}
			}
			PlanetMaterial->SetVectorParameterValue(TEXT("RegionalContextBoundsMin"),FLinearColor(NorthWest.X,NorthWest.Y,0.f,0.f));
			PlanetMaterial->SetVectorParameterValue(TEXT("RegionalContextBoundsMax"),FLinearColor(SouthEast.X,SouthEast.Y,0.f,0.f));
		}
	}
}

int32 AAftershockCityPresentationActor::GetGeographicFocusMarkerCount() const
{return GeographicFocusMarkers?GeographicFocusMarkers->GetInstanceCount():0;}

void AAftershockCityPresentationActor::RebuildGeographicFocusMarkers(const UTerrainGridSubsystem& Terrain)
{
	if(!GeographicFocusMarkers)return;
	const bool Draw=Terrain.IsUsingRealGeography()&&GetAftershockPresentationBudget(GetWorld()).Tier==EAftershockPresentationTier::Globe;
	GeographicFocusMarkers->SetVisibility(Draw);
	if(!Draw||!GeographicFocusMarkers->GetStaticMesh())return;
	const auto Layout=GetAftershockGlobeLayout(Terrain);
	uint64 Signature=(static_cast<uint64>(GetTypeHash(Layout.Center))<<32)^GetTypeHash(Layout.RadiusCm);
	Signature^=static_cast<uint64>(GetTypeHash(Terrain.GeographyIndex))<<16;Signature^=GetTypeHash(Terrain.GeographyFocusIndex);
	TArray<FTransform> Transforms;TArray<FLinearColor> Colours;
	const float NativeRadius=FMath::Max(1.f,GeographicFocusMarkers->GetStaticMesh()->GetBoundingBox().GetExtent().GetMax());
	for(int32 Region=0;Region<GetAftershockGeographyCount();++Region)for(int32 Focus=0;Focus<GetAftershockGeographyFocusCount(Region);++Focus)
	{
		const auto* Geo=TryGetAftershockGeographyFocus(Region,Focus);if(!Geo||!IsAftershockFocusGeodataReady(Region,Focus))continue;
		Signature=(Signature^static_cast<uint64>((Region+1)*65536+Focus+1))*1099511628211ull;
		const FVector N=AftershockLatLonToFocusGlobeNormal(Geo->Latitude,Geo->Longitude,Terrain.GeographyIndex,Terrain.GeographyFocusIndex);
		Transforms.Add(FTransform(FQuat::Identity,Layout.Center+N*(Layout.RadiusCm*1.001),FVector(Layout.RadiusCm*.006f/NativeRadius)));
		Colours.Add(Region==Terrain.GeographyIndex&&Focus==Terrain.GeographyFocusIndex?FLinearColor(1.f,.68f,.15f):FLinearColor(.92f,.94f,.91f));
	}
	if(Signature==GeographicMarkerSignature&&GeographicFocusMarkers->GetInstanceCount()==Transforms.Num())return;
	GeographicFocusMarkers->ClearInstances();GeographicFocusMarkers->AddInstances(Transforms,false,true);
	for(int32 I=0;I<Colours.Num();++I)
	{
		GeographicFocusMarkers->SetCustomDataValue(I,0,Colours[I].R,false);GeographicFocusMarkers->SetCustomDataValue(I,1,Colours[I].G,false);GeographicFocusMarkers->SetCustomDataValue(I,2,Colours[I].B,false);
	}
	if(!Cast<UMaterialInstanceDynamic>(GeographicFocusMarkers->GetMaterial(0)))
		if(auto* Material=GeographicFocusMarkers->CreateAndSetMaterialInstanceDynamic(0))Material->SetVectorParameterValue(TEXT("Color"),FLinearColor(1.f,.68f,.15f));
	GeographicFocusMarkers->MarkRenderStateDirty();GeographicMarkerSignature=Signature;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_GEOGRAPHIC_FOCUS_MARKERS count=%d oneISM=1 radiusFraction=.006 altitudeFraction=1.001 globeOnly=1 customActiveColourData=1 basicMaterialPerInstanceColourNotAsserted=1"),Transforms.Num());
}

void AAftershockCityPresentationActor::UpdateWorldClimate(const UTerrainGridSubsystem& Terrain, const FAftershockWorldClimateState& Climate)
{
	if (GlobeMaterials.IsEmpty())
	{
		for (UStaticMeshComponent* Component : { WorldGlobe.Get(), WorldTransition.Get(), WorldClouds.Get(), WorldAtmosphere.Get() })
		{
			UMaterialInstanceDynamic* Material=Cast<UMaterialInstanceDynamic>(Component->GetMaterial(0));
			GlobeMaterials.Add(Material?Material:Component->CreateAndSetMaterialInstanceDynamic(0));
		}
	}
	ApplyAftershockRegionalSurfaceStyle(Terrain,WorldTransition,0);
	if(Terrain.ContinuationGeneratorVersion==3)ApplyAftershockRegionalSurfaceStyle(Terrain,WorldTransition,1);
	ApplyAftershockGlobalRegionalColour(Terrain,WorldGlobe,0);
	ApplyAftershockGlobalRegionalColour(Terrain,WorldTransition,0);
	if(WorldTransition->GetNumMaterials()>1)ApplyAftershockGlobalRegionalColour(Terrain,WorldTransition,1);
	if(UWorld* World=GetWorld())for(TActorIterator<AAftershockCrowdHostActor> It(World);It;++It)
	{
		ApplyAftershockRegionalSurfaceStyle(Terrain,It->TerrainSurface);
		if(Terrain.ContinuationGeneratorVersion==3)
		{
			auto* ContextMID=Cast<UMaterialInstanceDynamic>(WorldTransition->GetMaterial(0));
			auto* GroundMID=Cast<UMaterialInstanceDynamic>(It->TerrainSurface->GetMaterial(0));
			if(ContextMID&&GroundMID&&ContextMID->K2_GetScalarParameterValue(TEXT("RegionalSourceMaskEnabled"))>.5f)
			{
				UTexture* SourceMask=ContextMID->K2_GetTextureParameterValue(TEXT("RegionalSourceMask"));
				if(SourceMask&&GroundMID->K2_GetTextureParameterValue(TEXT("RegionalSourceMask"))!=SourceMask)
				{
					GroundMID->SetTextureParameterValue(TEXT("RegionalSourceMask"),SourceMask);
					GroundMID->SetVectorParameterValue(TEXT("RegionalSourceBoundsMin"),ContextMID->K2_GetVectorParameterValue(TEXT("RegionalSourceBoundsMin")));
					GroundMID->SetVectorParameterValue(TEXT("RegionalSourceBoundsMax"),ContextMID->K2_GetVectorParameterValue(TEXT("RegionalSourceBoundsMax")));
					GroundMID->SetScalarParameterValue(TEXT("RegionalSourceMaskEnabled"),1.f);
				}
			}
		}
		ApplyAftershockGlobalRegionalColour(Terrain,It->TerrainSurface,0);
	}
	if(auto* OverviewMID=Cast<UMaterialInstanceDynamic>(WorldTransition->GetMaterial(1)))
		if(!GlobeMaterials.Contains(OverviewMID))GlobeMaterials.Add(OverviewMID);
	const FAftershockGlobeLayout Layout = GetAftershockGlobeLayout(Terrain);
	for (UMaterialInstanceDynamic* Material : GlobeMaterials)
	{
		if (!Material) continue;
		Material->SetVectorParameterValue(TEXT("PlanetCenter"), FLinearColor(Layout.Center.X, Layout.Center.Y, Layout.Center.Z, 1.f));
		Material->SetScalarParameterValue(TEXT("PlanetRadius"), Layout.RadiusCm);
		Material->SetScalarParameterValue(TEXT("UseRealGeographicScale"), Terrain.IsUsingRealGeography()?1.f:0.f);
		Material->SetScalarParameterValue(TEXT("WorldSeed"), Terrain.TerrainSeed * .001f);
		Material->SetScalarParameterValue(TEXT("ScenarioAnomalyC"), Climate.GlobalTemperatureAnomalyC);
		Material->SetScalarParameterValue(TEXT("DroughtPressure"), Climate.DroughtVisualPressure);
		Material->SetScalarParameterValue(TEXT("IceCoverage"), Climate.IceCoverageFraction);
		Material->SetScalarParameterValue(TEXT("StormPressure"), Climate.StormVisualPressure);
		Material->SetScalarParameterValue(TEXT("LocalHeatPressure"), Climate.LocalUrbanHeatPressure);
	}
}

void AAftershockCityPresentationActor::ConfigurePresentationLighting()
{
	bool bHasSun = false, bHasSky = false, bHasAtmosphere = false, bHasFog = false;
	float SunIntensity = FallbackSun->Intensity;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (*It == this) continue;
		TInlineComponentArray<UActorComponent*> Components(*It);
		for (UActorComponent* Component : Components)
		{
			if (UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(Component))
			{
				if (Light->IsVisible() && Light->Intensity > 0.01f)
				{
					if (!bHasSun) SunIntensity = Light->Intensity;
					bHasSun = true;
				}
			}
			if (const USkyLightComponent* Sky = Cast<USkyLightComponent>(Component)) bHasSky |= Sky->IsVisible() && Sky->Intensity > 0.01f;
			if (const USkyAtmosphereComponent* Sky = Cast<USkyAtmosphereComponent>(Component)) bHasAtmosphere |= Sky->IsVisible();
			if (const UExponentialHeightFogComponent* Fog = Cast<UExponentialHeightFogComponent>(Component)) bHasFog |= Fog->IsVisible();
		}
	}
	FallbackSun->SetVisibility(!bHasSun);
	FallbackSky->SetVisibility(!bHasSky);
	FallbackAtmosphere->SetVisibility(!bHasAtmosphere);
	FallbackFog->SetVisibility(!bHasFog);
	FPostProcessSettings& Grade = PresentationGrade->Settings;
	Grade.bOverride_AutoExposureMethod = true;
	Grade.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
	Grade.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	Grade.AutoExposureApplyPhysicalCameraExposure = false;
	Grade.bOverride_AutoExposureBias = true;
	// Adapt the fixed grade to authored low-lux maps as well as physical sun lux.
	Grade.AutoExposureBias = -FMath::Log2(FMath::Max(0.1f, SunIntensity) / 3.2f);
	Grade.bOverride_BloomIntensity = true;
	Grade.BloomIntensity = 0.12f;
	Grade.bOverride_AmbientOcclusionIntensity = true;
	Grade.AmbientOcclusionIntensity = 0.7f;
	Grade.bOverride_AmbientOcclusionRadius = true;
	Grade.AmbientOcclusionRadius = 110.f;
	Grade.bOverride_ColorSaturation = true;
	Grade.ColorSaturation = FVector4(1.02f, 1.02f, 1.02f, 1.f);
	Grade.bOverride_VignetteIntensity = true;
	Grade.VignetteIntensity = 0.12f;
	Grade.bOverride_MotionBlurAmount = true;
	Grade.MotionBlurAmount = 0.f;
	Grade.bOverride_SceneFringeIntensity = true;
	Grade.SceneFringeIntensity = 0.f;
	if (!bHasSky) FallbackSky->RecaptureSky();
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_PRESENTATION_LIGHTING fallbackSun=%d fallbackSky=%d fallbackAtmosphere=%d fallbackFog=%d sunLux=%.2f fixedExposureBias=%.2f"),
		!bHasSun, !bHasSky, !bHasAtmosphere, !bHasFog, SunIntensity, Grade.AutoExposureBias);
	CaptureHeightFogForOverview();
}

void AAftershockCityPresentationActor::CaptureHeightFogForOverview()
{
	UWorld* World = GetWorld();
	if (!World || bCapturedHeightFog) return;
	bCapturedHeightFog = true;
	// Capture once after fallback visibility has been resolved. Hidden map fog and
	// SkyAtmosphere retain their authored state throughout the camera transition.
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		TInlineComponentArray<UExponentialHeightFogComponent*> Components(*It);
		for (UExponentialHeightFogComponent* Fog : Components)
		{
			if (!Fog->IsVisible()) continue;
			FAftershockSavedHeightFog& Saved = SavedHeightFog.AddDefaulted_GetRef();
			Saved.Component = Fog;
			Saved.Density = Fog->FogDensity;
			Saved.SecondDensity = Fog->SecondFogData.FogDensity;
			Saved.bVisible = Fog->IsVisible();
		}
	}
	World->GetTimerManager().SetTimer(FogTransitionTimer, this, &AAftershockCityPresentationActor::UpdateOverviewFog, 0.1f, true);
	UpdateOverviewFog();
}

void AAftershockCityPresentationActor::UpdateOverviewFog()
{
	UWorld* World = GetWorld();
	if (!World) return;
	if (!FogCamera.IsValid())
	{
		APlayerController* Controller = World->GetFirstPlayerController();
		FogCamera = Controller ? Cast<AAftershockCameraPawn>(Controller->GetPawn()) : nullptr;
	}
	const float Blend = FogCamera.IsValid() ? FMath::Clamp(FogCamera->GetPlanetOverviewBlend(), 0.f, 1.f) : 0.f;
	ApplyPresentationBudget();
	float RegionalFogBlend=0.f;
	if(const auto* Grid=World->GetSubsystem<UTerrainGridSubsystem>();Grid&&Grid->ContinuationGeneratorVersion==3)
	{
		// A 40..250km real regional surface is still below the globe transition.
		// Local street fog must not make that whole map optically opaque. Fade
		// only the captured height-fog densities, restoring them on the way back;
		// sky atmosphere and weather simulation retain their own authority.
		RegionalFogBlend=FMath::SmoothStep(500000.f,5000000.f,GetAftershockPresentationBudget(World).ViewDistanceCm);
	}
	const float Scale = FMath::Pow(1.f - Blend, 4.f)*(1.f-RegionalFogBlend);
	// Exact endpoints matter: even a tiny remaining height-fog density is amplified
	// far below the authored map's Z=0 fog layer on the globe's lower hemisphere.
	const bool bExactEndpointChanged = (Scale == 0.f || Scale == 1.f) && Scale != LastFogScale;
	if (bExactEndpointChanged || !FMath::IsNearlyEqual(Scale, LastFogScale, 0.00001f))
	{
		for (const FAftershockSavedHeightFog& Saved : SavedHeightFog)
		{
			if (UExponentialHeightFogComponent* Fog = Saved.Component.Get())
			{
				Fog->SetFogDensity(Saved.Density * Scale);
				// Only second-layer density changes. Height offset/falloff and all
				// other SecondFogData fields remain exactly as authored.
				Fog->SetSecondFogDensity(Saved.SecondDensity * Scale);
			}
		}
		LastFogScale = Scale;
	}
	const int32 Region = (Blend <= 0.001f ? 0 : Blend >= 0.999f ? 2 : 1)+(RegionalFogBlend>=1.f?6:RegionalFogBlend>0.f?3:0);
	if (Region != LastFogTransitionRegion)
	{
		LastFogTransitionRegion = Region;
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_GLOBE_FOG overviewBlend=%.4f regionalFogBlend=%.4f densityScale=%.6f managedFogLayers=%d originalVisibilityPreserved=1 skyAtmosphereUnchanged=1"),
			Blend,RegionalFogBlend, Scale, SavedHeightFog.Num());
	}
}

void AAftershockCityPresentationActor::RestoreHeightFog()
{
	for (const FAftershockSavedHeightFog& Saved : SavedHeightFog)
	{
		if (UExponentialHeightFogComponent* Fog = Saved.Component.Get())
		{
			Fog->SetFogDensity(Saved.Density);
			Fog->SetSecondFogDensity(Saved.SecondDensity);
			Fog->SetVisibility(Saved.bVisible);
		}
	}
	SavedHeightFog.Reset();
	bCapturedHeightFog = false;
	LastFogScale = -1.f;
}

void AAftershockCityPresentationActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld()) World->GetTimerManager().ClearTimer(FogTransitionTimer);
	RestoreHeightFog();
	EnvironmentDressing.Reset();
	ConformingRoads.Reset();
	Super::EndPlay(EndPlayReason);
}

bool AAftershockCityPresentationActor::RefreshEnvironmentDressing(const UTerrainGridSubsystem& Terrain)
{
	if(!EnvironmentDressing.IsValid())EnvironmentDressing=MakeShared<FAftershockEnvironmentDressing>(*this);
	return EnvironmentDressing->Refresh(Terrain);
}

void AAftershockCityPresentationActor::ApplyPresentationBudget()
{
	ApplyAftershockEnvironmentSurfaceQuality(GetWorld());
	if(EnvironmentDressing.IsValid())EnvironmentDressing->ApplyPresentationBudget();
	if(ConformingRoads.IsValid())ConformingRoads->ApplyPresentationBudget();
	const auto Budget=GetAftershockPresentationBudget(GetWorld());
	const int32 Tier=static_cast<int32>(Budget.Tier);
	const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const bool RealEarth=Terrain&&Terrain->IsUsingRealGeography();
	const int32 DetailToken=Tier*2+(RealEarth?1:0);
	if(DetailToken==LastPresentationTier)return;
	LastPresentationTier=DetailToken;
	for(int32 Index=0;Index<MeshBatches.Num();++Index)
	{
		auto* Batch=MeshBatches[Index].Get();if(!Batch)continue;
		const bool Essential=Index<=Crossroads||Index==Pavement||Index==BridgeStructure;
		Batch->SetVisibility(Essential||Budget.bDrawDecorations);
		// Only the true globe view loses tiny local road triangles; the regional
		// and city skeleton stays visible. Canonical/picking data is untouched.
		const bool Premium=Index==PremiumStreetLamp||Index==PremiumParkBench;
		Batch->SetCullDistances(Premium?35000:0,Essential?(Budget.Tier==EAftershockPresentationTier::Globe?250000:0):Premium?50000:60000);
		const int32 MaxLOD=Batch->GetStaticMesh()?Batch->GetStaticMesh()->GetNumLODs():1;
		const int32 LOD=Budget.PropLODFloor?FMath::Clamp(Budget.PropLODFloor,1,FMath::Max(1,MaxLOD)):0;
		if(Batch->GetForcedLodModel()!=LOD)Batch->SetForcedLodModel(LOD);
		Batch->SetCastShadow((Index==LitStraight||Index==Fountain||Index==Woodland||Index==TallWoodland||Index==BridgeStructure||Premium)
			&&Budget.Tier==EAftershockPresentationTier::Street);
		Batch->StreamingDistanceMultiplier=Budget.Tier==EAftershockPresentationTier::Street?1.f:Budget.Tier==EAftershockPresentationTier::City?.7f:.4f;
		Batch->MarkRenderStateDirty();
	}
	if(Terrain)
	{
		const bool Overview=Budget.Tier==EAftershockPresentationTier::Regional||Budget.Tier==EAftershockPresentationTier::Globe;
		WorldGlobe->SetVisibility(!RealEarth||Overview);
		WorldGlobe->SetCastShadow(!RealEarth);
		WorldTransition->SetVisibility(!RealEarth||Budget.Tier!=EAftershockPresentationTier::Globe);
		WorldTransition->SetCastShadow(!RealEarth);
		WorldAtmosphere->SetVisibility(!RealEarth||Budget.Tier==EAftershockPresentationTier::Globe);
		WorldClouds->SetVisibility(!RealEarth||Budget.Tier==EAftershockPresentationTier::Globe);
		BuildGlobeVisual(*Terrain);
	}
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITY_RENDER_TIER tier=%d propLODFloor=%d decorations=%d essentialRoadsPreserved=1 geometryStride=%d viewDistanceCm=%.1f"),Tier,Budget.PropLODFloor,Budget.bDrawDecorations,Budget.GeometryStride,Budget.ViewDistanceCm);
}

void AAftershockCityPresentationActor::RebuildCosmeticDistrict(const UTerrainGridSubsystem& Terrain, const UBuildingManagerSubsystem& Buildings)
{
	const bool bEnvironmentChanged=RefreshEnvironmentDressing(Terrain);
    // The installed ground already owns CPU-readable exact render triangles.
    // Build every matching LOD once per geometry/topology/window change.
    UStaticMeshComponent* InstalledGround=nullptr;
    for(TActorIterator<AAftershockCrowdHostActor> It(GetWorld());It;++It){InstalledGround=It->TerrainSurface;break;}
    if(!ConformingRoads.IsValid())ConformingRoads=MakeShared<FAftershockConformingRoads>(*this);
    const bool bConformingReady=ConformingRoads->Refresh(Terrain,InstalledGround);
    if(!bConformingReady)UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_CONFORMING_ROADS_NOT_READY legacyFallback=1 noConformanceClaim=1"));
	const bool bDetailedEnvironment=EnvironmentDressing.IsValid()&&EnvironmentDressing->ShouldReplaceLegacyWoodland();
	struct FDesiredInstance { FIntVector Key; FTransform Transform; };
	TArray<TArray<FDesiredInstance>> Desired;Desired.SetNum(BatchCount);
	const auto AddDesired=[&](int32 Batch,FIntPoint Coord,int32 Piece,const FTransform& Transform)
	{
		Desired[Batch].Add({FIntVector(Coord.X,Coord.Y,Piece),Transform});
	};
	TArray<FBuildingInstance> Records;
	Buildings.GetAllBuildings(Records);
	Records.Sort([](const FBuildingInstance& A, const FBuildingInstance& B) { return A.InstanceID < B.InstanceID; });
	const FIntRect Visible=GetAftershockDrawableCellBounds(Terrain);
	const FIntRect Initial(0,0,Terrain.GridWidth,Terrain.GridHeight);
	const float DensityByQuality[]={.25f,.5f,.75f,1.f,1.f};
	const float FoliageDensity=DensityByQuality[GetCityFoliageQuality()];
	FVector RoadDetailView;const bool bHasRoadDetailView=GetRoadDetailView(GetWorld(),RoadDetailView);
	RenderedInitialRoadCount=0;CanonicalInitialRoadCount=0;ExpectedVisibleInitialRoadCount=0;RenderedRoadCount=0;RenderedCellBounds=Visible;
	Records.RemoveAll([&Visible](const FBuildingInstance& B){return !Visible.Contains(B.GridCoords);});
	// A player may keep/build roads before any building exists or after demolition.
	// Rendering those canonical records must not depend on building population.
	int32 MinX = Records.IsEmpty() ? Visible.Min.X : Visible.Max.X - 1;
	int32 MaxX = Records.IsEmpty() ? Visible.Max.X - 1 : Visible.Min.X;
	int32 MinY = Records.IsEmpty() ? Visible.Min.Y : Visible.Max.Y - 1;
	int32 MaxY = Records.IsEmpty() ? Visible.Max.Y - 1 : Visible.Min.Y;
	for (const FBuildingInstance& Building : Records)
	{
		MinX = FMath::Min(MinX, Building.GridCoords.X); MaxX = FMath::Max(MaxX, Building.GridCoords.X);
		MinY = FMath::Min(MinY, Building.GridCoords.Y); MaxY = FMath::Max(MaxY, Building.GridCoords.Y);
	}
	TSet<FIntPoint> RoadCells;
	const URoadNetworkSubsystem* Roads=GetWorld()->GetSubsystem<URoadNetworkSubsystem>();
	for(int32 Y=0;Y<Terrain.GridHeight;++Y)for(int32 X=0;X<Terrain.GridWidth;++X)
	{
		FGridCell Cell;if(Terrain.TryGetCell(FIntPoint(X,Y),Cell)&&Cell.bIsPlayable&&Cell.bHasRoad&&(!Cell.bIsRiverbed||Cell.BridgeID!=INDEX_NONE)&&Cell.OccupyingBuildingID==-1)++CanonicalInitialRoadCount;
	}
	for(int32 Y=Visible.Min.Y-1;Y<Visible.Max.Y+1;++Y)for(int32 X=Visible.Min.X-1;X<Visible.Max.X+1;++X)
	{
		FGridCell Cell;if(!Terrain.TryGetCell(FIntPoint(X,Y),Cell))continue;
		if (Cell.bIsPlayable && Cell.bHasRoad && (!Cell.bIsRiverbed || Cell.BridgeID!=INDEX_NONE) && Cell.OccupyingBuildingID == -1) RoadCells.Add(Cell.Coords);
	}
	int32 ConnectedLots = 0;
	for (const FBuildingInstance& Building : Records)
	{
		for (const FIntPoint& Direction : Cardinal)
		{
			if (RoadCells.Contains(Building.GridCoords + Direction) && (!Roads || Roads->CanAccessRoadFromCell(Building.GridCoords,Building.GridCoords+Direction))) { ++ConnectedLots; break; }
		}
	}

	int32 RoadInstances = 0, WoodlandInstances = 0, PlazaInstances = 0, BridgePieces=0;
	TArray<FIntPoint> SortedRoads = RoadCells.Array();
	int32 PremiumLampCount=0,PremiumBenchCount=0;
	const bool HasPremiumLamp=MeshBatches[PremiumStreetLamp]->GetStaticMesh()!=nullptr;
	SortedRoads.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.Y == B.Y ? A.X < B.X : A.Y < B.Y; });
	for (const FIntPoint& Coord : SortedRoads)
	{
		const bool bInitialRoad=Initial.Contains(Coord);
		if(!Visible.Contains(Coord))continue;
		ExpectedVisibleInitialRoadCount+=bInitialRoad?1:0;
		int32 Mask = 0;
		const FGridCell Cell=Terrain.GetCell(Coord);
		for (int32 Direction = 0; Direction < 4; ++Direction)
		{
			const FIntPoint Neighbor=Coord+Cardinal[Direction];
			if (!RoadCells.Contains(Neighbor)) continue;
			const FGridCell Other=Terrain.GetCell(Neighbor);
			// Connectivity is structural even when water closes an existing bridge.
			if ((Cell.BridgeID!=INDEX_NONE && !Cell.bBridgeEndpoint && ((Cell.BridgeAxis==1 && Cardinal[Direction].Y!=0) || (Cell.BridgeAxis==2 && Cardinal[Direction].X!=0)))
				|| (Other.BridgeID!=INDEX_NONE && !Other.bBridgeEndpoint && ((Other.BridgeAxis==1 && Cardinal[Direction].Y!=0) || (Other.BridgeAxis==2 && Cardinal[Direction].X!=0)))) continue;
			Mask|=1<<Direction;
		}
		int32 BatchIndex; float Yaw;
		const bool bNearForLightpost=!bHasRoadDetailView||FVector::DistSquared(RoadDetailView,Terrain.GridToWorld(Coord))<FMath::Square(45000.f);
		ChooseRoadTile(Mask, !HasPremiumLamp&&(Coord.X + Coord.Y) % 6 == 0&&bNearForLightpost, BatchIndex, Yaw);
		UHierarchicalInstancedStaticMeshComponent* Batch = MeshBatches[BatchIndex];
		if (!Batch->GetStaticMesh()) continue;
		FVector Surface = Roads ? Roads->RoadGridToWorld(Coord) : Terrain.GridToWorld(Coord)+FVector(0,0,8);
		FVector RoadNormal = FVector::UpVector;
		{
			auto HeightAt = [&Terrain, &Surface,Roads,&Cell](FIntPoint Neighbor)
			{
				if (Cell.BridgeID!=INDEX_NONE)
					return Terrain.IsValidCoord(Neighbor) && Terrain.GetCell(Neighbor).BridgeID==Cell.BridgeID && Roads ? Roads->GetRoadSurfaceElevation(Neighbor) : static_cast<float>(Surface.Z);
				return Terrain.IsValidCoord(Neighbor) && !Terrain.GetCell(Neighbor).bIsRiverbed
					? Terrain.GetCell(Neighbor).Elevation+8.f : static_cast<float>(Surface.Z);
			};
			const float GX = (HeightAt(Coord + FIntPoint(1, 0)) - HeightAt(Coord - FIntPoint(1, 0))) / (2.f * Terrain.CellSize);
			const float GY = (HeightAt(Coord + FIntPoint(0, 1)) - HeightAt(Coord - FIntPoint(0, 1))) / (2.f * Terrain.CellSize);
			RoadNormal = FVector(-GX, -GY, 1.f).GetSafeNormal();
		}
		// The upward-facing asphalt/lane vertices are at native Z=0, while
		// sidewalk tops are at Z=0.05. Never sink this tile under the terrain.
		FTransform RoadTransform = TileTransform(Batch->GetStaticMesh(), Surface, Terrain.CellSize, Yaw, 0.f);
		const FVector Heading = FRotator(0.f, Yaw, 0.f).Vector();
		RoadTransform.SetRotation(FRotationMatrix::MakeFromZX(RoadNormal, Heading).ToQuat());
		// Bridge asphalt preserves the authored deck/profile. Land asphalt is
        // emitted by one conforming batch, never by a second rigid surface.
        if(Cell.BridgeID!=INDEX_NONE||!bConformingReady||!ConformingRoads->HasRoadTile(Coord))
            AddDesired(BatchIndex,Coord,0,RoadTransform);
		const uint32 PropSeed=HashCombine(GetTypeHash(Terrain.TerrainSeed),GetTypeHash(Coord))^0xCE17u;
		const bool NearPremium=!bHasRoadDetailView||FVector::DistSquaredXY(RoadDetailView,Surface)<FMath::Square(35000.f);
		if(HasPremiumLamp&&PremiumLampCount<96&&Cell.BridgeID==INDEX_NONE&&(Mask==5||Mask==10)&&NearPremium&&PropSeed%9==0)
		{
			const FVector Across=Mask==5?FVector::RightVector:FVector::ForwardVector;
			const float Side=(PropSeed&16)?1.f:-1.f;FVector Position=Surface+Across*(Terrain.CellSize*.42f*Side);
			const float NZ=FMath::Max(.1f,static_cast<float>(RoadNormal.Z));
			Position.Z+=-(Position.X-Surface.X)*RoadNormal.X/NZ-(Position.Y-Surface.Y)*RoadNormal.Y/NZ+Terrain.CellSize*.05f;
			const FVector Inward=Across*(-Side);const float LampYaw=Inward.Rotation().Yaw-90.f;
			AddDesired(PremiumStreetLamp,Coord,0,FTransform(FRotator(0,LampYaw,0),Position,FVector::OneVector));++PremiumLampCount;
		}
		++RoadInstances;
		++RenderedRoadCount;
		RenderedInitialRoadCount+=bInitialRoad?1:0;
		if (Cell.BridgeID!=INDEX_NONE && MeshBatches[BridgeStructure]->GetStaticMesh())
		{
			// Original concrete deck, two barriers and bounded piers share one HISM.
			// The rendered surface and vehicle/pedestrian navigation use the same profile.
			const FVector Along=Cell.BridgeAxis==1 ? FVector::ForwardVector : FVector::RightVector;
			const FVector Across=Cell.BridgeAxis==1 ? FVector::RightVector : FVector::ForwardVector;
			const FQuat Rotation=FRotationMatrix::MakeFromZX(RoadNormal,Along).ToQuat();
			int32 Piece=0;
			const auto AddStructure=[&](FVector Position,FVector Dimensions,FQuat Orientation)
			{
				AddDesired(BridgeStructure,Coord,Piece++,FTransform(Orientation,Position,Dimensions/100.f)); ++BridgePieces;
			};
			AddStructure(Surface-FVector(0,0,25),FVector(Terrain.CellSize*1.01f,Terrain.CellSize,45.f),Rotation);
			for (float Side:{-1.f,1.f})
				AddStructure(Surface+Across*(Terrain.CellSize*.475f*Side)+FVector(0,0,42),FVector(Terrain.CellSize*1.01f,16.f,84.f),Rotation);
			const float PierHeight=Surface.Z-45.f-Cell.Elevation;
			if (PierHeight>50.f && ((Cell.Coords.X+Cell.Coords.Y)%2==0 || Cell.bBridgeEndpoint))
				AddStructure(FVector(Surface.X,Surface.Y,Cell.Elevation+PierHeight*.5f),FVector(50.f,Terrain.CellSize*.65f,PierHeight),FRotator(0,Cell.BridgeAxis==1 ? 0.f : 90.f,0).Quaternion());
		}
	}

	bool bHasFountain = false;
	for(int32 Y=Visible.Min.Y;Y<Visible.Max.Y;++Y)for(int32 X=Visible.Min.X;X<Visible.Max.X;++X)
	{
		FGridCell Cell;if(!Terrain.TryGetCell(FIntPoint(X,Y),Cell))continue;
		const FIntPoint Coord = Cell.Coords;
		if(!Visible.Contains(Coord))continue;
		// Each canonical cell owns its stream. Always take these three draws in
		// this order before any skip: another cell gaining a road/building cannot
		// change this cell's forest roll, tile scale or orientation on rebuild.
		const uint32 CellSeed = HashCombine(GetTypeHash(Terrain.TerrainSeed),
			HashCombine(GetTypeHash(Coord.X), GetTypeHash(Coord.Y))) ^ 0x45A71Du;
		FRandomStream CellRandom(static_cast<int32>(CellSeed));
		const float Roll = CellRandom.FRand();
		const float TreeWidthFraction = CellRandom.FRandRange(0.72f, 0.93f);
		const float TreeYaw = static_cast<float>(CellRandom.RandRange(0, 3)) * 90.f;
		if (!Cell.bIsPlayable || Cell.bIsRiverbed || Cell.OccupyingBuildingID != -1 || RoadCells.Contains(Coord)) continue;
		bool bNearBuilding = false, bRoadNeighbor = false;
		for (const FIntPoint& Direction : Cardinal)
		{
			const FIntPoint Neighbor = Coord + Direction;
			bNearBuilding |= Terrain.IsValidCoord(Neighbor) && Terrain.GetCell(Neighbor).OccupyingBuildingID != -1;
			bRoadNeighbor |= RoadCells.Contains(Neighbor);
		}
		if (bRoadNeighbor && bNearBuilding)
		{
		    // A rigid decorative slab cannot conform to relief. All nine canonical
		    // centre samples bound the four bilinear quads under this .98-cell tile;
		    // cardinal samples alone miss a high diagonal corner. Do not extrapolate
		    // across missing cells, coast, river or the edge of playable land.
		    float Lowest = Cell.Elevation;
		    float Highest = Cell.Elevation;
		    bool bSafeSlab = FMath::IsFinite(Lowest);
		    for (int32 DY = -1; DY <= 1 && bSafeSlab; ++DY)
		    {
		        for (int32 DX = -1; DX <= 1; ++DX)
		        {
		            FGridCell Neighbor;
		            if (!Terrain.TryGetCell(Coord + FIntPoint(DX, DY), Neighbor)
		                || !Neighbor.bIsPlayable || Neighbor.bIsRiverbed || Neighbor.bIsOcean
		                || !FMath::IsFinite(Neighbor.Elevation))
		            {
		                bSafeSlab = false;
		                break;
		            }
		            Lowest = FMath::Min(Lowest, Neighbor.Elevation);
		            Highest = FMath::Max(Highest, Neighbor.Elevation);
		        }
		    }
		    // Native slab top is approximately9.8cm above centre ground at1000cm
		    // cell size. An8cm bound leaves clearance instead of a crossing triangle.
		    // Scale the allowance down for older500cm fixtures; never grow above8cm.
		    const float MaxSlabReliefCm = FMath::Min(8.f, Terrain.CellSize * .008f);
		    if (!bSafeSlab || Highest - Lowest > MaxSlabReliefCm) continue;

		    int32 Index = Pavement;
		    if (!bHasFountain && Coord.X > MinX && Coord.X < MaxX && Coord.Y > MinY && Coord.Y < MaxY)
		        Index = Fountain;
		    if (UStaticMesh* Mesh = MeshBatches[Index]->GetStaticMesh())
		    {
		        if (PlazaInstances >=4096) continue;
		        const FTransform Tile = TileTransform(Mesh, Terrain.GridToWorld(Coord), Terrain.CellSize * .98f, 0.f, .04f);
		        AddDesired(Index, Coord, 0, Tile);
		        // Consume the optional fountain only after a valid slab was emitted.
		        bHasFountain |= Index == Fountain;
		        if (Index == Pavement && PremiumBenchCount <64 && CellSeed %7 ==0)
		        {
		            if (UStaticMesh* BenchMesh = MeshBatches[PremiumParkBench]->GetStaticMesh())
		            {
		                const FBox PadBox = Mesh->GetBoundingBox();
		                const FBox BenchBox = BenchMesh->GetBoundingBox();
		                FVector Position = Terrain.GridToWorld(Coord)
		                    + FVector(Terrain.CellSize * .34f, Terrain.CellSize * .34f, 0.f);
		                const float PadTopZ = Tile.TransformPosition(FVector(PadBox.GetCenter().X, PadBox.GetCenter().Y, PadBox.Max.Z)).Z;
		                Position.Z = PadTopZ +1.f - BenchBox.Min.Z;
		                const FTransform BenchTransform(FRotator(0.f,180.f,0.f), Position, FVector::OneVector);
		                const FBox BenchWorld = BenchBox.TransformBy(BenchTransform);
		                const FBox PadWorld = PadBox.TransformBy(Tile);
		                const bool bFeetSupported = BenchWorld.Min.X >= PadWorld.Min.X +1.f
		                    && BenchWorld.Min.Y >= PadWorld.Min.Y +1.f
		                    && BenchWorld.Max.X <= PadWorld.Max.X -1.f
		                    && BenchWorld.Max.Y <= PadWorld.Max.Y -1.f
		                    && PadTopZ >= Highest +1.f;
		                if (bFeetSupported && (!bHasRoadDetailView
		                    || FVector::DistSquaredXY(RoadDetailView, Position) < FMath::Square(35000.f)))
		                {
		                    AddDesired(PremiumParkBench, Coord, 0, BenchTransform);
		                    ++PremiumBenchCount;
		                }
		            }
		        }
		        ++PlazaInstances;
		    }
		    continue;
		}
		// Original physical meshes replace old square green woodland tiles.
		// A missing biome family is logged and omitted, never silently substituted.
		if(bDetailedEnvironment)continue;
		const bool bWithinDistrict = !Records.IsEmpty() && Coord.X >= MinX - 2 && Coord.X <= MaxX + 2 && Coord.Y >= MinY - 2 && Coord.Y <= MaxY + 2;
		// Leave river edges and access verges readable; reserve distant hills for denser trees.
		const float Chance = (Terrain.ContinuationGeneratorVersion==3
			?(bWithinDistrict?.035f:(Cell.ForestFuel01>.4f?.22f:0.f))
			:(bWithinDistrict?.06f:(Cell.RiverDistance<Terrain.CellSize*2.f?.12f:.20f)))*FoliageDensity;
		if (bNearBuilding || bRoadNeighbor || Roll > Chance) continue;
		const int32 Index = (Coord.X + Coord.Y) % 3 == 0 ? TallWoodland : Woodland;
		if (UStaticMesh* Mesh = MeshBatches[Index]->GetStaticMesh())
		{
			if(WoodlandInstances>=4096)continue;
			const float Width = Terrain.CellSize * TreeWidthFraction;
			AddDesired(Index,Coord,0,TileTransform(Mesh, Terrain.GridToWorld(Coord), Width, TreeYaw, 0.06f));
			++WoodlandInstances;
		}
	}
	LastUpdatedBatchCount=0;LastChangedInstanceCount=0;
	for(int32 BatchIndex=0;BatchIndex<MeshBatches.Num();++BatchIndex)
	{
		auto* Batch=MeshBatches[BatchIndex].Get();auto& Slots=CosmeticBatchSlots[BatchIndex];bool Changed=false;
		if(Batch->GetInstanceCount()!=Slots.KeyByIndex.Num())
		{
			// External fixture reset/replacement: only this batch is repaired.
			Batch->ClearInstances();Slots.KeyByIndex.Reset();Slots.IndexByKey.Reset();Changed=true;
		}
		TSet<FIntVector> Wanted;Wanted.Reserve(Desired[BatchIndex].Num());
		for(const auto& Item:Desired[BatchIndex])Wanted.Add(Item.Key);
		for(int32 I=Slots.KeyByIndex.Num()-1;I>=0;--I)if(!Wanted.Contains(Slots.KeyByIndex[I]))
		{
			const FIntVector Removed=Slots.KeyByIndex[I];
			if(Batch->RemoveInstance(I))
			{
				Slots.KeyByIndex.RemoveAtSwap(I,1,EAllowShrinking::No);Slots.IndexByKey.Remove(Removed);
				if(Slots.KeyByIndex.IsValidIndex(I))Slots.IndexByKey.FindChecked(Slots.KeyByIndex[I])=I;
				Changed=true;++LastChangedInstanceCount;
			}
		}
		for(const auto& Item:Desired[BatchIndex])
		{
			if(const int32* Index=Slots.IndexByKey.Find(Item.Key))
			{
				FTransform Current;if(!Batch->GetInstanceTransform(*Index,Current,true)||!Current.Equals(Item.Transform,.0001f))
				{Batch->UpdateInstanceTransform(*Index,Item.Transform,true,false,true);Changed=true;++LastChangedInstanceCount;}
			}
			else
			{
				const int32 NewIndex=Batch->AddInstance(Item.Transform,true);
				if(NewIndex!=INDEX_NONE){Slots.KeyByIndex.Add(Item.Key);Slots.IndexByKey.Add(Item.Key,NewIndex);Changed=true;++LastChangedInstanceCount;}
			}
		}
		if(Changed){Batch->BuildTreeIfOutdated(false,true);Batch->MarkRenderStateDirty();++LastUpdatedBatchCount;}
	}
	if(bEnvironmentChanged)
	{
		const auto& EnvironmentStats=EnvironmentDressing->GetStats();
		LastUpdatedBatchCount+=EnvironmentStats.ChangedBatches;
		LastChangedInstanceCount+=EnvironmentStats.ChangedInstances;
	}
	ApplyPresentationBudget();
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITYSAMPLE_PROPS lamps=%d lampCap=96 benches=%d benchCap=64 nativeCentimetreScale=1 nearOnly=1 perPropActors=0 perPropTicks=0"),PremiumLampCount,PremiumBenchCount);
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CITY_INCREMENTAL_BATCHES changedBatches=%d changedInstances=%d totalBatches=%d clearAllBatches=0 decorativeCapPerKind=4096 derivedOnly=1"),LastUpdatedBatchCount,LastChangedInstanceCount,MeshBatches.Num());
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CITY_PRESENTATION playerRoadSource=1 seed=%d roads=%d decoratedLotAccess=%d buildingRecords=%d plazas=%d woodlandTiles=%d bridgePieces=%d riverRoadsSupported=1 batches=%d perPropActors=0 perPropTick=0"),
		Terrain.TerrainSeed, RoadInstances, ConnectedLots, Records.Num(), PlazaInstances, WoodlandInstances,BridgePieces, MeshBatches.Num());
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_ROAD_PRESENTATION_COVERAGE canonicalInitialRoads=%d expectedVisibleInitialRoads=%d renderedInitialRoads=%d omittedInitialRoads=%d windowExcludedInitialRoads=%d initialWidth=%d initialHeight=%d initialSurfaceDistanceCull=0 lightpostDetailRadiusCm=45000 propsEndCullCm=60000 expansionVisibleWindow=%dx%d duplicateSurfaceLayers=0"),CanonicalInitialRoadCount,ExpectedVisibleInitialRoadCount,RenderedInitialRoadCount,ExpectedVisibleInitialRoadCount-RenderedInitialRoadCount,CanonicalInitialRoadCount-ExpectedVisibleInitialRoadCount,Terrain.GridWidth,Terrain.GridHeight,Visible.Width(),Visible.Height());
}

bool UCityPresentationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return Super::ShouldCreateSubsystem(Outer) && World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UCityPresentationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UTerrainGridSubsystem>();
	Collection.InitializeDependency<UBuildingManagerSubsystem>();
	Collection.InitializeDependency<URoadNetworkSubsystem>();
	Collection.InitializeDependency<UWorldClimateSubsystem>();
	if (UWorld* World = GetWorld())
	{
		if (URoadNetworkSubsystem* Roads = World->GetSubsystem<URoadNetworkSubsystem>()) Roads->OnRoadNetworkChanged.AddUniqueDynamic(this, &UCityPresentationSubsystem::HandleRoadNetworkChanged);
	}
}

void UCityPresentationSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	// Camera/HUD seed the initial canonical settlement during BeginPlay. Read it
	// after that lifecycle phase and observe future edits without touching owners.
	InWorld.GetTimerManager().SetTimer(RefreshTimer, this, &UCityPresentationSubsystem::PollPresentation, 1.f, true, 0.15f);
}

void UCityPresentationSubsystem::HandleRoadNetworkChanged()
{
	bHasVisualSignature=false;LastObservedTerrainRevision=MAX_uint64;RefreshPresentation();
}

void UCityPresentationSubsystem::PollPresentation()
{
	const auto* World=GetWorld();const auto* Terrain=World?World->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	if(!Terrain)return;
	if(IsValid(PresentationActor))
	{
		if(const auto* Climate=World->GetSubsystem<UWorldClimateSubsystem>())PresentationActor->UpdateWorldClimate(*Terrain,Climate->GetState());
		// Existing 1Hz timer only: a fine view/quality change diffs plants without rebuilding roads.
		if(PresentationActor->RefreshEnvironmentDressing(*Terrain))OnPresentationRebuilt.Broadcast(++PresentationRevision);
		FVector View;FIntVector DetailCell(MAX_int32,MAX_int32,MAX_int32);
		if(GetRoadDetailView(World,View))DetailCell=FIntVector(FMath::FloorToInt(View.X/5000.f),FMath::FloorToInt(View.Y/5000.f),FMath::FloorToInt(View.Z/5000.f));
		if(bHasVisualSignature&&LastObservedTerrainRevision==Terrain->GetTerrainRevision()
			&&LastObservedBounds==GetAftershockDrawableCellBounds(*Terrain)&&LastObservedDetailCell==DetailCell
			&&LastObservedFoliageQuality==GetCityFoliageQuality())return;
	}
	RefreshPresentation();
}

void UCityPresentationSubsystem::RefreshPresentation()
{
	if(bRefreshingPresentation)return;
	UWorld* World = GetWorld();
	if (!World) return;
	UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>();
	UBuildingManagerSubsystem* Buildings = World->GetSubsystem<UBuildingManagerSubsystem>();
	if (!Terrain || !Buildings || Terrain->GridWidth < 3 || Terrain->GridHeight < 3 || Terrain->Cells.IsEmpty()) return;
	TGuardValue<bool> RefreshGuard(bRefreshingPresentation,true);
	if(auto* Crowd=World->GetSubsystem<UCrowdRenderSubsystem>())Crowd->EnsureTerrainVisualBuilt();
	Buildings->RefreshVisibleBuildingPresentation();
	if(auto* Disaster=World->GetSubsystem<UDisasterSimulationSubsystem>())Disaster->RefreshVisibleRubblePresentation();
	if(auto* Coastal=World->GetSubsystem<UCoastalPresentationSubsystem>())Coastal->RefreshVisiblePresentation();
	if(auto* Effects=World->GetSubsystem<UDisasterPresentationSubsystem>())Effects->RefreshRegionalVolcanoPresentation();
	uint32 Signature = HashCombine(GetTypeHash(Terrain->TerrainSeed), GetTypeHash(Terrain->CellSize));
	Signature=HashCombine(Signature,GetTypeHash(GetCityFoliageQuality()));
	Signature = HashCombine(Signature, GetTypeHash(Terrain->GridOrigin));
	const FIntRect Visible=GetAftershockDrawableCellBounds(*Terrain);
	Signature=HashCombine(Signature,GetTypeHash(Visible.Min));Signature=HashCombine(Signature,GetTypeHash(Visible.Max));
	FVector RoadDetailView;
	if(GetRoadDetailView(World,RoadDetailView))
		Signature=HashCombine(Signature,GetTypeHash(FIntVector(FMath::FloorToInt(RoadDetailView.X/5000.f),FMath::FloorToInt(RoadDetailView.Y/5000.f),FMath::FloorToInt(RoadDetailView.Z/5000.f))));
	for(int32 Y=Visible.Min.Y-1;Y<Visible.Max.Y+1;++Y)for(int32 X=Visible.Min.X-1;X<Visible.Max.X+1;++X)
	{
		FGridCell Cell;const bool Present=Terrain->TryGetCell(FIntPoint(X,Y),Cell);Signature=HashCombine(Signature,GetTypeHash(Present));if(!Present)continue;
		Signature = HashCombine(Signature, GetTypeHash(Cell.OccupyingBuildingID));
		Signature = HashCombine(Signature, GetTypeHash(FMath::RoundToInt(Cell.Elevation * 10.f)));
		Signature = HashCombine(Signature, GetTypeHash(Cell.bIsRiverbed));
		Signature = HashCombine(Signature, GetTypeHash(Cell.bIsPlayable));
		Signature = HashCombine(Signature, GetTypeHash(Cell.bHasRoad));
		Signature = HashCombine(Signature, GetTypeHash(Cell.BridgeID));
		Signature = HashCombine(Signature, GetTypeHash(Cell.BridgeAxis));
		Signature = HashCombine(Signature, GetTypeHash(Cell.RoadSurfaceElevationCm));
		Signature = HashCombine(Signature, GetTypeHash(Cell.bBridgeEndpoint));
	}
	if (IsValid(PresentationActor))
	{
		PresentationActor->ApplyPresentationBudget();
		PresentationActor->BuildGlobeVisual(*Terrain);
		if (const UWorldClimateSubsystem* Climate = World->GetSubsystem<UWorldClimateSubsystem>())
			PresentationActor->UpdateWorldClimate(*Terrain, Climate->GetState());
		if(PresentationActor->RefreshEnvironmentDressing(*Terrain))OnPresentationRebuilt.Broadcast(++PresentationRevision);
	}
	LastObservedTerrainRevision=Terrain->GetTerrainRevision();LastObservedBounds=Visible;
	LastObservedFoliageQuality=GetCityFoliageQuality();
	LastObservedDetailCell=GetRoadDetailView(World,RoadDetailView)?FIntVector(FMath::FloorToInt(RoadDetailView.X/5000.f),FMath::FloorToInt(RoadDetailView.Y/5000.f),FMath::FloorToInt(RoadDetailView.Z/5000.f)):FIntVector(MAX_int32,MAX_int32,MAX_int32);
	if (bHasVisualSignature && LastVisualSignature == Signature) return;
	if (!IsValid(PresentationActor))
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		PresentationActor = World->SpawnActor<AAftershockCityPresentationActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!PresentationActor) return;
		PresentationActor->ConfigurePresentationLighting();
	}
	PresentationActor->BuildGlobeVisual(*Terrain);
	if(UCrowdRenderSubsystem* Crowd=World->GetSubsystem<UCrowdRenderSubsystem>())Crowd->EnsureTerrainVisualBuilt();
	if (const UWorldClimateSubsystem* Climate = World->GetSubsystem<UWorldClimateSubsystem>())
		PresentationActor->UpdateWorldClimate(*Terrain, Climate->GetState());
	PresentationActor->RebuildCosmeticDistrict(*Terrain, *Buildings);
	LastVisualSignature = Signature;
	bHasVisualSignature = true;
	if(PresentationActor->GetLastUpdatedBatchCount()>0)OnPresentationRebuilt.Broadcast(++PresentationRevision);
}

void UCityPresentationSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RefreshTimer);
		if (URoadNetworkSubsystem* Roads = World->GetSubsystem<URoadNetworkSubsystem>()) Roads->OnRoadNetworkChanged.RemoveDynamic(this, &UCityPresentationSubsystem::HandleRoadNetworkChanged);
	}
	OnPresentationRebuilt.Clear();
	if (IsValid(PresentationActor)) PresentationActor->Destroy();
	PresentationActor = nullptr;
	Super::Deinitialize();
}
