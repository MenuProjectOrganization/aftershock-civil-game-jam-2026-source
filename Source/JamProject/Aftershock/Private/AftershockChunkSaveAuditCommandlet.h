#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockChunkSaveAuditCommandlet.generated.h"

UCLASS()
class UAftershockChunkSaveAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockChunkSaveAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
