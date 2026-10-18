#pragma once
#include "CoreMinimal.h"
#include "Aftershock/Public/BuildingData.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "AftershockSessionTypes.generated.h"

UENUM(BlueprintType)
enum class EAftershockPlayMode:uint8 { Sandbox, GeographicSimulation, Campaign };
UENUM(BlueprintType)
enum class EAftershockDisasterPhase:uint8 { Ready, Warning, Aftermath };

USTRUCT(BlueprintType)
struct FAftershockSessionSettings
{
 GENERATED_BODY()
 UPROPERTY(EditAnywhere,BlueprintReadWrite) EAftershockPlayMode Mode=EAftershockPlayMode::Campaign;
 UPROPERTY(EditAnywhere,BlueprintReadWrite) FName RegionID=TEXT("atakent");
 UPROPERTY(EditAnywhere,BlueprintReadWrite) FName FocusID=NAME_None;
 UPROPERTY(EditAnywhere,BlueprintReadWrite) int32 Seed=1337;
 UPROPERTY(EditAnywhere,BlueprintReadWrite) float Difficulty=1.f;
 UPROPERTY(EditAnywhere,BlueprintReadWrite) bool bUnlimitedBudget=false;
 UPROPERTY(EditAnywhere,BlueprintReadWrite) int32 CampaignStage=0;
};

USTRUCT()
struct FAftershockCampaignProgress
{
 GENERATED_BODY()
 UPROPERTY() TSet<int32> CompletedStages;
 UPROPERTY() int32 FurthestStage=0;
};

USTRUCT()
struct FAftershockDirectorState
{
 GENERATED_BODY()
 UPROPERTY() EAftershockDisasterPhase Phase=EAftershockDisasterPhase::Ready;
 UPROPERTY() EDisasterType PendingType=EDisasterType::Earthquake;
 UPROPERTY() float RemainingSeconds=0.f;
 UPROPERTY() float WarningDuration=25.f;
 UPROPERTY() float Severity=42.f;
 UPROPERTY() int32 LastProcessedTurn=1;
 UPROPERTY() int32 LastTriggerTurn=0;
 UPROPERTY() int32 RandomSeed=1337;
 UPROPERTY() int64 EventSequence=0;
 UPROPERTY() int64 LastAppliedEventSequence=0;
 UPROPERTY() FDisasterReport LastReport;
};
