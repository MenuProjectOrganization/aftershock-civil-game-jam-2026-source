#include "Aftershock/Public/AftershockScaledChaos.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/AftershockBuildingCatalog.h"
#include "Engine/StaticMesh.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionObject.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Chaos/ImplicitObject.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/GeometryCollectionClusteringUtility.h"
#include "GeometryCollection/GeometryCollectionConvexUtility.h"
#include "GeometryCollection/GeometryCollectionEngineConversion.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#endif

namespace AftershockScaledChaos
{
	namespace
	{
		const TCHAR* TypeName(EBuildingType Type)
		{
			switch(Type)
			{
			case EBuildingType::House:return TEXT("House");
			case EBuildingType::Apartment:return TEXT("Apartment");
			case EBuildingType::Office:return TEXT("Office");
			case EBuildingType::Factory:return TEXT("Factory");
			case EBuildingType::Hospital:return TEXT("Hospital");
			default:return nullptr;
			}
		}

		bool IsCanonicalBuilding(const FBuildingInstance& Building)
		{
			return Building.BuildingData && TypeName(Building.BuildingData->Type)
				&& Building.CurrentFloors>=Building.BuildingData->FloorsMin && Building.CurrentFloors<=Building.BuildingData->FloorsMax
				&& Building.BuildingData->MeshFractured.ToSoftObjectPath()==UBuildingManagerSubsystem::GetDefaultFracturedMesh(Building.BuildingData->Type).ToSoftObjectPath();
		}

		bool ExpectedTenMetreScale(const FBuildingInstance& Building,FVector& OutScale)
		{
			if (!IsCanonicalBuilding(Building)) return false;
			UStaticMesh* Mesh=Building.BuildingData->MeshIntact.LoadSynchronous();
			if (!Mesh) return false;
			const FBox Bounds=Mesh->GetBoundingBox();
			if (!Bounds.IsValid || Bounds.Min.Z>=0 || Bounds.Max.Z<=0 || Bounds.GetSize().GetMin()<=0) return false;
			// Same published visible-height/760cm-lot convention as the intact
			// presentation helper; runtime refuses selection if that helper changes.
			float Height=0;
			switch(Building.BuildingData->Type)
			{
			case EBuildingType::House:Height=Building.CurrentFloors*300.f+100.f;break;
			case EBuildingType::Apartment:case EBuildingType::Office:Height=Building.CurrentFloors*310.f+60.f;break;
			case EBuildingType::Factory:Height=Building.CurrentFloors*450.f;break;
			case EBuildingType::Hospital:Height=Building.CurrentFloors*330.f+60.f;break;
			default:return false;
			}
			const float XY=760.f/FMath::Max(Bounds.GetSize().X,Bounds.GetSize().Y);
			OutScale=FVector(XY,XY,Height/Bounds.Max.Z);
			return !OutScale.ContainsNaN() && OutScale.GetMin()>0;
		}
	}

	FSoftObjectPath GetScaledAssetPath(const FBuildingInstance& Building)
	{
		if (!IsCanonicalBuilding(Building)) return FSoftObjectPath();
		const FString Name=FString::Printf(TEXT("GC_%s_F%02d_10m"),TypeName(Building.BuildingData->Type),Building.CurrentFloors);
		return FSoftObjectPath(TEXT("/Game/Buildings/Fractured/Scaled/")+Name+TEXT(".")+Name);
	}

	bool IsAllowedFracturedAssetPath(const FBuildingInstance& Building,const FSoftObjectPath& Path)
	{
		if (!IsCanonicalBuilding(Building)) return false;
		return Path.IsNull() || Path==Building.BuildingData->MeshFractured.ToSoftObjectPath() || Path==GetScaledAssetPath(Building);
	}

