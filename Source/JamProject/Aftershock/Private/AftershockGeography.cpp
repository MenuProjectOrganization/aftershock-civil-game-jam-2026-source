#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/AftershockRegionRules.h"

#include <limits>
#include "Misc/Compression.h"
#include "Misc/Base64.h"

namespace
{
	struct FRegionFocusRange { int32 FirstFocus; int32 FocusCount; };
	#include "AftershockOfflineElevation.inl"
	#include "AftershockRegionCatalog.inl"
	struct FAdditionalGeographicFocus
	{
		int32 RegionIndex,FocusIndex,SourceFocusIndex;
		bool bDefaultForNewGame;
		FAftershockGeographicLocation Location;
	};
	#include "AftershockAdditionalGeographicFocus.inl"
	const FAdditionalGeographicFocus* AdditionalFocus(int32 RegionIndex,int32 FocusIndex)
	{
		for(const auto& Focus:AdditionalGeographicFocuses)
			if(Focus.RegionIndex==RegionIndex&&Focus.FocusIndex==FocusIndex)return &Focus;
		return nullptr;
	}
	struct FCompiledGeographicTile
	{
		double West=0.,South=0.,East=0.,North=0.;
		const uint8* Compressed=nullptr;
		int32 CompressedBytes=0,UncompressedBytes=0;
		bool bCoastlineSource=false;
	};
	#include "AftershockOfflineGeographicTiles.inl"
	struct FCompiledGeographicDetailTile
	{
		double West=0.,South=0.,East=0.,North=0.;
		const ANSICHAR* const* Base64Chunks=nullptr;
		int32 ChunkCount=0,CompressedBytes=0,UncompressedBytes=0;
		bool bCoastlineSource=false;
	};
	#include "AftershockOfflineGeographicDetail.inl"
	#include "AftershockOfflineRegionalOverview.inl"
	constexpr int32 RealDemSide=65,RealMaskSide=257,RealDemCount=RealDemSide*RealDemSide,RealMaskCount=RealMaskSide*RealMaskSide;
	constexpr int32 RealPayloadBytes=RealDemCount*sizeof(float)+RealMaskCount*(sizeof(uint8)+sizeof(uint16)+sizeof(int16));
	constexpr int32 DetailSide=257,DetailCount=DetailSide*DetailSide,DetailPayloadBytes=DetailCount*9;
	constexpr int32 OverviewPayloadBytes=DetailCount*5;
	static_assert(DetailPayloadBytes*UE_ARRAY_COUNT(CompiledGeographicDetailTiles)<=32*1024*1024,"Detail cache exceeds32MiB source budget");
	struct FDecodedGeographicTile
	{
		bool bAttempted=false,bReady=false;
		TArray<float> Heights;
		TArray<uint8> Flags;
		TArray<uint16> RiverDistanceDm;
		TArray<int16> CoastDistanceDm;
	};
	TArray<FDecodedGeographicTile>& RealTileCache()
	{static TArray<FDecodedGeographicTile> Cache;return Cache;}
	TArray<FDecodedGeographicTile>& DetailTileCache()
	{static TArray<FDecodedGeographicTile> Cache;return Cache;}
	struct FDecodedRegionalOverview
	{
		int32 RegionIndex=INDEX_NONE;uint64 LastUse=0;TArray<float> Heights;TArray<uint8> Flags;
	};
	TArray<FDecodedRegionalOverview>& OverviewCache()
	{static TArray<FDecodedRegionalOverview> Cache;return Cache;}

	static_assert(UE_ARRAY_COUNT(GeographicRegions) == UE_ARRAY_COUNT(GeographicRegionRules), "Region/rules mismatch");
	static_assert(UE_ARRAY_COUNT(GeographicFocuses) == UE_ARRAY_COUNT(GeographicFocusRules), "Focus/rules mismatch");
	static_assert(UE_ARRAY_COUNT(GeographicFocuses) == UE_ARRAY_COUNT(OfflineElevationMetres), "Focus/DEM mismatch");
	static_assert(UE_ARRAY_COUNT(GeographicFocuses) == UE_ARRAY_COUNT(OfflineElevationFocusReady), "Focus/readiness mismatch");
	static_assert(UE_ARRAY_COUNT(GeographicFocuses) == UE_ARRAY_COUNT(CompiledGeographicTiles), "Focus/real-source mismatch");
	static_assert(UE_ARRAY_COUNT(GeographicFocuses) == UE_ARRAY_COUNT(CompiledGeographicDetailTiles), "Focus/detail-source mismatch");
	static_assert(UE_ARRAY_COUNT(GeographicRegions) == UE_ARRAY_COUNT(CompiledRegionalOverviewTiles), "Region/overview-source mismatch");
	static_assert(static_cast<int32>(EDisasterType::Drought) + 1 == FAftershockRegionRules::HazardCount, "Update authored hazard order");

