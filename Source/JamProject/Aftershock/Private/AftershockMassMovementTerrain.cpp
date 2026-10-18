#include "AftershockMassMovementTerrain.h"
#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/AftershockSaveTypes.h"
#include "AftershockHydraulicTerrain.h"
#include "Engine/StaticMesh.h"
#include "Serialization/MemoryWriter.h"
#include "Misc/SecureHash.h"
#include "Dom/JsonObject.h"

namespace
{
static inline bool MassTerrainFinite(double V){return FMath::IsFinite(V);}
bool InRange(double V,double Lo,double Hi){return MassTerrainFinite(V)&&V>=Lo&&V<=Hi;}
bool CoordLess(FIntPoint A,FIntPoint B){return A.Y==B.Y?A.X<B.X:A.Y<B.Y;}
bool WeatherValid(const FAftershockMaterialWeatherHistory& H,FString& Why)
{
    if(H.ScenarioVersion!=1||H.SourceCode.IsNone()||H.Segments.Num()>32||!InRange(H.ReferenceElevationM,-500.,10000.)
        ||!InRange(H.LapseRateCPerKm,0.,10.)||!InRange(H.AllSnowC,-10.,5.)||!InRange(H.AllRainC,H.AllSnowC+.01,10.)
        ||!InRange(H.MeltBaseC,-2.,3.)||!InRange(H.MeltMmPerDegreeDay,0.,10.)
        ||!InRange(H.SnowBulkDensityKgM3,50.,900.)||!InRange(H.SnowWeakLayerStrengthPa,0.,1.e7))
    {Why=TEXT("Invalid explicit authored weather/profile");return false;}
    int32 Days=0;for(const auto& S:H.Segments)
    {
        if(S.Days<1||S.Days>60||!InRange(S.AirTemperatureC,-90.,60.)||!InRange(S.PrecipitationMmPerDay,0.,500.)
            ||!InRange(S.SuppliedSnowWaterMmPerDay,-1.,S.PrecipitationMmPerDay)||(S.SuppliedSnowWaterMmPerDay<0.&&S.SuppliedSnowWaterMmPerDay!=-1.))
        {Why=TEXT("Invalid authored weather segment");return false;}Days+=S.Days;
    }
    if(Days>60){Why=TEXT("Authored antecedent history exceeds60day budget");return false;}return true;
}
bool MaterialValid(const FGridCell& C,FString& Why)
{
    if(!InRange(C.SnowWaterEquivalentMm,0.,10000.)||!InRange(C.SnowBulkDensityKgM3,50.,900.)||!InRange(C.SnowWeakLayerStrengthPa,0.,1.e7)
        ||!InRange(C.RegolithDepthM,0.,30.)||!InRange(C.SoilPorosity01,.01,.8)||!InRange(C.SoilDryBulkDensityKgM3,500.,2600.)
        ||!InRange(C.SoilPoreWaterDepthM,0.,C.RegolithDepthM*C.SoilPorosity01+1.e-6)||!InRange(C.SoilCohesionPa,0.,1.e7)
        ||!InRange(C.SoilFrictionAngleDegrees,0.,79.9)||!InRange(C.GroundErodedDepthM,0.,1000.)||!InRange(C.GroundDepositedDepthM,0.,1000.)
        ||(C.MaterialScenarioVersion<0||C.MaterialScenarioVersion>1)||(C.MaterialScenarioVersion==0&&(C.SnowWaterEquivalentMm!=0.f||C.RegolithDepthM!=0.f
            ||C.SoilPoreWaterDepthM!=0.f||C.GroundErodedDepthM!=0.f||C.GroundDepositedDepthM!=0.f)))
    {Why=TEXT("Invalid finite canonical snow/soil/pore material inventory");return false;}return true;
}
bool NaturalSolid(EBuildingType T)
{
    // Non-solid land uses/flat pads are not fabricated full-height walls.
    return T!=EBuildingType::GreenSpace;
}
FString BuildingDigest(TArray<uint8>& Bytes)
{
    uint8 Digest[FSHA1::DigestSize];FSHA1::HashBuffer(Bytes.GetData(),Bytes.Num(),Digest);return BytesToHex(Digest,FSHA1::DigestSize);
}
bool CaptureObstacles(const UTerrainGridSubsystem& Terrain,const UBuildingManagerSubsystem& Owner,FAftershockMassTerrainSnapshot& Out)
{
    TArray<FBuildingInstance> Instances;Owner.GetAllBuildings(Instances);
    Instances.Sort([](const auto& A,const auto& B){return A.InstanceID<B.InstanceID;});
    FAftershockBuildingsSaveState Saved;Owner.CaptureSaveState(Saved);TMap<int32,const FAftershockBuildingSaveRecord*> ByID;
    for(const auto& R:Saved.Buildings)ByID.Add(R.State.InstanceID,&R);
    TArray<uint8> Fingerprint;FMemoryWriter Writer(Fingerprint);int32 Previous=INDEX_NONE;
    TMap<TWeakObjectPtr<UBuildingData>,UStaticMesh*> Loaded;
    for(const auto& B:Instances)
    {
        if(B.InstanceID<=Previous||!B.BuildingData){Out.Failure=TEXT("Invalid or duplicate canonical building records");return false;}Previous=B.InstanceID;
        int32 ID=B.InstanceID;auto Coords=B.GridCoords;uint8 State=static_cast<uint8>(B.DamageState),Material=static_cast<uint8>(B.BuildingData->Material),Foundation=static_cast<uint8>(B.Foundation);
        Writer<<ID<<Coords<<State<<Material<<Foundation;
        if(B.DamageState>=EDamageState::Collapsed||!NaturalSolid(B.BuildingData->Type))continue;
        const auto* Found=ByID.Find(ID);if(!Found){Out.Failure=TEXT("Canonical building transform absent; no cull-dependent OBB omission");return false;}
        const auto& R=**Found;FTransform Transform=R.WorldTransform;Writer<<Transform;
        if(Transform.ContainsNaN()||!Transform.GetRotation().IsNormalized()||Transform.GetScale3D().GetMin()<=KINDA_SMALL_NUMBER
            ||FVector::DotProduct(Transform.GetRotation().GetUpVector(),FVector::UpVector)<.99999)
        {Out.Failure=TEXT("Canonical building has zero/tilted/nonfinite transform; vertical OBB approximation unsupported");return false;}
        const TWeakObjectPtr<UBuildingData> Key(B.BuildingData.Get());
        UStaticMesh* Mesh=nullptr;if(auto** Existing=Loaded.Find(Key))Mesh=*Existing;
        else {Mesh=B.BuildingData->MeshIntact.LoadSynchronous();Loaded.Add(Key,Mesh);}
        if(!Mesh){Out.Failure=TEXT("Missing canonical source bounds; no guessed building collider");return false;}
        FBox Bounds=Mesh->GetBoundingBox();if(!Bounds.IsValid){Out.Failure=TEXT("Invalid canonical mesh bounds");return false;}
        const double Base=Bounds.Min.Z<-.5&&Bounds.Max.Z>.5?0.:Bounds.Min.Z;
        const FVector Size=Bounds.GetSize()*Transform.GetScale3D();
        const FVector Center=Transform.TransformPosition(FVector(Bounds.GetCenter().X,Bounds.GetCenter().Y,Base));
        const FVector Parcel=Terrain.GridToWorld(B.GridCoords);
        if(Size.ContainsNaN()||Size.GetMin()<=0.||FVector2D::Distance(FVector2D(Center),FVector2D(Parcel))>Terrain.CellSize*2.)
        {Out.Failure=TEXT("Saved canonical envelope no longer matches its parcel");return false;}
        FAftershockMassObstacle O;O.BuildingID=ID;O.CenterM=FVector2D(Center)*.01;O.HalfExtentM=FVector2D(Size.X,Size.Y)*.005;
        O.HeightM=(Bounds.Max.Z-Base)*Transform.GetScale3D().Z*.01;O.YawRadians=FMath::DegreesToRadians(Transform.Rotator().Yaw);
        O.StructuralMaterialTag=Material;O.FoundationTag=Foundation;Out.Input.Obstacles.Add(O);Writer<<Bounds;
    }
    Out.BuildingFingerprint=BuildingDigest(Fingerprint);Out.IncludedSolidBuildings=Out.Input.Obstacles.Num();return true;
}
}

