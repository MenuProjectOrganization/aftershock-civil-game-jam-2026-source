#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

void UAftershockOpeningSessionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
 Super::Initialize(Collection);
#if !UE_BUILD_SHIPPING
 // Explicit fixture uses the exact New Game contract before the world exists.
 // Legacy fixtures omit this flag and keep their saved five-metre cell scale.
 if(FParse::Param(FCommandLine::Get(),TEXT("AftershockVisualAudit"))
  &&FParse::Param(FCommandLine::Get(),TEXT("AftershockConfiguredSessionAudit")))
 {
  FAftershockSessionSettings Initial;FText Error;
  int32 AuditRegion=0,AuditFocus=0;
  const bool HasAuditRegion=FParse::Value(FCommandLine::Get(),TEXT("AftershockAuditRegion="),AuditRegion);
  const bool HasAuditFocus=FParse::Value(FCommandLine::Get(),TEXT("AftershockAuditFocus="),AuditFocus);
  if(HasAuditRegion||HasAuditFocus)
  {
   const auto* Site=TryGetAftershockGeographyFocus(AuditRegion,AuditFocus);
   if(!Site)
   {
    UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_CONFIGURED_SESSION_AUDIT_INVALID_REGION region=%d focus=%d fallbackConfigured=0"),AuditRegion,AuditFocus);
    return;
   }
   Initial.Mode=EAftershockPlayMode::GeographicSimulation;
   Initial.RegionID=FName(Site->Id);Initial.FocusID=FName(Site->FocusId);
  }
  const bool Configured=ConfigureNewSession(Initial,Error);
  UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CONFIGURED_SESSION_AUDIT configured=%d region=%s focus=%s usesCanonicalNewGameSettings=1 cellScaleSource=Scenario error=%s"),Configured,*Initial.RegionID.ToString(),*Initial.FocusID.ToString(),*Error.ToString());
 }
#endif
}

bool UAftershockOpeningSessionSubsystem::ValidateSettings(const FAftershockSessionSettings& S,FText& Error)
{
 const int32 Region=FindAftershockGeographyIndex(S.RegionID);
 const int32 Focus=S.FocusID.IsNone()?0:FindAftershockGeographyFocusIndex(Region,S.FocusID);
 if(static_cast<uint8>(S.Mode)>static_cast<uint8>(EAftershockPlayMode::Campaign)||Region==INDEX_NONE||Focus==INDEX_NONE
  ||!IsAftershockGeographyFocusReady(Region,Focus)||!FMath::IsFinite(S.Difficulty)||S.Difficulty<.5f||S.Difficulty>2.f
  ||S.CampaignStage<0||S.CampaignStage>=UAftershockScenarioSubsystem::GetScenarioCount())
 {Error=FText::FromString(TEXT("Oyun modu, bölge veya zorluk seçimi geçersiz."));return false;}
 if(S.Mode==EAftershockPlayMode::Campaign&&(Region!=S.CampaignStage||(Focus!=0&&Focus!=GetAftershockDefaultNewGameFocusIndex(Region))))
 {Error=FText::FromString(TEXT("Kampanya aşaması ve hazırlanmış şehir eşleşmiyor."));return false;}
 return true;
}
bool UAftershockOpeningSessionSubsystem::ConfigureNewSession(const FAftershockSessionSettings& S,FText& Error)
{
 FAftershockSessionSettings NewSettings=S;
 // NEW session policy only. Save/load RestoreSession preserves legacy None
 // as original focus0; it must never silently move a recorded city anchor.
 if(NewSettings.FocusID.IsNone())
 {
  const int32 Region=FindAftershockGeographyIndex(NewSettings.RegionID);
  const auto* Focus=TryGetAftershockGeographyFocus(Region,GetAftershockDefaultNewGameFocusIndex(Region));
  if(Focus&&GetAftershockDefaultNewGameFocusIndex(Region)!=0)NewSettings.FocusID=FName(Focus->FocusId);
 }
 if(!ValidateSettings(NewSettings,Error))return false;
 Settings=NewSettings;Campaign={};Campaign.FurthestStage=NewSettings.CampaignStage;SessionID=FGuid::NewGuid();bHasLiveSession=true;bHasConfiguredSession=true;return true;
}
void UAftershockOpeningSessionSubsystem::RestoreSession(const FAftershockSessionSettings& S,const FAftershockCampaignProgress& P,FGuid ID)
{Settings=S;Campaign=P;SessionID=ID.IsValid()?ID:FGuid::NewGuid();bHasLiveSession=true;bHasConfiguredSession=true;}
void UAftershockOpeningSessionSubsystem::CompleteCampaignStage(int32 Stage)
{if(Settings.Mode==EAftershockPlayMode::Campaign&&Stage==Settings.CampaignStage&&Stage>=0&&Stage<UAftershockScenarioSubsystem::GetScenarioCount()){Campaign.CompletedStages.Add(Stage);Campaign.FurthestStage=FMath::Max(Campaign.FurthestStage,Stage+1);}}
bool UAftershockOpeningSessionSubsystem::AdvanceCampaignStage(int32 Stage,FText& Error)
{
 if(Settings.Mode!=EAftershockPlayMode::Campaign||Stage!=Settings.CampaignStage+1||!Campaign.CompletedStages.Contains(Stage-1)||Stage>=UAftershockScenarioSubsystem::GetScenarioCount())
 {Error=FText::FromString(TEXT("Sonraki kampanya aşaması henüz açılmadı."));return false;}
 Settings.CampaignStage=Stage;Settings.RegionID=FName(GetAftershockGeography(Stage).Id);
 const int32 Focus=GetAftershockDefaultNewGameFocusIndex(Stage);
 Settings.FocusID=Focus==0?NAME_None:FName(GetAftershockGeographyFocus(Stage,Focus).FocusId);return true;
}
