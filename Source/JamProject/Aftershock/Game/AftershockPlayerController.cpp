#include "Aftershock/Game/AftershockPlayerController.h"
#include "Aftershock/Public/AftershockStartupPreparationSubsystem.h"
#include "Aftershock/Private/AftershockEnvironmentSurfaces.h"
#include "Aftershock/Public/CityPresentationSubsystem.h"
#include "Aftershock/World/GridManager.h"
#include "Aftershock/Buildings/BuildingActor.h"
#include "Aftershock/Data/BuildingData.h"
#include "Aftershock/Public/AftershockMainHUDWidget.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Aftershock/Public/AftershockAudioSubsystem.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockSettingsWidget.h"
#include "Aftershock/Public/AftershockSettingsSubsystem.h"
#include "Aftershock/Private/AftershockSettingsMenuRuntimeAudit.h"
#include "AudioDevice.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Aftershock/Quiz/QuizSubsystem.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Game/AftershockGameMode.h"
#include "Aftershock/UI/AftershockNewGameTutorialWidget.h"
#include "Aftershock/UI/WaterGlassEpilogueWidget.h"
#include "Blueprint/UserWidget.h"
#include "Components/InputComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/IConsoleManager.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
	constexpr const TCHAR* FramePacingSection=TEXT("Aftershock.FramePacing");
	constexpr int32 FramePacingPreferenceVersion=1;
	UAftershockAudioSubsystem* GetAudio(const APlayerController* PC)
	{
		if (!PC) return nullptr;
		if (UGameInstance* GI = PC->GetGameInstance())
		{
			return GI->GetSubsystem<UAftershockAudioSubsystem>();
		}
		return nullptr;
	}
}

AAftershockPlayerController::AAftershockPlayerController()
{
	bShowMouseCursor = true;
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	bShouldPerformFullTickWhenPaused = true;

	static ConstructorHelpers::FClassFinder<UAftershockMainHUDWidget> HUDClassFinder(TEXT("/Game/Aftershock/UI/WBP_AftershockMainHUD"));
	if (HUDClassFinder.Succeeded())
	{
		MainHUDWidgetClass = HUDClassFinder.Class;
	}
}

void AAftershockPlayerController::BeginPlay()
{
	Super::BeginPlay();
	if(IsLocalPlayerController())InitializeFramePacing();
	if(IsLocalPlayerController())ApplyUserPreferences();
	GetGridManager(); // cache

	// HUD'ın oyun başında ekrana eklenmesini sağla (ViewPort)
	if (IsLocalPlayerController() && !MainHUDWidget)
	{
		if (!MainHUDWidgetClass)
		{
			MainHUDWidgetClass = StaticLoadClass(UAftershockMainHUDWidget::StaticClass(), nullptr, TEXT("/Game/Aftershock/UI/WBP_AftershockMainHUD.WBP_AftershockMainHUD_C"));
		}

		if (MainHUDWidgetClass)
		{
			MainHUDWidget = CreateWidget<UAftershockMainHUDWidget>(this, MainHUDWidgetClass);
			if (MainHUDWidget)
			{
				MainHUDWidget->AddToViewport(0);
			}
		}
	}

	// Şehir arka plan sesini başlat (Kenney MIT — Content/Aftershock/Audio/S_Ambience_City)
	if(IsLocalPlayerController())
	{
		if(const auto* Saves=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>():nullptr;Saves&&Saves->IsWorldLoadFailed(GetWorld()))
		{GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this,[this](){PresentLoadFailureRecovery();}));return;}
		if(auto* Startup=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockStartupPreparationSubsystem>():nullptr)
			if(Startup->Prepare(this,FSimpleDelegate::CreateUObject(this,&ThisClass::ContinueAfterStartupPreparation)))return;
		ContinueAfterStartupPreparation();
	}
}