bool AftershockMassTerrain::MakeAuthoredWinterHistory(const FAftershockMaterialClimateContext& C,
    const FAftershockAuthoredWinterOptions& O,FAftershockMaterialWeatherHistory& Out,FString& Why)
{
    Out={};Why.Reset();Out.SourceCode=TEXT("ExplicitNoSnowHistoryV1");
    if(!O.bExplicitlyRequestAuthoredWinter)return true;
    if(!C.bVerifiedContext||C.PrimarySourceURL.IsEmpty()||!InRange(C.ColdestMonthlyMeanTemperatureC,-90.,50.)
        ||!InRange(C.ReferenceElevationM,-500.,10000.)||!InRange(O.ColdSpellAnomalyC,-15.,0.)||!InRange(O.DailyPrecipitationMm,0.,100.)||O.Days<1||O.Days>30)
    {Why=TEXT("An authored winter history needs verified climate context and explicit bounded storm assumptions");return false;}
    Out.SourceCode=TEXT("AuthoredWinterColdSpellV1");Out.ContextSourceURL=C.PrimarySourceURL;Out.ReferenceElevationM=C.ReferenceElevationM;
    Out.SnowWeakLayerStrengthPa=O.SnowWeakLayerStrengthPa;
    Out.Assumptions=FString::Printf(TEXT("AUTHORED daily history, not NASA observations. Coarse coldest monthly mean%.3fC plus authored anomaly%.3fC; %.3fmm/day precipitation for%ddays; lapse%.3fC/km; temperature partition and degree-day melt. No snow from biome/colour."),
        C.ColdestMonthlyMeanTemperatureC,O.ColdSpellAnomalyC,O.DailyPrecipitationMm,O.Days,Out.LapseRateCPerKm);
    FAftershockMaterialWeatherSegment S;S.Days=O.Days;S.AirTemperatureC=C.ColdestMonthlyMeanTemperatureC+O.ColdSpellAnomalyC;S.PrecipitationMmPerDay=O.DailyPrecipitationMm;Out.Segments.Add(S);
    return WeatherValid(Out,Why);
}

bool AftershockMassTerrain::ComputeSnowInventory(const FAftershockMaterialWeatherHistory& H,double Ground,
    double& Snow,double& Liquid,FString& Why)
{
    Snow=0.;Liquid=0.;Why.Reset();if(!WeatherValid(H,Why)||!MassTerrainFinite(Ground))return false;
    for(const auto& S:H.Segments)
    {
        const double Temperature=S.AirTemperatureC-H.LapseRateCPerKm*(Ground-H.ReferenceElevationM)*.001;
        const double SnowFraction=FMath::Clamp((H.AllRainC-Temperature)/(H.AllRainC-H.AllSnowC),0.,1.);
        const double Supply=S.SuppliedSnowWaterMmPerDay>=0.?S.SuppliedSnowWaterMmPerDay:S.PrecipitationMmPerDay*SnowFraction;
        // Explicit snowfall at a warm site is liquid; it cannot manufacture a
        // persistent tropical snowpack by bypassing the temperature partition.
        const double Accumulation=(Temperature>=H.AllRainC?0.:Supply)*S.Days;
        const double Total=S.PrecipitationMmPerDay*S.Days;
        const double Melt=FMath::Min(Snow+Accumulation,FMath::Max(0.,Temperature-H.MeltBaseC)*H.MeltMmPerDegreeDay*S.Days);
        Snow+=Accumulation-Melt;Liquid+=Total-Accumulation+Melt;
        if(!InRange(Snow,0.,10000.)||!MassTerrainFinite(Liquid)){Why=TEXT("Authored snow balance exceeds finite inventory bounds");return false;}
    }
    return true;
}

bool AftershockMassTerrain::EffectiveGroundElevationM(const UTerrainGridSubsystem& T,const FGridCell& C,double& Out,FString& Why)
{
    if(!MaterialValid(C,Why)||!MassTerrainFinite(C.Elevation)||!MassTerrainFinite(C.HydraulicExcavationCm))return false;
    const double Base=C.bHasRealElevationSample?T.GridOrigin.Z*.01+C.RealElevationMetres:C.Elevation*.01;
    if(!MassTerrainFinite(Base)||(C.bHasRealElevationSample&&FMath::Abs(C.Elevation-Base*100.)>.1))
    {Why=TEXT("Immutable terrain base/DEM contract differs; refusing double application of erosion/deposition");return false;}
    double BedCm=0.;FText Error;
    if(!TryGetAftershockHydraulicBedCm(C,true,BedCm,Error)){Why=Error.ToString();return false;}
    Out=BedCm*.01;return MassTerrainFinite(Out);
}

