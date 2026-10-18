#pragma once

#include "CoreMinimal.h"
#include "Aftershock/Public/BuildingData.h"

/** Shared authored building definitions. Instances and purchases stay in the existing subsystems. */
struct JAMPROJECT_API FAftershockBuildingCatalog
{
	static void GetTypes(TArray<EBuildingType>& OutTypes);
	static UBuildingData* CreateData(UObject* Owner, EBuildingType Type);
	static int32 GetCategory(EBuildingType Type);
	static FText GetCategoryName(int32 Category);
	static const TCHAR* GetDefaultMeshPath(EBuildingType Type);
};
