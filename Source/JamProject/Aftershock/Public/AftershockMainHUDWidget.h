// AFTERSHOCK — Civil Game Jam 2026
// UMG Şehir Yöneticisi Arayüzü (HUD Dashboard)

#pragma once

#include "CoreMinimal.h"
#include "Components/Button.h"
#include "Blueprint/UserWidget.h"
#include "Aftershock/Public/BuildingData.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Types/AftershockTypes.h"
#include "AftershockMainHUDWidget.generated.h"

class UTextBlock;
class UProgressBar;
class UButton;
class UWidget;
class UBorder;
class UImage;
class USizeBox;
class AAftershockCameraPawn;
class UHorizontalBox;
class UAftershockMainHUDWidget;
class UCasualtyLedgerPanel;

/** Small native delegate adapter for data-driven catalog and tax controls. */
UCLASS()
class JAMPROJECT_API UAftershockCatalogActionButton final : public UButton
{
	GENERATED_BODY()
public:
	void Configure(UAftershockMainHUDWidget* HUD, int32 InAction, int32 InValue);
	int32 Action = 0;
	int32 Value = 0;
private:
	UFUNCTION() void HandleCatalogClick();
	UPROPERTY(Transient) TWeakObjectPtr<UAftershockMainHUDWidget> OwnerHUD;
};

/**
 * UAftershockMainHUDWidget — Cities: Skylines & Frostpunk tarzı şehir yöneticisi arayüzü.
 * - Üst şerit: Hazine, düzenli gelir/gider, memnuniyet, seviye/tur, afet durumu ve quiz butonu.
 * - İnce eylem paneli: 9 bina tipi yatırımı, 3 altyapı önlemi, afet test butonları.
 * - Afet yaklaşırken: Belirgin uyarı paneli ve kalan hazırlık süresi sayacı.
 * - Sonuç ekranı: Kurtarılan/kaybedilen insan, hasar, devlet afet ödeneği, bütçe/memnuniyet değişimi.
 * - Quiz modal: Kriz Yönetim Merkezi bilgi yarışması (tema bonusu).
 * - Duraklatma ve Seviye geçişi menüleri.
 * SIFIR TICK kuralına uygundur; sayaçlar FTimerHandle ile yönetilir.
 */
UCLASS(BlueprintType, Blueprintable)
class JAMPROJECT_API UAftershockMainHUDWidget : public UUserWidget
{
	GENERATED_BODY()
	friend class UAftershockHUDInputAuditCommandlet;

public:
	void FocusPauseMenu();
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnPreviewKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	/** Single native input route shared by the camera and Blueprint controller wrappers. */
	bool BlocksWorldInputAtCursor() const;
	bool HasModalInputBlock() const { return HasBlockingModal(); }
	bool CanIssueCityOrder() const;
	bool HandleWorldPrimaryAction();
	void SetOpeningMenuActive(bool bActive);
	void SetNarrativeFlowActive(bool bActive);
	void OpenCasualtyLedger();
	bool IsLegacyEndingPanelVisible() const;
	void HandleEscapeAction();
	void CancelMapTool();
	void HandleCatalogAction(int32 Action, int32 Value);
	/** Read-only Development console diagnostics for real pointer/UMG troubleshooting. */
	void DumpInputAuditState() const;
	/** Feedback for a world order that cannot be completed. */
	void ShowWorldOrderFeedback(const FText& Message) { ShowActionNotice(Message); }

	// =========================================================================
	// 1. ÜST ŞERİT BİLEŞENLERİ (TOP STATUS BAR)
	// =========================================================================
	/** Hazine bakiyesi metni (Örn: "52.400 TL") */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|TopBar")
	TObjectPtr<UTextBlock> TextTreasury;

	/** Düzenli tur gelir ve gideri (Örn: "+450 TL / -120 TL (Net: +330 TL)") */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|TopBar")
	TObjectPtr<UTextBlock> TextIncomeExpense;

	/** Halk memnuniyeti yüzdesi metni (Örn: "%84") */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|TopBar")
	TObjectPtr<UTextBlock> TextSatisfaction;

	/** Memnuniyet ilerleme çubuğu (%25 altına inince kırmızı uyarı rengine döner) */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|TopBar")
	TObjectPtr<UProgressBar> BarSatisfaction;

