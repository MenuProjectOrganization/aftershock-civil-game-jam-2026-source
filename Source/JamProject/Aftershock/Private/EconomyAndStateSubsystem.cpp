// AFTERSHOCK — Civil Game Jam 2026
// Ekonomi, Vergi, İklim/CO2 Döngüsü, Dinamik Ödüller ve Oyun Durumu Alt Sistemi Uygulaması

#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/AftershockStartupPreparationSubsystem.h"
#include "AftershockRoadCpuScope.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Narrative/CasualtyLedgerGenerator.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

namespace
{
	int64 SaturatingAddMoney(int64 A,int64 B)
	{if(B>0&&A>MAX_int64-B)return MAX_int64;if(B<0&&A<MIN_int64-B)return MIN_int64;return A+B;}
	bool UnlimitedInvestments(const UWorld* W)
	{const auto* GI=W?W->GetGameInstance():nullptr;const auto* S=GI?GI->GetSubsystem<UAftershockOpeningSessionSubsystem>():nullptr;return S&&S->IsUnlimitedBudget();}
	void RefreshCityLife(UWorld* World)
	{
		if(World)if(UCityLifeSubsystem* Life=World->GetSubsystem<UCityLifeSubsystem>())Life->RefreshCity();
	}
}

void UEconomyAndStateSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Collection.InitializeDependency<UTerrainGridSubsystem>();
	Collection.InitializeDependency<UBuildingManagerSubsystem>();
	Collection.InitializeDependency<UDisasterSimulationSubsystem>();

	CurrentTurn = 1;
	CurrentYear = 2026;
	TreasuryTL = 50000;
	PublicSatisfaction = 100.0f;
	CityCO2Level = 0.0f;
	ClimateRiskMultiplier = 1.0f;
	CurrentDisasterLevel = 1;
	PendingStateAidTL = 0;
	bIsGameOver = false;
	SuccessfulDisastersSurvived = 0;
	AppliedDisasterEventIDs.Reset();

	// Afet tamamlanma olayına abone ol
	if (UWorld* World = GetWorld())
	{
		if (UDisasterSimulationSubsystem* DisasterSubsystem = World->GetSubsystem<UDisasterSimulationSubsystem>())
		{
			DisasterSubsystem->OnDisasterCompleted.AddDynamic(this, &UEconomyAndStateSubsystem::HandleDisasterCompleted);
		}
	}
}

void UEconomyAndStateSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		if (UDisasterSimulationSubsystem* DisasterSubsystem = World->GetSubsystem<UDisasterSimulationSubsystem>())
		{
			DisasterSubsystem->OnDisasterCompleted.RemoveDynamic(this, &UEconomyAndStateSubsystem::HandleDisasterCompleted);
		}
	}

	OnEconomyUpdated.Clear();
	OnClimateUpdated.Clear();
	OnGameOver.Clear();

	Super::Deinitialize();
}

