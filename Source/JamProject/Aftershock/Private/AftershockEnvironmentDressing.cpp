#include "AftershockEnvironmentDressing.h"

#include "AftershockTerrainSurface.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/AftershockRegionEnvironmentProfile.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Misc/PackageName.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/FileManager.h"

namespace
{
	constexpr int32 Variants=3,FamilyCount=5,EntryCount=Variants*FamilyCount,MaxScanSide=128;
	constexpr float ViewBucketCm=1000.f;
	uint32 PlantSeed(int32 Seed,FIntPoint Coord,uint32 Salt)
	{
		return HashCombine(HashCombine(GetTypeHash(Seed),GetTypeHash(Coord)),Salt);
	}
	void MixEnvironmentDressingHash(uint64& Hash,uint64 Value){Hash=(Hash^Value)*1099511628211ull;}
	int32 Bucket(double Value)
	{
		return FMath::FloorToInt(FMath::Clamp(Value/ViewBucketCm,-2000000000.,2000000000.));
	}
	bool IsDryFreeLand(const FGridCell& Cell)
	{
		return Cell.bIsPlayable&&!Cell.bIsOcean&&!Cell.bIsRiverbed&&!Cell.bHasRoad
			&&Cell.BridgeID==INDEX_NONE&&Cell.OccupyingBuildingID==INDEX_NONE&&!Cell.bIsEmergencyAssemblyArea
			&&!Cell.bVolcanicVent&&!Cell.bBurning&&Cell.LavaCoverage01<.03f&&FMath::IsFinite(Cell.Elevation);
	}
	struct FBiomePolicy
	{
		int32 TreeFamily=4; // Broadleaf. Never silently substitute pine for a missing tropical/deciduous tree.
		float TreeScale=1.f,GrassScale=1.f,ShrubScale=1.f;
	};
	FBiomePolicy BiomePolicy(EAftershockRegionalBiome Biome)
	{
		FBiomePolicy Result;
		switch(Biome)
		{
		case EAftershockRegionalBiome::HumidBlackSea:Result.TreeFamily=2;Result.TreeScale=.85f;break;
		case EAftershockRegionalBiome::Mediterranean:Result.TreeFamily=2;Result.TreeScale=.68f;Result.GrassScale=.55f;Result.ShrubScale=1.25f;break;
		case EAftershockRegionalBiome::VolcanicMediterranean:Result.TreeFamily=2;Result.TreeScale=.4f;Result.GrassScale=.35f;Result.ShrubScale=.7f;break;
		case EAftershockRegionalBiome::ContinentalTaiga:Result.TreeFamily=2;Result.TreeScale=.8f;Result.GrassScale=.55f;Result.ShrubScale=.6f;break;
		case EAftershockRegionalBiome::MonsoonHighland:Result.TreeFamily=2;Result.TreeScale=.55f;Result.GrassScale=.8f;break;
		case EAftershockRegionalBiome::TropicalMaritime:Result.TreeFamily=3;Result.TreeScale=.7f;Result.ShrubScale=1.15f;break;
		case EAftershockRegionalBiome::TropicalMonsoon:Result.TreeFamily=3;Result.TreeScale=.8f;Result.ShrubScale=1.2f;break;
		case EAftershockRegionalBiome::SemiAridSteppe:Result.TreeFamily=INDEX_NONE;Result.TreeScale=0.f;Result.GrassScale=.12f;Result.ShrubScale=.3f;break;
		case EAftershockRegionalBiome::SubpolarVolcanic:Result.TreeFamily=INDEX_NONE;Result.TreeScale=0.f;Result.GrassScale=.08f;Result.ShrubScale=.08f;break;
		case EAftershockRegionalBiome::TemperateUrban:Result.TreeScale=.45f;break;
		case EAftershockRegionalBiome::TemperateRainforest:Result.TreeScale=1.f;break;
		case EAftershockRegionalBiome::TemperateCoastal:Result.TreeScale=.65f;break;
		case EAftershockRegionalBiome::DeltaWetland:Result.TreeScale=.7f;Result.ShrubScale=1.15f;break;
		}
		return Result;
	}
	bool GroundAt(const UTerrainGridSubsystem& Terrain,FIntPoint Coord,FVector& Position,FVector& Normal)
	{
		// These canonical neighbour samples support both the interpolation and
		// its local normal. Missing/sea/river samples never manufacture a flat bank.
		for(int32 DY=-1;DY<=1;++DY)for(int32 DX=-1;DX<=1;++DX)
		{
			FGridCell Neighbor;
			if(!Terrain.TryGetCell(Coord+FIntPoint(DX,DY),Neighbor)||!Neighbor.bIsPlayable
				||Neighbor.bIsOcean||Neighbor.bIsRiverbed||!FMath::IsFinite(Neighbor.Elevation))return false;
		}
		const float GX=static_cast<float>((Position.X-Terrain.GridOrigin.X)/Terrain.CellSize-.5);
		const float GY=static_cast<float>((Position.Y-Terrain.GridOrigin.Y)/Terrain.CellSize-.5);
		Position.Z=SampleAftershockResidentElevation(Terrain,GX,GY);
		const float DX=SampleAftershockResidentElevation(Terrain,GX+.1f,GY)-SampleAftershockResidentElevation(Terrain,GX-.1f,GY);
		const float DY=SampleAftershockResidentElevation(Terrain,GX,GY+.1f)-SampleAftershockResidentElevation(Terrain,GX,GY-.1f);
		Normal=FVector(-DX,-DY,Terrain.CellSize*.2f).GetSafeNormal();
		return FMath::IsFinite(Position.Z)&&!Normal.ContainsNaN()&&Normal.Z>.01;
	}
	bool ClearFootprint(const UTerrainGridSubsystem& Terrain,FIntPoint Coord,const FVector& Position,float Radius,bool RejectAgricultural=false)
	{
		// A fixed bounded scan is safe only if it covers the full actual canopy
		// plus the30cm obstacle margin. Never silently truncate a large footprint.
		if(!FMath::IsFinite(Radius)||Radius<0.f||!FMath::IsFinite(Terrain.CellSize)||Terrain.CellSize<=0.f
			||!FMath::IsFinite(Position.X)||!FMath::IsFinite(Position.Y))return false;
		const double ReachCells=(static_cast<double>(Radius)+30.+Terrain.CellSize*.71)/Terrain.CellSize;
		if(!FMath::IsFinite(ReachCells)||ReachCells>3.)return false;
		const int32 Reach=FMath::Max(1,FMath::CeilToInt(ReachCells));
		for(int32 DY=-Reach;DY<=Reach;++DY)for(int32 DX=-Reach;DX<=Reach;++DX)
		{
			const FIntPoint At=Coord+FIntPoint(DX,DY);FGridCell Other;
			const bool Present=Terrain.TryGetCell(At,Other);
			if(Present&&IsDryFreeLand(Other)&&(!RejectAgricultural||!Other.bAgriculturalLand))continue;
			// Missing neighbour is a boundary blocker; never call GetCell/GridToWorld
			// for it or invent an elevation. Only its canonical XY cell box is needed.
			const FVector CellCenter(Terrain.GridOrigin.X+(static_cast<double>(At.X)+.5)*Terrain.CellSize,
				Terrain.GridOrigin.Y+(static_cast<double>(At.Y)+.5)*Terrain.CellSize,0.);
			const double DistanceX=FMath::Max(0.,FMath::Abs(Position.X-CellCenter.X)-Terrain.CellSize*.5);
			const double DistanceY=FMath::Max(0.,FMath::Abs(Position.Y-CellCenter.Y)-Terrain.CellSize*.5);
			if(DistanceX*DistanceX+DistanceY*DistanceY<FMath::Square(static_cast<double>(Radius+30.f)))return false;
		}
		return true;
	}
}

