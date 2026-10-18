#include "Aftershock/Public/DisasterPresentationSubsystem.h"

#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/CityPresentationSubsystem.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "AftershockArchitecturalRecipe.h"
#include "GameFramework/PlayerController.h"
#include "AftershockTerrainSurface.h"
#include "AftershockPresentationBudget.h"
#include "AftershockRuntimeMeshUV.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "PhysicsEngine/BodySetup.h"
#include "TimerManager.h"
#include "Scalability.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr float WeatherUpdateSeconds = 0.08f;
	constexpr int32 MaxWindStreaks = 320;
	constexpr int32 MaxRainStreaks = 640;
	constexpr int32 MaxQuakeFissures = 512;
	constexpr int32 MaxVolcanoSurfaceCells=1500,MaxVolcanoSpatter=96;
	constexpr float VolcanoCullDistanceCm=5000000.f;
	constexpr const TCHAR* VolcanoLavaPath=TEXT("/Game/FX/Aftershock/Volcano/M_VolcanoLavaVolume.M_VolcanoLavaVolume");
	constexpr const TCHAR* VolcanoVentPath=TEXT("/Game/FX/Aftershock/Volcano/M_VolcanoVentRock.M_VolcanoVentRock");
	constexpr const TCHAR* VolcanoPlumePath=TEXT("/Game/FX/Aftershock/Volcano/M_VolcanoPlume.M_VolcanoPlume");
	constexpr const TCHAR* VolcanoSpatterPath=TEXT("/Game/FX/Aftershock/Volcano/M_VolcanoSpatter.M_VolcanoSpatter");
	bool DrawRegionalVolcano(const UWorld* World)
	{
		const auto Budget=GetAftershockPresentationBudget(World);
		return Budget.ViewDistanceCm<VolcanoCullDistanceCm&&Budget.OverviewBlend<.2f;
	}
	UMaterialInstanceDynamic* VolcanoMaterial(UStaticMeshComponent* Component,const TCHAR* Path)
	{
		if(auto* Base=LoadObject<UMaterialInterface>(nullptr,Path))
		{auto* Material=UMaterialInstanceDynamic::Create(Base,Component);Component->SetMaterial(0,Material);return Material;}
		UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_VOLCANO_MATERIAL_UNAVAILABLE path=%s"),Path);return nullptr;
	}
	struct FVolcanoVertex
	{
		FVector Position=FVector::ZeroVector;
		/** Nonzero only for smooth fire hulls; existing rock/lava retains face normals. */
		FVector ShadingNormal=FVector::ZeroVector;
		FVector2f UV=FVector2f::ZeroVector;
		FLinearColor Color=FLinearColor::White;
	};
	void VolcanoTriangle(TArray<int32>& Indices,int32 A,int32 B,int32 C){Indices.Append({A,B,C});}
	UStaticMesh* VolcanoMesh(AActor* Owner,const TCHAR* Name,const TArray<FVolcanoVertex>& Vertices,const TArray<int32>& Indices,UMaterialInterface* Material)
	{
		if(Vertices.IsEmpty()||Indices.IsEmpty()||!Material)return nullptr;
		FMeshDescription Description;FStaticMeshAttributes Attributes(Description);Attributes.Register();
		auto Positions=Attributes.GetVertexPositions();auto Normals=Attributes.GetVertexInstanceNormals();auto Tangents=Attributes.GetVertexInstanceTangents();
		auto Signs=Attributes.GetVertexInstanceBinormalSigns();auto Colors=Attributes.GetVertexInstanceColors();auto UVs=Attributes.GetVertexInstanceUVs();UVs.SetNumChannels(1);
		const auto Group=Description.CreatePolygonGroup();Attributes.GetPolygonGroupMaterialSlotNames()[Group]=TEXT("VolcanicDisplay");
		TArray<FVertexID> IDs;IDs.Reserve(Vertices.Num());for(const auto& Vertex:Vertices){const auto ID=Description.CreateVertex();Positions[ID]=FVector3f(Vertex.Position);IDs.Add(ID);}
		for(int32 T=0;T+2<Indices.Num();T+=3)
		{
			const auto& A=Vertices[Indices[T]];const auto& B=Vertices[Indices[T+1]];const auto& C=Vertices[Indices[T+2]];
			const FVector Normal=FVector::CrossProduct(C.Position-A.Position,B.Position-A.Position).GetSafeNormal();
			FVector Tangent=FVector::VectorPlaneProject(FVector::ForwardVector,Normal).GetSafeNormal();if(Tangent.IsNearlyZero())Tangent=FVector::RightVector;
			FVertexInstanceID Corners[3];for(int32 K=0;K<3;++K)
			{
				const auto& V=Vertices[Indices[T+K]];Corners[K]=Description.CreateVertexInstance(IDs[Indices[T+K]]);
				const FVector VertexNormal=V.ShadingNormal.IsNearlyZero()?Normal:V.ShadingNormal.GetSafeNormal();
				FVector VertexTangent=FVector::VectorPlaneProject(Tangent,VertexNormal).GetSafeNormal();
				if(VertexTangent.IsNearlyZero())VertexTangent=FVector::VectorPlaneProject(FVector::RightVector,VertexNormal).GetSafeNormal();
				Normals[Corners[K]]=FVector3f(VertexNormal);Tangents[Corners[K]]=FVector3f(VertexTangent);Signs[Corners[K]]=1.f;
				// Fast mesh builds encode vertex colors to sRGB bytes. Precompensate
				// DATA channels so canonical arrival fractions are not gamma-warped.
				const auto Byte=[](float Value){return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Value*255.f),0,255));};
				const auto Data=FLinearColor::FromSRGBColor(FColor(Byte(V.Color.R),Byte(V.Color.G),Byte(V.Color.B),Byte(V.Color.A)));
				Colors[Corners[K]]=FVector4f(Data.R,Data.G,Data.B,Data.A);UVs.Set(Corners[K],0,V.UV);
			}
			Description.CreateTriangle(Group,MakeArrayView(Corners));
		}
		auto* Mesh=NewObject<UStaticMesh>(Owner,MakeUniqueObjectName(Owner,UStaticMesh::StaticClass(),Name),RF_Transient);
		bool KeepCPU=false;
#if WITH_EDITOR
		KeepCPU=IsRunningCommandlet()&&Owner->ActorHasTag(TEXT("Aftershock_MeshCpuAudit"));
