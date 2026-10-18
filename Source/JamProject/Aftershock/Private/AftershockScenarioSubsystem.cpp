#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "AftershockSettlementPlanner.h"
#include "AftershockSettlementMorphology.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/WorldClimateSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/AftershockRegionRules.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Engine/GameInstance.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Containers/Deque.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

bool UAftershockScenarioSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return Super::ShouldCreateSubsystem(Outer) && World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UAftershockScenarioSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UTerrainGridSubsystem>();
	Collection.InitializeDependency<UBuildingManagerSubsystem>();
	Collection.InitializeDependency<UEconomyAndStateSubsystem>();
	Collection.InitializeDependency<UWorldClimateSubsystem>();
	Collection.InitializeDependency<UDisasterSimulationSubsystem>();
	Collection.InitializeDependency<URoadNetworkSubsystem>();
	if (UWorld* World = GetWorld())
	{
		World->GetSubsystem<UEconomyAndStateSubsystem>()->OnEconomyUpdated.AddUniqueDynamic(this, &UAftershockScenarioSubsystem::HandleEconomyChanged);
		World->GetSubsystem<UDisasterSimulationSubsystem>()->OnDisasterCompleted.AddUniqueDynamic(this, &UAftershockScenarioSubsystem::HandleDisasterCompleted);
	}
}

