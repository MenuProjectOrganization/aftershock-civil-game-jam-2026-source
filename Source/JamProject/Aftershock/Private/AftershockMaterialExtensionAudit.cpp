#include "AftershockMassMovementTerrain.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include <limits>

#if WITH_EDITOR
namespace AftershockMaterialExtensionAuditPrivate
{
TArray<uint8> CellBytes(const TArray<FGridCell>& Cells)
{
    TArray<uint8> Out;FMemoryWriter W(Out);int32 Count=Cells.Num();W<<Count;
    for(const auto& C:Cells)FGridCell::StaticStruct()->SerializeBin(W,const_cast<FGridCell*>(&C));
    return Out;
}
bool RestoreCellBytes(const TArray<uint8>& Bytes,TArray<FGridCell>& Out)
{
    FMemoryReader R(Bytes);int32 Count=0;R<<Count;if(Count<0||Count>65536)return false;
    TArray<FGridCell> Candidate;Candidate.SetNum(Count);
    for(auto& C:Candidate)FGridCell::StaticStruct()->SerializeBin(R,&C);
    if(R.IsError()||R.Tell()!=R.TotalSize())return false;Out=MoveTemp(Candidate);return true;
}
FGridCell NewCell(FIntPoint Coord)
{
    FGridCell C;C.Coords=Coord;C.bIsPlayable=true;C.bAuthoredContinuation=Coord.X>=4;
    C.Elevation=100000.f-400.f*Coord.Y;C.SoilType=EAftershockSoilType::Alluvium;C.ForestFuel01=.7f;
    C.RiverFill01=.37f;return C;
}
void AppendCells(UTerrainGridSubsystem& T,const TArray<FIntPoint>& Coords)
{for(FIntPoint Coord:Coords)T.Cells.Add(NewCell(Coord));}
bool SameProfile(const FAftershockMaterialContinuationProfile& A,const FAftershockMaterialContinuationProfile& B)
{
    TSharedPtr<FJsonObject> JA,JB;FString Why,SA,SB;
    return AftershockMassTerrain::EncodeMaterialContinuationProfile(A,JA,Why)
        &&AftershockMassTerrain::EncodeMaterialContinuationProfile(B,JB,Why)
        &&FJsonSerializer::Serialize(JA,TJsonWriterFactory<>::Create(&SA))
        &&FJsonSerializer::Serialize(JB,TJsonWriterFactory<>::Create(&SB))&&SA==SB;
}
}

/** Call only on an isolated disposable native fixture world. This tests actual
 * canonical material APIs and a reflected-cell/profile snapshot round trip;
 * it does not claim public SaveWorld, async worker, RHI or packaged acceptance. */