	/** Mevcut tur ve simülasyon yılı (Örn: "Tur: 14 | Yıl: 2027") */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|TopBar")
	TObjectPtr<UTextBlock> TextTurnYear;

	/** CO2 ve Kentsel Isı / İklim risk çarpanı metni */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|TopBar")
	TObjectPtr<UTextBlock> TextClimateRisk;

	/** Mevcut afet zorluk seviyesi metni */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|TopBar")
	TObjectPtr<UTextBlock> TextDisasterLevel;

	/** Sonraki tura geçme butonu */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|TopBar")
	TObjectPtr<UButton> BtnNextTurn;

	/** Oyunu duraklatma / menü açma butonu */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|TopBar")
	TObjectPtr<UButton> BtnPause;

	/** Kriz Yönetim Merkezi quiz modalını açma butonu */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|TopBar")
	TObjectPtr<UButton> BtnOpenQuiz;

	// =========================================================================
	// 2. İNCE EYLEM PANELİ — BİNA VE ALTYAPI YATIRIMLARI
	// =========================================================================
	// -------- 9 BİNA YATIRIM BUTONLARI --------
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Buildings")
	TObjectPtr<UButton> BtnBuildHouse;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Buildings")
	TObjectPtr<UButton> BtnBuildApartment;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Buildings")
	TObjectPtr<UButton> BtnBuildOffice;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Buildings")
	TObjectPtr<UButton> BtnBuildFactory;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Buildings")
	TObjectPtr<UButton> BtnBuildHospital;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Buildings")
	TObjectPtr<UButton> BtnBuildSchool;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Buildings")
	TObjectPtr<UButton> BtnBuildFireStation;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Buildings")
	TObjectPtr<UButton> BtnBuildPowerPlant;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Buildings")
	TObjectPtr<UButton> BtnBuildGreenSpace;

	// -------- 3 ALTYAPI ÖNLEM BUTONU --------
	/** Nehir yatağını genişletme (2.500 TL) */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Infrastructure")
	TObjectPtr<UButton> BtnInvestRiver;

	/** Su akışını yavaşlatıcı bent inşa etme (2.000 TL) */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Infrastructure")
	TObjectPtr<UButton> BtnInvestDamping;

	/** Sokak drenaj hatlarını temizleme/geliştirme (1.000 TL) */
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Infrastructure")
	TObjectPtr<UButton> BtnInvestDrainage;

	// -------- TEST/SİMÜLASYON AFET BUTONLARI --------
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Disasters")
	TObjectPtr<UButton> BtnTriggerEarthquake;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Disasters")
	TObjectPtr<UButton> BtnTriggerFlood;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Disasters")
	TObjectPtr<UButton> BtnTriggerWind;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Disasters")
	TObjectPtr<UButton> BtnTriggerTsunami;

	// =========================================================================
	// 3. AFET YAKLAŞIRKEN UYARI VE GERİ SAYIM PANELİ
	// =========================================================================
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Warning")
	TObjectPtr<UWidget> PanelDisasterWarning;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Warning")
	TObjectPtr<UTextBlock> TextDisasterWarning;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Warning")
	TObjectPtr<UTextBlock> TextDisasterCountdown;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Warning")
	TObjectPtr<UProgressBar> BarDisasterCountdown;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Warning")
	TObjectPtr<UButton> BtnSkipPreparation;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Warning")
	TObjectPtr<UButton> BtnFocusDisaster;

	// =========================================================================
	// 4. AFET SONUÇ VE BİLANÇO EKRANI (RESULTS SCREEN)
	// =========================================================================
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Results")
	TObjectPtr<UWidget> PanelDisasterResults;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Results")
	TObjectPtr<UTextBlock> TextResultDisasterTitle;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Results")
	TObjectPtr<UTextBlock> TextResultCasualties;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Results")
	TObjectPtr<UTextBlock> TextResultDamage;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Results")
	TObjectPtr<UTextBlock> TextResultPrevented;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Results")
	TObjectPtr<UTextBlock> TextResultAid;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Results")
	TObjectPtr<UTextBlock> TextResultBudgetChange;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Results")
	TObjectPtr<UTextBlock> TextResultSatisfactionDelta;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Results")
	TObjectPtr<UButton> BtnCloseResults;

