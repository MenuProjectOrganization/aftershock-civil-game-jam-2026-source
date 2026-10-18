#include "Aftershock/Public/AftershockSettingsWidget.h"
#include "Aftershock/Game/AftershockPlayerController.h"
#include "Aftershock/Public/AftershockAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/CheckBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScaleBox.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformTime.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetSystemLibrary.h"

namespace
{
	const FLinearColor Cream(.94f,.95f,.91f),Muted(.62f,.73f,.77f),Accent(.96f,.63f,.28f),SettingsBorderColour(.13f,.25f,.29f);
	const TArray<FString> QualityNames={TEXT("Düşük"),TEXT("Orta"),TEXT("Yüksek"),TEXT("Çok yüksek"),TEXT("Sinema (ağır)")};
	constexpr int32 QualityStart=10,QualityGroupCount=10,GraphicsPresetChoice=6;
	const int32 FPSValues[]={60,90,120,0};
}

void UAftershockSettingsButton::Configure(UAftershockSettingsWidget* InOwner,int32 InAction)
{SettingsOwner=InOwner;Action=InAction;OnClicked.AddUniqueDynamic(this,&UAftershockSettingsButton::Invoke);}
void UAftershockSettingsButton::Invoke(){if(SettingsOwner)SettingsOwner->HandleAction(Action);}
UAftershockSettingsChoice::UAftershockSettingsChoice(const FObjectInitializer& Initializer):Super(Initializer)
{
	InitForegroundColor(FSlateColor(Cream));
	FSlateFontInfo ChoiceFont=GetFont();ChoiceFont.Size=13;InitFont(ChoiceFont);
	// The engine's default light combo brush makes the inherited cream text unreadable.
	FComboBoxStyle Style=GetWidgetStyle();auto& Combo=Style.ComboButtonStyle;auto& Button=Combo.ButtonStyle;
	Button.SetNormal(FSlateRoundedBoxBrush(FLinearColor(.020f,.048f,.064f),4.f,SettingsBorderColour,1.f));
	Button.SetHovered(FSlateRoundedBoxBrush(FLinearColor(.065f,.13f,.16f),4.f,Accent,1.f));
	Button.SetPressed(FSlateRoundedBoxBrush(FLinearColor(.09f,.16f,.18f),4.f,Accent,1.f));
	Button.SetDisabled(FSlateRoundedBoxBrush(FLinearColor(.018f,.033f,.043f),4.f,SettingsBorderColour,1.f));
	Button.SetNormalForeground(FSlateColor(Cream));Button.SetHoveredForeground(FSlateColor(Cream));Button.SetPressedForeground(FSlateColor(Cream));
	Combo.DownArrowImage.TintColor=FSlateColor(Cream);
	Combo.SetMenuBorderBrush(FSlateRoundedBoxBrush(FLinearColor(.014f,.030f,.043f),4.f,SettingsBorderColour,1.f));
	Style.SetMenuRowPadding(FMargin(10,7));SetWidgetStyle(Style);SetContentPadding(FMargin(10,6));
	FTableRowStyle Items=GetItemStyle();
	const FSlateRoundedBoxBrush Row(FLinearColor(.020f,.048f,.064f),0.f);
	const FSlateRoundedBoxBrush Hover(FLinearColor(.065f,.13f,.16f),0.f);
	const FSlateRoundedBoxBrush Selected(FLinearColor(.12f,.20f,.23f),0.f);
	Items.SetEvenRowBackgroundBrush(Row).SetOddRowBackgroundBrush(Row);
	Items.SetEvenRowBackgroundHoveredBrush(Hover).SetOddRowBackgroundHoveredBrush(Hover);
	Items.SetActiveBrush(Selected).SetInactiveBrush(Selected).SetActiveHoveredBrush(Hover).SetInactiveHoveredBrush(Hover);
	Items.SetTextColor(FSlateColor(Cream)).SetSelectedTextColor(FSlateColor(Cream));SetItemStyle(Items);
}
void UAftershockSettingsChoice::Configure(UAftershockSettingsWidget* InOwner,int32 InChoiceID)
{
    QualityOwner=InOwner;QualityChoiceID=InChoiceID;
    OnSelectionChanged.AddUniqueDynamic(this,&UAftershockSettingsChoice::Changed);
}
void UAftershockSettingsChoice::Changed(FString Option,ESelectInfo::Type SelectionType)
{
    if(QualityOwner)QualityOwner->HandleGraphicsChoiceChanged(QualityChoiceID);
}

void UAftershockSettingsKeySelector::Configure(UAftershockSettingsWidget* InOwner,int32 InAction)
{
	// Keep key binding controls consistent with the settings combo palette.
	FButtonStyle KeyButtonStyle=GetButtonStyle();
	KeyButtonStyle.SetNormal(FSlateRoundedBoxBrush(FLinearColor(.020f,.048f,.064f),4.f,SettingsBorderColour,1.f));
	KeyButtonStyle.SetHovered(FSlateRoundedBoxBrush(FLinearColor(.065f,.13f,.16f),4.f,Accent,1.f));
	KeyButtonStyle.SetPressed(FSlateRoundedBoxBrush(FLinearColor(.09f,.16f,.18f),4.f,Accent,1.f));
	KeyButtonStyle.SetDisabled(FSlateRoundedBoxBrush(FLinearColor(.018f,.033f,.043f),4.f,SettingsBorderColour,1.f));
	KeyButtonStyle.SetNormalForeground(FSlateColor(Cream));KeyButtonStyle.SetHoveredForeground(FSlateColor(Cream));KeyButtonStyle.SetPressedForeground(FSlateColor(Cream));
	SetButtonStyle(KeyButtonStyle);
	FTextBlockStyle KeyTextStyle=GetTextStyle();KeyTextStyle.SetFontSize(13.f).SetColorAndOpacity(FSlateColor(Cream));
	SetTextStyle(KeyTextStyle);SetMargin(FMargin(10,6));
	SettingsOwner=InOwner;Action=InAction;OnKeySelected.AddUniqueDynamic(this,&UAftershockSettingsKeySelector::Selected);
}
void UAftershockSettingsKeySelector::Selected(FInputChord Chord){if(SettingsOwner)SettingsOwner->SelectKey(Action,Chord);}

