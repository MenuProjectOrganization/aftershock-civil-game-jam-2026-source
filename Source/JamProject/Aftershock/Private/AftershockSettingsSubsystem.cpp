#include "Aftershock/Public/AftershockSettingsSubsystem.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/App.h"

namespace { constexpr const TCHAR* PreferenceSection = TEXT("Aftershock.UserPreferences"); }

FAftershockUserPreferences::FAftershockUserPreferences()
{
	Keys = {EKeys::W,EKeys::S,EKeys::A,EKeys::D,EKeys::Q,EKeys::E,EKeys::G,EKeys::R,EKeys::F,EKeys::PageUp,EKeys::PageDown};
}

bool UAftershockSettingsSubsystem::CanWriteUserSettings()
{
	return !IsRunningCommandlet() && !IsRunningDedicatedServer() && !FApp::IsUnattended()
		&& !FParse::Param(FCommandLine::Get(),TEXT("AftershockVisualAudit"))
		&& !FParse::Param(FCommandLine::Get(),TEXT("AftershockSettingsMenuAudit"))
		&& !FParse::Param(FCommandLine::Get(),TEXT("nullrhi"));
}

void UAftershockSettingsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// Capture fixtures use deterministic defaults and never rewrite player preferences.
	if (!CanWriteUserSettings() || !GConfig) return;
	FAftershockUserPreferences Loaded;
#define READ_FLOAT(Name) GConfig->GetFloat(PreferenceSection,TEXT(#Name),Loaded.Name,GGameUserSettingsIni)
#define READ_BOOL(Name) GConfig->GetBool(PreferenceSection,TEXT(#Name),Loaded.Name,GGameUserSettingsIni)
	READ_FLOAT(MasterVolume); READ_FLOAT(CityAmbienceVolume); READ_FLOAT(UIVolume);
	READ_FLOAT(PanSensitivity); READ_FLOAT(RotationSensitivity); READ_FLOAT(ZoomSensitivity);
	READ_BOOL(bInvertWheel); READ_BOOL(bSmoothCamera); READ_BOOL(bReduceMotion);
	READ_BOOL(bColorSafePlacement); READ_BOOL(bLargeMenuText); READ_BOOL(bConfirmEndTurn);
#undef READ_FLOAT
#undef READ_BOOL
	for (int32 I=0;I<Loaded.Keys.Num();++I)
	{
		FString KeyName;
		if(GConfig->GetString(PreferenceSection,*FString::Printf(TEXT("Key%d"),I),KeyName,GGameUserSettingsIni)) Loaded.Keys[I]=FKey(FName(*KeyName));
	}
	FText Error;
	if(ValidatePreferences(Loaded,Error)) Preferences=Loaded;
	else UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_SETTINGS invalid stored preferences; safe defaults used: %s"),*Error.ToString());
}

FKey UAftershockSettingsSubsystem::GetKey(EAftershockUserAction Action) const
{
	const int32 Index=static_cast<int32>(Action);
	return Preferences.Keys.IsValidIndex(Index)?Preferences.Keys[Index]:FKey();
}

bool UAftershockSettingsSubsystem::ValidateKey(const TArray<FKey>& Keys,int32 Action,FKey Key,FText& Error)
{
	if(!Keys.IsValidIndex(Action)||!Key.IsValid()||Key.IsGamepadKey()||Key.IsMouseButton()||Key.IsAxis1D()||Key.IsAxis2D()||Key.IsAxis3D()
		||Key==EKeys::Escape||Key==EKeys::Tab||Key==EKeys::Enter||Key==EKeys::SpaceBar||Key==EKeys::Tilde||Key==EKeys::F11
		||Key==EKeys::LeftAlt||Key==EKeys::RightAlt||Key==EKeys::LeftControl||Key==EKeys::RightControl||Key==EKeys::LeftShift||Key==EKeys::RightShift
		||Key==EKeys::LeftCommand||Key==EKeys::RightCommand)
	{
		Error=NSLOCTEXT("Aftershock","SettingsReservedKey","Bu tuş menü gezintisine ayrılmış veya desteklenmiyor. Başka bir klavye tuşu seç.");return false;
	}
	for(int32 I=0;I<Keys.Num();++I)if(I!=Action&&Keys[I]==Key)
	{
		Error=FText::Format(NSLOCTEXT("Aftershock","SettingsKeyConflict","Bu tuş zaten «{0}» için kullanılıyor."),GetActionLabel(I));return false;
	}
	return true;
}

