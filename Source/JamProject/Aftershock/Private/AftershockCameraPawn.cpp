#include "Aftershock/Public/AftershockCameraPawn.h"
#include "GameFramework/GameUserSettings.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
// AFTERSHOCK — Civil Game Jam 2026
// 2.5D Şehir Yöneticisi Kamerası ve İnşaat Önizleme Kontrolcüsü Uygulaması

#include "Aftershock/Public/TerrainGridSubsystem.h"
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "Aftershock/Public/EconomyAndStateSubsystem.h"
#include "Aftershock/Public/RoadNetworkSubsystem.h"
#include "Aftershock/Public/CrowdRenderSubsystem.h"
#include "Aftershock/Game/AftershockPlayerController.h"
#include "Aftershock/Public/AftershockMainHUDWidget.h"
#include "Aftershock/Public/AftershockScenarioSubsystem.h"
#include "Aftershock/Public/AftershockSaveSubsystem.h"
#include "Aftershock/Public/AftershockSettingsSubsystem.h"
#include "Components/InputComponent.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Engine/GameInstance.h"
#include "AftershockGlobeSurface.h"
#include "AftershockArchitecturalRenderer.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "HAL/PlatformTime.h"

AAftershockCameraPawn::AAftershockCameraPawn()
{
	// SIFIR TICK KURALI: PrimaryActorTick tamamen devre dışı bırakılır
	PrimaryActorTick.bCanEverTick = false;

	RootScene = CreateDefaultSubobject<USceneComponent>(TEXT("RootScene"));
	SetRootComponent(RootScene);

	// 2.5D God-View SpringArm kurulumu
	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(RootScene);
	SpringArm->SetRelativeRotation(FRotator(-50.0f, 0.0f, 0.0f)); // Pitch açısı -50 derece
	SpringArm->TargetArmLength = 5000.0f;
	SpringArm->bDoCollisionTest = false; // Binalara çarpıp kamerayı zıplatmasın
	SpringArm->bInheritPitch = false;
	SpringArm->bInheritRoll = false;
	SpringArm->bInheritYaw = true;
	SpringArm->bEnableCameraLag = true;
	SpringArm->CameraLagSpeed = 10.0f;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
	Camera->bUsePawnControlRotation = false;

	// İnşaat Önizleme (Ghost Mesh) bileşeni
	PlacementGhostMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlacementGhostMesh"));
	PlacementGhostMesh->SetupAttachment(RootScene);
	PlacementGhostMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PlacementGhostMesh->SetCastShadow(false);
	PlacementGhostMesh->SetVisibility(false);
	PlacementGhostMesh->SetAbsolute(true, true, true);
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GhostMaterialFinder(
		TEXT("/Game/Buildings/Materials/M_BuildingGhost.M_BuildingGhost"));
	if (GhostMaterialFinder.Succeeded())
	{
		PlacementGhostMaterial = GhostMaterialFinder.Object;
		PlacementGhostMesh->SetMaterial(0, PlacementGhostMaterial);
	}
	RoadPreviewISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("RoadPreviewISM"));
	RoadPreviewISM->SetupAttachment(RootScene);
	RoadPreviewISM->SetAbsolute(true, true, true);
	RoadPreviewISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RoadPreviewISM->SetCastShadow(false);
	RoadPreviewISM->SetVisibility(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PreviewCellMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (PreviewCellMesh.Succeeded()) RoadPreviewISM->SetStaticMesh(PreviewCellMesh.Object);
	if (PlacementGhostMaterial) RoadPreviewISM->SetMaterial(0, PlacementGhostMaterial);

	DesiredArmLength = 5000.0f;
}

void AAftershockCameraPawn::InitializeCameraForGrid(int32 GridWidth, int32 GridHeight, float InCellSize)
{
	const float CenterX = static_cast<float>(GridWidth) * InCellSize * 0.5f;
	const float CenterY = static_cast<float>(GridHeight) * InCellSize * 0.5f;
	const UTerrainGridSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	FVector TargetCenter = (Terrain ? Terrain->GridOrigin : FVector::ZeroVector) + FVector(CenterX, CenterY, 0.0f);
	// A real DEM can start kilometres above sea level even before any settlement
	// is feasible. Anchor the initial view to that surface, including empty maps.
	if (Terrain && Terrain->IsUsingRealGeography())
	{
		double Latitude = 0., Longitude = 0.; FVector SurfacePoint;
		if (Terrain->TryWorldToGeographic(TargetCenter, Latitude, Longitude)
			&& Terrain->TryGeographicToWorld(Latitude, Longitude, SurfacePoint))
			TargetCenter.Z = SurfacePoint.Z;
	}

	LocalMapFocus = TargetCenter;
	const float GridSpan = FMath::Max(GridWidth, GridHeight) * InCellSize;
	PlanetRadius = Terrain ? GetAftershockGlobeLayout(*Terrain).RadiusCm : FMath::Max(1000.0f, GridSpan * 8.0f);
	PlanetCenter = Terrain ? GetAftershockGlobeLayout(*Terrain).Center : TargetCenter - FVector(0, 0, PlanetRadius);
	CityOverviewStartDistance = GridSpan * 0.6f;
	MaxArmLength = PlanetRadius * 6.0f;
	CurrentOverviewBlend = 0.0f;
	PlanetOrbitPitch = -48.0f;
	SetActorLocation(LocalMapFocus);

	if (SpringArm)
	{
		SpringArm->SetRelativeRotation(FRotator(-48.0f, -45.0f, 0.0f));
		SpringArm->TargetArmLength = 8500.0f;
		DesiredArmLength = 8500.0f;
		CurrentZoomDistance = DesiredArmLength;
		SavedLocalZoomDistance = DesiredArmLength;
	}

	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		PC->SetControlRotation(FRotator(-48.0f, -45.0f, 0.0f));
	}
}

