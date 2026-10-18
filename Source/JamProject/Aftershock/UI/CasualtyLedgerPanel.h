#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "CasualtyLedgerPanel.generated.h"
class UVerticalBox;
class UTextBlock;
class UScrollBox;
class UBorder;
class UCasualtyFlowerButton;
class UCasualtyLedgerPanel;
DECLARE_MULTICAST_DELEGATE(FOnCasualtyPanelClosed);

UCLASS()
class JAMPROJECT_API UCasualtyFlowerButton final : public UButton
{
	GENERATED_BODY()
public:
	void Configure(UCasualtyLedgerPanel* InPanel,int32 InResidentID);
private:
	UFUNCTION() void HandleClick();
	UPROPERTY(Transient) TWeakObjectPtr<UCasualtyLedgerPanel> Panel;
	int32 ResidentID=INDEX_NONE;
};

/** Paged read-only canonical identities; the sole write is an idempotent tribute. */
UCLASS()
class JAMPROJECT_API UCasualtyLedgerPanel final : public UUserWidget
{
	GENERATED_BODY()
public:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	UFUNCTION() void RefreshLedger();
	void LeaveFlower(int32 ResidentID);
	FOnCasualtyPanelClosed OnCloseRequested;
protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
private:
	void BuildLayout();
	UFUNCTION() void HandlePrevious();
	UFUNCTION() void HandleNext();
	UFUNCTION() void HandleClose();
	UPROPERTY(Transient) TObjectPtr<UTextBlock> Summary;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> PageText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> Feedback;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> Rows;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> Scroll;
	UPROPERTY(Transient) TObjectPtr<UButton> Previous;
	UPROPERTY(Transient) TObjectPtr<UButton> Next;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> EmptyNotice;
	UPROPERTY(Transient) TArray<TObjectPtr<UBorder>> PooledCards;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> PooledNameAgeTexts;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> PooledOccupationTexts;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> PooledStoryTexts;
	UPROPERTY(Transient) TArray<TObjectPtr<UCasualtyFlowerButton>> PooledFlowerButtons;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> PooledFlowerTexts;
	int32 Page=0;
};
