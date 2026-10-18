// AFTERSHOCK — Civil Game Jam 2026
// Legacy Blueprint compatibility facade. UCrowdRenderSubsystem owns all live agents.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Aftershock/Types/AftershockTypes.h"
#include "CitizenInstanceManager.generated.h"

class UHierarchicalInstancedStaticMeshComponent;
class ABuildingActor;
class AGridManager;

/** Bir vatandaşın özü — array içinde minimal state. */
USTRUCT()
struct FCitizenState
{
	GENERATED_BODY()

	UPROPERTY() FVector CurrentLoc = FVector::ZeroVector;
	UPROPERTY() FVector TargetLoc  = FVector::ZeroVector;
	UPROPERTY() float   MoveSpeed  = 100.f;
	UPROPERTY() int32   HomeIndex  = INDEX_NONE;
	UPROPERTY() int32   WorkIndex  = INDEX_NONE;
	UPROPERTY() float   NextDecisionTime = 0.f;
	UPROPERTY() bool    bPanicking = false;
	UPROPERTY() bool    bDead = false;
};

UCLASS(BlueprintType, Blueprintable)
class JAMPROJECT_API ACitizenInstanceManager : public AActor
{
	GENERATED_BODY()

public:
	ACitizenInstanceManager();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> CitizenMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Citizens", meta=(ClampMin="10", ClampMax="5000"))
	int32 MaxCitizens = 500;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Citizens")
	float TickInterval = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Citizens")
	float BaseMoveSpeed = 150.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Citizens")
	float PanicMoveSpeed = 400.f;

	UFUNCTION(BlueprintCallable, Category="Aftershock|Citizens")
	void Populate(AGridManager* InGridManager);

	/** Afet event'inde çağır — tüm vatandaşlar panik moduna geçer. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Citizens")
	void BroadcastPanic(FVector Epicenter, float Radius);

	/** Panik modunu temizle (afet bitince). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Citizens")
	void ClearPanic();

protected:
	virtual void BeginPlay() override;

	UPROPERTY()
	TArray<FCitizenState> Citizens;

	UPROPERTY()
	TObjectPtr<AGridManager> GridManager;

};