	int32 FlatFocusIndex(int32 RegionIndex, int32 FocusIndex)
	{
		if (RegionIndex < 0 || RegionIndex >= static_cast<int32>(UE_ARRAY_COUNT(GeographicRegions))) return INDEX_NONE;
		const FRegionFocusRange& Region = GeographicRegions[RegionIndex];
		if(const auto* Additional=AdditionalFocus(RegionIndex,FocusIndex))
			return Additional->SourceFocusIndex>=0&&Additional->SourceFocusIndex<Region.FocusCount?Region.FirstFocus+Additional->SourceFocusIndex:INDEX_NONE;
		return FocusIndex >= 0 && FocusIndex < Region.FocusCount ? Region.FirstFocus + FocusIndex : INDEX_NONE;
	}

	const FDecodedGeographicTile* DecodeRealTile(int32 FlatIndex)
	{
		if(FlatIndex<0||FlatIndex>=static_cast<int32>(UE_ARRAY_COUNT(CompiledGeographicTiles)))return nullptr;
		// Read-only source cache; game/editor callers run on the game thread. No
		// UObject/world ownership, provider request, per-cell copy or tick exists.
		auto& Cache=RealTileCache();
		if(Cache.IsEmpty())Cache.SetNum(UE_ARRAY_COUNT(CompiledGeographicTiles));
		auto& Decoded=Cache[FlatIndex];if(Decoded.bAttempted)return Decoded.bReady?&Decoded:nullptr;
		Decoded.bAttempted=true;const auto& Source=CompiledGeographicTiles[FlatIndex];
		if(!Source.Compressed||Source.CompressedBytes<=0||Source.UncompressedBytes!=RealPayloadBytes
			||!FMath::IsFinite(Source.West)||!FMath::IsFinite(Source.South)||!FMath::IsFinite(Source.East)||!FMath::IsFinite(Source.North)
			||Source.West>=Source.East||Source.South>=Source.North)return nullptr;
		TArray<uint8> Bytes;Bytes.SetNumUninitialized(RealPayloadBytes);
		if(!FCompression::UncompressMemory(NAME_Zlib,Bytes.GetData(),Bytes.Num(),Source.Compressed,Source.CompressedBytes))return nullptr;
		Decoded.Heights.SetNumUninitialized(RealDemCount);Decoded.Flags.SetNumUninitialized(RealMaskCount);
		Decoded.RiverDistanceDm.SetNumUninitialized(RealMaskCount);Decoded.CoastDistanceDm.SetNumUninitialized(RealMaskCount);
		int32 Offset=0;
		const auto Copy=[&](void* Destination,int32 Count){FMemory::Memcpy(Destination,Bytes.GetData()+Offset,Count);Offset+=Count;};
		Copy(Decoded.Heights.GetData(),RealDemCount*sizeof(float));Copy(Decoded.Flags.GetData(),RealMaskCount);
		Copy(Decoded.RiverDistanceDm.GetData(),RealMaskCount*sizeof(uint16));Copy(Decoded.CoastDistanceDm.GetData(),RealMaskCount*sizeof(int16));
		for(float Height:Decoded.Heights)if(!FMath::IsFinite(Height)||Height<=-12000.f||Height>=9000.f)return nullptr;
		for(uint8 Flags:Decoded.Flags)if((Flags&~31)!=0)return nullptr;
		Decoded.bReady=true;return &Decoded;
	}
	const FDecodedGeographicTile* DecodeDetailTile(int32 FlatIndex)
	{
		if(FlatIndex<0||FlatIndex>=static_cast<int32>(UE_ARRAY_COUNT(CompiledGeographicDetailTiles)))return nullptr;
		auto& Cache=DetailTileCache();if(Cache.IsEmpty())Cache.SetNum(UE_ARRAY_COUNT(CompiledGeographicDetailTiles));
		auto& Decoded=Cache[FlatIndex];if(Decoded.bAttempted)return Decoded.bReady?&Decoded:nullptr;
		Decoded.bAttempted=true;const auto& Source=CompiledGeographicDetailTiles[FlatIndex];
		if(!Source.Base64Chunks||Source.ChunkCount<1||Source.ChunkCount>172||Source.CompressedBytes<1||Source.CompressedBytes>1024*1024
			||Source.UncompressedBytes!=DetailPayloadBytes||!FMath::IsFinite(Source.West)||!FMath::IsFinite(Source.East)
			||!FMath::IsFinite(Source.South)||!FMath::IsFinite(Source.North)||Source.West>=Source.East||Source.South>=Source.North)return nullptr;
		const int32 EncodedBytes=((Source.CompressedBytes+2)/3)*4;FString Encoded;Encoded.Reserve(EncodedBytes);
		for(int32 I=0;I<Source.ChunkCount;++I)
		{
			const ANSICHAR* Chunk=Source.Base64Chunks[I];if(!Chunk)return nullptr;
			const int32 Length=FCStringAnsi::Strlen(Chunk);if(Length<1||Length>8192||Encoded.Len()+Length>EncodedBytes)return nullptr;
			Encoded.Append(UTF8_TO_TCHAR(Chunk));
		}
		TArray<uint8> Compressed;if(Encoded.Len()!=EncodedBytes||!FBase64::Decode(Encoded,Compressed)||Compressed.Num()!=Source.CompressedBytes)return nullptr;
		TArray<uint8> Bytes;Bytes.SetNumUninitialized(DetailPayloadBytes);
		if(!FCompression::UncompressMemory(NAME_Zlib,Bytes.GetData(),Bytes.Num(),Compressed.GetData(),Compressed.Num()))return nullptr;
		Decoded.Heights.SetNumUninitialized(DetailCount);Decoded.Flags.SetNumUninitialized(DetailCount);
		Decoded.RiverDistanceDm.SetNumUninitialized(DetailCount);Decoded.CoastDistanceDm.SetNumUninitialized(DetailCount);
		int32 Offset=0;const auto Copy=[&](void* Destination,int32 Count){FMemory::Memcpy(Destination,Bytes.GetData()+Offset,Count);Offset+=Count;};
		Copy(Decoded.Heights.GetData(),DetailCount*4);Copy(Decoded.Flags.GetData(),DetailCount);Copy(Decoded.RiverDistanceDm.GetData(),DetailCount*2);Copy(Decoded.CoastDistanceDm.GetData(),DetailCount*2);
		for(float Height:Decoded.Heights)if(!FMath::IsFinite(Height)||Height<=-12000.f||Height>=9000.f)return nullptr;
		for(uint8 Flags:Decoded.Flags)if((Flags&~63)!=0||((Flags&1)!=0&&(Flags&62)!=0))return nullptr;
		Decoded.bReady=true;return &Decoded;
	}
	const FDecodedRegionalOverview* DecodeOverview(int32 RegionIndex)
	{
		if(RegionIndex<0||RegionIndex>=static_cast<int32>(UE_ARRAY_COUNT(CompiledRegionalOverviewTiles)))return nullptr;
		auto& Cache=OverviewCache();static uint64 Use=0;++Use;
		for(auto& Row:Cache)if(Row.RegionIndex==RegionIndex){Row.LastUse=Use;return &Row;}
		const auto& Source=CompiledRegionalOverviewTiles[RegionIndex];
		if(!Source.Base64Chunks||Source.ChunkCount<1||Source.ChunkCount>172||Source.CompressedBytes<1||Source.CompressedBytes>1024*1024||Source.UncompressedBytes!=OverviewPayloadBytes)return nullptr;
		const int32 EncodedBytes=((Source.CompressedBytes+2)/3)*4;FString Encoded;Encoded.Reserve(EncodedBytes);
		for(int32 I=0;I<Source.ChunkCount;++I)
		{
			const ANSICHAR* Chunk=Source.Base64Chunks[I];if(!Chunk)return nullptr;
			const int32 Length=FCStringAnsi::Strlen(Chunk);if(Length<1||Length>8192||Encoded.Len()+Length>EncodedBytes)return nullptr;
			Encoded.Append(UTF8_TO_TCHAR(Chunk));
		}
		TArray<uint8> Packed,Raw;if(Encoded.Len()!=EncodedBytes||!FBase64::Decode(Encoded,Packed)||Packed.Num()!=Source.CompressedBytes)return nullptr;
		Raw.SetNumUninitialized(OverviewPayloadBytes);if(!FCompression::UncompressMemory(NAME_Zlib,Raw.GetData(),Raw.Num(),Packed.GetData(),Packed.Num()))return nullptr;
		FDecodedRegionalOverview Decoded;Decoded.RegionIndex=RegionIndex;Decoded.LastUse=Use;Decoded.Heights.SetNumUninitialized(DetailCount);Decoded.Flags.SetNumUninitialized(DetailCount);
		FMemory::Memcpy(Decoded.Heights.GetData(),Raw.GetData(),DetailCount*4);FMemory::Memcpy(Decoded.Flags.GetData(),Raw.GetData()+DetailCount*4,DetailCount);
		for(float Height:Decoded.Heights)if(!FMath::IsFinite(Height)||Height<=-12000.f||Height>=9000.f)return nullptr;
		for(uint8 Flags:Decoded.Flags)if((Flags&~19)!=0||((Flags&1)!=0&&(Flags&18)!=0))return nullptr;
		if(Cache.Num()<2)return &Cache.Add_GetRef(MoveTemp(Decoded));
		const int32 Victim=Cache[0].LastUse<=Cache[1].LastUse?0:1;Cache[Victim]=MoveTemp(Decoded);return &Cache[Victim];
	}

