#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockCityScaleAuditCommandlet.generated.h"

UCLASS()
class UAftershockCityScaleAuditCommandlet final:public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockCityScaleAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
