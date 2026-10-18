#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockSettingsAuditCommandlet.generated.h"

/** Opt-in preference contract checks; does not change a user's INI or launch a world. */
UCLASS()
class UAftershockSettingsAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockSettingsAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
