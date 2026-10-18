#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "AftershockRubblePersistenceAuditCommandlet.generated.h"

UCLASS()
class UAftershockRubbleAuditListener final : public UObject
{
	GENERATED_BODY()
public:
	int32 Completions=0;
	UFUNCTION() void Completed(const FDisasterReport& Report) { ++Completions; }
};

/** Native five-collection frozen-pose persistence and rejected-input audit. */
UCLASS()
class JAMPROJECT_API UAftershockRubblePersistenceAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockRubblePersistenceAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
