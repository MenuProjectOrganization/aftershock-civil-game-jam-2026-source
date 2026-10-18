#include "AftershockRegionalEnvironmentAuditCommandlet.h"
#if WITH_EDITOR
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/AftershockRegionEnvironmentProfile.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "AftershockRegionalSurfaceStyle.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include <limits>

namespace
{
	struct FEnvironmentAuditWorld
	{
		UWorld* World=nullptr;
		~FEnvironmentAuditWorld()
		{if(World){World->DestroyWorld(false);if(GEngine)GEngine->DestroyWorldContext(World);World->RemoveFromRoot();}}
	};
	bool FiniteEnvironmentSample(const FAftershockGeographicTerrainSample& Sample)
	{
		return FMath::IsFinite(Sample.ElevationMetres)&&Sample.ElevationMetres>-12000.f&&Sample.ElevationMetres<9000.f
			&&FMath::IsFinite(Sample.RiverDistanceMetres)&&Sample.RiverDistanceMetres>=0.f&&Sample.RiverDistanceMetres<=6553.51f
			&&FMath::IsFinite(Sample.SignedCoastDistanceMetres)&&FMath::Abs(Sample.SignedCoastDistanceMetres)<=3276.71f;
	}
	bool CorrectEnvironmentFlags(const FAftershockGeographicTerrainSample& Sample)
	{
		const uint8 Flags=Sample.SurfaceFlags;
		return (Flags&~63)==0&&Sample.bOcean==((Flags&1)!=0)
			&&Sample.bInlandWater==(!Sample.bOcean&&(Flags&(2|16))!=0)
			&&Sample.bMappedForest==(!Sample.bOcean&&(Flags&4)!=0)
			&&Sample.bMappedAgriculture==(!Sample.bOcean&&(Flags&8)!=0)
			&&Sample.bIntermittentWaterway==(!Sample.bOcean&&(Flags&32)!=0)
			&&(!Sample.bOcean||((Flags&62)==0&&!Sample.bInlandWater&&!Sample.bMappedForest&&!Sample.bMappedAgriculture));
	}
	FAftershockGeographicTerrainSample EnvironmentSentinel()
	{FAftershockGeographicTerrainSample Value;Value.ElevationMetres=123.75f;Value.SignedCoastDistanceMetres=99.f;Value.SurfaceFlags=128;return Value;}
	bool IsEnvironmentSentinel(const FAftershockGeographicTerrainSample& Value)
	{return Value.ElevationMetres==123.75f&&Value.SignedCoastDistanceMetres==99.f&&Value.RiverDistanceMetres==6553.5f&&Value.SurfaceFlags==128
		&&!Value.bOcean&&!Value.bInlandWater&&!Value.bMappedForest&&!Value.bMappedAgriculture&&!Value.bHasCoastlineSource&&!Value.bIntermittentWaterway;}
}

UAftershockRegionalEnvironmentAuditCommandlet::UAftershockRegionalEnvironmentAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}

