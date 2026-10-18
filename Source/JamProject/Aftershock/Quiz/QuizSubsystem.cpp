#include "Aftershock/Quiz/QuizSubsystem.h"
#include "Engine/DataTable.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Engine/World.h"

bool UQuizSubsystem::RequestRandomQuestion(EDisasterType PreferredCategory, bool bAnyCategoryFallback)
{
	if(bQuestionPending){OnQuizRequested.Broadcast(CurrentQuestion);return true;}
	const auto* Economy=GetWorld()?GetWorld()->GetSubsystem<UEconomyAndStateSubsystem>():nullptr;
	if(Economy&&LastAskedTurn==Economy->CurrentTurn)return false;
	if (!QuestionTable)
	{
		QuestionTable = LoadObject<UDataTable>(nullptr, TEXT("/Game/Aftershock/Quiz/DT_QuizQuestions.DT_QuizQuestions"));
	}

	if (QuestionTable && QuestionTable->GetRowNames().Num() > 0)
	{
		TArray<FName> RowNames = QuestionTable->GetRowNames();

		// 1. tercih: kategoriye göre + sorulmamış
		TArray<FName> Candidates;
		for (const FName& Row : RowNames)
		{
			if (AskedRowNames.Contains(Row)) continue;
			const FQuizQuestion* Q = QuestionTable->FindRow<FQuizQuestion>(Row, TEXT("QuizPick"));
			if (Q && Q->Category == PreferredCategory) Candidates.Add(Row);
		}

		// Fallback: kategori önemsiz, sorulmamış herhangi
		if (Candidates.Num() == 0 && bAnyCategoryFallback)
		{
			for (const FName& Row : RowNames)
			{
				if (!AskedRowNames.Contains(Row)) Candidates.Add(Row);
			}
		}

		// Last resort: tüm sorular soruldu, baştan başla
		if (Candidates.Num() == 0)
		{
			AskedRowNames.Reset();
			Candidates = RowNames;
		}

		if (Candidates.Num() > 0)
		{
			const FName Pick = Candidates[QuestionRandom.RandRange(0, Candidates.Num() - 1)];
			const FQuizQuestion* Picked = QuestionTable->FindRow<FQuizQuestion>(Pick, TEXT("QuizPick"));
			if (Picked)
			{
				CurrentQuestion = *Picked;
				AskedRowNames.Add(Pick);
				bQuestionPending=true;LastAskedTurn=Economy?Economy->CurrentTurn:INDEX_NONE;
				OnQuizRequested.Broadcast(CurrentQuestion);
				return true;
			}
		}
	}

	// Yerleşik yedek soru havuzu (DataTable bulunamazsa bile %100 çalışır)
	static const TArray<FQuizQuestion> BuiltinPool = []() {
		TArray<FQuizQuestion> Pool;

		FQuizQuestion Q1;
		Q1.Category = EDisasterType::Earthquake;
		Q1.Question = FText::FromString(TEXT("Deprem anında bina içindesiniz ve sarsıntı başladı. İlk yapmanız gereken hayati hareket nedir?"));
		Q1.Choices = {
			FText::FromString(TEXT("A) Hızla merdivenlere koşarak binayı terk etmek")),
			FText::FromString(TEXT("B) Asansöre binip zemin kata inmek")),
			FText::FromString(TEXT("C) Sağlam bir eşyanın yanında Çök-Kapan-Tutun pozisyonu almak")),
			FText::FromString(TEXT("D) Balkona veya pencere kenarına çıkıp yardım istemek"))
		};
		Q1.CorrectIndex = 2;
		Q1.InfoNote = FText::FromString(TEXT("Depremlerde en çok yaralanma, sarsıntı sırasında panikle kaçarken merdivenlerin çökmesi veya eşyaların düşmesi sonucu yaşanır."));
		Pool.Add(Q1);

		FQuizQuestion Q2;
		Q2.Category = EDisasterType::Flood;
		Q2.Question = FText::FromString(TEXT("Sel sularıyla kaplı bir yolda yürüyerek veya araçla geçmek neden ölümcül bir risk taşır?"));
		Q2.Choices = {
			FText::FromString(TEXT("A) Suların sıcaklık seviyesinin çok düşük olması")),
			FText::FromString(TEXT("B) Sel sularının elektrik iletmemesi")),
			FText::FromString(TEXT("C) Sadece 15 cm derinliğindeki akan suyun bir yetişkini devirebilmesi")),
			FText::FromString(TEXT("D) Suların arabanın boyasını bozması"))
		};
		Q2.CorrectIndex = 2;
		Q2.InfoNote = FText::FromString(TEXT("15 cm akan su bir insanı, 30 cm akan su ise çoğu aracı sürüklemeye yeterlidir. Sel sularına asla girilmemelidir."));
		Pool.Add(Q2);

		FQuizQuestion Q3;
		Q3.Category = EDisasterType::Wind;
		Q3.Question = FText::FromString(TEXT("Şiddetli fırtına ve kasırga sırasında bina içinde nerede durulmalıdır?"));
		Q3.Choices = {
			FText::FromString(TEXT("A) Pencerelere yakın durup dışarıyı izlemek")),
			FText::FromString(TEXT("B) Çatı katına veya balkona çıkmak")),
			FText::FromString(TEXT("C) Pencerelerden uzak, iç duvar veya koridorda durmak")),
			FText::FromString(TEXT("D) Dışarı çıkıp açık alanda beklemek"))
		};
		Q3.CorrectIndex = 2;
		Q3.InfoNote = FText::FromString(TEXT("Fırtınalarda kırılan camlar ve uçuşan döküntüler en büyük yaralanma kaynağıdır. Pencerelerden uzak durulmalıdır."));
		Pool.Add(Q3);

		return Pool;
	}();

	const int32 Index = QuestionRandom.RandRange(0, BuiltinPool.Num() - 1);
	CurrentQuestion = BuiltinPool[Index];
	bQuestionPending=true;LastAskedTurn=Economy?Economy->CurrentTurn:INDEX_NONE;
	OnQuizRequested.Broadcast(CurrentQuestion);
	return true;
}

