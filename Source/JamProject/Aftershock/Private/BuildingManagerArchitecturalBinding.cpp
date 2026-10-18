// Derived visual descriptors preserve canonical building records and envelopes.
#include "Aftershock/Public/BuildingManagerSubsystem.h"
#include "AftershockArchitecturalRecipe.h"
#include "AftershockArchitecturalRenderer.h"
#include "AftershockArchitecturalAudit.h"
#include "AftershockPresentationBudget.h"
#include "Aftershock/Public/AftershockGeography.h"
#include "Aftershock/Public/AftershockCameraPawn.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	bool FillArchitecturalFrame(const UTerrainGridSubsystem& Terrain,UBuildingData& Data,FIntPoint Coord,int32 Floors,
		int32 StableID,const FTransform& Saved,const UStaticMesh& Source,FAftershockArchitecturalInput& Out)
	{
		Out={};const auto* Focus=TryGetAftershockGeographyFocus(Terrain.GeographyIndex,Terrain.GeographyFocusIndex);
		if(!Focus||Saved.ContainsNaN()||Saved.GetScale3D().GetMin()<=KINDA_SMALL_NUMBER)return false;
		const FBox Bounds=Source.GetBoundingBox();if(!Bounds.IsValid)return false;
		const FVector Scale=Saved.GetScale3D();
		const bool BuriedFooting=Bounds.Min.Z<-.5&&Bounds.Max.Z>.5;
		const double VisibleBase=BuriedFooting?0.:Bounds.Min.Z;
		const FVector LocalOrigin(Bounds.GetCenter().X,Bounds.GetCenter().Y,VisibleBase);
		Out.Frame=FTransform(Saved.GetRotation(),Saved.TransformPosition(LocalOrigin),FVector::OneVector);
		Out.EnvelopeCm=FVector(Bounds.GetSize().X*Scale.X,Bounds.GetSize().Y*Scale.Y,(Bounds.Max.Z-VisibleBase)*Scale.Z);
		Out.RegionID=FName(Focus->Id);Out.FocusID=FName(Focus->FocusId);Out.TerrainSeed=Terrain.TerrainSeed;

        Out.BuildingID=StableID;Out.Coord=Coord;Out.Type=Data.Type;Out.Floors=Floors;Out.StructuralMaterial=Data.Material;
        if(const auto* Profile=TryGetAftershockRegionEnvironmentProfile(Terrain.GeographyIndex))Out.RegionalBiome=Profile->RegionalBiome;
        if(Data.Type==EBuildingType::GreenSpace)
        {
            bool Flat=FMath::IsFinite(Terrain.CellSize)&&Terrain.CellSize>0.f;
            for(int32 Y=-1;Y<=1&&Flat;++Y)for(int32 X=-1;X<=1;++X)
            {
                const FVector Point=Out.Frame.TransformPosition(FVector(X*Out.EnvelopeCm.X*.35,Y*Out.EnvelopeCm.Y*.35,0.));
                const float GX=static_cast<float>((Point.X-Terrain.GridOrigin.X)/Terrain.CellSize-.5);
                const float GY=static_cast<float>((Point.Y-Terrain.GridOrigin.Y)/Terrain.CellSize-.5);
                float Z=0.f;
                if(!Terrain.TrySampleResidentElevation(GX,GY,Z)||!FMath::IsFinite(Z)||FMath::Abs(Z-Out.Frame.GetLocation().Z)>3.)
                {Flat=false;break;}
            }
            Out.bParkGroundNearlyLevel=Flat;
        }
		return !Out.EnvelopeCm.ContainsNaN()&&Out.EnvelopeCm.GetMin()>0.;
	}
}

