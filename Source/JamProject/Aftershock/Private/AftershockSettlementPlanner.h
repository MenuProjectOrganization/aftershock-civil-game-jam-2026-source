#pragma once

#include "CoreMinimal.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"

class UTerrainGridSubsystem;
class UBuildingManagerSubsystem;

/** Pure bounded startup planning. Does not place structures, roads or people,
 * charge money, flatten terrain, generate water or mutate a hazard field.
 * The original essential-service lots are retained verbatim at the array front.
 */
void ExpandAftershockSettlementPlan(const UTerrainGridSubsystem& Terrain,
	UBuildingManagerSubsystem& Buildings,const TSet<FIntPoint>& ConnectedLand,
	FIntPoint CivicCentre,int32 Seed,int32 RegionIndex,int32 FocusIndex,TArray<FAftershockScenarioLot>& Lots,
	TArray<FIntPoint>& OutPublicCorridors,FAftershockCityPlanStatistics& OutStatistics);

uint32 HashAftershockSettlementPlan(const TArray<FAftershockScenarioLot>& Lots,const TArray<FIntPoint>& Corridors);
