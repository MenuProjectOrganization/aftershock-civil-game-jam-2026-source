// AFTERSHOCK — offline editor content generation; never fractures at runtime.
#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockFractureSetupCommandlet.generated.h"

/** -run=AftershockFractureSetup generates and validates the five canonical GC assets. */
UCLASS()
class JAMPROJECT_API UAftershockFractureSetupCommandlet final : public UCommandlet
{
	GENERATED_BODY()

public:
	UAftershockFractureSetupCommandlet();
	virtual int32 Main(const FString& Params) override;
};
