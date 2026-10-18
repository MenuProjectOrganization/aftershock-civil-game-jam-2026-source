#include "Aftershock/Public/AftershockStartupLoadingWidget.h"
#include "Aftershock/Public/AftershockStartupPreparationSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
#include "Components/ScaleBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Input/Events.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

UAftershockStartupLoadingWidget::UAftershockStartupLoadingWidget(const FObjectInitializer& Initializer):Super(Initializer)
{SetIsFocusable(true);}

void UAftershockStartupLoadingWidget::SetPreparation(UAftershockStartupPreparationSubsystem* InPreparation)
{Preparation=InPreparation;Refresh();}

TSharedRef<SWidget> UAftershockStartupLoadingWidget::RebuildWidget()
{
    if(!WidgetTree||!WidgetTree->RootWidget)BuildLayout();
    return Super::RebuildWidget();
}

void UAftershockStartupLoadingWidget::BuildLayout()
{
    if(!WidgetTree)WidgetTree=NewObject<UWidgetTree>(this,TEXT("StartupWidgetTree"),RF_Transient);
    auto* Root=WidgetTree->ConstructWidget<UOverlay>();WidgetTree->RootWidget=Root;
    auto* Black=WidgetTree->ConstructWidget<UBorder>();Black->SetBrushColor(FLinearColor::Black);
    auto* BG=Root->AddChildToOverlay(Black);BG->SetHorizontalAlignment(HAlign_Fill);BG->SetVerticalAlignment(VAlign_Fill);
    auto* Fit=WidgetTree->ConstructWidget<UScaleBox>();Fit->SetStretch(EStretch::ScaleToFit);
    auto* FitSlot=Root->AddChildToOverlay(Fit);FitSlot->SetHorizontalAlignment(HAlign_Center);FitSlot->SetVerticalAlignment(VAlign_Center);
    auto* Width=WidgetTree->ConstructWidget<USizeBox>();Width->SetWidthOverride(760.f);Fit->SetContent(Width);
    auto* Column=WidgetTree->ConstructWidget<UVerticalBox>();Width->SetContent(Column);
    const auto Text=[&](const FText& Value,int32 Size,FLinearColor Colour)->UTextBlock*
    {
        auto* Label=WidgetTree->ConstructWidget<UTextBlock>();Label->SetText(Value);Label->SetColorAndOpacity(Colour);
        FSlateFontInfo Font=Label->GetFont();Font.Size=Size;Label->SetFont(Font);Label->SetJustification(ETextJustify::Center);Label->SetAutoWrapText(true);
        auto* TextLayoutSlot=Column->AddChildToVerticalBox(Label);TextLayoutSlot->SetPadding(FMargin(12.f,8.f));return Label;
    };
    auto* LogoSize=WidgetTree->ConstructWidget<USizeBox>();LogoSize->SetWidthOverride(640.f);LogoSize->SetHeightOverride(300.f);
    auto* LogoFit=WidgetTree->ConstructWidget<UScaleBox>();LogoFit->SetStretch(EStretch::ScaleToFit);LogoSize->SetContent(LogoFit);
    StudioImage=WidgetTree->ConstructWidget<UImage>();LogoFit->SetContent(StudioImage);
    auto* LogoSlot=Column->AddChildToVerticalBox(LogoSize);LogoSlot->SetHorizontalAlignment(HAlign_Center);
    Title=Text(FText::FromString(TEXT("METU NCC Cyber Security Studios")),28,FLinearColor(.94f,.91f,.83f));
    Tagline=Text(FText::FromString(TEXT("gaming for today")),14,FLinearColor(.68f,.68f,.64f));
    Text(FText::FromString(TEXT("Unreal\u00ae Engine")),17,FLinearColor(.67f,.69f,.72f));
    Status=Text(FText::FromString(TEXT("Başlangıç kaynakları hazırlanıyor")),18,FLinearColor(.86f,.86f,.86f));
    Progress=WidgetTree->ConstructWidget<UProgressBar>();Progress->SetFillColorAndOpacity(FLinearColor(.82f,.74f,.49f));
    auto* Bar=Column->AddChildToVerticalBox(Progress);Bar->SetPadding(FMargin(80.f,12.f));
    Counts=Text(FText::GetEmpty(),13,FLinearColor(.63f,.66f,.69f));
    Failure=Text(FText::GetEmpty(),15,FLinearColor(1.f,.42f,.34f));
    auto* Buttons=WidgetTree->ConstructWidget<UHorizontalBox>();auto* ButtonsSlot=Column->AddChildToVerticalBox(Buttons);ButtonsSlot->SetHorizontalAlignment(HAlign_Center);
    const auto Button=[&](const TCHAR* Label)->UButton*
    {
        auto* B=WidgetTree->ConstructWidget<UButton>();auto* T=WidgetTree->ConstructWidget<UTextBlock>();T->SetText(FText::FromString(Label));
        T->SetColorAndOpacity(FLinearColor(.9f,.87f,.79f));B->SetContent(T);auto* S=Buttons->AddChildToHorizontalBox(B);S->SetPadding(FMargin(12.f,10.f));return B;
    };
    RetryButton=Button(TEXT("Yeniden dene"));RetryButton->OnClicked.AddUniqueDynamic(this,&ThisClass::RetryPressed);
    QuitButton=Button(TEXT("Çıkış"));QuitButton->OnClicked.AddUniqueDynamic(this,&ThisClass::QuitPressed);
    Text(FText::FromString(TEXT("AFTERSHOCK uses Unreal\u00ae Engine. Unreal\u00ae is a trademark or registered trademark of Epic Games, Inc. in the United States of America and elsewhere.\nUnreal\u00ae Engine, Copyright 1998 – 2026, Epic Games, Inc. All rights reserved.")),10,FLinearColor(.36f,.38f,.4f));
    Refresh();
}

