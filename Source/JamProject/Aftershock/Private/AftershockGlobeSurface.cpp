#include "AftershockGlobeSurface.h"
#include "AftershockRuntimeMeshUV.h"
#include "AftershockTerrainSurface.h"
#include "AftershockPresentationBudget.h"
#include "AftershockRegionalSurfaceStyle.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "GameFramework/Actor.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "PhysicsEngine/BodySetup.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Algo/Reverse.h"

bool AftershockGeographicPresentationWorld(const UTerrainGridSubsystem& Terrain,double Latitude,double Longitude,FVector& Out)
{
	FVector2D Metres;
	if(!Terrain.IsUsingRealGeography()||!AftershockFocusLatLonToLocalMetres(Terrain.GeographyIndex,Terrain.GeographyFocusIndex,Latitude,Longitude,Metres))return false;
	Out=FVector(Terrain.GridOrigin.X+Terrain.GridWidth*Terrain.CellSize*.5+Metres.X*100.,Terrain.GridOrigin.Y+Terrain.GridHeight*Terrain.CellSize*.5+Metres.Y*100.,Terrain.GridOrigin.Z);
	return !Out.ContainsNaN();
}

bool IsAftershockInstalledGroundCoverage(const UTerrainGridSubsystem& Terrain,
	const FIntRect& InstalledDrawBounds, FIntPoint Coord)
{
	FGridCell Cell;
	return InstalledDrawBounds.Contains(Coord) && Terrain.TryGetCell(Coord, Cell) && Cell.bIsPlayable;
}

void GetAftershockGlobeLatitudeAngles(bool bRealGeography, TArray<double>& OutAngles)
{
	const int32 BaseBands=bRealGeography?64:96;
	OutAngles.Reset();OutAngles.Reserve(BaseBands+1+(bRealGeography?9:0));
	for(int32 Latitude=0;Latitude<=BaseBands;++Latitude)
		OutAngles.Add(bRealGeography?UE_DOUBLE_PI*Latitude/BaseBands:static_cast<double>(PI*Latitude/BaseBands));
	if(!bRealGeography)return; // Preserve every V1 band and its previous float angle.
	constexpr double EarthRadiusMetres=6371000.;
	for(double ArcKilometres:{1.,2.,4.,8.,16.,32.,64.,128.,256.})OutAngles.Add(ArcKilometres*1000./EarthRadiusMetres);
	OutAngles.Sort();
	for(int32 I=OutAngles.Num()-1;I>0;--I)if(FMath::Abs(OutAngles[I]-OutAngles[I-1])<1.e-12)OutAngles.RemoveAt(I);
}

namespace
{
	struct FGlobeVertex
	{
		FVector Position;
		FVector Normal = FVector::UpVector;
		FVector2f UV = FVector2f::ZeroVector;
		FLinearColor Color = FLinearColor::White;
		int32 SurfaceIndex=0;
	};
	struct FRegionalMeshCache
	{
		uint64 PlanetSignature=0,ContextSignature=0;
		bool bPlanetReady=false,bContextReady=false;
		TWeakObjectPtr<UStaticMesh> Planet,Clouds,Atmosphere,Context;
		TWeakObjectPtr<UTexture2D> SourceMask,OverviewSourceMask;
		uint64 SourceMaskSignature=0,OverviewSourceMaskSignature=0;
		int32 SourceMaskBuilds=0;
		int32 PlanetBuilds=0,ContextBuilds=0;
	};
	TMap<TWeakObjectPtr<UStaticMeshComponent>,FRegionalMeshCache> RegionalMeshCaches;
	FRegionalMeshCache& RegionalCacheFor(UStaticMeshComponent* Component)
	{
		for(auto It=RegionalMeshCaches.CreateIterator();It;++It)if(!It.Key().IsValid())It.RemoveCurrent();
		const TWeakObjectPtr<UStaticMeshComponent> Key(Component);
		if(!RegionalMeshCaches.Contains(Key)&&RegionalMeshCaches.Num()>=32)RegionalMeshCaches.Remove(RegionalMeshCaches.CreateConstIterator().Key());
		return RegionalMeshCaches.FindOrAdd(Key);
	}

	float SampleCanonicalHeight(const UTerrainGridSubsystem& Terrain, float X, float Y)
	{
		return SampleAftershockResidentElevation(Terrain,X,Y);
	}

