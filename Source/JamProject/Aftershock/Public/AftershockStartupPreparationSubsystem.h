#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "AftershockStartupPreparationSubsystem.generated.h"

class AAftershockPlayerController;
class UAftershockStartupLoadingWidget;
class UTexture2D;
struct FWorldContext;
struct FAftershockStartupPreparationRuntime;

/** Observed counts, never a synthetic aggregate percentage or persisted game state. */
struct FAftershockStartupPreparationView
{
    FString Phase;
    FText Status;
    FText Error;
    int32 RequestedAssets=0;
    int32 ResolvedAssets=0;
    int32 CompletedPSOEvents=0;
    int32 RequestedPSOEvents=0;
    int32 RemainingShaders=0;
    int32 RemainingAssetCompiles=0;
    int32 ActivePrecacheRequests=0;
    int32 ActivePipelineTasks=0;
    int32 RemainingPipelineCacheJobs=0;
    int32 PendingComponents=0;
    int32 ReadySceneChecks=0;
    int32 SceneChecks=0;
    uint64 EstimatedExclusiveResourceBytes=0;
    double ElapsedSeconds=0.;
    double MinimumBrandingSeconds=0.;
    double BrandingDisplayedSeconds=0.;
    float AssetProgress=0.f;
    bool bPSOPrecachingSupported=false;
    bool bBranding=false;
    bool bFailed=false;
    bool bReady=false;
};

/** Application-lifetime shared bundle; worlds/physical clocks remain their existing owners. */
UCLASS()
class JAMPROJECT_API UAftershockStartupPreparationSubsystem final : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    UAftershockStartupPreparationSubsystem(const FObjectInitializer& Initializer);
    UAftershockStartupPreparationSubsystem(FVTableHelper& Helper);
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual ~UAftershockStartupPreparationSubsystem() override;

    /** Returns true when completion will be called asynchronously behind the loading UI. */
    bool Prepare(AAftershockPlayerController* Controller,FSimpleDelegate Completion);
    void Detach(AAftershockPlayerController* Controller);
    void Retry();
    void Quit();
    bool IsGameplayBlocked(const UWorld* World) const;
    /** Call only after a live world owns its loaded presentation resources. */
    void AcknowledgeLiveWorldOwnership(UWorld* World);
    const FAftershockStartupPreparationView& GetView() const;
    UTexture2D* GetStudioTexture() const;
    bool HasCompletedBranding() const { return bBrandingCompleted; }
    static bool IsPreparationEnabled();

private:
    void PreLoadMap(const FWorldContext& Context,const FString& MapName);
    bool TickPreparation(float DeltaTime);
    void BeginBundle();
    bool CheckSceneReady();
    bool BeginGraphicsPreparation();
    bool CheckGraphicsReady();
    void Complete();
    void Fail(const FText& Reason);
    void RefreshUI();
    void WriteAudit(bool bSuccess) const;
    UPROPERTY(Transient) TObjectPtr<UAftershockStartupLoadingWidget> LoadingWidget;
    UPROPERTY(Transient) TArray<TObjectPtr<UObject>> HeldBundleAssets;
    TUniquePtr<FAftershockStartupPreparationRuntime> Runtime;
    bool bBrandingCompleted=false;
};
