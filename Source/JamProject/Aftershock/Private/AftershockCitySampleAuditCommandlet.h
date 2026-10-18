#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockCitySampleAuditCommandlet.generated.h"
UCLASS()
class UAftershockCitySampleAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockCitySampleAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
