// AFTERSHOCK — Civil Game Jam 2026
// UMG Şehir Yöneticisi Arayüzü Uygulaması

#include "Aftershock/Public/AftershockMainHUDWidget.h"
#include "Aftershock/Public/AftershockDisasterDirectorSubsystem.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "AftershockPhysicalLifecycle.h"

#include "Aftershock/Public/AftershockBuildingCatalog.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Public/AftershockAudioSubsystem.h"
#include "Aftershock/Public/AftershockSettingsSubsystem.h"
#include "HAL/PlatformTime.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/WorldClimateSubsystem.h"
#include "Aftershock/Game/AftershockPlayerController.h"
#include "Aftershock/Quiz/QuizSubsystem.h"
#include "Engine/GameInstance.h"
#include "Components/TextBlock.h"
#include "Components/ProgressBar.h"
#include "Components/Button.h"
#include "Components/Widget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Image.h"
#include "Components/SizeBox.h"
#include "Components/ScaleBox.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/ScrollBox.h"
#include "Framework/Application/SlateApplication.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/SlateTypes.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/GameUserSettings.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "HAL/IConsoleManager.h"
#include "Aftershock/Game/AftershockGameMode.h"
#include "Aftershock/UI/CasualtyLedgerPanel.h"
#include "Aftershock/Narrative/CasualtyLedgerGenerator.h"

namespace
{
	FButtonStyle MakeAftershockButtonStyle(const FButtonStyle& Existing,const FLinearColor& Base,bool bSelected=false)
	{
		FButtonStyle Style=Existing;
		const FLinearColor Accent(.96f,.62f,.25f), Line(.11f,.19f,.22f);
		Style.SetNormal(FSlateRoundedBoxBrush(bSelected?FLinearColor(.17f,.10f,.04f):Base,4.f,bSelected?Accent:Line,bSelected?2.f:1.f));
		Style.SetHovered(FSlateRoundedBoxBrush(FLinearColor(Base.R+.035f,Base.G+.055f,Base.B+.055f,1.f),4.f,Accent,1.f));
		Style.SetPressed(FSlateRoundedBoxBrush(FLinearColor(.025f,.053f,.065f),4.f,Accent,1.f));
		Style.SetDisabled(FSlateRoundedBoxBrush(FLinearColor(.022f,.035f,.044f),4.f,FLinearColor(.065f,.10f,.12f),1.f));
		Style.SetNormalForeground(FSlateColor(FLinearColor(.94f,.94f,.89f)));
		Style.SetHoveredForeground(FSlateColor(FLinearColor(.99f,.81f,.53f)));
		Style.SetPressedForeground(FSlateColor(FLinearColor(.94f,.94f,.89f)));
		Style.SetDisabledForeground(FSlateColor(FLinearColor(.42f,.52f,.56f)));
		Style.SetNormalPadding(FMargin(11,8)); Style.SetPressedPadding(FMargin(11,9,11,7));
		return Style;
	}

	bool AllowsManualDisasterControl(const UGameInstance* Instance)
	{
		const auto* Session=Instance?Instance->GetSubsystem<UAftershockOpeningSessionSubsystem>():nullptr;
		return !Session||!Session->HasConfiguredSession()||Session->GetSettings().Mode!=EAftershockPlayMode::GeographicSimulation;
	}
	FText AutomaticGeographyNotice()
	{return NSLOCTEXT("Aftershock","GeographicAutomaticHazards","Coğrafi simülasyonda afetleri bölgenin risk modeli otomatik seçer. Başlamış bir hazırlık uyarısını hızlandırabilirsiniz.");}
	float RemainingPresentationSeconds(const UWorld* World,const FAftershockDisasterVisualSnapshot* Snapshot)
	{
		if(!World||!Snapshot||Snapshot->Revision<=0)return 0.f;
		const float Duration=Snapshot->GetEffectiveDurationSeconds();
		return AftershockPhysicalLifecycle::RemainingVisualSeconds(World->GetTimeSeconds(),Snapshot->StartedAtWorldSeconds,Duration);
	}
}

#if !UE_BUILD_SHIPPING
namespace
{
	FAutoConsoleCommandWithWorld HUDInputStateCommand(
		TEXT("Aftershock.HUDState"), TEXT("Read-only native HUD locks, tax button bindings and screen rectangles."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			const AAftershockPlayerController* Controller = World ? Cast<AAftershockPlayerController>(World->GetFirstPlayerController()) : nullptr;
			if (const UAftershockMainHUDWidget* HUD = Controller ? Controller->GetMainHUD() : nullptr) HUD->DumpInputAuditState();
		}));
}
#endif

void UAftershockMainHUDWidget::DumpInputAuditState() const
{
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_HUD_INPUT_STATE canOrder=%d modal=%d warning=%d inFlight=%d opening=%d paused=%d taxButtons=%d"), CanIssueCityOrder(), HasBlockingModal(), bIsWarningActive, bDisasterInFlight, bOpeningMenuActive, bIsGamePaused, TaxActionButtons.Num());
	for (const UAftershockCatalogActionButton* Button : TaxActionButtons)
	{
		if (!Button) continue;
		const FGeometry& Geometry = Button->GetCachedGeometry();
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_HUD_TAX_WIDGET action=%d value=%d enabled=%d visible=%d bound=%d screenMin=%s screenSize=%s"), Button->Action, Button->Value, Button->GetIsEnabled(), Button->IsVisible(), Button->OnClicked.IsBound(), *Geometry.GetAbsolutePosition().ToString(), *Geometry.GetAbsoluteSize().ToString());
	}
}

void UAftershockCatalogActionButton::Configure(UAftershockMainHUDWidget* HUD, int32 InAction, int32 InValue)
{
	OwnerHUD = HUD; Action = InAction; Value = InValue;
	OnClicked.AddUniqueDynamic(this, &UAftershockCatalogActionButton::HandleCatalogClick);
}

void UAftershockCatalogActionButton::HandleCatalogClick()
{
	// Pooled controls retain one delegate for their whole lifetime. A retired
	// slot can never dispatch the previous page's action through a stale click.
	if (Action<0||!OwnerHUD.IsValid()) return;
	OwnerHUD->HandleCatalogAction(Action, Value);
}

void UAftershockMainHUDWidget::HandleCatalogAction(int32 Action, int32 Value)
{
	HandleAnyButtonPressed();
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_HUD_CATALOG_ACTION action=%d value=%d canOrder=%d modal=%d disasterInFlight=%d"), Action, Value, CanIssueCityOrder(), HasBlockingModal(), bDisasterInFlight);
	if (Action == 0) { CatalogCategory = FMath::Clamp(Value, 0, 4); CatalogPage = 0; RefreshCatalogPage(); return; }
	if (Action == 2) { CatalogPage = FMath::Max(0, CatalogPage + Value); RefreshCatalogPage(); return; }
	if(Action==5)
	{
		if(auto* PC=Cast<AAftershockPlayerController>(GetOwningPlayer()))PC->SetUserFrameRateLimit(Value);
		RefreshFramePacingUI();return;
	}
	if (!CanIssueCityOrder()) return;
	if (Action == 1)
	{
		if (UBuildingData* Data = FindBuildingDataByType(static_cast<EBuildingType>(Value))) SetBuildModeWithData(Data, EFoundationQuality::Medium, Data->FloorsMin);
	}
	if (Action == 3 || Action == 4)
	{
		if (UCityLifeSubsystem* Life = GetWorld() ? GetWorld()->GetSubsystem<UCityLifeSubsystem>() : nullptr)
		{
			const FCityLifeState& State = Life->GetState();
			const float Change = Value * .02f;
			const bool Changed = Life->SetTaxRates(State.ResidentialTaxRate + (Action == 3 ? Change : 0), State.BusinessTaxRate + (Action == 4 ? Change : 0));
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_HUD_TAX_ACTION accepted=%d residential=%.4f business=%.4f"), Changed, Life->GetState().ResidentialTaxRate, Life->GetState().BusinessTaxRate);
			ShowActionNotice(Changed ? NSLOCTEXT("Aftershock", "TaxChanged", "Vergi oranları güncellendi. Gelir ve vatandaş memnuniyeti üzerindeki sonucu şehir göstergelerinden izleyin.") : NSLOCTEXT("Aftershock", "TaxUnchanged", "Vergi oranı değiştirilemedi; izin verilen aralık %0–30."), Changed);
			RefreshAllUI();
		}
	}
}

