#include "AftershockPhysicalReplayAudit.h"
#include "AftershockPhysicalReplay.h"
#include "AftershockMassMovementVisual.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Compression.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include <limits>

namespace AftershockPhysicalReplayAuditDetail
{
FAftershockMassInput Fixture(EAftershockMassMaterial Material)
{
    FAftershockMassInput I;I.Material=Material;I.HazardTypeTag=Material==EAftershockMassMaterial::Snow?7:8;
    I.TerrainRevision=123;I.CellSizeM=10.;I.Settings.MaximumSeconds=60.;
    const double Gradient=FMath::Tan(FMath::DegreesToRadians(35.));
    for(int32 Y=0;Y<24;++Y)for(int32 X=0;X<20;++X)
    {
        FAftershockMassCell C;C.Coord=FIntPoint(X,Y);C.GroundElevationM=-FMath::Min(Y,12)*I.CellSizeM*Gradient;
        C.SnowWaterEquivalentMm=Material==EAftershockMassMaterial::Snow?180.:0.;C.SnowDensityKgM3=300.;C.SnowWeakLayerStrengthPa=50.;
        C.RegolithVerticalDepthM=1.;C.SoilDensityKgM3=1800.;C.CohesionPa=50.;C.FrictionAngleDegrees=25.;C.EffectiveSaturation01=1.;I.Cells.Add(C);
    }
    FAftershockMassRelease R;R.Coord=FIntPoint(8,4);R.AvailableMassFraction=.5;I.Release.Add(R);return I;
}
FAftershockMassVisualEvent Event(const FAftershockMassInput& I)
{
    FAftershockMassVisualEvent V;V.EventID=FGuid(0x12,0x34,0x56,0x78);V.StartedAtWorldSeconds=10.;
    V.CellSizeM=I.CellSizeM;V.GridOriginM=I.GridOriginM;V.SupportCells=I.Cells;V.Obstacles=I.Obstacles;
    V.Result=FAftershockMassMovementSolver::Solve(I);return V;
}
bool Close(double A,double B)
{return FMath::IsFinite(A)&&FMath::IsFinite(B)&&FMath::Abs(A-B)<=FMath::Max3(1.,FMath::Abs(A),FMath::Abs(B))*1.e-8;}
bool Same(const FAftershockMassVisualEvent&A,const FAftershockMassVisualEvent&B)
{
    if(A.EventID!=B.EventID||A.StartedAtWorldSeconds!=B.StartedAtWorldSeconds||A.CellSizeM!=B.CellSizeM||A.GridOriginM!=B.GridOriginM
        ||A.WorldOffsetCm!=B.WorldOffsetCm||A.SupportCells.Num()!=B.SupportCells.Num()||A.Obstacles.Num()!=B.Obstacles.Num())return false;
    const auto& X=A.Result;const auto& Y=B.Result;
    if(X.Material!=Y.Material||X.HazardTypeTag!=Y.HazardTypeTag||X.SourceTerrainRevision!=Y.SourceTerrainRevision||X.Cells.Num()!=Y.Cells.Num()
        ||X.Impacts.Num()!=Y.Impacts.Num()||X.Frames.Num()!=Y.Frames.Num()||!Close(X.SimulatedSeconds,Y.SimulatedSeconds)
        ||!Close(X.Budget.DepositedKg,Y.Budget.DepositedKg)||!Close(X.Budget.GravityWorkJ,Y.Budget.GravityWorkJ)
        ||X.ExportedFrameCellRecords!=Y.ExportedFrameCellRecords||X.ExportedFrameParcelRecords!=Y.ExportedFrameParcelRecords
        ||X.ExportedFrameImpactRecords!=Y.ExportedFrameImpactRecords)return false;
    TArray<uint8> AX,BY;FText Error;
    return EncodeAftershockMassReplay(A,AX,Error)&&EncodeAftershockMassReplay(B,BY,Error)&&AX==BY;
}
bool ExtractRaw(const TArray<uint8>& Bytes,TArray<uint8>& Raw)
{
    if(Bytes.Num()<36)return false;FMemoryReader R(Bytes,true);uint32 M=0,V=0;int32 N=0,Count=0;R<<M<<V<<N<<Count;
    if(R.IsError()||M!=0x41534D46||V!=1||N<1||N>32*1024*1024||Count!=Bytes.Num()-36)return false;
    Raw.SetNumUninitialized(N);return FCompression::UncompressMemory(NAME_Zlib,Raw.GetData(),N,Bytes.GetData()+36,Count);
}
bool PackRaw(const TArray<uint8>& Raw,TArray<uint8>& Bytes)
{
    int32 Count=FCompression::CompressMemoryBound(NAME_Zlib,Raw.Num());if(Count<1)return false;
    TArray<uint8> Compressed;Compressed.SetNumUninitialized(Count);
    if(!FCompression::CompressMemory(NAME_Zlib,Compressed.GetData(),Count,Raw.GetData(),Raw.Num(),COMPRESS_BiasSpeed))return false;
    Bytes.Reset();FMemoryWriter W(Bytes,true);uint32 M=0x41534D46,V=1;int32 N=Raw.Num();W<<M<<V<<N<<Count;
    uint8 Digest[20];FSHA1::HashBuffer(Raw.GetData(),Raw.Num(),Digest);W.Serialize(Digest,20);W.Serialize(Compressed.GetData(),Count);return !W.IsError();
}
template<class T> bool Put(TArray<uint8>& Bytes,int32 Offset,const T& Value)
{if(Offset<0||Offset+static_cast<int32>(sizeof(T))>Bytes.Num())return false;FMemory::Memcpy(Bytes.GetData()+Offset,&Value,sizeof(T));return true;}
bool Save(const TSharedRef<FJsonObject>& Report,const FString& Path)
{
    FString Text;const auto Writer=TJsonWriterFactory<>::Create(&Text);if(!FJsonSerializer::Serialize(Report,Writer))return false;
    if(!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true))return false;
    const FString Temp=Path+TEXT(".tmp");return FFileHelper::SaveStringToFile(Text,*Temp,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
        &&IFileManager::Get().Move(*Path,*Temp,true,false,false,true);
}
}

