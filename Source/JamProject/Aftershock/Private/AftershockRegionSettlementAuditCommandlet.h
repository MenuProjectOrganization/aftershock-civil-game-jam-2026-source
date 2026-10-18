#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockRegionSettlementAuditCommandlet.generated.h"

/** Starts a separate real GameInstance city for every selectable local geographic focus. */
UCLASS()
class JAMPROJECT_API UAftershockRegionSettlementAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockRegionSettlementAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
