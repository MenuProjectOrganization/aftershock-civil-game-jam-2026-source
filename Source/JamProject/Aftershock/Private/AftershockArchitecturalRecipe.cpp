#include "AftershockArchitecturalRecipe.h"

namespace
{
	uint32 Mix(uint32 V){V^=V>>16;V*=0x7feb352du;V^=V>>15;V*=0x846ca68bu;return V^(V>>16);}
	uint32 StableName(FName Name)
	{
		// FName/object hash indices are process-dependent. Hash UTF16 character
		// values explicitly; stable under process restart and source array order.
		uint32 H=2166136261u;for(TCHAR C:Name.ToString()){H^=static_cast<uint32>(C);H*=16777619u;}return H;
	}
	bool IsCivic(EBuildingType T)
	{return T==EBuildingType::Hospital||T==EBuildingType::School||T==EBuildingType::FireStation||T==EBuildingType::Shelter||T==EBuildingType::TaxOffice||T==EBuildingType::Courthouse||T==EBuildingType::PoliceStation;}

    struct FWing {float X,Y,W,D;};
    FAftershockArchitecturalRecipe BuildParkRecipe(const FAftershockArchitecturalInput& I,EAftershockArchitecturalTier Tier,FAftershockArchitecturalRecipe R)
    {
        using P=EAftershockArchitecturalPart;using F=EAftershockArchitecturalFinish;
        R.StyleID=TEXT("derived_biome_park_no_plinth");
        const double W=I.EnvelopeCm.X,D=I.EnvelopeCm.Y,H=I.EnvelopeCm.Z;
        const FBox Envelope(FVector(-W*.5,-D*.5,0),FVector(W*.5,D*.5,H));
        const auto Add=[&](P Part,FVector Base,FVector Box,float Yaw=0.f)
        {
            if(R.Pieces.Num()>=R.MaximumPieces||Box.GetMin()<=.01)return;
            FAftershockArchitecturalPlacement A;A.Part=Part;A.Finish=Part==P::ParkPath?F::ParkPaving:F::ParkNative;
            A.Variant=static_cast<uint8>((R.ShapeSeed+R.Pieces.Num()*31u)%3u);
            // For native flora/bench this is a MAXIMUM reservation. Renderer
            // fits the measured native mesh UNIFORMLY; no leaf/tree stretching.
            A.LocalTransform=FTransform(FRotator(0,Yaw,0),Base,Box);
            A.LocalConservativeBounds=FBox(FVector(-.5,-.5,0),FVector(.5,.5,1)).TransformBy(A.LocalTransform);
            if(Envelope.ExpandBy(.01).IsInsideOrOn(A.LocalConservativeBounds.Min)&&
               Envelope.ExpandBy(.01).IsInsideOrOn(A.LocalConservativeBounds.Max))R.Pieces.Add(MoveTemp(A));
        };
        P Tree=P::ParkBroadleaf;
        switch(I.RegionalBiome)
        {
        case EAftershockRegionalBiome::HumidBlackSea:
        case EAftershockRegionalBiome::Mediterranean:
        case EAftershockRegionalBiome::VolcanicMediterranean:
        case EAftershockRegionalBiome::ContinentalTaiga:
        case EAftershockRegionalBiome::MonsoonHighland:Tree=P::ParkConifer;break;
        case EAftershockRegionalBiome::TropicalMaritime:
        case EAftershockRegionalBiome::TropicalMonsoon:Tree=P::ParkPalm;break;
        case EAftershockRegionalBiome::SemiAridSteppe:
        case EAftershockRegionalBiome::SubpolarVolcanic:Tree=P::ParkShrub;break;
        default:break;
        }
        // No decorative square lawn/tile base. The real regional soil remains
        // exposed. Cold/arid profiles receive shrubs, never invented palm/pines.
        const FVector TreeBox(W*.64,D*.64,FMath::Min(H*.93,650.));
        if(Tier==EAftershockArchitecturalTier::Regional)
        {Add(P::ParkShrub,FVector::ZeroVector,FVector(FMath::Min(W*.5,150.),FMath::Min(D*.5,150.),FMath::Min(H*.9,110.)));return R;}
        if(!I.bParkGroundNearlyLevel)
        {
            // On slopes, only the exact canonical centre is a guaranteed root
            // datum. Omit benches/slabs/offset plants instead of floating them.
            Add(Tree,FVector::ZeroVector,Tree==P::ParkShrub?FVector(FMath::Min(W*.55,160.),FMath::Min(D*.55,160.),FMath::Min(H*.9,130.)):TreeBox);
            return R;
        }
        Add(Tree,FVector(-W*.16,D*.12,.5),Tree==P::ParkShrub?FVector(FMath::Min(W*.5,150.),FMath::Min(D*.5,150.),FMath::Min(H*.9,120.)):TreeBox);
        const double SmallH=FMath::Min(H*.25,100.);
        Add(P::ParkShrub,FVector(W*.30,D*.28,.5),FVector(FMath::Min(W*.25,110.),FMath::Min(D*.25,110.),SmallH));
        Add(P::ParkShrub,FVector(W*.28,D*.08,.5),FVector(FMath::Min(W*.22,95.),FMath::Min(D*.22,95.),SmallH*.8));
        if(Tier==EAftershockArchitecturalTier::Street)
        {
            Add(P::ParkGrass,FVector(-W*.28,-D*.08,.5),FVector(50,50,FMath::Min(H*.1,45.)));
            Add(P::ParkGrass,FVector(W*.29,-D*.07,.5),FVector(50,50,FMath::Min(H*.1,45.)));
        }
        // Native~2.4m bench and narrow6cm pedestrian path, only where all9
        // terrain probes differ from the canonical root by <=3cm. No full pad.
        Add(P::ParkPath,FVector(0,-D*.28,.5),FVector(W*.80,FMath::Min(D*.14,95.),6.));
        if(W>=600.&&D>=500.&&H>=110.)Add(P::ParkBench,FVector(W*.16,-D*.13,.5),FVector(FMath::Min(W*.40,242.),80.,FMath::Min(H-1.,110.)));
        return R;
    }