bool UAftershockScenarioSubsystem::PrepareScenarioBeforeSettlement()
{
	if(bPrepared)return true;if(!GetWorld())return false;
	UWorld* World = GetWorld();
	UBuildingManagerSubsystem* Buildings = World->GetSubsystem<UBuildingManagerSubsystem>();
	UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>();
	UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>();
	if (!Buildings || !Terrain || !Economy || Buildings->GetActiveBuildingsCount() > 0) return false;
	const auto FailPreparation=[&](const FText& Error)
	{
		bPrepared=false;Definition={};DesignedLots.Empty();AssemblyCells.Empty();bStarterRoadsInitialized=false;StarterRoadCellCount=0;
		if(auto* Saves=World->GetGameInstance()?World->GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>():nullptr)Saves->MarkWorldStartupFailure(World,Error);
		UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_SCENARIO_PREPARATION_FAILED newPopulationSeeded=0 economyInitialized=0 inventedLocationLabel=0"));return false;
	};
	ScenarioIndex = FCString::Atoi(World->URL.GetOption(TEXT("AftershockScenario="), TEXT("0")));
	// An explicit URL option wins so campaign transitions are not pinned by a launch flag.
	if (!World->URL.HasOption(TEXT("AftershockScenario="))) FParse::Value(FCommandLine::Get(), TEXT("AftershockScenario="), ScenarioIndex);
	ScenarioIndex = FMath::Clamp(ScenarioIndex, 0, GetScenarioCount() - 1);
	const auto* Session=World->GetGameInstance()?World->GetGameInstance()->GetSubsystem<UAftershockOpeningSessionSubsystem>():nullptr;
	const FAftershockSessionSettings Settings=Session?Session->GetSettings():FAftershockSessionSettings();
	const bool bConfigured=Session&&Session->HasConfiguredSession();
	const bool bLegacyGeographyAudit=FParse::Param(FCommandLine::Get(),TEXT("AftershockVisualAudit"))&&FParse::Param(FCommandLine::Get(),TEXT("AftershockLegacyGeographyAudit"));
	const bool bUseRealGeography=bConfigured&&!bLegacyGeographyAudit;
	if(bLegacyGeographyAudit)UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_GEOGRAPHY_AUDIT_MODE legacy=1 explicit=1 configured=%d"),bConfigured);
	if(Session&&Session->HasConfiguredSession()&&Settings.Mode==EAftershockPlayMode::Campaign)ScenarioIndex=Settings.CampaignStage;
	Definition = FAftershockScenarioDefinition();
	if (ScenarioIndex == 0)
	{
		Definition.Name = TEXT("1 / 7 — Atakent / İstanbul: Fay Hattı");
		Definition.Briefing = TEXT("Şehir bağlı sokaklarla hazır. Yol ağını büyüt; okul ve hastane önündeki yeşil toplanma alanlarını açık tut. En az bir yapıyı güçlendir ve depremi atlat.");
	}
	else if (ScenarioIndex == 1)
	{
		Definition.Name = TEXT("2 / 7 — Sürmene / Trabzon: Taşkın Ovası");
		Definition.Briefing = TEXT("Kıyıdaki alçak evler taşkına açık. En az 6 kara hücresinin drenajını iyileştir; yüksek kotlu hastane ve toplanma alanlarına erişimi kur ve seli atlat.");
		Definition.Hazard = EDisasterType::Flood;
		Definition.Seed = 7447;
		Definition.StartYear = 2035;
		Definition.StartingBudgetTL = 60000;
		Definition.BaseSeverity = 65.f;
		Definition.InitialAnomalyC = 1.65f;
	}
	else if (ScenarioIndex == 2)
	{
		Definition.Name = TEXT("3 / 7 — Belen / Hatay: Vadi Koridoru");
		Definition.Briefing = TEXT("Vadi rüzgârına açık yapıları ve acil durum merkezlerini gözle. En az 3 yeşil alan oluştur; erişim yollarını bağla ve fırtınayı atlat.");
		Definition.Hazard = EDisasterType::Wind;
		Definition.Seed = 9851;
		Definition.StartYear = 2045;
		Definition.StartingBudgetTL = 70000;
		Definition.BaseSeverity = 78.f;
		Definition.InitialAnomalyC = 1.95f;
	}
	else if (ScenarioIndex == 3)
	{
		Definition.Name = TEXT("4 / 7 — Samandağ / Hatay: Açık Deniz");
		Definition.Briefing = TEXT("Şehir kıyıya ulaştı. Üç yeşil toplanma alanını hastane yol ağına bağla; iç kesimdeki üç acil durum merkezine erişim kur ve tsunamiyi atlat. Nehir barajı tsunami koruması sağlamaz.");
		Definition.Hazard = EDisasterType::Tsunami;
		Definition.Seed = 20261;
		Definition.StartYear = 2055;
		Definition.StartingBudgetTL = 90000;
		Definition.BaseSeverity = 60.f;
		Definition.InitialAnomalyC = 2.2f;
	}
	else if (ScenarioIndex == 4)
	{
		Definition.Name=TEXT("5 / 7 — Marmaris / Muğla: Orman Yangını");
		Definition.Briefing=TEXT("Kurumuş orman yakıtı ve rüzgâr yangını bağlı hücrelerde yayar. İkinci itfaiyeyi kur, yollarını bağla; toplanma alanlarına erişimi koru. Bu varsayımsal yangın geçmiş bir olayın yeniden canlandırması değildir.");
		Definition.Hazard=EDisasterType::Fire; Definition.Seed=43881; Definition.StartYear=2060;
		Definition.StartingBudgetTL=110000; Definition.BaseSeverity=76.f; Definition.InitialAnomalyC=2.3f;
	}
	else if (ScenarioIndex == 5)
	{
		Definition.Name=TEXT("6 / 7 — Etna / Sicilia: Lav ve Kül");
		Definition.Briefing=TEXT("Gerçek Etna bölgesinden örneklenmiş yamaçlarda varsayımsal lav/kül senaryosu. Üç kara parselinin zemin etüdünü al; tahliye yollarını bağla. Yanardağ iklim veya sıcak hava yüzünden patlamaz; yalnız haritalanmış volkan kaynağı etkindir.");
		Definition.Hazard=EDisasterType::Volcano; Definition.Seed=55153; Definition.StartYear=2065;
		Definition.StartingBudgetTL=130000; Definition.BaseSeverity=80.f; Definition.InitialAnomalyC=2.4f;
	}
	else
	{
		Definition.Name=TEXT("7 / 7 — Karapınar / Konya: Tarımsal Kuraklık");
		Definition.Briefing=TEXT("Yağış açığı toprak nemini, tarla verimini ve su rezervlerini turlar boyunca düşürür. İkinci tarlayı kur, gıda zincirine enerji ve yol sağla. Kuraklık binaları yapay biçimde yıkmaz; nüfusun su ve beslenme ihtiyacını yönet.");
		Definition.Hazard=EDisasterType::Drought; Definition.Seed=67319; Definition.StartYear=2070;
		Definition.StartingBudgetTL=150000; Definition.BaseSeverity=82.f; Definition.InitialAnomalyC=2.5f;
	}
	const bool bFreeRegion=Session&&Session->HasConfiguredSession()&&Settings.Mode!=EAftershockPlayMode::Campaign;
	Definition.GeographyIndex=bFreeRegion?FindAftershockGeographyIndex(Settings.RegionID):ScenarioIndex;
	const bool bExplicitFocus=Session&&Session->HasConfiguredSession()&&!Settings.FocusID.IsNone()&&(bFreeRegion||Settings.CampaignStage==ScenarioIndex);
	const int32 Focus=bExplicitFocus?FindAftershockGeographyFocusIndex(Definition.GeographyIndex,Settings.FocusID)
		:Session&&Session->HasConfiguredSession()?GetAftershockDefaultNewGameFocusIndex(Definition.GeographyIndex):0;
	const auto* Location=TryGetAftershockGeographyFocus(Definition.GeographyIndex,Focus);
	if(!Location)return FailPreparation(NSLOCTEXT("Aftershock","InvalidStartupRegion","Seçilen bölge veya mahalle bulunamadı. Yeni Oyun menüsünden geçerli bir yer seçin."));
	const auto& GeographicLocation=*Location;
	if(bFreeRegion)
	{
		Definition.Name=FString::Printf(TEXT("%s — %s"),Settings.Mode==EAftershockPlayMode::Sandbox?TEXT("SERBEST ŞEHİR / TANRI MODU"):TEXT("COĞRAFİ SİMÜLASYON"),GeographicLocation.FocusLabel);
		Definition.Briefing=Settings.Mode==EAftershockPlayMode::Sandbox?TEXT("Afetleri uygun coğrafyalarda kendin başlat. Şehri ve altyapıyı serbestçe geliştir. Otomatik afet takvimi kapalıdır."):TEXT("Afetler bu bölgenin nehir, kıyı, iklim ve jeoloji sözleşmesinden ağırlıklı olarak seçilir. Bu model gerçek afet tahmini değildir. Şehrin tüm ihtiyaçlarını ve erişimini birlikte yönet.");
		Definition.Hazard=GetAftershockRegionRules(Definition.GeographyIndex).PrimaryHazard;
		Definition.Seed=Settings.Seed;Definition.StartYear=2026;Definition.StartingBudgetTL=100000;Definition.BaseSeverity=48.f;Definition.InitialAnomalyC=1.4f;
	}
	Definition.Latitude=GeographicLocation.Latitude; Definition.Longitude=GeographicLocation.Longitude;
	// New player sessions use a ten metre road right of way. Old/unconfigured
	// fixtures and recorded save transforms retain the original five metre grid.
	float NewCellSize = Session && Session->HasConfiguredSession() ? 1000.f : 500.f;
	int32 InitialCitySide = Session && Session->HasConfiguredSession() ? 125 : 50;
	if (World->URL.HasOption(TEXT("GridSide="))) InitialCitySide = FMath::Clamp(FCString::Atoi(World->URL.GetOption(TEXT("GridSide="), TEXT("100"))), 25, 256);
	else FParse::Value(FCommandLine::Get(), TEXT("GridSide="), InitialCitySide);
	if (World->URL.HasOption(TEXT("CellSize="))) NewCellSize = FMath::Clamp(static_cast<float>(FCString::Atof(World->URL.GetOption(TEXT("CellSize="), TEXT("1000")))), 100.f, 5000.f);
	else FParse::Value(FCommandLine::Get(), TEXT("CellSize="), NewCellSize);
	Terrain->GenerateProceduralTerrain(InitialCitySide, InitialCitySide, NewCellSize, Definition.Seed);
	int32 SourceVersion=3;
#if !UE_BUILD_SHIPPING
	// Explicit comparison fixture only. Player sessions always use the current V3 source.
	if(FParse::Param(FCommandLine::Get(),TEXT("AftershockVisualAudit"))&&FParse::Param(FCommandLine::Get(),TEXT("AftershockConfiguredSessionAudit")))
	{
		int32 Requested=3;
		if(FParse::Value(FCommandLine::Get(),TEXT("AftershockAuditSourceVersion="),Requested))
		{
			if(Requested!=2&&Requested!=3)return FailPreparation(NSLOCTEXT("Aftershock","InvalidAuditSourceVersion","Görsel karşılaştırma kaynak sürümü geçersiz."));
			SourceVersion=Requested;
		}
	}
#endif
	const bool bGeographyReady=bUseRealGeography?Terrain->ApplyOfflineGeography(Definition.GeographyIndex,Focus,bFreeRegion,SourceVersion)
		:Terrain->ApplyLegacyOfflineGeography(Definition.GeographyIndex,Focus,bFreeRegion);
	if(!bGeographyReady)return FailPreparation(NSLOCTEXT("Aftershock","StartupGeodataUnavailable","Seçilen yerin arazi ve harita verileri yüklenemedi. Şehir başlatılmadı. Yeni Oyun menüsünden yeniden deneyin veya başka bir bölge seçin."));
	if(bUseRealGeography&&GetAftershockLocalFocusRules(Definition.GeographyIndex,Focus).bHasVolcanicSource)
	{
		// A mapped summit can be outside the initial city frame. Load only its
		// actual source chunk before settlement, never move/clamp the vent or fill
		// the gap with authored land. The normal source/cap validator is retained.
		const auto& RegionRules=GetAftershockRegionRules(Definition.GeographyIndex);
		FAftershockGeographicTerrainSample Source;FIntPoint ActualVent(INDEX_NONE,INDEX_NONE);FText SourceError;
		const bool bMappedSource=Terrain->TrySampleGeographicSource(RegionRules.VolcanoLatitude,RegionRules.VolcanoLongitude,Source)
			&&!Source.bOcean&&Terrain->TryGeographicToCell(RegionRules.VolcanoLatitude,RegionRules.VolcanoLongitude,ActualVent);
		if(!bMappedSource)return FailPreparation(NSLOCTEXT("Aftershock","StartupVolcanoSourceUnavailable","Haritalanmış volkan kaynağı bu bölgenin ayrıntılı veri kapsamında hazır değil. Şehir başlatılmadı; başka bir bölge seçin."));
		const int32 CellsBeforeSource=Terrain->Cells.Num();const FIntPoint SourceChunk=UTerrainGridSubsystem::CellToChunk(ActualVent);
		if((!Terrain->IsValidCoord(ActualVent)&&!Terrain->EnsureChunkGenerated(SourceChunk,SourceError))
			||Terrain->Cells.Num()-CellsBeforeSource>UTerrainGridSubsystem::ChunkSizeCells*UTerrainGridSubsystem::ChunkSizeCells
			||Terrain->VolcanicVentCoord!=ActualVent||!Terrain->IsPlayableCoord(ActualVent)||!Terrain->GetCell(ActualVent).bVolcanicVent)
			return FailPreparation(NSLOCTEXT("Aftershock","StartupVolcanoChunkUnavailable","Volkan kaynağının gerçek arazi verileri yüklenemedi. Şehir başlatılmadı. Yeni Oyun menüsünden başka bir bölge seçin."));
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_VOLCANIC_SOURCE_PREPARED region=%d focus=%d vent=(%d,%d) chunk=(%d,%d) sourceCellsAdded=%d beforeSettlement=1 mappedSourceUnmoved=1 gapCellsGenerated=0"),Definition.GeographyIndex,Focus,ActualVent.X,ActualVent.Y,SourceChunk.X,SourceChunk.Y,Terrain->Cells.Num()-CellsBeforeSource);
	}
	const bool bWantsCoast = (!bFreeRegion && ScenarioIndex == 3)
		|| (bFreeRegion && (GetAftershockLocalFocusRules(Definition.GeographyIndex, Focus).bHasCoast || ScenarioIndex == 3 || Settings.Mode == EAftershockPlayMode::Sandbox));
	if(!bUseRealGeography&&bWantsCoast)Terrain->ConfigureCoastFromSouthernEdge(FMath::RoundToInt(InitialCitySide*.76f));
	Economy->TreasuryTL = Definition.StartingBudgetTL;
	Economy->CurrentYear = Definition.StartYear;
	Economy->CurrentDisasterLevel = bFreeRegion?1:ScenarioIndex + 1;
	bPrepared = true;
	BuildDesignedLots();
	// Crowd initialization may have already drawn the default seed before camera
	// BeginPlay selects this scenario. Rebuild that SAME host from the new grid;
	// its surface builder also replaces collision, river ribbon and fallback ISMs.
	int32 RefreshedTerrainHosts = 0;
	for (TActorIterator<AAftershockCrowdHostActor> It(World); It; ++It)
	{
		It->BuildTerrainVisual(Terrain);
		++RefreshedTerrainHosts;
	}
	if (UCrowdRenderSubsystem* Crowd = World->GetSubsystem<UCrowdRenderSubsystem>()) Crowd->EnsureTerrainVisualBuilt();
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_SCENARIO_TERRAIN_REFRESH seed=%d existingHostsRebuilt=%d duplicateTerrainAuthority=0"), Definition.Seed, RefreshedTerrainHosts);
	World->GetSubsystem<UWorldClimateSubsystem>()->SetScenarioClimate(Definition.StartYear, Definition.InitialAnomalyC, .025f);
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_SCENARIO_PREPARED index=%d seed=%d year=%d authoredLots=%d assemblyCells=%d budget=%lld"),
		ScenarioIndex, Definition.Seed, Definition.StartYear, DesignedLots.Num(), AssemblyCells.Num(), Definition.StartingBudgetTL);
	OnScenarioUpdated.Broadcast();
	return true;
}

