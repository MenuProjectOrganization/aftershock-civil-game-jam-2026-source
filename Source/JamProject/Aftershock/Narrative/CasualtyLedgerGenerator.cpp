#include "Aftershock/Narrative/CasualtyLedgerGenerator.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/AftershockBuildingCatalog.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

void UCasualtyLedgerGenerator::GetStableDemographics(int32 ID,bool bWorkingAge,FText& Name,int32& Age)
{
	static const TCHAR* First[]={TEXT("Deniz"),TEXT("Ece"),TEXT("Mert"),TEXT("Elif"),TEXT("Can"),TEXT("Derya"),TEXT("Arda"),TEXT("Selin"),TEXT("Emre"),TEXT("Zeynep"),TEXT("Cem"),TEXT("Aslı")};
	static const TCHAR* Last[]={TEXT("Yılmaz"),TEXT("Kaya"),TEXT("Demir"),TEXT("Aydın"),TEXT("Arslan"),TEXT("Çelik"),TEXT("Yıldız"),TEXT("Koç"),TEXT("Öztürk"),TEXT("Aksoy"),TEXT("Güneş"),TEXT("Şahin"),TEXT("Ergin")};
	const uint32 H=HashCombineFast(GetTypeHash(ID),0xA57E2026u);
	Name=FText::FromString(FString::Printf(TEXT("%s %s"),First[H%UE_ARRAY_COUNT(First)],Last[(H/UE_ARRAY_COUNT(First))%UE_ARRAY_COUNT(Last)]));
	Age=bWorkingAge?18+static_cast<int32>((H>>8)%48):(H&1u)?7+static_cast<int32>((H>>8)%11):66+static_cast<int32>((H>>8)%23);
}

void UCasualtyLedgerGenerator::BuildLegacyLedgerState(const TArray<FCityResidentRecord>& Residents,FGuid SessionID,int32 Turn,FAftershockCasualtyLedgerSaveState& Out)
{
	Out={};Out.ArchiveSessionID=SessionID;
	const FGuid EventID(SessionID.A^0x1E6AC001u,SessionID.B^0xA57E5005u,SessionID.C^0xC17A2026u,SessionID.D^0x1E6AC004u);
	for(const auto& R:Residents)if(!R.bAlive)
	{
		FCitizenMicroStory& E=Out.Entries.AddDefaulted_GetRef();E.ResidentID=R.ResidentID;GetStableDemographics(R.ResidentID,R.bWorkingAge,E.FullName,E.Age);
		if(!R.FullName.IsEmpty())E.FullName=R.FullName;if(R.Age>0)E.Age=R.Age;
		E.Occupation=FText::FromString(TEXT("Önceki kayıtta meslek bilgisi yok"));E.BuildingDisplayName=FText::FromString(TEXT("Önceki kayıtta yapı ayrıntısı yok"));E.bHasBuildingSnapshot=false;
		E.BuildingID=R.HomeBuildingID;E.DisasterEventID=EventID;E.DisasterTurn=FMath::Max(1,Turn);E.Outcome=ECitizenMicroStoryOutcome::Deceased;E.Cause=ECitizenCasualtyCause::HistoricalUnknown;
		E.Narrative=FText::FromString(FString::Printf(TEXT("%s, %d. Önceki kayıtta hayatını kaybetmiş bir sakin. Ölümün zamanı, nedeni ve afet anındaki yapı bilgisi kayıtlı değil."),*E.FullName.ToString(),E.Age));
	}
	if(!Out.Entries.IsEmpty()){auto& E=Out.Events.AddDefaulted_GetRef();E.EventID=EventID;E.Turn=FMath::Max(1,Turn);E.ActualDeaths=Out.Entries.Num();E.bHistoricalUnattributed=true;for(const auto& Row:Out.Entries)E.ActualDeathResidentIDs.Add(Row.ResidentID);}
}

