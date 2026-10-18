#include "Aftershock/Public/AftershockRegionEnvironmentProfile.h"
#include "Aftershock/Public/AftershockGeography.h"

namespace
{
	struct FCompiledMonth {float T2MC=0.f,PrecipMmPerDay=0.f,RHPercent=0.f,WindMps=0.f;};
	struct FCompiledMonthlyClimate
	{
		bool bReady=false;float SourceElevationM=0.f;int32 StartYear=0,EndYear=0;FCompiledMonth Months[12];
	};
	#include "AftershockOfflineMonthlyClimate.inl"
	// Original colour choices interpret cited REGIONAL context, not mapped bedrock at every vertex.
	const FAftershockRegionEnvironmentProfile Profiles[]={
		{TEXT("atakent"),EAftershockRegionalBiome::TemperateUrban,{.40f,.34f,.25f},{.41f,.39f,.35f},TEXT("Marmara havzası ve göl çevresi; parsel zemini için etüt gerekir."),TEXT("https://depremzemin.ibb.gov.tr/uploads/prefix-kucukcekmece-tsunami-666af1fdc297f.pdf")},
		{TEXT("dereyolu"),EAftershockRegionalBiome::HumidBlackSea,{.25f,.24f,.15f},{.32f,.35f,.33f},TEXT("Doğu Karadeniz'in dik kıyı vadisi; akarsu ve orman kaynağı ayrıca haritalıdır."),TEXT("https://eticaret.mta.gov.tr/index.php?product_id=8039&route=product%2Fproduct")},
		{TEXT("yelova"),EAftershockRegionalBiome::Mediterranean,{.45f,.36f,.24f},{.50f,.48f,.42f},TEXT("Belen–Amanos dağ geçidi ve kırıklı bölgesel topoğrafya; yükseklik deprem güvenliği değildir."),TEXT("https://www.mta.gov.tr/v3.0/hizmetler/yenilenmis-diri-fay-haritalari")},
		{TEXT("kiyikent"),EAftershockRegionalBiome::Mediterranean,{.48f,.40f,.28f},{.45f,.43f,.36f},TEXT("Samandağ kıyı ovası, Asi deltası ve çevre yamaçları."),TEXT("https://www.mta.gov.tr/v3.0/hizmetler/jeoloji-haritalari")},
		{TEXT("mugla"),EAftershockRegionalBiome::Mediterranean,{.40f,.32f,.19f},{.43f,.43f,.38f},TEXT("Marmaris orman-kıyı ve dağlık yarımada bağlamı; güncel yangın bir senaryodur."),TEXT("https://ekotaban.tarimorman.gov.tr/alan/50")},
		{TEXT("etna"),EAftershockRegionalBiome::VolcanicMediterranean,{.23f,.21f,.18f},{.18f,.18f,.17f},TEXT("Etna volkanik konisi, lav arazisi ve zirve kraterleri; DEM tarihli yüzeyi temsil eder."),TEXT("https://data.ingv.it/dataset/712")},
		{TEXT("konya"),EAftershockRegionalBiome::SemiAridSteppe,{.53f,.44f,.28f},{.49f,.46f,.36f},TEXT("Karapınar kapalı havzası; yeraltı suyu çekimi ve obruk bağlamı, obruk çözücüsü değildir."),TEXT("https://airbornescience.nasa.gov/content/Land_Subsidence_and_Its_Relations_with_Sinkhole_Activity_in_Karap%C4%B1nar_Region_Turkey_A_Multi")},
		{TEXT("miami_keys"),EAftershockRegionalBiome::TropicalMaritime,{.64f,.57f,.39f},{.65f,.63f,.51f},TEXT("Pleistosen kireçtaşlı alçak adalar, kıyı ve kanallar; yapay dağ üretilmez."),TEXT("https://www.usgs.gov/publications/geology-and-hydrogeology-florida-keys")},
		{TEXT("cyprus"),EAftershockRegionalBiome::Mediterranean,{.53f,.42f,.27f},{.48f,.46f,.39f},TEXT("Troodos ofiyoliti, Mesaoria ovası ve Pentadaktylos sırası; üç odak farklı yerel pencerelerdir."),TEXT("https://www.moa.gov.cy/moa/gsd/gsd.nsf/page45_en/page45_en")},
		{TEXT("yakutsk"),EAftershockRegionalBiome::ContinentalTaiga,{.31f,.29f,.22f},{.42f,.42f,.39f},TEXT("Lena taşkın ovası ve permafrost bölgesi; donma/çözülme mühendisliği çözücüsü yoktur."),TEXT("https://www.usgs.gov/publications/origin-and-character-loesslike-silt-unglaciated-south-central-yakutia-siberia-ussr")},
		{TEXT("valparaiso"),EAftershockRegionalBiome::Mediterranean,{.47f,.36f,.23f},{.43f,.40f,.35f},TEXT("Şili kıyı yamaçları ve Nazca dalma-batma kenarı; gerçek fay/parsel haritası değildir."),TEXT("https://www.usgs.gov/publications/2017-valparaiso-earthquake-sequence-and-megathrust-patchwork-central-chile")},
		{TEXT("valdivia"),EAftershockRegionalBiome::TemperateRainforest,{.27f,.24f,.17f},{.34f,.36f,.34f},TEXT("Valdivia akarsu ağları ve Şili dalma-batma bağlamı; yerel odak açık okyanus değildir."),TEXT("https://pubs.usgs.gov/circ/c1187/")},
		{TEXT("sendai"),EAftershockRegionalBiome::TemperateCoastal,{.44f,.39f,.27f},{.43f,.43f,.39f},TEXT("Sendai alçak kıyı ovası ve belgelenmiş tsunami tortulları."),TEXT("https://www.gsj.jp/Muse/QR/en/014.html")},
		{TEXT("rotterdam"),EAftershockRegionalBiome::TemperateCoastal,{.34f,.30f,.22f},{.41f,.42f,.40f},TEXT("Ren–Maas delta tortulları; kum, kil ve turba bölgesel bağlamdır, parsel etüdü değildir."),TEXT("https://www.geologischedienst.nl/en/research/safe-liveable-delta/")},
		{TEXT("new_orleans"),EAftershockRegionalBiome::DeltaWetland,{.37f,.31f,.21f},{.40f,.39f,.34f},TEXT("Mississippi alçak delta ovası; çökel sıkışması/çökme, yağış ve fırtına kabarması ayrı süreçlerdir."),TEXT("https://www.usgs.gov/centers/spcmsc/science/subsidence-and-coastal-geomorphic-change-south-central-louisiana")},
		{TEXT("grindavik"),EAftershockRegionalBiome::SubpolarVolcanic,{.24f,.22f,.18f},{.19f,.20f,.20f},TEXT("Reykjanes bazalt lavları ve Sundhnúkur çatlak sırası; yeni yarık konumu tahmin edilmez."),TEXT("https://en.vedur.is/volcanoes/fagradalsfjall-eruption/")},
		{TEXT("vesuvius"),EAftershockRegionalBiome::VolcanicMediterranean,{.32f,.28f,.22f},{.29f,.28f,.27f},TEXT("Somma–Vezüv konisi ve Napoli Körfezi; piroklastik yoğunluk akımı hesaplanmaz."),TEXT("https://www.ov.ingv.it/index.php/monitoraggio-sismico-e-vulcanico/vesuvio/obiettivo-vesuvio")},
		{TEXT("christchurch"),EAftershockRegionalBiome::TemperateCoastal,{.42f,.36f,.24f},{.43f,.43f,.39f},TEXT("Canterbury çakıl-kum-silt-bataklık birikimleri ve Port Hills volkanik kayaçları."),TEXT("https://www.gns.cri.nz/our-science/land-and-marine-geoscience/geology-of-new-zealand/urban-geological-maps/christchurch/")},
		{TEXT("kathmandu"),EAftershockRegionalBiome::MonsoonHighland,{.43f,.33f,.22f},{.44f,.41f,.36f},TEXT("Eski göl tortullarıyla dolu Kathmandu havzası ve çevre sırtlar; genel zemin katsayısı ölçüm değildir."),TEXT("https://www.usgs.gov/programs/earthquake-hazards/science/m78-nepal-earthquake-2015-a-small-push-mt-everest")},
		{TEXT("jakarta"),EAftershockRegionalBiome::TropicalMonsoon,{.38f,.30f,.20f},{.39f,.38f,.34f},TEXT("Kuzey Jakarta alçak kıyı-delta odağı; çökme, gelgit ve yağış aynı olay sayılmaz."),TEXT("https://science.nasa.gov/earth/earth-observatory/as-jakarta-grows-so-do-the-water-issues-148303/")}
	};
}