void UEconomyAndStateSubsystem::AdvanceTurn()
{
	if(const auto* Startup=GetWorld()&&GetWorld()->GetGameInstance()?GetWorld()->GetGameInstance()->GetSubsystem<UAftershockStartupPreparationSubsystem>():nullptr)if(Startup->IsGameplayBlocked(GetWorld()))return;
	if (bIsGameOver || bAdvancingTurn)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UBuildingManagerSubsystem* BuildingMgr = World->GetSubsystem<UBuildingManagerSubsystem>();
	if (!BuildingMgr)
	{
		return;
	}
	TGuardValue<bool> TurnGuard(bAdvancingTurn,true);

	// 1) Tur ve Zaman
	CurrentTurn++;
	BuildingMgr->AdvanceMemorialTurn(CurrentTurn);

	if (CurrentTurn % 10 == 0)
	{
		CurrentYear++;
		BuildingMgr->AgeAllBuildingsOneYear();
	}

	// Seviye İlerlemesi: Her 30 turda bir afet kademesi artar
	if (CurrentTurn % 30 == 0)
	{
		AdvanceDisasterLevel();
	}

	// 2) Vergi ve Sabit Bakım Gideri
	TArray<FBuildingInstance> AllBuildings;
	BuildingMgr->GetAllBuildings(AllBuildings);
	UCityLifeSubsystem* Life=World->GetSubsystem<UCityLifeSubsystem>();
	const bool bHasCityLife=Life&&Life->IsCityInitialized();
	if(bHasCityLife)Life->AdvanceCityTurn(CurrentTurn);
	// Real needs casualties settle their cost inside AdvanceCityTurn. Dismissal
	// finalizes the turn there: no subsequent tax, recovery or queued aid transfer.
	if(bIsGameOver)return;

	int64 TotalTaxIncome = 0;
	int64 TotalMaintenance = 0;
	int32 ActiveHospitals = 0;
	float TurnCO2 = 0.0f;

	for (const FBuildingInstance& B : AllBuildings)
	{
		if (!B.BuildingData || B.DamageState==EDamageState::Memorial)
		{
			continue;
		}

		// Yalnızca sağlam veya hafif çatlak binalar vergi öder
		if (B.DamageState == EDamageState::Intact || B.DamageState == EDamageState::Cracked)
		{
			TotalTaxIncome += B.BuildingData->TaxPerTurn;
		}

		// Şehirdeki tüm aktif binaların sabit altyapı/bakım gideri
		TotalMaintenance += B.BuildingData->MaintenancePerTurn;

		// Hastane kontrolü (CurrentDamage < 50)
		if (B.BuildingData->Type == EBuildingType::Hospital && B.CurrentDamage < 50.0f)
		{
			ActiveHospitals++;
		}

		// CO2 salınımı (Fabrika pozitif, yeşil alan negatif)
		TurnCO2 += B.BuildingData->CO2PerTurn;
	}
	if(bHasCityLife)
	{
		TotalTaxIncome=Life->GetState().ProjectedTaxIncomeTL+Life->GetState().LastTransitIncomeTL;
		TotalMaintenance=Life->GetState().ProjectedMaintenanceTL;
		TurnCO2=0.f;
		for(const FBuildingInstance& B:AllBuildings)if(B.BuildingData&&B.DamageState!=EDamageState::Memorial)
			TurnCO2+=B.BuildingData->CO2PerTurn*(B.BuildingData->CO2PerTurn<0.f?1.f:Life->GetBuildingOperation01(B.InstanceID));
	}

	TreasuryTL = SaturatingAddMoney(TreasuryTL,TotalTaxIncome-TotalMaintenance);

	// Bütçe açığı varsa halk memnuniyeti düşer
	if (TreasuryTL < 0)
	{
		PublicSatisfaction = FMath::Clamp(PublicSatisfaction - 2.0f, 0.0f, 100.0f);
	}

	// 3) Hastane Memnuniyet Bonusu (BUILDING_STATS.md Bölüm 1: Hastane +2/tur memnuniyet)
	if (bHasCityLife)
	{
		const float NeedSatisfaction=Life->GetState().AverageHappiness01*100.f;
		// Slow changes preserve the immediate casualty penalty and require time
		// to recover; a hospital is useful only when actually staffed and powered.
		PublicSatisfaction=FMath::Clamp(PublicSatisfaction+FMath::Clamp((NeedSatisfaction-PublicSatisfaction)*.20f,-5.f,2.f),0.f,100.f);
	}
	else if (ActiveHospitals > 0)
	{
		const float HospitalBonus = static_cast<float>(ActiveHospitals) * 2.0f;
		PublicSatisfaction = FMath::Clamp(PublicSatisfaction + HospitalBonus, 0.0f, 100.0f);
	}

	// 4) Devlet Ödeneği Transferi
	if (PendingStateAidTL > 0)
	{
		TreasuryTL = SaturatingAddMoney(TreasuryTL,PendingStateAidTL);
		PendingStateAidTL = 0;
	}

	// 5) İklim ve Çevre Simülasyonu
	CityCO2Level = FMath::Max(0.0f, CityCO2Level + TurnCO2);

	// Kentsel ısı adası / betonlaşma ve kirlilik katsayısı
	int32 LandCells=0,BuiltLots=0;
	if(const auto* Terrain=World->GetSubsystem<UTerrainGridSubsystem>())for(const auto& Cell:Terrain->Cells)LandCells+=Cell.bIsPlayable&&!Cell.bIsOcean&&!Cell.bIsRiverbed?1:0;
	for(const auto& B:AllBuildings)BuiltLots+=B.BuildingData&&B.DamageState!=EDamageState::Memorial?1:0;
	ClimateRiskMultiplier=CalculateLocalClimateRisk(CityCO2Level,bHasCityLife?Life->GetState().LivingPopulation:0,BuiltLots,LandCells);

	// 6) Görevden Alınma / Game Over Kontrolü
	if (PublicSatisfaction <= 0.0f)
	{
		TriggerGameOver(EGameOverReason::DismissedByPublic);
		return;
	}

	// 7) Event Yayınları
	OnEconomyUpdated.Broadcast(TreasuryTL, PublicSatisfaction, CurrentTurn);
	OnClimateUpdated.Broadcast(CityCO2Level, ClimateRiskMultiplier);
}