UAftershockSettingsWidget::UAftershockSettingsWidget(const FObjectInitializer& Initializer):Super(Initializer){SetIsFocusable(true);}
TSharedRef<SWidget> UAftershockSettingsWidget::RebuildWidget()
{if(!WidgetTree||!WidgetTree->RootWidget)BuildLayout();return Super::RebuildWidget();}

UAftershockSettingsSubsystem* UAftershockSettingsWidget::PreferencesService() const
{return GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSettingsSubsystem>():nullptr;}

UTextBlock* UAftershockSettingsWidget::MakeText(const FText& Value,int32 Size)
{
	auto* Text=WidgetTree->ConstructWidget<UTextBlock>();Text->SetText(Value);Text->SetColorAndOpacity(FSlateColor(Cream));
	FSlateFontInfo Font=Text->GetFont();Font.Size=Size;Text->SetFont(Font);Text->SetAutoWrapText(true);Text->SetVisibility(ESlateVisibility::HitTestInvisible);
	MenuTexts.Add(Text);BaseTextSizes.Add(Size);return Text;
}

UAftershockSettingsButton* UAftershockSettingsWidget::MakeButton(const FText& Label,int32 Action)
{
	auto* Button=WidgetTree->ConstructWidget<UAftershockSettingsButton>();Button->Configure(this,Action);
	FButtonStyle Style=Button->GetStyle();Style.SetNormal(FSlateRoundedBoxBrush(FLinearColor(.025f,.065f,.081f),4.f,SettingsBorderColour,1.f));
	Style.SetHovered(FSlateRoundedBoxBrush(FLinearColor(.10f,.18f,.20f),4.f,Accent,1.f));Style.SetPressed(FSlateRoundedBoxBrush(FLinearColor(.16f,.20f,.21f),4.f,Accent,1.f));
	Style.SetNormalPadding(FMargin(14,10));Style.SetPressedPadding(FMargin(14,11,14,9));Button->SetStyle(Style);
	auto* Text=MakeText(Label,13);Text->SetAutoWrapText(false);Text->SetJustification(ETextJustify::Center);
	if(Action==100||Action==101)Text->SetMinDesiredWidth(104.f);
	Button->SetContent(Text);return Button;
}

UAftershockSettingsChoice* UAftershockSettingsWidget::AddChoice(UVerticalBox* Page,int32 ID,const FText& Label,const TArray<FString>& Options,const FText& Hint)
{
	auto* Row=WidgetTree->ConstructWidget<UHorizontalBox>();auto* RowLayout=Page->AddChildToVerticalBox(Row);RowLayout->SetPadding(FMargin(0,5));
	auto* Caption=MakeText(Label);Caption->SetToolTipText(Hint);auto* LabelLayout=Row->AddChildToHorizontalBox(Caption);LabelLayout->SetSize(FSlateChildSize(ESlateSizeRule::Fill));LabelLayout->SetVerticalAlignment(VAlign_Center);LabelLayout->SetPadding(FMargin(0,0,16,0));
	auto* Choice=WidgetTree->ConstructWidget<UAftershockSettingsChoice>();for(const auto& Option:Options)Choice->AddOption(Option);Choice->SetToolTipText(Hint);
	auto* Box=WidgetTree->ConstructWidget<USizeBox>();Box->SetWidthOverride(300);Box->SetMinDesiredHeight(38);Box->SetContent(Choice);Row->AddChildToHorizontalBox(Box);Choices.Add(ID,Choice);Choice->Configure(this,ID);return Choice;
}

USlider* UAftershockSettingsWidget::AddSlider(UVerticalBox* Page,int32 ID,const FText& Label,float Min,float Max,const FText& Hint)
{
	auto* Row=WidgetTree->ConstructWidget<UHorizontalBox>();Page->AddChildToVerticalBox(Row)->SetPadding(FMargin(0,9));
	auto* Caption=MakeText(Label);Caption->SetToolTipText(Hint);auto* LabelLayout=Row->AddChildToHorizontalBox(Caption);LabelLayout->SetSize(FSlateChildSize(ESlateSizeRule::Fill));LabelLayout->SetVerticalAlignment(VAlign_Center);
	auto* Slider=WidgetTree->ConstructWidget<USlider>();Slider->SetMinValue(Min);Slider->SetMaxValue(Max);Slider->SetStepSize(.05f);Slider->SetToolTipText(Hint);
	auto* Box=WidgetTree->ConstructWidget<USizeBox>();Box->SetWidthOverride(240);Box->SetMinDesiredHeight(36);Box->SetContent(Slider);Row->AddChildToHorizontalBox(Box);
	auto* ValueText=MakeText(FText::GetEmpty(),12);auto* ValueBox=WidgetTree->ConstructWidget<USizeBox>();ValueBox->SetWidthOverride(64);ValueBox->SetContent(ValueText);Row->AddChildToHorizontalBox(ValueBox)->SetVerticalAlignment(VAlign_Center);
	Sliders.Add(ID,Slider);SliderValues.Add(ID,ValueText);return Slider;
}

