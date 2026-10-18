#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockGeographicHazardsAuditCommandlet.generated.h"

/** Opt-in isolated ENGINE checks; no rendered-scene/forecast claim. */
UCLASS()
class UAftershockGeographicHazardsAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockGeographicHazardsAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
