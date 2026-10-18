#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Game/AftershockPlayerController.h"
#include "Aftershock/Public/AftershockAudioSubsystem.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/CheckBox.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScaleBox.h"
#include "Components/SizeBox.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "UObject/ConstructorHelpers.h"

UAftershockMenuComboBox::UAftershockMenuComboBox(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	InitForegroundColor(FSlateColor(FLinearColor(.94f,.94f,.89f)));
}

UAftershockOpeningWidget::UAftershockOpeningWidget(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	SetIsFocusable(true);
	ConstructorHelpers::FObjectFinder<UTexture2D> Artwork(TEXT("/Game/UI/Opening/T_AftershockOpening.T_AftershockOpening"));
	if (Artwork.Succeeded()) OpeningArtwork = Artwork.Object;
}

TSharedRef<SWidget> UAftershockOpeningWidget::RebuildWidget()
{
	// Build on the guaranteed Slate creation path. Initialize callbacks can be
	// skipped when the owning local-player context is not ready during startup.
	// Super takes RootWidget immediately; NativeConstruct would be too late.
	if (!WidgetTree || !WidgetTree->RootWidget) BuildOpeningLayout();
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_OPENING_LAYOUT_READY root=%s artworkLoaded=%d newGameButton=%d focusable=%d"),
		WidgetTree && WidgetTree->RootWidget ? *WidgetTree->RootWidget->GetClass()->GetName() : TEXT("None"),
		OpeningArtwork != nullptr, NewGameButton != nullptr, IsFocusable());
	return Super::RebuildWidget();
}

void UAftershockOpeningWidget::NativeConstruct()
{
	Super::NativeConstruct();
	RefreshSessionActions();
}