bool UQuizSubsystem::SubmitAnswer(int32 ChoiceIndex)
{
	if(!bQuestionPending||!CurrentQuestion.Choices.IsValidIndex(ChoiceIndex))return false;
	bQuestionPending=false;
	const bool bCorrect = (ChoiceIndex == CurrentQuestion.CorrectIndex);
	AnsweredCount++;
	if (bCorrect)
	{
		CorrectCount++;

		// Ödül uygula (+1.000 TL hazine, +5 memnuniyet)
		if (UWorld* World = GetWorld())
		{
			if (UEconomyAndStateSubsystem* Economy = World->GetSubsystem<UEconomyAndStateSubsystem>())
			{
				Economy->TreasuryTL += 1000;
				Economy->PublicSatisfaction = FMath::Clamp(Economy->PublicSatisfaction + 5.0f, 0.0f, 100.0f);
				Economy->OnEconomyUpdated.Broadcast(Economy->TreasuryTL, Economy->PublicSatisfaction, Economy->CurrentTurn);
			}
		}
	}

	OnQuizAnswered.Broadcast(bCorrect, CurrentQuestion);
	return bCorrect;
}

void UQuizSubsystem::ResetForNewSession(int32 Seed)
{CurrentQuestion={};AskedRowNames.Reset();AnsweredCount=0;CorrectCount=0;bQuestionPending=false;LastAskedTurn=INDEX_NONE;QuestionRandom.Initialize(Seed^9182);}

void UQuizSubsystem::Deinitialize()
{
	OnQuizRequested.Clear();
	OnQuizAnswered.Clear();
	AskedRowNames.Reset();
	AnsweredCount = 0;
	CorrectCount = 0;
	CurrentQuestion = FQuizQuestion();
	bQuestionPending = false;
	LastAskedTurn = INDEX_NONE;
	QuestionRandom.Initialize(0);
	Super::Deinitialize();
}