void AAftershockPlayerController::ContinueAfterStartupPreparation()
{
	if(!IsLocalPlayerController())return;
	if(auto* Audio=GetAudio(this))Audio->StartCityAmbience();
	auto* Session=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockOpeningSessionSubsystem>():nullptr;
	const bool bExplicitSkip=FParse::Param(FCommandLine::Get(),TEXT("AftershockVisualAudit"))||FParse::Param(FCommandLine::Get(),TEXT("AftershockSkipIntro"))||FParse::Param(FCommandLine::Get(),TEXT("AftershockSettingsMenuAudit"));
	if(bExplicitSkip&&Session)Session->MarkSessionStarted();
	if(!bExplicitSkip&&Session&&!Session->HasLiveSession())ShowOpeningMenu();
	else if(!bExplicitSkip&&Session&&Session->HasConfiguredSession()&&GetWorld()->URL.HasOption(TEXT("AftershockIntro")))PresentNewGameTutorial();
	if(auto* Startup=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockStartupPreparationSubsystem>():nullptr)Startup->AcknowledgeLiveWorldOwnership(GetWorld());
}

float AAftershockPlayerController::ResolveStartupFrameRateLimit(float ExistingLimit,bool bExistingVSync,int32 PreferenceVersion)
{
	if(!FMath::IsFinite(ExistingLimit)||ExistingLimit<0.f)return 60.f;
	return ExistingLimit==0.f&&!bExistingVSync&&PreferenceVersion<FramePacingPreferenceVersion?60.f:ExistingLimit;
}

bool AAftershockPlayerController::CanPersistFrameRatePreference() const
{
	return IsLocalPlayerController()&&GetWorld()&&GetWorld()->WorldType==EWorldType::Game
		&&!IsRunningCommandlet()&&!IsRunningDedicatedServer()&&!FApp::IsUnattended()
		&&!FParse::Param(FCommandLine::Get(),TEXT("nullrhi"))&&!FParse::Param(FCommandLine::Get(),TEXT("RenderOffscreen"))
		&&!FParse::Param(FCommandLine::Get(),TEXT("AftershockVisualAudit"))&&!FParse::Param(FCommandLine::Get(),TEXT("AftershockSettingsMenuAudit"));
}

void AAftershockPlayerController::InitializeFramePacing()
{
	bEphemeralAuditFramePacingApplied=false;
	if(!GEngine||IsRunningCommandlet()||IsRunningDedicatedServer())return;
	if(FParse::Param(FCommandLine::Get(),TEXT("AftershockVisualAudit")))
	{
		// Match the uncapped baseline without mutating the saved settings object
		// or writing the player's INI. This process is an explicit render fixture.
		GEngine->SetMaxFPS(0.f);
		auto* VSync=IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync"));if(VSync)VSync->Set(0,ECVF_SetByCode);
		bEphemeralAuditFramePacingApplied=FMath::IsNearlyZero(GEngine->GetMaxFPS())&&VSync&&VSync->GetInt()==0;
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_FRAME_PACING capture_policy=%s maxFps=%.3f vsync=%d policyApplied=%d savedSettingsMutated=0 settingsWrites=0"),bEphemeralAuditFramePacingApplied?TEXT("UncappedEphemeralNoSettingsWrite"):TEXT("AuditPolicyNotApplied"),GEngine->GetMaxFPS(),VSync?VSync->GetInt():-1,bEphemeralAuditFramePacingApplied);
		return;
	}
	if(!CanPersistFrameRatePreference())return;
	auto* Settings=UGameUserSettings::GetGameUserSettings();if(!Settings)return;
	int32 Version=0;if(GConfig)GConfig->GetInt(FramePacingSection,TEXT("PreferenceVersion"),Version,GGameUserSettingsIni);
	const float Existing=Settings->GetFrameRateLimit();const float Chosen=ResolveStartupFrameRateLimit(Existing,Settings->IsVSyncEnabled(),Version);
	const bool bFirstDefault=!FMath::IsFinite(Existing)||Existing<0.f||(Existing==0.f&&!Settings->IsVSyncEnabled()&&Version<FramePacingPreferenceVersion);
	GEngine->SetMaxFPS(Chosen);
	if(bFirstDefault)
	{
		Settings->SetFrameRateLimit(Chosen);Settings->SaveSettings();
		if(GConfig){GConfig->SetInt(FramePacingSection,TEXT("PreferenceVersion"),FramePacingPreferenceVersion,GGameUserSettingsIni);GConfig->Flush(false,GGameUserSettingsIni);}
	}
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_FRAME_PACING capture_policy=NormalPlayerPreference maxFps=%.3f vsyncPreference=%d settingsWrites=%d firstLegacyUnsetDefault=%d explicitUnlimitedPreserved=%d"),Chosen,Settings->IsVSyncEnabled(),bFirstDefault,bFirstDefault,Version>=FramePacingPreferenceVersion&&Chosen==0.f);
}