void UAftershockMainHUDWidget::RefreshCatalogPage()
{
	if (!CatalogItemsRow || !WidgetTree) return;
	TArray<EBuildingType> Types, Filtered; FAftershockBuildingCatalog::GetTypes(Types);
	for (EBuildingType Type : Types) if (FAftershockBuildingCatalog::GetCategory(Type) == CatalogCategory) Filtered.Add(Type);
	const int32 Pages = FMath::Max(1, FMath::DivideAndRoundUp(Filtered.Num(), 6));
	CatalogPage = FMath::Clamp(CatalogPage, 0, Pages - 1);
	if (TextCatalogPage) TextCatalogPage->SetText(FText::FromString(FString::Printf(TEXT("%d/%d"), CatalogPage + 1, Pages)));
	if (CatalogPreviousButton) CatalogPreviousButton->SetIsEnabled(CatalogPage>0);
	if (CatalogNextButton) CatalogNextButton->SetIsEnabled(CatalogPage+1<Pages);
	for (UAftershockCatalogActionButton* CategoryButton:CatalogCategoryButtons)
		if(CategoryButton)CategoryButton->SetStyle(MakeAftershockButtonStyle(CategoryButton->GetStyle(),FLinearColor(.026f,.054f,.068f),CategoryButton->Value==CatalogCategory));
	CatalogItemButtons.Reset();
	if (CatalogItemButtonsPool.IsEmpty())
	{
		for (int32 CatalogSlotIndex = 0; CatalogSlotIndex < 6; ++CatalogSlotIndex)
		{
			UAftershockCatalogActionButton* Button = WidgetTree->ConstructWidget<UAftershockCatalogActionButton>();
			Button->Configure(this,INDEX_NONE,INDEX_NONE);
			Button->SetIsEnabled(false);Button->SetVisibility(ESlateVisibility::Collapsed);
			FButtonStyle Style = MakeAftershockButtonStyle(Button->GetStyle(), FLinearColor(.026f,.054f,.068f));
			Style.SetNormalPadding(FMargin(8,8));
			Button->SetStyle(Style);
			UVerticalBox* Tile = WidgetTree->ConstructWidget<UVerticalBox>();
			Button->SetContent(Tile);

			UImage* Image = WidgetTree->ConstructWidget<UImage>();
			Image->SetVisibility(ESlateVisibility::HitTestInvisible);
			USizeBox* IconSize = WidgetTree->ConstructWidget<USizeBox>();
			IconSize->SetWidthOverride(30);
			IconSize->SetHeightOverride(30);
			IconSize->SetContent(Image);
			UVerticalBoxSlot* IconLayout = Tile->AddChildToVerticalBox(IconSize);
			IconLayout->SetHorizontalAlignment(HAlign_Center);

			UTextBlock* TitleLabel = WidgetTree->ConstructWidget<UTextBlock>();
			TitleLabel->SetJustification(ETextJustify::Center);
			TitleLabel->SetColorAndOpacity(FLinearColor(.94f,.94f,.89f));
			FSlateFontInfo TitleFont = TitleLabel->GetFont(); TitleFont.Size = 11;
			TitleLabel->SetFont(TitleFont);
			TitleLabel->SetAutoWrapText(true); TitleLabel->SetWrapTextAt(152.f);
			TitleLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
			Tile->AddChildToVerticalBox(TitleLabel);

			UTextBlock* CostLabel = WidgetTree->ConstructWidget<UTextBlock>();
			CostLabel->SetJustification(ETextJustify::Center);
			CostLabel->SetColorAndOpacity(FLinearColor(.96f,.62f,.25f));
			FSlateFontInfo CostFont = CostLabel->GetFont(); CostFont.Size = 10;
			CostLabel->SetFont(CostFont);
			CostLabel->SetAutoWrapText(true); CostLabel->SetWrapTextAt(152.f);
			CostLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
			Tile->AddChildToVerticalBox(CostLabel);

			UHorizontalBoxSlot* Layout = CatalogItemsRow->AddChildToHorizontalBox(Button);
			Layout->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			Layout->SetPadding(FMargin(4,2));

			CatalogItemButtonsPool.Add(Button);
			CatalogItemImages.Add(Image);
			CatalogItemIconSizes.Add(IconSize);
			CatalogItemTitleLabels.Add(TitleLabel);
			CatalogItemCostLabels.Add(CostLabel);
		}
	}

	const UBuildingManagerSubsystem* Buildings = GetWorld() ? GetWorld()->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
	const int32 PageStart = CatalogPage * 6;
	const int32 PageEnd = FMath::Min(Filtered.Num(), (CatalogPage + 1) * 6);
	const int32 PageItemCount = FMath::Max(0, PageEnd - PageStart);

	for (int32 CatalogSlotIndex = 0; CatalogSlotIndex < 6; ++CatalogSlotIndex)
	{
		UAftershockCatalogActionButton* Button = CatalogItemButtonsPool[CatalogSlotIndex];
		Button->Configure(this,INDEX_NONE,INDEX_NONE);
		Button->SetIsEnabled(false);Button->SetVisibility(ESlateVisibility::Collapsed);
		Button->SetToolTipText(FText::GetEmpty());
		// NativeConstruct can rebuild the row while retaining the six pooled
		// UObjects. Reattach those same controls instead of leaving them in the
		// retired widget subtree or allocating another pool.
		if (Button->GetParent() != CatalogItemsRow)
		{
			Button->RemoveFromParent();
			UHorizontalBoxSlot* Layout = CatalogItemsRow->AddChildToHorizontalBox(Button);
			Layout->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			Layout->SetPadding(FMargin(4,2));
		}
		if (CatalogSlotIndex < PageItemCount)
		{
			const int32 ItemIndex = PageStart + CatalogSlotIndex;
			UBuildingData* Data = FindBuildingDataByType(Filtered[ItemIndex]);
			if (Data)
			{
				UImage* Image = CatalogItemImages[CatalogSlotIndex];
				USizeBox* IconSize = CatalogItemIconSizes[CatalogSlotIndex];
				if (UTexture2D* Icon = Data->IconThumbnail.LoadSynchronous())
				{
					Image->SetBrushFromTexture(Icon, true);
					IconSize->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
				}
				else
				{
					IconSize->SetVisibility(ESlateVisibility::Collapsed);
				}

				const int32 Cost = Buildings ? Buildings->CalculateConstructionCost(Data, EFoundationQuality::Medium, Data->FloorsMin) : 0;
				CatalogItemTitleLabels[CatalogSlotIndex]->SetText(Data->DisplayName);
				CatalogItemCostLabels[CatalogSlotIndex]->SetText(FText::Format(NSLOCTEXT("Aftershock","CatalogCost","{0} TL"), FText::AsNumber(Cost)));
				Button->Configure(this, 1, static_cast<int32>(Filtered[ItemIndex]));

				FString Tooltip = FString::Printf(TEXT("%s\nOrta temel: %d TL | Kat: %d–%d\nİş kapasitesi: %d | Bakım: %d TL/tur\nEnerji: +%.0f / -%.0f oyun birimi\nGıda üretimi: %.0f / işleme girdisi: %.0f\n%s"), *Data->DisplayName.ToString(),Cost,Data->FloorsMin,Data->FloorsMax,Data->Workers,Data->MaintenancePerTurn,Data->PowerProductionUnitsPerTurn,Data->PowerDemandUnitsPerTurn,Data->FoodProductionUnitsPerTurn,Data->FoodProcessingInputUnitsPerTurn,*Data->Description.ToString());
				if (Data->Type == EBuildingType::BusTerminal || Data->Type == EBuildingType::BusStop) Tooltip += TEXT("\nUlaşım için yola bağlı terminal ve en az iki durak gerekir; bağlantı kesilirse hizmet durur.");
				Button->SetToolTipText(FText::FromString(Tooltip));

				Button->SetVisibility(ESlateVisibility::Visible);
				CatalogItemButtons.Add(Button);
			}
			else
			{
				Button->SetVisibility(ESlateVisibility::Collapsed);
			}
		}
		else
		{
			Button->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
	PresentedBuildingType=MIN_int32;
	RefreshActionAvailability();
}

void UAftershockMainHUDWidget::HandleCityLifeUpdated() { RefreshCityLifeUI(); RefreshBuildingInspector(); }

void UAftershockMainHUDWidget::RefreshCityLifeUI()
{
	if (CityLifeCard) CityLifeCard->SetVisibility(BuildingInspector && BuildingInspector->IsVisible() ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
	const UCityLifeSubsystem* Life = GetWorld() ? GetWorld()->GetSubsystem<UCityLifeSubsystem>() : nullptr;
	if (!Life || !Life->IsCityInitialized()) { if (TextCityLife) TextCityLife->SetText(NSLOCTEXT("Aftershock","LifePreparing","Şehir hizmetleri hazırlanıyor.")); return; }
	if (TextCityLife) TextCityLife->SetText(Life->DescribeCity());
	if (TextTaxRates) TextTaxRates->SetText(FText::FromString(FString::Printf(TEXT("Vergi  Konut: %%%.0f  |  İş: %%%.0f"),Life->GetState().ResidentialTaxRate*100,Life->GetState().BusinessTaxRate*100)));
}

void UAftershockMainHUDWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetIsFocusable(true);
	InitializeFallbackBuildingData();
	SetupCleanHUDLayout();
	BindHUDButtons();
	if(auto* D=GetWorld()?GetWorld()->GetSubsystem<UAftershockDisasterDirectorSubsystem>():nullptr) D->OnDirectorChanged.AddUObject(this,&UAftershockMainHUDWidget::RefreshDirectorUI);
	if(auto* GI=GetGameInstance())GI->GetSubsystem<UAftershockSaveSubsystem>()->OnSaveCompleted.AddUniqueDynamic(this,&UAftershockMainHUDWidget::ShowSaveFeedback);

	UWorld* World = GetWorld();
	if (World)
	{
		if (UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>())
		{
			Economy->OnEconomyUpdated.AddUniqueDynamic(this, &UAftershockMainHUDWidget::HandleEconomyUpdated);
			Economy->OnClimateUpdated.AddUniqueDynamic(this, &UAftershockMainHUDWidget::HandleClimateUpdated);
			Economy->OnGameOver.AddUniqueDynamic(this, &UAftershockMainHUDWidget::HandleGameOver);
		}
		if (UCityLifeSubsystem* Life = World->GetSubsystem<UCityLifeSubsystem>()) Life->OnCityLifeUpdated.AddUniqueDynamic(this, &UAftershockMainHUDWidget::HandleCityLifeUpdated);
		if (UDisasterSimulationSubsystem* Disaster = World->GetSubsystem<UDisasterSimulationSubsystem>())
		{
			Disaster->OnDisasterCompleted.AddUniqueDynamic(this, &UAftershockMainHUDWidget::HandleDisasterCompleted);
		}
		if (UAftershockScenarioSubsystem* Scenario = World->GetSubsystem<UAftershockScenarioSubsystem>())
		{
			Scenario->OnScenarioUpdated.AddUniqueDynamic(this, &UAftershockMainHUDWidget::HandleScenarioUpdated);
		}
		if (UGameInstance* GI = World->GetGameInstance())
		{
			if (UQuizSubsystem* Quiz = GI->GetSubsystem<UQuizSubsystem>())
			{
				Quiz->OnQuizRequested.AddUniqueDynamic(this, &UAftershockMainHUDWidget::HandleQuizRequested);
				Quiz->OnQuizAnswered.AddUniqueDynamic(this, &UAftershockMainHUDWidget::HandleQuizAnswered);
				if(Quiz->HasPendingQuestion())HandleQuizRequested(Quiz->GetCurrentQuestion());
			}
		}
		World->GetTimerManager().SetTimer(BuildHintTimerHandle, this, &UAftershockMainHUDWidget::UpdateBuildHintTick, 0.1f, true);
	}
	if (APlayerController* PC = GetOwningPlayer())
	{
		FInputModeGameAndUI InputMode;
		InputMode.SetHideCursorDuringCapture(false);
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PC->SetInputMode(InputMode);
		PC->bShowMouseCursor = true;
	}
	RefreshAllUI();RefreshDirectorUI();
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_HUD_READY catalogDefinitions=24 catalogCategories=5 catalogPageSize=6 roads=2 disasters=7 cityLife=1 soilSurvey=1 input=GameAndUI openingModalGuard=1"));
}

void UAftershockMainHUDWidget::BindHUDButtons()
{
#define BIND_HUD(Button, Handler) if (Button) { Button->OnClicked.AddUniqueDynamic(this, &UAftershockMainHUDWidget::Handler); Button->OnPressed.AddUniqueDynamic(this, &UAftershockMainHUDWidget::HandleAnyButtonPressed); }
	BIND_HUD(BtnNextTurn, HandleNextTurnClicked);
	BIND_HUD(BtnPause, HandlePauseClicked);
	BIND_HUD(BtnOpenQuiz, HandleOpenQuizClicked);
	BIND_HUD(BtnCloseQuiz, HandleCloseQuizClicked);
	BIND_HUD(BtnBuildHouse, HandleBuildHouseClicked);
	BIND_HUD(BtnBuildApartment, HandleBuildApartmentClicked);
	BIND_HUD(BtnBuildOffice, HandleBuildOfficeClicked);
	BIND_HUD(BtnBuildFactory, HandleBuildFactoryClicked);
	BIND_HUD(BtnBuildHospital, HandleBuildHospitalClicked);
	BIND_HUD(BtnBuildSchool, HandleBuildSchoolClicked);
	BIND_HUD(BtnBuildFireStation, HandleBuildFireStationClicked);
	BIND_HUD(BtnBuildPowerPlant, HandleBuildPowerPlantClicked);
	BIND_HUD(BtnBuildGreenSpace, HandleBuildGreenSpaceClicked);
	BIND_HUD(BtnInvestRiver, HandleInvestRiverClicked);
	BIND_HUD(BtnInvestDamping, HandleInvestDampingClicked);
	BIND_HUD(BtnInvestDrainage, HandleInvestDrainageClicked);
	BIND_HUD(BtnRetrofit, HandleRetrofitClicked);
	BIND_HUD(BtnRepair, HandleRepairClicked);
	BIND_HUD(BtnClearRubble, HandleClearRubbleClicked);
	BIND_HUD(BtnBuildRoad, HandleBuildRoadClicked);
	BIND_HUD(BtnRemoveRoad, HandleRemoveRoadClicked);
	BIND_HUD(BtnInspectorRetrofit, HandleInspectorRetrofitClicked);
	BIND_HUD(BtnInspectorRepair, HandleInspectorRepairClicked);
	BIND_HUD(BtnInspectorRubble, HandleInspectorRubbleClicked);
	BIND_HUD(BtnCloseInspector, HandleCloseInspectorClicked);
	BIND_HUD(BtnTriggerEarthquake, HandleTriggerEarthquakeClicked);
	BIND_HUD(BtnTriggerFlood, HandleTriggerFloodClicked);
	BIND_HUD(BtnTriggerWind, HandleTriggerWindClicked);
	BIND_HUD(BtnTriggerTsunami, HandleTriggerTsunamiClicked);
	BIND_HUD(BtnInvestDam, HandleInvestDamClicked);
	BIND_HUD(BtnTriggerFire, HandleTriggerFireClicked);
	BIND_HUD(BtnTriggerVolcano, HandleTriggerVolcanoClicked);
	BIND_HUD(BtnTriggerDrought, HandleTriggerDroughtClicked);
	BIND_HUD(BtnTriggerAvalanche, HandleTriggerAvalancheClicked);
	BIND_HUD(BtnTriggerLandslide, HandleTriggerLandslideClicked);
	BIND_HUD(BtnGroundSurvey, HandleGroundSurveyClicked);
	BIND_HUD(BtnSkipPreparation, HandleSkipPreparationClicked);
	BIND_HUD(BtnFocusDisaster, HandleFocusDisasterClicked);
	BIND_HUD(BtnCloseResults, HandleCloseResultsClicked);
	BIND_HUD(BtnQuizChoice0, HandleQuizChoice0Clicked);
	BIND_HUD(BtnQuizChoice1, HandleQuizChoice1Clicked);
	BIND_HUD(BtnQuizChoice2, HandleQuizChoice2Clicked);
	BIND_HUD(BtnQuizChoice3, HandleQuizChoice3Clicked);
	BIND_HUD(BtnResume, HandleResumeClicked);
	BIND_HUD(BtnRestartLevel, HandleRestartLevelClicked);
	BIND_HUD(BtnQuitGame, HandleQuitGameClicked);
	BIND_HUD(BtnSaveGame, HandleSaveGameClicked);
	BIND_HUD(BtnRestartGameOver, HandleRestartLevelClicked);
	BIND_HUD(BtnNextLevel, HandleNextLevelClicked);
	BIND_HUD(BtnMainMenu, HandleMainMenuClicked);
	BIND_HUD(BtnSettings, HandleSettingsClicked);
	BIND_HUD(BtnOpenLedger, HandleOpenLedger);
#undef BIND_HUD
}

bool UAftershockMainHUDWidget::HasBlockingModal() const
{
	auto IsOpen = [](const UWidget* Widget) { return Widget && Widget->GetVisibility() != ESlateVisibility::Collapsed && Widget->GetVisibility() != ESlateVisibility::Hidden; };
	return bNarrativeFlowActive || bOpeningMenuActive || bIsGamePaused || IsOpen(PanelPauseMenu) || IsOpen(PanelQuizModal) || IsOpen(PanelDisasterResults) || IsOpen(PanelGameOver) || IsOpen(PanelLevelTransition) || IsOpen(PanelCasualtyLedger);
}

bool UAftershockMainHUDWidget::CanIssueCityOrder() const
{
	const UWorld* World = GetWorld();
	const UEconomyAndStateSubsystem* Economy = World ? World->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	const auto* GI=GetGameInstance();const auto* Saves=GI?GI->GetSubsystem<UAftershockSaveSubsystem>():nullptr;
	const auto* Simulation=World?World->GetSubsystem<UDisasterSimulationSubsystem>():nullptr;
	return Economy && !Economy->bIsGameOver && !HasBlockingModal() && !bDisasterInFlight && (!Simulation||!Simulation->IsPhysicalEventRunning()) && (!Saves||(!Saves->IsRestoring()&&!Saves->IsWorldLoadFailed(World)));
}

void UAftershockMainHUDWidget::SetOpeningMenuActive(bool bActive)
{
	bOpeningMenuActive = bActive;
	if (bActive) { CancelBuildModeFromUI(); CancelMapTool(); }
	SetVisibility((bActive||bNarrativeFlowActive) ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
}

void UAftershockMainHUDWidget::SetNarrativeFlowActive(bool bActive)
{
	bNarrativeFlowActive=bActive;
	if(bActive)
	{
		for(UWidget* Modal:{PanelPauseMenu.Get(),PanelQuizModal.Get(),PanelDisasterResults.Get(),PanelGameOver.Get(),PanelLevelTransition.Get(),PanelCasualtyLedger.Get()})if(Modal)Modal->SetVisibility(ESlateVisibility::Collapsed);
	}
	SetVisibility((bActive||bOpeningMenuActive)?ESlateVisibility::Collapsed:ESlateVisibility::SelfHitTestInvisible);
}
void UAftershockMainHUDWidget::OpenCasualtyLedger()
{
	if(HasBlockingModal()||!PanelCasualtyLedger||!CasualtyLedger)return;
	CancelBuildModeFromUI();CancelMapTool();PanelCasualtyLedger->SetVisibility(ESlateVisibility::Visible);if(CasualtyLedger)CasualtyLedger->RefreshLedger();
}
void UAftershockMainHUDWidget::HandleOpenLedger(){OpenCasualtyLedger();}
void UAftershockMainHUDWidget::HandleCloseLedger(){if(PanelCasualtyLedger)PanelCasualtyLedger->SetVisibility(ESlateVisibility::Collapsed);}
bool UAftershockMainHUDWidget::IsLegacyEndingPanelVisible() const
{return (PanelGameOver&&PanelGameOver->IsVisible())||(PanelLevelTransition&&PanelLevelTransition->IsVisible());}

void UAftershockMainHUDWidget::HandleMainMenuClicked()
{
	if (AAftershockPlayerController* PC = Cast<AAftershockPlayerController>(GetOwningPlayer())) PC->ShowOpeningMenu();
}

void UAftershockMainHUDWidget::HandleSettingsClicked()
{if(auto* PC=Cast<AAftershockPlayerController>(GetOwningPlayer()))PC->ShowSettingsMenu();}

void UAftershockMainHUDWidget::FocusPauseMenu()
{RefreshFramePacingUI();if(BtnResume)BtnResume->SetKeyboardFocus();}

bool UAftershockMainHUDWidget::BlocksWorldInputAtCursor() const
{
	if (HasBlockingModal()) return true;
	if (FSlateApplication::IsInitialized())
	{
		const FVector2D PointerPosition = FSlateApplication::Get().GetCursorPos();
		for (const UWidget* Region : WorldInputRegions)
		{
			if (Region && Region->IsVisible() && Region->GetCachedGeometry().IsUnderLocation(PointerPosition)) return true;
		}
	}
	return false;
}

FReply UAftershockMainHUDWidget::NativeOnPreviewKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		HandleEscapeAction();
		return FReply::Handled();
	}
	return Super::NativeOnPreviewKeyDown(InGeometry, InKeyEvent);
}

void UAftershockMainHUDWidget::HandleEscapeAction()
{
	if(auto* PC=Cast<AAftershockPlayerController>(GetOwningPlayer());PC&&PC->IsSettingsMenuVisible()){PC->CloseSettingsMenu();return;}
	if(bNarrativeFlowActive)return;
	if(PanelCasualtyLedger&&PanelCasualtyLedger->IsVisible()){HandleCloseLedger();return;}
	if (bIsGamePaused) { ResumeGame(); return; }
	if (PanelQuizModal && PanelQuizModal->IsVisible()) { CloseQuizModal(); return; }
	if (PanelDisasterResults && PanelDisasterResults->IsVisible()) { CloseDisasterResults(); return; }
	if (ActiveMapTool != 0) { CancelMapTool(); return; }
	if (AAftershockCameraPawn* Pawn = GetCameraPawn())
	{
		if (Pawn->IsBuildModeActive()) { Pawn->CancelBuildMode(); return; }
	}
	if (!(PanelGameOver && PanelGameOver->IsVisible())) TogglePauseMenu();
}

void UAftershockMainHUDWidget::ShowActionNotice(const FText& Message, bool bSuccess)
{
	ActionNotice = Message;
	ActionNoticeUntil = GetWorld() ? GetWorld()->GetRealTimeSeconds() + 5.0f : 0.0f;
	bActionNoticeSuccess = bSuccess;
	UpdateBuildHintTick();
}

void UAftershockMainHUDWidget::CancelMapTool()
{
	bRoadStartSelected = false;
	RoadStart = FIntPoint(MAX_int32, MAX_int32);
	CurrentRoadQuote = FAftershockRoadQuote();
	if(AAftershockCameraPawn* Pawn=GetCameraPawn()){Pawn->ClearRoadPreview();Pawn->SetInfrastructureConstructionMode(false);}
	ActiveMapTool = 0;
	ActionNotice = FText::GetEmpty();
	UpdateBuildHintTick();
}

void UAftershockMainHUDWidget::SelectMapTool(int32 Tool)
{
	if (!CanIssueCityOrder()) return;
	if(AAftershockCameraPawn* Pawn=GetCameraPawn()){Pawn->CancelBuildMode();Pawn->ClearRoadPreview();Pawn->SetInfrastructureConstructionMode(Tool!=0);}
	bRoadStartSelected = false;
	RoadStart = FIntPoint(MAX_int32, MAX_int32);
	CurrentRoadQuote = FAftershockRoadQuote();
	ActiveMapTool = Tool;
	ActionNotice = FText::GetEmpty();
	UpdateBuildHintTick();
}

void UAftershockMainHUDWidget::HandleRetrofitClicked() { SelectMapTool(4); }
void UAftershockMainHUDWidget::HandleRepairClicked() { SelectMapTool(5); }
void UAftershockMainHUDWidget::HandleClearRubbleClicked() { SelectMapTool(6); }
void UAftershockMainHUDWidget::HandleBuildRoadClicked() { SelectMapTool(7); }
void UAftershockMainHUDWidget::HandleRemoveRoadClicked() { SelectMapTool(8); }
void UAftershockMainHUDWidget::HandleInvestDamClicked() { SelectMapTool(9); }
void UAftershockMainHUDWidget::HandleGroundSurveyClicked() { SelectMapTool(10); }
void UAftershockMainHUDWidget::HandleTriggerFireClicked() { StartDisasterWarningCountdown(EDisasterType::Fire, 5.f); }
void UAftershockMainHUDWidget::HandleTriggerVolcanoClicked() { StartDisasterWarningCountdown(EDisasterType::Volcano, 5.f); }
void UAftershockMainHUDWidget::HandleTriggerDroughtClicked() { StartDisasterWarningCountdown(EDisasterType::Drought, 5.f); }
void UAftershockMainHUDWidget::HandleTriggerAvalancheClicked() { StartDisasterWarningCountdown(EDisasterType::Avalanche, 5.f); }
void UAftershockMainHUDWidget::HandleTriggerLandslideClicked() { StartDisasterWarningCountdown(EDisasterType::Landslide, 5.f); }

bool UAftershockMainHUDWidget::HandleWorldPrimaryAction()
{
	if (BlocksWorldInputAtCursor() || !CanIssueCityOrder()) return true;
	AAftershockCameraPawn* Pawn = GetCameraPawn();
	if (Pawn && Pawn->IsBuildModeActive())
	{
		int32 OutID = INDEX_NONE;
		if (ConfirmPlacementFromUI(OutID)) ShowActionNotice(NSLOCTEXT("Aftershock", "PlacementDone", "Bina tamamlandı. Yeni bir yapı seçerek şehri geliştirebilirsiniz."), true);
		return true;
	}
	FIntPoint Coords(MAX_int32,MAX_int32);
	const bool bBuildingTool=ActiveMapTool==0||ActiveMapTool==4||ActiveMapTool==5||ActiveMapTool==6;
	const bool bPickedBuilding=bBuildingTool&&Pawn&&Pawn->GetHoveredBuildingGridCoords(Coords);
	if (!bPickedBuilding&&!GetHoveredGridCoords(Coords))
	{
		if (ActiveMapTool) ShowActionNotice(NSLOCTEXT("Aftershock", "SelectMapCell", "Harita üzerinde bir hücre seçin. Sağ tık: iptal."));
		return ActiveMapTool != 0;
	}
	if (ActiveMapTool == 7)
	{
		URoadNetworkSubsystem* Roads = GetWorld()->GetSubsystem<URoadNetworkSubsystem>();
		if (!Roads) return true;
		if (!bRoadStartSelected)
		{
			const FAftershockRoadQuote StartQuote = Roads->QuoteRoad(Coords, Coords);
			if (!StartQuote.bRouteValid) { ShowActionNotice(StartQuote.Feedback); return true; }
			RoadStart = Coords;
			bRoadStartSelected = true;
			ActionNotice = FText::GetEmpty();
			UpdateRoadToolPreview();
			UpdateBuildHintTick();
			return true;
		}
		FAftershockRoadQuote CommittedQuote;
		if (Roads->BuildRoad(RoadStart, Coords, CommittedQuote))
		{
			bRoadStartSelected = false;
			RoadStart = FIntPoint(MAX_int32, MAX_int32);
			if (Pawn) Pawn->ClearRoadPreview();
			RefreshAllUI();
			ShowActionNotice(FText::Format(NSLOCTEXT("Aftershock", "RoadDone", "Yol tamamlandı: {0} yeni hücre, {1} TL. Yeni bir başlangıç seçin."), FText::AsNumber(CommittedQuote.NewRoadCells), FText::AsNumber(CommittedQuote.CostTL)), true);
		}
		else ShowActionNotice(CommittedQuote.Feedback);
		return true;
	}
	if (ActiveMapTool == 8)
	{
		URoadNetworkSubsystem* Roads = GetWorld()->GetSubsystem<URoadNetworkSubsystem>();
		const bool bRemoved = Roads && Roads->RemoveRoadAt(Coords);
		if (Pawn) Pawn->ClearRoadPreview();
		ShowActionNotice(bRemoved ? NSLOCTEXT("Aftershock", "RoadRemoved", "Yol hücresi kaldırıldı. İade yapılmaz. Başka hücre seçin veya sağ tıkla bitirin.") : NSLOCTEXT("Aftershock", "RoadRemoveInvalid", "Bu hücrede kaldırılacak yol yok."), bRemoved);
		return true;
	}
	if (ActiveMapTool != 0)
	{
		bool bSuccess = false;
		UWorld* World = GetWorld();
		UEconomyAndStateSubsystem* Economy = World ? World->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
		UBuildingManagerSubsystem* Buildings = World ? World->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
		FBuildingInstance Building;
		bool bSpecificFeedback = false;
		switch (ActiveMapTool)
		{
		case 1: bSuccess = InvestRiverAtCoords(Coords); break;
		case 2: bSuccess = InvestWaterDampingAtCoords(Coords); break;
		case 3: bSuccess = InvestStreetDrainageAtCoords(Coords, 0); break;
		case 4: bSuccess = Economy && Buildings && Buildings->GetBuildingAtCoords(Coords, Building) && !Building.bIsRetrofitted && Building.DamageState != EDamageState::Collapsed && Economy->RetrofitWithBudget(Building.InstanceID); break;
		case 5: bSuccess = Economy && Buildings && Buildings->GetBuildingAtCoords(Coords, Building) && Economy->RepairWithBudget(Building.InstanceID); break;
		case 6: bSuccess = Economy && Buildings && Buildings->GetBuildingAtCoords(Coords, Building) && Building.DamageState == EDamageState::Collapsed && Economy->ClearRubbleWithBudget(Coords); break;
		case 9:
			if (UDisasterSimulationSubsystem* Disaster = World ? World->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr)
			{
				int32 ActualCost = 0;
				bSuccess = Disaster->InvestInDam(Coords, ActualCost); // Atomic backend purchase already charges.
				if (!bSuccess) { ShowActionNotice(Disaster->QuoteDam(Coords).Feedback); bSpecificFeedback = true; }
			}
			break;
		case 10:
			if (UDisasterSimulationSubsystem* Disaster = World ? World->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr)
			{
				int32 ActualCost = 0;
				bSuccess = Disaster->SurveyGround(Coords, ActualCost); // Backend owns the single500TL payment.
				if (!bSuccess) { ShowActionNotice(NSLOCTEXT("Aftershock","SurveyFailed","Etüt uygulanamadı: boş/yerleşik kara seçin; daha önce etüt edilmiş hücreye tekrar ücret alınmaz. Maliyet500 TL.")); bSpecificFeedback = true; }
			}
			break;
		}
		if (bSuccess)
		{
			ActiveMapTool = 0;
			RefreshAllUI();
			ShowActionNotice(NSLOCTEXT("Aftershock", "InvestmentDone", "Yatırım uygulandı; bedeli hazineden ödendi."), true);
		}
		else if (!bSpecificFeedback) ShowActionNotice(NSLOCTEXT("Aftershock", "InvestmentFailed", "İşlem uygulanamadı. Uygun hücre/bina ve bütçeyi kontrol edin. Sağ tık: iptal."));
		return true;
	}
	UWorld* World = GetWorld();
	UBuildingManagerSubsystem* Buildings = World ? World->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
	FBuildingInstance Selected;
	if (Buildings && Buildings->GetBuildingAtCoords(Coords, Selected))
	{
		InspectedBuildingID = Selected.InstanceID;
		if (BuildingInspector)
		{
			BuildingInspector->SetVisibility(ESlateVisibility::Visible);
			// Both cards share the same canvas position; switch them in this input event.
			if (CityLifeCard) CityLifeCard->SetVisibility(ESlateVisibility::Collapsed);
		}
		RefreshBuildingInspector();
		return true;
	}
	HandleCloseInspectorClicked();
	return false;
}

void UAftershockMainHUDWidget::HandleAnyButtonPressed()
{
	if (UGameInstance* GI = GetGameInstance())
	{
		if (UAftershockAudioSubsystem* Audio = GI->GetSubsystem<UAftershockAudioSubsystem>())
		{
			Audio->PlayUIClick();
		}
	}
}

void UAftershockMainHUDWidget::UpdateBuildHintTick()
{
	RefreshGeographicDisplay();
	if (GetWorld() && GetWorld()->GetRealTimeSeconds() >= NextScenarioGoalRefreshTime)
	{
		RefreshScenarioGoalUI();
		RefreshCityLifeUI();
		NextScenarioGoalRefreshTime = GetWorld()->GetRealTimeSeconds() + 1.f;
	}
	RefreshActionAvailability();
	RefreshBuildingInspector();
	UpdateRoadToolPreview();
	if (!BuildHintBorder || !TextBuildHint) return;
	AAftershockCameraPawn* Pawn = GetCameraPawn();
	FText Hint;
	FLinearColor Color(0.78f, 0.86f, 0.94f);
	if (!ActionNotice.IsEmpty() && GetWorld() && GetWorld()->GetRealTimeSeconds() < ActionNoticeUntil)
	{
		Hint = ActionNotice;
		Color = bActionNoticeSuccess ? FLinearColor(0.35f, 0.94f, 0.65f) : FLinearColor(1.0f, 0.65f, 0.3f);
	}
	else if (Pawn && Pawn->IsBuildModeActive())
	{
		Hint = Pawn->GetCurrentPlacementFeedback();
		Color = Pawn->IsPlacementValid() ? FLinearColor(0.35f, 0.94f, 0.65f) : FLinearColor(1.0f, 0.45f, 0.38f);
	}
	else if (bDisasterInFlight)
	{
		const UDisasterSimulationSubsystem* Disaster = GetWorld() ? GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
		const FAftershockDisasterVisualSnapshot* Snapshot = Disaster ? &Disaster->GetLatestVisualSnapshot() : nullptr;
		const int32 Seconds = FMath::CeilToInt(RemainingPresentationSeconds(GetWorld(),Snapshot));
		Hint = Disaster&&Disaster->IsPhysicalEventRunning()?NSLOCTEXT("Aftershock","ActivePhysicalDisasterHint","FİZİKSEL AFET SÜRÜYOR — Su ve malzeme hareketi hesaplanıyor.\nKamera açık; bilanço kanonik olay tamamlandığında gösterilecek."):FText::Format(NSLOCTEXT("Aftershock", "ActiveDisasterHint", "AFET SÜRÜYOR — Bilanço {0} sn sonra açılacak.\nKamera kontrolü açık; yeni şehir yatırımları afet sonrasında devam eder."), FText::AsNumber(Seconds));
		Color = FLinearColor(1.0f, 0.76f, 0.32f);
	}
	else if (ActiveMapTool == 7)
	{
		Hint = bRoadStartSelected ? CurrentRoadQuote.Feedback : NSLOCTEXT("Aftershock", "RoadSelectStart", "Yol inşası — Başlangıç için boş kara/yol hücresine sol tık. 200 TL / yeni hücre.\nİkinci tık: güzergâhı onayla. Sağ tık / Esc: iptal.");
		const UEconomyAndStateSubsystem* Economy = GetWorld() ? GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
		if (bRoadStartSelected && CurrentRoadQuote.bRouteValid && Economy && !Economy->CanAfford(CurrentRoadQuote.CostTL))
		{
			Hint = FText::Format(NSLOCTEXT("Aftershock", "RoadPreviewNoBudget", "Yol: {0} TL — Bütçe yetersiz; hiçbir hücre değiştirilmedi. Sağ tık: iptal."), FText::AsNumber(CurrentRoadQuote.CostTL));
			Color = FLinearColor(1.0f, 0.45f, 0.38f);
		}
	}
	else if (ActiveMapTool == 8)
	{
		Hint = NSLOCTEXT("Aftershock", "RoadRemoveHint", "Yol kaldır — Var olan bir yol hücresine sol tık. Söküm ücretsizdir; inşaat bedeli iade edilmez.\nSağ tık / Esc: iptal.");
	}
	else if (ActiveMapTool == 9)
	{
		Hint = NSLOCTEXT("Aftershock", "DamToolHint", "Baraj — Nehir hücresi seçin. 12.000 TL; sonlu depolama kapasitesi.\nYalnız nehir taşkınını azaltır. Sağ tık / Esc: iptal.");
		FIntPoint Hovered;
		if (GetHoveredGridCoords(Hovered))
			if (const UDisasterSimulationSubsystem* Disaster = GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>()) Hint = Disaster->QuoteDam(Hovered).Feedback;
	}
	else if (ActiveMapTool == 10)
	{
		FIntPoint Hovered;
		const UTerrainGridSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
		Hint = NSLOCTEXT("Aftershock","SurveyHint","Zemin etüdü: kara hücresine sol tık — 500 TL. Sağ tık / Esc: iptal.");
		if (Terrain && GetHoveredGridCoords(Hovered) && Terrain->GetCell(Hovered).bGroundSurveyed)
			Hint = FText::FromString(FString::Printf(TEXT("Etüt tamam: %s — senaryo zemin sınıfı, gerçek parsel araştırması değildir. Tekrar ücret alınmaz."),Terrain->GetCell(Hovered).SoilType == EAftershockSoilType::Bedrock ? TEXT("Ana kaya") : TEXT("Alüvyon")));
	}
	else if (ActiveMapTool)
	{
		static const TCHAR* ToolNames[] = { TEXT(""), TEXT("Nehir genişlet: nehir hücresine sol tık — 2.500 TL"), TEXT("Akış önlemi: nehir hücresine sol tık — 2.000 TL"), TEXT("Drenaj iyileştir: kara hücresine sol tık — 400 TL / hücre"), TEXT("Güçlendir: hasarsız veya ayakta bir binaya sol tık — 1.000 TL"), TEXT("Onar: hasarlı bir binaya sol tık — 500 / 2.000 / 5.000 TL"), TEXT("Enkaz kaldır: çöken binaya sol tık — 3.000 TL") };
		Hint = FText::FromString(FString(ToolNames[FMath::Clamp(ActiveMapTool, 0, 6)]) + TEXT("\nSağ tık veya Esc: iptal"));
	}
	if (ActiveMapTool == 0 || ActiveMapTool == 9)
	{
		FIntPoint Hovered;
		const UDisasterSimulationSubsystem* Disaster = GetWorld() ? GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
		if (!HasBlockingModal() && Disaster && GetHoveredGridCoords(Hovered))
		{
			const FAftershockDamStatus Status = Disaster->GetDamStatus(Hovered);
			if (Status.bHasDam)
			{
				Hint = FText::Format(NSLOCTEXT("Aftershock", "DamStatusHint", "BARAJ — Doluluk: {0} / {1} | Kalan: {2} oyun birimi\nSon tutulan: {3} | Aşan: {4}. Nehir depolaması; tsunami koruması sağlamaz."),
					FText::AsNumber(FMath::RoundToInt(Status.StoredWaterUnits)), FText::AsNumber(FMath::RoundToInt(Status.StorageCapacityUnits)),
					FText::AsNumber(FMath::RoundToInt(Status.RemainingStorageUnits)), FText::AsNumber(FMath::RoundToInt(Status.LastRetainedUnits)), FText::AsNumber(FMath::RoundToInt(Status.LastOvertoppingUnits)));
				Color = Status.bOvertopped ? FLinearColor(1.f,.6f,.25f) : FLinearColor(.35f,.94f,.65f);
			}
		}
	}
	BuildHintBorder->SetVisibility(!Hint.IsEmpty() && !HasBlockingModal() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	TextBuildHint->SetText(Hint);
	TextBuildHint->SetColorAndOpacity(FSlateColor(Color));
}

void UAftershockMainHUDWidget::UpdateRoadToolPreview()
{
	if (ActiveMapTool != 7 && ActiveMapTool != 8) return;
	AAftershockCameraPawn* Pawn = GetCameraPawn();
	const URoadNetworkSubsystem* Roads = GetWorld() ? GetWorld()->GetSubsystem<URoadNetworkSubsystem>() : nullptr;
	const UEconomyAndStateSubsystem* Economy = GetWorld() ? GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	if (!Pawn || !Roads) return;
	FIntPoint Hovered;
	if (HasBlockingModal() || BlocksWorldInputAtCursor() || !GetHoveredGridCoords(Hovered))
	{
		Pawn->ClearRoadPreview();
		CurrentRoadQuote.Feedback = NSLOCTEXT("Aftershock", "RoadHoverMap", "Yol güzergâhı için yakın şehir görünümünde oynanabilir kara seçin. Sağ tık: iptal.");
		return;
	}
	if (ActiveMapTool == 8)
	{
		Pawn->SetRoadPreview({Hovered}, Roads->HasRoad(Hovered));
		return;
	}
	CurrentRoadQuote = Roads->QuoteRoad(bRoadStartSelected ? RoadStart : Hovered, Hovered);
	const bool bValid = CurrentRoadQuote.bRouteValid && (!bRoadStartSelected || CurrentRoadQuote.NewRoadCells > 0) && Economy && Economy->CanAfford(CurrentRoadQuote.CostTL);
	Pawn->SetRoadPreview(CurrentRoadQuote.bRouteValid ? CurrentRoadQuote.Path : TArray<FIntPoint>{Hovered}, bValid, CurrentRoadQuote.bRouteValid ? CurrentRoadQuote.SurfaceElevationsCm : TArray<float>());
}

void UAftershockMainHUDWidget::RefreshActionAvailability()
{
	const bool bCanOrder = CanIssueCityOrder();
	const bool bManualDisasters=AllowsManualDisasterControl(GetGameInstance());
	UWorld* World = GetWorld();
	const UEconomyAndStateSubsystem* Economy = World ? World->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	const TArray<TPair<UButton*, EBuildingType>> BuildingButtons = {
		{BtnBuildHouse, EBuildingType::House}, {BtnBuildApartment, EBuildingType::Apartment}, {BtnBuildOffice, EBuildingType::Office},
		{BtnBuildFactory, EBuildingType::Factory}, {BtnBuildHospital, EBuildingType::Hospital}, {BtnBuildSchool, EBuildingType::School},
		{BtnBuildFireStation, EBuildingType::FireStation}, {BtnBuildPowerPlant, EBuildingType::PowerPlant}, {BtnBuildGreenSpace, EBuildingType::GreenSpace}
	};
	const UBuildingManagerSubsystem* Buildings = World ? World->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
	for (const auto& Pair : BuildingButtons)
	{
		const UBuildingData* Data = FindBuildingDataByType(Pair.Value);
		const int32 Cost = Buildings && Data ? Buildings->CalculateConstructionCost(Data, EFoundationQuality::Medium, Data->FloorsMin) : MAX_int32;
		if (Pair.Key) Pair.Key->SetIsEnabled(bCanOrder && Data && (Economy && Economy->CanAfford(Cost)));
	}
	for (UAftershockCatalogActionButton* Button : CatalogItemButtons)
	{
		const UBuildingData* Data = Button ? FindBuildingDataByType(static_cast<EBuildingType>(Button->Value)) : nullptr;
		const int32 Cost = Buildings && Data ? Buildings->CalculateConstructionCost(Data,EFoundationQuality::Medium,Data->FloorsMin) : MAX_int32;
		if (Button) Button->SetIsEnabled(bCanOrder && Data && (Economy && Economy->CanAfford(Cost)));
	}
	const UCityLifeSubsystem* Life = World ? World->GetSubsystem<UCityLifeSubsystem>() : nullptr;
	for (UAftershockCatalogActionButton* Button : TaxActionButtons)
	{
		if (!Button) continue;
		const float Rate = Life ? (Button->Action == 3 ? Life->GetState().ResidentialTaxRate : Life->GetState().BusinessTaxRate) : 0.f;
		const bool bWithinLimit = Button->Value > 0 ? Rate < .30f - KINDA_SMALL_NUMBER : Rate > KINDA_SMALL_NUMBER;
		Button->SetIsEnabled(bCanOrder && Life && Life->IsCityInitialized() && bWithinLimit);
	}
	if (BtnGroundSurvey) BtnGroundSurvey->SetIsEnabled(bCanOrder && (Economy && Economy->CanAfford(500)));
	if (BtnBuildRoad) BtnBuildRoad->SetIsEnabled(bCanOrder && (Economy && Economy->CanAfford(200)));
	if (BtnRemoveRoad) BtnRemoveRoad->SetIsEnabled(bCanOrder);
	if (BtnNextTurn) BtnNextTurn->SetIsEnabled(bCanOrder && !bIsWarningActive);
	const UQuizSubsystem* Quiz=GetGameInstance()?GetGameInstance()->GetSubsystem<UQuizSubsystem>():nullptr;
	if (BtnOpenQuiz) BtnOpenQuiz->SetIsEnabled(bCanOrder && !bIsWarningActive && Economy && Quiz && Quiz->CanRequestQuestion(Economy->CurrentTurn));
	if (BtnInvestRiver) BtnInvestRiver->SetIsEnabled(bCanOrder && (Economy && Economy->CanAfford(2500)));
	if (BtnInvestDamping) BtnInvestDamping->SetIsEnabled(bCanOrder && (Economy && Economy->CanAfford(2000)));
	if (BtnInvestDrainage) BtnInvestDrainage->SetIsEnabled(bCanOrder && (Economy && Economy->CanAfford(400)));
	if (BtnRetrofit) BtnRetrofit->SetIsEnabled(bCanOrder && (Economy && Economy->CanAfford(1000)));
	if (BtnRepair) BtnRepair->SetIsEnabled(bCanOrder && (Economy && Economy->CanAfford(500)));
	if (BtnClearRubble) BtnClearRubble->SetIsEnabled(bCanOrder && (Economy && Economy->CanAfford(3000)));
	if (BtnInvestDam) BtnInvestDam->SetIsEnabled(bCanOrder && (Economy && Economy->CanAfford(12000)));
	for (UButton* Button : {BtnTriggerEarthquake.Get(), BtnTriggerFlood.Get(), BtnTriggerWind.Get()})
	{
		if(Button)
		{
			const EDisasterType Type=Button==BtnTriggerEarthquake?EDisasterType::Earthquake:Button==BtnTriggerFlood?EDisasterType::Flood:EDisasterType::Wind;
			FText Reason;const auto* Disaster=World?World->GetSubsystem<UDisasterSimulationSubsystem>():nullptr;
			const float PhysicalWeight=Disaster?Disaster->GetPhysicalHazardWeight(Type):0.f;
			const bool Unlocked=Disaster&&Disaster->CanTriggerDisaster(Type,Reason)&&FMath::IsFinite(PhysicalWeight)&&PhysicalWeight>0.f;
			Button->SetIsEnabled(bCanOrder&&!bIsWarningActive&&bManualDisasters&&Unlocked);
			Button->SetToolTipText(!bManualDisasters?AutomaticGeographyNotice():Unlocked?FText::FromString(TEXT("Bu afet için hazırlık uyarısını başlat.")):Reason);
		}
	}
	if (BtnTriggerTsunami)
	{
		FText Reason;
		const UDisasterSimulationSubsystem* Disaster = World ? World->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
		const bool bUnlocked = Disaster && Disaster->CanTriggerDisaster(EDisasterType::Tsunami, Reason);
		BtnTriggerTsunami->SetIsEnabled(bCanOrder && !bIsWarningActive && bUnlocked&&bManualDisasters);
		BtnTriggerTsunami->SetToolTipText(!bManualDisasters?AutomaticGeographyNotice():bUnlocked ? NSLOCTEXT("Aftershock", "TsunamiAvailable", "Kıyı senaryosunda tsunami hazırlığını başlat. Yüksek kotlu kara toplanma alanlarına erişim sağlayın.") : Reason);
	}
	const TArray<TPair<UButton*,EDisasterType>> GeographicHazards = { {BtnTriggerFire,EDisasterType::Fire},{BtnTriggerVolcano,EDisasterType::Volcano},{BtnTriggerDrought,EDisasterType::Drought},{BtnTriggerAvalanche,EDisasterType::Avalanche},{BtnTriggerLandslide,EDisasterType::Landslide} };
	for (const auto& Entry : GeographicHazards)
	{
		FText Reason;
		const UDisasterSimulationSubsystem* Disaster = World ? World->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
		const float PhysicalWeight=Disaster?Disaster->GetPhysicalHazardWeight(Entry.Value):0.f;
		const bool Unlocked = Disaster && Disaster->CanTriggerDisaster(Entry.Value,Reason)&&FMath::IsFinite(PhysicalWeight)&&PhysicalWeight>0.f;
		if(!Unlocked&&Reason.IsEmpty())Reason=NSLOCTEXT("Aftershock","PhysicalMaterialHazardUnavailable","Mevcut kar, zemin ve eğim koşulları bu afet için uygun değil.");
		if (Entry.Key) { Entry.Key->SetIsEnabled(bCanOrder && !bIsWarningActive && Unlocked&&bManualDisasters); Entry.Key->SetToolTipText(!bManualDisasters?AutomaticGeographyNotice():Unlocked ? NSLOCTEXT("Aftershock","GeographicHazardReady","Bu coğrafyada afet hazırlığını başlat. Senaryo hedeflerini ve yerel riskleri incele.") : Reason); }
	}
	RefreshActionPresentation();
}

void UAftershockMainHUDWidget::RefreshActionPresentation()
{
	const AAftershockCameraPawn* Pawn=GetCameraPawn();
	const UBuildingData* Selected=Pawn&&Pawn->IsBuildModeActive()?Pawn->GetCurrentSelectedBuilding():nullptr;
	const int32 BuildingType=Selected?static_cast<int32>(Selected->Type):INDEX_NONE;
	if(PresentedMapTool==ActiveMapTool&&PresentedBuildingType==BuildingType)return;
	PresentedMapTool=ActiveMapTool; PresentedBuildingType=BuildingType;
	const TArray<TPair<UButton*,int32>> Tools={{BtnInvestRiver,1},{BtnInvestDamping,2},{BtnInvestDrainage,3},{BtnRetrofit,4},{BtnRepair,5},{BtnClearRubble,6},{BtnBuildRoad,7},{BtnRemoveRoad,8},{BtnInvestDam,9},{BtnGroundSurvey,10}};
	for(const auto& Tool:Tools)
		if(Tool.Key)Tool.Key->SetStyle(MakeAftershockButtonStyle(Tool.Key->GetStyle(),FLinearColor(.026f,.065f,.078f),!Selected&&ActiveMapTool==Tool.Value));
	for(UAftershockCatalogActionButton* Button:CatalogItemButtons)
		if(Button)
		{
			FButtonStyle Style=MakeAftershockButtonStyle(Button->GetStyle(),FLinearColor(.026f,.054f,.068f),Button->Value==BuildingType);
			Style.SetNormalPadding(FMargin(8)); Style.SetPressedPadding(FMargin(8,9,8,7)); Button->SetStyle(Style);
		}
}

void UAftershockMainHUDWidget::RefreshBuildingInspector()
{
	if (!BuildingInspector || !BuildingInspector->IsVisible() || !TextBuildingDetails) return;
	UWorld* World = GetWorld();
	const UBuildingManagerSubsystem* Buildings = World ? World->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
	const UEconomyAndStateSubsystem* Economy = World ? World->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	const UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	FBuildingInstance Building;
	if (!Buildings || !Buildings->GetBuildingByID(InspectedBuildingID, Building) || !Building.BuildingData)
	{
		HandleCloseInspectorClicked(); return;
	}
	const UBuildingData* Data = Building.BuildingData;
	const FString MaterialName = StaticEnum<EMaterialType>()->GetDisplayNameTextByValue(static_cast<int64>(Data->Material)).ToString();
	const FString FoundationName = Building.Foundation == EFoundationQuality::Strong ? TEXT("Güçlü") : Building.Foundation == EFoundationQuality::Weak ? TEXT("Zayıf") : TEXT("Orta");
	const FString DamageName = StaticEnum<EDamageState>()->GetDisplayNameTextByValue(static_cast<int64>(Building.DamageState)).ToString();
	FString Details = FString::Printf(TEXT("%s  |  #%d\nParsel: %d, %d\n\n%s  |  %d kat  |  %d yaş\nTemel: %s  |  Güçlendirme: %s\nHasar: %.0f / 100  —  %s\nYapı kalitesi: %.0f  |  Çatı: %.0f\n\nKonut kapasitesi: %d  |  İşyeri kapasitesi: %d\nVergi: %d TL/tur  |  Bakım: %d TL/tur"),
		*Data->DisplayName.ToString(), Building.InstanceID, Building.GridCoords.X, Building.GridCoords.Y,
		*MaterialName, Building.CurrentFloors, Building.AgeInYears, *FoundationName, Building.bIsRetrofitted ? TEXT("Var") : TEXT("Yok"),
		Building.CurrentDamage, *DamageName, Building.StructuralQuality * 100.0f, Building.RoofQuality * 100.0f,
		Data->Residents, Data->Workers, Data->TaxPerTurn, Data->MaintenancePerTurn);
	if (Terrain)
	{
		const FGridCell Cell = Terrain->GetCell(Building.GridCoords);
		Details += FString::Printf(TEXT("\n\nFay yakınlığı: %.0f  |  Nehir yakınlığı: %.0f\nDrenaj: %.0f  |  Rüzgâr koridoru: x%.2f"),
			Cell.FaultLineProximity * 100.0f, Cell.RiverProximity * 100.0f, Cell.DrainageQuality * 100.0f, Cell.ValleyWindMultiplier);
		Details += Cell.bGroundSurveyed ? FString::Printf(TEXT("\nEtüt: %s (senaryo zemin sınıfı)"), Cell.SoilType == EAftershockSoilType::Bedrock ? TEXT("Ana kaya") : TEXT("Alüvyon")) : TEXT("\nZemin: bilinmiyor — 500 TL etüt yapın.");
		if (Data->Type == EBuildingType::Farm || Data->Type == EBuildingType::LivestockFarm)
			Details += FString::Printf(TEXT("\nToprak nemi: %%%.0f | Ürün sağlığı: %%%.0f"),Cell.SoilMoisture01*100,Cell.CropHealth01*100);
	}
	if (const UCityLifeSubsystem* Life = World ? World->GetSubsystem<UCityLifeSubsystem>() : nullptr) Details += TEXT("\n\n") + Life->DescribeBuilding(Building.InstanceID).ToString();
	const bool bMemorial=Building.DamageState==EDamageState::Memorial;
	if(bMemorial)
	{
		const int32 RemainingTurns=Buildings->GetMemorialTurnsRemaining(Building.InstanceID);
		Details+=TEXT("\n\nANMA ALANI\n");
		Details+=RemainingTurns>0?FString::Printf(TEXT("Parsel %d tur daha inşaata kapalı."),RemainingTurns):TEXT("Anma süresi doldu; güvenli araç çıkışı bekleniyor.");
		Details+=FString::Printf(TEXT("\nBu alana bağlı kayıtlı kayıp: %d"),Buildings->GetMemorialCasualtyIDs(Building.InstanceID).Num());
	}
	FString Remembered;
	if(const auto* Ledger=World?World->GetSubsystem<UCasualtyLedgerGenerator>():nullptr)
	{
		const TArray<int32> IDs=bMemorial?Buildings->GetMemorialCasualtyIDs(Building.InstanceID):Ledger->GetBuildingCasualtyIDs(Building.InstanceID);
		for(int32 I=0;I<FMath::Min(IDs.Num(),10);++I){FResidentIdentityCard Person;if(Ledger->GetResidentIdentityCard(IDs[I],Person))Remembered+=FString::Printf(TEXT("%s · %d yaş · %s (#%d)\n"),*Person.FullName.ToString(),Person.Age,*Person.Occupation.ToString(),Person.ResidentID);}
		if(IDs.Num()>10)Remembered+=FString::Printf(TEXT("Diğer %d kişinin kaydı kayıp defterinde."),IDs.Num()-10);
		const auto Stories=Ledger->GetBuildingNarrativePreview(Building.InstanceID,3);
		if(!Stories.IsEmpty())
		{
			Details+=TEXT("\n\nBU BİNAYLA İLİŞKİLİ KAYITLAR\n")+Stories[0].Narrative.ToString();
			Remembered+=TEXT("\nBU BİNAYLA İLİŞKİLİ KAYITLAR\n");
			for(const auto& Story:Stories)Remembered+=Story.Narrative.ToString()+TEXT("\n\n");
		}
	}
	TextBuildingDetails->SetText(FText::FromString(Details));
	TextBuildingDetails->SetToolTipText(FText::FromString(Remembered));
	BuildingInspector->SetToolTipText(FText::FromString(Remembered));
	const bool bCanAct = CanIssueCityOrder();
	const bool bCollapsed = Building.DamageState == EDamageState::Collapsed;
	const int32 RepairCost = Building.CurrentDamage > 50.0f && Building.CurrentDamage < 80.0f ? 5000
		: Building.CurrentDamage >= 20.0f && Building.CurrentDamage <= 50.0f ? 2000
		: Building.CurrentDamage > 0.0f && Building.CurrentDamage < 20.0f ? 500 : 0;
	if (BtnInspectorRetrofit)
	{
		BtnInspectorRetrofit->SetIsEnabled(bCanAct && !bCollapsed && !bMemorial && !Building.bIsRetrofitted && (Economy && Economy->CanAfford(1000)));
		BtnInspectorRetrofit->SetToolTipText(FText::FromString(Building.bIsRetrofitted ? TEXT("Bu yapı zaten güçlendirilmiş.") : TEXT("Sismik güçlendirme: 1.000 TL. Bütçe yeterli olmalıdır.")));
	}
	if (BtnInspectorRepair)
	{
		BtnInspectorRepair->SetIsEnabled(bCanAct && !bCollapsed && !bMemorial && RepairCost > 0 && (Economy && Economy->CanAfford(RepairCost)));
		BtnInspectorRepair->SetToolTipText(FText::Format(NSLOCTEXT("Aftershock", "InspectorRepairQuote", "Onarım: {0} TL. Hasarlı ve ayakta bir bina gerektirir."), FText::AsNumber(RepairCost)));
	}
	if (BtnInspectorRubble) BtnInspectorRubble->SetIsEnabled(bCanAct && bCollapsed && (Economy && Economy->CanAfford(3000)));
	if(BtnInspectorRubble)BtnInspectorRubble->SetToolTipText(NSLOCTEXT("Aftershock","MemorialPurchaseTooltip","3.000 TL: çöken yapının enkazını bir anma alanına dönüştür. Parsel üç gerçek tur inşaata kapalı kalır; kayıtlı kayıplar ve çiçekler korunur."));
}

void UAftershockMainHUDWidget::ApplyInspectorAction(int32 Action)
{
	if (!CanIssueCityOrder()) return;
	UWorld* World = GetWorld();
	UEconomyAndStateSubsystem* Economy = World ? World->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	UBuildingManagerSubsystem* Buildings = World ? World->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
	FBuildingInstance Building;
	if (!Economy || !Buildings || !Buildings->GetBuildingByID(InspectedBuildingID, Building)) return;
	bool bSuccess = false;
	if (Action == 4 && !Building.bIsRetrofitted && Building.DamageState != EDamageState::Collapsed && Building.DamageState != EDamageState::Memorial) bSuccess = Economy->RetrofitWithBudget(Building.InstanceID);
	if (Action == 5 && Building.DamageState != EDamageState::Collapsed && Building.DamageState != EDamageState::Memorial) bSuccess = Economy->RepairWithBudget(Building.InstanceID);
	if (Action == 6 && Building.DamageState == EDamageState::Collapsed) bSuccess = Economy->ClearRubbleWithBudget(Building.GridCoords);
	RefreshAllUI(); RefreshBuildingInspector();
	ShowActionNotice(bSuccess ? NSLOCTEXT("Aftershock", "InspectorActionDone", "Yapı işlemi tamamlandı; hazine güncellendi.") : NSLOCTEXT("Aftershock", "InspectorActionFailed", "İşlem uygulanamadı. Yapının durumunu ve bütçeyi kontrol edin."), bSuccess);
}

void UAftershockMainHUDWidget::HandleInspectorRetrofitClicked() { ApplyInspectorAction(4); }
void UAftershockMainHUDWidget::HandleInspectorRepairClicked() { ApplyInspectorAction(5); }
void UAftershockMainHUDWidget::HandleInspectorRubbleClicked() { ApplyInspectorAction(6); }
void UAftershockMainHUDWidget::HandleCloseInspectorClicked()
{
	InspectedBuildingID = INDEX_NONE;
	if (BuildingInspector) BuildingInspector->SetVisibility(ESlateVisibility::Collapsed);
	if (CityLifeCard) CityLifeCard->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
}

void UAftershockMainHUDWidget::NativeDestruct()
{
	if(auto* D=GetWorld()?GetWorld()->GetSubsystem<UAftershockDisasterDirectorSubsystem>():nullptr)D->OnDirectorChanged.RemoveAll(this);
	if(auto* GI=GetGameInstance())GI->GetSubsystem<UAftershockSaveSubsystem>()->OnSaveCompleted.RemoveDynamic(this,&UAftershockMainHUDWidget::ShowSaveFeedback);
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(WarningTimerHandle);
		World->GetTimerManager().ClearTimer(BuildHintTimerHandle);
		World->GetTimerManager().ClearTimer(ResultsRevealTimer);

		if (UEconomyAndStateSubsystem* EconomySubsystem = World->GetSubsystem<UEconomyAndStateSubsystem>())
		{
			EconomySubsystem->OnEconomyUpdated.RemoveDynamic(this, &UAftershockMainHUDWidget::HandleEconomyUpdated);
			EconomySubsystem->OnClimateUpdated.RemoveDynamic(this, &UAftershockMainHUDWidget::HandleClimateUpdated);
			EconomySubsystem->OnGameOver.RemoveDynamic(this, &UAftershockMainHUDWidget::HandleGameOver);
		}

		if (UDisasterSimulationSubsystem* DisasterSubsystem = World->GetSubsystem<UDisasterSimulationSubsystem>())
		{
			DisasterSubsystem->OnDisasterCompleted.RemoveDynamic(this, &UAftershockMainHUDWidget::HandleDisasterCompleted);
		}
		if (UCityLifeSubsystem* Life = World->GetSubsystem<UCityLifeSubsystem>()) Life->OnCityLifeUpdated.RemoveDynamic(this, &UAftershockMainHUDWidget::HandleCityLifeUpdated);
		if (UAftershockScenarioSubsystem* Scenario = World->GetSubsystem<UAftershockScenarioSubsystem>())
		{
			Scenario->OnScenarioUpdated.RemoveDynamic(this, &UAftershockMainHUDWidget::HandleScenarioUpdated);
		}

		if (UGameInstance* GI = World->GetGameInstance())
		{
			if (UQuizSubsystem* QuizSubsystem = GI->GetSubsystem<UQuizSubsystem>())
			{
				QuizSubsystem->OnQuizRequested.RemoveDynamic(this, &UAftershockMainHUDWidget::HandleQuizRequested);
				QuizSubsystem->OnQuizAnswered.RemoveDynamic(this, &UAftershockMainHUDWidget::HandleQuizAnswered);
			}
		}
	}

	Super::NativeDestruct();
}

void UAftershockMainHUDWidget::HandleEconomyUpdated(int64 TreasuryTL, float Satisfaction, int32 Turn)
{
	if (TextTreasury)
	{
		TextTreasury->SetText(FText::Format(NSLOCTEXT("Aftershock", "TreasuryFmt", "Bütçe: {0} TL"), FText::AsNumber(TreasuryTL)));
	}

	if (TextSatisfaction)
	{
		TextSatisfaction->SetText(FText::Format(NSLOCTEXT("Aftershock", "SatisfactionFmt", "Şehir Durumu: %{0}"), FText::AsNumber(FMath::RoundToInt(Satisfaction))));
	}

	if (BarSatisfaction)
	{
		BarSatisfaction->SetPercent(FMath::Clamp(Satisfaction / 100.0f, 0.0f, 1.0f));

		if (Satisfaction < 25.0f)
		{
			// Kritik Eşik Kırmızı Uyarısı
			BarSatisfaction->SetFillColorAndOpacity(FLinearColor(0.95f, 0.15f, 0.15f, 1.0f));
		}
		else if (Satisfaction < 50.0f)
		{
			BarSatisfaction->SetFillColorAndOpacity(FLinearColor(1.0f, 0.65f, 0.1f, 1.0f));
		}
		else
		{
			BarSatisfaction->SetFillColorAndOpacity(FLinearColor(0.2f, 0.85f, 0.35f, 1.0f));
		}
	}

	if (TextTurnYear)
	{
		int32 Year = 2026;
		if (UWorld* World = GetWorld())
		{
			if (UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>())
			{
				Year = Economy->CurrentYear;
			}
		}
		TextTurnYear->SetText(FText::Format(NSLOCTEXT("Aftershock", "TurnYearFmt", "Tur: {0} | Yıl: {1}"), FText::AsNumber(Turn), FText::FromString(FString::FromInt(Year))));
	}

	// Düzenli gelir/gider metnini güncelle
	if (TextIncomeExpense)
	{
		int64 ProjectedIncome = 0;
		int64 ProjectedExpense = 0;
		if (UWorld* World = GetWorld())
		{
			if (UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>())
			{
				Economy->CalculateProjectedFinances(ProjectedIncome, ProjectedExpense);
			}
		}

		const int64 Net = ProjectedIncome - ProjectedExpense;
		TextIncomeExpense->SetText(FText::Format(
			NSLOCTEXT("Aftershock", "IncomeExpenseFmt", "+{0} TL / -{1} TL (Net: {2}{3} TL)"),
			FText::AsNumber(ProjectedIncome),
			FText::AsNumber(ProjectedExpense),
			FText::FromString(Net >= 0 ? TEXT("+") : TEXT("")),
			FText::AsNumber(Net)
		));
	}
}

void UAftershockMainHUDWidget::HandleClimateUpdated(float CO2Level, float ClimateRiskMultiplier)
{
	if (TextClimateRisk)
	{
		const float DisplayRisk = FMath::RoundToFloat(ClimateRiskMultiplier * 100.0f) / 100.0f;
		TextClimateRisk->SetText(FText::Format(
			NSLOCTEXT("Aftershock", "LocalClimateRiskFmt", "Kirlilik: {0} | Risk: x{1}"),
			FText::AsNumber(FMath::RoundToInt(CO2Level)),
			FText::AsNumber(DisplayRisk)
		));
	}
	RefreshScenarioGoalUI();
}

void UAftershockMainHUDWidget::HandleScenarioUpdated()
{
	RefreshScenarioGoalUI();
}

void UAftershockMainHUDWidget::RefreshScenarioGoalUI()
{
	RefreshGeographicDisplay();
	UWorld* World = GetWorld();
	const UAftershockScenarioSubsystem* Scenario = World ? World->GetSubsystem<UAftershockScenarioSubsystem>() : nullptr;
	const bool bPrepared = Scenario && Scenario->IsPrepared();
	if (TextGoalDescription)
	{
		TextGoalDescription->SetText(bPrepared ? Scenario->GetBriefingText()
			: NSLOCTEXT("Aftershock", "ScenarioPreparing", "Şehir görevi hazırlanıyor. Afet önlemleri için yapı ve arazi hücrelerini inceleyin."));
	}
	if (TextScenarioProgress)
	{
		TextScenarioProgress->SetText(bPrepared ? Scenario->GetProgressText() : FText::GetEmpty());
	}
	const UWorldClimateSubsystem* Climate = World ? World->GetSubsystem<UWorldClimateSubsystem>() : nullptr;
	if (TextScenarioClimate && Climate)
	{
		const FAftershockWorldClimateState& State = Climate->GetState();
		FNumberFormattingOptions ClimateNumber;
		ClimateNumber.MinimumFractionalDigits = 2;
		ClimateNumber.MaximumFractionalDigits = 2;
		TextScenarioClimate->SetText(FText::Format(NSLOCTEXT("Aftershock", "ScenarioClimateCard", "Sanayi öncesine göre: {0}{1} °C\nYerel ısı baskısı: %{2} (oyun göstergesi)"),
			FText::FromString(State.GlobalTemperatureAnomalyC >= 0.f ? TEXT("+") : TEXT("")),
			FText::AsNumber(State.GlobalTemperatureAnomalyC, &ClimateNumber), FText::AsNumber(FMath::RoundToInt(State.LocalUrbanHeatPressure * 100.f))));
	}
	if (BtnNextLevel)
	{
		const auto* GI=GetGameInstance();const bool bCampaign=!GI||GI->GetSubsystem<UAftershockOpeningSessionSubsystem>()->GetSettings().Mode==EAftershockPlayMode::Campaign;
		BtnNextLevel->SetVisibility(bCampaign?ESlateVisibility::Visible:ESlateVisibility::Collapsed);
		const bool bComplete = bPrepared && Scenario->CanAdvanceScenario();
		const bool bFinal = bPrepared && Scenario->IsFinalScenario();
		BtnNextLevel->SetIsEnabled(bComplete && !bFinal && CanIssueCityOrder() && !bIsWarningActive);
		if (UTextBlock* Label = Cast<UTextBlock>(BtnNextLevel->GetContent()))
		{
			Label->SetText(bComplete && bFinal ? NSLOCTEXT("Aftershock", "CampaignAllComplete", "KAMPANYA TAMAMLANDI")
				: bComplete ? NSLOCTEXT("Aftershock", "NextScenarioReady", "SONRAKİ ŞEHRE GEÇ >")
				: NSLOCTEXT("Aftershock", "NextScenarioLocked", "ÖNCE GÖREVLERİ TAMAMLA"));
		}
		BtnNextLevel->SetToolTipText(bComplete && bFinal
			? NSLOCTEXT("Aftershock", "CampaignAllCompleteTooltip", "Tüm şehir senaryolarının hedefleri tamamlandı. Şehri yönetmeye devam edebilir veya ana menüden yeni kampanya başlatabilirsiniz.")
			: NSLOCTEXT("Aftershock", "NextScenarioTooltip", "Ana afeti atlatın ve yukarıdaki görev hedeflerini tamamlayın. Sonraki şehir kendi bütçesi, nüfusu ve arazi verisiyle açılır."));
	}
}

void UAftershockMainHUDWidget::RefreshGeographicDisplay()
{
	const auto* W=GetWorld();const auto* Terrain=W?W->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const auto* Scenario=W?W->GetSubsystem<UAftershockScenarioSubsystem>():nullptr;
	const bool Ready=Terrain&&Scenario&&Scenario->IsPrepared()&&Terrain->IsUsingRealGeography();
	if(TextGeodataAttribution)TextGeodataAttribution->SetVisibility(Ready?ESlateVisibility::HitTestInvisible:ESlateVisibility::Collapsed);
	if(GeodataAttributionPlate)GeodataAttributionPlate->SetVisibility(Ready?ESlateVisibility::Visible:ESlateVisibility::Collapsed);
	if(TextGeographicLocation)TextGeographicLocation->SetVisibility(Ready?ESlateVisibility::HitTestInvisible:ESlateVisibility::Collapsed);
	if(!Ready)return;
	const auto* Location=TryGetAftershockGeographyFocus(Terrain->GeographyIndex,Terrain->GeographyFocusIndex);if(!Location)return;
	const auto* PC=GetOwningPlayer();const auto* Camera=PC?Cast<AAftershockCameraPawn>(PC->GetPawn()):nullptr;
	double Latitude=0,Longitude=0;const bool Hover=Camera&&Camera->GetPlanetOverviewBlend()>=.2f&&Camera->TryGetGlobeGeographicAtCursor(Latitude,Longitude);
	const bool HasPosition=Hover||(Camera&&Camera->GetCurrentGeographicFocus(Latitude,Longitude));
	const TCHAR* Region=GetAftershockGeography(Terrain->GeographyIndex).Label;
	const FString Place=FCString::Strcmp(Location->FocusLabel,Region)==0?FString(Region):FString::Printf(TEXT("%s / %s"),Location->FocusLabel,Region);
	const FString Display=HasPosition?FString::Printf(TEXT("%s\n%s: %.5f°, %.5f°"),*Place,Hover?TEXT("Küre üzerindeki konum"):TEXT("Şehir odağı"),Latitude,Longitude):Place;
	if(TextGeographicLocation&&Display!=LastGeographicDisplay){TextGeographicLocation->SetText(FText::FromString(Display));LastGeographicDisplay=Display;}
}

void UAftershockMainHUDWidget::HandleNextLevelClicked()
{
	if (!CanIssueCityOrder() || bIsWarningActive) return;
	UAftershockScenarioSubsystem* Scenario = GetWorld() ? GetWorld()->GetSubsystem<UAftershockScenarioSubsystem>() : nullptr;
	if (!Scenario || !Scenario->CanAdvanceScenario()) { RefreshScenarioGoalUI(); return; }
	CancelBuildModeFromUI();
	CancelMapTool();
	if (!Scenario->AdvanceToNextScenario()) RefreshScenarioGoalUI();
}

void UAftershockMainHUDWidget::HandleGameOver(FText ReasonMessage)
{
	CancelBuildModeFromUI();
	CancelMapTool();
	if (UWorld* World = GetWorld()) World->GetTimerManager().ClearTimer(WarningTimerHandle);
	bIsWarningActive = false;
	if (PanelDisasterWarning) PanelDisasterWarning->SetVisibility(ESlateVisibility::Collapsed);
	if(auto* Mode=GetWorld()?GetWorld()->GetAuthGameMode<AAftershockGameMode>():nullptr){Mode->EvaluateGameFlow();return;}
	const UWorld* World = GetWorld();
	const UDisasterSimulationSubsystem* Disaster = World ? World->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
	const FAftershockDisasterVisualSnapshot* Snapshot = Disaster ? &Disaster->GetLatestVisualSnapshot() : nullptr;
	if (RemainingPresentationSeconds(World,Snapshot)>0.f)
	{
		if (TextGameOverReason) TextGameOverReason->SetText(ReasonMessage);
		return; // Final outcome becomes visible after the existing visual event.
	}
	if (PanelGameOver)
	{
		PanelGameOver->SetVisibility(ESlateVisibility::Visible);
	}

	if (TextGameOverReason)
	{
		TextGameOverReason->SetText(ReasonMessage);
	}
}

void UAftershockMainHUDWidget::HandleDisasterCompleted(const FDisasterReport& Report)
{
	if(Report.bIsPending)return; // A queued physics job has no casualty/budget result yet.
	if(Report.bWasRejected)
	{
		bDisasterInFlight=false;bIsWarningActive=false;
		if(GetWorld())GetWorld()->GetTimerManager().ClearTimer(ResultsRevealTimer);
		PendingVisualRevision=INDEX_NONE;
		if(PanelDisasterWarning)PanelDisasterWarning->SetVisibility(ESlateVisibility::Collapsed);
		ShowActionNotice(Report.RejectionReason.IsEmpty()?NSLOCTEXT("Aftershock","PhysicalEventAborted","Fiziksel olay güvenle durduruldu; sonuç ve afet ödemesi uygulanmadı."):Report.RejectionReason);
		RefreshAllUI();return;
	}
	UWorld* World = GetWorld();
	if (World) World->GetTimerManager().ClearTimer(ResultsRevealTimer);
	PendingVisualRevision = INDEX_NONE;
	const UDisasterSimulationSubsystem* Disaster = World ? World->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
	const FAftershockDisasterVisualSnapshot* Snapshot = Disaster ? &Disaster->GetLatestVisualSnapshot() : nullptr;
	const float Remaining = RemainingPresentationSeconds(World,Snapshot);
	if (Remaining > 0.1f)
	{
		PendingVisualDisasterReport = Report;
		PendingVisualRevision = Snapshot->Revision;
		bDisasterInFlight = true;
		RefreshAllUI();
		ShowActionNotice(NSLOCTEXT("Aftershock", "WatchActiveDisaster", "Afet sürüyor. Kamera ile şehri inceleyin; görsel olay bitince bilanço açılacak."), true);
		const int32 ExpectedRevision = PendingVisualRevision;
		World->GetTimerManager().SetTimer(ResultsRevealTimer,
			FTimerDelegate::CreateWeakLambda(this, [this, ExpectedRevision]()
			{
				if (PendingVisualRevision == ExpectedRevision) RevealPendingDisasterResults();
			}), Remaining + 0.05f, false);
		return;
	}
	bDisasterInFlight = false;
	RefreshAllUI();
	ShowDisasterResults(Report);
}

void UAftershockMainHUDWidget::RevealPendingDisasterResults()
{
	const UWorld* World = GetWorld();
	const UDisasterSimulationSubsystem* Disaster = World ? World->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
	if (!Disaster || PendingVisualRevision == INDEX_NONE || Disaster->GetLatestVisualSnapshot().Revision != PendingVisualRevision) return;
	PendingVisualRevision = INDEX_NONE;
	bDisasterInFlight = false;
	RefreshAllUI();
	const UEconomyAndStateSubsystem* Economy = GetWorld() ? GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	if (!Economy || !Economy->bIsGameOver) ShowDisasterResults(PendingVisualDisasterReport);
}

void UAftershockMainHUDWidget::RefreshAllUI()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>())
	{
		HandleEconomyUpdated(Economy->TreasuryTL, Economy->PublicSatisfaction, Economy->CurrentTurn);
		HandleClimateUpdated(Economy->CityCO2Level, Economy->ClimateRiskMultiplier);

		if (TextDisasterLevel)
		{
			TextDisasterLevel->SetText(FText::Format(
				NSLOCTEXT("Aftershock", "DisasterLevelFmt", "Afet Seviyesi: {0}"),
				FText::AsNumber(Economy->CurrentDisasterLevel)
			));
		}

		if (Economy->bIsGameOver)
		{
			HandleGameOver(NSLOCTEXT("Aftershock", "GameOverDefault", "Yöneticilik Göreviniz Sonlandırıldı!"));
		}
	}
	RefreshScenarioGoalUI();
	RefreshCityLifeUI();
}

