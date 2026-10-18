#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "WaterGlassEpilogueWidget.generated.h"
class UWaterGlassFrameWidget;
class UTextBlock;
class UVerticalBox;
class UBorder;
class UCasualtyLedgerPanel;

UCLASS()
class JAMPROJECT_API UWaterGlassEpilogueWidget final : public UUserWidget
{
	GENERATED_BODY()
public:
	void SetOutcome(bool bSuccess,FText Reason);
	int64 GetDisplayedDeaths() const { return DisplayedDeaths; }
	int64 GetDisplayedSurvivors() const { return DisplayedSurvivors; }
	/** Uses the same handler as the actual epilogue ledger button. */
	bool OpenCasualtyLedger();
	bool IsCasualtyLedgerVisible() const;
	UCasualtyLedgerPanel* GetCasualtyLedgerPanel() const { return LedgerPanel; }
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& Geometry,float DeltaSeconds) override;
	virtual FReply NativeOnPreviewKeyDown(const FGeometry& Geometry,const FKeyEvent& Key) override;
protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
private:
	void BuildLayout();
	void RefreshEvidence();
	UFUNCTION() void HandleMainMenu();
	UFUNCTION() void HandleSave();
	UFUNCTION() void HandleLedger();
	void CloseLedger();
	UPROPERTY(Transient) TObjectPtr<UWaterGlassFrameWidget> Illustration;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ReflectionText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TotalsText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ReasonText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> FeedbackText;
	UPROPERTY(Transient) TObjectPtr<UBorder> LedgerOverlay;
	UPROPERTY(Transient) TObjectPtr<UCasualtyLedgerPanel> LedgerPanel;
	bool bCampaignSuccess=false;
	FText OutcomeReason;
	double StartedRealSeconds=0.;
	double NextEvidenceRealSeconds=0.;
	int32 DisplayedFlowers=0;
	bool bHasDeaths=false;
	int64 DisplayedDeaths=0;
	int64 DisplayedSurvivors=0;
};