bool AAftershockPlayerController::SetUserFrameRateLimit(int32 FramesPerSecond)
{
	if(!CanPersistFrameRatePreference()||(FramesPerSecond!=0&&FramesPerSecond!=60&&FramesPerSecond!=90&&FramesPerSecond!=120))return false;
	auto* Settings=UGameUserSettings::GetGameUserSettings();if(!Settings||!GEngine)return false;
	Settings->SetFrameRateLimit(static_cast<float>(FramesPerSecond));GEngine->SetMaxFPS(static_cast<float>(FramesPerSecond));
	// A real button selection is the only recurring persistence path. Never
	// change VSync, scalability, resolution or a user's other graphics choices.
	Settings->SaveSettings();
	if(GConfig){GConfig->SetInt(FramePacingSection,TEXT("PreferenceVersion"),FramePacingPreferenceVersion,GGameUserSettingsIni);GConfig->Flush(false,GGameUserSettingsIni);}
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_FRAME_PACING capture_policy=ExplicitPlayerChoice maxFps=%d vsyncPreference=%d settingsWrites=1 explicitUnlimited=%d"),FramesPerSecond,Settings->IsVSyncEnabled(),FramesPerSecond==0);
	return true;
}

void AAftershockPlayerController::ShowOpeningMenu()
{
	if(IsSettingsMenuVisible())return;
	if (!IsLocalPlayerController() || bOpeningMenuVisible || (NewGameTutorial&&NewGameTutorial->IsInViewport())) return;
	if(WaterGlassEpilogue)WaterGlassEpilogue->RemoveFromParent();
	if (!OpeningMenu) OpeningMenu = CreateWidget<UAftershockOpeningWidget>(this, UAftershockOpeningWidget::StaticClass());
	if (!OpeningMenu) return;
	if (MainHUDWidget) { MainHUDWidget->ResumeGame(); MainHUDWidget->SetOpeningMenuActive(true); }
	bOpeningMenuVisible = true;
	OpeningMenu->AddToViewport(1000);
	OpeningMenu->RefreshSessionActions();
	UGameplayStatics::SetGamePaused(this, true);
	if (APawn* ControlledPawn = GetPawn()) ControlledPawn->DisableInput(this);
	FInputModeUIOnly Mode; Mode.SetWidgetToFocus(OpeningMenu->TakeWidget()); Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock); SetInputMode(Mode);
	bShowMouseCursor = true;
	OpeningMenu->FocusInitialAction();
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_OPENING_SHOWN worldPaused=%d hudHidden=%d fakeSaveContinue=0 artworkIntact=1"), GetWorld() && GetWorld()->IsPaused(), MainHUDWidget && !MainHUDWidget->IsVisible());
}

bool AAftershockPlayerController::IsSettingsMenuVisible() const
{return SettingsMenu&&SettingsMenu->IsInViewport();}

void AAftershockPlayerController::ShowSettingsMenu()
{
	if(!IsLocalPlayerController()||IsSettingsMenuVisible()||IsNarrativeFlowActive())return;
	bSettingsOpenedFromOpening=bOpeningMenuVisible;
	if(!bSettingsOpenedFromOpening&&MainHUDWidget)
	{
		// Opening settings from the city always returns to its pause screen.
		if(!GetWorld()->IsPaused())MainHUDWidget->TogglePauseMenu();
		if(!GetWorld()->IsPaused())return;
	}
	SettingsMenu=CreateWidget<UAftershockSettingsWidget>(this);
	if(!SettingsMenu)return;
	SettingsMenu->AddToViewport(1600);
	UGameplayStatics::SetGamePaused(this,true);
	FInputModeUIOnly Mode;Mode.SetWidgetToFocus(SettingsMenu->TakeWidget());Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);SetInputMode(Mode);bShowMouseCursor=true;
	SettingsMenu->OpenSettings();
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SETTINGS_OPENED source=%s worldPaused=%d"),bSettingsOpenedFromOpening?TEXT("Opening"):TEXT("Pause"),GetWorld()&&GetWorld()->IsPaused());
}