void UAftershockOpeningWidget::BuildOpeningLayout()
{
	if (!WidgetTree) WidgetTree = NewObject<UWidgetTree>(this, TEXT("OpeningWidgetTree"), RF_Transient);
	UOverlay* Root = WidgetTree->ConstructWidget<UOverlay>(); WidgetTree->RootWidget = Root;
	UBorder* Backdrop = WidgetTree->ConstructWidget<UBorder>(); Backdrop->SetBrushColor(FLinearColor(.006f, .014f, .024f));
	UOverlaySlot* BackdropLayout = Root->AddChildToOverlay(Backdrop); BackdropLayout->SetHorizontalAlignment(HAlign_Fill); BackdropLayout->SetVerticalAlignment(VAlign_Fill);
	UScaleBox* ArtFit = WidgetTree->ConstructWidget<UScaleBox>(); ArtFit->SetStretch(EStretch::ScaleToFit);
	UOverlaySlot* ArtFitLayout = Root->AddChildToOverlay(ArtFit); ArtFitLayout->SetHorizontalAlignment(HAlign_Fill); ArtFitLayout->SetVerticalAlignment(VAlign_Fill);
	USizeBox* ArtSize = WidgetTree->ConstructWidget<USizeBox>(); ArtSize->SetWidthOverride(1280); ArtSize->SetHeightOverride(720); ArtFit->SetContent(ArtSize);
	UCanvasPanel* Design = WidgetTree->ConstructWidget<UCanvasPanel>(); ArtSize->SetContent(Design);
	UImage* Artwork = WidgetTree->ConstructWidget<UImage>(); Artwork->SetBrushFromTexture(OpeningArtwork, true); Artwork->SetVisibility(ESlateVisibility::HitTestInvisible);
	UCanvasPanelSlot* ArtworkLayout = Design->AddChildToCanvas(Artwork); ArtworkLayout->SetAnchors(FAnchors(0,0,1,1)); ArtworkLayout->SetOffsets(FMargin(0));
	const FLinearColor Cream(.94f, .94f, .89f), Muted(.61f, .71f, .74f), Gold(.96f, .62f, .25f);
	const FLinearColor Surface(.014f, .028f, .040f, .985f), Line(.11f, .19f, .22f);
	const auto Text = [this](const FText& Content, int32 Size, FLinearColor Color)
	{
		UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(); Label->SetText(Content); Label->SetColorAndOpacity(FSlateColor(Color));
		FSlateFontInfo Font = Label->GetFont(); Font.Size = Size; if (Size >= 14) Font.TypefaceFontName = TEXT("Bold");
		Label->SetFont(Font); Label->SetVisibility(ESlateVisibility::HitTestInvisible); return Label;
	};
	UTextBlock* Overline = Text(NSLOCTEXT("Aftershock", "OpeningOverline", "PLANLA  /  KORU  /  YENİDEN KUR"), 10, Gold);
	UCanvasPanelSlot* OverlineLayout = Design->AddChildToCanvas(Overline); OverlineLayout->SetPosition(FVector2D(60, 238)); OverlineLayout->SetSize(FVector2D(330, 24));
	UTextBlock* Tagline = Text(NSLOCTEXT("Aftershock", "OpeningTagline", "Bir şehir. Binlerce hayat.\nHer kararın bir sonucu var."), 18, Cream);
	UCanvasPanelSlot* TaglineLayout = Design->AddChildToCanvas(Tagline); TaglineLayout->SetPosition(FVector2D(60, 270)); TaglineLayout->SetSize(FVector2D(330, 66));
	UVerticalBox* Menu = WidgetTree->ConstructWidget<UVerticalBox>();
	UCanvasPanelSlot* MenuLayout = Design->AddChildToCanvas(Menu); MenuLayout->SetPosition(FVector2D(60, 350)); MenuLayout->SetSize(FVector2D(300, 326));
	const auto Button = [this, &Text, Cream, Gold, Line](TObjectPtr<UButton>& Target, const FText& Label, bool bPrimary)
	{
		Target = WidgetTree->ConstructWidget<UButton>();
		FButtonStyle Style = Target->GetStyle();
		Style.SetNormal(FSlateRoundedBoxBrush(bPrimary ? FLinearColor(.24f,.115f,.038f,.98f) : FLinearColor(.018f,.038f,.052f,.96f), 5.f, bPrimary ? Gold * .65f : Line, 1.f));
		Style.SetHovered(FSlateRoundedBoxBrush(FLinearColor(.12f,.19f,.21f,.99f), 5.f, Gold, 1.f));
		Style.SetPressed(FSlateRoundedBoxBrush(FLinearColor(.034f,.067f,.079f,.99f), 5.f, Gold, 1.f));
		Style.SetDisabled(FSlateRoundedBoxBrush(FLinearColor(.018f,.030f,.039f,.9f), 5.f, Line, 1.f));
		Style.SetNormalPadding(FMargin(20,13)); Style.SetPressedPadding(FMargin(20,14,20,12)); Target->SetStyle(Style);
		Target->SetBackgroundColor(FLinearColor::White); Target->SetColorAndOpacity(FLinearColor::White);
		Target->SetContent(Text(Label, 14, Cream));
	};
	Button(NewGameButton, NSLOCTEXT("Aftershock", "OpeningNewGame", "YENİ OYUN     →"), true);
	Button(ContinueButton, NSLOCTEXT("Aftershock", "OpeningContinue", "DEVAM ET"), false);
	Button(HowToPlayButton, NSLOCTEXT("Aftershock", "OpeningHowToPlay", "NASIL OYNANIR?"), false);
	Button(SettingsButton, NSLOCTEXT("Aftershock", "OpeningSettings", "AYARLAR"), false);
	Button(QuitButton, NSLOCTEXT("Aftershock", "OpeningQuit", "OYUNDAN ÇIK"), false);
	for (UButton* Action : { NewGameButton.Get(), ContinueButton.Get(), HowToPlayButton.Get(), SettingsButton.Get(), QuitButton.Get() })
	{
		UVerticalBoxSlot* ActionLayout = Menu->AddChildToVerticalBox(Action); ActionLayout->SetPadding(FMargin(0,0,0,8));
	}
	NewGameButton->OnClicked.AddUniqueDynamic(this, &UAftershockOpeningWidget::HandleNewGame);
	ContinueButton->OnClicked.AddUniqueDynamic(this, &UAftershockOpeningWidget::HandleContinue);
	HowToPlayButton->OnClicked.AddUniqueDynamic(this, &UAftershockOpeningWidget::HandleHowToPlay);
	SettingsButton->OnClicked.AddUniqueDynamic(this, &UAftershockOpeningWidget::HandleSettings);
	QuitButton->OnClicked.AddUniqueDynamic(this, &UAftershockOpeningWidget::HandleQuit);
	UTextBlock* Footer = Text(NSLOCTEXT("Aftershock", "OpeningFooter", "CIVIL GAME JAM 2026  /  ŞEHRİNİ YÖNET. AFETE DAYAN."), 9, Muted);
	UCanvasPanelSlot* FooterLayout = Design->AddChildToCanvas(Footer); FooterLayout->SetPosition(FVector2D(60,692)); FooterLayout->SetSize(FVector2D(540,20));

	HelpCard = WidgetTree->ConstructWidget<UBorder>(); HelpCard->SetBrush(FSlateRoundedBoxBrush(Surface, 8.f, Line, 1.f)); HelpCard->SetPadding(FMargin(28));
	UCanvasPanelSlot* HelpLayout = Design->AddChildToCanvas(HelpCard); HelpLayout->SetPosition(FVector2D(410,206)); HelpLayout->SetSize(FVector2D(790,454)); HelpLayout->SetZOrder(20);
	UVerticalBox* HelpStack = WidgetTree->ConstructWidget<UVerticalBox>(); HelpCard->SetContent(HelpStack);
	HelpStack->AddChildToVerticalBox(Text(NSLOCTEXT("Aftershock", "OpeningHelpTitle", "BİR ŞEHRİ AFETE HAZIRLA"), 20, Gold));
	UTextBlock* Help = Text(NSLOCTEXT("Aftershock", "OpeningHelpCityLife", "Görev kartını izle. Yapıları incele, altyapıyı geliştir ve afet gelmeden önlem al.\n\nVarsayılan tuşlar (Ayarlar → Kontroller ile özelleştir):\nWASD: kamera • Q/E: döndür • Tekerlek: yakınlaş • G: dünya / şehir\n\nYol: ilk tık başlangıç, ikinci tık inşa. Sağ tık veya Esc: aracı iptal.\n\nYapı yerleştirirken R: dönüş, F: temel, PageUp/Down: kat sayısı.\n\nTuru bitir: vergi, bakım, üretim ve ihtiyaçlar işlenir. Personel ve yol olmadan işletmeler çalışmaz; enerji kesilirse su ve gıda zinciri etkilenir. Duygu simgeleri vatandaşın gerçek ihtiyacını gösterir.\n\nKonut ve iş vergileri %0–30 arasında ayarlanır. Ulaşım için bağlı terminal ve iki durak kur. Zemin etüdü parselin senaryo zemin sınıfını gösterir.\n\nTsunami uygun kıyılarda, volkan yalnız kaynağı haritalanmış bölgelerde açılır. Baraj sonlu depoludur. Nehir önce dolar, ardından taşar. Rüzgâr türbini elektrik üretir; fırtınayı durdurmaz.\n\nCoğrafya kaynakları: Natural Earth — kamu malı (naturalearthdata.com). Contains modified Copernicus DEM data (2021), supplied by Open-Meteo. DOI:10.5270/ESA-c5d3d65. API verisi CC BY 4.0: creativecommons.org/licenses/by/4.0/ — open-meteo.com. Şehirler sıkıştırılmış senaryo yerleşimleridir; gerçek afet tahmini değildir."), 12, Cream);
	Help->SetAutoWrapText(true); Help->SetWrapTextAt(712);
	USizeBox* HelpViewport=WidgetTree->ConstructWidget<USizeBox>(); HelpViewport->SetHeightOverride(276);
	UScrollBox* HelpScroll=WidgetTree->ConstructWidget<UScrollBox>(); HelpScroll->AddChild(Help); HelpViewport->SetContent(HelpScroll);
	UVerticalBoxSlot* HelpTextLayout = HelpStack->AddChildToVerticalBox(HelpViewport); HelpTextLayout->SetPadding(FMargin(0,12,0,12));
	Button(CloseHelpButton, NSLOCTEXT("Aftershock", "OpeningHelpClose", "ANLADIM — MENÜYE DÖN"), true); HelpStack->AddChildToVerticalBox(CloseHelpButton);
	CloseHelpButton->OnClicked.AddUniqueDynamic(this, &UAftershockOpeningWidget::HandleCloseHowToPlay); HelpCard->SetVisibility(ESlateVisibility::Collapsed);
	NewGameCard = WidgetTree->ConstructWidget<UBorder>();
	NewGameCard->SetBrush(FSlateRoundedBoxBrush(Surface, 8.f, Line, 1.f)); NewGameCard->SetPadding(FMargin(24));
	UCanvasPanelSlot* NewLayout = Design->AddChildToCanvas(NewGameCard);
	NewLayout->SetPosition(FVector2D(400,36)); NewLayout->SetSize(FVector2D(820,648)); NewLayout->SetZOrder(30);
	UVerticalBox* NewFrame = WidgetTree->ConstructWidget<UVerticalBox>(); NewGameCard->SetContent(NewFrame);
	UScrollBox* NewScroll = WidgetTree->ConstructWidget<UScrollBox>();
	NewScroll->SetScrollBarVisibility(ESlateVisibility::Visible);
	UVerticalBoxSlot* SettingsViewport = NewFrame->AddChildToVerticalBox(NewScroll); SettingsViewport->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UVerticalBox* Settings = WidgetTree->ConstructWidget<UVerticalBox>(); NewScroll->AddChild(Settings);
	Settings->AddChildToVerticalBox(Text(NSLOCTEXT("Aftershock","OpeningSetupOverline","ŞEHİR KURULUMU  /  OYNANIŞINI SEÇ"),10,Gold));
	UVerticalBoxSlot* SetupTitle = Settings->AddChildToVerticalBox(Text(NSLOCTEXT("Aftershock","OpeningSetupTitle","Şehrin geleceği senin elinde."),24,Cream));
	SetupTitle->SetPadding(FMargin(0,4,0,16));

	// Cards route through the existing combo selection delegate; no second mode state.
	ModeChoice = WidgetTree->ConstructWidget<UComboBoxString>();
	ModeChoice->AddOption(TEXT("Kampanya — 7 hazırlanmış şehir görevi"));
	ModeChoice->AddOption(TEXT("Coğrafi simülasyon — bölgeye uygun ağırlıklı afetler"));
	ModeChoice->AddOption(TEXT("Serbest şehir / Tanrı modu — afetleri sen başlat"));
	Settings->AddChildToVerticalBox(ModeChoice); ModeChoice->SetVisibility(ESlateVisibility::Collapsed);
	UHorizontalBox* ModesRow = WidgetTree->ConstructWidget<UHorizontalBox>(); Settings->AddChildToVerticalBox(ModesRow);
	const TCHAR* ModeOverlines[] = { TEXT("01 / GÖREV ODAKLI"), TEXT("02 / GERÇEK COĞRAFYA"), TEXT("03 / SERBEST YÖNETİM") };
	const TCHAR* ModeTitles[] = { TEXT("Kampanya"), TEXT("Coğrafi simülasyon"), TEXT("Sandbox / God") };
	const TCHAR* ModeCopies[] = { TEXT("Yedi şehir görevi.\nHazırlan, koru, yeniden kur."), TEXT("20 bölge, 25 yerel odak.\nAfetleri yerel kurallar seçer."), TEXT("Şehrini özgürce kur.\nUygun afetleri kendin başlat.") };
	ModeCards.Reset();
	for (int32 ModeIndex=0; ModeIndex<3; ++ModeIndex)
	{
		TObjectPtr<UButton> Card;
		Button(Card,FText::GetEmpty(),false);
		UVerticalBox* CardContent=WidgetTree->ConstructWidget<UVerticalBox>(); Card->SetContent(CardContent);
		if(UButtonSlot* ContentSlot=Cast<UButtonSlot>(CardContent->Slot)) { ContentSlot->SetHorizontalAlignment(HAlign_Left); ContentSlot->SetVerticalAlignment(VAlign_Center); }
		CardContent->AddChildToVerticalBox(Text(FText::FromString(ModeOverlines[ModeIndex]),9,Gold));
		UVerticalBoxSlot* TitleSlot=CardContent->AddChildToVerticalBox(Text(FText::FromString(ModeTitles[ModeIndex]),14,Cream)); TitleSlot->SetPadding(FMargin(0,7,0,6));
		UTextBlock* Copy=Text(FText::FromString(ModeCopies[ModeIndex]),10,Muted); Copy->SetAutoWrapText(true); Copy->SetWrapTextAt(208);
		CardContent->AddChildToVerticalBox(Copy);
		USizeBox* CardSize=WidgetTree->ConstructWidget<USizeBox>(); CardSize->SetHeightOverride(118); CardSize->SetContent(Card);
		UHorizontalBoxSlot* CardSlot=ModesRow->AddChildToHorizontalBox(CardSize); CardSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); CardSlot->SetPadding(FMargin(ModeIndex==0?0:8,0,0,0));
		Card->SetToolTipText(FText::FromString(ModeCopies[ModeIndex])); ModeCards.Add(Card);
	}
	ModeCards[0]->OnClicked.AddUniqueDynamic(this,&UAftershockOpeningWidget::HandleCampaignCard);
	ModeCards[1]->OnClicked.AddUniqueDynamic(this,&UAftershockOpeningWidget::HandleGeographyCard);
	ModeCards[2]->OnClicked.AddUniqueDynamic(this,&UAftershockOpeningWidget::HandleSandboxCard);
	ModeDescription=Text(FText::GetEmpty(),11,Cream); ModeDescription->SetAutoWrapText(true); ModeDescription->SetWrapTextAt(742);
	UVerticalBoxSlot* ModeDescriptionSlot=Settings->AddChildToVerticalBox(ModeDescription); ModeDescriptionSlot->SetPadding(FMargin(0,12,0,6));

	const auto Choice=[&](UVerticalBox* Parent,const TCHAR* Label,TObjectPtr<UComboBoxString>& Field)
	{
		auto* Heading=Parent->AddChildToVerticalBox(Text(FText::FromString(Label),10,Muted)); Heading->SetPadding(FMargin(0,8,0,6));
		Field=WidgetTree->ConstructWidget<UAftershockMenuComboBox>();
		// Explicit UTextBlock foreground applies to both selection and popup rows;
		// the native subclass initializes its arrow foreground before Slate construction.
		Field->OnGenerateWidgetEvent.BindDynamic(this,&UAftershockOpeningWidget::GenerateChoiceWidget);
		const FSlateRoundedBoxBrush ChoiceSurface(FLinearColor(.026f,.054f,.068f),4.f,Line,1.f);
		FComboBoxStyle ChoiceStyle=Field->GetWidgetStyle();
		ChoiceStyle.ComboButtonStyle.SetMenuBorderBrush(FSlateRoundedBoxBrush(Surface,5.f,Line,1.f));
		ChoiceStyle.ComboButtonStyle.SetMenuBorderPadding(FMargin(4));
		ChoiceStyle.ComboButtonStyle.DownArrowImage.TintColor=FSlateColor(Cream);
		ChoiceStyle.ComboButtonStyle.ButtonStyle.SetNormal(ChoiceSurface);
		ChoiceStyle.ComboButtonStyle.ButtonStyle.SetHovered(FSlateRoundedBoxBrush(FLinearColor(.055f,.10f,.12f),4.f,Gold,1.f));
		ChoiceStyle.ComboButtonStyle.ButtonStyle.SetPressed(ChoiceSurface);
		ChoiceStyle.ComboButtonStyle.ButtonStyle.SetDisabled(FSlateRoundedBoxBrush(FLinearColor(.021f,.036f,.045f),4.f,Line,1.f));
		ChoiceStyle.ComboButtonStyle.ButtonStyle.SetNormalForeground(FSlateColor(Cream)).SetHoveredForeground(FSlateColor(Cream)).SetPressedForeground(FSlateColor(Cream)).SetDisabledForeground(FSlateColor(Muted));
		Field->SetWidgetStyle(ChoiceStyle);
		FTableRowStyle RowStyle=Field->GetItemStyle();
		RowStyle.SetTextColor(FSlateColor(Cream)).SetSelectedTextColor(FSlateColor(Cream));
		RowStyle.SetEvenRowBackgroundBrush(FSlateRoundedBoxBrush(Surface,3.f)).SetOddRowBackgroundBrush(FSlateRoundedBoxBrush(Surface,3.f));
		RowStyle.SetEvenRowBackgroundHoveredBrush(FSlateRoundedBoxBrush(FLinearColor(.075f,.13f,.15f),3.f));
		RowStyle.SetOddRowBackgroundHoveredBrush(FSlateRoundedBoxBrush(FLinearColor(.075f,.13f,.15f),3.f));
		RowStyle.SetActiveBrush(FSlateRoundedBoxBrush(FLinearColor(.19f,.11f,.045f),3.f,Gold,1.f));
		RowStyle.SetInactiveBrush(FSlateRoundedBoxBrush(FLinearColor(.07f,.11f,.13f),3.f));
		Field->SetItemStyle(RowStyle); Field->SetMaxListHeight(256.f); Field->SetContentPadding(FMargin(10,6));
		USizeBox* ChoiceSize=WidgetTree->ConstructWidget<USizeBox>(); ChoiceSize->SetMinDesiredHeight(40); ChoiceSize->SetContent(Field); Parent->AddChildToVerticalBox(ChoiceSize);
	};
	const auto FormColumns=[&](UVerticalBox*& Left,UVerticalBox*& Right)
	{
		UHorizontalBox* Row=WidgetTree->ConstructWidget<UHorizontalBox>(); Settings->AddChildToVerticalBox(Row);
		Left=WidgetTree->ConstructWidget<UVerticalBox>(); Right=WidgetTree->ConstructWidget<UVerticalBox>();
		UHorizontalBoxSlot* L=Row->AddChildToHorizontalBox(Left); L->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); L->SetPadding(FMargin(0,0,8,0));
		UHorizontalBoxSlot* R=Row->AddChildToHorizontalBox(Right); R->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); R->SetPadding(FMargin(8,0,0,0));
	};
	UVerticalBox* RegionColumn; UVerticalBox* FocusColumn; FormColumns(RegionColumn,FocusColumn);
	Choice(RegionColumn,TEXT("BÖLGE / 20 GERÇEK COĞRAFYA"),RegionChoice);
	for(int32 R=0;R<GetAftershockGeographyCount();++R)RegionChoice->AddOption(GetAftershockGeography(R).Label);
	Choice(FocusColumn,TEXT("YEREL ODAK"),FocusChoice);
	UVerticalBox* SeedColumn; UVerticalBox* DifficultyColumn; FormColumns(SeedColumn,DifficultyColumn);
	Choice(DifficultyColumn,TEXT("ZORLUK"),DifficultyChoice);
	DifficultyChoice->AddOption(TEXT("Rahat — 0.75"));DifficultyChoice->AddOption(TEXT("Standart — 1.00"));DifficultyChoice->AddOption(TEXT("Zorlu — 1.50"));DifficultyChoice->SetSelectedIndex(1);
	UVerticalBoxSlot* SeedHeading=SeedColumn->AddChildToVerticalBox(Text(NSLOCTEXT("Aftershock","OpeningSeedHeading","DÜNYA TOHUMU / TAM SAYI"),10,Muted)); SeedHeading->SetPadding(FMargin(0,8,0,6));
	SeedEntry=WidgetTree->ConstructWidget<UEditableTextBox>(); SeedEntry->SetText(FText::FromString(TEXT("1337"))); SeedEntry->SetHintText(FText::FromString(TEXT("Örn. 1337"))); SeedEntry->SetSelectAllTextWhenFocused(true);
	FEditableTextBoxStyle SeedStyle=SeedEntry->GetWidgetStyle();
	SeedStyle.SetBackgroundImageNormal(FSlateRoundedBoxBrush(FLinearColor(.026f,.054f,.068f),4.f,Line,1.f));
	SeedStyle.SetBackgroundImageHovered(FSlateRoundedBoxBrush(FLinearColor(.055f,.10f,.12f),4.f,Gold,1.f));
	SeedStyle.SetBackgroundImageFocused(FSlateRoundedBoxBrush(FLinearColor(.035f,.07f,.085f),4.f,Gold,1.f));
	SeedStyle.SetBackgroundImageReadOnly(FSlateRoundedBoxBrush(FLinearColor(.021f,.036f,.045f),4.f,Line,1.f));
	SeedStyle.SetForegroundColor(FSlateColor(Cream)).SetFocusedForegroundColor(FSlateColor(Cream)).SetPadding(FMargin(10,8));
	SeedEntry->SetWidgetStyle(SeedStyle); SeedEntry->SetForegroundColor(Cream);
	USizeBox* SeedSize=WidgetTree->ConstructWidget<USizeBox>(); SeedSize->SetMinDesiredHeight(40); SeedSize->SetContent(SeedEntry); SeedColumn->AddChildToVerticalBox(SeedSize);
	UnlimitedBudget=WidgetTree->ConstructWidget<UCheckBox>(); UnlimitedBudget->SetContent(Text(NSLOCTEXT("Aftershock","OpeningUnlimitedBudget","Sınırsız yatırım bütçesi  ·  yalnız Sandbox / God"),11,Cream));
	UVerticalBoxSlot* BudgetSlot=Settings->AddChildToVerticalBox(UnlimitedBudget); BudgetSlot->SetPadding(FMargin(0,14,0,10));
	RegionDescription=Text(FText::GetEmpty(),10,Muted); RegionDescription->SetAutoWrapText(true); RegionDescription->SetWrapTextAt(742); Settings->AddChildToVerticalBox(RegionDescription);
	SessionStatus=Text(FText::GetEmpty(),11,FLinearColor(1,.58f,.38f)); SessionStatus->SetAutoWrapText(true); SessionStatus->SetWrapTextAt(742);
	UVerticalBoxSlot* StatusSlot=NewFrame->AddChildToVerticalBox(SessionStatus); StatusSlot->SetPadding(FMargin(0,8,0,8)); SessionStatus->SetVisibility(ESlateVisibility::Collapsed);
	UHorizontalBox* SetupActions=WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBoxSlot* ActionsSlot=NewFrame->AddChildToVerticalBox(SetupActions); ActionsSlot->SetPadding(FMargin(0,14,0,0));
	Button(ConfirmNewGame,NSLOCTEXT("Aftershock","OpeningConfirmSetup","ŞEHRİ BAŞLAT  →"),true);
	Button(CancelNewGame,NSLOCTEXT("Aftershock","OpeningCancelSetup","GERİ  [Esc]"),false);
	UHorizontalBoxSlot* ConfirmSlot=SetupActions->AddChildToHorizontalBox(ConfirmNewGame); ConfirmSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UHorizontalBoxSlot* CancelSlot=SetupActions->AddChildToHorizontalBox(CancelNewGame); CancelSlot->SetPadding(FMargin(12,0,0,0));
	ModeChoice->OnSelectionChanged.AddUniqueDynamic(this,&UAftershockOpeningWidget::HandleModeSelected);
	RegionChoice->OnSelectionChanged.AddUniqueDynamic(this,&UAftershockOpeningWidget::HandleRegionSelected);
	FocusChoice->OnSelectionChanged.AddUniqueDynamic(this,&UAftershockOpeningWidget::HandleFocusSelected);
	ConfirmNewGame->OnClicked.AddUniqueDynamic(this,&UAftershockOpeningWidget::HandleConfirmNewGame);CancelNewGame->OnClicked.AddUniqueDynamic(this,&UAftershockOpeningWidget::HandleCancelNewGame);
	RegionChoice->SetSelectedIndex(0); ModeChoice->SetSelectedIndex(0); RefreshModePresentation(); RefreshRegionPresentation(); NewGameCard->SetVisibility(ESlateVisibility::Collapsed);
	if (!OpeningArtwork) UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK_OPENING_ARTWORK_MISSING expected=/Game/UI/Opening/T_AftershockOpening"));
}