	bool IsValidSavedFracturedAssetReference(const FBuildingInstance& Building,const FSoftObjectPath& Path,
		const FTransform& ActorTransform,FText& OutError)
	{
		if (!ActorTransform.IsValid() || !IsAllowedFracturedAssetPath(Building,Path)
			|| (!Path.IsNull() && Path==GetScaledAssetPath(Building) && !ActorTransform.GetScale3D().Equals(FVector::OneVector,.0001)))
		{
			OutError=NSLOCTEXT("Aftershock","InvalidRubbleAssetReference","Enkaz modeli yapı türü, kat sayısı veya kayıt ölçeğiyle uyuşmuyor.");
			return false;
		}
		return true;
	}

	UGeometryCollection* ResolveSavedFracturedAsset(const FBuildingInstance& Building,const FSoftObjectPath& Path,FText& OutError)
	{
		if (!IsAllowedFracturedAssetPath(Building,Path))
		{
			OutError=NSLOCTEXT("Aftershock","UnapprovedRubbleAsset","Enkaz modeli bu yapının onaylı modellerinden biri değil.");return nullptr;
		}
		const FSoftObjectPath Resolved=Path.IsNull()?Building.BuildingData->MeshFractured.ToSoftObjectPath():Path;
		UGeometryCollection* Asset=Cast<UGeometryCollection>(Resolved.TryLoad());
		if (!Asset) OutError=NSLOCTEXT("Aftershock","MissingBakedRubbleAsset","Enkaz modeli yüklenemedi; kayıt değiştirilmedi.");
		return Asset;
	}

	UGeometryCollection* ResolveFracturedAsset(const FBuildingInstance& Building,float CellSize,FTransform& InOutVisualTransform,FText& OutError)
	{
		if (!FMath::IsNearlyEqual(CellSize,1000.f,.01f))
			return ResolveSavedFracturedAsset(Building,FSoftObjectPath(),OutError);
		FVector ExpectedScale;
		if (!InOutVisualTransform.IsValid() || !ExpectedTenMetreScale(Building,ExpectedScale)
			|| !InOutVisualTransform.GetScale3D().Equals(ExpectedScale,.0001))
		{
			OutError=NSLOCTEXT("Aftershock","BakedRubbleScaleMismatch","Yapı görseli önceden hazırlanmış enkaz ölçeğiyle uyuşmuyor.");return nullptr;
		}
		UGeometryCollection* Asset=ResolveSavedFracturedAsset(Building,GetScaledAssetPath(Building),OutError);
		if (Asset) InOutVisualTransform.RemoveScaling();
		return Asset;
	}

#if WITH_EDITOR
	namespace
	{
		bool IsExactlyZeroScaledTriangle(const FGeometryCollection& Source,const TArray<FTransform>& Global,const FVector& Scale,int32 Face)
		{
			if(!Source.Indices.IsValidIndex(Face))return false;const FIntVector Tri=Source.Indices[Face];FVector P[3];
			for(int32 C=0;C<3;++C){const int32 V=Tri[C];if(!Source.Vertex.IsValidIndex(V)||!Source.BoneMap.IsValidIndex(V)||!Global.IsValidIndex(Source.BoneMap[V]))return false;P[C]=Global[Source.BoneMap[V]].TransformPosition(FVector(Source.Vertex[V]))*Scale;if(P[C].ContainsNaN())return false;}
			// Exact zero, not a small-area threshold. A nonzero source surface may
			// never be hidden to make a render/collision audit pass.
			return FVector::CrossProduct(P[1]-P[0],P[2]-P[0]).SizeSquared()==0.;
		}

