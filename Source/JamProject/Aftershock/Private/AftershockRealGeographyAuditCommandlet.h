#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AftershockRealGeographyAuditCommandlet.generated.h"

/** Source-data, physical-scale and save-policy fixture; no rendered/city/hazard claim. */
UCLASS()
class JAMPROJECT_API UAftershockRealGeographyAuditCommandlet final : public UCommandlet
{
	GENERATED_BODY()
public:
	UAftershockRealGeographyAuditCommandlet();
	virtual int32 Main(const FString& Params) override;
};