	bool BindRealRegionalSourceMask(const UTerrainGridSubsystem& Terrain,UStaticMeshComponent* Component,
		const FAftershockGeographicBounds& Bounds,const FVector& NorthWest,const FVector& SouthEast,FRegionalMeshCache& Cache,bool bOverview=false)
	{
		auto* MID=Cast<UMaterialInstanceDynamic>(Component->GetMaterial(bOverview?1:0));
		if(!MID)return false;
		auto& Mask=bOverview?Cache.OverviewSourceMask:Cache.SourceMask;
		auto& MaskSignature=bOverview?Cache.OverviewSourceMaskSignature:Cache.SourceMaskSignature;
		// Fail closed if a new focus cannot supply all source pixels. This parameter
		// is the only V2 water classifier; gamma-packed vertex blue is not a mask.
		MID->SetScalarParameterValue(TEXT("RegionalSourceMaskEnabled"),0.f);
		MID->SetScalarParameterValue(TEXT("RegionalDetailSourceJoinEnabled"),0.f);
		uint64 Signature=1469598103934665603ull;
		for(uint32 Value:{GetTypeHash(Terrain.GeographyIndex),GetTypeHash(Terrain.GeographyFocusIndex),GetTypeHash(Terrain.ContinuationGeneratorVersion),GetTypeHash(Bounds.West),
			GetTypeHash(Bounds.South),GetTypeHash(Bounds.East),GetTypeHash(Bounds.North)})Signature=(Signature^Value)*1099511628211ull;
		if(!Mask.IsValid()||MaskSignature!=Signature)
		{
			constexpr int32 Side=257;
			TArray<FColor> Pixels;Pixels.SetNumUninitialized(Side*Side);
			for(int32 Y=0;Y<Side;++Y)for(int32 X=0;X<Side;++X)
			{
				FAftershockGeographicTerrainSample Sample;
				const double Lat=FMath::Lerp(Bounds.North,Bounds.South,static_cast<double>(Y)/(Side-1));
				const double Lon=FMath::Lerp(Bounds.West,Bounds.East,static_cast<double>(X)/(Side-1));
				if(!(bOverview?TrySampleAftershockRegionalContext(Terrain.GeographyIndex,Lat,Lon,Sample):Terrain.TrySampleGeographicSource(Lat,Lon,Sample)))return false;
				Pixels[X+Y*Side]=FColor(Sample.bOcean||Sample.bInlandWater?255:0,Sample.bMappedForest?255:0,
					Sample.bMappedAgriculture?255:0,Sample.bOcean?255:0);
			}
			UTexture2D* Texture=UTexture2D::CreateTransient(Side,Side,PF_B8G8R8A8);
			if(!Texture||!Texture->GetPlatformData())return false;
			Texture->SRGB=false;Texture->NeverStream=true;Texture->Filter=TF_Trilinear;
			Texture->AddressX=TA_Clamp;Texture->AddressY=TA_Clamp;
			Texture->LODGroup=TEXTUREGROUP_Terrain_Weightmap;Texture->CompressionSettings=TC_Masks;
			int32 Width=Side,MipIndex=0;int64 TextureBytes=0;
			for(;;)
			{
				FTexture2DMipMap* Mip=MipIndex==0?&Texture->GetPlatformData()->Mips[0]:new FTexture2DMipMap(Width,Width,1);
				if(MipIndex>0)Texture->GetPlatformData()->Mips.Add(Mip);
				const int64 Bytes=static_cast<int64>(Pixels.Num())*sizeof(FColor);TextureBytes+=Bytes;
				Mip->BulkData.Lock(LOCK_READ_WRITE);void* Destination=Mip->BulkData.Realloc(Bytes);
				FMemory::Memcpy(Destination,Pixels.GetData(),Bytes);Mip->BulkData.Unlock();
				++MipIndex;if(Width==1)break;
				const int32 NextWidth=FMath::Max(1,Width/2);TArray<FColor> NextPixels;NextPixels.SetNumUninitialized(NextWidth*NextWidth);
				for(int32 Y=0;Y<NextWidth;++Y)for(int32 X=0;X<NextWidth;++X)
				{
					const FColor A=Pixels[X*2+Y*2*Width],B=Pixels[FMath::Min(X*2+1,Width-1)+Y*2*Width];
					const FColor C=Pixels[X*2+FMath::Min(Y*2+1,Width-1)*Width],D=Pixels[FMath::Min(X*2+1,Width-1)+FMath::Min(Y*2+1,Width-1)*Width];
					const auto Average=[](uint8 P,uint8 Q,uint8 R,uint8 S){return static_cast<uint8>((static_cast<int32>(P)+Q+R+S+2)/4);};
					NextPixels[X+Y*NextWidth]=FColor(Average(A.R,B.R,C.R,D.R),Average(A.G,B.G,C.G,D.G),
						Average(A.B,B.B,C.B,D.B),Average(A.A,B.A,C.A,D.A));
				}
				Pixels=MoveTemp(NextPixels);Width=NextWidth;
			}
			// The old mask may still be referenced by both detail and overview
			// MIDs. Replace bindings normally; normal UObject/texture GC owns
			// render-resource fences after the last actual owner releases it.
			Texture->UpdateResource();Mask=Texture;MaskSignature=Signature;++Cache.SourceMaskBuilds;
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_REGIONAL_SOURCE_MASK region=%d focus=%d side=257 mips=%d rawMipBytes=%lld builds=%d sourceRasterResolutionPreserved=1 srgb=0 waterExclusiveMaskR=1 mappedForestG=1 mappedAgricultureB=1 oceanA=1 tierIndependent=1 weakOnly=1 canonicalWrites=0"),
				Terrain.GeographyIndex,Terrain.GeographyFocusIndex,MipIndex,TextureBytes,Cache.SourceMaskBuilds);
		}
		MID->SetTextureParameterValue(TEXT("RegionalSourceMask"),Mask.Get());
		MID->SetVectorParameterValue(TEXT("RegionalSourceBoundsMin"),FLinearColor(NorthWest.X,NorthWest.Y,0.f));
		MID->SetVectorParameterValue(TEXT("RegionalSourceBoundsMax"),FLinearColor(SouthEast.X,SouthEast.Y,0.f));
		MID->SetScalarParameterValue(TEXT("RegionalSourceMaskEnabled"),1.f);
		MID->SetScalarParameterValue(TEXT("RegionalCurvatureEnabled"),bOverview?1.f:0.f);
		if(bOverview)
		{
			FAftershockGeographicBounds Detail;FVector DNW,DSE;
			if(!Terrain.TryGetGeographicSourceBounds(Detail)||!AftershockGeographicPresentationWorld(Terrain,Detail.North,Detail.West,DNW)||!AftershockGeographicPresentationWorld(Terrain,Detail.South,Detail.East,DSE))return false;
			// Share the EXACT detail raster already bound to material0. Resampling
			// that small focus into the wide257 overview mask loses its shoreline.
			// Both bindings are refreshed even on a geometry/source texture cache
			// hit, so a changed focus/origin cannot retain stale join coordinates.
			if(!Cache.SourceMask.IsValid())return false;
			MID->SetTextureParameterValue(TEXT("RegionalDetailSourceMask"),Cache.SourceMask.Get());
			MID->SetVectorParameterValue(TEXT("RegionalDetailSourceBoundsMin"),FLinearColor(DNW.X,DNW.Y,0.f));
			MID->SetVectorParameterValue(TEXT("RegionalDetailSourceBoundsMax"),FLinearColor(DSE.X,DSE.Y,0.f));
			MID->SetVectorParameterValue(TEXT("RegionalDetailSourceBlendCm"),FLinearColor(
				FMath::Max(1.,(SouthEast.X-NorthWest.X)/128.),FMath::Max(1.,(SouthEast.Y-NorthWest.Y)/128.),0.f));
			MID->SetScalarParameterValue(TEXT("RegionalDetailSourceJoinEnabled"),1.f);
			MID->SetVectorParameterValue(TEXT("RegionalCurvatureDetailMin"),FLinearColor(DNW.X,DNW.Y,0.f));
			MID->SetVectorParameterValue(TEXT("RegionalCurvatureDetailMax"),FLinearColor(DSE.X,DSE.Y,0.f));
			MID->SetVectorParameterValue(TEXT("RegionalCurvatureOuterMin"),FLinearColor(NorthWest.X,NorthWest.Y,0.f));
			MID->SetVectorParameterValue(TEXT("RegionalCurvatureOuterMax"),FLinearColor(SouthEast.X,SouthEast.Y,0.f));
			MID->SetVectorParameterValue(TEXT("RegionalTangentCenter"),FLinearColor(Terrain.GridOrigin.X+Terrain.GridWidth*Terrain.CellSize*.5,Terrain.GridOrigin.Y+Terrain.GridHeight*Terrain.CellSize*.5,0.f));
		}
		return true;
	}

	UStaticMesh* MakeGlobeMesh(AActor* Owner, const TCHAR* Name, const TArray<FGlobeVertex>& Vertices,
		const TArray<int32>& Indices, UMaterialInterface* Material,UMaterialInterface* OverviewMaterial=nullptr)
	{
		if (!Material || Vertices.IsEmpty() || Indices.IsEmpty()) return nullptr;
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		auto Positions = Attributes.GetVertexPositions();
		auto Normals = Attributes.GetVertexInstanceNormals();
		auto Tangents = Attributes.GetVertexInstanceTangents();
		auto Signs = Attributes.GetVertexInstanceBinormalSigns();
		auto Colors = Attributes.GetVertexInstanceColors();
		auto UVs = Attributes.GetVertexInstanceUVs();
		UVs.SetNumChannels(1);
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Surface");
		const FPolygonGroupID OverviewGroup=OverviewMaterial?Description.CreatePolygonGroup():Group;
		if(OverviewMaterial)Attributes.GetPolygonGroupMaterialSlotNames()[OverviewGroup]=TEXT("Overview");
		TArray<FVertexID> VertexIDs;
		VertexIDs.Reserve(Vertices.Num());
		for (const FGlobeVertex& Vertex : Vertices)
		{
			const FVertexID ID = Description.CreateVertex();
			Positions[ID] = FVector3f(Vertex.Position);
			VertexIDs.Add(ID);
		}
		for (int32 I = 0; I < Indices.Num(); I += 3)
		{
			FVertexInstanceID Corners[3];
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 Index = Indices[I + Corner];
				const FGlobeVertex& Vertex = Vertices[Index];
				Corners[Corner] = Description.CreateVertexInstance(VertexIDs[Index]);
				Normals[Corners[Corner]] = FVector3f(Vertex.Normal);
				FVector Tangent = FVector::VectorPlaneProject(FVector::ForwardVector, Vertex.Normal).GetSafeNormal();
				if (Tangent.IsNearlyZero()) Tangent = FVector::VectorPlaneProject(FVector::RightVector, Vertex.Normal).GetSafeNormal();
				Tangents[Corners[Corner]] = FVector3f(Tangent);
				Signs[Corners[Corner]] = 1.f;
				Colors[Corners[Corner]] = FVector4f(Vertex.Color.R, Vertex.Color.G, Vertex.Color.B, Vertex.Color.A);
				UVs.Set(Corners[Corner], 0, Vertex.UV);
			}
			Description.CreateTriangle(OverviewMaterial&&Vertices[Indices[I]].SurfaceIndex==1?OverviewGroup:Group, MakeArrayView(Corners));
		}
		UStaticMesh* Mesh = NewObject<UStaticMesh>(Owner, MakeUniqueObjectName(Owner, UStaticMesh::StaticClass(), Name), RF_Transient);
		Mesh->bAllowCPUAccess=false;
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, TEXT("Surface")));
		if(OverviewMaterial)Mesh->GetStaticMaterials().Add(FStaticMaterial(OverviewMaterial,TEXT("Overview")));
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bFastBuild = true;
		Params.bBuildSimpleCollision = false;
		Params.bCommitMeshDescription = false;
		Params.bMarkPackageDirty = false;
		Params.bAllowCpuAccess = false;
		if (!Mesh->BuildFromMeshDescriptions({ &Description }, Params)) return nullptr;
		InitializeAftershockRuntimeMeshUV(*Mesh, Vertices, Indices);
		if (UBodySetup* Body = Mesh->GetBodySetup())
		{
			Body->CollisionTraceFlag = CTF_UseSimpleAsComplex;
			Body->DefaultInstance.SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		return Mesh;
	}

	bool BuildRealRegionalContext(AActor* Owner,const UTerrainGridSubsystem& Terrain,UStaticMeshComponent* Component,int32 GeometryStride,FRegionalMeshCache& Cache)
	{
		FAftershockGeographicBounds DetailBounds,OverviewBounds;
		if(!Terrain.TryGetGeographicSourceBounds(DetailBounds))return false;
		const bool HasOverview=Terrain.ContinuationGeneratorVersion==3&&GetAftershockRegionalContextBounds(Terrain.GeographyIndex,OverviewBounds);
		if(HasOverview&&!Cast<UMaterialInstanceDynamic>(Component->GetMaterial(1)))
		{
			auto* Base=Cast<UMaterialInstanceDynamic>(Component->GetMaterial(0));
			if(!Base)return false;
			Component->SetMaterial(1,UMaterialInstanceDynamic::Create(Base->Parent,Owner));
		}
		const int32 DetailSegments=Terrain.ContinuationGeneratorVersion==3?(GeometryStride>=8?64:GeometryStride>=4?128:256):(GeometryStride>=8?16:GeometryStride>=4?32:64);
		const int32 OverviewSegments=GeometryStride>=8?32:GeometryStride>=4?64:128;
		const FIntRect Draw=GetAftershockDrawableCellBounds(Terrain);
		uint64 Signature=GetAftershockTerrainGeometrySignature(Terrain,Draw);
		Signature^=static_cast<uint64>(GetTypeHash(Terrain.GeographyIndex))<<32;
		Signature^=static_cast<uint64>(GetTypeHash(Terrain.GeographyFocusIndex))<<16;
		Signature^=GetTypeHash(Terrain.CoastalSeaLevelCm);
		Signature^=static_cast<uint64>(DetailSegments)*1099511628211ull;
		Signature^=static_cast<uint64>(Terrain.ContinuationGeneratorVersion)*314159265358979ull;
		Signature^=0x5633456467653230ull; // V3 outer20% display relief taper; invalidate old context meshes.
		ApplyAftershockRegionalSurfaceStyle(Terrain,Component,0);
		if(HasOverview)ApplyAftershockRegionalSurfaceStyle(Terrain,Component,1);
		if(Cache.bContextReady&&Cache.ContextSignature==Signature&&Component->GetStaticMesh()==Cache.Context.Get())return true;
		TArray<FGlobeVertex> AllVertices;TArray<int32> AllIndices;
		TArray<double> DetailXs,DetailYs;
		const auto BuildLayer=[&](bool bOverview)->bool
		{
		const FAftershockGeographicBounds& Bounds=bOverview?OverviewBounds:DetailBounds;
		const int32 Segments=bOverview?OverviewSegments:DetailSegments;
		FVector NorthWest,SouthEast,DetailNW,DetailSE;
		if(!AftershockGeographicPresentationWorld(Terrain,Bounds.North,Bounds.West,NorthWest)||!AftershockGeographicPresentationWorld(Terrain,Bounds.South,Bounds.East,SouthEast)
			||!AftershockGeographicPresentationWorld(Terrain,DetailBounds.North,DetailBounds.West,DetailNW)||!AftershockGeographicPresentationWorld(Terrain,DetailBounds.South,DetailBounds.East,DetailSE))return false;
		if(!BindRealRegionalSourceMask(Terrain,Component,Bounds,NorthWest,SouthEast,Cache,bOverview))return false;
		const auto DetailCovered=[&](double X,double Y){return X>DetailNW.X+.001&&X<DetailSE.X-.001&&Y>DetailNW.Y+.001&&Y<DetailSE.Y-.001;};
		const auto AtDetailBoundary=[&](double X,double Y)
		{
			return bOverview&&(((FMath::Abs(X-DetailNW.X)<.01||FMath::Abs(X-DetailSE.X)<.01)&&Y>=DetailNW.Y-.01&&Y<=DetailSE.Y+.01)
				||((FMath::Abs(Y-DetailNW.Y)<.01||FMath::Abs(Y-DetailSE.Y)<.01)&&X>=DetailNW.X-.01&&X<=DetailSE.X+.01));
		};
		const auto SourceAt=[&](double Lat,double Lon,FAftershockGeographicTerrainSample& Out)
		{
			// Share exact detailed edge heights; the coarse overview never replaces construction authority.
			if(!bOverview||(Lat>=DetailBounds.South-1.e-9&&Lat<=DetailBounds.North+1.e-9&&Lon>=DetailBounds.West-1.e-9&&Lon<=DetailBounds.East+1.e-9))
				return Terrain.TrySampleGeographicSource(FMath::Clamp(Lat,DetailBounds.South,DetailBounds.North),FMath::Clamp(Lon,DetailBounds.West,DetailBounds.East),Out);
			if(!TrySampleAftershockRegionalContext(Terrain.GeographyIndex,Lat,Lon,Out))return false;
			// Different source/LOD grids can disagree at their shared edge. Fade the
			// detailed-minus-overview EDGE difference across two overview source
			// samples, only on drawn land. This display seam correction never changes
			// a canonical height, source packet, coast flag or construction boundary.
			const double DY=FMath::Max3(DetailBounds.South-Lat,0.,Lat-DetailBounds.North);
			const double DX=FMath::Max3(DetailBounds.West-Lon,0.,Lon-DetailBounds.East);
			const double T=FMath::Clamp(FMath::Max(DY/FMath::Max((OverviewBounds.North-OverviewBounds.South)/128.,1.e-9),DX/FMath::Max((OverviewBounds.East-OverviewBounds.West)/128.,1.e-9)),0.,1.);
			if(T<1.&&!Out.bOcean)
			{
				const double EdgeLat=FMath::Clamp(Lat,DetailBounds.South,DetailBounds.North),EdgeLon=FMath::Clamp(Lon,DetailBounds.West,DetailBounds.East);
				FAftershockGeographicTerrainSample FineEdge,CoarseEdge;
				if(Terrain.TrySampleGeographicSource(EdgeLat,EdgeLon,FineEdge)&&TrySampleAftershockRegionalContext(Terrain.GeographyIndex,EdgeLat,EdgeLon,CoarseEdge))
				{
					const double FineHeight=FineEdge.bOcean?0.:FineEdge.ElevationMetres,CoarseHeight=CoarseEdge.bOcean?0.:CoarseEdge.ElevationMetres;
					Out.ElevationMetres+=static_cast<float>((FineHeight-CoarseHeight)*(1.-T*T*(3.-2.*T)));
				}
			}
			return true;
		};
		const auto CurvatureAt=[&](double X,double Y)
		{
			if(!bOverview)return 0.;
			const double DX=FMath::Max3(DetailNW.X-X,0.,X-DetailSE.X),DY=FMath::Max3(DetailNW.Y-Y,0.,Y-DetailSE.Y);
			const double SX=X<DetailNW.X?DetailNW.X-NorthWest.X:SouthEast.X-DetailSE.X;
			const double SY=Y<DetailNW.Y?DetailNW.Y-NorthWest.Y:SouthEast.Y-DetailSE.Y;
			const double T=FMath::Clamp(FMath::Max(DX/FMath::Max(SX,1.),DY/FMath::Max(SY,1.)),0.,1.);
			const double Blend=T*T*(3.-2.*T),LX=X-Terrain.GridOrigin.X-Terrain.GridWidth*Terrain.CellSize*.5,LY=Y-Terrain.GridOrigin.Y-Terrain.GridHeight*Terrain.CellSize*.5;
			constexpr double Radius=637100000.;const double R2=LX*LX+LY*LY;
			return -R2/(FMath::Sqrt(FMath::Max(1.,Radius*Radius-R2))+Radius)*Blend;
		};
		const FAftershockGlobeLayout PresentationGlobeLayout=GetAftershockGlobeLayout(Terrain);
		const auto PresentationHeightAt=[&](double X,double Y,double FlatSourceHeight)
		{
			const double ExistingHeight=FlatSourceHeight+CurvatureAt(X,Y);
			if(!bOverview)return ExistingHeight; // V1/V2 and the exact V3 detail surface stay unchanged.
			// Only the OUTER broad-context display band approaches the actual Earth.
			// Cap each20% band at its detail-source margin so every fine-source edge,
			// including negative-MSL construction, keeps its original height and normal.
			const double BandX=(SouthEast.X-NorthWest.X)*.20,BandY=(SouthEast.Y-NorthWest.Y)*.20;
			const auto EdgeWeight=[](double Distance,double DetailMargin,double MaxBand)
			{
				const double Band=FMath::Max(1.,FMath::Min(MaxBand,FMath::Max(0.,DetailMargin)));
				const double T=FMath::Clamp(Distance/Band,0.,1.);
				return T*T*(3.-2.*T);
			};
			const double West=EdgeWeight(X-NorthWest.X,DetailNW.X-NorthWest.X,BandX);
			const double East=EdgeWeight(SouthEast.X-X,SouthEast.X-DetailSE.X,BandX);
			const double North=EdgeWeight(Y-NorthWest.Y,DetailNW.Y-NorthWest.Y,BandY);
			const double South=EdgeWeight(SouthEast.Y-Y,SouthEast.Y-DetailSE.Y,BandY);
			// Product keeps the corner join C1 as well; min() would add diagonal normal ridges.
			const double ReliefWeight=West*East*North*South;
			if(ReliefWeight>=1.)return ExistingHeight;
			const double Radius=static_cast<double>(PresentationGlobeLayout.RadiusCm);
			const double LX=X-PresentationGlobeLayout.Center.X,LY=Y-PresentationGlobeLayout.Center.Y;
			const double R2=FMath::Min(LX*LX+LY*LY,Radius*Radius-1.);
			// Use the SAME actual planet radius/centre, with a stable sag expression.
			const double SphereZ=PresentationGlobeLayout.Center.Z+Radius-R2/(FMath::Sqrt(FMath::Max(1.,Radius*Radius-R2))+Radius);
			// A half-metre presentation clearance prevents coplanar planet/context
			// depth fighting. The existing closed skirt and planet underlap stay in place.
			// This is not a source DEM, shoreline, water level or gameplay-height edit.
			constexpr double JoinClearanceCm=50.;
			return FMath::Lerp(SphereZ+JoinClearanceCm,ExistingHeight,ReliefWeight);
		};
		const auto GroundCovered=[&](FIntPoint Coord){return IsAftershockInstalledGroundCoverage(Terrain,Draw,Coord);};
		const auto GroundCoveredAt=[&](double WX,double WY)
		{
			return GroundCovered(FIntPoint(FMath::FloorToInt((WX-Terrain.GridOrigin.X)/Terrain.CellSize),
				FMath::FloorToInt((WY-Terrain.GridOrigin.Y)/Terrain.CellSize)));
		};
		const auto AtGroundBoundary=[&](double WX,double WY)
		{
			// Four tiny probes also detect a concave/disconnected corner. Sampling
			// the same canonical surface joins every actual installed-ground edge.
			constexpr double Epsilon=.01;
			const bool A=GroundCoveredAt(WX-Epsilon,WY-Epsilon),B=GroundCoveredAt(WX+Epsilon,WY-Epsilon);
			const bool C=GroundCoveredAt(WX-Epsilon,WY+Epsilon),D=GroundCoveredAt(WX+Epsilon,WY+Epsilon);
			return (A||B||C||D)&&!(A&&B&&C&&D);
		};
		TArray<double> Xs,Ys;Xs.Reserve(Segments+3);Ys.Reserve(Segments+3);
		for(int32 I=0;I<=Segments;++I){Xs.Add(FMath::Lerp(NorthWest.X,SouthEast.X,static_cast<double>(I)/Segments));Ys.Add(FMath::Lerp(NorthWest.Y,SouthEast.Y,static_cast<double>(I)/Segments));}
		// Split at actual coverage transitions, not the rectangle enclosing them.
		// Missing chunks, non-playable parcels and sea keep their source context.
		// At most126 cell edges per axis are added in the initial dense window;
		// streamed windows are75 cells. This remains a bounded context mesh.
		TSet<int32> BoundaryXs,BoundaryYs;
		for(int32 Y=Draw.Min.Y;Y<Draw.Max.Y;++Y)for(int32 X=Draw.Min.X;X<Draw.Max.X;++X)
		{
			const FIntPoint Coord(X,Y);if(!GroundCovered(Coord))continue;
			if(!GroundCovered(Coord+FIntPoint(-1,0)))BoundaryXs.Add(X);
			if(!GroundCovered(Coord+FIntPoint(1,0)))BoundaryXs.Add(X+1);
			if(!GroundCovered(Coord+FIntPoint(0,-1)))BoundaryYs.Add(Y);
			if(!GroundCovered(Coord+FIntPoint(0,1)))BoundaryYs.Add(Y+1);
		}
		for(int32 X:BoundaryXs){const double WX=Terrain.GridOrigin.X+X*Terrain.CellSize;if(WX>NorthWest.X&&WX<SouthEast.X)Xs.Add(WX);}
		for(int32 Y:BoundaryYs){const double WY=Terrain.GridOrigin.Y+Y*Terrain.CellSize;if(WY>NorthWest.Y&&WY<SouthEast.Y)Ys.Add(WY);}
		if(bOverview){Xs.Add(DetailNW.X);Xs.Add(DetailSE.X);Ys.Add(DetailNW.Y);Ys.Add(DetailSE.Y);}
		Xs.Sort();Ys.Sort();
		for(int32 I=Xs.Num()-1;I>0;--I)if(FMath::Abs(Xs[I]-Xs[I-1])<.001)Xs.RemoveAt(I);
		for(int32 I=Ys.Num()-1;I>0;--I)if(FMath::Abs(Ys[I]-Ys[I-1])<.001)Ys.RemoveAt(I);
		if(!bOverview){DetailXs=Xs;DetailYs=Ys;}
		const auto SampleSourceHeight=[&](double SampleX,double SampleY,double& OutHeight)
		{
			// Convert the same source-local tangent frame without an extra terrain
			// lookup: each derivative tap reads one already cached geographic sample.
			const FVector2D Metres((SampleX-Terrain.GridOrigin.X-Terrain.GridWidth*Terrain.CellSize*.5)*.01,
				(SampleY-Terrain.GridOrigin.Y-Terrain.GridHeight*Terrain.CellSize*.5)*.01);
			double Latitude=0.,Longitude=0.;FAftershockGeographicTerrainSample Source;
			if(!AftershockFocusLocalMetresToLatLon(Terrain.GeographyIndex,Terrain.GeographyFocusIndex,Metres,Latitude,Longitude)
				||!SourceAt(Latitude,Longitude,Source))return false;
			OutHeight=PresentationHeightAt(SampleX,SampleY,Source.bOcean?Terrain.CoastalSeaLevelCm+4.:Terrain.GridOrigin.Z+Source.ElevationMetres*100.);return true;
		};
		const auto SampleVertex=[&](double WX,double WY,FGlobeVertex& V)
		{
			double Latitude=0,Longitude=0;
			const FVector2D LocalMetres((WX-Terrain.GridOrigin.X-Terrain.GridWidth*Terrain.CellSize*.5)*.01,
				(WY-Terrain.GridOrigin.Y-Terrain.GridHeight*Terrain.CellSize*.5)*.01);
			if(!AftershockFocusLocalMetresToLatLon(Terrain.GeographyIndex,Terrain.GeographyFocusIndex,LocalMetres,Latitude,Longitude))return false;
			// Round-off at an exact provider edge is clamped only after confirming
			// the point belongs to that bounding rectangle; no outside fill exists.
			if(Latitude<Bounds.South-1.e-9||Latitude>Bounds.North+1.e-9||Longitude<Bounds.West-1.e-9||Longitude>Bounds.East+1.e-9)return false;
			FAftershockGeographicTerrainSample Sample;
			if(!SourceAt(FMath::Clamp(Latitude,Bounds.South,Bounds.North),FMath::Clamp(Longitude,Bounds.West,Bounds.East),Sample))return false;
			float Height=PresentationHeightAt(WX,WY,Sample.bOcean?Terrain.CoastalSeaLevelCm+4.f:Terrain.GridOrigin.Z+Sample.ElevationMetres*100.f);
			if(AtGroundBoundary(WX,WY)&&!Sample.bOcean)
			{
				const float GX=(WX-Terrain.GridOrigin.X)/Terrain.CellSize-.5f,GY=(WY-Terrain.GridOrigin.Y)/Terrain.CellSize-.5f;
				Height=SampleAftershockResidentElevation(Terrain,GX,GY);
				const float DX=SampleAftershockResidentElevation(Terrain,GX+.25f,GY)-SampleAftershockResidentElevation(Terrain,GX-.25f,GY);
				const float DY=SampleAftershockResidentElevation(Terrain,GX,GY+.25f)-SampleAftershockResidentElevation(Terrain,GX,GY-.25f);
				V.Normal=FVector(-DX,-DY,Terrain.CellSize*.5f).GetSafeNormal();
			}
			else if(!Sample.bOcean)
			{
				// Source DEM normals apply to every land vertex, including seam-fan
				// centres. Finite differences never step outside the real source box;
				// its edge uses a one-sided difference against the existing centre.
				constexpr double StepCm=1000.; // Ten real metres, independent of display LOD.
				const double Left=FMath::Max(NorthWest.X,WX-StepCm),Right=FMath::Min(SouthEast.X,WX+StepCm);
				const double North=FMath::Max(NorthWest.Y,WY-StepCm),South=FMath::Min(SouthEast.Y,WY+StepCm);
				double HL=Height,HR=Height,HN=Height,HS=Height;
				const bool ValidL=FMath::Abs(Left-WX)<.001||SampleSourceHeight(Left,WY,HL);
				const bool ValidR=FMath::Abs(Right-WX)<.001||SampleSourceHeight(Right,WY,HR);
				const bool ValidN=FMath::Abs(North-WY)<.001||SampleSourceHeight(WX,North,HN);
				const bool ValidS=FMath::Abs(South-WY)<.001||SampleSourceHeight(WX,South,HS);
				const double DX=ValidL&&ValidR&&Right-Left>.001?(HR-HL)/(Right-Left):0.;
				const double DY=ValidN&&ValidS&&South-North>.001?(HS-HN)/(South-North):0.;
				V.Normal=FVector(-DX,-DY,1.).GetSafeNormal();
			}
			else V.Normal=FVector::UpVector;
			V.SurfaceIndex=bOverview?1:0;
			V.Position=FVector(WX,WY,Height);V.UV=FVector2f(WX*.005,WY*.005);
			const float Dry=FMath::Clamp(Sample.ElevationMetres/650.f,0.f,1.f);
			V.Color=FLinearColor::LerpUsingHSV(FLinearColor(.20f,.30f,.10f),FLinearColor(.40f,.39f,.23f),Dry);
			if(Sample.bMappedForest)V.Color=FLinearColor(.13f,.24f,.10f);
			if(Sample.bMappedAgriculture)V.Color=FLinearColor(.34f,.36f,.16f);
			if(Terrain.ContinuationGeneratorVersion<3&&Sample.ElevationMetres>1600.f)V.Color=FLinearColor::LerpUsingHSV(V.Color,FLinearColor(.85f,.88f,.94f),FMath::Clamp((Sample.ElevationMetres-1600.f)/700.f,0.f,1.f));
			// This existing material reserves B>.65 for water. Snow/rock tint
			// cannot accidentally activate its ocean marker.
			V.Color.B=FMath::Min(.60f,V.Color.B);
			if(Sample.bOcean||Sample.bInlandWater)V.Color=FLinearColor(.015f,.095f,1.f,1.f);
			V.Color.A=0.f; // Use bounded DEM/OSM ground, not the legacy annulus-to-globe blend.
			return true;
		};
		TArray<FGlobeVertex> Vertices;TArray<int32> Indices;TBitArray<> Valid(false,Xs.Num()*Ys.Num());
		Vertices.SetNum(Xs.Num()*Ys.Num());
		for(int32 Y=0;Y<Ys.Num();++Y)for(int32 X=0;X<Xs.Num();++X)
		{
			const int32 At=X+Y*Xs.Num();Valid[At]=SampleVertex(Xs[X],Ys[Y],Vertices[At]);
		}
		for(int32 Y=0;Y<Ys.Num()-1;++Y)for(int32 X=0;X<Xs.Num()-1;++X)
		{
			const double MidX=(Xs[X]+Xs[X+1])*.5,MidY=(Ys[Y]+Ys[Y+1])*.5;
			if(GroundCoveredAt(MidX,MidY)||(bOverview&&DetailCovered(MidX,MidY)))continue;
			const int32 A=X+Y*Xs.Num(),B=A+1,C=A+Xs.Num(),D=C+1;
			if(!Valid[A]||!Valid[B]||!Valid[C]||!Valid[D])continue;
			const auto OnHoleEdge=[&](int32 P,int32 Q)
			{
				const FVector& F=Vertices[P].Position;const FVector& T=Vertices[Q].Position;
				const double MX=(F.X+T.X)*.5,MY=(F.Y+T.Y)*.5;
				return (FMath::Abs(F.X-T.X)<.001||FMath::Abs(F.Y-T.Y)<.001)&&(AtGroundBoundary(MX,MY)||AtDetailBoundary(MX,MY));
			};
			if(!OnHoleEdge(A,C)&&!OnHoleEdge(C,D)&&!OnHoleEdge(D,B)&&!OnHoleEdge(B,A)){Indices.Append({A,C,B,B,C,D});continue;}
			TArray<int32,TInlineAllocator<64>> Perimeter;
			const auto Edge=[&](int32 P,int32 Q)
			{
				Perimeter.Add(P);if(!OnHoleEdge(P,Q))return;
				const FVector From=Vertices[P].Position,To=Vertices[Q].Position;
				const bool Vertical=FMath::Abs(From.X-To.X)<.001;
				const double Start=Vertical?From.Y:From.X,End=Vertical?To.Y:To.X,Origin=Vertical?Terrain.GridOrigin.Y:Terrain.GridOrigin.X;
				const double Step=Terrain.CellSize*.5;
				TArray<double,TInlineAllocator<64>> Samples;
				if(AtDetailBoundary((From.X+To.X)*.5,(From.Y+To.Y)*.5))
				{
					for(double S:(Vertical?DetailYs:DetailXs))if(S>FMath::Min(Start,End)+.001&&S<FMath::Max(Start,End)-.001)Samples.Add(S);
				}
				else for(int32 K=FMath::FloorToInt((FMath::Min(Start,End)-Origin)/Step)+1;K<FMath::CeilToInt((FMath::Max(Start,End)-Origin)/Step);++K)Samples.Add(Origin+K*Step);
				if(End<Start)Algo::Reverse(Samples);
				for(double S:Samples)
				{
					FGlobeVertex V;if(SampleVertex(Vertical?From.X:S,Vertical?S:From.Y,V)){Perimeter.Add(Vertices.Num());Vertices.Add(V);}
				}
			};
			Edge(A,C);Edge(C,D);Edge(D,B);Edge(B,A);
			FGlobeVertex Centre;if(!SampleVertex(MidX,MidY,Centre))continue;
			const int32 CentreIndex=Vertices.Num();Vertices.Add(Centre);
			for(int32 I=0;I<Perimeter.Num();++I)Indices.Append({CentreIndex,Perimeter[I],Perimeter[(I+1)%Perimeter.Num()]});
		}
		if(bOverview)
		{
			// A real source edge can finish1.5km above MSL. The planet's exact
			// footprint cut otherwise exposes an open side into black background.
			// Close it below the actual tessellated sphere without changing ANY
			// existing top/source vertex. This hidden skirt is presentation only.
			TArray<double> Angles;GetAftershockGlobeLatitudeAngles(true,Angles);
			const auto AddSkirt=[&](int32 A,int32 B)
			{
				if(!Valid[A]||!Valid[B])return;
				const int32 Start=Vertices.Num();
				for(int32 Index:{A,B})
				{
					const FGlobeVertex Top=Vertices[Index];FGlobeVertex Bottom=Top;
					const double X=Top.Position.X-Terrain.GridOrigin.X-Terrain.GridWidth*Terrain.CellSize*.5;
					const double Y=Top.Position.Y-Terrain.GridOrigin.Y-Terrain.GridHeight*Terrain.CellSize*.5;
					constexpr double R=637100000.;const double R2=FMath::Min(X*X+Y*Y,R*R-1.);
					const double Theta=FMath::Asin(FMath::Sqrt(R2)/R);int32 Band=1;
					while(Band<Angles.Num()-1&&Angles[Band]<Theta)++Band;
					const double Diameter=Angles[Band]-Angles[Band-1]+FMath::Sin(Angles[Band])*(2.*UE_DOUBLE_PI/128.);
					const double ChordGuard=(R*(1.-FMath::Cos(Diameter))+100.)/FMath::Max(.5,FMath::Cos(Angles[Band]));
					const double SphereZ=Terrain.GridOrigin.Z-R+FMath::Sqrt(R*R-R2);
					Bottom.Position.Z=FMath::Min(Top.Position.Z,SphereZ)-ChordGuard;
					// Keep the shared edge's optical normal/UV; this is not a new
					// surveyed cliff or a material/height authority of its own.
					Vertices.Add(Top);Vertices.Add(Bottom);
				}
				Indices.Append({Start,Start+1,Start+2,Start+2,Start+1,Start+3});
			};
			for(int32 Y=0;Y<Ys.Num()-1;++Y)AddSkirt(Y*Xs.Num(),(Y+1)*Xs.Num());
			for(int32 X=0;X<Xs.Num()-1;++X)AddSkirt((Ys.Num()-1)*Xs.Num()+X,(Ys.Num()-1)*Xs.Num()+X+1);
			for(int32 Y=Ys.Num()-1;Y>0;--Y)AddSkirt(Y*Xs.Num()+Xs.Num()-1,(Y-1)*Xs.Num()+Xs.Num()-1);
			for(int32 X=Xs.Num()-1;X>0;--X)AddSkirt(X,X-1);
		}
		const int32 Base=AllVertices.Num();AllVertices.Append(Vertices);
		for(int32 Index:Indices)AllIndices.Add(Base+Index);
		return true;
		};
		if(!BuildLayer(false)||(HasOverview&&!BuildLayer(true)))return false;
		UStaticMesh* Context=MakeGlobeMesh(Owner,TEXT("Aftershock_RealRegionalContext"),AllVertices,AllIndices,Component->GetMaterial(0),HasOverview?Component->GetMaterial(1):nullptr);
		if(!Context)return false;
		// SetStaticMesh defers old proxy teardown. Never release its buffers
		// before that proxy is removed; normal fenced GC handles retirement.
		Component->SetStaticMesh(Context);Cache.Context=Context;Cache.ContextSignature=Signature;Cache.bContextReady=true;++Cache.ContextBuilds;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_REAL_REGIONAL_CONTEXT vertices=%d triangles=%d sourceSegments=%d overviewSegments=%d overview=%d sourceVersion=%d contextBuilds=%d realMetres=1 mutuallyExclusiveLayers=1 overviewEarthCurvature=1 canonicalLocalTerrainUnbent=1 noOutsideSourceFill=1 canonicalWrites=0"),AllVertices.Num(),AllIndices.Num()/3,DetailSegments,HasOverview?OverviewSegments:0,HasOverview,Terrain.ContinuationGeneratorVersion,Cache.ContextBuilds);
		return true;
	}
}