void AAftershockCameraPawn::BeginPlay()
{
	Super::BeginPlay();
#if !UE_BUILD_SHIPPING
	int32 AuditQuality=INDEX_NONE;
	if(FParse::Value(FCommandLine::Get(),TEXT("AftershockGraphicsQuality="),AuditQuality)&&AuditQuality>=0&&AuditQuality<=4)
		if(auto* Settings=UGameUserSettings::GetGameUserSettings())
		{Settings->SetOverallScalabilityLevel(AuditQuality);Settings->ApplyNonResolutionSettings();UE_LOG(LogTemp,Display,TEXT("AFTERSHOCK_GRAPHICS_FIXTURE quality=%d settingsSaved=0"),AuditQuality);}
#endif
	ApplyUserPreferences();

	if (UWorld* World = GetWorld())
	{
		auto* Saves=World->GetGameInstance()?World->GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>():nullptr;
		bool bLoaded=false;
		if(Saves&&Saves->HasPendingLoad())
		{
			FText Error;bLoaded=Saves->RestorePendingWorld(World,this,Error);
			if(!bLoaded)
			{
				UE_LOG(LogTemp,Error,TEXT("AFTERSHOCK_LOAD_FAILED %s newPopulationSeeded=0"),*Error.ToString());
				World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this,[this]()
				{if(auto* PC=Cast<AAftershockPlayerController>(GetWorld()?GetWorld()->GetFirstPlayerController():nullptr))PC->PresentLoadFailureRecovery();}));
				return;
			}
		}
		if(!bLoaded)
		{
		if (UAftershockScenarioSubsystem* Scenarios = World->GetSubsystem<UAftershockScenarioSubsystem>())
		{
			if (!Scenarios->PrepareScenarioBeforeSettlement())
			{
				World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
				{
					if (auto* PC = Cast<AAftershockPlayerController>(GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr))
						PC->PresentLoadFailureRecovery();
				}));
				return;
			}
		}
		if (UTerrainGridSubsystem* Terrain = World->GetSubsystem<UTerrainGridSubsystem>())
		{
			InitializeCameraForGrid(Terrain->GridWidth, Terrain->GridHeight, Terrain->CellSize);

			// Terrain ve nehir ISM görselinin oluşturulduğunu garanti et
			if (UCrowdRenderSubsystem* CrowdSys = World->GetSubsystem<UCrowdRenderSubsystem>())
			{
				CrowdSys->EnsureTerrainVisualBuilt();
			}

			// Başlangıç binalarını ve nüfusunu tohumla
			if (UBuildingManagerSubsystem* BuildingMgr = World->GetSubsystem<UBuildingManagerSubsystem>())
			{
				BuildingMgr->SeedInitialSettlement();
				TArray<FBuildingInstance> Buildings;
				BuildingMgr->GetAllBuildings(Buildings);
				FBox CityBounds(ForceInit);
				for (const FBuildingInstance& Building : Buildings)
				{
					CityBounds += Terrain->GridToWorld(Building.GridCoords, true);
				}
				if (CityBounds.IsValid && SpringArm)
				{
					// A 3D box centre can lie below a steep DEM or between two islands.
					// Focus on a real occupied plot nearest the plan centre, then sample
					// the canonical surface; never move buildings or alter saved cameras.
					LocalMapFocus = CityBounds.GetCenter();
					double NearestDistance = TNumericLimits<double>::Max();
					for (const FBuildingInstance& Building : Buildings)
					{
						const FVector Candidate = Terrain->GridToWorld(Building.GridCoords, true);
						const double Distance = FVector::DistSquaredXY(Candidate, CityBounds.GetCenter());
						if (Distance < NearestDistance) { NearestDistance = Distance; LocalMapFocus = Candidate; }
					}
					if (Terrain->IsUsingRealGeography())
					{
						double Latitude=0., Longitude=0.; FVector SurfacePoint;
						if (Terrain->TryWorldToGeographic(LocalMapFocus,Latitude,Longitude)
							&& Terrain->TryGeographicToWorld(Latitude,Longitude,SurfacePoint)) LocalMapFocus.Z=SurfacePoint.Z;
					}
					SetActorLocation(LocalMapFocus);
					const FVector Span = CityBounds.GetSize();
					// A large city must start in local management view. A city-wide
					// bounding-box zoom can otherwise cross the globe threshold before
					// the player has issued their first command.
					const float InitialLocalLimit=FMath::Min(MaxArmLength,CityOverviewStartDistance*.86f);
					SpringArm->TargetArmLength = FMath::Clamp(FMath::Max(Span.X, Span.Y) * 1.05f + Terrain->CellSize, 5200.0f, FMath::Max(5200.f,InitialLocalLimit));
					if (Terrain->IsCoastalWorld()) SpringArm->TargetArmLength = FMath::Max(SpringArm->TargetArmLength, 12500.f);
				}
			}
		}

		}
		if (SpringArm)
		{
			DesiredArmLength = SpringArm->TargetArmLength;
			CurrentZoomDistance = DesiredArmLength;
			if(!bLoaded)SavedLocalZoomDistance = DesiredArmLength;
		}
		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_RUNTIME_CAMERA actor=%s actorLocation=%s actorRotation=%s cameraLocation=%s cameraRotation=%s armLength=%.1f"),
			*GetPathName(),
			*GetActorLocation().ToCompactString(),
			*GetActorRotation().ToCompactString(),
			Camera ? *Camera->GetComponentLocation().ToCompactString() : TEXT("None"),
			Camera ? *Camera->GetComponentRotation().ToCompactString() : TEXT("None"),
			SpringArm ? SpringArm->TargetArmLength : -1.0f);

		// 60 Hz optimize timer ile akıcı zoom ve ghost mesh takibi
		World->GetTimerManager().SetTimer(
			CameraTimerHandle,
			FTimerDelegate::CreateUObject(this, &AAftershockCameraPawn::UpdateCameraTick),
			0.016f,
			true
		);
	}
}

void AAftershockCameraPawn::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ArchitecturalPreview.Reset();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CameraTimerHandle);
		World->GetTimerManager().ClearTimer(ShakeTimerHandle);
	}

	Super::EndPlay(EndPlayReason);
}

void AAftershockCameraPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	BindUserInput(PlayerInputComponent);
	ApplyUserPreferences();
}

void AAftershockCameraPawn::BindUserInput(UInputComponent* PlayerInputComponent)
{
	const auto* Preferences=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSettingsSubsystem>():nullptr;
	static const FAftershockUserPreferences Defaults;
	const auto Key=[Preferences](EAftershockUserAction Action){return Preferences?Preferences->GetKey(Action):Defaults.Keys[static_cast<int32>(Action)];};
	if (PlayerInputComponent)
	{
		PlayerInputComponent->KeyBindings.RemoveAll([this](const FInputKeyBinding& Binding){return Binding.KeyDelegate.IsBoundToObject(this);});
		PlayerInputComponent->AxisKeyBindings.RemoveAll([this](const FInputAxisKeyBinding& Binding){return Binding.AxisDelegate.IsBoundToObject(this);});
		PlayerInputComponent->BindAxisKey(EKeys::MouseWheelAxis, this, &AAftershockCameraPawn::HandleZoomAxis);
		// MouseWheelAxis is the single wheel binding; button events would double zoom.

		PlayerInputComponent->BindKey(Key(EAftershockUserAction::RotateBuilding), IE_Pressed, this, &AAftershockCameraPawn::HandleRotateGhost);
		PlayerInputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &AAftershockCameraPawn::HandleLeftClick);
		PlayerInputComponent->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &AAftershockCameraPawn::HandleRightClick);
		FInputKeyBinding& EscapeBinding = PlayerInputComponent->BindKey(EKeys::Escape, IE_Pressed, this, &AAftershockCameraPawn::HandleEscapePressed);
		EscapeBinding.bExecuteWhenPaused = true;
		PlayerInputComponent->BindKey(Key(EAftershockUserAction::CycleFoundation), IE_Pressed, this, &AAftershockCameraPawn::HandleCycleFoundation);
		PlayerInputComponent->BindKey(Key(EAftershockUserAction::Overview), IE_Pressed, this, &AAftershockCameraPawn::HandleToggleOverview);
		PlayerInputComponent->BindKey(Key(EAftershockUserAction::IncreaseFloors), IE_Pressed, this, &AAftershockCameraPawn::HandleIncreaseFloors);
		PlayerInputComponent->BindKey(Key(EAftershockUserAction::DecreaseFloors), IE_Pressed, this, &AAftershockCameraPawn::HandleDecreaseFloors);
	}
}

void AAftershockCameraPawn::ApplyUserPreferences()
{
	const auto* Settings=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSettingsSubsystem>():nullptr;if(!Settings)return;
	const auto& P=Settings->GetPreferences();UserPanSensitivity=P.PanSensitivity;UserRotationSensitivity=P.RotationSensitivity;UserZoomSensitivity=P.ZoomSensitivity;
	bUserInvertWheel=P.bInvertWheel;bUserSmoothCamera=P.bSmoothCamera;bUserReduceMotion=P.bReduceMotion;bUserColorSafePlacement=P.bColorSafePlacement;
	if(SpringArm)SpringArm->bEnableCameraLag=bUserSmoothCamera&&!bUserReduceMotion;
	if(bUserReduceMotion&&ShakeRemainingTime>0.f)
	{ShakeRemainingTime=0.f;if(GetWorld())GetWorld()->GetTimerManager().ClearTimer(ShakeTimerHandle);if(SpringArm)SpringArm->SetRelativeRotation(BaseSpringArmRotation);}
	BindUserInput(InputComponent);
}

void AAftershockCameraPawn::SetNarrativeInputBlocked(bool bBlocked)
{
	if(!bBlocked&&bNarrativeTransitionActive){NarrativeStartedRealSeconds=FPlatformTime::Seconds()-NarrativeDuration;AdvanceNarrativeCameraTransition();}
	bNarrativeInputBlocked=bBlocked;
}
void AAftershockCameraPawn::StartNarrativeCameraTransition(bool bTowardsCity,float DurationSeconds)
{
	if(!SpringArm)return;
	NarrativeDuration=FMath::Clamp(DurationSeconds,.1f,15.f);NarrativeStartedRealSeconds=FPlatformTime::Seconds();
	const float CityZoom=FMath::Clamp(SavedLocalZoomDistance,MinArmLength,FMath::Max(MinArmLength,CityOverviewStartDistance));
	NarrativeStartZoom=bTowardsCity?FMath::Clamp(FMath::Max(CityZoom*2.4f,CityOverviewStartDistance*1.8f),MinArmLength,MaxArmLength):FMath::Max(MinArmLength,CurrentZoomDistance);
	NarrativeEndZoom=bTowardsCity?CityZoom:FMath::Clamp(FMath::Max(NarrativeStartZoom*3.f,PlanetRadius*1.8f),MinArmLength,MaxArmLength);
	bNarrativeTransitionActive=true;AdvanceNarrativeCameraTransition();
}
void AAftershockCameraPawn::AdvanceNarrativeCameraTransition()
{
	if(!bNarrativeTransitionActive||!SpringArm)return;
	const float Alpha=FMath::Clamp(static_cast<float>((FPlatformTime::Seconds()-NarrativeStartedRealSeconds)/NarrativeDuration),0.f,1.f);
	const float Ease=Alpha*Alpha*(3.f-2.f*Alpha);
	DesiredArmLength=FMath::Exp(FMath::Lerp(FMath::Loge(FMath::Max(1.f,NarrativeStartZoom)),FMath::Loge(FMath::Max(1.f,NarrativeEndZoom)),Ease));CurrentZoomDistance=DesiredArmLength;
	const float Blend=FMath::Clamp((CurrentZoomDistance-CityOverviewStartDistance)/FMath::Max(1.f,PlanetRadius*1.6f-CityOverviewStartDistance),0.f,1.f);
	CurrentOverviewBlend=Blend*Blend*(3.f-2.f*Blend);SetActorLocation(FMath::Lerp(LocalMapFocus,PlanetCenter,CurrentOverviewBlend));SpringArm->TargetArmLength=CurrentZoomDistance+PlanetRadius*CurrentOverviewBlend;
	FRotator Orbit=SpringArm->GetRelativeRotation();Orbit.Pitch=FMath::Lerp(-48.f,PlanetOrbitPitch,CurrentOverviewBlend);SpringArm->SetRelativeRotation(Orbit);
	// No world-time timer or gameplay update: refresh the view component only.
	SpringArm->TickComponent(0.f,LEVELTICK_All,nullptr);
	if(Alpha>=1.f)bNarrativeTransitionActive=false;
}

