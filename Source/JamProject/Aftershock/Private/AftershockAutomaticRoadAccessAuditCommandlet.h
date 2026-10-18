#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockAutomaticRoadAccessAuditCommandlet.generated.h"

class UBuildingData;
class UWorld;

/** Isolated canonical purchase/routing tests. No user maps or saves are loaded. */
UCLASS()
class JAMPROJECT_API UAftershockAutomaticRoadAccessAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockAutomaticRoadAccessAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
private:
	UFUNCTION() void ObserveBootstrapEconomy(int64 TreasuryTL,float Satisfaction,int32 CurrentTurn);
	UFUNCTION() void ObserveBootstrapLife();
	TWeakObjectPtr<UWorld> AuditWorld;
	TWeakObjectPtr<UBuildingData> AuditHouse;
	FIntPoint BootstrapPlot=FIntPoint::ZeroValue;
	int64 ExpectedBootstrapTreasury=0;
	int32 ExpectedBootstrapPopulation=0;
	int32 BootstrapEconomyNotifications=0,BootstrapLifeNotifications=0;
	bool bEconomyObservedPaidStructure=true,bLifeObservedPaidCanonicalRoster=true;
	bool bEconomyReentryRejected=true,bLifeReentryRejected=true;
};