FAftershockMaterialInitializationReceipt AftershockMassTerrain::InitializeAuthoredMaterialScenario(UTerrainGridSubsystem& T,
    const FAftershockMaterialWeatherHistory& H,const TArray<FAftershockAuthoredRegolithPatch>& Patches,bool NewGame)
{
    FAftershockMaterialInitializationReceipt R;R.WeatherHistory=H;R.RegolithPatches=Patches;R.BeforeTerrainRevision=T.GetTerrainRevision();
    if(!IsInGameThread()||!NewGame||T.ContinuationGeneratorVersion!=3||T.Cells.IsEmpty()||T.Cells.Num()>65536||Patches.Num()>32||!WeatherValid(H,R.Failure))
    {if(R.Failure.IsEmpty())R.Failure=TEXT("Material initialization requires explicit bounded new-game transaction");return R;}
    TMap<FIntPoint,const FAftershockAuthoredRegolithPatch*> Layers;
    for(const auto& P:Patches)
    {
        if(P.SourceCode.IsNone()||P.Cells.Num()>4096||!InRange(P.DepthM,0.,3.)||!InRange(P.Porosity01,.05,.7)
            ||!InRange(P.DryBulkDensityKgM3,500.,2400.)||!InRange(P.CohesionPa,0.,100000.)||!InRange(P.FrictionAngleDegrees,0.,70.)
            ||!InRange(P.InitialPoreWaterDepthM,0.,P.DepthM*P.Porosity01))
        {R.Failure=TEXT("Invalid explicit finite mantle/pore-water patch");return R;}
        for(FIntPoint C:P.Cells)
        {
            FGridCell Existing;if(Layers.Contains(C)||!T.TryGetCell(C,Existing)||Existing.bIsOcean||Existing.bIsRiverbed
                ||(Existing.SoilType==EAftershockSoilType::Bedrock&&!P.bExplicitWeatheredMantleOverScenarioBedrock))
            {R.Failure=TEXT("Mantle patch overlaps, lies outside supported dry land, or implicitly invents soil over bedrock");return R;}Layers.Add(C,&P);
        }
    }
    TArray<FGridCell> Staged;Staged.Reserve(T.Cells.Num());const double Area=FMath::Square(T.CellSize*.01);
    if(!InRange(T.CellSize,.5,100000.)){R.Failure=TEXT("Invalid canonical cell scale");return R;}
    for(const auto& Original:T.Cells)
    {
        if(T.CoordToIndex(Original.Coords)!=Staged.Num())
        {R.Failure=TEXT("Canonical storage key/index mismatch before transaction");return R;}
        if(Original.MaterialScenarioVersion!=0||Original.SnowWaterEquivalentMm!=0.f||Original.RegolithDepthM!=0.f
            ||Original.SoilPoreWaterDepthM!=0.f||Original.GroundErodedDepthM!=0.f||Original.GroundDepositedDepthM!=0.f)
        {R.Failure=TEXT("Existing material state cannot be re-seeded; V4 restore and active game are not new-game initialization");return R;}
        FGridCell C=Original;C.MaterialScenarioVersion=H.ScenarioVersion;double Ground;
        if(!EffectiveGroundElevationM(T,Original,Ground,R.Failure))return R;
        if(!C.bIsOcean&&!C.bIsRiverbed)
        {
            double Snow,Liquid;if(!ComputeSnowInventory(H,Ground,Snow,Liquid,R.Failure))return R;
            C.SnowWaterEquivalentMm=static_cast<float>(Snow);C.SnowBulkDensityKgM3=static_cast<float>(H.SnowBulkDensityKgM3);
            C.SnowWeakLayerStrengthPa=static_cast<float>(H.SnowWeakLayerStrengthPa);R.SnowWaterKg+=static_cast<double>(C.SnowWaterEquivalentMm)*Area;R.AntecedentLiquidOutflowM3+=Liquid*Area*.001;
            if(Snow>0.)++R.SnowCells;
        }
        if(const auto* Found=Layers.Find(C.Coords))
        {
            const auto& P=**Found;C.RegolithDepthM=static_cast<float>(P.DepthM);C.SoilPorosity01=static_cast<float>(P.Porosity01);
            C.SoilDryBulkDensityKgM3=static_cast<float>(P.DryBulkDensityKgM3);C.SoilPoreWaterDepthM=static_cast<float>(P.InitialPoreWaterDepthM);
            C.SoilCohesionPa=static_cast<float>(P.CohesionPa);C.SoilFrictionAngleDegrees=static_cast<float>(P.FrictionAngleDegrees);
            R.DrySoilKg+=static_cast<double>(C.RegolithDepthM)*Area*C.SoilDryBulkDensityKgM3;R.PoreWaterM3+=static_cast<double>(C.SoilPoreWaterDepthM)*Area;if(P.DepthM>0.)++R.RegolithCells;
        }
        if(!MaterialValid(C,R.Failure))return R;Staged.Add(C);
    }
    // No yield/callback or source generation occurs between preflight and apply.
    // SetCell only increments the owner's revision; it broadcasts no callbacks.
    if(T.GetTerrainRevision()!=R.BeforeTerrainRevision||T.Cells.Num()!=Staged.Num())
    {R.Failure=TEXT("Terrain changed before material transaction");return R;}
    for(int32 I=0;I<Staged.Num();++I)if(!T.SetCell(Staged[I].Coords,Staged[I]))
    {R.Failure=TEXT("Prevalidated material transaction unexpectedly rejected; caller must discard unstarted new-game world");return R;}
    R.CellsVisited=Staged.Num();R.AfterTerrainRevision=T.GetTerrainRevision();R.bApplied=true;return R;
}

namespace AftershockMaterialContinuationPrivate
{
bool MantleValid(const FAftershockAuthoredRegolithPatch& P,FString& Why)
{
    if(P.SourceCode.IsNone()||P.Cells.Num()>4096||!InRange(P.DepthM,0.,3.)||!InRange(P.Porosity01,.05,.7)
        ||!InRange(P.DryBulkDensityKgM3,500.,2400.)||!InRange(P.CohesionPa,0.,100000.)
        ||!InRange(P.FrictionAngleDegrees,0.,70.)||!InRange(P.InitialPoreWaterDepthM,0.,P.DepthM*P.Porosity01))
    {Why=TEXT("Invalid explicit continuation mantle profile");return false;}
    return true;
}
bool ProfileValid(const FAftershockMaterialContinuationProfile& P,FString& Why)
{
    if(P.ProfileVersion!=1||!P.Mantle.Cells.IsEmpty()||!WeatherValid(P.Weather,Why)||!MantleValid(P.Mantle,Why)
        ||!InRange(P.MinimumForestFuel01,0.,1.)||!InRange(P.MinimumSlopeDegrees,0.,90.)
        ||!InRange(P.MaximumSlopeDegrees,P.MinimumSlopeDegrees,90.)
        ||P.Weather.SourceCode.ToString().Len()>256||P.Weather.ContextSourceURL.Len()>4096||P.Weather.Assumptions.Len()>16384)
    {if(Why.IsEmpty())Why=TEXT("Incomplete or out-of-bounds saved material continuation contract");return false;}
    return true;
}
bool Number(const TSharedPtr<FJsonObject>& J,const TCHAR* Key,double& Out)
{return J.IsValid()&&J->TryGetNumberField(Key,Out)&&MassTerrainFinite(Out);}
}

