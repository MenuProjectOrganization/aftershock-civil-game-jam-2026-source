#include "Aftershock/UI/WaterGlassFrameWidget.h"
#include "Widgets/SLeafWidget.h"
#include "Rendering/DrawElements.h"
#include "Brushes/SlateRoundedBoxBrush.h"

class SWaterGlassScene final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SWaterGlassScene) {} SLATE_END_ARGS()
	void Construct(const FArguments&) { SetCanTick(false); }
	void SetScene(float InProgress,double InSeconds,bool InEpilogue,bool InLoss,int32 InFlowers)
	{ Progress=FMath::Clamp(InProgress,0.f,1.f);Seconds=InSeconds;bEpilogue=InEpilogue;bLoss=InLoss;Flowers=FMath::Clamp(InFlowers,0,12);Invalidate(EInvalidateWidgetReason::Paint); }
	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1600,900); }
	virtual int32 OnPaint(const FPaintArgs&,const FGeometry& Geometry,const FSlateRect&,FSlateWindowElementList& Elements,int32 Layer,const FWidgetStyle& Style,bool) const override
	{
		const FVector2f Available(Geometry.GetLocalSize());
		const float Scale=FMath::Max(.001f,FMath::Min(Available.X/1600.f,Available.Y/900.f));
		const FVector2f Offset=(Available-FVector2f(1600,900)*Scale)*.5f;
		const FGeometry Art=Geometry.MakeChild(FVector2f(1600,900),FSlateLayoutTransform(Scale,Offset));
		const FLinearColor Tint=Style.GetColorAndOpacityTint();
		const auto Box=[&](float X,float Y,float W,float H,FLinearColor Color,float Radius=0.f)
		{
			const FSlateRoundedBoxBrush Brush(Color,Radius);
			// MakeBox consumes vertex tint verbatim; it does not apply the brush's
			// fill tint. Match SBorder's paint contract so fills and their alpha
			// retain the authored navy/wood/garden palette instead of opaque white.
			FSlateDrawElement::MakeBox(Elements,Layer++,Art.ToPaintGeometry(FVector2f(W,H),FSlateLayoutTransform(FVector2f(X,Y))),&Brush,ESlateDrawEffect::None,Brush.GetTint(Style)*Tint);
		};
		const auto Line=[&](std::initializer_list<FVector2f> Points,FLinearColor Color,float Width=2.f)
		{
			TArray<FVector2f> Path;for(const auto& Point:Points)Path.Add(Point);
			FSlateDrawElement::MakeLines(Elements,Layer++,Art.ToPaintGeometry(),MoveTemp(Path),ESlateDrawEffect::None,Color*Tint,true,Width);
		};
		const auto Ellipse=[&](FVector2f Centre,FVector2f Radius,FLinearColor Color,float Width)
		{
			TArray<FVector2f> Path;for(int32 I=0;I<=48;++I){const float A=2.f*PI*I/48.f;Path.Add(Centre+FVector2f(FMath::Cos(A)*Radius.X,FMath::Sin(A)*Radius.Y));}
			FSlateDrawElement::MakeLines(Elements,Layer++,Art.ToPaintGeometry(),MoveTemp(Path),ESlateDrawEffect::None,Color*Tint,true,Width);
		};
		// Garden, evening light and distant timber fence, all authored geometry.
		Box(0,0,1600,900,FLinearColor(.018f,.043f,.047f));
		Box(760,0,840,600,FLinearColor(.11f,.18f,.15f));
		Box(1260,30,260,260,FLinearColor(.27f,.30f,.19f,.32f),130);
		for(int32 I=0;I<13;++I)
		{
			const float X=660.f+I*88.f;
			Box(X,260,62,350,FLinearColor(.09f,.13f,.105f),16);
			Line({{X+8,280},{X+12,580}},FLinearColor(.19f,.23f,.17f),1.f);
		}
		for(int32 I=0;I<19;++I)
		{
			const float X=590.f+I*62.f;const float Y=390.f+28.f*FMath::Sin(I*1.7f);
			Box(X,Y,125,160,FLinearColor(.035f,.10f,.075f),62);
		}
		// Seated silhouette and the hand around the glass.
		const FLinearColor Silhouette(.015f,.027f,.030f);
		Box(1240,190,108,133,Silhouette,54);
		Box(1202,320,189,355,Silhouette,72);
		Line({{1238,370},{1155,458},{1058,475}},Silhouette,49);
		Box(1018,451,72,42,Silhouette,20);
		Line({{1375,371},{1423,286},{1285,239}},Silhouette,43);
		Box(1243,216,75,43,Silhouette,19);
		// The garden table has grain, board joints and an honest illustrated surface.
		Box(0,605,1600,295,FLinearColor(.205f,.122f,.068f));
		Box(0,617,1600,10,FLinearColor(.33f,.205f,.10f));
		for(int32 I=0;I<11;++I)
		{
			const float Y=638.f+I*25.f;
			Line({{0,Y},{430,Y-5},{980,Y+3},{1600,Y-2}},FLinearColor(.32f,.20f,.115f),1.3f);
		}
		for(int32 I=1;I<6;++I)Line({{I*275.f,625},{I*275.f-80,900}},FLinearColor(.105f,.061f,.038f),3.f);
		Ellipse({1085,702},{117,16},FLinearColor(.10f,.065f,.043f),9);
		// Transparent drinking glass; the remaining water is poured into soil.
		const float Pour=FMath::SmoothStep(.18f,.78f,Progress);
		const float Fill=bEpilogue?0.f:FMath::Lerp(.22f,0.f,Pour);
		const FLinearColor Glass(.72f,.89f,.86f,.68f);
		if(Fill>.001f)
		{
			Box(963,510-Fill*170,154,Fill*170,FLinearColor(.28f,.64f,.66f,.68f),9);
			Ellipse({1040,510-Fill*170},{77,11},FLinearColor(.56f,.85f,.83f,.8f),2.f);
		}
		Line({{945,337},{963,533},{1117,533},{1135,337}},Glass,3.f);
		Ellipse({1040,337},{95,18},Glass,3.f);Ellipse({1040,533},{77,11},Glass,2.f);
		Line({{960,359},{973,500}},FLinearColor(.88f,.96f,.88f,.58f),4.f);
		// A small tilted stream and damp soil: a visual story, not a fluid solver.
		const float Stream=bEpilogue?0.f:FMath::Sin(PI*Pour);
		if(Stream>.02f)
		{
			const float Sway=FMath::Sin(Seconds*2.7)*5.f;
			Line({{1121,351},{1180+Sway,455},{1170,607},{1140,648}},FLinearColor(.49f,.82f,.81f,Stream),4.f);
			for(int32 I=0;I<4;++I)Box(1160+I*13,580+static_cast<float>(FMath::Fmod(Seconds*90+I*30,72.)),4,9,FLinearColor(.60f,.86f,.82f,Stream),2);
		}
		Box(1070,651,180,62,FLinearColor(.095f,.068f,.040f),30);
		Box(1093,658,143,30,FLinearColor(.065f,.064f,.039f),15);
		for(int32 I=0;I<19;++I)Box(1080+(I*37)%154,658+(I*13)%31,4,3,FLinearColor(.28f,.18f,.08f),1);
		Line({{1200,663},{1205,615},{1190,590}},FLinearColor(.25f,.48f,.26f),5);
		Box(1169,591,35,16,FLinearColor(.30f,.53f,.28f),8);Box(1203,613,34,15,FLinearColor(.38f,.58f,.30f),7);
		if(bEpilogue&&bLoss){Line({{1009,363},{1031,402},{1017,424},{1052,463}},FLinearColor(.68f,.80f,.76f,.42f),1.2f);}
		for(int32 I=0;I<Flowers;++I)
		{
			const FVector2f Flower(1090+(I%6)*24,735+(I/6)*25);
			Line({Flower,Flower+FVector2f(25,38)},FLinearColor(.20f,.36f,.21f),2.f);
			Box(Flower.X-7,Flower.Y-7,15,15,FLinearColor(.89f,.80f,.64f),7);
			Box(Flower.X-2,Flower.Y-2,5,5,FLinearColor(.74f,.48f,.19f),2);
		}
		// Quiet left reading surface, with no external background or font art.
		Box(0,0,790,900,FLinearColor(.012f,.027f,.034f,.94f));
		Box(790,0,40,900,FLinearColor(.012f,.027f,.034f,.56f));
		return Layer;
	}
private:
	float Progress=0.f;double Seconds=0.;bool bEpilogue=false;bool bLoss=false;int32 Flowers=0;
};

TSharedRef<SWidget> UWaterGlassFrameWidget::RebuildWidget()
{
	SAssignNew(Scene,SWaterGlassScene);Scene->SetScene(Progress,RealSeconds,bEpilogue,bLoss,Flowers);return Scene.ToSharedRef();
}
void UWaterGlassFrameWidget::SetScene(float InProgress,double InRealSeconds,bool bInEpilogue,bool bInLoss,int32 InFlowers)
{
	Progress=InProgress;RealSeconds=InRealSeconds;bEpilogue=bInEpilogue;bLoss=bInLoss;Flowers=InFlowers;
	if(Scene.IsValid())Scene->SetScene(Progress,RealSeconds,bEpilogue,bLoss,Flowers);
}
void UWaterGlassFrameWidget::ReleaseSlateResources(bool bReleaseChildren)
{ Super::ReleaseSlateResources(bReleaseChildren);Scene.Reset(); }
