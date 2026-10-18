#pragma once
#include <string>
#include <vector>
namespace AftershockWater
{
struct ReferenceCheck { std::string Name; bool Passed = false; };
// Pure CPU, isolated states. No engine, player settings, save slots or assets.
// Prepared only; no acceptance is implied until the caller actually runs it.
std::vector<ReferenceCheck> RunHydrologyReferenceChecks();
std::vector<ReferenceCheck> RunUrbanPorosityReferenceChecks();
}