	template<typename T> float SampleRealRaster(const TArray<T>& Values,int32 Side,double U,double V)
	{
		const double X=U*(Side-1),Y=V*(Side-1);const int32 X0=FMath::FloorToInt(X),Y0=FMath::FloorToInt(Y);
		const int32 X1=FMath::Min(Side-1,X0+1),Y1=FMath::Min(Side-1,Y0+1);
		return static_cast<float>(FMath::Lerp(FMath::Lerp(static_cast<double>(Values[X0+Y0*Side]),static_cast<double>(Values[X1+Y0*Side]),X-X0),
			FMath::Lerp(static_cast<double>(Values[X0+Y1*Side]),static_cast<double>(Values[X1+Y1*Side]),X-X0),Y-Y0));
	}
	bool MetresPerDegree(int32 RegionIndex,int32 FocusIndex,double& East,double& North)
	{
		const auto* Focus=TryGetAftershockGeographyFocus(RegionIndex,FocusIndex);if(!Focus)return false;
		const double Phi=FMath::DegreesToRadians(Focus->Latitude),Sin=FMath::Sin(Phi);
		constexpr double A=6378137.,E2=.0066943799901413165;
		const double D=1.-E2*Sin*Sin;
		East=A/FMath::Sqrt(D)*FMath::Cos(Phi)*(PI/180.);
		North=A*(1.-E2)/FMath::Pow(D,1.5)*(PI/180.);
		return FMath::IsFinite(East)&&FMath::IsFinite(North)&&East>1.&&North>1.;
	}