bool UCasualtyLedgerGenerator::GetResidentIdentityCard(int32 ID,FResidentIdentityCard& Out) const
{
	const UCityLifeSubsystem* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr;FCityResidentRecord R;
	if (!Life || !Life->GetResident(ID,R)) return false;
	Out={};Out.ResidentID=ID;Out.bAlive=R.bAlive;Out.HomeBuildingID=R.HomeBuildingID;Out.WorkBuildingID=R.WorkBuildingID;
	GetStableDemographics(ID,R.bWorkingAge,Out.FullName,Out.Age);
	if (!R.FullName.IsEmpty())Out.FullName=R.FullName;if(R.Age>0)Out.Age=R.Age;
	Out.Occupation=FText::FromString(R.bWorkingAge?TEXT("İş ataması olmayan sakin"):TEXT("Çalışma çağında olmayan sakin"));
	FBuildingInstance Work;const auto* Manager=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>();
	if (Manager&&Manager->GetBuildingByID(R.WorkBuildingID,Work)&&Work.BuildingData)Out.Occupation=FText::FromString(Work.BuildingData->DisplayName.ToString()+TEXT(" çalışanı"));
	if(!R.bAlive)if(const auto* Entry=State.Entries.FindByPredicate([ID](const auto& E){return E.ResidentID==ID;}))Out.Occupation=Entry->Occupation;
	return true;
}

int32 UCasualtyLedgerGenerator::GetSessionDeaths() const
{const auto* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr;return Life?Life->GetState().TotalDeaths:0;}
int32 UCasualtyLedgerGenerator::GetSessionSurvivors() const
{const auto* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr;return Life?Life->GetState().LivingPopulation:0;}