void UAftershockOpeningWidget::RefreshSessionActions()
{
	const UGameInstance* Instance = GetGameInstance();
	const UAftershockOpeningSessionSubsystem* Session = Instance ? Instance->GetSubsystem<UAftershockOpeningSessionSubsystem>() : nullptr;
	const auto* Saves=Instance?Instance->GetSubsystem<UAftershockSaveSubsystem>():nullptr;
	const bool bCanContinue = Session && ((Session->HasLiveSession()&&!(Saves&&Saves->IsWorldLoadFailed(GetWorld())))||(Saves&&Saves->HasContinueSave()));
	if (ContinueButton)
	{
		ContinueButton->SetIsEnabled(bCanContinue);
		ContinueButton->SetToolTipText(bCanContinue ? FText::FromString(TEXT("Açık şehre dön veya diskteki en yeni sağlam kaydı yükle.")):FText::FromString(TEXT("Geçerli kayıt yok. Yeni Oyun ile bir şehir başlat.")));
	}
	if (NewGameButton) NewGameButton->SetToolTipText(FText::FromString(TEXT("Oyun modu, bölge, yerel odak, tohum ve zorluk seç.")));
}

void UAftershockOpeningWidget::FocusInitialAction()
{
	if (HelpCard) HelpCard->SetVisibility(ESlateVisibility::Collapsed);
	if (NewGameCard) NewGameCard->SetVisibility(ESlateVisibility::Collapsed);
	if (NewGameButton) NewGameButton->SetKeyboardFocus();
}

