#pragma once
#include "CoreMinimal.h"

/** Read-only renderer evidence. Zero samples never certifies visible CPD fan-out. */
struct FAftershockArchitectureAudit
{
	bool RendererPresent=false,GlobeSuppressed=false,CanonicalStrideValid=true,DerivedStrideValid=true,AllocationWithinBudget=true;
	int32 CanonicalBuildings=0,OutsideWindowBuildings=0,CollapsedOrMemorialBuildings=0,UnsupportedBuildings=0;
	int32 CanonicalInputFailures=0,FallbackBuildings=0,ExpectedRenderedBuildings=0,RenderedBuildings=0;
	int32 MissingExpectedBuildings=0,UnexpectedRenderedBuildings=0,ReferencedModules=0,CheckedModuleInstances=0,CheckedCanonicalValues=0;
	int32 MismatchedCanonicalValues=0,NonfiniteValues=0,InvalidReferences=0,DuplicateReferences=0,UnexpectedHiddenModules=0;
	int32 AllocatedSlots=0,ActiveNonzeroSlots=0,ActiveMembershipKeys=0,PooledInactiveSlots=0,VisiblePooledSlots=0,UnreferencedNonzeroSlots=0;
	TArray<int32> SampleProblemBuildingIDs;
	void Problem(int32 ID){if(SampleProblemBuildingIDs.Num()<16)SampleProblemBuildingIDs.AddUnique(ID);}
	bool HasVisualSamples() const{return CheckedModuleInstances>0;}
	bool IsValid() const
	{
		return RendererPresent&&CanonicalStrideValid&&DerivedStrideValid&&AllocationWithinBudget
			&&CanonicalInputFailures==0&&MissingExpectedBuildings==0&&UnexpectedRenderedBuildings==0
			&&MismatchedCanonicalValues==0&&NonfiniteValues==0&&InvalidReferences==0&&DuplicateReferences==0
			&&UnexpectedHiddenModules==0&&VisiblePooledSlots==0&&UnreferencedNonzeroSlots==0;
	}
};