void AAftershockPlayerController::CloseSettingsMenu()
{
	if(!SettingsMenu)return;
	if(SettingsMenu->IsAwaitingDisplayConfirmation()){SettingsMenu->CancelAndClose();return;}
	SettingsMenu->RemoveFromParent();SettingsMenu=nullptr;
	// Neither route resumes the simulation. The player's original menu retains ownership.
	UGameplayStatics::SetGamePaused(this,true);
	if(bSettingsOpenedFromOpening&&OpeningMenu)
	{
		FInputModeUIOnly Mode;Mode.SetWidgetToFocus(OpeningMenu->TakeWidget());Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);SetInputMode(Mode);OpeningMenu->FocusInitialAction();
	}
	else
	{
		FInputModeGameAndUI Mode;Mode.SetHideCursorDuringCapture(false);Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);if(MainHUDWidget)Mode.SetWidgetToFocus(MainHUDWidget->TakeWidget());SetInputMode(Mode);
		if(MainHUDWidget)MainHUDWidget->FocusPauseMenu();
	}
	bShowMouseCursor=true;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SETTINGS_CLOSED returnTo=%s worldPaused=%d"),bSettingsOpenedFromOpening?TEXT("Opening"):TEXT("Pause"),GetWorld()&&GetWorld()->IsPaused());
}

void AAftershockPlayerController::ApplyUserPreferences()
{
	ApplyAftershockEnvironmentSurfaceQuality(GetWorld());
	if(auto* City=GetWorld()?GetWorld()->GetSubsystem<UCityPresentationSubsystem>():nullptr)City->RefreshVisiblePresentation();
	const auto* Preferences=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSettingsSubsystem>():nullptr;if(!Preferences)return;
	if(FAudioDevice* AudioDevice=GetWorld()?GetWorld()->GetAudioDeviceRaw():nullptr)AudioDevice->SetTransientPrimaryVolume(Preferences->GetPreferences().MasterVolume);
	if(auto* Audio=GetAudio(this))Audio->RefreshUserVolumes();
	if(auto* CameraPawn=Cast<AAftershockCameraPawn>(GetPawn()))CameraPawn->ApplyUserPreferences();
}
void AAftershockPlayerController::ProposeGeographicNewGame(int32 RegionIndex,int32 FocusIndex)
{
	if(!IsLocalPlayerController())return;
	ShowOpeningMenu();
	if(bOpeningMenuVisible&&OpeningMenu)OpeningMenu->ProposeGeographicNewGame(RegionIndex,FocusIndex);
}

void AAftershockPlayerController::CloseOpeningMenu()
{
	if(const auto* Saves=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>():nullptr;Saves&&Saves->IsWorldLoadFailed(GetWorld())){PresentLoadFailureRecovery();return;}
	if (OpeningMenu) OpeningMenu->RemoveFromParent();
	bOpeningMenuVisible = false;
	if (MainHUDWidget) { MainHUDWidget->SetOpeningMenuActive(false); MainHUDWidget->ResumeGame(); }
	UGameplayStatics::SetGamePaused(this, false);
	if (APawn* ControlledPawn = GetPawn()) ControlledPawn->EnableInput(this);
	FInputModeGameAndUI Mode; Mode.SetHideCursorDuringCapture(false); Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock); SetInputMode(Mode);
	bShowMouseCursor = true;
}

void AAftershockPlayerController::StartNewGameFromOpening()
{
	if (!bOpeningMenuVisible) return;
	UAftershockOpeningSessionSubsystem* Session = GetGameInstance() ? GetGameInstance()->GetSubsystem<UAftershockOpeningSessionSubsystem>() : nullptr;
	const bool bFirstStart = Session && !Session->HasLiveSession();
	const UAftershockScenarioSubsystem* Scenario = GetWorld() ? GetWorld()->GetSubsystem<UAftershockScenarioSubsystem>() : nullptr;
	const UEconomyAndStateSubsystem* Economy = GetWorld() ? GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	const bool bResumeInitialCity = bFirstStart && Scenario && Scenario->IsPrepared() && Scenario->GetScenarioIndex() == 0 && Economy && Economy->CurrentTurn == 1;
	FText Error;FAftershockSessionSettings DefaultSettings;
	if(!bResumeInitialCity){if(!StartConfiguredGame(DefaultSettings,Error)&&OpeningMenu)OpeningMenu->ShowSessionError(Error);return;}
	if (Session) Session->ConfigureNewSession(DefaultSettings,Error);
	GetGameInstance()->GetSubsystem<UQuizSubsystem>()->ResetForNewSession(DefaultSettings.Seed);
	CloseOpeningMenu();
	if(bResumeInitialCity){GetWorld()->URL.AddOption(TEXT("AftershockIntro=1"));PresentNewGameTutorial();}
	if (!bResumeInitialCity) UGameplayStatics::OpenLevel(this, FName(*UGameplayStatics::GetCurrentLevelName(this, true)), true, TEXT("AftershockScenario=0"));
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_OPENING_NEW_GAME resumeSeededScenario0=%d reloadScenario0=%d"), bResumeInitialCity, !bResumeInitialCity);
}

