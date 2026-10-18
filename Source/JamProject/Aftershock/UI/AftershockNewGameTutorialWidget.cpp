#include "Aftershock/UI/AftershockNewGameTutorialWidget.h"
#include "Aftershock/Game/AftershockPlayerController.h"
#include "Aftershock/Public/AftershockAudioSubsystem.h"
#include "Aftershock/Public/AftershockOpeningWidget.h"
#include "Aftershock/Public/AftershockSettingsSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformTime.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"

namespace AftershockTutorialStyle
{
const FLinearColor Ink(.95f,.95f,.89f),Muted(.60f,.72f,.75f),Gold(.96f,.63f,.29f);
const FLinearColor Surface(.013f,.028f,.039f,.98f),Edge(.10f,.19f,.23f);
}

TSharedRef<SWidget> UAftershockNewGameTutorialWidget::RebuildWidget()
{
    if(!WidgetTree||!WidgetTree->RootWidget)BuildLayout();return Super::RebuildWidget();
}

void UAftershockNewGameTutorialWidget::BuildSteps()
{
    const auto* Settings=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSettingsSubsystem>():nullptr;
    const auto* Session=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockOpeningSessionSubsystem>():nullptr;
    const FAftershockUserPreferences Defaults;
    const auto Key=[&](EAftershockUserAction Action)
    {
        const FKey Bound=Settings?Settings->GetKey(Action):Defaults.Keys[static_cast<int32>(Action)];
        return Bound.IsValid()?Bound.GetDisplayName():NSLOCTEXT("Aftershock","TutorialNoKey","Atanmamış");
    };
    const bool ColorSafe=Settings&&Settings->GetPreferences().bColorSafePlacement;
    Steps={
        {NSLOCTEXT("Aftershock","TutorialCameraTitle","Şehrini tanı"),
         FText::Format(NSLOCTEXT("Aftershock","TutorialCameraBody","Tekerlekle yakınlaş. {0} ile küre ve şehir görünümü arasında geçiş yap."),Key(EAftershockUserAction::Overview)),
         FText::Format(NSLOCTEXT("Aftershock","TutorialCameraKeys","{0} / {1} / {2} / {3}  Kaydır\n{4} / {5}  Kamerayı döndür"),Key(EAftershockUserAction::PanForward),Key(EAftershockUserAction::PanBackward),Key(EAftershockUserAction::PanLeft),Key(EAftershockUserAction::PanRight),Key(EAftershockUserAction::RotateLeft),Key(EAftershockUserAction::RotateRight))},
        {NSLOCTEXT("Aftershock","TutorialRoadTitle","Yolu iki tıkla kur"),
         NSLOCTEXT("Aftershock","TutorialRoadBody","YOL aracını seç. İlk sol tık başlangıcı, ikinci tık bitişi onaylar. Sağ tık veya Esc iptal eder."),
         ColorSafe?NSLOCTEXT("Aftershock","TutorialRoadColorSafe","Turkuaz: uygun güzergâh\nTuruncu: geçersiz; işlem uygulanmaz")
                  :NSLOCTEXT("Aftershock","TutorialRoadColors","Yeşil: uygun güzergâh\nKırmızı: geçersiz; işlem uygulanmaz")},
        {NSLOCTEXT("Aftershock","TutorialBuildingTitle","Önizlemeyi oku, sonra yerleştir"),
         Session&&Session->IsUnlimitedBudget()
             ?NSLOCTEXT("Aftershock","TutorialBuildingUnlimitedBody","Alt katalogdan bina seç. Uygun önizlemeye sol tıkla yerleştir. Sınırsız bütçede yatırım bedeli hazineyi azaltmaz.")
             :NSLOCTEXT("Aftershock","TutorialBuildingBody","Alt katalogdan bina seç. Uygun önizlemeye sol tıkla yerleştir; bedel başarılı onayda hazineden düşer."),
         FText::Format(NSLOCTEXT("Aftershock","TutorialBuildingKeys","{0}  Yön    {1}  Temel\n{2} / {3}  Kat sayısı"),Key(EAftershockUserAction::RotateBuilding),Key(EAftershockUserAction::CycleFoundation),Key(EAftershockUserAction::IncreaseFloors),Key(EAftershockUserAction::DecreaseFloors))},
        {NSLOCTEXT("Aftershock","TutorialBudgetTitle","Bütçe ve ihtiyaçları dengele"),
         NSLOCTEXT("Aftershock","TutorialBudgetBody","Vergiyi şehir kartından ayarla. Geliri, bakımı ve vatandaş ihtiyaçlarını izle. TURU BİTİR ekonomik döngüyü işler."),
         NSLOCTEXT("Aftershock","TutorialBudgetKeys","Bütçe  ·  Gıda / su  ·  Enerji\nMemnuniyet, şehir kararlarının sonucunu gösterir.")},
        {NSLOCTEXT("Aftershock","TutorialHazardTitle","Uyarıyı hazırlığa çevir"),
         NSLOCTEXT("Aftershock","TutorialHazardBody","Zemin etüdü, güçlendirme ve drenajı risklere göre seç. Toplanma alanlarına ulaşımı açık tut."),
         Session&&Session->GetSettings().Mode==EAftershockPlayMode::GeographicSimulation
             ?NSLOCTEXT("Aftershock","TutorialGeographicHazards","Bölgesel model afetleri otomatik seçer.\nHazırlık sayacını ve yerel riskleri izle.")
             :NSLOCTEXT("Aftershock","TutorialScenarioHazards","Senaryo hedeflerini ve hazırlık sayacını izle.\nAçık afet düğmeleri hazırlık uyarısı başlatır.")},
        {NSLOCTEXT("Aftershock","TutorialSaveTitle","Duraklat, kaydet, devam et"),
         NSLOCTEXT("Aftershock","TutorialSaveBody","Esc ile duraklat. ŞEHRİ KAYDET mevcut şehri kaydeder; Devam Et kayıtlı oturumu açar."),
         NSLOCTEXT("Aftershock","TutorialSaveKeys","Kaydın sonuç mesajını kontrol et.\nHazır olduğunda şehrin yönetimi sende.")}
    };
}

