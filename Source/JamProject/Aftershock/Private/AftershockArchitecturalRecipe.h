#pragma once

// Deterministic visual recipe inside the existing saved building envelope.
#include "CoreMinimal.h"
#include "Aftershock/Public/BuildingData.h"
#include "Aftershock/Public/AftershockRegionEnvironmentProfile.h"

enum class EAftershockArchitecturalTier:uint8 { Street,City,Regional,Globe };
enum class EAftershockArchitecturalPart:uint8 { Mass,FacadeStrip,RoofFlat,RoofGable,RoofHip,RoofShed,Canopy,Door,Parapet,ParkPath,ParkBench,ParkGrass,ParkShrub,ParkConifer,ParkPalm,ParkBroadleaf };
enum class EAftershockArchitecturalFinish:uint8 { PlasterCladding,TimberCladding,BrickVeneer,MetalCladding,GlassScreen,RoofTile,RoofMetal,ConcreteCladding,ParkPaving,ParkNative };

/** The frame is derived from the actual saved canonical transform/bounds.
 * Units cm; origin is visible envelope bottom centre, axes retain saved yaw.
 * Never derives hazards, structural material, occupancy or a new parcel. */
struct FAftershockArchitecturalInput
{
	FName RegionID,FocusID;
	int32 TerrainSeed=1337,BuildingID=INDEX_NONE,Floors=1;
	FIntPoint Coord=FIntPoint::ZeroValue;
	EBuildingType Type=EBuildingType::House;
	EMaterialType StructuralMaterial=EMaterialType::Wood;
	EDamageState DamageState=EDamageState::Intact;
	FTransform Frame=FTransform::Identity;
	FVector EnvelopeCm=FVector::ZeroVector;
    float CanonicalCustomData[4]={0.f,0.f,1.f,1.f};
    // Read-only presentation descriptors from the existing regional profile and
    // nine actual local terrain samples. Not saved or used by park simulation.
    EAftershockRegionalBiome RegionalBiome=EAftershockRegionalBiome::TemperateUrban;
    bool bParkGroundNearlyLevel=false;
};

struct FAftershockArchitecturalStyle
{
	FName ID=TEXT("unresolved");
	EAftershockArchitecturalPart Roof=EAftershockArchitecturalPart::RoofFlat;
	EAftershockArchitecturalFinish Wall=EAftershockArchitecturalFinish::PlasterCladding;
	EAftershockArchitecturalFinish RoofFinish=EAftershockArchitecturalFinish::RoofTile;
	float WidthFraction=.90f,DepthFraction=.90f,RoofHeightCm=80.f;
	uint8 FacadeBays=3;
	bool bPorch=false,bCourtyard=false,bAllowWings=true;
};

/** Module asset contract: unit bounds [-.5,.5]XY/[0,1]Z, front=-Y.
 * Transform scales one authored normalized module to these declared dimensions.
 * This does NOT license arbitrary nonuniform scaling of imported prefab assets.
 * Native window/roof pieces need size-specific recipes validated at import. */
struct FAftershockArchitecturalPlacement
{
	EAftershockArchitecturalPart Part=EAftershockArchitecturalPart::Mass;
	EAftershockArchitecturalFinish Finish=EAftershockArchitecturalFinish::PlasterCladding;
	EAftershockArchitecturalFinish AccentFinish=EAftershockArchitecturalFinish::PlasterCladding;
	uint8 Variant=0;
	uint8 Bays=1;
	FTransform LocalTransform=FTransform::Identity;
	FBox LocalConservativeBounds=FBox(ForceInit);
};

struct FAftershockArchitecturalRecipe
{
	static constexpr int32 Version=1,MaximumPieces=24;
	FName StyleID;
	uint32 ShapeSeed=0,AppearanceSeed=0;
	FLinearColor SurfaceTint=FLinearColor::White;
	float FloorPitchCm=0.f;
	uint8 MaterialVariant=0,FacadeBays=3;
	TArray<FAftershockArchitecturalPlacement> Pieces;
	FString Rejection;
	bool IsReady() const {return Rejection.IsEmpty()&&!Pieces.IsEmpty();}
};

FAftershockArchitecturalStyle ResolveAftershockArchitecturalStyle(FName RegionID,FName FocusID);
bool SupportsAftershockArchitecturalType(EBuildingType Type);
/** Complete bounded pure plan; no UObject allocation, asset loads, geometry
 * construction, world mutation, random global state or per-frame updates. */
FAftershockArchitecturalRecipe BuildAftershockArchitecturalRecipe(const FAftershockArchitecturalInput& Input,EAftershockArchitecturalTier Tier,bool bNativeFacadeFamilyReady=false);