	FLinearColor SurfacePalette(FName Region,uint32 AppearanceSeed)
	{
		// Authored restrained cladding palettes; not a measured cadastral palette.
		const uint32 N=AppearanceSeed%4u;
        if(Region==TEXT("cyprus"))
        {
            // Weathered chalk, limestone and warm plaster: bounded authored
            // tints, not a claim of surveyed local facade colours.
            const FLinearColor P[]={{.82f,.76f,.63f},{.65f,.58f,.45f},{.74f,.63f,.51f},{.72f,.74f,.69f}};
            return P[N];
        }
		if(Region==TEXT("valparaiso"))
		{const FLinearColor P[]={{.73f,.83f,.89f},{.90f,.78f,.66f},{.78f,.85f,.72f},{.87f,.73f,.72f}};return P[N];}
		if(Region==TEXT("grindavik")||Region==TEXT("yakutsk"))
		{const FLinearColor P[]={{.75f,.82f,.85f},{.83f,.73f,.65f},{.78f,.81f,.73f},{.89f,.88f,.82f}};return P[N];}
		if(Region==TEXT("new_orleans")||Region==TEXT("miami_keys"))
		{const FLinearColor P[]={{.95f,.91f,.79f},{.79f,.87f,.83f},{.87f,.81f,.78f},{.91f,.94f,.92f}};return P[N];}
		if(Region==TEXT("rotterdam")||Region==TEXT("kathmandu"))
		{const FLinearColor P[]={{.86f,.76f,.68f},{.88f,.84f,.77f},{.77f,.72f,.67f},{.93f,.89f,.81f}};return P[N];}
		const FLinearColor P[]={{.83f,.78f,.68f},{.72f,.67f,.57f},{.75f,.77f,.72f},{.88f,.82f,.73f}};return P[N];
	}
}