int32 UAftershockRegionalEnvironmentAuditCommandlet::Main(const FString& Params)
{
	int32 Passed=0,Failed=0,Focuses=0,ClimateMonths=0,DryIntermittentSamples=0;int64 DetailSamples=0,ContextSamples=0;
	TArray<FString> Checks,Metrics;
	const auto Check=[&](const FString& Name,bool Value)
	{
		Value?++Passed:++Failed;Checks.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"),*Name,Value?TEXT("true"):TEXT("false")));
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_REGIONAL_ENVIRONMENT_CHECK %s passed=%d"),*Name,Value);
	};
	const auto Finish=[&]()
	{
		const FString Folder=FPaths::ProjectSavedDir()/TEXT("PresentationValidation");IFileManager::Get().MakeDirectory(*Folder,true);
		const FString Json=FString::Printf(TEXT("{\"schema_version\":1,\"scope\":\"Native immutable V3 detail DEM/masks, generalized regional context, NASA monthly climatology, metre scale and unchanged V2 restoration. Source samples and artistic lapse-rate estimates are not surveyed soil, observed snow, weather, rendered imagery or calibrated hazard forecasts.\",\"region_count\":%d,\"focus_count\":%d,\"climate_months\":%d,\"detail_samples\":%lld,\"context_samples\":%lld,\"dry_intermittent_samples\":%d,\"passed\":%d,\"failed\":%d,\"focus_metrics\":[%s],\"checks\":[%s]}"),GetAftershockGeographyCount(),Focuses,ClimateMonths,DetailSamples,ContextSamples,DryIntermittentSamples,Passed,Failed,*FString::Join(Metrics,TEXT(",")),*FString::Join(Checks,TEXT(",")));
		const bool Saved=FFileHelper::SaveStringToFile(Json,*(Folder/TEXT("regional_environment_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_REGIONAL_ENVIRONMENT_RESULT regions=%d focuses=%d climateMonths=%d passed=%d failed=%d saved=%d"),GetAftershockGeographyCount(),Focuses,ClimateMonths,Passed,Failed,Saved);
		return Failed==0&&Saved?0:1;
	};
	Check(TEXT("twenty_region_catalogue"),GetAftershockGeographyCount()==20);
	const double NaN=std::numeric_limits<double>::quiet_NaN(),Infinity=std::numeric_limits<double>::infinity();
	auto Unchanged=EnvironmentSentinel();
	Check(TEXT("invalid_detail_id_and_nan_fail_without_sample_mutation"),!IsAftershockFocusDetailGeodataReady(-1,0)
		&&!TrySampleAftershockFocusDetailGeodata(-1,0,0.,0.,Unchanged)&&IsEnvironmentSentinel(Unchanged)
		&&!TrySampleAftershockFocusDetailGeodata(0,0,NaN,0.,Unchanged)&&IsEnvironmentSentinel(Unchanged));
	Check(TEXT("invalid_context_id_and_infinite_coordinate_fail_without_mutation"),!TrySampleAftershockRegionalContext(-1,0.,0.,Unchanged)
		&&!TrySampleAftershockRegionalContext(0,0.,Infinity,Unchanged)&&IsEnvironmentSentinel(Unchanged));
	FAftershockMonthlyClimate UntouchedClimate;UntouchedClimate.T2MC=123.f;
	Check(TEXT("invalid_climate_month_or_focus_has_no_substitute"),!TryGetAftershockFocusMonthlyClimate(0,0,0,UntouchedClimate)
		&&!TryGetAftershockFocusMonthlyClimate(0,0,13,UntouchedClimate)&&!TryGetAftershockFocusMonthlyClimate(0,-1,1,UntouchedClimate)&&UntouchedClimate.T2MC==123.f);
	FEnvironmentAuditWorld Fixture;
	if(GEngine)
	{
		Fixture.World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("RegionalEnvironmentAuditWorld")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
		if(Fixture.World)GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(Fixture.World);
	}
	auto* Terrain=Fixture.World?Fixture.World->GetSubsystem<UTerrainGridSubsystem>():nullptr;Check(TEXT("isolated_terrain_owner_available"),Terrain!=nullptr);
	for(int32 Region=0;Region<GetAftershockGeographyCount();++Region)
	{
		const auto* Profile=TryGetAftershockRegionEnvironmentProfile(Region);const FString RegionKey=FString::Printf(TEXT("region_%02d_"),Region);
		Check(RegionKey+TEXT("profile_has_exact_catalogue_identity_and_primary_source"),Profile&&FName(Profile->RegionId)==FName(GetAftershockGeography(Region).Id)
			&&!FString(Profile->GeologySummary).IsEmpty()&&FString(Profile->GeologySourceUrl).StartsWith(TEXT("https://")));
		FAftershockGeographicBounds ContextBounds;const bool ContextReady=GetAftershockRegionalContextBounds(Region,ContextBounds);
		Check(RegionKey+TEXT("actual_wide_context_ready"),ContextReady);
		float ContextMin=MAX_flt,ContextMax=-MAX_flt;bool ContextFinite=ContextReady,ContextFlags=ContextReady;
		if(ContextReady)
		{
			for(int32 Y=0;Y<257;++Y)for(int32 X=0;X<257;++X)
			{
				FAftershockGeographicTerrainSample Value;const bool Read=TrySampleAftershockRegionalContext(Region,FMath::Lerp(ContextBounds.North,ContextBounds.South,Y/256.),FMath::Lerp(ContextBounds.West,ContextBounds.East,X/256.),Value);
				++ContextSamples;ContextFinite&=Read&&FiniteEnvironmentSample(Value);ContextFlags&=Read&&CorrectEnvironmentFlags(Value)&&(Value.SurfaceFlags&~19)==0;
				if(Read){ContextMin=FMath::Min(ContextMin,Value.ElevationMetres);ContextMax=FMath::Max(ContextMax,Value.ElevationMetres);}
			}
			Unchanged=EnvironmentSentinel();Check(RegionKey+TEXT("overview_never_clamps_beyond_actual_source"),!TrySampleAftershockRegionalContext(Region,ContextBounds.North+.0001,ContextBounds.West,Unchanged)&&IsEnvironmentSentinel(Unchanged));
		}
		Check(RegionKey+TEXT("all_66049_context_samples_finite_and_ocean_tags_exclusive"),ContextFinite&&ContextFlags);
		// Independent, deliberately broad geographic sentinels: Etna's actual
		// summit exceeds3km; Cyprus' Troodos exceeds1.8km. Christchurch/Banks
		// Peninsula cannot contain the known provider's spurious5.7km spike.
		const FName RegionId(GetAftershockGeography(Region).Id);
		if(RegionId==TEXT("etna"))Check(RegionKey+TEXT("context_preserves_real_etna_summit_relief"),ContextReady&&ContextMax>3000.f&&ContextMax<4000.f);
		if(RegionId==TEXT("cyprus"))Check(RegionKey+TEXT("wide_context_includes_troodos_relief"),ContextReady&&ContextMax>1800.f&&ContextMax<2100.f);
		if(RegionId==TEXT("christchurch"))Check(RegionKey+TEXT("canterbury_context_has_no_spurious_alpine_spike"),ContextReady&&ContextMax>500.f&&ContextMax<=1200.f);
		for(int32 Focus=0;Focus<GetAftershockGeographyFocusCount(Region);++Focus)
		{
			++Focuses;const auto& Anchor=GetAftershockGeographyFocus(Region,Focus);const FString Key=RegionKey+FString::Printf(TEXT("focus_%d_"),Focus);
			FAftershockGeographicBounds Bounds,LegacyBounds;const bool Ready=IsAftershockFocusDetailGeodataReady(Region,Focus)&&GetAftershockFocusDetailGeodataBounds(Region,Focus,Bounds);
			Check(Key+TEXT("detail_and_legacy_source_footprints_remain_identical"),Ready&&GetAftershockFocusGeodataBounds(Region,Focus,LegacyBounds)
				&&Bounds.West==LegacyBounds.West&&Bounds.East==LegacyBounds.East&&Bounds.South==LegacyBounds.South&&Bounds.North==LegacyBounds.North);
			bool Finite=Ready,Flags=Ready;float Min=MAX_flt,Max=-MAX_flt,LandMin=MAX_flt,LandMax=-MAX_flt;int32 Dry=0,Ocean=0;
			if(Ready)
			{
				for(int32 Y=0;Y<257;++Y)for(int32 X=0;X<257;++X)
				{
					FAftershockGeographicTerrainSample Value;const bool Read=TrySampleAftershockFocusDetailGeodata(Region,Focus,FMath::Lerp(Bounds.North,Bounds.South,Y/256.),FMath::Lerp(Bounds.West,Bounds.East,X/256.),Value);
					++DetailSamples;Finite&=Read&&FiniteEnvironmentSample(Value);Flags&=Read&&CorrectEnvironmentFlags(Value);
					if(Read){Min=FMath::Min(Min,Value.ElevationMetres);Max=FMath::Max(Max,Value.ElevationMetres);Ocean+=Value.bOcean?1:0;
						if(!Value.bOcean){LandMin=FMath::Min(LandMin,Value.ElevationMetres);LandMax=FMath::Max(LandMax,Value.ElevationMetres);}
						if(Value.bIntermittentWaterway&&(Value.SurfaceFlags&(2|16))==0){++Dry;Flags&=!Value.bInlandWater;}}
				}
				Unchanged=EnvironmentSentinel();Check(Key+TEXT("outside_detail_source_does_not_clamp_height_or_flags"),!TrySampleAftershockFocusDetailGeodata(Region,Focus,Bounds.South-.0001,Bounds.West,Unchanged)&&IsEnvironmentSentinel(Unchanged));
			}
			Check(Key+TEXT("all_66049_detail_samples_finite_with_source_derived_flags"),Finite&&Flags);DryIntermittentSamples+=Dry;
			if(RegionId==TEXT("etna"))Check(Key+TEXT("detail_preserves_real_etna_summit_relief"),Ready&&Max>3000.f&&Max<4000.f);
			if(RegionId==TEXT("miami_keys"))Check(Key+TEXT("florida_bare_earth_window_cannot_contain_synthetic_mountains"),Ready&&LandMin<=LandMax&&LandMin>=-20.f&&LandMax<=100.f);
			bool MonthsValid=true,LapseValid=true;float Cold=MAX_flt,Hot=-MAX_flt;int32 Start=0,End=0;
			for(int32 Month=1;Month<=12;++Month)
			{
				FAftershockMonthlyClimate Climate;const bool Read=TryGetAftershockFocusMonthlyClimate(Region,Focus,Month,Climate);++ClimateMonths;
				MonthsValid&=Read&&FMath::IsFinite(Climate.T2MC)&&FMath::IsFinite(Climate.PrecipMmPerDay)&&FMath::IsFinite(Climate.RHPercent)&&FMath::IsFinite(Climate.WindMps)
					&&Climate.T2MC>=-95.f&&Climate.T2MC<=65.f&&Climate.PrecipMmPerDay>=0.f&&Climate.PrecipMmPerDay<=80.f
					&&Climate.RHPercent>=0.f&&Climate.RHPercent<=100.f&&Climate.WindMps>=0.f&&Climate.WindMps<=80.f
					&&Climate.StartYear>=1981&&Climate.EndYear>=Climate.StartYear&&Climate.SourceGridLatitudeDegrees==.5f&&Climate.SourceGridLongitudeDegrees==.625f;
				if(Month==1){Start=Climate.StartYear;End=Climate.EndYear;}else MonthsValid&=Start==Climate.StartYear&&End==Climate.EndYear;
				float AtSource=0.f,Above=0.f,RejectSentinel=77.f;
				LapseValid&=Read&&TryEstimateAftershockVisualTemperatureAtElevation(Climate,Climate.SourceElevationM,AtSource)&&FMath::IsNearlyEqual(AtSource,Climate.T2MC,.0001f)
					&&TryEstimateAftershockVisualTemperatureAtElevation(Climate,Climate.SourceElevationM+1000.f,Above)&&FMath::IsNearlyEqual(Above,Climate.T2MC-6.49f,.001f)
					&&!TryEstimateAftershockVisualTemperatureAtElevation(Climate,std::numeric_limits<float>::quiet_NaN(),RejectSentinel)&&RejectSentinel==77.f;
				if(Read){Cold=FMath::Min(Cold,Climate.T2MC);Hot=FMath::Max(Hot,Climate.T2MC);}
			}
			Check(Key+TEXT("all_twelve_actual_months_have_finite_units_period_and_source_grid"),MonthsValid);
			Check(Key+TEXT("visual_lapse_is_exact_and_nan_cannot_mutate_output"),LapseValid);
			if(RegionId==TEXT("yakutsk"))Check(Key+TEXT("yakutsk_monthly_climate_has_real_winter_and_summer"),MonthsValid&&Cold< -20.f&&Hot>10.f);
			if(Terrain&&Ready)
			{
				Terrain->GenerateProceduralTerrain(32,32,1000.f,15021+Region*31+Focus);
				const bool Applied=Terrain->ApplyOfflineGeography(Region,Focus,false,3);bool Match=Applied&&Terrain->ContinuationGeneratorVersion==3;
				for(const auto& Cell:Terrain->Cells)
				{
					double Lat=0.,Lon=0.;FAftershockGeographicTerrainSample Sample;const FVector Centre=Terrain->GridToWorld(Cell.Coords);float WorldHeight=0.f;
					Match&=Terrain->TryWorldToGeographic(Centre,Lat,Lon)&&Terrain->TrySampleGeographicSource(Lat,Lon,Sample)
						&&Cell.bHasRealElevationSample&&!Cell.bAuthoredContinuation&&FMath::IsNearlyEqual(Cell.RealElevationMetres,Sample.ElevationMetres,.01f)
						&&FMath::IsNearlyEqual(Cell.Elevation,Terrain->GridOrigin.Z+Sample.ElevationMetres*100.f,1.f)
						&&Terrain->TrySampleWorldElevation(Centre,WorldHeight)&&FMath::IsNearlyEqual(WorldHeight,Cell.Elevation,.1f)
						&&Cell.bIsOcean==Sample.bOcean&&Cell.bIsRiverbed==(!Sample.bOcean&&Sample.bInlandWater)&&Cell.bIsPlayable==!Sample.bOcean;
				}
				Check(Key+TEXT("v3_canonical_cells_world_sampler_and_detail_source_agree_without_normalization"),Match);
				const bool AppliedV2=Terrain->ApplyOfflineGeography(Region,Focus,false,2);FAftershockTerrainSaveState Saved;Terrain->CaptureSaveState(Saved);FText Error;
				const bool V2Valid=AppliedV2&&Saved.ContinuationGeneratorVersion==2&&Terrain->ValidateSaveState(Saved,Error);
				const bool ReappliedV3=Terrain->ApplyOfflineGeography(Region,Focus,false,3);const bool Restored=V2Valid&&ReappliedV3&&Terrain->RestoreSaveState(Saved,Error);
				bool Same=Restored&&Terrain->ContinuationGeneratorVersion==2&&Terrain->Cells.Num()==Saved.Cells.Num();
				if(Same)for(int32 I=0;I<Saved.Cells.Num();++I)Same&=Terrain->Cells[I].Coords==Saved.Cells[I].Coords&&Terrain->Cells[I].Elevation==Saved.Cells[I].Elevation&&Terrain->Cells[I].RealElevationMetres==Saved.Cells[I].RealElevationMetres;
				Check(Key+TEXT("v2_snapshot_restores_unchanged_after_v3_source_use"),Same);
			}
			Metrics.Add(FString::Printf(TEXT("{\"region\":%d,\"focus\":%d,\"detail_min_m\":%.6f,\"detail_max_m\":%.6f,\"context_min_m\":%.6f,\"context_max_m\":%.6f,\"ocean_samples\":%d,\"dry_intermittent_samples\":%d,\"coldest_month_c\":%.6f,\"warmest_month_c\":%.6f}"),Region,Focus,Min,Max,ContextMin,ContextMax,Ocean,Dry,Cold,Hot));
		}
	}
	Check(TEXT("all_twenty_six_focuses_and_312_climate_months_visited_on_25_unique_source_windows"),Focuses==26&&ClimateMonths==312&&DetailSamples==26ll*257*257&&ContextSamples==20ll*257*257
		&&GetAftershockFocusSourceIndex(5,1)==GetAftershockFocusSourceIndex(5,0));
	Check(TEXT("mapped_seasonal_channels_exist_but_are_not_permanent_water"),DryIntermittentSamples>0);
	const auto Cache=GetAftershockGeodataCacheStats();
	Check(TEXT("source_caches_remain_fixed_and_overview_lru_has_at_most_two_tiles"),Cache.DetailReadyTiles==25&&Cache.DetailAttemptedTiles==25&&Cache.DetailDecodedAllocatedBytes<=32ll*1024*1024
		&&Cache.ReadyTiles==25&&Cache.AttemptedTiles==25&&Cache.DecodedAllocatedBytes<=16ll*1024*1024&&Cache.OverviewResidentTiles<=2&&Cache.OverviewDecodedAllocatedBytes<=2ll*1024*1024);
	return Finish();
}
#else
UAftershockRegionalEnvironmentAuditCommandlet::UAftershockRegionalEnvironmentAuditCommandlet(){}
int32 UAftershockRegionalEnvironmentAuditCommandlet::Main(const FString&){return 1;}
#endif
