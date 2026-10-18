// AFTERSHOCK — Civil Game Jam 2026
// Her yerleştirilen bina bir ABuildingActor instance'ıdır.
// UBuildingData DataAsset referansı tutar, hasar ve simülasyon hesaplamaları bu aktör üzerinde gerçekleşir.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Aftershock/Types/AftershockTypes.h"
#include "Aftershock/Simulation/DisasterMath.h"
#include "BuildingActor.generated.h"

class UStaticMeshComponent;
class UGeometryCollectionComponent;
class UBuildingData;
class UNiagaraSystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnBuildingDamaged, ABuildingActor*, Building, float, NewDamage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnBuildingCollapsed, ABuildingActor*, Building);

UCLASS(BlueprintType, Blueprintable)
class JAMPROJECT_API ABuildingActor : public AActor
{
	GENERATED_BODY()

public:
	ABuildingActor();

	// -------- BİLEŞENLER --------
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<USceneComponent> Root;

	/** Nanite aktif, M_Building_Base master materyalli sağlam bina mesh bileşeni. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UStaticMeshComponent> IntactMeshComponent;

	/** Geriye dönük uyumluluk takma adı. */
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> IntactMesh;

	/** Chaos Destruction geometri koleksiyonu. Varsayılan olarak boştur; sadece Collapse durumunda yüklenir. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UGeometryCollectionComponent> FracturedMeshComponent;

	/** Geriye dönük uyumluluk takma adı. */
	UPROPERTY()
	TObjectPtr<UGeometryCollectionComponent> FracturedMesh;

	// -------- VERİ MODELİ --------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Data")
	TObjectPtr<UBuildingData> BuildingData;

	/** Geriye dönük uyumluluk için Data referansı. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Data")
	TObjectPtr<UBuildingData> Data;

	/** Benzersiz bina kimliği (UBuildingManagerSubsystem ile eşleşir). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|State")
	int32 BuildingID = -1;

	/** Seçilen kat sayısı (FloorsMin - FloorsMax aralığı). Modüler yükseklik ve Z ölçeğini belirler. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|State", meta=(ClampMin="1"))
	int32 CurrentFloors = 1;

	/** Geriye dönük uyumluluk için Floors. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|State")
	int32 Floors = 1;

	/** Temel kalitesi (Weak, Medium, Strong). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|State")
	EFoundationQuality FoundationQuality = EFoundationQuality::Medium;

	/** Geriye dönük uyumluluk için Foundation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|State")
	EFoundationQuality Foundation = EFoundationQuality::Medium;

	/** Yaş (Tur sayısı). Her 10 tur = 1 yıl. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|State")
	int32 CurrentTurnAge = 0;

	/** Geriye dönük uyumluluk için AgeYears. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|State")
	int32 AgeYears = 0;

	/** Güçlendirilmiş mi? (Retrofit uygulandığında true olur, yaşlanma sıfırlanır, +0.2 direnç bonusu gelir). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|State")
	bool bIsRetrofitted = false;

	/** Geriye dönük uyumluluk için bRetrofitted. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|State")
	bool bRetrofitted = false;

	/** Mevcut hasar puanı (0.0 - 100.0). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|State")
	float CurrentDamage = 0.0f;

	/** Mevcut hasar aşaması (Intact, Cracked, Damaged, HeavyDamage, Collapsed). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|State")
	EBuildingDamageState CurrentDamageState = EBuildingDamageState::Intact;

	/** Anlık sel su seviyesi (cm). CPD Index 2 üzerinden M_Building_Base'e aktarılır. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|State")
	float CurrentFloodWaterLevel = -200.0f;

	/** Anlık salınım genliği. CPD Index 3 üzerinden WPO salınımını tetikler. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|State")
	float CurrentSwayIntensity = 0.0f;

	/** Çökme gerçekleşti mi? */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Aftershock|State")
	bool bCollapsed = false;

