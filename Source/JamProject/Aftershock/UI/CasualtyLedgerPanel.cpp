#include "Aftershock/UI/CasualtyLedgerPanel.h"
#include "Aftershock/Narrative/CasualtyLedgerGenerator.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Engine/World.h"

namespace
{
	UTextBlock* LedgerText(UWidgetTree* Tree,FText Copy,int32 Size,FLinearColor Color)
	{
		auto* Text=Tree->ConstructWidget<UTextBlock>();Text->SetText(Copy);Text->SetColorAndOpacity(Color);Text->SetAutoWrapText(true);Text->SetWrapTextAt(480);Text->SetVisibility(ESlateVisibility::HitTestInvisible);
		FSlateFontInfo Font=Text->GetFont();Font.Size=Size;Font.TypefaceFontName=Size>=16?TEXT("Bold"):TEXT("Regular");Text->SetFont(Font);return Text;
	}
	void StyleLedgerButton(UButton* Button)
	{
		FButtonStyle Style=Button->GetStyle();Style.SetNormal(FSlateRoundedBoxBrush(FLinearColor(.025f,.065f,.070f),4.f,FLinearColor(.22f,.36f,.32f),1.f));Style.SetHovered(FSlateRoundedBoxBrush(FLinearColor(.075f,.14f,.12f),4.f,FLinearColor(.94f,.64f,.29f),1.f));Style.SetPressed(FSlateRoundedBoxBrush(FLinearColor(.018f,.04f,.04f),4.f));Style.SetNormalPadding(FMargin(10,8));Style.SetPressedPadding(FMargin(10,9,10,7));Button->SetStyle(Style);
	}
}
void UCasualtyFlowerButton::Configure(UCasualtyLedgerPanel* InPanel,int32 InResidentID)
{ Panel=InPanel;ResidentID=InResidentID;OnClicked.AddUniqueDynamic(this,&UCasualtyFlowerButton::HandleClick); }
void UCasualtyFlowerButton::HandleClick(){if(Panel.IsValid())Panel->LeaveFlower(ResidentID);}
TSharedRef<SWidget> UCasualtyLedgerPanel::RebuildWidget(){if(!WidgetTree||!WidgetTree->RootWidget)BuildLayout();return Super::RebuildWidget();}
void UCasualtyLedgerPanel::BuildLayout()
{
	if(!WidgetTree)WidgetTree=NewObject<UWidgetTree>(this,TEXT("WidgetTree"));
	UVerticalBox* Stack=WidgetTree->ConstructWidget<UVerticalBox>();WidgetTree->RootWidget=Stack;
	Summary=LedgerText(WidgetTree,FText::GetEmpty(),13,FLinearColor(.87f,.88f,.80f));auto* SummarySlot=Stack->AddChildToVerticalBox(Summary);SummarySlot->SetPadding(FMargin(0,0,0,10));
	Scroll=WidgetTree->ConstructWidget<UScrollBox>();Rows=WidgetTree->ConstructWidget<UVerticalBox>();Scroll->AddChild(Rows);
	USizeBox* Viewport=WidgetTree->ConstructWidget<USizeBox>();Viewport->SetHeightOverride(310);Viewport->SetContent(Scroll);Stack->AddChildToVerticalBox(Viewport);
	UHorizontalBox* LedgerNavigation=WidgetTree->ConstructWidget<UHorizontalBox>();auto* NavSlot=Stack->AddChildToVerticalBox(LedgerNavigation);NavSlot->SetPadding(FMargin(0,12,0,6));
	Previous=WidgetTree->ConstructWidget<UButton>();StyleLedgerButton(Previous);Previous->SetContent(LedgerText(WidgetTree,NSLOCTEXT("Aftershock","LedgerPrevious","← ÖNCEKİ"),11,FLinearColor(.92f,.87f,.72f)));LedgerNavigation->AddChildToHorizontalBox(Previous);Previous->OnClicked.AddUniqueDynamic(this,&UCasualtyLedgerPanel::HandlePrevious);
	PageText=LedgerText(WidgetTree,FText::GetEmpty(),11,FLinearColor(.65f,.75f,.72f));auto* PageSlot=LedgerNavigation->AddChildToHorizontalBox(PageText);PageSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));PageSlot->SetHorizontalAlignment(HAlign_Center);PageSlot->SetVerticalAlignment(VAlign_Center);
	Next=WidgetTree->ConstructWidget<UButton>();StyleLedgerButton(Next);Next->SetContent(LedgerText(WidgetTree,NSLOCTEXT("Aftershock","LedgerNext","SONRAKİ →"),11,FLinearColor(.92f,.87f,.72f)));LedgerNavigation->AddChildToHorizontalBox(Next);Next->OnClicked.AddUniqueDynamic(this,&UCasualtyLedgerPanel::HandleNext);
	Feedback=LedgerText(WidgetTree,NSLOCTEXT("Aftershock","LedgerIdentityNotice","Ad ve yaş, gerçek vatandaş ID'sine bağlı oyun kimliğidir. Meslek ve yapı koşulları mevcutsa olay anındaki kayıttır; bilinmeyen geçmiş belirtilir."),10,FLinearColor(.58f,.70f,.68f));Stack->AddChildToVerticalBox(Feedback);
	UButton* Close=WidgetTree->ConstructWidget<UButton>();StyleLedgerButton(Close);Close->SetContent(LedgerText(WidgetTree,NSLOCTEXT("Aftershock","LedgerClose","DEFTERİ KAPAT"),12,FLinearColor(.94f,.88f,.73f)));auto* CloseSlot=Stack->AddChildToVerticalBox(Close);CloseSlot->SetPadding(FMargin(0,10,0,0));Close->OnClicked.AddUniqueDynamic(this,&UCasualtyLedgerPanel::HandleClose);
}
void UCasualtyLedgerPanel::NativeConstruct()
{
	Super::NativeConstruct();if(auto* Ledger=GetWorld()?GetWorld()->GetSubsystem<UCasualtyLedgerGenerator>():nullptr)Ledger->OnLedgerChanged.AddUniqueDynamic(this,&UCasualtyLedgerPanel::RefreshLedger);RefreshLedger();
}
void UCasualtyLedgerPanel::NativeDestruct()
{
	if(auto* Ledger=GetWorld()?GetWorld()->GetSubsystem<UCasualtyLedgerGenerator>():nullptr)Ledger->OnLedgerChanged.RemoveDynamic(this,&UCasualtyLedgerPanel::RefreshLedger);Super::NativeDestruct();
}
void UCasualtyLedgerPanel::RefreshLedger()
{
	if(!Rows||!Summary)return;
	const auto* Ledger=GetWorld()?GetWorld()->GetSubsystem<UCasualtyLedgerGenerator>():nullptr;
	if(!Ledger)
	{
		Summary->SetText(NSLOCTEXT("Aftershock","LedgerUnavailable","Kayıp defteri şu an hazır değil."));
		if(Previous)Previous->SetIsEnabled(false);
		if(Next)Next->SetIsEnabled(false);
		for(auto& Card:PooledCards)if(Card)Card->SetVisibility(ESlateVisibility::Collapsed);
		if(EmptyNotice)EmptyNotice->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	const int32 Count=Ledger->GetEntries().Num();
	const int32 PageCount=FMath::Max(1,(Count+19)/20);
	Page=FMath::Clamp(Page,0,PageCount-1);
	Summary->SetText(FText::Format(NSLOCTEXT("Aftershock","LedgerSummary","Bu şehirde yaşayan: {0}  ·  Can kaybı: {1}\nDefterdeki gerçek kayıp kaydı: {2}  ·  Bırakılan çiçek: {3}"),FText::AsNumber(Ledger->GetSessionSurvivors()),FText::AsNumber(Ledger->GetSessionDeaths()),FText::AsNumber(Count),FText::AsNumber(Ledger->GetFlowerTributeCount())));
	if(Count!=Ledger->GetSessionDeaths()&&Feedback)Feedback->SetText(NSLOCTEXT("Aftershock","LedgerCountMismatch","Kayıp sayısı ile defter dökümü eşleşmiyor. Eksik kayıtlar için isim veya hikâye uydurulmadı."));

	if(!EmptyNotice)
	{
		EmptyNotice=LedgerText(WidgetTree,NSLOCTEXT("Aftershock","LedgerEmpty","Henüz bu şehirde can kaybı kaydedilmedi."),16,FLinearColor(.66f,.80f,.70f));
		Rows->AddChildToVerticalBox(EmptyNotice);
	}

	if(Count==0)
	{
		EmptyNotice->SetVisibility(ESlateVisibility::Visible);
		for(auto& Card:PooledCards)if(Card)Card->SetVisibility(ESlateVisibility::Collapsed);
	}
	else
	{
		EmptyNotice->SetVisibility(ESlateVisibility::Collapsed);
		const auto Entries=Ledger->GetEntriesPage(Page*20,20);
		for(int32 I=0;I<Entries.Num();++I)
		{
			if(I>=PooledCards.Num())
			{
				UBorder* Card=WidgetTree->ConstructWidget<UBorder>();
				Card->SetBrush(FSlateRoundedBoxBrush(FLinearColor(.025f,.052f,.056f),5.f,FLinearColor(.11f,.19f,.19f),1.f));
				Card->SetPadding(FMargin(13,11));
				auto* CardSlot=Rows->AddChildToVerticalBox(Card);
				CardSlot->SetPadding(FMargin(0,0,0,8));
				UVerticalBox* Copy=WidgetTree->ConstructWidget<UVerticalBox>();
				Card->SetContent(Copy);
				UTextBlock* NameAge=LedgerText(WidgetTree,FText::GetEmpty(),16,FLinearColor(.93f,.91f,.81f));
				Copy->AddChildToVerticalBox(NameAge);
				UTextBlock* Occ=LedgerText(WidgetTree,FText::GetEmpty(),11,FLinearColor(.82f,.66f,.43f));
				Copy->AddChildToVerticalBox(Occ);
				UTextBlock* Story=LedgerText(WidgetTree,FText::GetEmpty(),12,FLinearColor(.73f,.80f,.76f));
				auto* StorySlot=Copy->AddChildToVerticalBox(Story);
				StorySlot->SetPadding(FMargin(0,7,0,8));
				auto* Flower=WidgetTree->ConstructWidget<UCasualtyFlowerButton>();
				StyleLedgerButton(Flower);
				Flower->SetToolTipText(NSLOCTEXT("Aftershock","FlowerFacts","Bu kayıtlı vatandaşı an. Her gerçek kayıp için bir çiçek kaydedilir; para, ödül veya nüfus değiştirmez."));
				UTextBlock* FlowerLabel=LedgerText(WidgetTree,FText::GetEmpty(),10,FLinearColor(.89f,.80f,.63f));
				Flower->SetContent(FlowerLabel);
				Copy->AddChildToVerticalBox(Flower);

				PooledCards.Add(Card);
				PooledNameAgeTexts.Add(NameAge);
				PooledOccupationTexts.Add(Occ);
				PooledStoryTexts.Add(Story);
				PooledFlowerButtons.Add(Flower);
				PooledFlowerTexts.Add(FlowerLabel);
			}

			const auto& Entry=Entries[I];
			PooledNameAgeTexts[I]->SetText(FText::Format(NSLOCTEXT("Aftershock","LedgerNameAge","{0}  ·  {1} yaşında"),Entry.FullName,FText::AsNumber(Entry.Age)));
			PooledOccupationTexts[I]->SetText(Entry.Occupation);
			PooledStoryTexts[I]->SetText(Entry.Narrative);
			FString Evidence=Entry.Cause==ECitizenCasualtyCause::HistoricalUnknown
				?FString::Printf(TEXT("Vatandaş #%d | Önceki kayıt: ölüm zamanı ve nedeni bilinmiyor.\nTeknik kayıt geçişi kimliği: %s (gerçek afet olayı veya ölüm turu değildir)."),Entry.ResidentID,*Entry.DisasterEventID.ToString())
				:FString::Printf(TEXT("Vatandaş #%d | Tur %d | Olay %s"),Entry.ResidentID,Entry.DisasterTurn,*Entry.DisasterEventID.ToString());
			if(Entry.bHasBuildingSnapshot)Evidence+=FString::Printf(TEXT("\nYapı #%d | Parsel %d,%d\n%s | Malzeme: %s | Temel: %s | Güçlendirme: %s"),Entry.BuildingID,Entry.BuildingCoord.X,Entry.BuildingCoord.Y,*Entry.BuildingDisplayName.ToString(),*StaticEnum<EMaterialType>()->GetDisplayNameTextByValue(static_cast<int64>(Entry.Material)).ToString(),*StaticEnum<EFoundationQuality>()->GetDisplayNameTextByValue(static_cast<int64>(Entry.Foundation)).ToString(),Entry.bIsRetrofitted?TEXT("Var"):TEXT("Yok"));
			else Evidence+=TEXT("\nBu kayıt için afet anı yapı bilgisi yok; malzeme veya temel varsayılmadı.");
			PooledCards[I]->SetToolTipText(FText::FromString(Evidence));
			PooledFlowerButtons[I]->Configure(this,Entry.ResidentID);
			const bool HasFlower=Ledger->HasFlowerTribute(Entry.ResidentID);
			PooledFlowerButtons[I]->SetIsEnabled(!HasFlower);
			PooledFlowerTexts[I]->SetText(HasFlower?NSLOCTEXT("Aftershock","FlowerAlreadyPlaced","ÇİÇEK BIRAKILDI"):NSLOCTEXT("Aftershock","FlowerPlace","BİR ÇİÇEK BIRAK"));
			PooledCards[I]->SetVisibility(ESlateVisibility::Visible);
		}
		for(int32 I=Entries.Num();I<PooledCards.Num();++I)
		{
			PooledCards[I]->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
	if(PageText)PageText->SetText(FText::Format(NSLOCTEXT("Aftershock","LedgerPage","{0} / {1}"),FText::AsNumber(Page+1),FText::AsNumber(PageCount)));
	if(Previous)Previous->SetIsEnabled(Page>0);
	if(Next)Next->SetIsEnabled(Page+1<PageCount);
}
void UCasualtyLedgerPanel::LeaveFlower(int32 ResidentID)
{
	if(auto* Ledger=GetWorld()?GetWorld()->GetSubsystem<UCasualtyLedgerGenerator>():nullptr){const bool Added=Ledger->PlaceFlower(ResidentID);if(Feedback)Feedback->SetText(Added?NSLOCTEXT("Aftershock","FlowerAdded","Çiçeğin bu kişinin anısına kaydedildi."):NSLOCTEXT("Aftershock","FlowerNotAdded","Bu anma zaten kayıtlı veya geçerli bir kayıp kimliği yok."));RefreshLedger();}
}
void UCasualtyLedgerPanel::HandlePrevious(){Page=FMath::Max(0,Page-1);RefreshLedger();if(Scroll)Scroll->ScrollToStart();}
void UCasualtyLedgerPanel::HandleNext(){++Page;RefreshLedger();if(Scroll)Scroll->ScrollToStart();}
void UCasualtyLedgerPanel::HandleClose(){OnCloseRequested.Broadcast();}
