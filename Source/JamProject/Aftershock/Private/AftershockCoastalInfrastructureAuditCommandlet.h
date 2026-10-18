#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "Aftershock/Public/DisasterSimulationSubsystem.h"
#include "AftershockCoastalInfrastructureAuditCommandlet.generated.h"

/** Weak multicast observers used only by the opt-in isolated commandlet. */
UCLASS()
class UAftershockCoastalAuditListener final : public UObject
{
	GENERATED_BODY()
public:
	int32 Completions = 0;
	int32 DamEvents = 0;
	UFUNCTION() void RecordCompleted(const FDisasterReport& Report) { ++Completions; }
	UFUNCTION() void RecordDamChanged(FIntPoint Coord) { ++DamEvents; }
};

/** -run=AftershockCoastalInfrastructureAudit. Never creates or saves a gameplay map. */
UCLASS()
class JAMPROJECT_API UAftershockCoastalInfrastructureAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockCoastalInfrastructureAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