void UAftershockScenarioSubsystem::BuildDesignedLots()
{
	UTerrainGridSubsystem* Terrain = GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
	if(Terrain&&Terrain->ContinuationGeneratorVersion<3)
	{
		// Compatibility fixtures/old authored-generation policy must retain their
		// exact core, parity, heights and4x5 district service mix. Never apply a
		// modern real-place morphology profile retroactively to that authority.
		PlannedPublicCorridors.Reset();CityPlanStatistics={};
		BuildLegacyDesignedLots();return;
	}
	UBuildingManagerSubsystem* Buildings=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();
	DesignedLots.Reset(); AssemblyCells.Reset(); PlannedPublicCorridors.Reset(); CityPlanStatistics={};
	int32 RiverX = Terrain->GridWidth / 2;
	const bool bCityDistricts = Terrain->GridWidth > 50 && Terrain->CellSize >= 999.f;
	const FAftershockSettlementMorphology Morphology=GetAftershockSettlementMorphology(Definition.GeographyIndex,Terrain->GeographyFocusIndex);
	const int32 CentreRow=Terrain->GridHeight/2;
	for (int32 X = 0; X < Terrain->GridWidth; ++X) if (Terrain->GetCell(FIntPoint(X, CentreRow)).bIsRiverbed) { RiverX = X; break; }
	const FIntPoint Centre(FMath::Clamp(RiverX - (ScenarioIndex == 1 ? 4 : 7), 10, bCityDistricts ? Terrain->GridWidth-10 : 31), CentreRow);
	const FIntPoint Directions[] = { FIntPoint(0,1), FIntPoint(1,0), FIntPoint(-1,0), FIntPoint(0,-1) };
	TSet<FIntPoint> BankCells;
	TSet<FIntPoint> SeenLand;int32 LandComponents=0,BestLandCount=0;int64 BestCentreDistance=MAX_int64;
	FIntPoint BestTieCoord(MAX_int32,MAX_int32);
	const auto IsInitialLand=[&](FIntPoint Coord)
	{
		if(Coord.X<0||Coord.Y<0||Coord.X>=Terrain->GridWidth||Coord.Y>=Terrain->GridHeight)return false;
		const FGridCell Cell=Terrain->GetCell(Coord);
		return Cell.bIsPlayable&&!Cell.bIsOcean&&!Cell.bIsRiverbed&&!Cell.bVolcanicVent;
	};
	// O(V+E), stable row-major seeds/direction order. A nearest tiny island must
	// not win over a larger actual land bank within the SAME geographic frame.
	// No ocean fill, bridge fabrication or new terrain/focus coordinate exists.
	for(int32 Y=0;Y<Terrain->GridHeight;++Y)for(int32 X=0;X<Terrain->GridWidth;++X)
	{
		const FIntPoint Start(X,Y);if(SeenLand.Contains(Start)||!IsInitialLand(Start))continue;
		++LandComponents;TArray<FIntPoint> Component;Component.Add(Start);SeenLand.Add(Start);
		int64 Nearest=MAX_int64;FIntPoint TieCoord(MAX_int32,MAX_int32);
		for(int32 Head=0;Head<Component.Num();++Head)
		{
			const FIntPoint Coord=Component[Head];const int64 DX=Coord.X-Centre.X,DY=Coord.Y-Centre.Y,Distance=DX*DX+DY*DY;
			if(Distance<Nearest||(Distance==Nearest&&(Coord.Y<TieCoord.Y||(Coord.Y==TieCoord.Y&&Coord.X<TieCoord.X)))){Nearest=Distance;TieCoord=Coord;}
			for(FIntPoint Step:Directions)
			{
				const FIntPoint Next=Coord+Step;if(SeenLand.Contains(Next)||!IsInitialLand(Next))continue;
				SeenLand.Add(Next);Component.Add(Next);
			}
		}
		if(Component.Num()>BestLandCount||(Component.Num()==BestLandCount&&(Nearest<BestCentreDistance
			||(Nearest==BestCentreDistance&&(TieCoord.Y<BestTieCoord.Y||(TieCoord.Y==BestTieCoord.Y&&TieCoord.X<BestTieCoord.X))))))
		{BestLandCount=Component.Num();BestCentreDistance=Nearest;BestTieCoord=TieCoord;BankCells.Reset();for(FIntPoint Coord:Component)BankCells.Add(Coord);}
	}
	TArray<FIntPoint> Candidates;
	const uint32 LayoutSeed=static_cast<uint32>(Definition.Seed)*196314165u+907633515u;
	const int32 LotParityX=bCityDistricts?((Centre.X&1)^static_cast<int32>((LayoutSeed>>3)&1u)):(Centre.X&1);
	const int32 LotParityY=bCityDistricts?static_cast<int32>((LayoutSeed>>7)&1u):1;
	for (const FGridCell& Cell : Terrain->Cells)
	{
		if(bCityDistricts&&(Cell.Coords.X<0||Cell.Coords.Y<0||Cell.Coords.X>=Terrain->GridWidth||Cell.Coords.Y>=Terrain->GridHeight))continue;
		if (!BankCells.Contains(Cell.Coords) || Cell.bVolcanicVent || (Cell.Coords.X&1)!=LotParityX || (Cell.Coords.Y&1)!=LotParityY) continue;
		if(bCityDistricts&&!IsAftershockMorphologyCellSuitable(Morphology,Cell))continue;
		// An eight-neighbour land ring remains free under the two-cell lot parity.
		// Removing any authored lot therefore cannot cut a narrow river-bank route.
		bool bHasPublicAccessRing = true;
		for (int32 DY = -1; DY <= 1 && bHasPublicAccessRing; ++DY)
			for (int32 DX = -1; DX <= 1; ++DX)
				if (!BankCells.Contains(Cell.Coords + FIntPoint(DX, DY))
					||(bCityDistricts&&!IsAftershockMorphologyCellSuitable(Morphology,Terrain->GetCell(Cell.Coords+FIntPoint(DX,DY))))) { bHasPublicAccessRing = false; break; }
		if (bHasPublicAccessRing) Candidates.Add(Cell.Coords);
	}
	TSet<FIntPoint> Used;
	int32 EssentialLotSerial=0;
	const auto AddLot = [&](EBuildingType Type, FIntPoint Desired, int32 Floors, bool bEmergency)
	{
		const int32 Serial=EssentialLotSerial++;
		if(bCityDistricts)Type=ResolveAftershockMorphologyBuildingType(Morphology,Type,LayoutSeed^static_cast<uint32>(Serial+1)*0x9e3779b9u,true);
		const UBuildingData* Catalog=Buildings ? Buildings->GetCatalogData(Type) : nullptr;
		if (!Catalog)
		{
			UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_SCENARIO_LOT_MISSING_CATALOG type=%d authoredLotNotCreated=1"),static_cast<int32>(Type));
			return;
		}
		if(bCityDistricts)
		{
			// Core public services keep catalogue/stat identities. In a low-rise
			// village, incompatible apartments/offices become real house/small
			// employment records BEFORE catalogue lookup; no below-minimum tower.
			// The legacy44-lot campaign fixtures retain their original layout.
			FRandomStream Local(static_cast<int32>(LayoutSeed^(static_cast<uint32>(Serial+1)*0x9e3779b9u)));
			FIntPoint Offset=Desired-Centre;
			const bool bShoreCottage=ScenarioIndex==3&&Type==EBuildingType::House&&Serial>=11&&Serial<15;
			if(!bShoreCottage)for(uint32 Turn=0;Turn<((LayoutSeed>>11)&3u);++Turn)Offset=FIntPoint(-Offset.Y,Offset.X);
			Desired=Centre+Offset+FIntPoint(Local.RandRange(-1,1)*2,Local.RandRange(-1,1)*2);
			if(Type==EBuildingType::House||Type==EBuildingType::Apartment||Type==EBuildingType::Office)
				Floors=Local.RandRange(Catalog->FloorsMin,FMath::Max(Catalog->FloorsMin,FMath::Min(Catalog->FloorsMax,GetAftershockMorphologyFloorCap(Morphology,Type))));
		}
		// Exactly the same catalogue floor limits used by PlaceBuilding. The
		// authored plan must describe the resulting structure before seeding it.
		const int32 FloorCap=bCityDistricts?FMath::Min(Catalog->FloorsMax,GetAftershockMorphologyFloorCap(Morphology,Type)):Catalog->FloorsMax;
		if(FloorCap<Catalog->FloorsMin)
		{
			UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_MORPHOLOGY_CATALOG_CONFLICT profile=%s type=%d catalogueMin=%d profileCap=%d lotSkipped=1"),Morphology.Id,static_cast<int32>(Type),Catalog->FloorsMin,FloorCap);return;
		}
		const int32 CanonicalFloors=FMath::Clamp(Floors,Catalog->FloorsMin,FloorCap);
		float MinimumShelterElevation=-MAX_flt;
		if (!bCityDistricts && ScenarioIndex==1 && Type==EBuildingType::Shelter)
		{
			for (const FAftershockScenarioLot& Existing:DesignedLots)
				if (Existing.Foundation==EFoundationQuality::Strong)
					MinimumShelterElevation=FMath::Max(MinimumShelterElevation,Terrain->GetCell(Existing.Coord).Elevation);
			// Fixed flood neighbourhood: a shelter belongs above the existing
			// dry public centres, not in the nearby low hollow. This authored
			// siting margin is not a measured flood-level/engineering guarantee.
			if (MinimumShelterElevation>-MAX_flt) MinimumShelterElevation+=20.f;
		}
		FIntPoint Best(INDEX_NONE, INDEX_NONE); double BestScore = TNumericLimits<double>::Max();
		for (FIntPoint Coord : Candidates)
		{
			if (Used.Contains(Coord)) continue;
			const FGridCell Cell = Terrain->GetCell(Coord);
			if (Cell.Elevation<MinimumShelterElevation) continue;
			const double D2 = static_cast<double>(FMath::Square(Coord.X - Desired.X) + FMath::Square(Coord.Y - Desired.Y));
			const double Score = D2 * 20.0 + (bEmergency ? -Cell.Elevation * .35 + Cell.RiverProximity * 180.0 : 0.0)
				+(bCityDistricts?FMath::Square(Cell.Slope)*3.f:0.f);
			if (Score < BestScore) { Best = Coord; BestScore = Score; }
		}
		if (Best.X == INDEX_NONE) return;
		Used.Add(Best);
		FAftershockScenarioLot& Lot = DesignedLots.AddDefaulted_GetRef();
		Lot.Coord = Best; Lot.Type = Type; Lot.Floors = CanonicalFloors;
		Lot.Foundation = bEmergency ? EFoundationQuality::Strong : EFoundationQuality::Medium;
		if (Floors!=CanonicalFloors)
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SCENARIO_LOT_CANONICAL_FLOORS type=%d requested=%d authored=%d catalogueMin=%d catalogueMax=%d"),static_cast<int32>(Type),Floors,CanonicalFloors,Catalog->FloorsMin,Catalog->FloorsMax);
		if (Type==EBuildingType::Shelter)
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SHELTER_SITING coord=%s elevationCm=%.3f minimumAuthoredElevationCm=%.3f strongFoundation=%d sameConnectedLandBank=1 publicAccessRing=1"),*Best.ToString(),Terrain->GetCell(Best).Elevation,MinimumShelterElevation,bEmergency);
	};
	// Schools/hospital/fire station stand on higher ground; residential blocks
	// leave a free grid lane between every pair of lots for player-built roads.
	AddLot(EBuildingType::Hospital, Centre + FIntPoint(-2, -6), 3, true);
	AddLot(EBuildingType::School, Centre + FIntPoint(2, -6), 2, true);
	AddLot(EBuildingType::FireStation, Centre + FIntPoint(-4, 2), 2, true);
	for (int32 I = 0; I < 12; ++I)
	{
		FIntPoint Desired = Centre + FIntPoint(-4 + (I % 4) * 2, -2 + (I / 4) * 2);
		// The coastal expansion retains real empty-land access rings. Four existing
		// cottages form a shore district while emergency centres stay inland.
		if (ScenarioIndex == 3 && I >= 8) Desired.Y = FMath::RoundToInt(Terrain->GridHeight*.66f) + ((I - 8) / 2) * 2;
		AddLot(EBuildingType::House, Desired, 1 + I % 2, false);
	}
	for (int32 I = 0; I < 8; ++I) AddLot(EBuildingType::Apartment, Centre + FIntPoint(-4 + (I % 4) * 2, 4 + (I / 4) * 2), 4 + I % 3, false);
	for (int32 I = 0; I < 3; ++I) AddLot(EBuildingType::Office, Centre + FIntPoint(4, -4 + I * 2), 5 + I, false);
	AddLot(EBuildingType::Factory, Centre + FIntPoint(4, 6), 2, false);
	AddLot(EBuildingType::GreenSpace, Centre + FIntPoint(0, -4), 1, false);
	// A functioning starter economy, with ONE population authority in CityLife.
	// Job capacities are services, never extra residents or disaster victims.
	const EBuildingType StarterServices[] = { EBuildingType::Farm, EBuildingType::LivestockFarm,
		EBuildingType::Bakery, EBuildingType::Restaurant, EBuildingType::PowerPlant,
		EBuildingType::Generator, EBuildingType::TaxOffice, EBuildingType::Courthouse,
		EBuildingType::PoliceStation, EBuildingType::BusTerminal, EBuildingType::BusStop,
		EBuildingType::BusStop, EBuildingType::Shelter, EBuildingType::WaterPlant,
		EBuildingType::Warehouse, EBuildingType::Substation };
	for (int32 I=0;I<UE_ARRAY_COUNT(StarterServices);++I)
	{
		FIntPoint Desired=Centre+FIntPoint(-8+(I%5)*2,-10+(I/5)*2);
		if (I==10) Desired=Centre+FIntPoint(-4,2); // Residential frontage stop.
		if (I==11) Desired=Centre+FIntPoint(-4,-10); // Distinct employment/services stop.
		AddLot(StarterServices[I],Desired,1,StarterServices[I]==EBuildingType::Shelter);
	}
	if (bCityDistricts)
	{
		// The planner produces canonical lots and public corridors only. Placement,
		// resident creation, charges and saved state keep their existing owners.
		ExpandAftershockSettlementPlan(*Terrain,*Buildings,BankCells,Centre,Definition.Seed,
			Definition.GeographyIndex,Terrain->GeographyFocusIndex,DesignedLots,PlannedPublicCorridors,CityPlanStatistics);
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SETTLEMENT_MORPHOLOGY profile=%s sourceFocus=%d eligibleCoreParcels=%d authoredLots=%d cap=%d ordinaryHouseMaxFloors=2 apartmentCap=%d officeCap=%d publicServiceCap=%d exactCensus=0 sourceElevationCmUnchanged=1 savedLayoutsNeverRerolled=1"),
			Morphology.Id,Terrain->GeographyFocusIndex,Candidates.Num(),DesignedLots.Num(),Morphology.MaximumLots,Morphology.ApartmentFloorCap,Morphology.OfficeFloorCap,Morphology.PublicServiceFloorCap);
	}
	CityPlanStatistics.ChosenLandComponentCells=BankCells.Num();CityPlanStatistics.LargestLandComponentCells=BestLandCount;CityPlanStatistics.LandComponentsConsidered=LandComponents;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SETTLEMENT_LAND_BANK sourceVersion=3 components=%d chosenCells=%d largestCells=%d nearestChosenCoord=%s originalFocusUnchanged=1 initialFrameOnly=1"),LandComponents,BankCells.Num(),BestLandCount,*BestTieCoord.ToString());

	for (int32 I = 0; I < FMath::Min(3, DesignedLots.Num()); ++I)
	{
		for (FIntPoint Direction : Directions)
		{
			const FIntPoint Coord = DesignedLots[I].Coord + Direction;
			if (Terrain->IsPlayableCoord(Coord) && !Terrain->GetCell(Coord).bIsRiverbed && !Used.Contains(Coord) && !AssemblyCells.Contains(Coord))
			{
				AssemblyCells.Add(Coord);
				FGridCell Cell = Terrain->GetCell(Coord); Cell.bIsEmergencyAssemblyArea = true; Terrain->SetCell(Coord, Cell);
				break;
			}
		}
	}
	if (!bCityDistricts && ScenarioIndex == 1 && Definition.Seed == 7447 && !AssemblyCells.IsEmpty())
	{
		// Four existing cottages deliberately occupy the fixed scenario's low bank.
		// These authored coordinates are not generated by a hazard/display solver.
		// They are narrow-frontage exceptions to the main district's 3x3 land ring.
		const FIntPoint RiversideCottages[] = { FIntPoint(26,21), FIntPoint(25,19), FIntPoint(24,17), FIntPoint(22,13) };
		TArray<int32> HouseIndices;
		for (int32 I = 0; I < DesignedLots.Num(); ++I)
			if (DesignedLots[I].Type == EBuildingType::House) HouseIndices.Add(I);
		int32 RelocatedCottages = 0;
		for (int32 I = 0; I < UE_ARRAY_COUNT(RiversideCottages) && I < HouseIndices.Num(); ++I)
		{
			const FIntPoint Proposed = RiversideCottages[I];
			if (!Terrain->IsPlayableCoord(Proposed) || !BankCells.Contains(Proposed) || Used.Contains(Proposed)
				|| Terrain->GetCell(Proposed).bIsRiverbed || Terrain->GetCell(Proposed).OccupyingBuildingID != INDEX_NONE
				|| Terrain->GetCell(Proposed).bIsEmergencyAssemblyArea || AssemblyCells.Contains(Proposed)) continue;
			const int32 HouseIndex = HouseIndices[I];
			TSet<FIntPoint> TrialOccupied = Used;
			TrialOccupied.Remove(DesignedLots[HouseIndex].Coord);
			TrialOccupied.Add(Proposed);
			TSet<FIntPoint> Accessible;
			TArray<FIntPoint> WalkQueue;
			Accessible.Add(AssemblyCells[0]); WalkQueue.Add(AssemblyCells[0]);
			for (int32 Head = 0; Head < WalkQueue.Num(); ++Head)
			{
				for (FIntPoint Direction : Directions)
				{
					const FIntPoint Coord = WalkQueue[Head] + Direction;
					if (!Terrain->IsPlayableCoord(Coord) || Terrain->GetCell(Coord).bIsRiverbed
						|| TrialOccupied.Contains(Coord) || Accessible.Contains(Coord)) continue;
					Accessible.Add(Coord); WalkQueue.Add(Coord);
				}
			}
			bool bPreservesPublicAccess = true;
			for (FIntPoint Assembly : AssemblyCells) bPreservesPublicAccess &= Accessible.Contains(Assembly);
			for (int32 LotIndex = 0; LotIndex < DesignedLots.Num() && bPreservesPublicAccess; ++LotIndex)
			{
				const FIntPoint LotCoord = LotIndex == HouseIndex ? Proposed : DesignedLots[LotIndex].Coord;
				bool bHasConnectedFrontage = false;
				for (FIntPoint Direction : Directions) bHasConnectedFrontage |= Accessible.Contains(LotCoord + Direction);
				bPreservesPublicAccess &= bHasConnectedFrontage;
			}
			if (!bPreservesPublicAccess)
			{
				UE_LOG(LogTemp, Warning, TEXT("AFTERSHOCK_RIVERSIDE_COTTAGE_REJECTED coord=%s reason=DisconnectedPublicFrontage"), *Proposed.ToString());
				continue;
			}
			const FIntPoint Previous = DesignedLots[HouseIndex].Coord;
			Used = MoveTemp(TrialOccupied);
			DesignedLots[HouseIndex].Coord = Proposed;
			++RelocatedCottages;
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_RIVERSIDE_COTTAGE old=%s coord=%s elevationCm=%.2f riverDistanceCm=%.2f allLotFrontagesConnected=1 populationAdded=0"),
				*Previous.ToString(), *Proposed.ToString(), Terrain->GetCell(Proposed).Elevation, Terrain->GetCell(Proposed).RiverDistance);
		}
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_RIVERSIDE_LAYOUT cottages=%d totalLots=%d emergencyCentresMoved=0 automaticRoads=0"), RelocatedCottages, DesignedLots.Num());
	}
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_SCENARIO_ACCESS seed=%d connectedBankCells=%d servicedLotCandidates=%d authoredLots=%d assemblyCells=%d automaticRoads=0"),
		Definition.Seed, BankCells.Num(), Candidates.Num(), DesignedLots.Num(), AssemblyCells.Num());
}

