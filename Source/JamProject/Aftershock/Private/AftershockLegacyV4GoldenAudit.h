#pragma once
#include "CoreMinimal.h"
class UAftershockSaveGame;
namespace AftershockLegacyV4
{
    /** Explicit fixed .savfixture inputs only. No normal save-slot access. */
    int32 RunGoldenAudit(TFunctionRef<UAftershockSaveGame*(const TArray<uint8>&)> CurrentEnvelopeReader);
}