void UCasualtyLedgerArchiveSubsystem::ReplaceCitySnapshot(const FCasualtyCitySummary& S)
{
	if(!S.SessionID.IsValid())return;
	FText Error;if(!ValidateArchive(S.SessionID,{S},Error))return;
	if(SessionID!=S.SessionID){SessionID=S.SessionID;Summaries.Reset();}
	if(auto* Old=Summaries.FindByPredicate([&S](const auto& X){return X.CampaignStage==S.CampaignStage&&X.RegionID==S.RegionID&&X.FocusID==S.FocusID;}))*Old=S;
	else Summaries.Add(S);
}
bool UCasualtyLedgerArchiveSubsystem::ValidateArchive(FGuid ID,const TArray<FCasualtyCitySummary>& Saved,FText& Error)
{
	if(Saved.Num()>1000||(!ID.IsValid()&&!Saved.IsEmpty())){Error=FText::FromString(TEXT("Kampanya arşivi geçersiz."));return false;}
	TSet<FString> Keys;
	for(const auto& S:Saved){const FString Key=FString::Printf(TEXT("%d|%s|%s"),S.CampaignStage,*S.RegionID.ToString(),*S.FocusID.ToString());
		const int32 Region=FindAftershockGeographyIndex(S.RegionID);const int32 Focus=Region!=INDEX_NONE&&!S.FocusID.IsNone()?FindAftershockGeographyFocusIndex(Region,S.FocusID):INDEX_NONE;
		if(S.SessionID!=ID||Keys.Contains(Key)||S.CampaignStage<0||S.CampaignStage>=UAftershockScenarioSubsystem::GetScenarioCount()
			||S.RegionID.IsNone()||S.FocusID.IsNone()||!TryGetAftershockGeographyFocus(Region,Focus)
			||S.Deaths<0||S.LivingPopulation<0||S.UniqueExposedSurvivors<0||S.UniqueExposedSurvivors>S.LivingPopulation||S.RetrofitInvestments<0||S.FlowerTributes<0||S.FlowerTributes>S.Deaths){Error=FText::FromString(TEXT("Kampanya şehir kaydı tutarsız."));return false;}Keys.Add(Key);}
	return true;
}
bool UCasualtyLedgerArchiveSubsystem::RestoreArchive(FGuid ID,const TArray<FCasualtyCitySummary>& Saved,FText& Error)
{if(!ValidateArchive(ID,Saved,Error))return false;SessionID=ID;Summaries=Saved;return true;}
FCasualtyCampaignTotals UCasualtyLedgerArchiveSubsystem::GetTotals() const
{FCasualtyCampaignTotals T;T.Cities=Summaries.Num();for(const auto& S:Summaries){T.Deaths+=S.Deaths;T.UniqueExposedSurvivors+=S.UniqueExposedSurvivors;T.RetrofitInvestments+=S.RetrofitInvestments;T.FlowerTributes+=S.FlowerTributes;}return T;}
void UCasualtyLedgerGenerator::SnapshotCurrentCity()
{
	UGameInstance* GI=GetWorld()?GetWorld()->GetGameInstance():nullptr;const auto* Session=GI?GI->GetSubsystem<UAftershockOpeningSessionSubsystem>():nullptr;
	auto* Archive=GI?GI->GetSubsystem<UCasualtyLedgerArchiveSubsystem>():nullptr;if(!Session||!Archive||!Session->GetSessionID().IsValid())return;
	const auto* Life=GetWorld()->GetSubsystem<UCityLifeSubsystem>();if(!Life||!Life->IsCityInitialized())return;
	if(!State.bHasCityKey)
	{
		State.bHasCityKey=true;State.CitySessionID=Session->GetSessionID();State.CityCampaignStage=Session->GetSettings().CampaignStage;State.CityRegionID=Session->GetSettings().RegionID;State.CityFocusID=Session->GetSettings().FocusID;
		const int32 SettingsRegion=FindAftershockGeographyIndex(State.CityRegionID);
		const int32 SettingsFocus=State.CityFocusID.IsNone()?0:FindAftershockGeographyFocusIndex(SettingsRegion,State.CityFocusID);
		if(const auto* Focus=TryGetAftershockGeographyFocus(SettingsRegion,SettingsFocus)){State.CityRegionID=FName(Focus->Id);State.CityFocusID=FName(Focus->FocusId);}
		if(const auto* Scenario=GetWorld()->GetSubsystem<UAftershockScenarioSubsystem>())if(Scenario->IsPrepared())State.CityCampaignStage=Scenario->GetScenarioIndex();
		// Stage settings can still carry an old region key in campaign mode;
		// bind geography from the actual canonical world, once per city.
		if(const auto* Terrain=GetWorld()->GetSubsystem<UTerrainGridSubsystem>())if(const auto* Focus=TryGetAftershockGeographyFocus(Terrain->GeographyIndex,Terrain->GeographyFocusIndex))
		{State.CityRegionID=FName(Focus->Id);State.CityFocusID=FName(Focus->FocusId);}
	}
	// A new session can be configured in the old world's menu. Its old ledger
	// must not repopulate/reset the archive after the new SessionID was selected.
	if(State.CitySessionID!=Session->GetSessionID())return;
	FCasualtyCitySummary S;S.SessionID=State.CitySessionID;S.CampaignStage=State.CityCampaignStage;S.RegionID=State.CityRegionID;S.FocusID=State.CityFocusID;
	S.Deaths=GetSessionDeaths();S.LivingPopulation=GetSessionSurvivors();S.UniqueExposedSurvivors=0;
	for(int32 ID:State.ExposedSurvivorResidentIDs){FCityResidentRecord R;if(Life->GetResident(ID,R)&&R.bAlive)++S.UniqueExposedSurvivors;}
	S.RetrofitInvestments=GetRetrofitInvestmentCount();S.FlowerTributes=GetFlowerTributeCount();Archive->ReplaceCitySnapshot(S);
}
FCasualtyCampaignTotals UCasualtyLedgerGenerator::GetCampaignTotals()
{SnapshotCurrentCity();const auto* GI=GetWorld()?GetWorld()->GetGameInstance():nullptr;const auto* A=GI?GI->GetSubsystem<UCasualtyLedgerArchiveSubsystem>():nullptr;return A?A->GetTotals():FCasualtyCampaignTotals();}
void UCasualtyLedgerGenerator::CaptureSaveState(FAftershockCasualtyLedgerSaveState& Out) const
{const_cast<UCasualtyLedgerGenerator*>(this)->SnapshotCurrentCity();Out=State;const auto* GI=GetWorld()?GetWorld()->GetGameInstance():nullptr;if(const auto* A=GI?GI->GetSubsystem<UCasualtyLedgerArchiveSubsystem>():nullptr)A->CaptureArchive(Out.ArchiveSessionID,Out.ArchiveSummaries);}
TArray<FCitizenMicroStory> UCasualtyLedgerGenerator::GetEntriesPage(int32 Offset,int32 Limit) const
{TArray<FCitizenMicroStory> Result;for(int32 I=FMath::Max(0,Offset);I<State.Entries.Num()&&Result.Num()<FMath::Clamp(Limit,0,100);++I)Result.Add(State.Entries[I]);return Result;}
TArray<FCitizenMicroStory> UCasualtyLedgerGenerator::GetNarrativePreview(int32 Limit) const
{TArray<FCitizenMicroStory> Result=GetEntriesPage(0,FMath::Clamp(Limit,0,12));for(const auto& S:State.SurvivalStories){if(Result.Num()>=FMath::Clamp(Limit,0,12))break;Result.Add(S);}return Result;}
TArray<FCitizenMicroStory> UCasualtyLedgerGenerator::GetBuildingNarrativePreview(int32 BuildingID,int32 Limit) const
{
	TArray<FCitizenMicroStory> Result;const int32 Cap=FMath::Clamp(Limit,0,12);if(BuildingID<1||Cap==0)return Result;
	TMap<FGuid,int32> EventOrder;for(int32 I=0;I<State.Events.Num();++I)EventOrder.Add(State.Events[I].EventID,I);
	const auto Consider=[&](const FCitizenMicroStory& Story)
	{
		if(Story.BuildingID!=BuildingID)return;
		const int32 Order=EventOrder.FindRef(Story.DisasterEventID);int32 At=0;
		while(At<Result.Num()&&EventOrder.FindRef(Result[At].DisasterEventID)>=Order)++At;
		if(At<Cap){Result.Insert(Story,At);if(Result.Num()>Cap)Result.SetNum(Cap);}
	};
	for(const auto& Story:State.Entries)Consider(Story);
	for(const auto& Story:State.BuildingWitnessStories)Consider(Story);
	return Result;
}
TArray<int32> UCasualtyLedgerGenerator::GetBuildingCasualtyIDs(int32 ID) const
{TArray<int32> Result;for(const auto& E:State.Entries)if(E.BuildingID==ID)Result.Add(E.ResidentID);return Result;}
bool UCasualtyLedgerGenerator::PlaceFlower(int32 ID)
{if(State.FlowerResidentIDs.Contains(ID)||!State.Entries.ContainsByPredicate([ID](const auto& E){return E.ResidentID==ID;}))return false;State.FlowerResidentIDs.Add(ID);SnapshotCurrentCity();if(auto* M=GetWorld()->GetSubsystem<UBuildingManagerSubsystem>())M->RebuildMemorialVisuals();OnLedgerChanged.Broadcast();return true;}
void UCasualtyLedgerGenerator::RecordRetrofitInvestment(int32 ID)
{if(ID>0&&!State.RetrofittedBuildingIDs.Contains(ID)){State.RetrofittedBuildingIDs.Add(ID);SnapshotCurrentCity();OnLedgerChanged.Broadcast();}}
bool UCasualtyLedgerGenerator::HasRecordedEvent(const FGuid& ID) const
{return State.Events.ContainsByPredicate([&ID](const auto& E){return E.EventID==ID;});}