#endif
		Mesh->bAllowCPUAccess=KeepCPU;Mesh->GetStaticMaterials().Add(FStaticMaterial(Material,TEXT("VolcanicDisplay")));
		UStaticMesh::FBuildMeshDescriptionsParams Params;Params.bFastBuild=true;Params.bBuildSimpleCollision=false;Params.bCommitMeshDescription=false;Params.bMarkPackageDirty=false;Params.bAllowCpuAccess=KeepCPU;
		TArray<const FMeshDescription*> Descriptions{&Description};if(!Mesh->BuildFromMeshDescriptions(Descriptions,Params))return nullptr;
		InitializeAftershockRuntimeMeshUV(*Mesh,Vertices,Indices,true);
		if(auto* Body=Mesh->GetBodySetup()){Body->CollisionTraceFlag=CTF_UseSimpleAsComplex;Body->DefaultInstance.SetCollisionEnabled(ECollisionEnabled::NoCollision);}
		return Mesh;
	}
	void ReleaseDisasterTransientMeshReference(UStaticMeshComponent* Component)
	{
		// Release the component reference only. Its old render proxy survives
		// until deferred end-of-frame recreation; UObject GC owns the fence.
		if (Component) Component->SetStaticMesh(nullptr);
	}

	void SetDisasterTransientMeshReference(UStaticMeshComponent* Component, UStaticMesh* NewMesh)
	{
		// RF_Transient does not establish exclusive ownership or prove that
		// no other component/MID still needs this object's render resources.
		if (Component) Component->SetStaticMesh(NewMesh);
	}
	/** All geometry samples the active version's SAME elevation; no cell or source writes. */
	bool VolcanoGround(const UTerrainGridSubsystem& Terrain,FVector& World)
	{
		float Height=0.f;if(!Terrain.TrySampleWorldElevation(World,Height)||!FMath::IsFinite(Height))return false;World.Z=Height;return true;
	}
	bool BuildVolcanoLavaMesh(AActor* Owner,const UTerrainGridSubsystem& Terrain,const TMap<FIntPoint,float>& Lava,const FIntPoint Source,UStaticMeshComponent* Component,FAftershockVolcanoVisualStats& Stats,const TMap<FIntPoint,float>* ArrivalFractions=nullptr)
	{
		Stats.CanonicalLavaCells=0;Stats.DrawnLavaCells=0;Stats.LavaTopTriangles=0;Stats.LavaSkirtTriangles=0;Stats.MinimumGroundClearanceCm=MAX_flt;
		const FIntRect Drawable=GetAftershockDrawableCellBounds(Terrain);
		TArray<FIntPoint> Cells;for(const auto& Pair:Lava)if(FMath::IsFinite(Pair.Value)&&Pair.Value>0.f&&Terrain.IsPlayableCoord(Pair.Key))
		{++Stats.CanonicalLavaCells;if(Terrain.IsUsingRealGeography()||Drawable.Contains(Pair.Key))Cells.Add(Pair.Key);}
		Cells.Sort([Source](FIntPoint A,FIntPoint B){const int64 DA=FMath::Abs(static_cast<int64>(A.X)-Source.X)+FMath::Abs(static_cast<int64>(A.Y)-Source.Y),DB=FMath::Abs(static_cast<int64>(B.X)-Source.X)+FMath::Abs(static_cast<int64>(B.Y)-Source.Y);return DA==DB?(A.Y==B.Y?A.X<B.X:A.Y<B.Y):DA<DB;});
		if(Cells.Num()>MaxVolcanoSurfaceCells)Cells.SetNum(MaxVolcanoSurfaceCells,EAllowShrinking::No);
		TMap<FIntPoint,TArray<FVector>> Grounds;TSet<FIntPoint> Selected;
		for(FIntPoint Coord:Cells)
		{
			TArray<FVector> Ground;Ground.SetNum(9);bool Valid=true;
			for(int32 Y=0;Y<3;++Y)for(int32 X=0;X<3;++X){Ground[X+Y*3]=Terrain.GridOrigin+FVector((Coord.X+X*.5f)*Terrain.CellSize,(Coord.Y+Y*.5f)*Terrain.CellSize,0);Valid&=VolcanoGround(Terrain,Ground[X+Y*3]);}
			if(Valid){Selected.Add(Coord);Grounds.Add(Coord,MoveTemp(Ground));}
		}
		TArray<FVolcanoVertex> Vertices;TArray<int32> Indices;Vertices.Reserve(Cells.Num()*18);Indices.Reserve(Cells.Num()*60);
		float MaximumDistance=1.f;for(FIntPoint Coord:Cells)MaximumDistance=FMath::Max(MaximumDistance,static_cast<float>(FVector2D(Coord-Source).Size()));
		for(FIntPoint Coord:Cells)
		{
			const auto* GroundPtr=Grounds.Find(Coord);if(!GroundPtr)continue;const auto& Ground=*GroundPtr;
			const int32 Base=Vertices.Num();
			for(int32 Y=0;Y<3;++Y)for(int32 X=0;X<3;++X)
			{
				const float GX=Coord.X+X*.5f,GY=Coord.Y+Y*.5f;float Strength=0.f,ArrivalSum=0.f;int32 Count=0,ArrivalCount=0;
				for(int32 NY=FMath::FloorToInt(GY-.001f);NY<=FMath::FloorToInt(GY+.001f);++NY)for(int32 NX=FMath::FloorToInt(GX-.001f);NX<=FMath::FloorToInt(GX+.001f);++NX)
				{const FIntPoint At(NX,NY);const float S=Lava.FindRef(At);if(S>0.f){Strength+=FMath::Clamp(S/100.f,0.f,1.f);++Count;if(ArrivalFractions)if(const float* A=ArrivalFractions->Find(At)){ArrivalSum+=FMath::Clamp(*A,0.f,1.f);++ArrivalCount;}}}
				Strength=Count?Strength/Count:FMath::Clamp(Lava.FindRef(Coord)/100.f,0.f,1.f);
				const float Offset=14.f+Strength*55.f;const float Arrival=ArrivalCount?ArrivalSum/ArrivalCount:FMath::Clamp(static_cast<float>(FVector2D(GX-Source.X-.5f,GY-Source.Y-.5f).Size()/MaximumDistance),0.f,1.f)*.82f;
				FVolcanoVertex V;V.Position=Ground[X+Y*3]+FVector(0,0,Offset);V.UV=FVector2f(GX,GY);V.Color=FLinearColor(Strength,Arrival,1.f,1.f);Vertices.Add(V);Stats.MinimumGroundClearanceCm=FMath::Min(Stats.MinimumGroundClearanceCm,Offset);
			}
			for(int32 Y=0;Y<2;++Y)for(int32 X=0;X<2;++X){const int32 A=Base+X+Y*3;VolcanoTriangle(Indices,A,A+3,A+1);VolcanoTriangle(Indices,A+1,A+3,A+4);Stats.LavaTopTriangles+=2;}
			const int32 Bottom=Vertices.Num();for(int32 K=0;K<9;++K){auto V=Vertices[Base+K];V.Position=Ground[K]+FVector(0,0,3);V.Color.B=0;Vertices.Add(V);}
			for(int32 Y=0;Y<2;++Y)for(int32 X=0;X<2;++X){const int32 A=Bottom+X+Y*3;VolcanoTriangle(Indices,A,A+1,A+3);VolcanoTriangle(Indices,A+1,A+4,A+3);}
			const FIntPoint Adjacent[]={FIntPoint(0,-1),FIntPoint(1,0),FIntPoint(0,1),FIntPoint(-1,0)};const int32 Edge[4][3]={{0,1,2},{2,5,8},{8,7,6},{6,3,0}};
			for(int32 E=0;E<4;++E)if(!Selected.Contains(Coord+Adjacent[E]))for(int32 K=0;K<2;++K)
			{
				const int32 A=Base+Edge[E][K],B=Base+Edge[E][K+1],LowA=Bottom+Edge[E][K],LowB=Bottom+Edge[E][K+1];VolcanoTriangle(Indices,A,B,LowA);VolcanoTriangle(Indices,B,LowB,LowA);Stats.LavaSkirtTriangles+=2;
			}
			++Stats.DrawnLavaCells;
		}
		if(Stats.MinimumGroundClearanceCm==MAX_flt)Stats.MinimumGroundClearanceCm=0.f;
		SetDisasterTransientMeshReference(Component, VolcanoMesh(Owner,TEXT("Aftershock_VolcanicLavaVolume"),Vertices,Indices,Component->GetMaterial(0)));
		Component->SetVisibility(Component->GetStaticMesh()!=nullptr&&DrawRegionalVolcano(Owner->GetWorld()));
		return Component->GetStaticMesh()!=nullptr;
	}
	bool BuildRegionalVentMesh(AActor* Owner,const UTerrainGridSubsystem& Terrain,UStaticMeshComponent* Component,FAftershockVolcanoVisualStats& Stats)
	{
		FGridCell Source;Stats.bSourceResident=Terrain.TryGetCell(Terrain.VolcanicVentCoord,Source)&&Source.bVolcanicVent&&Source.bIsPlayable;
		Stats.bSourceInDrawableWindow=GetAftershockDrawableCellBounds(Terrain).Contains(Terrain.VolcanicVentCoord);
		Stats.bSourceContextBacked=false;Stats.VentTriangles=0;ReleaseDisasterTransientMeshReference(Component);
		if(!Stats.bSourceResident||!Component->GetMaterial(0)||(!Terrain.IsUsingRealGeography()&&!Stats.bSourceInDrawableWindow))return false;
		FVector Center=Terrain.GridToWorld(Source.Coords);if(!VolcanoGround(Terrain,Center))return false;Stats.SourceWorld=Center;
		// A small local spatter-rim prop, NOT a replacement Etna cone or a claim
		// that one authored vent represents all summit craters. The surrounding
		// mountain and every base elevation remain the versioned real DEM.
		constexpr int32 Segments=48,Rings=4;const float Radius=FMath::Clamp(Terrain.CellSize*2.5f,2000.f,3500.f);
		const float Radii[Rings]={.32f,.5f,.67f,1.f},Offsets[Rings]={45.f,100.f,350.f,12.f};
		TArray<FVolcanoVertex> Vertices;TArray<int32> Indices;FVolcanoVertex Middle;Middle.Position=Center+FVector(0,0,45);Middle.Color=FLinearColor(1,0,1,1);Vertices.Add(Middle);
		for(int32 Ring=0;Ring<Rings;++Ring)for(int32 Segment=0;Segment<Segments;++Segment)
		{
			const float Angle=2.f*PI*Segment/Segments;const float Irregular=1.f+.035f*FMath::Sin(Angle*5.f)+.018f*FMath::Cos(Angle*9.f);
			FVector Point=Center+FVector(FMath::Cos(Angle)*Radius*Radii[Ring]*Irregular,FMath::Sin(Angle)*Radius*Radii[Ring]*Irregular,0);
			if(!VolcanoGround(Terrain,Point)){ReleaseDisasterTransientMeshReference(Component);return false;}
			FVolcanoVertex V;V.Position=Point+FVector(0,0,Offsets[Ring]);V.UV=FVector2f(FMath::Cos(Angle)*Radii[Ring],FMath::Sin(Angle)*Radii[Ring]);V.Color=FLinearColor(Radii[Ring],0,Ring<2?1.f:0.f,1);Vertices.Add(V);
		}
		for(int32 S=0;S<Segments;++S)
		{
			const int32 Next=(S+1)%Segments;VolcanoTriangle(Indices,0,1+Next,1+S);
			for(int32 Ring=0;Ring<Rings-1;++Ring){const int32 A=1+Ring*Segments+S,B=1+Ring*Segments+Next,C=A+Segments,D=B+Segments;VolcanoTriangle(Indices,A,D,C);VolcanoTriangle(Indices,A,B,D);}
		}
		SetDisasterTransientMeshReference(Component, VolcanoMesh(Owner,TEXT("Aftershock_RegionalVentSpatterRim"),Vertices,Indices,Component->GetMaterial(0)));
		Stats.bSourceContextBacked=Component->GetStaticMesh()!=nullptr;Stats.VentTriangles=Stats.bSourceContextBacked?Indices.Num()/3:0;
		Component->SetVisibility(Stats.bSourceContextBacked&&DrawRegionalVolcano(Owner->GetWorld()));return Stats.bSourceContextBacked;
	}
	bool BuildVolcanoPlumeMesh(AActor* Owner,const FVector& Source,FVector Wind,float Intensity,UStaticMeshComponent* Component,FAftershockVolcanoVisualStats& Stats)
	{
		Wind.Z=0;if(!Wind.Normalize())Wind=FVector(1,0,0);
		constexpr int32 Rings=14,Segments=12;TArray<FVolcanoVertex> Vertices;TArray<int32> Indices;
		const float Height=FMath::Lerp(12000.f,30000.f,FMath::Clamp(Intensity,0.f,1.f));
		for(int32 Ring=0;Ring<Rings;++Ring)
		{
			const float T=Ring/static_cast<float>(Rings-1);const FVector Center=Source+Wind*(Height*.35f*T*T)+FVector(0,0,300.f+Height*T);
			const float Radius=FMath::Lerp(180.f,3300.f,FMath::Pow(T,.75f));
			for(int32 S=0;S<Segments;++S)
			{
				const float Angle=2.f*PI*S/Segments;const float Lobe=1.f+.12f*FMath::Sin(Angle*3.f+Ring*.71f)+.06f*FMath::Cos(Angle*5.f-Ring*.53f);
				FVolcanoVertex V;V.Position=Center+FVector(FMath::Cos(Angle)*Radius*Lobe,FMath::Sin(Angle)*Radius*Lobe,0);V.UV=FVector2f(S/static_cast<float>(Segments),T);V.Color=FLinearColor(1.f-T*.7f,T,0,1);Vertices.Add(V);
			}
		}
		for(int32 R=0;R<Rings-1;++R)for(int32 S=0;S<Segments;++S){const int32 N=(S+1)%Segments,A=R*Segments+S,B=R*Segments+N,C=A+Segments,D=B+Segments;VolcanoTriangle(Indices,A,C,B);VolcanoTriangle(Indices,B,C,D);}
		SetDisasterTransientMeshReference(Component, VolcanoMesh(Owner,TEXT("Aftershock_VolcanicWindBentPlume"),Vertices,Indices,Component->GetMaterial(0)));
		Stats.PlumeTriangles=Component->GetStaticMesh()?Indices.Num()/3:0;return Component->GetStaticMesh()!=nullptr;
	}

	float PresentationAge(const UWorld* World,const FAftershockDisasterVisualSnapshot& Snapshot)
	{
		if(!World)return 0.f;
		if(Snapshot.FloodModelVersion==2&&(Snapshot.Type==EDisasterType::Flood||Snapshot.Type==EDisasterType::Tsunami))
		{
			if(const auto* Sim=World->GetSubsystem<UDisasterSimulationSubsystem>())
			{
				const auto& Episode=Sim->GetPhysicalEpisodeState();
				if(Episode.EventID.IsValid()&&Episode.Type==Snapshot.Type&&Sim->GetLatestVisualSnapshot().Revision==Snapshot.Revision)
					return static_cast<float>(FMath::Max(0.,Episode.PlaybackSeconds)/FMath::Max(.001,Episode.SimulationTimeScale));
			}
		}
		return FMath::Max(0.f,World->GetTimeSeconds()-Snapshot.StartedAtWorldSeconds);
	}

	float WeatherEnvelope(float Age, float Duration)
	{
		const float Rise = FMath::SmoothStep(0.f, 4.f, Age);
		const float Drain = 1.f - FMath::SmoothStep(FMath::Max(4.f, Duration - 8.f), Duration, Age);
		return FMath::Clamp(Rise * Drain, 0.f, 1.f);
	}

	UMaterialInstanceDynamic* MakeWeatherMaterial(UInstancedStaticMeshComponent* Batch)
	{
		if (!Batch->GetMaterial(0) || !Batch->GetMaterial(0)->GetName().StartsWith(TEXT("M_Disaster")))
		{
			UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_DISASTER_MATERIAL_MISSING batch=%s"), *Batch->GetName());
			return nullptr;
		}
		UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Batch->GetMaterial(0), Batch);
		Batch->SetMaterial(0, Material);
		return Material;
	}

	// Four reusable draw batches, not an actor/system/material per flame. Their
	// small original closed hulls avoid the old three crossed flame planes and
	// single stationary ash card. This is display animation, not a combustion solver.
	constexpr int32 FireOuterLayer=0,FireCoreLayer=1,FireSmokeLayer=2,FireEmberLayer=3;
	constexpr int32 FireFlamesPerSource=2,FireSmokePerSource=3,FireEmbersPerSource=2;
	const TCHAR* FireVolumeMaterialPaths[]={
		TEXT("/Game/FX/Aftershock/Fire/M_FireFlameVolume.M_FireFlameVolume"),
		TEXT("/Game/FX/Aftershock/Fire/M_FireCoreVolume.M_FireCoreVolume"),
		TEXT("/Game/FX/Aftershock/Fire/M_FireSmokeVolume.M_FireSmokeVolume"),
		TEXT("/Game/FX/Aftershock/Fire/M_FireEmber.M_FireEmber")};

	UStaticMesh* BuildFireDisplayHull(AActor* Owner,UMaterialInterface* Material,bool bFlame)
	{
		constexpr int32 Rings=12,Segments=16;
		TArray<FVolcanoVertex> Vertices;TArray<int32> Indices;
		Vertices.Reserve(Rings*(Segments+1));Indices.Reserve((Rings-1)*Segments*6);
		const auto FlameRadius=[](float T){T=FMath::Clamp(T,0.f,1.f);return 50.f*FMath::Pow(1.f-T,1.35f)*FMath::SmoothStep(0.f,.065f,T);};
		for(int32 Ring=0;Ring<Rings;++Ring)
		{
			const float T=Ring/static_cast<float>(Rings-1);
			const float Radius=bFlame?FlameRadius(T):50.f*FMath::Max(0.f,FMath::Sin(PI*T));
			const float RadialSlope=bFlame?(FlameRadius(T+.002f)-FlameRadius(T-.002f))/.4f:0.f;
			for(int32 S=0;S<=Segments;++S)
			{
				const float A=2.f*PI*S/Segments;
				const float Lobe=1.f+.16f*FMath::Sin(A*3.f+T*8.f)+.07f*FMath::Cos(A*5.f-T*6.f);
				FVolcanoVertex Vertex;Vertex.Position=FVector(FMath::Cos(A)*Radius*Lobe+(bFlame?16.f*T+14.f*T*T:0.f),FMath::Sin(A)*Radius*Lobe+(bFlame?5.f*FMath::Sin(T*8.f)*T:0.f),bFlame?100.f*T:100.f*T-50.f);
				Vertex.ShadingNormal=bFlame?FVector(FMath::Cos(A),FMath::Sin(A),-RadialSlope).GetSafeNormal():FVector(FMath::Cos(A)*FMath::Sin(PI*T),FMath::Sin(A)*FMath::Sin(PI*T),-FMath::Cos(PI*T)).GetSafeNormal();
				Vertex.UV=FVector2f(S/static_cast<float>(Segments),T);Vertices.Add(Vertex);
			}
		}
		for(int32 R=0;R<Rings-1;++R)for(int32 S=0;S<Segments;++S)
		{
			const int32 A=R*(Segments+1)+S,B=A+1,C=A+Segments+1,D=C+1;
			// Poles use one nondegenerate triangle; no zero-area triangles/tangents.
			if(R>0)VolcanoTriangle(Indices,A,C,B);
			if(R<Rings-2)VolcanoTriangle(Indices,B,C,D);
		}
		return VolcanoMesh(Owner,bFlame?TEXT("Aftershock_FireTongueHull"):TEXT("Aftershock_FireSmokeBillowHull"),Vertices,Indices,Material);
	}

	void PrepareFireDisplay(AActor* Owner,TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Batches,
		TArray<TObjectPtr<UMaterialInstanceDynamic>>& Materials,FAftershockFireVisualState& State)
	{
		if(Batches.Num()==4)return;
		Batches.SetNum(4);Materials.SetNum(4);State.AuthoredMaterials=0;
		for(int32 Layer=0;Layer<4;++Layer)
		{
			auto* Batch=NewObject<UInstancedStaticMeshComponent>(Owner);
			Batch->PrimaryComponentTick.bCanEverTick=false;Batch->SetComponentTickEnabled(false);
			Batch->SetupAttachment(Owner->GetRootComponent());Batch->SetMobility(EComponentMobility::Movable);
			Batch->SetCollisionEnabled(ECollisionEnabled::NoCollision);Batch->SetCanEverAffectNavigation(false);Batch->SetCastShadow(false);
			Batch->SetCullDistances(0,Layer==FireSmokeLayer?300000:Layer==FireEmberLayer?24000:140000);
			Batch->SetNumCustomDataFloats(3);Batch->SetTranslucentSortPriority(Layer==FireSmokeLayer?3:4);
			UMaterialInterface* Base=LoadObject<UMaterialInterface>(nullptr,FireVolumeMaterialPaths[Layer]);
			if(Base)++State.AuthoredMaterials;
			else
			{
				// Existing project materials keep canonical fires visible when an
				// editor is unavailable. This fallback is reported, never called an
				// accepted authored-volume material or a completed visual test.
				const TCHAR* Fallback=Layer==FireSmokeLayer?VolcanoPlumePath:TEXT("/Game/FX/Aftershock/Materials/M_DisasterForestFire.M_DisasterForestFire");
				Base=LoadObject<UMaterialInterface>(nullptr,Fallback);
				UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_FIRE_VOLUME_MATERIAL_PENDING path=%s fallback=%s visualAcceptance=0"),FireVolumeMaterialPaths[Layer],Fallback);
			}
			if(Base)
			{
				Materials[Layer]=UMaterialInstanceDynamic::Create(Base,Owner);Batch->SetMaterial(0,Materials[Layer]);
				Batch->SetStaticMesh(BuildFireDisplayHull(Owner,Base,Layer!=FireSmokeLayer));
			}
			Owner->AddInstanceComponent(Batch);Batch->RegisterComponent();Batches[Layer]=Batch;
		}
	}

	void ResetFireDisplay(TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Batches,
		TArray<TObjectPtr<UMaterialInstanceDynamic>>& Materials,FAftershockFireVisualState& State)
	{
		for(UInstancedStaticMeshComponent* Batch:Batches){Batch->ClearInstances();Batch->SetVisibility(false);}
		for(UMaterialInstanceDynamic* Material:Materials)if(Material)Material->SetScalarParameterValue(TEXT("EventEnvelope"),0.f);
		State.Sources.Reset();for(auto& Transforms:State.Transforms)Transforms.Reset();
		State.CanonicalBurningCells=0;State.BuildingSources=0;
	}

	void BuildFireDisplay(AActor* Owner,const UTerrainGridSubsystem& Terrain,const TArray<FIntPoint>& Cells,
		const TMap<FIntPoint,float>* EventSeverity,FVector Wind,TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Batches,
		TArray<TObjectPtr<UMaterialInstanceDynamic>>& Materials,FAftershockFireVisualState& State)
	{
		ResetFireDisplay(Batches,Materials,State);
		const auto Budget=GetAftershockPresentationBudget(Owner->GetWorld());State.BuiltTier=static_cast<int32>(Budget.Tier);
		const int32 Quality=FMath::Clamp(Scalability::GetQualityLevels().EffectsQuality,0,3);
		State.BuiltEffectsQuality=Quality;
		const int32 TierLimit=Budget.Tier==EAftershockPresentationTier::Globe?0:Budget.Tier==EAftershockPresentationTier::Regional?32:Budget.Tier==EAftershockPresentationTier::City?128:192;
		const int32 Limit=FMath::Min(TierLimit,48+Quality*48);if(!Limit)return;
		Wind.Z=0.f;if(Wind.ContainsNaN())Wind=FVector::ForwardVector;
		State.WindDirection=Wind.GetSafeNormal(SMALL_NUMBER,FVector::ForwardVector);
		FVector Eye=Terrain.GridToWorld(FIntPoint(Terrain.GridWidth/2,Terrain.GridHeight/2));FRotator EyeRotation;
		if(auto* PC=Owner->GetWorld()?Owner->GetWorld()->GetFirstPlayerController():nullptr)PC->GetPlayerViewPoint(Eye,EyeRotation);
		struct FCandidate{FIntPoint Coord;float Strength;double Priority;};TArray<FCandidate> Candidates;
		for(const FIntPoint Coord:Cells)
		{
			FGridCell Cell;if(!Terrain.TryGetCell(Coord,Cell)||!Cell.bIsPlayable||Cell.bIsOcean||Cell.bIsRiverbed||!Cell.bBurning)continue;
			const bool HasBuilding=Cell.OccupyingBuildingID!=INDEX_NONE;
			if(Cell.ForestFuel01<=.005f&&!HasBuilding)continue;
			const float* CanonicalSeverity=EventSeverity?EventSeverity->Find(Coord):nullptr;
			const float Strength=CanonicalSeverity?FMath::Clamp(*CanonicalSeverity/100.f,0.f,1.f):FMath::Clamp(.2f+Cell.ForestFuel01*.8f,0.f,1.f);
			if(!FMath::IsFinite(Strength)||Strength<=.015f)continue;
			++State.CanonicalBurningCells;
			const double Distance=FVector::DistSquared2D(Eye,Terrain.GridToWorld(Coord));
			Candidates.Add({Coord,Strength,Distance*(HasBuilding?.30:1.)});
		}
		Candidates.Sort([](const FCandidate& A,const FCandidate& B){return A.Priority==B.Priority?(A.Coord.Y==B.Coord.Y?A.Coord.X<B.Coord.X:A.Coord.Y<B.Coord.Y):A.Priority<B.Priority;});
		if(Candidates.Num()>Limit)Candidates.SetNum(Limit,EAllowShrinking::No);
		if(Candidates.IsEmpty())return;
		PrepareFireDisplay(Owner,Batches,Materials,State);
		const auto* Buildings=Owner->GetWorld()?Owner->GetWorld()->GetSubsystem<UBuildingManagerSubsystem>():nullptr;
		TSet<int32> SeenBuildings;
		for(const auto& Candidate:Candidates)
		{
			const auto& Cell=Terrain.GetCell(Candidate.Coord);FAftershockFireVisualSource Source;
			Source.Cell=Candidate.Coord;Source.Strength=Candidate.Strength;Source.Anchor=Terrain.GridToWorld(Candidate.Coord)+FVector(0,0,8.f);
			const uint32 Seed=HashCombine(GetTypeHash(Candidate.Coord),GetTypeHash(Terrain.TerrainSeed));Source.Phase=(Seed%65521)/65521.f;
			Source.RadiusCm=FMath::Clamp(Terrain.CellSize*.12f,45.f,140.f)*(.65f+.55f*Source.Strength);
			// A fire tongue must stay taller than its footprint even for the low
			// positive severities around the canonical spread boundary. These are
			// authored display dimensions, never a measured flame-length formula.
			Source.HeightCm=FMath::Max(320.f+Source.Strength*1100.f,Source.RadiusCm*5.f);
			FBuildingInstance Building;
			if(Buildings&&Cell.OccupyingBuildingID!=INDEX_NONE&&Buildings->GetBuildingByID(Cell.OccupyingBuildingID,Building)&&Building.CurrentDamage>0.f&&Building.DamageState!=EDamageState::Memorial)
			{
				if(SeenBuildings.Contains(Building.InstanceID))continue;
				SeenBuildings.Add(Building.InstanceID);
				FAftershockArchitecturalInput Architecture;
				if(Buildings->GetCanonicalArchitecturalInput(Building.InstanceID,Architecture))
				{
					// Anchor to the SAME visible saved building envelope. Rooftop
					// flames never turn an intact building into a fabricated collapse.
					Source.Anchor=Architecture.Frame.TransformPosition(FVector(0,0,Architecture.EnvelopeCm.Z+8.f));
					Source.RadiusCm=FMath::Clamp(FMath::Min(Architecture.EnvelopeCm.X,Architecture.EnvelopeCm.Y)*.18f,50.f,260.f);
					Source.HeightCm=FMath::Max(FMath::Clamp(380.f+Building.CurrentDamage*7.f,380.f,1200.f),Source.RadiusCm*5.f);
				}
				Source.bBuilding=true;++State.BuildingSources;
			}
			State.Sources.Add(Source);
		}
		const int32 Multipliers[]={FireFlamesPerSource,1,FireSmokePerSource,FireEmbersPerSource};
		for(int32 Layer=0;Layer<4;++Layer)
		{
			auto* Batch=Batches[Layer].Get();State.Transforms[Layer].Init(FTransform(FQuat::Identity,FVector::ZeroVector,FVector::ZeroVector),State.Sources.Num()*Multipliers[Layer]);
			Batch->AddInstances(State.Transforms[Layer],false,true,false);
			for(int32 Index=0;Index<Batch->GetInstanceCount();++Index)
			{
				const auto& Source=State.Sources[Index/Multipliers[Layer]];
				Batch->SetCustomDataValue(Index,0,Source.Strength,false);Batch->SetCustomDataValue(Index,1,Source.Phase,false);Batch->SetCustomDataValue(Index,2,1.f,false);
			}
			Batch->SetVisibility(Batch->GetStaticMesh()!=nullptr&&Materials[Layer]!=nullptr);
		}
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_FIRE_VOLUME_BUILT canonicalBurning=%d displayedSources=%d buildingSources=%d materialReady=%d/4 geometry=OriginalClosedHulls flameActors=0 sharedBatches=4 simulationMutation=0"),State.CanonicalBurningCells,State.Sources.Num(),State.BuildingSources,State.AuthoredMaterials);
	}

	void AnimateFireDisplay(AActor* Owner,const UTerrainGridSubsystem& Terrain,float TimeSeconds,float Envelope,
		TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Batches,TArray<TObjectPtr<UMaterialInstanceDynamic>>& Materials,
		FAftershockFireVisualState& State)
	{
		if(State.Sources.IsEmpty()||Batches.Num()!=4)return;
		const auto Budget=GetAftershockPresentationBudget(Owner->GetWorld());const bool Visible=Budget.Tier!=EAftershockPresentationTier::Globe;
		const bool Detail=Budget.Tier==EAftershockPresentationTier::Street;
		const FVector Wind=State.WindDirection;const FVector Across(-Wind.Y,Wind.X,0.f);
		const float Yaw=FMath::RadiansToDegrees(FMath::Atan2(Wind.Y,Wind.X));
		for(int32 Index=0;Index<State.Sources.Num();++Index)
		{
			const auto& Source=State.Sources[Index];FGridCell Cell;
			const float Live=Visible&&Terrain.TryGetCell(Source.Cell,Cell)&&Cell.bBurning?FMath::Clamp(Envelope,0.f,1.f):0.f;
			const float Phase=Source.Phase*2.f*PI;
			const FVector LeanDirection=(FVector::UpVector+Wind*(.23f+Source.Strength*.15f)+Across*(.045f*FMath::Sin(TimeSeconds*2.1f+Phase))).GetSafeNormal();
			const FQuat LeanRotation=FRotationMatrix::MakeFromZ(LeanDirection).ToQuat();
			for(int32 Lobe=0;Lobe<FireFlamesPerSource;++Lobe)
			{
				const float Pulse=.88f+.12f*FMath::Sin(TimeSeconds*7.f+Phase+Lobe*2.1f);
				const FVector Position=Source.Anchor+Across*(Source.RadiusCm*(Lobe==0?-.36f:.36f))+Wind*(Source.RadiusCm*.12f*FMath::Sin(TimeSeconds*3.f+Phase));
				const FQuat Twist(FVector::UpVector,Phase+Lobe*1.9f);
				State.Transforms[FireOuterLayer][Index*FireFlamesPerSource+Lobe]=FTransform(LeanRotation*Twist,Position,FVector(Source.RadiusCm/50.f,Source.RadiusCm/62.f,Source.HeightCm*Pulse*(Lobe==0?1.f:.76f)/100.f)*Live);
			}
			State.Transforms[FireCoreLayer][Index]=FTransform(LeanRotation,Source.Anchor+Wind*8.f,FVector(Source.RadiusCm*.43f/50.f,Source.RadiusCm*.38f/50.f,Source.HeightCm*.69f/100.f)*Live);
			for(int32 Puff=0;Puff<FireSmokePerSource;++Puff)
			{
				const float T=FMath::Fmod(TimeSeconds/12.f+Source.Phase+Puff/static_cast<float>(FireSmokePerSource),1.f);
				const float PlumeHeight=Source.HeightCm*2.7f+900.f;
				const float BaseRadius=FMath::Max(Source.RadiusCm*1.8f,Source.HeightCm*.20f);
				const float Rise=Source.HeightCm*.35f+T*PlumeHeight;
				const float Radius=BaseRadius*(1.1f+T*2.7f);
				// Dissipate density, not scale: shrinking smoke spheres made gaps
				// and popping beads instead of a continuous rising billowing column.
				const float Fade=FMath::SmoothStep(0.f,.075f,T)*(1.f-FMath::SmoothStep(.80f,1.f,T));
				const FVector Position=Source.Anchor+Wind*(PlumeHeight*(.12f*T+.30f*T*T))+Across*(Radius*.12f*FMath::Sin(T*8.f+Phase))+FVector(0,0,Rise);
				State.Transforms[FireSmokeLayer][Index*FireSmokePerSource+Puff]=FTransform(FRotator(4.f*FMath::Sin(Phase+T*5.f),Yaw+Puff*47.f+T*24.f,0.f),Position,FVector(Radius/50.f,Radius*.88f/50.f,Radius*(2.15f-.35f*T)/50.f)*Live);
				Batches[FireSmokeLayer]->SetCustomDataValue(Index*FireSmokePerSource+Puff,2,Fade,false);
			}
			for(int32 Spark=0;Spark<FireEmbersPerSource;++Spark)
			{
				const float T=FMath::Fmod(TimeSeconds*.57f+Source.Phase+Spark*.5f,1.f),Seconds=T*1.75f;
				const FVector Position=Source.Anchor+Across*((Spark?1.f:-1.f)*Source.RadiusCm*.45f)+Wind*(Seconds*170.f)+FVector(0,0,Source.HeightCm*.65f+Seconds*530.f-.5f*280.f*Seconds*Seconds);
				const float Radius=(2.f+Source.Strength*3.f)*(1.f-T)*Live*(Detail?1.f:0.f);
				State.Transforms[FireEmberLayer][Index*FireEmbersPerSource+Spark]=FTransform(FRotator(0.f,Yaw,0.f),Position,FVector(Radius/50.f,Radius/50.f,Radius*3.f/100.f));
			}
		}
		for(int32 Layer=0;Layer<4;++Layer)
		{
			Batches[Layer]->SetVisibility(Visible&&(Layer!=FireEmberLayer||Detail)&&Materials[Layer]!=nullptr);
			if(Materials[Layer]){Materials[Layer]->SetScalarParameterValue(TEXT("EventEnvelope"),Envelope);Materials[Layer]->SetScalarParameterValue(TEXT("FireVisualClock"),TimeSeconds);}
			Batches[Layer]->BatchUpdateInstancesTransforms(0,State.Transforms[Layer],true,true,true);
		}
	}
}

