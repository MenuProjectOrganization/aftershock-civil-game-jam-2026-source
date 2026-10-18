#pragma once
#include "AftershockHydrologySolverReferenceTests.h"
namespace AftershockWater
{
// Engine/Core initialized, GAME THREAD only. Isolated plain-data instances;
// no world, player preferences, real saves, terrain, actors or assets mutated.
// Pumps real ThreadPool tasks with a five-second timeout per isolated fixture.
std::vector<ReferenceCheck> RunHydrologyAsyncStepperReferenceChecks();
}