void UEconomyAndStateSubsystem::HandleDisasterCompleted(const FDisasterReport& Report)
{
	if (Report.bWasRejected || Report.bIsPending || !Report.DisasterEventID.IsValid() || AppliedDisasterEventIDs.Contains(Report.DisasterEventID)
		||!FMath::IsFinite(Report.BaseSeverity)||Report.BaseSeverity<0.f||static_cast<uint8>(Report.DisasterType)>static_cast<uint8>(EDisasterType::Landslide))
	{
		return;
	}
	TSet<int32> IDs;
	const auto* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr;
	const auto* Ledger=GetWorld()?GetWorld()->GetSubsystem<UCasualtyLedgerGenerator>():nullptr;
	if(!Life||!Ledger||!Ledger->ValidateDisasterSettlement(Report))return;
	for(int32 ID:Report.ActualDeathResidentIDs){FCityResidentRecord R;if(ID<1||IDs.Contains(ID)||!Life->GetResident(ID,R)||R.bAlive)return;IDs.Add(ID);}
	for(int32 ID:Report.ExposedSurvivorResidentIDs){FCityResidentRecord R;if(ID<1||IDs.Contains(ID)||!Life->GetResident(ID,R)||!R.bAlive)return;IDs.Add(ID);}
	if(Report.TotalDeaths!=Report.ActualDeathResidentIDs.Num() || Report.TotalSurvivors!=Report.ExposedSurvivorResidentIDs.Num()
		||Report.TotalDeathCostTL!=static_cast<int64>(Report.TotalDeaths)*500 || Report.TotalStateAidGranted!=static_cast<int64>(Report.TotalSurvivors)*150)return;
	AppliedDisasterEventIDs.Add(Report.DisasterEventID);
	if(!Report.bNeedsCasualtyEvent)if(UCityLifeSubsystem* MutableLife=GetWorld()->GetSubsystem<UCityLifeSubsystem>())MutableLife->ApplyDisasterShock(Report.DisasterType,Report.BaseSeverity);
	// Deaths are a cost, never an aid source. The grant covers actual unique
	// exposed survivors only; no collapse grant or speculative prevention cash.
	TreasuryTL=SaturatingAddMoney(TreasuryTL,-Report.TotalDeathCostTL);
	PendingStateAidTL=SaturatingAddMoney(PendingStateAidTL,Report.TotalStateAidGranted);

	// 2) Can Kaybı Memnuniyet Düşüşü (-Deaths * 0.3)
	PublicSatisfaction = FMath::Clamp(PublicSatisfaction - static_cast<float>(Report.TotalDeaths)*.3f, 0.0f, 100.0f);

	// Başarıyla atlatılan afet sayacı ve seviye atlama
	if(!Report.bNeedsCasualtyEvent)++SuccessfulDisastersSurvived;
	if (!Report.bNeedsCasualtyEvent && SuccessfulDisastersSurvived % 2 == 0)
	{
		AdvanceDisasterLevel();
	}

	// 4) Kritik Eşik ve Görevden Alınma Kontrolü
	if (PublicSatisfaction <= 0.0f)
	{
		TriggerGameOver(EGameOverReason::DismissedByPublic);
		return;
	}

	OnEconomyUpdated.Broadcast(TreasuryTL, PublicSatisfaction, CurrentTurn);
}

void UEconomyAndStateSubsystem::CalculateProjectedFinances(int64& OutTaxIncome, int64& OutMaintenance) const
{
	OutTaxIncome = 0;
	OutMaintenance = 0;

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if(const UCityLifeSubsystem* Life=World->GetSubsystem<UCityLifeSubsystem>();Life&&Life->IsCityInitialized())
	{
		OutTaxIncome=Life->GetState().ProjectedTaxIncomeTL;
		OutMaintenance=Life->GetState().ProjectedMaintenanceTL;
		return;
	}

	UBuildingManagerSubsystem* BuildingMgr = World->GetSubsystem<UBuildingManagerSubsystem>();
	if (!BuildingMgr)
	{
		return;
	}

	TArray<FBuildingInstance> AllBuildings;
	BuildingMgr->GetAllBuildings(AllBuildings);

	for (const FBuildingInstance& B : AllBuildings)
	{
		if (!B.BuildingData)
		{
			continue;
		}

		if (B.DamageState == EDamageState::Intact || B.DamageState == EDamageState::Cracked)
		{
			OutTaxIncome += B.BuildingData->TaxPerTurn;
		}

		OutMaintenance += B.BuildingData->MaintenancePerTurn;
	}
}