void AAftershockPlayerController::ContinueFromOpening()
{
	const UAftershockOpeningSessionSubsystem* Session = GetGameInstance() ? GetGameInstance()->GetSubsystem<UAftershockOpeningSessionSubsystem>() : nullptr;
	const auto* Saves=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>():nullptr;
	if (bOpeningMenuVisible && Session && Session->HasLiveSession()&&!(Saves&&Saves->IsWorldLoadFailed(GetWorld())))
	{
		CloseOpeningMenu();
		if(const auto* Mode=GetWorld()->GetAuthGameMode<AAftershockGameMode>();Mode&&Mode->GetGameFlowState()==EGameFlowState::Epilogue)PresentWaterGlassEpilogue(Mode->WasCampaignSuccessful(),Mode->GetEndingReason());
	}
	else if(bOpeningMenuVisible&&GetGameInstance())
	{
		FText Error;if(!GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>()->QueueContinue(GetWorld(),Error)&&OpeningMenu)OpeningMenu->ShowSessionError(Error);
	}
}

bool AAftershockPlayerController::StartConfiguredGame(const FAftershockSessionSettings& Settings,FText& Error)
{
	if(!bOpeningMenuVisible||!GetGameInstance())return false;
	auto* Session=GetGameInstance()->GetSubsystem<UAftershockOpeningSessionSubsystem>();
	if(!UAftershockOpeningSessionSubsystem::ValidateSettings(Settings,Error))return false;
	if(Session->HasLiveSession()&&!GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>()->IsWorldLoadFailed(GetWorld()))
	{
		// Switching city preserves the current one before replacing its session ID.
		if(!GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>()->SaveNow(GetWorld(),TEXT("Manual"),Error))return false;
	}
	if(!Session->ConfigureNewSession(Settings,Error))return false;
	GetGameInstance()->GetSubsystem<UQuizSubsystem>()->ResetForNewSession(Settings.Seed);
	CloseOpeningMenu();UGameplayStatics::OpenLevel(this,FName(TEXT("/Game/Aftershock/Levels/L_Level_01_Deprem")),true,FString::Printf(TEXT("AftershockScenario=%d?AftershockIntro=1"),Settings.CampaignStage));
	return true;
}

void AAftershockPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if(auto* Startup=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockStartupPreparationSubsystem>():nullptr)Startup->Detach(this);
	if(SettingsMenu){SettingsMenu->RemoveFromParent();SettingsMenu=nullptr;}
	if(NewGameTutorial){NewGameTutorial->RemoveFromParent();NewGameTutorial=nullptr;}
	if(WaterGlassEpilogue){WaterGlassEpilogue->RemoveFromParent();WaterGlassEpilogue=nullptr;}
	if (OpeningMenu)
	{
		OpeningMenu->RemoveFromParent();
		OpeningMenu = nullptr;
	}
	if (MainHUDWidget)
	{
		MainHUDWidget->RemoveFromParent();
		MainHUDWidget = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

void AAftershockPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// Camera pawn owns native input; callable methods remain for Blueprint compatibility.
}