FAftershockEnvironmentDressing::FAftershockEnvironmentDressing(AActor& InOwner):Owner(&InOwner)
{
	Entries.SetNum(EntryCount);
	const TCHAR* Families[]={TEXT("GrassClump"),TEXT("Shrub"),TEXT("Conifer"),TEXT("Palm"),TEXT("Broadleaf")};
	TArray<FString> Missing;
	for(int32 Family=0;Family<FamilyCount;++Family)for(int32 Variant=0;Variant<Variants;++Variant)
	{
		FEntry& Entry=Entries[Family*Variants+Variant];Entry.Family=static_cast<EFamily>(Family);
		const FString Stem=FString::Printf(TEXT("SM_%s_%02d"),Families[Family],Variant+1);
		const FName Name(*FString::Printf(TEXT("Woodland_%s_%02d"),Families[Family],Variant+1));
		auto* Batch=FindObjectFast<UHierarchicalInstancedStaticMeshComponent>(&InOwner,Name);
		if(!Batch)
		{
			Batch=NewObject<UHierarchicalInstancedStaticMeshComponent>(&InOwner,Name,RF_Transient);
			InOwner.AddInstanceComponent(Batch);Batch->SetupAttachment(InOwner.GetRootComponent());
		}
		Entry.Component=Batch;
		Batch->SetMobility(EComponentMobility::Movable);Batch->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Batch->SetCanEverAffectNavigation(false);Batch->SetComponentTickEnabled(false);
		Batch->PrimaryComponentTick.bCanEverTick=false;Batch->bAutoRebuildTreeOnInstanceChanges=false;Batch->SetRemoveSwap();
		Batch->SetNumCustomDataFloats(2); //0canonical storm locality;1stable cosmetic phase.
		Batch->SetTextureForceResidentFlag(false);Batch->SetVisibility(false);
		const FString Package=FString(TEXT("/Game/Environment/Meshes/Realistic/"))+Stem;
		UStaticMesh* Mesh=FPackageName::DoesPackageExist(Package)?LoadObject<UStaticMesh>(nullptr,*(Package+TEXT(".")+Stem)):nullptr;
		if(Mesh&&Mesh->HasValidRenderData())
		{
			const FBox Bounds=Mesh->GetBoundingBox();const FVector Size=Bounds.GetSize();
			const bool BoundsValid=Bounds.IsValid&&Size.X>1.&&Size.Y>1.&&Size.Z>1.&&Size.Z<3000.
				&&FMath::Abs(Bounds.Min.Z)<3.&&Size.GetMax()<4000.;
			if(BoundsValid&&Mesh->GetStaticMaterials().Num()>0)
			{
				Entry.bReady=true;Entry.NativeBounds=Bounds;Batch->SetStaticMesh(Mesh);
				++Stats.ReadyMeshes;
				if(Mesh->GetNumLODs()<3)UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_ENVIRONMENT_LOD_INCOMPLETE mesh=%s lods=%d expected=3 forcedLODWillClamp=1 noPerformanceClaim=1"),*Package,Mesh->GetNumLODs());
			}
		}
		if(!Entry.bReady){++Stats.MissingMeshes;Missing.Add(Stem);}
		if(!Batch->IsRegistered())Batch->RegisterComponent();
	}
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_ENVIRONMENT_ASSETS ready=%d missing=%d missingNames=%s sharedHISMs=%d perPlantActors=0 perPlantTicks=0 perPlantMID=0 optionalBroadleaf=3 nativeCentimetres=1"),Stats.ReadyMeshes,Stats.MissingMeshes,*FString::Join(Missing,TEXT(",")),Entries.Num());
}

