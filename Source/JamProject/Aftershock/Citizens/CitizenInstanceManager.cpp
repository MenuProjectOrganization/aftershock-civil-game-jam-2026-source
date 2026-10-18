// AFTERSHOCK — legacy actor facade; there is exactly one timer/ISM citizen owner.
#include "Aftershock/Citizens/CitizenInstanceManager.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/World.h"

ACitizenInstanceManager::ACitizenInstanceManager()
{
	PrimaryActorTick.bCanEverTick = false;
	CitizenMesh = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("CitizenMesh"));
	RootComponent = CitizenMesh;
	CitizenMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CitizenMesh->SetCanEverAffectNavigation(false);
	CitizenMesh->SetCastShadow(false);
	CitizenMesh->SetVisibility(false);
}

void ACitizenInstanceManager::BeginPlay()
{
	Super::BeginPlay();
	SetActorTickEnabled(false);
	// Old maps may serialize instances. Hide and remove them instead of creating
	// duplicate population, independent schedules or per-person actors.
	Citizens.Reset();
	CitizenMesh->ClearInstances();
	CitizenMesh->SetVisibility(false);
}

void ACitizenInstanceManager::Populate(AGridManager* InGridManager)
{
	GridManager = InGridManager; // Serialized legacy reference only, never a data source.
	Citizens.Reset();
	CitizenMesh->ClearInstances();
	CitizenMesh->SetVisibility(false);
	if (UCrowdRenderSubsystem* Crowd = GetWorld() ? GetWorld()->GetSubsystem<UCrowdRenderSubsystem>() : nullptr)
		Crowd->SynchronizeCanonicalPopulation();
}

void ACitizenInstanceManager::BroadcastPanic(FVector Epicenter, float Radius)
{
	// The legacy radial escape vector had no valid destination contract. The
	// canonical planner now selects real aid centres and connected public routes.
	if (UCrowdRenderSubsystem* Crowd = GetWorld() ? GetWorld()->GetSubsystem<UCrowdRenderSubsystem>() : nullptr)
		Crowd->SetPanicMode(true);
}

void ACitizenInstanceManager::ClearPanic()
{
	if (UCrowdRenderSubsystem* Crowd = GetWorld() ? GetWorld()->GetSubsystem<UCrowdRenderSubsystem>() : nullptr)
		Crowd->SetPanicMode(false);
}
