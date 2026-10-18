#pragma once
#include "CoreMinimal.h"
class UTerrainGridSubsystem;
class UStaticMeshComponent;
struct FAftershockMonthlyClimate;

/** Ten existing economy turns sample one annual cycle; October is the initial scenario season. */
float GetAftershockRegionalVisualMonth(const UTerrainGridSubsystem& Terrain);
bool GetAftershockRegionalVisualClimate(const UTerrainGridSubsystem& Terrain,FAftershockMonthlyClimate& Out);
/** Uniform material parameters/textures only. No geometry rebuild, rainfall authority or parcel classification. */
bool ApplyAftershockRegionalSurfaceStyle(const UTerrainGridSubsystem& Terrain,UStaticMeshComponent* Component,int32 MaterialIndex=0);
/** Shared historical global albedo for V3 only; no coastline/height/coverage authority. */
bool ApplyAftershockGlobalRegionalColour(const UTerrainGridSubsystem& Terrain,UStaticMeshComponent* Component,int32 MaterialIndex=0);
