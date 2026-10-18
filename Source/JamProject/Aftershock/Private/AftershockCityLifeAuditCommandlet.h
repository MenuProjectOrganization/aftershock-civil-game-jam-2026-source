#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockCityLifeAuditCommandlet.generated.h"

/** Opt-in isolated canonical startup/needs/finance/identity API audit. */
UCLASS()
class JAMPROJECT_API UAftershockCityLifeAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockCityLifeAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
