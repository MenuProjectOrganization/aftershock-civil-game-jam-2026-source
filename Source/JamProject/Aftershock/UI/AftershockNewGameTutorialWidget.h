#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Containers/Ticker.h"
#include "AftershockNewGameTutorialWidget.generated.h"

class UBorder;
class UButton;
class UProgressBar;
class UTextBlock;

/** New-game-only overview. Presentation clock, never a city simulation owner. */
UCLASS()
class JAMPROJECT_API UAftershockNewGameTutorialWidget final : public UUserWidget
{
    GENERATED_BODY()
public:
    static constexpr int32 TutorialStepCount=6;
    static constexpr double SecondsPerStep=4.75;
    static constexpr double MaximumRealDurationSeconds=30.;
    static constexpr double ClosingSeconds=.25;
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual FReply NativeOnPreviewKeyDown(const FGeometry& Geometry,const FKeyEvent& Event) override;
    virtual FReply NativeOnMouseButtonDown(const FGeometry& Geometry,const FPointerEvent& Event) override;
    void NextStep();
    void PreviousStep();
    void SkipTutorial();
    int32 GetCurrentStep() const { return CurrentStep; }
    int32 GetStepCount() const { return Steps.Num(); }
    bool HasCompletedTutorial() const { return bCompleted; }
    double GetElapsedRealSeconds() const;
    FText GetCurrentHeadline() const;
    /** Used by the actual ticker and native policy checks. */
    static bool HasReachedDeadline(double ElapsedSeconds);
    static int32 ComputeElapsedStepCount(double StepElapsedSeconds);
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
private:
    struct FStep { FText Title,Body,Controls; };
    void BuildLayout();
    void BuildSteps();
    void ShowStep(int32 Index,bool bResetStepClock);
    bool TickTutorial(float IgnoredDeltaSeconds);
    void BeginFinish(const TCHAR* Reason);
    void FinishNow(const TCHAR* Reason);
    void StopClock();
    void PlayNavigationClick();
    UFUNCTION() void HandleNext();
    UFUNCTION() void HandleBack();
    UFUNCTION() void HandleSkip();
    UPROPERTY(Transient) TObjectPtr<UBorder> Card;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> StepLabel;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Headline;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Body;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Controls;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> TimeLabel;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> NextLabel;
    UPROPERTY(Transient) TObjectPtr<UButton> BackButton;
    UPROPERTY(Transient) TObjectPtr<UButton> NextButton;
    UPROPERTY(Transient) TObjectPtr<UButton> SkipButton;
    UPROPERTY(Transient) TObjectPtr<UProgressBar> Progress;
    UPROPERTY(Transient) TArray<TObjectPtr<UBorder>> StepSegments;
    TArray<FStep> Steps;
    FTSTicker::FDelegateHandle RealClock;
    double StartedAt=0.,StepStartedAt=0.,FinishStartedAt=-1.,CompletedAt=-1.;
    int32 CurrentStep=0,LastRemainingSecond=INDEX_NONE;
    FString FinishReason;
    bool bCompleted=false;
};
