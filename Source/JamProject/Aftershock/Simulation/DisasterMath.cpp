// AFTERSHOCK — Civil Game Jam 2026
// Saf hesap. Hiç UObject state tutmaz.

#include "Aftershock/Simulation/DisasterMath.h"

float UDisasterMath::MaterialResistance(EMaterialType Material, EDisasterType DisasterType)
{
	// BUILDING_STATS.md Bölüm 2 tablosu
	static const float Table[5][5] = {
		// Earthquake, Flood, Wind, Fire, Tsunami
		/*Wood*/     {0.30f, 0.20f, 0.40f, 0.10f, 0.15f},
		/*Prefab*/   {0.50f, 0.40f, 0.50f, 0.60f, 0.35f},
		/*Concrete*/ {0.80f, 0.90f, 0.80f, 0.90f, 0.75f},
		/*Steel*/    {0.90f, 0.70f, 0.60f, 0.80f, 0.70f},
		/*Brick*/    {0.60f, 0.70f, 0.70f, 0.80f, 0.65f}
	};

	const int32 M = FMath::Clamp(static_cast<int32>(Material), 0, 4);
	if(DisasterType==EDisasterType::Drought)return 1.f;
	const int32 D = DisasterType==EDisasterType::Volcano?3:FMath::Clamp(static_cast<int32>(DisasterType), 0, 4);
	return Table[M][D];
}

float UDisasterMath::FoundationBonus(EFoundationQuality Quality, EDisasterType DisasterType)
{
	// BUILDING_STATS.md Bölüm 3
	switch (Quality)
	{
		case EFoundationQuality::Weak:
			return DisasterType == EDisasterType::Flood ? -0.1f : 0.f;
		case EFoundationQuality::Medium:
			return DisasterType == EDisasterType::Earthquake ? 0.1f : 0.f;
		case EFoundationQuality::Strong:
			if (DisasterType == EDisasterType::Earthquake) return 0.3f;
			if (DisasterType == EDisasterType::Flood)      return 0.2f;
			return 0.1f;
	}
	return 0.f;
}

float UDisasterMath::TotalResistance(const FBuildingSnapshot& Snap, EDisasterType DisasterType)
{
	const float Base = MaterialResistance(Snap.Material, DisasterType);
	const float Foundation = FoundationBonus(Snap.Foundation, DisasterType);
	const float HeightPenalty = FMath::Max(0, Snap.Floors - 1) * 0.03f;
	const float AgeDecay = FMath::Max(0.5f, 1.f - (Snap.AgeYears / 10.f) * 0.05f);
	const float Retrofit = Snap.bRetrofitted ? 0.2f : 0.f;

	const float Raw = (Base + Foundation + Retrofit - HeightPenalty) * AgeDecay;
	return FMath::Clamp(Raw, 0.05f, 0.98f);
}

float UDisasterMath::ClimateMultiplier(float EnvironmentScore, float MaxMultiplier)
{
	if(!FMath::IsFinite(EnvironmentScore)||!FMath::IsFinite(MaxMultiplier)||MaxMultiplier<1.f)return 1.f;
	// Env 50 nötr. Env<50 → >1. Env>50 → <1.
	const float Normalized = (50.f - EnvironmentScore) / 50.f; // -1..1
	const float Mult = 1.f + Normalized * (MaxMultiplier - 1.f);
	return FMath::Clamp(Mult, 1.f / MaxMultiplier, MaxMultiplier);
}

float UDisasterMath::EarthquakeSeverityAt(const FDisasterEvent& Event, FVector At, float FaultLineProximity)
{
	if(!FMath::IsFinite(Event.Severity)||Event.Severity<0.f||!FMath::IsFinite(Event.AffectedRadius)||Event.AffectedRadius<=0.f||!FMath::IsFinite(FaultLineProximity)||At.ContainsNaN()||Event.Epicenter.ContainsNaN())return 0.f;
	const float Dist = FVector::Dist(At, Event.Epicenter);
	const float DistFactor = FMath::Max(0.f, 1.f - (Dist / FMath::Max(1.f, Event.AffectedRadius)));
	const float ScenarioShaking=FMath::Clamp((Event.Severity-3.f)/5.5f,0.f,1.f)*100.f;
	return ScenarioShaking * DistFactor * (1.f + FMath::Clamp(FaultLineProximity,0.f,1.f) * 0.5f);
}

float UDisasterMath::FloodSeverityAt(float WaterDepthMeters, float DrainageCapacity)
{
	if(!FMath::IsFinite(WaterDepthMeters)||!FMath::IsFinite(DrainageCapacity))return 0.f;
	return FMath::Clamp(FMath::Max(0.f,WaterDepthMeters)*20.f-FMath::Clamp(DrainageCapacity,0.f,1.f)*10.f,0.f,100.f);
}

