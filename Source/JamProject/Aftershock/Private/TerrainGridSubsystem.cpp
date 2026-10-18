// AFTERSHOCK — Civil Game Jam 2026
// Arazi Grid Sistemi, Dinamik Afet ve Önleme Alt Sistemi Uygulaması

#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/AftershockRegionRules.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "Aftershock/Public/CityLifeSubsystem.h"
#include "Engine/World.h"
#include "Math/UnrealMathUtility.h"
#include "UObject/UnrealType.h"

namespace
{
	bool SampleVersionedGeodata(int32 Version,int32 Region,int32 Focus,double Latitude,double Longitude,FAftershockGeographicTerrainSample& Out)
	{
		return Version==3?TrySampleAftershockFocusDetailGeodata(Region,Focus,Latitude,Longitude,Out)
			:Version==2&&TrySampleAftershockFocusGeodata(Region,Focus,Latitude,Longitude,Out);
	}
}

FIntPoint UTerrainGridSubsystem::CellToChunk(FIntPoint GlobalCell)
{
	const auto FloorDivide=[](int32 Value) { return Value/ChunkSizeCells-(Value%ChunkSizeCells<0 ? 1 : 0); };
	return FIntPoint(FloorDivide(GlobalCell.X),FloorDivide(GlobalCell.Y));
}

FIntPoint UTerrainGridSubsystem::CellToChunkLocal(FIntPoint GlobalCell)
{
	const auto PositiveRemainder=[](int32 Value) { return (Value%ChunkSizeCells+ChunkSizeCells)%ChunkSizeCells; };
	return FIntPoint(PositiveRemainder(GlobalCell.X),PositiveRemainder(GlobalCell.Y));
}

void UTerrainGridSubsystem::BuildChunkManifest(const TArray<FGridCell>& InCells,TArray<FAftershockTerrainChunkState>& Out)
{
	TMap<FIntPoint,FAftershockTerrainChunkState> Manifest;
	for (const FGridCell& Cell:InCells)
	{
		const FIntPoint Key=CellToChunk(Cell.Coords);
		FAftershockTerrainChunkState& Chunk=Manifest.FindOrAdd(Key);
		Chunk.ChunkCoord=Key; ++Chunk.CellCount;
		Chunk.bContainsAuthoredContinuation|=Cell.bAuthoredContinuation;
	}
	Manifest.GenerateValueArray(Out);
	Out.Sort([](const FAftershockTerrainChunkState& A,const FAftershockTerrainChunkState& B)
	{
		return A.ChunkCoord.X==B.ChunkCoord.X ? A.ChunkCoord.Y<B.ChunkCoord.Y : A.ChunkCoord.X<B.ChunkCoord.X;
	});
}

void UTerrainGridSubsystem::RebuildStorageLookup() const
{
	CellStorageIndices.Empty(Cells.Num());
	ResidentCellBounds=FIntRect(0,0,0,0);
	bool bFirst=true;
	for (int32 I=0;I<Cells.Num();++I)
	{
		const FIntPoint Coord=Cells[I].Coords;
		CellStorageIndices.Add(Coord,I);
		if (bFirst) { ResidentCellBounds=FIntRect(Coord,Coord+FIntPoint(1,1)); bFirst=false; }
		else
		{
			ResidentCellBounds.Min.X=FMath::Min(ResidentCellBounds.Min.X,Coord.X);
			ResidentCellBounds.Min.Y=FMath::Min(ResidentCellBounds.Min.Y,Coord.Y);
			ResidentCellBounds.Max.X=FMath::Max(ResidentCellBounds.Max.X,Coord.X+1);
			ResidentCellBounds.Max.Y=FMath::Max(ResidentCellBounds.Max.Y,Coord.Y+1);
		}
	}
	BuildChunkManifest(Cells,ResidentChunks);
	CachedStorageCount=Cells.Num(); CachedInitialSize=FIntPoint(GridWidth,GridHeight);
}

void UTerrainGridSubsystem::EnsureStorageLookup() const
{
	if (CachedStorageCount!=Cells.Num() || CachedInitialSize!=FIntPoint(GridWidth,GridHeight)) RebuildStorageLookup();
}

int32 UTerrainGridSubsystem::CoordToIndex(FIntPoint Coords) const
{
	// The initial prefix retains its exact historical storage IDs. Never use this
	// arithmetic for an appended global coordinate, even if the bounds grew.
	if (Coords.X>=0 && Coords.Y>=0 && Coords.X<GridWidth && Coords.Y<GridHeight)
	{
		const int64 InitialIndex=static_cast<int64>(Coords.Y)*GridWidth+Coords.X;
		if (InitialIndex<Cells.Num() && Cells[static_cast<int32>(InitialIndex)].Coords==Coords) return static_cast<int32>(InitialIndex);
	}
	EnsureStorageLookup();
	const int32* Found=CellStorageIndices.Find(Coords);
	return Found && Cells.IsValidIndex(*Found) && Cells[*Found].Coords==Coords ? *Found : INDEX_NONE;
}

FIntPoint UTerrainGridSubsystem::IndexToCoord(int32 StorageIndex) const
{
	return Cells.IsValidIndex(StorageIndex) ? Cells[StorageIndex].Coords : FIntPoint(MAX_int32,MAX_int32);
}

FIntRect UTerrainGridSubsystem::GetResidentCellBounds() const
{
	EnsureStorageLookup(); return ResidentCellBounds;
}

const TArray<FAftershockTerrainChunkState>& UTerrainGridSubsystem::GetResidentChunks() const
{
	EnsureStorageLookup(); return ResidentChunks;
}

bool UTerrainGridSubsystem::IsValidCoord(FIntPoint Coords) const { return CoordToIndex(Coords)!=INDEX_NONE; }

bool UTerrainGridSubsystem::TryGetCell(FIntPoint GlobalCoord,FGridCell& Out) const
{
	const int32 Index=CoordToIndex(GlobalCoord);
	if (Index==INDEX_NONE) return false;
	Out=Cells[Index]; return true;
}

void UTerrainGridSubsystem::NotifyTerrainStorageRestored()
{
	EnsureStorageLookup(); OnTerrainStorageChanged.Broadcast(TerrainRevision);
}

void UTerrainGridSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Varsayılan arazi oluştur (50x50, 500cm, Seed 1337)
	if (Cells.Num() == 0)
	{
		GenerateProceduralTerrain(50, 50, 500.0f, 1337);
	}
}

void UTerrainGridSubsystem::Deinitialize()
{
	Cells.Empty();
	CellStorageIndices.Empty(); ResidentChunks.Empty(); CachedStorageCount=INDEX_NONE;
	OnTerrainStorageChanged.Clear();
	Super::Deinitialize();
}

void UTerrainGridSubsystem::CaptureSaveState(FAftershockTerrainSaveState& Out) const
{
	Out.GridWidth=GridWidth; Out.GridHeight=GridHeight; Out.CellSize=CellSize; Out.GridOrigin=GridOrigin;
	Out.TerrainSeed=TerrainSeed; Out.GeographyIndex=GeographyIndex; Out.GeographyFocusIndex=GeographyFocusIndex;
	Out.ActiveDroughtSeverity=ActiveDroughtSeverity; Out.VolcanicVentCoord=VolcanicVentCoord;
	Out.CoastStartRow=CoastStartRow; Out.CoastalSeaLevelCm=CoastalSeaLevelCm; Out.Cells=Cells;
	Out.TerrainStorageVersion=2; Out.ChunkSize=ChunkSizeCells; Out.ContinuationGeneratorVersion=ContinuationGeneratorVersion;
	Out.bUsesRegionalLayout=bUsesRegionalLayout; Out.bInitialMaskUnlocked=bInitialMaskUnlocked; Out.TerrainRevision=TerrainRevision;
	BuildChunkManifest(Cells,Out.Chunks);
}

