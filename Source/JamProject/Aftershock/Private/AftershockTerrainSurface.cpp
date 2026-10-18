#include "AftershockTerrainSurface.h"
#include "AftershockPresentationBudget.h"
#include "AftershockRuntimeMeshUV.h"
#include "AftershockRegionalSurfaceStyle.h"
#include "AftershockHydraulicTerrain.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "HAL/PlatformTime.h"

#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "PhysicsEngine/BodySetup.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

namespace
{
	struct FTerrainSurfaceCacheEntry
	{
		TWeakObjectPtr<UStaticMeshComponent> WaterComponent;
		TWeakObjectPtr<UStaticMesh> GroundMesh, WaterMesh;
		uint64 GeometrySignature = 0;
		bool bHasSignature = false;
		FAftershockTerrainSurfaceBuildStats Stats;
	};
	// Weak only: never keep old transient meshes or departed worlds alive. One
	// current pair per component, at most64 entries even during editor world churn.
	TMap<TWeakObjectPtr<UStaticMeshComponent>, FTerrainSurfaceCacheEntry> TerrainSurfaceCache;
	struct FSeaSurfaceCacheEntry { uint64 Signature=0; bool bHasSignature=false; TWeakObjectPtr<UStaticMesh> Mesh; };
	TMap<TWeakObjectPtr<UStaticMeshComponent>, FSeaSurfaceCacheEntry> SeaSurfaceCache;
	void PruneTerrainSurfaceCache()
	{
		for (auto It = TerrainSurfaceCache.CreateIterator(); It; ++It)
			if (!It.Key().IsValid()) It.RemoveCurrent();
		while (TerrainSurfaceCache.Num() > 64) TerrainSurfaceCache.Remove(TerrainSurfaceCache.CreateConstIterator().Key());
	}
	void MixGeometryHash(uint64& Hash, uint64 Value) { Hash = (Hash ^ Value) * 1099511628211ull; }
}

uint64 GetAftershockTerrainGeometrySignature(const UTerrainGridSubsystem& Terrain, const FIntRect& Bounds)
{
	uint64 Hash = 1469598103934665603ull;
	MixGeometryHash(Hash,Terrain.IsUsingRealGeography()?2u:1u);
	for (uint32 Value : {GetTypeHash(Terrain.TerrainSeed), GetTypeHash(Terrain.CellSize), GetTypeHash(Terrain.GridOrigin),
		GetTypeHash(Terrain.GridWidth), GetTypeHash(Terrain.GridHeight), GetTypeHash(Terrain.GetResidentCellBounds().Min),
		GetTypeHash(Terrain.GetResidentCellBounds().Max), GetTypeHash(Bounds.Min), GetTypeHash(Bounds.Max)}) MixGeometryHash(Hash, Value);
	// Surface normals sample up to one neighbouring canonical quad. Include a
	// two-cell guard while keeping work bounded to the requested display window.
	for (int32 Y = Bounds.Min.Y - 2; Y < Bounds.Max.Y + 2; ++Y)
		for (int32 X = Bounds.Min.X - 2; X < Bounds.Max.X + 2; ++X)
		{
			FGridCell Cell; const bool Present = Terrain.TryGetCell(FIntPoint(X,Y), Cell);
			MixGeometryHash(Hash, Present ? 1u : 0u);
			if (!Present) continue;
			MixGeometryHash(Hash, GetTypeHash(Cell.Elevation));
			MixGeometryHash(Hash, GetTypeHash(Cell.HydraulicExcavationCm));
			MixGeometryHash(Hash, GetTypeHash(Cell.HydraulicProfileVersion));
			MixGeometryHash(Hash, GetTypeHash(Cell.GroundErodedDepthM));
			MixGeometryHash(Hash, GetTypeHash(Cell.GroundDepositedDepthM));
			MixGeometryHash(Hash, GetTypeHash(Cell.RiverProximity));
			MixGeometryHash(Hash, (Cell.bIsPlayable?1u:0u)|(Cell.bIsRiverbed?2u:0u)|(Cell.bIsCoastal?4u:0u)|(Cell.bIsOcean?8u:0u));
		}
	return Hash;
}

FAftershockTerrainSurfaceBuildStats GetAftershockTerrainSurfaceBuildStats(const UWorld* World)
{
	FAftershockTerrainSurfaceBuildStats Total;
	for (const auto& Pair : TerrainSurfaceCache)
	{
		const UStaticMeshComponent* Component = Pair.Key.Get();
		if (!Component || Component->GetWorld() != World) continue;
		const auto& S = Pair.Value.Stats;
		Total.GroundBuildAttempts += S.GroundBuildAttempts; Total.SuccessfulGroundBuilds += S.SuccessfulGroundBuilds;
		Total.CollisionCookAttempts += S.CollisionCookAttempts; Total.GroundCacheHits += S.GroundCacheHits;
		++Total.CachedGroundComponents;
		Total.ActiveGroundComponents += Component->GetStaticMesh() && Component->GetStaticMesh() == Pair.Value.GroundMesh.Get() ? 1 : 0;
		Total.LastLOD0Triangles += S.LastLOD0Triangles; Total.LastRenderLODs = FMath::Max(Total.LastRenderLODs, S.LastRenderLODs);
	}
	return Total;
}

void ApplyAftershockTerrainPresentationBudget(UStaticMeshComponent* GroundComponent, const UWorld* World)
{
	if (!GroundComponent || !GroundComponent->GetStaticMesh()) return;
	const auto Budget = GetAftershockPresentationBudget(World);
	const int32 RequestedLOD = Budget.GeometryStride >= 8 ? 4 : Budget.GeometryStride >= 4 ? 3 : Budget.GeometryStride >= 2 ? 2 : 1;
	const int32 LOD = FMath::Clamp(RequestedLOD, 1, FMath::Max(1, GroundComponent->GetStaticMesh()->GetNumLODs()));
	if (GroundComponent->GetForcedLodModel() != LOD) GroundComponent->SetForcedLodModel(LOD);
}

FIntRect GetAftershockVisibleCellBounds(const UTerrainGridSubsystem& Terrain)
{
	const FIntRect Resident=Terrain.GetResidentCellBounds();
	if(Resident.IsEmpty())return Resident;
	const FIntRect Initial(0,0,Terrain.GridWidth,Terrain.GridHeight);
	if(Resident==Initial&&Terrain.Cells.Num()==Terrain.GridWidth*Terrain.GridHeight)return Initial;
	FIntPoint Focus(Resident.Min.X+(Resident.Width()-1)/2,Resident.Min.Y+(Resident.Height()-1)/2);
	bool bHasCamera=false;
	if(const UWorld* World=Terrain.GetWorld())if(const APlayerController* PC=World->GetFirstPlayerController())
		if(const AAftershockCameraPawn* Camera=Cast<AAftershockCameraPawn>(PC->GetPawn()))
		{Focus=Terrain.WorldToGrid(Camera->GetLocalMapFocus());bHasCamera=true;}
	if(!bHasCamera&&Resident.Width()<=75&&Resident.Height()<=75)return Resident;
	if(Focus.X==MAX_int32||Focus.Y==MAX_int32)Focus=FIntPoint(Resident.Min.X+(Resident.Width()-1)/2,Resident.Min.Y+(Resident.Height()-1)/2);
	if(!Terrain.IsValidCoord(Focus))
	{
		const FIntPoint Clamped(FMath::Clamp(Focus.X,Resident.Min.X,Resident.Max.X-1),FMath::Clamp(Focus.Y,Resident.Min.Y,Resident.Max.Y-1));
		if(Terrain.IsValidCoord(Clamped))Focus=Clamped;
		else
		{
			// Exact original closest-CELL semantics, including the old Y/X tie.
			// Full chunks use their AABB closest point in O(number of chunks).
			// Partial legacy/sparse chunks fall back to only their real cells.
			constexpr int32 K=UTerrainGridSubsystem::ChunkSizeCells;
			int64 Best=MAX_int64;FIntPoint Nearest=Resident.Min;TSet<FIntPoint> PartialChunks;
			const auto Consider=[&](FIntPoint Candidate)
			{
				const int64 DX=int64(Candidate.X)-Focus.X,DY=int64(Candidate.Y)-Focus.Y,Distance=DX*DX+DY*DY;
				if(Distance<Best||(Distance==Best&&(Candidate.Y<Nearest.Y||(Candidate.Y==Nearest.Y&&Candidate.X<Nearest.X))))
				{Best=Distance;Nearest=Candidate;}
			};
			for(const auto& ResidentChunk:Terrain.GetResidentChunks())
			{
				const FIntPoint Min=ResidentChunk.ChunkCoord*K,Max=Min+FIntPoint(K-1,K-1);
				const FIntPoint Candidate(FMath::Clamp(Focus.X,Min.X,Max.X),FMath::Clamp(Focus.Y,Min.Y,Max.Y));
				if(ResidentChunk.CellCount==K*K&&Terrain.IsValidCoord(Candidate))Consider(Candidate);
				else PartialChunks.Add(ResidentChunk.ChunkCoord);
			}
			if(!PartialChunks.IsEmpty()||Best==MAX_int64)
				for(const FGridCell& Cell:Terrain.Cells)
					if(PartialChunks.IsEmpty()||PartialChunks.Contains(UTerrainGridSubsystem::CellToChunk(Cell.Coords)))Consider(Cell.Coords);
			Focus=Nearest;
		}
	}
	const FIntPoint Chunk=UTerrainGridSubsystem::CellToChunk(Focus);
	const FIntPoint Min((Chunk.X-1)*25,(Chunk.Y-1)*25),Max((Chunk.X+2)*25,(Chunk.Y+2)*25);
	return FIntRect(FIntPoint(FMath::Max(Min.X,Resident.Min.X),FMath::Max(Min.Y,Resident.Min.Y)),FIntPoint(FMath::Min(Max.X,Resident.Max.X),FMath::Min(Max.Y,Resident.Max.Y)));
}

FIntRect GetAftershockDrawableCellBounds(const UTerrainGridSubsystem& Terrain)
{
	const FIntRect Requested=GetAftershockVisibleCellBounds(Terrain);
	const UWorld* World=Terrain.GetWorld();
	const auto* Crowd=World?World->GetSubsystem<UCrowdRenderSubsystem>():nullptr;FIntRect Installed;
	if(!Crowd||!Crowd->TryGetInstalledTerrainVisualBounds(Installed))return Requested;
	const FIntPoint Min(FMath::Max(Requested.Min.X,Installed.Min.X),FMath::Max(Requested.Min.Y,Installed.Min.Y));
	const FIntPoint Max(FMath::Min(Requested.Max.X,Installed.Max.X),FMath::Min(Requested.Max.Y,Installed.Max.Y));
	return Max.X>Min.X&&Max.Y>Min.Y?FIntRect(Min,Max):FIntRect(0,0,0,0);
}

namespace
{
	// Triangulate the SAME four canonical heights along an existing diagonal
	// river pair. Bilinear interpolation otherwise creates a bank saddle across
	// this 8-neighbour channel. No river cells or elevations are manufactured.
	int32 RiverQuadDiagonal(const FGridCell (&C)[4])
	{
		if(C[0].bIsPlayable&&C[3].bIsPlayable&&C[0].bIsRiverbed&&C[3].bIsRiverbed&&!C[1].bIsRiverbed&&!C[2].bIsRiverbed)return 1;
		if(C[1].bIsPlayable&&C[2].bIsPlayable&&C[1].bIsRiverbed&&C[2].bIsRiverbed&&!C[0].bIsRiverbed&&!C[3].bIsRiverbed)return -1;
		return 0;
	}
	int32 RiverQuadDiagonalAt(const UTerrainGridSubsystem& Terrain,int32 X,int32 Y)
	{
		FGridCell C[4];const FIntPoint Coords[]={FIntPoint(X,Y),FIntPoint(X+1,Y),FIntPoint(X,Y+1),FIntPoint(X+1,Y+1)};
		for(int32 I=0;I<4;++I)if(!Terrain.TryGetCell(Coords[I],C[I]))return 0;
		return RiverQuadDiagonal(C);
	}
}

float SampleAftershockResidentElevation(const UTerrainGridSubsystem& Terrain,float X,float Y)
{
    float Height=Terrain.GridOrigin.Z;
    Terrain.TrySampleResidentElevation(X,Y,Height);
    return Height;
}

namespace
{
	struct FSurfaceVertex
	{
		FVector Position;
		FVector Normal = FVector::UpVector;
		FVector2f UV = FVector2f::ZeroVector;
		FVector2f HydraulicUV = FVector2f::ZeroVector; // Exact cm rise/depth; physical flood only.
		FVector2f ArrivalUV = FVector2f::ZeroVector; // River coverage / exact arrival.
		FLinearColor Color = FLinearColor::White;
	};

	// Cell centres remain exact. Interpolation only joins the display surface between
	// samples; elevation, river membership, placement and flood data stay in the grid.
	float SampleElevation(const UTerrainGridSubsystem& Terrain, float GridX, float GridY)
	{
		return SampleAftershockResidentElevation(Terrain,GridX,GridY);
	}