AAftershockEcologyStateActor::AAftershockEcologyStateActor()
{
	PrimaryActorTick.bCanEverTick = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("PersistentEcologyRoot")));
	const FName Names[] = { TEXT("PersistentForestFire"), TEXT("PersistentLava"), TEXT("PersistentAsh"), TEXT("PersistentDrought") };
	ConstructorHelpers::FObjectFinder<UStaticMesh> Plane(TEXT("/Engine/BasicShapes/Plane.Plane"));
	for (FName Name : Names)
	{
		UInstancedStaticMeshComponent* Batch = CreateDefaultSubobject<UInstancedStaticMeshComponent>(Name);
		Batch->SetupAttachment(GetRootComponent());
		Batch->SetMobility(EComponentMobility::Movable);
		Batch->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Batch->SetCanEverAffectNavigation(false); Batch->SetCastShadow(false);
		Batch->SetCullDistances(0, 70000); Batch->SetNumCustomDataFloats(1);
		if (Plane.Succeeded()) Batch->SetStaticMesh(Plane.Object);
		Batches.Add(Batch);
	}
	Materials.SetNum(4);
	RegionalVolcanoVent=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("RegionalCanonicalVolcanoVent"));
	PersistentLavaVolume=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PersistentCanonicalLavaVolume"));
	for(auto* Surface:{RegionalVolcanoVent.Get(),PersistentLavaVolume.Get()})
	{Surface->SetupAttachment(GetRootComponent());Surface->SetMobility(EComponentMobility::Movable);Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);Surface->SetCanEverAffectNavigation(false);Surface->SetCastShadow(false);Surface->SetCullDistance(VolcanoCullDistanceCm);}
}

void AAftershockEcologyStateActor::RefreshRegionalVolcanoVent(const UTerrainGridSubsystem& Terrain)
{
	uint32 Signature=HashCombine(GetTypeHash(Terrain.TerrainSeed),GetTypeHash(Terrain.VolcanicVentCoord));
	Signature=HashCombine(Signature,GetTypeHash(Terrain.GeographyIndex));Signature=HashCombine(Signature,GetTypeHash(Terrain.GeographyFocusIndex));Signature=HashCombine(Signature,GetTypeHash(Terrain.ContinuationGeneratorVersion));Signature=HashCombine(Signature,GetTypeHash(Terrain.CellSize));Signature=HashCombine(Signature,GetTypeHash(Terrain.GridOrigin));
	FGridCell VentCell;const bool VentResident=Terrain.TryGetCell(Terrain.VolcanicVentCoord,VentCell);
	Signature=HashCombine(Signature,GetTypeHash(VentResident));if(VentResident){Signature=HashCombine(Signature,GetTypeHash(VentCell.bVolcanicVent));Signature=HashCombine(Signature,GetTypeHash(VentCell.Elevation));}
	if(!Terrain.IsUsingRealGeography()){const auto Bounds=GetAftershockDrawableCellBounds(Terrain);Signature=HashCombine(Signature,GetTypeHash(Bounds.Min));Signature=HashCombine(Signature,GetTypeHash(Bounds.Max));}
	if(!bHasRegionalVentSignature||Signature!=RegionalVentSignature)
	{
		RegionalVentSignature=Signature;bHasRegionalVentSignature=true;RegionalVolcanoStats={};
		FGridCell Source;const bool HasVent=Terrain.TryGetCell(Terrain.VolcanicVentCoord,Source)&&Source.bVolcanicVent&&Source.bIsPlayable;
		if(HasVent&&!VentMaterial)VentMaterial=VolcanoMaterial(RegionalVolcanoVent,VolcanoVentPath);
		RegionalVolcanoStats.bMaterialsReady=!HasVent||VentMaterial!=nullptr;
		if(HasVent&&VentMaterial)BuildRegionalVentMesh(this,Terrain,RegionalVolcanoVent,RegionalVolcanoStats);else RegionalVolcanoVent->SetStaticMesh(nullptr);
		if(VentMaterial)VentMaterial->SetScalarParameterValue(TEXT("EventEnvelope"),.02f);
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_REGIONAL_VOLCANO_VENT sourceResident=%d sourceBacked=%d ventTriangles=%d location=%s localSpatterRimOnly=1 mainConeFromVersionedDEM=1 sourceVersion=%d"),RegionalVolcanoStats.bSourceResident,RegionalVolcanoStats.bSourceContextBacked,RegionalVolcanoStats.VentTriangles,*RegionalVolcanoStats.SourceWorld.ToCompactString(),Terrain.ContinuationGeneratorVersion);
	}
	RegionalVolcanoStats.bSourceInDrawableWindow=GetAftershockDrawableCellBounds(Terrain).Contains(Terrain.VolcanicVentCoord);
	RegionalVolcanoVent->SetVisibility(RegionalVolcanoVent->GetStaticMesh()!=nullptr&&DrawRegionalVolcano(GetWorld()));
	PersistentLavaVolume->SetVisibility(PersistentLavaVolume->GetStaticMesh()!=nullptr&&DrawRegionalVolcano(GetWorld()));
}

void AAftershockEcologyStateActor::RefreshFromTerrain(const UTerrainGridSubsystem& Terrain, EDisasterType AnimatedType, bool bAnimationActive)
{
	RefreshRegionalVolcanoVent(Terrain);
	// The original event owns its rise/fade animation. Keep other ongoing terrain
	// consequences visible, and take over this type after its animation completes.
	uint32 Signature = HashCombine(GetTypeHash(Terrain.TerrainSeed), GetTypeHash(Terrain.ActiveDroughtSeverity));
	Signature = HashCombine(Signature, GetTypeHash(Terrain.CellSize));
	Signature = HashCombine(Signature, GetTypeHash(Terrain.GridOrigin));
	Signature = HashCombine(Signature, GetTypeHash(bAnimationActive ? static_cast<uint8>(AnimatedType) + 1 : 0));
	const FIntRect Visible=GetAftershockDrawableCellBounds(Terrain);
	Signature=HashCombine(Signature,GetTypeHash(Visible.Min));Signature=HashCombine(Signature,GetTypeHash(Visible.Max));
	Signature=HashCombine(Signature,GetTypeHash(static_cast<uint8>(GetAftershockPresentationBudget(GetWorld()).Tier)));
	Signature=HashCombine(Signature,GetTypeHash(FMath::Clamp(Scalability::GetQualityLevels().EffectsQuality,0,3)));
	const FAftershockDisasterVisualSnapshot* AnimatedFire=nullptr;
	if(bAnimationActive&&AnimatedType==EDisasterType::Fire)
		if(const auto* Sim=GetWorld()?GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>():nullptr)
			if(Sim->GetLatestVisualSnapshot().Type==EDisasterType::Fire&&Sim->GetLatestVisualSnapshot().Revision>0)
			{AnimatedFire=&Sim->GetLatestVisualSnapshot();Signature=HashCombine(Signature,GetTypeHash(AnimatedFire->Revision));}
	const uint64 Revision=Terrain.GetTerrainRevision();
	if(bHasStateSignature&&LastTerrainRevision==Revision&&LastFrameSignature==Signature)return;
	LastTerrainRevision=Revision;LastFrameSignature=Signature;
	for(int32 Y=Visible.Min.Y;Y<Visible.Max.Y;++Y)for(int32 X=Visible.Min.X;X<Visible.Max.X;++X)
	{
		const int32 Index=Terrain.CoordToIndex(FIntPoint(X,Y));if(!Terrain.Cells.IsValidIndex(Index))continue;
		const FGridCell& Cell=Terrain.Cells[Index];++CanonicalCellsVisited;
		Signature = HashCombine(Signature, GetTypeHash(Cell.Coords));
		Signature = HashCombine(Signature, GetTypeHash(Cell.Elevation));
		Signature = HashCombine(Signature, GetTypeHash(Cell.bIsPlayable));
		Signature = HashCombine(Signature, GetTypeHash(Cell.bIsOcean));
		Signature = HashCombine(Signature, GetTypeHash(Cell.bIsRiverbed));
		Signature = HashCombine(Signature, GetTypeHash(Cell.bBurning));
		Signature = HashCombine(Signature, GetTypeHash(Cell.ForestFuel01));
		Signature = HashCombine(Signature, GetTypeHash(Cell.LavaCoverage01));
		Signature = HashCombine(Signature, GetTypeHash(Cell.AshLoad01));
		Signature = HashCombine(Signature, GetTypeHash(Cell.DroughtStress01));
	}
	TMap<FIntPoint,float> PersistentLava;
	if(!(bAnimationActive&&AnimatedType==EDisasterType::Volcano))for(const auto& Cell:Terrain.Cells)
		if(Cell.bIsPlayable&&!Cell.bIsOcean&&Cell.LavaCoverage01>.01f)
		{PersistentLava.Add(Cell.Coords,Cell.LavaCoverage01*100.f);Signature=HashCombine(Signature,GetTypeHash(Cell.Coords));Signature=HashCombine(Signature,GetTypeHash(Cell.LavaCoverage01));}
	if (bHasStateSignature && LastStateSignature == Signature) return;
	bHasStateSignature = true; LastStateSignature = Signature;++RebuildCount;
	const TCHAR* Paths[] = {
		TEXT("/Game/FX/Aftershock/Materials/M_DisasterForestFire.M_DisasterForestFire"),
		TEXT("/Game/FX/Aftershock/Materials/M_DisasterLava.M_DisasterLava"),
		TEXT("/Game/FX/Aftershock/Materials/M_DisasterVolcanicAsh.M_DisasterVolcanicAsh"),
		TEXT("/Game/FX/Aftershock/Materials/M_DisasterDrought.M_DisasterDrought") };
	TArray<const FGridCell*> Active[4];
	for(int32 Y=Visible.Min.Y;Y<Visible.Max.Y;++Y)for(int32 X=Visible.Min.X;X<Visible.Max.X;++X)
	{
		const int32 Index=Terrain.CoordToIndex(FIntPoint(X,Y));if(!Terrain.Cells.IsValidIndex(Index))continue;
		const FGridCell& Cell=Terrain.Cells[Index];++CanonicalCellsVisited;
		if (!Visible.Contains(Cell.Coords)||!Cell.bIsPlayable || Cell.bIsOcean || Cell.bIsRiverbed) continue;
		const bool AnimatedFireCell=bAnimationActive&&AnimatedType==EDisasterType::Fire
			&&(!AnimatedFire||AnimatedFire->LocalSeverity.Contains(Cell.Coords));
		if (Cell.bBurning && (Cell.ForestFuel01 > .005f || Cell.OccupyingBuildingID != INDEX_NONE)
			&&!AnimatedFireCell) Active[0].Add(&Cell);
		if (!(bAnimationActive && AnimatedType == EDisasterType::Volcano))
		{
			if (Cell.LavaCoverage01 > .01f) Active[1].Add(&Cell);
			if (Cell.AshLoad01 > .02f) Active[2].Add(&Cell);
		}
		if (Terrain.ActiveDroughtSeverity > 0.f && Cell.DroughtStress01 > .05f
			&& !(bAnimationActive && AnimatedType == EDisasterType::Drought)) Active[3].Add(&Cell);
	}
	for (int32 Layer = 0; Layer < 4; ++Layer)
	{
		// Coordinate iteration is already stable global Y/X, without sorting an
		// all-resident pointer list after every unrelated building/road edit.
		UInstancedStaticMeshComponent* Batch = Batches[Layer];if(Batch->GetInstanceCount()>0)Batch->ClearInstances();
		if(Layer==0)continue; // Shared continuous fire-volume batches are built below.
		if(Layer==1)
		{
			uint32 LavaSignature=HashCombine(RegionalVentSignature,GetTypeHash(PersistentLava.Num()));
			for(const auto& Pair:PersistentLava){LavaSignature=HashCombine(LavaSignature,GetTypeHash(Pair.Key));LavaSignature=HashCombine(LavaSignature,GetTypeHash(Pair.Value));}
			if(bHasPersistentLavaStateSignature&&PersistentLavaStateSignature==LavaSignature)continue;
			PersistentLavaStateSignature=LavaSignature;bHasPersistentLavaStateSignature=true;
			if(!PersistentLava.IsEmpty())
			{
				if(!PersistentLavaMaterial)PersistentLavaMaterial=VolcanoMaterial(PersistentLavaVolume,VolcanoLavaPath);
				if(PersistentLavaMaterial){PersistentLavaMaterial->SetScalarParameterValue(TEXT("FlowProgress"),1.f);PersistentLavaMaterial->SetScalarParameterValue(TEXT("EventEnvelope"),.02f);PersistentLavaMaterial->SetScalarParameterValue(TEXT("Cooling"),1.f);BuildVolcanoLavaMesh(this,Terrain,PersistentLava,Terrain.VolcanicVentCoord,PersistentLavaVolume,RegionalVolcanoStats);}
			}
			else {PersistentLavaVolume->SetStaticMesh(nullptr);RegionalVolcanoStats.CanonicalLavaCells=0;RegionalVolcanoStats.DrawnLavaCells=0;RegionalVolcanoStats.LavaTopTriangles=0;RegionalVolcanoStats.LavaSkirtTriangles=0;}
			continue;
		}
		if (Active[Layer].IsEmpty()) continue;
		if (!Materials[Layer])
		{
			if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, Paths[Layer]))
			{
				Batch->SetMaterial(0, Base); Materials[Layer] = MakeWeatherMaterial(Batch);
			}
		}
		if (!Materials[Layer]) continue;
		Materials[Layer]->SetScalarParameterValue(TEXT("EventEnvelope"), 1.f);
		const int32 Stride = FMath::Max(1, FMath::DivideAndRoundUp(Active[Layer].Num(), Layer == 3 ? 800 : 240));
		for (int32 Index = 0; Index < Active[Layer].Num(); Index += Stride)
		{
			const FGridCell& Cell = *Active[Layer][Index]; const FVector Base = Terrain.GridToWorld(Cell.Coords);
			const float Strength = FMath::Clamp(Layer == 2 ? Cell.AshLoad01 : Cell.DroughtStress01, 0.f, 1.f);
			const bool bAsh = Layer == 2;
			const FTransform Transform(bAsh ? FRotator(0,Index%180,70.f).Quaternion() : FQuat::Identity,
				Base + FVector(0,0,bAsh ? 450.f+Strength*1000.f : 3.f),
				bAsh ? FVector(8,10,1) : FVector(Terrain.CellSize*.0098f,Terrain.CellSize*.0098f,1));
			const int32 Instance = Batch->AddInstance(Transform,true); Batch->SetCustomDataValue(Instance,0,Strength,false);
		}
		Batch->MarkRenderStateDirty();
	}
	TArray<FIntPoint> BurningCells;BurningCells.Reserve(Active[0].Num());
	for(const FGridCell* Cell:Active[0])BurningCells.Add(Cell->Coords);
	FVector Wind=FVector::ForwardVector;
	const TMap<FIntPoint,float>* FireField=nullptr;
	if(const auto* Sim=GetWorld()?GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>():nullptr)
	{
		Wind=Sim->ScenarioWindDirection;
		const auto& Latest=Sim->GetLatestVisualSnapshot();
		if(Latest.Type==EDisasterType::Fire&&Latest.Revision>0)
		{
			// Preserve intensity and wind across the real event -> persistent
			// handoff. The simulation retains this canonical field; no renderer
			// copy or second fire-intensity authority is introduced.
			FireField=&Latest.LocalSeverity;Wind=Latest.DominantWindDirection;
		}
	}
	BuildFireDisplay(this,Terrain,BurningCells,FireField,Wind,FireBatches,FireMaterials,FireVisuals);
	if(GetWorld())
	{
		if(FireVisuals.Sources.IsEmpty())GetWorld()->GetTimerManager().ClearTimer(FireAnimationTimer);
		else if(!GetWorld()->GetTimerManager().IsTimerActive(FireAnimationTimer))
		{
			FTimerManagerTimerParameters Parameters;Parameters.bLoop=true;Parameters.bMaxOncePerFrame=true;
			GetWorld()->GetTimerManager().SetTimer(FireAnimationTimer,this,&AAftershockEcologyStateActor::AnimatePersistentFire,WeatherUpdateSeconds,Parameters);
		}
	}
	AnimatePersistentFire();
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PERSISTENT_ECOLOGY %s source=CanonicalTerrainOnly gameplayMutation=0 rebuildOnStateChange=1"),*DescribeCurrentVisuals());
}