bool UTerrainGridSubsystem::ValidateSaveState(const FAftershockTerrainSaveState& State,FText& OutError) const
{
	const auto Reject=[&OutError](const TCHAR* Reason) { OutError=FText::FromString(Reason); return false; };
	const int64 Count=static_cast<int64>(State.GridWidth)*State.GridHeight;
	if (State.GridWidth<1 || State.GridHeight<1 || Count>MaximumResidentCells || Count>State.Cells.Num() || State.Cells.Num()>MaximumResidentCells
		|| !FMath::IsFinite(State.CellSize) || State.CellSize<1.f || State.CellSize>100000.f || State.GridOrigin.ContainsNaN()
		|| !FMath::IsFinite(State.ActiveDroughtSeverity) || State.ActiveDroughtSeverity<0.f || State.ActiveDroughtSeverity>100.f
		|| !FMath::IsFinite(State.CoastalSeaLevelCm)) return Reject(TEXT("Kayıt arazi boyutu veya sayısal veri geçersiz."));
	if ((State.TerrainStorageVersion!=1 && State.TerrainStorageVersion!=2) || State.ChunkSize!=ChunkSizeCells
		|| (State.ContinuationGeneratorVersion<1||State.ContinuationGeneratorVersion>3) || State.TerrainRevision==MAX_uint64)
		return Reject(TEXT("Kayıt parça depolama sürümü veya revizyonu desteklenmiyor."));
	if (State.TerrainStorageVersion==1 && (State.Cells.Num()!=Count || !State.Chunks.IsEmpty() || State.bInitialMaskUnlocked))
		return Reject(TEXT("Eski yoğun arazi kaydı parça uzantısı içeremez."));
	if (State.GeographyIndex!=INDEX_NONE && !IsAftershockGeographyFocusReady(State.GeographyIndex,State.GeographyFocusIndex))
		return Reject(TEXT("Kayıt bölgesi veya yerel coğrafya verisi geçersiz/hazır değil."));
	const bool bRealSource=State.ContinuationGeneratorVersion>=2;
	if(bRealSource&&!(State.ContinuationGeneratorVersion==3?IsAftershockFocusDetailGeodataReady(State.GeographyIndex,State.GeographyFocusIndex):IsAftershockFocusGeodataReady(State.GeographyIndex,State.GeographyFocusIndex)))return Reject(TEXT("Kayıt için seçilmiş gerçek coğrafya kaynak sürümü mevcut değil."));
	if (State.CoastStartRow!=INDEX_NONE && (State.CoastStartRow<=0 || State.CoastStartRow>=State.GridHeight)) return Reject(TEXT("Kayıt kıyı sınırı geçersiz."));
	if (!bRealSource&&State.VolcanicVentCoord!=FIntPoint(INDEX_NONE,INDEX_NONE) && (State.VolcanicVentCoord.X<0 || State.VolcanicVentCoord.X>=State.GridWidth || State.VolcanicVentCoord.Y<0 || State.VolcanicVentCoord.Y>=State.GridHeight))
		return Reject(TEXT("Kayıt volkan kaynak koordinatı geçersiz."));
	TMap<int32,TArray<const FGridCell*>> Bridges;
	TSet<FIntPoint> UniqueCoords;
	const auto In01=[](float Value) { return Value>=0.f && Value<=1.f; };
	// Resolve the same reflected float fields once per validation, rather than
	// rebuilding the iterator for every cell. Keep descriptors local so they
	// cannot survive struct reinstancing or hot reload.
	TArray<const FFloatProperty*,TInlineAllocator<64>> CellFloatProperties;
	for (TFieldIterator<FFloatProperty> Property(FGridCell::StaticStruct());Property;++Property)
		CellFloatProperties.Add(*Property);
	for (int32 I=0;I<State.Cells.Num();++I)
	{
		const FGridCell& Cell=State.Cells[I];
		if (FMath::Abs(static_cast<int64>(Cell.Coords.X))>MaximumAbsGlobalCellCoord || FMath::Abs(static_cast<int64>(Cell.Coords.Y))>MaximumAbsGlobalCellCoord
			|| UniqueCoords.Contains(Cell.Coords)) return Reject(TEXT("Kayıt arazi hücre koordinatı tekrarlı veya sınır dışında."));
		UniqueCoords.Add(Cell.Coords);
		if (I<Count)
		{
			if (Cell.Coords!=FIntPoint(I%State.GridWidth,I/State.GridWidth) || Cell.bAuthoredContinuation)
				return Reject(TEXT("Kayıt başlangıç arazi hücre sırası tutarsız."));
			if (State.bInitialMaskUnlocked && !Cell.bIsOcean && !Cell.bIsPlayable)
				return Reject(TEXT("Genişletilmiş kayıtta başlangıç kara maskesi hâlâ kilitli."));
		}
		else if (State.TerrainStorageVersion<2 || (!bRealSource&&(!Cell.bAuthoredContinuation || Cell.bHasRealElevationSample || Cell.RealElevationMetres!=0.f))
			|| (Cell.Coords.X>=0 && Cell.Coords.Y>=0 && Cell.Coords.X<State.GridWidth && Cell.Coords.Y<State.GridHeight))
			return Reject(TEXT("Kayıt devam arazisi gerçek DEM gibi işaretlenmiş veya başlangıç çerçevesiyle çakışıyor."));
		if(bRealSource)
		{
			const FVector2D Metres((Cell.Coords.X+.5-State.GridWidth*.5)*State.CellSize*.01,(Cell.Coords.Y+.5-State.GridHeight*.5)*State.CellSize*.01);
			double Latitude=0.,Longitude=0.;FAftershockGeographicTerrainSample Source;
			if(Cell.bAuthoredContinuation||!Cell.bHasRealElevationSample
				||!AftershockFocusLocalMetresToLatLon(State.GeographyIndex,State.GeographyFocusIndex,Metres,Latitude,Longitude)
				||!SampleVersionedGeodata(State.ContinuationGeneratorVersion,State.GeographyIndex,State.GeographyFocusIndex,Latitude,Longitude,Source)
				||!FMath::IsNearlyEqual(Cell.RealElevationMetres,Source.ElevationMetres,.01f)
				||!FMath::IsNearlyEqual(Cell.Elevation,State.GridOrigin.Z+Source.ElevationMetres*100.f,1.f)
				||Cell.bIsOcean!=Source.bOcean||Cell.bIsRiverbed!=(!Source.bOcean&&Source.bInlandWater))return Reject(TEXT("Kayıt gerçek coğrafya hücresi kaynak sınırı/yükseklik/su maskesiyle uyuşmuyor."));
		}
		for (const FFloatProperty* Property:CellFloatProperties)
			if (!FMath::IsFinite(Property->GetPropertyValue_InContainer(&Cell))) return Reject(TEXT("Kayıt arazi hücresinde NaN/sonsuz sayı var."));
		if(Cell.HydraulicProfileVersion<0||Cell.HydraulicProfileVersion>1||Cell.HydraulicExcavationCm<0.f
			||(Cell.HydraulicProfileVersion==0&&Cell.HydraulicExcavationCm!=0.f)
			||Cell.DamCrestHeightM<0.f||Cell.DamGateOpen01<0.f||Cell.DamGateOpen01>1.f
			||Cell.SnowWaterEquivalentMm<0.f||Cell.SnowWaterEquivalentMm>10000.f||Cell.SnowBulkDensityKgM3<50.f||Cell.SnowBulkDensityKgM3>900.f
			||Cell.SnowWeakLayerStrengthPa<0.f||Cell.SnowWeakLayerStrengthPa>1.e7f||Cell.RegolithDepthM<0.f||Cell.RegolithDepthM>30.f
			||Cell.SoilPorosity01<.01f||Cell.SoilPorosity01>.8f||Cell.SoilDryBulkDensityKgM3<500.f||Cell.SoilDryBulkDensityKgM3>2600.f
			||Cell.SoilPoreWaterDepthM<0.f||Cell.SoilPoreWaterDepthM>Cell.RegolithDepthM*Cell.SoilPorosity01+1.e-6f
			||Cell.SoilCohesionPa<0.f||Cell.SoilCohesionPa>1.e7f||Cell.SoilFrictionAngleDegrees<0.f||Cell.SoilFrictionAngleDegrees>=80.f
			||Cell.GroundErodedDepthM<0.f||Cell.GroundErodedDepthM>1000.f||Cell.GroundDepositedDepthM<0.f||Cell.GroundDepositedDepthM>1000.f
			||Cell.MaterialScenarioVersion<0||Cell.MaterialScenarioVersion>1
			||(Cell.MaterialScenarioVersion==0&&(Cell.SnowWaterEquivalentMm!=0.f||Cell.RegolithDepthM!=0.f||Cell.SoilPoreWaterDepthM!=0.f
				||Cell.GroundErodedDepthM!=0.f||Cell.GroundDepositedDepthM!=0.f)))return Reject(TEXT("Kayıt kar, toprak, gözenek suyu veya hidrolik profil sınırlarını aşıyor."));
		if (!In01(Cell.ForestFuel01) || !In01(Cell.SoilMoisture01) || !In01(Cell.CropHealth01) || !In01(Cell.DroughtStress01)
			|| !In01(Cell.LavaCoverage01) || !In01(Cell.AshLoad01) || !In01(Cell.RiverFill01) || !In01(Cell.DrainageQuality)
			|| Cell.RiverCapacity<0.f || Cell.FloodDepth<0.f || Cell.InitialFloodDepth<0.f || Cell.BridgeID<INDEX_NONE)
			return Reject(TEXT("Kayıt arazi hücre değerleri izin verilen sınırların dışında."));
		if (Cell.BridgeID==INDEX_NONE)
		{
			if (Cell.BridgeAxis!=0 || Cell.bBridgeEndpoint || Cell.bRoadPredatedBridge || Cell.RoadSurfaceElevationCm!=0.f)
				return Reject(TEXT("Kayıt köprü üyeliği olmayan hücrede köprü verisi içeriyor."));
			if (Cell.bHasRoad && (Cell.bIsRiverbed || Cell.bIsOcean)) return Reject(TEXT("Kayıt su üzerinde köprüsüz kara yolu içeriyor."));
		}
		else
		{
			if (!Cell.bHasRoad || !Cell.bIsPlayable || Cell.bIsOcean || Cell.bHasDam || Cell.OccupyingBuildingID!=INDEX_NONE
				|| (Cell.BridgeAxis!=1 && Cell.BridgeAxis!=2) || Cell.RoadSurfaceElevationCm<Cell.Elevation+7.f || (Cell.bIsRiverbed && Cell.bRoadPredatedBridge))
				return Reject(TEXT("Kayıt köprü yüzeyi, ekseni veya arazi bağlantısı geçersiz."));
			Bridges.FindOrAdd(Cell.BridgeID).Add(&Cell);
		}
	}
	if (State.TerrainStorageVersion==2)
	{
		TArray<FAftershockTerrainChunkState> Expected; BuildChunkManifest(State.Cells,Expected);
		if (Expected.Num()!=State.Chunks.Num()) return Reject(TEXT("Kayıt parça manifesti hücre depolamasıyla uyuşmuyor."));
		for (int32 I=0;I<Expected.Num();++I)
			if (Expected[I].CellCount>ChunkSizeCells*ChunkSizeCells || (Expected[I].bContainsAuthoredContinuation && Expected[I].CellCount!=ChunkSizeCells*ChunkSizeCells)
				|| Expected[I].ChunkCoord!=State.Chunks[I].ChunkCoord || Expected[I].CellCount!=State.Chunks[I].CellCount
				|| Expected[I].bContainsAuthoredContinuation!=State.Chunks[I].bContainsAuthoredContinuation)
				return Reject(TEXT("Kayıt parça manifesti sırası veya hücre sayısı tutarsız."));
		if (State.Cells.Num()>Count && !State.bInitialMaskUnlocked) return Reject(TEXT("Genişletilmiş kayıt başlangıç maskesi kilidini taşımıyor."));
	}
	if(bRealSource&&State.VolcanicVentCoord!=FIntPoint(INDEX_NONE,INDEX_NONE))
	{
		const auto& Rules=GetAftershockLocalFocusRules(State.GeographyIndex,State.GeographyFocusIndex);const auto& Region=GetAftershockRegionRules(State.GeographyIndex);
		FVector2D SourceMetres;FAftershockGeographicTerrainSample Source;
		if(!Rules.bHasVolcanicSource||!AftershockFocusLatLonToLocalMetres(State.GeographyIndex,State.GeographyFocusIndex,Region.VolcanoLatitude,Region.VolcanoLongitude,SourceMetres)
			||!TrySampleAftershockFocusGeodata(State.GeographyIndex,State.GeographyFocusIndex,Region.VolcanoLatitude,Region.VolcanoLongitude,Source))return Reject(TEXT("Kayıt volkan kaynağı gerçek kaynak alanında değil."));
		const FIntPoint Expected(FMath::FloorToInt(SourceMetres.X*100./State.CellSize+State.GridWidth*.5),FMath::FloorToInt(SourceMetres.Y*100./State.CellSize+State.GridHeight*.5));
		if(Expected!=State.VolcanicVentCoord)return Reject(TEXT("Kayıt volkan kaynağı gerçek konumuyla uyuşmuyor."));
	}
	for (auto& Entry:Bridges)
	{
		TArray<const FGridCell*>& Span=Entry.Value;
		if (Span.Num()<3 || Span.Num()>34) return Reject(TEXT("Kayıt köprü açıklığı geçersiz."));
		const uint8 Axis=Span[0]->BridgeAxis;
		Span.Sort([Axis](const FGridCell& A,const FGridCell& B) { return Axis==1 ? A.Coords.X<B.Coords.X : A.Coords.Y<B.Coords.Y; });
		int32 Rivers=0,FirstRiver=INDEX_NONE,LastRiver=INDEX_NONE;
		for (int32 I=0;I<Span.Num();++I)
		{
			const FGridCell& Cell=*Span[I];
			if (Cell.BridgeAxis!=Axis || Cell.bBridgeEndpoint!=(I==0 || I==Span.Num()-1)) return Reject(TEXT("Kayıt köprü uç/eksen sözleşmesi tutarsız."));
			if (I>0 && (Cell.Coords-Span[I-1]->Coords!=(Axis==1 ? FIntPoint(1,0) : FIntPoint(0,1))
				|| FMath::Abs(Cell.RoadSurfaceElevationCm-Span[I-1]->RoadSurfaceElevationCm)>State.CellSize*.12f+1.f)) return Reject(TEXT("Kayıt köprü kesintili veya rampa eğimi fazla."));
			if (Cell.bIsRiverbed) { if (FirstRiver==INDEX_NONE) FirstRiver=I; LastRiver=I; ++Rivers; }
		}
		if (Rivers<1 || Rivers>8 || FirstRiver<1 || LastRiver>=Span.Num()-1 || LastRiver-FirstRiver+1!=Rivers
			|| FirstRiver-1>12 || Span.Num()-LastRiver-2>12) return Reject(TEXT("Kayıt köprü nehir/iki kıyı bağlantısı geçersiz."));
		if (!FMath::IsNearlyEqual(Span[0]->RoadSurfaceElevationCm,Span[0]->Elevation+8.f,1.f)
			|| !FMath::IsNearlyEqual(Span.Last()->RoadSurfaceElevationCm,Span.Last()->Elevation+8.f,1.f)) return Reject(TEXT("Kayıt köprü rampaları kuru kara yüzeyine bağlanmıyor."));
		const float Deck=Span[FirstRiver]->RoadSurfaceElevationCm;
		for (int32 I=FirstRiver-1;I<=LastRiver+1;++I)
			if (!FMath::IsNearlyEqual(Span[I]->RoadSurfaceElevationCm,Deck,1.f) || (Span[I]->bIsRiverbed && Deck<Span[I]->Elevation+159.f))
				return Reject(TEXT("Kayıt köprü tabliyesi nehir açıklığı profilini korumuyor."));
	}
	OutError=FText::GetEmpty(); return true;
}

bool UTerrainGridSubsystem::RestoreSaveState(const FAftershockTerrainSaveState& State,FText& OutError)
{
	if (!ValidateSaveState(State,OutError)) return false;
	// Validation above is read-only. Commit all canonical state once; the save
	// coordinator rebuilds network/service caches only after every owner imports.
	GridWidth=State.GridWidth; GridHeight=State.GridHeight; CellSize=State.CellSize; GridOrigin=State.GridOrigin;
	TerrainSeed=State.TerrainSeed; GeographyIndex=State.GeographyIndex; GeographyFocusIndex=State.GeographyFocusIndex;
	ActiveDroughtSeverity=State.ActiveDroughtSeverity; VolcanicVentCoord=State.VolcanicVentCoord;
	CoastStartRow=State.CoastStartRow; CoastalSeaLevelCm=State.CoastalSeaLevelCm; Cells=State.Cells;
	bUsesRegionalLayout=State.bUsesRegionalLayout; bInitialMaskUnlocked=State.bInitialMaskUnlocked;
	ContinuationGeneratorVersion=State.ContinuationGeneratorVersion;
	TerrainRevision=State.TerrainRevision+1; RebuildStorageLookup();
	OutError=FText::GetEmpty(); return true;
}