const FAftershockRegionEnvironmentProfile* TryGetAftershockRegionEnvironmentProfile(int32 RegionIndex)
{
	if(RegionIndex<0||RegionIndex>=GetAftershockGeographyCount()||RegionIndex>=static_cast<int32>(UE_ARRAY_COUNT(Profiles)))return nullptr;
	return FName(Profiles[RegionIndex].RegionId)==FName(GetAftershockGeography(RegionIndex).Id)?&Profiles[RegionIndex]:nullptr;
}

bool TryGetAftershockFocusMonthlyClimate(int32 RegionIndex,int32 FocusIndex,int32 Month1To12,FAftershockMonthlyClimate& Out)
{
	if(!TryGetAftershockGeographyFocus(RegionIndex,FocusIndex)||Month1To12<1||Month1To12>12)return false;
	// Public focus count26, immutable provider windows25: never sum public
	// counts to index source arrays after an additive same-payload focus.
	const int32 Flat=GetAftershockFocusSourceIndex(RegionIndex,FocusIndex);
	if(Flat<0||Flat>=static_cast<int32>(UE_ARRAY_COUNT(CompiledMonthlyClimates)))return false;
	const auto& Row=CompiledMonthlyClimates[Flat];if(!Row.bReady||Row.StartYear<1981||Row.EndYear<Row.StartYear||!FMath::IsFinite(Row.SourceElevationM))return false;
	const auto& Month=Row.Months[Month1To12-1];
	if(!FMath::IsFinite(Month.T2MC)||!FMath::IsFinite(Month.PrecipMmPerDay)||!FMath::IsFinite(Month.RHPercent)||!FMath::IsFinite(Month.WindMps)
		||Month.T2MC< -95.f||Month.T2MC>65.f||Month.PrecipMmPerDay<0.f||Month.PrecipMmPerDay>80.f||Month.RHPercent<0.f||Month.RHPercent>100.f||Month.WindMps<0.f||Month.WindMps>80.f)return false;
	FAftershockMonthlyClimate Climate;Climate.T2MC=Month.T2MC;Climate.PrecipMmPerDay=Month.PrecipMmPerDay;Climate.RHPercent=Month.RHPercent;Climate.WindMps=Month.WindMps;
	Climate.SourceElevationM=Row.SourceElevationM;Climate.StartYear=Row.StartYear;Climate.EndYear=Row.EndYear;Out=Climate;return true;
}

bool TryEstimateAftershockVisualTemperatureAtElevation(const FAftershockMonthlyClimate& Climate,float ElevationMetres,float& OutTemperatureC)
{
	if(!FMath::IsFinite(Climate.T2MC)||!FMath::IsFinite(Climate.SourceElevationM)||!FMath::IsFinite(ElevationMetres)||ElevationMetres< -12000.f||ElevationMetres>9000.f)return false;
	// NASA Glenn's standard tropospheric gradient .00649 C/m; visual approximation,
	// not a site-calibrated lapse rate or a new climate/hazard authority.
	OutTemperatureC=Climate.T2MC-.00649f*(ElevationMetres-Climate.SourceElevationM);return FMath::IsFinite(OutTemperatureC);
}
