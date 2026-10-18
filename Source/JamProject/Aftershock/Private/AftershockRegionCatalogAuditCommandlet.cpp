#include "AftershockRegionCatalogAuditCommandlet.h"

#if WITH_EDITOR
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/AftershockRegionRules.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include <limits>

UAftershockRegionCatalogAuditCommandlet::UAftershockRegionCatalogAuditCommandlet()
{
	IsClient=false; IsServer=false; IsEditor=true; LogToConsole=true;
}

int32 UAftershockRegionCatalogAuditCommandlet::Main(const FString& Params)
{
	int32 Passed=0, Failed=0;
	TArray<FString> Results;
	const auto Check=[&](const TCHAR* Name, bool bPass)
	{
		bPass ? ++Passed : ++Failed;
		Results.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"),Name,bPass?TEXT("true"):TEXT("false")));
		if (bPass) { UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_REGION_CATALOG_PASS %s"),Name); }
		else { UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_REGION_CATALOG_FAIL %s"),Name); }
	};
	Check(TEXT("twenty_stable_region_identities"),GetAftershockGeographyCount()==20);
	Check(TEXT("invalid_region_id_has_no_fallback"),FindAftershockGeographyIndex(NAME_None)==INDEX_NONE && FindAftershockGeographyIndex(TEXT("not_a_region"))==INDEX_NONE);
	Check(TEXT("invalid_region_focus_is_unavailable"),GetAftershockGeographyFocusCount(-1)==0 && GetAftershockGeographyFocusCount(20)==0 && !TryGetAftershockGeographyFocus(20,0) && !IsAftershockGeographyFocusReady(-1,0));
	Check(TEXT("invalid_hazard_is_rejected"),!IsAftershockHazardAllowed(0,static_cast<EDisasterType>(255)) && !IsAftershockHazardAllowed(-1,EDisasterType::Earthquake));
	Check(TEXT("invalid_sample_is_not_fabricated_dem"),!FMath::IsFinite(SampleAftershockFocusElevationMetres(20,0,.5f,.5f)) && !FMath::IsFinite(SampleAftershockFocusElevationMetres(0,9,.5f,.5f)) && !FMath::IsFinite(SampleAftershockFocusElevationMetres(0,0,std::numeric_limits<float>::quiet_NaN(),.5f)));

	struct FLegacySample { const TCHAR* Id; double Lat; double Lon; float First; float Centre; float Last; float Sum; };
	const FLegacySample Legacy[] = {
		{TEXT("atakent"),41.041,28.775,112.f,24.f,32.f,4567.f},
		{TEXT("dereyolu"),40.912,40.112,0.f,10.f,733.f,12731.f},
		{TEXT("yelova"),36.490,36.190,121.f,562.f,296.f,54998.f},
		{TEXT("kiyikent"),36.090,35.980,335.f,50.f,444.f,9310.f},
		{TEXT("mugla"),36.855,28.274,105.f,8.f,66.f,17641.f},
		{TEXT("etna"),37.730,15.015,1514.f,2474.f,562.f,134379.f},
		{TEXT("konya"),37.715,33.550,1081.f,993.f,1031.f,81363.f}
	};
	for (int32 Index=0;Index<UE_ARRAY_COUNT(Legacy);++Index)
	{
		const auto& Expected=Legacy[Index]; const auto& Site=GetAftershockGeography(Index);
		float Sum=0.f; for (int32 Y=0;Y<9;++Y) for (int32 X=0;X<9;++X) Sum+=SampleAftershockElevationMetres(Index,X/8.f,Y/8.f);
		Check(*FString::Printf(TEXT("legacy_%d_identity_anchor_and_samples_unchanged"),Index),
			FindAftershockGeographyIndex(Expected.Id)==Index && FName(Site.Id)==FName(Expected.Id) && FMath::IsNearlyEqual(Site.Latitude,Expected.Lat,1.e-8) && FMath::IsNearlyEqual(Site.Longitude,Expected.Lon,1.e-8)
			&& GetAftershockGeographyFocusCount(Index)==(Index==5?2:1) && SampleAftershockElevationMetres(Index,0.f,0.f)==Expected.First && SampleAftershockElevationMetres(Index,.5f,.5f)==Expected.Centre
			&& SampleAftershockElevationMetres(Index,1.f,1.f)==Expected.Last && Sum==Expected.Sum);
	}

	int32 TotalFocuses=0; TSet<FName> RegionIds;
	for (int32 RegionIndex=0;RegionIndex<GetAftershockGeographyCount();++RegionIndex)
	{
		const auto& Region=GetAftershockRegionRules(RegionIndex); const FName Id(Region.RegionId);
		const FString Prefix=FString::Printf(TEXT("region_%02d_"),RegionIndex);
		Check(*(Prefix+TEXT("id_roundtrip_unique")),!RegionIds.Contains(Id) && FindAftershockGeographyIndex(Id)==RegionIndex && FName(GetAftershockGeography(RegionIndex).Id)==Id);
		RegionIds.Add(Id);
		bool bFiniteWeights=true;
		for (int32 H=0;H<FAftershockRegionRules::HazardCount;++H) bFiniteWeights &= FMath::IsFinite(Region.Weights[H]) && Region.Weights[H]>=0.f && Region.Weights[H]<=1.f;
		Check(*(Prefix+TEXT("authored_weights_and_primary_valid")),bFiniteWeights && IsAftershockHazardAllowed(RegionIndex,Region.PrimaryHazard));
		const auto& Default=GetAftershockLocalFocusRules(RegionIndex,0);
		Check(*(Prefix+TEXT("legacy_default_rules_match_focus_zero")),Region.bHasCoast==Default.bHasCoast && Region.bHasRiver==Default.bHasRiver && Region.bHasVolcanicSource==Default.bHasVolcanicSource && Region.LandscapeProfile.ReliefCm==Default.LandscapeProfile.ReliefCm);
		TSet<FName> FocusIds;
		for (int32 FocusIndex=0;FocusIndex<GetAftershockGeographyFocusCount(RegionIndex);++FocusIndex)
		{
			++TotalFocuses;
			const auto& Site=GetAftershockGeographyFocus(RegionIndex,FocusIndex); const auto& Local=GetAftershockLocalFocusRules(RegionIndex,FocusIndex);
			const FString Key=Prefix+FString::Printf(TEXT("focus_%d_"),FocusIndex); const FName FocusId(Site.FocusId);
			Check(*(Key+TEXT("ready_id_roundtrip_unique")),IsAftershockGeographyFocusReady(RegionIndex,FocusIndex) && !FocusIds.Contains(FocusId) && FindAftershockGeographyFocusIndex(RegionIndex,FocusId)==FocusIndex && FName(Site.Id)==Id);
			FocusIds.Add(FocusId);
			bool bFiniteGrid=true;
			for (int32 Y=0;Y<9;++Y) for (int32 X=0;X<9;++X)
			{
				const float Value=SampleAftershockFocusElevationMetres(RegionIndex,FocusIndex,X/8.f,Y/8.f);
				bFiniteGrid &= FMath::IsFinite(Value) && Value>=-500.f && Value<=9000.f;
			}
			const float CornerAverage=(SampleAftershockFocusElevationMetres(RegionIndex,FocusIndex,0.f,0.f)+SampleAftershockFocusElevationMetres(RegionIndex,FocusIndex,.125f,0.f)+SampleAftershockFocusElevationMetres(RegionIndex,FocusIndex,0.f,.125f)+SampleAftershockFocusElevationMetres(RegionIndex,FocusIndex,.125f,.125f))*.25f;
			Check(*(Key+TEXT("real_grid_finite_bilinear_and_clamped")),bFiniteGrid && FMath::IsNearlyEqual(CornerAverage,SampleAftershockFocusElevationMetres(RegionIndex,FocusIndex,.0625f,.0625f),.001f) && SampleAftershockFocusElevationMetres(RegionIndex,FocusIndex,-2.f,-2.f)==SampleAftershockFocusElevationMetres(RegionIndex,FocusIndex,0.f,0.f));
			FVector East,South,Up; GetAftershockGeographicFocusBasis(RegionIndex,FocusIndex,East,South,Up);
			Check(*(Key+TEXT("local_globe_frame_and_regional_bounds")),East.IsNormalized() && South.IsNormalized() && Up.IsNormalized() && FMath::Abs(FVector::DotProduct(East,Up))<1.e-5 && FMath::Abs(FVector::DotProduct(South,Up))<1.e-5
				&& AftershockLatLonToFocusGlobeNormal(Site.Latitude,Site.Longitude,RegionIndex,FocusIndex).Equals(FVector::UpVector,1.e-5)
				&& Site.Longitude>=Region.RegionalBounds.West && Site.Longitude<=Region.RegionalBounds.East && Site.Latitude>=Region.RegionalBounds.South && Site.Latitude<=Region.RegionalBounds.North);
			bool bPhysicalGates=true;
			for (int32 H=0;H<FAftershockRegionRules::HazardCount;++H)
			{
				const auto Hazard=static_cast<EDisasterType>(H); const bool Allowed=IsAftershockFocusHazardAllowed(RegionIndex,FocusIndex,Hazard);
				if (Region.Weights[H]<=0.f) bPhysicalGates &= !Allowed;
				if (Hazard==EDisasterType::Tsunami && !Local.bHasCoast) bPhysicalGates &= !Allowed;
				if (Hazard==EDisasterType::Volcano && !Local.bHasVolcanicSource) bPhysicalGates &= !Allowed;
				if (Hazard==EDisasterType::Fire && Local.LandscapeProfile.ForestFuel01<=0.f) bPhysicalGates &= !Allowed;
				bPhysicalGates &= (Allowed ? GetAftershockHazardWeight(RegionIndex,Hazard,FocusIndex)>0.f : GetAftershockHazardWeight(RegionIndex,Hazard,FocusIndex)==0.f);
			}
			if (Local.bHasVolcanicSource) bPhysicalGates &= FMath::Abs(Region.VolcanoLatitude-Site.Latitude)<=Site.HalfLatitudeExtent && FMath::Abs(Region.VolcanoLongitude-Site.Longitude)<=Site.HalfLongitudeExtent;
			Check(*(Key+TEXT("local_hazard_source_and_zero_weight_gates")),bPhysicalGates);
		}
		Check(*(Prefix+TEXT("invalid_focus_rejected")),!IsAftershockGeographyFocusReady(RegionIndex,-1) && !IsAftershockGeographyFocusReady(RegionIndex,GetAftershockGeographyFocusCount(RegionIndex)) && FindAftershockGeographyFocusIndex(RegionIndex,TEXT("missing_focus"))==INDEX_NONE);
	}
	Check(TEXT("twenty_six_distinct_local_focus_records"),TotalFocuses==26);
	const auto* Lower=TryGetAftershockGeographyFocus(5,1);const auto* Summit=TryGetAftershockGeographyFocus(5,0);
	Check(TEXT("etna_additive_lower_flank_identity_default_and_immutable_source_alias"),Lower&&Summit&&FName(Lower->FocusId)==TEXT("lower_flank")&&FName(Summit->FocusId)==TEXT("etna")
		&&Lower->Latitude==37.685&&Lower->Longitude==15.065&&Summit->Latitude==37.73&&Summit->Longitude==15.015
		&&GetAftershockDefaultNewGameFocusIndex(5)==1&&GetAftershockDefaultNewGameFocusIndex(8)==0
		&&GetAftershockFocusSourceIndex(5,1)==GetAftershockFocusSourceIndex(5,0));
	const int32 Miami=FindAftershockGeographyIndex(TEXT("miami_keys")), Cyprus=FindAftershockGeographyIndex(TEXT("cyprus"));
	Check(TEXT("miami_four_local_focuses_not_one_corridor_dem"),GetAftershockGeographyFocusCount(Miami)==4 && FindAftershockGeographyFocusIndex(Miami,TEXT("key_west"))==3 && FMath::Abs(GetAftershockGeographyFocus(Miami,0).Latitude-GetAftershockGeographyFocus(Miami,3).Latitude)>1. && GetAftershockRegionRules(Miami).RegionalBounds.East-GetAftershockRegionRules(Miami).RegionalBounds.West>1.);
	Check(TEXT("cyprus_three_separate_local_focuses"),GetAftershockGeographyFocusCount(Cyprus)==3 && FindAftershockGeographyFocusIndex(Cyprus,TEXT("kalkanli"))==0 && FindAftershockGeographyFocusIndex(Cyprus,TEXT("alaykoy"))==1 && FindAftershockGeographyFocusIndex(Cyprus,TEXT("lefke"))==2 && GetAftershockLocalFocusRules(Cyprus,0).LandscapeProfile.ReliefCm!=GetAftershockLocalFocusRules(Cyprus,1).LandscapeProfile.ReliefCm);
	Check(TEXT("storm_surge_is_not_relabeled_tsunami"),GetAftershockRegionRules(Miami).bStormSurgeContext && !IsAftershockHazardAllowed(Miami,EDisasterType::Tsunami) && !IsAftershockHazardAllowed(FindAftershockGeographyIndex(TEXT("new_orleans")),EDisasterType::Tsunami) && !IsAftershockHazardAllowed(FindAftershockGeographyIndex(TEXT("rotterdam")),EDisasterType::Tsunami));
	const FString Directory=FPaths::ProjectSavedDir()/TEXT("PresentationValidation");
	IFileManager::Get().MakeDirectory(*Directory,true);
	const FString JSON=FString::Printf(TEXT("{\"schema_version\":1,\"scope\":\"Native immutable region/focus identities, cached DEM interpolation and authored eligibility only. No terrain, UI, forecast, package, audio or throughput claim.\",\"region_count\":%d,\"focus_count\":%d,\"passed\":%d,\"failed\":%d,\"checks\":[%s]}"),GetAftershockGeographyCount(),TotalFocuses,Passed,Failed,*FString::Join(Results,TEXT(",")));
	const FString Path=Directory/TEXT("region_catalog_audit.json");
	if (!FFileHelper::SaveStringToFile(JSON,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return 2;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_REGION_CATALOG_RESULT passed=%d failed=%d regions=%d focuses=%d report=%s"),Passed,Failed,GetAftershockGeographyCount(),TotalFocuses,*Path);
	return Failed==0 ? 0 : 1;
}
#else
UAftershockRegionCatalogAuditCommandlet::UAftershockRegionCatalogAuditCommandlet() {}
int32 UAftershockRegionCatalogAuditCommandlet::Main(const FString& Params) { return 1; }
#endif