bool AftershockMassTerrain::EncodeMaterialContinuationProfile(const FAftershockMaterialContinuationProfile& P,
    TSharedPtr<FJsonObject>& Out,FString& Why)
{
    using namespace AftershockMaterialContinuationPrivate;
    Out.Reset();Why.Reset();if(!ProfileValid(P,Why))return false;
    auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("profile_version"),P.ProfileVersion);
    const auto& H=P.Weather;
    J->SetNumberField(TEXT("scenario_version"),H.ScenarioVersion);J->SetStringField(TEXT("weather_source"),H.SourceCode.ToString());
    J->SetStringField(TEXT("context_url"),H.ContextSourceURL);J->SetStringField(TEXT("weather_assumptions"),H.Assumptions);
    J->SetNumberField(TEXT("reference_elevation_m"),H.ReferenceElevationM);J->SetNumberField(TEXT("lapse_c_per_km"),H.LapseRateCPerKm);
    J->SetNumberField(TEXT("all_snow_c"),H.AllSnowC);J->SetNumberField(TEXT("all_rain_c"),H.AllRainC);
    J->SetNumberField(TEXT("melt_base_c"),H.MeltBaseC);J->SetNumberField(TEXT("melt_mm_per_degree_day"),H.MeltMmPerDegreeDay);
    J->SetNumberField(TEXT("snow_density_kg_m3"),H.SnowBulkDensityKgM3);J->SetNumberField(TEXT("weak_layer_pa"),H.SnowWeakLayerStrengthPa);
    TArray<TSharedPtr<FJsonValue>> Segments;for(const auto& S:H.Segments)
    {
        auto Segment=MakeShared<FJsonObject>();Segment->SetNumberField(TEXT("days"),S.Days);
        Segment->SetNumberField(TEXT("air_c"),S.AirTemperatureC);Segment->SetNumberField(TEXT("precip_mm_day"),S.PrecipitationMmPerDay);
        Segment->SetNumberField(TEXT("supplied_snow_mm_day"),S.SuppliedSnowWaterMmPerDay);Segments.Add(MakeShared<FJsonValueObject>(Segment));
    }
    J->SetArrayField(TEXT("segments"),Segments);
    const auto& M=P.Mantle;auto Mantle=MakeShared<FJsonObject>();Mantle->SetStringField(TEXT("source"),M.SourceCode.ToString());
    Mantle->SetNumberField(TEXT("depth_m"),M.DepthM);Mantle->SetNumberField(TEXT("porosity_01"),M.Porosity01);
    Mantle->SetNumberField(TEXT("dry_density_kg_m3"),M.DryBulkDensityKgM3);Mantle->SetNumberField(TEXT("cohesion_pa"),M.CohesionPa);
    Mantle->SetNumberField(TEXT("friction_deg"),M.FrictionAngleDegrees);Mantle->SetNumberField(TEXT("initial_pore_depth_m"),M.InitialPoreWaterDepthM);
    Mantle->SetBoolField(TEXT("explicit_weathered_mantle_over_bedrock"),M.bExplicitWeatheredMantleOverScenarioBedrock);
    Mantle->SetNumberField(TEXT("minimum_forest_fuel_01"),P.MinimumForestFuel01);
    Mantle->SetNumberField(TEXT("minimum_slope_deg"),P.MinimumSlopeDegrees);Mantle->SetNumberField(TEXT("maximum_slope_deg"),P.MaximumSlopeDegrees);
    J->SetObjectField(TEXT("mantle"),Mantle);Out=J;return true;
}

bool AftershockMassTerrain::DecodeMaterialContinuationProfile(const TSharedPtr<FJsonObject>& J,
    FAftershockMaterialContinuationProfile& Out,FString& Why)
{
    using namespace AftershockMaterialContinuationPrivate;
    Why.Reset();FAftershockMaterialContinuationProfile Candidate;
    auto& H=Candidate.Weather;auto& M=Candidate.Mantle;
    FString WeatherSource,MantleSource;double ProfileVersion=0.,ScenarioVersion=0.;
    const TArray<TSharedPtr<FJsonValue>>* Segments=nullptr;const TSharedPtr<FJsonObject>* Mantle=nullptr;
    if(!Number(J,TEXT("profile_version"),ProfileVersion)||ProfileVersion!=1.
        ||!Number(J,TEXT("scenario_version"),ScenarioVersion)||ScenarioVersion!=1.
        ||!J->TryGetStringField(TEXT("weather_source"),WeatherSource)||WeatherSource.IsEmpty()||WeatherSource.Len()>256
        ||!J->TryGetStringField(TEXT("context_url"),H.ContextSourceURL)||!J->TryGetStringField(TEXT("weather_assumptions"),H.Assumptions)
        ||!Number(J,TEXT("reference_elevation_m"),H.ReferenceElevationM)||!Number(J,TEXT("lapse_c_per_km"),H.LapseRateCPerKm)
        ||!Number(J,TEXT("all_snow_c"),H.AllSnowC)||!Number(J,TEXT("all_rain_c"),H.AllRainC)
        ||!Number(J,TEXT("melt_base_c"),H.MeltBaseC)||!Number(J,TEXT("melt_mm_per_degree_day"),H.MeltMmPerDegreeDay)
        ||!Number(J,TEXT("snow_density_kg_m3"),H.SnowBulkDensityKgM3)||!Number(J,TEXT("weak_layer_pa"),H.SnowWeakLayerStrengthPa)
        ||!J->TryGetArrayField(TEXT("segments"),Segments)||!Segments||Segments->Num()>32
        ||!J->TryGetObjectField(TEXT("mantle"),Mantle)||!Mantle||!Mantle->IsValid())
    {Why=TEXT("Saved material receipt omits complete weather/melt/mantle continuation controls; no history guessed");return false;}
    H.SourceCode=FName(*WeatherSource);H.Segments.Reset();
    for(const auto& Value:*Segments)
    {
        if(!Value.IsValid()||Value->Type!=EJson::Object){Why=TEXT("Malformed saved weather segment");return false;}
        const auto Segment=Value->AsObject();FAftershockMaterialWeatherSegment S;double Days=0.;
        if(!Number(Segment,TEXT("days"),Days)||Days<1.||Days>60.||Days!=FMath::FloorToDouble(Days)
            ||!Number(Segment,TEXT("air_c"),S.AirTemperatureC)||!Number(Segment,TEXT("precip_mm_day"),S.PrecipitationMmPerDay)
            ||!Number(Segment,TEXT("supplied_snow_mm_day"),S.SuppliedSnowWaterMmPerDay))
        {Why=TEXT("Saved weather segment lacks exact bounded controls");return false;}
        S.Days=static_cast<int32>(Days);H.Segments.Add(S);
    }
    const auto MJ=*Mantle;
    if(!MJ->TryGetStringField(TEXT("source"),MantleSource)||MantleSource.IsEmpty()||MantleSource.Len()>256
        ||!Number(MJ,TEXT("depth_m"),M.DepthM)||!Number(MJ,TEXT("porosity_01"),M.Porosity01)
        ||!Number(MJ,TEXT("dry_density_kg_m3"),M.DryBulkDensityKgM3)||!Number(MJ,TEXT("cohesion_pa"),M.CohesionPa)
        ||!Number(MJ,TEXT("friction_deg"),M.FrictionAngleDegrees)||!Number(MJ,TEXT("initial_pore_depth_m"),M.InitialPoreWaterDepthM)
        ||!MJ->TryGetBoolField(TEXT("explicit_weathered_mantle_over_bedrock"),M.bExplicitWeatheredMantleOverScenarioBedrock)
        ||!Number(MJ,TEXT("minimum_forest_fuel_01"),Candidate.MinimumForestFuel01)
        ||!Number(MJ,TEXT("minimum_slope_deg"),Candidate.MinimumSlopeDegrees)||!Number(MJ,TEXT("maximum_slope_deg"),Candidate.MaximumSlopeDegrees))
    {Why=TEXT("Saved mantle eligibility/stock assumptions are incomplete; no continuation defaults guessed");return false;}
    M.SourceCode=FName(*MantleSource);if(!ProfileValid(Candidate,Why))return false;Out=MoveTemp(Candidate);return true;
}