bool UCasualtyLedgerGenerator::ValidateDisasterSettlement(const FDisasterReport& Report) const
{
	const auto* Event=State.Events.FindByPredicate([&](const auto& E){return E.EventID==Report.DisasterEventID;});
	if(!Event||Event->bHistoricalUnattributed||!Event->bHasSettlementInputs||Report.bWasRejected
		||!FMath::IsFinite(Report.BaseSeverity)||Report.BaseSeverity<0.f
		||Report.bNeedsCasualtyEvent!=Event->bNeedsCasualtyEvent||Report.DisasterType!=Event->DisasterType||Report.BaseSeverity!=Event->BaseSeverity
		||Report.TotalDeaths!=Event->ActualDeaths||Report.TotalSurvivors!=Event->ExposedSurvivors
		||Report.TotalDeathCostTL!=Event->DeathCostTL||Report.TotalStateAidGranted!=Event->SurvivorAidTL)return false;
	const auto SameIDs=[](const TArray<int32>& A,const TArray<int32>& B)
	{if(A.Num()!=B.Num())return false;TSet<int32> IDs;for(int32 ID:A){if(ID<1||IDs.Contains(ID))return false;IDs.Add(ID);}for(int32 ID:B)if(!IDs.Remove(ID))return false;return IDs.IsEmpty();};
	return SameIDs(Event->ActualDeathResidentIDs,Report.ActualDeathResidentIDs)&&SameIDs(Event->ExposedSurvivorResidentIDs,Report.ExposedSurvivorResidentIDs);
}

