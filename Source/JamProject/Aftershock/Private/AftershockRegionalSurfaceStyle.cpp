#include "AftershockRegionalSurfaceStyle.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/AftershockRegionEnvironmentProfile.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/WorldClimateSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameters.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"

namespace
{
	constexpr int32 MaxRegionalStyleMIDs=96;
	struct FRegionalStyleState
	{
		int32 Slot=0;
		FLinearColor DryTint,RockTint;
		FLinearColor MacroBoundsMin=FLinearColor::Black,MacroBoundsMax=FLinearColor::Black;
		TWeakObjectPtr<UTexture2D> MacroTexture;
		bool bMacroReady=false;
		float Temperature=0.f,Elevation=0.f,OriginZ=0.f,Month=0.f,Wetness=0.f,Volcanic=0.f;
		bool operator==(const FRegionalStyleState& Other)const
		{
			return Slot==Other.Slot&&DryTint==Other.DryTint&&RockTint==Other.RockTint
				&&MacroTexture==Other.MacroTexture&&bMacroReady==Other.bMacroReady&&MacroBoundsMin==Other.MacroBoundsMin&&MacroBoundsMax==Other.MacroBoundsMax
				&&Temperature==Other.Temperature&&Elevation==Other.Elevation&&OriginZ==Other.OriginZ
				&&Month==Other.Month&&Wetness==Other.Wetness&&Volcanic==Other.Volcanic;
		}
	};
	struct FRegionalStyleMIDEntry
	{
		TWeakObjectPtr<UMaterialInstanceDynamic> MID;
		FRegionalStyleState State;
		uint64 LastUse=0;
		bool bEnabled=false;
	};
	// Metadata only: neither this bounded cache nor the fixed texture cache roots
	// a component, MID, world or texture. PIE/hot reload can collect them normally.
	TArray<FRegionalStyleMIDEntry> RegionalStyleMIDs;
	uint64 RegionalStyleUseSerial=0;
	struct FRegionalStyleTextureEntry {TWeakObjectPtr<UTexture2D> Texture;double RetryAfter=0.;};
	FRegionalStyleTextureEntry RegionalStyleTextures[5][3]; // source4 is greenmeadow, not a new biome
	FRegionalStyleTextureEntry RegionalMacroTextures[20];
	FRegionalStyleTextureEntry RegionalSurroundTextures[20];
	FRegionalStyleTextureEntry GlobalMacroTexture;
	struct FGlobalColourMIDEntry
	{
		TWeakObjectPtr<UMaterialInstanceDynamic> MID;TWeakObjectPtr<UTexture2D> Texture,SurroundTexture;
		FLinearColor WorldMin=FLinearColor::Black,WorldMax=FLinearColor::Black,GeoMin=FLinearColor::Black,GeoMax=FLinearColor::Black;
		FLinearColor SurroundGeoMin=FLinearColor::Black,SurroundGeoMax=FLinearColor::Black;
		uint64 LastUse=0;bool bEnabled=false,bSurroundReady=false;
	};
	TArray<FGlobalColourMIDEntry> GlobalColourMIDs;uint64 GlobalColourUseSerial=0;
	const TCHAR* const RegionalTexturePaths[5][3]={
		{TEXT("/Game/Environment/Textures/PolyHaven/T_LeafyGrass_Diffuse"),TEXT("/Game/Environment/Textures/PolyHaven/T_LeafyGrass_NormalDX"),TEXT("/Game/Environment/Textures/PolyHaven/T_LeafyGrass_Roughness")},
		{TEXT("/Game/Environment/Textures/Regional/T_RegionalDrySoil_Diffuse"),TEXT("/Game/Environment/Textures/Regional/T_RegionalDrySoil_NormalDX"),TEXT("/Game/Environment/Textures/Regional/T_RegionalDrySoil_Roughness")},
		{TEXT("/Game/Environment/Textures/Regional/T_RegionalForestFloor_Diffuse"),TEXT("/Game/Environment/Textures/Regional/T_RegionalForestFloor_NormalDX"),TEXT("/Game/Environment/Textures/Regional/T_RegionalForestFloor_Roughness")},
		{TEXT("/Game/Environment/Textures/Regional/T_RegionalRock_Diffuse"),TEXT("/Game/Environment/Textures/Regional/T_RegionalRock_NormalDX"),TEXT("/Game/Environment/Textures/Regional/T_RegionalRock_Roughness")},
        {TEXT("/Game/Environment/Textures/Regional/T_RegionalGreenMeadow_Diffuse"),TEXT("/Game/Environment/Textures/Regional/T_RegionalGreenMeadow_NormalDX"),TEXT("/Game/Environment/Textures/Regional/T_RegionalGreenMeadow_Roughness")}
	};