bool UAftershockSettingsSubsystem::ValidatePreferences(const FAftershockUserPreferences& V,FText& Error)
{
	const auto InRange=[](float Value,float Min,float Max){return FMath::IsFinite(Value)&&Value>=Min&&Value<=Max;};
	if(!InRange(V.MasterVolume,0,1)||!InRange(V.CityAmbienceVolume,0,1)||!InRange(V.UIVolume,0,1)
		||!InRange(V.PanSensitivity,.25f,3.f)||!InRange(V.RotationSensitivity,.25f,3.f)||!InRange(V.ZoomSensitivity,.25f,3.f)
		||V.Keys.Num()!=static_cast<int32>(EAftershockUserAction::Count))
	{Error=NSLOCTEXT("Aftershock","SettingsInvalidValues","Ayar değeri desteklenen aralığın dışında.");return false;}
	for(int32 I=0;I<V.Keys.Num();++I)if(!ValidateKey(V.Keys,I,V.Keys[I],Error))return false;
	return true;
}

bool UAftershockSettingsSubsystem::SetPreferences(const FAftershockUserPreferences& NewPreferences,bool bPersist,FText& Error)
{
	if(!ValidatePreferences(NewPreferences,Error))return false;
	if(bPersist&&!CanWriteUserSettings()){Error=NSLOCTEXT("Aftershock","SettingsAuditReadOnly","Bu doğrulama oturumunda kullanıcı ayarları kaydedilmez.");return false;}
	Preferences=NewPreferences;if(bPersist)SavePreferences();return true;
}

void UAftershockSettingsSubsystem::SavePreferences()
{
	if(!GConfig||!CanWriteUserSettings())return;
#define WRITE_FLOAT(Name) GConfig->SetFloat(PreferenceSection,TEXT(#Name),Preferences.Name,GGameUserSettingsIni)
#define WRITE_BOOL(Name) GConfig->SetBool(PreferenceSection,TEXT(#Name),Preferences.Name,GGameUserSettingsIni)
	WRITE_FLOAT(MasterVolume); WRITE_FLOAT(CityAmbienceVolume); WRITE_FLOAT(UIVolume);
	WRITE_FLOAT(PanSensitivity); WRITE_FLOAT(RotationSensitivity); WRITE_FLOAT(ZoomSensitivity);
	WRITE_BOOL(bInvertWheel); WRITE_BOOL(bSmoothCamera); WRITE_BOOL(bReduceMotion);
	WRITE_BOOL(bColorSafePlacement); WRITE_BOOL(bLargeMenuText); WRITE_BOOL(bConfirmEndTurn);
#undef WRITE_FLOAT
#undef WRITE_BOOL
	for(int32 I=0;I<Preferences.Keys.Num();++I)GConfig->SetString(PreferenceSection,*FString::Printf(TEXT("Key%d"),I),*Preferences.Keys[I].GetFName().ToString(),GGameUserSettingsIni);
	// Versioned unlimited is an explicit choice, not the old legacy unbounded default.
	GConfig->SetInt(TEXT("Aftershock.FramePacing"),TEXT("PreferenceVersion"),1,GGameUserSettingsIni);
	GConfig->Flush(false,GGameUserSettingsIni);
}

FText UAftershockSettingsSubsystem::GetActionLabel(int32 Action)
{
	static const TCHAR* Labels[]={TEXT("Kamerayı ileri taşı"),TEXT("Kamerayı geri taşı"),TEXT("Kamerayı sola taşı"),TEXT("Kamerayı sağa taşı"),TEXT("Sola döndür"),TEXT("Sağa döndür"),TEXT("Şehir / dünya görünümü"),TEXT("Binayı döndür"),TEXT("Temel kalitesini değiştir"),TEXT("Kat sayısını artır"),TEXT("Kat sayısını azalt")};
	return FText::FromString(Action>=0&&Action<UE_ARRAY_COUNT(Labels)?Labels[Action]:TEXT("Bilinmeyen işlem"));
}