UCheckBox* UAftershockSettingsWidget::AddToggle(UVerticalBox* Page,int32 ID,const FText& Label,const FText& Hint)
{
	auto* Toggle=WidgetTree->ConstructWidget<UCheckBox>();auto* Caption=MakeText(Label);
	Caption->SetAutoWrapText(false);Caption->SetMargin(FMargin(8,0,0,0));Toggle->SetContent(Caption);Toggle->SetToolTipText(Hint);
	auto* ToggleSlot=Page->AddChildToVerticalBox(Toggle);ToggleSlot->SetHorizontalAlignment(HAlign_Fill);ToggleSlot->SetPadding(FMargin(0,9));Toggles.Add(ID,Toggle);return Toggle;
}

void UAftershockSettingsWidget::BuildLayout()
{
	if(!WidgetTree)WidgetTree=NewObject<UWidgetTree>(this,TEXT("SettingsWidgetTree"),RF_Transient);
	auto* Root=WidgetTree->ConstructWidget<UOverlay>();WidgetTree->RootWidget=Root;
	auto* Shade=WidgetTree->ConstructWidget<UBorder>();Shade->SetBrushColor(FLinearColor(.003f,.008f,.013f,.96f));auto* ShadeSlot=Root->AddChildToOverlay(Shade);ShadeSlot->SetHorizontalAlignment(HAlign_Fill);ShadeSlot->SetVerticalAlignment(VAlign_Fill);
	auto* Fit=WidgetTree->ConstructWidget<UScaleBox>();Fit->SetStretch(EStretch::ScaleToFit);auto* FitSlot=Root->AddChildToOverlay(Fit);FitSlot->SetHorizontalAlignment(HAlign_Fill);FitSlot->SetVerticalAlignment(VAlign_Fill);
	auto* Design=WidgetTree->ConstructWidget<USizeBox>();Design->SetWidthOverride(1120);Design->SetHeightOverride(680);Fit->SetContent(Design);
	auto* Card=WidgetTree->ConstructWidget<UBorder>();SettingsCard=Card;Card->SetPadding(FMargin(30,22));Card->SetBrush(FSlateRoundedBoxBrush(FLinearColor(.014f,.030f,.043f),10.f,SettingsBorderColour,1.f));Design->SetContent(Card);
	auto* Stack=WidgetTree->ConstructWidget<UVerticalBox>();Card->SetContent(Stack);
	Stack->AddChildToVerticalBox(MakeText(NSLOCTEXT("Aftershock","SettingsTitle","AYARLAR  /  ŞEHRİNİN RİTMİNİ BELİRLE"),22))->SetPadding(FMargin(0,0,0,16));
	auto* TabRow=WidgetTree->ConstructWidget<UHorizontalBox>();Stack->AddChildToVerticalBox(TabRow)->SetPadding(FMargin(0,0,0,12));
	const TCHAR* Titles[]={TEXT("GRAFİK"),TEXT("SES"),TEXT("KONTROLLER"),TEXT("OYNANIŞ"),TEXT("ERİŞİLEBİLİRLİK")};
	for(int32 I=0;I<5;++I){auto* Tab=MakeButton(FText::FromString(Titles[I]),I);Tabs.Add(Tab);auto* TabSlot=TabRow->AddChildToHorizontalBox(Tab);TabSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));TabSlot->SetPadding(FMargin(0,0,5,0));}
	auto* Content=WidgetTree->ConstructWidget<UScrollBox>();Content->SetScrollBarVisibility(ESlateVisibility::Visible);auto* ContentSlot=Stack->AddChildToVerticalBox(Content);ContentSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	for(int32 I=0;I<5;++I){auto* Page=WidgetTree->ConstructWidget<UVerticalBox>();Content->AddChild(Page);Pages.Add(Page);}
	const auto Hint=[](const TCHAR* T){return FText::FromString(T);};
	Pages[0]->AddChildToVerticalBox(MakeText(Hint(TEXT("Mevcut motor tercihlerin korunur. Yüksek kaliteyi yalnız kendin seçtiğinde açarız. Ekran değişikliğini 15 saniyede onayla; aksi halde önceki görüntü geri gelir.")),12));
	AddChoice(Pages[0],0,Hint(TEXT("Ekran modu")),{TEXT("Kenarlıksız pencere"),TEXT("Tam ekran"),TEXT("Pencere")},Hint(TEXT("Kenarlıksız mod masaüstü çözünürlüğünü kullanır.")));
	AddChoice(Pages[0],1,Hint(TEXT("Çözünürlük")),{},Hint(TEXT("İşletim sistemi tarafından bildirilen ekran/pencere boyutları.")));
	AddSlider(Pages[0],2,Hint(TEXT("3B render ölçeği (%)")),50,100,Hint(TEXT("Düşürmek GPU yükünü azaltır; arayüz çözünürlüğünü değiştirmez.")));
	AddToggle(Pages[0],5,Hint(TEXT("Motorun otomatik render ölçeği")),Hint(TEXT("Seçiliyken yukarıdaki elle girilen yüzde uygulanmaz; etkin motor/upscaler varsayılanı kullanılır. Mevcut otomatik tercih korunur.")));
	AddChoice(Pages[0],3,Hint(TEXT("Kare hızı sınırı")),{TEXT("60 FPS"),TEXT("90 FPS"),TEXT("120 FPS"),TEXT("Sınırsız")},Hint(TEXT("60 FPS güç tüketimi ve GPU yükünü sınırlamaya yardımcı olur.")));
	AddToggle(Pages[0],4,Hint(TEXT("Dikey senkronizasyon (VSync)")),Hint(TEXT("Ekran yırtılmasını azaltır; gecikme ve kare hızı ekranına bağlıdır.")));
	AddChoice(Pages[0],GraphicsPresetChoice,Hint(TEXT("Genel kalite")),
        {TEXT("Düşük"),TEXT("Dengeli"),TEXT("Yüksek"),TEXT("Ultra"),TEXT("Sinema"),TEXT("Özel")},
        Hint(TEXT("Beş hazır seviye mevcut on kalite grubunu birlikte seçer. Özel, tek tek değiştirdiğin grupları korur. Değişiklik Uygula ile etkinleşir; çözünürlük, render ölçeği ve FPS sınırını değiştirmez. Sinema daha çok yakın bitki ve ayrıntı ister; performans garantisi değildir.")));
	const TCHAR* QualityLabels[]={TEXT("Görüş mesafesi"),TEXT("Gölgeler"),TEXT("Küresel aydınlatma"),TEXT("Yansımalar"),TEXT("Kenar yumuşatma"),TEXT("Dokular"),TEXT("Görsel efektler"),TEXT("Görüntü son işlemleri"),TEXT("Bitki örtüsü"),TEXT("Yüzey gölgelendirmesi")};
	for(int32 I=0;I<UE_ARRAY_COUNT(QualityLabels);++I)
	{
		const TCHAR* Method=I==2?TEXT("r.DynamicGlobalIlluminationMethod"):I==3?TEXT("r.ReflectionMethod"):nullptr;
		const IConsoleVariable* CVar=Method?IConsoleManager::Get().FindConsoleVariable(Method):nullptr;
		const bool bProfileDisabled=CVar&&CVar->GetInt()==0;
		const FText Caption=bProfileDisabled?FText::FromString(FString(QualityLabels[I])+TEXT(" (bu grafik profilinde kapalı)")):Hint(QualityLabels[I]);
		auto* Choice=AddChoice(Pages[0],QualityStart+I,Caption,QualityNames,Hint(TEXT("Unreal Engine kalite grubu; etkin render yönteminin desteklediği özelliklere uygulanır. Çok yüksek ve Sinema GPU maliyetini artırabilir.")));
		Choice->SetIsEnabled(!bProfileDisabled);
	}
	Pages[1]->AddChildToVerticalBox(MakeText(Hint(TEXT("Ana ses bütün oyun seslerini kapsar. Şehir ambiyansı ve arayüz sesleri ayrıca azaltılabilir. Afet sırasındaki ambiyans kısılması tercihinle birlikte uygulanır.")),12));
	AddSlider(Pages[1],30,Hint(TEXT("Ana ses")),0,1,FText::GetEmpty());AddSlider(Pages[1],31,Hint(TEXT("Şehir ambiyansı")),0,1,FText::GetEmpty());AddSlider(Pages[1],32,Hint(TEXT("Arayüz tıklama / panel sesleri")),0,1,FText::GetEmpty());
	Pages[2]->AddChildToVerticalBox(MakeText(Hint(TEXT("Bir tuşa tıkla, ardından yeni klavye tuşuna bas. Esc seçimi iptal eder. Çakışan tuş kabul edilmez. Sol tık: seç/inşa et · Sağ tık: aracı iptal et · Esc: geri/duraklat · Tekerlek: yakınlaştır. Bu güvenlik tuşları sabittir.")),12));
	AddSlider(Pages[2],40,Hint(TEXT("Kamera hareket hassasiyeti")),.25f,3,FText::GetEmpty());AddSlider(Pages[2],41,Hint(TEXT("Kamera dönüş hassasiyeti")),.25f,3,FText::GetEmpty());AddSlider(Pages[2],42,Hint(TEXT("Yakınlaştırma hassasiyeti")),.25f,3,FText::GetEmpty());
	AddToggle(Pages[2],43,Hint(TEXT("Fare tekerleğini ters çevir")),FText::GetEmpty());AddToggle(Pages[2],44,Hint(TEXT("Yumuşak kamera geçişleri")),FText::GetEmpty());
	for(int32 I=0;I<static_cast<int32>(EAftershockUserAction::Count);++I)
	{
		auto* Row=WidgetTree->ConstructWidget<UHorizontalBox>();Pages[2]->AddChildToVerticalBox(Row)->SetPadding(FMargin(0,4));auto* Label=MakeText(UAftershockSettingsSubsystem::GetActionLabel(I));auto* LabelSlot=Row->AddChildToHorizontalBox(Label);LabelSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));LabelSlot->SetVerticalAlignment(VAlign_Center);
		auto* Selector=WidgetTree->ConstructWidget<UAftershockSettingsKeySelector>();Selector->Configure(this,I);Selector->SetAllowModifierKeys(false);Selector->SetAllowGamepadKeys(false);Selector->SetEscapeKeys({EKeys::Escape});Selector->SetKeySelectionText(Hint(TEXT("Bir tuşa bas…")));
		auto* Box=WidgetTree->ConstructWidget<USizeBox>();Box->SetWidthOverride(300);Box->SetMinDesiredHeight(36);Box->SetContent(Selector);Row->AddChildToHorizontalBox(Box);KeySelectors.Add(Selector);
	}
	Pages[2]->AddChildToVerticalBox(MakeButton(Hint(TEXT("TUŞLARI VARSAYILANA GETİR")),102))->SetPadding(FMargin(0,12));
	AddToggle(Pages[3],50,Hint(TEXT("Tur bitirmek için ikinci tıklamayla onay iste")),Hint(TEXT("İlk tıklama yalnız uyarı verir. 6 saniye içinde tekrar tıklarsan ekonomi turu ilerler. Afet uyarı sistemi korunur.")));
	Pages[3]->AddChildToVerticalBox(MakeText(Hint(TEXT("Oyun modu, zorluk, şehir/coğrafya ve sınırsız bütçe yeni oyun ekranında seçilir. Bu menü mevcut oturumun simülasyonunu veya kayıtlı şehrini değiştirmez. Tur ve afet sonrası otomatik kayıt ile manuel kayıt aynı şekilde çalışır.")),13));
	AddToggle(Pages[4],60,Hint(TEXT("Hareketi azalt")),Hint(TEXT("Afet kamera sarsıntısını kapatır ve elle kamera kontrolünün yumuşatmasını kaldırır. Oyun dünyasındaki afet görselleri ve anlatı geçişi sürer.")));
	AddToggle(Pages[4],61,Hint(TEXT("Renk ayrımlı inşaat önizlemesi")),Hint(TEXT("Geçerli alanı camgöbeği, geçersiz alanı turuncu gösterir. Metinle verilen geçerlilik nedeni her zaman görünür.")));
	AddToggle(Pages[4],62,Hint(TEXT("Ayar menüsü açıklamalarını büyüt")),Hint(TEXT("Kaydırılabilir ayar menüsünün açıklama ve düğme yazılarını büyütür. Tuş seçicinin kendi etiketi ve ana oyun HUD ölçeği korunur.")));
	Pages[4]->AddChildToVerticalBox(MakeText(Hint(TEXT("Bütün seçenekler Tab / Shift+Tab ile gezilebilir; odaklı düğme Enter veya Boşluk ile çalışır. Ses ve inşaat geçerliliği tek başına renge bağlı değildir. Klavye atamalarında menü tuşları korunur.")),13));
	Feedback=MakeText(FText::GetEmpty(),12);Stack->AddChildToVerticalBox(Feedback)->SetPadding(FMargin(0,10));
	auto* Footer=WidgetTree->ConstructWidget<UHorizontalBox>();Stack->AddChildToVerticalBox(Footer);ApplyButton=MakeButton(Hint(TEXT("UYGULA")),100);CloseButton=MakeButton(Hint(TEXT("GERİ  [Esc]")),101);Footer->AddChildToHorizontalBox(ApplyButton)->SetPadding(FMargin(0,0,12,0));Footer->AddChildToHorizontalBox(CloseButton);
	ConfirmationCard=WidgetTree->ConstructWidget<UBorder>();ConfirmationCard->SetPadding(FMargin(30));ConfirmationCard->SetBrush(FSlateRoundedBoxBrush(FLinearColor(.025f,.045f,.06f),8.f,Accent,2.f));auto* ConfirmSlot=Root->AddChildToOverlay(ConfirmationCard);ConfirmSlot->SetHorizontalAlignment(HAlign_Center);ConfirmSlot->SetVerticalAlignment(VAlign_Center);
	auto* ConfirmBox=WidgetTree->ConstructWidget<USizeBox>();ConfirmBox->SetWidthOverride(620);ConfirmationCard->SetContent(ConfirmBox);auto* ConfirmStack=WidgetTree->ConstructWidget<UVerticalBox>();ConfirmBox->SetContent(ConfirmStack);ConfirmationText=MakeText(FText::GetEmpty(),18);ConfirmStack->AddChildToVerticalBox(ConfirmationText)->SetPadding(FMargin(0,0,0,20));
	KeepButton=MakeButton(Hint(TEXT("GÖRÜNTÜYÜ KORU")),103);ConfirmStack->AddChildToVerticalBox(KeepButton)->SetPadding(FMargin(0,0,0,8));ConfirmStack->AddChildToVerticalBox(MakeButton(Hint(TEXT("ÖNCEKİ AYARLARA DÖN  [Esc]")),104));ConfirmationCard->SetVisibility(ESlateVisibility::Collapsed);ShowPage(0);
}

