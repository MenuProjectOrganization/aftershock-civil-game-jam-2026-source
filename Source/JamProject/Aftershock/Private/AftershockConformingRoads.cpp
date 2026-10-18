#include "AftershockConformingRoads.h"
#include "AftershockTerrainSurface.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "StaticMeshResources.h"
#include "HAL/PlatformTime.h"

namespace
{
	struct FRect { double MinX,MinY,MaxX,MaxY; int32 Batch; };
	struct FTile { TArray<FRect,TInlineAllocator<12>> Rects; };
	// A convex triangle clipped by four rectangle half-planes has at most7
	// vertices. Keep every intermediate polygon on the stack in this hot loop.
	using FClipPolygon=TArray<FVector,TInlineAllocator<8>>;
	void Mix(uint64& Hash,uint64 Value){Hash=(Hash^Value)*1099511628211ull;}
	bool IsLandRoad(const UTerrainGridSubsystem& Terrain,FIntPoint Coord)
	{
		FGridCell Cell;
		return Terrain.TryGetCell(Coord,Cell)&&Cell.bIsPlayable&&Cell.bHasRoad&&!Cell.bIsRiverbed
			&&Cell.BridgeID==INDEX_NONE&&Cell.OccupyingBuildingID==INDEX_NONE;
	}
	// Sutherland-Hodgman clipping preserves the ORIGINAL installed-ground plane.
	// Newly cut vertices interpolate Z; sampling the bilinear DEM here would make
	// a different surface from the actual rasterized triangle and reintroduce cuts.
	void Clip(FClipPolygon& Polygon,int32 Axis,double Bound,bool KeepGreater)
	{
		if(Polygon.IsEmpty())return;
		FClipPolygon Output;Output.Reserve(Polygon.Num()+1);
		FVector Previous=Polygon.Last();double Before=Previous[Axis]-Bound;
		bool WasInside=KeepGreater?Before>=0.:Before<=0.;
		for(const FVector& Current:Polygon)
		{
			const double After=Current[Axis]-Bound;const bool IsInside=KeepGreater?After>=0.:After<=0.;
			if(IsInside!=WasInside)
			{
				const double Denominator=Before-After;
				if(FMath::Abs(Denominator)>UE_DOUBLE_SMALL_NUMBER)
					Output.Add(FMath::Lerp(Previous,Current,FMath::Clamp(Before/Denominator,0.,1.)));
			}
			if(IsInside)Output.Add(Current);
			Previous=Current;Before=After;WasInside=IsInside;
		}
		Polygon=MoveTemp(Output);
	}
	void Emit(const FVector P[3],const FRect& Rect,TArray<FAftershockPresentationMeshVertex>& Vertices,TArray<int32>& Indices)
	{
		FClipPolygon Polygon{P[0],P[1],P[2]};
		Clip(Polygon,0,Rect.MinX,true);Clip(Polygon,0,Rect.MaxX,false);
		Clip(Polygon,1,Rect.MinY,true);Clip(Polygon,1,Rect.MaxY,false);
		if(Polygon.Num()<3)return;
		FVector Normal=FVector::CrossProduct(P[2]-P[0],P[1]-P[0]).GetSafeNormal();
		if(Normal.Z<0.)Normal=-Normal;
		if(Normal.Z<.0001)return;
		const double Offset=Rect.Batch==0?8.:Rect.Batch==1?14.:8.35;
		for(int32 Index=1;Index+1<Polygon.Num();++Index)
		{
			if(FMath::Abs(FVector::CrossProduct(Polygon[Index]-Polygon[0],Polygon[Index+1]-Polygon[0]).Z)<.001)continue;
			const int32 Base=Vertices.Num();
			for(const FVector& Position:{Polygon[0],Polygon[Index],Polygon[Index+1]})
			{
				auto& Vertex=Vertices.AddDefaulted_GetRef();Vertex.Position=Position+FVector(0,0,Offset);
				Vertex.Normal=Normal;Vertex.UV=FVector2f(Position.X/300.,Position.Y/300.);
				Vertex.Color=FLinearColor::White;
			}
			Indices.Append({Base,Base+1,Base+2});
		}
	}
}