	FVector EarthNormal(double Latitude, double Longitude)
	{
		if (!FMath::IsFinite(Latitude) || !FMath::IsFinite(Longitude)) return FVector::UpVector;
		const double Lat = FMath::DegreesToRadians(Latitude), Lon = FMath::DegreesToRadians(Longitude);
		return FVector(FMath::Cos(Lat)*FMath::Cos(Lon), FMath::Cos(Lat)*FMath::Sin(Lon), FMath::Sin(Lat));
	}
}

bool IsAftershockFocusGeodataReady(int32 RegionIndex,int32 FocusIndex)
{return DecodeRealTile(FlatFocusIndex(RegionIndex,FocusIndex))!=nullptr;}

bool GetAftershockFocusGeodataBounds(int32 RegionIndex,int32 FocusIndex,FAftershockGeographicBounds& Out)
{
	const int32 Flat=FlatFocusIndex(RegionIndex,FocusIndex);if(!DecodeRealTile(Flat))return false;
	const auto& Source=CompiledGeographicTiles[Flat];Out={Source.West,Source.South,Source.East,Source.North};return true;
}

bool TrySampleAftershockFocusGeodata(int32 RegionIndex,int32 FocusIndex,double Latitude,double Longitude,FAftershockGeographicTerrainSample& Out)
{
	if(!FMath::IsFinite(Latitude)||!FMath::IsFinite(Longitude))return false;
	const int32 Flat=FlatFocusIndex(RegionIndex,FocusIndex);const auto* Data=DecodeRealTile(Flat);if(!Data)return false;
	const auto& Source=CompiledGeographicTiles[Flat];
	// Permit only micrometre-scale arithmetic roundoff at an exact source edge;
	// a real coordinate outside the footprint is never clamped into the source.
	constexpr double EdgeRoundoffDegrees=1.e-11;
	if(Longitude<Source.West-EdgeRoundoffDegrees||Longitude>Source.East+EdgeRoundoffDegrees||Latitude<Source.South-EdgeRoundoffDegrees||Latitude>Source.North+EdgeRoundoffDegrees)return false;
	const double U=FMath::Clamp((Longitude-Source.West)/(Source.East-Source.West),0.,1.),V=FMath::Clamp((Source.North-Latitude)/(Source.North-Source.South),0.,1.);
	FAftershockGeographicTerrainSample Sample;Sample.ElevationMetres=SampleRealRaster(Data->Heights,RealDemSide,U,V);
	const int32 X=FMath::Clamp(FMath::RoundToInt(U*(RealMaskSide-1)),0,RealMaskSide-1),Y=FMath::Clamp(FMath::RoundToInt(V*(RealMaskSide-1)),0,RealMaskSide-1);
	Sample.SurfaceFlags=Data->Flags[X+Y*RealMaskSide];Sample.bOcean=(Sample.SurfaceFlags&1)!=0;
	Sample.bInlandWater=(Sample.SurfaceFlags&(2|16))!=0;Sample.bMappedForest=(Sample.SurfaceFlags&4)!=0;Sample.bMappedAgriculture=(Sample.SurfaceFlags&8)!=0;
	Sample.RiverDistanceMetres=SampleRealRaster(Data->RiverDistanceDm,RealMaskSide,U,V)*.1f;
	Sample.SignedCoastDistanceMetres=SampleRealRaster(Data->CoastDistanceDm,RealMaskSide,U,V)*.1f;
	Sample.bHasCoastlineSource=Source.bCoastlineSource;Out=Sample;return true;
}