float UDisasterMath::WindSeverityAt(float WindSpeedKmh, float ValleyChannelFactor, int32 Floors)
{
	if(!FMath::IsFinite(WindSpeedKmh)||!FMath::IsFinite(ValleyChannelFactor)||WindSpeedKmh<=0.f)return 0.f;
	const float HeightMetres=FMath::Max(3.f,FMath::Clamp(Floors,1,100)*3.f);
	const float Exposure=FMath::Pow(HeightMetres/10.f,.14f);
	const float MetresPerSecond=WindSpeedKmh/3.6f*FMath::Clamp(ValleyChannelFactor,.5f,1.5f)*Exposure;
	return FMath::Clamp(.5f*1.225f*MetresPerSecond*MetresPerSecond/1531.25f*100.f,0.f,100.f);
}

float UDisasterMath::FireSeverityAt(int32 BurningTurns)
{
	return FMath::Min(100.f, FMath::Clamp(BurningTurns,0,4) * 30.f);
}

FDamageResult UDisasterMath::ComputeDamage(
	const FBuildingSnapshot& Building,
	const FDisasterEvent& Disaster,
	const FCityPreventionState& City,
	const FTerrainCell& Cell,
	float RawSeverity)
{
	FDamageResult Result;
	if(!FMath::IsFinite(RawSeverity)||RawSeverity<0.f||!FMath::IsFinite(City.AwarenessLevel)||!FMath::IsFinite(City.EmergencyCapacity)||!FMath::IsFinite(City.DrainageCapacity)||Disaster.Type==EDisasterType::Drought)return Result;

	// A municipal environmental score cannot instantly change a quake, tsunami
	// or a measured incoming gust. Climate belongs in scenario weather inputs.
	const float ScaledSeverity = RawSeverity;

	// 2) Dayanıklılık
	const float Resistance = TotalResistance(Building, Disaster.Type);

	// 3) Şehir çapı önleme bonus
	float PreventionBonus = 0.f;
	switch (Disaster.Type)
	{
		case EDisasterType::Earthquake: PreventionBonus = 0.f; break;
		case EDisasterType::Flood:      PreventionBonus = FMath::Clamp(City.DrainageCapacity,0.f,1.f) * 10.f; break;
		case EDisasterType::Wind:       PreventionBonus = 0.f; break;
		case EDisasterType::Fire:       PreventionBonus = FMath::Clamp(City.EmergencyCapacity,0.f,1.f) * 15.f; break;
	}

	// 4) Hasar
	const float Multiplier = 1.f; // Every helper now returns the same0..100 hazard domain.

	const float Damage = FMath::Clamp(ScaledSeverity * Multiplier * (1.f - Resistance) - PreventionBonus, 0.f, 100.f);
	Result.Damage = Damage;
	Result.bCollapsed = Damage >= 80.f;

	// 5) Ölüm hesabı
	const int32 AtRisk = static_cast<int32>(FMath::Clamp(static_cast<int64>(Building.Residents)+Building.Workers,static_cast<int64>(0),static_cast<int64>(MAX_int32)));
	const float PanicLevel = Damage * (1.f - FMath::Clamp(City.AwarenessLevel,0.f,1.f)) * 0.01f;
	const float Deaths = AtRisk * PanicLevel * DeathCoefficient(Damage);

	// 6) Emergency capacity ölü azaltır
	const float Rescued = Deaths * FMath::Clamp(City.EmergencyCapacity,0.f,1.f) * 0.5f;
	const int32 FinalDeaths = FMath::Max(0, FMath::RoundToInt(Deaths - Rescued));

	Result.Deaths = FinalDeaths;
	Result.Survivors = FMath::Max(0, AtRisk - FinalDeaths);

	return Result;
}

float UDisasterMath::DeathCoefficient(float Damage)
{
	// 0 hasar → 0, 100 hasar → 0.6, between lineer
	return FMath::GetMappedRangeValueClamped(FVector2D(0.f, 100.f), FVector2D(0.02f, 0.6f), Damage);
}

float UDisasterMath::SatisfactionDeltaFromDeaths(int32 Deaths)
{
	return -0.3f * static_cast<float>(Deaths);
}

int32 UDisasterMath::GovernmentAidFromDeaths(int32 Deaths)
{
	return 0;
}
int64 UDisasterMath::GovernmentAidFromSurvivors(int32 Survivors){return static_cast<int64>(FMath::Max(0,Survivors))*150;}
int64 UDisasterMath::BurialCostFromDeaths(int32 Deaths){return static_cast<int64>(FMath::Max(0,Deaths))*500;}
