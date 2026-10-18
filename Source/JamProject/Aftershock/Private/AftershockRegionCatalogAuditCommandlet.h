#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockRegionCatalogAuditCommandlet.generated.h"

/** Native catalogue/DEM/identity audit. Does not claim terrain, UI, or hazard simulation was played. */
UCLASS()
class JAMPROJECT_API UAftershockRegionCatalogAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockRegionCatalogAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