	UTexture2D* GetRegionalStyleTexture(int32 Slot,int32 Role)
	{
		auto& Entry=RegionalStyleTextures[Slot][Role];
		if(auto* Texture=Entry.Texture.Get())return Texture;
		const double Now=FPlatformTime::Seconds();if(Now<Entry.RetryAfter)return nullptr;
		Entry.RetryAfter=Now+5.;
		auto* Texture=LoadObject<UTexture2D>(nullptr,RegionalTexturePaths[Slot][Role]);
		Entry.Texture=Texture;return Texture;
	}
	UTexture2D* GetOptionalRegionalMacroTexture(int32 Region,const TCHAR* RegionId)
	{
		if(Region<0||Region>=static_cast<int32>(UE_ARRAY_COUNT(RegionalMacroTextures)))return nullptr;
		auto& Entry=RegionalMacroTextures[Region];if(auto* Texture=Entry.Texture.Get())return Texture;
		const double Now=FPlatformTime::Seconds();if(Now<Entry.RetryAfter)return nullptr;Entry.RetryAfter=Now+5.;
		const FString Path=FString(TEXT("/Game/Environment/Textures/Regional/Macro/T_RegionalMacro_"))+RegionId;
		// Optional historical imagery is allowed to be absent. Do not generate
		// missing-asset warnings or substitute another region's photograph.
		if(!FPackageName::DoesPackageExist(Path))return nullptr;
		auto* Texture=LoadObject<UTexture2D>(nullptr,*Path);
		if(!Texture||Texture->GetSizeX()<1||Texture->GetSizeY()<1||Texture->GetSizeX()>1024||Texture->GetSizeY()>1024||!Texture->SRGB)return nullptr;
		Entry.Texture=Texture;return Texture;
	}
	UTexture2D* GetOptionalGlobalMacroTexture()
	{
		if(auto* Texture=GlobalMacroTexture.Texture.Get())return Texture;
		const double Now=FPlatformTime::Seconds();if(Now<GlobalMacroTexture.RetryAfter)return nullptr;
		GlobalMacroTexture.RetryAfter=Now+5.;
		constexpr const TCHAR* Path=TEXT("/Game/Environment/Textures/Regional/Macro/T_GlobalMacro_EOX2016");
		if(!FPackageName::DoesPackageExist(Path))return nullptr;
		auto* Texture=LoadObject<UTexture2D>(nullptr,Path);
		if(!Texture||Texture->GetSizeX()!=8192||Texture->GetSizeY()!=4096||!Texture->SRGB)return nullptr;
		GlobalMacroTexture.Texture=Texture;return Texture;
	}
	UTexture2D* GetOptionalRegionalSurroundTexture(int32 Region,const TCHAR* RegionId)
	{
		if(Region<0||Region>=static_cast<int32>(UE_ARRAY_COUNT(RegionalSurroundTextures))||!RegionId||!*RegionId)return nullptr;
		const auto IsReady=[](const UTexture2D* Texture)
		{return Texture&&Texture->GetSizeX()==2048&&Texture->GetSizeY()==2048&&Texture->GetNumMips()==12&&Texture->SRGB;};
		auto& Entry=RegionalSurroundTextures[Region];
		// A loaded asset can still be finishing platform-data initialization. Keep
		// only its weak reference and revalidate on the existing presentation poll;
		// never mistake pending/invalid metadata for an absent package or fake Ready.
		if(auto* Texture=Entry.Texture.Get())return IsReady(Texture)?Texture:nullptr;
		const double Now=FPlatformTime::Seconds();if(Now<Entry.RetryAfter)return nullptr;
		const FString Path=FString(TEXT("/Game/Environment/Textures/Regional/Surround/T_RegionalSurround_"))+RegionId;
		if(!FPackageName::DoesPackageExist(Path)){Entry.RetryAfter=Now+5.;return nullptr;}
		auto* Texture=LoadObject<UTexture2D>(nullptr,*Path);
		if(!Texture){Entry.RetryAfter=Now+5.;return nullptr;}
		Entry.Texture=Texture;Entry.RetryAfter=0.;
		return IsReady(Texture)?Texture:nullptr;
	}
	FGlobalColourMIDEntry& RememberGlobalColour(UMaterialInstanceDynamic* MID)
	{
		GlobalColourMIDs.RemoveAll([](const FGlobalColourMIDEntry& Entry){return !Entry.MID.IsValid();});
		if(auto* Entry=GlobalColourMIDs.FindByPredicate([MID](const FGlobalColourMIDEntry& Value){return Value.MID.Get()==MID;}))
		{Entry->LastUse=++GlobalColourUseSerial;return *Entry;}
		if(GlobalColourMIDs.Num()>=MaxRegionalStyleMIDs)
		{
			int32 Oldest=0;for(int32 I=1;I<GlobalColourMIDs.Num();++I)if(GlobalColourMIDs[I].LastUse<GlobalColourMIDs[Oldest].LastUse)Oldest=I;
			GlobalColourMIDs.RemoveAtSwap(Oldest,1,EAllowShrinking::No);
		}
		auto& Entry=GlobalColourMIDs.AddDefaulted_GetRef();Entry.MID=MID;Entry.LastUse=++GlobalColourUseSerial;return Entry;
	}