bool AftershockMassTerrain::BuildAuthoredMantlePatches(const UTerrainGridSubsystem& T,
    const FAftershockMaterialContinuationProfile& P,const TArray<FIntPoint>& Targets,
    TArray<FAftershockAuthoredRegolithPatch>& Out,FString& Why)
{
    using namespace AftershockMaterialContinuationPrivate;
    Out.Reset();Why.Reset();if(!ProfileValid(P,Why)||Targets.Num()>65536||!InRange(T.CellSize,.5,100000.))return false;
    TSet<FIntPoint> Seen;Seen.Reserve(Targets.Num());
    for(FIntPoint Coord:Targets)
    {
        FGridCell C;if(Seen.Contains(Coord)||!T.TryGetCell(Coord,C)){Why=TEXT("Mantle target is duplicated or nonresident");return false;}Seen.Add(Coord);
        if(C.bIsOcean||C.bIsRiverbed||C.bVolcanicVent||C.ForestFuel01<P.MinimumForestFuel01)continue;
        double DX=0.,DY=0.;FGridCell A,B;
        if(T.TryGetCell(Coord+FIntPoint(1,0),A)&&T.TryGetCell(Coord-FIntPoint(1,0),B))DX=(A.Elevation-B.Elevation)/(2.*T.CellSize);
        if(T.TryGetCell(Coord+FIntPoint(0,1),A)&&T.TryGetCell(Coord-FIntPoint(0,1),B))DY=(A.Elevation-B.Elevation)/(2.*T.CellSize);
        const double Grade=FMath::RadiansToDegrees(FMath::Atan(FMath::Sqrt(DX*DX+DY*DY)));
        if(!MassTerrainFinite(Grade)){Why=TEXT("Nonfinite canonical slope before mantle continuation");return false;}
        if(Grade<P.MinimumSlopeDegrees||Grade>P.MaximumSlopeDegrees)continue;
        if(Out.IsEmpty()||Out.Last().Cells.Num()==4096)
        {if(Out.Num()==32){Why=TEXT("Mantle patch budget exceeded; no partial initialization");return false;}Out.Add(P.Mantle);}
        Out.Last().Cells.Add(Coord);
    }
    return true;
}