void UAftershockNewGameTutorialWidget::BuildLayout()
{
    using namespace AftershockTutorialStyle;
    if(!WidgetTree)WidgetTree=NewObject<UWidgetTree>(this,TEXT("NewGameTutorialTree"));
    const auto* Settings=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSettingsSubsystem>():nullptr;
    const float FontScale=Settings&&Settings->GetPreferences().bLargeMenuText?1.12f:1.f;
    UCanvasPanel* Root=WidgetTree->ConstructWidget<UCanvasPanel>();WidgetTree->RootWidget=Root;
    UBorder* Scrim=WidgetTree->ConstructWidget<UBorder>();Scrim->SetBrushColor(FLinearColor(.005f,.012f,.018f,.10f));Scrim->SetPadding(FMargin(0));Scrim->SetVisibility(ESlateVisibility::HitTestInvisible);
    auto* ScrimSlot=Root->AddChildToCanvas(Scrim);ScrimSlot->SetAnchors(FAnchors(0,0,1,1));ScrimSlot->SetOffsets(FMargin(0));
    Card=WidgetTree->ConstructWidget<UBorder>();Card->SetBrush(FSlateRoundedBoxBrush(Surface,10.f,Edge,1.f));Card->SetPadding(FMargin(25,23));
    auto* CardSlot=Root->AddChildToCanvas(Card);CardSlot->SetAnchors(FAnchors(1.f,.5f));CardSlot->SetAlignment(FVector2D(1.f,.5f));CardSlot->SetPosition(FVector2D(-30,0));CardSlot->SetAutoSize(true);
    USizeBox* CardWidth=WidgetTree->ConstructWidget<USizeBox>();CardWidth->SetWidthOverride(530.f);Card->SetContent(CardWidth);
    UVerticalBox* Stack=WidgetTree->ConstructWidget<UVerticalBox>();CardWidth->SetContent(Stack);
    const auto MakeText=[&](FText Content,int32 Size,FLinearColor Color,bool Bold=false)
    {
        auto* Label=WidgetTree->ConstructWidget<UTextBlock>();Label->SetText(Content);Label->SetColorAndOpacity(Color);
        FSlateFontInfo Font=Label->GetFont();Font.Size=FMath::RoundToInt(Size*FontScale);Font.TypefaceFontName=Bold?TEXT("Bold"):TEXT("Regular");Label->SetFont(Font);
        Label->SetAutoWrapText(true);Label->SetWrapTextAt(480.f);Label->SetVisibility(ESlateVisibility::HitTestInvisible);return Label;
    };
    Stack->AddChildToVerticalBox(MakeText(NSLOCTEXT("Aftershock","TutorialBrand","AFTERSHOCK  /  İLK KARARLARIN"),10,Gold,true));
    UHorizontalBox* StatusRow=WidgetTree->ConstructWidget<UHorizontalBox>();auto* StatusSlot=Stack->AddChildToVerticalBox(StatusRow);StatusSlot->SetPadding(FMargin(0,15,0,8));
    StepLabel=MakeText(FText::GetEmpty(),12,Ink,true);auto* StepStatusSlot=StatusRow->AddChildToHorizontalBox(StepLabel);StepStatusSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    TimeLabel=MakeText(FText::GetEmpty(),10,Muted);StatusRow->AddChildToHorizontalBox(TimeLabel);
    UHorizontalBox* Segments=WidgetTree->ConstructWidget<UHorizontalBox>();Stack->AddChildToVerticalBox(Segments);StepSegments.Reset();
    for(int32 Index=0;Index<TutorialStepCount;++Index)
    {
        auto* Segment=WidgetTree->ConstructWidget<UBorder>();Segment->SetPadding(FMargin(0,2));Segment->SetBrushColor(Edge);Segment->SetVisibility(ESlateVisibility::HitTestInvisible);
        auto* SegmentSlot=Segments->AddChildToHorizontalBox(Segment);SegmentSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));SegmentSlot->SetPadding(FMargin(Index==0?0:4,0,0,0));StepSegments.Add(Segment);
    }
    Headline=MakeText(FText::GetEmpty(),25,Ink,true);auto* HeadlineSlot=Stack->AddChildToVerticalBox(Headline);HeadlineSlot->SetPadding(FMargin(0,20,0,12));
    Body=MakeText(FText::GetEmpty(),15,Ink);auto* BodySlot=Stack->AddChildToVerticalBox(Body);BodySlot->SetPadding(FMargin(0,0,0,16));
    UBorder* ControlPanel=WidgetTree->ConstructWidget<UBorder>();ControlPanel->SetBrush(FSlateRoundedBoxBrush(FLinearColor(.026f,.052f,.065f),5.f));ControlPanel->SetPadding(FMargin(14,12));
    Controls=MakeText(FText::GetEmpty(),12,Gold);ControlPanel->SetContent(Controls);Stack->AddChildToVerticalBox(ControlPanel);
    auto* PauseNote=MakeText(NSLOCTEXT("Aftershock","TutorialPausedNote","Şehir beklemede. Rehber bitince bu adımları uygulayabilirsin."),10,Muted);
    auto* PauseNoteSlot=Stack->AddChildToVerticalBox(PauseNote);PauseNoteSlot->SetPadding(FMargin(0,12,0,14));
    UHorizontalBox* NavigationRow=WidgetTree->ConstructWidget<UHorizontalBox>();Stack->AddChildToVerticalBox(NavigationRow);
    const auto MakeButton=[&](TObjectPtr<UButton>& Target,FText Label,bool Primary)
    {
        // UE 5.8 UButton is keyboard-focusable by default; it has no SetIsFocusable setter.
        Target=WidgetTree->ConstructWidget<UButton>();
        FButtonStyle Style=Target->GetStyle();Style.SetNormal(FSlateRoundedBoxBrush(Primary?FLinearColor(.20f,.108f,.041f):FLinearColor(.026f,.049f,.062f),5.f,Primary?Gold:Edge,1.f));
        Style.SetHovered(FSlateRoundedBoxBrush(FLinearColor(.11f,.18f,.20f),5.f,Gold,1.f));Style.SetPressed(FSlateRoundedBoxBrush(FLinearColor(.035f,.073f,.084f),5.f,Gold,1.f));
        Style.SetDisabled(FSlateRoundedBoxBrush(FLinearColor(.018f,.031f,.040f),5.f,Edge,1.f));Style.SetNormalPadding(FMargin(14,11));Style.SetPressedPadding(FMargin(14,12,14,10));Target->SetStyle(Style);
        auto* LabelWidget=MakeText(Label,11,Primary?Ink:Muted,true);Target->SetContent(LabelWidget);return LabelWidget;
    };
    MakeButton(BackButton,NSLOCTEXT("Aftershock","TutorialBack","GERİ"),false);auto* BackSlot=NavigationRow->AddChildToHorizontalBox(BackButton);BackSlot->SetPadding(FMargin(0,0,8,0));
    NextLabel=MakeButton(NextButton,NSLOCTEXT("Aftershock","TutorialNext","DEVAM  →"),true);auto* NextSlot=NavigationRow->AddChildToHorizontalBox(NextButton);NextSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    MakeButton(SkipButton,NSLOCTEXT("Aftershock","TutorialSkip","ATLA"),false);auto* SkipSlot=NavigationRow->AddChildToHorizontalBox(SkipButton);SkipSlot->SetPadding(FMargin(8,0,0,0));
    BackButton->OnClicked.AddUniqueDynamic(this,&ThisClass::HandleBack);NextButton->OnClicked.AddUniqueDynamic(this,&ThisClass::HandleNext);SkipButton->OnClicked.AddUniqueDynamic(this,&ThisClass::HandleSkip);
    BackButton->SetToolTipText(NSLOCTEXT("Aftershock","TutorialBackHint","Önceki adımı gözden geçir; rehberin toplam süresi uzamaz."));
    NextButton->SetToolTipText(NSLOCTEXT("Aftershock","TutorialNextHint","Sonraki adıma geç. Son adımda şehri yönetmeye başla."));
    SkipButton->SetToolTipText(NSLOCTEXT("Aftershock","TutorialSkipHint","Şehri değiştirmeden rehberi kapat."));
    Progress=WidgetTree->ConstructWidget<UProgressBar>();Progress->SetFillColorAndOpacity(Gold);Progress->SetVisibility(ESlateVisibility::HitTestInvisible);
    USizeBox* ProgressHeight=WidgetTree->ConstructWidget<USizeBox>();ProgressHeight->SetHeightOverride(2);ProgressHeight->SetContent(Progress);auto* ProgressSlot=Stack->AddChildToVerticalBox(ProgressHeight);ProgressSlot->SetPadding(FMargin(0,17,0,10));
    Stack->AddChildToVerticalBox(MakeText(NSLOCTEXT("Aftershock","TutorialNavigationHint","Enter / →  Devam     ←  Geri     Esc  Atla"),9,Muted));
}