bool FAftershockEnvironmentDressing::FamilyReady(EFamily Family) const
{
	for(int32 Variant=0;Variant<Variants;++Variant)
	{
		const auto& Entry=Entries[static_cast<int32>(Family)*Variants+Variant];
		if(!Entry.bReady||!Entry.Component.IsValid())return false;
	}
	return true;
}
int32 FAftershockEnvironmentDressing::ReadyVariant(EFamily Family,uint32 Seed) const
{
	const int32 Base=static_cast<int32>(Family)*Variants;
	for(int32 Offset=0;Offset<Variants;++Offset)
	{
		const int32 Index=Base+static_cast<int32>((Seed+static_cast<uint32>(Offset))%Variants);
		if(Entries[Index].bReady&&Entries[Index].Component.IsValid())return Index;
	}
	return INDEX_NONE;
}

void FAftershockEnvironmentDressing::ApplyPresentationBudget()
{
	const AActor* Host=Owner.Get();if(!Host||!Host->GetWorld())return;
	const auto Quality=GetAftershockEnvironmentQuality(Host->GetWorld());
	const auto Camera=GetAftershockPresentationBudget(Host->GetWorld());
	const auto Budget=MakeAftershockEnvironmentBudget(Quality,Camera);
	const uint32 Signature=HashCombine(HashCombine(Quality.GetSignature(),GetTypeHash(Budget.bTreeShadows)),GetTypeHash(Budget.bShrubShadows));
	const int32 Tier=static_cast<int32>(Camera.Tier);
	if(Signature==LastAppliedQuality&&Tier==LastAppliedTier)return;
	LastAppliedQuality=Signature;LastAppliedTier=Tier;
	const bool Overview=Camera.Tier==EAftershockPresentationTier::Regional||Camera.Tier==EAftershockPresentationTier::Globe;
	for(FEntry& Entry:Entries)if(auto* Batch=Entry.Component.Get())
	{
		const bool Grass=Entry.Family==EFamily::Grass,Shrub=Entry.Family==EFamily::Shrub;
		const float End=Grass?Budget.GrassCullEndCm:Shrub?FMath::Min(Budget.TreeCullEndCm,30000.f):Budget.TreeCullEndCm;
		const float Start=Grass?Budget.GrassCullStartCm:End*.75f;
		Batch->SetVisibility(Entry.bReady&&!Overview&&Camera.bDrawDecorations&&End>0.f);
		Batch->SetCullDistances(FMath::RoundToInt(Start),FMath::RoundToInt(End));
		Batch->SetCastShadow(!Grass&&(Shrub?Budget.bShrubShadows:Budget.bTreeShadows));
		const int32 QualityLOD=Quality.Foliage==0?3:Quality.Foliage==1?2:0;
		const int32 Requested=FMath::Max(QualityLOD,Camera.PropLODFloor);
		const int32 MaxLOD=Batch->GetStaticMesh()?FMath::Max(1,Batch->GetStaticMesh()->GetNumLODs()):1;
		Batch->SetForcedLodModel(Requested>0?FMath::Clamp(Requested,1,MaxLOD):0);
		Batch->StreamingDistanceMultiplier=Camera.Tier==EAftershockPresentationTier::Street?1.f:.65f;
	}
}

