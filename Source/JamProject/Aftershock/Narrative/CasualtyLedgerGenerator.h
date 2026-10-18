#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Aftershock/Public/BuildingData.h"
#include "CasualtyLedgerGenerator.generated.h"

struct FCityResidentRecord;
struct FDisasterReport;

UENUM(BlueprintType)
enum class ECitizenMicroStoryOutcome:uint8 { Survived,Deceased };
UENUM(BlueprintType)
enum class ECitizenCasualtyCause:uint8 { Disaster,UnmetBasicNeeds,HistoricalUnknown };

/** A factual simulation identity attached to a real canonical resident ID. */
USTRUCT(BlueprintType)
struct JAMPROJECT_API FCitizenMicroStory
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) int32 ResidentID=INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) FText FullName;
	UPROPERTY(BlueprintReadOnly) int32 Age=0;
	UPROPERTY(BlueprintReadOnly) FText Occupation;
	UPROPERTY(BlueprintReadOnly) FText BuildingDisplayName;
	UPROPERTY(BlueprintReadOnly) EBuildingType BuildingDefinitionType=EBuildingType::House;
	UPROPERTY(BlueprintReadOnly) bool bHasBuildingSnapshot=true;
	UPROPERTY(BlueprintReadOnly) EMaterialType Material=EMaterialType::Concrete;
	UPROPERTY(BlueprintReadOnly) EFoundationQuality Foundation=EFoundationQuality::Medium;
	UPROPERTY(BlueprintReadOnly) bool bIsRetrofitted=false;
	UPROPERTY(BlueprintReadOnly) ECitizenMicroStoryOutcome Outcome=ECitizenMicroStoryOutcome::Deceased;
	UPROPERTY(BlueprintReadOnly) ECitizenCasualtyCause Cause=ECitizenCasualtyCause::Disaster;
	UPROPERTY(BlueprintReadOnly) FText Narrative;
	UPROPERTY(BlueprintReadOnly) FGuid DisasterEventID;
	UPROPERTY(BlueprintReadOnly) EDisasterType DisasterType=EDisasterType::Earthquake;
	UPROPERTY(BlueprintReadOnly) int32 DisasterTurn=1;
	UPROPERTY(BlueprintReadOnly) int32 BuildingID=INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) FIntPoint BuildingCoord=FIntPoint::ZeroValue;
};

using FCasualtyLedgerEntry=FCitizenMicroStory;

USTRUCT(BlueprintType)
struct JAMPROJECT_API FResidentIdentityCard
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) int32 ResidentID=INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) FText FullName;
	UPROPERTY(BlueprintReadOnly) int32 Age=0;
	UPROPERTY(BlueprintReadOnly) FText Occupation;
	UPROPERTY(BlueprintReadOnly) bool bAlive=true;
	UPROPERTY(BlueprintReadOnly) int32 HomeBuildingID=INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) int32 WorkBuildingID=INDEX_NONE;
};

USTRUCT()
struct JAMPROJECT_API FAftershockCasualtyEventRecord
{
	GENERATED_BODY()
	UPROPERTY() FGuid EventID;
	UPROPERTY() int32 Turn=1;
	UPROPERTY() int32 ActualDeaths=0;
	UPROPERTY() int32 ExposedSurvivors=0;
	UPROPERTY() int64 DeathCostTL=0;
	UPROPERTY() int64 SurvivorAidTL=0;
	UPROPERTY() bool bHistoricalUnattributed=false;
	/** Missing in earlier Schema 4 saves. Such already-settled events remain
	 * readable, but may never be replayed using inferred hazard inputs. */
	UPROPERTY() bool bHasSettlementInputs=false;
	UPROPERTY() bool bNeedsCasualtyEvent=false;
	UPROPERTY() EDisasterType DisasterType=EDisasterType::Earthquake;
	UPROPERTY() float BaseSeverity=0.f;
	UPROPERTY() TArray<int32> ActualDeathResidentIDs;
	UPROPERTY() TArray<int32> ExposedSurvivorResidentIDs;
};

USTRUCT(BlueprintType)
struct JAMPROJECT_API FCasualtyCitySummary
{
	GENERATED_BODY()
	UPROPERTY() FGuid SessionID;
	UPROPERTY() int32 CampaignStage=0;
	UPROPERTY() FName RegionID;
	UPROPERTY() FName FocusID;
	UPROPERTY() int32 Deaths=0;
	UPROPERTY() int32 LivingPopulation=0;
	UPROPERTY() int32 UniqueExposedSurvivors=0;
	UPROPERTY() int32 RetrofitInvestments=0;
	UPROPERTY() int32 FlowerTributes=0;
};
USTRUCT(BlueprintType)
struct JAMPROJECT_API FCasualtyCampaignTotals
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) int64 Deaths=0;
	UPROPERTY(BlueprintReadOnly) int64 UniqueExposedSurvivors=0;
	UPROPERTY(BlueprintReadOnly) int64 RetrofitInvestments=0;
	UPROPERTY(BlueprintReadOnly) int64 FlowerTributes=0;
	UPROPERTY(BlueprintReadOnly) int32 Cities=0;
};