FAftershockConformingRoads::FAftershockConformingRoads(AActor& InOwner):Owner(&InOwner){}
FAftershockConformingRoads::~FAftershockConformingRoads(){ResetPresentation();}

void FAftershockConformingRoads::DestroyChunk(FPresentedChunk& Chunk)
{
	for(auto& Weak:Chunk.Batches)if(auto* C=Weak.Get())C->DestroyComponent();
	Chunk={};
}
void FAftershockConformingRoads::ResetPresentation()
{
	for(auto& Pair:PresentedChunks)DestroyChunk(Pair.Value);
	PresentedChunks.Reset();SourceChunks.Reset();RoadTiles.Reset();CachedGround.Reset();bReady=bHasGeometry=false;
}
UMaterialInterface* FAftershockConformingRoads::ResolveMaterial(int32 Batch)
{
	if(auto* Existing=Materials[Batch].Get())return Existing;
	const TCHAR* Paths[]={TEXT("/Game/Environment/Materials/Realistic/M_EnvironmentAsphalt.M_EnvironmentAsphalt"),
		TEXT("/Game/Environment/Materials/Realistic/M_EnvironmentSidewalk.M_EnvironmentSidewalk"),
		TEXT("/Game/Environment/Materials/Realistic/M_EnvironmentLaneMarkings.M_EnvironmentLaneMarkings")};
	const FString Path(Paths[Batch]);
	if(!FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)))return nullptr;
	auto* M=LoadObject<UMaterialInterface>(nullptr,*Path);Materials[Batch]=M;return M;
}
void FAftershockConformingRoads::ApplyPresentationBudget()
{
	const auto* Installed=Ground.Get();const int32 Requested=Installed?FMath::Max(1,Installed->GetForcedLodModel()):1;
	for(auto& Pair:PresentedChunks)for(auto& Weak:Pair.Value.Batches)if(auto* C=Weak.Get())
	{
		const int32 MaxLOD=C->GetStaticMesh()?FMath::Max(1,C->GetStaticMesh()->GetNumLODs()):1;
		const int32 LOD=FMath::Clamp(Requested,1,MaxLOD);
		if(C->GetForcedLodModel()!=LOD)C->SetForcedLodModel(LOD);
		C->SetVisibility(bReady&&Installed&&Installed->IsVisible()&&C->GetStaticMesh()!=nullptr);
	}
}