	// =========================================================================
	// 5. KRİZ YÖNETİM MERKEZİ QUIZ MODALİ
	// =========================================================================
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Quiz")
	TObjectPtr<UWidget> PanelQuizModal;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Quiz")
	TObjectPtr<UTextBlock> TextQuizQuestion;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Quiz")
	TObjectPtr<UButton> BtnQuizChoice0;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Quiz")
	TObjectPtr<UButton> BtnQuizChoice1;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Quiz")
	TObjectPtr<UButton> BtnQuizChoice2;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Quiz")
	TObjectPtr<UButton> BtnQuizChoice3;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Quiz")
	TObjectPtr<UTextBlock> TextQuizChoice0;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Quiz")
	TObjectPtr<UTextBlock> TextQuizChoice1;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Quiz")
	TObjectPtr<UTextBlock> TextQuizChoice2;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Quiz")
	TObjectPtr<UTextBlock> TextQuizChoice3;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Quiz")
	TObjectPtr<UTextBlock> TextQuizFeedback;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Quiz")
	TObjectPtr<UButton> BtnCloseQuiz;

	// =========================================================================
	// 6. DURAKLATMA (PAUSE) MENÜSÜ
	// =========================================================================
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Pause")
	TObjectPtr<UWidget> PanelPauseMenu;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Pause")
	TObjectPtr<UButton> BtnResume;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Pause")
	TObjectPtr<UButton> BtnRestartLevel;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Pause")
	TObjectPtr<UButton> BtnQuitGame;

	// =========================================================================
	// 7. SEVİYE GEÇİŞİ / ZAFER PANELİ
	// =========================================================================
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Transition")
	TObjectPtr<UWidget> PanelLevelTransition;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Transition")
	TObjectPtr<UTextBlock> TextLevelTransitionTitle;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Transition")
	TObjectPtr<UButton> BtnNextLevel;

	// =========================================================================
	// 8. OYUN BİTİŞİ (GAME OVER) PANELİ
	// =========================================================================
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|GameOver")
	TObjectPtr<UWidget> PanelGameOver;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|GameOver")
	TObjectPtr<UTextBlock> TextGameOverReason;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|GameOver")
	TObjectPtr<UButton> BtnRestartGameOver;

	// =========================================================================
	// 9. REHBER VE İNŞAAT BİLGİ KARTLARI (ONBOARDING & HINTS)
	// =========================================================================
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Hints")
	TObjectPtr<UBorder> GoalCardBorder;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Hints")
	TObjectPtr<UTextBlock> TextGoalDescription;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Hints")
	TObjectPtr<UBorder> BuildHintBorder;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category="Aftershock|UI|Hints")
	TObjectPtr<UTextBlock> TextBuildHint;

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI")
	void UpdateBuildHintTick();

	FTimerHandle BuildHintTimerHandle;

	// =========================================================================
	// BİNA KATALOĞU (DataAsset Eşleşmeleri)
	// =========================================================================
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Aftershock|UI|Catalog")
	TMap<EBuildingType, TObjectPtr<UBuildingData>> BuildingCatalog;

	// =========================================================================
	// PUBLIC BLUEPRINTCALLABLE FONKSİYONLAR
	// =========================================================================
	/** Programatik olarak temiz ve şık TopBar ve BottomActionDock arayüzünü oluşturur */
	UFUNCTION(BlueprintCallable, Category="Aftershock|UI")
	void SetupCleanHUDLayout();

	/** Bütçe (TL), memnuniyet, tur/yıl, afet seviyesi ve CO2 metriklerini günceller */
	UFUNCTION(BlueprintCallable, Category="Aftershock|UI")
	void UpdateMetricsUI(int32 Budget, int32 StabilityPct, int32 Turn, int32 Year, int32 DisasterLevel, int32 CO2, int32 RiskMult);

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Turn")
	void AdvanceTurnFromUI();

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Disaster")
	void StartDisasterWarningCountdown(EDisasterType DisasterType, float WarningSeconds = 15.0f);

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Disaster")
	void TriggerDisasterFromUI(EDisasterType DisasterType);

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Results")
	void ShowDisasterResults(const FDisasterReport& Report);

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Results")
	void CloseDisasterResults();

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Quiz")
	void OpenQuizModal();

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Quiz")
	void SubmitQuizChoice(int32 ChoiceIndex);

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Quiz")
	void CloseQuizModal();

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Pause")
	void TogglePauseMenu();

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Pause")
	void ResumeGame();

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Pause")
	void RestartCurrentLevel();

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Pause")
	void QuitGame();

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Build")
	void SetBuildModeFromUI(EBuildingType BuildingType, EFoundationQuality Foundation = EFoundationQuality::Medium, int32 Floors = 1);

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Build")
	void SetBuildModeWithData(UBuildingData* BuildingData, EFoundationQuality Foundation = EFoundationQuality::Medium, int32 Floors = 1);

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Build")
	void CancelBuildModeFromUI();

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Build")
	bool ConfirmPlacementFromUI(int32& OutAssignedID);

	// -------- ALTYAPI YATIRIMLARI --------
	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Infrastructure")
	bool OnInvestRiverClicked();

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Infrastructure")
	bool OnInvestWaterDampingClicked();

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Infrastructure")
	bool OnInvestDrainageClicked();

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Infrastructure")
	bool InvestRiverAtCoords(FIntPoint Coords);

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Infrastructure")
	bool InvestWaterDampingAtCoords(FIntPoint Coords);

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI|Infrastructure")
	bool InvestStreetDrainageAtCoords(FIntPoint Coords, int32 Radius = 0);

	UFUNCTION(BlueprintCallable, Category="Aftershock|UI")
	void RefreshAllUI();

	UFUNCTION(BlueprintPure, Category="Aftershock|UI")
	AAftershockCameraPawn* GetCameraPawn() const;

	UFUNCTION(BlueprintPure, Category="Aftershock|UI")
	bool GetHoveredGridCoords(FIntPoint& OutCoords) const;

