#pragma once
#include "Commandlets/Commandlet.h"
#include "AftershockMunicipalAuditCommandlet.generated.h"

/** Explicit canonical-worker/derived-fleet fixture. Does not load or write player saves. */
UCLASS()
class UAftershockMunicipalAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockMunicipalAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