		bool EquivalentScaledGeometry(const FGeometryCollection& Source,const FGeometryCollection& Baked,const FVector& Scale,
			int32& OutLeaves,double& OutMaxError,FString& OutError)
		{
			OutLeaves=0;OutMaxError=0.;
			if (Source.Transform.Num()!=Baked.Transform.Num() || Source.Vertex.Num()!=Baked.Vertex.Num() || Source.Indices.Num()!=Baked.Indices.Num()
				|| Source.TransformIndex.Num()!=Baked.TransformIndex.Num()) { OutError=TEXT("topology count mismatch");return false; }
			TArray<FTransform> SourceGlobal,BakedGlobal;
			GeometryCollectionAlgo::GlobalMatrices(Source.Transform,Source.Parent,SourceGlobal);
			GeometryCollectionAlgo::GlobalMatrices(Baked.Transform,Baked.Parent,BakedGlobal);
			for (int32 I=0;I<Source.Transform.Num();++I)
			{
				if (Source.Parent[I]!=Baked.Parent[I] || Source.Children[I].Num()!=Baked.Children[I].Num() || !Source.Children[I].Includes(Baked.Children[I]) || Source.SimulationType[I]!=Baked.SimulationType[I]
					|| Source.TransformToGeometryIndex[I]!=Baked.TransformToGeometryIndex[I] || !BakedGlobal[I].GetScale3D().Equals(FVector::OneVector,.0001))
				{ OutError=TEXT("bone ID/order/hierarchy or unit-scale invariant failed");return false; }
				OutLeaves+=Baked.SimulationType[I]==FGeometryCollection::FST_Rigid && Baked.Children[I].IsEmpty() ? 1:0;
			}
			for (int32 V=0;V<Source.Vertex.Num();++V)
			{
				const int32 Bone=Source.BoneMap[V];
				if (Bone!=Baked.BoneMap[V] || !SourceGlobal.IsValidIndex(Bone) || Baked.Normal[V].ContainsNaN()
					|| FMath::Abs(Baked.Normal[V].SizeSquared()-1.f)>.02f) { OutError=FString::Printf(TEXT("vertex bone/normal invariant failed vertex=%d sourceBone=%d bakedBone=%d sourceNormal=%s bakedNormal=%s sourceNormalSquared=%.7f bakedNormalSquared=%.7f"),V,Bone,Baked.BoneMap[V],*Source.Normal[V].ToString(),*Baked.Normal[V].ToString(),Source.Normal[V].SizeSquared(),Baked.Normal[V].SizeSquared());return false; }
				if(Baked.TangentU[V].ContainsNaN()||Baked.TangentV[V].ContainsNaN()
					||FMath::Abs(Baked.TangentU[V].SizeSquared()-1.f)>.02f||FMath::Abs(Baked.TangentV[V].SizeSquared()-1.f)>.02f
					||FMath::Abs(FVector3f::DotProduct(Baked.Normal[V],Baked.TangentU[V]))>.02f
					||FMath::Abs(FVector3f::DotProduct(Baked.Normal[V],Baked.TangentV[V]))>.02f
					||FMath::Abs(FVector3f::DotProduct(Baked.TangentU[V],Baked.TangentV[V]))>.02f)
				{OutError=FString::Printf(TEXT("vertex tangent orthonormal invariant failed vertex=%d"),V);return false;}
				const FVector Expected=SourceGlobal[Bone].TransformPosition(FVector(Source.Vertex[V]))*Scale;
				const FVector Actual=BakedGlobal[Bone].TransformPosition(FVector(Baked.Vertex[V]));
				OutMaxError=FMath::Max(OutMaxError,FVector::Dist(Expected,Actual));
			}
			for (int32 F=0;F<Source.Indices.Num();++F)
			{
				const bool AllowedZeroAreaCleanup=Source.Visible[F]&&!Baked.Visible[F]&&IsExactlyZeroScaledTriangle(Source,SourceGlobal,Scale,F);
				if (Source.Indices[F]!=Baked.Indices[F] || Source.MaterialID[F]!=Baked.MaterialID[F] || (Source.Visible[F]!=Baked.Visible[F]&&!AllowedZeroAreaCleanup))
				{ OutError=FString::Printf(TEXT("face order/material/visibility mismatch; hiding a nonzero-area source face is forbidden face=%d"),F);return false; }
			}
			if (OutLeaves<20 || OutLeaves>40 || OutMaxError>.05) { OutError=TEXT("leaf budget or exact scaled-vertex equivalence failed");return false; }
			return true;
		}

		bool HasCompleteCookedCollision(const FGeometryCollection& Collection)
		{
			const auto* Implicits=Collection.FindAttribute<Chaos::FImplicitObjectPtr>(TEXT("Implicits"),FGeometryCollection::TransformGroup);
			for (int32 I=0;I<Collection.Transform.Num();++I)
				if (Collection.SimulationType[I]==FGeometryCollection::FST_Rigid && Collection.Children[I].IsEmpty()
					&& (!Implicits || !Implicits->IsValidIndex(I) || !(*Implicits)[I])) return false;
			return true;
		}

