#include "AftershockSettingsMenuRuntimeAudit.h"
#if !UE_BUILD_SHIPPING
#include "Aftershock/Game/AftershockPlayerController.h"
#include "Aftershock/Public/AftershockMainHUDWidget.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockSettingsWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CheckBox.h"
#include "Components/PanelWidget.h"
#include "Components/ScrollBox.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HighResScreenshot.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
struct FMenuAudit
{
 double Started=0.,Next=0.,PendingDeadline=0.;int32 Stage=0,Page=0;bool Done=false,Valid=true;
 FString Folder,Pending,SettingsIniHash,SettingsIniPath;TArray<TSharedPtr<FJsonValue>> Rows,Images;
};
TMap<TWeakObjectPtr<AAftershockPlayerController>,FMenuAudit> Audits;
bool Visible(const UWidget* W)
{for(const UWidget* P=W;P;P=P->GetParent())if(!P->IsVisible())return false;return W!=nullptr;}
FString Label(UWidget* W,int32 Depth=0)
{
 if(!W||Depth>12)return FString();if(auto* T=Cast<UTextBlock>(W))return T->GetText().ToString();
 FString Text;if(auto* P=Cast<UPanelWidget>(W))for(int32 I=0;I<P->GetChildrenCount();++I)Text+=Label(P->GetChildAt(I),Depth+1);
 return Text;
}
template<class T>T* Find(AAftershockPlayerController& PC)
{
 TArray<UUserWidget*> Found;UWidgetBlueprintLibrary::GetAllWidgetsOfClass(PC.GetWorld(),Found,T::StaticClass(),false);
 for(auto* W:Found)if(W&&W->GetOwningPlayer()==&PC&&W->IsInViewport())return Cast<T>(W);return nullptr;
}
TArray<UWidget*> Widgets(UUserWidget* W)
{TArray<UWidget*> All;if(W&&W->WidgetTree)W->WidgetTree->GetAllWidgets(All);return All;}
UButton* Button(UUserWidget* W,const FString& Prefix)
{for(UWidget* Item:Widgets(W))if(auto* B=Cast<UButton>(Item);B&&Visible(B)&&Label(B).StartsWith(Prefix))return B;return nullptr;}
int32 QuitButtons(UUserWidget* W)
{int32 Count=0;for(UWidget* Item:Widgets(W))if(auto* B=Cast<UButton>(Item);B&&Visible(B)&&Label(B).Contains(TEXT("OYUNDAN ÇIK")))++Count;return Count;}
FString PhysicalIniPath()
{
 const FConfigBranch* Branch=GConfig?GConfig->FindBranchWithNoReload(NAME_None,GGameUserSettingsIni):nullptr;
 return Branch&&Branch->IniName==FName(TEXT("GameUserSettings"))?Branch->IniPath:FString();
}
FString IniHash(const FString& PhysicalPath)
{
 if(PhysicalPath.IsEmpty())return FString();
 if(!FPaths::FileExists(PhysicalPath))return TEXT("resolved-but-absent");
 TArray<uint8> Bytes;if(!FFileHelper::LoadFileToArray(Bytes,*PhysicalPath))return FString();
 return FString(TEXT("present:"))+FMD5::HashBytes(Bytes.GetData(),Bytes.Num());
}
void Finish(AAftershockPlayerController& PC,FMenuAudit& S,const FString& Failure=FString())
{
 if(S.Done)return;S.Done=true;if(!Failure.IsEmpty())S.Valid=false;
 const FString PhysicalAfter=PhysicalIniPath(),HashAfter=IniHash(PhysicalAfter);
 const bool PhysicalResolved=!S.SettingsIniPath.IsEmpty()&&!PhysicalAfter.IsEmpty();
 const bool SamePhysical=PhysicalResolved&&FPaths::IsSamePath(S.SettingsIniPath,PhysicalAfter);
 const bool IniUnchanged=SamePhysical&&!S.SettingsIniHash.IsEmpty()&&!HashAfter.IsEmpty()&&HashAfter==S.SettingsIniHash;
 auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("schema_version"),1);J->SetBoolField(TEXT("valid"),S.Valid);
 J->SetStringField(TEXT("failure"),Failure);J->SetArrayField(TEXT("steps"),S.Rows);J->SetArrayField(TEXT("images"),S.Images);
 J->SetNumberField(TEXT("pages_exercised"),S.Page);J->SetNumberField(TEXT("elapsed_real_seconds"),FPlatformTime::Seconds()-S.Started);
 J->SetBoolField(TEXT("user_settings_writes_disabled"),!UAftershockSettingsSubsystem::CanWriteUserSettings());
 J->SetBoolField(TEXT("user_settings_ini_unchanged"),IniUnchanged);
 J->SetBoolField(TEXT("physical_ini_resolved"),PhysicalResolved);J->SetBoolField(TEXT("physical_ini_destination_same"),SamePhysical);
 J->SetStringField(TEXT("physical_ini_resolution_source"),TEXT("LoadedGConfigBranch.IniPath"));
 J->SetBoolField(TEXT("apply_or_quit_button_invoked"),false);
 J->SetStringField(TEXT("image_folder"),FPaths::GetCleanFilename(S.Folder));
 J->SetStringField(TEXT("scope"),TEXT("Opt-in actual UMG/controller/button delegates and ShowUI game-viewport PNGs. Loaded physical config compared before process exit. Parent checks original user bytes after exit. No OS mouse input, Apply, display change, settings persistence, application quit action, GPU benchmark, packaged or cross-platform claim."));
 S.Valid&=IniUnchanged&&!UAftershockSettingsSubsystem::CanWriteUserSettings()&&S.Page==5;
 J->SetBoolField(TEXT("valid"),S.Valid);FString Text;const auto Writer=TJsonWriterFactory<>::Create(&Text);
 const FString Path=FPaths::ProjectSavedDir()/TEXT("PresentationValidation/settings_menu_runtime_audit.json");
 const bool Saved=FJsonSerializer::Serialize(J,Writer)&&FFileHelper::SaveStringToFile(Text,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
 UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SETTINGS_MENU_RUNTIME_RESULT valid=%d pages=%d reportSaved=%d elapsedReal=%.3f actualUMG=1 applyWrites=0"),S.Valid&&Saved,S.Page,Saved,FPlatformTime::Seconds()-S.Started);
 if(FParse::Param(FCommandLine::Get(),TEXT("AftershockSettingsMenuAuditExit")))FPlatformMisc::RequestExit(false);
}
bool Capture(AAftershockPlayerController& PC,FMenuAudit& S,const TCHAR* Name)
{
 int32 X=0,Y=0;PC.GetViewportSize(X,Y);if(X!=1600||Y!=900)return false;
 S.Pending=S.Folder/(FString(Name)+TEXT(".png"));if(FPaths::FileExists(S.Pending))return false;
 FScreenshotRequest::RequestScreenshot(S.Pending,true,false,false,FIntRect(),true);
 S.PendingDeadline=FPlatformTime::Seconds()+3.;S.Next=FPlatformTime::Seconds()+.35;return true;
}
bool PageRecord(AAftershockPlayerController& PC,FMenuAudit& S,UAftershockSettingsWidget* Menu)
{
 if(!Menu)return false;auto Row=MakeShared<FJsonObject>();int32 Choices=0,Sliders=0,Toggles=0,Keys=0,ActivePages=0,ActualPage=INDEX_NONE;
 TArray<TSharedPtr<FJsonValue>> Labels;
 for(UWidget* W:Widgets(Menu))
 {
  if(auto* Scroll=Cast<UScrollBox>(W);Scroll&&Scroll->GetChildrenCount()==5)
   for(int32 I=0;I<5;++I)if(auto* V=Cast<UVerticalBox>(Scroll->GetChildAt(I));V&&V->IsVisible()){++ActivePages;ActualPage=I;}
  if(!Visible(W))continue;
  Choices+=W->IsA<UComboBoxString>()?1:0;Sliders+=W->IsA<USlider>()?1:0;Toggles+=W->IsA<UCheckBox>()?1:0;Keys+=W->IsA<UInputKeySelector>()?1:0;
  if(auto* B=Cast<UButton>(W))Labels.Add(MakeShared<FJsonValueString>(Label(B)));
 }
 const int32 Expected[][4]={{13,1,2,0},{0,3,0,0},{0,3,2,11},{0,0,1,0},{0,0,3,0}};
 const bool Good=PC.GetWorld()->IsPaused()&&Menu->IsInViewport()&&ActivePages==1&&ActualPage==S.Page&&Choices==Expected[S.Page][0]&&Sliders==Expected[S.Page][1]&&Toggles==Expected[S.Page][2]&&Keys==Expected[S.Page][3];
 Row->SetStringField(TEXT("stage"),FString::Printf(TEXT("pause_settings_page_%d"),S.Page));Row->SetBoolField(TEXT("passed"),Good);Row->SetBoolField(TEXT("world_paused"),PC.GetWorld()->IsPaused());
 Row->SetNumberField(TEXT("actual_visible_page"),ActualPage);Row->SetNumberField(TEXT("active_pages"),ActivePages);Row->SetNumberField(TEXT("choices"),Choices);Row->SetNumberField(TEXT("sliders"),Sliders);Row->SetNumberField(TEXT("toggles"),Toggles);Row->SetNumberField(TEXT("key_selectors"),Keys);Row->SetArrayField(TEXT("actual_visible_button_labels"),Labels);S.Rows.Add(MakeShared<FJsonValueObject>(Row));
 return Good;
}
void Record(FMenuAudit& S,const TCHAR* Stage,bool Passed,int32 QuitCount)
{auto J=MakeShared<FJsonObject>();J->SetStringField(TEXT("stage"),Stage);J->SetBoolField(TEXT("passed"),Passed);J->SetNumberField(TEXT("actual_visible_quit_buttons"),QuitCount);S.Rows.Add(MakeShared<FJsonValueObject>(J));S.Valid&=Passed;}
}