void AAftershockEcologyStateActor::AnimatePersistentFire()
{
	if(const auto* World=GetWorld())if(const auto* Terrain=World->GetSubsystem<UTerrainGridSubsystem>())
	{
		if(FireVisuals.Sources.IsEmpty())return;
		const double Began=FPlatformTime::Seconds();
		AnimateFireDisplay(this,*Terrain,World->GetTimeSeconds(),1.f,FireBatches,FireMaterials,FireVisuals);
		const double Milliseconds=(FPlatformTime::Seconds()-Began)*1000.;
		FireAnimationCpuStats.TotalMilliseconds+=Milliseconds;FireAnimationCpuStats.PeakMilliseconds=FMath::Max(FireAnimationCpuStats.PeakMilliseconds,Milliseconds);++FireAnimationCpuStats.Updates;
	}
}

void AAftershockEcologyStateActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if(GetWorld())GetWorld()->GetTimerManager().ClearTimer(FireAnimationTimer);
	ResetFireDisplay(FireBatches,FireMaterials,FireVisuals);
	Super::EndPlay(EndPlayReason);
}

FString AAftershockEcologyStateActor::DescribeCurrentVisuals() const
{
	return FString::Printf(TEXT("fireInstances=%d fireSources=%d fireBuildingSources=%d fireVolumeMaterials=%d/4 lavaInstances=%d ashInstances=%d droughtInstances=%d lavaSurfaceCells=%d lavaTopTriangles=%d regionalVentTriangles=%d regionalVentGroundBacked=%d"),
		FireBatches.Num()==4?FireBatches[0]->GetInstanceCount():0,FireVisuals.Sources.Num(),FireVisuals.BuildingSources,FireVisuals.AuthoredMaterials,Batches[1]->GetInstanceCount(),Batches[2]->GetInstanceCount(),Batches[3]->GetInstanceCount(),RegionalVolcanoStats.DrawnLavaCells,RegionalVolcanoStats.LavaTopTriangles,RegionalVolcanoStats.VentTriangles,RegionalVolcanoStats.bSourceContextBacked);
}

AAftershockDisasterPresentationActor::AAftershockDisasterPresentationActor()
{
	PrimaryActorTick.bCanEverTick = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("DisasterPresentationRoot")));
	FloodSurfaceISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("CanonicalFloodSurfaceISM"));
	WindStreakISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("CanonicalWindStreakISM"));
	RainStreakISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("CanonicalRainStreakISM"));
	RiverStageISM=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("CanonicalRiverFillStage"));
	LavaSurfaceISM=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("CanonicalLavaSurface"));
	AshCloudISM=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("CanonicalVolcanicAsh"));
	DroughtSurfaceISM=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("CanonicalDroughtGround"));
	QuakeEpicenterRingISM=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("QuakeEpicenterRingISM"));
	QuakeFissureISM=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("QuakeFissureISM"));
	DisasterEpicenterBeaconISM=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("DisasterEpicenterBeaconISM"));
	TsunamiSurface = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CanonicalTsunamiWaveSurface"));
	VolcanoLavaVolume=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CanonicalVolcanoLavaVolume"));
	VolcanoPlume=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CanonicalVolcanoWindBentPlume"));
	VolcanoSpatterISM=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("CanonicalVolcanoSpatter"));
	for(auto* Surface:{VolcanoLavaVolume.Get(),VolcanoPlume.Get(),static_cast<UStaticMeshComponent*>(VolcanoSpatterISM.Get())})
	{Surface->SetupAttachment(GetRootComponent());Surface->SetMobility(EComponentMobility::Movable);Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);Surface->SetCanEverAffectNavigation(false);Surface->SetCastShadow(false);Surface->SetCullDistance(VolcanoCullDistanceCm);}
	VolcanoSpatterISM->SetNumCustomDataFloats(1);VolcanoPlume->SetTranslucentSortPriority(4);
	TsunamiSurface->SetupAttachment(GetRootComponent());
	TsunamiSurface->SetMobility(EComponentMobility::Movable);
	TsunamiSurface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	TsunamiSurface->SetCanEverAffectNavigation(false); TsunamiSurface->SetCastShadow(false);
	TsunamiSurface->SetBoundsScale(4.f);
	TsunamiSurface->SetTranslucentSortPriority(2);
	for (UInstancedStaticMeshComponent* Batch : { FloodSurfaceISM.Get(), WindStreakISM.Get(), RainStreakISM.Get(),RiverStageISM.Get(),LavaSurfaceISM.Get(),AshCloudISM.Get(),DroughtSurfaceISM.Get(), QuakeEpicenterRingISM.Get(), QuakeFissureISM.Get(), DisasterEpicenterBeaconISM.Get() })
	{
		Batch->SetupAttachment(GetRootComponent());
		Batch->SetMobility(EComponentMobility::Movable);
		Batch->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Batch->SetCanEverAffectNavigation(false);
		Batch->SetCastShadow(false);
		Batch->SetCullDistances(0, 70000);
	}
	ConstructorHelpers::FObjectFinder<UStaticMesh> Plane(TEXT("/Engine/BasicShapes/Plane.Plane"));
	ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	ConstructorHelpers::FObjectFinder<UStaticMesh> Cone(TEXT("/Engine/BasicShapes/Cone.Cone"));
	ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if(Sphere.Succeeded())VolcanoSpatterISM->SetStaticMesh(Sphere.Object);
	if (Plane.Succeeded()) { FloodSurfaceISM->SetStaticMesh(Plane.Object); WindStreakISM->SetStaticMesh(Plane.Object); QuakeFissureISM->SetStaticMesh(Plane.Object); }
	if (Cube.Succeeded()) RainStreakISM->SetStaticMesh(Cube.Object);
	if (Plane.Succeeded()) RiverStageISM->SetStaticMesh(Plane.Object);
	if (Cylinder.Succeeded()) QuakeEpicenterRingISM->SetStaticMesh(Cylinder.Object);
	if (Cone.Succeeded()) DisasterEpicenterBeaconISM->SetStaticMesh(Cone.Object);
	else if (Cylinder.Succeeded()) DisasterEpicenterBeaconISM->SetStaticMesh(Cylinder.Object);
	RiverStageISM->SetTranslucentSortPriority(1);
	for (UInstancedStaticMeshComponent* Batch:{LavaSurfaceISM.Get(),AshCloudISM.Get(),DroughtSurfaceISM.Get(),QuakeEpicenterRingISM.Get(),QuakeFissureISM.Get(),DisasterEpicenterBeaconISM.Get()})
	{
		if (Batch->GetStaticMesh() == nullptr && Plane.Succeeded()) Batch->SetStaticMesh(Plane.Object);
		Batch->SetNumCustomDataFloats(1);
	}
	ConstructorHelpers::FObjectFinder<UMaterialInterface> Flood(TEXT("/Game/FX/Aftershock/Materials/M_DisasterFloodVolume.M_DisasterFloodVolume"));
	ConstructorHelpers::FObjectFinder<UMaterialInterface> Wind(TEXT("/Game/FX/Aftershock/Materials/M_DisasterWindStreak.M_DisasterWindStreak"));
	ConstructorHelpers::FObjectFinder<UMaterialInterface> Rain(TEXT("/Game/FX/Aftershock/Materials/M_DisasterRainStreak.M_DisasterRainStreak"));
	ConstructorHelpers::FObjectFinder<UMaterialInterface> Tsunami(TEXT("/Game/FX/Aftershock/Materials/M_DisasterTsunamiWave.M_DisasterTsunamiWave"));
	ConstructorHelpers::FObjectFinder<UMaterialInterface> Fissure(TEXT("/Game/FX/Aftershock/Materials/M_DisasterQuakeFissure.M_DisasterQuakeFissure"));
	ConstructorHelpers::FObjectFinder<UMaterialInterface> Rock(TEXT("/Game/Environment/Materials/Presentation/M_PresentationRock.M_PresentationRock"));
	if (Flood.Succeeded()) FloodSurfaceISM->SetMaterial(0, Flood.Object);
	if (Wind.Succeeded())
	{
		WindStreakISM->SetMaterial(0, Wind.Object);
		QuakeEpicenterRingISM->SetMaterial(0, Wind.Object);
		DisasterEpicenterBeaconISM->SetMaterial(0, Wind.Object);
	}
	if (Fissure.Succeeded()) QuakeFissureISM->SetMaterial(0, Fissure.Object);
	else if (Rock.Succeeded()) QuakeFissureISM->SetMaterial(0, Rock.Object);
	else if (Wind.Succeeded()) QuakeFissureISM->SetMaterial(0, Wind.Object);
	if (Rain.Succeeded()) RainStreakISM->SetMaterial(0, Rain.Object);
	if (Tsunami.Succeeded()) TsunamiSurface->SetMaterial(0, Tsunami.Object);
}

void AAftershockDisasterPresentationActor::BindFoliageWind(const FAftershockDisasterVisualSnapshot& Snapshot, const UTerrainGridSubsystem& Terrain)
{
	const auto* City=GetWorld()?GetWorld()->GetSubsystem<UCityPresentationSubsystem>():nullptr;
	const int32 CityRevision=City?City->GetPresentationRevision():INDEX_NONE;
	if(FoliageBoundSnapshotRevision==Snapshot.Revision&&FoliageBoundCityRevision==CityRevision&&FoliageBoundCellBounds==BuiltVisibleCellBounds)return;
	FoliageBoundSnapshotRevision=Snapshot.Revision;FoliageBoundCityRevision=CityRevision;FoliageBoundCellBounds=BuiltVisibleCellBounds;
	for (UMaterialInstanceDynamic* Material : FoliageWindMaterials) if (Material) Material->SetScalarParameterValue(TEXT("StormStrength"), 0.f);
	FoliageWindMaterials.Reset();
	int32 FoliageInstances = 0;
	for (TActorIterator<AAftershockCityPresentationActor> It(GetWorld()); It; ++It)
	{
		TInlineComponentArray<UInstancedStaticMeshComponent*> Batches(*It);
		for (UInstancedStaticMeshComponent* Batch : Batches)
		{
			if (!Batch->GetName().Contains(TEXT("Woodland")) || !Batch->GetMaterial(0)) continue;
			for(int32 Slot=0;Slot<Batch->GetNumMaterials();++Slot)
			{
				auto* BaseMaterial=Batch->GetMaterial(Slot);if(!BaseMaterial)continue;
				auto* Material=Cast<UMaterialInstanceDynamic>(BaseMaterial);
				if(!Material){Material=UMaterialInstanceDynamic::Create(BaseMaterial,Batch);Batch->SetMaterial(Slot,Material);}
				if(!Material)continue;
				FoliageWindMaterials.AddUnique(Material);
				Material->SetVectorParameterValue(TEXT("StormDirection"),FLinearColor(Snapshot.DominantWindDirection.X,Snapshot.DominantWindDirection.Y,0.f));
				Material->SetScalarParameterValue(TEXT("StormStrength"),0.f);
			}
			if (Batch->NumCustomDataFloats < 1) Batch->SetNumCustomDataFloats(1);
			for (int32 Index = 0; Index < Batch->GetInstanceCount(); ++Index)
			{
				FTransform Transform;
				if (!Batch->GetInstanceTransform(Index, Transform, true)) continue;
				const FIntPoint Coord = Terrain.WorldToGrid(Transform.GetLocation());
				const float Local = FMath::Clamp(Snapshot.LocalSeverity.FindRef(Coord) / 100.f, 0.f, 1.5f);
				Batch->SetCustomDataValue(Index, 0, Local, false);
				++FoliageInstances;
			}
			Batch->MarkRenderStateDirty();
		}
	}
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_WIND_FOLIAGE batches=%d instances=%d source=CanonicalLocalSeverity direction=(%.3f,%.3f) scenarioDirectionOnly=1"),
		FoliageWindMaterials.Num(), FoliageInstances, Snapshot.DominantWindDirection.X, Snapshot.DominantWindDirection.Y);
}