void UAftershockSettingsWidget::CaptureSnapshot()
{
	if(auto* S=UGameUserSettings::GetGameUserSettings()){PreviousResolution=S->GetScreenResolution();PreviousWindowMode=S->GetFullscreenMode();PreviousQuality=S->ScalabilityQuality;PreviousFrameLimit=S->GetFrameRateLimit();bPreviousVSync=S->IsVSyncEnabled();}
	if(auto* P=PreferencesService())PreviousPreferences=P->GetPreferences();
}

void UAftershockSettingsWidget::OpenSettings()
{
	if(!WidgetTree||!WidgetTree->RootWidget)TakeWidget();CaptureSnapshot();LoadControls();ShowPage(0);SetFeedback(NSLOCTEXT("Aftershock","SettingsApplyHint","Değişiklikler Uygula ile etkinleşir. Geri, uygulanmamış seçimleri bırakır."));if(UAftershockSettingsChoice* Initial=Choices.FindRef(0))Initial->SetKeyboardFocus();
}

void UAftershockSettingsWidget::LoadControls()
{
	bLoadingControls=true;auto* S=UGameUserSettings::GetGameUserSettings();auto* P=PreferencesService();if(!S||!P){bLoadingControls=false;return;}StagedPreferences=P->GetPreferences();
	Choices[0]->SetSelectedIndex(S->GetFullscreenMode()==EWindowMode::WindowedFullscreen?0:S->GetFullscreenMode()==EWindowMode::Fullscreen?1:2);
	FullscreenResolutions.Reset();Resolutions.Reset();UKismetSystemLibrary::GetSupportedFullscreenResolutions(FullscreenResolutions);Resolutions=FullscreenResolutions;TArray<FIntPoint> Windowed;UKismetSystemLibrary::GetConvenientWindowedResolutions(Windowed);for(const auto& Size:Windowed)Resolutions.AddUnique(Size);Resolutions.AddUnique(S->GetScreenResolution());Resolutions.AddUnique(S->GetDesktopResolution());
	Resolutions.RemoveAll([](const FIntPoint& Size){return Size.X<640||Size.Y<480;});Resolutions.Sort([](const FIntPoint& A,const FIntPoint& B){return A.X==B.X?A.Y<B.Y:A.X<B.X;});Choices[1]->ClearOptions();for(const auto& Size:Resolutions)Choices[1]->AddOption(FString::Printf(TEXT("%d × %d"),Size.X,Size.Y));Choices[1]->SetSelectedIndex(Resolutions.IndexOfByKey(S->GetScreenResolution()));
	float N,V,Min,Max;S->GetResolutionScaleInformationEx(N,V,Min,Max);Toggles[5]->SetIsChecked(V<=0.f);Sliders[2]->SetMinValue(FMath::Max(1.f,FMath::Min(V>0.f?V:25.f,25.f)));Sliders[2]->SetMaxValue(FMath::Max(V,Max));Sliders[2]->SetValue(V>0.f?V:100.f);
	int32 FPSIndex=INDEX_NONE;for(int32 I=0;I<UE_ARRAY_COUNT(FPSValues);++I)if(FMath::IsNearlyEqual(S->GetFrameRateLimit(),static_cast<float>(FPSValues[I]),.1f))FPSIndex=I;
	// Preserve an existing custom cap until the player explicitly changes it.
	Choices[3]->ClearOptions();for(const auto& Value:{TEXT("60 FPS"),TEXT("90 FPS"),TEXT("120 FPS"),TEXT("Sınırsız")})Choices[3]->AddOption(Value);
	if(FPSIndex==INDEX_NONE){Choices[3]->AddOption(FString::Printf(TEXT("Mevcut (%.0f FPS)"),S->GetFrameRateLimit()));FPSIndex=4;}Choices[3]->SetSelectedIndex(FPSIndex);Toggles[4]->SetIsChecked(S->IsVSyncEnabled());
	const int32 Values[]={S->GetViewDistanceQuality(),S->GetShadowQuality(),S->GetGlobalIlluminationQuality(),S->GetReflectionQuality(),S->GetAntiAliasingQuality(),S->GetTextureQuality(),S->GetVisualEffectQuality(),S->GetPostProcessingQuality(),S->GetFoliageQuality(),S->GetShadingQuality()};for(int32 I=0;I<UE_ARRAY_COUNT(Values);++I)Choices[QualityStart+I]->SetSelectedIndex(FMath::Clamp(Values[I],0,4));
	RefreshGraphicsPresetChoice();
	Sliders[30]->SetValue(StagedPreferences.MasterVolume);Sliders[31]->SetValue(StagedPreferences.CityAmbienceVolume);Sliders[32]->SetValue(StagedPreferences.UIVolume);Sliders[40]->SetValue(StagedPreferences.PanSensitivity);Sliders[41]->SetValue(StagedPreferences.RotationSensitivity);Sliders[42]->SetValue(StagedPreferences.ZoomSensitivity);
	Toggles[43]->SetIsChecked(StagedPreferences.bInvertWheel);Toggles[44]->SetIsChecked(StagedPreferences.bSmoothCamera);Toggles[50]->SetIsChecked(StagedPreferences.bConfirmEndTurn);Toggles[60]->SetIsChecked(StagedPreferences.bReduceMotion);Toggles[61]->SetIsChecked(StagedPreferences.bColorSafePlacement);Toggles[62]->SetIsChecked(StagedPreferences.bLargeMenuText);
	for(int32 I=0;I<KeySelectors.Num();++I)KeySelectors[I]->SetSelectedKey(FInputChord(StagedPreferences.Keys[I]));bLoadingControls=false;ApplyMenuTextPreference();
}