void UAftershockOpeningWidget::PlayClickFeedback()
{
	if (UGameInstance* Instance = GetGameInstance())
		if (UAftershockAudioSubsystem* Audio = Instance->GetSubsystem<UAftershockAudioSubsystem>()) Audio->PlayUIClick();
}

void UAftershockOpeningWidget::HandleNewGame()
{
	PlayClickFeedback();
	if (HelpCard) HelpCard->SetVisibility(ESlateVisibility::Collapsed);
	if (NewGameCard) NewGameCard->SetVisibility(ESlateVisibility::Visible);
	RefreshModePresentation(); RefreshRegionPresentation();
	const int32 Mode=ModeChoice?ModeChoice->GetSelectedIndex():0;
	if (ModeCards.IsValidIndex(Mode)&&ModeCards[Mode]) ModeCards[Mode]->SetKeyboardFocus();
}
void UAftershockOpeningWidget::ProposeGeographicNewGame(int32 RegionIndex,int32 FocusIndex)
{
	if(!TryGetAftershockGeographyFocus(RegionIndex,FocusIndex)||!IsAftershockFocusGeodataReady(RegionIndex,FocusIndex))
	{ShowSessionError(NSLOCTEXT("Aftershock","UnavailableGlobeRegion","Bu yerin çevrimdışı harita verileri kullanılamıyor. Başka bir bölge seçin."));return;}
	HandleNewGame();HandleGeographyCard();
	if(RegionChoice)RegionChoice->SetSelectedIndex(RegionIndex);
	if(FocusChoice)FocusChoice->SetSelectedIndex(FocusIndex);
	RefreshRegionPresentation();
	// No ConfigureNewSession, OpenLevel, reset or save write here. The ordinary
	// New Game confirmation remains the only transition boundary.
}
void UAftershockOpeningWidget::HandleContinue() { PlayClickFeedback(); if (AAftershockPlayerController* PC = Cast<AAftershockPlayerController>(GetOwningPlayer())) PC->ContinueFromOpening(); }
void UAftershockOpeningWidget::HandleHowToPlay() { PlayClickFeedback(); if(NewGameCard)NewGameCard->SetVisibility(ESlateVisibility::Collapsed); if (HelpCard) HelpCard->SetVisibility(ESlateVisibility::Visible); if (CloseHelpButton) CloseHelpButton->SetKeyboardFocus(); }
void UAftershockOpeningWidget::HandleSettings() { PlayClickFeedback(); if(auto* PC=Cast<AAftershockPlayerController>(GetOwningPlayer()))PC->ShowSettingsMenu(); }
void UAftershockOpeningWidget::HandleCloseHowToPlay() { PlayClickFeedback(); if (HelpCard) HelpCard->SetVisibility(ESlateVisibility::Collapsed); if (HowToPlayButton) HowToPlayButton->SetKeyboardFocus(); }
void UAftershockOpeningWidget::HandleQuit()
{
	PlayClickFeedback();auto* GI=GetGameInstance();
	if(GI&&GI->GetSubsystem<UAftershockOpeningSessionSubsystem>()->HasLiveSession()&&!GI->GetSubsystem<UAftershockSaveSubsystem>()->IsWorldLoadFailed(GetWorld()))
	{FText Error;if(!GI->GetSubsystem<UAftershockSaveSubsystem>()->SaveAndExit(GetWorld(),Error))ShowSessionError(Error);}
	else UKismetSystemLibrary::QuitGame(this,GetOwningPlayer(),EQuitPreference::Quit,false);
}