FAftershockMaterialInitializationReceipt AftershockMassTerrain::InitializeAuthoredMaterialSubset(UTerrainGridSubsystem& T,
    const FAftershockMaterialWeatherHistory& H,const TArray<FAftershockAuthoredRegolithPatch>& Patches,
    const TArray<FIntPoint>& Targets,TFunctionRef<bool(const FAftershockMaterialInitializationReceipt&,FString&)> StageReceipt)
{
    using namespace AftershockMaterialContinuationPrivate;
    FAftershockMaterialInitializationReceipt R;R.WeatherHistory=H;R.RegolithPatches=Patches;R.BeforeTerrainRevision=T.GetTerrainRevision();
    if(!IsInGameThread()||T.ContinuationGeneratorVersion!=3||T.Cells.IsEmpty()||T.Cells.Num()>65536||Targets.IsEmpty()
        ||Targets.Num()>T.Cells.Num()||Patches.Num()>32||!InRange(T.CellSize,.5,100000.)||!WeatherValid(H,R.Failure))
    {if(R.Failure.IsEmpty())R.Failure=TEXT("Material continuation requires an explicit bounded canonical subset");return R;}
    // Validate the entire resident material snapshot, including preserved cells.
    // No target is silently skipped, clamped or normalized after failure.
    for(int32 I=0;I<T.Cells.Num();++I)
    {
        const auto& C=T.Cells[I];double Ground=0.;
        if(T.CoordToIndex(C.Coords)!=I||!MaterialValid(C,R.Failure)||!InRange(C.ForestFuel01,0.,1.)
            ||!EffectiveGroundElevationM(T,C,Ground,R.Failure))
        {if(R.Failure.IsEmpty())R.Failure=TEXT("Canonical storage key/index mismatch before continuation");return R;}
    }
    TSet<FIntPoint> TargetSet;TargetSet.Reserve(Targets.Num());
    for(FIntPoint Coord:Targets)
    {
        FGridCell C;if(TargetSet.Contains(Coord)||!T.TryGetCell(Coord,C)||C.MaterialScenarioVersion!=0)
        {R.Failure=TEXT("Continuation target is duplicated, nonresident or already initialized; existing stocks never reseeded");return R;}
        TargetSet.Add(Coord);
    }
    TMap<FIntPoint,const FAftershockAuthoredRegolithPatch*> Layers;
    for(const auto& P:Patches)
    {
        if(!MantleValid(P,R.Failure))return R;
        for(FIntPoint Coord:P.Cells)
        {
            FGridCell C;if(!TargetSet.Contains(Coord)||Layers.Contains(Coord)||!T.TryGetCell(Coord,C)||C.bIsOcean||C.bIsRiverbed
                ||(C.SoilType==EAftershockSoilType::Bedrock&&!P.bExplicitWeatheredMantleOverScenarioBedrock))
            {R.Failure=TEXT("Continuation patch overlaps, is outside the target dry land, or invents implicit mantle over bedrock");return R;}
            Layers.Add(Coord,&P);
        }
    }
    TArray<FIntPoint> Ordered=Targets;Ordered.Sort([](FIntPoint A,FIntPoint B){return CoordLess(A,B);});
    TArray<FGridCell> Prior,Staged;Prior.Reserve(Ordered.Num());Staged.Reserve(Ordered.Num());const double Area=FMath::Square(T.CellSize*.01);
    for(FIntPoint Coord:Ordered)
    {
        FGridCell C=T.GetCell(Coord);Prior.Add(C);double Ground=0.;if(!EffectiveGroundElevationM(T,C,Ground,R.Failure))return R;
        C.MaterialScenarioVersion=H.ScenarioVersion;
        if(!C.bIsOcean&&!C.bIsRiverbed)
        {
            double Snow=0.,Liquid=0.;if(!ComputeSnowInventory(H,Ground,Snow,Liquid,R.Failure))return R;
            C.SnowWaterEquivalentMm=static_cast<float>(Snow);C.SnowBulkDensityKgM3=static_cast<float>(H.SnowBulkDensityKgM3);
            C.SnowWeakLayerStrengthPa=static_cast<float>(H.SnowWeakLayerStrengthPa);R.SnowWaterKg+=static_cast<double>(C.SnowWaterEquivalentMm)*Area;
            R.AntecedentLiquidOutflowM3+=Liquid*Area*.001;if(Snow>0.)++R.SnowCells;
        }
        if(const auto* Found=Layers.Find(Coord))
        {
            const auto& P=**Found;C.RegolithDepthM=static_cast<float>(P.DepthM);C.SoilPorosity01=static_cast<float>(P.Porosity01);
            C.SoilDryBulkDensityKgM3=static_cast<float>(P.DryBulkDensityKgM3);C.SoilPoreWaterDepthM=static_cast<float>(P.InitialPoreWaterDepthM);
            C.SoilCohesionPa=static_cast<float>(P.CohesionPa);C.SoilFrictionAngleDegrees=static_cast<float>(P.FrictionAngleDegrees);
            R.DrySoilKg+=static_cast<double>(C.RegolithDepthM)*Area*C.SoilDryBulkDensityKgM3;
            R.PoreWaterM3+=static_cast<double>(C.SoilPoreWaterDepthM)*Area;if(P.DepthM>0.)++R.RegolithCells;
        }
        if(!MaterialValid(C,R.Failure))return R;Staged.Add(C);
    }
    R.CellsVisited=Staged.Num();
    if(!StageReceipt(R,R.Failure))
    {if(R.Failure.IsEmpty())R.Failure=TEXT("Provenance receipt refused before material commit");return R;}
    if(T.GetTerrainRevision()!=R.BeforeTerrainRevision)
    {R.Failure=TEXT("Terrain changed while provenance was staged; no material cells committed");return R;}
    // SetCell only assigns a previously validated resident slot and increments
    // the owner's revision. No callback, yield or source generation occurs here.
    for(int32 I=0;I<Staged.Num();++I)if(!T.SetCell(Staged[I].Coords,Staged[I]))
    {
        bool Restored=true;for(int32 J=0;J<I;++J)Restored=T.SetCell(Prior[J].Coords,Prior[J])&&Restored;
        R.Failure=Restored?TEXT("Prevalidated subset commit failed; applied prefix rolled back"):
            TEXT("Material subset rollback failed; canonical owner must stop");return R;
    }
    R.AfterTerrainRevision=T.GetTerrainRevision();R.bApplied=true;return R;
}