FCitizenMicroStory UCasualtyLedgerGenerator::MakeStory(const FCityResidentRecord& R,const FBuildingInstance& B,const FBuildingInstance* HistoricalWorkplace,
	const FGuid& EventID,EDisasterType Type,int32 Turn,ECitizenMicroStoryOutcome Outcome) const
{
	FCitizenMicroStory Story;Story.ResidentID=R.ResidentID;GetStableDemographics(R.ResidentID,R.bWorkingAge,Story.FullName,Story.Age);
	if(!R.FullName.IsEmpty())Story.FullName=R.FullName;if(R.Age>0)Story.Age=R.Age;
	Story.Occupation=FText::FromString(R.bWorkingAge?TEXT("İş ataması olmayan sakin"):TEXT("Çalışma çağında olmayan sakin"));
	if(HistoricalWorkplace&&HistoricalWorkplace->BuildingData)
		Story.Occupation=FText::FromString(HistoricalWorkplace->BuildingData->DisplayName.ToString()+TEXT(" çalışanı"));
	Story.DisasterEventID=EventID;Story.DisasterType=Type;Story.DisasterTurn=Turn;Story.BuildingID=B.InstanceID;Story.BuildingCoord=B.GridCoords;Story.Outcome=Outcome;
	Story.bHasBuildingSnapshot=B.BuildingData!=nullptr;
	if(B.BuildingData){Story.BuildingDisplayName=B.BuildingData->DisplayName;Story.BuildingDefinitionType=B.BuildingData->Type;Story.Material=B.BuildingData->Material;}else Story.BuildingDisplayName=FText::FromString(TEXT("Yapı kaydı mevcut değil"));Story.Foundation=B.Foundation;Story.bIsRetrofitted=B.bIsRetrofitted;
	const auto MaterialName=StaticEnum<EMaterialType>()->GetDisplayNameTextByValue(static_cast<int64>(Story.Material));
	const auto FoundationName=StaticEnum<EFoundationQuality>()->GetDisplayNameTextByValue(static_cast<int64>(Story.Foundation));
	Story.Narrative=FText::FromString(FString::Printf(TEXT("%s, %d — %s. Afet anındaki yapı: %s. Malzeme: %s; temel: %s; güçlendirme: %s. %s"),
		*Story.FullName.ToString(),Story.Age,*Story.Occupation.ToString(),*Story.BuildingDisplayName.ToString(),*MaterialName.ToString(),*FoundationName.ToString(),
		Story.bIsRetrofitted?TEXT("uygulanmış"):TEXT("uygulanmamış"),Outcome==ECitizenMicroStoryOutcome::Deceased?TEXT("Bu afette hayatını kaybetti."):TEXT("Afete maruz kaldı ve hayatta kaldı.")));
	return Story;
}