void UAftershockScenarioSubsystem::BuildAssemblyMarkers()
{
    if(AssemblyMarkerHost||!GetWorld())return;
    auto* Terrain=GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
    if(!Terrain)return;
    auto* Sign=LoadObject<UStaticMesh>(nullptr,TEXT("/Game/Environment/Meshes/Realistic/SM_EmergencyAssemblySign.SM_EmergencyAssemblySign"));
    if(!Sign)
    {
        UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_ASSEMBLY_SIGN_MISSING markers=0 horizontalRoadOverlayFallback=0 canonicalAssemblyUnchanged=1"));
        return;
    }
    const FBox Native=Sign->GetBoundingBox();const FVector NativeSize=Native.GetSize();
    if(FMath::Abs(Native.Min.Z)>.5||FMath::Abs(NativeSize.X-84.)>.5||FMath::Abs(NativeSize.Z-230.)>.5)
    {
        UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_ASSEMBLY_SIGN_BOUNDS_INVALID size=%s canonicalAssemblyUnchanged=1"),*NativeSize.ToCompactString());
        return;
    }
    FActorSpawnParameters Params;Params.ObjectFlags|=RF_Transient;
    AssemblyMarkerHost=GetWorld()->SpawnActor<AActor>(FVector::ZeroVector,FRotator::ZeroRotator,Params);
    if(!AssemblyMarkerHost)return;
    AssemblyMarkerHost->PrimaryActorTick.bCanEverTick=false;
    auto* Root=NewObject<USceneComponent>(AssemblyMarkerHost);
    AssemblyMarkerHost->SetRootComponent(Root);AssemblyMarkerHost->AddInstanceComponent(Root);Root->RegisterComponent();
    auto* Markers=NewObject<UInstancedStaticMeshComponent>(AssemblyMarkerHost,TEXT("EmergencyAssemblySigns"));
    AssemblyMarkerHost->AddInstanceComponent(Markers);Markers->SetupAttachment(Root);
    Markers->SetCollisionEnabled(ECollisionEnabled::NoCollision);Markers->SetCanEverAffectNavigation(false);
    Markers->PrimaryComponentTick.bCanEverTick=false;Markers->SetComponentTickEnabled(false);
    Markers->SetMobility(EComponentMobility::Movable);Markers->SetStaticMesh(Sign);
    // Keep native shared MetalPost+AssemblyIcon slots. No per-sign MID or huge green ground plane.
    Markers->SetCastShadow(true);Markers->SetCullDistances(0,0);Markers->RegisterComponent();
    const FIntPoint DX(1,0),DY(0,1);
    const auto IsRoad=[Terrain](FIntPoint At)
    {FGridCell C;return Terrain->TryGetCell(At,C)&&C.bHasRoad;};
    int32 Placed=0,Unsupported=0;
    for(const FIntPoint Coord:AssemblyCells)
    {
        FGridCell Cell;
        if(!Terrain->TryGetCell(Coord,Cell)||!Cell.bIsPlayable||Cell.bIsOcean||Cell.bIsRiverbed
            ||Cell.OccupyingBuildingID!=INDEX_NONE||!Cell.bIsEmergencyAssemblyArea||Cell.BridgeID!=INDEX_NONE)
        {++Unsupported;continue;}
        const int32 XNeighbors=static_cast<int32>(IsRoad(Coord+DX))+static_cast<int32>(IsRoad(Coord-DX));
        const int32 YNeighbors=static_cast<int32>(IsRoad(Coord+DY))+static_cast<int32>(IsRoad(Coord-DY));
        const FVector Along=XNeighbors>=YNeighbors?FVector::ForwardVector:FVector::RightVector;
        const FVector Across(-Along.Y,Along.X,0.);
        const FVector Centre=Terrain->GridToWorld(Coord);
        const uint32 Salt=GetTypeHash(Coord);
        bool bPlaced=false;
        for(int32 Attempt=0;Attempt<4&&!bPlaced;++Attempt)
        {
            const int32 Corner=(Attempt+static_cast<int32>(Salt%4u))%4;
            const double Side=(Corner&1)?-1.:1.,End=(Corner&2)?-1.:1.;
            FVector Position=Centre+Along*(Terrain->CellSize*.31*End)+Across*(Terrain->CellSize*.44*Side);
            const FRotator Rotation(0.f,Along.Rotation().Yaw+(Side<0.?180.f:0.f),0.f);
            // Only small physical post footing; source land/road cells never move.
            // Native84cm board lies ALONG the verge; its22cm footing stays in this same public cell.
            bool bSupported=Terrain->WorldToGrid(Position)==Coord;
            float Lowest=MAX_flt,Highest=-MAX_flt,Height=0.f;
            for(int32 CornerX:{-1,1})for(int32 CornerY:{-1,1})
            {
                const FVector Sample=Position+Along*(CornerX*11.)+Across*(CornerY*11.);
                float H=0.f;
                if(Terrain->WorldToGrid(Sample)!=Coord||!Terrain->TrySampleWorldElevation(Sample,H)||!FMath::IsFinite(H))
                {bSupported=false;continue;}
                Lowest=FMath::Min(Lowest,H);Highest=FMath::Max(Highest,H);
            }
            if(!bSupported||Highest-Lowest>4.f||!Terrain->TrySampleWorldElevation(Position,Height))continue;
            Position.Z=Height+(Cell.bHasRoad?8.f:0.f)-.7f;
            Markers->AddInstance(FTransform(Rotation,Position,FVector::OneVector),true);
            ++Placed;bPlaced=true;
        }
        if(!bPlaced)++Unsupported;
    }
    Markers->MarkRenderStateDirty();
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_ASSEMBLY_SIGNS placed=%d canonicalCells=%d unsupportedSkipped=%d signHeightCm=230 boardWidthCm=84 footingWidthCm=22 nativeScale=1 sameCanonicalCell=1 horizontalRoadOverlay=0 perSignActors=0 perSignMID=0 capacityOrNavigationWrites=0"),
        Placed,AssemblyCells.Num(),Unsupported);
}