void UAftershockMainHUDWidget::HandleNextTurnClicked()
{
	if(!CanIssueCityOrder()||bIsWarningActive)return;
	const auto* Settings=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSettingsSubsystem>():nullptr;
	if(Settings&&Settings->GetPreferences().bConfirmEndTurn&&FPlatformTime::Seconds()>TurnConfirmationDeadline)
	{
		TurnConfirmationDeadline=FPlatformTime::Seconds()+6.;
		ShowActionNotice(NSLOCTEXT("Aftershock","ConfirmEndTurn","Turu bitirmek üzeresin. Onaylamak için 6 saniye içinde TURU BİTİR düğmesine yeniden tıkla."));return;
	}
	TurnConfirmationDeadline=0.;
	AdvanceTurnFromUI();
}

void UAftershockMainHUDWidget::AdvanceTurnFromUI()
{
 if(!CanIssueCityOrder()||bIsWarningActive)return;
 if(auto* E=GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>())E->AdvanceTurn();
 RefreshAllUI();
}

// =========================================================================
// AFET UYARISI, GERİ SAYIM VE HAZIRLIK AKIŞI
// =========================================================================
void UAftershockMainHUDWidget::StartDisasterWarningCountdown(EDisasterType DisasterType, float WarningSeconds)
{
	if (!AllowsManualDisasterControl(GetGameInstance())) { ShowActionNotice(AutomaticGeographyNotice()); return; }
	if (!CanIssueCityOrder()) return;
	// İkinci kez tıklandığında uyarının bitmesini beklemeden afeti anında başlat
	if (bIsWarningActive && PendingDisasterType == DisasterType)
	{
		TriggerDisasterFromUI(DisasterType);
		return;
	}
	if (bIsWarningActive) return;
	if (auto* Director = GetWorld()->GetSubsystem<UAftershockDisasterDirectorSubsystem>())
	{
		FText Error;
		if (!Director->RequestWarning(DisasterType, WarningSeconds, Error)) ShowActionNotice(Error);
		else RefreshDirectorUI();
	}
}

