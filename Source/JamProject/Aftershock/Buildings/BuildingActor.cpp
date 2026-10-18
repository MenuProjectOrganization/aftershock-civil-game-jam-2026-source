// AFTERSHOCK — Civil Game Jam 2026
// ABuildingActor implementation.

#include "Aftershock/Buildings/BuildingActor.h"
#include "Aftershock/Data/BuildingData.h"
#include "Components/StaticMeshComponent.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "TimerManager.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/AftershockAudioSubsystem.h"
#include "Engine/GameInstance.h"

ABuildingActor::ABuildingActor()
{
	PrimaryActorTick.bCanEverTick = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	// 1. Intact Mesh Component (Varsayılan aktif, Nanite ve M_Building_Base)
	IntactMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("IntactMeshComponent"));
	IntactMeshComponent->SetupAttachment(Root);
	IntactMeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	IntactMeshComponent->SetCollisionObjectType(ECC_WorldStatic);
	IntactMesh = IntactMeshComponent;

	// 2. Fractured Mesh Component (Varsayılan boş ve devre dışı, sadece Damage >= 80 anında asenkron yüklenir)
	FracturedMeshComponent = CreateDefaultSubobject<UGeometryCollectionComponent>(TEXT("FracturedMeshComponent"));
	FracturedMeshComponent->SetupAttachment(Root);
	FracturedMeshComponent->SetVisibility(false);
	FracturedMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FracturedMeshComponent->SetSimulatePhysics(false);
	FracturedMesh = FracturedMeshComponent;

	CurrentFloors = 1;
	Floors = 1;
	FoundationQuality = EFoundationQuality::Medium;
	Foundation = EFoundationQuality::Medium;
	CurrentTurnAge = 0;
	AgeYears = 0;
	bIsRetrofitted = false;
	bRetrofitted = false;
	CurrentDamage = 0.0f;
	CurrentDamageState = EBuildingDamageState::Intact;
	CurrentFloodWaterLevel = -200.0f;
	CurrentSwayIntensity = 0.0f;
	bCollapsed = false;
}

void ABuildingActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	UBuildingData* ActiveData = BuildingData ? BuildingData : Data;
	if (ActiveData)
	{
		BuildingData = ActiveData;
		Data = ActiveData;

		if (ActiveData->MeshIntact.IsValid() && IntactMeshComponent)
		{
			IntactMeshComponent->SetStaticMesh(ActiveData->MeshIntact.Get());
		}

		if (CurrentFloors < ActiveData->FloorsMin)
		{
			CurrentFloors = ActiveData->FloorsMin;
			Floors = CurrentFloors;
		}

		ApplyFloorScale();
		UpdateCustomPrimitiveData();
	}
}

void ABuildingActor::BeginPlay()
{
	Super::BeginPlay();

	UBuildingData* ActiveData = BuildingData ? BuildingData : Data;
	if (ActiveData)
	{
		BuildingData = ActiveData;
		Data = ActiveData;

		if (ActiveData->MeshIntact.IsValid() && IntactMeshComponent)
		{
			IntactMeshComponent->SetStaticMesh(ActiveData->MeshIntact.Get());
		}
	}

	ApplyFloorScale();
	UpdateDamageState();
}

void ABuildingActor::ApplyFloorScale()
{
	UBuildingData* ActiveData = BuildingData ? BuildingData : Data;
	if (ActiveData && ActiveData->FloorsMin > 0 && IntactMeshComponent)
	{
		// Kat sayısına göre modüler dikey ölçekleme
		const float ScaleZ = static_cast<float>(CurrentFloors) / static_cast<float>(ActiveData->FloorsMin);
		IntactMeshComponent->SetRelativeScale3D(FVector(1.0f, 1.0f, FMath::Max(1.0f, ScaleZ)));
	}
}

float ABuildingActor::GetEarthquakeFoundationBonus() const
{
	const EFoundationQuality ActiveQuality = FoundationQuality != EFoundationQuality::Medium ? FoundationQuality : Foundation;
	switch (ActiveQuality)
	{
		case EFoundationQuality::Weak:   return 0.0f;
		case EFoundationQuality::Medium: return 0.1f;
		case EFoundationQuality::Strong: return 0.3f;
		default:                         return 0.1f;
	}
}