void AAftershockCameraPawn::HandleZoomAxis(float AxisVal)
{
	if (!FMath::IsNearlyZero(AxisVal) && !IsWorldInputBlocked())
	{
		ZoomCamera(AxisVal*(bUserInvertWheel?1.f:-1.f)*UserZoomSensitivity);
	}
}

void AAftershockCameraPawn::HandleZoomIn()
{
	if (!IsWorldInputBlocked()) ZoomCamera(-UserZoomSensitivity);
}

void AAftershockCameraPawn::HandleZoomOut()
{
	if (!IsWorldInputBlocked()) ZoomCamera(UserZoomSensitivity);
}

bool AAftershockCameraPawn::IsWorldInputBlocked() const
{
	if(bNarrativeInputBlocked)return true;
	if(const auto* Saves=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSaveSubsystem>():nullptr;Saves&&Saves->IsWorldLoadFailed(GetWorld()))return true;
	if (const AAftershockPlayerController* PC = Cast<AAftershockPlayerController>(GetController()))
	{
		if(PC->IsNarrativeFlowActive())return true;
		if (const UAftershockMainHUDWidget* HUD = PC->GetMainHUD()) return HUD->BlocksWorldInputAtCursor();
	}
	return GetWorld() && GetWorld()->IsPaused();
}

void AAftershockCameraPawn::HandleRotateGhost()
{
	if (bIsBuildModeActive && !IsWorldInputBlocked()) RotatePlacementGhost(90.0f);
}

void AAftershockCameraPawn::HandleCycleFoundation()
{
	if (!bIsBuildModeActive || IsWorldInputBlocked()) return;
	CurrentSelectedFoundation = static_cast<EFoundationQuality>((static_cast<uint8>(CurrentSelectedFoundation) + 1) % 3);
	RefreshPlacementPreview();
}

void AAftershockCameraPawn::HandleIncreaseFloors()
{
	if (!bIsBuildModeActive || !CurrentSelectedBuilding || IsWorldInputBlocked()) return;
	const float SavedYaw = CurrentGhostYaw;
	SetBuildMode(CurrentSelectedBuilding, CurrentSelectedFoundation, CurrentSelectedFloors + 1);
	RotatePlacementGhost(SavedYaw);
}

void AAftershockCameraPawn::HandleDecreaseFloors()
{
	if (!bIsBuildModeActive || !CurrentSelectedBuilding || IsWorldInputBlocked()) return;
	const float SavedYaw = CurrentGhostYaw;
	SetBuildMode(CurrentSelectedBuilding, CurrentSelectedFoundation, CurrentSelectedFloors - 1);
	RotatePlacementGhost(SavedYaw);
}

void AAftershockCameraPawn::HandleToggleOverview()
{
	if (GetWorld() && GetWorld()->IsPaused()) return;
	if (const AAftershockPlayerController* PC = Cast<AAftershockPlayerController>(GetController()))
	{
		if (const UAftershockMainHUDWidget* HUD = PC->GetMainHUD())
		{
			if (HUD->HasModalInputBlock()) return;
		}
	}
	if (DesiredArmLength > CityOverviewStartDistance)
	{
		DesiredArmLength = FMath::Clamp(SavedLocalZoomDistance, MinArmLength, CityOverviewStartDistance);
	}
	else
	{
		SavedLocalZoomDistance = DesiredArmLength;
		DesiredArmLength = PlanetRadius * 2.2f;
		CancelBuildMode();
		ClearRoadPreview();
	}
}

void AAftershockCameraPawn::HandleLeftClick()
{
	if (IsWorldInputBlocked()) return;
	if (CurrentOverviewBlend > .6f)
	{
		const auto* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
		if (Terrain && Terrain->IsUsingRealGeography())
		{
			int32 Region = INDEX_NONE, Focus = INDEX_NONE;
			if (FindGeographicFocusAtCursor(Region, Focus)
				&& (Region != Terrain->GeographyIndex || Focus != Terrain->GeographyFocusIndex))
			{
				if (auto* PC = Cast<AAftershockPlayerController>(GetController())) PC->ProposeGeographicNewGame(Region, Focus);
				return;
			}
			double Latitude = 0., Longitude = 0.; FText Error;
			if (TryGetGlobeGeographicAtCursor(Latitude, Longitude) && !FocusGeographicLocation(Latitude, Longitude, Error))
				if (auto* PC = Cast<AAftershockPlayerController>(GetController()))
					if (auto* HUD = PC->GetMainHUD()) HUD->ShowWorldOrderFeedback(Error);
			return;
		}
	}
	if (AAftershockPlayerController* PC = Cast<AAftershockPlayerController>(GetController()))
	{
		if (UAftershockMainHUDWidget* HUD = PC->GetMainHUD())
		{
			if (HUD->HandleWorldPrimaryAction()) return;
		}
	}
	if (bIsBuildModeActive)
	{
		int32 OutAssignedID = -1;
		ConfirmPlacement(OutAssignedID);
	}
}

void AAftershockCameraPawn::HandleRightClick()
{
	if(bNarrativeInputBlocked)return;
	CancelBuildMode();
	if (AAftershockPlayerController* PC = Cast<AAftershockPlayerController>(GetController()))
	{
		if (UAftershockMainHUDWidget* HUD = PC->GetMainHUD()) HUD->CancelMapTool();
	}
}

void AAftershockCameraPawn::HandleEscapePressed()
{
	if(bNarrativeInputBlocked)return;
	if (AAftershockPlayerController* PC = Cast<AAftershockPlayerController>(GetController()))
	{
		if (UAftershockMainHUDWidget* HUD = PC->GetMainHUD())
		{
			HUD->HandleEscapeAction();
			return;
		}
	}
	CancelBuildMode();
}

