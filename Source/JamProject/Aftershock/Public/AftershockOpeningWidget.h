#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Aftershock/Public/AftershockSessionTypes.h"
#include "Components/ComboBoxString.h"
#include "AftershockOpeningWidget.generated.h"

class UButton;
class UBorder;
class UTexture2D;
class UEditableTextBox;
class UCheckBox;
class UTextBlock;

/** Constructor-only Slate foreground also keeps the native dropdown arrow legible. */
UCLASS()
class JAMPROJECT_API UAftershockMenuComboBox final : public UComboBoxString
{
	GENERATED_BODY()
public:
	UAftershockMenuComboBox(const FObjectInitializer& ObjectInitializer);
};

/** Session settings and campaign progress survive level travel; world state belongs to its owners. */
UCLASS()
class JAMPROJECT_API UAftershockOpeningSessionSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	bool HasLiveSession() const { return bHasLiveSession; }
	bool HasConfiguredSession() const { return bHasConfiguredSession; }
	void MarkSessionStarted() { bHasLiveSession = true; if (!SessionID.IsValid()) SessionID=FGuid::NewGuid(); }
	const FAftershockSessionSettings& GetSettings() const { return Settings; }
	const FAftershockCampaignProgress& GetCampaignProgress() const { return Campaign; }
	FGuid GetSessionID() const { return SessionID; }
	bool ConfigureNewSession(const FAftershockSessionSettings& NewSettings,FText& Error);
	void RestoreSession(const FAftershockSessionSettings& NewSettings,const FAftershockCampaignProgress& Progress,FGuid ID);
	void CompleteCampaignStage(int32 Stage);
	bool AdvanceCampaignStage(int32 Stage,FText& Error);
	bool IsUnlimitedBudget() const { return Settings.Mode==EAftershockPlayMode::Sandbox&&Settings.bUnlimitedBudget; }
	static bool ValidateSettings(const FAftershockSessionSettings& Settings,FText& Error);
private:
	bool bHasLiveSession = false;
	bool bHasConfiguredSession=false;
	UPROPERTY() FAftershockSessionSettings Settings;
	UPROPERTY() FAftershockCampaignProgress Campaign;
	UPROPERTY() FGuid SessionID;
};

/** Native opening screen. The supplied artwork remains intact under a fitted1280x720 layout. */
UCLASS()
class JAMPROJECT_API UAftershockOpeningWidget final : public UUserWidget
{
	GENERATED_BODY()
public:
	UAftershockOpeningWidget(const FObjectInitializer& ObjectInitializer);
	virtual void NativeConstruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	void RefreshSessionActions();
	void FocusInitialAction();
	void ShowSessionError(FText Error);
	/** Preselect only; the existing confirmation button is still required. */
	void ProposeGeographicNewGame(int32 RegionIndex,int32 FocusIndex);
protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
private:
	void BuildOpeningLayout();
	void PlayClickFeedback();
	void RefreshModePresentation();
	void RefreshRegionPresentation();
	UFUNCTION() UWidget* GenerateChoiceWidget(FString Item);
	UFUNCTION() void HandleCampaignCard();
	UFUNCTION() void HandleGeographyCard();
	UFUNCTION() void HandleSandboxCard();
	UFUNCTION() void HandleNewGame();
	UFUNCTION() void HandleContinue();
	UFUNCTION() void HandleHowToPlay();
	UFUNCTION() void HandleSettings();
	UFUNCTION() void HandleCloseHowToPlay();
	UFUNCTION() void HandleQuit();
	UFUNCTION() void HandleConfirmNewGame();
	UFUNCTION() void HandleCancelNewGame();
	UFUNCTION() void HandleModeSelected(FString Item,ESelectInfo::Type Selection);
	UFUNCTION() void HandleRegionSelected(FString Item,ESelectInfo::Type Selection);
	UFUNCTION() void HandleFocusSelected(FString Item,ESelectInfo::Type Selection);
	UPROPERTY() TObjectPtr<UBorder> NewGameCard;
	UPROPERTY() TObjectPtr<UComboBoxString> ModeChoice;
	UPROPERTY() TObjectPtr<UComboBoxString> RegionChoice;
	UPROPERTY() TObjectPtr<UComboBoxString> FocusChoice;
	UPROPERTY() TObjectPtr<UComboBoxString> DifficultyChoice;
	UPROPERTY() TObjectPtr<UEditableTextBox> SeedEntry;
	UPROPERTY() TObjectPtr<UCheckBox> UnlimitedBudget;
	UPROPERTY() TObjectPtr<UTextBlock> SessionStatus;
	UPROPERTY() TObjectPtr<UTextBlock> ModeDescription;
	UPROPERTY() TObjectPtr<UTextBlock> RegionDescription;
	UPROPERTY() TArray<TObjectPtr<UButton>> ModeCards;
	UPROPERTY() TObjectPtr<UButton> ConfirmNewGame;
	UPROPERTY() TObjectPtr<UButton> CancelNewGame;
	UPROPERTY() TObjectPtr<UTexture2D> OpeningArtwork;
	UPROPERTY() TObjectPtr<UButton> NewGameButton;
	UPROPERTY() TObjectPtr<UButton> ContinueButton;
	UPROPERTY() TObjectPtr<UButton> HowToPlayButton;
	UPROPERTY() TObjectPtr<UButton> SettingsButton;
	UPROPERTY() TObjectPtr<UButton> QuitButton;
	UPROPERTY() TObjectPtr<UButton> CloseHelpButton;
	UPROPERTY() TObjectPtr<UBorder> HelpCard;
};