void UAftershockSettingsWidget::RefreshGraphicsPresetChoice()
{
    auto* Preset=Choices.FindRef(GraphicsPresetChoice).Get();if(!Preset)return;
    const auto* First=Choices.FindRef(QualityStart).Get();int32 Common=First?First->GetSelectedIndex():INDEX_NONE;
    bool Uniform=Common>=0&&Common<=4;
    for(int32 I=1;I<QualityGroupCount;++I)
    {
        const auto* Choice=Choices.FindRef(QualityStart+I).Get();
        Uniform=Uniform&&Choice&&Choice->GetSelectedIndex()==Common;
    }
    TGuardValue<bool> Guard(bLoadingControls,true);Preset->SetSelectedIndex(Uniform?Common:5);
}
void UAftershockSettingsWidget::HandleGraphicsChoiceChanged(int32 ChoiceID)
{
    if(bLoadingControls||bDisplayConfirmation)return;
    if(ChoiceID==GraphicsPresetChoice)
    {
        const auto* Preset=Choices.FindRef(GraphicsPresetChoice).Get();const int32 Level=Preset?Preset->GetSelectedIndex():INDEX_NONE;
        if(Level<0||Level>4)return; // Custom does not overwrite the ten choices.
        {TGuardValue<bool> Guard(bLoadingControls,true);for(int32 I=0;I<QualityGroupCount;++I)if(auto* Choice=Choices.FindRef(QualityStart+I).Get())Choice->SetSelectedIndex(Level);}
        SetFeedback(NSLOCTEXT("Aftershock","SettingsGraphicsPresetStaged","Genel kalite seçildi. Bu yalnız bir taslak; etkinleştirmek için Uygula. Bu profilde kapalı render yöntemleri kendiliğinden açılmaz."));return;
    }
    if(ChoiceID>=QualityStart&&ChoiceID<QualityStart+QualityGroupCount)RefreshGraphicsPresetChoice();
}