FReply UAftershockOpeningWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		if (NewGameCard&&NewGameCard->IsVisible())HandleCancelNewGame();
		else if (HelpCard && HelpCard->IsVisible()) HandleCloseHowToPlay();
		else if (ContinueButton && ContinueButton->GetIsEnabled()) HandleContinue();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UAftershockOpeningWidget::HandleModeSelected(FString Item,ESelectInfo::Type Selection)
{
	const int32 Mode=ModeChoice?ModeChoice->GetSelectedIndex():0;const bool Campaign=Mode==0;
	if(RegionChoice){RegionChoice->SetIsEnabled(!Campaign);if(Campaign)RegionChoice->SetSelectedIndex(0);}
	if(FocusChoice)FocusChoice->SetIsEnabled(!Campaign);
	if(SeedEntry)
	{
		SeedEntry->SetIsEnabled(!Campaign);
		if(Campaign)
		{
			const FAftershockSessionSettings CampaignDefaults;
			SeedEntry->SetText(FText::FromString(FString::FromInt(CampaignDefaults.Seed)));
		}
	}
	if(UnlimitedBudget){UnlimitedBudget->SetIsEnabled(Mode==2);if(Mode!=2)UnlimitedBudget->SetIsChecked(false);}
	RefreshModePresentation(); RefreshRegionPresentation();
}
void UAftershockOpeningWidget::HandleRegionSelected(FString Item,ESelectInfo::Type Selection)
{
	if(!FocusChoice||!RegionChoice)return;FocusChoice->ClearOptions();const int32 R=RegionChoice->GetSelectedIndex();
	for(int32 F=0;F<GetAftershockGeographyFocusCount(R);++F)FocusChoice->AddOption(GetAftershockGeographyFocus(R,F).FocusLabel);
	if(FocusChoice->GetOptionCount()>0)FocusChoice->SetSelectedIndex(GetAftershockDefaultNewGameFocusIndex(R));
	RefreshRegionPresentation();
}
void UAftershockOpeningWidget::HandleFocusSelected(FString Item,ESelectInfo::Type Selection)
{
	RefreshRegionPresentation();
}