		bool BakeShadingBasis(const FGeometryCollection& Source,FGeometryCollection& Baked,const TArray<FTransform>& SourceGlobal,
			const TArray<FTransform>& TargetGlobal,const FVector& Scale,FString& OutError)
		{
			struct FFaceBasis {int32 Bone;FVector Centre,Normal,Tangent,Bitangent;};
			TArray<FFaceBasis> Faces;
			TArray<FVector> Normals,Tangents,Bitangents,Positions;Normals.Init(FVector::ZeroVector,Source.Vertex.Num());Tangents=Normals;Bitangents=Normals;Positions=Normals;
			TArray<int32> VisibleIncident;VisibleIncident.Init(0,Source.Vertex.Num());
			TArray<int32> IncidentBasis;IncidentBasis.Init(INDEX_NONE,Source.Vertex.Num());TSet<int32> RealFaceBones;
			const auto* UV0=Source.FindUVLayer(0);
			for(int32 V=0;V<Source.Vertex.Num();++V)Positions[V]=TargetGlobal[Source.BoneMap[V]].TransformPosition(FVector(Baked.Vertex[V]));
			for(int32 F=0;F<Source.Indices.Num();++F)
			{
				const FIntVector Tri=Source.Indices[F];
				if(!Positions.IsValidIndex(Tri.X)||!Positions.IsValidIndex(Tri.Y)||!Positions.IsValidIndex(Tri.Z)){OutError=TEXT("invalid shading triangle");return false;}
				const int32 Vertices[]={Tri.X,Tri.Y,Tri.Z};for(int32 V:Vertices)VisibleIncident[V]+=Baked.Visible[F]?1:0;
				const FVector E1=Positions[Tri.Y]-Positions[Tri.X],E2=Positions[Tri.Z]-Positions[Tri.X];const FVector Cross=FVector::CrossProduct(E1,E2);
				if(Cross.ContainsNaN()||Cross.SizeSquared()==0.)continue;
				const double AreaWeight=Cross.Size();FFaceBasis Face;Face.Bone=Source.BoneMap[Tri.X];Face.Centre=(Positions[Tri.X]+Positions[Tri.Y]+Positions[Tri.Z])/3.;Face.Normal=Cross/AreaWeight;
				// UV gradients preserve normal-map orientation when UV0 has area.
				// A collapsed UV triangle instead uses its actual geometric edge.
				Face.Tangent=E1/E1.Size();Face.Bitangent=FVector::CrossProduct(Face.Normal,Face.Tangent);
				if(UV0&&UV0->IsValidIndex(Tri.X)&&UV0->IsValidIndex(Tri.Y)&&UV0->IsValidIndex(Tri.Z))
				{
					const FVector2f D1=(*UV0)[Tri.Y]-(*UV0)[Tri.X],D2=(*UV0)[Tri.Z]-(*UV0)[Tri.X];const double Det=static_cast<double>(D1.X)*D2.Y-static_cast<double>(D1.Y)*D2.X;
					if(FMath::IsFinite(Det)&&FMath::Abs(Det)>1.e-12){Face.Tangent=((E1*D2.Y-E2*D1.Y)/Det).GetSafeNormal();Face.Bitangent=((E2*D1.X-E1*D2.X)/Det).GetSafeNormal();}
				}
				const int32 BasisIndex=Faces.Add(Face);RealFaceBones.Add(Face.Bone);for(int32 V:Vertices){if(IncidentBasis[V]==INDEX_NONE)IncidentBasis[V]=BasisIndex;Normals[V]+=Cross;Tangents[V]+=Face.Tangent*AreaWeight;Bitangents[V]+=Face.Bitangent*AreaWeight;}
			}
			if(Faces.IsEmpty()){OutError=TEXT("no nondegenerate triangles for a shading basis");return false;}
			for(int32 Bone=0;Bone<Source.SimulationType.Num();++Bone)if(Source.SimulationType[Bone]==FGeometryCollection::FST_Rigid&&Source.Children[Bone].IsEmpty()&&!RealFaceBones.Contains(Bone))
			{OutError=FString::Printf(TEXT("rigid shard has no real nondegenerate face bone=%d"),Bone);return false;}
			int32 RepairedNormals=0,RepairedTangents=0,NonRenderedOrphanRepairs=0;
			for(int32 V=0;V<Source.Vertex.Num();++V)
			{
				const int32 Bone=Source.BoneMap[V];
				const bool ValidSourceNormal=!Source.Normal[V].ContainsNaN()&&Source.Normal[V].SizeSquared()>1.e-12f;
				FVector Normal=ValidSourceNormal?(SourceGlobal[Bone].TransformVectorNoScale(FVector(Source.Normal[V]))/Scale).GetSafeNormal():Normals[V].GetSafeNormal();
				const FFaceBasis* Fallback=nullptr;
				const auto FindActualFace=[&]()
				{
					if(Fallback)return;double Best=MAX_dbl;
					for(const auto& Face:Faces){if(Face.Bone!=Bone)continue;const double Distance=FVector::DistSquared(Positions[V],Face.Centre);if(Distance<Best){Fallback=&Face;Best=Distance;}}
				};
				if(Normal.ContainsNaN()||Normal.IsNearlyZero())
				{
					FindActualFace();
					// Never assign an arbitrary axis. Vertices with cancelling face
					// normals use a real incident surface; entirely non-rendered orphan
					// vertices use the nearest actual face of the SAME bone only.
					if(VisibleIncident[V]>0&&Normals[V].IsNearlyZero())
					{
						if(IncidentBasis[V]==INDEX_NONE){OutError=FString::Printf(TEXT("visible vertex has only degenerate triangles vertex=%d bone=%d"),V,Bone);return false;}Normal=Faces[IncidentBasis[V]].Normal;
					}
					else {if(!Fallback){OutError=FString::Printf(TEXT("no same-bone real surface for orphan vertex=%d bone=%d"),V,Bone);return false;}Normal=Fallback->Normal;++NonRenderedOrphanRepairs;}
				}
				if(!ValidSourceNormal)++RepairedNormals;
				FVector Tangent=Source.TangentU[V].ContainsNaN()?FVector::ZeroVector:SourceGlobal[Bone].TransformVectorNoScale(FVector(Source.TangentU[V]))*Scale;
				Tangent=(Tangent-Normal*FVector::DotProduct(Tangent,Normal)).GetSafeNormal();
				const bool ValidTangents=!Source.TangentV[V].ContainsNaN()&&Source.TangentV[V].SizeSquared()>1.e-12f&&!Tangent.IsNearlyZero();
				double Handedness=FVector3f::DotProduct(FVector3f::CrossProduct(Source.TangentU[V],Source.TangentV[V]),Source.Normal[V])>=0?1.:-1.;
				if(!ValidTangents||!ValidSourceNormal)
				{
					++RepairedTangents;Tangent=Tangents[V];Tangent=(Tangent-Normal*FVector::DotProduct(Tangent,Normal)).GetSafeNormal();
					if(Tangent.IsNearlyZero()){FindActualFace();if(!Fallback){OutError=TEXT("no same-bone triangle tangent");return false;}Tangent=(Fallback->Tangent-Normal*FVector::DotProduct(Fallback->Tangent,Normal)).GetSafeNormal();if(Tangent.IsNearlyZero())Tangent=(Fallback->Bitangent-Normal*FVector::DotProduct(Fallback->Bitangent,Normal)).GetSafeNormal();}
					if(Tangent.ContainsNaN()||Tangent.IsNearlyZero()){OutError=FString::Printf(TEXT("no actual triangle tangent basis vertex=%d"),V);return false;}
					FVector Reference=Bitangents[V];if(Reference.IsNearlyZero()){FindActualFace();if(!Fallback){OutError=TEXT("no same-bone triangle bitangent");return false;}Reference=Fallback->Bitangent;}Handedness=FVector::DotProduct(FVector::CrossProduct(Normal,Tangent),Reference)>=0?1.:-1.;
				}
				Baked.Normal[V]=FVector3f(TargetGlobal[Bone].InverseTransformVectorNoScale(Normal).GetSafeNormal());
				Baked.TangentU[V]=FVector3f(TargetGlobal[Bone].InverseTransformVectorNoScale(Tangent).GetSafeNormal());
				Baked.TangentV[V]=FVector3f(TargetGlobal[Bone].InverseTransformVectorNoScale(FVector::CrossProduct(Normal,Tangent)*Handedness).GetSafeNormal());
			}
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SCALED_CHAOS_SHADING_REPAIR sourceZeroOrNonfiniteNormals=%d repairedTangents=%d nonRenderedOrphanNormals=%d basis=ActualBakedTrianglesAndUV0 canonicalSourceMutated=0 arbitraryAxisFallback=0"),RepairedNormals,RepairedTangents,NonRenderedOrphanRepairs);
			return true;
		}