void UAftershockMainHUDWidget::UpdateWarningCountdownTick()
{
 RefreshDirectorUI(); // The director owns time and transitions; UMG only displays it.
}

void UAftershockMainHUDWidget::HandleSkipPreparationClicked()
{
 if(!bIsWarningActive||HasBlockingModal())return;
 TriggerDisasterFromUI(PendingDisasterType);
}

void UAftershockMainHUDWidget::HandleFocusDisasterClicked()
{
	AAftershockCameraPawn* Cam = GetCameraPawn();
	const UTerrainGridSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	UDisasterSimulationSubsystem* Sim = GetWorld() ? GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
	const UAftershockDisasterDirectorSubsystem* Director = GetWorld() ? GetWorld()->GetSubsystem<UAftershockDisasterDirectorSubsystem>() : nullptr;
	if (!Cam || !Terrain) return;

	FIntPoint TargetCoord(Terrain->GridWidth / 2, Terrain->GridHeight / 2);
	bool bFoundTarget = false;

	// 1) If Director is in Warning phase, focus on the upcoming disaster's anticipated epicenter
	if (Director && Director->GetState().Phase == EAftershockDisasterPhase::Warning)
	{
		const EDisasterType Pending = Director->GetState().PendingType;
		if (Pending == EDisasterType::Volcano && Terrain->VolcanicVentCoord != FIntPoint(INDEX_NONE, INDEX_NONE))
		{
			TargetCoord = Terrain->VolcanicVentCoord;
			bFoundTarget = true;
		}
		else if (Pending == EDisasterType::Tsunami)
		{
			TargetCoord = FIntPoint(Terrain->GridWidth / 2, FMath::Max(0, Terrain->CoastStartRow));
			bFoundTarget = true;
		}
		else if (Pending == EDisasterType::Earthquake)
		{
			float MaxScore = -1.f;
			for (const FGridCell& Cell : Terrain->Cells)
			{
				if (!Cell.bIsPlayable) continue;
				float Weight = (Cell.OccupyingBuildingID != INDEX_NONE) ? 20.f : (Cell.bHasRoad ? 6.f : (!Cell.bIsRiverbed && !Cell.bIsOcean ? 2.f : 0.05f));
				const float Score = Cell.FaultLineProximity * Weight;
				if (Score > MaxScore)
				{
					MaxScore = Score;
					TargetCoord = Cell.Coords;
					bFoundTarget = true;
				}
			}
		}
		else if (Pending == EDisasterType::Flood)
		{
			// Real-source V3 warning focus uses the SAME read-only hydrology
			// preview at the pending intensity. Capacity alone cannot locate wet land.
			const bool bPhysicalPreview=Terrain->ContinuationGeneratorVersion>=3;
			const TMap<FIntPoint,float> Preview=bPhysicalPreview&&Sim?Sim->SimulateFloodMap(Director->GetState().Severity):TMap<FIntPoint,float>();
			float BestScore = -1.f;
			for (const FGridCell& Cell : Terrain->Cells)
			{
				if (!Cell.bIsPlayable || Cell.bIsOcean) continue;
				const float LocalSeverity=bPhysicalPreview?Preview.FindRef(Cell.Coords):0.f;
				if(bPhysicalPreview&&(!FMath::IsFinite(LocalSeverity)||LocalSeverity<=0.f))continue;
				float Weight = (Cell.OccupyingBuildingID != INDEX_NONE) ? 25.f : (Cell.bHasRoad ? 8.f : (!Cell.bIsRiverbed ? 3.f : 0.1f));
				const float Score = (bPhysicalPreview?LocalSeverity:100.f-FMath::Min(99.f,Cell.RiverCapacity))*Weight;
				if (Score > BestScore)
				{
					BestScore = Score;
					TargetCoord = Cell.Coords;
					bFoundTarget = true;
				}
			}
			if(bPhysicalPreview&&!bFoundTarget)
			{
				ShowActionNotice(FText::FromString(TEXT("Bu yağış senaryosunun mevcut önizlemesinde ıslak bir hücre bulunmuyor.")));
				return; // Do not fall back to an unrelated previous event or a dry building.
			}
		}
		else if (Pending == EDisasterType::Wind)
		{
			float MaxScore = -1.f;
			for (const FGridCell& Cell : Terrain->Cells)
			{
				if (!Cell.bIsPlayable || Cell.bIsOcean) continue;
				float Weight = (Cell.OccupyingBuildingID != INDEX_NONE) ? 15.f : (Cell.bHasRoad ? 5.f : (!Cell.bIsRiverbed ? 2.f : 0.2f));
				const float Score = Cell.ValleyWindMultiplier * Weight;
				if (Score > MaxScore)
				{
					MaxScore = Score;
					TargetCoord = Cell.Coords;
					bFoundTarget = true;
				}
			}
		}
	}

	// 2) If active or latest snapshot has a valid SourceCoord, use it
	if (!bFoundTarget && Sim)
	{
		const auto& Snapshot = Sim->GetLatestVisualSnapshot();
		if (Snapshot.SourceCoord != FIntPoint(INDEX_NONE, INDEX_NONE) && Terrain->IsValidCoord(Snapshot.SourceCoord))
		{
			TargetCoord = Snapshot.SourceCoord;
			bFoundTarget = true;
		}
		else if (Snapshot.Type == EDisasterType::Volcano && Terrain->VolcanicVentCoord != FIntPoint(INDEX_NONE, INDEX_NONE))
		{
			TargetCoord = Terrain->VolcanicVentCoord;
			bFoundTarget = true;
		}
		else if (Snapshot.Type == EDisasterType::Tsunami)
		{
			TargetCoord = FIntPoint(Terrain->GridWidth / 2, FMath::Max(0, Terrain->CoastStartRow));
			bFoundTarget = true;
		}
	}
	Cam->PanToCell(TargetCoord);
}