UWidget* UAftershockOpeningWidget::GenerateChoiceWidget(FString Item)
{
	if (!WidgetTree) return nullptr;
	UTextBlock* Label=WidgetTree->ConstructWidget<UTextBlock>();
	Label->SetText(FText::FromString(Item)); Label->SetColorAndOpacity(FSlateColor(FLinearColor(.94f,.94f,.89f)));
	FSlateFontInfo Font=Label->GetFont(); Font.Size=12; Label->SetFont(Font);
	Label->SetAutoWrapText(true); Label->SetWrapTextAt(330.f); Label->SetVisibility(ESlateVisibility::HitTestInvisible);
	UBorder* ChoiceContainer=WidgetTree->ConstructWidget<UBorder>(); ChoiceContainer->SetBrushColor(FLinearColor::Transparent);
	ChoiceContainer->SetPadding(FMargin(4,4)); ChoiceContainer->SetContent(Label); ChoiceContainer->SetVisibility(ESlateVisibility::HitTestInvisible);
	return ChoiceContainer;
}

void UAftershockOpeningWidget::HandleCampaignCard() { PlayClickFeedback(); if(ModeChoice)ModeChoice->SetSelectedIndex(0); }
void UAftershockOpeningWidget::HandleGeographyCard() { PlayClickFeedback(); if(ModeChoice)ModeChoice->SetSelectedIndex(1); }
void UAftershockOpeningWidget::HandleSandboxCard() { PlayClickFeedback(); if(ModeChoice)ModeChoice->SetSelectedIndex(2); }