void AAftershockCameraPawn::UpdateCameraTick()
{
	const float DeltaSeconds = 0.016f;

	// 0) Klavye ile Pan ve Rotasyon Kontrolü (WASD / Ok Tuşları / QE)
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	}

	if (PC && !IsWorldInputBlocked())
	{
		const auto* Settings=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSettingsSubsystem>():nullptr;
		static const FAftershockUserPreferences Defaults;
		const auto Down=[PC,Settings](EAftershockUserAction Action)
		{
			const int32 Index=static_cast<int32>(Action);const FKey Primary=Settings?Settings->GetKey(Action):Defaults.Keys[Index];
			if(PC->IsInputKeyDown(Primary))return true;
			// Preserve the original arrow aliases until explicitly assigned to another action.
			static const FKey Arrows[]={EKeys::Up,EKeys::Down,EKeys::Left,EKeys::Right};
			return Index<4&&Primary==Defaults.Keys[Index]&&(!Settings||!Settings->GetPreferences().Keys.Contains(Arrows[Index]))&&PC->IsInputKeyDown(Arrows[Index]);
		};
		FVector2D PanInput(0.0f, 0.0f);
		if (Down(EAftershockUserAction::PanForward)) { PanInput.Y += 1.0f; }
		if (Down(EAftershockUserAction::PanBackward)) { PanInput.Y -= 1.0f; }
		if (Down(EAftershockUserAction::PanRight)) { PanInput.X += 1.0f; }
		if (Down(EAftershockUserAction::PanLeft)) { PanInput.X -= 1.0f; }

		if (!PanInput.IsNearlyZero())
		{
			MoveCamera(PanInput.GetSafeNormal());
		}

		if (Down(EAftershockUserAction::RotateLeft))
		{
			RotateCamera(-1.0f);
		}
		if (Down(EAftershockUserAction::RotateRight))
		{
			RotateCamera(1.0f);
		}
	}

	// Smooth altitude and pivot together; the radius offset prevents zooming inside the sphere.
	if (SpringArm)
	{
		// Physical Earth overview is many orders of magnitude above a street view.
		// Interpolating in log space avoids a single-frame million-metre camera jump.
		CurrentZoomDistance = (!bUserSmoothCamera||bUserReduceMotion)?DesiredArmLength:FMath::Exp(FMath::FInterpTo(FMath::Loge(FMath::Max(1.f, CurrentZoomDistance)),
			FMath::Loge(FMath::Max(1.f, DesiredArmLength)), DeltaSeconds, ZoomInterpSpeed));
		const float BlendRange = FMath::Max(1.0f, PlanetRadius * 1.6f - CityOverviewStartDistance);
		const float LinearBlend = FMath::Clamp((CurrentZoomDistance - CityOverviewStartDistance) / BlendRange, 0.0f, 1.0f);
		CurrentOverviewBlend = LinearBlend * LinearBlend * (3.0f - 2.0f * LinearBlend);
		SetActorLocation(FMath::Lerp(LocalMapFocus, PlanetCenter, CurrentOverviewBlend));
		SpringArm->TargetArmLength = CurrentZoomDistance + PlanetRadius * CurrentOverviewBlend;
		if (ShakeRemainingTime <= 0.0f)
		{
			FRotator Orbit = SpringArm->GetRelativeRotation();
			Orbit.Pitch = FMath::Lerp(-48.0f, PlanetOrbitPitch, CurrentOverviewBlend);
			SpringArm->SetRelativeRotation(Orbit);
		}
	}

	if (auto* Crowd = GetWorld() ? GetWorld()->GetSubsystem<UCrowdRenderSubsystem>() : nullptr)
		Crowd->RefreshTerrainPresentationBudget();
	RefreshExpansionFrontier();
	RefreshPlacementPreview();
}

void AAftershockCameraPawn::RefreshPlacementPreview(bool bForceRoadQuote)
{
	// Recompute on click as well; cached timer results must never authorize spending.
	// 2) İnşaat Önizleme (Ghost Mesh) Takibi
	if (bIsBuildModeActive && CurrentSelectedBuilding && PlacementGhostMesh)
	{
		// Hovering a modal/HUD control must neither run a bounded-but-expensive
		// road search nor leave a green world preview behind blocked input. Keep
		// the selected tool; normal hover resumes when the world regains input.
		if(IsWorldInputBlocked())
		{
			bIsPlacementValid=false;
			if(bAutomaticRoadPreviewDrawn){ClearRoadPreview();bAutomaticRoadPreviewDrawn=false;}
			PlacementGhostMesh->SetVisibility(false);
			if(ArchitecturalPreview.IsValid())ArchitecturalPreview->HidePreview();
			return;
		}
		FIntPoint HoveredCoords;
		if (GetHoveredGridCoords(HoveredCoords))
		{
			CurrentHoveredGridCoords = HoveredCoords;

			UWorld* World = GetWorld();
			UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
			UBuildingManagerSubsystem* BuildingMgr = World ? World->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
			UEconomyAndStateSubsystem* Economy = World ? World->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
			const URoadNetworkSubsystem* Roads=World?World->GetSubsystem<URoadNetworkSubsystem>():nullptr;

			if (Terrain && BuildingMgr)
			{
				const FVector TargetLocation = BuildingMgr->GetBuildingVisualTransform(HoveredCoords,CurrentSelectedBuilding,CurrentSelectedFloors,CurrentGhostYaw).GetTranslation();
				PlacementGhostMesh->SetWorldLocation(TargetLocation);

				const bool bLocationFree = BuildingMgr->CanPlaceBuildingAt(HoveredCoords, CurrentSelectedBuilding);
				const int32 BuildingCost = BuildingMgr->CalculateConstructionCost(CurrentSelectedBuilding, CurrentSelectedFoundation, CurrentSelectedFloors);
				const double RealNow=FPlatformTime::Seconds();
				const bool RefreshAccess=bLocationFree&&(bForceRoadQuote||!AutomaticRoadPreviewQuote.IsValid()
					||AutomaticRoadPreviewCell!=HoveredCoords||AutomaticRoadPreviewData.Get()!=CurrentSelectedBuilding.Get()
					||AutomaticRoadPreviewRevision!=Terrain->GetTerrainRevision()||RealNow>=AutomaticRoadPreviewExpiry);
				if(RefreshAccess&&(bForceRoadQuote||RealNow>=AutomaticRoadNextSearch))
				{
					AutomaticRoadPreviewQuote=MakeShared<FAftershockBuildingRoadAccessQuote>(BuildingMgr->QuoteAutomaticRoadAccess(HoveredCoords,CurrentSelectedBuilding));
					AutomaticRoadPreviewCell=HoveredCoords;AutomaticRoadPreviewData=CurrentSelectedBuilding;
					AutomaticRoadPreviewRevision=Terrain->GetTerrainRevision();AutomaticRoadPreviewExpiry=RealNow+.25;
					AutomaticRoadNextSearch=RealNow+.075; // bounded UI work while moving over disconnected land.
				}
				const bool MatchingAccess=bLocationFree&&AutomaticRoadPreviewQuote.IsValid()&&AutomaticRoadPreviewCell==HoveredCoords
					&&AutomaticRoadPreviewData.Get()==CurrentSelectedBuilding.Get()&&AutomaticRoadPreviewRevision==Terrain->GetTerrainRevision();
				const bool HasAccess=MatchingAccess&&AutomaticRoadPreviewQuote->bRouteValid;
				const int32 RoadCost=HasAccess?AutomaticRoadPreviewQuote->RoadCostTL:0;
				const int64 AggregateCost=static_cast<int64>(BuildingCost)+RoadCost;
				const int32 Cost=static_cast<int32>(FMath::Min<int64>(MAX_int32,AggregateCost));
				const bool bAffordable = AggregateCost<=MAX_int32&&Economy&&!Economy->bIsGameOver&&Economy->CanAfford(Cost);

				bIsPlacementValid = HasAccess && bAffordable && !IsWorldInputBlocked();
				if(HasAccess&&AutomaticRoadPreviewQuote->NewRoadCells>0&&!IsWorldInputBlocked())
				{
					if(RefreshAccess||!bAutomaticRoadPreviewDrawn||bAutomaticRoadPreviewValid!=bIsPlacementValid)
					{
						TArray<FIntPoint> AccessPreview=AutomaticRoadPreviewQuote->RoadPath;
						AccessPreview.Pop(EAllowShrinking::No); // existing road is not new construction.
						TArray<float> AccessSurfaceElevationsCm;AccessSurfaceElevationsCm.Reserve(AccessPreview.Num());
						if(Roads)for(const FIntPoint Coord:AccessPreview)
							AccessSurfaceElevationsCm.Add(Roads->GetRoadSurfaceElevation(Coord));
						SetRoadPreview(AccessPreview,bIsPlacementValid,AccessSurfaceElevationsCm);
						bAutomaticRoadPreviewDrawn=true;bAutomaticRoadPreviewValid=bIsPlacementValid;
					}
				}
				else if(bAutomaticRoadPreviewDrawn){ClearRoadPreview();bAutomaticRoadPreviewDrawn=false;}

				if (!bLocationFree)
				{
					const FGridCell Cell = Terrain->GetCell(HoveredCoords);
					if(Cell.bIsOcean)
					{
						CurrentPlacementFeedback=NSLOCTEXT("Aftershock","FailOceanBuilding","Deniz üzerine bina kurulamaz. Kıyıdaki uygun kara parselini seçin.");
					}
					else if(!Terrain->IsPlayableCoord(HoveredCoords))
					{
						CurrentPlacementFeedback=NSLOCTEXT("Aftershock","FailLockedBuildingParcel","Bu arazi henüz inşaata açık değil. Oynanabilir şehir alanında bir parsel seçin.");
					}
					else if (Cell.bIsRiverbed)
					{
						CurrentPlacementFeedback = NSLOCTEXT("Aftershock", "FailRiver", "Nehir yatağı üzerine inşa edilemez!");
					}
					else if (Cell.bHasRoad)
					{
						CurrentPlacementFeedback = NSLOCTEXT("Aftershock", "FailRoad", "Yol üzerine bina kurulamaz. Önce yolu kaldırın.");
					}
					else if(Cell.bIsEmergencyAssemblyArea)
					{
						CurrentPlacementFeedback=NSLOCTEXT("Aftershock","FailEmergencyAssemblyBuilding","Acil toplanma alanı yapılaşmaya ayrılmamıştır. Bu alanı açık bırakın.");
					}
					else if(Cell.bVolcanicVent)
					{
						CurrentPlacementFeedback=NSLOCTEXT("Aftershock","FailVolcanicVentBuilding","Etkin volkan ağzına yapı kurulamaz.");
					}
					else if(Cell.bBurning||Cell.LavaCoverage01>.1f)
					{
						CurrentPlacementFeedback=NSLOCTEXT("Aftershock","FailActiveHazardBuilding","Yangın veya lav etkisi altındaki parsel inşaata güvenli değil.");
					}
					else if(Cell.OccupyingBuildingID!=INDEX_NONE)
					{
						CurrentPlacementFeedback = NSLOCTEXT("Aftershock", "FailOccupied", "Bu hücrede zaten bir bina var!");
					}
					else
					{
						CurrentPlacementFeedback=NSLOCTEXT("Aftershock","FailUnavailableBuildingParcel","Bu parsel mevcut arazi veya afet koşullarında inşaata uygun değil.");
					}
				}
				else if (!HasAccess)
				{
					CurrentPlacementFeedback=MatchingAccess?AutomaticRoadPreviewQuote->Feedback
						:NSLOCTEXT("Aftershock","AccessQuotePending","Yol erişimi değerlendiriliyor…");
				}
				else if (!bAffordable)
				{
					CurrentPlacementFeedback = FText::Format(
						NSLOCTEXT("Aftershock", "FailBudgetFmt", "Yetersiz bütçe! (Gereken: {0} TL)"),
						FText::AsNumber(Cost)
					);
				}
				else
				{
					const auto* Preferences=GetGameInstance()?GetGameInstance()->GetSubsystem<UAftershockSettingsSubsystem>():nullptr;
					static const FAftershockUserPreferences Defaults;
					const auto KeyLabel=[Preferences](EAftershockUserAction Action){return (Preferences?Preferences->GetKey(Action):Defaults.Keys[static_cast<int32>(Action)]).GetDisplayName();};
					CurrentPlacementFeedback = FText::Format(
						NSLOCTEXT("Aftershock", "ValidPlacementBindingFmt", "{0} | {1} TL | {2} kat | Temel: {3}\nSol tık: kur   {4}: döndür   {5}: temel   {6}/{7}: kat   Sağ tık: iptal"),
						CurrentSelectedBuilding->DisplayName,
						FText::AsNumber(Cost),
						FText::AsNumber(CurrentSelectedFloors),
						FText::FromString(CurrentSelectedFoundation == EFoundationQuality::Strong ? TEXT("Güçlü") : CurrentSelectedFoundation == EFoundationQuality::Weak ? TEXT("Zayıf") : TEXT("Orta")),
						KeyLabel(EAftershockUserAction::RotateBuilding),KeyLabel(EAftershockUserAction::CycleFoundation),KeyLabel(EAftershockUserAction::IncreaseFloors),KeyLabel(EAftershockUserAction::DecreaseFloors)
					);
				}

				UpdateGhostVisuals(bIsPlacementValid);
				if(RoadCost>0)CurrentPlacementFeedback=FText::Format(
					NSLOCTEXT("Aftershock","BuildingAutoRoadFee","{0}\nOtomatik yol: {1} hücre, {2} TL — toplam bedele dahil."),
					CurrentPlacementFeedback,FText::AsNumber(AutomaticRoadPreviewQuote->NewRoadCells),FText::AsNumber(RoadCost));
				if(bLocationFree&&!MatchingAccess&&GhostDynMat&&!IsWorldInputBlocked())
					GhostDynMat->SetVectorParameterValue(TEXT("GhostTint"),FLinearColor(1.f,.65f,.08f));
				bool bArchitecturalGhost=false;
				if(!IsWorldInputBlocked()&&GhostDynMat&&!FParse::Param(FCommandLine::Get(),TEXT("AftershockLegacyBuildingVisuals")))
				{
					FAftershockArchitecturalInput Input;
					if(BuildingMgr->GetArchitecturalPlacementInput(HoveredCoords,CurrentSelectedBuilding,CurrentSelectedFloors,CurrentGhostYaw,Input))
					{
						if(!ArchitecturalPreview.IsValid())ArchitecturalPreview=MakeShared<FAftershockArchitecturalRenderer>();
						bArchitecturalGhost=ArchitecturalPreview->ShowPreview(*this,Input,GhostDynMat);
					}
				}
				if(!bArchitecturalGhost&&ArchitecturalPreview.IsValid())ArchitecturalPreview->HidePreview();
				PlacementGhostMesh->SetVisibility(!IsWorldInputBlocked()&&!bArchitecturalGhost);
			}
		}
		else
		{
			// Grid dışı veya geçersiz hücre davranışı
			bIsPlacementValid = false;
			if(bAutomaticRoadPreviewDrawn){ClearRoadPreview();bAutomaticRoadPreviewDrawn=false;}
			CurrentHoveredGridCoords = FIntPoint(MAX_int32, MAX_int32);
			CurrentPlacementFeedback = CurrentOverviewBlend > 0.6f
				? NSLOCTEXT("Aftershock", "BuildFromOverviewZoom", "İnşa etmek için tekerlekle şehir görünümüne yakınlaşın.")
				: NSLOCTEXT("Aftershock", "FailOutOfBounds", "Oynanabilir şehir sınırları dışında.");
			PlacementGhostMesh->SetVisibility(false);
			if(ArchitecturalPreview.IsValid())ArchitecturalPreview->HidePreview();
		}
	}
}