FAftershockArchitecturalStyle ResolveAftershockArchitecturalStyle(FName RegionID,FName FocusID)
{
	using P=EAftershockArchitecturalPart;using F=EAftershockArchitecturalFinish;
	FAftershockArchitecturalStyle S;
	// Authored forms, not a reconstruction of all local buildings. Primary
	// reference scope and outstanding local review are in the accompanying plan.
	if(RegionID==TEXT("atakent")){S.ID=TEXT("marmara_balcony_block");S.Roof=P::RoofFlat;S.FacadeBays=4;}
	else if(RegionID==TEXT("dereyolu")){S.ID=TEXT("blacksea_timber_eaves");S.Roof=P::RoofGable;S.Wall=F::TimberCladding;S.WidthFraction=.62f;S.RoofHeightCm=105.f;S.bPorch=true;}
	else if(RegionID==TEXT("yelova")){S.ID=TEXT("amanos_slope_town");S.Roof=P::RoofHip;S.bAllowWings=true;S.RoofHeightCm=95.f;}
	else if(RegionID==TEXT("kiyikent")){S.ID=TEXT("levant_shaded_flat");S.Roof=P::RoofFlat;S.bPorch=true;S.bCourtyard=true;}
	else if(RegionID==TEXT("mugla")){S.ID=TEXT("aegean_shutter_tile");S.Roof=P::RoofGable;S.RoofHeightCm=100.f;S.bPorch=true;}
	else if(RegionID==TEXT("etna")){S.ID=TEXT("sicilian_lowerflank_tile");S.Roof=P::RoofHip;S.RoofHeightCm=100.f;S.bCourtyard=true;}
	else if(RegionID==TEXT("konya")){S.ID=TEXT("steppe_courtyard_farm");S.Roof=P::RoofGable;S.bCourtyard=true;S.DepthFraction=.82f;}
	else if(RegionID==TEXT("miami_keys"))
	{
		if(FocusID==TEXT("miami")){S.ID=TEXT("miami_modern_balcony");S.Roof=P::RoofFlat;S.Wall=F::GlassScreen;S.FacadeBays=4;}
		else if(FocusID==TEXT("key_largo")){S.ID=TEXT("key_largo_deep_porch");S.Roof=P::RoofHip;S.Wall=F::TimberCladding;S.bPorch=true;S.DepthFraction=.72f;}
		else if(FocusID==TEXT("marathon")){S.ID=TEXT("marathon_island_service");S.Roof=P::RoofHip;S.bPorch=true;S.DepthFraction=.78f;}
		else if(FocusID==TEXT("key_west")){S.ID=TEXT("key_west_front_veranda");S.Roof=P::RoofGable;S.Wall=F::TimberCladding;S.WidthFraction=.68f;S.bPorch=true;S.RoofHeightCm=100.f;}
	}
	else if(RegionID==TEXT("cyprus"))
	{
		if(FocusID==TEXT("kalkanli")){S.ID=TEXT("kalkanli_small_courtyard");S.Roof=P::RoofFlat;S.WidthFraction=.82f;S.bCourtyard=true;S.bPorch=true;}
		else if(FocusID==TEXT("alaykoy")){S.ID=TEXT("alaykoy_service_court");S.Roof=P::RoofFlat;S.FacadeBays=4;}
		else if(FocusID==TEXT("lefke")){S.ID=TEXT("lefke_orchard_veranda");S.Roof=P::RoofHip;S.bCourtyard=true;S.bPorch=true;S.RoofHeightCm=100.f;}
	}
	else if(RegionID==TEXT("yakutsk")){S.ID=TEXT("yakutsk_insulated_timber");S.Roof=P::RoofGable;S.Wall=F::TimberCladding;S.WidthFraction=.56f;S.RoofHeightCm=110.f;S.FacadeBays=2;S.bPorch=true;}
	else if(RegionID==TEXT("valparaiso")){S.ID=TEXT("valparaiso_narrow_wings");S.Roof=P::RoofGable;S.Wall=F::MetalCladding;S.WidthFraction=.68f;S.RoofHeightCm=100.f;}
	else if(RegionID==TEXT("valdivia")){S.ID=TEXT("valdivia_timber_rain_eaves");S.Roof=P::RoofGable;S.Wall=F::TimberCladding;S.WidthFraction=.62f;S.RoofHeightCm=110.f;S.bPorch=true;}
	else if(RegionID==TEXT("sendai")){S.ID=TEXT("sendai_coastal_hip");S.Roof=P::RoofHip;S.WidthFraction=.80f;S.RoofHeightCm=110.f;S.bPorch=true;}
	else if(RegionID==TEXT("rotterdam")){S.ID=TEXT("dutch_narrow_bay");S.Roof=P::RoofGable;S.Wall=F::BrickVeneer;S.WidthFraction=.60f;S.RoofHeightCm=110.f;S.FacadeBays=2;}
	else if(RegionID==TEXT("new_orleans")){S.ID=TEXT("creole_shotgun_gallery");S.Roof=P::RoofGable;S.Wall=F::TimberCladding;S.WidthFraction=.58f;S.RoofHeightCm=105.f;S.bPorch=true;S.FacadeBays=2;}
	else if(RegionID==TEXT("grindavik")){S.ID=TEXT("iceland_timber_metal");S.Roof=P::RoofGable;S.Wall=F::MetalCladding;S.WidthFraction=.60f;S.RoofHeightCm=110.f;S.FacadeBays=2;}
	else if(RegionID==TEXT("vesuvius")){S.ID=TEXT("campanian_terrace_tile");S.Roof=P::RoofHip;S.bPorch=true;S.RoofHeightCm=100.f;}
	else if(RegionID==TEXT("christchurch")){S.ID=TEXT("canterbury_bay_veranda");S.Roof=P::RoofHip;S.Wall=F::TimberCladding;S.bPorch=true;S.WidthFraction=.84f;}
	else if(RegionID==TEXT("kathmandu")){S.ID=TEXT("himalayan_brick_courtyard");S.Roof=P::RoofFlat;S.Wall=F::BrickVeneer;S.bCourtyard=true;S.FacadeBays=3;}
	else if(RegionID==TEXT("jakarta")){S.ID=TEXT("jakarta_ventilated_eaves");S.Roof=P::RoofHip;S.bPorch=true;S.DepthFraction=.76f;S.RoofHeightCm=105.f;}
	if(RegionID==TEXT("yakutsk")||RegionID==TEXT("valparaiso")||RegionID==TEXT("valdivia")||RegionID==TEXT("grindavik")||RegionID==TEXT("new_orleans"))S.RoofFinish=F::RoofMetal;
	return S;
}