FAftershockMassTerrainSnapshot AftershockMassTerrain::BuildSnapshot(const UTerrainGridSubsystem& T,
    const UBuildingManagerSubsystem& B,const FAftershockMassTerrainOptions& O)
{
    FAftershockMassTerrainSnapshot R;auto& I=R.Input;
    if(!IsInGameThread()||T.ContinuationGeneratorVersion!=3||T.Cells.IsEmpty()||T.Cells.Num()>65536||!InRange(T.CellSize,50.,100000.)
        ||O.MinimumReleaseCells<16||O.MaximumReleaseCells<O.MinimumReleaseCells||O.MaximumReleaseCells>64
        ||O.MinimumResidentSupportCells<1||O.MinimumResidentSupportCells>128
        ||!InRange(O.ReleaseFraction,.001,1.)||(O.bForceRelease&&!O.bCallerAuthorizedGodMode))
    {R.Failure=TEXT("Invalid bounded material snapshot or missing canonical GodMode authorization");return R;}
    I.Material=O.Material;I.HazardTypeTag=O.Material==EAftershockMassMaterial::Snow?7:8;I.TerrainRevision=T.GetTerrainRevision();
    I.CellSizeM=T.CellSize*.01;I.GridOriginM=FVector2D(T.GridOrigin)*.01;I.Settings.Seed=O.Seed;I.Settings.MaxCells=T.Cells.Num();
    I.Settings.MaxWorkUnits=250000000;I.Settings.MaxReleaseCells=64;I.Settings.MaxParcels=512;
    // Full resident physical domain. Outside/missing support is not an artificial
    // drainage outlet: owner must deliberately expand/stage a larger domain.
    I.Settings.bExportAtDomainBoundary=false;
    I.MaterialFriction=[](const FAftershockMassCell& C,EAftershockMassMaterial Material,FAftershockMassFriction& F)
    {
        if(Material==EAftershockMassMaterial::SoilDebris)
            F.Mu=FAftershockMassMovementSolver::EffectiveSoilCoulombCoefficient(F.Mu,C.EffectiveSaturation01,C.SoilDensityKgM3);
        return FMath::IsFinite(F.Mu)&&F.Mu>=0.;
    };
    TArray<FGridCell> Ordered=T.Cells;Ordered.Sort([](const auto& A,const auto& C){return CoordLess(A.Coords,C.Coords);});
    TMap<FIntPoint,int32> Index;for(const auto& C:Ordered)
    {
        if(Index.Contains(C.Coords)){R.Failure=TEXT("Duplicate canonical terrain coordinate");return R;}
        FAftershockMassCell M;M.Coord=C.Coords;M.bLand=!C.bIsOcean;
        if(!EffectiveGroundElevationM(T,C,M.GroundElevationM,R.Failure))return R;
        M.SnowWaterEquivalentMm=C.SnowWaterEquivalentMm;M.SnowDensityKgM3=C.SnowBulkDensityKgM3;M.SnowWeakLayerStrengthPa=C.SnowWeakLayerStrengthPa;
        M.RegolithVerticalDepthM=C.RegolithDepthM;
        M.SoilDensityKgM3=C.SoilDryBulkDensityKgM3+(C.RegolithDepthM>0.f?1000.*C.SoilPoreWaterDepthM/C.RegolithDepthM:0.);
        M.EffectiveSaturation01=C.RegolithDepthM>0.f?FMath::Clamp(C.SoilPoreWaterDepthM/(C.SoilPorosity01*C.RegolithDepthM),0.f,1.f):0.;
        M.CohesionPa=C.SoilCohesionPa;M.FrictionAngleDegrees=C.SoilFrictionAngleDegrees;
        if(!M.bLand){M.SnowWaterEquivalentMm=0.;M.RegolithVerticalDepthM=0.;}
        FAftershockMassSourceInventory Source;Source.Coord=C.Coords;const double Area=I.CellSizeM*I.CellSizeM;
        if(O.Material==EAftershockMassMaterial::Snow)
        {Source.InitialMixtureMassKg=M.SnowWaterEquivalentMm*Area;Source.InitialMixtureVolumeM3=Source.InitialMixtureMassKg/M.SnowDensityKgM3;Source.InitialWaterM3=Source.InitialMixtureMassKg*.001;}
        else {Source.InitialMixtureVolumeM3=M.RegolithVerticalDepthM*Area;Source.InitialDryMassKg=M.RegolithVerticalDepthM*Area*C.SoilDryBulkDensityKgM3;
            Source.InitialWaterM3=M.bLand?C.SoilPoreWaterDepthM*Area:0.;Source.InitialMixtureMassKg=Source.InitialDryMassKg+Source.InitialWaterM3*1000.;}
        Index.Add(C.Coords,I.Cells.Add(M));R.SourceInventories.Add(Source);
    }
    if(!CaptureObstacles(T,B,R))return R;
    if(I.Obstacles.Num()>1024){R.Failure=TEXT("Canonical solid building count exceeds current kernel1024OBB cap; never skip hidden buildings");return R;}
    I.Settings.MaxObstacles=FMath::Max(1,I.Obstacles.Num());
    TSet<FIntPoint> Blocked;
    const FIntRect Bounds=T.GetResidentCellBounds();const double JitterAndRadius=.24*I.CellSizeM+I.CellSizeM/FMath::Sqrt(4.*UE_DOUBLE_PI);
    for(const auto& Obstacle:I.Obstacles)
    {
        const double C=FMath::Abs(FMath::Cos(Obstacle.YawRadians)),S=FMath::Abs(FMath::Sin(Obstacle.YawRadians));
        const FVector2D Extent(C*Obstacle.HalfExtentM.X+S*Obstacle.HalfExtentM.Y+JitterAndRadius,S*Obstacle.HalfExtentM.X+C*Obstacle.HalfExtentM.Y+JitterAndRadius);
        const auto CellAt=[&](FVector2D P){const auto Q=(P-I.GridOriginM)/I.CellSizeM;return FIntPoint(FMath::FloorToInt(Q.X),FMath::FloorToInt(Q.Y));};
        auto Min=CellAt(Obstacle.CenterM-Extent);auto Max=CellAt(Obstacle.CenterM+Extent);
        Min.X=FMath::Max(Min.X,Bounds.Min.X);Min.Y=FMath::Max(Min.Y,Bounds.Min.Y);Max.X=FMath::Min(Max.X,Bounds.Max.X-1);Max.Y=FMath::Min(Max.Y,Bounds.Max.Y-1);
        const int64 Count=static_cast<int64>(Max.X-Min.X+1)*(Max.Y-Min.Y+1);if(Count>65536){R.Failure=TEXT("Oversized canonical obstacle footprint cannot be safely staged");return R;}
        for(int32 Y=Min.Y;Y<=Max.Y;++Y)for(int32 X=Min.X;X<=Max.X;++X)if(Index.Contains(FIntPoint(X,Y)))Blocked.Add(FIntPoint(X,Y));
    }
    R.BlockedSourceCells=Blocked.Num();TArray<FAftershockMassRelease> Requests;Requests.Reserve(I.Cells.Num());
    for(const auto& C:I.Cells){FAftershockMassRelease Q;Q.Coord=C.Coord;Q.AvailableMassFraction=O.ReleaseFraction;Q.bForceRelease=O.bForceRelease;Requests.Add(Q);}
    TArray<FAftershockMassEligibility> Eligible;
    if(!FAftershockMassMovementSolver::EvaluateReleaseCandidates(I,Requests,Eligible,R.Failure))return R;
    // Exact resident topology, including holes and disconnected support. An
    // AABB alone would falsely make the isolated mapped vent chunk look safe.
    // 8-neighbour distance is a conservative square interpolation-support
    // margin for Surface's bilinear stencil, not a promise of full runout.
    const FIntPoint SupportSteps[8]={FIntPoint(-1,-1),FIntPoint(0,-1),FIntPoint(1,-1),FIntPoint(-1,0),
        FIntPoint(1,0),FIntPoint(-1,1),FIntPoint(0,1),FIntPoint(1,1)};
    TArray<int32> SupportDistance;SupportDistance.Init(MAX_int32,I.Cells.Num());
    TArray<int32> SupportQueue;SupportQueue.Reserve(I.Cells.Num());
    for(int32 N=0;N<I.Cells.Num();++N)
    {
        if(!I.Cells[N].bLand){SupportDistance[N]=0;continue;}
        for(FIntPoint D:SupportSteps)
        {
            const int32* Other=Index.Find(I.Cells[N].Coord+D);
            if(!Other||!I.Cells[*Other].bLand){SupportDistance[N]=0;SupportQueue.Add(N);break;}
        }
    }
    for(int32 Head=0;Head<SupportQueue.Num();++Head)
    {
        const int32 N=SupportQueue[Head];for(FIntPoint D:SupportSteps)
        {
            const int32* Other=Index.Find(I.Cells[N].Coord+D);
            if(Other&&I.Cells[*Other].bLand&&SupportDistance[*Other]==MAX_int32)
            {SupportDistance[*Other]=SupportDistance[N]+1;SupportQueue.Add(*Other);}
        }
    }
    TSet<FIntPoint> Allowed;TMap<FIntPoint,double> Score;
    for(int32 N=0;N<Eligible.Num();++N)
    {
        const auto& E=Eligible[N];const auto C=I.Cells[N].Coord;
        const bool SlopeSupported=O.Material==EAftershockMassMaterial::Snow
            ?E.SlopeDegrees>=I.Settings.MinimumSnowSlopeDegrees&&E.SlopeDegrees<=I.Settings.MaximumSnowSlopeDegrees
            :E.SlopeDegrees>=I.Settings.MinimumSoilSlopeDegrees;
        const double MinimumDepth=O.Material==EAftershockMassMaterial::Snow?I.Settings.MinimumSnowDepthM:I.Settings.MinimumSoilDepthM;
        const bool StockSupported=E.AvailableVolumeM3>=MinimumDepth*I.CellSizeM*I.CellSizeM&&Ordered[N].MaterialScenarioVersion>0;
        const double Risk=SlopeSupported&&StockSupported?FMath::Clamp(1.-E.FactorOfSafety,0.,1.):0.;R.Instability01.Add(C,Risk);
        if(E.bEligible&&SlopeSupported&&StockSupported&&!Blocked.Contains(C)&&Ordered[N].MaterialScenarioVersion>0)
        {
            if(SupportDistance[N]<O.MinimumResidentSupportCells){++R.BoundaryExcludedSourceCells;continue;}
            Allowed.Add(C);
            // GodMode may release a stable slab but still requires real stock
            // and the supported slope range. Prefer steeper valid ground when
            // all factors of safety are stable, instead of a random flat patch.
            Score.Add(C,Risk+1.e-6*FMath::Clamp(E.SlopeDegrees/90.,0.,1.));
            if(Risk>0.)++R.NaturalEligibleCells;
        }
    }
    if(O.bHasPreferredRelease&&!Allowed.Contains(O.PreferredRelease))
    {R.Failure=TEXT("Requested release cell lacks eligible finite stock, intersects a solid footprint or needs a larger real resident support margin; no distant source substitution");return R;}
    TArray<FIntPoint> Seeds=O.bHasPreferredRelease?TArray<FIntPoint>{O.PreferredRelease}:Allowed.Array();Seeds.Sort([&](FIntPoint A,FIntPoint C)
    {
        if(O.bHasPreferredRelease){const int64 DA=FMath::Abs(static_cast<int64>(A.X)-O.PreferredRelease.X)+FMath::Abs(static_cast<int64>(A.Y)-O.PreferredRelease.Y),DB=FMath::Abs(static_cast<int64>(C.X)-O.PreferredRelease.X)+FMath::Abs(static_cast<int64>(C.Y)-O.PreferredRelease.Y);if(DA!=DB)return DA<DB;}
        if(Score[A]!=Score[C])return Score[A]>Score[C];const uint32 HA=HashCombine(O.Seed,GetTypeHash(A)),HB=HashCombine(O.Seed,GetTypeHash(C));return HA==HB?CoordLess(A,C):HA<HB;
    });
    TSet<FIntPoint> Seen;TArray<FIntPoint> Selected;
    const FIntPoint Deltas[4]={FIntPoint(1,0),FIntPoint(0,1),FIntPoint(-1,0),FIntPoint(0,-1)};
    for(FIntPoint Seed:Seeds)
    {
        if(Seen.Contains(Seed))continue;TArray<FIntPoint> Queue{Seed};Seen.Add(Seed);int32 Head=0;
        while(Head<Queue.Num()&&Queue.Num()<O.MaximumReleaseCells)
        {const auto C=Queue[Head++];for(auto D:Deltas)if(Allowed.Contains(C+D)&&!Seen.Contains(C+D)){Seen.Add(C+D);Queue.Add(C+D);if(Queue.Num()>=O.MaximumReleaseCells)break;}}
        if(Queue.Num()>=O.MinimumReleaseCells){Selected=MoveTemp(Queue);break;}
    }
    if(Selected.IsEmpty()){R.Failure=TEXT("No connected finite-stock eligible release patch within16–64configured cells; no snow/soil is fabricated");return R;}
    R.MinimumSelectedSupportCells=MAX_int32;
    Selected.Sort(CoordLess);for(auto C:Selected)
    {
        R.MinimumSelectedSupportCells=FMath::Min(R.MinimumSelectedSupportCells,SupportDistance[Index.FindChecked(C)]);
        FAftershockMassRelease Q;Q.Coord=C;Q.AvailableMassFraction=O.ReleaseFraction;Q.bForceRelease=O.bForceRelease;I.Release.Add(Q);
    }
    if(I.TerrainRevision!=T.GetTerrainRevision()){R.Failure=TEXT("Canonical terrain changed during snapshot");return R;}
    R.bReady=true;return R;
}