bool AAftershockCameraPawn::GetCurrentGeographicFocus(double& OutLatitude, double& OutLongitude) const
{
	const auto* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	return Terrain && Terrain->TryWorldToGeographic(LocalMapFocus, OutLatitude, OutLongitude);
}

bool AAftershockCameraPawn::FindGeographicFocusAtCursor(int32& OutRegionIndex, int32& OutFocusIndex) const
{
	OutRegionIndex = OutFocusIndex = INDEX_NONE;
	const auto* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!Terrain || !Terrain->IsUsingRealGeography() || !PC || !PC->PlayerCameraManager || CurrentOverviewBlend <= .6f) return false;
	float MouseX = 0.f, MouseY = 0.f;
	if (!PC->GetMousePosition(MouseX, MouseY)) return false;
	const auto Layout = GetAftershockGlobeLayout(*Terrain);
	const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
	double BestSquared = 16. * 16.;
	for (int32 Region = 0; Region < GetAftershockGeographyCount(); ++Region)
		for (int32 Focus = 0; Focus < GetAftershockGeographyFocusCount(Region); ++Focus)
		{
			if (!IsAftershockFocusGeodataReady(Region, Focus)) continue;
			const auto& Place = GetAftershockGeographyFocus(Region, Focus);
			const FVector Normal = AftershockLatLonToFocusGlobeNormal(Place.Latitude, Place.Longitude, Terrain->GeographyIndex, Terrain->GeographyFocusIndex);
			const FVector Position = Layout.Center + Normal * (Layout.RadiusCm * 1.001);
			// A projected marker on the far hemisphere is occluded by the Earth.
			if (FVector::DotProduct(Eye - Position, Normal) <= 0.) continue;
			FVector2D Pixel;
			if (!PC->ProjectWorldLocationToScreen(Position, Pixel, false)) continue;
			const double Squared = FVector2D::DistSquared(Pixel, FVector2D(MouseX, MouseY));
			if (Squared < BestSquared) { BestSquared = Squared; OutRegionIndex = Region; OutFocusIndex = Focus; }
		}
	return OutRegionIndex != INDEX_NONE;
}

