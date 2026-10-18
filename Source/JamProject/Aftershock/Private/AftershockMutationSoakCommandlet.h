#pragma once
#include "Commandlets/Commandlet.h"
#include "AftershockMutationSoakCommandlet.generated.h"
UCLASS()
class UAftershockMutationSoakCommandlet final : public UCommandlet
{
    GENERATED_BODY()
public:
    UAftershockMutationSoakCommandlet();
    virtual int32 Main(const FString& Params) override;
};