	UStaticMesh* MakePhysicalFloodSurface(AActor* Owner,const TCHAR* Name,
		const TArray<FSurfaceVertex>& Vertices,const TArray<int32>& Indices,UMaterialInterface* Material)
	{
		if(!Owner||!Material||Vertices.IsEmpty()||Indices.IsEmpty()||Indices.Num()%3!=0)return nullptr;
		// A fresh, display-only resource has no render-thread readers yet. Do not
		// rebuild an already initialized mesh buffer or wait on a GPU fence here.
		TArray<FStaticMeshBuildVertex> BuildVertices;BuildVertices.SetNum(Vertices.Num());
		FBox Bounds(ForceInit);
		for(int32 Index=0;Index<Vertices.Num();++Index)
		{
			const FSurfaceVertex& Source=Vertices[Index];
			if(Source.Position.ContainsNaN()||Source.Normal.ContainsNaN()||Source.UV.ContainsNaN()
				||Source.HydraulicUV.ContainsNaN()||Source.ArrivalUV.ContainsNaN())return nullptr;
			FStaticMeshBuildVertex& Vertex=BuildVertices[Index];
			Vertex.Position=FVector3f(Source.Position);Vertex.TangentZ=FVector3f(Source.Normal.GetSafeNormal());
			FVector Tangent=FVector::VectorPlaneProject(FVector::ForwardVector,Source.Normal).GetSafeNormal();
			if(Tangent.IsNearlyZero())Tangent=FVector::VectorPlaneProject(FVector::RightVector,Source.Normal).GetSafeNormal();
			Vertex.TangentX=FVector3f(Tangent);Vertex.TangentY=FVector3f::CrossProduct(Vertex.TangentZ,Vertex.TangentX);
			Vertex.UVs[0]=Source.UV;Vertex.UVs[1]=Source.HydraulicUV;Vertex.UVs[2]=Source.ArrivalUV;
			// Same compensated DATA conversion as the existing fast mesh path.
			Vertex.Color=Source.Color.ToFColor(true);Bounds+=Source.Position;
		}
		TArray<uint32> RenderIndices;RenderIndices.Reserve(Indices.Num());
		for(int32 Index:Indices){if(!Vertices.IsValidIndex(Index))return nullptr;RenderIndices.Add(static_cast<uint32>(Index));}
		bool bKeepCPUAccess=false;
#if WITH_EDITOR
		bKeepCPUAccess=IsRunningCommandlet()&&Owner->ActorHasTag(TEXT("Aftershock_MeshCpuAudit"));
#endif
		UStaticMesh* Mesh=NewObject<UStaticMesh>(Owner,MakeUniqueObjectName(Owner,UStaticMesh::StaticClass(),Name),RF_Transient);
		Mesh->NeverStream=true;Mesh->bAllowCPUAccess=bKeepCPUAccess;Mesh->bSupportRayTracing=false;Mesh->bDoFastBuild=true;
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material,TEXT("Surface")));
		auto RenderData=MakeUnique<FStaticMeshRenderData>();RenderData->AllocateLODResources(1);
		RenderData->NumInlinedLODs=1;RenderData->CurrentFirstLODIdx=0;RenderData->ScreenSize[0].Default=1.f;
		RenderData->Bounds=FBoxSphereBounds(Bounds);
		FStaticMeshLODResources& LOD=RenderData->LODResources[0];LOD.SourceMeshBounds=RenderData->Bounds;
		LOD.bBuffersInlined=true;LOD.bIsOptionalLOD=false;
		// UE5.8's TArray+NumUV overload expands all MAX_STATIC_TEXCOORDS
		// entries. The public view overload preserves the requested three streams
		// and copies their values synchronously before BuildVertices goes away.
		FConstMeshBuildVertexView View=MakeConstMeshBuildVertexView(MakeArrayView(BuildVertices));View.UVs.SetNum(3);
		// MakeConstMeshBuildVertexView may omit an all-white stream. This shader
		// always consumes color DATA, including a valid all-white top-face mask.
		View.Color=MakeConstStridedView(sizeof(FStaticMeshBuildVertex),&BuildVertices[0].Color,BuildVertices.Num());
		LOD.VertexBuffers.PositionVertexBuffer.Init(View,bKeepCPUAccess);
		LOD.VertexBuffers.StaticMeshVertexBuffer.SetUseFullPrecisionUVs(true);
		FStaticMeshVertexBufferFlags Flags;Flags.bNeedsCPUAccess=bKeepCPUAccess;Flags.bUseBackwardsCompatibleF16TruncUVs=false;
		LOD.VertexBuffers.StaticMeshVertexBuffer.Init(View,Flags);
		LOD.VertexBuffers.ColorVertexBuffer.Init(View,bKeepCPUAccess);
		LOD.bHasColorVertexData=true;
		LOD.IndexBuffer.TrySetAllowCPUAccess(bKeepCPUAccess);
		LOD.IndexBuffer.SetIndices(RenderIndices,Vertices.Num()>65536?EIndexBufferStride::Force32Bit:EIndexBufferStride::Force16Bit);
		FStaticMeshSection& Section=LOD.Sections.AddDefaulted_GetRef();
		Section.MaterialIndex=0;Section.FirstIndex=0;Section.NumTriangles=RenderIndices.Num()/3;
		Section.MinVertexIndex=0;Section.MaxVertexIndex=Vertices.Num()-1;
		Section.bEnableCollision=false;Section.bCastShadow=false;Section.bVisibleInRayTracing=false;Section.bAffectDistanceFieldLighting=false;
#if WITH_EDITOR
		// Editor InitResources resolves these flags from the section-info map.
		Mesh->SetAutoComputeLODScreenSize(true);
		FMeshSectionInfo SectionInfo(0);SectionInfo.bEnableCollision=false;SectionInfo.bCastShadow=false;
		SectionInfo.bVisibleInRayTracing=false;SectionInfo.bAffectDistanceFieldLighting=false;
		Mesh->GetSectionInfoMap().Set(0,0,SectionInfo);
#endif
		Mesh->SetRenderData(MoveTemp(RenderData));
		InitializeAftershockRuntimeMeshUV(*Mesh,Vertices,Indices,true,3);
		Mesh->CalculateExtendedBounds();
		Mesh->CreateBodySetup();
		if(UBodySetup* Body=Mesh->GetBodySetup())
		{Body->CollisionTraceFlag=CTF_UseSimpleAsComplex;Body->DefaultInstance.SetCollisionEnabled(ECollisionEnabled::NoCollision);}
		// All CPU layout/bounds/material setup is finished before resource commands
		// are queued. The component later owns the mesh and normal GC handles release.
		Mesh->InitResources();
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_FLOOD_MESH_LAYOUT vertices=%d triangles=%d uvChannels=%u fullPrecision=%d sharedSourceVertices=1 runtimePublicVertexView=1 collision=0 gpuFenceWait=0"),
			Vertices.Num(),RenderIndices.Num()/3,LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords(),LOD.VertexBuffers.StaticMeshVertexBuffer.GetUseFullPrecisionUVs());
		return Mesh;
	}

	void ReleaseTerrainTransientMeshReference(UStaticMeshComponent* Component)
	{
		// Release the component reference only. Its old render proxy survives
		// until deferred end-of-frame recreation; UObject GC owns the fence.
		if (Component) Component->SetStaticMesh(nullptr);
	}

	void SetTerrainTransientMeshReference(UStaticMeshComponent* Component, UStaticMesh* NewMesh)
	{
		// RF_Transient does not establish exclusive ownership or prove that
		// no other component/MID still needs this object's render resources.
		if (Component) Component->SetStaticMesh(NewMesh);
	}

	UStaticMesh* MakeSurface(AActor* Owner, const TCHAR* Name,
		const TArray<FSurfaceVertex>& Vertices, const TArray<int32>& Indices,
		UMaterialInterface* Material, bool bBuildGroundCollision = false, bool bUV0ContainsFlowDirections = false,
		const TArray<TArray<int32>>* ExtraLODIndices = nullptr, int32 NumUVChannels = 1)
	{
		if (Vertices.IsEmpty() || Indices.IsEmpty()) return nullptr;
		if(NumUVChannels>1)
		{
			if(NumUVChannels!=3||bBuildGroundCollision||(ExtraLODIndices&&!ExtraLODIndices->IsEmpty()))return nullptr;
			return MakePhysicalFloodSurface(Owner,Name,Vertices,Indices,Material);
		}
		const auto FillDescription = [&Vertices,NumUVChannels](FMeshDescription& Description, const TArray<int32>& SourceIndices)
		{
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		auto Positions = Attributes.GetVertexPositions();
		auto Normals = Attributes.GetVertexInstanceNormals();
		auto Tangents = Attributes.GetVertexInstanceTangents();
		auto Signs = Attributes.GetVertexInstanceBinormalSigns();
		auto Colors = Attributes.GetVertexInstanceColors();
		auto UVs = Attributes.GetVertexInstanceUVs();
		UVs.SetNumChannels(NumUVChannels);
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Surface");
		TArray<FVertexID> VertexIDs;
		VertexIDs.Init(FVertexID(INDEX_NONE), Vertices.Num());
		for (int32 I = 0; I < SourceIndices.Num(); I += 3)
		{
			FVertexInstanceID Corners[3];
			for (int32 C = 0; C < 3; ++C)
			{
				const int32 Index = SourceIndices[I + C];
				const FSurfaceVertex& Vertex = Vertices[Index];
				if (VertexIDs[Index] == FVertexID(INDEX_NONE))
				{
					VertexIDs[Index] = Description.CreateVertex();
					Positions[VertexIDs[Index]] = FVector3f(Vertex.Position);
				}
				Corners[C] = Description.CreateVertexInstance(VertexIDs[Index]);
				Normals[Corners[C]] = FVector3f(Vertex.Normal);
				FVector Tangent = FVector::VectorPlaneProject(FVector::ForwardVector, Vertex.Normal).GetSafeNormal();
				if (Tangent.IsNearlyZero()) Tangent = FVector::VectorPlaneProject(FVector::RightVector, Vertex.Normal).GetSafeNormal();
				Tangents[Corners[C]] = FVector3f(Tangent);
				Signs[Corners[C]] = 1.0f;
				Colors[Corners[C]] = FVector4f(Vertex.Color.R, Vertex.Color.G, Vertex.Color.B, Vertex.Color.A);
				UVs.Set(Corners[C], 0, Vertex.UV);
				if(NumUVChannels>1)UVs.Set(Corners[C],1,Vertex.HydraulicUV);
				if(NumUVChannels>2)UVs.Set(Corners[C],2,Vertex.ArrivalUV);
			}
			Description.CreateTriangle(Group, MakeArrayView(Corners));
		}
		};
		FMeshDescription Description; FillDescription(Description, Indices);
		TArray<TUniquePtr<FMeshDescription>> ExtraDescriptions;
		TArray<const FMeshDescription*> Descriptions{&Description};
		if (ExtraLODIndices) for (const TArray<int32>& LODIndices : *ExtraLODIndices)
		{
			if (LODIndices.IsEmpty()) continue;
			auto& Extra = ExtraDescriptions.Add_GetRef(MakeUnique<FMeshDescription>());
			FillDescription(*Extra, LODIndices); Descriptions.Add(Extra.Get());
		}
		UStaticMesh* Mesh = NewObject<UStaticMesh>(Owner, MakeUniqueObjectName(Owner, UStaticMesh::StaticClass(), Name), RF_Transient);
		bool bKeepCPUAccess=bBuildGroundCollision;
#if WITH_EDITOR
		// Native geometry assertions read the real built indices; NullRHI discards
		// non-retained arrays too. Only an explicitly tagged commandlet fixture opts in.
		bKeepCPUAccess|=IsRunningCommandlet()&&Owner->ActorHasTag(TEXT("Aftershock_MeshCpuAudit"));
#endif
		Mesh->bAllowCPUAccess = bKeepCPUAccess;
		Mesh->LODForCollision = 0; // Picking/collision never use a display approximation.
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, TEXT("Surface")));
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bFastBuild = true; // Required for packaged builds; no editor mesh builder.
		Params.bBuildSimpleCollision = false;
		Params.bCommitMeshDescription = false;
		Params.bMarkPackageDirty = false;
		Params.bAllowCpuAccess = bKeepCPUAccess;
		if(NumUVChannels>1){Params.PerLODOverrides.SetNum(Descriptions.Num());for(auto& LOD:Params.PerLODOverrides)LOD.bUseFullPrecisionUVs=true;}
		if (!Mesh->BuildFromMeshDescriptions(Descriptions, Params)) return nullptr;
		InitializeAftershockRuntimeMeshUV(*Mesh, Vertices, Indices, bUV0ContainsFlowDirections,NumUVChannels);
		if (bBuildGroundCollision)
		{
			UBodySetup* Body = Mesh->GetBodySetup();
			if (!Body) return nullptr;
			Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
			Body->bMeshCollideAll = true;
			Body->bDoubleSidedGeometry = true;
			Body->InvalidatePhysicsData();
			Body->CreatePhysicsMeshes();
			if (Body->bFailedToCreatePhysicsMeshes || Body->TriMeshGeometries.IsEmpty())
			{
				UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_TERRAIN_COLLISION_COOK_FAILED"));
				return nullptr;
			}
		}
		else if (UBodySetup* Body = Mesh->GetBodySetup())
		{
			// These display-only meshes have no shapes. Prevent component mesh
			// assignment from attempting a complex collision cook without CPU data.
			Body->CollisionTraceFlag = CTF_UseSimpleAsComplex;
			Body->DefaultInstance.SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		return Mesh;
	}
}