bool AAftershockCameraPawn::TryGetGlobeGeographicAtCursor(double& OutLatitude, double& OutLongitude) const
{
	const auto* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!Terrain || !Terrain->IsUsingRealGeography() || !PC || CurrentOverviewBlend <= .6f) return false;
	int32 Region = INDEX_NONE, Focus = INDEX_NONE;
	if (FindGeographicFocusAtCursor(Region, Focus))
	{
		const auto& Place = GetAftershockGeographyFocus(Region, Focus);
		OutLatitude = Place.Latitude; OutLongitude = Place.Longitude; return true;
	}
	FVector Origin, Direction;
	if (!PC->DeprojectMousePositionToWorld(Origin, Direction) || !Direction.Normalize()) return false;
	const auto Layout = GetAftershockGlobeLayout(*Terrain);
	const FVector Offset = Origin - Layout.Center;
	const double Along = FVector::DotProduct(Offset, Direction);
	const double Discriminant = Along * Along - (Offset.SizeSquared() - FMath::Square(static_cast<double>(Layout.RadiusCm)));
	if (!FMath::IsFinite(Discriminant) || Discriminant < 0.) return false;
	double Distance = -Along - FMath::Sqrt(Discriminant);
	if (Distance < 0.) Distance = -Along + FMath::Sqrt(Discriminant);
	if (!FMath::IsFinite(Distance) || Distance < 0.) return false;
	const FVector LocalNormal = (Origin + Direction * Distance - Layout.Center).GetSafeNormal();
	FVector East, South, Up;
	GetAftershockGeographicFocusBasis(Terrain->GeographyIndex, Terrain->GeographyFocusIndex, East, South, Up);
	const FVector GeographicNormal = (East * LocalNormal.X + South * LocalNormal.Y + Up * LocalNormal.Z).GetSafeNormal();
	OutLatitude = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(GeographicNormal.Z, -1., 1.)));
	OutLongitude = FMath::RadiansToDegrees(FMath::Atan2(GeographicNormal.Y, GeographicNormal.X));
	return FMath::IsFinite(OutLatitude) && FMath::IsFinite(OutLongitude);
}

bool AAftershockCameraPawn::FocusGeographicLocation(double Latitude, double Longitude, FText& Error)
{
	const auto* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	FVector Position;
	if (!Terrain || !Terrain->TryGeographicToWorld(Latitude, Longitude, Position))
	{
		Error = NSLOCTEXT("Aftershock", "OutsideOfflineGeography", "Bu nokta açık bölgenin ayrıntılı harita kapsamı dışında. Başka bir bölgeye geçmek için dünya üzerindeki konum işaretini seçin.");
		return false;
	}
	PanToLocation(Position);
	Error = FText::GetEmpty();
	return true;
}

void AAftershockCameraPawn::SetInfrastructureConstructionMode(bool bEnabled)
{
	bInfrastructureConstructionMode = bEnabled;
	if (bEnabled)
	{
		bExpansionRequested = true;
		EnsureFocusTerrain(LocalMapFocus);
	}
}

void AAftershockCameraPawn::MoveCamera(FVector2D PanInput)
{
	if (PanInput.IsNearlyZero() || IsWorldInputBlocked()) return;
	constexpr float DeltaTime = 0.016f;
	if (CurrentOverviewBlend > 0.6f && SpringArm)
	{
		FRotator Orbit = SpringArm->GetRelativeRotation();
		Orbit.Yaw = FRotator::NormalizeAxis(Orbit.Yaw + PanInput.X * RotateSpeed * UserRotationSensitivity * DeltaTime);
		PlanetOrbitPitch = FMath::Clamp(PlanetOrbitPitch - PanInput.Y * RotateSpeed * UserRotationSensitivity * DeltaTime * 0.6f, -85.0f, 70.0f);
		Orbit.Pitch = FMath::Lerp(-48.0f, PlanetOrbitPitch, CurrentOverviewBlend);
		SpringArm->SetRelativeRotation(Orbit);
		return;
	}
	UWorld* World = GetWorld();
	const FRotator Yaw(0.0f, SpringArm ? SpringArm->GetRelativeRotation().Yaw : GetActorRotation().Yaw, 0.0f);
	const FVector Forward = FRotationMatrix(Yaw).GetUnitAxis(EAxis::X);
	const FVector Right = FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y);
	const UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	const bool bRealGeography = Terrain && Terrain->IsUsingRealGeography();
	const float Scale = FMath::Clamp(CurrentZoomDistance / 7000.0f, 0.35f, bRealGeography ? 128.f : 2.0f);
	FVector NewFocus = LocalMapFocus + (Forward * PanInput.Y + Right * PanInput.X) * MoveSpeed * UserPanSensitivity * DeltaTime * Scale;
	if (bRealGeography)
	{
		const FIntRect Bounds = Terrain->GetSupportedGeographicCellBounds();
		if (Bounds.IsEmpty()) return;
		NewFocus.X = FMath::Clamp(NewFocus.X, Terrain->GridOrigin.X + (Bounds.Min.X + .5) * Terrain->CellSize,
			Terrain->GridOrigin.X + (Bounds.Max.X - .5) * Terrain->CellSize);
		NewFocus.Y = FMath::Clamp(NewFocus.Y, Terrain->GridOrigin.Y + (Bounds.Min.Y + .5) * Terrain->CellSize,
			Terrain->GridOrigin.Y + (Bounds.Max.Y - .5) * Terrain->CellSize);
		double Latitude = 0., Longitude = 0.; FVector GeographicPoint;
		if (Terrain->TryWorldToGeographic(NewFocus, Latitude, Longitude)
			&& Terrain->TryGeographicToWorld(Latitude, Longitude, GeographicPoint)) NewFocus.Z = GeographicPoint.Z;
	}
	if (EnsureFocusTerrain(NewFocus)) LocalMapFocus = NewFocus;
}

void AAftershockCameraPawn::PanToLocation(const FVector& NewWorldLocation)
{
	LocalMapFocus = NewWorldLocation;
	DesiredArmLength = FMath::Clamp(DesiredArmLength, 4500.0f, 9500.0f);
	CurrentZoomDistance = DesiredArmLength;
	CurrentOverviewBlend = 0.0f;
	SetActorLocation(LocalMapFocus);
	if (SpringArm)
	{
		SpringArm->TargetArmLength = DesiredArmLength;
	}
	EnsureFocusTerrain(LocalMapFocus);
	RefreshExpansionFrontier();
}

void AAftershockCameraPawn::PanToCell(FIntPoint GridCoord)
{
	if (const UTerrainGridSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr)
	{
		PanToLocation(Terrain->GridToWorld(GridCoord));
	}
}

bool AAftershockCameraPawn::EnsureFocusTerrain(const FVector& Focus)
{
	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || Terrain->Cells.IsEmpty() || Focus.ContainsNaN()) return false;
	if (Terrain->IsUsingRealGeography() && !bIsBuildModeActive && !bInfrastructureConstructionMode)
	{
		double Latitude = 0., Longitude = 0.;
		return Terrain->TryWorldToGeographic(Focus, Latitude, Longitude);
	}
	const FIntPoint Cell = Terrain->WorldToGrid(Focus);
	if (Cell.X == MAX_int32 || Cell.Y == MAX_int32) return false;
	if (Terrain->CoordToIndex(Cell) != INDEX_NONE) return true;
	bExpansionRequested = true;
	FText Error;
	if (Terrain->EnsureChunkGenerated(UTerrainGridSubsystem::CellToChunk(Cell), Error)) return true;
	const double Now = World->GetTimeSeconds();
	if (Now >= NextExpansionNoticeTime)
	{
		NextExpansionNoticeTime = Now + 5.0;
		if (const AAftershockPlayerController* PC = Cast<AAftershockPlayerController>(World->GetFirstPlayerController()))
			if (UAftershockMainHUDWidget* HUD = PC->GetMainHUD()) HUD->ShowWorldOrderFeedback(Error);
	}
	return false;
}

void AAftershockCameraPawn::RefreshExpansionFrontier()
{
	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || CurrentOverviewBlend > 0.6f || IsWorldInputBlocked()) return;
	// Merely exploring a real source window must not grow persistent simulation
	// cells. Construction remains explicit and keeps the resident-state cap.
	if (Terrain->IsUsingRealGeography() && !bIsBuildModeActive && !bInfrastructureConstructionMode) return;
	// Opening a map preserves the authored starting island. Expansion begins only
	// after a deliberate pan beyond it, or when loading an expanded settlement.
	if (!bExpansionRequested && Terrain->Cells.Num() <= Terrain->GridWidth * Terrain->GridHeight) return;
	const double Now = World->GetTimeSeconds();
	if (Now < NextFrontierRefreshTime) return;
	NextFrontierRefreshTime = Now + 0.25;
	const FIntPoint FocusCell = Terrain->WorldToGrid(LocalMapFocus);
	if (FocusCell.X == MAX_int32 || FocusCell.Y == MAX_int32) return;
	const FIntPoint Center = UTerrainGridSubsystem::CellToChunk(FocusCell);
	// At most one chunk per refresh. The presentation window remains bounded;
	// these cells are persistent simulation data, independent of camera visibility.
	for (int32 Radius = 0; Radius <= 1; ++Radius)
		for (int32 Y = -Radius; Y <= Radius; ++Y)
			for (int32 X = -Radius; X <= Radius; ++X)
			{
				if (Radius && FMath::Max(FMath::Abs(X), FMath::Abs(Y)) != Radius) continue;
				const FIntPoint Chunk = Center + FIntPoint(X, Y);
				const FIntPoint ChunkMin = Chunk * UTerrainGridSubsystem::ChunkSizeCells;
				bool bComplete = true;
				for (int32 Row = 0; Row < UTerrainGridSubsystem::ChunkSizeCells && bComplete; ++Row)
					for (int32 Column = 0; Column < UTerrainGridSubsystem::ChunkSizeCells; ++Column)
						if (Terrain->CoordToIndex(ChunkMin + FIntPoint(Column, Row)) == INDEX_NONE) { bComplete = false; break; }
				if (!bComplete)
				{
					FText Error;
					if (!Terrain->EnsureChunkGenerated(Chunk, Error)) NextFrontierRefreshTime=Now+2.0;
					return;
				}
			}
}