void UAftershockMainHUDWidget::TriggerDisasterFromUI(EDisasterType DisasterType)
{
 if(!CanIssueCityOrder())return;
 auto* Director=GetWorld()->GetSubsystem<UAftershockDisasterDirectorSubsystem>();if(!Director)return;
 FText Error;
 if(Director->GetState().Phase==EAftershockDisasterPhase::Ready&&!AllowsManualDisasterControl(GetGameInstance())){ShowActionNotice(AutomaticGeographyNotice());return;}
 if(Director->GetState().Phase==EAftershockDisasterPhase::Ready&&!Director->RequestWarning(DisasterType,1.f,Error)){ShowActionNotice(Error);return;}
 if(Director->GetState().PendingType!=DisasterType){ShowActionNotice(FText::FromString(TEXT("Önce devam eden uyarıyı tamamlayın.")));return;}
 CancelBuildModeFromUI();CancelMapTool();
 if(!Director->TriggerPendingNow(Error)){ShowActionNotice(Error);return;}
 RefreshDirectorUI();
}

// =========================================================================
// AFET BİLANÇO VE SONUÇ EKRANI (RESULTS MODAL)
// =========================================================================
void UAftershockMainHUDWidget::ShowDisasterResults(const FDisasterReport& Report)
{
	if(Report.bIsPending||Report.bWasRejected)return;
	CancelBuildModeFromUI();
	CancelMapTool();
	if (PanelDisasterResults)
	{
		PanelDisasterResults->SetVisibility(ESlateVisibility::Visible);
	}

	// Sonuç modal açılış sesi + devlet ödeneği geldi ise coin feedback
	if (UGameInstance* GI = GetGameInstance())
	{
		if (UAftershockAudioSubsystem* Audio = GI->GetSubsystem<UAftershockAudioSubsystem>())
		{
			Audio->PlayPanelSwitch();
			if (Report.TotalStateAidGranted > 0)
			{
				Audio->PlayCoinGain();
			}
		}
	}

	if (TextResultDisasterTitle)
	{
		FText TypeName;
		switch (Report.DisasterType)
		{
		case EDisasterType::Earthquake:
			TypeName = NSLOCTEXT("Aftershock", "ReportEarthquake", "Deprem");
			break;
		case EDisasterType::Flood:
			TypeName = NSLOCTEXT("Aftershock", "ReportFlood", "Sel / Taşkın");
			break;
		case EDisasterType::Wind:
			TypeName = NSLOCTEXT("Aftershock", "ReportWind", "Şiddetli Fırtına");
			break;
		case EDisasterType::Tsunami:
			TypeName = NSLOCTEXT("Aftershock", "ReportTsunami", "Tsunami / Kıyı Taşkını");
			break;
		case EDisasterType::Fire: TypeName = FText::FromString(TEXT("Orman Yangını")); break;
		case EDisasterType::Volcano: TypeName = FText::FromString(TEXT("Volkanik Olay")); break;
		case EDisasterType::Drought: TypeName = FText::FromString(TEXT("Kuraklık")); break;
		case EDisasterType::Avalanche: TypeName = FText::FromString(TEXT("Çığ")); break;
		case EDisasterType::Landslide: TypeName = FText::FromString(TEXT("Heyelan")); break;
		default:
			TypeName = NSLOCTEXT("Aftershock", "ReportDisaster", "Doğal Afet");
			break;
		}

		TextResultDisasterTitle->SetText(FText::Format(
			NSLOCTEXT("Aftershock", "ResultTitleFmt", "AFET BİLANÇOSU — {0} (Şiddet: {1})"),
			TypeName,
			FText::AsNumber(FMath::RoundToInt(Report.BaseSeverity))
		));
	}

	if (TextResultCasualties)
	{
		TextResultCasualties->SetText(FText::Format(
			NSLOCTEXT("Aftershock", "ResultCasualtiesFmt", "Sağ kalan: {0} kişi | Can kaybı: {1} kişi"),
			FText::AsNumber(Report.TotalSurvivors),
			FText::AsNumber(Report.TotalDeaths)
		));
	}

	if (TextResultDamage)
	{
		TextResultDamage->SetText(FText::Format(
			NSLOCTEXT("Aftershock", "ResultDamageFmt", "Hasar Alan Bina: {0} | Çöken / Enkaz: {1}"),
			FText::AsNumber(Report.TotalBuildingsAffected),
			FText::AsNumber(Report.TotalBuildingsCollapsed)
		));
	}

	if (TextResultPrevented)
	{
		TextResultPrevented->SetText(FText::Format(
			NSLOCTEXT("Aftershock", "ResultPreventedFmt", "Mühendislik ve Önlemler Sayesinde Kurtarılan (Tahmini): {0} Kişi"),
			FText::AsNumber(Report.PreventedDeathsEstimate)
		));
	}

	if (TextResultAid)
	{
		TextResultAid->SetText(FText::Format(
			NSLOCTEXT("Aftershock", "ResultAidFmt", "Sonraki tur devlet ödeneği: +{0} TL"),
			FText::AsNumber(Report.TotalStateAidGranted)
		));
	}

	if (TextResultSatisfactionDelta)
	{
		const float Delta = Report.TotalSatisfactionChange;
		TextResultSatisfactionDelta->SetText(FText::Format(
			NSLOCTEXT("Aftershock", "ResultSatDeltaFmt", "Memnuniyet Değişimi: {0}{1} Puan"),
			FText::FromString(Delta >= 0 ? TEXT("+") : TEXT("")),
			FText::AsNumber(FMath::RoundToFloat(Delta * 10.0f) / 10.0f)
		));
	}

	if (TextResultBudgetChange)
	{
		int64 CurrentBudget = 0;
		if (UWorld* World = GetWorld())
		{
			if (UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>())
			{
				CurrentBudget = Economy->TreasuryTL;
			}
		}

		TextResultBudgetChange->SetText(FText::Format(
			NSLOCTEXT("Aftershock", "ResultBudgetFmt", "Mevcut Şehir Hazinesi: {0} TL"),
			FText::AsNumber(CurrentBudget)
		));
	}
}

void UAftershockMainHUDWidget::CloseDisasterResults()
{
	if (PanelDisasterResults)
	{
		PanelDisasterResults->SetVisibility(ESlateVisibility::Collapsed);
	}
	RefreshScenarioGoalUI();
	if(auto* Mode=GetWorld()?GetWorld()->GetAuthGameMode<AAftershockGameMode>():nullptr)Mode->EvaluateGameFlow();
}

void UAftershockMainHUDWidget::HandleCloseResultsClicked()
{
	CloseDisasterResults();
}

// =========================================================================
// KRİZ YÖNETİM MERKEZİ QUIZ MODALİ
// =========================================================================
void UAftershockMainHUDWidget::OpenQuizModal()
{
	if (!CanIssueCityOrder() || bIsWarningActive) return;
	UEconomyAndStateSubsystem* Economy = GetWorld() ? GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	const UQuizSubsystem* CanonicalQuiz=GetGameInstance()?GetGameInstance()->GetSubsystem<UQuizSubsystem>():nullptr;
	if (!Economy || (CanonicalQuiz&&!CanonicalQuiz->CanRequestQuestion(Economy->CurrentTurn))) return;
	LastQuizTurn = Economy->CurrentTurn;
	bAwaitingQuizAnswer = true;
	CancelBuildModeFromUI();
	CancelMapTool();
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (PanelQuizModal)
	{
		PanelQuizModal->SetVisibility(ESlateVisibility::Visible);
	}

	if (TextQuizFeedback)
	{
		TextQuizFeedback->SetText(FText::GetEmpty());
	}

	if (BtnCloseQuiz)
	{
		BtnCloseQuiz->SetVisibility(ESlateVisibility::Visible);
	}

	bool bQuestionLoaded = false;
	if (UGameInstance* GI = World->GetGameInstance())
	{
		if (UQuizSubsystem* Quiz = GI->GetSubsystem<UQuizSubsystem>())
		{
			const UAftershockScenarioSubsystem* Scenario = World->GetSubsystem<UAftershockScenarioSubsystem>();
			const EDisasterType Category = Scenario && Scenario->IsPrepared() ? Scenario->GetDefinition().Hazard : EDisasterType::Earthquake;
			bQuestionLoaded = Quiz->RequestRandomQuestion(Category, true);
		}
	}
	if (!bQuestionLoaded)
	{
		LastQuizTurn = INDEX_NONE;
		CloseQuizModal();
		ShowActionNotice(NSLOCTEXT("Aftershock", "QuizUnavailable", "Farkındalık sorusu yüklenemedi. Bu tur yeniden deneyebilirsiniz."));
	}
}

void UAftershockMainHUDWidget::HandleQuizRequested(const FQuizQuestion& Question)
{
	if(PanelQuizModal)PanelQuizModal->SetVisibility(ESlateVisibility::Visible);
	bAwaitingQuizAnswer = true;
	if (TextQuizQuestion)
	{
		TextQuizQuestion->SetText(Question.Question);
	}

	const TArray<FText>& Choices = Question.Choices;
	if (Choices.IsValidIndex(0) && TextQuizChoice0) TextQuizChoice0->SetText(Choices[0]);
	if (Choices.IsValidIndex(1) && TextQuizChoice1) TextQuizChoice1->SetText(Choices[1]);
	if (Choices.IsValidIndex(2) && TextQuizChoice2) TextQuizChoice2->SetText(Choices[2]);
	if (Choices.IsValidIndex(3) && TextQuizChoice3) TextQuizChoice3->SetText(Choices[3]);

	if (BtnQuizChoice0) BtnQuizChoice0->SetIsEnabled(true);
	if (BtnQuizChoice1) BtnQuizChoice1->SetIsEnabled(true);
	if (BtnQuizChoice2) BtnQuizChoice2->SetIsEnabled(true);
	if (BtnQuizChoice3) BtnQuizChoice3->SetIsEnabled(true);
}

void UAftershockMainHUDWidget::SubmitQuizChoice(int32 ChoiceIndex)
{
	if (!bAwaitingQuizAnswer || ChoiceIndex < 0 || ChoiceIndex > 3 || !PanelQuizModal || !PanelQuizModal->IsVisible()) return;
	bAwaitingQuizAnswer = false;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (BtnQuizChoice0) BtnQuizChoice0->SetIsEnabled(false);
	if (BtnQuizChoice1) BtnQuizChoice1->SetIsEnabled(false);
	if (BtnQuizChoice2) BtnQuizChoice2->SetIsEnabled(false);
	if (BtnQuizChoice3) BtnQuizChoice3->SetIsEnabled(false);

	if (UGameInstance* GI = World->GetGameInstance())
	{
		if (UQuizSubsystem* Quiz = GI->GetSubsystem<UQuizSubsystem>())
		{
			Quiz->SubmitAnswer(ChoiceIndex);
		}
	}
}