bool IsAftershockFocusDetailGeodataReady(int32 RegionIndex,int32 FocusIndex)
{return DecodeDetailTile(FlatFocusIndex(RegionIndex,FocusIndex))!=nullptr;}

bool GetAftershockFocusDetailGeodataBounds(int32 RegionIndex,int32 FocusIndex,FAftershockGeographicBounds& Out)
{
	const int32 Flat=FlatFocusIndex(RegionIndex,FocusIndex);if(!DecodeDetailTile(Flat))return false;
	const auto& Source=CompiledGeographicDetailTiles[Flat];Out={Source.West,Source.South,Source.East,Source.North};return true;
}

bool TrySampleAftershockFocusDetailGeodata(int32 RegionIndex,int32 FocusIndex,double Latitude,double Longitude,FAftershockGeographicTerrainSample& Out)
{
	if(!FMath::IsFinite(Latitude)||!FMath::IsFinite(Longitude))return false;
	const int32 Flat=FlatFocusIndex(RegionIndex,FocusIndex);const auto* Data=DecodeDetailTile(Flat);if(!Data)return false;
	const auto& Source=CompiledGeographicDetailTiles[Flat];constexpr double Roundoff=1.e-11;
	if(Longitude<Source.West-Roundoff||Longitude>Source.East+Roundoff||Latitude<Source.South-Roundoff||Latitude>Source.North+Roundoff)return false;
	const double U=FMath::Clamp((Longitude-Source.West)/(Source.East-Source.West),0.,1.),V=FMath::Clamp((Source.North-Latitude)/(Source.North-Source.South),0.,1.);
	FAftershockGeographicTerrainSample Sample;Sample.ElevationMetres=SampleRealRaster(Data->Heights,DetailSide,U,V);
	const int32 X=FMath::Clamp(FMath::RoundToInt(U*(DetailSide-1)),0,DetailSide-1),Y=FMath::Clamp(FMath::RoundToInt(V*(DetailSide-1)),0,DetailSide-1);
	Sample.SurfaceFlags=Data->Flags[X+Y*DetailSide];Sample.bOcean=(Sample.SurfaceFlags&1)!=0;Sample.bInlandWater=(Sample.SurfaceFlags&(2|16))!=0;
	Sample.bIntermittentWaterway=(Sample.SurfaceFlags&32)!=0;Sample.bMappedForest=(Sample.SurfaceFlags&4)!=0;Sample.bMappedAgriculture=(Sample.SurfaceFlags&8)!=0;
	Sample.RiverDistanceMetres=SampleRealRaster(Data->RiverDistanceDm,DetailSide,U,V)*.1f;Sample.SignedCoastDistanceMetres=SampleRealRaster(Data->CoastDistanceDm,DetailSide,U,V)*.1f;
	Sample.bHasCoastlineSource=Source.bCoastlineSource;Out=Sample;return true;
}

bool GetAftershockRegionalContextBounds(int32 RegionIndex,FAftershockGeographicBounds& Out)
{
	if(RegionIndex<0||RegionIndex>=static_cast<int32>(UE_ARRAY_COUNT(CompiledRegionalOverviewTiles)))return false;
	const auto& Source=CompiledRegionalOverviewTiles[RegionIndex];
	if(!Source.Base64Chunks||Source.UncompressedBytes!=OverviewPayloadBytes||!FMath::IsFinite(Source.West)||!FMath::IsFinite(Source.South)
		||!FMath::IsFinite(Source.East)||!FMath::IsFinite(Source.North)||Source.West>=Source.East||Source.South>=Source.North)return false;
	Out={Source.West,Source.South,Source.East,Source.North};return true;
}