void AAftershockCameraPawn::ZoomCamera(float ZoomDelta)
{
	if(bNarrativeInputBlocked)return;
	// Public camera orders (including runtime audits) do not depend on OS cursor position.
	// Pointer-sensitive guards belong to mouse-wheel handlers above.
	DesiredArmLength = FMath::Clamp(DesiredArmLength * FMath::Exp(ZoomDelta * 0.12f), MinArmLength, MaxArmLength);
}

void AAftershockCameraPawn::RotateCamera(float YawDelta)
{
	if(bNarrativeInputBlocked)return;
	if (SpringArm)
	{
		UWorld* World = GetWorld();
		const float DeltaTime = 0.016f;

		FRotator NewRotation = SpringArm->GetRelativeRotation();
		NewRotation.Yaw = FRotator::NormalizeAxis(NewRotation.Yaw + YawDelta * RotateSpeed * UserRotationSensitivity * DeltaTime);
		SpringArm->SetRelativeRotation(NewRotation);
	}
}

bool AAftershockCameraPawn::GetHoveredBuildingGridCoords(FIntPoint& OutCoords) const
{
	OutCoords=FIntPoint(MAX_int32,MAX_int32);
	if (CurrentOverviewBlend>.6f || IsWorldInputBlocked()) return false;
	UWorld* World=GetWorld();
	APlayerController* PC=World?World->GetFirstPlayerController():nullptr;
	UBuildingManagerSubsystem* Buildings=World?World->GetSubsystem<UBuildingManagerSubsystem>():nullptr;
	if (!PC || !Buildings) return false;
	FVector Origin,Direction;
	if (!PC->DeprojectMousePositionToWorld(Origin,Direction)) return false;
	FHitResult Hit;
	const float Limit=PC->GetHitResultUnderCursor(ECC_Visibility,false,Hit)&&Hit.bBlockingHit
		?static_cast<float>(FVector::Distance(Origin,Hit.ImpactPoint))+5.f:10000000.f;
	FBuildingInstance Building;float Distance=0;
	if (!Buildings->RaycastBuilding(Origin,Direction,Limit,Building,Distance)) return false;
	OutCoords=Building.GridCoords;return true;
}

bool AAftershockCameraPawn::GetHoveredGridCoords(FIntPoint& OutCoords) const
{
	OutCoords = FIntPoint(MAX_int32, MAX_int32);
	if (CurrentOverviewBlend > 0.6f) return false;

	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	}

	if (!PC)
	{
		return false;
	}

	UWorld* World = GetWorld();
	UTerrainGridSubsystem* Terrain = World ? World->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain)
	{
		return false;
	}

	// 1. Tercih: GetHitResultUnderCursor
	FHitResult HitResult;
	if (PC->GetHitResultUnderCursor(ECC_Visibility, false, HitResult) && HitResult.bBlockingHit)
	{
		OutCoords = Terrain->WorldToGrid(HitResult.ImpactPoint);
		return Terrain->IsPlayableCoord(OutCoords) && FMath::Abs(HitResult.ImpactPoint.Z - Terrain->GetCell(OutCoords).Elevation) < 6000.0f;
	}

	// 2. Tercih (Sağlam Matematiksel Fallback): Z düzlemi Line-Plane kesişimi
	FVector WorldOrigin, WorldDirection;
	if (PC->DeprojectMousePositionToWorld(WorldOrigin, WorldDirection))
	{
		if (FMath::IsNearlyZero(WorldDirection.Z)) return false;
		const float Distance = (Terrain->GridOrigin.Z - WorldOrigin.Z) / WorldDirection.Z;
		if (Distance <= 0.0f) return false;
		const FPlane GroundPlane(FVector(0.0f, 0.0f, Terrain->GridOrigin.Z), FVector::UpVector);
		const FVector Intersection = FMath::LinePlaneIntersection(
			WorldOrigin,
			WorldOrigin + WorldDirection * 100000.0f,
			GroundPlane
		);

		OutCoords = Terrain->WorldToGrid(Intersection);
		return Terrain->IsPlayableCoord(OutCoords);
	}

	return false;
}

void AAftershockCameraPawn::SetBuildMode(
	UBuildingData* SelectedBuilding,
	EFoundationQuality SelectedFoundation,
	int32 SelectedFloors)
{
	CurrentSelectedBuilding = SelectedBuilding;
	CurrentSelectedFoundation = SelectedFoundation;
	CurrentSelectedFloors = SelectedBuilding ? FMath::Clamp(SelectedFloors, FMath::Max(1, SelectedBuilding->FloorsMin), FMath::Max(SelectedBuilding->FloorsMin, SelectedBuilding->FloorsMax)) : 1;

	if (!SelectedBuilding)
	{
		CancelBuildMode();
		return;
	}

	bIsBuildModeActive = true;
	if (const auto* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr)
		if (Terrain->IsUsingRealGeography()) { bExpansionRequested = true; EnsureFocusTerrain(LocalMapFocus); }
	CurrentGhostYaw = 0.0f;

	if (PlacementGhostMesh)
	{
		UStaticMesh* Mesh = SelectedBuilding->MeshIntact.LoadSynchronous();
		if (!Mesh)
		{
			Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		}
		if (Mesh)
		{
			PlacementGhostMesh->SetStaticMesh(Mesh);
		}

		UWorld* World = GetWorld();
		if (UBuildingManagerSubsystem* Buildings=World?World->GetSubsystem<UBuildingManagerSubsystem>():nullptr)
			PlacementGhostMesh->SetWorldScale3D(Buildings->GetBuildingVisualTransform(FIntPoint::ZeroValue,SelectedBuilding,CurrentSelectedFloors).GetScale3D());

		PlacementGhostMesh->SetRelativeRotation(FRotator(0.0f, CurrentGhostYaw, 0.0f));

		if (PlacementGhostMaterial && !GhostDynMat)
		{
			GhostDynMat = UMaterialInstanceDynamic::Create(PlacementGhostMaterial, PlacementGhostMesh);
		}
		if (GhostDynMat)
		{
			for (int32 Slot = 0; Slot < PlacementGhostMesh->GetNumMaterials(); ++Slot)
			{
				PlacementGhostMesh->SetMaterial(Slot, GhostDynMat);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("AFTERSHOCK_BUILDING_GHOST_MATERIAL_MISSING path=/Game/Buildings/Materials/M_BuildingGhost"));
		}

		bIsPlacementValid = false;
		PlacementGhostMesh->SetVisibility(false);
		RefreshPlacementPreview();
	}
}

void AAftershockCameraPawn::CancelBuildMode()
{
	if(ArchitecturalPreview.IsValid())ArchitecturalPreview->HidePreview();
	if(bAutomaticRoadPreviewDrawn)ClearRoadPreview();
	bAutomaticRoadPreviewDrawn=false;AutomaticRoadPreviewQuote.Reset();AutomaticRoadPreviewData.Reset();
	bIsBuildModeActive = false;
	bIsPlacementValid = false;
	CurrentHoveredGridCoords = FIntPoint(MAX_int32, MAX_int32);
	CurrentSelectedBuilding = nullptr;
	CurrentPlacementFeedback = FText::GetEmpty();

	if (PlacementGhostMesh)
	{
		PlacementGhostMesh->SetVisibility(false);
	}
}

void AAftershockCameraPawn::RotatePlacementGhost(float Degrees)
{
	CurrentGhostYaw = FRotator::NormalizeAxis(CurrentGhostYaw + Degrees);
	if (PlacementGhostMesh)
	{
		PlacementGhostMesh->SetRelativeRotation(FRotator(0.0f, CurrentGhostYaw, 0.0f));
	}
}