void UAftershockOpeningWidget::RefreshModePresentation()
{
	const int32 Selected=ModeChoice?ModeChoice->GetSelectedIndex():INDEX_NONE;
	for (int32 I=0;I<ModeCards.Num();++I)
	{
		UButton* Card=ModeCards[I]; if(!Card)continue;
		const bool bSelected=I==Selected;
		const FLinearColor Accent(.96f,.62f,.25f), Stroke(.11f,.19f,.22f);
		FButtonStyle Style=Card->GetStyle();
		Style.SetNormal(FSlateRoundedBoxBrush(bSelected?FLinearColor(.16f,.095f,.04f):FLinearColor(.022f,.045f,.058f),5.f,bSelected?Accent:Stroke,bSelected?2.f:1.f));
		Style.SetHovered(FSlateRoundedBoxBrush(FLinearColor(.07f,.12f,.14f),5.f,Accent,2.f));
		Style.SetPressed(FSlateRoundedBoxBrush(FLinearColor(.035f,.065f,.075f),5.f,Accent,2.f));
		Style.SetNormalPadding(FMargin(12)); Style.SetPressedPadding(FMargin(12,13,12,11)); Card->SetStyle(Style);
	}
	if (ModeDescription)
	{
		static const TCHAR* Descriptions[]={
			TEXT("KAMPANYA SEÇİLİ  ·  Atakent'ten başlayan yedi görev. Bölge ve tohum görevle belirlenir; hazırlık, afet ve iyileşme hedeflerini tamamla."),
			TEXT("COĞRAFİ SİMÜLASYON SEÇİLİ  ·  Bölge ve yerel odağını seç. Afet zamanını ve türünü coğrafyaya uygun oyun kuralları belirler."),
			TEXT("SANDBOX / GOD SEÇİLİ  ·  Bölge ve tohum sende. Afetleri kendin başlat; istersen sınırsız yatırım aç. Coğrafi uygunluk kuralları korunur.")};
		ModeDescription->SetText(FText::FromString(Selected>=0&&Selected<3?Descriptions[Selected]:TEXT("Bir oyun modu seç.")));
	}
}