void UAftershockScenarioSubsystem::NotifySettlementSeeded()
{
	if (!bPrepared) return;
	SeedStarterStreets();
	if (UCityLifeSubsystem* Life=GetWorld()->GetSubsystem<UCityLifeSubsystem>()) Life->InitializeForCity();
	BuildAssemblyMarkers(); OnScenarioUpdated.Broadcast();
}

void UAftershockScenarioSubsystem::SeedStarterStreets()
{
	if (bStarterRoadsInitialized || AssemblyCells.IsEmpty() || !GetWorld()) return;
	UTerrainGridSubsystem* Terrain = GetWorld()->GetSubsystem<UTerrainGridSubsystem>();
	URoadNetworkSubsystem* Roads = GetWorld()->GetSubsystem<URoadNetworkSubsystem>();
	if (!Terrain || !Roads || Roads->CountRoadCells() != 0) return;
	const FIntPoint Root = AssemblyCells[0];
	const FIntPoint Directions[] = { FIntPoint(0,1), FIntPoint(1,0), FIntPoint(0,-1), FIntPoint(-1,0) };
	if (Terrain->GridWidth>50)
	{
		// One public-space tree serves the whole authored city. Running four
		// full-grid road quotes per lot made large settlements quadratic.
		TArray<int32> Parent,Distance,Queue;Parent.Init(INDEX_NONE,Terrain->Cells.Num());Distance.Init(MAX_int32,Terrain->Cells.Num());
		const int32 RootIndex=Terrain->CoordToIndex(Root);if(!Parent.IsValidIndex(RootIndex))return;
		Parent[RootIndex]=RootIndex;Distance[RootIndex]=0;Queue.Add(RootIndex);
		int32 RouteVisitedCells=0;
		const bool bPreferPlannedCorridors=Terrain->ContinuationGeneratorVersion>=3&&!PlannedPublicCorridors.IsEmpty();
		if(bPreferPlannedCorridors)
		{
			TSet<int32> Preferred;for(FIntPoint Coord:PlannedPublicCorridors){const int32 Index=Terrain->CoordToIndex(Coord);if(Parent.IsValidIndex(Index))Preferred.Add(Index);}
			struct FRouteEntry{int32 Index,Cost;};TDeque<FRouteEntry> Pending;Pending.PushLast({RootIndex,0});
			TArray<uint8> Settled;Settled.Init(0,Parent.Num());
			FRouteEntry Entry{INDEX_NONE,MAX_int32};
			//0-1 BFS: every settled cell/edge is inspected once. Zero-cost
			// perimeter travel prevents independent root-directed combs across
			// every block interior; off-corridor links still use real safe cells.
			while(Pending.TryPopFirst(Entry))
			{
				const int32 At=Entry.Index;if(Settled[At]||Entry.Cost!=Distance[At])continue;
				Settled[At]=1;++RouteVisitedCells;const FIntPoint Coord=Terrain->IndexToCoord(At);
				for(FIntPoint Step:Directions)
				{
					const FIntPoint Next=Coord+Step;const int32 Index=Terrain->CoordToIndex(Next);
					if(!Parent.IsValidIndex(Index)||Settled[Index]||Terrain->GetCell(Next).OccupyingBuildingID!=INDEX_NONE||Terrain->GetCell(Next).bVolcanicVent||!Roads->IsPublicWalkingCell(Next)||!Roads->CanTraverseWalkingEdge(Coord,Next))continue;
					const int32 Cost=Preferred.Contains(Index)?0:1,NewDistance=Distance[At]+Cost;
					if(NewDistance>=Distance[Index])continue;
					Parent[Index]=At;Distance[Index]=NewDistance;
					if(Cost==0)Pending.PushFirst({Index,NewDistance});else Pending.PushLast({Index,NewDistance});
				}
			}
		}
		else for(int32 Head=0;Head<Queue.Num();++Head)
		{
			// Immutable authored V1 FIFO routing semantics.
			++RouteVisitedCells;const int32 At=Queue[Head];const FIntPoint Coord=Terrain->IndexToCoord(At);
			for(FIntPoint Step:Directions)
			{
				const FIntPoint Next=Coord+Step;const int32 Index=Terrain->CoordToIndex(Next);
				if(!Parent.IsValidIndex(Index)||Parent[Index]!=INDEX_NONE||Terrain->GetCell(Next).OccupyingBuildingID!=INDEX_NONE||Terrain->GetCell(Next).bVolcanicVent||!Roads->IsPublicWalkingCell(Next)||!Roads->CanTraverseWalkingEdge(Coord,Next))continue;
				Parent[Index]=At;Distance[Index]=Distance[At]+1;Queue.Add(Index);
			}
		}
		TSet<int32> StreetIndices;bool bConnected=true;
		const auto AddBranch=[&](int32 Index)
		{
			if(!Parent.IsValidIndex(Index)||Parent[Index]==INDEX_NONE)return false;
			for(int32 Steps=0;Steps<Parent.Num();++Steps)
			{
				if(StreetIndices.Contains(Index))return true;
				StreetIndices.Add(Index);if(Index==RootIndex)return true;Index=Parent[Index];
			}
			return false;
		};
		for(FIntPoint Assembly:AssemblyCells)bConnected&=AddBranch(Terrain->CoordToIndex(Assembly));
		for(const FAftershockScenarioLot& Lot:DesignedLots)
		{
			int32 Best=INDEX_NONE,BestDistance=MAX_int32;
			for(FIntPoint Step:Directions)
			{
				const int32 Index=Terrain->CoordToIndex(Lot.Coord+Step);
				if(Distance.IsValidIndex(Index)&&Distance[Index]<BestDistance){Best=Index;BestDistance=Distance[Index];}
			}
			bConnected&=AddBranch(Best);
		}
		// All populated block perimeter loops become the SAME canonical network.
		// Missing/unsafe gaps are never turned into a bridge or fabricated land.
		int32 ConnectedPlannedCorridors=0;
		for(FIntPoint Coord:PlannedPublicCorridors)
		{
			const int32 Index=Terrain->CoordToIndex(Coord);
			if(Parent.IsValidIndex(Index)&&Parent[Index]!=INDEX_NONE&&AddBranch(Index))++ConnectedPlannedCorridors;
		}
		if(!bConnected){UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_CITY_STREETS_REJECTED disconnectedLot=1 noPartialRoads=1"));return;}
		TArray<int32> Ordered=StreetIndices.Array();Ordered.Sort();TArray<FIntPoint> CityStreets;CityStreets.Reserve(Ordered.Num());
		for(int32 Index:Ordered)CityStreets.Add(Terrain->IndexToCoord(Index));
		if(Roads->SeedInitialRoads(CityStreets))
		{
			bStarterRoadsInitialized=true;StarterRoadCellCount=CityStreets.Num();
			UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_STARTER_CITY index=%d roadCells=%d connectedLots=%d connectedAssemblyAreas=%d sharedRouteTree=1 visitedCells=%d connectedBlockCorridors=%d plannedBlockCorridors=%d corridorCostZero=%d eachBranchCellVisitedOnce=1 playerCanExtend=1"),ScenarioIndex,StarterRoadCellCount,DesignedLots.Num(),AssemblyCells.Num(),RouteVisitedCells,ConnectedPlannedCorridors,PlannedPublicCorridors.Num(),bPreferPlannedCorridors);
		}
		return;
	}
	TSet<FIntPoint> StreetSet;
	TArray<FIntPoint> Streets;
	const auto AddPath = [&](const TArray<FIntPoint>& Path)
	{
		for (FIntPoint Coord : Path) if (!StreetSet.Contains(Coord)) { StreetSet.Add(Coord); Streets.Add(Coord); }
	};
	for (FIntPoint Assembly : AssemblyCells)
	{
		const FAftershockRoadQuote Quote = Roads->QuoteRoad(Root, Assembly);
		if (!Quote.bRouteValid) return;
		AddPath(Quote.Path);
	}
	for (const FAftershockScenarioLot& Lot : DesignedLots)
	{
		FAftershockRoadQuote Best;
		int32 BestNewCells = MAX_int32;
		for (FIntPoint Direction : Directions)
		{
			const FAftershockRoadQuote Quote = Roads->QuoteRoad(Root, Lot.Coord + Direction);
			if (!Quote.bRouteValid) continue;
			int32 NewCells = 0;
			for (FIntPoint Coord : Quote.Path) if (!StreetSet.Contains(Coord)) ++NewCells;
			if (NewCells < BestNewCells) { Best = Quote; BestNewCells = NewCells; }
		}
		if (!Best.bRouteValid) return; // Do not leave a partially connected starter city.
		AddPath(Best.Path);
	}
	if (Roads->SeedInitialRoads(Streets))
	{
		bStarterRoadsInitialized = true;
		StarterRoadCellCount = Streets.Num();
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_STARTER_CITY index=%d roadCells=%d connectedLots=%d connectedAssemblyAreas=%d playerCanExtend=1"),ScenarioIndex, StarterRoadCellCount, DesignedLots.Num(), AssemblyCells.Num());
	}
}

void UAftershockScenarioSubsystem::GetObjectiveCounts(int32& Roads, int32& Preparedness, int32& Centres) const
{
	Roads = 0; Preparedness = 0; Centres = 0;
	const UWorld* World = GetWorld(); if (!World) return;
	if (const URoadNetworkSubsystem* Network = World->GetSubsystem<URoadNetworkSubsystem>()) Roads = Network->CountRoadCells();
	const UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>();
	TSet<FIntPoint> ConnectedRoads;
	TArray<FIntPoint> Queue;
	const FIntPoint Directions[] = { FIntPoint(1,0), FIntPoint(-1,0), FIntPoint(0,1), FIntPoint(0,-1) };
	// Count emergency centres only when their frontage belongs to the same
	// actual road component as the first authored public assembly area.
	if (Terrain && !AssemblyCells.IsEmpty() && Terrain->GetCell(AssemblyCells[0]).bHasRoad)
	{
		Queue.Add(AssemblyCells[0]); ConnectedRoads.Add(AssemblyCells[0]);
		for (int32 Head = 0; Head < Queue.Num(); ++Head)
			for (FIntPoint Direction : Directions)
			{
				const FIntPoint Coord = Queue[Head] + Direction;
				if (Terrain->IsPlayableCoord(Coord) && Terrain->GetCell(Coord).bHasRoad && !ConnectedRoads.Contains(Coord))
				{ ConnectedRoads.Add(Coord); Queue.Add(Coord); }
			}
	}
	if (const UBuildingManagerSubsystem* Buildings = World->GetSubsystem<UBuildingManagerSubsystem>())
	{
		TArray<FBuildingInstance> Records; Buildings->GetAllBuildings(Records);
		for (const FBuildingInstance& B : Records)
		{
			if (!B.BuildingData || B.DamageState >= EDamageState::Damaged) continue;
			const EBuildingType Type = B.BuildingData->Type;
			if (Type == EBuildingType::Hospital || Type == EBuildingType::School || Type == EBuildingType::FireStation || Type == EBuildingType::Shelter)
			{
				for (FIntPoint Direction : Directions)
					if (ConnectedRoads.Contains(B.GridCoords + Direction)) { ++Centres; break; }
			}
			if (ScenarioIndex == 0 && B.bIsRetrofitted) ++Preparedness;
			if (ScenarioIndex == 2 && Type == EBuildingType::GreenSpace) ++Preparedness;
			if (ScenarioIndex == 4 && Type == EBuildingType::FireStation)
			{
				for (FIntPoint Direction : Directions) if (ConnectedRoads.Contains(B.GridCoords+Direction)) { ++Preparedness; break; }
			}
			if (ScenarioIndex == 6 && Type == EBuildingType::Farm)
			{
				// An isolated field cannot deliver food to the settlement. Use the
				// same actual assembly/city road component as the emergency centres.
				for (FIntPoint Direction : Directions)
					if (ConnectedRoads.Contains(B.GridCoords + Direction)) { ++Preparedness; break; }
			}
		}
	}
	if (ScenarioIndex == 1)
	{
		if (Terrain)
			for (const FGridCell& Cell : Terrain->Cells) if (Cell.bIsPlayable && !Cell.bIsRiverbed && Cell.DrainageQuality >= .2f) ++Preparedness;
	}
	if (ScenarioIndex == 3)
		for (FIntPoint Assembly : AssemblyCells) if (ConnectedRoads.Contains(Assembly)) ++Preparedness;
	if (ScenarioIndex == 5 && Terrain)
		for (const FGridCell& Cell : Terrain->Cells) if (Cell.bIsPlayable && !Cell.bIsRiverbed && Cell.bGroundSurveyed) ++Preparedness;
}

FText UAftershockScenarioSubsystem::GetBriefingText() const { return FText::FromString(Definition.Name + TEXT("\n") + Definition.Briefing); }
FText UAftershockScenarioSubsystem::GetProgressText() const
{
	if(const auto* GI=GetWorld()?GetWorld()->GetGameInstance():nullptr)
	if(GI->GetSubsystem<UAftershockOpeningSessionSubsystem>()->GetSettings().Mode!=EAftershockPlayMode::Campaign)
		return FText::FromString(TEXT("Açık şehir: nüfus, gıda, su, elektrik, ulaşım ve memnuniyeti yönet. Kampanya görev kilidi yok."));
	int32 Roads, Preparedness, Centres; GetObjectiveCounts(Roads, Preparedness, Centres);
	const TCHAR* Preparation = ScenarioIndex == 0 ? TEXT("güçlendirilen yapı") : ScenarioIndex == 1 ? TEXT("drenaj hücresi") : ScenarioIndex == 2 ? TEXT("yeşil alan") : ScenarioIndex==3 ? TEXT("bağlı toplanma alanı") : ScenarioIndex==4 ? TEXT("bağlı itfaiye") : ScenarioIndex==5 ? TEXT("zemin etüdü") : TEXT("bağlı tarla");
	const int32 Target = ScenarioIndex == 0 ? 1 : ScenarioIndex == 1 ? 6 : (ScenarioIndex==4 || ScenarioIndex==6) ? 2 : 3;
	return FText::FromString(FString::Printf(TEXT("Yol: %d/8 | Hastaneden bağlı merkez: %d/%d\n%s: %d/%d | Ana afet: %s"), Roads, Centres, ScenarioIndex == 3 ? 3 : 2, Preparation, Preparedness, Target, bPrimaryDisasterSurvived ? TEXT("atlatıldı") : TEXT("bekleniyor")));
}

bool UAftershockScenarioSubsystem::CanAdvanceScenario() const
{
	const auto* GI=GetWorld()?GetWorld()->GetGameInstance():nullptr;
	if(GI&&GI->GetSubsystem<UAftershockOpeningSessionSubsystem>()->GetSettings().Mode!=EAftershockPlayMode::Campaign)return false;
	if (!bPrepared || !bPrimaryDisasterSurvived || !GetWorld()) return false;
	const UEconomyAndStateSubsystem* Economy = GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>();
	if (!Economy || Economy->bIsGameOver || Economy->PublicSatisfaction <= 0.f) return false;
	int32 Roads, Preparedness, Centres; GetObjectiveCounts(Roads, Preparedness, Centres);
	return Roads >= 8 && Centres >= (ScenarioIndex == 3 ? 3 : 2) && Preparedness >= (ScenarioIndex == 0 ? 1 : ScenarioIndex == 1 ? 6 : (ScenarioIndex==4 || ScenarioIndex==6) ? 2 : 3);
}

float UAftershockScenarioSubsystem::GetScenarioBaseSeverity() const
{return GetScenarioSeverityForHazard(Definition.Hazard);}

float UAftershockScenarioSubsystem::GetScenarioSeverityForHazard(EDisasterType Type) const
{
	// A tectonic tsunami is not made stronger by a city's pollution score.
	if (Type == EDisasterType::Tsunami || Type==EDisasterType::Earthquake || Type==EDisasterType::Volcano) return FMath::Clamp(Definition.BaseSeverity, 0.f, 95.f);
	const UEconomyAndStateSubsystem* Economy = GetWorld() ? GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	return FMath::Clamp(Definition.BaseSeverity * (Economy ? Economy->ClimateRiskMultiplier : 1.f), 0.f, 95.f);
}

bool UAftershockScenarioSubsystem::AdvanceToNextScenario()
{
	if (!CanAdvanceScenario() || IsFinalScenario()) return false;
	if(auto* GI=GetWorld()->GetGameInstance())
	{
		auto* Session=GI->GetSubsystem<UAftershockOpeningSessionSubsystem>();Session->CompleteCampaignStage(ScenarioIndex);
		// Save the completed city before changing session settings to the next city.
		FText Error;if(!GI->GetSubsystem<UAftershockSaveSubsystem>()->SaveNow(GetWorld(),TEXT("Manual"),Error))return false;
		if(!Session->AdvanceCampaignStage(ScenarioIndex+1,Error))return false;
	}
	UGameplayStatics::OpenLevel(GetWorld(), FName(*UGameplayStatics::GetCurrentLevelName(GetWorld(), true)), true, FString::Printf(TEXT("AftershockScenario=%d"), ScenarioIndex + 1));
	return true;
}

void UAftershockScenarioSubsystem::HandleEconomyChanged(int64 TreasuryTL, float Satisfaction, int32 Turn)
{
	if(CanAdvanceScenario())if(auto* GI=GetWorld()->GetGameInstance())GI->GetSubsystem<UAftershockOpeningSessionSubsystem>()->CompleteCampaignStage(ScenarioIndex);
	if (bPrepared) OnScenarioUpdated.Broadcast();
}
void UAftershockScenarioSubsystem::HandleDisasterCompleted(const FDisasterReport& Report)
{
	if (!bPrepared || Report.DisasterType != Definition.Hazard) return;
	const UEconomyAndStateSubsystem* Economy = GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>();
	bPrimaryDisasterSurvived = Economy && !Economy->bIsGameOver && Economy->PublicSatisfaction > 0.f;
	if(bPrimaryDisasterSurvived&&CanAdvanceScenario())if(auto* GI=GetWorld()->GetGameInstance())GI->GetSubsystem<UAftershockOpeningSessionSubsystem>()->CompleteCampaignStage(ScenarioIndex);
	OnScenarioUpdated.Broadcast();
}
void UAftershockScenarioSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		if (auto* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>()) Economy->OnEconomyUpdated.RemoveDynamic(this, &UAftershockScenarioSubsystem::HandleEconomyChanged);
		if (auto* Disaster = World->GetSubsystem<UDisasterSimulationSubsystem>()) Disaster->OnDisasterCompleted.RemoveDynamic(this, &UAftershockScenarioSubsystem::HandleDisasterCompleted);
	}
	if (AssemblyMarkerHost) AssemblyMarkerHost->Destroy();
	OnScenarioUpdated.Clear(); Super::Deinitialize();
}