void AAftershockCameraPawn::UpdateGhostVisuals(bool bValid)
{
	if (!PlacementGhostMesh)
	{
		return;
	}

	if (GhostDynMat)
	{
		const FLinearColor TargetColor = bUserColorSafePlacement
			? (bValid?FLinearColor(0.f,.85f,1.f):FLinearColor(1.f,.38f,.015f))
			: (bValid?FLinearColor(.08f,1.f,.18f):FLinearColor(1.f,.06f,.04f));

		// This component owns one dedicated preview MID. Do not write the building
		// damage CPD channel; it is reserved for CrackAmount on placed buildings.
		GhostDynMat->SetVectorParameterValue(TEXT("GhostTint"), TargetColor);
	}
}

void AAftershockCameraPawn::SetRoadPreview(const TArray<FIntPoint>& Path, bool bValid,const TArray<float>& SurfaceElevationsCm)
{
	if (!RoadPreviewISM) return;
	const UTerrainGridSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainGridSubsystem>() : nullptr;
	if (!Terrain || Path.IsEmpty()) { ClearRoadPreview(); return; }
	if (!RoadPreviewDynMat && PlacementGhostMaterial)
	{
		RoadPreviewDynMat = UMaterialInstanceDynamic::Create(PlacementGhostMaterial, RoadPreviewISM);
		RoadPreviewISM->SetMaterial(0, RoadPreviewDynMat);
	}
	if (RoadPreviewDynMat) RoadPreviewDynMat->SetVectorParameterValue(TEXT("GhostTint"), bUserColorSafePlacement?(bValid?FLinearColor(0.f,.85f,1.f):FLinearColor(1.f,.38f,.015f)):(bValid?FLinearColor(.1f,1.f,.55f):FLinearColor(1.f,.08f,.04f)));
	TArray<FTransform> Transforms;
	Transforms.Reserve(Path.Num());
	for (int32 Index=0;Index<Path.Num();++Index)
	{
		const FIntPoint Coords=Path[Index];
		if (!Terrain->IsPlayableCoord(Coords)) continue;
		FVector Position=Terrain->GridToWorld(Coords,true); FRotator Rotation=FRotator::ZeroRotator;
		if (SurfaceElevationsCm.Num()==Path.Num())
		{
			Position.Z=SurfaceElevationsCm[Index];
			const int32 Previous=FMath::Max(0,Index-1),Next=FMath::Min(Path.Num()-1,Index+1);
			if (Previous!=Next)
			{
				FVector Tangent=Terrain->GridToWorld(Path[Next])-Terrain->GridToWorld(Path[Previous]); Tangent.Z=SurfaceElevationsCm[Next]-SurfaceElevationsCm[Previous];
				Rotation=Tangent.Rotation();
			}
		}
		Transforms.Emplace(Rotation,Position+FVector(0,0,5),FVector(Terrain->CellSize*.0092f,Terrain->CellSize*.0092f,.035f));
	}
	if(RoadPreviewISM->GetInstanceCount()!=Transforms.Num())
	{
		if(RoadPreviewISM->GetInstanceCount()>0)RoadPreviewISM->ClearInstances();
		if(!Transforms.IsEmpty())RoadPreviewISM->AddInstances(Transforms,false,true,false);
	}
	else
	{
		// Preserve instance identities and temporal history during unchanged
		// hover quotes. A changed route of the same size needs one batch update.
		bool Changed=false;
		for(int32 Index=0;Index<Transforms.Num()&&!Changed;++Index)
		{
			FTransform Current;
			Changed=!RoadPreviewISM->GetInstanceTransform(Index,Current,true)||!Current.Equals(Transforms[Index],.01f);
		}
		if(Changed)RoadPreviewISM->BatchUpdateInstancesTransforms(0,Transforms,true,true,true);
	}
	const bool Visible=!Transforms.IsEmpty();
	if(RoadPreviewISM->IsVisible()!=Visible)RoadPreviewISM->SetVisibility(Visible);
}

void AAftershockCameraPawn::ClearRoadPreview()
{
	if(RoadPreviewISM)
	{
		if(RoadPreviewISM->GetInstanceCount()>0)RoadPreviewISM->ClearInstances();
		if(RoadPreviewISM->IsVisible())RoadPreviewISM->SetVisibility(false);
	}
}

void AAftershockCameraPawn::StartDisasterCameraShake(float Duration, float Intensity)
{
	if(bUserReduceMotion)return;
	TotalShakeDuration = FMath::Max(0.1f, Duration);
	ShakeRemainingTime = TotalShakeDuration;
	ShakeIntensity = FMath::Clamp(Intensity, 0.5f, 10.0f);

	if (SpringArm)
	{
		BaseSpringArmRotation = SpringArm->GetRelativeRotation();
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			ShakeTimerHandle,
			this,
			&AAftershockCameraPawn::UpdateCameraShakeTick,
			0.02f,
			true
		);
	}
}

void AAftershockCameraPawn::UpdateCameraShakeTick()
{
	ShakeRemainingTime -= 0.02f;
	if (ShakeRemainingTime <= 0.0f)
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(ShakeTimerHandle);
		}
		if (SpringArm)
		{
			SpringArm->SetRelativeRotation(BaseSpringArmRotation);
		}
		return;
	}

	if (SpringArm)
	{
		const float Fade = FMath::Clamp(ShakeRemainingTime / TotalShakeDuration, 0.0f, 1.0f);
		const float Elapsed = TotalShakeDuration - ShakeRemainingTime;
		// Smooth low-frequency harmonic seismic ground roll with gentle rolling wave
		const float PitchOffset = (FMath::Sin(Elapsed * 7.5f) * 0.7f + FMath::Cos(Elapsed * 4.2f) * 0.3f) * Fade * ShakeIntensity * 0.016f;
		const float YawOffset = (FMath::Cos(Elapsed * 6.0f) * 0.6f + FMath::Sin(Elapsed * 3.5f) * 0.4f) * Fade * ShakeIntensity * 0.018f;
		const float RollOffset = FMath::Sin(Elapsed * 5.0f) * Fade * ShakeIntensity * 0.010f;
		SpringArm->SetRelativeRotation(BaseSpringArmRotation + FRotator(PitchOffset, YawOffset, RollOffset));
	}
}

bool AAftershockCameraPawn::ConfirmPlacement(int32& OutAssignedID)
{
	OutAssignedID = -1;
	if (!bIsBuildModeActive || !CurrentSelectedBuilding || IsWorldInputBlocked()) return false;
	// A real click bypasses only the UI search throttle. The manager independently
	// requotes again inside the aggregate purchase; cached hover never authorizes it.
	RefreshPlacementPreview(true);
	if (!bIsPlacementValid) return false;
	UWorld* World = GetWorld();
	UBuildingManagerSubsystem* Buildings = World ? World->GetSubsystem<UBuildingManagerSubsystem>() : nullptr;
	UEconomyAndStateSubsystem* Economy = World ? World->GetSubsystem<UEconomyAndStateSubsystem>() : nullptr;
	if (!Buildings || !Economy) return false;
	const FIntPoint PlacementCoords = CurrentHoveredGridCoords;
	if (!Economy->BuildBuildingWithBudget(PlacementCoords, CurrentSelectedBuilding, CurrentSelectedFoundation, CurrentSelectedFloors))
	{
		RefreshPlacementPreview();
		return false;
	}
	FBuildingInstance PlacedBuilding;
	if (Buildings->GetBuildingAtCoords(PlacementCoords, PlacedBuilding))
	{
		OutAssignedID = PlacedBuilding.InstanceID;
		if (UInstancedStaticMeshComponent* ISM = Buildings->GetOrCreateISM(PlacedBuilding.BuildingData))
		{
			FTransform InstanceTransform;
			if (ISM->GetInstanceTransform(PlacedBuilding.ISMInstanceIndex, InstanceTransform, true))
			{
				InstanceTransform.SetRotation(FRotator(0.0f, CurrentGhostYaw, 0.0f).Quaternion());
				ISM->UpdateInstanceTransform(PlacedBuilding.ISMInstanceIndex, InstanceTransform, true, true, true);
			}
		}
	}
	UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_PLAYER_PLACEMENT id=%d cell=%d,%d floors=%d yaw=%.0f"),
		OutAssignedID, PlacementCoords.X, PlacementCoords.Y, CurrentSelectedFloors, CurrentGhostYaw);
	CancelBuildMode();
	return true;
}
