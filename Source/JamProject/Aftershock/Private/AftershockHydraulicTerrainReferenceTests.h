#pragma once
#include "CoreMinimal.h"
class UTerrainGridSubsystem;
class UStaticMeshComponent;
struct FAftershockHydraulicTerrainCheck { FString Name; bool bPassed=false; };

// Caller MUST supply an isolated disposable native-test terrain.
// This function seeds only that fixture; it does not obtain a gameplay world.
TArray<FAftershockHydraulicTerrainCheck> RunAftershockHydraulicTerrainReferenceChecks(UTerrainGridSubsystem& FixtureTerrain);

// Root calls AFTER actual shared-ground mesh build/collision cook in its isolated
// world. A mathematical height check is not substituted for an engine trace.
TArray<FAftershockHydraulicTerrainCheck> RunAftershockHydraulicGroundCollisionChecks(
	const UTerrainGridSubsystem& FixtureTerrain,UStaticMeshComponent& BuiltGround);
#if WITH_EDITOR
bool RunAftershockHydraulicTerrainFixtures(const FString& AbsoluteReportPath);
#endif
