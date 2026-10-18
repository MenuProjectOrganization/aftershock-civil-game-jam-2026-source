#pragma once
#include "Commandlets/Commandlet.h"
#include "AftershockPresentationPerformanceAuditCommandlet.generated.h"

UCLASS()
class UAftershockPresentationPerformanceAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockPresentationPerformanceAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
