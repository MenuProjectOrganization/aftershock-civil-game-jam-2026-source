#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockBridgeAuditCommandlet.generated.h"

UCLASS()
class UAftershockBridgeAuditListener final : public UObject
{
	GENERATED_BODY()
public:
	int32 Changes=0;
	UFUNCTION() void OnChanged() { ++Changes; }
};

/** Opt-in native bridge transaction, profile, graph, water, render and save fixture. */
UCLASS()
class JAMPROJECT_API UAftershockBridgeAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockBridgeAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