void UAftershockNewGameTutorialWidget::NativeConstruct()
{
    Super::NativeConstruct();SetIsFocusable(true);SetNavigationRuleBase(EUINavigation::Next,EUINavigationRule::Wrap);SetNavigationRuleBase(EUINavigation::Previous,EUINavigationRule::Wrap);StopClock();BuildSteps();bCompleted=false;CurrentStep=0;FinishReason.Reset();
    StartedAt=StepStartedAt=FPlatformTime::Seconds();FinishStartedAt=CompletedAt=-1.;LastRemainingSecond=INDEX_NONE;SetRenderOpacity(1.f);
    ShowStep(0,false);RealClock=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this,&ThisClass::TickTutorial),.025f);
}
void UAftershockNewGameTutorialWidget::NativeDestruct(){StopClock();Super::NativeDestruct();}
void UAftershockNewGameTutorialWidget::StopClock()
{if(RealClock.IsValid())FTSTicker::GetCoreTicker().RemoveTicker(RealClock);RealClock.Reset();}
double UAftershockNewGameTutorialWidget::GetElapsedRealSeconds()const
{return FMath::Max(0.,(CompletedAt>=0.?CompletedAt:FPlatformTime::Seconds())-StartedAt);}
bool UAftershockNewGameTutorialWidget::HasReachedDeadline(double ElapsedSeconds)
{return FMath::IsFinite(ElapsedSeconds)&&ElapsedSeconds>=MaximumRealDurationSeconds;}
int32 UAftershockNewGameTutorialWidget::ComputeElapsedStepCount(double StepElapsedSeconds)
{
    if(!FMath::IsFinite(StepElapsedSeconds)||StepElapsedSeconds<SecondsPerStep)return 0;
    if(StepElapsedSeconds>=TutorialStepCount*SecondsPerStep)return TutorialStepCount;
    return FMath::FloorToInt(StepElapsedSeconds/SecondsPerStep);
}
FText UAftershockNewGameTutorialWidget::GetCurrentHeadline()const
{return Steps.IsValidIndex(CurrentStep)?Steps[CurrentStep].Title:FText::GetEmpty();}
void UAftershockNewGameTutorialWidget::ShowStep(int32 Index,bool bResetStepClock)
{
    if(!Steps.IsValidIndex(Index)||bCompleted)return;CurrentStep=Index;if(bResetStepClock)StepStartedAt=FPlatformTime::Seconds();
    if(StepLabel)StepLabel->SetText(FText::Format(NSLOCTEXT("Aftershock","TutorialStepCount","ADIM {0} / {1}"),FText::AsNumber(CurrentStep+1),FText::AsNumber(TutorialStepCount)));
    if(Headline)Headline->SetText(Steps[Index].Title);if(Body)Body->SetText(Steps[Index].Body);if(Controls)Controls->SetText(Steps[Index].Controls);
    if(BackButton)BackButton->SetIsEnabled(Index>0);if(NextLabel)NextLabel->SetText(Index+1==Steps.Num()?NSLOCTEXT("Aftershock","TutorialStartCity","ŞEHRE BAŞLA  →"):NSLOCTEXT("Aftershock","TutorialNext","DEVAM  →"));
    for(int32 Segment=0;Segment<StepSegments.Num();++Segment)if(StepSegments[Segment])StepSegments[Segment]->SetBrushColor(Segment<=Index?AftershockTutorialStyle::Gold:AftershockTutorialStyle::Edge);
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_NEW_GAME_TUTORIAL_STEP step=%d total=%d elapsedReal=%.3f canonicalWrites=0"),Index+1,Steps.Num(),GetElapsedRealSeconds());
}
bool UAftershockNewGameTutorialWidget::TickTutorial(float)
{
    if(bCompleted||!IsInViewport())return false;
    const double Now=FPlatformTime::Seconds(),Elapsed=FMath::Max(0.,Now-StartedAt);
    if(HasReachedDeadline(Elapsed)){FinishNow(TEXT("Deadline"));return false;}
    if(FinishStartedAt>=0.)
    {
        const float Alpha=FMath::Clamp(static_cast<float>((Now-FinishStartedAt)/ClosingSeconds),0.f,1.f);SetRenderOpacity(1.f-Alpha*Alpha*(3.f-2.f*Alpha));
        if(Alpha>=1.f){FinishNow(*FinishReason);return false;}return true;
    }
    const int32 Remaining=FMath::Max(0,FMath::CeilToInt(MaximumRealDurationSeconds-Elapsed));
    if(Remaining!=LastRemainingSecond&&TimeLabel){TimeLabel->SetText(FText::Format(NSLOCTEXT("Aftershock","TutorialRealTimeRemaining","En geç {0} sn"),FText::AsNumber(Remaining)));LastRemainingSecond=Remaining;}
    if(Progress)Progress->SetPercent(FMath::Clamp(static_cast<float>(Elapsed/(TutorialStepCount*SecondsPerStep)),0.f,1.f));
    if(Now-StepStartedAt>=SecondsPerStep)
    {
        const int32 Advance=ComputeElapsedStepCount(Now-StepStartedAt);StepStartedAt+=Advance*SecondsPerStep;
        if(CurrentStep+Advance>=Steps.Num())BeginFinish(TEXT("Autoplay"));else ShowStep(CurrentStep+Advance,false);
    }
    return !bCompleted;
}
void UAftershockNewGameTutorialWidget::BeginFinish(const TCHAR* Reason)
{
    if(bCompleted||FinishStartedAt>=0.)return;FinishReason=Reason;FinishStartedAt=FPlatformTime::Seconds();
    for(UButton* Button:{BackButton.Get(),NextButton.Get(),SkipButton.Get()})if(Button)Button->SetIsEnabled(false);
}
void UAftershockNewGameTutorialWidget::FinishNow(const TCHAR* Reason)
{
    if(bCompleted)return;bCompleted=true;CompletedAt=FPlatformTime::Seconds();const double Elapsed=GetElapsedRealSeconds();StopClock();
    UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_NEW_GAME_TUTORIAL_FINISHED reason=%s elapsedReal=%.3f maximumPolicySeconds=30 noOutcomeOrSaveWrites=1"),Reason,Elapsed);
    if(auto* PC=Cast<AAftershockPlayerController>(GetOwningPlayer()))PC->CompleteNewGameTutorial();
}
void UAftershockNewGameTutorialWidget::PlayNavigationClick()
{if(auto* GI=GetGameInstance())if(auto* Audio=GI->GetSubsystem<UAftershockAudioSubsystem>())Audio->PlayUIClick();}
void UAftershockNewGameTutorialWidget::NextStep()
{if(bCompleted||FinishStartedAt>=0.)return;if(CurrentStep+1>=Steps.Num())BeginFinish(TEXT("ManualComplete"));else ShowStep(CurrentStep+1,true);}
void UAftershockNewGameTutorialWidget::PreviousStep()
{if(!bCompleted&&FinishStartedAt<0.&&CurrentStep>0)ShowStep(CurrentStep-1,true);}
void UAftershockNewGameTutorialWidget::SkipTutorial(){BeginFinish(TEXT("Skip"));}
void UAftershockNewGameTutorialWidget::HandleNext(){PlayNavigationClick();NextStep();}
void UAftershockNewGameTutorialWidget::HandleBack(){PlayNavigationClick();PreviousStep();}
void UAftershockNewGameTutorialWidget::HandleSkip(){PlayNavigationClick();SkipTutorial();}
FReply UAftershockNewGameTutorialWidget::NativeOnPreviewKeyDown(const FGeometry& Geometry,const FKeyEvent& Event)
{
    const FKey Key=Event.GetKey();if(Key==EKeys::Tab)return Super::NativeOnPreviewKeyDown(Geometry,Event);
    if(Event.IsRepeat())return FReply::Handled();
    if(Key==EKeys::Escape){HandleSkip();return FReply::Handled();}
    if(Key==EKeys::Left){HandleBack();return FReply::Handled();}
    if(Key==EKeys::Enter||Key==EKeys::SpaceBar||Key==EKeys::Right){HandleNext();return FReply::Handled();}
    return FReply::Handled(); // No keyboard input may leak into the paused city.
}
FReply UAftershockNewGameTutorialWidget::NativeOnMouseButtonDown(const FGeometry&,const FPointerEvent&)
{return FReply::Handled();}
