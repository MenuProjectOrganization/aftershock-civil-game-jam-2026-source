#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockSessionSaveAuditCommandlet.generated.h"
UCLASS()
class UAftershockSessionSaveAuditCommandlet final:public UCommandlet
{
 GENERATED_BODY()
public:
 UAftershockSessionSaveAuditCommandlet();
 virtual int32 Main(const FString& Params) override;
};