void UAftershockMainHUDWidget::HandleQuizAnswered(bool bCorrect, const FQuizQuestion& Question)
{
	if (TextQuizFeedback)
	{
		if (bCorrect)
		{
			TextQuizFeedback->SetText(NSLOCTEXT("Aftershock", "QuizCorrect", "[DOĞRU] Tebrikler! Şehre +1.000 TL afet farkındalık hibesi ve +5 halk güveni kazandırıldı."));
		}
		else
		{
			TextQuizFeedback->SetText(FText::Format(
				NSLOCTEXT("Aftershock", "QuizWrong", "Yanlış cevap. Bilgi notu: {0}"),
				Question.InfoNote
			));
		}
	}

	if (BtnCloseQuiz)
	{
		BtnCloseQuiz->SetVisibility(ESlateVisibility::Visible);
	}
}

void UAftershockMainHUDWidget::CloseQuizModal()
{
	bAwaitingQuizAnswer = false;
	if (PanelQuizModal)
	{
		PanelQuizModal->SetVisibility(ESlateVisibility::Collapsed);
	}
}

void UAftershockMainHUDWidget::HandleQuizChoice0Clicked() { SubmitQuizChoice(0); }
void UAftershockMainHUDWidget::HandleQuizChoice1Clicked() { SubmitQuizChoice(1); }
void UAftershockMainHUDWidget::HandleQuizChoice2Clicked() { SubmitQuizChoice(2); }
void UAftershockMainHUDWidget::HandleQuizChoice3Clicked() { SubmitQuizChoice(3); }
void UAftershockMainHUDWidget::HandleCloseQuizClicked() { CloseQuizModal(); }
void UAftershockMainHUDWidget::HandleOpenQuizClicked() { OpenQuizModal(); }

// =========================================================================
// DURAKLATMA (PAUSE) MENÜSÜ
// =========================================================================
void UAftershockMainHUDWidget::RefreshFramePacingUI()
{
	const float Limit=GEngine?GEngine->GetMaxFPS():0.f;const auto* Settings=UGameUserSettings::GetGameUserSettings();
	const auto* PC=Cast<AAftershockPlayerController>(GetOwningPlayer());const bool bCanChoose=PC&&PC->CanPersistFrameRatePreference();
	if(TextFramePacing)
	{
		const FString Label=Limit>0.f?FString::Printf(TEXT("%.0f FPS"),Limit):FString(TEXT("Sınırsız"));
		TextFramePacing->SetText(FText::FromString(FString::Printf(TEXT("GÖRÜNTÜ  ·  Kare sınırı: %s%s"),*Label,Settings&&Settings->IsVSyncEnabled()?TEXT("  ·  VSync açık"):TEXT(""))));
	}
	for(UAftershockCatalogActionButton* Button:FramePacingButtons)if(Button)
	{
		Button->SetIsEnabled(bCanChoose);
		Button->SetStyle(MakeAftershockButtonStyle(Button->GetStyle(),FLinearColor(.026f,.054f,.068f),FMath::IsNearlyEqual(Limit,static_cast<float>(Button->Value),.1f)));
	}
}

void UAftershockMainHUDWidget::TogglePauseMenu()
{
	if (bIsGamePaused) { ResumeGame(); return; }
	if (HasBlockingModal()) return;
	CancelBuildModeFromUI();
	CancelMapTool();
	TurnConfirmationDeadline=0.;
	RefreshFramePacingUI();
	bIsGamePaused = true;
	if (UWorld* World = GetWorld())
	{
		UGameplayStatics::SetGamePaused(World, bIsGamePaused);
	}

	if (PanelPauseMenu)
	{
		PanelPauseMenu->SetVisibility(bIsGamePaused ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	FocusPauseMenu();
}

void UAftershockMainHUDWidget::ResumeGame()
{
	if(const auto* Saves=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>():nullptr;Saves&&Saves->IsWorldLoadFailed(GetWorld())){UGameplayStatics::SetGamePaused(this,true);return;}
	bIsGamePaused = false;
	if (UWorld* World = GetWorld())
	{
		UGameplayStatics::SetGamePaused(World, false);
	}

	if (PanelPauseMenu)
	{
		PanelPauseMenu->SetVisibility(ESlateVisibility::Collapsed);
	}
}

void UAftershockMainHUDWidget::RestartCurrentLevel()
{
	ResumeGame();
	if (UWorld* World = GetWorld())
	{
		const UAftershockScenarioSubsystem* Scenario = World->GetSubsystem<UAftershockScenarioSubsystem>();
		const int32 ScenarioIndex = Scenario && Scenario->IsPrepared() ? Scenario->GetScenarioIndex() : 0;
		UGameplayStatics::OpenLevel(World, FName(*UGameplayStatics::GetCurrentLevelName(World, true)), true,
			FString::Printf(TEXT("AftershockScenario=%d"), ScenarioIndex));
	}
}

void UAftershockMainHUDWidget::QuitGame()
{
	// Preserve Blueprint callers: only the opening menu can exit the application.
	HandleMainMenuClicked();
}

void UAftershockMainHUDWidget::HandlePauseClicked() { TogglePauseMenu(); }
void UAftershockMainHUDWidget::HandleResumeClicked() { ResumeGame(); }
void UAftershockMainHUDWidget::HandleRestartLevelClicked() { RestartCurrentLevel(); }
void UAftershockMainHUDWidget::HandleQuitGameClicked() { QuitGame(); }

// =========================================================================
// 9 BİNA BUTONU CLICK HANDLER'LARI
// =========================================================================
void UAftershockMainHUDWidget::HandleBuildHouseClicked()       { SetBuildModeFromUI(EBuildingType::House); }
void UAftershockMainHUDWidget::HandleBuildApartmentClicked()   { SetBuildModeFromUI(EBuildingType::Apartment); }
void UAftershockMainHUDWidget::HandleBuildOfficeClicked()      { SetBuildModeFromUI(EBuildingType::Office); }
void UAftershockMainHUDWidget::HandleBuildFactoryClicked()     { SetBuildModeFromUI(EBuildingType::Factory); }
void UAftershockMainHUDWidget::HandleBuildHospitalClicked()    { SetBuildModeFromUI(EBuildingType::Hospital); }
void UAftershockMainHUDWidget::HandleBuildSchoolClicked()      { SetBuildModeFromUI(EBuildingType::School); }
void UAftershockMainHUDWidget::HandleBuildFireStationClicked() { SetBuildModeFromUI(EBuildingType::FireStation); }
void UAftershockMainHUDWidget::HandleBuildPowerPlantClicked()  { SetBuildModeFromUI(EBuildingType::PowerPlant); }
void UAftershockMainHUDWidget::HandleBuildGreenSpaceClicked()  { SetBuildModeFromUI(EBuildingType::GreenSpace); }

// =========================================================================
// ALTYAPI VE AFET HANDLER'LARI
// =========================================================================
void UAftershockMainHUDWidget::HandleInvestRiverClicked()    { OnInvestRiverClicked(); }
void UAftershockMainHUDWidget::HandleInvestDampingClicked()  { OnInvestWaterDampingClicked(); }
void UAftershockMainHUDWidget::HandleInvestDrainageClicked() { OnInvestDrainageClicked(); }

void UAftershockMainHUDWidget::HandleTriggerEarthquakeClicked()
{
	StartDisasterWarningCountdown(EDisasterType::Earthquake, 5.0f);
}

void UAftershockMainHUDWidget::HandleTriggerFloodClicked()
{
	StartDisasterWarningCountdown(EDisasterType::Flood, 5.0f);
}

void UAftershockMainHUDWidget::HandleTriggerWindClicked()
{
	StartDisasterWarningCountdown(EDisasterType::Wind, 5.0f);
}

void UAftershockMainHUDWidget::HandleTriggerTsunamiClicked()
{
	StartDisasterWarningCountdown(EDisasterType::Tsunami, 5.0f);
}

AAftershockCameraPawn* UAftershockMainHUDWidget::GetCameraPawn() const
{
	APlayerController* PC = GetOwningPlayer();
	return PC ? Cast<AAftershockCameraPawn>(PC->GetPawn()) : nullptr;
}

bool UAftershockMainHUDWidget::GetHoveredGridCoords(FIntPoint& OutCoords) const
{
	if (AAftershockCameraPawn* Pawn = GetCameraPawn())
	{
		return Pawn->GetHoveredGridCoords(OutCoords);
	}
	return false;
}

void UAftershockMainHUDWidget::InitializeFallbackBuildingData()
{
	if (FallbackBuildingCatalog.Num() > 0) return;
	UBuildingManagerSubsystem* Buildings = GetWorld() ? GetWorld()->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
	TArray<EBuildingType> Types; FAftershockBuildingCatalog::GetTypes(Types);
	for (EBuildingType Type : Types)
	{
		UBuildingData* Data = Buildings ? Buildings->GetCatalogData(Type) : FAftershockBuildingCatalog::CreateData(this, Type);
		if (Data) FallbackBuildingCatalog.Add(Type, Data);
	}
}

UBuildingData* UAftershockMainHUDWidget::FindBuildingDataByType(EBuildingType Type) const
{
	if (const TObjectPtr<UBuildingData>* Found = BuildingCatalog.Find(Type))
	{
		if (*Found)
		{
			return *Found;
		}
	}

	if (const TObjectPtr<UBuildingData>* FallbackFound = FallbackBuildingCatalog.Find(Type))
	{
		return *FallbackFound;
	}

	return nullptr;
}

void UAftershockMainHUDWidget::SetBuildModeFromUI(EBuildingType BuildingType, EFoundationQuality Foundation, int32 Floors)
{
	UBuildingData* Data = FindBuildingDataByType(BuildingType);
	if (!Data)
	{
		return;
	}

	SetBuildModeWithData(Data, Foundation, Floors);
}

void UAftershockMainHUDWidget::SetBuildModeWithData(UBuildingData* BuildingData, EFoundationQuality Foundation, int32 Floors)
{
	if (!CanIssueCityOrder()) return;
	CancelMapTool();
	if (AAftershockCameraPawn* Pawn = GetCameraPawn())
	{
		Pawn->SetBuildMode(BuildingData, Foundation, Floors);
	}
}

void UAftershockMainHUDWidget::CancelBuildModeFromUI()
{
	if (AAftershockCameraPawn* Pawn = GetCameraPawn())
	{
		Pawn->CancelBuildMode();
	}
}

bool UAftershockMainHUDWidget::ConfirmPlacementFromUI(int32& OutAssignedID)
{
	OutAssignedID = INDEX_NONE;
	if (!CanIssueCityOrder() || BlocksWorldInputAtCursor()) return false;
	AAftershockCameraPawn* Pawn = GetCameraPawn();
	if (!Pawn || !Pawn->ConfirmPlacement(OutAssignedID)) return false;
	RefreshAllUI();
	if (UGameInstance* GI = GetGameInstance())
	{
		if (UAftershockAudioSubsystem* Audio = GI->GetSubsystem<UAftershockAudioSubsystem>()) Audio->PlayPlacement();
	}
	return true;
}

bool UAftershockMainHUDWidget::OnInvestRiverClicked() { SelectMapTool(1); return ActiveMapTool == 1; }
bool UAftershockMainHUDWidget::OnInvestWaterDampingClicked() { SelectMapTool(2); return ActiveMapTool == 2; }
bool UAftershockMainHUDWidget::OnInvestDrainageClicked() { SelectMapTool(3); return ActiveMapTool == 3; }

bool UAftershockMainHUDWidget::InvestRiverAtCoords(FIntPoint Coords)
{
	UWorld* World = GetWorld();
	UEconomyAndStateSubsystem* Economy = World ? World->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	UDisasterSimulationSubsystem* Disaster = World ? World->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!CanIssueCityOrder() || !Economy || !Disaster || !Terrain || !Terrain->IsValidCoord(Coords) || !Terrain->GetCell(Coords).bIsRiverbed || !Economy->CanAfford(2500)) return false;
	int32 Cost = 0;
	if (!Disaster->InvestInRiverExpansion(Coords, Cost)) return false;
	const bool bPaid = Economy->TrySpendMoney(Cost);
	ensureMsgf(bPaid, TEXT("River investment cost changed after affordability check"));
	RefreshAllUI(); return bPaid;
}

bool UAftershockMainHUDWidget::InvestWaterDampingAtCoords(FIntPoint Coords)
{
	UWorld* World = GetWorld();
	UEconomyAndStateSubsystem* Economy = World ? World->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	UDisasterSimulationSubsystem* Disaster = World ? World->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!CanIssueCityOrder() || !Economy || !Disaster || !Terrain || !Terrain->IsValidCoord(Coords) || !Economy->CanAfford(2000)) return false;
	const FGridCell Cell = Terrain->GetCell(Coords);
	if (!Cell.bIsRiverbed || Cell.WaterFlowDamping >= 1.0f) return false;
	int32 Cost = 0;
	if (!Disaster->InvestInWaterDamping(Coords, Cost)) return false;
	const bool bPaid = Economy->TrySpendMoney(Cost);
	ensureMsgf(bPaid, TEXT("Water damping cost changed after affordability check"));
	RefreshAllUI(); return bPaid;
}

bool UAftershockMainHUDWidget::InvestStreetDrainageAtCoords(FIntPoint Coords, int32 Radius)
{
	UWorld* World = GetWorld();
	UEconomyAndStateSubsystem* Economy = World ? World->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	UDisasterSimulationSubsystem* Disaster = World ? World->GetSubsystem<UDisasterSimulationSubsystem>() : nullptr;
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!CanIssueCityOrder() || !Economy || !Disaster || !Terrain || !Terrain->IsValidCoord(Coords) || Terrain->GetCell(Coords).bIsRiverbed) return false;
	const int32 ClampedRadius = FMath::Clamp(Radius, 0, 5);
	int32 BillableCells = 0;
	bool bAnyBenefit = false;
	for (int32 Y = -ClampedRadius; Y <= ClampedRadius; ++Y)
	{
		for (int32 X = -ClampedRadius; X <= ClampedRadius; ++X)
		{
			const FIntPoint Target = Coords + FIntPoint(X, Y);
			if (X * X + Y * Y <= ClampedRadius * ClampedRadius && Terrain->IsValidCoord(Target))
			{
				++BillableCells;
				bAnyBenefit |= Terrain->GetCell(Target).DrainageQuality < 1.0f;
			}
		}
	}
	// Canonical API bills every valid cell in its radius, at 400 TL each.
	const int32 QuotedCost = BillableCells * 400;
	if (!bAnyBenefit || QuotedCost <= 0 || !Economy->CanAfford(QuotedCost)) return false;
	int32 ActualCost = 0;
	if (!Disaster->InvestInStreetDrainage(Coords, ClampedRadius, ActualCost)) return false;
	const bool bPaid = Economy->TrySpendMoney(ActualCost);
	ensureMsgf(bPaid && ActualCost == QuotedCost, TEXT("Drainage quote differs from canonical investment cost"));
	RefreshAllUI(); return bPaid;
}

void UAftershockMainHUDWidget::UpdateMetricsUI(int32 Budget, int32 StabilityPct, int32 Turn, int32 Year, int32 DisasterLevel, int32 CO2, int32 RiskMult)
{
	if (TextTreasury)
	{
		TextTreasury->SetText(FText::FromString(FString::Printf(TEXT("Bütçe: %s TL"), *FText::AsNumber(Budget).ToString())));
	}
	if (TextSatisfaction)
	{
		TextSatisfaction->SetText(FText::FromString(FString::Printf(TEXT("Şehir Durumu: %%%d"), StabilityPct)));
	}
	if (TextTurnYear)
	{
		TextTurnYear->SetText(FText::FromString(FString::Printf(TEXT("Tur: %d | Yıl: %d"), Turn, Year)));
	}
	if (TextDisasterLevel)
	{
		TextDisasterLevel->SetText(FText::FromString(FString::Printf(TEXT("Afet Seviyesi: %d"), DisasterLevel)));
	}
	if (TextClimateRisk)
	{
		TextClimateRisk->SetText(FText::FromString(FString::Printf(TEXT("CO2: %d | Risk: x%d"), CO2, RiskMult)));
	}
}