void AAftershockDisasterPresentationActor::BeginSnapshot(const FAftershockDisasterVisualSnapshot& Snapshot, const UTerrainGridSubsystem& Terrain)
{
	ClearPresentation();
	BuiltVisibleCellBounds=GetAftershockDrawableCellBounds(Terrain);
	SetActorHiddenInGame(false);
	if (!FloodMaterial) FloodMaterial = MakeWeatherMaterial(FloodSurfaceISM);
	if (!WindMaterial) WindMaterial = MakeWeatherMaterial(WindStreakISM);
	if (!RainMaterial) RainMaterial = MakeWeatherMaterial(RainStreakISM);
	FloodSurfaceISM->SetVisibility(FloodMaterial != nullptr);
	WindStreakISM->SetVisibility(WindMaterial != nullptr);
	RainStreakISM->SetVisibility(RainMaterial != nullptr);
	const auto PrepareEcologyMaterial=[](UInstancedStaticMeshComponent* Batch,const TCHAR* Path,UMaterialInstanceDynamic*& Out)
	{
		if (!Out)
		{
			if (UMaterialInterface* Base=LoadObject<UMaterialInterface>(nullptr,Path)) { Batch->SetMaterial(0,Base); Out=MakeWeatherMaterial(Batch); }
		}
		Batch->SetVisibility(Out!=nullptr);
		if (Out) Out->SetScalarParameterValue(TEXT("EventEnvelope"),0.f);
	};
	// Loading by stable authored paths also permits the editor authoring script
	// to finish before the first event, without a stale CDO missing-asset pointer.
	if(Snapshot.Type==EDisasterType::Volcano){UMaterialInstanceDynamic* Ash=AshMaterial;PrepareEcologyMaterial(AshCloudISM,TEXT("/Game/FX/Aftershock/Materials/M_DisasterVolcanicAsh.M_DisasterVolcanicAsh"),Ash);AshMaterial=Ash;}
	if(Snapshot.Type==EDisasterType::Drought){UMaterialInstanceDynamic* Drought=DroughtMaterial;PrepareEcologyMaterial(DroughtSurfaceISM,TEXT("/Game/FX/Aftershock/Materials/M_DisasterDrought.M_DisasterDrought"),Drought);DroughtMaterial=Drought;}
	TArray<FIntPoint> Cells;
	Snapshot.LocalSeverity.GetKeys(Cells);
	if(Snapshot.Type==EDisasterType::Volcano)
	{
		TSet<FIntPoint> Union;for(FIntPoint Coord:Cells)Union.Add(Coord);
		for(const auto& Pair:Snapshot.LavaSeverity)if(Pair.Value>0.f)Union.Add(Pair.Key);
		for(const auto& Pair:Snapshot.AshSeverity)if(Pair.Value>0.f)Union.Add(Pair.Key);
		Cells=Union.Array();
	}
	Cells.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.Y == B.Y ? A.X < B.X : A.Y < B.Y; });
	Cells.RemoveAll([&Terrain, &Snapshot](const FIntPoint& Coord)
	{
		return !Terrain.IsPlayableCoord(Coord) || (Snapshot.LocalSeverity.FindRef(Coord)<=0.f
			&&(Snapshot.Type!=EDisasterType::Volcano||(Snapshot.LavaSeverity.FindRef(Coord)<=0.f&&Snapshot.AshSeverity.FindRef(Coord)<=0.f)));
	});
	TArray<FIntPoint> VisualCells=Cells;VisualCells.RemoveAll([this](FIntPoint Coord){return !BuiltVisibleCellBounds.Contains(Coord);});
	// Precipitation is an event input, not the consequence of ponding. A drained
	// city (or an empty canonical flood map) still receives the same visible rain.
	const bool bWeatherStreakEvent=Snapshot.Type==EDisasterType::Flood||Snapshot.Type==EDisasterType::Wind;
	TArray<FIntPoint> WeatherCells;
	if(bWeatherStreakEvent)WeatherCells=VisualCells;
	if (Snapshot.Type == EDisasterType::Flood)
	{
		WeatherCells.Reset();
		for (const FGridCell& Cell : Terrain.Cells) if (Cell.bIsPlayable&&BuiltVisibleCellBounds.Contains(Cell.Coords)) WeatherCells.Add(Cell.Coords);
		WeatherCells.Sort([](const FIntPoint& A,const FIntPoint& B){return A.Y==B.Y?A.X<B.X:A.Y<B.Y;});
	}
	// Cosmetic density LOD only: retain the same affected cells/intensities, but
	// spend 75% of visible streak seeds within two cells of occupied city lots.
	// The remaining 25% still samples the whole canonical affected region.
	TSet<FIntPoint> CityWeatherMask;
	if(bWeatherStreakEvent)for (const FGridCell& Cell : Terrain.Cells)
	{
		if (!Cell.bIsPlayable || Cell.OccupyingBuildingID == INDEX_NONE) continue;
		for (int32 Y = -2; Y <= 2; ++Y)
		{
			for (int32 X = -2; X <= 2; ++X) CityWeatherMask.Add(Cell.Coords + FIntPoint(X, Y));
		}
	}
	TArray<FIntPoint> CityWeatherCells;
	for (const FIntPoint Coord : WeatherCells) if (CityWeatherMask.Contains(Coord)) CityWeatherCells.Add(Coord);
	FRandomStream Random(Terrain.TerrainSeed ^ (Snapshot.Revision * 73856093));
	auto PickWeatherCell = [&](int32 StreakIndex)
	{
		const TArray<FIntPoint>& SeedPool = !CityWeatherCells.IsEmpty() && StreakIndex % 4 != 3 ? CityWeatherCells : WeatherCells;
		return SeedPool[Random.RandRange(0, SeedPool.Num() - 1)];
	};
	const FTransform Hidden(FQuat::Identity, FVector::ZeroVector, FVector::ZeroVector);
	if (Snapshot.Type == EDisasterType::Flood)
	{
		FloodCells = Cells;
		if(!RiverWaterMaterial)RiverWaterMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Environment/Materials/Presentation/M_PresentationWater.M_PresentationWater"));
		RiverStageISM->SetMaterial(0,RiverWaterMaterial);
		const FIntPoint Steps[]={FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1)};
		float BeforeSum=0.f,AfterSum=0.f;
		for (const FGridCell& Cell:Terrain.Cells)
		{
			// V3 absolute water heads are the only above-bank event surface.
			// Preserve the legacy display-only 14..90cm fill stage solely for V1/V2.
			if(Terrain.ContinuationGeneratorVersion>=3)continue;
			if (!BuiltVisibleCellBounds.Contains(Cell.Coords)||!Cell.bIsPlayable || !Cell.bIsRiverbed || Cell.bIsOcean || !Snapshot.RiverFillAfter.Contains(Cell.Coords)) continue;
			float BankHead=Cell.Elevation+90.f;
			for (FIntPoint Step:Steps)
			{
				const FIntPoint Neighbour=Cell.Coords+Step;
				if (!Terrain.IsPlayableCoord(Neighbour)) continue;
				const FGridCell Bank=Terrain.GetCell(Neighbour);
				if (!Bank.bIsRiverbed && !Bank.bIsOcean && Bank.Elevation>Cell.Elevation+14.f) BankHead=FMath::Min(BankHead,Bank.Elevation);
			}
			const float Headroom=FMath::Clamp(BankHead-Cell.Elevation-14.f,10.f,90.f);
			RiverStageCells.Add(Cell.Coords); RiverStageHeadroomCm.Add(Headroom);
			const float Fill=FMath::Clamp(Snapshot.RiverFillBefore.FindRef(Cell.Coords),0.f,1.f);
			RiverStageTransforms.Add(FTransform(FQuat::Identity,Terrain.GridToWorld(Cell.Coords)+FVector(0,0,12.f+Fill*Headroom),FVector(Terrain.CellSize*.0102f,Terrain.CellSize*.0102f,1.f)));
			BeforeSum+=Fill; AfterSum+=Snapshot.RiverFillAfter.FindRef(Cell.Coords);
		}
		RiverStageISM->AddInstances(RiverStageTransforms,false,true,false);
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_RIVER_FILL_PRESENTATION cells=%d meanBefore=%.3f meanAfter=%.3f riverFillSeconds=6 landInundationStartsSeconds=6 source=CanonicalRiverFillBeforeAfter artBankHeadroomNotMetres=1 separateFlowSolver=0 rainStartsImmediately=1"),RiverStageCells.Num(),RiverStageCells.IsEmpty() ? 0.f : BeforeSum/RiverStageCells.Num(),RiverStageCells.IsEmpty() ? 0.f : AfterSum/RiverStageCells.Num());
		FAftershockFloodSurfaceStats FloodStats;
		const bool bBuiltFloodSurface=Snapshot.FloodModelVersion!=2&&BuildAftershockFloodSurface(this,&Terrain,Snapshot.LocalSeverity,FloodSurfaceISM,&FloodStats,&VisibleFloodPeakDepthCm,&Snapshot);
		bPhysicalFloodArrival=Snapshot.FloodModelVersion==1;
		CurrentFloodArrivalProgress=bPhysicalFloodArrival?0.f:1.f;
		// The derived depth/arrival maps cover ALL resident cells even when the
		// current camera has no local mesh; far-road closure must use the same age.
		if(bPhysicalFloodArrival)for(const auto& Pair:VisibleFloodPeakDepthCm)
			VisibleFloodArrival.Add(Pair.Key,FMath::Clamp(Snapshot.WaterArrivalFraction.FindRef(Pair.Key),0.f,1.f));
		if (bBuiltFloodSurface)
		{
			// The shore-clipped volume is prepared at event start or window change.
			// Its rise/drain keeps the original age; hazard values stay read-only.
			VisibleFloodLandCells = FloodStats.ReconstructedLandCells;
			FloodVolumeFrontTriangles = FloodStats.FrontTriangles;
			if (FloodMaterial)
			{
				FloodSurfaceISM->SetMaterial(0, FloodMaterial);
				FloodMaterial->SetScalarParameterValue(TEXT("FloodEnvelope"), 0.f);
				FloodMaterial->SetScalarParameterValue(TEXT("FloodEncodingCm"),FloodStats.EncodingCm);
				FloodMaterial->SetScalarParameterValue(TEXT("FloodPhysicalArrivalEnabled"),bPhysicalFloodArrival?1.f:0.f);
				FloodMaterial->SetScalarParameterValue(TEXT("FloodArrivalProgress"),CurrentFloodArrivalProgress);
				FloodMaterial->SetScalarParameterValue(TEXT("Opacity"), 0.88f);
				FloodMaterial->SetScalarParameterValue(TEXT("VisualStrength"), FMath::Clamp(Snapshot.EffectiveIntensity / 100.f, 0.f, 1.f));
				{
					const UWorld* FW = GetWorld();
					const auto* FSim = FW ? FW->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
					if (FSim && !FSim->ComputedVelX.IsEmpty())
					{
						float SumVx = 0.f, SumVy = 0.f;
						int32 N = 0;
						for (const auto& Pair : FSim->ComputedVelX)
						{
							SumVx += Pair.Value;
							SumVy += FSim->ComputedVelY.FindRef(Pair.Key);
							++N;
						}
						if (N > 0)
						{
							FloodMaterial->SetScalarParameterValue(TEXT("FloodFlowOverrideX"), SumVx / N);
							FloodMaterial->SetScalarParameterValue(TEXT("FloodFlowOverrideY"), SumVy / N);
						}
					}
				}
			}
			FloodSurfaceISM->AddInstance(FTransform::Identity, true);
		}
		else
		{
			FloodSurfaceISM->SetVisibility(false);
			if (FloodStats.ReconstructedWetCells > 0)
			{
				bool bLocalWet=false;for(const auto& Pair:VisibleFloodPeakDepthCm)bLocalWet|=BuiltVisibleCellBounds.Contains(Pair.Key);
				if(bLocalWet){UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_FLOOD_SURFACE_BUILD_FAILED revision=%d localWetCellsPresent=1 fullResidentDepthMapRetained=1"),Snapshot.Revision);}
				else{UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_FLOOD_NO_LOCAL_MESH revision=%d canonicalFloodCells=%d fullResidentDepthMapRetained=1"), Snapshot.Revision, FloodCells.Num());}
			}
			else
			{
				FloodSurfaceISM->SetStaticMesh(nullptr);
				UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_FLOOD_NO_VISIBLE_INUNDATION revision=%d canonicalFloodCells=%d weakRainSheetSuppressed=1 precipitationRemainsIndependent=1"), Snapshot.Revision, FloodCells.Num());
			}
		}
		const int32 Count = WeatherCells.IsEmpty() || Snapshot.EffectiveIntensity <= 0.f ? 0 : FMath::Clamp(FMath::RoundToInt(Snapshot.EffectiveIntensity * 9.f), 48, MaxRainStreaks);
		RainStreaks.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FAftershockWeatherStreak& Drop = RainStreaks.AddDefaulted_GetRef();
			Drop.Cell = PickWeatherCell(Index);
			Drop.Offset = FVector(Random.FRandRange(-0.45f, 0.45f) * Terrain.CellSize, Random.FRandRange(-0.45f, 0.45f) * Terrain.CellSize, 0.f);
			Drop.Phase = Random.FRand();
			Drop.LocalStrength = FMath::Clamp(Snapshot.EffectiveIntensity / 100.f, 0.f, 1.f);
			Drop.VisualSpeed = Random.FRandRange(1200.f, 1900.f);
			Drop.Length = Random.FRandRange(170.f, 320.f);
		}
		RainTransforms.Init(Hidden, RainStreaks.Num());
		RainStreakISM->AddInstances(RainTransforms, false, true, false);
	}
	else if (Snapshot.Type == EDisasterType::Tsunami)
	{
		// A marine event is neither rainfall nor a river flood. Only a single
		// canonical coast/inundation mesh is animated; no rain/wind streak seeds.
		bTsunamiActive = true;
		FloodSurfaceISM->SetVisibility(false); WindStreakISM->SetVisibility(false); RainStreakISM->SetVisibility(false);
		if (!TsunamiMaterial && TsunamiSurface->GetMaterial(0))
		{
			TsunamiMaterial = UMaterialInstanceDynamic::Create(TsunamiSurface->GetMaterial(0), TsunamiSurface);
			TsunamiSurface->SetMaterial(0,TsunamiMaterial);
		}
		FAftershockTsunamiSurfaceData Data;
		const bool bBuilt = Snapshot.FloodModelVersion!=2 && Snapshot.EffectiveIntensity > 0.f && TsunamiMaterial
			&& BuildAftershockTsunamiSurface(this,&Terrain,Snapshot,TsunamiSurface,Data);
		TsunamiSurface->SetVisibility(bBuilt);
		TsunamiPeakDepthCm = MoveTemp(Data.PeakDepthCm);
		TsunamiArrivalFraction = MoveTemp(Data.ArrivalFraction);
		TsunamiOceanCells = Data.OceanCells; TsunamiLandCells = Data.AffectedLandCells;
		if (TsunamiMaterial)
		{
			TsunamiMaterial->SetScalarParameterValue(TEXT("TsunamiProgress"),0.f);
			TsunamiMaterial->SetScalarParameterValue(TEXT("TsunamiDrain"),1.f);
			TsunamiMaterial->SetScalarParameterValue(TEXT("CrestHeightCm"),FMath::Clamp((Snapshot.SurgePeakHeadCm-Terrain.CoastalSeaLevelCm)*.45f,60.f,500.f));
		}
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_TSUNAMI_PRESENTATION revision=%d oceanCells=%d inundatedLandCells=%d triangles=%d rain=0 wind=0 perCellActors=0 canonicalOnly=1"),
			Snapshot.Revision,TsunamiOceanCells,TsunamiLandCells,Data.Triangles);
	}
	else if (Snapshot.Type == EDisasterType::Wind)
	{
		const int32 Count = WeatherCells.IsEmpty() ? 0 : FMath::Clamp(FMath::RoundToInt(Snapshot.EffectiveIntensity * 4.f), 32, MaxWindStreaks);
		WindStreaks.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FAftershockWeatherStreak& Streak = WindStreaks.AddDefaulted_GetRef();
			Streak.Cell = PickWeatherCell(Index);
			Streak.Offset = FVector(Random.FRandRange(-0.3f, 0.3f) * Terrain.CellSize, Random.FRandRange(-0.3f, 0.3f) * Terrain.CellSize, Random.FRandRange(80.f, 700.f));
			Streak.Phase = Random.FRand();
			Streak.LocalStrength = FMath::Clamp(Snapshot.LocalSeverity.FindRef(Streak.Cell) / 100.f, 0.f, 1.5f);
			Streak.VisualSpeed = FMath::Lerp(300.f, 1600.f, FMath::Min(1.f, Streak.LocalStrength));
			Streak.Length = FMath::Lerp(120.f, 480.f, FMath::Min(1.f, Streak.LocalStrength)) * Random.FRandRange(0.7f, 1.2f);
		}
		WindTransforms.Init(Hidden, WindStreaks.Num());
		WindStreakISM->AddInstances(WindTransforms, false, true, false);
		BindFoliageWind(Snapshot, Terrain);
	}
	else if (Snapshot.Type==EDisasterType::Fire || Snapshot.Type==EDisasterType::Volcano || Snapshot.Type==EDisasterType::Drought)
	{
		if(Snapshot.Type==EDisasterType::Fire)
			BuildFireDisplay(this,Terrain,VisualCells,&Snapshot.LocalSeverity,Snapshot.DominantWindDirection,FireBatches,FireMaterials,FireVisuals);
		if(Snapshot.Type==EDisasterType::Volcano)BuildVolcanoPresentation(Snapshot,Terrain);
		const int32 Stride=FMath::Max(1,FMath::DivideAndRoundUp(VisualCells.Num(),Snapshot.Type==EDisasterType::Drought ? 800 : 240));
		for (int32 I=0;I<VisualCells.Num();I+=Stride)
		{
			const FIntPoint Coord=VisualCells[I]; const float Strength=FMath::Clamp(Snapshot.LocalSeverity.FindRef(Coord)/100.f,0.f,1.f);
			const FVector Base=Terrain.GridToWorld(Coord);
			if (Snapshot.Type==EDisasterType::Volcano)
			{
				// The actual three-dimensional ground-backed volume replaces the
				// old overlapping horizontal lava cards. Ash fall remains local.
				const float AshStrength=FMath::Clamp(Snapshot.AshSeverity.FindRef(Coord)/100.f,0.f,1.f);
				if (AshStrength>.02f && AshMaterial)
				{
					const int32 Instance=AshCloudISM->AddInstance(FTransform(FRotator(0,I%180,70.f),Base+FVector(0,0,450+AshStrength*1000),FVector(8.f,10.f,1.f)),true);
					AshCloudISM->SetCustomDataValue(Instance,0,AshStrength,false);
				}
			}
			if (Snapshot.Type==EDisasterType::Drought && Strength>.05f && DroughtMaterial)
			{
				const int32 Instance=DroughtSurfaceISM->AddInstance(FTransform(FQuat::Identity,Base+FVector(0,0,3.f),FVector(Terrain.CellSize*.0098f,Terrain.CellSize*.0098f,1)),true);
				DroughtSurfaceISM->SetCustomDataValue(Instance,0,Strength,false);
			}
		}
		for (UInstancedStaticMeshComponent* Batch:{LavaSurfaceISM.Get(),AshCloudISM.Get(),DroughtSurfaceISM.Get()}) Batch->MarkRenderStateDirty();
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_ECOLOGY_PRESENTATION type=%d canonicalCells=%d flameInstances=%d lavaInstances=%d ashInstances=%d droughtInstances=%d source=ExactHazardSnapshot displayOnly=1 perParticleActors=0"),static_cast<int32>(Snapshot.Type),Cells.Num(),FireBatches.Num()==4?FireBatches[0]->GetInstanceCount():0,LavaSurfaceISM->GetInstanceCount(),AshCloudISM->GetInstanceCount(),DroughtSurfaceISM->GetInstanceCount());
	}
	else if (Snapshot.Type == EDisasterType::Earthquake)
	{
		const FIntPoint EpicenterCoord = (Snapshot.SourceCoord != FIntPoint(INDEX_NONE, INDEX_NONE) && Terrain.IsValidCoord(Snapshot.SourceCoord))
			? Snapshot.SourceCoord : FIntPoint(Terrain.GridWidth / 2, Terrain.GridHeight / 2);
		const FVector EpicenterWorld = Terrain.GridToWorld(EpicenterCoord);
		if (QuakeEpicenterRingISM)
		{
			QuakeEpicenterRingISM->ClearInstances();
			for (int32 Ring = 0; Ring < 3; ++Ring)
			{
				const float InitialRadius = (Ring + 1) * 300.f;
				const int32 Inst = QuakeEpicenterRingISM->AddInstance(FTransform(FQuat::Identity, EpicenterWorld + FVector(0, 0, 15.f + Ring * 2.f), FVector(InitialRadius / 50.f, InitialRadius / 50.f, 0.04f)), true);
				QuakeEpicenterRingISM->SetCustomDataValue(Inst, 0, 1.0f, false);
			}
		}
		if (QuakeFissureISM)
		{
			QuakeFissureISM->ClearInstances();
			TArray<FTransform> CrackTransforms;CrackTransforms.Reserve(MaxQuakeFissures);
			TArray<float> CrackStrengths;CrackStrengths.Reserve(MaxQuakeFissures);
			const auto* Roads=GetWorld()?GetWorld()->GetSubsystem<URoadNetworkSubsystem>():nullptr;
			for (const FIntPoint Coord : VisualCells)
			{
				if(CrackTransforms.Num()>=MaxQuakeFissures)break;
				FGridCell Cell;if(!Terrain.TryGetCell(Coord,Cell))continue;
				if (!Cell.bIsPlayable || !Cell.bHasRoad) continue;
				const float DistToEpicenter = FVector2D::Distance(FVector2D(Cell.Coords), FVector2D(EpicenterCoord));
				const bool bNearEpicenter = (DistToEpicenter <= 8.5f);
				const bool bHighFault = (Cell.FaultLineProximity > 0.40f);
				if (bNearEpicenter || bHighFault)
				{
					const float CrackIntensity = FMath::Clamp(
						(bNearEpicenter ? (1.0f - DistToEpicenter / 8.5f) : 0.0f) + Cell.FaultLineProximity * 0.7f,
						0.2f, 1.0f
					);
					const FVector RoadPos = Roads?Roads->RoadGridToWorld(Cell.Coords):Terrain.GridToWorld(Cell.Coords)+FVector(0,0,8.f);
					const auto HeightAt=[&](FIntPoint Neighbor)
					{
						if(!Terrain.IsValidCoord(Neighbor))return static_cast<float>(RoadPos.Z);
						const auto Other=Terrain.GetCell(Neighbor);
						if(Cell.BridgeID!=INDEX_NONE&&Other.BridgeID!=Cell.BridgeID)return static_cast<float>(RoadPos.Z);
						if(Cell.BridgeID==INDEX_NONE&&Other.bIsRiverbed)return static_cast<float>(RoadPos.Z);
						return Roads?Roads->GetRoadSurfaceElevation(Neighbor):static_cast<float>(Terrain.GridToWorld(Neighbor).Z+8.f);
					};
					const float GX=(HeightAt(Cell.Coords+FIntPoint(1,0))-HeightAt(Cell.Coords-FIntPoint(1,0)))/(2.f*Terrain.CellSize);
					const float GY=(HeightAt(Cell.Coords+FIntPoint(0,1))-HeightAt(Cell.Coords-FIntPoint(0,1)))/(2.f*Terrain.CellSize);
					FRandomStream CellRnd((Cell.Coords.X * 1973) ^ (Cell.Coords.Y * 9277));
					const int32 NumCracks = (CrackIntensity > 0.6f) ? 2 : 1;
					for (int32 c = 0; c < NumCracks && CrackTransforms.Num()<MaxQuakeFissures; ++c)
					{
						const float YawOffset = CellRnd.FRandRange(0.f, 360.f);
						const float ScaleL = CellRnd.FRandRange(1.6f, 3.2f) * CrackIntensity;
						const float ScaleW = CellRnd.FRandRange(0.18f, 0.38f) * CrackIntensity;
						FVector Position=RoadPos+FVector(CellRnd.FRandRange(-110.f,110.f),CellRnd.FRandRange(-110.f,110.f),0.f);
						Position.Z=RoadPos.Z+(Position.X-RoadPos.X)*GX+(Position.Y-RoadPos.Y)*GY+2.f+c*.5f;
						const FQuat Rotation=FRotationMatrix::MakeFromZX(FVector(-GX,-GY,1.f).GetSafeNormal(),FRotator(0.f,YawOffset,0.f).Vector()).ToQuat();
						CrackTransforms.Add(FTransform(Rotation,Position,FVector(ScaleL,ScaleW,1.f)));
						CrackStrengths.Add(CrackIntensity);
					}
				}
			}
			QuakeFissureISM->AddInstances(CrackTransforms,false,true,false);
			for(int32 I=0;I<CrackStrengths.Num();++I)QuakeFissureISM->SetCustomDataValue(I,0,CrackStrengths[I],false);
			QuakeFissureISM->SetVisibility(GetAftershockPresentationBudget(GetWorld()).bDrawDecorations);
			QuakeFissureISM->MarkRenderStateDirty();
		}
	}
	// Debug location markers are not atmospheric effects. Never draw the old
	// 45-metre white cone over a normal fire/quake/flood scene or an art capture.
	if (FParse::Param(FCommandLine::Get(),TEXT("AftershockDisasterDebugMarkers"))
		&&DisasterEpicenterBeaconISM && Snapshot.SourceCoord != FIntPoint(INDEX_NONE, INDEX_NONE) && Terrain.IsValidCoord(Snapshot.SourceCoord))
	{
		DisasterEpicenterBeaconISM->ClearInstances();
		const FVector SourceWorld = Terrain.GridToWorld(Snapshot.SourceCoord);
		const float BeaconHeight = 4500.f;
		const int32 BeaconInst = DisasterEpicenterBeaconISM->AddInstance(FTransform(FRotator(0, 0, 0), SourceWorld + FVector(0, 0, BeaconHeight * 0.5f), FVector(4.5f, 4.5f, BeaconHeight / 100.f)), true);
		DisasterEpicenterBeaconISM->SetCustomDataValue(BeaconInst, 0, 1.0f, false);
	}
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_DISASTER_PRESENTATION_START revision=%d type=%d canonicalCells=%d positivePlayableCells=%d canonicalFloodCells=%d visibleFloodCells=%d visibleFloodLandCells=%d floodMeshInstances=%d weatherSeedCells=%d cityWeatherSeedCells=%d cosmeticNearCitySeedFraction=0.75 windStreaks=%d rainStreaks=%d duration=%.1f severityUnits=Dimensionless floodTopology=TerrainAwareShoreClippedVolume floodEnvelopeOnGpu=1 rainIndependentOfPonding=1 noPhysicalFluidSolver=1"),
		Snapshot.Revision, static_cast<int32>(Snapshot.Type), Snapshot.LocalSeverity.Num(), Cells.Num(), FloodCells.Num(), VisibleFloodPeakDepthCm.Num(), VisibleFloodLandCells, FloodSurfaceISM->GetInstanceCount(), WeatherCells.Num(), CityWeatherCells.Num(), WindStreaks.Num(), RainStreaks.Num(), Snapshot.DurationSeconds);
}