void UAftershockSettingsWidget::ShowPage(int32 Index)
{
	CurrentPage=FMath::Clamp(Index,0,Pages.Num()-1);for(int32 I=0;I<Pages.Num();++I)
	{
		Pages[I]->SetVisibility(I==CurrentPage?ESlateVisibility::Visible:ESlateVisibility::Collapsed);
		auto Style=Tabs[I]->GetStyle();Style.SetNormal(FSlateRoundedBoxBrush(I==CurrentPage?FLinearColor(.17f,.10f,.04f):FLinearColor(.025f,.065f,.081f),4.f,I==CurrentPage?Accent:SettingsBorderColour,I==CurrentPage?2.f:1.f));Tabs[I]->SetStyle(Style);
	}
}

void UAftershockSettingsWidget::SetFeedback(const FText& Message,bool bError)
{if(Feedback){Feedback->SetText(Message);Feedback->SetColorAndOpacity(FSlateColor(bError?FLinearColor(1,.47f,.25f):Muted));}}

void UAftershockSettingsWidget::HandleAction(int32 Action)
{
	if(auto* Audio=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockAudioSubsystem>():nullptr)Audio->PlayUIClick();
	if(bDisplayConfirmation){if(Action==103)KeepDisplayChanges();else if(Action==104||Action==101)RevertDisplayChanges();return;}
	if(Action>=0&&Action<5){ShowPage(Action);return;}if(Action==100)ApplyChanges();else if(Action==101)CancelAndClose();else if(Action==102)
	{StagedPreferences.Keys=FAftershockUserPreferences().Keys;bLoadingControls=true;for(int32 I=0;I<KeySelectors.Num();++I)KeySelectors[I]->SetSelectedKey(FInputChord(StagedPreferences.Keys[I]));bLoadingControls=false;SetFeedback(NSLOCTEXT("Aftershock","SettingsKeysReset","Varsayılan tuşlar seçildi. Kaydetmek için Uygula."));}
}