void UAftershockMainHUDWidget::SetupCleanHUDLayout()
{
	if (!WidgetTree) WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"));
	UCanvasPanel* Root = Cast<UCanvasPanel>(WidgetTree->RootWidget);
	if (!Root)
	{
		Root = WidgetTree->ConstructWidget<UCanvasPanel>();
		WidgetTree->RootWidget = Root;
	}
	Root->ClearChildren();
	Root->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	WorldInputRegions.Reset();
	TaxActionButtons.Reset();
	FramePacingButtons.Reset();
	CatalogCategoryButtons.Reset(); CatalogPreviousButton=nullptr; CatalogNextButton=nullptr;
	PresentedMapTool=MIN_int32; PresentedBuildingType=MIN_int32;

	const FLinearColor Ink(.94f,.94f,.89f);
	const FLinearColor Muted(.61f,.71f,.74f);
	const FLinearColor Gold(.96f,.62f,.25f);
	const FLinearColor Teal(.27f,.81f,.69f);
	const FLinearColor Surface(.014f,.028f,.040f,.97f);
	const FLinearColor Line(.11f,.19f,.22f);

	auto MakeText = [&](const FText& Text, int32 Size, FLinearColor Color) -> UTextBlock*
	{
		UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>();
		Label->SetText(Text);
		Label->SetColorAndOpacity(FSlateColor(Color));
		FSlateFontInfo Font=Label->GetFont(); Font.Size=Size; if(Size>=14)Font.TypefaceFontName=TEXT("Bold"); Label->SetFont(Font);
		Label->SetVisibility(ESlateVisibility::HitTestInvisible);
		return Label;
	};
	auto PlaceText = [&](TObjectPtr<UTextBlock>& Target, UVerticalBox* Parent, int32 Size, FLinearColor Color)
	{
		if (!Target) Target = MakeText(FText::GetEmpty(), Size, Color);
		Target->RemoveFromParent();
		Target->SetColorAndOpacity(FSlateColor(Color));
		FSlateFontInfo Font = Target->GetFont(); Font.Size = Size; Target->SetFont(Font);
		Target->SetAutoWrapText(true);
		if (UVerticalBoxSlot* LayoutSlot = Parent->AddChildToVerticalBox(Target)) LayoutSlot->SetPadding(FMargin(0, 6));
	};
	auto EnsureButton = [&](TObjectPtr<UButton>& Button, const FText& Label, FLinearColor Color)
	{
		if (!Button) Button = WidgetTree->ConstructWidget<UButton>();
		Button->RemoveFromParent();
		Button->SetVisibility(ESlateVisibility::Visible);
		Button->SetStyle(MakeAftershockButtonStyle(Button->GetStyle(),Color));
		Button->SetBackgroundColor(FLinearColor::White);
		Button->SetContent(MakeText(Label, 11, Ink));
	};
	auto RowItem = [](UHorizontalBox* Row, UWidget* Child, bool bFill = false)
	{
		UHorizontalBoxSlot* LayoutSlot = Row->AddChildToHorizontalBox(Child);
		LayoutSlot->SetPadding(FMargin(4, 3)); LayoutSlot->SetVerticalAlignment(VAlign_Center);
		if (bFill) LayoutSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	};
	auto MakeStrip = [&](bool bBottom, float Height) -> UVerticalBox*
	{
		UScaleBox* Fit = WidgetTree->ConstructWidget<UScaleBox>();
		Fit->SetStretch(EStretch::ScaleToFit);
		Fit->SetStretchDirection(EStretchDirection::DownOnly);
		UCanvasPanelSlot* FitSlot = Root->AddChildToCanvas(Fit);
		FitSlot->SetAnchors(FAnchors(0.0f, bBottom ? 1.0f : 0.0f, 1.0f, bBottom ? 1.0f : 0.0f));
		FitSlot->SetAlignment(FVector2D(0, bBottom ? 1 : 0));
		FitSlot->SetOffsets(FMargin(16, bBottom ? -16 : 12, 16, Height));
		FitSlot->SetZOrder(10);
		USizeBox* Width = WidgetTree->ConstructWidget<USizeBox>(); Width->SetWidthOverride(1120);
		UBorder* Panel = WidgetTree->ConstructWidget<UBorder>();
		Panel->SetBrush(FSlateRoundedBoxBrush(Surface, 6.f,Line,1.f));
		Panel->SetPadding(FMargin(16, 10));
		UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();
		Panel->SetContent(Stack); Width->SetContent(Panel); Fit->SetContent(Width);
		WorldInputRegions.Add(Panel);
		return Stack;
	};

	UVerticalBox* Header = MakeStrip(false, 112);
	UHorizontalBox* HeaderRow = WidgetTree->ConstructWidget<UHorizontalBox>(); Header->AddChildToVerticalBox(HeaderRow);
	RowItem(HeaderRow, MakeText(FText::FromString(TEXT("AFTERSHOCK")), 18, Gold));
	UTextBlock* Strap = MakeText(FText::FromString(TEXT("ŞEHİR YÖNETİMİ  /  KRİZ MERKEZİ")), 10, Muted); RowItem(HeaderRow, Strap, true);
	EnsureButton(BtnOpenQuiz, FText::FromString(TEXT("Afet farkındalığı")), FLinearColor(0.10f, 0.17f, 0.23f)); RowItem(HeaderRow, BtnOpenQuiz);
	EnsureButton(BtnPause, FText::FromString(TEXT("Menü  [Esc]")), FLinearColor(0.10f, 0.17f, 0.23f)); RowItem(HeaderRow, BtnPause);
	BtnPause->SetToolTipText(NSLOCTEXT("Aftershock","PauseActionHint","Şehri duraklat; kaydet, ayarları değiştir veya ana menüye dön. Kısayol: Esc."));
	BtnOpenQuiz->SetToolTipText(NSLOCTEXT("Aftershock","QuizActionHint","Afete hazırlık bilgini ölç. Hazırlık ve aktif afet sırasında yeni soru açılamaz."));
	UHorizontalBox* Metrics = WidgetTree->ConstructWidget<UHorizontalBox>(); Header->AddChildToVerticalBox(Metrics);
	auto Metric = [&](TObjectPtr<UTextBlock>& Text, FLinearColor Color)
	{
		if (!Text) Text = MakeText(FText::GetEmpty(), 12, Color);
		Text->RemoveFromParent(); Text->SetColorAndOpacity(FSlateColor(Color));
		FSlateFontInfo Font = Text->GetFont(); Font.Size = 12; Text->SetFont(Font);
		UBorder* MetricPlate=WidgetTree->ConstructWidget<UBorder>(); MetricPlate->SetBrush(FSlateRoundedBoxBrush(FLinearColor(.025f,.046f,.058f),3.f));
		MetricPlate->SetPadding(FMargin(10,7)); MetricPlate->SetContent(Text); RowItem(Metrics,MetricPlate,true);
	};
	Metric(TextTreasury, Gold); Metric(TextSatisfaction, Teal); Metric(TextTurnYear, Ink); Metric(TextDisasterLevel, Gold); Metric(TextClimateRisk, Muted);
	if (!TextIncomeExpense) TextIncomeExpense = MakeText(FText::GetEmpty(), 10, Muted);
	TextIncomeExpense->RemoveFromParent();
	UVerticalBoxSlot* FinanceSlot=Header->AddChildToVerticalBox(TextIncomeExpense); FinanceSlot->SetPadding(FMargin(6,4,0,0));
	TextIncomeExpense->SetToolTipText(NSLOCTEXT("Aftershock","FinanceForecastHint","Gelecek tur için tahmini vergi/gelir ve bakım/gider. Bekleyen ulaşım ücreti tahsil edilene kadar hazinenize eklenmez."));

	// Five categories and six tiles per page; all24 definitions remain reachable.
	UVerticalBox* Dock = MakeStrip(true, 242);
	UHorizontalBox* Categories = WidgetTree->ConstructWidget<UHorizontalBox>(); Dock->AddChildToVerticalBox(Categories);
	const auto CatalogAction = [this, &EnsureButton](UHorizontalBox* Row, const FText& Label, int32 Action, int32 Value)
	{
		UAftershockCatalogActionButton* Button = WidgetTree->ConstructWidget<UAftershockCatalogActionButton>();
		TObjectPtr<UButton> Styled = Button; EnsureButton(Styled, Label, FLinearColor(.065f,.13f,.19f));
		Button->Configure(this, Action, Value);
		UHorizontalBoxSlot* Layout = Row->AddChildToHorizontalBox(Button); Layout->SetPadding(FMargin(3,2)); Layout->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		return Button;
	};
	for (int32 Category = 0; Category < 5; ++Category) CatalogCategoryButtons.Add(CatalogAction(Categories, FAftershockBuildingCatalog::GetCategoryName(Category), 0, Category));
	CatalogPreviousButton=CatalogAction(Categories, FText::FromString(TEXT("‹")), 2, -1);
	CatalogPreviousButton->SetToolTipText(NSLOCTEXT("Aftershock","CatalogPreviousHint","Önceki bina sayfası."));
	TextCatalogPage = MakeText(FText::GetEmpty(), 10, Gold); RowItem(Categories, TextCatalogPage);
	CatalogNextButton=CatalogAction(Categories, FText::FromString(TEXT("›")), 2, 1);
	CatalogNextButton->SetToolTipText(NSLOCTEXT("Aftershock","CatalogNextHint","Sonraki bina sayfası."));
	CatalogItemsRow = WidgetTree->ConstructWidget<UHorizontalBox>(); Dock->AddChildToVerticalBox(CatalogItemsRow);
	RefreshCatalogPage();

	UHorizontalBox* ActionRow = WidgetTree->ConstructWidget<UHorizontalBox>(); Dock->AddChildToVerticalBox(ActionRow);
	auto Action = [&](TObjectPtr<UButton>& Button, const TCHAR* Label, const TCHAR* Tooltip)
	{
		EnsureButton(Button, FText::FromString(Label), FLinearColor(0.07f, 0.15f, 0.19f));
		if (UTextBlock* ActionLabel = Cast<UTextBlock>(Button->GetContent()))
		{
			ActionLabel->SetJustification(ETextJustify::Center);
			ActionLabel->SetAutoWrapText(true);
		}
		Button->SetClipping(EWidgetClipping::ClipToBounds);
		Button->SetToolTipText(FText::FromString(Tooltip)); RowItem(ActionRow, Button, true);
	};
	Action(BtnBuildRoad, TEXT("Yol / köprü\nTeklif al"), TEXT("İki tıkla güzergâh seçin. Kara yolu: yeni hücre başına 200 TL. Uygun karşı kıyılarda köprü teklifi tabliye ve rampanın tam maliyetini gösterir. Bina üzerinden geçilmez; mevcut yol yeniden ücretlenmez."));
	Action(BtnRemoveRoad, TEXT("Yol\nsil"), TEXT("Var olan yol hücresini kaldırır; ücretsizdir, inşaat bedeli iade edilmez."));
	Action(BtnInvestRiver, TEXT("Nehir\n2.500"), TEXT("Nehir yatağında bir hücre seçin; taşkın kapasitesini artırır. Maliyet 2.500 TL."));
	Action(BtnInvestDamping, TEXT("Akış\n2.000"), TEXT("Nehir hücresinde akışı yavaşlatıcı önlem. Maliyet 2.000 TL; en çok dört iyileştirme."));
	Action(BtnInvestDam, TEXT("Baraj\n12.000"), TEXT("Geçerli nehir hücresinde 180 oyun birimlik sonlu depolama. Dolduğunda fazla su aşağıya geçer. Maliyet12.000 TL; nehir taşkını içindir, tsunami önlemi değildir."));
	Action(BtnInvestDrainage, TEXT("Drenaj\n400"), TEXT("Kara hücresinde drenajı iyileştirir. Maliyet hücre başına 400 TL."));
	Action(BtnGroundSurvey, TEXT("Etüt\n500"), TEXT("Seçilen kara hücresinin senaryo zemin sınıfını öğren. 500 TL tek ödeme; gerçek parsel zemin araştırması değildir."));
	Action(BtnRetrofit, TEXT("Güçlendir\n1.000"), TEXT("Ayakta bir binayı sismik olarak güçlendirir. Maliyet 1.000 TL."));
	Action(BtnRepair, TEXT("Onar\nYapı seç"), TEXT("Hasarlı binayı seçin. Hasara göre 500 / 2.000 / 5.000 TL."));
	Action(BtnClearRubble, TEXT("Enkaz\n3.000"), TEXT("Çöken binanın parselini temizler. Maliyet 3.000 TL."));
	EnsureButton(BtnNextTurn, FText::FromString(TEXT("TURU\nBİTİR >")), FLinearColor(0.07f, 0.37f, 0.28f));
	BtnNextTurn->SetToolTipText(NSLOCTEXT("Aftershock","NextTurnHint","Vergi, bakım, üretim ve ihtiyaçları bir tur işle. Hazırlık/aktif afet bitmeden yeni tur başlatılamaz."));
	if (UTextBlock* TurnLabel = Cast<UTextBlock>(BtnNextTurn->GetContent())) TurnLabel->SetJustification(ETextJustify::Center);
	RowItem(ActionRow, BtnNextTurn, true);

	UHorizontalBox* ScenarioRow = WidgetTree->ConstructWidget<UHorizontalBox>(); Dock->AddChildToVerticalBox(ScenarioRow);
	RowItem(ScenarioRow, MakeText(FText::FromString(TEXT("AFET KONTROLÜ")), 9, Muted));
	EnsureButton(BtnTriggerEarthquake, FText::FromString(TEXT("Deprem")), FLinearColor(0.22f, 0.12f, 0.105f)); RowItem(ScenarioRow, BtnTriggerEarthquake);
	EnsureButton(BtnTriggerFlood, FText::FromString(TEXT("Sel")), FLinearColor(0.07f, 0.17f, 0.24f)); RowItem(ScenarioRow, BtnTriggerFlood);
	EnsureButton(BtnTriggerWind, FText::FromString(TEXT("Fırtına")), FLinearColor(0.14f, 0.17f, 0.23f)); RowItem(ScenarioRow, BtnTriggerWind);
	EnsureButton(BtnTriggerTsunami, FText::FromString(TEXT("Tsunami")), FLinearColor(.09f,.16f,.25f)); RowItem(ScenarioRow, BtnTriggerTsunami);
	EnsureButton(BtnTriggerFire, FText::FromString(TEXT("Yangın")), FLinearColor(.27f,.10f,.05f)); RowItem(ScenarioRow, BtnTriggerFire);
	EnsureButton(BtnTriggerVolcano, FText::FromString(TEXT("Volkan")), FLinearColor(.27f,.13f,.08f)); RowItem(ScenarioRow, BtnTriggerVolcano);
	EnsureButton(BtnTriggerDrought, FText::FromString(TEXT("Kuraklık")), FLinearColor(.23f,.19f,.08f)); RowItem(ScenarioRow, BtnTriggerDrought);
	EnsureButton(BtnTriggerAvalanche,FText::FromString(TEXT("Çığ")),FLinearColor(.11f,.20f,.25f));RowItem(ScenarioRow,BtnTriggerAvalanche);
	EnsureButton(BtnTriggerLandslide,FText::FromString(TEXT("Heyelan")),FLinearColor(.23f,.16f,.10f));RowItem(ScenarioRow,BtnTriggerLandslide);
	RowItem(ScenarioRow, MakeText(NSLOCTEXT("Aftershock","OverviewSettingsHint","Dünya kısayolu: Ayarlar → Kontroller  |  Esc: menü"), 9, Muted), true);

	GoalCardBorder = WidgetTree->ConstructWidget<UBorder>();
	GoalCardBorder->SetBrush(FSlateRoundedBoxBrush(Surface,6.f,Line,1.f)); GoalCardBorder->SetPadding(FMargin(16,14));
	GoalCardBorder->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	UCanvasPanelSlot* GoalSlot = Root->AddChildToCanvas(GoalCardBorder); GoalSlot->SetAnchors(FAnchors(1, 0)); GoalSlot->SetAlignment(FVector2D(1, 0)); GoalSlot->SetPosition(FVector2D(-18, 140)); GoalSlot->SetAutoSize(true); GoalSlot->SetZOrder(20);
	USizeBox* GoalWidth = WidgetTree->ConstructWidget<USizeBox>(); GoalWidth->SetWidthOverride(296); GoalCardBorder->SetContent(GoalWidth);
	UVerticalBox* GoalStack = WidgetTree->ConstructWidget<UVerticalBox>(); GoalWidth->SetContent(GoalStack);
	GoalStack->AddChildToVerticalBox(MakeText(FText::FromString(TEXT("HEDEFLER / ŞEHİR PLANI")), 11, Gold));
	PlaceText(TextGoalDescription, GoalStack, 11, Ink);
	PlaceText(TextGeographicLocation,GoalStack,10,Teal);TextGeographicLocation->SetWrapTextAt(296.f);
	TextGoalDescription->SetWrapTextAt(296.f);
	PlaceText(TextScenarioProgress, GoalStack, 10, Teal); TextScenarioProgress->SetWrapTextAt(296.f);
	PlaceText(TextScenarioClimate, GoalStack, 10, Muted); TextScenarioClimate->SetWrapTextAt(296.f);
	EnsureButton(BtnNextLevel, FText::FromString(TEXT("ÖNCE GÖREVLERİ TAMAMLA")), FLinearColor(0.07f, 0.30f, 0.24f)); GoalStack->AddChildToVerticalBox(BtnNextLevel);
	WorldInputRegions.Add(GoalCardBorder);
	TextGeodataAttribution=MakeText(FText::FromString(GetAftershockGeodataAttribution()),9,FLinearColor(.82f,.86f,.86f));
	GeodataAttributionPlate=WidgetTree->ConstructWidget<UBorder>();GeodataAttributionPlate->SetBrushColor(FLinearColor(.012f,.022f,.03f,.9f));GeodataAttributionPlate->SetPadding(FMargin(5,0));GeodataAttributionPlate->SetContent(TextGeodataAttribution);
	GeodataAttributionPlate->SetVisibility(ESlateVisibility::Visible);
	GeodataAttributionPlate->SetToolTipText(FText::FromString(GetAftershockGeodataLicenseNotice()));
	UCanvasPanelSlot* AttributionSlot=Root->AddChildToCanvas(GeodataAttributionPlate);AttributionSlot->SetAnchors(FAnchors(0,1,1,1));AttributionSlot->SetAlignment(FVector2D(0,1));AttributionSlot->SetOffsets(FMargin(18,-2,18,14));AttributionSlot->SetZOrder(11);
	LastGeographicDisplay.Reset();RefreshGeographicDisplay();

	CityLifeCard = WidgetTree->ConstructWidget<UBorder>(); CityLifeCard->SetBrush(FSlateRoundedBoxBrush(Surface,6.f,Line,1.f)); CityLifeCard->SetPadding(FMargin(16,14));
	UCanvasPanelSlot* LifePosition = Root->AddChildToCanvas(CityLifeCard); LifePosition->SetPosition(FVector2D(18,140)); LifePosition->SetAutoSize(true); LifePosition->SetZOrder(20);
	USizeBox* LifeWidth = WidgetTree->ConstructWidget<USizeBox>(); LifeWidth->SetWidthOverride(276); CityLifeCard->SetContent(LifeWidth);
	UVerticalBox* LifeStack = WidgetTree->ConstructWidget<UVerticalBox>(); LifeWidth->SetContent(LifeStack);
	LifeStack->AddChildToVerticalBox(MakeText(FText::FromString(TEXT("YAŞAYAN ŞEHİR / HİZMETLER")),11,Gold));
	PlaceText(TextCityLife,LifeStack,11,Ink); TextCityLife->SetWrapTextAt(276); TextCityLife->RemoveFromParent();
	USizeBox* LifeTextViewport=WidgetTree->ConstructWidget<USizeBox>(); LifeTextViewport->SetHeightOverride(164);
	UScrollBox* LifeScroll=WidgetTree->ConstructWidget<UScrollBox>(); LifeScroll->AddChild(TextCityLife); LifeTextViewport->SetContent(LifeScroll);
	UVerticalBoxSlot* LifeTextSlot=LifeStack->AddChildToVerticalBox(LifeTextViewport); LifeTextSlot->SetPadding(FMargin(0,8,0,4));
	PlaceText(TextTaxRates,LifeStack,10,Gold);
	UHorizontalBox* TaxActions = WidgetTree->ConstructWidget<UHorizontalBox>(); LifeStack->AddChildToVerticalBox(TaxActions);
	TaxActionButtons.Add(CatalogAction(TaxActions,FText::FromString(TEXT("Ev −")),3,-1));
	TaxActionButtons.Add(CatalogAction(TaxActions,FText::FromString(TEXT("Ev +")),3,1));
	TaxActionButtons.Add(CatalogAction(TaxActions,FText::FromString(TEXT("İş −")),4,-1));
	TaxActionButtons.Add(CatalogAction(TaxActions,FText::FromString(TEXT("İş +")),4,1));
	for (UAftershockCatalogActionButton* Button : TaxActionButtons)
		Button->SetToolTipText(NSLOCTEXT("Aftershock", "TaxStepTooltip", "Vergiyi 2 puan değiştirir (%0–30). Afet sürerken veya bir pencere açıkken şehir kararları bekler."));
	EnsureButton(BtnOpenLedger,NSLOCTEXT("Aftershock","OpenLedgerButton","KAYIP DEFTERİ  /  İSİMLERİ HATIRLA"),FLinearColor(.055f,.10f,.095f));LifeStack->AddChildToVerticalBox(BtnOpenLedger);
	BtnOpenLedger->SetToolTipText(NSLOCTEXT("Aftershock","OpenLedgerTooltip","Gerçek kayıp kayıtlarını ad, yaş, meslek ve afet anındaki yapı bilgileriyle incele. Bir kişinin anısına bir çiçek bırakabilirsin."));
	WorldInputRegions.Add(CityLifeCard);

	BuildingInspector = WidgetTree->ConstructWidget<UBorder>();
	BuildingInspector->SetBrush(FSlateRoundedBoxBrush(FLinearColor(Surface.R,Surface.G,Surface.B,1.f),6.f,Line,1.f)); BuildingInspector->SetPadding(FMargin(16,14));
	UCanvasPanelSlot* InspectorCanvasSlot = Root->AddChildToCanvas(BuildingInspector); InspectorCanvasSlot->SetAnchors(FAnchors(0, 0)); InspectorCanvasSlot->SetPosition(FVector2D(18,140)); InspectorCanvasSlot->SetAutoSize(true); InspectorCanvasSlot->SetZOrder(25);
	USizeBox* InspectorWidth = WidgetTree->ConstructWidget<USizeBox>(); InspectorWidth->SetWidthOverride(312); BuildingInspector->SetContent(InspectorWidth);
	UVerticalBox* InspectorStack = WidgetTree->ConstructWidget<UVerticalBox>(); InspectorWidth->SetContent(InspectorStack);
	UHorizontalBox* InspectorHeading = WidgetTree->ConstructWidget<UHorizontalBox>(); InspectorStack->AddChildToVerticalBox(InspectorHeading);
	RowItem(InspectorHeading, MakeText(FText::FromString(TEXT("YAPI DENETİMİ")), 12, Gold), true);
	EnsureButton(BtnCloseInspector, FText::FromString(TEXT("X")), FLinearColor(0.10f, 0.17f, 0.23f)); RowItem(InspectorHeading, BtnCloseInspector);
	PlaceText(TextBuildingDetails, InspectorStack, 11, Ink);
	TextBuildingDetails->SetWrapTextAt(312.f); TextBuildingDetails->RemoveFromParent();
	USizeBox* InspectorTextViewport = WidgetTree->ConstructWidget<USizeBox>(); InspectorTextViewport->SetHeightOverride(210);
	UScrollBox* InspectorScroll = WidgetTree->ConstructWidget<UScrollBox>(); InspectorScroll->AddChild(TextBuildingDetails);
	InspectorTextViewport->SetContent(InspectorScroll); InspectorStack->AddChildToVerticalBox(InspectorTextViewport);
	UHorizontalBox* InspectorActions = WidgetTree->ConstructWidget<UHorizontalBox>(); InspectorStack->AddChildToVerticalBox(InspectorActions);
	EnsureButton(BtnInspectorRetrofit, FText::FromString(TEXT("Güçlendir")), FLinearColor(0.07f, 0.28f, 0.23f)); RowItem(InspectorActions, BtnInspectorRetrofit, true);
	EnsureButton(BtnInspectorRepair, FText::FromString(TEXT("Onar")), FLinearColor(0.07f, 0.20f, 0.27f)); RowItem(InspectorActions, BtnInspectorRepair, true);
	EnsureButton(BtnInspectorRubble, FText::FromString(TEXT("Enkaz")), FLinearColor(0.25f, 0.13f, 0.08f)); RowItem(InspectorActions, BtnInspectorRubble, true);
	BuildingInspector->SetVisibility(ESlateVisibility::Collapsed); WorldInputRegions.Add(BuildingInspector);

	BuildHintBorder = WidgetTree->ConstructWidget<UBorder>(); BuildHintBorder->SetBrush(FSlateRoundedBoxBrush(Surface,6.f,Line,1.f)); BuildHintBorder->SetPadding(FMargin(18,12));
	UCanvasPanelSlot* HintSlot = Root->AddChildToCanvas(BuildHintBorder); HintSlot->SetAnchors(FAnchors(0.5f, 1)); HintSlot->SetAlignment(FVector2D(0.5f, 1)); HintSlot->SetPosition(FVector2D(0,-278)); HintSlot->SetSize(FVector2D(820,60)); HintSlot->SetAutoSize(true); HintSlot->SetZOrder(20);
	if (!TextBuildHint) TextBuildHint = MakeText(FText::GetEmpty(),12,Ink);
	TextBuildHint->RemoveFromParent(); TextBuildHint->SetJustification(ETextJustify::Center); TextBuildHint->SetAutoWrapText(true); TextBuildHint->SetWrapTextAt(724);
	USizeBox* HintWidth=WidgetTree->ConstructWidget<USizeBox>(); HintWidth->SetWidthOverride(760); HintWidth->SetContent(TextBuildHint); BuildHintBorder->SetContent(HintWidth); BuildHintBorder->SetVisibility(ESlateVisibility::Collapsed);

	// All modal surfaces exist in C++ as well as in Blueprint-only HUD variants.
	auto Modal = [&](TObjectPtr<UWidget>& Target, const TCHAR* Title, int32 Z) -> UVerticalBox*
	{
		UBorder* Shade = WidgetTree->ConstructWidget<UBorder>(); Shade->SetBrushColor(FLinearColor(.003f,.008f,.012f,.78f)); Shade->SetPadding(FMargin(24));
		UCanvasPanelSlot* ShadeSlot = Root->AddChildToCanvas(Shade); ShadeSlot->SetAnchors(FAnchors(0, 0, 1, 1)); ShadeSlot->SetOffsets(FMargin(0)); ShadeSlot->SetZOrder(Z);
		UScaleBox* ModalFit=WidgetTree->ConstructWidget<UScaleBox>(); ModalFit->SetStretch(EStretch::ScaleToFit); ModalFit->SetStretchDirection(EStretchDirection::DownOnly); Shade->SetContent(ModalFit);
		UBorder* Card = WidgetTree->ConstructWidget<UBorder>(); Card->SetBrush(FSlateRoundedBoxBrush(Surface,8.f,Line,1.f)); Card->SetPadding(FMargin(28,24)); ModalFit->SetContent(Card);
		USizeBox* Width = WidgetTree->ConstructWidget<USizeBox>(); Width->SetWidthOverride(560); Card->SetContent(Width);
		UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>(); Width->SetContent(Stack);
		Stack->AddChildToVerticalBox(MakeText(NSLOCTEXT("Aftershock","ModalOverline","AFTERSHOCK  /  KRİZ MERKEZİ"),9,Muted));
		UTextBlock* Heading=MakeText(FText::FromString(Title),22,Ink); Heading->SetAutoWrapText(true); Heading->SetWrapTextAt(560);
		UVerticalBoxSlot* HeadingSlot=Stack->AddChildToVerticalBox(Heading); HeadingSlot->SetPadding(FMargin(0,5,0,14));
		UBorder* Rule=WidgetTree->ConstructWidget<UBorder>(); Rule->SetBrushColor(Gold*.6f); Rule->SetPadding(FMargin(0));
		USizeBox* RuleSize=WidgetTree->ConstructWidget<USizeBox>(); RuleSize->SetHeightOverride(1); RuleSize->SetContent(Rule);
		UVerticalBoxSlot* RuleSlot=Stack->AddChildToVerticalBox(RuleSize); RuleSlot->SetPadding(FMargin(0,0,0,12));
		Target = Shade; Target->SetVisibility(ESlateVisibility::Collapsed); return Stack;
	};
	const auto ModalAction=[](UVerticalBox* Stack,UButton* Button)
	{
		UVerticalBoxSlot* ActionSlot=Stack->AddChildToVerticalBox(Button); ActionSlot->SetPadding(FMargin(0,5));
	};
	UVerticalBox* LedgerStack=Modal(PanelCasualtyLedger,TEXT("Her rakamın bir adı var."),125);
	CasualtyLedger=CreateWidget<UCasualtyLedgerPanel>(this);
	if(CasualtyLedger){LedgerStack->AddChildToVerticalBox(CasualtyLedger);CasualtyLedger->OnCloseRequested.AddUObject(this,&UAftershockMainHUDWidget::HandleCloseLedger);}
	UVerticalBox* Results = Modal(PanelDisasterResults, TEXT("Şehrin durum raporu"), 100);
	PlaceText(TextResultDisasterTitle, Results, 14, Ink); PlaceText(TextResultCasualties, Results, 16, Teal); PlaceText(TextResultDamage, Results, 13, Gold);
	PlaceText(TextResultPrevented, Results, 12, Teal); PlaceText(TextResultAid, Results, 12, Ink); PlaceText(TextResultBudgetChange, Results, 12, Ink); PlaceText(TextResultSatisfactionDelta, Results, 12, Ink);
	EnsureButton(BtnCloseResults, FText::FromString(TEXT("ŞEHRE DÖN  →")), FLinearColor(0.07f, 0.30f, 0.24f)); ModalAction(Results,BtnCloseResults);
	BtnCloseResults->SetToolTipText(NSLOCTEXT("Aftershock","CloseReportHint","Raporu kapat ve şehri incele. Bu işlem afet sonucunu veya ödeneği tekrar uygulamaz."));

	UVerticalBox* Quiz = Modal(PanelQuizModal, TEXT("AFET FARKINDALIĞI"), 110);
	PlaceText(TextQuizQuestion, Quiz, 15, Ink);
	TObjectPtr<UButton>* ChoiceButtons[] = {&BtnQuizChoice0, &BtnQuizChoice1, &BtnQuizChoice2, &BtnQuizChoice3};
	TObjectPtr<UTextBlock>* ChoiceLabels[] = {&TextQuizChoice0, &TextQuizChoice1, &TextQuizChoice2, &TextQuizChoice3};
	for (int32 Choice = 0; Choice < 4; ++Choice)
	{
		EnsureButton(*ChoiceButtons[Choice], FText::GetEmpty(), FLinearColor(0.08f, 0.14f, 0.21f));
		if (!*ChoiceLabels[Choice]) *ChoiceLabels[Choice] = MakeText(FText::GetEmpty(), 12, Ink);
		(*ChoiceLabels[Choice])->RemoveFromParent(); (*ChoiceLabels[Choice])->SetAutoWrapText(true);
		(*ChoiceButtons[Choice])->SetContent(*ChoiceLabels[Choice]);
		UVerticalBoxSlot* LayoutSlot = Quiz->AddChildToVerticalBox(*ChoiceButtons[Choice]); LayoutSlot->SetPadding(FMargin(0, 4));
	}
	PlaceText(TextQuizFeedback, Quiz, 12, Teal);
	EnsureButton(BtnCloseQuiz, FText::FromString(TEXT("ŞEHRE DÖN")), FLinearColor(0.07f, 0.30f, 0.24f)); ModalAction(Quiz,BtnCloseQuiz);

	UVerticalBox* Pause = Modal(PanelPauseMenu, TEXT("Şehir seni bekliyor."), 120);
	UVerticalBoxSlot* PauseDescription=Pause->AddChildToVerticalBox(MakeText(NSLOCTEXT("Aftershock","PauseDescription","OYUN DURAKLATILDI  ·  Kararlarına kaldığın yerden devam et."),11,Muted)); PauseDescription->SetPadding(FMargin(0,0,0,12));
	PlaceText(TextFramePacing,Pause,11,Ink);
	UHorizontalBox* FrameRow=WidgetTree->ConstructWidget<UHorizontalBox>();Pause->AddChildToVerticalBox(FrameRow);
	for(const int32 Fps:{60,90,120,0})
	{
		auto* Button=WidgetTree->ConstructWidget<UAftershockCatalogActionButton>();Button->Configure(this,5,Fps);
		Button->SetContent(MakeText(FText::FromString(Fps==0?FString(TEXT("Sınırsız")):FString::Printf(TEXT("%d FPS"),Fps)),11,Ink));
		Button->SetToolTipText(NSLOCTEXT("Aftershock","FramePacingChoiceHint","Kare hızını sınırla. 60 FPS daha düşük GPU yükü ve güç tüketimine yardımcı olur; VSync tercihin korunur."));
		FramePacingButtons.Add(Button);RowItem(FrameRow,Button,true);
	}
	RefreshFramePacingUI();
	EnsureButton(BtnResume, FText::FromString(TEXT("DEVAM ET  [Esc]")), FLinearColor(0.07f,0.30f,0.24f)); ModalAction(Pause,BtnResume);
	EnsureButton(BtnSettings, NSLOCTEXT("Aftershock","PauseSettings","AYARLAR"), FLinearColor(.026f,.054f,.068f)); ModalAction(Pause,BtnSettings);
	EnsureButton(BtnSaveGame,FText::FromString(TEXT("ŞEHRİ KAYDET")),FLinearColor(.035f,.10f,.12f)); ModalAction(Pause,BtnSaveGame);
	BtnSaveGame->SetToolTipText(NSLOCTEXT("Aftershock","SaveCityHint","Bu oturumu Manual yuvasına yaz. İşlem sonucu aşağıdaki kayıt kartında gösterilir."));
	TextSaveStatus=MakeText(NSLOCTEXT("Aftershock","SaveStatusHint","Tur ve afet sonrasında otomatik kayıt alınır. Manuel kayıt ayrı yuvada saklanır."),11,Muted); TextSaveStatus->SetAutoWrapText(true); TextSaveStatus->SetWrapTextAt(530);
	UBorder* SaveFeedbackCard=WidgetTree->ConstructWidget<UBorder>(); SaveFeedbackCard->SetBrush(FSlateRoundedBoxBrush(FLinearColor(.025f,.046f,.058f),4.f,Line,1.f)); SaveFeedbackCard->SetPadding(FMargin(12,10)); SaveFeedbackCard->SetContent(TextSaveStatus);
	UVerticalBoxSlot* SaveFeedbackSlot=Pause->AddChildToVerticalBox(SaveFeedbackCard); SaveFeedbackSlot->SetPadding(FMargin(0,5,0,12));
	EnsureButton(BtnMainMenu, FText::FromString(TEXT("ANA MENÜ  /  OTURUMU KORU")), FLinearColor(.026f,.054f,.068f)); ModalAction(Pause,BtnMainMenu);
	BtnMainMenu->SetToolTipText(NSLOCTEXT("Aftershock","ReturnOpeningHint","Açılış menüsüne dön. Devam Et açık oturuma geri getirir; yeni şehir başlatmadan önce mevcut şehir kaydedilir."));
	EnsureButton(BtnRestartLevel, FText::FromString(TEXT("ŞEHRİ YENİDEN BAŞLAT")), FLinearColor(.026f,.054f,.068f)); ModalAction(Pause,BtnRestartLevel);
	BtnRestartLevel->SetToolTipText(NSLOCTEXT("Aftershock","RestartCityHint","Mevcut bölümün başlangıcına dön. Kaydetmediğin şehir değişiklikleri bu yeniden başlangıca taşınmaz."));
	if(BtnQuitGame){BtnQuitGame->RemoveFromParent();BtnQuitGame->SetVisibility(ESlateVisibility::Collapsed);}

	UVerticalBox* GameOver = Modal(PanelGameOver, TEXT("GÖREVİNİZ SONA ERDİ"), 130);
	PlaceText(TextGameOverReason, GameOver, 14, Ink);
	EnsureButton(BtnRestartGameOver, FText::FromString(TEXT("YENİDEN DENE")), FLinearColor(0.07f,0.30f,0.24f)); ModalAction(GameOver,BtnRestartGameOver);
	if (PanelLevelTransition) PanelLevelTransition->SetVisibility(ESlateVisibility::Collapsed);

	UBorder* Warning = WidgetTree->ConstructWidget<UBorder>(); Warning->SetBrush(FSlateRoundedBoxBrush(FLinearColor(.13f,.055f,.025f,.98f),6.f,Gold,1.f)); Warning->SetPadding(FMargin(18,14));
	UCanvasPanelSlot* WarningSlot = Root->AddChildToCanvas(Warning); WarningSlot->SetAnchors(FAnchors(0.5f,0)); WarningSlot->SetAlignment(FVector2D(0.5f,0)); WarningSlot->SetOffsets(FMargin(0,140,620,188)); WarningSlot->SetZOrder(30);
	UVerticalBox* WarningStack = WidgetTree->ConstructWidget<UVerticalBox>(); Warning->SetContent(WarningStack);
	WarningStack->AddChildToVerticalBox(MakeText(NSLOCTEXT("Aftershock","WarningOverline","ERKEN UYARI  /  HAZIRLIK SÜRESİ"),9,Gold));
	PlaceText(TextDisasterWarning, WarningStack, 13, Gold); PlaceText(TextDisasterCountdown, WarningStack, 11, Ink);
	if (!BarDisasterCountdown) BarDisasterCountdown = WidgetTree->ConstructWidget<UProgressBar>(); BarDisasterCountdown->RemoveFromParent(); BarDisasterCountdown->SetFillColorAndOpacity(Gold); WarningStack->AddChildToVerticalBox(BarDisasterCountdown);
	EnsureButton(BtnSkipPreparation, FText::FromString(TEXT("HAZIRLIĞI BİTİR / AFETİ BAŞLAT")), FLinearColor(0.35f, 0.12f, 0.055f)); WarningStack->AddChildToVerticalBox(BtnSkipPreparation);
	EnsureButton(BtnFocusDisaster, FText::FromString(TEXT("AFET BÖLGESİNE ODAKLAN")), FLinearColor(0.12f, 0.28f, 0.44f)); WarningStack->AddChildToVerticalBox(BtnFocusDisaster);
	PanelDisasterWarning = Warning; Warning->SetVisibility(ESlateVisibility::Collapsed); WorldInputRegions.Add(Warning);
	// Catalog construction can precede the map-tool buttons on this path.
	PresentedMapTool=MIN_int32; PresentedBuildingType=MIN_int32;
}