void UTerrainGridSubsystem::GenerateProceduralTerrain(int32 Width, int32 Height, float InCellSize, int32 Seed)
{
	if (Width<1 || Height<1 || static_cast<int64>(Width)*Height>MaximumResidentCells || !FMath::IsFinite(InCellSize) || InCellSize<1.f || InCellSize>100000.f)
	{
		UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_TERRAIN_GENERATION_REJECTED width=%d height=%d cellSize=%.2f residentCap=%d"),Width,Height,InCellSize,MaximumResidentCells);
		return;
	}
	GridWidth = FMath::Max(1, Width);
	GridHeight = FMath::Max(1, Height);
	CellSize = FMath::Max(1.0f, InCellSize);
	CoastStartRow = INDEX_NONE;
	CoastalSeaLevelCm = 24.f;
	GeographyIndex = INDEX_NONE;
	GeographyFocusIndex = 0;
	bUsesRegionalLayout=false; bInitialMaskUnlocked=false;
	ContinuationGeneratorVersion=1;
	ActiveDroughtSeverity = 0.f;
	VolcanicVentCoord = FIntPoint(INDEX_NONE, INDEX_NONE);

	const int32 TotalCells = GridWidth * GridHeight;
	Cells.Empty(TotalCells);
	Cells.SetNum(TotalCells);

	// Diyagonal fay hattı için doğru katsayıları
	const float A = static_cast<float>(GridHeight - 1);
	const float B = -static_cast<float>(GridWidth - 1);
	const float LineLength = FMath::Sqrt(A * A + B * B);
	const float FaultInfluenceRadius = FMath::Max(static_cast<float>(GridWidth), static_cast<float>(GridHeight)) * 0.22f;

	TerrainSeed = Seed;

	for (int32 Y = 0; Y < GridHeight; ++Y)
	{
		const float NormalizedY = static_cast<float>(Y) / static_cast<float>(FMath::Max(1, GridHeight - 1));

		// Kıvrımlı nehir merkezi hesabı
		const float CenterX = static_cast<float>(GridWidth) * 0.5f;
		const float RiverMeander = FMath::Sin(NormalizedY * 2.0f * PI + static_cast<float>(Seed) * 0.7f) * (static_cast<float>(GridWidth) * 0.22f)
		                         + FMath::Sin(NormalizedY * 4.0f * PI + static_cast<float>(Seed) * 1.3f) * (static_cast<float>(GridWidth) * 0.08f);
		const float RiverX = CenterX + RiverMeander;

		for (int32 X = 0; X < GridWidth; ++X)
		{
			const int32 CellIndex = X+Y*GridWidth;
			FGridCell& Cell = Cells[CellIndex];

			Cell.Coords = FIntPoint(X, Y);
			const float RegionX = (X + 0.5f - GridWidth * 0.5f) / FMath::Max(0.5f, GridWidth * 0.485f);
			const float RegionY = (Y + 0.5f - GridHeight * 0.5f) / FMath::Max(0.5f, GridHeight * 0.485f);
			Cell.bIsPlayable = RegionX * RegionX + RegionY * RegionY <= 1.0f;
			Cell.OccupyingBuildingID = -1;
			Cell.DrainageQuality = 0.0f;

			// 1) Perlin Noise ile Yükseklik Haritası ve Kuzey Dağ Sıradağları
			const float SampleX = static_cast<float>(X) + static_cast<float>(Seed) * 17.13f;
			const float SampleY = static_cast<float>(Y) + static_cast<float>(Seed) * 29.37f;
			const float N1 = FMath::PerlinNoise2D(FVector2D(SampleX * 0.04f, SampleY * 0.04f));
			const float N2 = FMath::PerlinNoise2D(FVector2D(SampleX * 0.12f, SampleY * 0.12f)) * 0.35f;
			const float CombinedNoise = (N1 + N2) / 1.35f;
			const float NormalizedNoise = FMath::Clamp((CombinedNoise + 1.0f) * 0.5f, 0.0f, 1.0f);
			const float BaseElevation = NormalizedNoise * 600.0f; // 0 - 600 cm

			// Dağ Sıradağları (Kuzey Sektöründe Yükselen Dağ Zirveleri ve Sırtlar)
			const float NorthMountainFactor = FMath::Clamp((0.42f - NormalizedY) / 0.42f, 0.0f, 1.0f);
			const float Ridge1 = 1.0f - FMath::Abs(FMath::PerlinNoise2D(FVector2D(SampleX * 0.045f, SampleY * 0.045f)));
			const float Ridge2 = 1.0f - FMath::Abs(FMath::PerlinNoise2D(FVector2D(SampleX * 0.09f, SampleY * 0.09f))) * 0.5f;
			const float CombinedRidge = FMath::Clamp((Ridge1 + Ridge2) / 1.5f, 0.0f, 1.0f);
			const float MountainPeak = FMath::Pow(CombinedRidge, 2.2f) * 2600.0f;
			const float MountainElevation = NorthMountainFactor * (MountainPeak + 200.0f * CombinedRidge);
			const float ElevatedGround = BaseElevation + MountainElevation;

			// 2) Nehir Yatağı ve Alçak Vadi/Rüzgar Koridorları
			const float DistToRiver = FMath::Abs(static_cast<float>(X) - RiverX);
			Cell.RiverDistance = DistToRiver * InCellSize;
			Cell.RiverProximity = FMath::Clamp(1.0f - (DistToRiver / 4.0f), 0.0f, 1.0f);

			if (DistToRiver < 1.25f)
			{
				// Nehir Yatağı
				Cell.bIsRiverbed = true;
				Cell.Elevation = FMath::Min(ElevatedGround * 0.15f, 30.0f);
				Cell.RiverCapacity = 1.0f;
				Cell.WaterFlowDamping = 0.0f;
				Cell.ValleyWindMultiplier = 1.35f;
				Cell.InitialFloodDepth = 100.0f;
			}
			else if (DistToRiver <= 3.5f)
			{
				// Nehir Çevresindeki Alçak Vadi / Rüzgar Koridoru
				Cell.bIsRiverbed = false;
				Cell.ValleyWindMultiplier = 1.35f;
				const float Alpha = (DistToRiver - 1.25f) / (3.5f - 1.25f);
				Cell.Elevation = FMath::Lerp(50.0f, ElevatedGround, Alpha);
				Cell.RiverCapacity = 1.0f;
				Cell.WaterFlowDamping = 0.0f;
				Cell.InitialFloodDepth = 0.0f;
			}
			else
			{
				// Normal Arazi ve Dağlar
				Cell.bIsRiverbed = false;
				Cell.ValleyWindMultiplier = 1.0f;
				Cell.Elevation = ElevatedGround;
				Cell.RiverCapacity = 1.0f;
				Cell.WaterFlowDamping = 0.0f;
				Cell.InitialFloodDepth = 0.0f;
			}

			Cell.FloodDepth = Cell.InitialFloodDepth;
			Cell.SoilType = DistToRiver <= 4.f ? EAftershockSoilType::Alluvium : EAftershockSoilType::Bedrock;
			Cell.ForestFuel01 = Cell.bIsPlayable && !Cell.bIsRiverbed ? FMath::Clamp(.45f + CombinedNoise*.5f, .08f, .9f) : 0.f;
			Cell.SoilMoisture01 = Cell.bIsRiverbed ? 1.f : .65f;
			Cell.Slope = FMath::Abs(CombinedNoise) * 15.0f; // Yaklaşık parsel eğimi

			// 3) Diyagonal Tektonik Fay Hattı Yakınlığı
			const float DistanceToFault = (LineLength > KINDA_SMALL_NUMBER)
				? (FMath::Abs(A * static_cast<float>(X) + B * static_cast<float>(Y)) / LineLength)
				: 0.0f;

			const float FaultProximity = FMath::Clamp(1.0f - (DistanceToFault / FaultInfluenceRadius), 0.0f, 1.0f);
			Cell.FaultLineProximity = FaultProximity;

			// 4) Parsel elverişlilik puanı
			Cell.ParcelSuitability = Cell.bIsRiverbed ? 0.0f : FMath::Clamp(1.0f - (FaultProximity * 0.3f + Cell.RiverProximity * 0.3f), 0.1f, 1.0f);
		}
	}
	RebuildStorageLookup(); MarkTerrainEdited(); OnTerrainStorageChanged.Broadcast(TerrainRevision);
}

bool UTerrainGridSubsystem::TrySampleGeographicSource(double Latitude,double Longitude,FAftershockGeographicTerrainSample& Out) const
{
	return SampleVersionedGeodata(ContinuationGeneratorVersion,GeographyIndex,GeographyFocusIndex,Latitude,Longitude,Out);
}

bool UTerrainGridSubsystem::TryGetGeographicSourceBounds(FAftershockGeographicBounds& Out) const
{
	return ContinuationGeneratorVersion==3?GetAftershockFocusDetailGeodataBounds(GeographyIndex,GeographyFocusIndex,Out)
		:ContinuationGeneratorVersion==2&&GetAftershockFocusGeodataBounds(GeographyIndex,GeographyFocusIndex,Out);
}

bool UTerrainGridSubsystem::TrySampleResidentElevation(float X,float Y,float& Out) const
{
	if(!FMath::IsFinite(X)||!FMath::IsFinite(Y))return false;
	const FIntRect Bounds=GetResidentCellBounds();if(Bounds.IsEmpty())return false;
	X=FMath::Clamp(X,static_cast<float>(Bounds.Min.X),static_cast<float>(Bounds.Max.X-1));
	Y=FMath::Clamp(Y,static_cast<float>(Bounds.Min.Y),static_cast<float>(Bounds.Max.Y-1));
	const int32 X0=FMath::FloorToInt(X),Y0=FMath::FloorToInt(Y),X1=FMath::Min(X0+1,Bounds.Max.X-1),Y1=FMath::Min(Y0+1,Bounds.Max.Y-1);
	FGridCell C[4];const FIntPoint Coords[]={FIntPoint(X0,Y0),FIntPoint(X1,Y0),FIntPoint(X0,Y1),FIntPoint(X1,Y1)};
	bool Valid[4];for(int32 I=0;I<4;++I)Valid[I]=TryGetCell(Coords[I],C[I]);
	const float FX=X-X0,FY=Y-Y0;
	if(Valid[0]&&Valid[1]&&Valid[2]&&Valid[3])
	{
		if(C[0].bIsPlayable&&C[3].bIsPlayable&&C[0].bIsRiverbed&&C[3].bIsRiverbed&&!C[1].bIsRiverbed&&!C[2].bIsRiverbed)
			Out=FX>=FY?C[0].Elevation*(1.f-FX)+C[1].Elevation*(FX-FY)+C[3].Elevation*FY:C[0].Elevation*(1.f-FY)+C[2].Elevation*(FY-FX)+C[3].Elevation*FX;
		else if(C[1].bIsPlayable&&C[2].bIsPlayable&&C[1].bIsRiverbed&&C[2].bIsRiverbed&&!C[0].bIsRiverbed&&!C[3].bIsRiverbed)
			Out=FX+FY<=1.f?C[0].Elevation*(1.f-FX-FY)+C[1].Elevation*FX+C[2].Elevation*FY:C[3].Elevation*(FX+FY-1.f)+C[1].Elevation*(1.f-FY)+C[2].Elevation*(1.f-FX);
		else Out=FMath::Lerp(FMath::Lerp(C[0].Elevation,C[1].Elevation,FX),FMath::Lerp(C[2].Elevation,C[3].Elevation,FX),FY);
		return FMath::IsFinite(Out);
	}
	const float W[]={(1-FX)*(1-FY),FX*(1-FY),(1-FX)*FY,FX*FY};float Sum=0,Weight=0,NearestHeight=GridOrigin.Z,NearestDistance=MAX_flt;bool Any=false;
	for(int32 I=0;I<4;++I)if(Valid[I]){Any=true;Sum+=C[I].Elevation*W[I];Weight+=W[I];const float D=FMath::Square(X-Coords[I].X)+FMath::Square(Y-Coords[I].Y);if(D<NearestDistance){NearestDistance=D;NearestHeight=C[I].Elevation;}}
	if(!Any)return false;Out=Weight>SMALL_NUMBER?Sum/Weight:NearestHeight;return FMath::IsFinite(Out);
}

bool UTerrainGridSubsystem::TrySampleWorldElevation(FVector World,float& Out) const
{
	if(World.ContainsNaN()||!FMath::IsFinite(CellSize)||CellSize<=0.f)return false;
	const float X=(World.X-GridOrigin.X)/CellSize-.5,Y=(World.Y-GridOrigin.Y)/CellSize-.5;
	const FIntPoint At=WorldToGrid(World);
	if(IsValidCoord(At)&&TrySampleResidentElevation(X,Y,Out))return true;
	if(!IsUsingRealGeography())return false;
	double Latitude=0.,Longitude=0.;FAftershockGeographicTerrainSample Sample;
	const FVector2D Metres((World.X-GridOrigin.X-GridWidth*CellSize*.5)*.01,(World.Y-GridOrigin.Y-GridHeight*CellSize*.5)*.01);
	if(!AftershockFocusLocalMetresToLatLon(GeographyIndex,GeographyFocusIndex,Metres,Latitude,Longitude)||!TrySampleGeographicSource(Latitude,Longitude,Sample))return false;
	Out=GridOrigin.Z+Sample.ElevationMetres*100.f;return FMath::IsFinite(Out);
}