UStaticMesh* BuildAftershockPresentationMesh(AActor* Owner,const TCHAR* Name,
    const TArray<FAftershockPresentationMeshVertex>& Source,const TArray<int32>& Indices,
    UMaterialInterface* Material,const TArray<TArray<int32>>* ExtraLODIndices)
{
	// Display-only path. Canonical ground/collision and the physical flood helper
	// continue through their existing separate builders unchanged.
	const double Started=FPlatformTime::Seconds();
	if(!Owner||!Material||Source.IsEmpty()||Indices.IsEmpty()||Indices.Num()%3)return nullptr;
	TArray<const TArray<int32>*,TInlineAllocator<4>> LODIndices;LODIndices.Add(&Indices);
	if(ExtraLODIndices)for(const auto& LOD:*ExtraLODIndices)LODIndices.Add(&LOD);
	if(LODIndices.Num()>MAX_STATIC_MESH_LODS)return nullptr;
	FBox Bounds(ForceInit);
	for(const auto& V:Source)
	{
		if(V.Position.ContainsNaN()||V.Normal.ContainsNaN()||V.UV.ContainsNaN())return nullptr;
		Bounds+=V.Position;
	}
	for(const auto* Values:LODIndices)
	{
		if(Values->IsEmpty()||Values->Num()%3)return nullptr;
		for(int32 I:*Values)if(!Source.IsValidIndex(I))return nullptr;
	}
	bool KeepCPU=false;
#if WITH_EDITOR
	KeepCPU=IsRunningCommandlet()&&Owner->ActorHasTag(TEXT("Aftershock_MeshCpuAudit"));
#endif
	auto* Mesh=NewObject<UStaticMesh>(Owner,MakeUniqueObjectName(Owner,UStaticMesh::StaticClass(),Name),RF_Transient);
	Mesh->NeverStream=true;Mesh->bAllowCPUAccess=KeepCPU;Mesh->bSupportRayTracing=false;Mesh->bDoFastBuild=true;
	Mesh->GetStaticMaterials().Add(FStaticMaterial(Material,TEXT("Surface")));
	auto RenderData=MakeUnique<FStaticMeshRenderData>();RenderData->AllocateLODResources(LODIndices.Num());
	RenderData->NumInlinedLODs=LODIndices.Num();RenderData->CurrentFirstLODIdx=0;RenderData->Bounds=FBoxSphereBounds(Bounds);
#if WITH_EDITOR
	Mesh->SetAutoComputeLODScreenSize(true);
#endif
	int32 TotalVertices=0,TotalTriangles=0;
	double PackingSeconds=0.;
	for(int32 At=0;At<LODIndices.Num();++At)
	{
		const double PackStarted=FPlatformTime::Seconds();
		const auto& Values=*LODIndices[At];
		// Compact only this LOD's referenced source vertices. Shared input stores
		// all road LODs; uploading that whole union for each LOD would multiply VRAM.
		TArray<int32> Remap;Remap.Init(INDEX_NONE,Source.Num());
		TArray<FStaticMeshBuildVertex> BuildVertices;BuildVertices.Reserve(FMath::Min(Source.Num(),Values.Num()));
		TArray<uint32> RenderIndices;RenderIndices.Reserve(Values.Num());
		for(int32 OldIndex:Values)
		{
			int32& NewIndex=Remap[OldIndex];
			if(NewIndex==INDEX_NONE)
			{
				NewIndex=BuildVertices.Num();const auto& V=Source[OldIndex];auto& B=BuildVertices.AddDefaulted_GetRef();
				B.Position=FVector3f(V.Position);B.TangentZ=FVector3f(V.Normal.GetSafeNormal());
				FVector Tangent=FVector::VectorPlaneProject(FVector::ForwardVector,V.Normal).GetSafeNormal();
				if(Tangent.IsNearlyZero())Tangent=FVector::VectorPlaneProject(FVector::RightVector,V.Normal).GetSafeNormal();
				B.TangentX=FVector3f(Tangent);B.TangentY=FVector3f::CrossProduct(B.TangentZ,B.TangentX);
				B.UVs[0]=V.UV;B.Color=V.Color.ToFColor(true);
			}
			RenderIndices.Add(static_cast<uint32>(NewIndex));
		}
		PackingSeconds+=FPlatformTime::Seconds()-PackStarted;
		FStaticMeshLODResources& LOD=RenderData->LODResources[At];LOD.SourceMeshBounds=RenderData->Bounds;
		LOD.bBuffersInlined=true;LOD.bIsOptionalLOD=false;RenderData->ScreenSize[At].Default=1.f/static_cast<float>(1<<At);
		FConstMeshBuildVertexView View=MakeConstMeshBuildVertexView(MakeArrayView(BuildVertices));View.UVs.SetNum(1);
		View.Color=MakeConstStridedView(sizeof(FStaticMeshBuildVertex),&BuildVertices[0].Color,BuildVertices.Num());
		LOD.VertexBuffers.PositionVertexBuffer.Init(View,KeepCPU);
		LOD.VertexBuffers.StaticMeshVertexBuffer.SetUseFullPrecisionUVs(true);
		FStaticMeshVertexBufferFlags Flags;Flags.bNeedsCPUAccess=KeepCPU;Flags.bUseBackwardsCompatibleF16TruncUVs=false;
		LOD.VertexBuffers.StaticMeshVertexBuffer.Init(View,Flags);LOD.VertexBuffers.ColorVertexBuffer.Init(View,KeepCPU);LOD.bHasColorVertexData=true;
		LOD.IndexBuffer.TrySetAllowCPUAccess(KeepCPU);
		LOD.IndexBuffer.SetIndices(RenderIndices,BuildVertices.Num()>65536?EIndexBufferStride::Force32Bit:EIndexBufferStride::Force16Bit);
		FStaticMeshSection& Section=LOD.Sections.AddDefaulted_GetRef();Section.MaterialIndex=0;Section.FirstIndex=0;
		Section.NumTriangles=RenderIndices.Num()/3;Section.MinVertexIndex=0;Section.MaxVertexIndex=BuildVertices.Num()-1;
		Section.bEnableCollision=false;Section.bCastShadow=false;Section.bVisibleInRayTracing=false;Section.bAffectDistanceFieldLighting=false;
#if WITH_EDITOR
		FMeshSectionInfo Info(0);Info.bEnableCollision=false;Info.bCastShadow=false;Info.bVisibleInRayTracing=false;Info.bAffectDistanceFieldLighting=false;
		Mesh->GetSectionInfoMap().Set(At,0,Info);
#endif
		TotalVertices+=BuildVertices.Num();TotalTriangles+=RenderIndices.Num()/3;
	}
	Mesh->SetRenderData(MoveTemp(RenderData));
	InitializeAftershockRuntimeMeshUV(*Mesh,Source,Indices,false,1);
	Mesh->CalculateExtendedBounds();Mesh->CreateBodySetup();
	if(auto* Body=Mesh->GetBodySetup()){Body->CollisionTraceFlag=CTF_UseSimpleAsComplex;Body->DefaultInstance.SetCollisionEnabled(ECollisionEnabled::NoCollision);}
	// Fresh resource only: no live-buffer mutation, render-fence wait, editor
	// mesh description, asset save, source model or canonical physics cook.
	const double SubmitStarted=FPlatformTime::Seconds();Mesh->InitResources();
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_DISPLAY_MESH_DIRECT mesh=%s lods=%d lodVerticesSum=%d lodTrianglesSum=%d totalMs=%.3f packingMs=%.3f resourceEnqueueMs=%.3f uvChannels=1 fullPrecisionUV=1 meshDescription=0 collisionCook=0 gpuFenceWait=0 resourceEnqueueNotGPUTime=1"),
		Name,LODIndices.Num(),TotalVertices,TotalTriangles,(FPlatformTime::Seconds()-Started)*1000.,PackingSeconds*1000.,(FPlatformTime::Seconds()-SubmitStarted)*1000.);
	return Mesh;
}


bool BuildAftershockCoastalSeaSurface(AActor* Owner, const UTerrainGridSubsystem* Terrain,
	UStaticMeshComponent* SurfaceComponent)
{
	if (!Owner || !Terrain || !SurfaceComponent || !SurfaceComponent->GetMaterial(0)) return false;
	for (auto It=SeaSurfaceCache.CreateIterator();It;++It) if(!It.Key().IsValid()) It.RemoveCurrent();
	const TWeakObjectPtr<UStaticMeshComponent> Key(SurfaceComponent);
	if(!SeaSurfaceCache.Contains(Key)&&SeaSurfaceCache.Num()>=64)SeaSurfaceCache.Remove(SeaSurfaceCache.CreateConstIterator().Key());
	auto& Cached=SeaSurfaceCache.FindOrAdd(Key);
	const FIntRect Visible=GetAftershockDrawableCellBounds(*Terrain);
	uint64 Signature=GetAftershockTerrainGeometrySignature(*Terrain,Visible);
	MixGeometryHash(Signature,GetTypeHash(Terrain->CoastalSeaLevelCm));
	MixGeometryHash(Signature,Owner->ActorHasTag(TEXT("Aftershock_MeshCpuAudit"))?1u:0u);
	if(Cached.bHasSignature&&Cached.Signature==Signature&&SurfaceComponent->GetStaticMesh()==Cached.Mesh.Get()) return true;
	TArray<FSurfaceVertex> Vertices;
	TArray<int32> Indices;
	for(int32 Y=Visible.Min.Y;Y<Visible.Max.Y;++Y)for(int32 X=Visible.Min.X;X<Visible.Max.X;++X)
	{
		FGridCell Cell;if(!Terrain->TryGetCell(FIntPoint(X,Y),Cell)||!Cell.bIsOcean)continue;
		const int32 Base = Vertices.Num();
		for (int32 CornerY = 0; CornerY <= 1; ++CornerY) for (int32 CornerX = 0; CornerX <= 1; ++CornerX)
		{
			FSurfaceVertex& V = Vertices.AddDefaulted_GetRef();
			V.Position = FVector(Terrain->GridOrigin.X + (Cell.Coords.X + CornerX) * Terrain->CellSize,
				Terrain->GridOrigin.Y + (Cell.Coords.Y + CornerY) * Terrain->CellSize, Terrain->CoastalSeaLevelCm + 4.f);
			V.UV = FVector2f(V.Position.X / 800.f, V.Position.Y / 800.f);
		}
		Indices.Append({Base, Base + 2, Base + 1, Base + 1, Base + 2, Base + 3});
	}
	SurfaceComponent->SetStaticMesh(MakeSurface(Owner, TEXT("Aftershock_CanonicalCoastalSea"), Vertices, Indices, SurfaceComponent->GetMaterial(0)));
	SurfaceComponent->SetVisibility(!Indices.IsEmpty());
	if(!Indices.IsEmpty()&&!SurfaceComponent->GetStaticMesh()) return false;
	Cached.Signature=Signature;Cached.bHasSignature=true;Cached.Mesh=SurfaceComponent->GetStaticMesh();
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_COASTAL_SEA oceanCells=%d triangles=%d seaLevelCm=%.1f landOverlayCells=0 source=CanonicalOceanFlag"), Indices.Num() / 6, Indices.Num() / 3, Terrain->CoastalSeaLevelCm);
	return true;
}