FAftershockMassWaterTransferMetadata AftershockMassTerrain::StageSourceWaterWithdrawals(const FAftershockMassTerrainSnapshot& S,
    const FAftershockMassResult& R)
{
    FAftershockMassWaterTransferMetadata Out;
    if(!S.bReady||!R.bAccepted||!R.bComplete||!R.bMassConserved||!R.bEnergyClosed||R.SourceTerrainRevision!=S.Input.TerrainRevision||R.Material!=S.Input.Material)
    {Out.Failure=TEXT("Source-water metadata requires same complete accepted terrain snapshot");return Out;}
    TMap<FIntPoint,const FAftershockMassSourceInventory*> ByCoord;for(const auto& C:S.SourceInventories)ByCoord.Add(C.Coord,&C);
    for(const auto& C:R.Cells)
    {
        const auto* Found=ByCoord.Find(C.Coord);if(!Found){Out.Failure=TEXT("Unknown source inventory in accepted result");return Out;}const auto& Initial=**Found;
        if(!InRange(C.ReleasedMassKg,0.,Initial.InitialMixtureMassKg+1.e-5)||!InRange(C.EntrainedMassKg,0.,Initial.InitialMixtureMassKg-C.ReleasedMassKg+1.e-5))
        {Out.Failure=TEXT("Source withdrawal exceeds finite wet inventory");return Out;}
        if(C.ReleasedMassKg+C.EntrainedMassKg<=0.)continue;
        const double WaterPerKg=Initial.InitialMixtureMassKg>0.?Initial.InitialWaterM3/Initial.InitialMixtureMassKg:0.;
        FAftershockMassSourceWaterWithdrawal W;W.Coord=C.Coord;W.ReleasedWaterM3=C.ReleasedMassKg*WaterPerKg;W.EntrainedWaterM3=C.EntrainedMassKg*WaterPerKg;
        W.RemainingSourceWaterM3=FMath::Max(0.,Initial.InitialWaterM3-W.ReleasedWaterM3-W.EntrainedWaterM3);Out.Sources.Add(W);
        Out.ReleasedWaterM3+=W.ReleasedWaterM3;Out.EntrainedWaterM3+=W.EntrainedWaterM3;
    }
    const double WetMass=R.Budget.ReleasedKg+R.Budget.EntrainedKg;
    Out.WaterMassFraction=WetMass>0.?(Out.ReleasedWaterM3+Out.EntrainedWaterM3)*1000./WetMass:0.;
    Out.bValid=InRange(Out.WaterMassFraction,0.,1.+1.e-8);if(!Out.bValid)Out.Failure=TEXT("Invalid event-wide finite water-mixture fraction");return Out;
}