FIntRect UTerrainGridSubsystem::GetSupportedGeographicCellBounds() const
{
	if(!IsUsingRealGeography()||!FMath::IsFinite(CellSize)||CellSize<=0.f)return FIntRect(0,0,0,0);
	FAftershockGeographicBounds Bounds;FVector2D NW,SE;
	if(!TryGetGeographicSourceBounds(Bounds)
		||!AftershockFocusLatLonToLocalMetres(GeographyIndex,GeographyFocusIndex,Bounds.North,Bounds.West,NW)
		||!AftershockFocusLatLonToLocalMetres(GeographyIndex,GeographyFocusIndex,Bounds.South,Bounds.East,SE))return FIntRect(0,0,0,0);
	const double M=CellSize*.01;
	const int32 MinX=FMath::CeilToInt(NW.X/M+GridWidth*.5-.5),MinY=FMath::CeilToInt(NW.Y/M+GridHeight*.5-.5);
	const int32 MaxX=FMath::FloorToInt(SE.X/M+GridWidth*.5-.5)+1,MaxY=FMath::FloorToInt(SE.Y/M+GridHeight*.5-.5)+1;
	return FIntRect(FIntPoint(MinX,MinY),FIntPoint(MaxX,MaxY));
}

bool UTerrainGridSubsystem::TryWorldToGeographic(FVector World,double& OutLatitude,double& OutLongitude) const
{
	if(!IsUsingRealGeography()||World.ContainsNaN())return false;
	const FVector2D Metres((World.X-GridOrigin.X-GridWidth*CellSize*.5)*.01,(World.Y-GridOrigin.Y-GridHeight*CellSize*.5)*.01);
	double Latitude=0.,Longitude=0.;FAftershockGeographicTerrainSample Sample;
	if(!AftershockFocusLocalMetresToLatLon(GeographyIndex,GeographyFocusIndex,Metres,Latitude,Longitude)
		||!TrySampleGeographicSource(Latitude,Longitude,Sample))return false;
	OutLatitude=Latitude;OutLongitude=Longitude;return true;
}

bool UTerrainGridSubsystem::TryGeographicToWorld(double Latitude,double Longitude,FVector& OutWorld) const
{
	if(!IsUsingRealGeography())return false;
	FVector2D Metres;FAftershockGeographicTerrainSample Sample;
	if(!AftershockFocusLatLonToLocalMetres(GeographyIndex,GeographyFocusIndex,Latitude,Longitude,Metres)
		||!TrySampleGeographicSource(Latitude,Longitude,Sample))return false;
	OutWorld=FVector(GridOrigin.X+GridWidth*CellSize*.5+Metres.X*100.,GridOrigin.Y+GridHeight*CellSize*.5+Metres.Y*100.,GridOrigin.Z+Sample.ElevationMetres*100.);
	return true;
}

bool UTerrainGridSubsystem::TryGeographicToCell(double Latitude,double Longitude,FIntPoint& OutCell) const
{
	FVector Position;if(!TryGeographicToWorld(Latitude,Longitude,Position))return false;
	const FIntPoint Cell=WorldToGrid(Position);if(!GetSupportedGeographicCellBounds().Contains(Cell))return false;
	OutCell=Cell;return true;
}

bool UTerrainGridSubsystem::TryGenerateGeographicCell(FIntPoint Coord,FGridCell& Out) const
{
	if(!IsUsingRealGeography())return false;
	const FVector2D Metres((Coord.X+.5-GridWidth*.5)*CellSize*.01,(Coord.Y+.5-GridHeight*.5)*CellSize*.01);
	double Latitude=0.,Longitude=0.;FAftershockGeographicTerrainSample Source;
	if(!AftershockFocusLocalMetresToLatLon(GeographyIndex,GeographyFocusIndex,Metres,Latitude,Longitude)
		||!TrySampleGeographicSource(Latitude,Longitude,Source))return false;
	FGridCell Cell;Cell.Coords=Coord;Cell.RealElevationMetres=Source.ElevationMetres;Cell.bHasRealElevationSample=true;
	Cell.bAuthoredContinuation=false;Cell.Elevation=GridOrigin.Z+Source.ElevationMetres*100.f;
	Cell.bIsOcean=Source.bOcean;Cell.bIsPlayable=!Source.bOcean;
	Cell.bIsRiverbed=!Source.bOcean&&Source.bInlandWater;
	Cell.bIsCoastal=!Source.bOcean&&Source.bHasCoastlineSource&&Source.SignedCoastDistanceMetres>=0.f&&Source.SignedCoastDistanceMetres<=CellSize*.03f;
	Cell.RiverDistance=Source.RiverDistanceMetres*100.f;
	Cell.RiverProximity=Source.bOcean?0.f:FMath::Clamp(1.f-Cell.RiverDistance/FMath::Max(1.f,CellSize*4.f),0.f,1.f);
	// These remain authored gameplay state, not an inferred geotechnical survey.
	const auto& Rules=GetAftershockLocalFocusRules(GeographyIndex,GeographyFocusIndex);
	// Legacy-named field retains the authored regional risk control. This is a
	// uniform scenario proxy, NOT a measured/DEM-derived distance to a real fault.
	Cell.FaultLineProximity=FMath::Clamp(Rules.LandscapeProfile.FaultInfluence01,0.f,1.f);
	Cell.SoilType=Cell.RiverProximity>.1f?EAftershockSoilType::Alluvium:EAftershockSoilType::Bedrock;Cell.bGroundSurveyed=false;
	Cell.ForestFuel01=!Cell.bIsOcean&&!Cell.bIsRiverbed&&Source.bMappedForest?.75f:0.f;
	Cell.bAgriculturalLand=!Cell.bIsOcean&&!Cell.bIsRiverbed&&Source.bMappedAgriculture;
	Cell.SoilMoisture01=Cell.bIsRiverbed?1.f:Rules.LandscapeProfile.Moisture01;
	Cell.ValleyWindMultiplier=1.f+Rules.LandscapeProfile.ValleyWindExposure01*.5f*(.4f+Cell.RiverProximity*.6f);
	Cell.InitialFloodDepth=Cell.bIsOcean?FMath::Max(0.f,GridOrigin.Z-Cell.Elevation):Cell.bIsRiverbed?100.f:0.f;
	Cell.FloodDepth=Cell.InitialFloodDepth;Cell.DroughtStress01=FMath::Clamp(ActiveDroughtSeverity/100.f,0.f,1.f);
	double EastLat=0.,EastLon=0.,SouthLat=0.,SouthLon=0.;FAftershockGeographicTerrainSample East,South;
	float DX=0.f,DY=0.f;
	if(AftershockFocusLocalMetresToLatLon(GeographyIndex,GeographyFocusIndex,Metres+FVector2D(CellSize*.01,0),EastLat,EastLon)
		&&TrySampleGeographicSource(EastLat,EastLon,East))DX=East.ElevationMetres-Source.ElevationMetres;
	if(AftershockFocusLocalMetresToLatLon(GeographyIndex,GeographyFocusIndex,Metres+FVector2D(0,CellSize*.01),SouthLat,SouthLon)
		&&TrySampleGeographicSource(SouthLat,SouthLon,South))DY=South.ElevationMetres-Source.ElevationMetres;
	Cell.Slope=FMath::RadiansToDegrees(FMath::Atan2(FMath::Sqrt(DX*DX+DY*DY),CellSize*.01f));
	Cell.ParcelSuitability=Cell.bIsOcean||Cell.bIsRiverbed?0.f:FMath::Clamp(1.f-Cell.Slope*.025f-Cell.RiverProximity*.3f,0.f,1.f);
	const auto& Region=GetAftershockRegionRules(GeographyIndex);
	if(Rules.bHasVolcanicSource)
	{
		FIntPoint Vent;if(TryGeographicToCell(Region.VolcanoLatitude,Region.VolcanoLongitude,Vent)&&Vent==Coord)
		{Cell.bVolcanicVent=true;Cell.ParcelSuitability=0.f;}
	}
	Out=Cell;return true;
}

bool UTerrainGridSubsystem::ApplyOfflineGeography(int32 ScenarioIndex,int32 FocusIndex,bool bUseRegionalLayout,int32 SourceVersion)
{
	if((SourceVersion!=2&&SourceVersion!=3)||!(SourceVersion==3?IsAftershockFocusDetailGeodataReady(ScenarioIndex,FocusIndex):IsAftershockFocusGeodataReady(ScenarioIndex,FocusIndex))||Cells.Num()!=GridWidth*GridHeight||Cells.IsEmpty())
	{
		UE_LOG(LogTemp,Warning,TEXT("AFTERSHOCK_REAL_GEOGRAPHY_UNAVAILABLE region=%d focus=%d noAuthoredSubstitute=1"),ScenarioIndex,FocusIndex);
		return false;
	}
	for(const FGridCell& Cell:Cells)if(Cell.OccupyingBuildingID!=INDEX_NONE||Cell.bHasRoad||Cell.bHasDam)return false;
	const int32 OldRegion=GeographyIndex,OldFocus=GeographyFocusIndex,OldVersion=ContinuationGeneratorVersion;
	GeographyIndex=ScenarioIndex;GeographyFocusIndex=FocusIndex;ContinuationGeneratorVersion=SourceVersion;
	TArray<FGridCell> Staged;Staged.Reserve(Cells.Num());float MinM=MAX_flt,MaxM=-MAX_flt;
	for(const FGridCell& Existing:Cells)
	{
		FGridCell Sample;
		if(!TryGenerateGeographicCell(Existing.Coords,Sample))
		{GeographyIndex=OldRegion;GeographyFocusIndex=OldFocus;ContinuationGeneratorVersion=OldVersion;return false;}
		MinM=FMath::Min(MinM,Sample.RealElevationMetres);MaxM=FMath::Max(MaxM,Sample.RealElevationMetres);Staged.Add(Sample);
	}
	Cells=MoveTemp(Staged);bUsesRegionalLayout=bUseRegionalLayout||ScenarioIndex>=7;bInitialMaskUnlocked=false;
	CoastStartRow=INDEX_NONE;CoastalSeaLevelCm=GridOrigin.Z;VolcanicVentCoord=FIntPoint(INDEX_NONE,INDEX_NONE);
	const auto& Rules=GetAftershockLocalFocusRules(ScenarioIndex,FocusIndex);const auto& Region=GetAftershockRegionRules(ScenarioIndex);
	if(Rules.bHasVolcanicSource)TryGeographicToCell(Region.VolcanoLatitude,Region.VolcanoLongitude,VolcanicVentCoord);
	int32 Ocean=0,River=0,Forest=0,Farms=0;
	for(const auto& Cell:Cells){Ocean+=Cell.bIsOcean?1:0;River+=Cell.bIsRiverbed?1:0;Forest+=Cell.ForestFuel01>0.f?1:0;Farms+=Cell.bAgriculturalLand?1:0;}
	const auto& Site=GetAftershockGeographyFocus(ScenarioIndex,FocusIndex);
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_REAL_GEOGRAPHY region=%d focus=%d location=%s source=OfflineDEM_OSM_ProviderPerPacket demSide=%d sourceVersion=%d surfaceSide=257 realElevationRangeM=%.3f..%.3f cellMetres=%.3f oceanCells=%d inlandWaterCells=%d forestCells=%d agricultureCells=%d ovalMask=0 elevationNormalization=0 runtimeNetwork=0 noGeotechnicalSurvey=1"),
		ScenarioIndex,FocusIndex,Site.Label,SourceVersion==3?257:65,SourceVersion,MinM,MaxM,CellSize*.01f,Ocean,River,Forest,Farms);
	MarkTerrainEdited();return true;
}