	/** Grid haritasındaki koordinat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Grid")
	FIntPoint GridCoord = FIntPoint::ZeroValue;

	/** Çökme anında ortaya çıkacak toz/parçacık Niagara efekti. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aftershock|Effects")
	TObjectPtr<UNiagaraSystem> CollapseDustFX;

	// -------- EVENTS --------
	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnBuildingDamaged OnBuildingDamaged;

	UPROPERTY(BlueprintAssignable, Category="Aftershock|Events")
	FOnBuildingCollapsed OnBuildingCollapsed;

	// -------- BÖLÜM #5 SİMÜLASYON VE HESAPLAMA API --------
	/** Bölüm #5 Dayanıklılık ve Hasar Hesaplama Fonksiyonu */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Simulation")
	float CalculateAndApplyDisasterDamage(EDisasterType DisasterType, float DisasterSeverity, float CityPreventionBonus);

	/** Hasar Eşik Yönetimi (Intact -> Cracked -> Damaged -> HeavyDamage -> Collapsed) */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Simulation")
	void UpdateDamageState();

	/** Custom Primitive Data (CPD) Güncelleme (Damage, MaterialID, Flood, Sway, FoundationQuality) */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Visual")
	void UpdateCustomPrimitiveData();

	/** Kat sayısına göre mesh ölçeğini (Z-scale) güncelle */
	UFUNCTION(BlueprintCallable, Category="Aftershock|Visual")
	void ApplyFloorScale();

	/** Temel Kalitesi Deprem Bonusu (+0.0, +0.1, +0.3) */
	UFUNCTION(BlueprintPure, Category="Aftershock|Foundation")
	float GetEarthquakeFoundationBonus() const;

	/** Temel Kalitesi Sel Bonusu (-0.1, +0.0, +0.2) */
	UFUNCTION(BlueprintPure, Category="Aftershock|Foundation")
	float GetFloodFoundationBonus() const;

	/** Hesaplama snapshot'ı üret (DisasterMath uyumluluğu için) */
	UFUNCTION(BlueprintPure, Category="Aftershock")
	FBuildingSnapshot MakeSnapshot() const;

	/** Hasar uygula. CurrentDamage artırılır, UpdateDamageState ve Collapse tetiklenebilir. */
	UFUNCTION(BlueprintCallable, Category="Aftershock")
	void ApplyDamage(float Delta);

	/** Chaos çökme sekansını başlatır: Intact gizlenir, Fractured mesh asenkron yüklenip fizik simülasyonu başlar. */
	UFUNCTION(BlueprintCallable, Category="Aftershock")
	void Collapse();

	/** Güçlendirme yap (Retrofit). Dayanıklılık sıfırlanır, +0.2 direnç bonusu eklenir. */
	UFUNCTION(BlueprintCallable, Category="Aftershock")
	void Retrofit();
	/** Canonical manager owns the memorial state; no duplicate actor enum. */
	UFUNCTION(BlueprintCallable,Category="Aftershock|Memorial") bool ClearRubbleForMemorial();
	UFUNCTION(BlueprintPure,Category="Aftershock|Memorial") EDamageState GetCanonicalDamageState() const;

	/** Tur başına vergi katkısı (Hasar aldıkça düşer, çökünce 0 olur). */
	UFUNCTION(BlueprintPure, Category="Aftershock|Economy")
	int32 GetTaxContribution() const;

	/** Tur başına bakım masrafı. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Economy")
	int32 GetMaintenanceCost() const;

	/** Tur başına CO2 katkısı. */
	UFUNCTION(BlueprintPure, Category="Aftershock|Economy")
	float GetCO2Contribution() const;

protected:
	virtual void BeginPlay() override;
	virtual void OnConstruction(const FTransform& Transform) override;

	/** MeshFractured asenkron yüklendiğinde çağrılan callback */
	UFUNCTION()
	void OnFracturedMeshLoaded();

	/** Performans optimizasyonu: Çökme sonrası fizik simülasyonunu uyku moduna al */
	UFUNCTION()
	void PutFracturedMeshToSleep();
};
