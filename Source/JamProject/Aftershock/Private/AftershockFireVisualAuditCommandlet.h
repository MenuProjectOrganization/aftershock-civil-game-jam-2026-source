#pragma once

#include "Commandlets/Commandlet.h"
#include "AftershockFireVisualAuditCommandlet.generated.h"

/** Opt-in isolated native display/state checks; no rendered quality or GPU timing claim. */
UCLASS()
class UAftershockFireVisualAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockFireVisualAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