FAftershockRegionalMeshBuildStats GetAftershockRegionalMeshBuildStats(const UWorld* World)
{
	FAftershockRegionalMeshBuildStats Stats;
	for(const auto& Pair:RegionalMeshCaches)
	{
		const auto* Component=Pair.Key.Get();if(!Component||Component->GetWorld()!=World)continue;
		++Stats.CachedOwners;Stats.PlanetBuilds+=Pair.Value.PlanetBuilds;Stats.ContextBuilds+=Pair.Value.ContextBuilds;
		Stats.ActiveMeshReferences+=(Pair.Value.Planet.IsValid()?1:0)+(Pair.Value.Clouds.IsValid()?1:0)+(Pair.Value.Atmosphere.IsValid()?1:0)+(Pair.Value.Context.IsValid()?1:0);
	}
	return Stats;
}

FAftershockGlobeLayout GetAftershockGlobeLayout(const UTerrainGridSubsystem& Terrain)
{
	FAftershockGlobeLayout Layout;
	Layout.GridSpanCm = FMath::Max(Terrain.GridWidth, Terrain.GridHeight) * Terrain.CellSize;
	Layout.RadiusCm = Terrain.IsUsingRealGeography()?637100000.f:FMath::Max(1000.f, Layout.GridSpanCm * 8.f);
	// Canonical elevation is an absolute world Z, as in GridToWorld().
	Layout.Center = FVector(Terrain.GridOrigin.X + Terrain.GridWidth * Terrain.CellSize * 0.5f,
		Terrain.GridOrigin.Y + Terrain.GridHeight * Terrain.CellSize * 0.5f, Terrain.IsUsingRealGeography()?Terrain.GridOrigin.Z-Layout.RadiusCm:-Layout.RadiusCm);
	return Layout;
}

