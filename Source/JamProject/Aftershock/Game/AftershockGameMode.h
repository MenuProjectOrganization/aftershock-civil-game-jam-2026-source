// AFTERSHOCK — Civil Game Jam 2026
// Level akışını yöneten GameMode. Üstteki Blueprint subclass'ı level config'i taşır.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Aftershock/Types/AftershockTypes.h"
#include "AftershockGameMode.generated.h"

class AGridManager;
class UDisasterSimulator;
class UDisasterData;
class AAftershockGameState;

UENUM(BlueprintType)
enum class EGameFlowState : uint8
{
	Intro,
	Playing,
	StreetView UMETA(DisplayName="Street View (reserved)"),
	Epilogue
};

UCLASS(BlueprintType)
class JAMPROJECT_API AAftershockGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AAftershockGameMode();
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	EGameFlowState GetGameFlowState() const { return GameFlowState; }
	bool IsNarrativeInputBlocked() const { return GameFlowState==EGameFlowState::Intro||GameFlowState==EGameFlowState::Epilogue; }
	void BeginIntro();
	void FinishIntro();
	void EvaluateGameFlow();
	bool WasCampaignSuccessful() const { return bCampaignSuccessful; }
	FText GetEndingReason() const { return EndingReason; }

	// Level config — Blueprint subclass'ta set edilir.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Level")
	TObjectPtr<UDisasterData> LevelDisaster;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Level")
	float LevelSeverityOverride = -1.f; // <0 = DisasterData range'inden random

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Level")
	float LevelWarningSeconds = 240.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Level")
	int32 StartingBudget = 10000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Level")
	float StartingSatisfaction = 70.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Refs")
	TObjectPtr<AGridManager> GridManager;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|Refs")
	TObjectPtr<UDisasterSimulator> Simulator;

	UFUNCTION(BlueprintCallable, Category="Aftershock")
	void BeginLevel();

	UFUNCTION(BlueprintCallable, Category="Aftershock")
	void ScheduleCurrentDisaster();

	UFUNCTION(BlueprintPure, Category="Aftershock")
	AAftershockGameState* GetAftershockState() const;

protected:
	virtual void BeginPlay() override;
	UFUNCTION() void HandleCanonicalGameOver(FText Reason);
	UFUNCTION() void HandleCanonicalEconomy(int64 Treasury,float Satisfaction,int32 Turn);
	UFUNCTION() void HandleCanonicalScenario();

	UFUNCTION()
	void HandleDisasterComplete(const FLevelOutcome& Outcome);
private:
	void EnterEpilogue(bool bSuccess,FText Reason);
	UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Aftershock|Flow",meta=(AllowPrivateAccess="true"))
	EGameFlowState GameFlowState=EGameFlowState::Playing;
	bool bCampaignSuccessful=false;
	FText EndingReason;
	FTimerHandle FlowTimer;
};