bool UTerrainGridSubsystem::ApplyLegacyOfflineGeography(int32 ScenarioIndex,int32 FocusIndex,bool bUseRegionalLayout)
{
	// This additive real-distance lower-flank anchor never existed in V1's
	// compressed ellipse/vent placement policy. Reject BEFORE any state writes;
	// its25 immutable original source focuses keep their historical behavior.
	if(ScenarioIndex==5&&FocusIndex==1)return false;
	if (!IsAftershockGeographyFocusReady(ScenarioIndex,FocusIndex) || Cells.Num()!=GridWidth*GridHeight || Cells.IsEmpty()) return false;
	for (const FGridCell& Cell : Cells)
		if (Cell.OccupyingBuildingID != INDEX_NONE || Cell.bHasRoad || Cell.bHasDam) return false;
	TArray<float> Altitudes; Altitudes.Reserve(Cells.Num());
	float MinM=MAX_flt, MaxM=-MAX_flt;
	for (const FGridCell& Cell:Cells)
	{
		const float Altitude=SampleAftershockFocusElevationMetres(ScenarioIndex,FocusIndex,(Cell.Coords.X+.5f)/GridWidth,(Cell.Coords.Y+.5f)/GridHeight);
		if (!FMath::IsFinite(Altitude)) return false;
		Altitudes.Add(Altitude); MinM=FMath::Min(MinM,Altitude); MaxM=FMath::Max(MaxM,Altitude);
	}
	ContinuationGeneratorVersion=1;
	GeographyIndex = ScenarioIndex;
	GeographyFocusIndex=FocusIndex;
	const auto& LocalRules=GetAftershockLocalFocusRules(ScenarioIndex,FocusIndex);
	const auto& Profile=LocalRules.LandscapeProfile;
	// Legacy campaign layouts are immutable. Free-region sessions use local rules
	// for every region, including the original seven, so advertised gates match
	// actual river/coast/vent cells rather than inheriting the old campaign map.
	const bool bRegionalLayout=bUseRegionalLayout || ScenarioIndex>=7;
	bUsesRegionalLayout=bRegionalLayout;
	if (bRegionalLayout)
	{
		CoastStartRow=INDEX_NONE; VolcanicVentCoord=FIntPoint(INDEX_NONE,INDEX_NONE);
		for (FGridCell& Cell:Cells)
		{
			const float X=(Cell.Coords.X+.5f-GridWidth*.5f)/FMath::Max(.5f,GridWidth*.485f);
			const float Y=(Cell.Coords.Y+.5f-GridHeight*.5f)/FMath::Max(.5f,GridHeight*.485f);
			Cell.bIsPlayable=X*X+Y*Y<=1.f; Cell.bIsOcean=false; Cell.bIsCoastal=false; Cell.bVolcanicVent=false;
		}
	}
	for (int32 I=0;I<Cells.Num();++I) { Cells[I].RealElevationMetres=Altitudes[I]; Cells[I].bHasRealElevationSample=true; }
	for (FGridCell& Cell : Cells)
	{
		if (bRegionalLayout)
		{
			const float U=(Cell.Coords.X+.5f)/GridWidth,V=(Cell.Coords.Y+.5f)/GridHeight;
			const float RiverX=Profile.RiverCenter01*GridWidth+FMath::Sin(V*2.f*PI+TerrainSeed*.7f)*GridWidth*.055f;
			const float RiverCells=FMath::Abs(Cell.Coords.X-RiverX);
			const float Relief=FMath::Clamp((Cell.RealElevationMetres-MinM)/FMath::Max(1.f,MaxM-MinM),0.f,1.f);
			Cell.bIsRiverbed=LocalRules.bHasRiver && RiverCells<1.25f;
			Cell.RiverDistance=LocalRules.bHasRiver ? RiverCells*CellSize : 100000.f;
			Cell.RiverProximity=LocalRules.bHasRiver ? FMath::Clamp(1.f-RiverCells/4.f,0.f,1.f) : 0.f;
			Cell.Elevation=Cell.bIsRiverbed ? 25.f : 80.f+Relief*Profile.ReliefCm;
			if (!Cell.bIsRiverbed && Cell.RiverProximity>0.f) Cell.Elevation=FMath::Lerp(Cell.Elevation,65.f,Cell.RiverProximity*.7f);
			Cell.InitialFloodDepth=Cell.bIsRiverbed ? 100.f : 0.f; Cell.FloodDepth=Cell.InitialFloodDepth;
			Cell.SoilType=Cell.RiverProximity>.1f || Profile.Agriculture01>.55f ? EAftershockSoilType::Alluvium : EAftershockSoilType::Bedrock;
			FRandomStream Random(static_cast<int32>(GetTypeHash(Cell.Coords)^GetTypeHash(TerrainSeed)));
			Cell.ForestFuel01=Cell.bIsPlayable && !Cell.bIsRiverbed ? FMath::Clamp(Profile.ForestFuel01*(.8f+Random.FRand()*.4f),0.f,1.f) : 0.f;
			Cell.SoilMoisture01=Cell.bIsRiverbed ? 1.f : Profile.Moisture01;
			Cell.bAgriculturalLand=Cell.bIsPlayable && !Cell.bIsRiverbed && Random.FRand()<Profile.Agriculture01;
			Cell.FaultLineProximity=FMath::Clamp(Cell.FaultLineProximity*Profile.FaultInfluence01,0.f,1.f);
			Cell.ValleyWindMultiplier=1.f+Profile.ValleyWindExposure01*.5f*(.4f+Cell.RiverProximity*.6f);
			Cell.ParcelSuitability=Cell.bIsRiverbed ? 0.f : .85f;
			continue;
		}
		if (ScenarioIndex >= 4)
		{
			const float Relief=FMath::Clamp((Cell.RealElevationMetres-MinM)/FMath::Max(1.f,MaxM-MinM),0.f,1.f);
			Cell.Elevation=80.f+Relief*(ScenarioIndex==5 ? 1450.f : (ScenarioIndex==6 ? 160.f : 660.f));
			if (ScenarioIndex==5 || ScenarioIndex==6)
			{
				// Etna lava terrain and Karapinar closed agricultural basin have no
				// invented perennial meandering river. Original4 river contracts remain.
				Cell.bIsRiverbed=false; Cell.RiverProximity=0.f; Cell.RiverDistance=100000.f;
				Cell.InitialFloodDepth=0.f; Cell.FloodDepth=0.f;
				Cell.ValleyWindMultiplier=1.f;
			}
			else if (Cell.bIsRiverbed) Cell.Elevation=FMath::Min(Cell.Elevation*.15f,35.f);
			Cell.SoilType=(ScenarioIndex==6 || Cell.RiverProximity>.1f) ? EAftershockSoilType::Alluvium : EAftershockSoilType::Bedrock;
			const uint32 Hash=GetTypeHash(Cell.Coords)^GetTypeHash(TerrainSeed);
			FRandomStream Random(static_cast<int32>(Hash));
			Cell.ForestFuel01=ScenarioIndex==4 && Cell.bIsPlayable && !Cell.bIsRiverbed ? .55f+Random.FRand()*.4f : (ScenarioIndex==5 ? .05f : .04f);
			Cell.SoilMoisture01=ScenarioIndex==4 ? .23f : (ScenarioIndex==6 ? .38f : .5f);
			Cell.bAgriculturalLand=ScenarioIndex==6 && Cell.bIsPlayable && !Cell.bIsRiverbed;
			Cell.ParcelSuitability=Cell.bIsRiverbed ? 0.f : .85f;
		}
	}
	if (bRegionalLayout && LocalRules.bHasVolcanicSource)
	{
		const auto& Site=GetAftershockGeographyFocus(ScenarioIndex,FocusIndex);
		const auto& Region=GetAftershockRegionRules(ScenarioIndex);
		VolcanicVentCoord=FIntPoint(FMath::Clamp(FMath::FloorToInt((Region.VolcanoLongitude-Site.Longitude+Site.HalfLongitudeExtent)/(Site.HalfLongitudeExtent*2.)*GridWidth),0,GridWidth-1),
			FMath::Clamp(FMath::FloorToInt((Site.Latitude+Site.HalfLatitudeExtent-Region.VolcanoLatitude)/(Site.HalfLatitudeExtent*2.)*GridHeight),0,GridHeight-1));
		if (IsPlayableCoord(VolcanicVentCoord)) { Cells[CoordToIndex(VolcanicVentCoord)].bVolcanicVent=true; Cells[CoordToIndex(VolcanicVentCoord)].ParcelSuitability=0.f; }
	}
	if (bRegionalLayout && LocalRules.bHasCoast)
	{
		// This is a compressed authored local coastline, not a DEM-derived survey.
		const bool Vertical=LocalRules.CoastEdge==EAftershockCoastEdge::East || LocalRules.CoastEdge==EAftershockCoastEdge::West;
		const int32 Size=Vertical ? GridWidth : GridHeight;
		const int32 SeaBand=FMath::Max(2,Size/5); CoastStartRow=GridHeight-SeaBand;
		for (FGridCell& Cell:Cells)
		{
			const int32 Inward=LocalRules.CoastEdge==EAftershockCoastEdge::North ? Cell.Coords.Y : LocalRules.CoastEdge==EAftershockCoastEdge::West ? Cell.Coords.X
				: LocalRules.CoastEdge==EAftershockCoastEdge::East ? GridWidth-1-Cell.Coords.X : GridHeight-1-Cell.Coords.Y;
			Cell.bIsOcean=Inward<SeaBand;
			Cell.bIsCoastal=!Cell.bIsOcean && Cell.bIsPlayable && Inward<SeaBand+3;
			if (!Cell.bIsOcean) continue;
			Cell.bIsPlayable=false; Cell.bIsRiverbed=false; Cell.bAgriculturalLand=false; Cell.ForestFuel01=0.f;
			Cell.Elevation=CoastalSeaLevelCm-140.f; Cell.InitialFloodDepth=140.f; Cell.FloodDepth=140.f; Cell.RiverProximity=0.f; Cell.ParcelSuitability=0.f;
		}
	}
	if (!bRegionalLayout && ScenarioIndex==5)
	{
		// Smithsonian GVP Etna211060 reference37.748N14.999E, projected
		// into the same sampled bounding box; not the hottest/random cell.
		const auto& Site=GetAftershockGeography(ScenarioIndex);
		VolcanicVentCoord=FIntPoint(FMath::Clamp(FMath::FloorToInt((14.999-(Site.Longitude-Site.HalfLongitudeExtent))/(Site.HalfLongitudeExtent*2.)*GridWidth),0,GridWidth-1),
			FMath::Clamp(FMath::FloorToInt(((Site.Latitude+Site.HalfLatitudeExtent)-37.748)/(Site.HalfLatitudeExtent*2.)*GridHeight),0,GridHeight-1));
	}
	if (!bRegionalLayout && ScenarioIndex==5 && IsPlayableCoord(VolcanicVentCoord))
	{
		Cells[CoordToIndex(VolcanicVentCoord)].bVolcanicVent=true;
		// The summit itself is a hazard source, never an authored building lot.
		Cells[CoordToIndex(VolcanicVentCoord)].ParcelSuitability=0.f;
	}
	const auto& Site=GetAftershockGeographyFocus(ScenarioIndex,FocusIndex);
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_OFFLINE_GEOGRAPHY index=%d focus=%d location=%s latitude=%.6f longitude=%.6f source=CopernicusGLO90_OpenMeteo cachedSamples=81 realAltitudeRangeM=%.2f..%.2f normalizedGameRelief=%d runtimeNetwork=0 legacyCampaignLayoutsPreserved=%d regionalLayout=%d soilForestRiverAuthoredNotSurvey=1"),
		ScenarioIndex,FocusIndex,Site.Label,Site.Latitude,Site.Longitude,MinM,MaxM,bRegionalLayout || ScenarioIndex>=4,!bRegionalLayout,bRegionalLayout);
	MarkTerrainEdited();
	return true;
}