void TickAftershockSettingsMenuRuntimeAudit(AAftershockPlayerController& PC)
{
 if(!PC.IsLocalPlayerController()||!FParse::Param(FCommandLine::Get(),TEXT("AftershockSettingsMenuAudit")))return;
 for(auto It=Audits.CreateIterator();It;++It)if(!It.Key().IsValid())It.RemoveCurrent();
 auto& S=Audits.FindOrAdd(&PC);const double Now=FPlatformTime::Seconds();if(S.Done)return;
 if(S.Started==0.){S.Started=Now;S.Next=Now+1.;S.SettingsIniPath=PhysicalIniPath();S.SettingsIniHash=IniHash(S.SettingsIniPath);if(S.SettingsIniPath.IsEmpty()||S.SettingsIniHash.IsEmpty())S.Valid=false;S.Folder=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("PresentationValidation")/FString::Printf(TEXT("settings-menu-%lld"),FDateTime::UtcNow().GetTicks()));IFileManager::Get().MakeDirectory(*S.Folder,true);}
 if(Now-S.Started>30.){Finish(PC,S,TEXT("Real-clock30seconddeadline"));return;}if(Now<S.Next)return;
 if(!S.Pending.IsEmpty())
 {
  TArray<uint8> Raw;if(!FFileHelper::LoadFileToArray(Raw,*S.Pending)){if(Now<S.PendingDeadline)return;Finish(PC,S,TEXT("ShowUIviewportPNGnotwritten"));return;}
  const auto BE=[&Raw](int32 At){return(static_cast<uint32>(Raw[At])<<24)|(static_cast<uint32>(Raw[At+1])<<16)|(static_cast<uint32>(Raw[At+2])<<8)|Raw[At+3];};
  if(Raw.Num()<24||Raw[0]!=137||Raw[1]!=80||Raw[2]!=78||Raw[3]!=71||BE(16)!=1600||BE(20)!=900){Finish(PC,S,TEXT("InvalidactualviewportPNGdimensions"));return;}
  S.Images.Add(MakeShared<FJsonValueString>(FPaths::GetCleanFilename(S.Pending)));S.Pending.Empty();
 }
 auto* HUD=PC.GetMainHUD();if(!HUD){Finish(PC,S,TEXT("ActualmainHUDmissing"));return;}
 switch(S.Stage)
 {
 case 0:PC.HandleEscapePressed();S.Stage=1;S.Next=Now+.35;return;
 case 1:{const int32 Q=QuitButtons(HUD);const bool Good=PC.GetWorld()->IsPaused()&&!PC.IsOpeningMenuVisible()&&Button(HUD,TEXT("DEVAM ET"))&&Q==0;Record(S,TEXT("actual_pause"),Good,Q);if(!Good||!Capture(PC,S,TEXT("settings-pause"))){Finish(PC,S,TEXT("Pausemenucontract"));return;}S.Stage=2;return;}
 case 2:PC.ShowSettingsMenu();S.Stage=3;S.Next=Now+.35;return;
 case 3:if(auto* M=Find<UAftershockSettingsWidget>(PC)){M->HandleAction(S.Page);S.Stage=4;S.Next=Now+.3;return;}Finish(PC,S,TEXT("Actualsettingswidgetmissing"));return;
 case 4:if(!PageRecord(PC,S,Find<UAftershockSettingsWidget>(PC))||!Capture(PC,S,*FString::Printf(TEXT("settings-page-%d"),S.Page))){Finish(PC,S,TEXT("Actualpagetreecontract"));return;}++S.Page;S.Stage=S.Page<5?3:5;return;
 case 5:PC.CloseSettingsMenu();S.Stage=6;S.Next=Now+.3;return;
 case 6:{const bool Good=PC.GetWorld()->IsPaused()&&!PC.IsSettingsMenuVisible()&&!PC.IsOpeningMenuVisible()&&Button(HUD,TEXT("DEVAM ET"));Record(S,TEXT("settings_returned_to_pause"),Good,QuitButtons(HUD));if(!Good||!Capture(PC,S,TEXT("settings-return-pause"))){Finish(PC,S,TEXT("Closereturnedoutsidepause"));return;}S.Stage=7;return;}
 case 7:if(auto* B=Button(HUD,TEXT("ANA MENÜ"))){B->OnClicked.Broadcast();S.Stage=8;S.Next=Now+.35;return;}Finish(PC,S,TEXT("Actualmainmenureturndelegatemissing"));return;
 case 8:{auto* Opening=Find<UAftershockOpeningWidget>(PC);const int32 Q=QuitButtons(Opening);const bool Good=Opening&&PC.IsOpeningMenuVisible()&&PC.GetWorld()->IsPaused()&&Q==1;Record(S,TEXT("actual_return_to_main_menu"),Good,Q);if(!Good||!Capture(PC,S,TEXT("settings-main-menu"))){Finish(PC,S,TEXT("Mainmenuquitcontract"));return;}S.Stage=9;return;}
 case 9:PC.ShowSettingsMenu();S.Stage=10;S.Next=Now+.35;return;
 case 10:{const bool Good=PC.IsSettingsMenuVisible()&&PC.IsOpeningMenuVisible()&&PC.GetWorld()->IsPaused();Record(S,TEXT("settings_from_main_menu"),Good,0);if(!Good||!Capture(PC,S,TEXT("settings-from-main"))){Finish(PC,S,TEXT("Mainsettingsorigin"));return;}S.Stage=11;return;}
 case 11:PC.HandleEscapePressed();S.Stage=12;S.Next=Now+.35;return;
 case 12:{auto* Opening=Find<UAftershockOpeningWidget>(PC);const int32 Q=QuitButtons(Opening);const bool Good=Opening&&PC.IsOpeningMenuVisible()&&!PC.IsSettingsMenuVisible()&&PC.GetWorld()->IsPaused()&&Q==1;Record(S,TEXT("settings_returned_to_main_menu"),Good,Q);if(!Good||!Capture(PC,S,TEXT("settings-return-main"))){Finish(PC,S,TEXT("Mainsettingsclosereturn"));return;}S.Stage=13;return;}
 default:Finish(PC,S);return;
 }
}
#else
void TickAftershockSettingsMenuRuntimeAudit(AAftershockPlayerController&){}
#endif