void AAftershockDisasterPresentationActor::RebindFoliageWind(const FAftershockDisasterVisualSnapshot& Snapshot, const UTerrainGridSubsystem& Terrain, float AgeSeconds)
{
	// City rebuilding clears per-instance custom data but retains batch materials.
	// Restore just the affected foliage inputs at the existing event's current age.
	BindFoliageWind(Snapshot, Terrain);
	const float Envelope = WeatherEnvelope(AgeSeconds, Snapshot.GetEffectiveDurationSeconds());
	for (UMaterialInstanceDynamic* Material : FoliageWindMaterials) if (Material) Material->SetScalarParameterValue(TEXT("StormStrength"), Envelope);
}

void AAftershockDisasterPresentationActor::BuildVolcanoPresentation(const FAftershockDisasterVisualSnapshot& Snapshot,const UTerrainGridSubsystem& Terrain)
{
	if(!VolcanoLavaMaterial)VolcanoLavaMaterial=VolcanoMaterial(VolcanoLavaVolume,VolcanoLavaPath);
	if(!VolcanoPlumeMaterial)VolcanoPlumeMaterial=VolcanoMaterial(VolcanoPlume,VolcanoPlumePath);
	if(!VolcanoSpatterMaterial)VolcanoSpatterMaterial=VolcanoMaterial(VolcanoSpatterISM,VolcanoSpatterPath);
	VolcanoStats={};VolcanoStats.bMaterialsReady=VolcanoLavaMaterial&&VolcanoPlumeMaterial&&VolcanoSpatterMaterial;
	FIntPoint SourceCoord=Snapshot.SourceCoord;FGridCell Source;
	if(!Terrain.TryGetCell(SourceCoord,Source)||!Source.bVolcanicVent)SourceCoord=Terrain.VolcanicVentCoord;
	VolcanoStats.bSourceResident=Terrain.TryGetCell(SourceCoord,Source)&&Source.bVolcanicVent&&Source.bIsPlayable;
	VolcanoStats.bSourceInDrawableWindow=BuiltVisibleCellBounds.Contains(SourceCoord);
	FVector SourceWorld=VolcanoStats.bSourceResident?Terrain.GridToWorld(SourceCoord):FVector::ZeroVector;
	VolcanoStats.bSourceContextBacked=VolcanoStats.bSourceResident&&(Terrain.IsUsingRealGeography()||VolcanoStats.bSourceInDrawableWindow)&&VolcanoGround(Terrain,SourceWorld);VolcanoStats.SourceWorld=SourceWorld;
	if(VolcanoLavaMaterial)
	{
		VolcanoLavaMaterial->SetScalarParameterValue(TEXT("EventEnvelope"),0.f);VolcanoLavaMaterial->SetScalarParameterValue(TEXT("FlowProgress"),0.f);VolcanoLavaMaterial->SetScalarParameterValue(TEXT("Cooling"),0.f);
		BuildVolcanoLavaMesh(this,Terrain,Snapshot.LavaSeverity,SourceCoord,VolcanoLavaVolume,VolcanoStats,&Snapshot.LavaArrivalFraction);
	}
	if(VolcanoStats.bSourceContextBacked&&Snapshot.EffectiveIntensity>0.f)
	{
		const float Strength=FMath::Clamp(Snapshot.EffectiveIntensity/100.f,0.f,1.f);
		if(VolcanoPlumeMaterial)
		{
			BuildVolcanoPlumeMesh(this,SourceWorld,Snapshot.DominantWindDirection,Strength,VolcanoPlume,VolcanoStats);
			VolcanoPlumeMaterial->SetScalarParameterValue(TEXT("EventEnvelope"),0.f);VolcanoPlumeMaterial->SetScalarParameterValue(TEXT("AshDensity"),Snapshot.AshSeverity.IsEmpty()?.18f:.75f);
		}
		if(VolcanoSpatterMaterial&&!Snapshot.LavaSeverity.IsEmpty())
		{
			FRandomStream Random(Terrain.TerrainSeed^(Snapshot.Revision*73856093));FVector Wind=Snapshot.DominantWindDirection;Wind.Z=0;Wind.Normalize();
			for(int32 I=0;I<MaxVolcanoSpatter;++I)
			{
				FAftershockVolcanoSpatter Particle;const float Angle=Random.FRandRange(0,2.f*PI),SideSpeed=Random.FRandRange(220.f,620.f);
				Particle.Velocity=FVector(FMath::Cos(Angle)*SideSpeed,FMath::Sin(Angle)*SideSpeed,Random.FRandRange(2000.f,4000.f))+Wind*180.f;
				Particle.LifetimeSeconds=2.f*Particle.Velocity.Z/980.f;Particle.PhaseSeconds=Random.FRandRange(0,Particle.LifetimeSeconds);Particle.RadiusCm=Random.FRandRange(8.f,21.f);
				VolcanoSpatter.Add(Particle);FTransform Hidden;Hidden.SetScale3D(FVector::ZeroVector);VolcanoSpatterTransforms.Add(Hidden);
			}
			VolcanoSpatterISM->AddInstances(VolcanoSpatterTransforms,false,true,false);VolcanoStats.SpatterInstances=VolcanoSpatter.Num();
		}
	}
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_VOLCANO_VOLUME canonicalLavaCells=%d drawnLavaCells=%d cellCap=%d topTriangles=%d skirtTriangles=%d minimumGroundClearanceCm=%.3f sourceResident=%d sourceInCityWindow=%d sourceContextBacked=%d plumeTriangles=%d spatterInstances=%d materialsReady=%d canonicalArrival=%d canonicalWrites=0 newHazardSolver=0"),VolcanoStats.CanonicalLavaCells,VolcanoStats.DrawnLavaCells,MaxVolcanoSurfaceCells,VolcanoStats.LavaTopTriangles,VolcanoStats.LavaSkirtTriangles,VolcanoStats.MinimumGroundClearanceCm,VolcanoStats.bSourceResident,VolcanoStats.bSourceInDrawableWindow,VolcanoStats.bSourceContextBacked,VolcanoStats.PlumeTriangles,VolcanoStats.SpatterInstances,VolcanoStats.bMaterialsReady,!Snapshot.LavaArrivalFraction.IsEmpty());
}

void AAftershockDisasterPresentationActor::AnimateVolcanoPresentation(const FAftershockDisasterVisualSnapshot& Snapshot,float AgeSeconds,float Envelope)
{
	const bool Visible=DrawRegionalVolcano(GetWorld());const float Progress=FMath::SmoothStep(0.f,FMath::Max(1.f,Snapshot.GetEffectiveDurationSeconds()*.72f),AgeSeconds);
	if(VolcanoLavaMaterial){VolcanoLavaMaterial->SetScalarParameterValue(TEXT("FlowProgress"),Progress);VolcanoLavaMaterial->SetScalarParameterValue(TEXT("EventEnvelope"),Envelope);}
	if(VolcanoPlumeMaterial)VolcanoPlumeMaterial->SetScalarParameterValue(TEXT("EventEnvelope"),Envelope);
	if(VolcanoSpatterMaterial)VolcanoSpatterMaterial->SetScalarParameterValue(TEXT("EventEnvelope"),Envelope);
	VolcanoLavaVolume->SetVisibility(Visible&&VolcanoLavaVolume->GetStaticMesh()!=nullptr);VolcanoPlume->SetVisibility(Visible&&VolcanoPlume->GetStaticMesh()!=nullptr);VolcanoSpatterISM->SetVisibility(Visible);
	for(int32 I=0;I<VolcanoSpatter.Num();++I)
	{
		const auto& Particle=VolcanoSpatter[I];const float Time=FMath::Fmod(FMath::Max(0.f,AgeSeconds)+Particle.PhaseSeconds,Particle.LifetimeSeconds);
		const FVector Position=VolcanoStats.SourceWorld+FVector(0,0,450.f)+Particle.Velocity*Time-FVector(0,0,.5f*980.f*Time*Time);
		const float Radius=Particle.RadiusCm/50.f;VolcanoSpatterTransforms[I]=FTransform(FQuat::Identity,Position,FVector(Radius*Envelope));
		VolcanoSpatterISM->SetCustomDataValue(I,0,1.f-Time/Particle.LifetimeSeconds,false);
	}
	if(!VolcanoSpatterTransforms.IsEmpty())VolcanoSpatterISM->BatchUpdateInstancesTransforms(0,VolcanoSpatterTransforms,true,true,true);
}