void UAftershockStartupLoadingWidget::Refresh()
{
    if(!Preparation||!Title)return;const auto& V=Preparation->GetView();
    Title->SetText(FText::FromString(V.bBranding?TEXT("METU NCC Cyber Security Studios"):TEXT("AFTERSHOCK")));
    if(StudioImage)
    {
        auto* Texture=Preparation->GetStudioTexture();if(Texture&&StudioImage->GetBrush().GetResourceObject()!=Texture)StudioImage->SetBrushFromTexture(Texture,true);
        StudioImage->SetVisibility(V.bBranding&&Texture?ESlateVisibility::HitTestInvisible:ESlateVisibility::Collapsed);
        Title->SetVisibility(V.bBranding&&Texture?ESlateVisibility::Collapsed:ESlateVisibility::HitTestInvisible);
        Tagline->SetVisibility(V.bBranding&&!Texture?ESlateVisibility::HitTestInvisible:ESlateVisibility::Collapsed);
    }
    Status->SetText(V.Status);Failure->SetText(V.Error);
    // Only the actual asynchronous asset request has a measurable fractional progress.
    // Unknown future shader/PSO work is shown as indeterminate with exact outstanding counts.
    Progress->SetIsMarquee(V.Phase!=TEXT("Assets")&&!V.bReady);
    Progress->SetPercent(V.bReady?1.f:V.AssetProgress);
    const bool Diagnostics=FParse::Param(FCommandLine::Get(),TEXT("AftershockStartupAudit"));
    if(Diagnostics) Counts->SetText(FText::FromString(FString::Printf(TEXT("Varlık %d / %d   •   PSO işleri %d / %d\nShader %d   •   Asset derlemesi %d   •   PSO bekleyen %d   •   Pipeline cache %d\nSahne %d / %d   •   PSO desteği: %s"),
        V.ResolvedAssets,V.RequestedAssets,V.CompletedPSOEvents,V.RequestedPSOEvents,V.RemainingShaders,V.RemainingAssetCompiles,V.ActivePrecacheRequests+V.ActivePipelineTasks,V.RemainingPipelineCacheJobs,V.ReadySceneChecks,V.SceneChecks,
        V.bPSOPrecachingSupported?TEXT("etkin"):TEXT("bu çalıştırmada desteklenmiyor"))));
    else Counts->SetText(V.Phase==TEXT("Assets")?FText::FromString(FString::Printf(TEXT("%d / %d kaynak"),V.ResolvedAssets,V.RequestedAssets)):FText::GetEmpty());
    Counts->SetVisibility(Diagnostics||V.Phase==TEXT("Assets")?ESlateVisibility::HitTestInvisible:ESlateVisibility::Collapsed);
    RetryButton->SetVisibility(V.bFailed?ESlateVisibility::Visible:ESlateVisibility::Collapsed);
    QuitButton->SetVisibility(V.bFailed?ESlateVisibility::Visible:ESlateVisibility::Collapsed);
}

FReply UAftershockStartupLoadingWidget::NativeOnKeyDown(const FGeometry&,const FKeyEvent&)
{return FReply::Handled();}
void UAftershockStartupLoadingWidget::RetryPressed(){if(Preparation)Preparation->Retry();}
void UAftershockStartupLoadingWidget::QuitPressed(){if(Preparation)Preparation->Quit();}
