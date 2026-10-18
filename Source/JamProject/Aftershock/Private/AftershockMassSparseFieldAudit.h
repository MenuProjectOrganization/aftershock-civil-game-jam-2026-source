#pragma once
#include "CoreMinimal.h"
#include "AftershockMassMovement.h"

// Original plain diagnostics, not UPROPERTY/save/physics authority. Frame scans
// are recorded separately so replay detail never changes numerical acceptance.
struct FAftershockMassFieldWorkAudit
{
    int64 ScratchInitializedCells=0,RecordCalls=0;
    int64 SparseClearedIndices=0,SparseScatteredIndices=0,SparseUpdatedIndices=0;
    int64 SparseSortComparisons=0,SparseSortReservedUnits=0;
    int64 DenseResetAndScanUnits=0;
    int64 ReplayFrameCalls=0,ReplayCellScanIndices=0;
    int32 PeakTouchedIndices=0;
};
#if WITH_EDITOR
// Same numerical kernel. The dense reference uses the original RecordFields
// arithmetic, solely in native fixtures; there is no production setting/CVar.
FAftershockMassResult SolveAftershockMassFieldsForAudit(const FAftershockMassInput&,
    bool bDenseReference,FAftershockMassFieldWorkAudit& OutWork);
bool RunAftershockMassSparseFieldFixtures(const FString& AbsoluteReportPath);
#endif
