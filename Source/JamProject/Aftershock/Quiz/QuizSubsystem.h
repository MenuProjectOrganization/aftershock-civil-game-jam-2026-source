// AFTERSHOCK — Civil Game Jam 2026
// Kriz Yönetim Merkezi quiz sistemi. GameInstance subsystem — level geçişlerinde kalıcı.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Aftershock/Types/AftershockTypes.h"
#include "QuizSubsystem.generated.h"

class UDataTable;
struct FAftershockQuizSaveState;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnQuizRequested, const FQuizQuestion&, Question);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnQuizAnswered, bool, bCorrect, const FQuizQuestion&, Question);

UCLASS(BlueprintType)
class JAMPROJECT_API UQuizSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	void CaptureSaveState(FAftershockQuizSaveState& Out) const;
	bool RestoreSaveState(const FAftershockQuizSaveState& State,FText& OutError);
	void ResetForNewSession(int32 Seed);
	bool HasPendingQuestion() const { return bQuestionPending; }
	bool CanRequestQuestion(int32 Turn) const { return bQuestionPending||LastAskedTurn!=Turn; }

	virtual void Deinitialize() override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Quiz")
	TObjectPtr<UDataTable> QuestionTable;

	UPROPERTY(BlueprintAssignable, Category="Quiz")
	FOnQuizRequested OnQuizRequested;

	UPROPERTY(BlueprintAssignable, Category="Quiz")
	FOnQuizAnswered OnQuizAnswered;

	/** Rastgele bir soru çek ve OnQuizRequested broadcast et. */
	UFUNCTION(BlueprintCallable, Category="Quiz")
	bool RequestRandomQuestion(EDisasterType PreferredCategory, bool bAnyCategoryFallback = true);

	/** UI oyuncunun cevabını bildirir. Doğru/yanlış döner. */
	UFUNCTION(BlueprintCallable, Category="Quiz")
	bool SubmitAnswer(int32 ChoiceIndex);

	UFUNCTION(BlueprintPure, Category="Quiz")
	FQuizQuestion GetCurrentQuestion() const { return CurrentQuestion; }

	UFUNCTION(BlueprintPure, Category="Quiz")
	int32 GetAnsweredCount() const { return AnsweredCount; }

	UFUNCTION(BlueprintPure, Category="Quiz")
	int32 GetCorrectCount() const { return CorrectCount; }

protected:
	UPROPERTY()
	FQuizQuestion CurrentQuestion;

	TSet<FName> AskedRowNames;
	int32 AnsweredCount = 0;
	int32 CorrectCount = 0;
	bool bQuestionPending=false;
	int32 LastAskedTurn=INDEX_NONE;
	FRandomStream QuestionRandom{9182};
};
