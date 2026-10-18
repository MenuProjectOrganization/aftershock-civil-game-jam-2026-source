#include "AftershockSettingsAuditCommandlet.h"

#if WITH_EDITOR
#include "Aftershock/Public/AftershockSettingsSubsystem.h"

#include "Aftershock/Public/AftershockSettingsWidget.h"
#include "Components/VerticalBox.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeExit.h"

#include "Aftershock/Game/AftershockPlayerController.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>
#endif

UAftershockSettingsAuditCommandlet::UAftershockSettingsAuditCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;ShowErrorCount=true;}

int32 UAftershockSettingsAuditCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	int32 Passed=0,Failed=0;TArray<TSharedPtr<FJsonValue>> Checks;
	const auto Check=[&](const TCHAR* Name,bool bPass)
	{
		if(bPass)++Passed;else ++Failed;
		auto Entry=MakeShared<FJsonObject>();Entry->SetStringField(TEXT("name"),Name);Entry->SetBoolField(TEXT("passed"),bPass);Checks.Add(MakeShared<FJsonValueObject>(Entry));
		UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SETTINGS_AUDIT %s %s"),bPass?TEXT("PASS"):TEXT("FAIL"),Name);
	};
	const FAftershockUserPreferences Defaults;FText Error;
	Check(TEXT("default_preferences_are_valid"),UAftershockSettingsSubsystem::ValidatePreferences(Defaults,Error));
	Check(TEXT("all_11_actual_camera_actions_have_keys"),Defaults.Keys.Num()==static_cast<int32>(EAftershockUserAction::Count)&&Defaults.Keys.Num()==11);
	TSet<FKey> Unique;for(const auto& Key:Defaults.Keys)Unique.Add(Key);Check(TEXT("default_keys_are_unique"),Unique.Num()==Defaults.Keys.Num());
	Check(TEXT("commandlet_cannot_write_player_preferences"),!UAftershockSettingsSubsystem::CanWriteUserSettings());
	Check(TEXT("escape_is_reserved"),!UAftershockSettingsSubsystem::ValidateKey(Defaults.Keys,0,EKeys::Escape,Error));
	Check(TEXT("left_click_is_reserved"),!UAftershockSettingsSubsystem::ValidateKey(Defaults.Keys,0,EKeys::LeftMouseButton,Error));
	Check(TEXT("right_click_is_reserved"),!UAftershockSettingsSubsystem::ValidateKey(Defaults.Keys,0,EKeys::RightMouseButton,Error));
	Check(TEXT("tab_navigation_is_reserved"),!UAftershockSettingsSubsystem::ValidateKey(Defaults.Keys,0,EKeys::Tab,Error));
	Check(TEXT("enter_activation_is_reserved"),!UAftershockSettingsSubsystem::ValidateKey(Defaults.Keys,0,EKeys::Enter,Error));
	Check(TEXT("space_activation_is_reserved"),!UAftershockSettingsSubsystem::ValidateKey(Defaults.Keys,0,EKeys::SpaceBar,Error));
	Check(TEXT("console_key_is_reserved"),!UAftershockSettingsSubsystem::ValidateKey(Defaults.Keys,0,EKeys::Tilde,Error));
	Check(TEXT("fullscreen_key_is_reserved"),!UAftershockSettingsSubsystem::ValidateKey(Defaults.Keys,0,EKeys::F11,Error));
	Check(TEXT("modifier_alone_is_rejected"),!UAftershockSettingsSubsystem::ValidateKey(Defaults.Keys,0,EKeys::LeftControl,Error));
	Check(TEXT("unsupported_gamepad_is_rejected"),!UAftershockSettingsSubsystem::ValidateKey(Defaults.Keys,0,EKeys::Gamepad_FaceButton_Bottom,Error));
	Check(TEXT("mouse_axis_is_rejected"),!UAftershockSettingsSubsystem::ValidateKey(Defaults.Keys,0,EKeys::MouseWheelAxis,Error));
	Check(TEXT("duplicate_binding_is_rejected"),!UAftershockSettingsSubsystem::ValidateKey(Defaults.Keys,0,EKeys::Q,Error));
	Check(TEXT("unchanged_binding_is_accepted"),UAftershockSettingsSubsystem::ValidateKey(Defaults.Keys,0,EKeys::W,Error));
	Check(TEXT("unused_keyboard_binding_is_accepted"),UAftershockSettingsSubsystem::ValidateKey(Defaults.Keys,0,EKeys::I,Error));
	Check(TEXT("invalid_action_index_is_rejected"),!UAftershockSettingsSubsystem::ValidateKey(Defaults.Keys,99,EKeys::I,Error));
	TStrongObjectPtr<UGameInstance> GI(NewObject<UGameInstance>());
	TStrongObjectPtr<UAftershockSettingsSubsystem> Preferences(NewObject<UAftershockSettingsSubsystem>(GI.Get()));
	FAftershockUserPreferences Changed=Defaults;Changed.Keys[0]=EKeys::I;Changed.MasterVolume=.25f;Changed.bReduceMotion=true;Changed.bInvertWheel=true;
	Check(TEXT("transient_preferences_apply_through_real_api"),Preferences->SetPreferences(Changed,false,Error));
	Check(TEXT("camera_lookup_reads_new_binding"),Preferences->GetKey(EAftershockUserAction::PanForward)==EKeys::I);
	Check(TEXT("sound_and_accessibility_preferences_are_retained"),Preferences->GetPreferences().MasterVolume==.25f&&Preferences->GetPreferences().bReduceMotion&&Preferences->GetPreferences().bInvertWheel);
	FAftershockUserPreferences Invalid=Changed;Invalid.MasterVolume=std::numeric_limits<float>::quiet_NaN();
	Check(TEXT("nan_volume_is_rejected_atomically"),!Preferences->SetPreferences(Invalid,false,Error)&&Preferences->GetPreferences().MasterVolume==.25f);
	Invalid=Changed;Invalid.UIVolume=1.1f;Check(TEXT("over_range_volume_is_rejected"),!Preferences->SetPreferences(Invalid,false,Error));
	Invalid=Changed;Invalid.PanSensitivity=0.f;Check(TEXT("zero_camera_sensitivity_is_rejected"),!Preferences->SetPreferences(Invalid,false,Error));
	Invalid=Changed;Invalid.ZoomSensitivity=3.01f;Check(TEXT("over_range_zoom_sensitivity_is_rejected"),!Preferences->SetPreferences(Invalid,false,Error));
	Invalid=Changed;Invalid.Keys.Pop();Check(TEXT("missing_action_binding_is_rejected"),!Preferences->SetPreferences(Invalid,false,Error));
	Check(TEXT("persist_rejects_commandlet_and_preserves_state"),!Preferences->SetPreferences(Defaults,true,Error)&&Preferences->GetKey(EAftershockUserAction::PanForward)==EKeys::I);
	Check(TEXT("legacy_unset_frame_cap_migrates_to_60"),AAftershockPlayerController::ResolveStartupFrameRateLimit(0.f,false,0)==60.f);
	Check(TEXT("explicit_unlimited_is_preserved"),AAftershockPlayerController::ResolveStartupFrameRateLimit(0.f,false,1)==0.f);
	Check(TEXT("existing_custom_frame_cap_is_preserved"),AAftershockPlayerController::ResolveStartupFrameRateLimit(75.f,false,1)==75.f);

    // Native UMG tree and actual combo delegates only. No TakeWidget, platform
    // window, ApplyNonResolutionSettings, ApplyResolutionSettings or SaveSettings.
    // UE5.8 UComboBoxString::SetSelectedIndex broadcasts OnSelectionChanged even
    // before Slate construction (ComboBoxString.cpp226-245).
    const auto ResolvePhysicalIni=[]()
    {
        const FConfigBranch* Branch=GConfig?GConfig->FindBranchWithNoReload(NAME_None,GGameUserSettingsIni):nullptr;
        return Branch&&Branch->IniName==FName(TEXT("GameUserSettings"))?Branch->IniPath:FString();
    };
    const FString PhysicalIniBefore=ResolvePhysicalIni();const bool IniResolvedBefore=!PhysicalIniBefore.IsEmpty();
    TArray<uint8> IniBefore;const bool IniExisted=IniResolvedBefore&&IFileManager::Get().FileExists(*PhysicalIniBefore);
    const bool IniReadBefore=IniResolvedBefore&&(!IniExisted||FFileHelper::LoadFileToArray(IniBefore,*PhysicalIniBefore));
    UGameUserSettings* User=UGameUserSettings::GetGameUserSettings();
    Check(TEXT("graphics_audit_has_existing_engine_settings_without_creating_or_loading_an_ini"),GEngine&&User);
    if(GEngine&&User)
    {
        const Scalability::FQualityLevels BeforeQuality=User->ScalabilityQuality;
        const FIntPoint BeforeResolution=User->GetScreenResolution();const EWindowMode::Type BeforeMode=User->GetFullscreenMode();
        const float BeforeCap=User->GetFrameRateLimit();const bool BeforeVSync=User->IsVSyncEnabled();
        const IConsoleVariable* Reflection=IConsoleManager::Get().FindConsoleVariable(TEXT("r.ReflectionMethod"));
        const IConsoleVariable* GIProfile=IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicGlobalIlluminationMethod"));
        const IConsoleVariable* SSR=IConsoleManager::Get().FindConsoleVariable(TEXT("r.SSR.Quality"));
        const int32 BeforeReflection=Reflection?Reflection->GetInt():INDEX_NONE,BeforeGI=GIProfile?GIProfile->GetInt():INDEX_NONE,BeforeSSR=SSR?SSR->GetInt():INDEX_NONE;
        TStrongObjectPtr<UGameInstance> GraphicsGI(NewObject<UGameInstance>(GEngine));
        GraphicsGI->InitializeStandalone(TEXT("AftershockSettingsGraphicsFixture"));
        UWorld* GraphicsWorld=GraphicsGI->GetWorld();
        ON_SCOPE_EXIT
        {
            GraphicsGI->Shutdown();
            if(GraphicsWorld){GraphicsWorld->DestroyWorld(false);GEngine->DestroyWorldContext(GraphicsWorld);GraphicsWorld->RemoveFromRoot();}
        };
        TStrongObjectPtr<UAftershockSettingsWidget> Widget(GraphicsWorld?CreateWidget<UAftershockSettingsWidget>(GraphicsGI.Get()):nullptr);
        Check(TEXT("settings_native_widget_created_in_isolated_game_instance"),Widget.IsValid());
        if(Widget.IsValid())
        {
            Widget->BuildLayout();
            auto* Preset=Widget->Choices.FindRef(6).Get();
            bool BoundGroups=true;
            for(int32 I=0;I<10;++I)
            {
                const auto* Choice=Widget->Choices.FindRef(10+I).Get();
                BoundGroups&=Choice&&Choice->GetOptionCount()==5&&Choice->OnSelectionChanged.IsBound();
            }
            Check(TEXT("general_quality_id6_has_five_presets_and_custom_with_real_callback"),Preset&&Preset->GetOptionCount()==6&&Preset->OnSelectionChanged.IsBound());
            Check(TEXT("all_ten_quality_groups_remain_independent_bound_five_level_choices"),BoundGroups);
            Check(TEXT("settings_preserves_all_five_tabs"),Widget->Pages.Num()==5&&Widget->Tabs.Num()==5);
            for(int32 Level=0;Level<=4&&Preset&&BoundGroups;++Level)
            {
                Preset->SetSelectedIndex(Level);bool Same=true;
                for(int32 I=0;I<10;++I)Same&=Widget->Choices.FindRef(10+I)->GetSelectedIndex()==Level;
                const FString Name=FString::Printf(TEXT("general_quality_real_callback_stages_all_ten_groups_level_%d"),Level);
                Check(*Name,Same&&Preset->GetSelectedIndex()==Level);
            }
            if(Preset&&BoundGroups)
            {
                Widget->Choices.FindRef(17)->SetSelectedIndex(2);
                bool UntouchedOthers=true;for(int32 I=0;I<10;++I)if(I!=7)UntouchedOthers&=Widget->Choices.FindRef(10+I)->GetSelectedIndex()==4;
                Check(TEXT("individual_group_change_selects_custom_and_preserves_other_nine"),Preset->GetSelectedIndex()==5&&Widget->Choices.FindRef(17)->GetSelectedIndex()==2&&UntouchedOthers);
                TArray<int32> Staged;for(int32 I=0;I<10;++I)Staged.Add(Widget->Choices.FindRef(10+I)->GetSelectedIndex());
                Preset->OnSelectionChanged.Broadcast(Preset->GetSelectedOption(),ESelectInfo::Direct);
                bool CustomPreserves=true;for(int32 I=0;I<10;++I)CustomPreserves&=Widget->Choices.FindRef(10+I)->GetSelectedIndex()==Staged[I];
                Check(TEXT("custom_callback_does_not_overwrite_staged_groups"),CustomPreserves);
                Widget->bDisplayConfirmation=true;Preset->SetSelectedIndex(0);
                bool ModalPreserves=true;for(int32 I=0;I<10;++I)ModalPreserves&=Widget->Choices.FindRef(10+I)->GetSelectedIndex()==Staged[I];
                Widget->bDisplayConfirmation=false;Widget->RefreshGraphicsPresetChoice();
                Check(TEXT("display_confirmation_blocks_preset_group_mutation"),ModalPreserves&&Preset->GetSelectedIndex()==5);
            }
            bool TabsWork=Widget->Pages.Num()==5;
            for(int32 Tab=0;Tab<Widget->Pages.Num();++Tab)
            {
                Widget->HandleAction(Tab);
                for(int32 I=0;I<Widget->Pages.Num();++I)TabsWork&=Widget->Pages[I]->GetVisibility()==(I==Tab?ESlateVisibility::Visible:ESlateVisibility::Collapsed);
            }
            Check(TEXT("five_real_tab_handlers_select_only_the_matching_page"),TabsWork);
            const auto* ReflectionChoice=Widget->Choices.FindRef(13).Get();
            const auto* GIChoice=Widget->Choices.FindRef(12).Get();
            Check(TEXT("reflection_choice_capability_depends_on_renderer_method_not_current_ssr_quality"),ReflectionChoice&&Reflection&&ReflectionChoice->GetIsEnabled()==(BeforeReflection!=0));
            Check(TEXT("disabled_gi_profile_does_not_claim_an_active_gi_method"),GIChoice&&GIProfile&&GIChoice->GetIsEnabled()==(BeforeGI!=0));
            Widget->CaptureSnapshot();
            Check(TEXT("rollback_snapshot_captures_all_ten_groups_resolution_mode_cap_and_vsync"),Widget->PreviousQuality==BeforeQuality&&Widget->PreviousResolution==BeforeResolution&&Widget->PreviousWindowMode==BeforeMode&&Widget->PreviousFrameLimit==BeforeCap&&Widget->bPreviousVSync==BeforeVSync);
            Widget->HandleAction(100);
            Check(TEXT("actual_apply_handler_rejects_commandlet_before_settings_or_confirmation_mutation"),Widget->Feedback&&!Widget->Feedback->GetText().IsEmpty()&&!Widget->bDisplayConfirmation&&User->ScalabilityQuality==BeforeQuality&&User->GetScreenResolution()==BeforeResolution&&User->GetFullscreenMode()==BeforeMode&&User->GetFrameRateLimit()==BeforeCap&&User->IsVSyncEnabled()==BeforeVSync);
            Check(TEXT("preset_custom_tab_and_rejected_apply_leave_renderer_methods_and_ssr_unchanged"),Reflection&&GIProfile&&SSR&&Reflection->GetInt()==BeforeReflection&&GIProfile->GetInt()==BeforeGI&&SSR->GetInt()==BeforeSSR);
            Widget->ReleaseSlateResources(true);
        }
    }
    const FString PhysicalIniAfter=ResolvePhysicalIni();const bool IniResolvedAfter=!PhysicalIniAfter.IsEmpty();
    TArray<uint8> IniAfter;const bool IniExistsAfter=IniResolvedAfter&&IFileManager::Get().FileExists(*PhysicalIniAfter);
    const bool IniReadAfter=IniResolvedAfter&&(!IniExistsAfter||FFileHelper::LoadFileToArray(IniAfter,*PhysicalIniAfter));
    const bool SamePhysicalIni=IniResolvedBefore&&IniResolvedAfter&&FPaths::IsSamePath(PhysicalIniBefore,PhysicalIniAfter);
    Check(TEXT("graphics_tree_and_real_callbacks_preserve_user_ini_existence_and_exact_bytes"),SamePhysicalIni&&IniReadBefore&&IniReadAfter&&IniExistsAfter==IniExisted&&IniBefore==IniAfter);

	auto Report=MakeShared<FJsonObject>();Report->SetStringField(TEXT("generated_at_utc"),FDateTime::UtcNow().ToIso8601());Report->SetStringField(TEXT("scope"),TEXT("Native preferences plus actual native UMG tree/combo/tab callbacks and commandlet Apply rejection. No window, mouse hit testing, real Apply/display rollback, GPU or audible output claim; loaded physical config bytes compared during the fixture; parent process must separately verify original user bytes after engine exit."));Report->SetNumberField(TEXT("passed"),Passed);Report->SetNumberField(TEXT("failed"),Failed);Report->SetArrayField(TEXT("checks"),Checks);
    Report->SetBoolField(TEXT("physical_ini_resolved_before"),IniResolvedBefore);Report->SetBoolField(TEXT("physical_ini_resolved_after"),IniResolvedAfter);
    Report->SetBoolField(TEXT("physical_ini_destination_same"),SamePhysicalIni);Report->SetStringField(TEXT("physical_ini_resolution_source"),TEXT("LoadedGConfigBranch.IniPath"));
	FString Json;const auto Writer=TJsonWriterFactory<>::Create(&Json);FJsonSerializer::Serialize(Report,Writer);const FString Output=FPaths::ProjectSavedDir()/TEXT("PresentationValidation/settings_audit.json");IFileManager::Get().MakeDirectory(*FPaths::GetPath(Output),true);
	if(!FFileHelper::SaveStringToFile(Json,*Output,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return 2;
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SETTINGS_AUDIT passed=%d failed=%d report=%s playerSettingsWrites=0"),Passed,Failed,*Output);return Failed?1:0;
#else
	return 1;
#endif
}