bool BuildAftershockTsunamiSurface(AActor* Owner, const UTerrainGridSubsystem* Terrain,
	const FAftershockDisasterVisualSnapshot& Snapshot, UStaticMeshComponent* SurfaceComponent,
	FAftershockTsunamiSurfaceData& OutData)
{
	OutData = FAftershockTsunamiSurfaceData();
	if (!Owner || !Terrain || !SurfaceComponent || !SurfaceComponent->GetMaterial(0) || !Terrain->IsCoastalWorld()) return false;
	FVector Direction = Snapshot.PropagationDirection.GetSafeNormal2D();
	if (Direction.IsNearlyZero()) Direction = FVector(0.f, -1.f, 0.f);
	float First = MAX_flt, Last = -MAX_flt;
	for (const FGridCell& Cell : Terrain->Cells)
	{
		if (!Cell.bIsOcean && !(Cell.bIsPlayable && Snapshot.LocalSeverity.FindRef(Cell.Coords) > 0.f)) continue;
		const float Projection = FVector::DotProduct(Terrain->GridToWorld(Cell.Coords), Direction);
		First = FMath::Min(First, Projection - Terrain->CellSize * .5f);
		Last = FMath::Max(Last, Projection + Terrain->CellSize * .5f);
	}
	if (First == MAX_flt || Last <= First) return false;
	const float Range = FMath::Max(Terrain->CellSize, Last - First);
	const FIntRect Visible=GetAftershockDrawableCellBounds(*Terrain);
	TArray<FSurfaceVertex> Vertices;
	TArray<int32> Indices;
	const auto DepthFromSeverity = [&Snapshot](float Severity,float Ground)
	{
		const float LocalFraction=FMath::Clamp(Severity/FMath::Max(1.f,Snapshot.EffectiveIntensity),0.f,1.f);
		return FMath::Clamp((Snapshot.SurgePeakHeadCm-Ground)*LocalFraction,0.f,1400.f);
	};
	const auto SampleField = [&Snapshot,Terrain](float X,float Y,bool bOcean)
	{
		const FIntRect Bounds=Terrain->GetResidentCellBounds();
		X=FMath::Clamp(X,static_cast<float>(Bounds.Min.X),static_cast<float>(Bounds.Max.X-1));
		Y=FMath::Clamp(Y,static_cast<float>(Bounds.Min.Y),static_cast<float>(Bounds.Max.Y-1));
		const int32 X0=FMath::FloorToInt(X),Y0=FMath::FloorToInt(Y);
		const int32 X1=FMath::Min(X0+1,Bounds.Max.X-1),Y1=FMath::Min(Y0+1,Bounds.Max.Y-1);
		const auto V=[&](int32 GX,int32 GY)
		{ const FIntPoint C(GX,GY); return bOcean ? (Terrain->GetCell(C).bIsOcean?1.f:0.f) : Snapshot.LocalSeverity.FindRef(C); };
		const FIntPoint C[]={FIntPoint(X0,Y0),FIntPoint(X1,Y0),FIntPoint(X0,Y1),FIntPoint(X1,Y1)};
		if(Terrain->IsValidCoord(C[0])&&Terrain->IsValidCoord(C[1])&&Terrain->IsValidCoord(C[2])&&Terrain->IsValidCoord(C[3]))return FMath::Lerp(FMath::Lerp(V(X0,Y0),V(X1,Y0),X-X0),FMath::Lerp(V(X0,Y1),V(X1,Y1),X-X0),Y-Y0);
		const float FX=X-X0,FY=Y-Y0,W[]={(1-FX)*(1-FY),FX*(1-FY),(1-FX)*FY,FX*FY};float Sum=0,Weight=0;
		for(int32 I=0;I<4;++I)if(Terrain->IsValidCoord(C[I])){Sum+=V(C[I].X,C[I].Y)*W[I];Weight+=W[I];}return Weight>SMALL_NUMBER?Sum/Weight:0.f;
	};
	for (const FGridCell& Cell : Terrain->Cells)
	{
		if (!Cell.bIsOcean && !(Cell.bIsPlayable && Snapshot.LocalSeverity.FindRef(Cell.Coords) > 0.f)) continue;
		const float CenterDepth = DepthFromSeverity(Snapshot.LocalSeverity.FindRef(Cell.Coords),FMath::Max(Terrain->CoastalSeaLevelCm,Cell.Elevation));
		if (CenterDepth < 4.f) continue;
		Cell.bIsOcean ? ++OutData.OceanCells : ++OutData.AffectedLandCells;
		OutData.PeakDepthCm.Add(Cell.Coords, CenterDepth);
		OutData.ArrivalFraction.Add(Cell.Coords, FMath::Clamp((FVector::DotProduct(Terrain->GridToWorld(Cell.Coords), Direction) - First) / Range, 0.f, 1.f));
		if(!Visible.Contains(Cell.Coords))continue;
		constexpr int32 Divisions = 4;
		const int32 Base = Vertices.Num();
		for (int32 Y = 0; Y <= Divisions; ++Y) for (int32 X = 0; X <= Divisions; ++X)
		{
			const float GX = Cell.Coords.X + static_cast<float>(X) / Divisions;
			const float GY = Cell.Coords.Y + static_cast<float>(Y) / Divisions;
			const float Ground = FMath::Max(Terrain->CoastalSeaLevelCm,SampleElevation(*Terrain,GX-.5f,GY-.5f));
			const float Depth=DepthFromSeverity(SampleField(GX-.5f,GY-.5f,false),Ground);
			const float OceanMask=SampleField(GX-.5f,GY-.5f,true);
			FSurfaceVertex& V = Vertices.AddDefaulted_GetRef();
			V.Position = FVector(Terrain->GridOrigin.X + GX * Terrain->CellSize, Terrain->GridOrigin.Y + GY * Terrain->CellSize, Ground - 4.f);
			const FVector LateralAxis(-Direction.Y, Direction.X, 0.f);
			const FVector CoastCenter = Terrain->GridOrigin + FVector(Terrain->GridWidth * Terrain->CellSize * 0.5f, (Terrain->CoastStartRow > 0 ? Terrain->CoastStartRow : Terrain->GridHeight * 0.75f) * Terrain->CellSize, 0.f);
			const float LateralDist = FVector::DotProduct(V.Position - CoastCenter, LateralAxis);
			const float MaxLateral = FMath::Max(Terrain->CellSize * 6.f, Range * 1.4f);
			const float BowCurvature = FMath::Square(LateralDist / MaxLateral) * 0.12f;
			const float Refraction = FMath::Sin(LateralDist * 0.0007f) * 0.025f + FMath::Cos(LateralDist * 0.0018f) * 0.012f;
			const float LinearArrival = (FVector::DotProduct(V.Position, Direction) - First) / Range;
			const float Arrival = FMath::Clamp(LinearArrival + BowCurvature + Refraction, 0.f, 1.f);
			const FVector WaveFlow = (Direction + LateralAxis * (LateralDist / MaxLateral * 0.35f)).GetSafeNormal();
			V.UV = FVector2f(WaveFlow.X, WaveFlow.Y);
			const FColor Encoded(static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Depth / 2048.f * 255.f),0,255)),
				static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Arrival * 255.f),0,255)), static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(OceanMask*255.f),0,255)), 255);
			V.Color = FLinearColor::FromSRGBColor(Encoded);
			if (X < Divisions && Y < Divisions)
			{
				const int32 A = Base + X + Y * (Divisions + 1);
				Indices.Append({A, A + Divisions + 1, A + 1, A + 1, A + Divisions + 1, A + Divisions + 2});
			}
		}
	}
	SurfaceComponent->SetStaticMesh(MakeSurface(Owner, TEXT("Aftershock_CanonicalTsunamiWave"), Vertices, Indices, SurfaceComponent->GetMaterial(0), false, true));
	if(UStaticMesh* Mesh=SurfaceComponent->GetStaticMesh()) Mesh->SetPositiveBoundsExtension(FVector(0,0,2000));
	OutData.Triangles = Indices.Num() / 3;
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_TSUNAMI_MESH oceanCells=%d affectedLandCells=%d triangles=%d arrivalDirection=%s authoredSurgeHeadCm=%.1f canonicalSeverityWrites=0"),
		OutData.OceanCells, OutData.AffectedLandCells, OutData.Triangles, *Direction.ToCompactString(), Snapshot.SurgePeakHeadCm);
	return !Indices.IsEmpty() && SurfaceComponent->GetStaticMesh();
}

