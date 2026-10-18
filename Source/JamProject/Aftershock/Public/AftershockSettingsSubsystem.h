#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "AftershockSettingsSubsystem.generated.h"

/** Native camera actions. Escape and mouse selection remain fixed safety controls. */
enum class EAftershockUserAction : uint8
{
	PanForward, PanBackward, PanLeft, PanRight, RotateLeft, RotateRight,
	Overview, RotateBuilding, CycleFoundation, IncreaseFloors, DecreaseFloors, Count
};

struct JAMPROJECT_API FAftershockUserPreferences
{
	float MasterVolume = 1.f;
	float CityAmbienceVolume = 1.f;
	float UIVolume = 1.f;
	float PanSensitivity = 1.f;
	float RotationSensitivity = 1.f;
	float ZoomSensitivity = 1.f;
	bool bInvertWheel = false;
	bool bSmoothCamera = true;
	bool bReduceMotion = false;
	bool bColorSafePlacement = false;
	bool bLargeMenuText = false;
	bool bConfirmEndTurn = false;
	TArray<FKey> Keys;
	FAftershockUserPreferences();
};

/** Local preferences only; never owns city state, simulation, or save-game data. */
UCLASS()
class JAMPROJECT_API UAftershockSettingsSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	const FAftershockUserPreferences& GetPreferences() const { return Preferences; }
	FKey GetKey(EAftershockUserAction Action) const;
	bool SetPreferences(const FAftershockUserPreferences& NewPreferences, bool bPersist, FText& Error);
	static bool ValidatePreferences(const FAftershockUserPreferences& Value, FText& Error);
	static bool ValidateKey(const TArray<FKey>& Keys, int32 Action, FKey Key, FText& Error);
	static FText GetActionLabel(int32 Action);
	static bool CanWriteUserSettings();
private:
	void SavePreferences();
	FAftershockUserPreferences Preferences;
};
