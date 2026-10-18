#pragma once
#include "CoreMinimal.h"
class UAftershockSaveGame;
namespace AftershockLegacyV4
{
    struct FConversionStats
    {int64 Properties=0,ArrayElements=0,MapPairs=0,SetElements=0;int32 CurrentOnlyFieldsKept=0;};
    /** Returns a NEW transient snapshot only; no world/user-file mutation. Caller
     * performs normal full ValidateSnapshot/migration before any RestoreWorld. */
    UAftershockSaveGame* ConvertToCurrent(const UObject& Frozen,FString& Error,FConversionStats* Stats=nullptr);
}