	FRegionalStyleMIDEntry* FindStyleEntry(UMaterialInstanceDynamic* MID)
	{
		RegionalStyleMIDs.RemoveAll([](const FRegionalStyleMIDEntry& Entry){return !Entry.MID.IsValid();});
		return RegionalStyleMIDs.FindByPredicate([MID](const FRegionalStyleMIDEntry& Entry){return Entry.MID.Get()==MID;});
	}
	FRegionalStyleMIDEntry& RememberStyle(UMaterialInstanceDynamic* MID)
	{
		if(auto* Entry=FindStyleEntry(MID)){Entry->LastUse=++RegionalStyleUseSerial;return *Entry;}
		if(RegionalStyleMIDs.Num()>=MaxRegionalStyleMIDs)
		{
			int32 Oldest=0;for(int32 I=1;I<RegionalStyleMIDs.Num();++I)if(RegionalStyleMIDs[I].LastUse<RegionalStyleMIDs[Oldest].LastUse)Oldest=I;
			RegionalStyleMIDs.RemoveAtSwap(Oldest,1,EAllowShrinking::No);
		}
		auto& Entry=RegionalStyleMIDs.AddDefaulted_GetRef();Entry.MID=MID;Entry.LastUse=++RegionalStyleUseSerial;return Entry;
	}
	void DisableExistingStyle(UMaterialInstanceDynamic* MID)
	{
		if(!MID)return;
		if(auto* Entry=FindStyleEntry(MID))
		{
			Entry->LastUse=++RegionalStyleUseSerial;
			if(!Entry->bEnabled)return;
			MID->SetScalarParameterValue(TEXT("RegionalStyleEnabled"),0.f);Entry->bEnabled=false;return;
		}
		// A previously styled MID may survive a metadata-cache eviction or be
		// supplied by another presentation owner. Never allocate a V1/V2 MID.
		if(MID->K2_GetScalarParameterValue(TEXT("RegionalStyleEnabled"))!=0.f)
		{MID->SetScalarParameterValue(TEXT("RegionalStyleEnabled"),0.f);RememberStyle(MID).bEnabled=false;}
	}
}

float GetAftershockRegionalVisualMonth(const UTerrainGridSubsystem& Terrain)
{
	const auto* Economy=Terrain.GetWorld()?Terrain.GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>():nullptr;
	const int32 Turn=Economy?FMath::Max(1,Economy->CurrentTurn):1;
	return 1.f+FMath::Fmod(9.f+static_cast<float>((Turn-1)%10)*1.2f,12.f);
}