bool UEconomyAndStateSubsystem::CanAfford(int64 Amount) const
{return Amount>=0&&(UnlimitedInvestments(GetWorld())||TreasuryTL>=Amount);}

bool UEconomyAndStateSubsystem::TrySpendMoney(int64 Amount)
{
    FAftershockRoadCpuScope Cpu(TEXT("Economy.TrySpendMoney"));
	if (Amount < 0)
	{
		return false;
	}

	if (CanAfford(Amount))
	{
		if(!UnlimitedInvestments(GetWorld()))TreasuryTL -= Amount;
        // Treasury is the only changed input here. Infrastructure, construction,
        // repair and hazard owners refresh their own completed canonical changes.
        // Rebuilding residents/jobs here duplicates their final refresh.
		OnEconomyUpdated.Broadcast(TreasuryTL, PublicSatisfaction, CurrentTurn);
		return true;
	}

	return false;
}

bool UEconomyAndStateSubsystem::BuildBuildingWithBudget(
	FIntPoint GridCoords,
	UBuildingData* Data,
	EFoundationQuality Foundation,
	int32 Floors)
{
	if (!Data || bIsGameOver)
	{
		return false;
	}

	UWorld* World = GetWorld();
	UBuildingManagerSubsystem* BuildingMgr = World ? World->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
	if (!BuildingMgr)
	{
		return false;
	}

	int32 AssignedID=INDEX_NONE;
	// Construction owns one all-or-nothing purchase, including the automatically
	// required access road. Startup still uses the explicitly free PlaceBuilding.
	return BuildingMgr->PlaceBuildingWithRoadAccess(GridCoords,Data,Foundation,Floors,AssignedID);
}

bool UEconomyAndStateSubsystem::RetrofitWithBudget(int32 BuildingID)
{
	if (bIsGameOver || !CanAfford(1000))
	{
		return false;
	}

	UWorld* World = GetWorld();
	UBuildingManagerSubsystem* BuildingMgr = World ? World->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
	if (!BuildingMgr)
	{
		return false;
	}

	int32 CostTL = 0;
	if (BuildingMgr->RetrofitBuilding(BuildingID, CostTL))
	{
		if(!UnlimitedInvestments(GetWorld()))TreasuryTL -= CostTL;
		RefreshCityLife(World);
		OnEconomyUpdated.Broadcast(TreasuryTL, PublicSatisfaction, CurrentTurn);
		return true;
	}

	return false;
}

bool UEconomyAndStateSubsystem::RepairWithBudget(int32 BuildingID)
{
	if (bIsGameOver)
	{
		return false;
	}

	UWorld* World = GetWorld();
	UBuildingManagerSubsystem* BuildingMgr = World ? World->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
	if (!BuildingMgr)
	{
		return false;
	}

	FBuildingInstance Building;
	if (!BuildingMgr->GetBuildingByID(BuildingID, Building))
	{
		return false;
	}

	int64 EstimatedCost = 0;
	if (Building.CurrentDamage >= 20.0f && Building.CurrentDamage <= 50.0f)
	{
		EstimatedCost = 2000;
	}
	else if (Building.CurrentDamage > 50.0f && Building.CurrentDamage < 80.0f)
	{
		EstimatedCost = 5000;
	}
	else if (Building.CurrentDamage > 0.0f && Building.CurrentDamage < 20.0f)
	{
		EstimatedCost = 500;
	}
	else
	{
		return false;
	}

	if (!CanAfford(EstimatedCost))
	{
		return false;
	}

	int32 CostTL = 0;
	if (BuildingMgr->RepairBuilding(BuildingID, CostTL))
	{
		if(!UnlimitedInvestments(GetWorld()))TreasuryTL -= CostTL;
		RefreshCityLife(World);
		OnEconomyUpdated.Broadcast(TreasuryTL, PublicSatisfaction, CurrentTurn);
		return true;
	}

	return false;
}

