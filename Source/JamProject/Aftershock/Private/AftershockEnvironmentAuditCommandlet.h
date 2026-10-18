#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockEnvironmentAuditCommandlet.generated.h"
UCLASS()
class UAftershockEnvironmentAuditCommandlet:public UCommandlet
{
 GENERATED_BODY()
public:
 UAftershockEnvironmentAuditCommandlet();
 virtual int32 Main(const FString& Params) override;
};
