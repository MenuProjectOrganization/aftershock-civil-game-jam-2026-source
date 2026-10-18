#include "Aftershock/UI/WaterGlassEpilogueWidget.h"
#include "Aftershock/UI/WaterGlassFrameWidget.h"
#include "Aftershock/UI/CasualtyLedgerPanel.h"
#include "Aftershock/Narrative/CasualtyLedgerGenerator.h"
#include "Aftershock/Game/AftershockPlayerController.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ScaleBox.h"
#include "Components/SizeBox.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"

TSharedRef<SWidget> UWaterGlassEpilogueWidget::RebuildWidget(){if(!WidgetTree||!WidgetTree->RootWidget)BuildLayout();return Super::RebuildWidget();}
void UWaterGlassEpilogueWidget::BuildLayout()
{
	if(!WidgetTree)WidgetTree=NewObject<UWidgetTree>(this,TEXT("WidgetTree"));
	UBorder* Background=WidgetTree->ConstructWidget<UBorder>();Background->SetBrushColor(FLinearColor(.008f,.018f,.023f));Background->SetPadding(FMargin(0));WidgetTree->RootWidget=Background;
	UScaleBox* Fit=WidgetTree->ConstructWidget<UScaleBox>();Fit->SetStretch(EStretch::ScaleToFit);Background->SetContent(Fit);
	USizeBox* DesignSize=WidgetTree->ConstructWidget<USizeBox>();DesignSize->SetWidthOverride(1600);DesignSize->SetHeightOverride(900);Fit->SetContent(DesignSize);
	UCanvasPanel* Design=WidgetTree->ConstructWidget<UCanvasPanel>();DesignSize->SetContent(Design);
	Illustration=WidgetTree->ConstructWidget<UWaterGlassFrameWidget>();Illustration->SetVisibility(ESlateVisibility::HitTestInvisible);auto* SceneSlot=Design->AddChildToCanvas(Illustration);SceneSlot->SetAnchors(FAnchors(0,0,1,1));SceneSlot->SetOffsets(FMargin(0));
	UVerticalBox* Stack=WidgetTree->ConstructWidget<UVerticalBox>();auto* StackSlot=Design->AddChildToCanvas(Stack);StackSlot->SetPosition({96,78});StackSlot->SetSize({625,755});
	const auto Text=[&](FText Copy,int32 Size,FLinearColor Color)
	{
		auto* Label=WidgetTree->ConstructWidget<UTextBlock>();Label->SetText(Copy);Label->SetColorAndOpacity(Color);Label->SetAutoWrapText(true);Label->SetWrapTextAt(615);Label->SetVisibility(ESlateVisibility::HitTestInvisible);FSlateFontInfo Font=Label->GetFont();Font.Size=Size;Font.TypefaceFontName=Size>=23?TEXT("Bold"):TEXT("Regular");Label->SetFont(Font);auto* TextSlot=Stack->AddChildToVerticalBox(Label);TextSlot->SetPadding(FMargin(0,0,0,17));return Label;
	};
	Text(NSLOCTEXT("Aftershock","EpilogueOverline","AFTERSHOCK  /  GERİDE KALAN İZLER"),12,FLinearColor(.96f,.66f,.34f));
	TitleText=Text(FText::GetEmpty(),32,FLinearColor(.94f,.93f,.85f));
	ReflectionText=Text(FText::GetEmpty(),23,FLinearColor(.85f,.86f,.78f));
	TotalsText=Text(FText::GetEmpty(),16,FLinearColor(.66f,.79f,.72f));
	ReasonText=Text(FText::GetEmpty(),12,FLinearColor(.67f,.75f,.71f));
	const auto Button=[&](FText Label,FLinearColor Accent)
	{
		auto* Action=WidgetTree->ConstructWidget<UButton>();FButtonStyle Style=Action->GetStyle();Style.SetNormal(FSlateRoundedBoxBrush(FLinearColor(.035f,.075f,.073f),5.f,Accent,1.f));Style.SetHovered(FSlateRoundedBoxBrush(FLinearColor(.09f,.14f,.115f),5.f,FLinearColor(.97f,.73f,.38f),1.f));Style.SetPressed(FSlateRoundedBoxBrush(FLinearColor(.018f,.04f,.04f),5.f));Style.SetNormalPadding(FMargin(16,12));Style.SetPressedPadding(FMargin(16,13,16,11));Action->SetStyle(Style);
		auto* Caption=WidgetTree->ConstructWidget<UTextBlock>();Caption->SetText(Label);Caption->SetColorAndOpacity(FLinearColor(.94f,.88f,.73f));Caption->SetVisibility(ESlateVisibility::HitTestInvisible);Action->SetContent(Caption);auto* ActionSlot=Stack->AddChildToVerticalBox(Action);ActionSlot->SetPadding(FMargin(0,0,0,9));return Action;
	};
	Button(NSLOCTEXT("Aftershock","EpilogueLedger","KAYIP DEFTERİNİ AÇ  /  BİR ÇİÇEK BIRAK"),FLinearColor(.50f,.43f,.27f))->OnClicked.AddUniqueDynamic(this,&UWaterGlassEpilogueWidget::HandleLedger);
	Button(NSLOCTEXT("Aftershock","EpilogueSave","BU ŞEHRİ KAYDET"),FLinearColor(.19f,.35f,.30f))->OnClicked.AddUniqueDynamic(this,&UWaterGlassEpilogueWidget::HandleSave);
	Button(NSLOCTEXT("Aftershock","EpilogueMenu","ANA MENÜYE DÖN"),FLinearColor(.13f,.24f,.24f))->OnClicked.AddUniqueDynamic(this,&UWaterGlassEpilogueWidget::HandleMainMenu);
	FeedbackText=Text(FText::GetEmpty(),11,FLinearColor(.95f,.70f,.40f));
	LedgerOverlay=WidgetTree->ConstructWidget<UBorder>();LedgerOverlay->SetBrush(FSlateRoundedBoxBrush(FLinearColor(.014f,.030f,.036f),7.f,FLinearColor(.20f,.31f,.29f),1.f));LedgerOverlay->SetPadding(FMargin(26));auto* OverlaySlot=Design->AddChildToCanvas(LedgerOverlay);OverlaySlot->SetPosition({76,58});OverlaySlot->SetSize({675,780});OverlaySlot->SetZOrder(10);
	LedgerPanel=CreateWidget<UCasualtyLedgerPanel>(this);if(LedgerPanel){LedgerOverlay->SetContent(LedgerPanel);LedgerPanel->OnCloseRequested.AddUObject(this,&UWaterGlassEpilogueWidget::CloseLedger);}LedgerOverlay->SetVisibility(ESlateVisibility::Collapsed);
}
void UWaterGlassEpilogueWidget::SetOutcome(bool bSuccess,FText Reason){bCampaignSuccess=bSuccess;OutcomeReason=Reason;RefreshEvidence();}
void UWaterGlassEpilogueWidget::NativeConstruct(){Super::NativeConstruct();SetIsFocusable(true);StartedRealSeconds=FPlatformTime::Seconds();NextEvidenceRealSeconds=0.;RefreshEvidence();}
void UWaterGlassEpilogueWidget::NativeTick(const FGeometry& Geometry,float DeltaSeconds)
{
	Super::NativeTick(Geometry,DeltaSeconds);const double Now=FPlatformTime::Seconds();const float Elapsed=static_cast<float>(Now-StartedRealSeconds);SetRenderOpacity(FMath::Clamp(Elapsed/1.2f,0.f,1.f));
	if(Now>=NextEvidenceRealSeconds){NextEvidenceRealSeconds=Now+1.;RefreshEvidence();}
	if(Illustration)Illustration->SetScene(1.f,Elapsed,true,bHasDeaths,DisplayedFlowers);
}
void UWaterGlassEpilogueWidget::RefreshEvidence()
{
	if(!TitleText||!TotalsText)return;
	TitleText->SetText(bCampaignSuccess?NSLOCTEXT("Aftershock","EpilogueCampaignTitle","Yolculuğun bilançosu."):NSLOCTEXT("Aftershock","EpilogueLossTitle","Geride kalanlar."));
	if(ReasonText)ReasonText->SetText(OutcomeReason);
	if(auto* Ledger=GetWorld()?GetWorld()->GetSubsystem<UCasualtyLedgerGenerator>():nullptr)
	{
		const auto Totals=Ledger->GetCampaignTotals();DisplayedDeaths=Totals.Deaths;DisplayedSurvivors=Totals.UniqueExposedSurvivors;bHasDeaths=Totals.Deaths>0;DisplayedFlowers=static_cast<int32>(FMath::Clamp<int64>(Totals.FlowerTributes,0,12));
		TotalsText->SetText(FText::Format(NSLOCTEXT("Aftershock","EpilogueCanonicalTotals","Oturumda afete maruz kalan benzersiz sağ kalan: {0}\nHayatını kaybeden: {1}\nGerçek güçlendirme yatırımı: {2}  ·  Anma çiçeği: {3}\nBu toplama dahil şehir: {4}"),FText::AsNumber(Totals.UniqueExposedSurvivors),FText::AsNumber(Totals.Deaths),FText::AsNumber(Totals.RetrofitInvestments),FText::AsNumber(Totals.FlowerTributes),FText::AsNumber(Totals.Cities)));
		ReflectionText->SetText(bHasDeaths?NSLOCTEXT("Aftershock","EpilogueQuestion","Yukarıdan baktığında sadece bir bütçe kararı mıydı?\n\nAşağıdakiler içinse bir kıyametti."):NSLOCTEXT("Aftershock","EpilogueNoDeaths","Bu kayıtlarda can kaybı yok.\n\nAşağıda hayat devam ediyor."));
		TotalsText->SetToolTipText(NSLOCTEXT("Aftershock","EpilogueEvidenceScope","Sayaçlar şehir ve vatandaş kimliğiyle tekilleştirilmiş gerçek kayıtlardır. Her afetin sağ kalan sayısı tekrar toplanmaz. Güçlendirme adedi, her kaybın nedeninin bütçe olduğu anlamına gelmez. Ayrıntılı isim listesi mevcut şehrin defteridir."));
	}
	else{TotalsText->SetText(NSLOCTEXT("Aftershock","EpilogueLedgerUnavailable","Oturumun gerçek kişi kayıtları şu an okunamıyor. Sayı veya neden üretilmedi."));ReflectionText->SetText(NSLOCTEXT("Aftershock","EpilogueFallbackReflection","Bir şehir, içindeki hayatlar kadar büyüktür."));}
}
void UWaterGlassEpilogueWidget::HandleMainMenu(){if(auto* PC=Cast<AAftershockPlayerController>(GetOwningPlayer()))PC->ShowOpeningMenu();}
void UWaterGlassEpilogueWidget::HandleSave()
{
	FText Error;auto* Saves=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>():nullptr;const bool Saved=Saves&&Saves->SaveNow(GetWorld(),TEXT("Manual"),Error);
	if(FeedbackText)FeedbackText->SetText(Saved?NSLOCTEXT("Aftershock","EpilogueSaved","Şehir ve gerçek kişi kayıtları kaydedildi."):Error.IsEmpty()?NSLOCTEXT("Aftershock","EpilogueSaveUnavailable","Kayıt hizmeti hazır değil; şehir kapatılmadı."):Error);
}
void UWaterGlassEpilogueWidget::HandleLedger(){if(LedgerOverlay&&LedgerPanel){LedgerOverlay->SetVisibility(ESlateVisibility::Visible);LedgerPanel->RefreshLedger();}}
bool UWaterGlassEpilogueWidget::OpenCasualtyLedger(){HandleLedger();return IsCasualtyLedgerVisible();}
bool UWaterGlassEpilogueWidget::IsCasualtyLedgerVisible() const{return LedgerOverlay&&LedgerOverlay->IsVisible()&&LedgerPanel&&LedgerPanel->IsVisible();}
void UWaterGlassEpilogueWidget::CloseLedger(){if(LedgerOverlay)LedgerOverlay->SetVisibility(ESlateVisibility::Collapsed);RefreshEvidence();}
FReply UWaterGlassEpilogueWidget::NativeOnPreviewKeyDown(const FGeometry& Geometry,const FKeyEvent& Key)
{
	if(Key.GetKey()==EKeys::Escape){if(LedgerOverlay&&LedgerOverlay->IsVisible())CloseLedger();else HandleMainMenu();return FReply::Handled();}return Super::NativeOnPreviewKeyDown(Geometry,Key);
}