bool TrySampleAftershockRegionalContext(int32 RegionIndex,double Latitude,double Longitude,FAftershockGeographicTerrainSample& Out)
{
	FAftershockGeographicBounds Bounds;if(!GetAftershockRegionalContextBounds(RegionIndex,Bounds)||!FMath::IsFinite(Latitude)||!FMath::IsFinite(Longitude))return false;
	constexpr double Roundoff=1.e-11;if(Longitude<Bounds.West-Roundoff||Longitude>Bounds.East+Roundoff||Latitude<Bounds.South-Roundoff||Latitude>Bounds.North+Roundoff)return false;
	const auto* Data=DecodeOverview(RegionIndex);if(!Data)return false;
	const double U=FMath::Clamp((Longitude-Bounds.West)/(Bounds.East-Bounds.West),0.,1.),V=FMath::Clamp((Bounds.North-Latitude)/(Bounds.North-Bounds.South),0.,1.);
	const int32 X=FMath::Clamp(FMath::RoundToInt(U*256),0,256),Y=FMath::Clamp(FMath::RoundToInt(V*256),0,256);
	FAftershockGeographicTerrainSample Sample;Sample.ElevationMetres=SampleRealRaster(Data->Heights,DetailSide,U,V);Sample.SurfaceFlags=Data->Flags[X+Y*DetailSide];
	Sample.bOcean=(Sample.SurfaceFlags&1)!=0;Sample.bInlandWater=(Sample.SurfaceFlags&18)!=0;Sample.bHasCoastlineSource=true;Out=Sample;return true;
}

bool AftershockFocusLatLonToLocalMetres(int32 RegionIndex,int32 FocusIndex,double Latitude,double Longitude,FVector2D& Out)
{
	const auto* Focus=TryGetAftershockGeographyFocus(RegionIndex,FocusIndex);double East=0.,North=0.;
	if(!Focus||!FMath::IsFinite(Latitude)||!FMath::IsFinite(Longitude)||FMath::Abs(Latitude)>85.||FMath::Abs(Longitude)>180.||!MetresPerDegree(RegionIndex,FocusIndex,East,North))return false;
	Out=FVector2D((Longitude-Focus->Longitude)*East,(Focus->Latitude-Latitude)*North);return true;
}

bool AftershockFocusLocalMetresToLatLon(int32 RegionIndex,int32 FocusIndex,FVector2D Metres,double& OutLatitude,double& OutLongitude)
{
	const auto* Focus=TryGetAftershockGeographyFocus(RegionIndex,FocusIndex);double East=0.,North=0.;
	if(!Focus||Metres.ContainsNaN()||!MetresPerDegree(RegionIndex,FocusIndex,East,North))return false;
	const double Lat=Focus->Latitude-Metres.Y/North,Lon=Focus->Longitude+Metres.X/East;
	if(!FMath::IsFinite(Lat)||!FMath::IsFinite(Lon)||FMath::Abs(Lat)>85.||FMath::Abs(Lon)>180.)return false;
	OutLatitude=Lat;OutLongitude=Lon;return true;
}

const TCHAR* GetAftershockGeodataAttribution()
{return TEXT("© OpenStreetMap contributors (ODbL) · Arazi: Mapzen / USGS / Copernicus · Renk: EOX2016(CCBY4)");}

const TCHAR* GetAftershockGeodataLicenseNotice()
{
	return TEXT("Map data © OpenStreetMap contributors, ODbL1.0. https://www.openstreetmap.org/copyright\n")
		TEXT("Terrain: Mapzen/Tilezen source mosaic (including USGS, Copernicus, LINZ and NOAA); source-specific attribution: https://github.com/tilezen/joerd/blob/master/docs/attribution.md\n")
		TEXT("USGS3DEP source exports: public-domain bare-earth data. Regional context: NaturalEarth public-domain generalized1:10million vectors; not10metre resolution.\n")
		TEXT("produced using Copernicus WorldDEM-30 © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018 provided under COPERNICUS by the European Union and ESA; all rights reserved\n")
		TEXT("Copernicus licence: https://dataspace.copernicus.eu/explore-data/data-collections/copernicus-contributing-missions/collections-description/COP-DEM\n")
		TEXT("Monthly climatology: NASA POWER/MERRA2,2001-2025,0.5°latitude×0.625°longitude grid; not a local weather station or forecast. https://power.larc.nasa.gov/docs/methodology/citations/\n")
		TEXT("EOxCloudless https://cloudless.eox.at by EOX IT Services GmbH (Contains modified Copernicus Sentinel data 2016 & 2017). CC BY4.0: https://creativecommons.org/licenses/by/4.0/ . Historical true-colour composite, reprojected/resampled for regional game presentation; not current imagery or physically measured surface albedo. https://cloudless.eox.at/license-non-commercial\n")
		TEXT("Local elevation/OSM source windows and regional context have distinct scope. Procedural city layouts, broad geology colours and scenario hazard weights are authored game interpretations, not surveyed parcels or forecasts.");
}