bool FAftershockConformingRoads::Refresh(const UTerrainGridSubsystem& Terrain,UStaticMeshComponent* InstalledGround)
{
	const double Started=FPlatformTime::Seconds();auto* Host=Owner.Get();Ground=InstalledGround;
	UStaticMesh* Mesh=InstalledGround?InstalledGround->GetStaticMesh():nullptr;
	const auto* Data=Mesh?Mesh->GetRenderData():nullptr;const FIntRect Visible=GetAftershockDrawableCellBounds(Terrain);
	const auto Reject=[&](){ResetPresentation();return false;};
	if(!Host||!InstalledGround||!Data||Data->LODResources.IsEmpty()||!Mesh->bAllowCPUAccess||Visible.IsEmpty()
		||Visible.Width()>128||Visible.Height()>128||!FMath::IsFinite(Terrain.CellSize)||Terrain.CellSize<=0.f||Terrain.GridOrigin.ContainsNaN())return Reject();
	const FTransform GroundToWorld=InstalledGround->GetComponentTransform();if(GroundToWorld.ContainsNaN())return Reject();
	const FIntPoint FirstChunk=UTerrainGridSubsystem::CellToChunk(Visible.Min),LastChunk=UTerrainGridSubsystem::CellToChunk(Visible.Max-FIntPoint(1,1));
	const int64 ChunkBound=static_cast<int64>(LastChunk.X-FirstChunk.X+1)*(LastChunk.Y-FirstChunk.Y+1);
	if(ChunkBound<=0||ChunkBound>49)return Reject();
	uint64 Geometry=1469598103934665603ull;
	Mix(Geometry,reinterpret_cast<UPTRINT>(Data));Mix(Geometry,GetTypeHash(Visible.Min));Mix(Geometry,GetTypeHash(Visible.Max));
	Mix(Geometry,GetTypeHash(Terrain.GridOrigin));Mix(Geometry,GetTypeHash(Terrain.CellSize));
	Mix(Geometry,GetTypeHash(GroundToWorld.GetLocation()));Mix(Geometry,GetTypeHash(GroundToWorld.GetRotation()));Mix(Geometry,GetTypeHash(GroundToWorld.GetScale3D()));
	for(const auto& LOD:Data->LODResources)
	{
		if(!LOD.IndexBuffer.GetAllowCPUAccess()||!LOD.VertexBuffers.PositionVertexBuffer.GetAllowCPUAccess()
			||!LOD.VertexBuffers.PositionVertexBuffer.GetVertexData()||!LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices())return Reject();
		const auto View=LOD.IndexBuffer.GetArrayView();if(View.Num()==0||View.Num()%3)return Reject();
		Mix(Geometry,View.Num());Mix(Geometry,LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices());
	}
	const bool Reindex=!bHasGeometry||CachedGround.Get()!=Mesh||Geometry!=GeometrySignature;
	const auto ReadTriangle=[&](int32 LODIndex,int32 Offset,FVector (&P)[3])
	{
		const auto& LOD=Data->LODResources[LODIndex];const auto View=LOD.IndexBuffer.GetArrayView();
		if(Offset<0||Offset+2>=View.Num())return false;
		for(int32 Corner=0;Corner<3;++Corner)
		{
			const uint32 I=View[Offset+Corner];if(I>=LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices())return false;
			P[Corner]=GroundToWorld.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(I)));
			if(P[Corner].ContainsNaN())return false;
		}
		return true;
	};
	const auto CellRange=[&](const FVector (&P)[3],FIntPoint& First,FIntPoint& Last)
	{
		const double MinX=FMath::Min3(P[0].X,P[1].X,P[2].X),MaxX=FMath::Max3(P[0].X,P[1].X,P[2].X);
		const double MinY=FMath::Min3(P[0].Y,P[1].Y,P[2].Y),MaxY=FMath::Max3(P[0].Y,P[1].Y,P[2].Y);
		const double X0=(MinX-Terrain.GridOrigin.X)/Terrain.CellSize,X1=(MaxX-Terrain.GridOrigin.X)/Terrain.CellSize;
		const double Y0=(MinY-Terrain.GridOrigin.Y)/Terrain.CellSize,Y1=(MaxY-Terrain.GridOrigin.Y)/Terrain.CellSize;
		if(X1<=Visible.Min.X||X0>=Visible.Max.X||Y1<=Visible.Min.Y||Y0>=Visible.Max.Y)return false;
		First=FIntPoint(FMath::FloorToInt(FMath::Clamp(X0,static_cast<double>(Visible.Min.X),static_cast<double>(Visible.Max.X-1))),
			FMath::FloorToInt(FMath::Clamp(Y0,static_cast<double>(Visible.Min.Y),static_cast<double>(Visible.Max.Y-1))));
		Last=FIntPoint(FMath::Clamp(FMath::CeilToInt(FMath::Min(X1,static_cast<double>(Visible.Max.X)))-1,Visible.Min.X,Visible.Max.X-1),
			FMath::Clamp(FMath::CeilToInt(FMath::Min(Y1,static_cast<double>(Visible.Max.Y)))-1,Visible.Min.Y,Visible.Max.Y-1));
		return First.X<=Last.X&&First.Y<=Last.Y;
	};
	if(Reindex)
	{
		SourceChunks.Reset();
		for(int32 LOD=0;LOD<Data->LODResources.Num();++LOD)
		{
			const int32 N=Data->LODResources[LOD].IndexBuffer.GetArrayView().Num();
			for(int32 Offset=0;Offset<N;Offset+=3)
			{
				FVector P[3];if(!ReadTriangle(LOD,Offset,P))return Reject();FIntPoint A,B;if(!CellRange(P,A,B))continue;
				const auto CA=UTerrainGridSubsystem::CellToChunk(A),CB=UTerrainGridSubsystem::CellToChunk(B);
				for(int32 Y=CA.Y;Y<=CB.Y;++Y)for(int32 X=CA.X;X<=CB.X;++X)
				{
					auto& S=SourceChunks.FindOrAdd(FIntPoint(X,Y));if(S.TriangleOffsets.IsEmpty())S.TriangleOffsets.SetNum(Data->LODResources.Num());
					S.TriangleOffsets[LOD].Add(Offset);
				}
			}
		}
		if(SourceChunks.Num()>ChunkBound)return Reject();
		for(auto& P:PresentedChunks)P.Value.bHasSignature=false;
		GeometrySignature=Geometry;CachedGround=Mesh;bHasGeometry=true;
	}
	const double IndexFinished=FPlatformTime::Seconds();
	struct FInput{TMap<FIntPoint,int32> Masks;uint64 Signature=1469598103934665603ull;};
	TMap<FIntPoint,FInput> Inputs;
	const FIntPoint Directions[]={FIntPoint(1,0),FIntPoint(0,1),FIntPoint(-1,0),FIntPoint(0,-1)};
	for(int32 Y=Visible.Min.Y;Y<Visible.Max.Y;++Y)for(int32 X=Visible.Min.X;X<Visible.Max.X;++X)
	{
		const FIntPoint C(X,Y);if(!IsLandRoad(Terrain,C))continue;int32 Mask=0;
		for(int32 D=0;D<4;++D)
		{
			FGridCell N;if(!Terrain.TryGetCell(C+Directions[D],N)||!N.bIsPlayable||!N.bHasRoad||N.OccupyingBuildingID!=INDEX_NONE||
				(N.bIsRiverbed&&N.BridgeID==INDEX_NONE))continue;
			if(N.BridgeID!=INDEX_NONE&&!N.bBridgeEndpoint&&((N.BridgeAxis==1&&Directions[D].Y!=0)||(N.BridgeAxis==2&&Directions[D].X!=0)))continue;
			Mask|=1<<D;
		}
		auto& I=Inputs.FindOrAdd(UTerrainGridSubsystem::CellToChunk(C));I.Masks.Add(C,Mask);Mix(I.Signature,GetTypeHash(C));Mix(I.Signature,Mask);
	}
	if(Inputs.Num()>ChunkBound)return Reject();
	TArray<FIntPoint> Retired;for(const auto& P:PresentedChunks)if(!Inputs.Contains(P.Key))Retired.Add(P.Key);
	for(const auto& C:Retired){DestroyChunk(PresentedChunks.FindChecked(C));PresentedChunks.Remove(C);}
	const double ScanFinished=FPlatformTime::Seconds();double ClipSeconds=0.,MeshSeconds=0.;int32 Changed=0,Cached=0,ClippedSourceTriangles=0;
	TArray<FIntPoint> Ordered;Inputs.GetKeys(Ordered);Ordered.Sort([](const auto& A,const auto& B){return A.Y==B.Y?A.X<B.X:A.Y<B.Y;});
	for(const auto& Key:Ordered)
	{
		const auto& I=Inputs.FindChecked(Key);auto& Chunk=PresentedChunks.FindOrAdd(Key);
		bool ComponentsAlive=true;for(const auto& Weak:Chunk.Batches)ComponentsAlive&=Weak.IsValid();
		if(Chunk.bHasSignature&&Chunk.Signature==I.Signature&&ComponentsAlive){++Cached;continue;}
		const auto* Spatial=SourceChunks.Find(Key);if(!Spatial||Spatial->TriangleOffsets.Num()!=Data->LODResources.Num())return Reject();
		for(int32 B=0;B<3;++B)
		{
			UMaterialInterface* Material=ResolveMaterial(B);if(!Material)return Reject();
			if(!Chunk.Batches[B].IsValid())
			{
				const FString Name=FString::Printf(TEXT("ConformingRoadChunk_%d_%d_%d"),Key.X,Key.Y,B);
				auto* C=NewObject<UStaticMeshComponent>(Host,MakeUniqueObjectName(Host,UStaticMeshComponent::StaticClass(),FName(*Name)),RF_Transient);
				Host->AddInstanceComponent(C);C->SetupAttachment(Host->GetRootComponent());C->SetAbsolute(true,true,true);C->SetWorldTransform(FTransform::Identity);
				C->SetMobility(EComponentMobility::Movable);C->SetCollisionEnabled(ECollisionEnabled::NoCollision);C->SetCanEverAffectNavigation(false);
				C->SetCastShadow(false);C->SetComponentTickEnabled(false);C->PrimaryComponentTick.bCanEverTick=false;C->SetTextureForceResidentFlag(false);
				C->SetMaterial(0,Material);C->SetVisibility(false);C->RegisterComponent();Chunk.Batches[B]=C;
			}
		}
		const double ClipStarted=FPlatformTime::Seconds();
		TMap<FIntPoint,FTile> Tiles;
		for(const auto& M:I.Masks)
		{
			const auto C=M.Key;const int32 Mask=M.Value;auto& T=Tiles.FindOrAdd(C);
			const double MX=Terrain.GridOrigin.X+C.X*static_cast<double>(Terrain.CellSize),MY=Terrain.GridOrigin.Y+C.Y*static_cast<double>(Terrain.CellSize);
			const double Bound[]={0.,.12,.88,1.};
			for(int32 PY=0;PY<3;++PY)for(int32 PX=0;PX<3;++PX)
			{
				const bool A=(PX==1&&PY==1)||(PY==1&&((PX==2&&(Mask&1))||(PX==0&&(Mask&4))))||(PX==1&&((PY==2&&(Mask&2))||(PY==0&&(Mask&8))));
				T.Rects.Add({MX+Bound[PX]*Terrain.CellSize,MY+Bound[PY]*Terrain.CellSize,MX+Bound[PX+1]*Terrain.CellSize,MY+Bound[PY+1]*Terrain.CellSize,A?0:1});
			}
			const bool H=(Mask&5)!=0&&(Mask&10)==0,V=(Mask&10)!=0&&(Mask&5)==0;
			if(H||V)for(double Dash:{.22,.72})T.Rects.Add({MX+(H?Dash:.494)*Terrain.CellSize,MY+(H?.494:Dash)*Terrain.CellSize,
				MX+(H?Dash+.16:.506)*Terrain.CellSize,MY+(H?.506:Dash+.16)*Terrain.CellSize,2});
		}
		TArray<FAftershockPresentationMeshVertex> Vertices[3];TArray<TArray<int32>> Indices[3];TArray<TSet<FIntPoint>> LODCoverage;LODCoverage.SetNum(Data->LODResources.Num());
		const FIntPoint ChunkMin(Key.X*UTerrainGridSubsystem::ChunkSizeCells,Key.Y*UTerrainGridSubsystem::ChunkSizeCells),ChunkMax=ChunkMin+FIntPoint(UTerrainGridSubsystem::ChunkSizeCells,UTerrainGridSubsystem::ChunkSizeCells);
		for(int32 LOD=0;LOD<Data->LODResources.Num();++LOD)
		{
			for(int32 B=0;B<3;++B)Indices[B].AddDefaulted();
			for(int32 Offset:Spatial->TriangleOffsets[LOD])
			{
				++ClippedSourceTriangles;FVector P[3];if(!ReadTriangle(LOD,Offset,P))return Reject();FIntPoint A,B;if(!CellRange(P,A,B))continue;
				A.X=FMath::Max(A.X,ChunkMin.X);A.Y=FMath::Max(A.Y,ChunkMin.Y);B.X=FMath::Min(B.X,ChunkMax.X-1);B.Y=FMath::Min(B.Y,ChunkMax.Y-1);
				const double X0=FMath::Min3(P[0].X,P[1].X,P[2].X),X1=FMath::Max3(P[0].X,P[1].X,P[2].X),Y0=FMath::Min3(P[0].Y,P[1].Y,P[2].Y),Y1=FMath::Max3(P[0].Y,P[1].Y,P[2].Y);
				for(int32 Y=A.Y;Y<=B.Y;++Y)for(int32 X=A.X;X<=B.X;++X)
				{
					const auto* T=Tiles.Find(FIntPoint(X,Y));if(!T)continue;
					for(const auto& Rect:T->Rects)
					{
						if(X1<=Rect.MinX||X0>=Rect.MaxX||Y1<=Rect.MinY||Y0>=Rect.MaxY)continue;
						const int32 Before=Vertices[Rect.Batch].Num();Emit(P,Rect,Vertices[Rect.Batch],Indices[Rect.Batch][LOD]);
						if(Rect.Batch==0&&Vertices[0].Num()>Before)LODCoverage[LOD].Add(FIntPoint(X,Y));
					}
				}
			}
		}
		for(const auto& Coverage:LODCoverage)if(Coverage.Num()!=I.Masks.Num())return Reject();
		ClipSeconds+=FPlatformTime::Seconds()-ClipStarted;
		const double BuildStarted=FPlatformTime::Seconds();UStaticMesh* New[3]={nullptr,nullptr,nullptr};
		for(int32 B=0;B<3;++B)if(!Indices[B][0].IsEmpty())
		{
			TArray<TArray<int32>> Extra;for(int32 LOD=1;LOD<Indices[B].Num();++LOD)Extra.Add(Indices[B][LOD]);
			New[B]=BuildAftershockPresentationMesh(Host,TEXT("Aftershock_ConformingRoadChunk"),Vertices[B],Indices[B][0],Chunk.Batches[B]->GetMaterial(0),&Extra);
			if(!New[B])return Reject();
		}
		for(int32 B=0;B<3;++B)Chunk.Batches[B]->SetStaticMesh(New[B]);
		MeshSeconds+=FPlatformTime::Seconds()-BuildStarted;Chunk.Tiles=MoveTemp(LODCoverage[0]);Chunk.Signature=I.Signature;Chunk.bHasSignature=true;++Changed;
	}
	RoadTiles.Reset();for(const auto& P:PresentedChunks)for(const auto& C:P.Value.Tiles)RoadTiles.Add(C);
	bReady=true;ApplyPresentationBudget();
	if(Reindex||Changed||!Retired.IsEmpty())
	{
		uint64 IndexBytes=0;for(const auto& P:SourceChunks)for(const auto& L:P.Value.TriangleOffsets)IndexBytes+=L.GetAllocatedSize();
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CONFORMING_ROADS tiles=%d groundLODs=%d chunks=%d components=%d changedChunks=%d cachedChunks=%d retiredChunks=%d sourceIndexRebuilt=%d sourceTriangleOffsetsBytes=%llu visitedTriangles=%d buildMs=%.3f indexMs=%.3f scanMs=%.3f clipMs=%.3f meshMs=%.3f perRoadActors=0 collision=0 canonicalWrites=0 meshDescription=0 timingCPUOnly=1"),
			RoadTiles.Num(),Data->LODResources.Num(),PresentedChunks.Num(),PresentedChunks.Num()*3,Changed,Cached,Retired.Num(),Reindex,IndexBytes,ClippedSourceTriangles,
			(FPlatformTime::Seconds()-Started)*1000.,(IndexFinished-Started)*1000.,(ScanFinished-IndexFinished)*1000.,ClipSeconds*1000.,MeshSeconds*1000.);
	}
	return true;
}
