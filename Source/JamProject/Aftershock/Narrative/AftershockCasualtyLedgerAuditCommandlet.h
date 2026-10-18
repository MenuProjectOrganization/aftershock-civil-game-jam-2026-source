#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockCasualtyLedgerAuditCommandlet.generated.h"
UCLASS()
class JAMPROJECT_API UAftershockCasualtyLedgerAuditCommandlet:public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockCasualtyLedgerAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