bool RunAftershockPhysicalReplayFixtures(const FString& AbsoluteReportPath)
{
    using namespace AftershockPhysicalReplayAuditDetail;
    auto Report=MakeShared<FJsonObject>();Report->SetBoolField(TEXT("valid"),false);Report->SetBoolField(TEXT("in_progress"),true);
    Report->SetStringField(TEXT("run_id"),FGuid::NewGuid().ToString());Report->SetStringField(TEXT("started_utc"),FDateTime::UtcNow().ToIso8601());
    Report->SetStringField(TEXT("scope"),TEXT("Independent physical replay codec roundtrip and malformed envelope/DTO fixtures from actual480-cell numerical solves. No world import, historical migration, permission, render or packaged acceptance."));
    if(!Save(Report,AbsoluteReportPath))return false;
    TArray<TSharedPtr<FJsonValue>> Rows;int32 Failed=0;
    const auto Check=[&](const TCHAR* Name,bool Pass)
    {auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("name"),Name);Row->SetBoolField(TEXT("passed"),Pass);Rows.Add(MakeShared<FJsonValueObject>(Row));
        Failed+=Pass?0:1;UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_REPLAY_FIXTURE name=%s passed=%d"),Name,Pass);};
    const auto Snow=Event(Fixture(EAftershockMassMaterial::Snow)),Soil=Event(Fixture(EAftershockMassMaterial::SoilDebris));
    auto BlockedInput=Fixture(EAftershockMassMaterial::Snow);FAftershockMassObstacle O;O.BuildingID=44;O.CenterM=FVector2D(85.,105.);O.HalfExtentM=FVector2D(25.,1.);O.HeightM=5.;O.FoundationTag=2;
    BlockedInput.Obstacles.Add(O);BlockedInput.FoundationResponse=[](const auto&,auto& Response){Response.TangentialRetention=0.;return true;};
    const auto Blocked=Event(BlockedInput);
    Check(TEXT("01_actual_snow_solver_fixture_accepted"),Snow.Result.bAccepted&&Snow.Result.bFrameExportComplete);
    Check(TEXT("02_actual_soil_solver_fixture_accepted"),Soil.Result.bAccepted&&Soil.Result.bFrameExportComplete);
    Check(TEXT("03_actual_building_contact_fixture_accepted"),Blocked.Result.bAccepted&&Blocked.Result.Impacts.Num()==1&&Blocked.Result.Impacts[0].FirstContactSeconds>0.);
    if(!Snow.Result.bAccepted||Snow.Result.Frames.IsEmpty()||Snow.Result.Frames[0].Parcels.Num()<2||Snow.Result.Frames[0].Cells.IsEmpty()
        ||Snow.Result.Cells.Num()<2||Snow.SupportCells.Num()<2)
    {
        Report->SetArrayField(TEXT("checks"),Rows);Report->SetNumberField(TEXT("expected"),50);Report->SetNumberField(TEXT("checked"),Rows.Num());
        Report->SetNumberField(TEXT("failed"),FMath::Max(1,Failed));Report->SetBoolField(TEXT("in_progress"),false);
        Report->SetStringField(TEXT("error"),TEXT("Actual solver did not produce the required valid baseline; mutation fixtures were not dereferenced."));
        Save(Report,AbsoluteReportPath);return false;
    }
    FText Error;TArray<uint8> SnowBytes,SoilBytes,BlockedBytes;FAftershockMassVisualEvent Restored;
    const bool SnowEncoded=EncodeAftershockMassReplay(Snow,SnowBytes,Error);
    const bool SoilEncoded=EncodeAftershockMassReplay(Soil,SoilBytes,Error);
    const bool BlockedEncoded=EncodeAftershockMassReplay(Blocked,BlockedBytes,Error);
    Check(TEXT("04_snow_roundtrip_all_wire_fields"),SnowEncoded&&DecodeAftershockMassReplay(SnowBytes,Restored,Error)&&Same(Snow,Restored));
    Check(TEXT("05_soil_roundtrip_all_wire_fields"),SoilEncoded&&DecodeAftershockMassReplay(SoilBytes,Restored,Error)&&Same(Soil,Restored));
    Check(TEXT("06_actual_contacts_timing_roundtrip"),BlockedEncoded&&DecodeAftershockMassReplay(BlockedBytes,Restored,Error)&&Same(Blocked,Restored));
    Check(TEXT("07_record_counters_reconstructed_not_serialized"),SnowEncoded&&DecodeAftershockMassReplay(SnowBytes,Restored,Error)
        &&Restored.Result.ExportedFrameCellRecords==Snow.Result.ExportedFrameCellRecords&&Restored.Result.ExportedFrameParcelRecords==Snow.Result.ExportedFrameParcelRecords);
    const auto RejectEvent=[&](const TCHAR* Name,const TFunction<void(FAftershockMassVisualEvent&)>& Mutate)
    {auto Bad=Snow;Mutate(Bad);TArray<uint8> Preserved={3,1,4};FText Why;Check(Name,!EncodeAftershockMassReplay(Bad,Preserved,Why)&&Preserved==TArray<uint8>({3,1,4})&&!Why.IsEmpty());};
    RejectEvent(TEXT("08_invalid_event_id"),[](auto& V){V.EventID=FGuid();});
    RejectEvent(TEXT("09_nonfinite_event_clock"),[](auto& V){V.StartedAtWorldSeconds=std::numeric_limits<double>::quiet_NaN();});
    RejectEvent(TEXT("10_unknown_material"),[](auto& V){V.Result.Material=static_cast<EAftershockMassMaterial>(255);});
    RejectEvent(TEXT("11_material_hazard_mismatch"),[](auto& V){V.Result.HazardTypeTag=8;});
    RejectEvent(TEXT("12_duplicate_source_coords"),[](auto& V){V.SupportCells[1].Coord=V.SupportCells[0].Coord;});
    RejectEvent(TEXT("13_duplicate_result_coords"),[](auto& V){V.Result.Cells[1].Coord=V.Result.Cells[0].Coord;});
    RejectEvent(TEXT("14_source_inventory_forgery"),[](auto& V){V.SupportCells[0].SnowWaterEquivalentMm+=1.;});
    RejectEvent(TEXT("15_result_negative_mass"),[](auto& V){V.Result.Cells[0].DepositedMassKg=-1.;});
    RejectEvent(TEXT("16_nonfinite_budget"),[](auto& V){V.Result.Budget.DissipatedJ=std::numeric_limits<double>::infinity();});
    RejectEvent(TEXT("17_energy_ledger_forgery"),[](auto& V){V.Result.Budget.DissipatedJ+=1000.;});
    RejectEvent(TEXT("18_mass_ledger_forgery"),[](auto& V){V.Result.Budget.ExportedKg+=1000.;});
    RejectEvent(TEXT("19_duplicate_parcel_id"),[](auto& V){if(V.Result.Frames[0].Parcels.Num()>1)V.Result.Frames[0].Parcels[1].ParcelID=V.Result.Frames[0].Parcels[0].ParcelID;});
    RejectEvent(TEXT("20_parcel_outside_support"),[](auto& V){V.Result.Frames[0].Parcels[0].PositionM.X+=1000000.;});
    RejectEvent(TEXT("21_parcel_off_actual_bed"),[](auto& V){V.Result.Frames[0].Parcels[0].PositionM.Z+=1.;});
    RejectEvent(TEXT("22_duplicate_frame_cells"),[](auto& V){const auto Copy=V.Result.Frames[0].Cells[0];V.Result.Frames[0].Cells.Add(Copy);++V.Result.ExportedFrameCellRecords;});
    RejectEvent(TEXT("23_forged_export_record_counter"),[](auto& V){++V.Result.ExportedFrameCellRecords;});
    RejectEvent(TEXT("24_future_frame_clock"),[](auto& V){V.Result.Frames.Last().Seconds=V.Result.SimulatedSeconds+1.;});
    RejectEvent(TEXT("25_incomplete_result"),[](auto& V){V.Result.bComplete=false;});
    RejectEvent(TEXT("26_unknown_solver_model_flags"),[](auto& V){V.Result.bLateralGranularPressureModeled=true;});
    RejectEvent(TEXT("27_deposit_volume_forgery"),[](auto& V){for(auto& C:V.Result.Frames.Last().Cells)if(C.DepositedMassKg>0.){C.DepositedVolumeM3+=1.;break;}});
    RejectEvent(TEXT("28_source_coordinate_bound"),[](auto& V){V.SupportCells[0].Coord=FIntPoint(MAX_int32,0);});
    RejectEvent(TEXT("29_invalid_work_counter"),[](auto& V){V.Result.Budget.WorkUnits=MAX_int64;});
    const auto RejectBlocked=[&](const TCHAR* Name,const TFunction<void(FAftershockMassVisualEvent&)>& Mutate)
    {auto Bad=Blocked;Mutate(Bad);TArray<uint8> Out;FText Why;Check(Name,BlockedEncoded&&!EncodeAftershockMassReplay(Bad,Out,Why));};
    RejectBlocked(TEXT("30_future_contact_time"),[](auto& V){if(!V.Result.Impacts.IsEmpty())V.Result.Impacts[0].FirstContactSeconds=V.Result.SimulatedSeconds+1.;});
    RejectBlocked(TEXT("31_unknown_building_contact"),[](auto& V){if(!V.Result.Impacts.IsEmpty())V.Result.Impacts[0].BuildingID=999;});
    RejectBlocked(TEXT("32_duplicate_final_contact"),[](auto& V){if(!V.Result.Impacts.IsEmpty()){const auto Copy=V.Result.Impacts[0];V.Result.Impacts.Add(Copy);}});
    const auto RejectBytes=[&](const TCHAR* Name,const TArray<uint8>& Bad)
    {FAftershockMassVisualEvent Kept;Kept.EventID=FGuid(7,7,7,7);Kept.StartedAtWorldSeconds=123.;FText Why;
        const bool Pass=!DecodeAftershockMassReplay(Bad,Kept,Why)&&Kept.EventID==FGuid(7,7,7,7)&&Kept.StartedAtWorldSeconds==123.&&!Why.IsEmpty();Check(Name,Pass);};
    auto Bytes=SnowBytes;Put<uint32>(Bytes,0,0);RejectBytes(TEXT("33_bad_magic"),Bytes);
    Bytes=SnowBytes;Put<uint32>(Bytes,4,2);RejectBytes(TEXT("34_unknown_version"),Bytes);
    Bytes=SnowBytes;Put<int32>(Bytes,8,MAX_int32);RejectBytes(TEXT("35_raw_allocation_cap"),Bytes);
    Bytes=SnowBytes;Put<int32>(Bytes,12,-1);RejectBytes(TEXT("36_negative_compressed_size"),Bytes);
    Bytes=SnowBytes;if(Bytes.Num()>36)Bytes[16]^=1;RejectBytes(TEXT("37_raw_digest_corruption"),Bytes);
    Bytes=SnowBytes;if(Bytes.Num()>40)Bytes[Bytes.Num()-2]^=127;RejectBytes(TEXT("38_compressed_stream_corruption"),Bytes);
    Bytes=SnowBytes;if(!Bytes.IsEmpty())Bytes.SetNum(Bytes.Num()-1);RejectBytes(TEXT("39_truncated_envelope"),Bytes);
    Bytes=SnowBytes;Bytes.Add(0);RejectBytes(TEXT("40_trailing_envelope_byte"),Bytes);
    Bytes.Init(0,8*1024*1024+1);RejectBytes(TEXT("41_stored_allocation_cap"),Bytes);
    TArray<uint8> Raw;const bool Extracted=SnowEncoded&&ExtractRaw(SnowBytes,Raw);
    const auto RejectRaw=[&](const TCHAR* Name,const TFunction<void(TArray<uint8>&)>& Mutate)
    {TArray<uint8> BadRaw=Raw,Packed;Mutate(BadRaw);const bool PackedOK=Extracted&&PackRaw(BadRaw,Packed);
        FAftershockMassVisualEvent Out;FText Why;Check(Name,PackedOK&&!DecodeAftershockMassReplay(Packed,Out,Why));};
    RejectRaw(TEXT("42_oversized_body_count_with_valid_digest"),[](auto& V){Put<int32>(V,72,MAX_int32);});
    RejectRaw(TEXT("43_negative_body_count_with_valid_digest"),[](auto& V){Put<int32>(V,72,-1);});
    RejectRaw(TEXT("44_noncanonical_bool_with_valid_digest"),[](auto& V){Put<uint32>(V,92,2);});
    RejectRaw(TEXT("45_nonfinite_ground_with_valid_digest"),[](auto& V){Put<double>(V,84,std::numeric_limits<double>::quiet_NaN());});
    RejectRaw(TEXT("46_trailing_raw_byte_with_valid_digest"),[](auto& V){V.Add(0);});
    TArray<uint8> Empty;RejectBytes(TEXT("47_empty_envelope"),Empty);
    auto Resumed=Snow;Resumed.StartedAtWorldSeconds=-20.;TArray<uint8> ResumeBytes;
    Check(TEXT("48_resumed_world_clock_can_be_negative"),EncodeAftershockMassReplay(Resumed,ResumeBytes,Error)&&DecodeAftershockMassReplay(ResumeBytes,Restored,Error)&&Same(Resumed,Restored));
    Check(TEXT("49_validator_uses_same_deep_decode"),SnowEncoded&&ValidateAftershockMassReplayBytes(SnowBytes,Error)&&!ValidateAftershockMassReplayBytes(Empty,Error));
    Bytes=SnowBytes;Bytes.Add(0);Put<int32>(Bytes,12,Bytes.Num()-36);RejectBytes(TEXT("50_trailing_compressed_byte_with_matching_count"),Bytes);
    const bool Valid=Rows.Num()==50&&Failed==0;
    Report->SetArrayField(TEXT("checks"),Rows);Report->SetNumberField(TEXT("expected"),50);Report->SetNumberField(TEXT("checked"),Rows.Num());
    Report->SetNumberField(TEXT("failed"),Failed);Report->SetNumberField(TEXT("passed"),Rows.Num()-Failed);Report->SetNumberField(TEXT("snow_stored_bytes"),SnowBytes.Num());
    Report->SetNumberField(TEXT("soil_stored_bytes"),SoilBytes.Num());Report->SetNumberField(TEXT("contact_stored_bytes"),BlockedBytes.Num());
    Report->SetBoolField(TEXT("valid"),Valid);Report->SetBoolField(TEXT("in_progress"),false);Report->SetStringField(TEXT("finished_utc"),FDateTime::UtcNow().ToIso8601());
    const bool Saved=Save(Report,AbsoluteReportPath);UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_PHYSICAL_REPLAY checked=%d failed=%d saved=%d"),Rows.Num(),Failed,Saved);
    return Valid&&Saved;
}