static bool RunAftershockMaterialExtensionChecks(UTerrainGridSubsystem& FixtureTerrain,const FString& AbsoluteReportPath)
{
    using namespace AftershockMaterialExtensionAuditPrivate;
    TArray<TSharedPtr<FJsonValue>> Rows;int32 Failed=0;
    const auto Check=[&](const TCHAR* Name,bool Pass)
    {
        auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("name"),Name);Row->SetBoolField(TEXT("passed"),Pass);
        Rows.Add(MakeShared<FJsonValueObject>(Row));Failed+=Pass?0:1;
        if(Pass){UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_MATERIAL_EXTENSION_PASS %s"),Name);}
        else{UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_MATERIAL_EXTENSION_FAIL %s"),Name);}
    };
    const auto Finish=[&]()
    {
        auto Report=MakeShared<FJsonObject>();Report->SetBoolField(TEXT("valid"),Failed==0);
        Report->SetNumberField(TEXT("passed"),Rows.Num()-Failed);Report->SetNumberField(TEXT("failed"),Failed);
        Report->SetStringField(TEXT("scope"),TEXT("Isolated actual GT canonical material subset and complete saved-profile codec; reflected current-cell binary snapshot restore. Not public SaveWorld migration, worker timing, streamed real DEM, rendered snow, RHI, cook or performance proof."));
        Report->SetArrayField(TEXT("checks"),Rows);FString Text;
        if(!FJsonSerializer::Serialize(Report,TJsonWriterFactory<>::Create(&Text)))return false;
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsoluteReportPath),true);
        return FFileHelper::SaveStringToFile(Text,*AbsoluteReportPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)&&Failed==0;
    };
    Check(TEXT("fixture_is_game_thread_with_world"),IsInGameThread()&&FixtureTerrain.GetWorld()!=nullptr);
    if(!IsInGameThread()||!FixtureTerrain.GetWorld())return Finish();
    FixtureTerrain.GenerateProceduralTerrain(4,4,1000.f,87031);FixtureTerrain.ContinuationGeneratorVersion=3;
    for(const auto& Existing:FixtureTerrain.Cells)FixtureTerrain.SetCell(Existing.Coords,NewCell(Existing.Coords));
    FAftershockMaterialContinuationProfile Profile;
    auto& H=Profile.Weather;H.SourceCode=TEXT("FixtureExplicit12DayHistory");H.ContextSourceURL=TEXT("https://power.larc.nasa.gov/docs/methodology/citations/");
    H.Assumptions=TEXT("Isolated authored 12-day regression, not observed meteorology.");H.ReferenceElevationM=1000.;
    H.AllSnowC=-1.;H.AllRainC=1.8;H.MeltBaseC=.25;H.MeltMmPerDegreeDay=2.75;H.SnowWeakLayerStrengthPa=80.;
    FAftershockMaterialWeatherSegment A;A.Days=5;A.AirTemperatureC=-8.;A.PrecipitationMmPerDay=12.;A.SuppliedSnowWaterMmPerDay=8.;H.Segments.Add(A);
    FAftershockMaterialWeatherSegment B;B.Days=7;B.AirTemperatureC=-4.;B.PrecipitationMmPerDay=10.;H.Segments.Add(B);
    Profile.Mantle.SourceCode=TEXT("FixtureExplicitMantleV1");Profile.Mantle.DepthM=.8;Profile.Mantle.Porosity01=.4;
    Profile.Mantle.DryBulkDensityKgM3=1500.;Profile.Mantle.CohesionPa=800.;Profile.Mantle.FrictionAngleDegrees=30.;Profile.Mantle.InitialPoreWaterDepthM=.12;
    Profile.Mantle.bExplicitWeatheredMantleOverScenarioBedrock=true;
    TArray<FIntPoint> Initial;for(const auto& C:FixtureTerrain.Cells)Initial.Add(C.Coords);
    TArray<FAftershockAuthoredRegolithPatch> Patches;FString Why;
    const bool InitialPatches=AftershockMassTerrain::BuildAuthoredMantlePatches(FixtureTerrain,Profile,Initial,Patches,Why);
    const auto First=InitialPatches?AftershockMassTerrain::InitializeAuthoredMaterialScenario(FixtureTerrain,H,Patches,true):FAftershockMaterialInitializationReceipt{};
    Check(TEXT("initial_explicit_history_and_mantle_applied"),First.bApplied&&First.SnowCells==16&&First.RegolithCells>0);
    if(!First.bApplied)return Finish();
    // Consumption, erosion and deposition are legitimate Version 1 state. New
    // chunks must not reset even a now-empty consumed stock.
    auto Consumed=FixtureTerrain.GetCell(FIntPoint(1,1));Consumed.SnowWaterEquivalentMm=0.f;
    Consumed.RegolithDepthM=Consumed.SoilPoreWaterDepthM=0.f;Consumed.GroundErodedDepthM=.2f;Consumed.GroundDepositedDepthM=.15f;
    FixtureTerrain.SetCell(Consumed.Coords,Consumed);const auto PriorCells=FixtureTerrain.Cells;
    const auto PriorBytes=CellBytes(PriorCells);TSharedPtr<FJsonObject> Contract;FString ContractText;
    const bool Encoded=AftershockMassTerrain::EncodeMaterialContinuationProfile(Profile,Contract,Why)
        &&FJsonSerializer::Serialize(Contract,TJsonWriterFactory<>::Create(&ContractText));
    TSharedPtr<FJsonObject> Parsed;FAftershockMaterialContinuationProfile RestoredProfile;
    const bool Decoded=Encoded&&FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ContractText),Parsed)
        &&AftershockMassTerrain::DecodeMaterialContinuationProfile(Parsed,RestoredProfile,Why);
    Check(TEXT("complete_saved_weather_melt_and_mantle_codec_exact"),Decoded&&SameProfile(Profile,RestoredProfile));
    TStrongObjectPtr<UTerrainGridSubsystem> Restored(NewObject<UTerrainGridSubsystem>(FixtureTerrain.GetWorld()));
    Restored->GenerateProceduralTerrain(4,4,1000.f,87031);Restored->ContinuationGeneratorVersion=3;
    const bool CellsRestored=RestoreCellBytes(PriorBytes,Restored->Cells);
    Check(TEXT("reflected_current_cell_snapshot_preserves_consumed_stocks"),CellsRestored&&CellBytes(Restored->Cells)==PriorBytes);
    TArray<FIntPoint> Added;for(int32 Y=0;Y<4;++Y)for(int32 X=4;X<6;++X)Added.Add(FIntPoint(X,Y));
    AppendCells(FixtureTerrain,Added);AppendCells(*Restored,Added);
    int32 ReceiptStages=0;const auto Stage=[&](const FAftershockMaterialInitializationReceipt& Prepared,FString& Failure)
    {++ReceiptStages;return Prepared.CellsVisited==Added.Num();};
    const auto BeforeRevision=FixtureTerrain.GetTerrainRevision();
    const bool Built=AftershockMassTerrain::BuildAuthoredMantlePatches(FixtureTerrain,Profile,Added,Patches,Why);
    const auto Extension=Built?AftershockMassTerrain::InitializeAuthoredMaterialSubset(FixtureTerrain,H,Patches,Added,Stage):FAftershockMaterialInitializationReceipt{};
    Check(TEXT("new_resident_cells_initialized_once_from_saved_history"),Extension.bApplied&&Extension.CellsVisited==Added.Num()&&ReceiptStages==1
        &&Extension.SnowCells==Added.Num()&&Extension.RegolithCells>0);
    TArray<FGridCell> Preserved;Preserved.Append(FixtureTerrain.Cells.GetData(),PriorCells.Num());
    Check(TEXT("all_existing_fields_and_consumed_deposits_unchanged"),CellBytes(Preserved)==PriorBytes);
    Check(TEXT("only_target_SetCell_writes_change_revision"),FixtureTerrain.GetTerrainRevision()-BeforeRevision==static_cast<uint64>(Added.Num()));
    bool NoWaterInjection=true,Versioned=true;for(FIntPoint Coord:Added)
    {const auto C=FixtureTerrain.GetCell(Coord);NoWaterInjection&=C.RiverFill01==.37f;Versioned&=C.MaterialScenarioVersion==1&&C.SnowWaterEquivalentMm>0.f;}
    Check(TEXT("no_antecedent_meltwater_added_to_live_water_state"),NoWaterInjection&&Versioned&&Extension.AntecedentLiquidOutflowM3>0.);
    TArray<FAftershockAuthoredRegolithPatch> RestoredPatches;
    const bool RestoreBuilt=Decoded&&CellsRestored&&AftershockMassTerrain::BuildAuthoredMantlePatches(*Restored,RestoredProfile,Added,RestoredPatches,Why);
    const auto RestoreExtension=RestoreBuilt?AftershockMassTerrain::InitializeAuthoredMaterialSubset(*Restored,RestoredProfile.Weather,RestoredPatches,Added,
        [](const auto&,FString&){return true;}):FAftershockMaterialInitializationReceipt{};
    Check(TEXT("new_chunk_after_profile_and_cell_restore_is_identical"),RestoreExtension.bApplied&&CellBytes(Restored->Cells)==CellBytes(FixtureTerrain.Cells)
        &&RestoreExtension.SnowWaterKg==Extension.SnowWaterKg&&RestoreExtension.DrySoilKg==Extension.DrySoilKg&&RestoreExtension.PoreWaterM3==Extension.PoreWaterM3);
    const auto AfterOnce=CellBytes(FixtureTerrain.Cells);const auto RevisionOnce=FixtureTerrain.GetTerrainRevision();
    const auto Repeat=AftershockMassTerrain::InitializeAuthoredMaterialSubset(FixtureTerrain,H,Patches,Added,[](const auto&,FString&){return true;});
    Check(TEXT("repeated_subset_rejected_without_stock_or_revision_change"),!Repeat.bApplied&&CellBytes(FixtureTerrain.Cells)==AfterOnce&&FixtureTerrain.GetTerrainRevision()==RevisionOnce);
    const FIntPoint ZeroCoord(6,1);AppendCells(FixtureTerrain,{ZeroCoord});FAftershockMaterialWeatherHistory NoSnow;NoSnow.SourceCode=TEXT("ExplicitNoSnowHistoryV1");
    const auto Zero=AftershockMassTerrain::InitializeAuthoredMaterialSubset(FixtureTerrain,NoSnow,{},TArray<FIntPoint>{ZeroCoord},[](const auto&,FString&){return true;});
    const auto ZeroCell=FixtureTerrain.GetCell(ZeroCoord);
    Check(TEXT("zero_initial_inventory_still_versioned_once"),Zero.bApplied&&ZeroCell.MaterialScenarioVersion==1&&ZeroCell.SnowWaterEquivalentMm==0.f&&ZeroCell.RegolithDepthM==0.f);
    const FIntPoint Pending(7,1);AppendCells(FixtureTerrain,{Pending});
    const auto RejectUnchanged=[&](const TCHAR* Name,const FAftershockMaterialWeatherHistory& Weather,
        const TArray<FAftershockAuthoredRegolithPatch>& Layers,const TArray<FIntPoint>& Targets,bool RejectReceipt=false)
    {
        const auto Before=CellBytes(FixtureTerrain.Cells);const auto Revision=FixtureTerrain.GetTerrainRevision();
        const auto Result=AftershockMassTerrain::InitializeAuthoredMaterialSubset(FixtureTerrain,Weather,Layers,Targets,
            [&](const auto&,FString& Error){if(RejectReceipt)Error=TEXT("Fixture provenance size refusal before commit");return !RejectReceipt;});
        Check(Name,!Result.bApplied&&Before==CellBytes(FixtureTerrain.Cells)&&Revision==FixtureTerrain.GetTerrainRevision());
    };
    RejectUnchanged(TEXT("duplicate_target_rejects_whole_transaction"),H,{},TArray<FIntPoint>{Pending,Pending});
    RejectUnchanged(TEXT("nonresident_target_rejects_whole_transaction"),H,{},TArray<FIntPoint>{Pending,FIntPoint(900,900)});
    RejectUnchanged(TEXT("receipt_staging_failure_has_no_material_mutation"),H,{},TArray<FIntPoint>{Pending},true);
    auto InvalidWeather=H;InvalidWeather.Segments[0].PrecipitationMmPerDay=-1.;
    RejectUnchanged(TEXT("malformed_saved_weather_rejects_before_commit"),InvalidWeather,{},TArray<FIntPoint>{Pending});
    auto InvalidTarget=FixtureTerrain.GetCell(Pending);InvalidTarget.RegolithDepthM=.01f;FixtureTerrain.SetCell(Pending,InvalidTarget);
    RejectUnchanged(TEXT("version_zero_nonzero_stock_never_silently_reset"),H,{},TArray<FIntPoint>{Pending});
    FixtureTerrain.SetCell(Pending,NewCell(Pending));
    auto Unrelated=FixtureTerrain.GetCell(FIntPoint(0,0));const auto ValidUnrelated=Unrelated;
    Unrelated.SnowWaterEquivalentMm=std::numeric_limits<float>::quiet_NaN();FixtureTerrain.SetCell(Unrelated.Coords,Unrelated);
    RejectUnchanged(TEXT("invalid_preserved_material_rejects_entire_snapshot"),H,{},TArray<FIntPoint>{Pending});
    FixtureTerrain.SetCell(ValidUnrelated.Coords,ValidUnrelated);
    auto DuplicateLayer=Profile.Mantle;DuplicateLayer.Cells={Pending,Pending};
    RejectUnchanged(TEXT("duplicate_mantle_patch_never_partially_applies"),H,TArray<FAftershockAuthoredRegolithPatch>{DuplicateLayer},TArray<FIntPoint>{Pending});
    // Missing old receipt fields are not filled from C++ defaults.
    TSharedPtr<FJsonObject> Incomplete;AftershockMassTerrain::EncodeMaterialContinuationProfile(Profile,Incomplete,Why);
    Incomplete->RemoveField(TEXT("melt_mm_per_degree_day"));FAftershockMaterialContinuationProfile Kept=Profile;
    const bool MissingRejected=!AftershockMassTerrain::DecodeMaterialContinuationProfile(Incomplete,Kept,Why);
    Check(TEXT("incomplete_old_weather_contract_deferred_without_guessing"),MissingRejected&&SameProfile(Kept,Profile));
    TSharedPtr<FJsonObject> BadProfile;AftershockMassTerrain::EncodeMaterialContinuationProfile(Profile,BadProfile,Why);BadProfile->SetNumberField(TEXT("profile_version"),99.);
    Check(TEXT("unknown_material_profile_version_rejected"),!AftershockMassTerrain::DecodeMaterialContinuationProfile(BadProfile,Kept,Why));
    return Finish();
}

bool RunAftershockMaterialExtensionFixtures(const FString& AbsoluteReportPath)
{
    if(!GEngine||!IsInGameThread())return false;
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("MaterialExtensionFixture")),GetTransientPackage(),true,ERHIFeatureLevel::SM5,nullptr,false);
    if(!World)return false;GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    auto* Terrain=World->GetSubsystem<UTerrainGridSubsystem>();
    const bool Passed=Terrain&&RunAftershockMaterialExtensionChecks(*Terrain,AbsoluteReportPath);
    World->DestroyWorld(false);GEngine->DestroyWorldContext(World);World->RemoveFromRoot();return Passed;
}
#endif