void UAftershockSettingsWidget::SelectKey(int32 Action,const FInputChord& Chord)
{
	if(bLoadingControls||!KeySelectors.IsValidIndex(Action))return;FText Error;
	if(!UAftershockSettingsSubsystem::ValidateKey(StagedPreferences.Keys,Action,Chord.Key,Error))
	{bLoadingControls=true;KeySelectors[Action]->SetSelectedKey(FInputChord(StagedPreferences.Keys[Action]));bLoadingControls=false;SetFeedback(Error,true);return;}
	StagedPreferences.Keys[Action]=Chord.Key;SetFeedback(NSLOCTEXT("Aftershock","SettingsKeyStaged","Tuş seçildi. Yeni atama Uygula sonrasında çalışır."));
}

void UAftershockSettingsWidget::ApplyChanges()
{
	auto* S=UGameUserSettings::GetGameUserSettings();auto* P=PreferencesService();if(!S||!P||!UAftershockSettingsSubsystem::CanWriteUserSettings()){SetFeedback(NSLOCTEXT("Aftershock","SettingsUnavailable","Bu doğrulama oturumunda ayarlar değiştirilemez."),true);return;}
	StagedPreferences.MasterVolume=Sliders[30]->GetValue();StagedPreferences.CityAmbienceVolume=Sliders[31]->GetValue();StagedPreferences.UIVolume=Sliders[32]->GetValue();StagedPreferences.PanSensitivity=Sliders[40]->GetValue();StagedPreferences.RotationSensitivity=Sliders[41]->GetValue();StagedPreferences.ZoomSensitivity=Sliders[42]->GetValue();
	StagedPreferences.bInvertWheel=Toggles[43]->IsChecked();StagedPreferences.bSmoothCamera=Toggles[44]->IsChecked();StagedPreferences.bConfirmEndTurn=Toggles[50]->IsChecked();StagedPreferences.bReduceMotion=Toggles[60]->IsChecked();StagedPreferences.bColorSafePlacement=Toggles[61]->IsChecked();StagedPreferences.bLargeMenuText=Toggles[62]->IsChecked();FText Error;if(!UAftershockSettingsSubsystem::ValidatePreferences(StagedPreferences,Error)){SetFeedback(Error,true);return;}
	const int32 ModeIndex=Choices[0]->GetSelectedIndex();const EWindowMode::Type Mode=ModeIndex==0?EWindowMode::WindowedFullscreen:ModeIndex==1?EWindowMode::Fullscreen:EWindowMode::Windowed;
	FIntPoint Resolution=Resolutions.IsValidIndex(Choices[1]->GetSelectedIndex())?Resolutions[Choices[1]->GetSelectedIndex()]:S->GetScreenResolution();if(Mode==EWindowMode::WindowedFullscreen)Resolution=S->GetDesktopResolution();
	if(Mode==EWindowMode::Fullscreen&&!FullscreenResolutions.IsEmpty()&&!FullscreenResolutions.Contains(Resolution)){SetFeedback(NSLOCTEXT("Aftershock","SettingsUnsupportedFullscreen","Bu çözünürlük tam ekran için bildirilmedi. Desteklenen başka bir boyut veya pencere modu seç."),true);return;}
	CaptureSnapshot();S->SetScreenResolution(Resolution);S->SetFullscreenMode(Mode);S->SetResolutionScaleValueEx(Toggles[5]->IsChecked()?0.f:Sliders[2]->GetValue());S->SetVSyncEnabled(Toggles[4]->IsChecked());const int32 Cap=Choices[3]->GetSelectedIndex();S->SetFrameRateLimit(Cap>=0&&Cap<UE_ARRAY_COUNT(FPSValues)?FPSValues[Cap]:PreviousFrameLimit);
	const auto Q=[this](int32 Index){return FMath::Clamp(Choices[QualityStart+Index]->GetSelectedIndex(),0,4);};S->SetViewDistanceQuality(Q(0));S->SetShadowQuality(Q(1));S->SetGlobalIlluminationQuality(Q(2));S->SetReflectionQuality(Q(3));S->SetAntiAliasingQuality(Q(4));S->SetTextureQuality(Q(5));S->SetVisualEffectQuality(Q(6));S->SetPostProcessingQuality(Q(7));S->SetFoliageQuality(Q(8));S->SetShadingQuality(Q(9));
	P->SetPreferences(StagedPreferences,false,Error);S->ApplyNonResolutionSettings();if(auto* PC=Cast<AAftershockPlayerController>(GetOwningPlayer()))PC->ApplyUserPreferences();if(GEngine)GEngine->SetMaxFPS(S->GetFrameRateLimit());
	if(PreviousResolution!=Resolution||PreviousWindowMode!=Mode)
	{S->ApplyResolutionSettings(false);bDisplayConfirmation=true;DisplayConfirmDeadline=FPlatformTime::Seconds()+15.;ConfirmationCard->SetVisibility(ESlateVisibility::Visible);SettingsCard->SetIsEnabled(false);KeepButton->SetKeyboardFocus();UpdateRealTimeConfirmation();}
	else PersistAppliedSettings();
}