		bool BakeGeometry(const FGeometryCollection& Source,FGeometryCollection& Baked,const FVector& Scale,FString& OutError)
		{
			// Identity append preserves leaf IDs and source vertex/face ordering. The
			// engine's scaled append does not inverse-transpose normals or scale the
			// translation of parentless rigid bones, so apply the affine bake here.
			if (!FGeometryCollectionEngineConversion::AppendGeometryCollection(&Source,0,FTransform::Identity,&Baked,true)) return false;
			TArray<FTransform> SourceGlobal,TargetGlobal;
			GeometryCollectionAlgo::GlobalMatrices(Source.Transform,Source.Parent,SourceGlobal);
			TargetGlobal=SourceGlobal;
			for (int32 I=0;I<SourceGlobal.Num();++I)
			{
				if (!SourceGlobal[I].GetScale3D().Equals(FVector::OneVector,.0001)) { OutError=TEXT("source rest bone contains unsupported scale");return false; }
				TargetGlobal[I].SetTranslation(SourceGlobal[I].GetTranslation()*Scale);
				TargetGlobal[I].SetScale3D(FVector::OneVector);
			}
			for (int32 I=0;I<Baked.Transform.Num();++I)
				Baked.Transform[I]=FTransform3f(Source.Parent[I]==INDEX_NONE ? TargetGlobal[I] : TargetGlobal[I].GetRelativeTransform(TargetGlobal[Source.Parent[I]]));
			for (int32 V=0;V<Source.Vertex.Num();++V)
			{
				const int32 Bone=Source.BoneMap[V];
				if (!TargetGlobal.IsValidIndex(Bone)) { OutError=TEXT("invalid source vertex bone");return false; }
				Baked.Vertex[V]=FVector3f(TargetGlobal[Bone].InverseTransformPosition(SourceGlobal[Bone].TransformPosition(FVector(Source.Vertex[V]))*Scale));
			}
			int32 HiddenZeroAreaFaces=0;
			for(int32 F=0;F<Source.Indices.Num();++F)if(Source.Visible[F]&&IsExactlyZeroScaledTriangle(Source,SourceGlobal,Scale,F)){Baked.Visible[F]=false;++HiddenZeroAreaFaces;}
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SCALED_CHAOS_ZERO_AREA_CLEANUP hiddenFaces=%d proof=ExactZeroSquaredCrossOfScaledSourceTriangleArea idsReordered=0 verticesMovedForCleanup=0 canonicalSourceMutated=0"),HiddenZeroAreaFaces);
			if(!BakeShadingBasis(Source,Baked,SourceGlobal,TargetGlobal,Scale,OutError))return false;
			for (int32 G=0;G<Baked.TransformIndex.Num();++G)
			{
				FBox Bounds(ForceInit);FVector Centre=FVector::ZeroVector;
				const int32 Start=Baked.VertexStart[G],Count=Baked.VertexCount[G];
				if (Start<0 || Count<=0 || Start>Baked.Vertex.Num() || Count>Baked.Vertex.Num()-Start) { OutError=TEXT("invalid geometry range");return false; }
				for (int32 V=Start;V<Start+Count;++V) { Bounds+=FVector(Baked.Vertex[V]);Centre+=FVector(Baked.Vertex[V]); }
				Centre/=Count;float Inner=MAX_flt,Outer=0;
				for (int32 V=Start;V<Start+Count;++V) { const float D=FVector::Dist(FVector(Baked.Vertex[V]),Centre);Inner=FMath::Min(Inner,D);Outer=FMath::Max(Outer,D); }
				Baked.BoundingBox[G]=Bounds;Baked.InnerRadius[G]=Inner;Baked.OuterRadius[G]=Outer;
			}
			if (Source.HasAttribute(TEXT("GUID"),FGeometryCollection::TransformGroup))
			{
				if (!Baked.HasAttribute(TEXT("GUID"),FGeometryCollection::TransformGroup)) Baked.AddAttribute<FGuid>(TEXT("GUID"),FGeometryCollection::TransformGroup);
				Baked.CopyAttribute(Source,TEXT("GUID"),FGeometryCollection::TransformGroup);
			}
			return true;
		}
	}