bool GetAftershockRegionalVisualClimate(const UTerrainGridSubsystem& Terrain,FAftershockMonthlyClimate& Out)
{
	const float Month=GetAftershockRegionalVisualMonth(Terrain);const int32 A=FMath::FloorToInt(Month),B=A%12+1;
	FAftershockMonthlyClimate First,Second;
	if(!TryGetAftershockFocusMonthlyClimate(Terrain.GeographyIndex,Terrain.GeographyFocusIndex,A,First)
		||!TryGetAftershockFocusMonthlyClimate(Terrain.GeographyIndex,Terrain.GeographyFocusIndex,B,Second))return false;
	Out=First;const float T=Month-A;
	Out.T2MC=FMath::Lerp(First.T2MC,Second.T2MC,T);Out.PrecipMmPerDay=FMath::Lerp(First.PrecipMmPerDay,Second.PrecipMmPerDay,T);
	Out.RHPercent=FMath::Lerp(First.RHPercent,Second.RHPercent,T);Out.WindMps=FMath::Lerp(First.WindMps,Second.WindMps,T);
	if(const auto* Climate=Terrain.GetWorld()?Terrain.GetWorld()->GetSubsystem<UWorldClimateSubsystem>():nullptr)
		Out.T2MC+=Climate->GetState().GlobalTemperatureAnomalyC-Climate->GetState().InitialAnomalyC;
	return true;
}