bool UBuildingManagerSubsystem::GetCanonicalArchitecturalInput(int32 BuildingID,FAftershockArchitecturalInput& Out) const
{
	Out={};const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	const auto* Building=ActiveBuildings.Find(BuildingID);
	if(!Terrain||!Building||!Building->BuildingData||Building->DamageState>=EDamageState::Collapsed||!IsBuildingInPresentationWindow(BuildingID))return false;
	const auto* Batch=BuildingISMs.Find(Building->BuildingData);
	if(!Batch||!IsValid(*Batch)||!(*Batch)->GetStaticMesh())return false;
	FTransform Saved;if(!(*Batch)->GetInstanceTransform(Building->ISMInstanceIndex,Saved,true))return false;
	if(!FillArchitecturalFrame(*Terrain,*Building->BuildingData,Building->GridCoords,Building->CurrentFloors,BuildingID,Saved,*(*Batch)->GetStaticMesh(),Out))return false;
	Out.DamageState=Building->DamageState;
	for(int32 Channel=0;Channel<4;++Channel)
	{
		const int32 Index=Building->ISMInstanceIndex*(*Batch)->NumCustomDataFloats+Channel;
		if((*Batch)->PerInstanceSMCustomData.IsValidIndex(Index))Out.CanonicalCustomData[Channel]=(*Batch)->PerInstanceSMCustomData[Index];
	}
	return true;
}

bool UBuildingManagerSubsystem::GetArchitecturalPlacementInput(FIntPoint Coord,UBuildingData* Data,int32 Floors,float Yaw,FAftershockArchitecturalInput& Out) const
{
	Out={};const auto* Terrain=GetWorld()?GetWorld()->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	if(!Terrain||!Data||!Terrain->IsValidCoord(Coord)||!FMath::IsFinite(Yaw))return false;
	// Preview art only: legality/cost remain CanPlace/Economy. Match the actual
	// shared canonical transform function, including saved yaw/footing contract.
	UStaticMesh* Source=Data->MeshIntact.LoadSynchronous();if(!Source)return false;
	const int32 Count=FMath::Clamp(Floors,Data->FloorsMin,Data->FloorsMax);
	return FillArchitecturalFrame(*Terrain,*Data,Coord,Count,INDEX_NONE,GetBuildingVisualTransform(Coord,Data,Count,Yaw),*Source,Out);
}

void UBuildingManagerSubsystem::SyncArchitecturalBuilding(int32 ID)
{
	if(!ArchitecturalRenderer.IsValid())return;
	FAftershockArchitecturalInput Input;
	ArchitecturalRenderer->SyncBuilding(ID,GetCanonicalArchitecturalInput(ID,Input)?&Input:nullptr);
}

const FAftershockArchitectureWork* UBuildingManagerSubsystem::GetArchitecturalPresentationWork() const
{
	return ArchitecturalRenderer.IsValid()?&ArchitecturalRenderer->GetWork():nullptr;
}