bool BuildAftershockTerrainSurface(AActor* Owner, const UTerrainGridSubsystem* Terrain,
	UStaticMeshComponent* GroundComponent, UStaticMeshComponent* WaterComponent)
{
	if (!Owner || !Terrain || !GroundComponent || !WaterComponent || Terrain->Cells.IsEmpty()) return false;
	if (!GroundComponent->GetMaterial(0) || !WaterComponent->GetMaterial(0))
	{
		UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_PRESENTATION_SURFACE_MATERIAL_MISSING"));
		return false;
	}

	ApplyAftershockRegionalSurfaceStyle(*Terrain,GroundComponent);
	const FIntRect Visible=GetAftershockVisibleCellBounds(*Terrain),Resident=Terrain->GetResidentCellBounds();
	const UDisasterSimulationSubsystem* HydraulicOwner=Owner->GetWorld()?Owner->GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>():nullptr;
	const bool bHydraulicModel2=HydraulicOwner&&HydraulicOwner->IsHydraulicModel2Active();
	PruneTerrainSurfaceCache();
	const TWeakObjectPtr<UStaticMeshComponent> CacheKey(GroundComponent);
	if(!TerrainSurfaceCache.Contains(CacheKey)&&TerrainSurfaceCache.Num()>=64) TerrainSurfaceCache.Remove(TerrainSurfaceCache.CreateConstIterator().Key());
	auto& Cached=TerrainSurfaceCache.FindOrAdd(CacheKey);
	uint64 GeometrySignature=GetAftershockTerrainGeometrySignature(*Terrain,Visible);
	MixGeometryHash(GeometrySignature,bHydraulicModel2?2u:1u);
	MixGeometryHash(GeometrySignature,Owner->ActorHasTag(TEXT("Aftershock_MeshCpuAudit"))?1u:0u);
	if(Cached.bHasSignature&&Cached.GeometrySignature==GeometrySignature&&Cached.WaterComponent.Get()==WaterComponent
		&&GroundComponent->GetStaticMesh()==Cached.GroundMesh.Get()&&WaterComponent->GetStaticMesh()==Cached.WaterMesh.Get())
	{
		++Cached.Stats.GroundCacheHits;
		ApplyAftershockTerrainPresentationBudget(GroundComponent,Owner->GetWorld());
		return true;
	}
	++Cached.Stats.GroundBuildAttempts;
	const bool bInitialDense=Resident==FIntRect(0,0,Terrain->GridWidth,Terrain->GridHeight)&&Terrain->Cells.Num()==Terrain->GridWidth*Terrain->GridHeight;
	const bool bLegacyRiverRibbon=!bHydraulicModel2&&bInitialDense&&!Terrain->IsUsingRealGeography();
	const int32 Columns = Visible.Width() * 2 + 1;
	const int32 Rows = Visible.Height() * 2 + 1;
	// Shared canonical half-grid knots are computed once on a geometry cache
	// miss. Ground render AND the existing complex collision cook consume this
	// exact vertex set. No per-step mesh/collision rebuild or separate water bed.
	TArray<double> HydraulicKnots;
	const int32 HydraulicPitch=Columns+2;
	if(bHydraulicModel2)
	{
		HydraulicKnots.SetNumUninitialized((Columns+2)*(Rows+2));
		FText HydraulicError;
		int32 SourceOnlySupportKnots=0;
		for(int32 HY=-1;HY<=Rows;++HY)for(int32 HX=-1;HX<=Columns;++HX)
		{
			double HeightCm=0.;
			if(!TryGetAftershockHydraulicGroundKnotCm(*Terrain,FIntPoint(Visible.Min.X*2+HX-1,Visible.Min.Y*2+HY-1),true,HeightCm,HydraulicError))
			{
				// Sparse resident chunks can leave unused rectangular vertices or
				// a normal stencil outside the resident set. Query the same real
				// geographic DEM for those display supports only. No cell, bed,
				// hydraulic boundary or triangle across the gap is fabricated.
				const double GridX=Visible.Min.X+HX*.5-.5,GridY=Visible.Min.Y+HY*.5-.5;
				const int32 SupportX=FMath::FloorToInt(FMath::Clamp(GridX,static_cast<double>(Resident.Min.X),static_cast<double>(Resident.Max.X-1)));
				const int32 SupportY=FMath::FloorToInt(FMath::Clamp(GridY,static_cast<double>(Resident.Min.Y),static_cast<double>(Resident.Max.Y-1)));
				bool HasResidentSupport=false;
				for(int32 DY=0;DY<2;++DY)for(int32 DX=0;DX<2;++DX)
					HasResidentSupport|=Terrain->IsValidCoord(FIntPoint(FMath::Min(SupportX+DX,Resident.Max.X-1),FMath::Min(SupportY+DY,Resident.Max.Y-1)));
				const FVector SourcePoint=Terrain->GridOrigin+FVector((GridX+.5)*Terrain->CellSize,(GridY+.5)*Terrain->CellSize,0.);
				float SourceHeight=0.f;
				if(HasResidentSupport||!Terrain->IsUsingRealGeography()||!Terrain->TrySampleWorldElevation(SourcePoint,SourceHeight)||!FMath::IsFinite(SourceHeight))
				{
					UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_HYDRAULIC_GROUND_REJECT %s sourceDisplaySupportMissing=1"),*HydraulicError.ToString());
					return false;
				}
				HeightCm=SourceHeight;++SourceOnlySupportKnots;
			}
			HydraulicKnots[(HX+1)+(HY+1)*HydraulicPitch]=HeightCm;
		}
		if(SourceOnlySupportKnots>0)UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_HYDRAULIC_DISPLAY_SUPPORT sourceDemOnlyKnots=%d residentPhysicsChanged=0 gapTrianglesAdded=0"),SourceOnlySupportKnots);
	}
	TArray<FSurfaceVertex> Vertices;
	TArray<int32> Indices;
	Vertices.Reserve(Columns * Rows);
	Indices.Reserve((Columns - 1) * (Rows - 1) * 6);
	for (int32 Y = 0; Y < Rows; ++Y)
	{
		for (int32 X = 0; X < Columns; ++X)
		{
			const float GX = Visible.Min.X + X * 0.5f - 0.5f;
			const float GY = Visible.Min.Y + Y * 0.5f - 0.5f;
			const int32 HydraulicVertexAt=(X+1)+(Y+1)*HydraulicPitch;
			const double Height = bHydraulicModel2?HydraulicKnots[HydraulicVertexAt]:SampleElevation(*Terrain,GX,GY);
			const double DX = bHydraulicModel2?HydraulicKnots[HydraulicVertexAt+1]-HydraulicKnots[HydraulicVertexAt-1]:SampleElevation(*Terrain,GX+.25f,GY)-SampleElevation(*Terrain,GX-.25f,GY);
			const double DY = bHydraulicModel2?HydraulicKnots[HydraulicVertexAt+HydraulicPitch]-HydraulicKnots[HydraulicVertexAt-HydraulicPitch]:SampleElevation(*Terrain,GX,GY+.25f)-SampleElevation(*Terrain,GX,GY-.25f);
			FSurfaceVertex& V = Vertices.AddDefaulted_GetRef();
			V.Position = FVector(Terrain->GridOrigin.X + (Visible.Min.X+X*.5f)*Terrain->CellSize, Terrain->GridOrigin.Y + (Visible.Min.Y+Y*.5f)*Terrain->CellSize, Height);
			V.Normal = FVector(-DX,-DY,Terrain->CellSize*(bHydraulicModel2?1.f:.5f)).GetSafeNormal();
			// Match the active world-aligned ground material's .005 UV/cm scale;
			// retained UV metadata must not ask for city-wide highest mips.
			V.UV = FVector2f(V.Position.X*.005,V.Position.Y*.005);
			const FIntPoint CellCoord(FMath::Clamp(FMath::RoundToInt(GX), Resident.Min.X, Resident.Max.X - 1),
				FMath::Clamp(FMath::RoundToInt(GY), Resident.Min.Y, Resident.Max.Y - 1));
			const FGridCell Cell = Terrain->GetCell(CellCoord);
			// Legacy relief was compressed into game centimetres. Real geography
			// keeps MSL metres: do not turn a16-metre real hill into a snow cap.
			const float ColourHeight=static_cast<float>(Terrain->IsUsingRealGeography()?(Height-Terrain->GridOrigin.Z)*.01:Height);
			const float Dry = FMath::Clamp(ColourHeight / 650.0f, 0.0f, 1.0f);
			FLinearColor GroundCol = FLinearColor::LerpUsingHSV(FLinearColor(0.20f, 0.30f, 0.10f), FLinearColor(0.40f, 0.39f, 0.23f), Dry);
			GroundCol = FLinearColor::LerpUsingHSV(GroundCol, FLinearColor(0.38f, 0.33f, 0.22f), FMath::Pow(Cell.RiverProximity, 3.0f) * 0.65f);
			if (ColourHeight > 650.0f)
			{
				const float RockAlpha = FMath::Clamp((ColourHeight - 650.0f) / 600.0f, 0.0f, 1.0f);
				GroundCol = FLinearColor::LerpUsingHSV(GroundCol, FLinearColor(0.24f, 0.23f, 0.22f), RockAlpha);
			}
			if (Terrain->ContinuationGeneratorVersion<3 && ColourHeight > 1600.0f)
			{
				const float SnowAlpha = FMath::Clamp((ColourHeight - 1600.0f) / 700.0f, 0.0f, 1.0f);
				GroundCol = FLinearColor::LerpUsingHSV(GroundCol, FLinearColor(0.85f, 0.88f, 0.94f), SnowAlpha);
			}
			if (Cell.bIsCoastal)
			{
				GroundCol = FLinearColor::LerpUsingHSV(GroundCol, FLinearColor(0.72f, 0.64f, 0.44f), 0.75f);
			}
			V.Color = GroundCol;
			if (X < Columns - 1 && Y < Rows - 1)
			{
				const FIntPoint PatchCell(FMath::Clamp(FMath::RoundToInt(GX + 0.25f), Resident.Min.X, Resident.Max.X - 1),
					FMath::Clamp(FMath::RoundToInt(GY + 0.25f), Resident.Min.Y, Resident.Max.Y - 1));
				FGridCell PatchSource;
				if(!Terrain->TryGetCell(PatchCell,PatchSource)||(!PatchSource.bIsPlayable&&!IsAftershockHydraulicExcavatedCell(PatchSource,bHydraulicModel2)))continue;
				const int32 A = X + Y * Columns;
				// Upward-facing clockwise triangles for Unreal's left-handed coordinates.
				if(RiverQuadDiagonalAt(*Terrain,FMath::FloorToInt(GX+.25f),FMath::FloorToInt(GY+.25f))==1)
					Indices.Append({A,A+Columns,A+Columns+1,A,A+Columns+1,A+1});
				else Indices.Append({A,A+Columns,A+1,A+1,A+Columns,A+Columns+1});
			}
		}
	}
	// Keep exact half-cell LOD0 for every physics/picking query. Render-only LODs
	// simplify quiet land; river/coast/missing-cell boundaries keep their original
	// triangles. No coarse patch can span an ocean hole or bridge a river saddle.
	TArray<TArray<int32>> ExtraLODIndices;
	const auto AddFinePatch=[&](int32 X,int32 Y,TArray<int32>& Out)
	{
		const float GX=Visible.Min.X+X*.5f-.5f,GY=Visible.Min.Y+Y*.5f-.5f;
		const FIntPoint Cell(FMath::Clamp(FMath::RoundToInt(GX+.25f),Resident.Min.X,Resident.Max.X-1),FMath::Clamp(FMath::RoundToInt(GY+.25f),Resident.Min.Y,Resident.Max.Y-1));
		FGridCell PatchSource;
		if(!Terrain->TryGetCell(Cell,PatchSource)||(!PatchSource.bIsPlayable&&!IsAftershockHydraulicExcavatedCell(PatchSource,bHydraulicModel2)))return;
		const int32 A=X+Y*Columns;
		if(RiverQuadDiagonalAt(*Terrain,FMath::FloorToInt(GX+.25f),FMath::FloorToInt(GY+.25f))==1)Out.Append({A,A+Columns,A+Columns+1,A,A+Columns+1,A+1});
		else Out.Append({A,A+Columns,A+1,A+1,A+Columns,A+Columns+1});
	};
	const int32 ProtectedWidth=Visible.Width()+4,ProtectedHeight=Visible.Height()+4;
	TArray<int32> ProtectionPrefix;ProtectionPrefix.Init(0,(ProtectedWidth+1)*(ProtectedHeight+1));
	for(int32 Y=0;Y<ProtectedHeight;++Y)for(int32 X=0;X<ProtectedWidth;++X)
	{
		FGridCell Cell;const bool Protected=!Terrain->TryGetCell(Visible.Min+FIntPoint(X-2,Y-2),Cell)||!Cell.bIsPlayable||Cell.bIsRiverbed||Cell.bIsCoastal||Cell.bIsOcean||IsAftershockHydraulicExcavatedCell(Cell,bHydraulicModel2);
		const int32 At=(X+1)+(Y+1)*(ProtectedWidth+1);
		ProtectionPrefix[At]=(Protected?1:0)+ProtectionPrefix[At-1]+ProtectionPrefix[At-(ProtectedWidth+1)]-ProtectionPrefix[At-(ProtectedWidth+1)-1];
	}
	const auto NeedsDense=[&](int32 X,int32 Y,int32 EndX,int32 EndY)
	{
		const int32 A=X/2,B=Y/2,C=FMath::DivideAndRoundUp(EndX,2)+4,D=FMath::DivideAndRoundUp(EndY,2)+4;
		const int32 Pitch=ProtectedWidth+1;
		return ProtectionPrefix[C+D*Pitch]-ProtectionPrefix[A+D*Pitch]-ProtectionPrefix[C+B*Pitch]+ProtectionPrefix[A+B*Pitch]>0;
	};
	for(int32 Stride : {2,4,8})
	{
		auto& LODIndices=ExtraLODIndices.AddDefaulted_GetRef();LODIndices.Reserve(Indices.Num()/Stride);
		const int32 BlocksX=FMath::DivideAndRoundUp(Columns-1,Stride),BlocksY=FMath::DivideAndRoundUp(Rows-1,Stride);
		TBitArray<> DenseBlocks(false,BlocksX*BlocksY);
		for(int32 BY=0;BY<BlocksY;++BY)for(int32 BX=0;BX<BlocksX;++BX)
		{
			const int32 X=BX*Stride,Y=BY*Stride,EndX=FMath::Min((BX+1)*Stride,Columns-1),EndY=FMath::Min((BY+1)*Stride,Rows-1);
			bool Dense=NeedsDense(X,Y,EndX,EndY);
			const double A=Vertices[X+Y*Columns].Position.Z,B=Vertices[EndX+Y*Columns].Position.Z;
			const double C=Vertices[X+EndY*Columns].Position.Z,D=Vertices[EndX+EndY*Columns].Position.Z;
			const double Tolerance=FMath::Max(2.f,Terrain->CellSize*.005f)*Stride*.5f;
			for(int32 FY=Y;FY<=EndY&&!Dense;++FY)for(int32 FX=X;FX<=EndX;++FX)
			{
				const double U=static_cast<double>(FX-X)/(EndX-X),V=static_cast<double>(FY-Y)/(EndY-Y);
				const double Approx=U+V<=1.?A*(1.-U-V)+B*U+C*V:D*(U+V-1.)+B*(1.-V)+C*(1.-U);
				if(FMath::Abs(Vertices[FX+FY*Columns].Position.Z-Approx)>Tolerance){Dense=true;break;}
			}
			// A sharp edited mound or ridge keeps its exact triangles even when
			// no water/coast flag protects it. Quiet flat regions actually decimate.
			DenseBlocks[BX+BY*BlocksX]=Dense;
		}
		const auto DenseAt=[&](int32 X,int32 Y){return X<0||Y<0||X>=BlocksX||Y>=BlocksY||DenseBlocks[X+Y*BlocksX];};
		for(int32 BY=0;BY<BlocksY;++BY)for(int32 BX=0;BX<BlocksX;++BX)
		{
			const int32 X=BX*Stride,Y=BY*Stride;
			const int32 EndX=FMath::Min(X+Stride,Columns-1),EndY=FMath::Min(Y+Stride,Rows-1);
			if(DenseBlocks[BX+BY*BlocksX])for(int32 FY=Y;FY<EndY;++FY)for(int32 FX=X;FX<EndX;++FX)AddFinePatch(FX,FY,LODIndices);
			else
			{
				const int32 A=X+Y*Columns,B=EndX+Y*Columns,C=X+EndY*Columns,D=EndX+EndY*Columns;
				const bool L=DenseAt(BX-1,BY),R=DenseAt(BX+1,BY),U=DenseAt(BX,BY-1),V=DenseAt(BX,BY+1);
				if(!L&&!R&&!U&&!V)LODIndices.Append({A,C,B,B,C,D});
				else
				{
					// Keep every fine edge vertex beside a dense patch. The coarse
					// interior fan prevents T-junction cracks on nonlinear slopes.
					TArray<int32,TInlineAllocator<32>> Perimeter;
					const auto Edge=[&](int32 AX,int32 AY,int32 EX,int32 EY,bool Detail)
					{
						const int32 Length=FMath::Max(FMath::Abs(EX-AX),FMath::Abs(EY-AY));
						const int32 DX=FMath::Sign(EX-AX),DY=FMath::Sign(EY-AY);
						for(int32 I=0;I<Length;I+=Detail?1:Length)Perimeter.Add(AX+I*DX+(AY+I*DY)*Columns);
					};
					Edge(X,Y,X,EndY,L);Edge(X,EndY,EndX,EndY,V);Edge(EndX,EndY,EndX,Y,R);Edge(EndX,Y,X,Y,U);
					const int32 Centre=(X+EndX)/2+((Y+EndY)/2)*Columns;
					for(int32 I=0;I<Perimeter.Num();++I)LODIndices.Append({Centre,Perimeter[I],Perimeter[(I+1)%Perimeter.Num()]});
				}
			}
		}
	}
	// A larger protected block can retain more fine faces than the preceding
	// display level. Reuse that already valid, cheaper topology rather than
	// increasing rendered geometry as the camera moves farther away.
	for(int32 LOD=0;LOD<ExtraLODIndices.Num();++LOD)
	{
		const TArray<int32>& Previous=LOD>0?ExtraLODIndices[LOD-1]:Indices;
		if(ExtraLODIndices[LOD].Num()>Previous.Num())ExtraLODIndices[LOD]=Previous;
	}
	Cached.Stats.CollisionCookAttempts+=Indices.IsEmpty()?0:1;
	UStaticMesh* Ground = MakeSurface(Owner, TEXT("Aftershock_ContinuousGround"), Vertices, Indices, GroundComponent->GetMaterial(0), true, false, &ExtraLODIndices);
	if (!Ground && !Indices.IsEmpty()) return false;
	GroundComponent->SetStaticMesh(Ground);
	GroundComponent->SetSimulatePhysics(false);
	GroundComponent->SetCollisionObjectType(ECC_WorldStatic);
	GroundComponent->SetCollisionResponseToAllChannels(ECR_Block);
	GroundComponent->SetCollisionEnabled(Ground?ECollisionEnabled::QueryAndPhysics:ECollisionEnabled::NoCollision);
	GroundComponent->RecreatePhysicsState();
	int32 CollisionSamplesPassed = 0;
	for (int32 Sample = 0; Sample < 5; ++Sample)
	{
		const FIntPoint Coord(Visible.Min.X+(Visible.Width()-1)*(Sample+1)/6,Visible.Min.Y+(Visible.Height()-1)*(5-Sample)/6);
		const FVector Center = Terrain->GridToWorld(Coord);
		double ExpectedGroundCm=Center.Z;FText HydraulicError;
		if(!TryGetAftershockHydraulicCellGroundCm(*Terrain,Coord,bHydraulicModel2,ExpectedGroundCm,HydraulicError))continue;
		const double SpanCm=FMath::Max(2000.,FMath::Abs(Center.Z-ExpectedGroundCm)+1000.);
		FHitResult Hit;
		FCollisionQueryParams Query(SCENE_QUERY_STAT(AftershockTerrainAudit), true);
		if (GroundComponent->LineTraceComponent(Hit,Center+FVector(0,0,SpanCm),Center-FVector(0,0,SpanCm),Query)
			&& FMath::Abs(Hit.ImpactPoint.Z-ExpectedGroundCm)<.5)++CollisionSamplesPassed;
	}
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_TERRAIN_COLLISION triangles=%d canonicalCentreSamples=%d/5"), Indices.Num() / 3, CollisionSamplesPassed);

	// Water is presentation only: no new rainfall, water depth or flow authority.
	TArray<FSurfaceVertex> WaterVertices;
	TArray<int32> WaterIndices;
	if(!bHydraulicModel2&&Terrain->IsUsingRealGeography())
	{
		// Reuse the ACTUAL fine ground triangles and their shared vertices. Merely
		// putting cell-centre heights at ribbon edges buries water under a rising
		// bank or an interior ridge. The identical triangles stay6cm above ground
		// everywhere, including below-MSL samples and canonical river diagonals.
		// River LOD protection above keeps these same ground faces at every tier.
		TArray<int32> GroundToWater;GroundToWater.Init(INDEX_NONE,Vertices.Num());
		for(int32 Triangle=0;Triangle<Indices.Num();Triangle+=3)
		{
			const int32 A=Indices[Triangle],B=Indices[Triangle+1],C=Indices[Triangle+2];
			const int32 PatchX=FMath::Min3(A%Columns,B%Columns,C%Columns);
			const int32 PatchY=FMath::Min3(A/Columns,B/Columns,C/Columns);
			FGridCell Cell;
			if(!Terrain->TryGetCell(Visible.Min+FIntPoint(PatchX/2,PatchY/2),Cell)||!Cell.bIsPlayable||!Cell.bIsRiverbed)continue;
			for(int32 GroundIndex:{A,B,C})
			{
				int32& WaterIndex=GroundToWater[GroundIndex];
				if(WaterIndex==INDEX_NONE)
				{
					WaterIndex=WaterVertices.Num();auto& Vertex=WaterVertices.Add_GetRef(Vertices[GroundIndex]);
					Vertex.Position.Z+=6.;Vertex.Color=FLinearColor::White;
					Vertex.Normal=FVector::UpVector;
					Vertex.UV=FVector2f((Vertex.Position.X-Terrain->GridOrigin.X)/Terrain->CellSize,(Vertex.Position.Y-Terrain->GridOrigin.Y)/Terrain->CellSize);
				}
				WaterIndices.Add(WaterIndex);
			}
		}
	}
	else if(!bHydraulicModel2)
	{
	// Saved V1 presentations retain their authored connected ribbon footprint.
	const auto AddRiverSection = [&](float WorldY, float Left, float Right, float HeightLeft, float HeightRight, int32 PreviousSection)
	{
		const int32 Base = WaterVertices.Num();
		for (int32 Side = 0; Side < 2; ++Side)
		{
			FSurfaceVertex& V = WaterVertices.AddDefaulted_GetRef();
			const float H = (Side == 0) ? HeightLeft : HeightRight;
			// V1 saves retain the original ribbon clearance. V2 uses the exact
			// ground-triangle surface +6cm above, without migrating old geometry.
			V.Position = FVector(Terrain->GridOrigin.X + (Side == 0 ? Left : Right), Terrain->GridOrigin.Y + WorldY, H + 12.0f);
			V.UV = FVector2f(static_cast<float>(Side), WorldY / Terrain->CellSize);
		}
		if (PreviousSection != INDEX_NONE)
		{
			WaterIndices.Append({ PreviousSection, Base, PreviousSection + 1, PreviousSection + 1, Base, Base + 1 });
		}
		return Base;
	};
	struct FRiverRun{int32 First;int32 Last;float HeightLeft;float HeightRight;int32 Section=INDEX_NONE;};
	// Adjacent rows may share a river only at a diagonal cell corner. Comparing
	// cell indices for overlap alone incorrectly caps this canonical 8-neighbour
	// chain. Never connect a missing row or jump over a non-river cell gap.
	const auto RunsTouch=[](const FRiverRun& A,const FRiverRun& B)
	{
		return static_cast<int64>(A.First)<=static_cast<int64>(B.Last)+1
			&&static_cast<int64>(B.First)<=static_cast<int64>(A.Last)+1;
	};
	const auto RunsForRow=[&](int32 Y)
	{
		TArray<FRiverRun> Runs;
		if(Y<Visible.Min.Y||Y>=Visible.Max.Y)return Runs;
		bool bOpen=false;
		for(int32 X=Visible.Min.X;X<Visible.Max.X;++X)
		{
			FGridCell Cell;const bool bRiver=Terrain->TryGetCell(FIntPoint(X,Y),Cell)&&Cell.bIsRiverbed&&Cell.bIsPlayable;
			if(!bRiver){if(!bLegacyRiverRibbon)bOpen=false;continue;}
			// Preserve each canonical bank elevation rather than levelling the
			// entire ribbon to its highest cell. Real geography may be below MSL.
			const float Height=Terrain->IsUsingRealGeography()?Cell.Elevation:FMath::Max(0.f,Cell.Elevation);
			if(!bOpen){Runs.Add({X,X,Height,Height});bOpen=true;}
			else {Runs.Last().Last=X;Runs.Last().HeightRight=Height;}
		}
		return Runs;
	};
	TArray<FRiverRun> PreviousRuns;
	for(int32 Y=Visible.Min.Y;Y<Visible.Max.Y;++Y)
	{
		TArray<FRiverRun> Runs=RunsForRow(Y);const TArray<FRiverRun> NextRuns=RunsForRow(Y+1);
		for(FRiverRun& Run:Runs)
		{
			int32 Previous=INDEX_NONE;
			for(const FRiverRun& P:PreviousRuns)if(bLegacyRiverRibbon||RunsTouch(P,Run)){Previous=P.Section;break;}
			const float Left=Run.First*Terrain->CellSize,Right=(Run.Last+1)*Terrain->CellSize;
			if(Previous==INDEX_NONE)Previous=AddRiverSection(Y*Terrain->CellSize,Left,Right,Run.HeightLeft,Run.HeightRight,INDEX_NONE);
			Run.Section=AddRiverSection((Y+.5f)*Terrain->CellSize,Left,Right,Run.HeightLeft,Run.HeightRight,Previous);
			const bool bHasNext=NextRuns.ContainsByPredicate([&Run,bLegacyRiverRibbon,&RunsTouch](const FRiverRun& N){return bLegacyRiverRibbon||RunsTouch(N,Run);});
			if(!bHasNext)AddRiverSection((Y+1)*Terrain->CellSize,Left,Right,Run.HeightLeft,Run.HeightRight,Run.Section);
		}
		PreviousRuns=MoveTemp(Runs);
	}
	}
	UStaticMesh* Water = MakeSurface(Owner, TEXT("Aftershock_ContinuousRiver"), WaterVertices, WaterIndices, WaterComponent->GetMaterial(0));
	if (!WaterVertices.IsEmpty() && !Water) return false;
	WaterComponent->SetStaticMesh(Water);
	Cached.bHasSignature=true;Cached.GeometrySignature=GeometrySignature;Cached.WaterComponent=WaterComponent;
	Cached.GroundMesh=Ground;Cached.WaterMesh=Water;
	++Cached.Stats.SuccessfulGroundBuilds;Cached.Stats.LastLOD0Triangles=Indices.Num()/3;Cached.Stats.LastRenderLODs=Ground?Ground->GetNumLODs():0;
	ApplyAftershockTerrainPresentationBudget(GroundComponent,Owner->GetWorld());
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_TERRAIN_GEOMETRY_BUDGET builds=%llu collisionCookAttempts=%llu cacheHits=%llu cacheLimit=64 weakOnly=1 collisionLOD=0 renderLODs=%d lodTriangles=%d,%d,%d,%d canonicalWrites=0"),
		Cached.Stats.SuccessfulGroundBuilds,Cached.Stats.CollisionCookAttempts,Cached.Stats.GroundCacheHits,Cached.Stats.LastRenderLODs,Indices.Num()/3,ExtraLODIndices[0].Num()/3,ExtraLODIndices[1].Num()/3,ExtraLODIndices[2].Num()/3);
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_PRESENTATION_SURFACE groundVertices=%d groundTriangles=%d riverVertices=%d riverTriangles=%d canonicalSeed=%d visibleBounds=(%d,%d)-(%d,%d) residentCells=%d maxWindowCellsPerAxis=75"),
		Vertices.Num(), Indices.Num() / 3, WaterVertices.Num(), WaterIndices.Num() / 3, Terrain->TerrainSeed,Visible.Min.X,Visible.Min.Y,Visible.Max.X,Visible.Max.Y,Terrain->Cells.Num());
	return true;
}