bool UCasualtyLedgerGenerator::RecordDisaster(const FGuid& EventID,EDisasterType Type,int32 Turn,bool bDaytime,const TArray<FCityResidentRecord>& Before,
	const TArray<FBuildingInstance>& Buildings,const TArray<int32>& DeathIDs,const TArray<int32>& SurvivorIDs,bool bNeedsEvent,float BaseSeverity)
{
	const UCityLifeSubsystem* Life=GetWorld()?GetWorld()->GetSubsystem<UCityLifeSubsystem>():nullptr;
	if(!EventID.IsValid()||HasRecordedEvent(EventID)||!Life||Turn<1||!FMath::IsFinite(BaseSeverity)||BaseSeverity<0.f
		||static_cast<uint8>(Type)>static_cast<uint8>(EDisasterType::Landslide)||(bNeedsEvent&&!SurvivorIDs.IsEmpty()))return false;
	TSet<int32> Seen;TArray<FCitizenMicroStory> NewDeaths,NewSurvival;TMap<int32,FCitizenMicroStory> WitnessByBuilding;
	TMap<int32,const FCityResidentRecord*> BeforeByID;for(const auto& R:Before)BeforeByID.Add(R.ResidentID,&R);
	TMap<int32,const FBuildingInstance*> BuildingsByID,HistoricalWorkByID;
	for(const auto& B:Buildings)
	{
		BuildingsByID.Add(B.InstanceID,&B);
		// Use the same pre-event records and first-match semantics as the former
		// linear workplace search. Never read a reassigned/deleted live employer.
		if(!HistoricalWorkByID.Contains(B.InstanceID))HistoricalWorkByID.Add(B.InstanceID,&B);
	}
	TSet<int32> RecordedDeathIDs;for(const auto& E:State.Entries)RecordedDeathIDs.Add(E.ResidentID);
	for(const bool bDeaths:{true,false})for(int32 ID:bDeaths?DeathIDs:SurvivorIDs)
	{
		const auto* PriorPtr=BeforeByID.Find(ID);const auto* Prior=PriorPtr?*PriorPtr:nullptr;FCityResidentRecord Current;
		if(Seen.Contains(ID)||!Prior||!Prior->bAlive||!Life->GetResident(ID,Current)||Current.bAlive==bDeaths)return false;Seen.Add(ID);
		const int32 At=bDaytime&&Prior->WorkBuildingID!=INDEX_NONE?Prior->WorkBuildingID:Prior->HomeBuildingID;
		const auto* BuildingPtr=BuildingsByID.Find(At);const auto* Building=BuildingPtr?*BuildingPtr:nullptr;
		if(((!Building||!Building->BuildingData)&&!bNeedsEvent)||RecordedDeathIDs.Contains(ID))return false;
		FBuildingInstance Missing;Missing.InstanceID=At;
		const FBuildingInstance* HistoricalWorkplace=HistoricalWorkByID.FindRef(Prior->WorkBuildingID);
		if(bDeaths)NewDeaths.Add(MakeStory(*Prior,Building?*Building:Missing,HistoricalWorkplace,EventID,Type,Turn,ECitizenMicroStoryOutcome::Deceased));
		else
		{
			if(!Building||!Building->BuildingData)return false;
			auto* Witness=WitnessByBuilding.Find(At);
			if(!Witness||ID<Witness->ResidentID)WitnessByBuilding.Add(At,MakeStory(*Prior,*Building,HistoricalWorkplace,EventID,Type,Turn,ECitizenMicroStoryOutcome::Survived));
			if(NewSurvival.Num()<12)NewSurvival.Add(MakeStory(*Prior,*Building,HistoricalWorkplace,EventID,Type,Turn,ECitizenMicroStoryOutcome::Survived));
		}
	}
	if(bNeedsEvent)for(auto& E:NewDeaths){E.Cause=ECitizenCasualtyCause::UnmetBasicNeeds;E.Narrative=FText::FromString(FString::Printf(TEXT("%s, %d — %s. Karşılanamayan temel ihtiyaçlar: açlık ve sağlık göstergeleri kritik eşiğe ulaştı."),*E.FullName.ToString(),E.Age,*E.Occupation.ToString()));}
	State.Entries.Append(NewDeaths);State.SurvivalStories=MoveTemp(NewSurvival);
	// One latest witness per actual building. An old witness remains factual even
	// if that person dies later; it records survival AT that earlier event.
	State.BuildingWitnessStories.RemoveAll([&](const auto& Story){return WitnessByBuilding.Contains(Story.BuildingID);});
	TArray<int32> WitnessBuildingIDs;WitnessByBuilding.GetKeys(WitnessBuildingIDs);WitnessBuildingIDs.Sort();
	for(int32 ID:WitnessBuildingIDs)State.BuildingWitnessStories.Add(MoveTemp(WitnessByBuilding.FindChecked(ID)));
	if(State.BuildingWitnessStories.Num()>100000)State.BuildingWitnessStories.RemoveAt(0,State.BuildingWitnessStories.Num()-100000);
	for(int32 ID:DeathIDs)State.ExposedSurvivorResidentIDs.Remove(ID);
	for(int32 ID:SurvivorIDs)State.ExposedSurvivorResidentIDs.Add(ID);
	FAftershockCasualtyEventRecord& Event=State.Events.AddDefaulted_GetRef();Event.EventID=EventID;Event.Turn=Turn;Event.ActualDeaths=DeathIDs.Num();Event.ExposedSurvivors=SurvivorIDs.Num();
	Event.bHasSettlementInputs=true;Event.bNeedsCasualtyEvent=bNeedsEvent;Event.DisasterType=Type;Event.BaseSeverity=BaseSeverity;
	Event.ActualDeathResidentIDs=DeathIDs;Event.ExposedSurvivorResidentIDs=SurvivorIDs;
	Event.DeathCostTL=static_cast<int64>(Event.ActualDeaths)*500;Event.SurvivorAidTL=static_cast<int64>(Event.ExposedSurvivors)*150;
	SnapshotCurrentCity();OnLedgerChanged.Broadcast();return true;
}

