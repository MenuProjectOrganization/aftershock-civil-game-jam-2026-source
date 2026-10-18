// AFTERSHOCK — Civil Game Jam 2026
// Dünya grid'inin sahibi. Terrain cell'leri + bina yerleştirme + sorgulama.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Aftershock/Types/AftershockTypes.h"
#include "GridManager.generated.h"

class ABuildingActor;
class UBuildingData;

UCLASS(BlueprintType, Blueprintable)
class JAMPROJECT_API AGridManager : public AActor
{
	GENERATED_BODY()

public:
	AGridManager();

	// -------- GRID PARAMETRELERİ --------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grid")
	FIntPoint GridSize = FIntPoint(50, 50);

	/** Bir hücrenin dünya boyutu (cm). Default 500cm = 5m. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grid")
	float CellSizeCm = 500.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grid")
	FVector GridOrigin = FVector::ZeroVector;

	// -------- TERRAIN DATA --------
	/** GridSize.X × GridSize.Y hücre. Index = X + Y * GridSize.X. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain")
	TArray<FTerrainCell> Cells;

	// -------- BİNA REGİSTRY --------
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Buildings")
	TMap<FIntPoint, TObjectPtr<ABuildingActor>> BuildingByCoord;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Buildings")
	TArray<TObjectPtr<ABuildingActor>> AllBuildings;

	// -------- COORDINATE CONVERT --------
	UFUNCTION(BlueprintPure, Category="Aftershock|Grid")
	FVector CoordToWorld(FIntPoint Coord) const;

	UFUNCTION(BlueprintPure, Category="Aftershock|Grid")
	FIntPoint WorldToCoord(FVector World) const;

	UFUNCTION(BlueprintPure, Category="Aftershock|Grid")
	bool IsCoordValid(FIntPoint Coord) const;

	UFUNCTION(BlueprintPure, Category="Aftershock|Grid")
	bool IsCoordFree(FIntPoint Coord) const;

	UFUNCTION(BlueprintPure, Category="Aftershock|Grid")
	FTerrainCell GetCell(FIntPoint Coord) const;

	UFUNCTION(BlueprintCallable, Category="Aftershock|Grid")
	void SetCell(FIntPoint Coord, const FTerrainCell& Cell);

	// -------- BİNA YERLEŞTİRME --------
	/** Bir binayı yerleştirir. Başarıysa oluşturulan actor'ü döndürür, yoksa nullptr. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Build")
	ABuildingActor* PlaceBuilding(UBuildingData* BuildingData, FIntPoint Coord, TSubclassOf<ABuildingActor> ActorClass);
	/** Native adapter preserves the live preview's selected foundation and floor quote. */
	ABuildingActor* PlaceBuildingWithOptions(UBuildingData* BuildingData, FIntPoint Coord, TSubclassOf<ABuildingActor> ActorClass, EFoundationQuality Foundation, int32 Floors, float Yaw = 0.f);

	UFUNCTION(BlueprintCallable, Category="Aftershock|Build")
	bool DemolishBuilding(FIntPoint Coord);

	UFUNCTION(BlueprintPure, Category="Aftershock|Build")
	ABuildingActor* GetBuildingAt(FIntPoint Coord) const;

	// -------- TERRAIN GENERATION --------
	/** Basit terrain init — tüm cell'leri default değerle doldur. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Terrain")
	void InitializeFlatTerrain();

	/** Nehir yatağı tanımla (hücre koordinatları). */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Terrain")
	void MarkRiver(const TArray<FIntPoint>& RiverCoords, float ProximityRadius = 2.f);

	/** Vadi tanımla. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Terrain")
	void MarkValley(const TArray<FIntPoint>& ValleyCoords, float ChannelFactor = 1.8f);

	/** Fay hattı tanımla. */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Terrain")
	void MarkFaultLine(const TArray<FIntPoint>& FaultCoords, float ProximityRadius = 3.f);

protected:
	virtual void BeginPlay() override;

	int32 CoordToIndex(FIntPoint Coord) const;
};
