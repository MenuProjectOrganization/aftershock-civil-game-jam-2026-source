#pragma once

#include "Commandlets/Commandlet.h"
#include "AftershockCrowdProfileCommandlet.generated.h"

/** Headless CPU timing harness for the timer-driven ISM crowd presentation subsystem. */
UCLASS()
class UAftershockCrowdProfileCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UAftershockCrowdProfileCommandlet();
	virtual int32 Main(const FString& Params) override;
};
