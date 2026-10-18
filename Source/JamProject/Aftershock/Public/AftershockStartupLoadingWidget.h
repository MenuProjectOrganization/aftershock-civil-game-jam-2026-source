#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "AftershockStartupLoadingWidget.generated.h"

class UAftershockStartupPreparationSubsystem;
class UImage;
class UTextBlock;
class UProgressBar;
class UButton;

/** Opaque viewport gate. No fake progress animation and no implicit timeout skip. */
UCLASS()
class JAMPROJECT_API UAftershockStartupLoadingWidget final : public UUserWidget
{
    GENERATED_BODY()
public:
    UAftershockStartupLoadingWidget(const FObjectInitializer& Initializer);
    void SetPreparation(UAftershockStartupPreparationSubsystem* InPreparation);
    void Refresh();
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual FReply NativeOnKeyDown(const FGeometry&,const FKeyEvent&) override;
private:
    void BuildLayout();
    UFUNCTION() void RetryPressed();
    UFUNCTION() void QuitPressed();
    UPROPERTY(Transient) TObjectPtr<UAftershockStartupPreparationSubsystem> Preparation;
    UPROPERTY(Transient) TObjectPtr<UImage> StudioImage;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Title;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Tagline;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Status;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Counts;
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Failure;
    UPROPERTY(Transient) TObjectPtr<UProgressBar> Progress;
    UPROPERTY(Transient) TObjectPtr<UButton> RetryButton;
    UPROPERTY(Transient) TObjectPtr<UButton> QuitButton;
};