protected:
	// -------- ALT SİSTEM DELEGATE HANDLER'LARI --------
	UFUNCTION()
	void HandleEconomyUpdated(int64 TreasuryTL, float Satisfaction, int32 Turn);

	UFUNCTION()
	void HandleClimateUpdated(float CO2Level, float ClimateRiskMultiplier);

	UFUNCTION()
	void HandleScenarioUpdated();

	UFUNCTION()
	void HandleGameOver(FText ReasonMessage);

	UFUNCTION()
	void HandleDisasterCompleted(const FDisasterReport& Report);

	UFUNCTION()
	void HandleQuizRequested(const FQuizQuestion& Question);

	UFUNCTION()
	void HandleQuizAnswered(bool bCorrect, const FQuizQuestion& Question);

	// -------- BUTON CLICK HANDLER'LARI --------
	UFUNCTION() void HandleNextTurnClicked();
	UFUNCTION() void HandlePauseClicked();
	UFUNCTION() void HandleOpenQuizClicked();
	UFUNCTION() void HandleCloseQuizClicked();
	UFUNCTION() void HandleQuizChoice0Clicked();
	UFUNCTION() void HandleQuizChoice1Clicked();
	UFUNCTION() void HandleQuizChoice2Clicked();
	UFUNCTION() void HandleQuizChoice3Clicked();
	UFUNCTION() void HandleSkipPreparationClicked();
	UFUNCTION() void HandleFocusDisasterClicked();
	UFUNCTION() void HandleCloseResultsClicked();
	UFUNCTION() void HandleResumeClicked();
	UFUNCTION() void HandleRestartLevelClicked();
	UFUNCTION() void HandleQuitGameClicked();
	UFUNCTION() void HandleSaveGameClicked();
	UFUNCTION() void ShowSaveFeedback(bool bSuccess,FText Message);
	void RefreshDirectorUI();
	UPROPERTY() TObjectPtr<UButton> BtnSaveGame;
	UPROPERTY() TObjectPtr<UTextBlock> TextSaveStatus;
	UFUNCTION() void HandleNextLevelClicked();
	UFUNCTION() void HandleMainMenuClicked();
	UFUNCTION() void HandleSettingsClicked();

	// -------- 9 BİNA BUTON CLICK HANDLER'LARI --------
	UFUNCTION() void HandleBuildHouseClicked();
	UFUNCTION() void HandleBuildApartmentClicked();
	UFUNCTION() void HandleBuildOfficeClicked();
	UFUNCTION() void HandleBuildFactoryClicked();
	UFUNCTION() void HandleBuildHospitalClicked();
	UFUNCTION() void HandleBuildSchoolClicked();
	UFUNCTION() void HandleBuildFireStationClicked();
	UFUNCTION() void HandleBuildPowerPlantClicked();
	UFUNCTION() void HandleBuildGreenSpaceClicked();

	// -------- ALTYAPI & AFET BUTON HANDLER'LARI --------
	UFUNCTION() void HandleInvestRiverClicked();
	UFUNCTION() void HandleInvestDampingClicked();
	UFUNCTION() void HandleInvestDrainageClicked();
	UFUNCTION() void HandleTriggerEarthquakeClicked();
	UFUNCTION() void HandleTriggerFloodClicked();
	UFUNCTION() void HandleTriggerWindClicked();
	UFUNCTION() void HandleTriggerTsunamiClicked();
	UFUNCTION() void HandleInvestDamClicked();
	UFUNCTION() void HandleRetrofitClicked();
	UFUNCTION() void HandleRepairClicked();
	UFUNCTION() void HandleClearRubbleClicked();
	UFUNCTION() void HandleInspectorRetrofitClicked();
	UFUNCTION() void HandleInspectorRepairClicked();
	UFUNCTION() void HandleInspectorRubbleClicked();
	UFUNCTION() void HandleCloseInspectorClicked();
	UFUNCTION() void HandleBuildRoadClicked();
	UFUNCTION() void HandleRemoveRoadClicked();

	/** HUD butonlarının tümüne OnPressed listener olarak eklenir — Kenney UIClick ses çalar. */
	UFUNCTION() void HandleAnyButtonPressed();

	// -------- İÇ YARDIMCI VE SAYAÇ FONKSİYONLARI --------
	void UpdateWarningCountdownTick();
	UBuildingData* FindBuildingDataByType(EBuildingType Type) const;
	void InitializeFallbackBuildingData();
	bool HasBlockingModal() const;
	void SelectMapTool(int32 Tool);
	void ShowActionNotice(const FText& Message, bool bSuccess = false);
	void BindHUDButtons();
	void RefreshActionAvailability();
	void RefreshActionPresentation();
	void RefreshBuildingInspector();
	void ApplyInspectorAction(int32 Action);
	void UpdateRoadToolPreview();
	void RevealPendingDisasterResults();
	void RefreshScenarioGoalUI();
	void RefreshGeographicDisplay();
	void RefreshFramePacingUI();
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TextFramePacing;
	UPROPERTY(Transient) TArray<TObjectPtr<UAftershockCatalogActionButton>> FramePacingButtons;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TextGeographicLocation;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TextGeodataAttribution;
	UPROPERTY(Transient) TObjectPtr<UBorder> GeodataAttributionPlate;
	FString LastGeographicDisplay;
	void RefreshCatalogPage();
	void RefreshCityLifeUI();
	UFUNCTION() void HandleOpenLedger();
	UFUNCTION() void HandleCloseLedger();
	UPROPERTY(Transient) TObjectPtr<UButton> BtnOpenLedger;
	UPROPERTY(Transient) TObjectPtr<UWidget> PanelCasualtyLedger;
	UPROPERTY(Transient) TObjectPtr<UCasualtyLedgerPanel> CasualtyLedger;
	UFUNCTION() void HandleCityLifeUpdated();
	UFUNCTION() void HandleTriggerFireClicked();
	UFUNCTION() void HandleTriggerVolcanoClicked();
	UFUNCTION() void HandleTriggerDroughtClicked();
	UFUNCTION() void HandleTriggerAvalancheClicked();
	UFUNCTION() void HandleTriggerLandslideClicked();
	UFUNCTION() void HandleGroundSurveyClicked();
	UPROPERTY(Transient) TObjectPtr<UHorizontalBox> CatalogItemsRow;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TextCatalogPage;
	UPROPERTY(Transient) TArray<TObjectPtr<UAftershockCatalogActionButton>> CatalogItemButtons;
	UPROPERTY(Transient) TArray<TObjectPtr<UAftershockCatalogActionButton>> CatalogCategoryButtons;
	UPROPERTY(Transient) TObjectPtr<UAftershockCatalogActionButton> CatalogPreviousButton;
	UPROPERTY(Transient) TObjectPtr<UAftershockCatalogActionButton> CatalogNextButton;
	UPROPERTY(Transient) TArray<TObjectPtr<UAftershockCatalogActionButton>> CatalogItemButtonsPool;
	UPROPERTY(Transient) TArray<TObjectPtr<UImage>> CatalogItemImages;
	UPROPERTY(Transient) TArray<TObjectPtr<USizeBox>> CatalogItemIconSizes;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> CatalogItemTitleLabels;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> CatalogItemCostLabels;
	UPROPERTY(Transient) TArray<TObjectPtr<UAftershockCatalogActionButton>> TaxActionButtons;
	UPROPERTY(Transient) TObjectPtr<UBorder> CityLifeCard;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TextCityLife;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TextTaxRates;
	UPROPERTY(Transient) TObjectPtr<UButton> BtnTriggerFire;
	UPROPERTY(Transient) TObjectPtr<UButton> BtnTriggerVolcano;
	UPROPERTY(Transient) TObjectPtr<UButton> BtnTriggerDrought;
	UPROPERTY(Transient) TObjectPtr<UButton> BtnTriggerAvalanche;
	UPROPERTY(Transient) TObjectPtr<UButton> BtnTriggerLandslide;
	UPROPERTY(Transient) TObjectPtr<UButton> BtnGroundSurvey;
	int32 CatalogCategory = 0;
	int32 CatalogPage = 0;
	int32 PresentedMapTool = MIN_int32;
	int32 PresentedBuildingType = MIN_int32;
	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> TextScenarioProgress;
	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> TextScenarioClimate;
	float NextScenarioGoalRefreshTime = 0.f;

	UPROPERTY(Transient)
	TObjectPtr<UButton> BtnRetrofit;
	UPROPERTY(Transient)
	TObjectPtr<UButton> BtnRepair;
	UPROPERTY(Transient)
	TObjectPtr<UButton> BtnClearRubble;
	UPROPERTY(Transient)
	TObjectPtr<UBorder> BuildingInspector;
	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> TextBuildingDetails;
	UPROPERTY(Transient)
	TObjectPtr<UButton> BtnInspectorRetrofit;
	UPROPERTY(Transient)
	TObjectPtr<UButton> BtnInspectorRepair;
	UPROPERTY(Transient)
	TObjectPtr<UButton> BtnInspectorRubble;
	UPROPERTY(Transient)
	TObjectPtr<UButton> BtnCloseInspector;
	UPROPERTY(Transient)
	TObjectPtr<UButton> BtnBuildRoad;
	UPROPERTY(Transient)
	TObjectPtr<UButton> BtnRemoveRoad;
	UPROPERTY(Transient) TObjectPtr<UButton> BtnMainMenu;
	UPROPERTY(Transient) TObjectPtr<UButton> BtnSettings;
	UPROPERTY(Transient) TObjectPtr<UButton> BtnInvestDam;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UWidget>> WorldInputRegions;

	int32 ActiveMapTool = 0;
	int32 InspectedBuildingID = INDEX_NONE;
	bool bRoadStartSelected = false;
	FIntPoint RoadStart = FIntPoint(MAX_int32, MAX_int32);
	FAftershockRoadQuote CurrentRoadQuote;
	FDisasterReport PendingVisualDisasterReport;
	int32 PendingVisualRevision = INDEX_NONE;
	FTimerHandle ResultsRevealTimer;
	int32 LastDisasterTriggerTurn = 0;
	int32 LastQuizTurn = INDEX_NONE;
	bool bAwaitingQuizAnswer = false;
	bool bDisasterInFlight = false;
	FText ActionNotice;
	float ActionNoticeUntil = 0.0f;
	bool bActionNoticeSuccess = false;

	UPROPERTY()
	mutable TMap<EBuildingType, TObjectPtr<UBuildingData>> FallbackBuildingCatalog;

	FTimerHandle WarningTimerHandle;
	float WarningRemainingSeconds = 0.0f;
	float WarningTotalDuration = 15.0f;
	EDisasterType PendingDisasterType = EDisasterType::Earthquake;
	bool bIsWarningActive = false;
	bool bIsGamePaused = false;
	bool bOpeningMenuActive = false;
	bool bNarrativeFlowActive = false;
	double TurnConfirmationDeadline = 0.;
};