bool UEconomyAndStateSubsystem::ClearRubbleWithBudget(FIntPoint GridCoords)
{
	if (bIsGameOver || !CanAfford(3000))
	{
		return false;
	}

	UWorld* World = GetWorld();
	UDisasterSimulationSubsystem* DisasterSubsystem = World ? World->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
	if (!DisasterSubsystem)
	{
		return false;
	}

	int32 CostTL = 0;
	if (DisasterSubsystem->ClearCollapsedRubble(GridCoords, CostTL))
	{
		if(!UnlimitedInvestments(GetWorld()))TreasuryTL -= CostTL;
		RefreshCityLife(World);
		OnEconomyUpdated.Broadcast(TreasuryTL, PublicSatisfaction, CurrentTurn);
		return true;
	}

	return false;
}

void UEconomyAndStateSubsystem::AdvanceDisasterLevel()
{
	CurrentDisasterLevel++;
}

float UEconomyAndStateSubsystem::GetNextDisasterBaseSeverity() const
{
	return 30.0f + (static_cast<float>(CurrentDisasterLevel) * 12.0f) * ClimateRiskMultiplier;
}

void UEconomyAndStateSubsystem::TriggerGameOver(EGameOverReason Reason)
{
	const auto* GI=GetWorld()?GetWorld()->GetGameInstance():nullptr;const auto* Session=GI?GI->GetSubsystem<UAftershockOpeningSessionSubsystem>():nullptr;
	if(Session&&Session->GetSettings().Mode==EAftershockPlayMode::Sandbox)return;
	if (bIsGameOver)
	{
		return;
	}

	bIsGameOver = true;

	FText ReasonText;
	switch (Reason)
	{
	case EGameOverReason::DismissedByPublic:
		ReasonText = NSLOCTEXT(
			"Aftershock",
			"GameOverDismissed",
			"Halk memnuniyeti sıfıra düştü. Yöneticilik göreviniz sona erdi."
		);
		break;
	case EGameOverReason::Bankruptcy:
		ReasonText = NSLOCTEXT(
			"Aftershock",
			"GameOverBankruptcy",
			"Şehir hazinesi iflas etti! Şehir borçlarını ödeyemediğiniz için görevden alındınız."
		);
		break;
	}

	OnGameOver.Broadcast(ReasonText);
}

void UEconomyAndStateSubsystem::ResetState(int64 StartingBudget)
{
	CurrentTurn = 1;
	CurrentYear = 2026;
	TreasuryTL = StartingBudget;
	PublicSatisfaction = 100.0f;
	CityCO2Level = 0.0f;
	ClimateRiskMultiplier = 1.0f;
	CurrentDisasterLevel = 1;
	PendingStateAidTL = 0;
	bIsGameOver = false;
	SuccessfulDisastersSurvived = 0;
	AppliedDisasterEventIDs.Reset();
	if(GetWorld())if(auto* Ledger=GetWorld()->GetSubsystem<UCasualtyLedgerGenerator>())Ledger->ResetLedger();

	OnEconomyUpdated.Broadcast(TreasuryTL, PublicSatisfaction, CurrentTurn);
	OnClimateUpdated.Broadcast(CityCO2Level, ClimateRiskMultiplier);
}

void UEconomyAndStateSubsystem::CaptureAppliedDisasterEvents(TArray<FGuid>& Out) const
{Out=AppliedDisasterEventIDs.Array();Out.Sort([](const FGuid& A,const FGuid& B){return A.ToString()<B.ToString();});}
bool UEconomyAndStateSubsystem::RestoreAppliedDisasterEvents(const TArray<FGuid>& Events,FText& Error)
{TSet<FGuid> Valid;for(const FGuid& ID:Events){if(!ID.IsValid()||Valid.Contains(ID)){Error=FText::FromString(TEXT("İşlenmiş afet ödeme kimlikleri geçersiz."));return false;}Valid.Add(ID);}AppliedDisasterEventIDs=MoveTemp(Valid);return true;}

float UEconomyAndStateSubsystem::CalculateLocalClimateRisk(float CO2,int32 Population,int32 Lots,int32 LandCells)
{
	const double Total=FMath::IsFinite(CO2)?FMath::Max(0.,static_cast<double>(CO2)):0.;
	const double Pollution=Total/(Total+static_cast<double>(FMath::Max(1,Population))*20.);
	const double Sealed=FMath::Clamp(static_cast<double>(FMath::Max(0,Lots))*.5776/FMath::Max(1,LandCells),0.,1.);
	return static_cast<float>(1.+.65*Pollution+.35*Sealed);
}
