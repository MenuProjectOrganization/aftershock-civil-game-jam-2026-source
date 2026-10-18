#include "Aftershock/Public/AftershockBuildingCatalog.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"

void FAftershockBuildingCatalog::GetTypes(TArray<EBuildingType>& OutTypes)
{
	OutTypes.Reset(24);
	for (int32 Index = 0; Index <= 23; ++Index) OutTypes.Add(static_cast<EBuildingType>(Index));
}

int32 FAftershockBuildingCatalog::GetCategory(EBuildingType Type)
{
	switch (Type)
	{
	case EBuildingType::House: case EBuildingType::Apartment: case EBuildingType::Office: return 0;
	case EBuildingType::Farm: case EBuildingType::LivestockFarm: case EBuildingType::Bakery: case EBuildingType::Restaurant: return 1;
	case EBuildingType::Factory: case EBuildingType::PowerPlant: case EBuildingType::WindTurbine: case EBuildingType::Generator: case EBuildingType::Substation: case EBuildingType::Warehouse: return 2;
	case EBuildingType::Hospital: case EBuildingType::School: case EBuildingType::FireStation: case EBuildingType::Shelter:
	case EBuildingType::TaxOffice: case EBuildingType::Courthouse: case EBuildingType::PoliceStation: return 3;
	default: return 4;
	}
}

FText FAftershockBuildingCatalog::GetCategoryName(int32 Category)
{
	static const TCHAR* Names[] = { TEXT("KONUT / İŞ"), TEXT("GIDA / TARIM"), TEXT("ENERJİ / ÜRETİM"), TEXT("KAMU HİZMETİ"), TEXT("ULAŞIM / ÇEVRE") };
	return FText::FromString(Names[FMath::Clamp(Category, 0, 4)]);
}

const TCHAR* FAftershockBuildingCatalog::GetDefaultMeshPath(EBuildingType Type)
{
	switch (Type)
	{
	case EBuildingType::House: return TEXT("/Game/Buildings/Meshes/SM_House_01.SM_House_01");
	case EBuildingType::Apartment: return TEXT("/Game/Buildings/Meshes/SM_Apartment_01.SM_Apartment_01");
	case EBuildingType::Office: return TEXT("/Game/Buildings/Meshes/SM_Office_01.SM_Office_01");
	case EBuildingType::Factory: return TEXT("/Game/Buildings/Meshes/SM_Factory_01.SM_Factory_01");
	case EBuildingType::Hospital: return TEXT("/Game/Buildings/Meshes/SM_Hospital_01.SM_Hospital_01");
	case EBuildingType::School: return TEXT("/Game/Buildings/Meshes/SM_School_01.SM_School_01");
	case EBuildingType::FireStation: return TEXT("/Game/Buildings/Meshes/SM_FireStation_01.SM_FireStation_01");
	case EBuildingType::PowerPlant: return TEXT("/Game/Buildings/Meshes/SM_PowerPlant_01.SM_PowerPlant_01");
	case EBuildingType::GreenSpace: return TEXT("/Game/Buildings/Meshes/SM_GreenSpace_01.SM_GreenSpace_01");
	case EBuildingType::WindTurbine: return TEXT("/Game/Buildings/Meshes/Services/SM_WindTurbine.SM_WindTurbine");
	case EBuildingType::Shelter: return TEXT("/Game/Buildings/Meshes/Services/SM_Shelter.SM_Shelter");
	case EBuildingType::Farm: return TEXT("/Game/Buildings/Meshes/Services/SM_Farm.SM_Farm");
	case EBuildingType::LivestockFarm: return TEXT("/Game/Buildings/Meshes/Services/SM_LivestockFarm.SM_LivestockFarm");
	case EBuildingType::Generator: return TEXT("/Game/Buildings/Meshes/Services/SM_Generator.SM_Generator");
	case EBuildingType::Bakery: return TEXT("/Game/Buildings/Meshes/Services/SM_Bakery.SM_Bakery");
	case EBuildingType::Restaurant: return TEXT("/Game/Buildings/Meshes/Services/SM_Restaurant.SM_Restaurant");
	case EBuildingType::TaxOffice: return TEXT("/Game/Buildings/Meshes/Services/SM_TaxOffice.SM_TaxOffice");
	case EBuildingType::Courthouse: return TEXT("/Game/Buildings/Meshes/Services/SM_Courthouse.SM_Courthouse");
	case EBuildingType::PoliceStation: return TEXT("/Game/Buildings/Meshes/Services/SM_PoliceStation.SM_PoliceStation");
	case EBuildingType::BusTerminal: return TEXT("/Game/Buildings/Meshes/Services/SM_BusTerminal.SM_BusTerminal");
	case EBuildingType::BusStop: return TEXT("/Game/Buildings/Meshes/Services/SM_BusStop.SM_BusStop");
	case EBuildingType::WaterPlant: return TEXT("/Game/Buildings/Meshes/Services/SM_WaterPlant.SM_WaterPlant");
	case EBuildingType::Warehouse: return TEXT("/Game/Buildings/Meshes/Services/SM_Warehouse.SM_Warehouse");
	case EBuildingType::Substation: return TEXT("/Game/Buildings/Meshes/Services/SM_Substation.SM_Substation");
	default: return nullptr;
	}
}

