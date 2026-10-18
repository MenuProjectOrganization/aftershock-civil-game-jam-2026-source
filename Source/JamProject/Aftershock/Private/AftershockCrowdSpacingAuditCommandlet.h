#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockCrowdSpacingAuditCommandlet.generated.h"

/** Isolated native visual-spacing, ISM-state and reflected crowd-save regression fixture. */
UCLASS()
class JAMPROJECT_API UAftershockCrowdSpacingAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockCrowdSpacingAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