bool BuildAftershockRegionalGlobe(AActor* Owner, const UTerrainGridSubsystem& Terrain,
	UStaticMeshComponent* GlobeComponent, UStaticMeshComponent* TransitionComponent,
	UStaticMeshComponent* AtmosphereComponent, UStaticMeshComponent* CloudComponent)
{
	if (!Owner || Terrain.Cells.IsEmpty() || !GlobeComponent || !TransitionComponent || !AtmosphereComponent || !CloudComponent) return false;
	const FAftershockGlobeLayout Layout = GetAftershockGlobeLayout(Terrain);
	const float Radius = Layout.RadiusCm;
	const FVector Center = Layout.Center;
	FVector GeoEast,GeoSouth,GeoUp;
	GetAftershockGeographicFocusBasis(Terrain.GeographyIndex,Terrain.GeographyFocusIndex,GeoEast,GeoSouth,GeoUp);
	for (UStaticMeshComponent* Component:{GlobeComponent,TransitionComponent,AtmosphereComponent,CloudComponent})
	{
		UMaterialInstanceDynamic* MID=Cast<UMaterialInstanceDynamic>(Component->GetMaterial(0));
		if (!MID) MID=Component->CreateAndSetMaterialInstanceDynamic(0);
		if (!MID) continue;
		MID->SetVectorParameterValue(TEXT("GeoEast"),FLinearColor(GeoEast.X,GeoEast.Y,GeoEast.Z));
		MID->SetVectorParameterValue(TEXT("GeoSouth"),FLinearColor(GeoSouth.X,GeoSouth.Y,GeoSouth.Z));
		MID->SetVectorParameterValue(TEXT("GeoUp"),FLinearColor(GeoUp.X,GeoUp.Y,GeoUp.Z));
		if(Component==TransitionComponent&&!Terrain.IsUsingRealGeography())MID->SetScalarParameterValue(TEXT("RegionalSourceMaskEnabled"),0.f);
	}
	TArray<FGlobeVertex> Vertices;
	TArray<int32> Indices;
	auto& Cache=RegionalCacheFor(GlobeComponent);
	const auto Budget=GetAftershockPresentationBudget(Owner->GetWorld());
	const bool RealGeography=Terrain.IsUsingRealGeography();
	const bool NeedPlanet=!RealGeography||Budget.Tier==EAftershockPresentationTier::Regional||Budget.Tier==EAftershockPresentationTier::Globe;
	const uint64 PlanetSignature=static_cast<uint64>(GetTypeHash(Layout.Center))<<32
		^GetTypeHash(Layout.RadiusCm)^static_cast<uint64>(GetTypeHash(RealGeography));
	if(NeedPlanet&&(!Cache.bPlanetReady||Cache.PlanetSignature!=PlanetSignature||GlobeComponent->GetStaticMesh()!=Cache.Planet.Get()
		||CloudComponent->GetStaticMesh()!=Cache.Clouds.Get()||AtmosphereComponent->GetStaticMesh()!=Cache.Atmosphere.Get()))
	{
	const int32 LongitudeSegments=RealGeography?128:192;
	TArray<double> LatitudeAngles;GetAftershockGlobeLatitudeAngles(RealGeography,LatitudeAngles);
	const int32 LatitudeSegments=LatitudeAngles.Num()-1;
	Vertices.Reserve((LongitudeSegments + 1) * (LatitudeSegments + 1));
	for (int32 Lat = 0; Lat <= LatitudeSegments; ++Lat)
	{
		const double Theta = LatitudeAngles[Lat];
		for (int32 Lon = 0; Lon <= LongitudeSegments; ++Lon)
		{
			const float Phi = 2.f * PI * Lon / LongitudeSegments;
			FGlobeVertex& V = Vertices.AddDefaulted_GetRef();
			// Double angles keep the one-kilometre cap's centimetre sea curvature.
			// The legacy branch retains its original float trigonometry.
			V.Normal = RealGeography
				?FVector(FMath::Sin(Theta)*FMath::Cos(static_cast<double>(Phi)),FMath::Sin(Theta)*FMath::Sin(static_cast<double>(Phi)),FMath::Cos(Theta))
				:FVector(FMath::Sin(static_cast<float>(Theta))*FMath::Cos(Phi),FMath::Sin(static_cast<float>(Theta))*FMath::Sin(Phi),FMath::Cos(static_cast<float>(Theta)));
			V.Position = Center + V.Normal * Radius;
			V.UV = FVector2f(static_cast<float>(Lon) / LongitudeSegments,RealGeography?static_cast<float>(Theta/UE_DOUBLE_PI):static_cast<float>(Lat)/LatitudeSegments);
			if (Lat < LatitudeSegments && Lon < LongitudeSegments)
			{
				const int32 A = Lon + Lat * (LongitudeSegments + 1);
				Indices.Append({ A, A + 1, A + LongitudeSegments + 1, A + 1, A + LongitudeSegments + 2, A + LongitudeSegments + 1 });
			}
		}
	}
	UStaticMesh* Globe = MakeGlobeMesh(Owner, TEXT("Aftershock_RegionalGlobe"), Vertices, Indices, GlobeComponent->GetMaterial(0));
	if (!Globe) return false;
	GlobeComponent->SetStaticMesh(Globe);
	const float CloudRadius=RealGeography?Radius+1200000.f:Radius*1.006f; // Real12km cloud layer; preserve V1 scale.
	for (FGlobeVertex& Vertex : Vertices) Vertex.Position = Center + Vertex.Normal * CloudRadius;
	CloudComponent->SetStaticMesh(MakeGlobeMesh(Owner, TEXT("Aftershock_ClimateClouds"), Vertices, Indices, CloudComponent->GetMaterial(0)));
	for (FGlobeVertex& Vertex : Vertices) Vertex.Position = Center + Vertex.Normal * Radius * 1.014f;
	AtmosphereComponent->SetStaticMesh(MakeGlobeMesh(Owner, TEXT("Aftershock_RegionalAtmosphere"), Vertices, Indices, AtmosphereComponent->GetMaterial(0)));
	if(!CloudComponent->GetStaticMesh()||!AtmosphereComponent->GetStaticMesh())return false;
	Cache.Planet=Globe;Cache.Clouds=CloudComponent->GetStaticMesh();Cache.Atmosphere=AtmosphereComponent->GetStaticMesh();
	Cache.PlanetSignature=PlanetSignature;Cache.bPlanetReady=true;++Cache.PlanetBuilds;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PLANET_GEOMETRY_CACHE builds=%d longitudeSegments=%d latitudeSegments=%d extraAnalyticNearCapRings=%d meshObjectsPerLayout=3 weakCacheLimit=32 canonicalWrites=0 inventedDEMOutsideSource=0"),Cache.PlanetBuilds,LongitudeSegments,LatitudeSegments,RealGeography?9:0);
	}
	if(RealGeography)
	{
		// V2 is real-distance terrain. A fictional annulus scaled from the city's
		// small initial rectangle would invent an island; use bounded DEM/OSM.
		return BuildRealRegionalContext(Owner,Terrain,TransitionComponent,Budget.GeometryStride,Cache);
	}

	Vertices.Reset(); Indices.Reset();
	constexpr int32 RingSegments = 256, RingSteps = 40;
	const FIntRect Resident=Terrain.GetResidentCellBounds(),Visible=GetAftershockVisibleCellBounds(Terrain);
	const bool bExpanded=Resident!=FIntRect(0,0,Terrain.GridWidth,Terrain.GridHeight)||Terrain.Cells.Num()!=Terrain.GridWidth*Terrain.GridHeight;
	const FVector ApronCenter=bExpanded?FVector(Terrain.GridOrigin.X+(Visible.Min.X+Visible.Max.X)*Terrain.CellSize*.5f,Terrain.GridOrigin.Y+(Visible.Min.Y+Visible.Max.Y)*Terrain.CellSize*.5f,0):Center;
	const float RX = (bExpanded?Visible.Width()*.5f:Terrain.GridWidth*.485f)*Terrain.CellSize;
	const float RY = (bExpanded?Visible.Height()*.5f:Terrain.GridHeight*.485f)*Terrain.CellSize;
	const float BaseGridX=bExpanded?(ApronCenter.X-Terrain.GridOrigin.X)/Terrain.CellSize:Terrain.GridWidth*.5f;
	const float BaseGridY=bExpanded?(ApronCenter.Y-Terrain.GridOrigin.Y)/Terrain.CellSize:Terrain.GridHeight*.5f;
	for (int32 Ring = 0; Ring <= RingSteps; ++Ring)
	{
		const float T = static_cast<float>(Ring) / RingSteps;
		for (int32 Segment = 0; Segment <= RingSegments; ++Segment)
		{
			const float Angle = 2.f * PI * Segment / RingSegments;
			const float C=FMath::Cos(Angle),S=FMath::Sin(Angle);
			const float RectangleScale=bExpanded?1.f/FMath::Max(FMath::Abs(C),FMath::Abs(S)):1.f;
			const float DX = C * RX*RectangleScale, DY = S * RY*RectangleScale;
			const float EdgeRadius = FMath::Sqrt(DX * DX + DY * DY);
			const float InnerScale = bExpanded?1.f:0.95f;
			// The far boundary is intentionally irregular: no visible circular biome stamp.
			const float OuterScale = 2.8f + 0.18f * FMath::Sin(Angle * 3.f + 0.7f) + 0.12f * FMath::Sin(Angle * 5.f);
			const float Scale = FMath::Lerp(InnerScale, OuterScale, T);
			const float InnerHeight = SampleCanonicalHeight(Terrain, BaseGridX + DX * InnerScale / Terrain.CellSize - .5f,
				BaseGridY + DY * InnerScale / Terrain.CellSize - .5f) - 10.f;
			const float OuterDistance = EdgeRadius * OuterScale;
			const float OuterDX=ApronCenter.X-Center.X+DX*OuterScale,OuterDY=ApronCenter.Y-Center.Y+DY*OuterScale;
			const float OuterRoot = FMath::Sqrt(FMath::Max(1.f, Radius * Radius - (bExpanded?OuterDX*OuterDX+OuterDY*OuterDY:OuterDistance * OuterDistance)));
			const float OuterHeight = Center.Z + OuterRoot + 2.f;
			const float OuterSlope = -(bExpanded?(OuterDX*DX+OuterDY*DY)/FMath::Max(1.f,EdgeRadius):OuterDistance) / OuterRoot;
			const float Length = EdgeRadius * (OuterScale - InnerScale);
			// Cubic Hermite joins a quiet land slope to the exact sphere tangent.
			// Geometry outside the city is presentation only and never queried by gameplay.
			const float T2 = T * T, T3 = T2 * T;
			const float HermiteHeight = (2*T3 - 3*T2 + 1)*InnerHeight + (-2*T3 + 3*T2)*OuterHeight
				+ (T3 - T2) * Length * OuterSlope;
			const float SampleX = BaseGridX + DX * Scale / Terrain.CellSize - .5f;
			const float SampleY = BaseGridY + DY * Scale / Terrain.CellSize - .5f;
			const float CanonicalHeight = SampleCanonicalHeight(Terrain, SampleX, SampleY) - 10.f;
			// Match the actual edge samples before easing into the spherical landscape.
			// The slight overlap hides cell-cut tessellation without a raised city rim.
			const float Height = FMath::Lerp(CanonicalHeight, HermiteHeight, FMath::SmoothStep(.025f, .16f, T));
			FGlobeVertex& V = Vertices.AddDefaulted_GetRef();
			V.Position = FVector(ApronCenter.X + DX * Scale, ApronCenter.Y + DY * Scale, Height);
			V.UV = FVector2f(Segment / 64.f, static_cast<float>(Ring));
			const float Dry = FMath::Clamp(CanonicalHeight / 650.f, 0.f, 1.f);
			V.Color = FLinearColor::LerpUsingHSV(FLinearColor(.20f, .30f, .10f), FLinearColor(.40f, .39f, .23f), Dry);
			const FGridCell Cell = Terrain.GetCell(FIntPoint(FMath::Clamp(FMath::RoundToInt(SampleX), Resident.Min.X, Resident.Max.X - 1),
				FMath::Clamp(FMath::RoundToInt(SampleY), Resident.Min.Y, Resident.Max.Y - 1)));
			V.Color = FLinearColor::LerpUsingHSV(V.Color, FLinearColor(.38f, .33f, .22f), FMath::Pow(Cell.RiverProximity, 3.f) * .65f);
			// B=1 is a regional-material ocean marker; ordinary canonical biome
			// blue stays below .24. The sea side must never become a tan plateau.
			if(Cell.bIsOcean) V.Color=FLinearColor(.015f,.095f,1.f,1.f);
			V.Color.A = FMath::SmoothStep(0.10f, 0.93f, T);
			if (Ring < RingSteps && Segment < RingSegments)
			{
				const int32 A = Segment + Ring * (RingSegments + 1);
				Indices.Append({ A, A + 1, A + RingSegments + 1, A + 1, A + RingSegments + 2, A + RingSegments + 1 });
			}
		}
	}
	for (int32 Ring = 0; Ring <= RingSteps; ++Ring)
	{
		for (int32 Segment = 0; Segment <= RingSegments; ++Segment)
		{
			const int32 S = Segment % RingSegments;
			const FVector Radial = Vertices[S + FMath::Min(Ring + 1, RingSteps)*(RingSegments + 1)].Position
				- Vertices[S + FMath::Max(Ring - 1, 0)*(RingSegments + 1)].Position;
			const FVector Angular = Vertices[(S + 1)%RingSegments + Ring*(RingSegments + 1)].Position
				- Vertices[(S + RingSegments - 1)%RingSegments + Ring*(RingSegments + 1)].Position;
			Vertices[Segment + Ring*(RingSegments + 1)].Normal = FVector::CrossProduct(Radial, Angular).GetSafeNormal();
		}
	}
	UStaticMesh* Transition = MakeGlobeMesh(Owner, TEXT("Aftershock_RegionalTransition"), Vertices, Indices, TransitionComponent->GetMaterial(0));
	if (!Transition) return false;
	TransitionComponent->SetStaticMesh(Transition);
	const auto& Geo=GetAftershockGeographyFocus(Terrain.GeographyIndex,Terrain.GeographyFocusIndex);
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_REGIONAL_GLOBE radiusCm=%.0f gridSpanCm=%.0f cityDiameterToPlanet=%.4f transitionTriangles=%d cartography=NaturalEarth110m anchorLatitude=%.6f anchorLongitude=%.6f neighbourhoodCompressedNot1to1=1 gameplayCoordinatesChanged=0"),
		Radius, Layout.GridSpanCm, .485f * Layout.GridSpanCm / Radius, Indices.Num()/3,Geo.Latitude,Geo.Longitude);
	return CloudComponent->GetStaticMesh() && AtmosphereComponent->GetStaticMesh();
}
