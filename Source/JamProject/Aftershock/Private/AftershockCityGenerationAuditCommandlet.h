#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockCityGenerationAuditCommandlet.generated.h"

UCLASS()
class UAftershockCityGenerationAuditCommandlet final:public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockCityGenerationAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
