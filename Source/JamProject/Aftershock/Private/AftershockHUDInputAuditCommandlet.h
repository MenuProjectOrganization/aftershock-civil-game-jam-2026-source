#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockHUDInputAuditCommandlet.generated.h"

/** Native HUD delegate/transaction/modal regression audit; no pointer or GPU claims. */
UCLASS()
class JAMPROJECT_API UAftershockHUDInputAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockHUDInputAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