UBuildingData* FAftershockBuildingCatalog::CreateData(UObject* Owner, EBuildingType Type)
{
	if (!Owner || static_cast<uint8>(Type) > 23) return nullptr;
	UBuildingData* D = NewObject<UBuildingData>(Owner);
	D->Type = Type; D->Residents = 0; D->Workers = 0; D->FloorsMin = 1; D->FloorsMax = 1;
	D->Material = EMaterialType::Concrete;
	const auto Stats = [D](const TCHAR* Name, int32 Cost, int32 Tax, int32 Maintenance, int32 Residents, int32 Workers, int32 MinFloors, int32 MaxFloors, EMaterialType Material)
	{
		D->DisplayName = FText::FromString(Name); D->CostTL = Cost; D->TaxPerTurn = Tax; D->MaintenancePerTurn = Maintenance;
		D->Residents = Residents; D->Workers = Workers; D->FloorsMin = MinFloors; D->FloorsMax = MaxFloors; D->Material = Material;
	};
	switch (Type)
	{
	case EBuildingType::House: Stats(TEXT("Küçük Ev"),1000,50,10,4,0,1,2,EMaterialType::Wood); D->CO2PerTurn=.2f; D->PowerDemandUnitsPerTurn=2; break;
	case EBuildingType::Apartment: Stats(TEXT("Apartman"),5000,300,50,35,0,4,10,EMaterialType::Concrete); D->CO2PerTurn=.8f; D->PowerDemandUnitsPerTurn=12; break;
	case EBuildingType::Office: Stats(TEXT("Ofis"),8000,500,80,0,65,5,15,EMaterialType::Steel); D->CO2PerTurn=1.5f; D->PowerDemandUnitsPerTurn=18; break;
	case EBuildingType::Factory: Stats(TEXT("Üretim Tesisi"),10000,700,100,0,60,2,4,EMaterialType::Steel); D->CO2PerTurn=8; D->PowerDemandUnitsPerTurn=25; break;
	case EBuildingType::Hospital: Stats(TEXT("Hastane"),15000,0,200,0,40,3,6,EMaterialType::Concrete); D->CO2PerTurn=1.2f; D->SatisfactionPerTurn=2; D->RescueSpeedMultiplier=2; D->PowerDemandUnitsPerTurn=20; break;
	case EBuildingType::School: Stats(TEXT("Okul"),7000,0,70,0,30,2,4,EMaterialType::Concrete); D->CO2PerTurn=.6f; D->SatisfactionPerTurn=1; D->PowerDemandUnitsPerTurn=10; break;
	case EBuildingType::FireStation: Stats(TEXT("İtfaiye"),6000,0,80,0,15,2,3,EMaterialType::Concrete); D->CO2PerTurn=.4f; D->SatisfactionPerTurn=.5f; D->RescueSpeedMultiplier=1.5f; D->PowerDemandUnitsPerTurn=6; break;
	case EBuildingType::PowerPlant: Stats(TEXT("Enerji Santrali"),12000,600,150,0,25,1,2,EMaterialType::Steel); D->CO2PerTurn=3; D->PowerProductionUnitsPerTurn=240; D->PowerDemandUnitsPerTurn=0; break;
	case EBuildingType::GreenSpace: Stats(TEXT("Mahalle Parkı"),2000,0,20,0,0,1,1,EMaterialType::Wood); D->CO2PerTurn=-1.5f; D->SatisfactionPerTurn=1.5f; D->PowerDemandUnitsPerTurn=0; break;
	case EBuildingType::WindTurbine: Stats(TEXT("Rüzgâr Türbini"),11000,0,75,0,4,1,1,EMaterialType::Steel); D->PowerProductionUnitsPerTurn=65; D->PowerDemandUnitsPerTurn=0; break;
	case EBuildingType::Shelter: Stats(TEXT("Afet Sığınağı"),6500,0,70,0,8,1,1,EMaterialType::Concrete); D->SatisfactionPerTurn=.6f; D->RescueSpeedMultiplier=1.3f; break;
	case EBuildingType::Farm: Stats(TEXT("Tarla / Çiftlik"),4500,140,45,0,14,1,1,EMaterialType::Wood); D->FoodProductionUnitsPerTurn=90; D->PowerDemandUnitsPerTurn=4; D->CO2PerTurn=.1f; break;
	case EBuildingType::LivestockFarm: Stats(TEXT("Hayvancılık Tesisi"),6500,210,70,0,18,1,1,EMaterialType::Wood); D->FoodProductionUnitsPerTurn=24; D->PowerDemandUnitsPerTurn=6; D->CO2PerTurn=1.2f; break;
	case EBuildingType::Generator: Stats(TEXT("Yedek Jeneratör"),3500,0,90,0,2,1,1,EMaterialType::Steel); D->PowerProductionUnitsPerTurn=80; D->PowerDemandUnitsPerTurn=0; D->CO2PerTurn=1.5f; break;
	case EBuildingType::Bakery: Stats(TEXT("Mahalle Fırını"),4000,180,50,0,10,1,2,EMaterialType::Brick); D->FoodProductionUnitsPerTurn=87.5f; D->FoodProcessingInputUnitsPerTurn=70; D->FoodServiceCapacity=220; D->PowerDemandUnitsPerTurn=8; break;
	case EBuildingType::Restaurant: Stats(TEXT("Restoran"),5000,240,65,0,14,1,2,EMaterialType::Brick); D->FoodProcessingInputUnitsPerTurn=20; D->FoodServiceCapacity=160; D->PowerDemandUnitsPerTurn=6; D->SatisfactionPerTurn=.4f; break;
	case EBuildingType::TaxOffice: Stats(TEXT("Vergi Dairesi"),7000,0,100,0,16,2,3,EMaterialType::Concrete); D->PowerDemandUnitsPerTurn=5; break;
	case EBuildingType::Courthouse: Stats(TEXT("Adliye"),9500,0,130,0,22,2,3,EMaterialType::Concrete); D->SatisfactionPerTurn=.4f; D->PowerDemandUnitsPerTurn=7; break;
	case EBuildingType::PoliceStation: Stats(TEXT("Polis Merkezi"),6500,0,90,0,18,2,3,EMaterialType::Concrete); D->SatisfactionPerTurn=.5f; D->PowerDemandUnitsPerTurn=5; break;
	case EBuildingType::BusTerminal: Stats(TEXT("Otobüs Terminali"),6500,140,80,0,16,1,1,EMaterialType::Steel); D->TransitCapacity=40; D->PowerDemandUnitsPerTurn=5; break;
	case EBuildingType::BusStop: Stats(TEXT("Otobüs Durağı"),500,0,8,0,0,1,1,EMaterialType::Steel); D->TransitCapacity=12; D->PowerDemandUnitsPerTurn=0; break;
	case EBuildingType::WaterPlant: Stats(TEXT("Su Arıtma Tesisi"),7500,0,95,0,10,1,1,EMaterialType::Concrete); D->PowerDemandUnitsPerTurn=12; break;
	case EBuildingType::Warehouse: Stats(TEXT("Erzak Deposu"),4500,80,40,0,6,1,1,EMaterialType::Prefab); D->PowerDemandUnitsPerTurn=3; break;
	case EBuildingType::Substation: Stats(TEXT("Trafo Merkezi"),3000,0,35,0,3,1,1,EMaterialType::Steel); D->PowerDemandUnitsPerTurn=0; break;
	default: return nullptr;
	}
	if (const TCHAR* MeshPath = GetDefaultMeshPath(Type)) D->MeshIntact = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(MeshPath));
	D->MeshFractured = UBuildingManagerSubsystem::GetDefaultFracturedMesh(Type);
	static const TCHAR* ExistingIcons[] = { TEXT("House"),TEXT("Apartment"),TEXT("Office"),TEXT("Factory"),TEXT("Hospital"),TEXT("School"),TEXT("FireStation"),TEXT("PowerPlant"),TEXT("GreenSpace") };
	if (static_cast<uint8>(Type) < UE_ARRAY_COUNT(ExistingIcons))
		D->IconThumbnail = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(FString::Printf(TEXT("/Game/UI/Icons/Buildings/T_Icon_%s_01.T_Icon_%s_01"), ExistingIcons[static_cast<uint8>(Type)], ExistingIcons[static_cast<uint8>(Type)])));
	else
	{
		static const TCHAR* ServiceNames[] = { TEXT("WindTurbine"),TEXT("Shelter"),TEXT("Farm"),TEXT("LivestockFarm"),TEXT("Generator"),TEXT("Bakery"),TEXT("Restaurant"),TEXT("TaxOffice"),TEXT("Courthouse"),TEXT("PoliceStation"),TEXT("BusTerminal"),TEXT("BusStop"),TEXT("WaterPlant"),TEXT("Warehouse"),TEXT("Substation") };
		const TCHAR* Name = ServiceNames[static_cast<uint8>(Type)-9];
		D->IconThumbnail = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(FString::Printf(TEXT("/Game/UI/Icons/Buildings/T_Icon_%s_01.T_Icon_%s_01"),Name,Name)));
	}
	D->Description = FText::FromString(TEXT("İşleyiş, yol bağlantısı, personel, enerji ve yerel hizmet koşulları şehir yaşam sistemi tarafından değerlendirilir. Kapasite ve üretim değerleri oyun birimidir."));
	return D;
}