float ABuildingActor::GetFloodFoundationBonus() const
{
	const EFoundationQuality ActiveQuality = FoundationQuality != EFoundationQuality::Medium ? FoundationQuality : Foundation;
	switch (ActiveQuality)
	{
		case EFoundationQuality::Weak:   return -0.1f;
		case EFoundationQuality::Medium: return 0.0f;
		case EFoundationQuality::Strong: return 0.2f;
		default:                         return 0.0f;
	}
}

float ABuildingActor::CalculateAndApplyDisasterDamage(EDisasterType DisasterType, float DisasterSeverity, float CityPreventionBonus)
{
	if (bCollapsed) return 0.0f;

	UBuildingData* ActiveData = BuildingData ? BuildingData : Data;
	if (!ActiveData) return 0.0f;

	// Bölüm #5 formül uygulaması
	const float BaseRes = ActiveData->GetBaseResistance(DisasterType);
	const float FoundationBonus = (DisasterType == EDisasterType::Earthquake) ? GetEarthquakeFoundationBonus() :
	                              (DisasterType == EDisasterType::Flood)      ? GetFloodFoundationBonus() : 0.0f;
	const float HeightPenalty = FMath::Max(0, CurrentFloors - 1) * 0.03f;
	const int32 AgeInYears = (CurrentTurnAge > 0) ? (CurrentTurnAge / 10) : AgeYears;
	const bool bRetro = bIsRetrofitted || bRetrofitted;
	const float AgeDecay = bRetro ? 1.0f : FMath::Max(0.5f, 1.0f - (AgeInYears / 10.0f) * 0.05f);
	const float RetrofitBonus = bRetro ? 0.2f : 0.0f;

	const float TotalResistance = FMath::Clamp((BaseRes + FoundationBonus + RetrofitBonus - HeightPenalty) * AgeDecay, 0.05f, 0.98f);
	const float Damage = FMath::Clamp(DisasterSeverity * (1.0f - TotalResistance) - CityPreventionBonus, 0.0f, 100.0f);

	ApplyDamage(Damage);
	return Damage;
}

void ABuildingActor::ApplyDamage(float Delta)
{
	if (bCollapsed||GetCanonicalDamageState()==EDamageState::Memorial) return;

	const float PreviousDamage = CurrentDamage;
	CurrentDamage = FMath::Clamp(CurrentDamage + Delta, 0.0f, 100.0f);
	UpdateDamageState();
	// Play each damage cue only when crossing a meaningful visual state threshold;
	// repeated small simulation updates cannot spam audio.
	if ((PreviousDamage < 20.0f && CurrentDamage >= 20.0f) ||
		(PreviousDamage < 50.0f && CurrentDamage >= 50.0f))
	{
		if (UWorld* World = GetWorld())
		{
			if (UGameInstance* GI = World->GetGameInstance())
			{
				if (UAftershockAudioSubsystem* Audio = GI->GetSubsystem<UAftershockAudioSubsystem>())
				{
					Audio->PlayDamageTaken();
					if (PreviousDamage < 50.0f && CurrentDamage >= 50.0f)
					{
						Audio->PlayBreakAtLocation(GetActorLocation());
					}
				}
			}
		}
	}

	if (BuildingID != -1)
	{
		if (UWorld* World = GetWorld())
		{
			if (UBuildingManagerSubsystem* Mgr = World->GetSubsystem<UBuildingManagerSubsystem>())
			{
				Mgr->ApplyDamageToBuilding(BuildingID, CurrentDamage);
			}
		}
	}

	OnBuildingDamaged.Broadcast(this, CurrentDamage);

	if (CurrentDamage >= 80.0f)
	{
		Collapse();
	}
}

void ABuildingActor::UpdateDamageState()
{
	if (CurrentDamage <= 0.0f)
	{
		CurrentDamageState = EBuildingDamageState::Intact;
	}
	else if (CurrentDamage <= 20.0f)
	{
		CurrentDamageState = EBuildingDamageState::Cracked;
	}
	else if (CurrentDamage <= 50.0f)
	{
		CurrentDamageState = EBuildingDamageState::Damaged;
	}
	else if (CurrentDamage < 80.0f)
	{
		CurrentDamageState = EBuildingDamageState::HeavyDamage;
	}
	else
	{
		CurrentDamageState = EBuildingDamageState::Collapsed;
	}

	UpdateCustomPrimitiveData();
}

