#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockScenarioAuditCommandlet.generated.h"

/** Isolated canonical fixture: -run=AftershockScenarioAudit -AftershockScenario=0/1/2. */
UCLASS()
class JAMPROJECT_API UAftershockScenarioAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockScenarioAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
