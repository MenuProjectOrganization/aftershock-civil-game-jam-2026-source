#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockPresentationBudgetAuditCommandlet.generated.h"

UCLASS()
class UAftershockPresentationBudgetAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockPresentationBudgetAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};