bool BuildAftershockGlobeVisual(AActor* Owner, const UTerrainGridSubsystem* Terrain,
	UStaticMeshComponent* GlobeComponent, UStaticMeshComponent* TransitionComponent, UStaticMeshComponent* AtmosphereComponent)
{
	if (!Owner || !Terrain || !GlobeComponent || !TransitionComponent || !AtmosphereComponent) return false;
	const float SpanX = Terrain->GridWidth * Terrain->CellSize;
	const float SpanY = Terrain->GridHeight * Terrain->CellSize;
	const float Radius = FMath::Max(SpanX, SpanY) * 2.0f;
	const FVector Center(Terrain->GridOrigin.X + SpanX * 0.5f, Terrain->GridOrigin.Y + SpanY * 0.5f, -Radius);
	constexpr int32 LongitudeSegments = 128;
	constexpr int32 LatitudeSegments = 64;
	TArray<FSurfaceVertex> Vertices;
	TArray<int32> Indices;
	Vertices.Reserve((LongitudeSegments + 1) * (LatitudeSegments + 1));
	for (int32 Lat = 0; Lat <= LatitudeSegments; ++Lat)
	{
		const float Theta = PI * Lat / LatitudeSegments;
		for (int32 Lon = 0; Lon <= LongitudeSegments; ++Lon)
		{
			const float Phi = 2.0f * PI * Lon / LongitudeSegments;
			const FVector N(FMath::Sin(Theta) * FMath::Cos(Phi), FMath::Sin(Theta) * FMath::Sin(Phi), FMath::Cos(Theta));
			FSurfaceVertex& V = Vertices.AddDefaulted_GetRef();
			V.Position = Center + N * Radius;
			V.Normal = N;
			V.UV = FVector2f(static_cast<float>(Lon) / LongitudeSegments, static_cast<float>(Lat) / LatitudeSegments);
			// An original fictional globe, not a satellite map or a second playable terrain.
			const float Noise = FMath::PerlinNoise3D(N * 3.1f + FVector(4.2f, 1.7f, Terrain->TerrainSeed * 0.001f));
			const float Continental = Noise + FMath::Max(0.0f, N.Z - 0.5f) * 0.8f;
			const float Land = FMath::SmoothStep(-0.03f, 0.08f, Continental);
			const FLinearColor Vegetation = FLinearColor::LerpUsingHSV(FLinearColor(0.035f, 0.12f, 0.035f), FLinearColor(0.34f, 0.28f, 0.12f), FMath::Clamp(Noise * 1.7f + 0.4f, 0.0f, 1.0f));
			V.Color = FLinearColor::LerpUsingHSV(FLinearColor(0.012f, 0.075f, 0.19f), Vegetation, Land);
			V.Color.A = FMath::Lerp(0.18f, 0.88f, Land);
			if (Lat < LatitudeSegments && Lon < LongitudeSegments)
			{
				const int32 A = Lon + Lat * (LongitudeSegments + 1);
				// Sphere outward face order (latitude increases from the north pole).
				Indices.Append({A, A + 1, A + LongitudeSegments + 1, A + 1, A + LongitudeSegments + 2, A + LongitudeSegments + 1});
			}
		}
	}
	UStaticMesh* Globe = MakeSurface(Owner, TEXT("Aftershock_FictionalGlobe"), Vertices, Indices, GlobeComponent->GetMaterial(0));
	if (!Globe) return false;
	GlobeComponent->SetStaticMesh(Globe);
	// The same geometry, expanded around its real centre, makes a soft limb halo.
	for (FSurfaceVertex& Vertex : Vertices) Vertex.Position = Center + (Vertex.Position - Center) * 1.012f;
	AtmosphereComponent->SetStaticMesh(MakeSurface(Owner, TEXT("Aftershock_GlobeAtmosphere"), Vertices, Indices, AtmosphereComponent->GetMaterial(0)));

	Vertices.Reset(); Indices.Reset();
	constexpr int32 RingSegments = 256;
	constexpr int32 RingSteps = 12;
	const float RX = SpanX * 0.485f;
	const float RY = SpanY * 0.485f;
	for (int32 Ring = 0; Ring <= RingSteps; ++Ring)
	{
		const float Alpha = static_cast<float>(Ring) / RingSteps;
		const float Scale = FMath::Lerp(0.965f, 1.6f, Alpha);
		for (int32 Segment = 0; Segment <= RingSegments; ++Segment)
		{
			const float Angle = 2.0f * PI * Segment / RingSegments;
			const float DX = FMath::Cos(Angle) * RX;
			const float DY = FMath::Sin(Angle) * RY;
			const float EdgeHeight = SampleElevation(*Terrain, Terrain->GridWidth * 0.5f + DX * 0.965f / Terrain->CellSize - 0.5f,
				Terrain->GridHeight * 0.5f + DY * 0.965f / Terrain->CellSize - 0.5f);
			const float DistanceSquared = (DX * DX + DY * DY) * Scale * Scale;
			const float SphereHeight = Center.Z + FMath::Sqrt(FMath::Max(0.0f, Radius * Radius - DistanceSquared));
			FSurfaceVertex& V = Vertices.AddDefaulted_GetRef();
			V.Position = FVector(Center.X + DX * Scale, Center.Y + DY * Scale, FMath::Lerp(EdgeHeight - 8.f, SphereHeight + 3.f, FMath::SmoothStep(0.f, 1.f, Alpha)));
			V.Normal = FMath::Lerp(FVector::UpVector, (V.Position - Center).GetSafeNormal(), Alpha).GetSafeNormal();
			V.Color = FLinearColor(0.22f, 0.30f, 0.12f);
			V.UV = FVector2f(Segment / 64.0f, static_cast<float>(Ring));
			if (Ring < RingSteps && Segment < RingSegments)
			{
				const int32 A = Segment + Ring * (RingSegments + 1);
				Indices.Append({A, A + 1, A + RingSegments + 1, A + 1, A + RingSegments + 2, A + RingSegments + 1});
			}
		}
	}
	// Derive the annulus normals from its actual sloped surface, including the
	// canonical edge heights, rather than interpolating unrelated sphere normals.
	for (int32 Ring = 0; Ring <= RingSteps; ++Ring)
	{
		for (int32 Segment = 0; Segment <= RingSegments; ++Segment)
		{
			const int32 Wrapped = Segment % RingSegments;
			const FVector Radial = Vertices[Wrapped + FMath::Min(Ring + 1, RingSteps) * (RingSegments + 1)].Position
				- Vertices[Wrapped + FMath::Max(Ring - 1, 0) * (RingSegments + 1)].Position;
			const FVector Angular = Vertices[(Wrapped + 1) % RingSegments + Ring * (RingSegments + 1)].Position
				- Vertices[(Wrapped + RingSegments - 1) % RingSegments + Ring * (RingSegments + 1)].Position;
			Vertices[Segment + Ring * (RingSegments + 1)].Normal = FVector::CrossProduct(Radial, Angular).GetSafeNormal();
		}
	}
	TransitionComponent->SetStaticMesh(MakeSurface(Owner, TEXT("Aftershock_CityGlobeTransition"), Vertices, Indices, TransitionComponent->GetMaterial(0)));
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_GLOBE radiusCm=%.0f center=%s gridCoordinatesUnchanged=1 fictionalGlobe=1"), Radius, *Center.ToCompactString());
	return true;
}

