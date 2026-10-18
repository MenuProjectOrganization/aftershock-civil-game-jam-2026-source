#pragma once
#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/AftershockPhysicalHazardState.h"
#include "AftershockSaveGame.generated.h"

UCLASS()
class JAMPROJECT_API UAftershockSaveGame final:public USaveGame
{
 GENERATED_BODY()
public:
 static constexpr int32 CurrentVersion=5;
 UPROPERTY() int32 SchemaVersion=CurrentVersion;
 UPROPERTY() FDateTime SavedAt;
 UPROPERTY() FGuid SessionID;
 UPROPERTY() FString MapPackage=TEXT("/Game/Aftershock/Levels/L_Level_01_Deprem");
 UPROPERTY() FAftershockSessionSettings Settings;
 UPROPERTY() FAftershockCampaignProgress Campaign;
 UPROPERTY() FAftershockTerrainSaveState Terrain;
 UPROPERTY() FAftershockBuildingsSaveState Buildings;
 UPROPERTY() TArray<FAftershockRubbleSaveRecord> Rubble;
 UPROPERTY() FAftershockDisasterVisualSnapshot DisasterVisual;
 UPROPERTY() float DisasterVisualRemainingSeconds=0.f;
 UPROPERTY() FAftershockEconomySaveState Economy;
 UPROPERTY() FAftershockCasualtyLedgerSaveState CasualtyLedger;
 UPROPERTY() FAftershockCityLifeSaveState CityLife;
 UPROPERTY() FAftershockTransportSaveState Transport;
 UPROPERTY() FAftershockWorldClimateState Climate;
 UPROPERTY() FAftershockScenarioSaveState Scenario;
 UPROPERTY() FAftershockDirectorState Director;
 UPROPERTY() FAftershockCrowdSaveState Crowd;
 UPROPERTY() FAftershockQuizSaveState Quiz;
 UPROPERTY() FAftershockCameraSaveState Camera;
 UPROPERTY() FAftershockPhysicalHazardSaveState Physics;
};
