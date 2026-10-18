// AFTERSHOCK — Civil Game Jam 2026
// İsometric kamera + mouse click to place/select bina.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Aftershock/Types/AftershockTypes.h"
#include "Aftershock/Public/AftershockSessionTypes.h"
#include "AftershockPlayerController.generated.h"

class AGridManager;
class UBuildingData;
class ABuildingActor;
class UAftershockMainHUDWidget;
class UAftershockOpeningWidget;
class UAftershockSettingsWidget;
class UAftershockNewGameTutorialWidget;
class UWaterGlassEpilogueWidget;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnBuildingSelected, ABuildingActor*, Building);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPlacementModeChanged, bool, bEnabled);

UCLASS(BlueprintType)
class JAMPROJECT_API AAftershockPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AAftershockPlayerController();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Build")
	TSubclassOf<ABuildingActor> DefaultBuildingClass;

	UPROPERTY(BlueprintReadWrite, Category="Aftershock|Build")
	TObjectPtr<UBuildingData> SelectedBuildingData;

	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnBuildingSelected OnBuildingSelected;

	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnPlacementModeChanged OnPlacementModeChanged;

	// -------- HUD VE ARAYÜZ YÖNETİMİ --------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|UI")
	TSubclassOf<UAftershockMainHUDWidget> MainHUDWidgetClass;

	UPROPERTY(BlueprintReadOnly, Category="Aftershock|UI")
	TObjectPtr<UAftershockMainHUDWidget> MainHUDWidget;

	UFUNCTION(BlueprintPure, Category="Aftershock|UI")
	UAftershockMainHUDWidget* GetMainHUD() const { return MainHUDWidget; }
	UFUNCTION(BlueprintCallable, Category="Aftershock|Opening") void ShowOpeningMenu();
	void ProposeGeographicNewGame(int32 RegionIndex,int32 FocusIndex);
	UFUNCTION(BlueprintCallable, Category="Aftershock|Opening") void StartNewGameFromOpening();
	UFUNCTION(BlueprintCallable, Category="Aftershock|Opening") void ContinueFromOpening();
	bool StartConfiguredGame(const FAftershockSessionSettings& Settings,FText& Error);
	/** Pure first-run policy; versioned explicit unlimited (0) remains unlimited. */
	static float ResolveStartupFrameRateLimit(float ExistingLimit,bool bExistingVSync,int32 PreferenceVersion);
	bool CanPersistFrameRatePreference() const;
	bool SetUserFrameRateLimit(int32 FramesPerSecond);
	bool HasAppliedEphemeralAuditFramePacing() const { return bEphemeralAuditFramePacingApplied; }
	bool IsOpeningMenuVisible() const { return bOpeningMenuVisible; }
	UFUNCTION(BlueprintCallable, Category="Aftershock|Settings") void ShowSettingsMenu();
	void CloseSettingsMenu();
	bool IsSettingsMenuVisible() const;
	void ApplyUserPreferences();
	bool IsNarrativeFlowActive() const;
	bool IsNewGameTutorialVisible() const;
	bool IsWaterGlassEpilogueVisible() const;
	UWaterGlassEpilogueWidget* GetWaterGlassEpilogue() const { return WaterGlassEpilogue; }
	UAftershockNewGameTutorialWidget* GetNewGameTutorial() const { return NewGameTutorial; }
	void PresentNewGameTutorial();
	void CompleteNewGameTutorial();
	void PresentWaterGlassEpilogue(bool bCampaignSuccess,FText Reason);
	void PresentLoadFailureRecovery();

	// -------- INPUT HANDLERS --------
	UFUNCTION(BlueprintCallable, Category="Aftershock|Input")
	void HandleLeftClick();

	UFUNCTION(BlueprintCallable, Category="Aftershock|Input")
	void HandleRightClick();

	UFUNCTION(BlueprintCallable, Category="Aftershock|Input")
	void HandleEscapePressed();

	UFUNCTION(BlueprintCallable, Category="Aftershock|Input")
	void HandleRotateGhost();

	/** UI'den bina seçildi — placement moduna gir. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Build")
	void EnterPlacementMode(UBuildingData* BuildingData);

	UFUNCTION(BlueprintCallable, Category="Aftershock|Build")
	void ExitPlacementMode();

	UFUNCTION(BlueprintPure, Category="Aftershock|Build")
	bool IsInPlacementMode() const { return SelectedBuildingData != nullptr; }

	/** Uses the same terrain hit test as the native placement preview when the camera is present. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Build")
	bool GetMouseGridCoord(FIntPoint& OutCoord) const;

	/** Click/key input'ları Blueprint tarafı çağırır (bu temel sağlar). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Build")
	ABuildingActor* TryPlaceAtMouse();

	UFUNCTION(BlueprintCallable, Category="Aftershock|Build")
	ABuildingActor* TrySelectAtMouse();

	UFUNCTION(BlueprintPure, Category="Aftershock|Refs")
	AGridManager* GetGridManager() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;

	UPROPERTY()
	mutable TObjectPtr<AGridManager> CachedGridManager;
	UPROPERTY(Transient) TObjectPtr<UAftershockOpeningWidget> OpeningMenu;
	UPROPERTY(Transient) TObjectPtr<UAftershockSettingsWidget> SettingsMenu;
	UPROPERTY(Transient) TObjectPtr<UAftershockNewGameTutorialWidget> NewGameTutorial;
	UPROPERTY(Transient) TObjectPtr<UWaterGlassEpilogueWidget> WaterGlassEpilogue;
	FGuid TutorialPresentedSessionID; // Transient presentation guard; never save or change the session.
	bool bOpeningMenuVisible = false;
	bool bSettingsOpenedFromOpening = false;
	void CloseOpeningMenu();
	void ContinueAfterStartupPreparation();
	void InitializeFramePacing();
	bool bEphemeralAuditFramePacingApplied=false;
};