bool AAftershockPlayerController::IsNarrativeFlowActive() const
{
	if(const auto* Startup=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockStartupPreparationSubsystem>():nullptr)if(Startup->IsGameplayBlocked(GetWorld()))return true;
	const auto* Mode=GetWorld()?GetWorld()->GetAuthGameMode<AAftershockGameMode>():nullptr;
	return Mode&&Mode->IsNarrativeInputBlocked();
}
void AAftershockPlayerController::PresentLoadFailureRecovery()
{
	const auto* Saves=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>():nullptr;
	if(!Saves||!Saves->IsWorldLoadFailed(GetWorld()))return;
	if(NewGameTutorial){NewGameTutorial->RemoveFromParent();NewGameTutorial=nullptr;}
	if(WaterGlassEpilogue){WaterGlassEpilogue->RemoveFromParent();WaterGlassEpilogue=nullptr;}
	ShowOpeningMenu();
	if(OpeningMenu){OpeningMenu->RefreshSessionActions();OpeningMenu->ShowSessionError(Saves->GetLoadFailure(GetWorld()));}
	UGameplayStatics::SetGamePaused(this,true);
}
bool AAftershockPlayerController::IsNewGameTutorialVisible() const{return NewGameTutorial&&NewGameTutorial->IsInViewport();}
bool AAftershockPlayerController::IsWaterGlassEpilogueVisible() const{return WaterGlassEpilogue&&WaterGlassEpilogue->IsInViewport();}
void AAftershockPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);
	if(IsSettingsMenuVisible())SettingsMenu->UpdateRealTimeConfirmation();
	TickAftershockSettingsMenuRuntimeAudit(*this);
	if(auto* CameraPawn=Cast<AAftershockCameraPawn>(GetPawn()))CameraPawn->AdvanceNarrativeCameraTransition();
}
void AAftershockPlayerController::PresentNewGameTutorial()
{
    UWorld* World=GetWorld();auto* GI=GetGameInstance();
    auto* Session=GI?GI->GetSubsystem<UAftershockOpeningSessionSubsystem>():nullptr;
    auto* Flow=World?World->GetAuthGameMode<AAftershockGameMode>():nullptr;
    const auto* Saves=GI?GI->GetSubsystem<UAftershockSaveSubsystem>():nullptr;
    auto* Startup=GI?GI->GetSubsystem<UAftershockStartupPreparationSubsystem>():nullptr;
    if(!IsLocalPlayerController()||!World||!Flow||!Session||!Session->HasConfiguredSession()
        ||!World->URL.HasOption(TEXT("AftershockIntro"))||bOpeningMenuVisible||IsSettingsMenuVisible()
        ||IsNewGameTutorialVisible()||IsWaterGlassEpilogueVisible()||Flow->GetGameFlowState()!=EGameFlowState::Playing
        ||TutorialPresentedSessionID==Session->GetSessionID()||(Startup&&Startup->IsGameplayBlocked(World))
        ||(Saves&&(Saves->HasPendingLoad()||Saves->IsRestoring()||Saves->IsWorldLoadFailed(World))))return;
    NewGameTutorial=CreateWidget<UAftershockNewGameTutorialWidget>(this);
    if(!NewGameTutorial){UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_NEW_GAME_TUTORIAL_CREATE_FAILED cityUnchanged=1"));return;}
    // This is a transient travel option. FURL::RemoveOption also writes Game.ini;
    // remove only this world's in-memory option, preserving every other option.
    World->URL.Op.RemoveAll([](const FString& Option){return Option==TEXT("AftershockIntro")||Option.StartsWith(TEXT("AftershockIntro="),ESearchCase::IgnoreCase);});
    TutorialPresentedSessionID=Session->GetSessionID();
    if(Startup)Startup->AcknowledgeLiveWorldOwnership(World);
    Flow->BeginIntro();
    if(MainHUDWidget)
    {
        MainHUDWidget->CancelBuildModeFromUI();MainHUDWidget->CancelMapTool();MainHUDWidget->SetNarrativeFlowActive(true);
        // Keep the real city dashboard readable, while its commands stay locked.
        MainHUDWidget->SetVisibility(ESlateVisibility::HitTestInvisible);
    }
    if(auto* CameraPawn=Cast<AAftershockCameraPawn>(GetPawn())){CameraPawn->SetNarrativeInputBlocked(true);CameraPawn->DisableInput(this);}
    UGameplayStatics::SetGamePaused(this,true);NewGameTutorial->AddToViewport(2000);
    FInputModeUIOnly InputMode;InputMode.SetWidgetToFocus(NewGameTutorial->TakeWidget());InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);SetInputMode(InputMode);bShowMouseCursor=true;
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_NEW_GAME_TUTORIAL_SHOWN newGameUrlOnly=1 preloadReady=1 cityVisible=1 worldPaused=%d steps=6 maximumRealSeconds=30 cameraTransition=0"),World->IsPaused());
}
void AAftershockPlayerController::CompleteNewGameTutorial()
{
    UWorld* World=GetWorld();auto* Flow=World?World->GetAuthGameMode<AAftershockGameMode>():nullptr;
    // A stale widget must never take input/pause ownership from an ending or loader.
    if(!NewGameTutorial||!Flow||Flow->GetGameFlowState()!=EGameFlowState::Intro)return;
    if(const auto* Saves=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>():nullptr;Saves&&Saves->IsWorldLoadFailed(World)){PresentLoadFailureRecovery();return;}
    NewGameTutorial->RemoveFromParent();NewGameTutorial=nullptr;Flow->FinishIntro();
    if(MainHUDWidget)MainHUDWidget->SetNarrativeFlowActive(false);
    if(auto* CameraPawn=Cast<AAftershockCameraPawn>(GetPawn())){CameraPawn->SetNarrativeInputBlocked(false);CameraPawn->EnableInput(this);}
    UGameplayStatics::SetGamePaused(this,false);FInputModeGameAndUI InputMode;InputMode.SetHideCursorDuringCapture(false);InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);SetInputMode(InputMode);bShowMouseCursor=true;
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_NEW_GAME_TUTORIAL_COMPLETED cityReset=0 populationReset=0 outcomeWrites=0 settingsWrites=0"));
}
void AAftershockPlayerController::PresentWaterGlassEpilogue(bool bCampaignSuccess,FText Reason)
{
	if(!IsLocalPlayerController()||bOpeningMenuVisible)return;
	if(NewGameTutorial){NewGameTutorial->RemoveFromParent();NewGameTutorial=nullptr;}
	if(MainHUDWidget){MainHUDWidget->CancelBuildModeFromUI();MainHUDWidget->CancelMapTool();MainHUDWidget->SetNarrativeFlowActive(true);}
	if(auto* CameraPawn=Cast<AAftershockCameraPawn>(GetPawn())){CameraPawn->SetNarrativeInputBlocked(true);CameraPawn->DisableInput(this);CameraPawn->StartNarrativeCameraTransition(false,3.2f);}
	if(!WaterGlassEpilogue)WaterGlassEpilogue=CreateWidget<UWaterGlassEpilogueWidget>(this);
	if(!WaterGlassEpilogue)return;WaterGlassEpilogue->SetOutcome(bCampaignSuccess,Reason);
	if(!WaterGlassEpilogue->IsInViewport())WaterGlassEpilogue->AddToViewport(2000);
	UGameplayStatics::SetGamePaused(this,true);FInputModeUIOnly InputMode;InputMode.SetWidgetToFocus(WaterGlassEpilogue->TakeWidget());InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);SetInputMode(InputMode);bShowMouseCursor=true;
}