FGridCell UTerrainGridSubsystem::GenerateContinuationCell(FIntPoint Coord,float SourceMinM,float SourceMaxM) const
{
	if(IsUsingRealGeography())
	{
		FGridCell Source;if(TryGenerateGeographicCell(Coord,Source))return Source;
		Source.Coords=Coord;Source.bIsPlayable=false;return Source; // Caller rejects; never an authored real-source substitute.
	}
	FGridCell Cell; Cell.Coords=Coord; Cell.bIsPlayable=true;
	Cell.bAuthoredContinuation=true; Cell.bHasRealElevationSample=false; Cell.RealElevationMetres=0.f;
	// Extend the original seeded field in global coordinates. The generation frame
	// remains immutable; adding a chunk never stretches existing hills or rivers.
	const auto NoiseAt=[this](FIntPoint At)
	{
		const float X=At.X+static_cast<float>(TerrainSeed)*17.13f,Y=At.Y+static_cast<float>(TerrainSeed)*29.37f;
		return (FMath::PerlinNoise2D(FVector2D(X*.04f,Y*.04f))+FMath::PerlinNoise2D(FVector2D(X*.12f,Y*.12f))*.35f)/1.35f;
	};
	const float Noise=NoiseAt(Coord),BaseElevation=FMath::Clamp((Noise+1.f)*.5f,0.f,1.f)*600.f;
	const float Y=static_cast<float>(Coord.Y)/FMath::Max(1,GridHeight-1);
	const float RiverX=GridWidth*.5f+FMath::Sin(Y*2.f*PI+TerrainSeed*.7f)*GridWidth*.22f+FMath::Sin(Y*4.f*PI+TerrainSeed*1.3f)*GridWidth*.08f;
	const float RiverDistance=FMath::Abs(Coord.X-RiverX);
	Cell.RiverDistance=RiverDistance*CellSize; Cell.RiverProximity=FMath::Clamp(1.f-RiverDistance/4.f,0.f,1.f);
	Cell.bIsRiverbed=RiverDistance<1.25f;
	Cell.Elevation=Cell.bIsRiverbed ? FMath::Min(BaseElevation*.2f,30.f)
		: RiverDistance<=3.5f ? FMath::Lerp(50.f,BaseElevation,(RiverDistance-1.25f)/2.25f) : BaseElevation;
	Cell.ValleyWindMultiplier=RiverDistance<=3.5f ? 1.35f : 1.f;
	Cell.InitialFloodDepth=Cell.bIsRiverbed ? 100.f : 0.f; Cell.FloodDepth=Cell.InitialFloodDepth;
	Cell.SoilType=RiverDistance<=4.f ? EAftershockSoilType::Alluvium : EAftershockSoilType::Bedrock;
	Cell.ForestFuel01=Cell.bIsRiverbed ? 0.f : FMath::Clamp(.45f+Noise*.5f,.08f,.9f);
	Cell.SoilMoisture01=Cell.bIsRiverbed ? 1.f : .65f; Cell.Slope=FMath::Abs(Noise)*15.f;
	const float A=GridHeight-1.f,B=-(GridWidth-1.f),Length=FMath::Sqrt(A*A+B*B);
	const float FaultDistance=Length>KINDA_SMALL_NUMBER ? FMath::Abs(A*Coord.X+B*Coord.Y)/Length : 0.f;
	Cell.FaultLineProximity=FMath::Clamp(1.f-FaultDistance/(FMath::Max(GridWidth,GridHeight)*.22f),0.f,1.f);
	Cell.ParcelSuitability=Cell.bIsRiverbed ? 0.f : FMath::Clamp(1.f-Cell.FaultLineProximity*.3f-Cell.RiverProximity*.3f,.1f,1.f);
	const bool bGeographic=IsAftershockGeographyFocusReady(GeographyIndex,GeographyFocusIndex);
	const auto& Rules=GetAftershockLocalFocusRules(GeographyIndex,GeographyFocusIndex);
	if (bGeographic && (bUsesRegionalLayout || GeographyIndex>=4))
	{
		// The source DEM covers the original selected focus only. Its nearest edge
		// provides a continuity anchor; ALL new heights are authored, never labelled
		// as real sampled terrain and never written into RealElevationMetres.
		const FIntPoint Edge(FMath::Clamp(Coord.X,0,GridWidth-1),FMath::Clamp(Coord.Y,0,GridHeight-1));
		const float EdgeM=SampleAftershockFocusElevationMetres(GeographyIndex,GeographyFocusIndex,(Edge.X+.5f)/GridWidth,(Edge.Y+.5f)/GridHeight);
		const float Relief=bUsesRegionalLayout ? Rules.LandscapeProfile.ReliefCm : GeographyIndex==5 ? 1450.f : GeographyIndex==6 ? 160.f : 660.f;
		const float EdgeRelief=FMath::Clamp((EdgeM-SourceMinM)/FMath::Max(1.f,SourceMaxM-SourceMinM),0.f,1.f);
		const float AuthoredRelief=FMath::Clamp(EdgeRelief+(Noise-NoiseAt(Edge))*.35f,0.f,1.f);
		Cell.Elevation=80.f+AuthoredRelief*Relief;
		FRandomStream Random(static_cast<int32>(GetTypeHash(Coord)^GetTypeHash(TerrainSeed)));
		if (bUsesRegionalLayout)
		{
			const auto& Profile=Rules.LandscapeProfile;
			const float V=(Coord.Y+.5f)/GridHeight;
			const float RegionalRiverX=Profile.RiverCenter01*GridWidth+FMath::Sin(V*2.f*PI+TerrainSeed*.7f)*GridWidth*.055f;
			const float Distance=FMath::Abs(Coord.X-RegionalRiverX);
			Cell.bIsRiverbed=Rules.bHasRiver && Distance<1.25f;
			Cell.RiverDistance=Rules.bHasRiver ? Distance*CellSize : 100000.f;
			Cell.RiverProximity=Rules.bHasRiver ? FMath::Clamp(1.f-Distance/4.f,0.f,1.f) : 0.f;
			Cell.Elevation=Cell.bIsRiverbed ? 25.f : Cell.Elevation;
			if (!Cell.bIsRiverbed && Cell.RiverProximity>0.f) Cell.Elevation=FMath::Lerp(Cell.Elevation,65.f,Cell.RiverProximity*.7f);
			Cell.SoilType=Cell.RiverProximity>.1f || Profile.Agriculture01>.55f ? EAftershockSoilType::Alluvium : EAftershockSoilType::Bedrock;
			Cell.ForestFuel01=Cell.bIsRiverbed ? 0.f : FMath::Clamp(Profile.ForestFuel01*(.8f+Random.FRand()*.4f),0.f,1.f);
			Cell.SoilMoisture01=Cell.bIsRiverbed ? 1.f : Profile.Moisture01;
			Cell.bAgriculturalLand=!Cell.bIsRiverbed && Random.FRand()<Profile.Agriculture01;
			Cell.FaultLineProximity=FMath::Clamp(Cell.FaultLineProximity*Profile.FaultInfluence01,0.f,1.f);
			Cell.ValleyWindMultiplier=1.f+Profile.ValleyWindExposure01*.5f*(.4f+Cell.RiverProximity*.6f);
		}
		else
		{
			if (GeographyIndex==5 || GeographyIndex==6)
			{
				Cell.bIsRiverbed=false; Cell.RiverProximity=0.f; Cell.RiverDistance=100000.f; Cell.ValleyWindMultiplier=1.f;
			}
			else if (Cell.bIsRiverbed) Cell.Elevation=FMath::Min(Cell.Elevation*.15f,35.f);
			Cell.SoilType=GeographyIndex==6 || Cell.RiverProximity>.1f ? EAftershockSoilType::Alluvium : EAftershockSoilType::Bedrock;
			Cell.ForestFuel01=GeographyIndex==4 && !Cell.bIsRiverbed ? .55f+Random.FRand()*.4f : GeographyIndex==5 ? .05f : .04f;
			Cell.SoilMoisture01=GeographyIndex==4 ? .23f : GeographyIndex==6 ? .38f : .5f;
			Cell.bAgriculturalLand=GeographyIndex==6 && !Cell.bIsRiverbed;
		}
		Cell.InitialFloodDepth=Cell.bIsRiverbed ? 100.f : 0.f; Cell.FloodDepth=Cell.InitialFloodDepth;
		Cell.ParcelSuitability=Cell.bIsRiverbed ? 0.f : .85f;
	}
	if (CoastStartRow!=INDEX_NONE)
	{
		int32 Inward=GridHeight-1-Coord.Y;
		int32 SeaBand=GridHeight-CoastStartRow;
		if (bUsesRegionalLayout && Rules.bHasCoast)
		{
			const bool bVertical=Rules.CoastEdge==EAftershockCoastEdge::East || Rules.CoastEdge==EAftershockCoastEdge::West;
			SeaBand=FMath::Max(2,(bVertical ? GridWidth : GridHeight)/5);
			Inward=Rules.CoastEdge==EAftershockCoastEdge::North ? Coord.Y : Rules.CoastEdge==EAftershockCoastEdge::West ? Coord.X
				: Rules.CoastEdge==EAftershockCoastEdge::East ? GridWidth-1-Coord.X : GridHeight-1-Coord.Y;
		}
		Cell.bIsOcean=Inward<SeaBand; Cell.bIsCoastal=!Cell.bIsOcean && Inward<SeaBand+3;
		if (Cell.bIsOcean)
		{
			Cell.bIsPlayable=false; Cell.bIsRiverbed=false; Cell.bAgriculturalLand=false; Cell.ForestFuel01=0.f;
			Cell.Elevation=CoastalSeaLevelCm-140.f; Cell.InitialFloodDepth=140.f; Cell.FloodDepth=140.f;
			Cell.RiverProximity=0.f; Cell.ParcelSuitability=0.f;
		}
	}
	// Newly exposed land enters the same current environmental turn state.
	Cell.DroughtStress01=FMath::Clamp(ActiveDroughtSeverity/100.f,0.f,1.f);
	return Cell;
}

bool UTerrainGridSubsystem::BeginChunkExpansion(TConstArrayView<FIntPoint> ChunkCoords,
    FAftershockTerrainChunkExpansion& Out,FText& OutError)
{
    const auto Reject=[&OutError](const TCHAR* Why){OutError=FText::FromString(Why);return false;};
    if(!IsInGameThread()||ChunkCoords.Num()>256||Cells.IsEmpty()||Cells.Num()>MaximumResidentCells||GridWidth<=0||GridHeight<=0
        ||static_cast<int64>(GridWidth)*GridHeight>Cells.Num())
        return Reject(TEXT("Canonical chunk preparation needs a bounded initialized game-thread world."));
    EnsureStorageLookup();FAftershockTerrainChunkExpansion Candidate;
    Candidate.Owner=this;Candidate.ExpectedRevision=TerrainRevision;Candidate.OriginalCells=Cells.Num();
    Candidate.RequestedChunks=ChunkCoords.Num();
    const FIntRect Supported=IsUsingRealGeography()?GetSupportedGeographicCellBounds():FIntRect(0,0,0,0);
    TSet<FIntPoint> Seen;Seen.Reserve(ChunkCoords.Num());
    for(FIntPoint ChunkCoord:ChunkCoords)
    {
        if(Seen.Contains(ChunkCoord))return Reject(TEXT("Duplicate chunk in canonical expansion request."));Seen.Add(ChunkCoord);
        const int64 StartX=static_cast<int64>(ChunkCoord.X)*ChunkSizeCells,StartY=static_cast<int64>(ChunkCoord.Y)*ChunkSizeCells;
        if(StartX < -MaximumAbsGlobalCellCoord || StartY < -MaximumAbsGlobalCellCoord
            ||StartX+ChunkSizeCells-1>MaximumAbsGlobalCellCoord||StartY+ChunkSizeCells-1>MaximumAbsGlobalCellCoord)
            return Reject(TEXT("Canonical expansion exceeds the world coordinate limit."));
        for(int32 Y=0;Y<ChunkSizeCells;++Y)for(int32 X=0;X<ChunkSizeCells;++X)
        {
            const FIntPoint Coord(static_cast<int32>(StartX+X),static_cast<int32>(StartY+Y));
            if(CellStorageIndices.Contains(Coord))continue;
            if(IsUsingRealGeography()&&!Supported.Contains(Coord))
                return Reject(TEXT("Canonical expansion exceeds the actual offline geographic source; no cells added."));
            if(Cells.Num()+Candidate.MissingCoords.Num()+1>MaximumResidentCells)
                return Reject(TEXT("Canonical expansion exceeds the 65,536-cell resident budget; no cells added."));
            Candidate.MissingCoords.Add(Coord);
        }
    }
    Candidate.SourceMinM=MAX_flt;Candidate.SourceMaxM=-MAX_flt;
    for(int32 I=0;I<GridWidth*GridHeight;++I)
    {Candidate.SourceMinM=FMath::Min(Candidate.SourceMinM,Cells[I].RealElevationMetres);Candidate.SourceMaxM=FMath::Max(Candidate.SourceMaxM,Cells[I].RealElevationMetres);}
    Candidate.PreparedCells.Reserve(Candidate.MissingCoords.Num());Candidate.bReady=Candidate.MissingCoords.IsEmpty();
    Out=MoveTemp(Candidate);OutError=FText::GetEmpty();return true;
}

bool UTerrainGridSubsystem::ContinueChunkExpansion(FAftershockTerrainChunkExpansion& Candidate,
    int32 MaximumGeneratedCells,FText& OutError)
{
    const auto Reject=[&OutError](const TCHAR* Why){OutError=FText::FromString(Why);return false;};
    if(!IsInGameThread()||Candidate.Owner.Get()!=this||Candidate.bCommitted
        ||Candidate.ExpectedRevision!=TerrainRevision||Candidate.OriginalCells!=Cells.Num()
        ||MaximumGeneratedCells<1||MaximumGeneratedCells>2500)
        return Reject(TEXT("Canonical expansion candidate is stale or its cooperative work budget is invalid."));
    const int32 End=FMath::Min(Candidate.MissingCoords.Num(),Candidate.PreparedCells.Num()+MaximumGeneratedCells);
    while(Candidate.PreparedCells.Num()<End)
    {
        const FIntPoint Coord=Candidate.MissingCoords[Candidate.PreparedCells.Num()];
        FGridCell Cell=GenerateContinuationCell(Coord,Candidate.SourceMinM,Candidate.SourceMaxM);
        if(IsUsingRealGeography()&&!Cell.bHasRealElevationSample)
            return Reject(TEXT("Offline geographic sample failed; the whole staged expansion remains uncommitted."));
        for(TFieldIterator<FFloatProperty> Property(FGridCell::StaticStruct());Property;++Property)
            if(!FMath::IsFinite(Property->GetPropertyValue_InContainer(&Cell)))
                return Reject(TEXT("Staged canonical terrain contains a nonfinite float; no cells added."));
        for(TFieldIterator<FDoubleProperty> Property(FGridCell::StaticStruct());Property;++Property)
            if(!FMath::IsFinite(Property->GetPropertyValue_InContainer(&Cell)))
                return Reject(TEXT("Staged canonical terrain contains a nonfinite double; no cells added."));
        Candidate.PreparedCells.Add(MoveTemp(Cell));
    }
    Candidate.bReady=Candidate.PreparedCells.Num()==Candidate.MissingCoords.Num();OutError=FText::GetEmpty();return true;
}