bool ApplyAftershockRegionalSurfaceStyle(const UTerrainGridSubsystem& Terrain,UStaticMeshComponent* Component,int32 MaterialIndex)
{
	check(IsInGameThread());
	if(!Component||MaterialIndex<0||MaterialIndex>=Component->GetNumMaterials()||!Component->GetMaterial(MaterialIndex))return false;
	auto* MID=Cast<UMaterialInstanceDynamic>(Component->GetMaterial(MaterialIndex));
	const auto* Profile=TryGetAftershockRegionEnvironmentProfile(Terrain.GeographyIndex);FAftershockMonthlyClimate Climate;
	const bool Enabled=Terrain.ContinuationGeneratorVersion==3&&Profile&&GetAftershockRegionalVisualClimate(Terrain,Climate);
	if(!Enabled){DisableExistingStyle(MID);return true;}
	FRegionalStyleState State;State.DryTint=Profile->DrySoilTint;State.RockTint=Profile->RockTint;
	switch(Profile->RegionalBiome)
	{
	case EAftershockRegionalBiome::Mediterranean:case EAftershockRegionalBiome::SemiAridSteppe:State.Slot=1;break;
	case EAftershockRegionalBiome::VolcanicMediterranean:case EAftershockRegionalBiome::SubpolarVolcanic:State.Slot=3;break;
	case EAftershockRegionalBiome::ContinentalTaiga:case EAftershockRegionalBiome::TemperateRainforest:State.Slot=2;break;
	default:break;
	}
	State.Temperature=Climate.T2MC;State.Elevation=Climate.SourceElevationM;State.OriginZ=static_cast<float>(Terrain.GridOrigin.Z);
	State.Month=GetAftershockRegionalVisualMonth(Terrain);State.Wetness=FMath::Clamp(Climate.PrecipMmPerDay/8.f*.7f+Climate.RHPercent*.003f,0.f,1.f);
	State.Volcanic=State.Slot==3?1.f:0.f;
	State.MacroTexture=GetOptionalRegionalMacroTexture(Terrain.GeographyIndex,Profile->RegionId);
	FAftershockGeographicBounds MacroBounds;FVector2D NorthWest,SouthEast;
	if(State.MacroTexture.IsValid()&&GetAftershockRegionalContextBounds(Terrain.GeographyIndex,MacroBounds)
		&&AftershockFocusLatLonToLocalMetres(Terrain.GeographyIndex,Terrain.GeographyFocusIndex,MacroBounds.North,MacroBounds.West,NorthWest)
		&&AftershockFocusLatLonToLocalMetres(Terrain.GeographyIndex,Terrain.GeographyFocusIndex,MacroBounds.South,MacroBounds.East,SouthEast))
	{
		const double CentreX=Terrain.GridOrigin.X+Terrain.GridWidth*Terrain.CellSize*.5,CentreY=Terrain.GridOrigin.Y+Terrain.GridHeight*Terrain.CellSize*.5;
		State.MacroBoundsMin=FLinearColor(CentreX+NorthWest.X*100.,CentreY+NorthWest.Y*100.,0.f,0.f);
		State.MacroBoundsMax=FLinearColor(CentreX+SouthEast.X*100.,CentreY+SouthEast.Y*100.,0.f,0.f);
		State.bMacroReady=State.MacroBoundsMin.R<State.MacroBoundsMax.R&&State.MacroBoundsMin.G<State.MacroBoundsMax.G;
	}
	const auto FiniteColor=[](const FLinearColor& Color){return FMath::IsFinite(Color.R)&&FMath::IsFinite(Color.G)&&FMath::IsFinite(Color.B)&&FMath::IsFinite(Color.A);};
	if(!FMath::IsFinite(State.Temperature)||!FMath::IsFinite(State.Elevation)||!FMath::IsFinite(State.OriginZ)
		||!FMath::IsFinite(State.Month)||!FMath::IsFinite(State.Wetness)||!FiniteColor(State.DryTint)||!FiniteColor(State.RockTint)
		||(State.bMacroReady&&(!FiniteColor(State.MacroBoundsMin)||!FiniteColor(State.MacroBoundsMax))))
	{DisableExistingStyle(MID);return false;}
	if(MID)if(auto* Entry=FindStyleEntry(MID))if(Entry->bEnabled&&Entry->State==State)
	{Entry->LastUse=++RegionalStyleUseSerial;return true;}
	// One additional EXISTING source set; no new data authority or saved field.
    // Grass profiles gain dry-soil breakup; dry/forest profiles gain grass.
    // Volcano retains exposed rock, with cultivated/forest accent gated in shader.
    const int32 AccentSlots[]={1,0,0,1};
    UTexture2D* Base[3];UTexture2D* Rock[3];UTexture2D* Accent[3];bool Ready=true;
	for(int32 Role=0;Role<3;++Role)
	{
		// Keep biomeSlot0 semantics but source humid grass from a genuinelygreen
        // PBR set. Source0 remains oldLeafyGrass for existing dry/forest accents.
        Base[Role]=GetRegionalStyleTexture(State.Slot==0?4:State.Slot,Role);Rock[Role]=GetRegionalStyleTexture(3,Role);
        Accent[Role]=GetRegionalStyleTexture(AccentSlots[State.Slot],Role);Ready&=Base[Role]&&Rock[Role]&&Accent[Role];
	}
	if(!Ready){DisableExistingStyle(MID);return false;}
	if(!MID)MID=Component->CreateAndSetMaterialInstanceDynamic(MaterialIndex);if(!MID)return false;
	const TCHAR* Roles[]={TEXT("Diffuse"),TEXT("NormalDX"),TEXT("Roughness")};
	for(int32 Role=0;Role<3;++Role)
	{MID->SetTextureParameterValue(FName(*(FString(TEXT("RegionalBase"))+Roles[Role])),Base[Role]);MID->SetTextureParameterValue(FName(*(FString(TEXT("RegionalRock"))+Roles[Role])),Rock[Role]);
        MID->SetTextureParameterValue(FName(*(FString(TEXT("RegionalAccent"))+Roles[Role])),Accent[Role]);}
	MID->SetVectorParameterValue(TEXT("RegionalDrySoilTint"),State.DryTint);MID->SetVectorParameterValue(TEXT("RegionalRockTint"),State.RockTint);
	MID->SetScalarParameterValue(TEXT("RegionalStyleTexturesReady"),1.f);
	MID->SetScalarParameterValue(TEXT("RegionalMeanTemperatureC"),State.Temperature);MID->SetScalarParameterValue(TEXT("RegionalClimateElevationM"),State.Elevation);
	MID->SetScalarParameterValue(TEXT("RegionalWorldOriginZ"),State.OriginZ);MID->SetScalarParameterValue(TEXT("RegionalVisualMonth"),State.Month);
	MID->SetScalarParameterValue(TEXT("RegionalWetness"),State.Wetness);MID->SetScalarParameterValue(TEXT("RegionalVolcanic"),State.Volcanic);
	MID->SetScalarParameterValue(TEXT("RegionalVolcanicActualStyle"),State.Volcanic);
	MID->SetScalarParameterValue(TEXT("RegionalMacroReady"),State.bMacroReady?1.f:0.f);
	if(State.bMacroReady)
	{
		MID->SetTextureParameterValue(TEXT("RegionalMacroTexture"),State.MacroTexture.Get());
		MID->SetVectorParameterValue(TEXT("RegionalMacroBoundsMin"),State.MacroBoundsMin);MID->SetVectorParameterValue(TEXT("RegionalMacroBoundsMax"),State.MacroBoundsMax);
	}
	const float BaseTileCm[]={140.f,400.f,200.f,8000.f}; // official ambientCGGrass001 ca1.4m
	MID->SetScalarParameterValue(TEXT("RegionalBaseTileCm"),BaseTileCm[State.Slot]);MID->SetScalarParameterValue(TEXT("RegionalRockTileCm"),8000.f);
	const float AccentTileCm[]={400.f,200.f,200.f,400.f};
    MID->SetScalarParameterValue(TEXT("RegionalAccentTileCm"),AccentTileCm[State.Slot]);
    MID->SetScalarParameterValue(TEXT("RegionalSurfaceRefinement"),1.f);
    MID->SetScalarParameterValue(TEXT("RegionalBiomeSlot"),static_cast<float>(State.Slot));
	MID->SetScalarParameterValue(TEXT("RegionalStyleEnabled"),1.f);
	auto& Entry=RememberStyle(MID);Entry.State=State;Entry.bEnabled=true;return true;
}

