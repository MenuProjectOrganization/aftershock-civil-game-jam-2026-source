#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockRegionalEnvironmentAuditCommandlet.generated.h"

/** Actual detail/context/climate source contracts; no rendered or forecast claim. */
UCLASS()
class JAMPROJECT_API UAftershockRegionalEnvironmentAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockRegionalEnvironmentAuditCommandlet();
	virtual int32 Main(const FString& Params)override;
};
