#pragma once

#include "Commandlets/Commandlet.h"
#include "AftershockPlayableAuditCommandlet.generated.h"

/** Editor-only, isolated-world checks of canonical playable transactions and ISM state. */
UCLASS()
class UAftershockPlayableAuditCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UAftershockPlayableAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