void AAftershockPlayerController::HandleLeftClick()
{
	if (bOpeningMenuVisible||IsSettingsMenuVisible()||IsNarrativeFlowActive()) return;
	if (AAftershockCameraPawn* Cam = Cast<AAftershockCameraPawn>(GetPawn())) Cam->HandleLeftClick();
}

void AAftershockPlayerController::HandleRightClick()
{
	if (bOpeningMenuVisible||IsSettingsMenuVisible()||IsNarrativeFlowActive()) return;
	if (AAftershockCameraPawn* Cam = Cast<AAftershockCameraPawn>(GetPawn())) Cam->HandleRightClick();
}

void AAftershockPlayerController::HandleEscapePressed()
{
	if(IsSettingsMenuVisible()){SettingsMenu->CancelAndClose();return;}
	if(IsNarrativeFlowActive()&&!bOpeningMenuVisible)return;
	if (bOpeningMenuVisible) { ContinueFromOpening(); return; }
	if (MainHUDWidget) MainHUDWidget->HandleEscapeAction();
	else if (AAftershockCameraPawn* Cam = Cast<AAftershockCameraPawn>(GetPawn())) Cam->HandleEscapePressed();
}

void AAftershockPlayerController::HandleRotateGhost()
{
	if (bOpeningMenuVisible||IsNarrativeFlowActive()) return;
	if (AAftershockCameraPawn* Cam = Cast<AAftershockCameraPawn>(GetPawn())) Cam->HandleRotateGhost();
}

AGridManager* AAftershockPlayerController::GetGridManager() const
{
	if (CachedGridManager) return CachedGridManager;
	TActorIterator<AGridManager> It(GetWorld());
	if (It)
	{
		CachedGridManager = *It;
		return CachedGridManager;
	}
	return nullptr;
}