bool ApplyAftershockGlobalRegionalColour(const UTerrainGridSubsystem& Terrain,UStaticMeshComponent* Component,int32 MaterialIndex)
{
	check(IsInGameThread());
	if(!Component||MaterialIndex<0||MaterialIndex>=Component->GetNumMaterials())return false;
	UMaterialInterface* Parent=Component->GetMaterial(MaterialIndex);if(!Parent)return false;
	auto* MID=Cast<UMaterialInstanceDynamic>(Parent);
	float Ready=0.f;const bool HasWrapper=Parent->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(TEXT("GlobalImageryReady"))),Ready);
	float SurroundReady=0.f;
	const bool HasSurroundWrapper=Parent->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(TEXT("RegionalSurroundReady"))),SurroundReady);
	const auto Disable=[MID,Parent,HasWrapper,Ready,HasSurroundWrapper,SurroundReady]()
	{
		if(MID&&HasWrapper&&Ready!=0.f)MID->SetScalarParameterValue(TEXT("GlobalImageryReady"),0.f);
		if(MID&&HasSurroundWrapper&&SurroundReady!=0.f)
		{
			MID->SetScalarParameterValue(TEXT("RegionalSurroundReady"),0.f);
			UTexture* Fallback=nullptr;
			if(Parent->GetTextureParameterValue(FHashedMaterialParameterInfo(FName(TEXT("GlobalImageryTexture"))),Fallback)&&Fallback)
				MID->SetTextureParameterValue(TEXT("RegionalSurroundTexture"),Fallback);
		}
		if(MID){auto& Entry=RememberGlobalColour(MID);Entry.bEnabled=false;Entry.Texture.Reset();Entry.SurroundTexture.Reset();Entry.bSurroundReady=false;}
	};
	if(Terrain.ContinuationGeneratorVersion!=3){Disable();return true;}
	if(!HasWrapper)return false; // No new MID or optional asset load before authoring.
	UTexture2D* Texture=GetOptionalGlobalMacroTexture();FAftershockGeographicBounds Bounds;
	FVector2D NorthWest=FVector2D::ZeroVector,SouthEast=FVector2D::ZeroVector;
	if(!Texture||!GetAftershockRegionalContextBounds(Terrain.GeographyIndex,Bounds)
		||!AftershockFocusLatLonToLocalMetres(Terrain.GeographyIndex,Terrain.GeographyFocusIndex,Bounds.North,Bounds.West,NorthWest)
		||!AftershockFocusLatLonToLocalMetres(Terrain.GeographyIndex,Terrain.GeographyFocusIndex,Bounds.South,Bounds.East,SouthEast))
	{Disable();return false;}
	const double CentreX=Terrain.GridOrigin.X+Terrain.GridWidth*Terrain.CellSize*.5,CentreY=Terrain.GridOrigin.Y+Terrain.GridHeight*Terrain.CellSize*.5;
	const FLinearColor WorldMin(CentreX+NorthWest.X*100.,CentreY+NorthWest.Y*100.,0.f,0.f),WorldMax(CentreX+SouthEast.X*100.,CentreY+SouthEast.Y*100.,0.f,0.f);
	const FLinearColor GeoMin(Bounds.West,Bounds.South,0.f,0.f),GeoMax(Bounds.East,Bounds.North,0.f,0.f);
	const auto Finite=[](const FLinearColor& Value){return FMath::IsFinite(Value.R)&&FMath::IsFinite(Value.G);};
	if(!Finite(WorldMin)||!Finite(WorldMax)||!Finite(GeoMin)||!Finite(GeoMax)||WorldMin.R>=WorldMax.R||WorldMin.G>=WorldMax.G||GeoMin.R>=GeoMax.R||GeoMin.G>=GeoMax.G)
	{Disable();return false;}
	// One optional photograph per region, not per focus/cell. The source packet
	// uses exactly this centre/span x8 construction, independently clamped to WGS84.
	// Absent/un-authored surround imagery keeps the existing global-image fallback.
	UTexture2D* SurroundTexture=nullptr;
	FLinearColor SurroundGeoMin=FLinearColor::Black,SurroundGeoMax=FLinearColor::Black;
	if(HasSurroundWrapper)
	{
		const auto* Profile=TryGetAftershockRegionEnvironmentProfile(Terrain.GeographyIndex);
		const double LongitudeCentre=(Bounds.West+Bounds.East)*.5,LatitudeCentre=(Bounds.South+Bounds.North)*.5;
		const double HalfLongitudeSpan=(Bounds.East-Bounds.West)*4.,HalfLatitudeSpan=(Bounds.North-Bounds.South)*4.;
		SurroundGeoMin=FLinearColor(FMath::Max(-180.,LongitudeCentre-HalfLongitudeSpan),FMath::Max(-90.,LatitudeCentre-HalfLatitudeSpan),0.f,0.f);
		SurroundGeoMax=FLinearColor(FMath::Min(180.,LongitudeCentre+HalfLongitudeSpan),FMath::Min(90.,LatitudeCentre+HalfLatitudeSpan),0.f,0.f);
		if(Profile&&Finite(SurroundGeoMin)&&Finite(SurroundGeoMax)&&SurroundGeoMin.R<SurroundGeoMax.R&&SurroundGeoMin.G<SurroundGeoMax.G)
			SurroundTexture=GetOptionalRegionalSurroundTexture(Terrain.GeographyIndex,Profile->RegionId);
	}
	const bool bSurroundReady=SurroundTexture!=nullptr;
	if(!MID)MID=Component->CreateAndSetMaterialInstanceDynamic(MaterialIndex);if(!MID)return false;
	auto& Entry=RememberGlobalColour(MID);
	if(Entry.bEnabled&&Ready==1.f&&Entry.Texture.Get()==Texture&&Entry.WorldMin==WorldMin&&Entry.WorldMax==WorldMax&&Entry.GeoMin==GeoMin&&Entry.GeoMax==GeoMax
		&&Entry.bSurroundReady==bSurroundReady&&Entry.SurroundTexture.Get()==SurroundTexture&&Entry.SurroundGeoMin==SurroundGeoMin&&Entry.SurroundGeoMax==SurroundGeoMax
		&&(!HasSurroundWrapper||SurroundReady==(bSurroundReady?1.f:0.f)))return true;
	MID->SetTextureParameterValue(TEXT("GlobalImageryTexture"),Texture);
	MID->SetVectorParameterValue(TEXT("GlobalRegionWorldMin"),WorldMin);MID->SetVectorParameterValue(TEXT("GlobalRegionWorldMax"),WorldMax);
	MID->SetVectorParameterValue(TEXT("GlobalRegionWGS84Min"),GeoMin);MID->SetVectorParameterValue(TEXT("GlobalRegionWGS84Max"),GeoMax);
	MID->SetScalarParameterValue(TEXT("GlobalImageryReady"),1.f);
	if(HasSurroundWrapper)
	{
		MID->SetTextureParameterValue(TEXT("RegionalSurroundTexture"),bSurroundReady?SurroundTexture:Texture);
		MID->SetVectorParameterValue(TEXT("RegionalSurroundGeoMin"),SurroundGeoMin);
		MID->SetVectorParameterValue(TEXT("RegionalSurroundGeoMax"),SurroundGeoMax);
		MID->SetScalarParameterValue(TEXT("RegionalSurroundReady"),bSurroundReady?1.f:0.f);
	}
	Entry.Texture=Texture;Entry.WorldMin=WorldMin;Entry.WorldMax=WorldMax;Entry.GeoMin=GeoMin;Entry.GeoMax=GeoMax;Entry.bEnabled=true;
	Entry.SurroundTexture=SurroundTexture;Entry.SurroundGeoMin=SurroundGeoMin;Entry.SurroundGeoMax=SurroundGeoMax;Entry.bSurroundReady=bSurroundReady;
	return true;
}
