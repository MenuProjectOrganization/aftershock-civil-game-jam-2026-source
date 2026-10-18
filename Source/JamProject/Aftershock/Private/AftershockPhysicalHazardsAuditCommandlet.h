#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockPhysicalHazardsAuditCommandlet.generated.h"

/** Opt-in isolated numerical/API checks. No rendered-scene or physical-forecast claim. */
UCLASS()
class UAftershockPhysicalHazardsAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockPhysicalHazardsAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