bool UTerrainGridSubsystem::CommitChunkExpansion(FAftershockTerrainChunkExpansion& Candidate,FText& OutError)
{
    const auto Reject=[&OutError](const TCHAR* Why){OutError=FText::FromString(Why);return false;};
    if(!IsInGameThread()||Candidate.Owner.Get()!=this||Candidate.bCommitted||!Candidate.bReady
        ||Candidate.ExpectedRevision!=TerrainRevision||Candidate.OriginalCells!=Cells.Num()
        ||Candidate.PreparedCells.Num()!=Candidate.MissingCoords.Num()
        ||Cells.Num()+Candidate.PreparedCells.Num()>MaximumResidentCells)
        return Reject(TEXT("Canonical expansion commit is incomplete, stale or over budget; no cells added."));
    EnsureStorageLookup();
    for(int32 I=0;I<Candidate.MissingCoords.Num();++I)
        if(Candidate.PreparedCells[I].Coords!=Candidate.MissingCoords[I]||CellStorageIndices.Contains(Candidate.MissingCoords[I]))
            return Reject(TEXT("Canonical expansion keys changed before commit; no cells added."));
    const int32 Added=Candidate.PreparedCells.Num();int32 Unlocked=0;
    if(Added>0)
    {
        // Exactly the existing explicit expansion policy, once for this batch.
        if(!bInitialMaskUnlocked)
        {for(int32 I=0;I<GridWidth*GridHeight;++I)if(!Cells[I].bIsOcean&&!Cells[I].bIsPlayable){Cells[I].bIsPlayable=true;++Unlocked;}bInitialMaskUnlocked=true;}
        Cells.Append(MoveTemp(Candidate.PreparedCells));RebuildStorageLookup();MarkTerrainEdited();
    }
    Candidate.bCommitted=true;OutError=FText::GetEmpty();
    if(Added>0)OnTerrainStorageChanged.Broadcast(TerrainRevision);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHUNK_BATCH_COMMITTED requestedChunks=%d appended=%d residentCells=%d initialNonOceanMaskUnlocked=%d revision=%llu realDEMOnly=%d atomicCommit=1"),
        Candidate.RequestedChunks,Added,Cells.Num(),Unlocked,TerrainRevision,IsUsingRealGeography());
    return true;
}

bool UTerrainGridSubsystem::EnsureChunkGenerated(FIntPoint ChunkCoord,FText& OutError)
{
	const auto Reject=[&OutError](const TCHAR* Reason) { OutError=FText::FromString(Reason); return false; };
	const int64 StartX=static_cast<int64>(ChunkCoord.X)*ChunkSizeCells,StartY=static_cast<int64>(ChunkCoord.Y)*ChunkSizeCells;
	if (StartX < -MaximumAbsGlobalCellCoord || StartY < -MaximumAbsGlobalCellCoord
		|| StartX+ChunkSizeCells-1>MaximumAbsGlobalCellCoord || StartY+ChunkSizeCells-1>MaximumAbsGlobalCellCoord)
		return Reject(TEXT("Parça koordinatı desteklenen dünya sınırlarının dışında."));
	if (Cells.Num()<1 || static_cast<int64>(GridWidth)*GridHeight>Cells.Num()) return Reject(TEXT("Başlangıç arazi depolaması henüz hazır değil."));
	EnsureStorageLookup();
	TArray<FIntPoint> Missing; Missing.Reserve(ChunkSizeCells*ChunkSizeCells);
	for (int32 Y=0;Y<ChunkSizeCells;++Y) for (int32 X=0;X<ChunkSizeCells;++X)
	{
		const FIntPoint Coord(static_cast<int32>(StartX+X),static_cast<int32>(StartY+Y));
		if (!CellStorageIndices.Contains(Coord)) Missing.Add(Coord);
	}
	if (Missing.IsEmpty()) { OutError=FText::GetEmpty(); return true; }
	if(IsUsingRealGeography())
	{
		const FIntRect Supported=GetSupportedGeographicCellBounds();
		for(FIntPoint Coord:Missing)if(!Supported.Contains(Coord))return Reject(TEXT("Bu parça çevrimdışı gerçek coğrafya veri sınırının dışında; uydurma arazi eklenmedi."));
	}
	if (Cells.Num()+Missing.Num()>MaximumResidentCells) return Reject(TEXT("Yerleşik arazi hücre bütçesi dolu; bu sürüm parça boşaltma desteklemiyor."));
	float SourceMinM=MAX_flt,SourceMaxM=-MAX_flt;
	for (int32 I=0;I<GridWidth*GridHeight;++I)
	{
		SourceMinM=FMath::Min(SourceMinM,Cells[I].RealElevationMetres); SourceMaxM=FMath::Max(SourceMaxM,Cells[I].RealElevationMetres);
	}
	TArray<FGridCell> Appended; Appended.Reserve(Missing.Num());
	for (FIntPoint Coord:Missing)
	{
		FGridCell Cell=GenerateContinuationCell(Coord,SourceMinM,SourceMaxM);
		if(IsUsingRealGeography()&&!Cell.bHasRealElevationSample)return Reject(TEXT("Gerçek coğrafya örneği çözülemedi; parça eklenmedi."));
		for (TFieldIterator<FFloatProperty> Property(FGridCell::StaticStruct());Property;++Property)
			if (!FMath::IsFinite(Property->GetPropertyValue_InContainer(&Cell))) return Reject(TEXT("Devam arazisi üretimi sayısal doğrulamadan geçemedi."));
		Appended.Add(MoveTemp(Cell));
	}
	// Commit atomically after all allocations/generation checks. The explicit
	// expansion policy removes only the old oval's non-ocean placement lock, so a
	// real road can connect to the new land. All physical/gameplay values survive.
	int32 Unlocked=0;
	if (!bInitialMaskUnlocked)
	{
		for (int32 I=0;I<GridWidth*GridHeight;++I) if (!Cells[I].bIsOcean && !Cells[I].bIsPlayable) { Cells[I].bIsPlayable=true; ++Unlocked; }
		bInitialMaskUnlocked=true;
	}
	Cells.Append(MoveTemp(Appended)); RebuildStorageLookup(); MarkTerrainEdited();
	OutError=FText::GetEmpty(); OnTerrainStorageChanged.Broadcast(TerrainRevision);
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_CHUNK_COMMITTED chunk=(%d,%d) appended=%d residentCells=%d residentChunks=%d initialNonOceanMaskUnlocked=%d revision=%llu realDEMOutsideOriginalFocus=0"),
		ChunkCoord.X,ChunkCoord.Y,Missing.Num(),Cells.Num(),ResidentChunks.Num(),Unlocked,TerrainRevision);
	return true;
}

FVector UTerrainGridSubsystem::GetCoastInlandDirection() const
{
	// Derive the current canonical shoreline, not just region metadata: original
	// campaign Samandag has its preserved south coast, free Samandag a west coast.
	// Cells persist this distinction across saves without a second mode authority.
	FVector Inward=FVector::ZeroVector;
	const FIntPoint Steps[]={FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1)};
	for (const FGridCell& Sea:Cells) if (Sea.bIsOcean)
		for (FIntPoint Step:Steps) if (IsPlayableCoord(Sea.Coords+Step) && !GetCell(Sea.Coords+Step).bIsOcean) Inward+=FVector(Step.X,Step.Y,0);
	if (!Inward.IsNearlyZero()) return Inward.GetSafeNormal2D();
	switch (GetAftershockLocalFocusRules(GeographyIndex,GeographyFocusIndex).CoastEdge)
	{
	case EAftershockCoastEdge::North: return FVector(0,1,0);
	case EAftershockCoastEdge::East: return FVector(-1,0,0);
	case EAftershockCoastEdge::West: return FVector(1,0,0);
	default: return FVector(0,-1,0);
	}
}

void UTerrainGridSubsystem::AdvanceEnvironmentalTurn()
{
	const float Drought=FMath::Clamp(ActiveDroughtSeverity/100.f,0.f,1.f);
	for (FGridCell& Cell : Cells)
	{
		if (!Cell.bIsPlayable || Cell.bIsOcean) continue;
		if (Cell.bBurning)
		{
			Cell.ForestFuel01=FMath::Max(0.f,Cell.ForestFuel01-.12f);
			Cell.BurningTurns=FMath::Max(0,Cell.BurningTurns-1);
			Cell.bBurning=Cell.BurningTurns>0 && Cell.ForestFuel01>.03f;
		}
		if (Drought>0.f)
		{
			Cell.SoilMoisture01=FMath::Max(0.f,Cell.SoilMoisture01-.025f*Drought);
			Cell.RiverFill01=FMath::Max(0.f,Cell.RiverFill01-.035f*Drought);
			Cell.DroughtStress01=FMath::Max(Cell.DroughtStress01,1.f-Cell.SoilMoisture01);
			if (Cell.bAgriculturalLand) Cell.CropHealth01=FMath::Max(0.f,Cell.CropHealth01-.025f*Cell.DroughtStress01);
		}
		else if (Cell.bAgriculturalLand && Cell.SoilMoisture01>.45f && Cell.DroughtStress01<.55f)
		{
			// Rain replenishes moisture through TriggerDisaster(Flood). Crops then
			// regrow gradually on real city turns; the display timer never heals them.
			Cell.CropHealth01=FMath::Min(1.f,Cell.CropHealth01+.018f*Cell.SoilMoisture01);
		}
		Cell.LavaCoverage01=FMath::Max(0.f,Cell.LavaCoverage01-.025f);
		Cell.AshLoad01=FMath::Max(0.f,Cell.AshLoad01-.012f);
	}
	MarkTerrainEdited();
}

bool UTerrainGridSubsystem::ConfigureCoastFromSouthernEdge(int32 InCoastStartRow)
{
	if(IsUsingRealGeography())return IsCoastalWorld(); // Never replace mapped coastline with an authored edge band.
	if (GridHeight < 2 || InCoastStartRow < 1 || InCoastStartRow >= GridHeight || Cells.Num() != GridWidth * GridHeight) return false;
	if (CoastStartRow != INDEX_NONE) return CoastStartRow == InCoastStartRow && IsCoastalWorld();
	// Do not silently remove authored buildings, roads, assembly areas or dams.
	// Scenarios configure the coast before their canonical settlement is placed.
	for (const FGridCell& Cell : Cells)
	{
		if (Cell.Coords.Y >= InCoastStartRow && (Cell.OccupyingBuildingID != INDEX_NONE || Cell.bHasRoad || Cell.bIsEmergencyAssemblyArea || Cell.bHasDam)) return false;
	}
	CoastStartRow = InCoastStartRow;
	int32 OceanCells = 0, CoastalLandCells = 0;
	for (FGridCell& Cell : Cells)
	{
		Cell.bIsOcean = Cell.Coords.Y >= CoastStartRow;
		Cell.bIsCoastal = !Cell.bIsOcean && Cell.bIsPlayable && Cell.Coords.Y >= CoastStartRow - 3;
		if (Cell.bIsCoastal) ++CoastalLandCells;
		if (!Cell.bIsOcean) continue;
		++OceanCells;
		Cell.bIsPlayable = false;
		Cell.bIsRiverbed = false;
		Cell.Elevation = CoastalSeaLevelCm - 140.f;
		Cell.RiverProximity = 0.f;
		Cell.InitialFloodDepth = 140.f;
		Cell.FloodDepth = 140.f;
		Cell.ParcelSuitability = 0.f;
	}
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_CANONICAL_COAST startRow=%d oceanCells=%d coastalLandCells=%d seaLevelCm=%.1f oceanPlayable=0 configuredBeforeSettlement=1"),
		CoastStartRow, OceanCells, CoastalLandCells, CoastalSeaLevelCm);
	MarkTerrainEdited();
	return true;
}

