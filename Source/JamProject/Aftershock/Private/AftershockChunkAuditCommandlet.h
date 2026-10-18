#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockChunkAuditCommandlet.generated.h"

/** Explicit native coverage of canonical chunk storage; no GPU/pixel claim. */
UCLASS()
class JAMPROJECT_API UAftershockChunkAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockChunkAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
