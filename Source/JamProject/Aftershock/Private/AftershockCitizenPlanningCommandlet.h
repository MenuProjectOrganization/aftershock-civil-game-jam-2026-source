#pragma once

#include "Commandlets/Commandlet.h"
#include "AftershockCitizenPlanningCommandlet.generated.h"

/** Synthetic canonical-building fixtures; no gameplay save/economy/asset changes. */
UCLASS()
class UAftershockCitizenPlanningCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockCitizenPlanningCommandlet();
	virtual int32 Main(const FString& Params) override;
};
