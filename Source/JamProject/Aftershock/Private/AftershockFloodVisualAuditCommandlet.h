#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockFloodVisualAuditCommandlet.generated.h"

/** -run=AftershockFloodVisualAudit: actual Engine flood mesh reconstruction fixtures. */
UCLASS()
class JAMPROJECT_API UAftershockFloodVisualAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockFloodVisualAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
