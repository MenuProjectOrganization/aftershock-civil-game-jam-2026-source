#pragma once

#include "Commandlets/Commandlet.h"
#include "AftershockFxSetupCommandlet.generated.h"

/** Regenerates the small, presentation-only Niagara systems used by AFTERSHOCK. */
UCLASS()
class UAftershockFxSetupCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UAftershockFxSetupCommandlet();
	virtual int32 Main(const FString& Params) override;
};
