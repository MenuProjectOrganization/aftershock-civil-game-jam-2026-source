#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "MeshUVChannelInfo.h"

/**
 * Runtime fast mesh builds do not get the editor's UV-streaming-data rebuild.
 * Initialize metadata from the supplied geometry AFTER BuildFromMeshDescriptions,
 * before a component references the mesh. No retained CPU vertex buffer is needed.
 * Ordinary surfaces use UV0. Physical flood surfaces additionally carry two
 * float data channels; those contain no texture addresses.
 */
template <typename VertexType>
void InitializeAftershockRuntimeMeshUV(UStaticMesh& Mesh, const TArray<VertexType>& Vertices,
	const TArray<int32>& Indices, bool bUV0ContainsFlowDirections = false, int32 NumUVChannels=1)
{
	double WorldTriangleArea = 0.0;
	double UVTriangleArea = 0.0;
	int32 ValidTriangles = 0;
	if (!bUV0ContainsFlowDirections)
	{
		for (int32 I = 0; I + 2 < Indices.Num(); I += 3)
		{
			if (!Vertices.IsValidIndex(Indices[I]) || !Vertices.IsValidIndex(Indices[I + 1]) || !Vertices.IsValidIndex(Indices[I + 2])) continue;
			const VertexType& A = Vertices[Indices[I]];
			const VertexType& B = Vertices[Indices[I + 1]];
			const VertexType& C = Vertices[Indices[I + 2]];
			const double WorldArea = FVector::CrossProduct(B.Position - A.Position, C.Position - A.Position).Size() * 0.5;
			const double U1 = static_cast<double>(B.UV.X) - A.UV.X, V1 = static_cast<double>(B.UV.Y) - A.UV.Y;
			const double U2 = static_cast<double>(C.UV.X) - A.UV.X, V2 = static_cast<double>(C.UV.Y) - A.UV.Y;
			const double UVArea = FMath::Abs(U1 * V2 - V1 * U2) * 0.5;
			if (!FMath::IsFinite(WorldArea) || !FMath::IsFinite(UVArea) || WorldArea <= 1.e-12 || UVArea <= 1.e-12) continue;
			WorldTriangleArea += WorldArea;
			UVTriangleArea += UVArea;
			++ValidTriangles;
		}
	}
	// The flood material uses UV0 as a flow vector, not a texture coordinate.
	// A nominal1m/UV is valid streaming metadata for that no-texture shader and
	// also safely covers collapsed/degenerate UV mappings without NaN/infinity.
	constexpr float FallbackWorldUnitsPerUV = 100.f;
	const double Computed = UVTriangleArea > 1.e-12 ? FMath::Sqrt(WorldTriangleArea / UVTriangleArea) : 0.0;
	const bool bUsedFallback = bUV0ContainsFlowDirections || !FMath::IsFinite(Computed) || Computed <= 0.0;
	const float Density = bUsedFallback ? FallbackWorldUnitsPerUV : static_cast<float>(FMath::Clamp(Computed, 0.001, 10000000.0));
	for (FStaticMaterial& Slot : Mesh.GetStaticMaterials())
	{
		Slot.UVChannelData = FMeshUVChannelInfo(0.f);
		Slot.UVChannelData.bInitialized = true;
		Slot.UVChannelData.bOverrideDensities = true;
		Slot.UVChannelData.LocalUVDensities[0] = Density;
		for(int32 Channel=1;Channel<FMath::Min(NumUVChannels,4);++Channel)Slot.UVChannelData.LocalUVDensities[Channel]=FallbackWorldUnitsPerUV;
	}
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_RUNTIME_MESH_UV_STREAMING mesh=%s materialSlots=%d uvChannels=%d initialized=1 override=1 densityWorldUnitsPerUV=%.6f areaTriangles=%d worldArea=%.6f uvArea=%.9f fallback=%d flowVectorUV=%d metadataAfterFastBuild=1 textureStreamingDisabled=0"),
		*Mesh.GetPathName(), Mesh.GetStaticMaterials().Num(), NumUVChannels, Density, ValidTriangles, WorldTriangleArea, UVTriangleArea, bUsedFallback, bUV0ContainsFlowDirections);
}
