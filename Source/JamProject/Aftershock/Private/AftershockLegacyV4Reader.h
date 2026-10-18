#pragma once
#include "CoreMinimal.h"
class UAftershockSaveGame;
namespace AftershockLegacyV4
{
    struct FReadDiagnostics
    {int32 BodyBytes=0,Names=0;int64 FirstContainerCountOffset=INDEX_NONE;int64 Elements=0,AllocationBudgetUsed=0;bool bExactLegacyBodyRoundtrip=false;double HeaderMs=0.,DtoDecodeMs=0.,AuditRoundtripMs=0.,ConvertMs=0.;};
    /** Indexed payload2 only; caller already verified/decompressed envelope.
     * Frozen DTO decoder with per-container limits, never current SerializeBin.
     * Optional raw-roundtrip is an audit gate, not an ordinary gameplay write. */
    UAftershockSaveGame* DecodePayload(const TArray<uint8>& Payload,FString& Error,
        FReadDiagnostics* Diagnostics=nullptr,bool bRequireExactBodyRoundtrip=false);
}