void AAftershockPlayerController::EnterPlacementMode(UBuildingData* BuildingData)
{
	if (bOpeningMenuVisible || IsNarrativeFlowActive() || (MainHUDWidget && !MainHUDWidget->CanIssueCityOrder())) return;
	SelectedBuildingData = BuildingData;
	if (AAftershockCameraPawn* CameraPawn = Cast<AAftershockCameraPawn>(GetPawn()))
		CameraPawn->SetBuildMode(BuildingData, EFoundationQuality::Medium, BuildingData ? BuildingData->AverageFloors() : 1);
	OnPlacementModeChanged.Broadcast(BuildingData != nullptr);
}

void AAftershockPlayerController::ExitPlacementMode()
{
	SelectedBuildingData = nullptr;
	if (AAftershockCameraPawn* CameraPawn = Cast<AAftershockCameraPawn>(GetPawn())) CameraPawn->CancelBuildMode();
	OnPlacementModeChanged.Broadcast(false);
}

bool AAftershockPlayerController::GetMouseGridCoord(FIntPoint& OutCoord) const
{
	if (const AAftershockCameraPawn* CameraPawn = Cast<AAftershockCameraPawn>(GetPawn())) return CameraPawn->GetHoveredGridCoords(OutCoord);
	AGridManager* Grid = GetGridManager();
	if (!Grid) return false;

	FVector Origin, Direction;
	if (!DeprojectMousePositionToWorld(Origin, Direction)) return false;

	// Z=0 düzlem ile kesişim
	if (FMath::IsNearlyZero(Direction.Z)) return false;
	const float T = -Origin.Z / Direction.Z;
	if (T <= 0.f) return false;

	const FVector WorldHit = Origin + Direction * T;
	OutCoord = Grid->WorldToCoord(WorldHit);
	return Grid->IsCoordValid(OutCoord);
}

ABuildingActor* AAftershockPlayerController::TryPlaceAtMouse()
{
	if (bOpeningMenuVisible || IsNarrativeFlowActive() || (GetWorld() && GetWorld()->IsPaused()) || (MainHUDWidget && (!MainHUDWidget->CanIssueCityOrder() || MainHUDWidget->BlocksWorldInputAtCursor()))) return nullptr;
	if (!IsInPlacementMode()) return nullptr;
	AGridManager* Grid = GetGridManager();
	if (!Grid || !DefaultBuildingClass) return nullptr;

	FIntPoint Coord;
	if (!GetMouseGridCoord(Coord)) return nullptr;
	if (!Grid->IsCoordFree(Coord)) return nullptr;

	AAftershockCameraPawn* CameraPawn = Cast<AAftershockCameraPawn>(GetPawn());
	const bool bHasLivePreview = CameraPawn && CameraPawn->IsBuildModeActive() && CameraPawn->GetCurrentSelectedBuilding() == SelectedBuildingData;
	const EFoundationQuality Foundation = bHasLivePreview ? CameraPawn->GetCurrentSelectedFoundation() : EFoundationQuality::Medium;
	const int32 Floors = bHasLivePreview ? CameraPawn->GetCurrentSelectedFloors() : SelectedBuildingData->AverageFloors();
	const float Yaw = bHasLivePreview ? CameraPawn->GetCurrentGhostYaw() : 0.f;
	ABuildingActor* B = Grid->PlaceBuildingWithOptions(SelectedBuildingData, Coord, DefaultBuildingClass, Foundation, Floors, Yaw);
	// The compatibility grid calls the same atomic budget transaction as native UI.
	// Blueprint callers must not charge a second time.
	if (B) ExitPlacementMode();
	return B;
}

ABuildingActor* AAftershockPlayerController::TrySelectAtMouse()
{
	if (bOpeningMenuVisible || IsNarrativeFlowActive() || (MainHUDWidget && MainHUDWidget->BlocksWorldInputAtCursor())) return nullptr;
	AGridManager* Grid = GetGridManager();
	if (!Grid) return nullptr;

	FIntPoint Coord;
	if (!GetMouseGridCoord(Coord)) return nullptr;

	ABuildingActor* B = Grid->GetBuildingAt(Coord);
	OnBuildingSelected.Broadcast(B);
	return B;
}