void AAftershockDisasterPresentationActor::RefreshVisibleWindow(const FAftershockDisasterVisualSnapshot& Snapshot,const UTerrainGridSubsystem& Terrain,float AgeSeconds)
{
	if(BuiltVisibleCellBounds==GetAftershockDrawableCellBounds(Terrain))return;
	if(Snapshot.Type==EDisasterType::Volcano&&Terrain.IsUsingRealGeography())
	{
		// The source-context lava/plume do not depend on the city's75-cell mask.
		// Panning only republishes bounded local ash; never rebuild those meshes,
		// reset the spatter phases, or restart the canonical arrival clock.
		BuiltVisibleCellBounds=GetAftershockDrawableCellBounds(Terrain);AshCloudISM->ClearInstances();
		if(AshMaterial)
		{
			TArray<FIntPoint> AshCells;for(const auto& Pair:Snapshot.AshSeverity)if(Pair.Value>2.f&&BuiltVisibleCellBounds.Contains(Pair.Key)&&Terrain.IsPlayableCoord(Pair.Key))AshCells.Add(Pair.Key);
			AshCells.Sort([](FIntPoint A,FIntPoint B){return A.Y==B.Y?A.X<B.X:A.Y<B.Y;});const int32 Stride=FMath::Max(1,FMath::DivideAndRoundUp(AshCells.Num(),240));
			for(int32 I=0;I<AshCells.Num();I+=Stride){const auto Coord=AshCells[I];const float Strength=FMath::Clamp(Snapshot.AshSeverity.FindRef(Coord)/100.f,0.f,1.f);const int32 Instance=AshCloudISM->AddInstance(FTransform(FRotator(0,I%180,70.f),Terrain.GridToWorld(Coord)+FVector(0,0,450.f+Strength*1000.f),FVector(8.f,10.f,1.f)),true);AshCloudISM->SetCustomDataValue(Instance,0,Strength,false);}
			AshCloudISM->MarkRenderStateDirty();
		}
		AnimateSnapshot(Snapshot,Terrain,AgeSeconds);return;
	}
	BeginSnapshot(Snapshot,Terrain);AnimateSnapshot(Snapshot,Terrain,AgeSeconds);
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_WEATHER_WINDOW_REBUILT revision=%d age=%.2f eventClockRestarted=0 bounds=(%d,%d)-(%d,%d) fullResidentDepthMap=1"),Snapshot.Revision,AgeSeconds,BuiltVisibleCellBounds.Min.X,BuiltVisibleCellBounds.Min.Y,BuiltVisibleCellBounds.Max.X,BuiltVisibleCellBounds.Max.Y);
}

void AAftershockDisasterPresentationActor::AnimateSnapshot(const FAftershockDisasterVisualSnapshot& Snapshot, const UTerrainGridSubsystem& Terrain, float AgeSeconds)
{
	if(QuakeFissureISM)QuakeFissureISM->SetVisibility(GetAftershockPresentationBudget(GetWorld()).bDrawDecorations);
	const float Envelope = WeatherEnvelope(AgeSeconds, Snapshot.GetEffectiveDurationSeconds());
	if(Snapshot.Type==EDisasterType::Fire)
	{
		if(FireVisuals.BuiltTier!=static_cast<int32>(GetAftershockPresentationBudget(GetWorld()).Tier)
			||FireVisuals.BuiltEffectsQuality!=FMath::Clamp(Scalability::GetQualityLevels().EffectsQuality,0,3))
		{
			TArray<FIntPoint> Cells;Snapshot.LocalSeverity.GetKeys(Cells);
			Cells.RemoveAll([this](FIntPoint Coord){return !BuiltVisibleCellBounds.Contains(Coord);});
			BuildFireDisplay(this,Terrain,Cells,&Snapshot.LocalSeverity,Snapshot.DominantWindDirection,FireBatches,FireMaterials,FireVisuals);
		}
		// Fire remains active while the canonical burning state remains active.
		// Shared absolute time preserves puff/flame continuity on persistent takeover.
		AnimateFireDisplay(this,Terrain,GetWorld()?GetWorld()->GetTimeSeconds():AgeSeconds,FMath::SmoothStep(0.f,.55f,AgeSeconds),FireBatches,FireMaterials,FireVisuals);
	}
	if(Snapshot.Type==EDisasterType::Volcano)AnimateVolcanoPresentation(Snapshot,AgeSeconds,Envelope);
	// Sequence is presentation only: canonical event results are already fixed.
	// The river first interpolates its SAME pre/post capacity fill for six seconds;
	// the reconstructed land volume then rises, while rainfall remains independent.
	CurrentRiverStageProgress=Snapshot.Type==EDisasterType::Flood ? FMath::SmoothStep(0.f,6.f,AgeSeconds) : 0.f;
	CurrentFloodEnvelope = Snapshot.Type == EDisasterType::Flood ? WeatherEnvelope(AgeSeconds-6.f,FMath::Max(1.f,Snapshot.DurationSeconds-6.f)) : 0.f;
	CurrentFloodArrivalProgress=bPhysicalFloodArrival?FMath::Clamp((AgeSeconds-6.f)/FMath::Max(.01f,Snapshot.DurationSeconds*.55f-6.f),0.f,1.f):1.f;
	for (int32 I=0;I<RiverStageCells.Num();++I)
	{
		const FIntPoint Coord=RiverStageCells[I];
		const float Before=FMath::Clamp(Snapshot.RiverFillBefore.FindRef(Coord),0.f,1.f),After=FMath::Clamp(Snapshot.RiverFillAfter.FindRef(Coord),0.f,1.f);
		const float Fill=FMath::Lerp(Before,After,CurrentRiverStageProgress);
		RiverStageTransforms[I].SetLocation(Terrain.GridToWorld(Coord)+FVector(0,0,12.f+Fill*RiverStageHeadroomCm[I]));
	}
	if (!RiverStageTransforms.IsEmpty()) RiverStageISM->BatchUpdateInstancesTransforms(0,RiverStageTransforms,true,true,true);
	for (UMaterialInstanceDynamic* Material:{LavaMaterial.Get(),AshMaterial.Get(),DroughtMaterial.Get()}) if (Material) Material->SetScalarParameterValue(TEXT("EventEnvelope"),Envelope);
	if (bTsunamiActive)
	{
		CurrentTsunamiProgress = FMath::SmoothStep(1.f,FMath::Max(2.f,Snapshot.DurationSeconds*.65f),AgeSeconds);
		CurrentTsunamiDrain = 1.f-FMath::SmoothStep(Snapshot.DurationSeconds*.72f,Snapshot.DurationSeconds,AgeSeconds);
		if (TsunamiMaterial)
		{
			TsunamiMaterial->SetScalarParameterValue(TEXT("TsunamiProgress"),CurrentTsunamiProgress);
			TsunamiMaterial->SetScalarParameterValue(TEXT("TsunamiDrain"),CurrentTsunamiDrain);
		}
	}
	if (FloodMaterial)
	{
		FloodMaterial->SetScalarParameterValue(TEXT("FloodEnvelope"), CurrentFloodEnvelope);
		FloodMaterial->SetScalarParameterValue(TEXT("FloodArrivalProgress"),CurrentFloodArrivalProgress);
		FloodMaterial->SetScalarParameterValue(TEXT("Opacity"), 0.88f);
		{
			const UWorld* FW = GetWorld();
			const auto* FSim = FW ? FW->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
			if (FSim && !FSim->ComputedVelX.IsEmpty())
			{
				float SumVx = 0.f, SumVy = 0.f;
				int32 N = 0;
				for (const auto& Pair : FSim->ComputedVelX)
				{
					SumVx += Pair.Value;
					SumVy += FSim->ComputedVelY.FindRef(Pair.Key);
					++N;
				}
				if (N > 0)
				{
					FloodMaterial->SetScalarParameterValue(TEXT("FloodFlowOverrideX"), SumVx / N);
					FloodMaterial->SetScalarParameterValue(TEXT("FloodFlowOverrideY"), SumVy / N);
				}
			}
		}
	}
	const FVector Direction = Snapshot.DominantWindDirection;
	const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X));
	for (int32 Index = 0; Index < WindStreaks.Num(); ++Index)
	{
		const FAftershockWeatherStreak& Streak = WindStreaks[Index];
		const float Travel = Terrain.CellSize * 4.f;
		const float Offset = FMath::Fmod(Streak.Phase * Travel + AgeSeconds * Streak.VisualSpeed, Travel) - Travel * 0.5f;
		FVector Position = Terrain.GridToWorld(Streak.Cell) + Streak.Offset + Direction * Offset;
		const FIntPoint CurrentCoord = Terrain.WorldToGrid(Position);
		if (!BuiltVisibleCellBounds.Contains(CurrentCoord)||!Terrain.IsPlayableCoord(CurrentCoord)) { WindTransforms[Index].SetScale3D(FVector::ZeroVector); continue; }
		Position.Z = Terrain.GetCell(CurrentCoord).Elevation + Streak.Offset.Z;
		const float Local = FMath::Clamp(Snapshot.LocalSeverity.FindRef(CurrentCoord) / 100.f, 0.f, 1.5f);
		const float Pulse = 0.55f + 0.45f * FMath::Sin((Streak.Phase + AgeSeconds * 0.7f) * 2.f * PI);
		WindTransforms[Index] = FTransform(FRotator(0.f, Yaw, 0.f), Position, FVector(Streak.Length * Envelope / 100.f, (18.f + 12.f * Local) * Pulse / 100.f, 1.f));
	}
	if (!WindTransforms.IsEmpty()) WindStreakISM->BatchUpdateInstancesTransforms(0, WindTransforms, true, true, true);
	if (WindMaterial) WindMaterial->SetScalarParameterValue(TEXT("Opacity"), 0.55f * Envelope);
	const FQuat RainRotation = FRotationMatrix::MakeFromZ(FVector(-Direction.X * 0.15f, -Direction.Y * 0.15f, -1.f)).ToQuat();
	for (int32 Index = 0; Index < RainStreaks.Num(); ++Index)
	{
		const FAftershockWeatherStreak& Drop = RainStreaks[Index];
		const float Phase = FMath::Fmod(Drop.Phase + AgeSeconds * Drop.VisualSpeed / 1500.f, 1.f);
		FVector Position = Terrain.GridToWorld(Drop.Cell) + Drop.Offset;
		Position += FVector(0.f, 0.f, (1.f - Phase) * 1500.f);
		RainTransforms[Index] = FTransform(RainRotation, Position, FVector(0.09f * Envelope, 0.09f * Envelope, Drop.Length / 100.f));
	}
	if (!RainTransforms.IsEmpty()) RainStreakISM->BatchUpdateInstancesTransforms(0, RainTransforms, true, true, true);
	if (RainMaterial) RainMaterial->SetScalarParameterValue(TEXT("Opacity"), 0.55f * Envelope);
	for (UMaterialInstanceDynamic* Material : FoliageWindMaterials) if (Material) Material->SetScalarParameterValue(TEXT("StormStrength"), Envelope);
	if (Snapshot.Type == EDisasterType::Earthquake && QuakeEpicenterRingISM && QuakeEpicenterRingISM->GetInstanceCount() > 0)
	{
		const FIntPoint EpicenterCoord = (Snapshot.SourceCoord != FIntPoint(INDEX_NONE, INDEX_NONE) && Terrain.IsValidCoord(Snapshot.SourceCoord))
			? Snapshot.SourceCoord : FIntPoint(Terrain.GridWidth / 2, Terrain.GridHeight / 2);
		const FVector EpicenterWorld = Terrain.GridToWorld(EpicenterCoord);
		TArray<FTransform> RingTransforms;
		RingTransforms.SetNum(3);
		for (int32 Ring = 0; Ring < 3; ++Ring)
		{
			const float Cycle = FMath::Fmod(AgeSeconds * 0.8f + Ring * 0.33f, 1.0f);
			const float WaveRadiusCm = Cycle * (Terrain.CellSize * Terrain.GridWidth * 0.75f);
			const float RadiusScale = FMath::Max(1.0f, WaveRadiusCm / 50.f);
			const float Fade = (1.0f - Cycle) * Envelope;
			RingTransforms[Ring] = FTransform(FQuat::Identity, EpicenterWorld + FVector(0, 0, 15.f + Ring * 2.f), FVector(RadiusScale, RadiusScale, 0.04f));
			QuakeEpicenterRingISM->SetCustomDataValue(Ring, 0, Fade, false);
		}
		QuakeEpicenterRingISM->BatchUpdateInstancesTransforms(0, RingTransforms, true, true, true);
	}
	if (DisasterEpicenterBeaconISM && DisasterEpicenterBeaconISM->GetInstanceCount() > 0)
	{
		const float Pulse = 0.85f + 0.35f * FMath::Sin(AgeSeconds * 4.5f);
		DisasterEpicenterBeaconISM->SetCustomDataValue(0, Envelope * Pulse, false);
	}
}

void AAftershockDisasterPresentationActor::ClearPresentation()
{
	ResetFireDisplay(FireBatches,FireMaterials,FireVisuals);FireVisuals.BuiltTier=INDEX_NONE;
	FloodSurfaceISM->ClearInstances(); WindStreakISM->ClearInstances(); RainStreakISM->ClearInstances();
	// A hidden component still strongly owns its transient mesh and render buffers.
	// Release completed event geometry; UObject/RHI destruction follows normal GC.
	ReleaseDisasterTransientMeshReference(FloodSurfaceISM);ReleaseDisasterTransientMeshReference(TsunamiSurface);
	RiverStageISM->ClearInstances(); RiverStageCells.Reset(); RiverStageHeadroomCm.Reset(); RiverStageTransforms.Reset(); CurrentRiverStageProgress=0.f;
	if (QuakeEpicenterRingISM) QuakeEpicenterRingISM->ClearInstances();
	if (QuakeFissureISM) QuakeFissureISM->ClearInstances();
	if (DisasterEpicenterBeaconISM) DisasterEpicenterBeaconISM->ClearInstances();
	for (UInstancedStaticMeshComponent* Batch:{LavaSurfaceISM.Get(),AshCloudISM.Get(),DroughtSurfaceISM.Get()}) Batch->ClearInstances();
	ReleaseDisasterTransientMeshReference(VolcanoLavaVolume);ReleaseDisasterTransientMeshReference(VolcanoPlume);VolcanoSpatterISM->ClearInstances();VolcanoSpatter.Empty();VolcanoSpatterTransforms.Empty();VolcanoStats={};
	for(auto* Surface:{VolcanoLavaVolume.Get(),VolcanoPlume.Get(),static_cast<UStaticMeshComponent*>(VolcanoSpatterISM.Get())})Surface->SetVisibility(false);
	for (UMaterialInstanceDynamic* Material:{LavaMaterial.Get(),AshMaterial.Get(),DroughtMaterial.Get()}) if (Material) Material->SetScalarParameterValue(TEXT("EventEnvelope"),0.f);
	FloodCells.Empty();
	bTsunamiActive = false; CurrentTsunamiProgress = 0.f; CurrentTsunamiDrain = 0.f;
	TsunamiPeakDepthCm.Empty(); TsunamiArrivalFraction.Empty(); TsunamiOceanCells=0; TsunamiLandCells=0;
	TsunamiSurface->SetVisibility(false);
	if(TsunamiMaterial) TsunamiMaterial->SetScalarParameterValue(TEXT("TsunamiDrain"),0.f);
	VisibleFloodPeakDepthCm.Empty();VisibleFloodArrival.Empty();bPhysicalFloodArrival=false;CurrentFloodArrivalProgress=1.f;CurrentFloodEnvelope = 0.f;
	VisibleFloodLandCells = 0; FloodVolumeFrontTriangles = 0;
	if (FloodMaterial){FloodMaterial->SetScalarParameterValue(TEXT("FloodEnvelope"),0.f);FloodMaterial->SetScalarParameterValue(TEXT("FloodPhysicalArrivalEnabled"),0.f);FloodMaterial->SetScalarParameterValue(TEXT("FloodArrivalProgress"),1.f);}
	if (FloodMaterial)
	{
		FloodMaterial->SetScalarParameterValue(TEXT("FloodFlowOverrideX"), 0.f);
		FloodMaterial->SetScalarParameterValue(TEXT("FloodFlowOverrideY"), 0.f);
	}
	WindStreaks.Reset(); WindTransforms.Reset(); RainStreaks.Reset(); RainTransforms.Reset();
	for (UMaterialInstanceDynamic* Material : FoliageWindMaterials) if (Material) Material->SetScalarParameterValue(TEXT("StormStrength"), 0.f);
	FoliageWindMaterials.Reset();
	FoliageBoundSnapshotRevision=INDEX_NONE;FoliageBoundCityRevision=INDEX_NONE;FoliageBoundCellBounds=FIntRect(0,0,0,0);
	SetActorHiddenInGame(true);
}

