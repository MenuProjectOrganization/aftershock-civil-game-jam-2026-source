#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "WaterGlassFrameWidget.generated.h"

class SWaterGlassScene;

/** Original vector illustration. Water here is a narrative metaphor, never hazard data. */
UCLASS()
class JAMPROJECT_API UWaterGlassFrameWidget final : public UWidget
{
	GENERATED_BODY()
public:
	void SetScene(float InProgress, double InRealSeconds, bool bInEpilogue, bool bInLoss, int32 InFlowers);
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;
protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
private:
	TSharedPtr<SWaterGlassScene> Scene;
	float Progress=0.f;
	double RealSeconds=0.;
	bool bEpilogue=false;
	bool bLoss=false;
	int32 Flowers=0;
};