bool BuildAftershockFloodSurface(AActor* Owner, const UTerrainGridSubsystem* Terrain,
	const TMap<FIntPoint, float>& LocalSeverity, UStaticMeshComponent* SurfaceComponent,
	FAftershockFloodSurfaceStats* OutStats, TMap<FIntPoint, float>* OutVisibleDisplayDepthCm,
	const FAftershockDisasterVisualSnapshot* PhysicalSnapshot)
{
	const double ReconstructionStart = FPlatformTime::Seconds();
	FAftershockFloodSurfaceStats Stats;
	if (OutStats) *OutStats = Stats;
	if (OutVisibleDisplayDepthCm) OutVisibleDisplayDepthCm->Reset();
	if (!Owner || !Terrain || !SurfaceComponent || Terrain->Cells.IsEmpty() || Terrain->CellSize <= 0.f)
	{
		if (SurfaceComponent) SurfaceComponent->SetStaticMesh(nullptr);
		return false;
	}
	UMaterialInterface* Material = SurfaceComponent->GetMaterial(0);
	const UMaterial* BaseMaterial = Material ? Material->GetMaterial() : nullptr;
	if (!BaseMaterial || !BaseMaterial->GetName().Contains(TEXT("M_DisasterFloodVolume")))
	{
		SurfaceComponent->SetStaticMesh(nullptr);
		UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_FLOOD_VOLUME_MATERIAL_MISSING expected=M_DisasterFloodVolume"));
		return false;
	}

	const bool bPhysical=PhysicalSnapshot&&PhysicalSnapshot->FloodModelVersion==1;
	const int32 RelaxationIterations = bPhysical?0:160;
	const float VisibleColumnCm = bPhysical?1.f:12.f;
	const float ShoreClipCm = bPhysical?.25f:2.f;
	constexpr float HiddenBaseCm = 4.f;
	float EncodingCm = 512.f;
	const FIntRect Resident=Terrain->GetResidentCellBounds(),Visible=GetAftershockDrawableCellBounds(*Terrain);
	const int32 CellCount = Terrain->Cells.Num();
	TArray<int32> Ordered;Ordered.Reserve(CellCount);for(int32 I=0;I<CellCount;++I)Ordered.Add(I);
	Ordered.Sort([Terrain](int32 A,int32 B){const FIntPoint X=Terrain->IndexToCoord(A),Y=Terrain->IndexToCoord(B);return X.Y==Y.Y?X.X<Y.X:X.Y<Y.Y;});
	TArray<float> Ground, Column, Delta, Outgoing;
	TBitArray<> Allowed(false, CellCount);
	Ground.SetNumUninitialized(CellCount);
	Column.Init(0.f, CellCount);
	Delta.Init(0.f, CellCount);
	Outgoing.Init(0.f, CellCount);
	for (int32 Index : Ordered)
	{
		const FGridCell& Cell = Terrain->Cells[Index];
		Ground[Index] = Cell.Elevation;
		const float Severity = LocalSeverity.FindRef(Cell.Coords);
		if(bPhysical)
		{
			const float* Head=PhysicalSnapshot->WaterSurfaceElevationCm.Find(Cell.Coords);
			const float* BedColumn=PhysicalSnapshot->WaterDepthCm.Find(Cell.Coords);
			if(!Cell.bIsPlayable||!Head||!BedColumn||!FMath::IsFinite(*Head)||!FMath::IsFinite(*BedColumn)||*BedColumn<=0.f)continue;
			// The simulation bed can be below the mapped bank. Only a head ABOVE
			// rendered canonical terrain is land inundation; never lift the full river column.
			Allowed[Index]=true;++Stats.CanonicalPositiveCells;
			Column[Index]=FMath::Max(0.f,*Head-Cell.Elevation);
			EncodingCm=FMath::Max(EncodingCm,Column[Index]+16.f);
		}
		else
		{
			if (!Cell.bIsPlayable || !FMath::IsFinite(Severity) || Severity <= 0.f) continue;
			Allowed[Index] = true;++Stats.CanonicalPositiveCells;
			Column[Index] = Cell.bIsRiverbed ? 12.f + 2.2f * FMath::Max(0.f, FMath::Min(100.f, Severity) - Cell.RiverCapacity * 18.75f)
				: FMath::Max(0.f, FMath::Min(100.f, Severity) - 6.f) * 0.18f;
		}
		Stats.InitialDisplayColumnSumCm += Column[Index];
	}
	Stats.EncodingCm=EncodingCm;
	if(auto* MID=Cast<UMaterialInstanceDynamic>(Material))MID->SetScalarParameterValue(TEXT("FloodEncodingCm"),EncodingCm);
	struct FFlux { int32 Source; int32 Target; float Amount; };
	TArray<FFlux> Fluxes;
	Fluxes.Reserve(CellCount * 2);
	for (int32 Iteration = 0; Iteration < RelaxationIterations; ++Iteration)
	{
		Fluxes.Reset();
		FMemory::Memzero(Outgoing.GetData(), Outgoing.Num() * sizeof(float));
		FMemory::Memzero(Delta.GetData(), Delta.Num() * sizeof(float));
		for (int32 A : Ordered)
		{
				const FIntPoint Coord=Terrain->IndexToCoord(A);
				if (!Allowed[A]) continue;
				for (const FIntPoint Offset : { FIntPoint(1, 0), FIntPoint(0, 1) })
				{
					const int32 B = Terrain->CoordToIndex(Coord+Offset);
					if (B==INDEX_NONE || !Allowed[B]) continue;
					const float HeadDifference = Ground[A] + Column[A] - Ground[B] - Column[B];
					const int32 Source = HeadDifference > 0.f ? A : B;
					const int32 Target = HeadDifference > 0.f ? B : A;
					const float Amount = FMath::Min(Column[Source], FMath::Abs(HeadDifference) * 0.22f);
					if (Amount <= 0.0001f) continue;
					Fluxes.Add({ Source, Target, Amount });
					Outgoing[Source] += Amount;
				}
		}
		for (const FFlux& Flux : Fluxes)
		{
			// Simultaneous edges share the source budget. This avoids negative
			// columns and iteration-order bias and conserves display volume.
			const float Scale = FMath::Min(1.f, Column[Flux.Source] * 0.9f / FMath::Max(Outgoing[Flux.Source], SMALL_NUMBER));
			const float Amount = Flux.Amount * Scale;
			Delta[Flux.Source] -= Amount;
			Delta[Flux.Target] += Amount;
		}
		for (int32 Index = 0; Index < CellCount; ++Index) Column[Index] = FMath::Max(0.f, Column[Index] + Delta[Index]);
	}
	for (int32 Index : Ordered)
	{
		Stats.FinalDisplayColumnSumCm += Column[Index];
		Stats.MaximumDisplayDepthCm = FMath::Max(Stats.MaximumDisplayDepthCm, Column[Index]);
		const float MeaningfulColumnCm = Terrain->Cells[Index].bIsRiverbed ? VisibleColumnCm + 2.f : VisibleColumnCm;
		if (Allowed[Index] && Column[Index] >= MeaningfulColumnCm)
		{
			++Stats.ReconstructedWetCells;
			if (!Terrain->Cells[Index].bIsRiverbed) ++Stats.ReconstructedLandCells;
			if (OutVisibleDisplayDepthCm) OutVisibleDisplayDepthCm->Add(Terrain->Cells[Index].Coords, FMath::Min(Column[Index], EncodingCm - HiddenBaseCm - 4.f));
		}
	}
	if (OutStats) *OutStats = Stats;
	if (Stats.ReconstructedWetCells == 0)
	{
		SurfaceComponent->SetStaticMesh(nullptr);
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_FLOOD_RECONSTRUCTION canonicalPositiveCells=%d visibleCells=0 residualRainSheetSuppressed=1 initialColumnSum=%.3f finalColumnSum=%.3f noGameplayWrites=1"),
			Stats.CanonicalPositiveCells, Stats.InitialDisplayColumnSumCm, Stats.FinalDisplayColumnSumCm);
		return false;
	}
	const auto SampleHead = [&](float GX, float GY)
	{
		GX = FMath::Clamp(GX, static_cast<float>(Resident.Min.X), static_cast<float>(Resident.Max.X - 1));
		GY = FMath::Clamp(GY, static_cast<float>(Resident.Min.Y), static_cast<float>(Resident.Max.Y - 1));
		const int32 X0 = FMath::FloorToInt(GX), Y0 = FMath::FloorToInt(GY);
		const float FX = GX - X0, FY = GY - Y0;
		float WeightedHead = 0.f, WeightSum = 0.f;
		for (int32 DY = 0; DY <= 1; ++DY)
		{
			for (int32 DX = 0; DX <= 1; ++DX)
			{
				const int32 I = Terrain->CoordToIndex(FIntPoint(FMath::Min(X0+DX,Resident.Max.X-1),FMath::Min(Y0+DY,Resident.Max.Y-1)));
				if (I==INDEX_NONE || (!bPhysical&&(!Allowed[I] || Column[I] <= 0.1f))) continue;
				const float W = (DX ? FX : 1.f - FX) * (DY ? FY : 1.f - FY);
				// Preserve absolute heads below a river bank as well as above it.
				// Renormalizing only over overbank columns invents wet wedges across
				// dry banks. A dry physical node contributes its actual ground head.
				const float* PhysicalHead=bPhysical?PhysicalSnapshot->WaterSurfaceElevationCm.Find(Terrain->Cells[I].Coords):nullptr;
				WeightedHead += (bPhysical?(PhysicalHead?*PhysicalHead:Ground[I]):Ground[I]+Column[I]) * W;
				WeightSum += W;
			}
		}
		return WeightSum > SMALL_NUMBER ? WeightedHead / WeightSum : SampleElevation(*Terrain, GX, GY);
	};
	const int32 Columns = Visible.Width() * 2 + 1;
	const int32 Rows = Visible.Height() * 2 + 1;
	struct FFloodSample { float GX; float GY; float GroundHeight; float Head; FVector Normal; float Arrival=1.f; };
	const auto SampleArrival=[&](float GX,float GY)
	{
		if(!bPhysical)return 1.f;
		const int32 X0=FMath::FloorToInt(GX),Y0=FMath::FloorToInt(GY);const float FX=GX-X0,FY=GY-Y0;
		float Sum=0.f,Weights=0.f;
		for(int32 DY=0;DY<=1;++DY)for(int32 DX=0;DX<=1;++DX)
		{
			const FIntPoint C(FMath::Clamp(X0+DX,Resident.Min.X,Resident.Max.X-1),FMath::Clamp(Y0+DY,Resident.Min.Y,Resident.Max.Y-1));
			if(const float* Arrival=PhysicalSnapshot->WaterArrivalFraction.Find(C))
			{const float W=(DX?FX:1.f-FX)*(DY?FY:1.f-FY);Sum+=FMath::Clamp(*Arrival,0.f,1.f)*W;Weights+=W;}
		}
		return Weights>SMALL_NUMBER?Sum/Weights:1.f;
	};
	TArray<FFloodSample> Samples;
	Samples.Reserve(Columns * Rows);
	for (int32 Y = 0; Y < Rows; ++Y)
	{
		for (int32 X = 0; X < Columns; ++X)
		{
			const float GX = Visible.Min.X+X * 0.5f - 0.5f, GY = Visible.Min.Y+Y * 0.5f - 0.5f;
			const float DX = SampleHead(GX + 0.25f, GY) - SampleHead(GX - 0.25f, GY);
			const float DY = SampleHead(GX, GY + 0.25f) - SampleHead(GX, GY - 0.25f);
			Samples.Add({ GX, GY, SampleElevation(*Terrain, GX, GY), SampleHead(GX, GY), FVector(-DX, -DY, Terrain->CellSize * 0.5f).GetSafeNormal(),SampleArrival(GX,GY) });
		}
	}
	TArray<FSurfaceVertex> Vertices;
	TArray<int32> Indices;
	TMap<FIntPoint, int32> SharedVertices;
	struct FBoundaryEdge { int32 A = 0; int32 B = 0; int32 Count = 0; };
	TMap<uint64, FBoundaryEdge> Boundaries;
	const auto EncodeColor = [EncodingCm](float Rise, float Depth, bool bTop,float Arrival)
	{
		const auto Byte = [](float V) { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(V * 255.f), 0, 255)); };
		// Fast StaticMesh color conversion is sRGB; precompensate so the shader
		// receives raw normalized DATA bytes, rather than gamma-shifted depths.
		return FLinearColor::FromSRGBColor(FColor(Byte(Rise / EncodingCm), Byte(Depth / EncodingCm), bTop ? 255 : 0, Byte(Arrival)));
	};
	const auto AddVertex = [&](const FFloodSample& Sample)
	{
		const FIntPoint Key(FMath::RoundToInt((Sample.GX-Visible.Min.X + 0.5f) * Terrain->CellSize * 10.f), FMath::RoundToInt((Sample.GY-Visible.Min.Y + 0.5f) * Terrain->CellSize * 10.f));
		if (const int32* Existing = SharedVertices.Find(Key)) return *Existing;
		FSurfaceVertex V;
		V.Position = FVector(Terrain->GridOrigin.X + (Sample.GX + 0.5f) * Terrain->CellSize,
			Terrain->GridOrigin.Y + (Sample.GY + 0.5f) * Terrain->CellSize, Sample.GroundHeight - HiddenBaseCm);
		V.Normal = Sample.Normal;
		// Dynamic hydraulic flow velocity vector (downhill slope + river downstream tangent + outward spill):
		const float DownhillX = -(SampleElevation(*Terrain, Sample.GX + 0.5f, Sample.GY) - SampleElevation(*Terrain, Sample.GX - 0.5f, Sample.GY));
		const float DownhillY = -(SampleElevation(*Terrain, Sample.GX, Sample.GY + 0.5f) - SampleElevation(*Terrain, Sample.GX, Sample.GY - 0.5f));
		const float CenterX = static_cast<float>(Terrain->GridWidth) * 0.5f;
		const float NormY = FMath::Clamp(Sample.GY / FMath::Max(1, Terrain->GridHeight - 1), 0.f, 1.f);
		const float RiverMeanderDeriv = FMath::Cos(NormY * 2.0f * PI + static_cast<float>(Terrain->TerrainSeed) * 0.7f) * (Terrain->GridWidth * 0.22f);
		const FVector2D RiverTangent = FVector2D(RiverMeanderDeriv, Terrain->GridHeight * 1.2f).GetSafeNormal();
		const float BankOffset = Sample.GX - (CenterX + FMath::Sin(NormY * 2.0f * PI + static_cast<float>(Terrain->TerrainSeed) * 0.7f) * (Terrain->GridWidth * 0.22f));
		const FVector2D OutwardSpill(FMath::Clamp(BankOffset * 0.4f, -1.f, 1.f), 0.35f);
		FVector2D FlowDir = FVector2D(DownhillX, DownhillY).GetSafeNormal() * 0.50f + RiverTangent * 0.35f + OutwardSpill.GetSafeNormal() * 0.25f;
		if (FlowDir.IsNearlyZero()) FlowDir = FVector2D(0.f, 1.f);
		FlowDir.Normalize();
		if(bPhysical)
		{
			// Visual drift direction follows this event's canonical head gradient.
			// No synthetic river meander is introduced on a real regional source.
			FlowDir=FVector2D(-(SampleHead(Sample.GX+.5f,Sample.GY)-SampleHead(Sample.GX-.5f,Sample.GY)),
				-(SampleHead(Sample.GX,Sample.GY+.5f)-SampleHead(Sample.GX,Sample.GY-.5f))).GetSafeNormal();
			// A zero head gradient keeps zero visual drift.
		}
		V.UV = FVector2f(FlowDir.X, FlowDir.Y);
		const float Depth = FMath::Clamp(Sample.Head - Sample.GroundHeight, 0.f, EncodingCm - HiddenBaseCm - 4.f);
		V.Color = EncodeColor(Depth + HiddenBaseCm, Depth, true,Sample.Arrival);
		V.HydraulicUV=FVector2f(Depth+HiddenBaseCm,Depth);
		// A bounded classification sample is sufficient for a display exclusion
		// over the existing mapped river. It never changes the hydraulic head.
		FGridCell RiverCell;
		const bool bRiver=Terrain->TryGetCell(FIntPoint(FMath::FloorToInt(Sample.GX+.5f),FMath::FloorToInt(Sample.GY+.5f)),RiverCell)&&RiverCell.bIsRiverbed;
		V.ArrivalUV=FVector2f(bRiver?1.f:0.f,Sample.Arrival);
		const int32 Index = Vertices.Add(V);
		SharedVertices.Add(Key, Index);
		return Index;
	};
	const auto AddTriangle = [&](int32 A, int32 B, int32 C)
	{
		if (A == B || A == C || B == C) return;
		Indices.Append({ A, B, C });
		for (const FIntPoint Edge : { FIntPoint(A, B), FIntPoint(B, C), FIntPoint(C, A) })
		{
			const uint64 Key = (static_cast<uint64>(FMath::Min(Edge.X, Edge.Y)) << 32) | static_cast<uint32>(FMath::Max(Edge.X, Edge.Y));
			FBoundaryEdge& Boundary = Boundaries.FindOrAdd(Key);
			Boundary.A = Edge.X; Boundary.B = Edge.Y; ++Boundary.Count;
		}
	};
	const auto ClipAndAdd = [&](const FFloodSample& A, const FFloodSample& B, const FFloodSample& C)
	{
		if (FMath::Max3(A.Head - A.GroundHeight, B.Head - B.GroundHeight, C.Head - C.GroundHeight) < VisibleColumnCm) return;
		TArray<FFloodSample, TInlineAllocator<4>> Polygon;
		const FFloodSample Input[] = { A, B, C };
		for (int32 I = 0; I < 3; ++I)
		{
			const FFloodSample& P = Input[I];
			const FFloodSample& Q = Input[(I + 1) % 3];
			const float DP = P.Head - P.GroundHeight - ShoreClipCm, DQ = Q.Head - Q.GroundHeight - ShoreClipCm;
			if (DP >= 0.f) Polygon.Add(P);
			if ((DP >= 0.f) != (DQ >= 0.f))
			{
				const float T = DP / (DP - DQ);
				FFloodSample Cross;
				Cross.GX = FMath::Lerp(P.GX, Q.GX, T); Cross.GY = FMath::Lerp(P.GY, Q.GY, T);
				Cross.GroundHeight = FMath::Lerp(P.GroundHeight, Q.GroundHeight, T);
				Cross.Head = Cross.GroundHeight + ShoreClipCm;
				Cross.Normal = FMath::Lerp(P.Normal, Q.Normal, T).GetSafeNormal();
				Cross.Arrival=FMath::Lerp(P.Arrival,Q.Arrival,T);
				Polygon.Add(Cross);
			}
		}
		for (int32 I = 1; I + 1 < Polygon.Num(); ++I) AddTriangle(AddVertex(Polygon[0]), AddVertex(Polygon[I]), AddVertex(Polygon[I + 1]));
	};
	for (int32 Y = 0; Y < Rows - 1; ++Y)
	{
		for (int32 X = 0; X < Columns - 1; ++X)
		{
			const FIntPoint Coord(FMath::Clamp(FMath::RoundToInt(Visible.Min.X+X * 0.5f - 0.25f), Resident.Min.X, Resident.Max.X - 1), FMath::Clamp(FMath::RoundToInt(Visible.Min.Y+Y * 0.5f - 0.25f), Resident.Min.Y, Resident.Max.Y - 1));
			const int32 CellIndex=Terrain->CoordToIndex(Coord);
			if (!Terrain->IsPlayableCoord(Coord) || CellIndex==INDEX_NONE || !Allowed[CellIndex]) continue;
			const int32 A = X + Y * Columns;
			ClipAndAdd(Samples[A], Samples[A + Columns], Samples[A + 1]);
			ClipAndAdd(Samples[A + 1], Samples[A + Columns], Samples[A + Columns + 1]);
		}
	}
	Stats.SurfaceTriangles = Indices.Num() / 3;
	// Close only exposed boundaries, including the playable edge. Bottom vertices
	// stay hidden in ground; top vertices follow exactly the same GPU rise as water.
	for (const TPair<uint64, FBoundaryEdge>& Pair : Boundaries)
	{
		const FBoundaryEdge& Edge = Pair.Value;
		if (Edge.Count != 1) continue;
		const FSurfaceVertex TopA = Vertices[Edge.A], TopB = Vertices[Edge.B];
		const FVector SideNormal = FVector::CrossProduct(TopB.Position - TopA.Position, FVector::UpVector).GetSafeNormal();
		const int32 Base = Vertices.Num();
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			FSurfaceVertex V = Corner % 2 == 0 ? TopA : TopB;
			V.Normal = SideNormal;
			// Decode the compensated DATA color before changing the top-face mask.
			FColor Data = V.Color.ToFColor(true);
			Data.B = 0;
			if (Corner >= 2) { V.Position.Z -= 8.f; Data.R = 0; V.HydraulicUV.X=0.f; }
			V.Color = FLinearColor::FromSRGBColor(Data);
			Vertices.Add(V);
		}
		Indices.Append({ Base, Base + 1, Base + 2, Base + 1, Base + 3, Base + 2 });
		Stats.FrontTriangles += 2;
	}
	if (Indices.IsEmpty())
	{
		ReleaseTerrainTransientMeshReference(SurfaceComponent);
		return false;
	}
	if(auto* MID=Cast<UMaterialInstanceDynamic>(Material))MID->SetScalarParameterValue(TEXT("FloodPhysicalDataEnabled"),bPhysical?1.f:0.f);
	UStaticMesh* Surface = MakeSurface(Owner, TEXT("Aftershock_ReconstructedFloodVolume"), Vertices, Indices, Material, false, true,nullptr,bPhysical?3:1);
	if (!Surface)
	{
		ReleaseTerrainTransientMeshReference(SurfaceComponent);
		return false;
	}
	Surface->SetPositiveBoundsExtension(Surface->GetPositiveBoundsExtension() + FVector(0.f, 0.f, EncodingCm + 4.f));
	Surface->SetNegativeBoundsExtension(Surface->GetNegativeBoundsExtension() + FVector(0.f, 0.f, 8.f));
	Surface->CalculateExtendedBounds();
	SetTerrainTransientMeshReference(SurfaceComponent, Surface);
	if (OutStats) *OutStats = Stats;
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_FLOOD_RECONSTRUCTION canonicalPositiveCells=%d visibleCells=%d landCells=%d topTriangles=%d frontTriangles=%d vertices=%d maxDisplayDepthCm=%.2f initialColumnSum=%.3f finalColumnSum=%.3f relaxationIterations=%d physicalModel=%d shaderEncodingCm=%.2f collision=0 buildAtEventOrWindowChange=1 noGameplayWrites=1"),
		Stats.CanonicalPositiveCells, Stats.ReconstructedWetCells, Stats.ReconstructedLandCells, Stats.SurfaceTriangles, Stats.FrontTriangles, Vertices.Num(), Stats.MaximumDisplayDepthCm,
		Stats.InitialDisplayColumnSumCm, Stats.FinalDisplayColumnSumCm, RelaxationIterations,bPhysical,EncodingCm);
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_FLOOD_RECONSTRUCTION_CPU elapsedMs=%.3f includes=Relaxation_MeshPreparation_FastStaticMeshBuild excludes=GpuDraw"),
		(FPlatformTime::Seconds() - ReconstructionStart) * 1000.0);
	return true;
}