bool FAftershockEnvironmentDressing::Refresh(const UTerrainGridSubsystem& Terrain,bool bForce)
{
	AActor* Host=Owner.Get();if(!Host||!Host->GetWorld()||!FMath::IsFinite(Terrain.CellSize)||Terrain.CellSize<=0.f)return false;
	ApplyPresentationBudget();
	const auto Quality=GetAftershockEnvironmentQuality(Host->GetWorld());const auto Camera=GetAftershockPresentationBudget(Host->GetWorld());
	const auto Budget=MakeAftershockEnvironmentBudget(Quality,Camera);
	const FIntRect Visible=GetAftershockDrawableCellBounds(Terrain);
	FVector View=Visible.IsEmpty()?Terrain.GridOrigin:Terrain.GridToWorld(FIntPoint((Visible.Min.X+Visible.Max.X)/2,(Visible.Min.Y+Visible.Max.Y)/2));
	if(const APlayerController* PC=Host->GetWorld()->GetFirstPlayerController())
		if(PC->PlayerCameraManager&&!PC->PlayerCameraManager->GetCameraLocation().ContainsNaN())View=PC->PlayerCameraManager->GetCameraLocation();
	const FIntVector ViewCell(Bucket(View.X),Bucket(View.Y),Bucket(View.Z));
	uint64 Signature=1469598103934665603ull;
	for(uint64 Value:{Terrain.GetTerrainRevision(),static_cast<uint64>(Quality.GetSignature()),static_cast<uint64>(GetTypeHash(Visible.Min)),
		static_cast<uint64>(GetTypeHash(Visible.Max)),static_cast<uint64>(GetTypeHash(ViewCell)),static_cast<uint64>(GetTypeHash(Terrain.TerrainSeed)),
		static_cast<uint64>(GetTypeHash(Terrain.GridOrigin)),static_cast<uint64>(GetTypeHash(Terrain.CellSize)),
		static_cast<uint64>(GetTypeHash(Terrain.GeographyIndex)),static_cast<uint64>(GetTypeHash(Terrain.GeographyFocusIndex)),static_cast<uint64>(Camera.Tier)})MixEnvironmentDressingHash(Signature,Value);
	bool SlotsConsistent=true;
	for(const FEntry& Entry:Entries)if(const auto* Batch=Entry.Component.Get())
		SlotsConsistent&=Batch->GetInstanceCount()==Entry.KeyByIndex.Num();
	if(!bForce&&bHasSignature&&Signature==LastSignature&&SlotsConsistent)return false;
	LastSignature=Signature;bHasSignature=true;
	const int32 Ready=Stats.ReadyMeshes,Missing=Stats.MissingMeshes;Stats={};Stats.ReadyMeshes=Ready;Stats.MissingMeshes=Missing;
	Stats.QualitySignature=Quality.GetSignature();
	Stats.bOverviewDisabled=!Camera.bDrawDecorations||Camera.Tier==EAftershockPresentationTier::Regional||Camera.Tier==EAftershockPresentationTier::Globe;
	const auto* Profile=TryGetAftershockRegionEnvironmentProfile(Terrain.GeographyIndex);
	const auto Biome=Profile?Profile->RegionalBiome:EAftershockRegionalBiome::TemperateUrban;
	const auto Policy=BiomePolicy(Biome);
	Stats.bRequiredTreeFamilyMissing=Policy.TreeFamily!=INDEX_NONE&&!FamilyReady(static_cast<EFamily>(Policy.TreeFamily));
	// Latest art policy: missing proper species is explicitly omitted. Never
	// bring back the Kenney green plinth or substitute a wrong-biome pine.
	Stats.bReplacesLegacyWoodland=true;
	TArray<TArray<FDesired>> Desired;Desired.SetNum(EntryCount);
    const auto* Buildings=Host->GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();
    const bool MediterraneanGarden=Biome==EAftershockRegionalBiome::Mediterranean||Biome==EAftershockRegionalBiome::VolcanicMediterranean;
    const int32 GardenTreeFamily=MediterraneanGarden?static_cast<int32>(EFamily::Broadleaf):Policy.TreeFamily;
    const bool GardenTreeReady=GardenTreeFamily!=INDEX_NONE&&FamilyReady(static_cast<EFamily>(GardenTreeFamily));
    int32 GardenEligibleCells=0,GardenTrees=0,GardenShrubs=0;
    int32 GardenRejectedBounds=0,GardenRejectedOverlap=0;
    TMap<int32,bool> ActiveGardenNeighborCache;
	if(!Stats.bOverviewDisabled&&!Visible.IsEmpty())
	{
		FIntPoint Focus=Terrain.WorldToGrid(View);
		Focus.X=FMath::Clamp(Focus.X,Visible.Min.X,Visible.Max.X-1);Focus.Y=FMath::Clamp(Focus.Y,Visible.Min.Y,Visible.Max.Y-1);
		const FIntPoint Min(FMath::Max(Visible.Min.X,Focus.X-MaxScanSide/2),FMath::Max(Visible.Min.Y,Focus.Y-MaxScanSide/2));
		const FIntPoint Max(FMath::Min(Visible.Max.X,Min.X+MaxScanSide),FMath::Min(Visible.Max.Y,Min.Y+MaxScanSide));
		Stats.ScanBounds=FIntRect(Min,Max);
		struct FCellCandidate{FIntPoint Coord;double DistanceSquared=0.;};
		TArray<FCellCandidate> Cells;Cells.Reserve(Stats.ScanBounds.Area());
		const double MaximumDistance=FMath::Max(Budget.TreeCullEndCm,Budget.GrassCullEndCm)+Terrain.CellSize;
		for(int32 Y=Min.Y;Y<Max.Y;++Y)for(int32 X=Min.X;X<Max.X;++X)
		{
			++Stats.ScannedCells;FGridCell Cell;if(!Terrain.TryGetCell(FIntPoint(X,Y),Cell)||!IsDryFreeLand(Cell))continue;
			const double Distance=FVector::DistSquared(View,Terrain.GridToWorld(Cell.Coords));
			if(Distance>MaximumDistance*MaximumDistance)continue;
			Cells.Add({Cell.Coords,Distance});++Stats.EligibleCells;
		}
		Cells.Sort([](const FCellCandidate& A,const FCellCandidate& B){return A.DistanceSquared!=B.DistanceSquared?A.DistanceSquared<B.DistanceSquared:A.Coord.Y!=B.Coord.Y?A.Coord.Y<B.Coord.Y:A.Coord.X<B.Coord.X;});
		for(const FCellCandidate& Candidate:Cells)
		{
			FGridCell Cell;if(!Terrain.TryGetCell(Candidate.Coord,Cell))continue;
			const float Fuel=FMath::Clamp(Cell.ForestFuel01,0.f,1.f),Moisture=FMath::Clamp(Cell.SoilMoisture01,0.f,1.f),Drought=FMath::Clamp(Cell.DroughtStress01,0.f,1.f);
            // At most3 new garden pieces; no all-world/all-plant pair scan.
            TArray<FVector,TInlineAllocator<3>> GardenCircles;
			const auto AddPlant=[&](EFamily Family,int32 Piece,float Chance,float EndDistance,bool AlignToGround,bool Garden=false)->bool
			{
				const uint32 Seed=PlantSeed(Terrain.TerrainSeed,Cell.Coords,static_cast<uint32>(Family)*0x9E3779B9u+static_cast<uint32>(Piece)*0x85EBCA6Bu+0x7051u);
				FRandomStream Random(static_cast<int32>(Seed));if(Random.FRand()>Chance)return false;
				const int32 EntryIndex=ReadyVariant(Family,Seed>>8);if(EntryIndex==INDEX_NONE)return false;
				const bool Tree=Family==EFamily::Conifer||Family==EFamily::Palm||Family==EFamily::Broadleaf;
				float Scale=Tree?Random.FRandRange(.94f,1.03f):Random.FRandRange(.88f,1.12f);
                if(Garden)
                {
                    // Original generic plants uniformly fitted to an authored
                    // garden scale, not a species/olive-orchard reconstruction.
                    const float NativeHeight=static_cast<float>(Entries[EntryIndex].NativeBounds.GetSize().Z);
                    if(!FMath::IsFinite(NativeHeight)||NativeHeight<=1.f)return false;
                    Scale=Random.FRandRange(Tree?200.f:45.f,Tree?400.f:100.f)/NativeHeight;
                }
                if(!FMath::IsFinite(Scale)||Scale<=0.f){if(Garden)++GardenRejectedBounds;return false;}
                const float Jitter=Garden?.22f:.39f;
				FVector Position=Terrain.GridToWorld(Cell.Coords)+FVector(Random.FRandRange(-Jitter,Jitter)*Terrain.CellSize,Random.FRandRange(-Jitter,Jitter)*Terrain.CellSize,0.);
				FVector Normal;
				if(!GroundAt(Terrain,Cell.Coords,Position,Normal)||Normal.Z<(Tree?.88:.78)){++Stats.RejectedGround;return false;}
				if(FVector::DistSquared(Position,View)>FMath::Square(static_cast<double>(EndDistance)))return false;
				const FVector Heading=FRotator(0.f,Random.FRandRange(0.f,360.f),0.f).Vector();
				const FQuat Rotation=AlignToGround?FRotationMatrix::MakeFromZX(Normal,Heading).ToQuat():Heading.Rotation().Quaternion();
				// Native canopy need not be symmetric about its trunk pivot. Bound the
				// actual yaw/ground-tilted box about that pivot, including lean/offset.
				const FBox Rotated=Entries[EntryIndex].NativeBounds.TransformBy(FTransform(Rotation,FVector::ZeroVector,FVector(Scale)));
				const double MaxX=FMath::Max(FMath::Abs(Rotated.Min.X),FMath::Abs(Rotated.Max.X));
				const double MaxY=FMath::Max(FMath::Abs(Rotated.Min.Y),FMath::Abs(Rotated.Max.Y));
				const float Radius=static_cast<float>(FMath::Sqrt(MaxX*MaxX+MaxY*MaxY));
                if(!Rotated.IsValid||Rotated.Min.ContainsNaN()||Rotated.Max.ContainsNaN()||!FMath::IsFinite(Radius))
                {if(Garden)++GardenRejectedBounds;return false;}
                if(Garden)
                {
                    // Keep the entire actual rotated canopy inside its free
                    // garden cell. Adjacent garden cells cannot interpenetrate.
                    const FVector Center=Terrain.GridToWorld(Cell.Coords);
                    if(FMath::Abs(Position.X-Center.X)+Radius+30.>Terrain.CellSize*.5
                        ||FMath::Abs(Position.Y-Center.Y)+Radius+30.>Terrain.CellSize*.5)
                    {++GardenRejectedBounds;return false;}
                    for(const FVector& Circle:GardenCircles)
                        if(FVector::DistSquaredXY(Position,Circle)<FMath::Square(static_cast<double>(Radius)+Circle.Z+20.))
                        {++GardenRejectedOverlap;return false;}
                }
				if(!ClearFootprint(Terrain,Cell.Coords,Position,Radius,Garden)){++Stats.RejectedFootprints;return false;}
				Position.Z-=Tree?2.f:Family==EFamily::Shrub?1.f:.5f;
				Desired[EntryIndex].Add({FIntVector(Cell.Coords.X,Cell.Coords.Y,Piece),FTransform(Rotation,Position,FVector(Scale)),Random.FRand()});
                if(Garden)GardenCircles.Add(FVector(Position.X,Position.Y,Radius));
				return true;
			};
            bool NearBuilding=false,NearRoad=false;
            if(!Cell.bAgriculturalLand&&Buildings)
            {
                const FIntPoint Steps[]={FIntPoint(1,0),FIntPoint(0,1),FIntPoint(-1,0),FIntPoint(0,-1)};
                // Four bounded canonical queries, no all-building scan/pairs.
                for(const FIntPoint Step:Steps)
                {
                    FGridCell Neighbor;if(!Terrain.TryGetCell(Cell.Coords+Step,Neighbor)||!Neighbor.bIsPlayable)continue;
                    NearRoad|=Neighbor.bHasRoad&&!Neighbor.bIsRiverbed&&!Neighbor.bIsOcean&&Neighbor.BridgeID==INDEX_NONE;
                    if(Neighbor.OccupyingBuildingID!=INDEX_NONE)
                    {
                        const int32 ID=Neighbor.OccupyingBuildingID;
                        if(const bool* Cached=ActiveGardenNeighborCache.Find(ID))NearBuilding|=*Cached;
                        else
                        {
                            FBuildingInstance Record;
                            const bool Active=Buildings->GetBuildingByID(ID,Record)&&Record.BuildingData&&Record.DamageState<EDamageState::Collapsed;
                            ActiveGardenNeighborCache.Add(ID,Active);NearBuilding|=Active;
                        }
                    }
                    if(NearBuilding&&NearRoad)break;
                }
            }
            const bool GardenCell=!Cell.bAgriculturalLand&&NearBuilding&&NearRoad;
            if(GardenCell)
            {
                ++GardenEligibleCells;
                // Piece keys100/110/111 are disjoint from existing forest0/1,
                // shrub0 and grass0..23; unrelated cells keep their exact RNG.
                if(GardenTreeReady&&Stats.GetTrees()<Budget.TreeInstanceCap)
                {
                    const auto Family=static_cast<EFamily>(GardenTreeFamily);
                    const float Chance=FMath::Clamp((MediterraneanGarden?.34f:.38f)*Budget.TreeDensity,0.f,1.f);
                    if(AddPlant(Family,100,Chance,Budget.TreeCullEndCm,false,true))
                    {++GardenTrees;if(Family==EFamily::Conifer)++Stats.Conifers;else if(Family==EFamily::Palm)++Stats.Palms;else ++Stats.Broadleaf;}
                }
                if(Stats.Shrubs<Budget.ShrubInstanceCap)
                {
                    const float Chance=FMath::Clamp(.42f*Policy.ShrubScale*Budget.ShrubDensity,0.f,.8f);
                    for(int32 Piece=110;Piece<112&&Stats.Shrubs<Budget.ShrubInstanceCap;++Piece)
                        if(AddPlant(EFamily::Shrub,Piece,Chance,FMath::Min(Budget.TreeCullEndCm,30000.f),true,true)){++GardenShrubs;++Stats.Shrubs;}
                }
            }
			if(!GardenCell&&Policy.TreeFamily!=INDEX_NONE&&!Cell.bAgriculturalLand&&!Stats.bRequiredTreeFamilyMissing&&Stats.GetTrees()<Budget.TreeInstanceCap)
			{
				const EFamily Family=static_cast<EFamily>(Policy.TreeFamily);
				const float Chance=(Family==EFamily::Palm?.18f+Fuel*.35f:FMath::Max(.012f,(Fuel-.25f)*1.2f))*Policy.TreeScale*Budget.TreeDensity;
				for(int32 Piece=0;Piece<(Fuel>.65f?2:1)&&Stats.GetTrees()<Budget.TreeInstanceCap;++Piece)
					if(AddPlant(Family,Piece,Chance,Budget.TreeCullEndCm,false))
					{if(Family==EFamily::Conifer)++Stats.Conifers;else if(Family==EFamily::Palm)++Stats.Palms;else ++Stats.Broadleaf;}
			}
			if(!GardenCell&&!Cell.bAgriculturalLand&&Stats.Shrubs<Budget.ShrubInstanceCap)
			{
				const float Chance=FMath::Clamp(.025f+Fuel*.18f+Moisture*.08f-Drought*.08f,.005f,.35f)*Policy.ShrubScale*Budget.ShrubDensity;
				if(AddPlant(EFamily::Shrub,0,Chance,FMath::Min(Budget.TreeCullEndCm,30000.f),true))++Stats.Shrubs;
			}
			if(Stats.Grass<Budget.GrassInstanceCap&&Candidate.DistanceSquared<FMath::Square(static_cast<double>(Budget.GrassCullEndCm+Terrain.CellSize)))
			{
				const float Chance=FMath::Clamp(.18f+Moisture*.75f+Fuel*.3f-Drought*.65f,0.f,.98f)*Policy.GrassScale*Budget.GrassDensity*(Cell.bAgriculturalLand?.1f:1.f);
				for(int32 Piece=0;Piece<24&&Stats.Grass<Budget.GrassInstanceCap;++Piece)
					if(AddPlant(EFamily::Grass,Piece,Chance,Budget.GrassCullEndCm,true))++Stats.Grass;
			}
			if(Stats.GetTrees()>=Budget.TreeInstanceCap&&Stats.Shrubs>=Budget.ShrubInstanceCap&&Stats.Grass>=Budget.GrassInstanceCap)break;
		}
	}
	const bool Changed=Commit(Desired);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_URBAN_GARDEN_DRESSING eligibleCells=%d trees=%d shrubs=%d gardenTreeFamily=%d requiredGardenFamilyMissing=%d sharedTreeCap=%d sharedShrubCap=%d nativeHISMs=%d rejectedBounds=%d rejectedOverlap=%d nonAgriculturalFootprintsOnly=1 genericSpeciesOnly=1 canonicalWrites=0 newTimers=0 newActors=0 newMIDs=0"),
        GardenEligibleCells,GardenTrees,GardenShrubs,GardenTreeFamily,GardenTreeFamily!=INDEX_NONE&&!GardenTreeReady,Budget.TreeInstanceCap,Budget.ShrubInstanceCap,EntryCount,GardenRejectedBounds,GardenRejectedOverlap);
    if(FParse::Param(FCommandLine::Get(),TEXT("AftershockUrbanGardenAudit")))
    {
        // Read actual post-Commit native instances, not desired/admission counts.
        int32 ActualTrees=0,ActualShrubs=0,ActualGrass=0,ActualGardens=0,Failures=0;
        TArray<TSharedPtr<FJsonValue>> GardenRows;
        TMap<FIntPoint,TArray<FVector>> ActualGardenCircles;
        for(const FEntry& Entry:Entries)
        {
            const auto* Batch=Entry.Component.Get();
            if(!Batch){++Failures;continue;}
            if(Batch->GetInstanceCount()!=Entry.KeyByIndex.Num())++Failures;
            const bool IsTree=Entry.Family==EFamily::Conifer||Entry.Family==EFamily::Palm||Entry.Family==EFamily::Broadleaf;
            if(IsTree)ActualTrees+=Batch->GetInstanceCount();
            else if(Entry.Family==EFamily::Shrub)ActualShrubs+=Batch->GetInstanceCount();
            else ActualGrass+=Batch->GetInstanceCount();
            for(int32 Index=0;Index<Entry.KeyByIndex.Num();++Index)
            {
                const FIntVector Key=Entry.KeyByIndex[Index];
                if(Key.Z!=100&&Key.Z!=110&&Key.Z!=111)continue;
                ++ActualGardens;const FIntPoint Coord(Key.X,Key.Y);FGridCell Source;
                FTransform Transform;const bool HasTransform=Batch->GetInstanceTransform(Index,Transform,true);
                const FVector Scale=Transform.GetScale3D(),Position=Transform.GetLocation();
                const FBox Rotated=Entry.NativeBounds.TransformBy(FTransform(Transform.GetRotation(),FVector::ZeroVector,Scale));
                const double X=FMath::Max(FMath::Abs(Rotated.Min.X),FMath::Abs(Rotated.Max.X));
                const double Y=FMath::Max(FMath::Abs(Rotated.Min.Y),FMath::Abs(Rotated.Max.Y));
                const float Radius=static_cast<float>(FMath::Sqrt(X*X+Y*Y));
                const float AuthoredHeight=static_cast<float>(Entry.NativeBounds.GetSize().Z*Scale.Z);
                const bool SourcePresent=Terrain.TryGetCell(Coord,Source);
                const FVector Center=SourcePresent?Terrain.GridToWorld(Coord):Terrain.GridOrigin;
                bool Valid=HasTransform&&!Transform.ContainsNaN()&&SourcePresent
                    &&IsDryFreeLand(Source)&&!Source.bAgriculturalLand&&Scale.X>0.
                    &&FMath::IsNearlyEqual(Scale.X,Scale.Y,.0001)&&FMath::IsNearlyEqual(Scale.X,Scale.Z,.0001)
                    &&FMath::IsFinite(AuthoredHeight)&&AuthoredHeight>=(IsTree?199.9f:44.9f)&&AuthoredHeight<=(IsTree?400.1f:100.1f)
                    &&ClearFootprint(Terrain,Coord,Position,Radius,true)
                    &&FMath::Abs(Position.X-Center.X)+Radius+29.9<=Terrain.CellSize*.5
                    &&FMath::Abs(Position.Y-Center.Y)+Radius+29.9<=Terrain.CellSize*.5;
                const UStaticMesh* Mesh=Batch->GetStaticMesh();
                const int32 Slots=Mesh?Mesh->GetStaticMaterials().Num():0;
                bool MaterialsReady=Slots>=2;
                for(int32 Slot=0;Slot<Slots;++Slot)MaterialsReady&=Batch->GetMaterial(Slot)!=nullptr;
                Valid&=MaterialsReady&&Mesh&&Mesh->GetNumLODs()>=3&&Batch->NumCustomDataFloats==2;
                Valid&=Key.Z==100?(IsTree&&static_cast<int32>(Entry.Family)==GardenTreeFamily):Entry.Family==EFamily::Shrub;
                auto& Circles=ActualGardenCircles.FindOrAdd(Coord);
                for(const FVector& Circle:Circles)
                    Valid&=FVector::DistSquaredXY(Position,Circle)>=FMath::Square(static_cast<double>(Radius)+Circle.Z+19.9);
                Circles.Add(FVector(Position.X,Position.Y,Radius));
                if(!Valid)++Failures;
                auto Row=MakeShared<FJsonObject>();Row->SetBoolField(TEXT("valid"),Valid);
                Row->SetStringField(TEXT("mesh"),GetPathNameSafe(Mesh));Row->SetNumberField(TEXT("cell_x"),Coord.X);Row->SetNumberField(TEXT("cell_y"),Coord.Y);
                Row->SetNumberField(TEXT("piece_key"),Key.Z);Row->SetNumberField(TEXT("height_cm"),FMath::IsFinite(AuthoredHeight)?AuthoredHeight:-1.f);Row->SetNumberField(TEXT("footprint_radius_cm"),FMath::IsFinite(Radius)?Radius:-1.f);
                Row->SetNumberField(TEXT("material_slots"),Slots);Row->SetBoolField(TEXT("all_material_slots_present"),MaterialsReady);
                GardenRows.Add(MakeShared<FJsonValueObject>(Row));
            }
        }
        if(ActualTrees>Budget.TreeInstanceCap||ActualShrubs>Budget.ShrubInstanceCap||ActualGrass>Budget.GrassInstanceCap
            ||ActualTrees!=Stats.GetTrees()||ActualShrubs!=Stats.Shrubs||ActualGrass!=Stats.Grass
            ||ActualGardens!=GardenTrees+GardenShrubs)++Failures;
        auto Receipt=MakeShared<FJsonObject>();Receipt->SetBoolField(TEXT("valid"),Failures==0);
        Receipt->SetStringField(TEXT("checked_utc"),FDateTime::UtcNow().ToIso8601());Receipt->SetNumberField(TEXT("failures"),Failures);
        Receipt->SetStringField(TEXT("scope"),TEXT("Actual post-Commit native HISM counts, uniform garden heights, full bounded nonagricultural clearances, same-cell separation, material slots and3LODs; not visual/GPU/whole-world acceptance."));
        Receipt->SetNumberField(TEXT("region"),Terrain.GeographyIndex);Receipt->SetNumberField(TEXT("focus"),Terrain.GeographyFocusIndex);
        Receipt->SetNumberField(TEXT("foliage_quality"),Quality.Foliage);Receipt->SetNumberField(TEXT("eligible_garden_cells"),GardenEligibleCells);
        Receipt->SetBoolField(TEXT("zero_legal_garden_cells"),GardenEligibleCells==0);Receipt->SetNumberField(TEXT("actual_garden_instances"),ActualGardens);
        Receipt->SetNumberField(TEXT("actual_total_trees"),ActualTrees);Receipt->SetNumberField(TEXT("actual_total_shrubs"),ActualShrubs);Receipt->SetNumberField(TEXT("actual_total_grass"),ActualGrass);
        Receipt->SetNumberField(TEXT("tree_cap"),Budget.TreeInstanceCap);Receipt->SetNumberField(TEXT("shrub_cap"),Budget.ShrubInstanceCap);Receipt->SetNumberField(TEXT("grass_cap"),Budget.GrassInstanceCap);
        Receipt->SetNumberField(TEXT("shared_hisms"),Entries.Num());Receipt->SetArrayField(TEXT("garden_instances"),GardenRows);
        FString Json;const auto Writer=TJsonWriterFactory<>::Create(&Json);FJsonSerializer::Serialize(Receipt,Writer);
        const FString Path=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation/urban_garden_native.json"));
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
        const bool Saved=FFileHelper::SaveStringToFile(Json,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_URBAN_GARDEN_NATIVE valid=%d failures=%d actualGardens=%d actualTrees=%d actualShrubs=%d actualGrass=%d saved=%d"),Failures==0&&Saved,Failures,ActualGardens,ActualTrees,ActualShrubs,ActualGrass,Saved);
    }
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_ENVIRONMENT_DRESSING region=%d focus=%d biome=%d foliageQuality=%d qualitySignature=%u overviewDisabled=%d scanned=%d eligible=%d trees=%d conifer=%d palm=%d broadleaf=%d shrubs=%d grass=%d treeCap=%d shrubCap=%d grassCap=%d treeEndCm=%.0f grassEndCm=%.0f changedBatches=%d changedInstances=%d readyMeshes=%d missingMeshes=%d missingRequiredTreeFamily=%d replacesLegacyWoodland=%d badFootprintsRejected=%d unsupportedGroundRejected=%d nativeCentimetres=1 deterministicPerCell=1 canonicalWrites=0"),
		Terrain.GeographyIndex,Terrain.GeographyFocusIndex,static_cast<int32>(Biome),Quality.Foliage,Stats.QualitySignature,Stats.bOverviewDisabled,
		Stats.ScannedCells,Stats.EligibleCells,Stats.GetTrees(),Stats.Conifers,Stats.Palms,Stats.Broadleaf,Stats.Shrubs,Stats.Grass,
		Budget.TreeInstanceCap,Budget.ShrubInstanceCap,Budget.GrassInstanceCap,Budget.TreeCullEndCm,Budget.GrassCullEndCm,
		Stats.ChangedBatches,Stats.ChangedInstances,Stats.ReadyMeshes,Stats.MissingMeshes,Stats.bRequiredTreeFamilyMissing,Stats.bReplacesLegacyWoodland,Stats.RejectedFootprints,Stats.RejectedGround);
	return Changed;
}

