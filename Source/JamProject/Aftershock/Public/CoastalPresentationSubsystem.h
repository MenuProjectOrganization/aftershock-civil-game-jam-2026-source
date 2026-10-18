#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"
#include "CoastalPresentationSubsystem.generated.h"

class UStaticMeshComponent;
class UInstancedStaticMeshComponent;
class UTerrainGridSubsystem;

/** One zero-tick host for the canonical sea and all installed dam props. */
UCLASS(NotBlueprintable, Transient)
class JAMPROJECT_API AAftershockCoastalPresentationActor final : public AActor
{
	GENERATED_BODY()
public:
	AAftershockCoastalPresentationActor();
	void Rebuild(const UTerrainGridSubsystem& Terrain);
	FString DescribeVisuals() const;
private:
	UPROPERTY() TObjectPtr<UStaticMeshComponent> SeaSurface;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> ConcreteParts;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> SteelParts;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> ReservoirParts;
	int32 OceanCells = 0;
	int32 InstalledDams = 0;
	int32 OvertoppedDams = 0;
};

/** Read-only infrastructure presentation; all purchases/storage remain canonical. */
UCLASS()
class JAMPROJECT_API UCoastalPresentationSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	UFUNCTION(BlueprintPure, Category="Aftershock|Presentation") FString DescribeCurrentVisuals() const;
	void RefreshVisiblePresentation() { Refresh(); }
private:
	void Refresh();
	UFUNCTION() void HandleDamChanged(FIntPoint Coord);
	UPROPERTY() TObjectPtr<AAftershockCoastalPresentationActor> PresentationActor;
	FTimerHandle RefreshTimer;
	uint32 LastSignature = 0;
	bool bHasSignature = false;
};