void UBuildingManagerSubsystem::RefreshArchitecturalPresentation(bool Force)
{
	UWorld* World=GetWorld();const auto* Terrain=World?World->GetSubsystem<UTerrainGridSubsystem>():nullptr;
	if(!Terrain||!World->IsGameWorld())return;
	if(FParse::Param(FCommandLine::Get(),TEXT("AftershockLegacyBuildingVisuals")))
	{
		ArchitecturalRenderer.Reset();
		for(auto& Pair:BuildingISMs)if(IsValid(Pair.Value))Pair.Value->SetVisibility(!bUseDerivedBuildingPresentation);
		for(auto& Pair:PresentationISMs)if(IsValid(Pair.Value))Pair.Value->SetVisibility(bUseDerivedBuildingPresentation);
		return;
	}
	AActor* Host=GetOrCreateISMHostActor();if(!Host)return;
	if(!ArchitecturalRenderer.IsValid())ArchitecturalRenderer=MakeShared<FAftershockArchitecturalRenderer>();
	const auto Budget=GetAftershockPresentationBudget(World);
	const auto Tier=static_cast<EAftershockArchitecturalTier>(Budget.Tier);
	FVector Focus=Terrain->GridToWorld(FIntPoint(Terrain->GridWidth/2,Terrain->GridHeight/2),true);
	if(const auto* PC=World->GetFirstPlayerController())if(const auto* Camera=Cast<AAftershockCameraPawn>(PC->GetPawn()))Focus=Camera->GetLocalMapFocus();
	if(Force||bArchitecturalLayoutDirty||bBuildingPresentationDirty||ArchitecturalRenderer->NeedsRefresh(Tier,Focus))
	{
		TArray<FAftershockArchitecturalCandidate> Candidates;Candidates.Reserve(ActiveBuildings.Num());
		for(const auto& Pair:ActiveBuildings)
		{
			FAftershockArchitecturalInput Input;
			if(!GetCanonicalArchitecturalInput(Pair.Key,Input))continue;
			auto& Candidate=Candidates.AddDefaulted_GetRef();Candidate.Definition=Pair.Value.BuildingData;Candidate.Input=MoveTemp(Input);
		}
		// Restore may recreate DataAsset objects with byte-identical descriptors.
		// A dirty layout must still replace pointer-grouped ownership completely.
		ArchitecturalRenderer->Refresh(*Host,Candidates,Tier,Focus,Force||bArchitecturalLayoutDirty||bBuildingPresentationDirty);bArchitecturalLayoutDirty=false;
	}
	// Canonical ISM transforms, collision, instance indices and metadata remain
	// intact. Old derived components remain valid canonical-transform/query
	// descriptors. Hide only a WHOLE definition after its complete replacement
	// succeeds; unresolved/legacy envelopes retain their original presentation.
	for(auto& Pair:BuildingISMs)if(IsValid(Pair.Value))
		Pair.Value->SetVisibility(!bUseDerivedBuildingPresentation&&!ArchitecturalRenderer->OwnsDefinition(Pair.Key));
	for(auto& Pair:PresentationISMs)if(IsValid(Pair.Value))
		Pair.Value->SetVisibility(bUseDerivedBuildingPresentation&&!ArchitecturalRenderer->OwnsDefinition(Pair.Key));
}

bool UBuildingManagerSubsystem::ReadArchitecturalCPDAudit(FAftershockArchitectureAudit& Out) const
{
	Out={};Out.CanonicalBuildings=ActiveBuildings.Num();
	TArray<FAftershockArchitecturalCandidate> Inputs;Inputs.Reserve(ActiveBuildings.Num());
	for(const auto& Pair:ActiveBuildings)
	{
		const auto& B=Pair.Value;
		if(B.DamageState>=EDamageState::Collapsed){++Out.CollapsedOrMemorialBuildings;continue;}
		if(!IsBuildingInPresentationWindow(Pair.Key)){++Out.OutsideWindowBuildings;continue;}
		const auto* Canonical=BuildingISMs.Find(B.BuildingData);
		if(!Canonical||!IsValid(*Canonical)||(*Canonical)->NumCustomDataFloats!=4)
		{Out.CanonicalStrideValid=false;Out.Problem(Pair.Key);}
		else for(int32 Ch=0;Ch<4;++Ch)if(!(*Canonical)->PerInstanceSMCustomData.IsValidIndex(B.ISMInstanceIndex*4+Ch))
		{Out.CanonicalStrideValid=false;Out.Problem(Pair.Key);}
		FAftershockArchitecturalInput Input;
		if(!GetCanonicalArchitecturalInput(Pair.Key,Input)){++Out.CanonicalInputFailures;Out.Problem(Pair.Key);continue;}
		auto& C=Inputs.AddDefaulted_GetRef();C.Definition=B.BuildingData;C.Input=MoveTemp(Input);
	}
	if(ArchitecturalRenderer.IsValid())ArchitecturalRenderer->ReadCanonicalCPDAudit(Inputs,Out);
	return Out.IsValid();
}