void ABuildingActor::UpdateCustomPrimitiveData()
{
	if (!IntactMeshComponent) return;

	UBuildingData* ActiveData = BuildingData ? BuildingData : Data;
	const float MatID = ActiveData ? static_cast<float>(ActiveData->Material) : 2.0f; // Default Concrete
	const float FoundID = static_cast<float>(FoundationQuality);

	// CPD 0: DamageAmount (0.0 - 100.0)
	IntactMeshComponent->SetCustomPrimitiveDataFloat(0, CurrentDamage);
	// CPD 1: MaterialTypeID (0..4)
	IntactMeshComponent->SetCustomPrimitiveDataFloat(1, MatID);
	// CPD 2: FloodWaterLevelCm
	IntactMeshComponent->SetCustomPrimitiveDataFloat(2, CurrentFloodWaterLevel);
	// CPD 3: SwayIntensity
	IntactMeshComponent->SetCustomPrimitiveDataFloat(3, CurrentSwayIntensity);
	// CPD 4: FoundationQualityID
	IntactMeshComponent->SetCustomPrimitiveDataFloat(4, FoundID);
}

void ABuildingActor::Collapse()
{
	if (bCollapsed||GetCanonicalDamageState()==EDamageState::Memorial) return;

	bCollapsed = true;
	CurrentDamage = 100.0f;
	CurrentDamageState = EBuildingDamageState::Collapsed;

	// 1. Sağlam mesh'i gizle ve çarpışmasını kapat
	if (IntactMeshComponent)
	{
		IntactMeshComponent->SetVisibility(false);
		IntactMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	// 2. Niagara toz patlaması spawn et
	if (CollapseDustFX)
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			GetWorld(),
			CollapseDustFX,
			GetActorLocation(),
			GetActorRotation()
		);
	}

	// 2b. Collapse ses katmanları — Kenney CC0 sound effects:
	//     a) Explosion (dramatic boom, FPS enemy_destroy) — ilk anda
	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			if (UAftershockAudioSubsystem* Audio = GI->GetSubsystem<UAftershockAudioSubsystem>())
			{
				Audio->PlayExplosionAtLocation(GetActorLocation());
			}
		}
	}

	// 3. Chaos Geometry Collection asenkron yükle ve aktifleştir
	UBuildingData* ActiveData = BuildingData ? BuildingData : Data;
	if (ActiveData && !ActiveData->MeshFractured.IsNull())
	{
		if (ActiveData->MeshFractured.IsValid())
		{
			OnFracturedMeshLoaded();
		}
		else
		{
			FStreamableManager& Streamable = UAssetManager::GetStreamableManager();
			Streamable.RequestAsyncLoad(
				ActiveData->MeshFractured.ToSoftObjectPath(),
				FStreamableDelegate::CreateUObject(this, &ABuildingActor::OnFracturedMeshLoaded)
			);
		}
	}

	OnBuildingCollapsed.Broadcast(this);
}

void ABuildingActor::OnFracturedMeshLoaded()
{
	UBuildingData* ActiveData = BuildingData ? BuildingData : Data;
	if (ActiveData && ActiveData->MeshFractured.IsValid() && FracturedMeshComponent)
	{
		FracturedMeshComponent->SetRestCollection(ActiveData->MeshFractured.Get());
		FracturedMeshComponent->SetVisibility(true);
		FracturedMeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		FracturedMeshComponent->SetSimulatePhysics(true);

		// Performans optimizasyonu: 6 saniye sonra simülasyonu uykuya al
		FTimerHandle SleepTimer;
		GetWorld()->GetTimerManager().SetTimer(
			SleepTimer,
			this,
			&ABuildingActor::PutFracturedMeshToSleep,
			6.0f,
			false
		);
	}
}