	bool AuthorScaledChaosAssets(bool bWriteAssets,int32& OutValidated,FString& OutError)
	{
		OutValidated=0;OutError.Reset();FString Rows;
		const EBuildingType Types[]={EBuildingType::House,EBuildingType::Apartment,EBuildingType::Office,EBuildingType::Factory,EBuildingType::Hospital};
		for (EBuildingType Type:Types)
		{
			TStrongObjectPtr<UBuildingData> Data(FAftershockBuildingCatalog::CreateData(GetTransientPackage(),Type));
			UGeometryCollection* Source=Data.IsValid()?Data->MeshFractured.LoadSynchronous():nullptr;
			const auto SourceCollection=Source?Source->GetGeometryCollection():nullptr;
			if (!SourceCollection) { OutError=TEXT("canonical source GC missing");return false; }
			for (int32 Floors=Data->FloorsMin;Floors<=Data->FloorsMax;++Floors)
			{
				FBuildingInstance Building;Building.BuildingData=Data.Get();Building.CurrentFloors=Floors;
				FVector Scale;if (!ExpectedTenMetreScale(Building,Scale)) { OutError=TEXT("invalid canonical mesh/10m scale");return false; }
				const FSoftObjectPath Path=GetScaledAssetPath(Building);
				UGeometryCollection* Asset=LoadObject<UGeometryCollection>(nullptr,*Path.ToString(),nullptr,LOAD_NoWarn|LOAD_Quiet);
				bool bCreated=false;
				TStrongObjectPtr<UGeometryCollection> Candidate;
				if (!Asset && bWriteAssets)
				{
					Candidate.Reset(NewObject<UGeometryCollection>(GetTransientPackage()));Asset=Candidate.Get();
					Asset->bManualDataCreate=true;Asset->Materials=Source->Materials;
					const auto Baked=Asset->GetGeometryCollection();
					if (!BakeGeometry(*SourceCollection,*Baked,Scale,OutError)) return false;
					Asset->GeometrySource=Source->GeometrySource;
					for (auto& GeometrySource:Asset->GeometrySource) GeometrySource.LocalTransform=FTransform(FQuat::Identity,FVector::ZeroVector,Scale);
					Asset->EnableClustering=false;Asset->DamageThreshold=Source->DamageThreshold;Asset->EnableNanite=false;Asset->bImportCollisionFromSource=false;
					Asset->bRemoveOnMaxSleep=false;Asset->bScaleOnRemoval=false;Asset->SizeSpecificData=Source->SizeSpecificData;
					FGeometryCollectionClusteringUtility::UpdateHierarchyLevelOfChildren(Baked.Get(),INDEX_NONE);
					Baked->ReindexMaterials();Asset->InitializeMaterials(false);
					FGeometryCollectionConvexUtility::CreateNonOverlappingConvexHullData(Baked.Get(),.3,2.,.5,EConvexOverlapRemoval::OnlyClusters);
					Asset->InvalidateCollection();Asset->CreateSimulationData();Asset->RebuildRenderData();
					bCreated=true;
				}
				const auto Baked=Asset?Asset->GetGeometryCollection():nullptr;
				int32 Leaves=0;double MaxError=0.;
				if (!Baked || !EquivalentScaledGeometry(*SourceCollection,*Baked,Scale,Leaves,MaxError,OutError)
					|| !HasCompleteCookedCollision(*Baked) || !Asset->HasMeshData())
				{
					OutError=Path.ToString()+TEXT(": ")+OutError+TEXT("; missing asset, cooked implicit or native render data is also fatal");return false;
				}
				if (bCreated)
				{
					const FString PackageName=Path.GetLongPackageName();UPackage* Package=CreatePackage(*PackageName);
					if (!Asset->Rename(*Path.GetAssetName(),Package,REN_DontCreateRedirectors|REN_NonTransactional)) { OutError=TEXT("asset rename failed");return false; }
					Asset->ClearFlags(RF_Transient);Asset->SetFlags(RF_Public|RF_Standalone);Asset->bManualDataCreate=false;
					FAssetRegistryModule::AssetCreated(Asset);Package->MarkPackageDirty();
					const FString Filename=FPackageName::LongPackageNameToFilename(PackageName,FPackageName::GetAssetPackageExtension());
					IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename),true);FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;Args.SaveFlags=SAVE_NoError;
					if (!UPackage::SavePackage(Package,Asset,*Filename,Args)) { OutError=TEXT("package write failed");return false; }
				}
				++OutValidated;
				int32 HiddenZeroAreaFaces=0;for(int32 F=0;F<SourceCollection->Visible.Num();++F)HiddenZeroAreaFaces+=SourceCollection->Visible[F]&&!Baked->Visible[F]?1:0;
				Rows+=FString::Printf(TEXT("%s    {\"path\":\"%s\",\"source\":\"%s\",\"floors\":%d,\"leaves\":%d,\"vertices\":%d,\"max_scaled_vertex_error_cm\":%.6f,\"scale\":[%.7f,%.7f,%.7f],\"zero_area_faces_hidden\":%d,\"created\":%s}"),
					Rows.IsEmpty()?TEXT(""):TEXT(",\n"),*Path.ToString(),*Source->GetPathName(),Floors,Leaves,Baked->Vertex.Num(),MaxError,Scale.X,Scale.Y,Scale.Z,HiddenZeroAreaFaces,bCreated?TEXT("true"):TEXT("false"));
				UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SCALED_CHAOS_ASSET path=%s source=%s floors=%d rigidLeaves=%d vertices=%d maxScaledVertexErrorCm=%.6f nativeActorScale=1 cookedImplicitsComplete=1 renderData=1 created=%d"),
					*Path.ToString(),*Source->GetPathName(),Floors,Leaves,Baked->Vertex.Num(),MaxError,bCreated);
			}
		}
		const FString Directory=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("PresentationValidation"));IFileManager::Get().MakeDirectory(*Directory,true);
		const FString Report=FString::Printf(TEXT("{\n  \"schema_version\":1,\n  \"utc\":\"%s\",\n  \"validated\":%d,\n  \"authoring\":%s,\n  \"scope\":\"Editor-native scaled geometry/collision authoring; preserves canonical five assets, topology and leaf IDs. Derived-only exact-zero-area faces hidden; invalid shading repaired from actual triangles/UV0 and same-bone surfaces. Nonzero-area hiding and shards without real faces fail. Not runtime contact or GPU proof.\",\n  \"assets\":[\n%s\n  ]\n}\n"),*FDateTime::UtcNow().ToIso8601(),OutValidated,bWriteAssets?TEXT("true"):TEXT("false"),*Rows);
		if (!FFileHelper::SaveStringToFile(Report,*FPaths::Combine(Directory,TEXT("scaled_chaos_assets.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { OutError=TEXT("authoring report write failed");return false; }
		return OutValidated==27;
	}
#endif
}