USTRUCT()
struct JAMPROJECT_API FAftershockCasualtyLedgerSaveState
{
	GENERATED_BODY()
	UPROPERTY() TArray<FCitizenMicroStory> Entries;
	UPROPERTY() TArray<FCitizenMicroStory> SurvivalStories;
	/** Latest actual survivor witness per building, capped at 100000 buildings.
	 * Older saves can legitimately have no witnesses; none are fabricated on load. */
	UPROPERTY() TArray<FCitizenMicroStory> BuildingWitnessStories;
	UPROPERTY() TArray<FAftershockCasualtyEventRecord> Events;
	UPROPERTY() TSet<int32> FlowerResidentIDs;
	UPROPERTY() TSet<int32> RetrofittedBuildingIDs;
	UPROPERTY() TSet<int32> ExposedSurvivorResidentIDs;
	UPROPERTY() FGuid ArchiveSessionID;
	UPROPERTY() TArray<FCasualtyCitySummary> ArchiveSummaries;
	/** Bound once to this world; GI settings can advance BEFORE level travel. */
	UPROPERTY() bool bHasCityKey=false;
	UPROPERTY() FGuid CitySessionID;
	UPROPERTY() int32 CityCampaignStage=0;
	UPROPERTY() FName CityRegionID;
	UPROPERTY() FName CityFocusID;
};

/** Replaces a city's snapshot, never adds it again on travel/load/endplay. */
UCLASS()
class JAMPROJECT_API UCasualtyLedgerArchiveSubsystem final:public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	void ReplaceCitySnapshot(const FCasualtyCitySummary& Summary);
	void CaptureArchive(FGuid& OutSession,TArray<FCasualtyCitySummary>& Out) const {OutSession=SessionID;Out=Summaries;}
	bool RestoreArchive(FGuid Session,const TArray<FCasualtyCitySummary>& Saved,FText& Error);
	static bool ValidateArchive(FGuid Session,const TArray<FCasualtyCitySummary>& Saved,FText& Error);
	FCasualtyCampaignTotals GetTotals() const;
private:
	UPROPERTY() FGuid SessionID;
	UPROPERTY() TArray<FCasualtyCitySummary> Summaries;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnCasualtyLedgerChanged);

/** Current city's canonical casualties and player tributes. Counts read CityLife;
 * they never add survivors from successive disasters or create another population.
 * A campaign that loads another city needs its own city-qualified archive. */
UCLASS()
class JAMPROJECT_API UCasualtyLedgerGenerator final:public UWorldSubsystem
{
	GENERATED_BODY()
public:
	virtual void Deinitialize() override;
	static void GetStableDemographics(int32 ResidentID,bool bWorkingAge,FText& OutName,int32& OutAge);
	static void BuildLegacyLedgerState(const TArray<FCityResidentRecord>& Residents,FGuid SessionID,int32 Turn,FAftershockCasualtyLedgerSaveState& Out);
	bool GetResidentIdentityCard(int32 ResidentID,FResidentIdentityCard& Out) const;
	int32 GetSessionDeaths() const;
	int32 GetSessionSurvivors() const;
	FCasualtyCampaignTotals GetCampaignTotals();
	void SnapshotCurrentCity();
	const TArray<FCitizenMicroStory>& GetEntries() const {return State.Entries;}
	TArray<FCitizenMicroStory> GetEntriesPage(int32 Offset,int32 Limit=50) const;
	TArray<FCitizenMicroStory> GetNarrativePreview(int32 Limit=12) const;
	/** Newest first, actual death rows plus the building's latest living-at-event witness. */
	TArray<FCitizenMicroStory> GetBuildingNarrativePreview(int32 BuildingID,int32 Limit=4) const;
	TArray<int32> GetBuildingCasualtyIDs(int32 BuildingID) const;
	bool PlaceFlower(int32 ResidentID);
	bool HasFlowerTribute(int32 ResidentID) const {return State.FlowerResidentIDs.Contains(ResidentID);}
	int32 GetFlowerTributeCount() const {return State.FlowerResidentIDs.Num();}
	int32 GetRetrofitInvestmentCount() const {return State.RetrofittedBuildingIDs.Num();}
	void RecordRetrofitInvestment(int32 BuildingID);
	/** The caller supplies pre-disaster canonical records and ACTUAL assigned IDs. */
	bool RecordDisaster(const FGuid& EventID,EDisasterType Type,int32 Turn,bool bDaytime,
		const TArray<FCityResidentRecord>& Before,const TArray<FBuildingInstance>& Buildings,
		const TArray<int32>& ActualDeathIDs,const TArray<int32>& ExposedSurvivorIDs,bool bNeedsEvent=false,float BaseSeverity=0.f);
	bool HasRecordedEvent(const FGuid& EventID) const;
	/** Rejects a forged first report before economics marks the GUID consumed. */
	bool ValidateDisasterSettlement(const FDisasterReport& Report) const;
	void CaptureSaveState(FAftershockCasualtyLedgerSaveState& Out) const;
	bool ValidateSaveState(const FAftershockCasualtyLedgerSaveState& Saved,FText& OutError) const;
	bool RestoreSaveState(const FAftershockCasualtyLedgerSaveState& Saved,FText& OutError);
	void ResetLedger();
	/** Basic-needs deaths are factual service outcomes, never invented disasters. */
	void RecordNeedsFatalities(int32 Turn,const TArray<FCityResidentRecord>& Before,const TArray<FBuildingInstance>& Buildings,const TArray<int32>& IDs);
	UPROPERTY(BlueprintAssignable) FOnCasualtyLedgerChanged OnLedgerChanged;
private:
	FCitizenMicroStory MakeStory(const FCityResidentRecord& Person,const FBuildingInstance& Building,
		const FBuildingInstance* HistoricalWorkplace,const FGuid& EventID,EDisasterType Type,int32 Turn,ECitizenMicroStoryOutcome Outcome) const;
	UPROPERTY() FAftershockCasualtyLedgerSaveState State;
};