uint64 AAftershockDisasterPresentationActor::GetRetainedTransientMeshBytes() const
{
	uint64 Bytes=0;
	const UStaticMeshComponent* Surfaces[]={FloodSurfaceISM.Get(),TsunamiSurface.Get(),VolcanoLavaVolume.Get(),VolcanoPlume.Get()};
	for(const UStaticMeshComponent* Surface:Surfaces)
		if(UStaticMesh* Mesh=Surface?Surface->GetStaticMesh():nullptr)if(Mesh->HasAnyFlags(RF_Transient))Bytes+=Mesh->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal);
	// Reusable fire hulls intentionally remain component-owned between events.
	// Report their real retained resources instead of calling the event release zero.
	for(const UInstancedStaticMeshComponent* Batch:FireBatches)
		if(UStaticMesh* Mesh=Batch?Batch->GetStaticMesh():nullptr)Bytes+=Mesh->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal);
	return Bytes;
}

FString AAftershockDisasterPresentationActor::DescribeCurrentVisuals() const
{
	return FString::Printf(TEXT("canonicalFloodCells=%d visibleFloodCells=%d visibleFloodLandCells=%d floodFrontTriangles=%d floodEnvelope=%.3f floodMeshInstances=%d floodVisible=%d floodMaterial=%s windInstances=%d windVisible=%d rainInstances=%d rainVisible=%d foliageBatches=%d actorHidden=%d tsunamiOceanCells=%d tsunamiLandCells=%d tsunamiProgress=%.3f tsunamiDrain=%.3f tsunamiVisible=%d riverStageInstances=%d riverFillProgress=%.3f lavaVolumeCells=%d lavaVolumeTopTriangles=%d volcanoPlumeTriangles=%d volcanoSpatterInstances=%d volcanoSourceGroundBacked=%d volcanoMaterialsReady=%d floodPhysicalArrival=%d floodArrivalProgress=%.3f floodArrivalMapBytes=%llu fireCanonicalBurning=%d fireSources=%d fireBuildingSources=%d fireVolumeMaterials=%d/4 fireFlameInstances=%d fireSmokeInstances=%d firePerParticleActors=0"),
		FloodCells.Num(), VisibleFloodPeakDepthCm.Num(), VisibleFloodLandCells, FloodVolumeFrontTriangles, CurrentFloodEnvelope, FloodSurfaceISM->GetInstanceCount(), FloodSurfaceISM->IsVisible(), FloodMaterial ? *FloodMaterial->GetName() : TEXT("MISSING"),
		WindStreakISM->GetInstanceCount(), WindStreakISM->IsVisible(), RainStreakISM->GetInstanceCount(), RainStreakISM->IsVisible(), FoliageWindMaterials.Num(), IsHidden(),TsunamiOceanCells,TsunamiLandCells,CurrentTsunamiProgress,CurrentTsunamiDrain,TsunamiSurface->IsVisible(),RiverStageISM->GetInstanceCount(),CurrentRiverStageProgress,VolcanoStats.DrawnLavaCells,VolcanoStats.LavaTopTriangles,VolcanoStats.PlumeTriangles,VolcanoStats.SpatterInstances,VolcanoStats.bSourceContextBacked,VolcanoStats.bMaterialsReady,bPhysicalFloodArrival,CurrentFloodArrivalProgress,static_cast<uint64>(VisibleFloodArrival.GetAllocatedSize()),FireVisuals.CanonicalBurningCells,FireVisuals.Sources.Num(),FireVisuals.BuildingSources,FireVisuals.AuthoredMaterials,FireBatches.Num()==4?FireBatches[0]->GetInstanceCount():0,FireBatches.Num()==4?FireBatches[2]->GetInstanceCount():0);
}

float AAftershockDisasterPresentationActor::GetDisplayedFloodDepthCm(FIntPoint Coord) const
{
	if(bTsunamiActive)
	{
		if(IsHidden()) return 0.f;
		const float* Peak=TsunamiPeakDepthCm.Find(Coord); const float* Arrival=TsunamiArrivalFraction.Find(Coord);
		if(!Peak || !Arrival) return 0.f;
		const float Wet=FMath::SmoothStep(*Arrival-.055f,*Arrival+.065f,CurrentTsunamiProgress)*CurrentTsunamiDrain;
		return FMath::Max(0.f,(*Peak+4.f)*Wet-4.f);
	}
	if (IsHidden()) return 0.f;
	const float* Peak = VisibleFloodPeakDepthCm.Find(Coord);
	const float* Arrival=VisibleFloodArrival.Find(Coord);
	if(bPhysicalFloodArrival&&!Arrival)return 0.f;
	const float ArrivalWeight=bPhysicalFloodArrival&&CurrentFloodArrivalProgress<1.f?FMath::SmoothStep(FMath::Max(0.f,*Arrival-.05f),FMath::Min(1.f,*Arrival+.05f),CurrentFloodArrivalProgress):1.f;
	return Peak ? FMath::Max(0.f, (*Peak + 4.f) * CurrentFloodEnvelope * ArrivalWeight - 4.f) : 0.f;
}

bool UDisasterPresentationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return Super::ShouldCreateSubsystem(Outer) && World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UDisasterPresentationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UTerrainGridSubsystem>();
	Collection.InitializeDependency<UDisasterSimulationSubsystem>();
	Collection.InitializeDependency<UCityPresentationSubsystem>();
}

void UDisasterPresentationSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (UCityPresentationSubsystem* City = InWorld.GetSubsystem<UCityPresentationSubsystem>())
	{
		CityRebuiltDelegateHandle = City->OnPresentationRebuilt.AddUObject(this, &UDisasterPresentationSubsystem::HandleCityPresentationRebuilt);
	}
	if (UDisasterSimulationSubsystem* Disaster = InWorld.GetSubsystem<UDisasterSimulationSubsystem>())
	{
		Disaster->OnDisasterCompleted.AddUniqueDynamic(this,&UDisasterPresentationSubsystem::HandleDisasterCompleted);
		SnapshotDelegateHandle = Disaster->OnDisasterVisualSnapshotChanged.AddUObject(this, &UDisasterPresentationSubsystem::HandleSnapshot);
		if (Disaster->GetLatestVisualSnapshot().Revision > 0) HandleSnapshot(Disaster->GetLatestVisualSnapshot());
	}
	UpdatePersistentEcology();
	FTimerManagerTimerParameters Parameters;Parameters.bLoop=true;Parameters.bMaxOncePerFrame=true;
	InWorld.GetTimerManager().SetTimer(EcologyStateTimer, this, &UDisasterPresentationSubsystem::UpdatePersistentEcology, 1.f, Parameters);
}

void UDisasterPresentationSubsystem::HandleDisasterCompleted(const FDisasterReport& Report)
{
	if(Report.bWasRejected||Report.bNeedsCasualtyEvent||Report.DisasterType!=EDisasterType::Earthquake)return;
	UWorld* World=GetWorld();if(!World||World->bIsTearingDown)return;
	APlayerController* Controller=World->GetFirstPlayerController();
	if(auto* Camera=Controller?Cast<AAftershockCameraPawn>(Controller->GetPawn()):nullptr)
	{
		Camera->StartDisasterCameraShake(3.5f,3.8f);
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_QUAKE_CAMERA_SHAKE source=NewDisasterCompleted durationSeconds=3.5 intensity=3.8 snapshotRestoreOrWindowRebuild=0"));
	}
}

void UDisasterPresentationSubsystem::UpdatePersistentEcology()
{
	UWorld* World = GetWorld();
	const UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!World || !Terrain) return;
	if (!IsValid(EcologyStateActor))
	{
		FActorSpawnParameters Params; Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		EcologyStateActor = World->SpawnActor<AAftershockEcologyStateActor>(FVector::ZeroVector,FRotator::ZeroRotator,Params);
	}
	const float Age = PresentationAge(World,ActiveSnapshot);
	if (EcologyStateActor) EcologyStateActor->RefreshFromTerrain(*Terrain,ActiveSnapshot.Type,ActiveSnapshot.Revision>0 && Age<ActiveSnapshot.GetEffectiveDurationSeconds());
}

void UDisasterPresentationSubsystem::HandleCityPresentationRebuilt(int32 CityRevision)
{
	UpdatePersistentEcology();
	UWorld* World = GetWorld();
	const UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!World || !Terrain || !IsValid(PresentationActor)) return;
	const float Age = PresentationAge(World,ActiveSnapshot);
	if (ActiveSnapshot.Revision <= 0 || Age >= ActiveSnapshot.GetEffectiveDurationSeconds()) return;
	PresentationActor->RefreshVisibleWindow(ActiveSnapshot,*Terrain,Age);
	if(ActiveSnapshot.Type!=EDisasterType::Wind)return;
	PresentationActor->RebindFoliageWind(ActiveSnapshot, *Terrain, Age);
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_WIND_FOLIAGE_REBOUND cityRevision=%d disasterRevision=%d elapsed=%.2f duration=%.2f weatherTimelineRestarted=0"),
		CityRevision, ActiveSnapshot.Revision, Age, ActiveSnapshot.DurationSeconds);
}

void UDisasterPresentationSubsystem::RefreshVisiblePresentation()
{
	HandleCityPresentationRebuilt(0);
}

void UDisasterPresentationSubsystem::RefreshRegionalVolcanoPresentation()
{
	UpdatePersistentEcology();
}

void UDisasterPresentationSubsystem::HandleSnapshot(const FAftershockDisasterVisualSnapshot& Snapshot)
{
	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!World || !Terrain) return;
	World->GetTimerManager().ClearTimer(AnimationTimer);
	ReleaseCompletedSnapshotFields();
	ActiveSnapshot = Snapshot;
	ActiveCanonicalCellCount=Snapshot.LocalSeverity.Num();
	UpdatePersistentEcology();
	bLoggedPeakState = false;
	TotalAnimationCpuMilliseconds = 0.0;
	PeakAnimationCpuMilliseconds=0.;LastHandoffCpuMilliseconds=0.;
	AnimationUpdateCount = 0;
	const float Age=PresentationAge(World,Snapshot);
	if(Snapshot.GetEffectiveDurationSeconds()<=0.f||Age>=Snapshot.GetEffectiveDurationSeconds())
	{
		if(IsValid(PresentationActor))PresentationActor->ClearPresentation();
		ReleaseCompletedSnapshotFields();return;
	}
	if (!IsValid(PresentationActor))
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		PresentationActor = World->SpawnActor<AAftershockDisasterPresentationActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	}
	if (!PresentationActor) {ReleaseCompletedSnapshotFields();return;}
	PresentationActor->BeginSnapshot(Snapshot, *Terrain);
	UpdatePresentation();
	// Every animation reads absolute accepted event age. Replaying a timer many
	// times in one slow frame only resubmits identical geometry/material data.
	FTimerManagerTimerParameters Parameters;Parameters.bLoop=true;Parameters.bMaxOncePerFrame=true;
	World->GetTimerManager().SetTimer(AnimationTimer, this, &UDisasterPresentationSubsystem::UpdatePresentation, WeatherUpdateSeconds, Parameters);
}

void UDisasterPresentationSubsystem::UpdatePresentation()
{
	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!World || !Terrain || !IsValid(PresentationActor)) return;
	const float Age = PresentationAge(World,ActiveSnapshot);
	if (Age >= ActiveSnapshot.GetEffectiveDurationSeconds())
	{
		const double HandoffBegan=FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_DISASTER_PRESENTATION_TIMING revision=%d timerUpdates=%d avgGameThreadUpdateMs=%.3f measuredScope=TransformBatchAndMaterialUpdates_NoGpuTiming"),
			ActiveSnapshot.Revision, AnimationUpdateCount, AnimationUpdateCount > 0 ? TotalAnimationCpuMilliseconds / AnimationUpdateCount : 0.0);
		PresentationActor->ClearPresentation();
		ReleaseCompletedSnapshotFields();
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_DISASTER_RENDER_RELEASE revision=%d componentHeldTransientMeshBytes=%llu retainedSnapshotFieldBytes=%llu gcCompletionClaim=0 processWorkingSetClaim=0"),
			ActiveSnapshot.Revision,PresentationActor->GetRetainedTransientMeshBytes(),static_cast<uint64>(ActiveSnapshot.LocalSeverity.GetAllocatedSize()+ActiveSnapshot.LavaSeverity.GetAllocatedSize()+ActiveSnapshot.LavaArrivalFraction.GetAllocatedSize()+ActiveSnapshot.AshSeverity.GetAllocatedSize()+ActiveSnapshot.RiverFillBefore.GetAllocatedSize()+ActiveSnapshot.RiverFillAfter.GetAllocatedSize()+ActiveSnapshot.WaterDepthCm.GetAllocatedSize()+ActiveSnapshot.WaterSurfaceElevationCm.GetAllocatedSize()+ActiveSnapshot.WaterArrivalFraction.GetAllocatedSize()));
		UpdatePersistentEcology();
		World->GetTimerManager().ClearTimer(AnimationTimer);
		LastHandoffCpuMilliseconds=(FPlatformTime::Seconds()-HandoffBegan)*1000.;
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_DISASTER_PRESENTATION_COMPLETE revision=%d elapsed=%.2f sharedSnapshot=1"), ActiveSnapshot.Revision, Age);
		return;
	}
	const double UpdateStart = FPlatformTime::Seconds();
	PresentationActor->AnimateSnapshot(ActiveSnapshot, *Terrain, Age);
	const double UpdateMilliseconds=(FPlatformTime::Seconds()-UpdateStart)*1000.;
	TotalAnimationCpuMilliseconds += UpdateMilliseconds;
	PeakAnimationCpuMilliseconds=FMath::Max(PeakAnimationCpuMilliseconds,UpdateMilliseconds);
	++AnimationUpdateCount;
	if (!bLoggedPeakState && Age >= (ActiveSnapshot.Type==EDisasterType::Flood ? 10.f : 4.f))
	{
		bLoggedPeakState = true;
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_DISASTER_PRESENTATION_PEAK %s"), *DescribeCurrentVisuals());
	}
}

void UDisasterPresentationSubsystem::ReleaseCompletedSnapshotFields()
{
	// The simulation retains the canonical latest report for save/inspection.
	// This renderer no longer needs its duplicate full-resident event maps.
	ActiveSnapshot.LocalSeverity.Empty();ActiveSnapshot.LavaSeverity.Empty();ActiveSnapshot.AshSeverity.Empty();
	ActiveSnapshot.LavaArrivalFraction.Empty();
	ActiveSnapshot.RiverFillBefore.Empty();ActiveSnapshot.RiverFillAfter.Empty();
	ActiveSnapshot.WaterDepthCm.Empty();ActiveSnapshot.WaterSurfaceElevationCm.Empty();ActiveSnapshot.WaterArrivalFraction.Empty();
}

FString UDisasterPresentationSubsystem::DescribeCurrentVisuals() const
{
	const float Age = PresentationAge(GetWorld(),ActiveSnapshot);
	const uint64 RetainedSnapshotBytes=ActiveSnapshot.LocalSeverity.GetAllocatedSize()+ActiveSnapshot.LavaSeverity.GetAllocatedSize()+ActiveSnapshot.LavaArrivalFraction.GetAllocatedSize()+ActiveSnapshot.AshSeverity.GetAllocatedSize()+ActiveSnapshot.RiverFillBefore.GetAllocatedSize()+ActiveSnapshot.RiverFillAfter.GetAllocatedSize()+ActiveSnapshot.WaterDepthCm.GetAllocatedSize()+ActiveSnapshot.WaterSurfaceElevationCm.GetAllocatedSize()+ActiveSnapshot.WaterArrivalFraction.GetAllocatedSize();
	return FString::Printf(TEXT("revision=%d type=%d elapsed=%.2f duration=%.2f canonicalCells=%d retainedSnapshotBytes=%llu %s persistent={%s}"), ActiveSnapshot.Revision,
		static_cast<int32>(ActiveSnapshot.Type), Age, ActiveSnapshot.DurationSeconds, ActiveCanonicalCellCount,RetainedSnapshotBytes,
		IsValid(PresentationActor) ? *PresentationActor->DescribeCurrentVisuals() : TEXT("noRenderActor"),
		IsValid(EcologyStateActor) ? *EcologyStateActor->DescribeCurrentVisuals() : TEXT("notCreated"));
}

float UDisasterPresentationSubsystem::GetDisplayedFloodDepthCm(FIntPoint Coord) const
{
	if(const auto* D=GetWorld()?GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>():nullptr;D&&D->IsHydraulicModel2Active())
	{
		double Head=0.,Column=0.;FVector2D Velocity;
		const auto* T=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
		return D->TryGetPhysicalWaterSample(Coord,Head,Column,Velocity)&&T&&T->IsValidCoord(Coord)?FMath::Max(0.,Head-T->GetCell(Coord).Elevation):0.f;
	}
	return IsValid(PresentationActor) ? PresentationActor->GetDisplayedFloodDepthCm(Coord) : 0.f;
}

void UDisasterPresentationSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(AnimationTimer);
		World->GetTimerManager().ClearTimer(EcologyStateTimer);
		if (UDisasterSimulationSubsystem* Disaster = World->GetSubsystem<UDisasterSimulationSubsystem>())
		{
			Disaster->OnDisasterVisualSnapshotChanged.Remove(SnapshotDelegateHandle);
			Disaster->OnDisasterCompleted.RemoveDynamic(this,&UDisasterPresentationSubsystem::HandleDisasterCompleted);
		}
		if (UCityPresentationSubsystem* City = World->GetSubsystem<UCityPresentationSubsystem>()) City->OnPresentationRebuilt.Remove(CityRebuiltDelegateHandle);
	}
	if (IsValid(PresentationActor)) { PresentationActor->ClearPresentation(); PresentationActor->Destroy(); }
	PresentationActor = nullptr;
	if (IsValid(EcologyStateActor)) EcologyStateActor->Destroy();
	EcologyStateActor = nullptr;
	ReleaseCompletedSnapshotFields();
	Super::Deinitialize();
}