void UAftershockMainHUDWidget::RefreshDirectorUI()
{
 const auto* D=GetWorld()?GetWorld()->GetSubsystem<UAftershockDisasterDirectorSubsystem>():nullptr;if(!D)return;
 const auto& State=D->GetState();const bool WasWarning=bIsWarningActive;
 const auto* Sim=GetWorld()->GetSubsystem<UDisasterSimulationSubsystem>();
 if(Sim&&Sim->IsPhysicalEventRunning())
 {
  bIsWarningActive=false;bDisasterInFlight=true;PendingDisasterType=State.PendingType;
  const auto& Episode=Sim->GetPhysicalEpisodeState();
  const bool Water=Episode.Type==EDisasterType::Flood||Episode.Type==EDisasterType::Tsunami;
  const double Total=Episode.SourceDurationSeconds+(Water?120.:0.);
  const float Progress=Total>0.?FMath::Clamp(Episode.PlaybackSeconds/Total,0.,1.):0.f;
  FText Failure;const bool Stopped=Sim->GetPhysicalRuntimeFailure(Failure);
  if(PanelDisasterWarning)PanelDisasterWarning->SetVisibility(ESlateVisibility::Visible);
  if(TextDisasterWarning)TextDisasterWarning->SetText(Stopped
   ?NSLOCTEXT("Aftershock","StoppedPhysicalDisasterBanner","FİZİKSEL HESAP DURDU — SONUÇ ÜRETİLMEDİ")
   :NSLOCTEXT("Aftershock","ActivePhysicalDisasterBanner","FİZİKSEL AFET DEVAM EDİYOR"));
  if(TextDisasterCountdown)
  {
   const TCHAR* Phase=Water?(Episode.SourceElapsedSeconds<Episode.SourceDurationSeconds?TEXT("Su girişi / yayılım"):TEXT("Çekilme / son değerlendirme")):TEXT("Zemin hareketi");
   TextDisasterCountdown->SetText(Stopped
    ?NSLOCTEXT("Aftershock","StoppedPhysicalDisasterSub","Ayrıntı için uyarının üzerine gelin. Kaydı yeniden açarak devam edin.")
    :FText::FromString(FString::Printf(TEXT("%s: %d%% | Hesaplanan %.0f / %.0f fizik saniyesi"),Phase,FMath::FloorToInt(Progress*100.f),Episode.PlaybackSeconds,Total)));
   TextDisasterCountdown->SetToolTipText(Stopped?Failure:FText::GetEmpty());
  }
  if(BtnSkipPreparation)BtnSkipPreparation->SetIsEnabled(false);
  if(BarDisasterCountdown)BarDisasterCountdown->SetPercent(Progress);
  RefreshAllUI();return;
 }
 if(BtnSkipPreparation)BtnSkipPreparation->SetIsEnabled(true);
 bIsWarningActive=State.Phase==EAftershockDisasterPhase::Warning;
 PendingDisasterType=State.PendingType;WarningTotalDuration=State.WarningDuration;WarningRemainingSeconds=State.RemainingSeconds;
 if(PanelDisasterWarning)PanelDisasterWarning->SetVisibility(bIsWarningActive?ESlateVisibility::Visible:ESlateVisibility::Collapsed);
 if(bIsWarningActive)
 {
  static const TCHAR* Names[]={TEXT("Deprem"),TEXT("Sel / taşkın"),TEXT("Fırtına"),TEXT("Yangın"),TEXT("Tsunami"),TEXT("Volkan"),TEXT("Kuraklık"),TEXT("Çığ"),TEXT("Heyelan")};
  if(TextDisasterWarning)TextDisasterWarning->SetText(FText::FromString(FString::Printf(TEXT("UYARI — %s yaklaşıyor. Şehri hazırlayın."),Names[FMath::Clamp(static_cast<int32>(State.PendingType),0,static_cast<int32>(UE_ARRAY_COUNT(Names))-1)])));
  if(TextDisasterCountdown)TextDisasterCountdown->SetText(FText::FromString(FString::Printf(TEXT("Hazırlık: %d sn"),FMath::CeilToInt(State.RemainingSeconds))));
  if(BarDisasterCountdown)BarDisasterCountdown->SetPercent(State.RemainingSeconds/FMath::Max(1.f,State.WarningDuration));
  if(!WasWarning)if(auto* GI=GetGameInstance())GI->GetSubsystem<UAftershockAudioSubsystem>()->PlayWarningSiren();
 }
 if(State.Phase==EAftershockDisasterPhase::Aftermath)
 {
  if(PanelDisasterWarning)
  {
   PanelDisasterWarning->SetVisibility(ESlateVisibility::Visible);
   if(TextDisasterWarning)TextDisasterWarning->SetText(NSLOCTEXT("Aftershock","ActiveDisasterBanner","DİKKAT: AFET GERÇEKLEŞİYOR"));
   if(TextDisasterCountdown)TextDisasterCountdown->SetText(NSLOCTEXT("Aftershock","ActiveDisasterSub","Afetin şehir üzerindeki etkileri değerlendiriliyor..."));
   if(BarDisasterCountdown)BarDisasterCountdown->SetPercent(1.f);
  }
  if(!bDisasterInFlight)
  {
   bDisasterInFlight=true;PendingVisualDisasterReport=State.LastReport;
   if(GetWorld()&&!GetWorld()->GetTimerManager().IsTimerActive(ResultsRevealTimer))
    GetWorld()->GetTimerManager().SetTimer(ResultsRevealTimer,FTimerDelegate::CreateWeakLambda(this,[this]()
    {bDisasterInFlight=false;if(PanelDisasterWarning)PanelDisasterWarning->SetVisibility(ESlateVisibility::Collapsed);RefreshAllUI();if(!GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>()->bIsGameOver)ShowDisasterResults(PendingVisualDisasterReport);}),FMath::Max(.05f,State.RemainingSeconds+.05f),false);
  }
 }
 else if(State.Phase==EAftershockDisasterPhase::Ready&&!GetWorld()->GetTimerManager().IsTimerActive(ResultsRevealTimer))
 {
  bDisasterInFlight=false;
  if(PanelDisasterWarning)PanelDisasterWarning->SetVisibility(ESlateVisibility::Collapsed);
 }
 RefreshAllUI();
}
void UAftershockMainHUDWidget::HandleSaveGameClicked()
{
	UGameInstance* GI=GetGameInstance();
	UAftershockSaveSubsystem* Saves=GI?GI->GetSubsystem<UAftershockSaveSubsystem>():nullptr;
	if(!Saves)
	{
		ShowSaveFeedback(false,NSLOCTEXT("Aftershock","SaveButtonServiceUnavailable","Kayıt hizmeti hazır değil; şehir kaydedilmedi."));
		return;
	}
	FText Error; if(!Saves->SaveNow(GetWorld(),TEXT("Manual"),Error)) ShowSaveFeedback(false,Error);
}
void UAftershockMainHUDWidget::ShowSaveFeedback(bool bSuccess,FText Message)
{
 if(TextSaveStatus){TextSaveStatus->SetText(Message);TextSaveStatus->SetColorAndOpacity(FSlateColor(bSuccess?FLinearColor(.4f,.9f,.7f):FLinearColor(1.f,.45f,.3f)));}
 if(!bSuccess)UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_SAVE_UI_ERROR %s"),*Message.ToString());
}