bool UTerrainGridSubsystem::IsCoastalWorld() const
{
	if(IsUsingRealGeography())return Cells.ContainsByPredicate([](const FGridCell& Cell){return Cell.bIsOcean;});
	return CoastStartRow > 0 && CoastStartRow < GridHeight
		&& Cells.ContainsByPredicate([](const FGridCell& Cell) { return Cell.bIsOcean; });
}

float UTerrainGridSubsystem::CalculateParcelSuitability(FIntPoint Coords) const
{
	if (!IsValidCoord(Coords))
	{
		return 0.0f;
	}

	const FGridCell& Cell = GetCell(Coords);
	if (Cell.bIsRiverbed || Cell.bIsOcean)
	{
		return 0.0f; // Nehir yatağına bina yapılamaz
	}

	float Suitability = 1.0f;
	Suitability -= Cell.FaultLineProximity * 0.3f;
	Suitability -= FMath::Clamp(Cell.Slope / 45.0f, 0.0f, 0.4f);
	Suitability -= Cell.RiverProximity * 0.3f;
	return FMath::Clamp(Suitability, 0.1f, 1.0f);
}

bool UTerrainGridSubsystem::UpgradeRiverPrevention(FIntPoint Coords, float ExtraCapacity, float ExtraDamping)
{
	if (!IsValidCoord(Coords))
	{
		return false;
	}

	FGridCell& Cell = Cells[CoordToIndex(Coords)];
	if (Cell.bIsOcean) return false;
	Cell.RiverCapacity = FMath::Max(0.0f, Cell.RiverCapacity + ExtraCapacity);
	Cell.WaterFlowDamping = FMath::Clamp(Cell.WaterFlowDamping + ExtraDamping, 0.0f, 1.0f);
	MarkTerrainEdited();
	return true;
}

bool UTerrainGridSubsystem::UpgradeCellDrainage(FIntPoint Coords, float BonusDrainage)
{
	if (!IsValidCoord(Coords))
	{
		return false;
	}

	FGridCell& Cell = Cells[CoordToIndex(Coords)];
	if (Cell.bIsOcean) return false;
	Cell.DrainageQuality = FMath::Clamp(Cell.DrainageQuality + BonusDrainage, 0.0f, 1.0f);
	MarkTerrainEdited();
	return true;
}

FDisasterOutcome UTerrainGridSubsystem::EvaluateBuildingInDisaster(
	FBuildingInstance& Building,
	EDisasterType DisasterType,
	float BaseDisasterSeverity,
	float CityAwarenessLevel,
	bool bIsDaytime,bool bHazardAlreadyLocalized)
{
	// New coastal hazard deliberately shares the existing flood susceptibility
	// contract. The caller's report/snapshot retains Tsunami; older formulas stay intact.
	const bool bCoastalSurge = DisasterType == EDisasterType::Tsunami;
	const bool bVolcanicDamage = DisasterType == EDisasterType::Volcano;
	const UCityLifeSubsystem* Life=GetWorld() ? GetWorld()->GetSubsystem<UCityLifeSubsystem>() : nullptr;
	if (DisasterType==EDisasterType::Drought)
	{
		FDisasterOutcome NoStructuralDamage;
		NoStructuralDamage.FinalDamage=Building.CurrentDamage;
		NoStructuralDamage.NewDamageState=Building.DamageState;
		NoStructuralDamage.Survivors=Life && Life->IsCityInitialized() ? Life->GetLivingOccupants(Building.InstanceID,bIsDaytime) : FMath::Max(0,Building.BuildingData ? Building.BuildingData->Residents : 0);
		return NoStructuralDamage;
	}
	if (bCoastalSurge) DisasterType = EDisasterType::Flood;
	if (bVolcanicDamage) DisasterType=EDisasterType::Fire;
	const FGridCell Cell = GetCell(Building.GridCoords);

	// 1) BUILDING_STATS.md Bölüm 5 — Dayanıklılık Formülü
	const float BaseResistance = Building.BuildingData
		? Building.BuildingData->GetBaseResistance(DisasterType)
		: 0.3f;

	float FoundationBonus = 0.0f;
	if (DisasterType == EDisasterType::Earthquake)
	{
		switch (Building.Foundation)
		{
		case EFoundationQuality::Weak:   FoundationBonus = 0.0f; break;
		case EFoundationQuality::Medium: FoundationBonus = 0.1f; break;
		case EFoundationQuality::Strong: FoundationBonus = 0.3f; break;
		}
	}
	else if (DisasterType == EDisasterType::Flood)
	{
		switch (Building.Foundation)
		{
		case EFoundationQuality::Weak:   FoundationBonus = -0.1f; break;
		case EFoundationQuality::Medium: FoundationBonus = 0.0f; break;
		case EFoundationQuality::Strong: FoundationBonus = 0.2f; break;
		}
	}

	const float HeightPenalty = static_cast<float>(FMath::Max(1, Building.CurrentFloors) - 1) * 0.03f;
	const float AgeDecay = FMath::Max(0.5f, 1.0f - (static_cast<float>(Building.AgeInYears) / 10.0f) * 0.05f);
	const float RetrofitBonus = Building.bIsRetrofitted ? 0.2f : 0.0f;

	const float TotalResistance = FMath::Clamp(
		(BaseResistance + FoundationBonus + RetrofitBonus - HeightPenalty) * AgeDecay,
		0.05f,
		0.98f
	);

	// 2) Arazi Çarpanları ve Şehir Önleme Bonusu
	float DisasterSeverity = BaseDisasterSeverity;
	float CityPreventionBonus = 0.0f;

	switch (DisasterType)
	{
	case EDisasterType::Earthquake:
		DisasterSeverity = BaseDisasterSeverity * (1.0f + Cell.FaultLineProximity * 0.5f)
			* (Cell.SoilType==EAftershockSoilType::Alluvium ? 1.18f : 1.f);
		CityPreventionBonus = 0.0f;
		break;

	case EDisasterType::Flood:
	{
		const float RiverOverflow = Cell.bIsRiverbed
			? FMath::Max(0.5f, 1.8f - Cell.RiverCapacity * 0.4f)
			: 1.0f;
		DisasterSeverity = bHazardAlreadyLocalized?BaseDisasterSeverity:BaseDisasterSeverity * RiverOverflow * (1.0f - Cell.WaterFlowDamping * 0.5f);
		CityPreventionBonus = bHazardAlreadyLocalized?0.f:Cell.DrainageQuality * 20.0f;
		break;
	}

	case EDisasterType::Wind:
		DisasterSeverity = bHazardAlreadyLocalized?BaseDisasterSeverity:BaseDisasterSeverity * Cell.ValleyWindMultiplier;
		CityPreventionBonus = 0.0f;
		break;

	case EDisasterType::Fire:
		DisasterSeverity = BaseDisasterSeverity;
		CityPreventionBonus = 0.f; // Street storm drains do not suppress a forest/lava fire.
		break;
	}
	if (bCoastalSurge)
	{
		// Reuse flood material/foundation susceptibility, while ordinary storm
		// drains, river widening/damping and inland dams provide NO tsunami defence.
		DisasterSeverity = BaseDisasterSeverity;
		CityPreventionBonus = 0.f;
	}
	if (bVolcanicDamage) CityPreventionBonus=0.f;

	// 3) Hasar Hesabı
	const float Damage = FMath::Clamp(
		DisasterSeverity * (1.0f - TotalResistance) - CityPreventionBonus,
		0.0f,
		100.0f
	);

	Building.CurrentDamage = FMath::Clamp(Building.CurrentDamage + Damage, 0.0f, 100.0f);

	// 4) Hasar Eşikleri ve Ölüm Katsayısı (DeathCoefficient)
	EDamageState NewDamageState;
	float DeathCoefficient = 0.0f;

	if (Building.CurrentDamage <= 0.0f)
	{
		NewDamageState = EDamageState::Intact;
		DeathCoefficient = 0.0f;
	}
	else if (Building.CurrentDamage <= 20.0f)
	{
		NewDamageState = EDamageState::Cracked;
		DeathCoefficient = 0.02f;
	}
	else if (Building.CurrentDamage <= 50.0f)
	{
		NewDamageState = EDamageState::Damaged;
		DeathCoefficient = 0.10f;
	}
	else if (Building.CurrentDamage < 80.0f)
	{
		NewDamageState = EDamageState::Severe;
		DeathCoefficient = 0.30f;
	}
	else
	{
		NewDamageState = EDamageState::Collapsed;
		DeathCoefficient = 0.60f;
	}

	Building.DamageState = NewDamageState;

	// 5) BUILDING_STATS.md Bölüm 6 — Ölüm / Kurtulma ve Ekonomik Çıktılar
	const float ClampedAwareness = FMath::Clamp(CityAwarenessLevel, 0.0f, 1.0f);
	const float PanicLevel = (Building.CurrentDamage / 100.0f) * (1.0f - ClampedAwareness) * 0.5f;

	const int32 Residents = Building.BuildingData ? Building.BuildingData->Residents : 4;
	const int32 Workers = Building.BuildingData ? Building.BuildingData->Workers : 0;
	// Only isolated legacy fixtures without an initialized ledger use capacity.
	// A job slot is NEVER an extra person in a live game.
	const int32 ResidentsAtRisk = Life && Life->IsCityInitialized()
		? Life->GetLivingOccupants(Building.InstanceID,bIsDaytime)
		: Residents + (bIsDaytime ? Workers : 0);

	const int32 Deaths = FMath::Clamp(
		FMath::RoundToInt(static_cast<float>(ResidentsAtRisk) * PanicLevel * DeathCoefficient),
		0,
		ResidentsAtRisk
	);
	const int32 Survivors = ResidentsAtRisk - Deaths;
	const float SatisfactionChange = -static_cast<float>(Deaths) * 0.3f;
	const int32 StateAidNextTurn = Survivors * 150;

	// 6) Sonuç Paketi
	FDisasterOutcome Outcome;
	Outcome.FinalDamage = Building.CurrentDamage;
	Outcome.NewDamageState = Building.DamageState;
	Outcome.Deaths = Deaths;
	Outcome.Survivors = Survivors;
	Outcome.SatisfactionChange = SatisfactionChange;
	Outcome.StateAidNextTurn = StateAidNextTurn;

	return Outcome;
}

FGridCell UTerrainGridSubsystem::GetCell(FIntPoint Coords) const
{
	if (IsValidCoord(Coords))
	{
		return Cells[CoordToIndex(Coords)];
	}

	FGridCell DefaultCell;
	DefaultCell.Coords = Coords;
	DefaultCell.bIsPlayable=false;
	return DefaultCell;
}

bool UTerrainGridSubsystem::SetCell(FIntPoint Coords, const FGridCell& NewCell)
{
	if (NewCell.Coords!=Coords || !IsValidCoord(Coords))
	{
		return false;
	}

	const int32 Index=CoordToIndex(Coords);
	const bool bMetadataChanged=Cells[Index].bAuthoredContinuation!=NewCell.bAuthoredContinuation;
	Cells[Index] = NewCell;
	if (bMetadataChanged) CachedStorageCount=INDEX_NONE;
	MarkTerrainEdited();
	return true;
}

FIntPoint UTerrainGridSubsystem::WorldToGrid(FVector WorldLocation) const
{
	if (!FMath::IsFinite(CellSize) || CellSize <= KINDA_SMALL_NUMBER || WorldLocation.ContainsNaN())
	{
		return FIntPoint(MAX_int32,MAX_int32);
	}
	const double LocalX=(WorldLocation.X-GridOrigin.X)/CellSize,LocalY=(WorldLocation.Y-GridOrigin.Y)/CellSize;
	if (!FMath::IsFinite(LocalX) || !FMath::IsFinite(LocalY) || LocalX < -MaximumAbsGlobalCellCoord || LocalX>=MaximumAbsGlobalCellCoord+1.
		|| LocalY < -MaximumAbsGlobalCellCoord || LocalY>=MaximumAbsGlobalCellCoord+1.) return FIntPoint(MAX_int32,MAX_int32);
	const int32 X = FMath::FloorToInt(LocalX);
	const int32 Y = FMath::FloorToInt(LocalY);
	return FIntPoint(X, Y);
}

FVector UTerrainGridSubsystem::GridToWorld(FIntPoint GridCoords, bool bCenter) const
{
	const float Offset = bCenter ? (CellSize * 0.5f) : 0.0f;
	const double WorldX = GridOrigin.X + static_cast<double>(GridCoords.X) * CellSize + Offset;
	const double WorldY = GridOrigin.Y + static_cast<double>(GridCoords.Y) * CellSize + Offset;
	const float WorldZ = GetCell(GridCoords).Elevation;
	return FVector(WorldX, WorldY, WorldZ);
}
