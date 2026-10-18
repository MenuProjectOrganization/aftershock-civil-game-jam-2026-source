#pragma once
#include "Commandlets/Commandlet.h"
#include "AftershockTransportAuditCommandlet.generated.h"

/** Explicit synthetic road/resident/boarding fixture; no project map mutations. */
UCLASS()
class UAftershockTransportAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockTransportAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