bool UCasualtyLedgerGenerator::ValidateSaveState(const FAftershockCasualtyLedgerSaveState& S,FText& Error) const
{
	const auto Fail=[&](){Error=FText::FromString(TEXT("Anma ve kayıp defteri kimlikleri veya mali kayıtları geçersiz."));return false;};
	if(S.Entries.Num()>100000||S.SurvivalStories.Num()>12||S.BuildingWitnessStories.Num()>100000||S.Events.Num()>100000)return Fail();
	if(S.bHasCityKey&&(!S.CitySessionID.IsValid()||S.CityCampaignStage<0||S.CityRegionID.IsNone()||S.ArchiveSessionID!=S.CitySessionID))return Fail();
	TSet<FGuid> Events;TMap<FGuid,const FAftershockCasualtyEventRecord*> EventByID;TMap<FGuid,int32> EventOrder;TMap<FGuid,int32> EventDeaths;TSet<int32> IDs;TMap<FGuid,bool> HistoricalEvents;TMap<int32,FGuid> RowEventByID;
	for(const auto& E:S.Events)
	{
		if(!E.EventID.IsValid()||Events.Contains(E.EventID)||E.Turn<1||E.ActualDeaths<0||E.ExposedSurvivors<0
			||(E.bHistoricalUnattributed?(E.DeathCostTL!=0||E.SurvivorAidTL!=0||E.ExposedSurvivors!=0):(E.DeathCostTL!=static_cast<int64>(E.ActualDeaths)*500||E.SurvivorAidTL!=static_cast<int64>(E.ExposedSurvivors)*150))
			||(E.bHasSettlementInputs&&(!FMath::IsFinite(E.BaseSeverity)||E.BaseSeverity<0.f||static_cast<uint8>(E.DisasterType)>static_cast<uint8>(EDisasterType::Landslide)||E.bHistoricalUnattributed||(E.bNeedsCasualtyEvent&&E.ExposedSurvivors!=0)))
			||(!E.bHasSettlementInputs&&(E.bNeedsCasualtyEvent||E.DisasterType!=EDisasterType::Earthquake||E.BaseSeverity!=0.f)))
		{
			return Fail();
		}
		EventOrder.Add(E.EventID,Events.Num());
		Events.Add(E.EventID);
		EventByID.Add(E.EventID,&E);
		HistoricalEvents.Add(E.EventID,E.bHistoricalUnattributed);
	}
	for(const auto& E:S.Entries){if(E.ResidentID<1||IDs.Contains(E.ResidentID)||!Events.Contains(E.DisasterEventID)||E.Outcome!=ECitizenMicroStoryOutcome::Deceased||E.Age<1||E.Age>120||E.FullName.IsEmpty()||(E.bHasBuildingSnapshot&&E.BuildingID<1)||static_cast<uint8>(E.Material)>4||static_cast<uint8>(E.Foundation)>2)return Fail();IDs.Add(E.ResidentID);RowEventByID.Add(E.ResidentID,E.DisasterEventID);++EventDeaths.FindOrAdd(E.DisasterEventID);}
	for(const auto& E:S.Events)if(EventDeaths.FindRef(E.EventID)!=E.ActualDeaths)return Fail();
	for(const auto& E:S.Events)
	{
		if(E.ActualDeathResidentIDs.Num()!=E.ActualDeaths||E.ExposedSurvivorResidentIDs.Num()!=E.ExposedSurvivors)return Fail();
		TSet<int32> EventIDs;
		for(int32 ID:E.ActualDeathResidentIDs){const auto* RowEvent=RowEventByID.Find(ID);if(ID<1||EventIDs.Contains(ID)||!RowEvent||*RowEvent!=E.EventID)return Fail();EventIDs.Add(ID);}
		for(int32 ID:E.ExposedSurvivorResidentIDs){if(ID<1||EventIDs.Contains(ID))return Fail();EventIDs.Add(ID);}
	}
	for(const auto& E:S.Entries)
	{
		const auto* Recorded=EventByID.FindChecked(E.DisasterEventID);
		if(static_cast<uint8>(E.Cause)>static_cast<uint8>(ECitizenCasualtyCause::HistoricalUnknown)
			||((E.Cause==ECitizenCasualtyCause::HistoricalUnknown)!=HistoricalEvents.FindRef(E.DisasterEventID))
			||(E.Cause==ECitizenCasualtyCause::HistoricalUnknown&&E.bHasBuildingSnapshot)
			||(Recorded->bHasSettlementInputs&&(E.DisasterTurn!=Recorded->Turn||E.DisasterType!=Recorded->DisasterType
				||((E.Cause==ECitizenCasualtyCause::UnmetBasicNeeds)!=Recorded->bNeedsCasualtyEvent))))return Fail();
	}
	for(int32 ID:S.FlowerResidentIDs)if(!IDs.Contains(ID))return Fail();
	for(int32 ID:S.RetrofittedBuildingIDs)if(ID<1)return Fail();
	for(int32 ID:S.ExposedSurvivorResidentIDs)if(ID<1||IDs.Contains(ID))return Fail();
	// Pure archive preflight never mutates the live game-instance archive.
	if(!UCasualtyLedgerArchiveSubsystem::ValidateArchive(S.ArchiveSessionID,S.ArchiveSummaries,Error))return false;
	for(const auto& E:S.SurvivalStories)if(E.ResidentID<1||E.Outcome!=ECitizenMicroStoryOutcome::Survived||!Events.Contains(E.DisasterEventID)||E.FullName.IsEmpty()||E.Age<1||E.Age>120)return Fail();
	TArray<EBuildingType> KnownDefinitions;FAftershockBuildingCatalog::GetTypes(KnownDefinitions);
	TMap<FGuid,TSet<int32>> WitnessResidentIDs;for(const auto& Story:S.BuildingWitnessStories)WitnessResidentIDs.FindOrAdd(Story.DisasterEventID).Add(Story.ResidentID);
	// Inspect each event's ID list once, rather than buildingCount*population.
	for(const auto& Event:S.Events)if(auto* Pending=WitnessResidentIDs.Find(Event.EventID))
	{
		if(Event.bHistoricalUnattributed)return Fail();
		for(int32 ID:Event.ExposedSurvivorResidentIDs)Pending->Remove(ID);
		if(!Pending->IsEmpty())return Fail();
	}
	TSet<int32> WitnessBuildings;
	for(const auto& Story:S.BuildingWitnessStories)
	{
		const auto* EventPtr=EventByID.Find(Story.DisasterEventID);const auto* Event=EventPtr?*EventPtr:nullptr;
		const auto* DeathEvent=RowEventByID.Find(Story.ResidentID);
		if(!Event||Story.ResidentID<1||Story.BuildingID<1||WitnessBuildings.Contains(Story.BuildingID)||!Story.bHasBuildingSnapshot
			||(DeathEvent&&EventOrder.FindRef(*DeathEvent)<=EventOrder.FindRef(Story.DisasterEventID))
			||!KnownDefinitions.Contains(Story.BuildingDefinitionType)||Story.BuildingDisplayName.IsEmpty()||Story.Narrative.IsEmpty()||Story.Occupation.IsEmpty()
			||Story.Outcome!=ECitizenMicroStoryOutcome::Survived||Story.Cause!=ECitizenCasualtyCause::Disaster||Story.DisasterTurn!=Event->Turn
			||(Event->bHasSettlementInputs&&(Event->bNeedsCasualtyEvent||Story.DisasterType!=Event->DisasterType))
			||static_cast<uint8>(Story.DisasterType)>static_cast<uint8>(EDisasterType::Landslide)||static_cast<uint8>(Story.Material)>4||static_cast<uint8>(Story.Foundation)>2
			||Story.FullName.IsEmpty()||Story.Age<1||Story.Age>120)return Fail();
		WitnessBuildings.Add(Story.BuildingID);
	}
	return true;
}
bool UCasualtyLedgerGenerator::RestoreSaveState(const FAftershockCasualtyLedgerSaveState& S,FText& Error)
{if(!ValidateSaveState(S,Error))return false;const auto* GI=GetWorld()?GetWorld()->GetGameInstance():nullptr;if(auto* A=GI?GI->GetSubsystem<UCasualtyLedgerArchiveSubsystem>():nullptr)if(!A->RestoreArchive(S.ArchiveSessionID,S.ArchiveSummaries,Error))return false;State=S;OnLedgerChanged.Broadcast();return true;}
void UCasualtyLedgerGenerator::ResetLedger(){State={};OnLedgerChanged.Broadcast();}

void UCasualtyLedgerGenerator::RecordNeedsFatalities(int32 Turn,const TArray<FCityResidentRecord>& Before,const TArray<FBuildingInstance>& Buildings,const TArray<int32>& IDs)
{
	if(IDs.IsEmpty())return;const FGuid Event=FGuid::NewGuid();
	if(!RecordDisaster(Event,EDisasterType::Drought,Turn,false,Before,Buildings,IDs,{},true))return;
	FDisasterReport Report;Report.DisasterEventID=Event;Report.DisasterType=EDisasterType::Drought;Report.bNeedsCasualtyEvent=true;Report.ActualDeathResidentIDs=IDs;Report.TotalDeaths=IDs.Num();Report.TotalDeathCostTL=static_cast<int64>(IDs.Num())*500;Report.TotalSatisfactionChange=-IDs.Num()*.3f;
	if(auto* E=GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>())E->HandleDisasterCompleted(Report);
}

void UCasualtyLedgerGenerator::Deinitialize()
{
	OnLedgerChanged.Clear();
	ResetLedger();
	Super::Deinitialize();
}
