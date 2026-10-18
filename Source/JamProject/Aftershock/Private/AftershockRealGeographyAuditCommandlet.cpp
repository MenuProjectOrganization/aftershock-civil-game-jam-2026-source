#include "AftershockRealGeographyAuditCommandlet.h"
#if WITH_EDITOR
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include <limits>

UAftershockRealGeographyAuditCommandlet::UAftershockRealGeographyAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}

int32 UAftershockRealGeographyAuditCommandlet::Main(const FString& Params)
{
	int32 Passed=0,Failed=0,Focuses=0;TArray<FString> Checks;
	const auto Check=[&](const FString& Name,bool Value)
	{
		Value?++Passed:++Failed;Checks.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"),*Name,Value?TEXT("true"):TEXT("false")));
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_REAL_GEOGRAPHY_CHECK %s passed=%d"),*Name,Value);
	};
	Check(TEXT("twenty_actual_region_identities"),GetAftershockGeographyCount()==20);
	FAftershockGeographicTerrainSample Unchanged;Unchanged.ElevationMetres=123.f;
	Check(TEXT("invalid_focus_is_unavailable_without_output_mutation"),!TrySampleAftershockFocusGeodata(-1,0,0.,0.,Unchanged)&&Unchanged.ElevationMetres==123.f);
	Check(TEXT("nonfinite_coordinate_is_rejected"),!TrySampleAftershockFocusGeodata(0,0,std::numeric_limits<double>::quiet_NaN(),0.,Unchanged));
	for(int32 Region=0;Region<GetAftershockGeographyCount();++Region)for(int32 Focus=0;Focus<GetAftershockGeographyFocusCount(Region);++Focus)
	{
		++Focuses;const FString Key=FString::Printf(TEXT("region_%d_focus_%d_"),Region,Focus);
		FAftershockGeographicBounds Bounds;const bool Ready=GetAftershockFocusGeodataBounds(Region,Focus,Bounds);
		Check(Key+TEXT("real_packet_ready"),Ready&&IsAftershockFocusGeodataReady(Region,Focus));
		if(!Ready)continue;
		const auto& Anchor=GetAftershockGeographyFocus(Region,Focus);FVector2D Local;
		const auto& SourceAnchor=GetAftershockGeographyFocus(Region,Region==5&&Focus==1?0:Focus);
		Check(Key+TEXT("source_bbox_is_immutable_and_local_anchor_is_independent"),FMath::IsNearlyEqual(Bounds.West,SourceAnchor.Longitude-SourceAnchor.HalfLongitudeExtent,1.e-10)&&FMath::IsNearlyEqual(Bounds.East,SourceAnchor.Longitude+SourceAnchor.HalfLongitudeExtent,1.e-10)
			&&FMath::IsNearlyEqual(Bounds.South,SourceAnchor.Latitude-SourceAnchor.HalfLatitudeExtent,1.e-10)&&FMath::IsNearlyEqual(Bounds.North,SourceAnchor.Latitude+SourceAnchor.HalfLatitudeExtent,1.e-10)
			&&Anchor.Longitude>=Bounds.West&&Anchor.Longitude<=Bounds.East&&Anchor.Latitude>=Bounds.South&&Anchor.Latitude<=Bounds.North
			&&AftershockFocusLatLonToLocalMetres(Region,Focus,Anchor.Latitude,Anchor.Longitude,Local)&&Local.IsNearlyZero(1.e-6));
		bool Finite=true,CoastFlags=true;
		for(int32 Y=0;Y<65;++Y)for(int32 X=0;X<65;++X)
		{
			const double Lat=FMath::Lerp(Bounds.North,Bounds.South,Y/64.),Lon=FMath::Lerp(Bounds.West,Bounds.East,X/64.);FAftershockGeographicTerrainSample Sample;
			Finite&=TrySampleAftershockFocusGeodata(Region,Focus,Lat,Lon,Sample)&&FMath::IsFinite(Sample.ElevationMetres)&&Sample.ElevationMetres>-12000.f&&Sample.ElevationMetres<9000.f
				&&FMath::IsFinite(Sample.RiverDistanceMetres)&&Sample.RiverDistanceMetres>=0.f&&FMath::IsFinite(Sample.SignedCoastDistanceMetres);
			CoastFlags&=!Sample.bOcean||(!Sample.bInlandWater&&!Sample.bMappedForest&&!Sample.bMappedAgriculture);
		}
		Check(Key+TEXT("all_4225_actual_dem_sample_positions_finite"),Finite);
		Check(Key+TEXT("ocean_never_claims_inland_water_forest_or_farmland"),CoastFlags);
		Check(Key+TEXT("outside_source_never_clamps_or_invents_height"),!TrySampleAftershockFocusGeodata(Region,Focus,Bounds.North+.0001,Bounds.West,Unchanged)
			&&!TrySampleAftershockFocusGeodata(Region,Focus,Bounds.South,Bounds.East+.0001,Unchanged)&&Unchanged.ElevationMetres==123.f);
		double Lat=0.,Lon=0.;FVector2D Metres;
		Check(Key+TEXT("local_metres_roundtrip_and_axis_orientation"),AftershockFocusLocalMetresToLatLon(Region,Focus,FVector2D(10.,20.),Lat,Lon)
			&&Lon>Anchor.Longitude&&Lat<Anchor.Latitude&&AftershockFocusLatLonToLocalMetres(Region,Focus,Lat,Lon,Metres)&&Metres.Equals(FVector2D(10.,20.),1.e-6));
	}
	Check(TEXT("twenty_six_ready_focuses_on_twenty_five_immutable_source_windows"),Focuses==26&&GetAftershockFocusSourceIndex(5,1)==GetAftershockFocusSourceIndex(5,0));
	UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("RealGeographyAuditWorld")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
	Check(TEXT("isolated_world_created"),World&&GEngine);
	if(World&&GEngine)
	{
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);auto* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();
		Check(TEXT("terrain_owner_available"),Terrain!=nullptr);
		if(Terrain)
		{
			Terrain->GenerateProceduralTerrain(125,125,1000.f,1337);
			Check(TEXT("explicit_version_two_source_remains_real"),Terrain->ApplyOfflineGeography(0,0,false,2)&&Terrain->IsUsingRealGeography());
			FAftershockTerrainSaveState Before;Terrain->CaptureSaveState(Before);FText Error;
			Check(TEXT("version_two_save_contract_valid"),Before.ContinuationGeneratorVersion==2&&Terrain->ValidateSaveState(Before,Error));
			bool SourceElevations=true,NoOval=true;
			for(const auto& Cell:Terrain->Cells)
			{
				SourceElevations&=Cell.bHasRealElevationSample&&!Cell.bAuthoredContinuation&&FMath::IsNearlyEqual(Cell.Elevation,Terrain->GridOrigin.Z+Cell.RealElevationMetres*100.f,.1f);
				NoOval&=Cell.bIsOcean?!Cell.bIsPlayable:Cell.bIsPlayable;
			}
			Check(TEXT("actual_metres_times_100_without_minmax_height_normalization"),SourceElevations);
			Check(TEXT("all_mapped_non_ocean_land_has_no_oval_placement_mask"),NoOval);
			const auto& Anchor=GetAftershockGeographyFocus(0,0);FVector A=FVector::ZeroVector,B=FVector::ZeroVector;double Lat=0.,Lon=0.;
			const bool Projected=Terrain->TryGeographicToWorld(Anchor.Latitude,Anchor.Longitude,A)
				&&AftershockFocusLocalMetresToLatLon(0,0,FVector2D(10.,0.),Lat,Lon)&&Terrain->TryGeographicToWorld(Lat,Lon,B);
			Check(TEXT("ten_geographic_metres_are_exactly_one_thousand_world_cm"),Projected&&FMath::Abs(B.X-A.X-1000.)<.01&&FMath::Abs(B.Y-A.Y)<.01);
			double BackLat=0.,BackLon=0.;Check(TEXT("world_to_wgs84_roundtrip_uses_same_source_frame"),Projected&&Terrain->TryWorldToGeographic(A,BackLat,BackLon)&&FMath::Abs(BackLat-Anchor.Latitude)<1.e-9&&FMath::Abs(BackLon-Anchor.Longitude)<1.e-9);
			const FIntRect Supported=Terrain->GetSupportedGeographicCellBounds();const FIntPoint OutsideChunk=UTerrainGridSubsystem::CellToChunk(Supported.Max+FIntPoint(50,50));
			const int32 OriginalCells=Terrain->Cells.Num();const uint64 Revision=Terrain->GetTerrainRevision();
			Check(TEXT("outside_source_chunk_is_rejected_without_terrain_mutation"),!Terrain->EnsureChunkGenerated(OutsideChunk,Error)&&Terrain->Cells.Num()==OriginalCells&&Terrain->GetTerrainRevision()==Revision);
			Check(TEXT("inside_source_expansion_uses_real_heights_without_authored_continuation"),Terrain->EnsureChunkGenerated(FIntPoint(5,0),Error)
				&&Terrain->GetCell(FIntPoint(125,0)).bHasRealElevationSample&&!Terrain->GetCell(FIntPoint(125,0)).bAuthoredContinuation);
			FAftershockTerrainSaveState Expanded;Terrain->CaptureSaveState(Expanded);Check(TEXT("expanded_source_save_roundtrip_keeps_real_generation_policy"),Terrain->ValidateSaveState(Expanded,Error)&&Terrain->RestoreSaveState(Expanded,Error)&&Terrain->IsUsingRealGeography());
			Terrain->GenerateProceduralTerrain(125,125,1000.f,55153);
			const bool LowerReady=Terrain->ApplyOfflineGeography(5,1,true,3);FAftershockTerrainSaveState LowerSaved;
			Terrain->CaptureSaveState(LowerSaved);const float LowerCentre=Terrain->GetCell(FIntPoint(62,62)).Elevation;
			Check(TEXT("lower_flank_actual_terrain_save_has_explicit_focus_one_and_real_elevation"),LowerReady&&LowerSaved.GeographyIndex==5&&LowerSaved.GeographyFocusIndex==1
				&&LowerSaved.ContinuationGeneratorVersion==3&&Terrain->ValidateSaveState(LowerSaved,Error)&&LowerCentre>110000.f&&LowerCentre<130000.f);
			Terrain->GenerateProceduralTerrain(125,125,1000.f,55153);const bool SummitReady=Terrain->ApplyOfflineGeography(5,0,true,3);
			FAftershockTerrainSaveState SummitSaved;Terrain->CaptureSaveState(SummitSaved);
			Check(TEXT("old_summit_and_new_lower_flank_restore_their_recorded_frames_without_redirect"),SummitReady&&Terrain->ValidateSaveState(SummitSaved,Error)
				&&Terrain->RestoreSaveState(LowerSaved,Error)&&Terrain->GeographyFocusIndex==1&&Terrain->GetCell(FIntPoint(62,62)).Elevation==LowerCentre
				&&Terrain->RestoreSaveState(SummitSaved,Error)&&Terrain->GeographyFocusIndex==0&&Terrain->GetCell(FIntPoint(62,62)).RealElevationMetres>1800.f);
			Terrain->GenerateProceduralTerrain(50,50,500.f,1337);Check(TEXT("explicit_legacy_fixture_is_version_one"),Terrain->ApplyLegacyOfflineGeography(0,0,false)&&!Terrain->IsUsingRealGeography());
			FAftershockTerrainSaveState Legacy;Terrain->CaptureSaveState(Legacy);Check(TEXT("version_one_save_stays_readable_without_regeneration"),Legacy.ContinuationGeneratorVersion==1&&Terrain->ValidateSaveState(Legacy,Error)&&Terrain->RestoreSaveState(Legacy,Error)&&!Terrain->IsUsingRealGeography());
		}
		World->DestroyWorld(false);GEngine->DestroyWorldContext(World);World->RemoveFromRoot();
	}
	const FString Folder=FPaths::ProjectSavedDir()/TEXT("PresentationValidation");IFileManager::Get().MakeDirectory(*Folder,true);
	const FString Json=FString::Printf(TEXT("{\"schema_version\":1,\"scope\":\"Native real-source decode, WGS84 local metre scale, immutable bounded source expansion and V1/V2 save policy. No image, GPU, full settlement, calibrated hazard, river bathymetry or geotechnical survey claim.\",\"focuses\":%d,\"passed\":%d,\"failed\":%d,\"checks\":[%s]}"),Focuses,Passed,Failed,*FString::Join(Checks,TEXT(",")));
	const bool Saved=FFileHelper::SaveStringToFile(Json,*(Folder/TEXT("real_geography_audit.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_REAL_GEOGRAPHY_RESULT focuses=%d passed=%d failed=%d saved=%d"),Focuses,Passed,Failed,Saved);
	return Failed==0&&Saved?0:1;
}
#else
UAftershockRealGeographyAuditCommandlet::UAftershockRealGeographyAuditCommandlet(){}
int32 UAftershockRealGeographyAuditCommandlet::Main(const FString&){return 1;}
#endif