bool SupportsAftershockArchitecturalType(EBuildingType T)
{
	return T==EBuildingType::GreenSpace||T==EBuildingType::House||T==EBuildingType::Apartment||T==EBuildingType::Office||T==EBuildingType::Factory||T==EBuildingType::Warehouse
		||T==EBuildingType::Bakery||T==EBuildingType::Restaurant||IsCivic(T);
}

FAftershockArchitecturalRecipe BuildAftershockArchitecturalRecipe(const FAftershockArchitecturalInput& I,EAftershockArchitecturalTier Tier,bool bNativeFacadeFamilyReady)
{
	FAftershockArchitecturalRecipe R;
	if(!SupportsAftershockArchitecturalType(I.Type)){R.Rejection=TEXT("Unsupported logical type retains its existing specialized renderer");return R;}
	if(I.Frame.ContainsNaN()||I.EnvelopeCm.ContainsNaN()||I.EnvelopeCm.GetMin()<=0.f||I.Floors<1||I.Floors>30)
	{R.Rejection=TEXT("Invalid canonical visual frame");return R;}
	if(I.DamageState>=EDamageState::Collapsed){R.Rejection=TEXT("Collapsed/memorial intact architecture must stay hidden");return R;}
	if(Tier==EAftershockArchitecturalTier::Globe){R.Rejection=TEXT("Globe tier omits local architecture");return R;}
	FAftershockArchitecturalStyle S=ResolveAftershockArchitecturalStyle(I.RegionID,I.FocusID);R.StyleID=S.ID;
	if(S.ID==TEXT("unresolved")){R.Rejection=TEXT("Unresolved region/focus cannot inherit an arbitrary city's architecture");return R;}
	R.ShapeSeed=Mix(static_cast<uint32>(I.TerrainSeed)^StableName(I.RegionID)^Mix(StableName(I.FocusID))
		^Mix(static_cast<uint32>(I.Coord.X))^Mix(static_cast<uint32>(I.Coord.Y)+0x9e3779b9u)
		^static_cast<uint32>(I.Type)*0x85ebca6bu^static_cast<uint32>(I.Floors)*0x632be59bu);
	R.AppearanceSeed=Mix(R.ShapeSeed^static_cast<uint32>(FMath::Max(0,I.BuildingID)));
	R.SurfaceTint=SurfacePalette(I.RegionID,R.AppearanceSeed);
	R.MaterialVariant=static_cast<uint8>(R.AppearanceSeed%4u);R.FacadeBays=S.FacadeBays;
	if(I.Type==EBuildingType::GreenSpace)return BuildParkRecipe(I,Tier,MoveTemp(R));
    const bool Industry=I.Type==EBuildingType::Factory||I.Type==EBuildingType::Warehouse;
	const bool Tall=I.Floors>=4||I.Type==EBuildingType::Office;
	if(Industry){S.Roof=EAftershockArchitecturalPart::RoofShed;S.Wall=EAftershockArchitecturalFinish::MetalCladding;S.WidthFraction=.94f;S.DepthFraction=.94f;S.bCourtyard=false;S.bPorch=false;}
	if(Tall){S.Roof=EAftershockArchitecturalPart::RoofFlat;S.WidthFraction=.94f;S.DepthFraction=.94f;S.bAllowWings=false;S.bCourtyard=false;S.RoofHeightCm=FMath::Clamp(I.EnvelopeCm.Z-I.Floors*300.f,60.f,240.f);}
    // A small minority of low Kalkanli houses receive a shallow hipped tile
    // roof. Tall/civic/industrial forms retain their explicit policies.
    if(I.RegionID==TEXT("cyprus")&&I.FocusID==TEXT("kalkanli")&&
        I.Type==EBuildingType::House&&I.Floors<=2&&(R.ShapeSeed>>12)%5u==0u)
    {S.Roof=EAftershockArchitecturalPart::RoofHip;S.RoofHeightCm=95.f;}
    // Keep the existing honest floor pitch. A flat cap need not consume the
    // entire saved roof allowance as an 80-240cm solid slab; unused top space
    // remains inside the immutable canonical envelope, without raising floors.
    const float RoofAllowance=FMath::Min(S.RoofHeightCm,I.EnvelopeCm.Z*.24f);
    const bool Native=bNativeFacadeFamilyReady&&Tier!=EAftershockArchitecturalTier::Regional&&I.Floors<=4
        &&(I.RegionID==TEXT("cyprus")||I.RegionID==TEXT("atakent"))
        &&(I.Type==EBuildingType::House||I.Type==EBuildingType::Apartment)
        &&I.EnvelopeCm.Z-I.Floors*300.f>=32.f&&I.EnvelopeCm.Z-I.Floors*300.f<=240.f;
    const float BodyH=Native?I.Floors*300.f:I.EnvelopeCm.Z-RoofAllowance;
    const float RoofH=S.Roof==EAftershockArchitecturalPart::RoofFlat?
        FMath::Min(32.f,I.EnvelopeCm.Z-BodyH):FMath::Min(RoofAllowance,I.EnvelopeCm.Z-BodyH);
    R.FloorPitchCm=BodyH/I.Floors;
	if(R.FloorPitchCm<240.f||(!Industry&&R.FloorPitchCm>380.f))
	{R.Rejection=TEXT("Legacy/canonical envelope cannot fit honest floor height; retain original renderer");return R;}
	const FBox Envelope(FVector(-I.EnvelopeCm.X*.5,-I.EnvelopeCm.Y*.5,0),FVector(I.EnvelopeCm.X*.5,I.EnvelopeCm.Y*.5,I.EnvelopeCm.Z));
	const auto Add=[&](EAftershockArchitecturalPart Part,EAftershockArchitecturalFinish Finish,FVector Centre,FVector Size,float Yaw=0.f)
	{
		if(R.Pieces.Num()>=R.MaximumPieces||Size.GetMin()<=0.f)return;
		auto& P=R.Pieces.AddDefaulted_GetRef();P.Part=Part;P.Finish=Finish;P.Variant=static_cast<uint8>((R.ShapeSeed>>8)%3u);
		P.LocalTransform=FTransform(FRotator(0,Yaw,0),Centre-FVector(0,0,Size.Z*.5),Size);
		P.LocalConservativeBounds=FBox(FVector(-.5,-.5,0),FVector(.5,.5,1)).TransformBy(P.LocalTransform);
	};
	TArray<FWing,TInlineAllocator<3>> Wings;
	const float W=I.EnvelopeCm.X*S.WidthFraction,D=I.EnvelopeCm.Y*S.DepthFraction;
	const uint32 Form=R.ShapeSeed%4u;
	if(Tier==EAftershockArchitecturalTier::Regional)Wings.Add({0,0,W,D}); // One conservative mass+roof far proxy, at most2 instances/building.
    else if(S.bCourtyard&&I.Type==EBuildingType::House&&I.Floors==1&&
        (R.ShapeSeed>>16)%11u==0u&&W>=620.f&&D>=600.f)
    {
        // Rare, broad single-storey court. Never three thin multi-storey walls
        // around a deep shaft. Wings exactly meet without coplanar overlap.
        Wings.Add({-W*.33f,0,W*.34f,D});Wings.Add({W*.33f,0,W*.34f,D});
        Wings.Add({0,D*.33f,W*.32f,D*.34f});
    }
    else if(S.bAllowWings&&I.Floors<=2&&Form==1&&(R.ShapeSeed>>9)%3u==0u&&!Industry)
	{
		Wings.Add({0,-D*.225f,W,D*.55f});Wings.Add({-W*.225f,D*.275f,W*.55f,D*.45f});
	}
	else Wings.Add({0,0,W,D});
	for(const FWing& Wing:Wings)
	{
		// The facade panels own the outer surface. Inset the core so its face
		// cannot coincide with a panel and produce a second coplanar surface.
		const float T=Tier==EAftershockArchitecturalTier::Regional?0.f:Native?75.f:FMath::Min(30.f,FMath::Min(Wing.W,Wing.D)*.08f);
		Add(EAftershockArchitecturalPart::Mass,S.Wall,FVector(Wing.X,Wing.Y,BodyH*.5),FVector(Wing.W-2.f*T,Wing.D-2.f*T,BodyH));
		const auto RoofFinish=S.Roof==EAftershockArchitecturalPart::RoofShed?EAftershockArchitecturalFinish::RoofMetal:
			S.Roof==EAftershockArchitecturalPart::RoofFlat?EAftershockArchitecturalFinish::ConcreteCladding:S.RoofFinish;
		Add(S.Roof,RoofFinish,FVector(Wing.X,Wing.Y,BodyH+RoofH*.5),FVector(Wing.W,Wing.D,RoofH));
		R.Pieces.Last().AccentFinish=S.Wall; // roof gable wall retains this building's cladding
		if(Tier==EAftershockArchitecturalTier::Regional)continue;
		// One strip covers the entire facade; a metric UV/trim material repeats
		// floors/bays. No individually spawned windows or per-floor CPU meshes.
		Add(EAftershockArchitecturalPart::FacadeStrip,S.Wall,FVector(Wing.X,Wing.Y-Wing.D*.5+T*.5,BodyH*.5),FVector(Wing.W,T,BodyH));
		Add(EAftershockArchitecturalPart::FacadeStrip,S.Wall,FVector(Wing.X,Wing.Y+Wing.D*.5-T*.5,BodyH*.5),FVector(Wing.W,T,BodyH),180.f);
		Add(EAftershockArchitecturalPart::FacadeStrip,S.Wall,FVector(Wing.X-Wing.W*.5+T*.5,Wing.Y,BodyH*.5),FVector(Wing.D-2.f*T,T,BodyH),-90.f);
		Add(EAftershockArchitecturalPart::FacadeStrip,S.Wall,FVector(Wing.X+Wing.W*.5-T*.5,Wing.Y,BodyH*.5),FVector(Wing.D-2.f*T,T,BodyH),90.f);
		for(int32 At=R.Pieces.Num()-4;At<R.Pieces.Num();++At)
        {
            auto& Face=R.Pieces[At];const double Width=Face.LocalTransform.GetScale3D().X;
            Face.Bays=static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Width/(Native?300.:250.)),1,4));
            if(Native)
            {
                const double Pitch=Width/Face.Bays;
                // Native source is uniformly baked at300cm pitch. Bound the
                // remaining whole-envelope fit; never stretch onefloor across4.
                // Keep an even frontbay count around the existing central door.
                const bool DoorFront=At==R.Pieces.Num()-4;
                if(Pitch<255.||Pitch>360.||(DoorFront&&Face.Bays%2!=0))
                {R.Pieces.Reset();R.Rejection=TEXT("Native facade cannot fit a door-safe real bay inside this shape; retry original recipe");return R;}
                Face.Variant=240;
            }
        }
	}
	if(Tier==EAftershockArchitecturalTier::Street)
	{
		// Courtyard forms have no central front wall. Bind the door to an
		// actual wing; never leave a door floating in the courtyard opening.
		const FWing& FrontWing=Wings[0];
		const float DoorH=FMath::Min(215.f,R.FloorPitchCm*.74f),DoorY=FrontWing.Y-FrontWing.D*.5-2.5f;
		Add(EAftershockArchitecturalPart::Door,S.Wall,FVector(FrontWing.X,DoorY,DoorH*.5),FVector(FMath::Min(95.f,FrontWing.W-30.f),15.f,DoorH));
		if(S.bPorch&&Wings.Num()==1)
		{
			const float FrontSpace=(I.EnvelopeCm.Y-D)*.5;
			if(FrontSpace>=25.f)Add(EAftershockArchitecturalPart::Canopy,S.Wall,FVector(0,-D*.5-FrontSpace*.5,DoorH+12.f),FVector(W*.70f,FrontSpace,12.f));
		}
	}
	for(const auto& Piece:R.Pieces)
	{
		if(!Envelope.ExpandBy(.01f).IsInsideOrOn(Piece.LocalConservativeBounds.Min)||!Envelope.ExpandBy(.01f).IsInsideOrOn(Piece.LocalConservativeBounds.Max))
		{R.Pieces.Reset();R.Rejection=TEXT("Module conservative bounds escaped canonical envelope");break;}
	}
	return R;
}
