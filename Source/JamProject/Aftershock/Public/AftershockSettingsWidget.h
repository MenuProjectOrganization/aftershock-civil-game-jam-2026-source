#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/ComboBoxString.h"
#include "Components/InputKeySelector.h"
#include "GameFramework/GameUserSettings.h"
#include "Aftershock/Public/AftershockSettingsSubsystem.h"
#include "AftershockSettingsWidget.generated.h"

class UAftershockSettingsWidget;
class UTextBlock;
class UVerticalBox;
class UBorder;
class USlider;
class UCheckBox;

UCLASS()
class JAMPROJECT_API UAftershockSettingsButton final : public UButton
{
	GENERATED_BODY()
public:
	void Configure(UAftershockSettingsWidget* InOwner,int32 InAction);
private:
	UFUNCTION() void Invoke();
	UPROPERTY() TObjectPtr<UAftershockSettingsWidget> SettingsOwner;
	int32 Action=0;
};

UCLASS()
class JAMPROJECT_API UAftershockSettingsChoice final : public UComboBoxString
{
	GENERATED_BODY()
public:
	UAftershockSettingsChoice(const FObjectInitializer& Initializer);
	void Configure(UAftershockSettingsWidget* InOwner,int32 InChoiceID);
private:
	UFUNCTION() void Changed(FString Option,ESelectInfo::Type SelectionType);
	UPROPERTY() TObjectPtr<UAftershockSettingsWidget> QualityOwner;
	int32 QualityChoiceID=INDEX_NONE;
};

UCLASS()
class JAMPROJECT_API UAftershockSettingsKeySelector final : public UInputKeySelector
{
	GENERATED_BODY()
public:
	void Configure(UAftershockSettingsWidget* InOwner,int32 InAction);
private:
	UFUNCTION() void Selected(FInputChord Chord);
	UPROPERTY() TObjectPtr<UAftershockSettingsWidget> SettingsOwner;
	int32 Action=0;
};

/** Native settings; world remains paused, display confirmation uses the real clock. */
UCLASS()
class JAMPROJECT_API UAftershockSettingsWidget final : public UUserWidget
{
	GENERATED_BODY()
public:
	UAftershockSettingsWidget(const FObjectInitializer& Initializer);
	void OpenSettings();
	void HandleAction(int32 Action);
	void HandleGraphicsChoiceChanged(int32 ChoiceID);
	void SelectKey(int32 Action,const FInputChord& Chord);
	void UpdateRealTimeConfirmation();
	void CancelAndClose();
	bool IsAwaitingDisplayConfirmation() const { return bDisplayConfirmation; }
protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnPreviewKeyDown(const FGeometry& Geometry,const FKeyEvent& Event) override;
private:
	friend class UAftershockSettingsAuditCommandlet;
	void BuildLayout();
	void LoadControls();
	void RefreshGraphicsPresetChoice();
	void ShowPage(int32 Index);
	void ApplyChanges();
	void KeepDisplayChanges();
	void RevertDisplayChanges();
	void CaptureSnapshot();
	void PersistAppliedSettings();
	void SetFeedback(const FText& Message,bool bError=false);
	void ApplyMenuTextPreference();
	UTextBlock* MakeText(const FText& Value,int32 Size=14);
	UAftershockSettingsButton* MakeButton(const FText& Label,int32 Action);
	UAftershockSettingsChoice* AddChoice(UVerticalBox* Page,int32 ID,const FText& Label,const TArray<FString>& Options,const FText& Hint);
	USlider* AddSlider(UVerticalBox* Page,int32 ID,const FText& Label,float Min,float Max,const FText& Hint);
	UCheckBox* AddToggle(UVerticalBox* Page,int32 ID,const FText& Label,const FText& Hint);
	UAftershockSettingsSubsystem* PreferencesService() const;
	UPROPERTY() TArray<TObjectPtr<UVerticalBox>> Pages;
	UPROPERTY() TArray<TObjectPtr<UAftershockSettingsButton>> Tabs;
	UPROPERTY() TMap<int32,TObjectPtr<UAftershockSettingsChoice>> Choices;
	UPROPERTY() TMap<int32,TObjectPtr<USlider>> Sliders;
	UPROPERTY() TMap<int32,TObjectPtr<UTextBlock>> SliderValues;
	UPROPERTY() TMap<int32,TObjectPtr<UCheckBox>> Toggles;
	UPROPERTY() TArray<TObjectPtr<UAftershockSettingsKeySelector>> KeySelectors;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> MenuTexts;
	TArray<int32> BaseTextSizes;
	UPROPERTY() TObjectPtr<UTextBlock> Feedback;
	UPROPERTY() TObjectPtr<UTextBlock> ConfirmationText;
	UPROPERTY() TObjectPtr<UBorder> ConfirmationCard;
	UPROPERTY() TObjectPtr<UBorder> SettingsCard;
	UPROPERTY() TObjectPtr<UAftershockSettingsButton> ApplyButton;
	UPROPERTY() TObjectPtr<UAftershockSettingsButton> KeepButton;
	UPROPERTY() TObjectPtr<UAftershockSettingsButton> CloseButton;
	TArray<FIntPoint> Resolutions;
	TArray<FIntPoint> FullscreenResolutions;
	FAftershockUserPreferences StagedPreferences;
	FAftershockUserPreferences PreviousPreferences;
	Scalability::FQualityLevels PreviousQuality;
	FIntPoint PreviousResolution=FIntPoint::ZeroValue;
	EWindowMode::Type PreviousWindowMode=EWindowMode::Windowed;
	float PreviousFrameLimit=60;
	bool bPreviousVSync=false;
	bool bDisplayConfirmation=false;
	bool bLoadingControls=false;
	double DisplayConfirmDeadline=0;
	int32 CurrentPage=0;
};