FAftershockGeographicCacheStats GetAftershockGeodataCacheStats()
{
	FAftershockGeographicCacheStats Stats;const auto& Cache=RealTileCache();Stats.DecodedAllocatedBytes=Cache.GetAllocatedSize();
	for(const auto& Tile:Cache)
	{
		Stats.ReadyTiles+=Tile.bReady?1:0;Stats.AttemptedTiles+=Tile.bAttempted?1:0;
		Stats.DecodedAllocatedBytes+=Tile.Heights.GetAllocatedSize()+Tile.Flags.GetAllocatedSize()+Tile.RiverDistanceDm.GetAllocatedSize()+Tile.CoastDistanceDm.GetAllocatedSize();
	}
	for(const auto& Tile:CompiledGeographicTiles)Stats.CompiledCompressedBytes+=Tile.CompressedBytes;
	const auto& Detail=DetailTileCache();Stats.DetailDecodedAllocatedBytes=Detail.GetAllocatedSize();
	for(const auto& Tile:Detail)
	{
		Stats.DetailReadyTiles+=Tile.bReady?1:0;Stats.DetailAttemptedTiles+=Tile.bAttempted?1:0;
		Stats.DetailDecodedAllocatedBytes+=Tile.Heights.GetAllocatedSize()+Tile.Flags.GetAllocatedSize()+Tile.RiverDistanceDm.GetAllocatedSize()+Tile.CoastDistanceDm.GetAllocatedSize();
	}
	for(const auto& Tile:CompiledGeographicDetailTiles)Stats.DetailCompiledCompressedBytes+=Tile.CompressedBytes;
	const auto& Overview=OverviewCache();Stats.OverviewResidentTiles=Overview.Num();Stats.OverviewDecodedAllocatedBytes=Overview.GetAllocatedSize();
	for(const auto& Tile:Overview)Stats.OverviewDecodedAllocatedBytes+=Tile.Heights.GetAllocatedSize()+Tile.Flags.GetAllocatedSize();
	for(const auto& Tile:CompiledRegionalOverviewTiles)Stats.OverviewCompiledCompressedBytes+=Tile.CompressedBytes;
	return Stats;
}

int32 GetAftershockGeographyCount()
{
	return static_cast<int32>(UE_ARRAY_COUNT(GeographicRegions));
}

int32 FindAftershockGeographyIndex(FName RegionId)
{
	if (RegionId.IsNone()) return INDEX_NONE;
	for (int32 Index = 0; Index < GetAftershockGeographyCount(); ++Index)
		if (FName(GeographicRegionRules[Index].RegionId) == RegionId) return Index;
	return INDEX_NONE;
}

const FAftershockGeographicLocation& GetAftershockGeography(int32 ScenarioIndex)
{
	return GetAftershockGeographyFocus(FMath::Clamp(ScenarioIndex, 0, GetAftershockGeographyCount()-1), 0);
}

int32 GetAftershockGeographyFocusCount(int32 RegionIndex)
{
	if(RegionIndex<0||RegionIndex>=GetAftershockGeographyCount())return 0;
	int32 Count=GeographicRegions[RegionIndex].FocusCount;
	for(const auto& Focus:AdditionalGeographicFocuses)if(Focus.RegionIndex==RegionIndex)++Count;
	return Count;
}

int32 GetAftershockDefaultNewGameFocusIndex(int32 RegionIndex)
{
	if(RegionIndex<0||RegionIndex>=GetAftershockGeographyCount())return INDEX_NONE;
	for(const auto& Focus:AdditionalGeographicFocuses)if(Focus.RegionIndex==RegionIndex&&Focus.bDefaultForNewGame)return Focus.FocusIndex;
	return 0;
}

int32 GetAftershockFocusSourceIndex(int32 RegionIndex,int32 FocusIndex)
{return FlatFocusIndex(RegionIndex,FocusIndex);}

int32 FindAftershockGeographyFocusIndex(int32 RegionIndex, FName FocusId)
{
	if (FocusId.IsNone()) return INDEX_NONE;
	for (int32 Index = 0; Index < GetAftershockGeographyFocusCount(RegionIndex); ++Index)
		if (FName(GetAftershockGeographyFocus(RegionIndex, Index).FocusId) == FocusId) return Index;
	return INDEX_NONE;
}

const FAftershockGeographicLocation* TryGetAftershockGeographyFocus(int32 RegionIndex, int32 FocusIndex)
{
	if(const auto* Additional=AdditionalFocus(RegionIndex,FocusIndex))return &Additional->Location;
	const int32 FlatIndex = FlatFocusIndex(RegionIndex, FocusIndex);
	if(RegionIndex==5&&FocusIndex==0&&FlatIndex!=INDEX_NONE)
	{
		// Display-only qualification. The legacy ID, anchor, source, index and
		// all recorded save coordinates remain exactly unchanged.
		static const FAftershockGeographicLocation Summit=[]()
		{auto Site=GeographicFocuses[5];Site.FocusLabel=TEXT("Etna üst yamaç / bilimsel inceleme — başlangıç şehri yok");return Site;}();
		return &Summit;
	}
	return FlatIndex != INDEX_NONE ? &GeographicFocuses[FlatIndex] : nullptr;
}