bool FAftershockEnvironmentDressing::Commit(const TArray<TArray<FDesired>>& Desired)
{
	for(int32 EntryIndex=0;EntryIndex<Entries.Num();++EntryIndex)
	{
		FEntry& Entry=Entries[EntryIndex];auto* Batch=Entry.Component.Get();if(!Batch)continue;
		bool Changed=false;
		if(Batch->GetInstanceCount()!=Entry.KeyByIndex.Num())
		{
			Batch->ClearInstances();Entry.KeyByIndex.Reset();Entry.IndexByKey.Reset();Changed=true;
		}
		TSet<FIntVector> Wanted;Wanted.Reserve(Desired[EntryIndex].Num());
		for(const FDesired& Item:Desired[EntryIndex])Wanted.Add(Item.Key);
		for(int32 Index=Entry.KeyByIndex.Num()-1;Index>=0;--Index)if(!Wanted.Contains(Entry.KeyByIndex[Index]))
		{
			const FIntVector Removed=Entry.KeyByIndex[Index];
			if(Batch->RemoveInstance(Index))
			{
				Entry.KeyByIndex.RemoveAtSwap(Index,1,EAllowShrinking::No);Entry.IndexByKey.Remove(Removed);
				if(Entry.KeyByIndex.IsValidIndex(Index))Entry.IndexByKey.FindChecked(Entry.KeyByIndex[Index])=Index;
				Changed=true;++Stats.ChangedInstances;
			}
		}
		for(const FDesired& Item:Desired[EntryIndex])
		{
			if(const int32* Index=Entry.IndexByKey.Find(Item.Key))
			{
				FTransform Current;if(!Batch->GetInstanceTransform(*Index,Current,true)||!Current.Equals(Item.Transform,.001f))
				{Batch->UpdateInstanceTransform(*Index,Item.Transform,true,false,true);Changed=true;++Stats.ChangedInstances;}
			}
			else
			{
				const int32 NewIndex=Batch->AddInstance(Item.Transform,true);
				if(NewIndex!=INDEX_NONE)
				{
					Entry.KeyByIndex.Add(Item.Key);Entry.IndexByKey.Add(Item.Key,NewIndex);
					Batch->SetCustomDataValue(NewIndex,0,0.f,false);Batch->SetCustomDataValue(NewIndex,1,Item.Phase,false);
					Changed=true;++Stats.ChangedInstances;
				}
			}
		}
		if(Changed){Batch->BuildTreeIfOutdated(false,true);Batch->MarkRenderStateDirty();++Stats.ChangedBatches;}
	}
	return Stats.ChangedBatches>0;
}