void UAftershockSettingsWidget::PersistAppliedSettings()
{
	FText Error;auto* P=PreferencesService();if(!P||!P->SetPreferences(P->GetPreferences(),true,Error)){SetFeedback(Error,true);return;}
	if(auto* S=UGameUserSettings::GetGameUserSettings()){S->ConfirmVideoMode();S->SaveSettings();}CaptureSnapshot();LoadControls();SetFeedback(NSLOCTEXT("Aftershock","SettingsSaved","Ayarlar uygulandı ve bu bilgisayara kaydedildi."));
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SETTINGS_APPLIED persisted=1 heavyDefaultsForced=0"));
}

void UAftershockSettingsWidget::KeepDisplayChanges()
{bDisplayConfirmation=false;ConfirmationCard->SetVisibility(ESlateVisibility::Collapsed);SettingsCard->SetIsEnabled(true);PersistAppliedSettings();CloseButton->SetKeyboardFocus();}

void UAftershockSettingsWidget::RevertDisplayChanges()
{
	if(!bDisplayConfirmation)return;bDisplayConfirmation=false;auto* S=UGameUserSettings::GetGameUserSettings();
	if(S){S->RevertVideoMode();S->SetScreenResolution(PreviousResolution);S->SetFullscreenMode(PreviousWindowMode);S->ScalabilityQuality=PreviousQuality;S->SetResolutionScaleValueEx(PreviousQuality.ResolutionQuality);S->SetFrameRateLimit(PreviousFrameLimit);S->SetVSyncEnabled(bPreviousVSync);S->ApplyResolutionSettings(false);S->ApplyNonResolutionSettings();if(GEngine)GEngine->SetMaxFPS(PreviousFrameLimit);}
	FText Error;if(auto* P=PreferencesService())P->SetPreferences(PreviousPreferences,false,Error);if(auto* PC=Cast<AAftershockPlayerController>(GetOwningPlayer()))PC->ApplyUserPreferences();
	if(ConfirmationCard)ConfirmationCard->SetVisibility(ESlateVisibility::Collapsed);if(SettingsCard)SettingsCard->SetIsEnabled(true);LoadControls();SetFeedback(NSLOCTEXT("Aftershock","SettingsReverted","Onay verilmedi: önceki görüntü ve tercihler geri yüklendi."));if(CloseButton)CloseButton->SetKeyboardFocus();
	UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_SETTINGS_DISPLAY_REVERTED persisted=0 realClock=1"));
}

void UAftershockSettingsWidget::UpdateRealTimeConfirmation()
{
	for(const auto& Pair:Sliders)if(UTextBlock* Value=SliderValues.FindRef(Pair.Key))Value->SetText(FText::FromString(Pair.Key==2?FString::Printf(TEXT(" %.0f%%"),Pair.Value->GetValue()):Pair.Key>=40?FString::Printf(TEXT(" %.2f×"),Pair.Value->GetValue()):FString::Printf(TEXT(" %.0f%%"),Pair.Value->GetValue()*100.f)));
	if(!bDisplayConfirmation)return;const int32 Seconds=FMath::CeilToInt(DisplayConfirmDeadline-FPlatformTime::Seconds());if(Seconds<=0){RevertDisplayChanges();return;}
	ConfirmationText->SetText(FText::Format(NSLOCTEXT("Aftershock","SettingsConfirmDisplay","Yeni görüntüyü koruyalım mı?\n{0} saniye içinde onaylamazsan önceki ayarlara döneriz."),FText::AsNumber(Seconds)));
}

void UAftershockSettingsWidget::ApplyMenuTextPreference()
{
	const bool bLarge=PreferencesService()&&PreferencesService()->GetPreferences().bLargeMenuText;for(int32 I=0;I<MenuTexts.Num();++I)if(MenuTexts[I]&&BaseTextSizes.IsValidIndex(I)){auto Font=MenuTexts[I]->GetFont();Font.Size=FMath::RoundToInt(BaseTextSizes[I]*(bLarge?1.2f:1.f));MenuTexts[I]->SetFont(Font);}
}

void UAftershockSettingsWidget::CancelAndClose()
{if(bDisplayConfirmation){RevertDisplayChanges();return;}if(auto* PC=Cast<AAftershockPlayerController>(GetOwningPlayer()))PC->CloseSettingsMenu();}
void UAftershockSettingsWidget::NativeDestruct(){RevertDisplayChanges();Super::NativeDestruct();}
FReply UAftershockSettingsWidget::NativeOnPreviewKeyDown(const FGeometry& Geometry,const FKeyEvent& Event)
{
	// InputKeySelector owns Escape while it is listening; do not close its parent.
	if(Event.GetKey()==EKeys::Escape){for(UAftershockSettingsKeySelector* Selector:KeySelectors)if(Selector&&Selector->GetIsSelectingKey())return Super::NativeOnPreviewKeyDown(Geometry,Event);CancelAndClose();return FReply::Handled();}
	return Super::NativeOnPreviewKeyDown(Geometry,Event);
}