void UAftershockOpeningWidget::RefreshRegionPresentation()
{
	if (!RegionDescription) return;
	const int32 R=RegionChoice?RegionChoice->GetSelectedIndex():INDEX_NONE;
	const int32 F=FocusChoice?FocusChoice->GetSelectedIndex():INDEX_NONE;
	const auto* Focus=TryGetAftershockGeographyFocus(R,F);
	if (!Focus)
	{
		RegionDescription->SetText(NSLOCTEXT("Aftershock","OpeningNoFocus","Geçerli bir bölge ve yerel odak seç."));
		return;
	}
	const bool bReady=IsAftershockGeographyFocusReady(R,F);
	RegionDescription->SetText(FText::FromString(FString::Printf(
		TEXT("%s  ·  %.3f°%s / %.3f°%s\n%s Şehir sıkıştırılmış oyun ölçeğindedir; afet ağırlıkları gerçek olasılık veya mühendislik tahmini değildir."),
		Focus->FocusLabel,FMath::Abs(Focus->Latitude),Focus->Latitude>=0?TEXT("K"):TEXT("G"),
		FMath::Abs(Focus->Longitude),Focus->Longitude>=0?TEXT("D"):TEXT("B"),
		bReady?TEXT("Çevrimdışı yükselti verisi hazır."):TEXT("Bu odağın yükselti verisi hazır değil; şehir başlatılamaz."))));
}
void UAftershockOpeningWidget::HandleConfirmNewGame()
{
	PlayClickFeedback();
	FAftershockSessionSettings Settings;const int32 Mode=ModeChoice?ModeChoice->GetSelectedIndex():INDEX_NONE;
	if(Mode<0||Mode>2){ShowSessionError(FText::FromString(TEXT("Bir oyun modu seçin.")));return;}
	Settings.Mode=Mode==0?EAftershockPlayMode::Campaign:Mode==1?EAftershockPlayMode::GeographicSimulation:EAftershockPlayMode::Sandbox;
	const int32 R=RegionChoice?RegionChoice->GetSelectedIndex():INDEX_NONE,F=FocusChoice?FocusChoice->GetSelectedIndex():INDEX_NONE;
	const auto* Focus=TryGetAftershockGeographyFocus(R,F);if(!Focus){ShowSessionError(FText::FromString(TEXT("Bölge ve yerel odak seçilmedi.")));return;}
	Settings.RegionID=FName(Focus->Id);Settings.FocusID=FName(Focus->FocusId);
	// Campaign uses the canonical session default, never stale text in its disabled field.
	if(Settings.Mode!=EAftershockPlayMode::Campaign&&(!SeedEntry||!LexTryParseString(Settings.Seed,*SeedEntry->GetText().ToString()))){ShowSessionError(FText::FromString(TEXT("Tohum geçerli bir tam sayı olmalı.")));return;}
	Settings.Difficulty=DifficultyChoice&&DifficultyChoice->GetSelectedIndex()==0?.75f:DifficultyChoice&&DifficultyChoice->GetSelectedIndex()==2?1.5f:1.f;
	Settings.bUnlimitedBudget=Settings.Mode==EAftershockPlayMode::Sandbox&&UnlimitedBudget&&UnlimitedBudget->IsChecked();
	FText Error;
	if(auto* PC=Cast<AAftershockPlayerController>(GetOwningPlayer()))
	{ if(!PC->StartConfiguredGame(Settings,Error))ShowSessionError(Error); }
	else ShowSessionError(NSLOCTEXT("Aftershock","OpeningPlayerUnavailable","Şehir başlatılamadı: oyuncu oturumu hazır değil. Menüye dönüp yeniden dene."));
}
void UAftershockOpeningWidget::HandleCancelNewGame(){PlayClickFeedback();if(NewGameCard)NewGameCard->SetVisibility(ESlateVisibility::Collapsed);if(NewGameButton)NewGameButton->SetKeyboardFocus();}
void UAftershockOpeningWidget::ShowSessionError(FText Error)
{if(NewGameCard)NewGameCard->SetVisibility(ESlateVisibility::Visible);if(SessionStatus){SessionStatus->SetText(Error);SessionStatus->SetColorAndOpacity(FSlateColor(FLinearColor(1,.58f,.38f)));SessionStatus->SetVisibility(ESlateVisibility::Visible);}}
