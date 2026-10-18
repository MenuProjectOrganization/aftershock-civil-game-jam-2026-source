#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockSaveCodecAuditCommandlet.generated.h"

UCLASS()
class UAftershockSaveCodecAuditCommandlet final:public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockSaveCodecAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