void ABuildingActor::PutFracturedMeshToSleep()
{
	if (FracturedMeshComponent && FracturedMeshComponent->IsSimulatingPhysics())
	{
		FracturedMeshComponent->PutAllRigidBodiesToSleep();
	}
}

void ABuildingActor::Retrofit()
{
	if(GetCanonicalDamageState()>=EDamageState::Collapsed)return;
	bIsRetrofitted = true;
	bRetrofitted = true;
	CurrentTurnAge = 0;
	AgeYears = 0;
	CurrentDamage = FMath::Max(0.0f, CurrentDamage - 30.0f);
	UpdateDamageState();
}

EDamageState ABuildingActor::GetCanonicalDamageState() const
{FBuildingInstance B;const auto* M=GetWorld()?GetWorld()->GetSubsystem<UBuildingManagerSubsystem>():nullptr;return M&&M->GetBuildingByID(BuildingID,B)?B.DamageState:static_cast<EDamageState>(CurrentDamageState);}
bool ABuildingActor::ClearRubbleForMemorial()
{
	const auto* M=GetWorld()?GetWorld()->GetSubsystem<UBuildingManagerSubsystem>():nullptr;auto* E=GetWorld()?GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>():nullptr;FBuildingInstance B;
	if(!M||!E||!M->GetBuildingByID(BuildingID,B)||!E->ClearRubbleWithBudget(B.GridCoords))return false;
	if(IntactMeshComponent)IntactMeshComponent->SetVisibility(false);
	if(FracturedMeshComponent){FracturedMeshComponent->SetVisibility(false);FracturedMeshComponent->SetSimulatePhysics(false);FracturedMeshComponent->DestroyPhysicsState();}
	return true;
}

FBuildingSnapshot ABuildingActor::MakeSnapshot() const
{
	FBuildingSnapshot Snap;
	UBuildingData* ActiveData = BuildingData ? BuildingData : Data;
	if (ActiveData)
	{
		Snap.Type = ActiveData->Type;
		Snap.Material = ActiveData->Material;
		Snap.Residents = ActiveData->Residents;
		Snap.Workers = ActiveData->Workers;
	}
	Snap.Foundation = FoundationQuality != EFoundationQuality::Medium ? FoundationQuality : Foundation;
	Snap.Floors = CurrentFloors > 0 ? CurrentFloors : Floors;
	Snap.AgeYears = CurrentTurnAge > 0 ? (CurrentTurnAge / 10) : AgeYears;
	Snap.bRetrofitted = bIsRetrofitted || bRetrofitted;
	Snap.BaseResistance = ActiveData ? ActiveData->GetBaseResistance(EDisasterType::Earthquake) : 0.3f;
	Snap.WorldLocation = GetActorLocation();
	Snap.GridCoord = GridCoord;
	return Snap;
}

int32 ABuildingActor::GetTaxContribution() const
{
	UBuildingData* ActiveData = BuildingData ? BuildingData : Data;
	if (!ActiveData || bCollapsed||GetCanonicalDamageState()>=EDamageState::Collapsed) return 0;

	// Hasarlı binalarda vergi geliri kademeli olarak düşer
	switch (CurrentDamageState)
	{
		case EBuildingDamageState::Intact:
		case EBuildingDamageState::Cracked:
			return ActiveData->TaxPerTurn;
		case EBuildingDamageState::Damaged:
			return FMath::RoundToInt(ActiveData->TaxPerTurn * 0.5f);
		case EBuildingDamageState::HeavyDamage:
		case EBuildingDamageState::Collapsed:
		default:
			return 0;
	}
}

int32 ABuildingActor::GetMaintenanceCost() const
{
	UBuildingData* ActiveData = BuildingData ? BuildingData : Data;
	if (!ActiveData || bCollapsed||GetCanonicalDamageState()>=EDamageState::Collapsed) return 0;
	return ActiveData->MaintenancePerTurn;
}

float ABuildingActor::GetCO2Contribution() const
{
	UBuildingData* ActiveData = BuildingData ? BuildingData : Data;
	if (!ActiveData || bCollapsed||GetCanonicalDamageState()>=EDamageState::Collapsed) return 0.0f;
	return ActiveData->CO2PerTurn;
}