const FAftershockGeographicLocation& GetAftershockGeographyFocus(int32 RegionIndex, int32 FocusIndex)
{
	const FAftershockGeographicLocation* Focus = TryGetAftershockGeographyFocus(RegionIndex, FocusIndex);
	return Focus ? *Focus : GeographicFocuses[0];
}

bool IsAftershockGeographyFocusReady(int32 RegionIndex, int32 FocusIndex)
{
	const int32 FlatIndex = FlatFocusIndex(RegionIndex, FocusIndex);
	return FlatIndex != INDEX_NONE && OfflineElevationFocusReady[FlatIndex];
}

float SampleAftershockElevationMetres(int32 ScenarioIndex, float U, float V)
{
	return SampleAftershockFocusElevationMetres(FMath::Clamp(ScenarioIndex, 0, GetAftershockGeographyCount()-1), 0, U, V);
}

float SampleAftershockFocusElevationMetres(int32 RegionIndex, int32 FocusIndex, float U, float V)
{
	const int32 FlatIndex = FlatFocusIndex(RegionIndex, FocusIndex);
	if (FlatIndex == INDEX_NONE || !OfflineElevationFocusReady[FlatIndex] || !FMath::IsFinite(U) || !FMath::IsFinite(V))
		return std::numeric_limits<float>::quiet_NaN();
	const float* Data = OfflineElevationMetres[FlatIndex];
	const float X = FMath::Clamp(U,0.f,1.f)*8.f, Y = FMath::Clamp(V,0.f,1.f)*8.f;
	const int32 X0=FMath::FloorToInt(X), Y0=FMath::FloorToInt(Y), X1=FMath::Min(8,X0+1), Y1=FMath::Min(8,Y0+1);
	return FMath::Lerp(FMath::Lerp(Data[X0+Y0*9],Data[X1+Y0*9],X-X0),FMath::Lerp(Data[X0+Y1*9],Data[X1+Y1*9],X-X0),Y-Y0);
}

const FAftershockRegionRules& GetAftershockRegionRules(int32 RegionIndex)
{
	return GeographicRegionRules[FMath::Clamp(RegionIndex, 0, GetAftershockGeographyCount()-1)];
}

const FAftershockLocalFocusRules& GetAftershockLocalFocusRules(int32 RegionIndex, int32 FocusIndex)
{
	const int32 FlatIndex = FlatFocusIndex(RegionIndex, FocusIndex);
	static const FAftershockLocalFocusRules InvalidFocus;
	return FlatIndex != INDEX_NONE ? GeographicFocusRules[FlatIndex] : InvalidFocus;
}

void GetAftershockGeographicBasis(int32 AnchorScenarioIndex, FVector& East, FVector& South, FVector& Up)
{
	GetAftershockGeographicFocusBasis(FMath::Clamp(AnchorScenarioIndex, 0, GetAftershockGeographyCount()-1), 0, East, South, Up);
}

void GetAftershockGeographicFocusBasis(int32 RegionIndex, int32 FocusIndex, FVector& East, FVector& South, FVector& Up)
{
	const FAftershockGeographicLocation& Site = GetAftershockGeographyFocus(RegionIndex, FocusIndex);
	const double Lon = FMath::DegreesToRadians(Site.Longitude);
	Up = EarthNormal(Site.Latitude,Site.Longitude);
	East = FVector(-FMath::Sin(Lon),FMath::Cos(Lon),0.);
	South = -FVector::CrossProduct(Up,East).GetSafeNormal();
}

FVector AftershockLatLonToFocusGlobeNormal(double Latitude, double Longitude, int32 RegionIndex, int32 FocusIndex)
{
	FVector East,South,Up;
	GetAftershockGeographicFocusBasis(RegionIndex,FocusIndex,East,South,Up);
	const FVector N=EarthNormal(Latitude,Longitude);
	return FVector(FVector::DotProduct(N,East),FVector::DotProduct(N,South),FVector::DotProduct(N,Up)).GetSafeNormal();
}

FVector AftershockLatLonToGlobeNormal(double Latitude, double Longitude, int32 AnchorScenarioIndex)
{
	return AftershockLatLonToFocusGlobeNormal(Latitude,Longitude,FMath::Clamp(AnchorScenarioIndex,0,GetAftershockGeographyCount()-1),0);
}
