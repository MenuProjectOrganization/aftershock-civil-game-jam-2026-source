#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

struct FBuildingInstance;
class UGeometryCollection;

/** Presentation-only GC selection. New 10m lots use geometry with scale baked
 * into vertices/collision, so Chaos never reconstructs nonuniform actor scale
 * through its rotated mass frame. The five canonical assets remain unchanged. */
namespace AftershockScaledChaos
{
	JAMPROJECT_API FSoftObjectPath GetScaledAssetPath(const FBuildingInstance& Building);
	JAMPROJECT_API bool IsAllowedFracturedAssetPath(const FBuildingInstance& Building,const FSoftObjectPath& Path);
	/** Pure preflight: no asset load and no world mutation. Empty means legacy canonical. */
	JAMPROJECT_API bool IsValidSavedFracturedAssetReference(const FBuildingInstance& Building,const FSoftObjectPath& Path,
		const FTransform& ActorTransform,FText& OutError);
	JAMPROJECT_API UGeometryCollection* ResolveSavedFracturedAsset(const FBuildingInstance& Building,const FSoftObjectPath& Path,FText& OutError);
	/** Removes scale only after the exact 10m baked asset is present and selected. */
	JAMPROJECT_API UGeometryCollection* ResolveFracturedAsset(const FBuildingInstance& Building,float CellSize,
		FTransform& InOutVisualTransform,FText& OutError);
#if WITH_EDITOR
	/** Explicit authoring writes only /Game/Buildings/Fractured/Scaled. Read-only
 	 * mode validates every allowed catalog floor without changing packages. */
	JAMPROJECT_API bool AuthorScaledChaosAssets(bool bWriteAssets,int32& OutValidated,FString& OutError);
#endif
}
