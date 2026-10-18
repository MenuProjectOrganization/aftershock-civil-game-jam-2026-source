#include "AftershockFxSetupCommandlet.h"

#if WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraGraph.h"
#include "NiagaraCommon.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	const FString FxRoot = TEXT("/Game/FX/Aftershock");

	bool SaveAssetPackage(UObject* Asset)
	{
		if (!Asset)
		{
			return false;
		}

		UPackage* Package = Asset->GetOutermost();
		if (!Package)
		{
			return false;
		}

		Package->MarkPackageDirty();
		const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Asset, *FileName, SaveArgs);
	}

	template<typename TValue>
	bool SetRapidIterationParameter(
		UNiagaraEmitter* Emitter,
		UNiagaraScript* TargetScript,
		const FString& ModulePath,
		FName InputName,
		const FNiagaraTypeDefinition& InputType,
		const TValue& Value)
	{
		if (!Emitter || !TargetScript)
		{
			return false;
		}

		FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
		UNiagaraScriptSource* ScriptSource = EmitterData ? Cast<UNiagaraScriptSource>(EmitterData->GraphSource) : nullptr;
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (!Graph)
		{
			return false;
		}

		for (UEdGraphNode* GraphNode : Graph->Nodes)
		{
			UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(GraphNode);
			if (!ModuleNode || !ModuleNode->FunctionScript || !ModuleNode->FunctionScript->GetPathName().Contains(ModulePath))
			{
				continue;
			}

			const FNiagaraParameterHandle ModuleParameter = FNiagaraParameterHandle::CreateModuleParameterHandle(InputName);
			const FNiagaraParameterHandle AliasedParameter = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleParameter, ModuleNode);
			const FNiagaraVariable InputVariable(InputType, AliasedParameter.GetParameterHandleString());
			FNiagaraVariable RapidIterationParameter = FNiagaraUtilities::ConvertVariableToRapidIterationConstantName(
				InputVariable,
				*Emitter->GetUniqueEmitterName(),
				TargetScript->GetUsage());
			RapidIterationParameter.SetValue(Value);
			TargetScript->RapidIterationParameters.SetParameterData(RapidIterationParameter.GetData(), RapidIterationParameter, true);
			return true;
		}

		return false;
	}

	UNiagaraEmitter* CreateEmitter(const FString& Name, float SpawnRate, FVector3f Velocity, int32 MaxSpawnPerFrame)
	{
		const FString PackagePath = FxRoot + TEXT("/Emitters/") + Name;
		const FString ObjectPath = PackagePath + TEXT(".") + Name;
		if (UNiagaraEmitter* ExistingEmitter = LoadObject<UNiagaraEmitter>(nullptr, *ObjectPath))
		{
			FVersionedNiagaraEmitterData* ExistingData = ExistingEmitter->GetLatestEmitterData();
			const bool bGpuConfigured = ExistingData && ExistingData->SimTarget == ENiagaraSimTarget::GPUComputeSim && ExistingData->MaxGPUParticlesSpawnPerFrame > 0;
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_NIAGARA_EMITTER_AUDIT %s SimTarget=%s MaxSpawnPerFrame=%d"),
				*ObjectPath,
				bGpuConfigured ? TEXT("GPUComputeSim") : TEXT("NOT_GPU_COMPUTE"),
				ExistingData ? ExistingData->MaxGPUParticlesSpawnPerFrame : 0);
			return bGpuConfigured ? ExistingEmitter : nullptr;
		}

		UPackage* Package = CreatePackage(*PackagePath);
		UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
		if (!Emitter)
		{
			return nullptr;
		}

		UNiagaraEmitterFactoryNew::InitializeEmitter(Emitter, true);
		Emitter->SetUniqueEmitterName(Name);
		FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
		if (!EmitterData)
		{
			return nullptr;
		}

		EmitterData->SimTarget = ENiagaraSimTarget::GPUComputeSim;
		EmitterData->MaxGPUParticlesSpawnPerFrame = MaxSpawnPerFrame;
		EmitterData->AllocationMode = EParticleAllocationMode::FixedCount;
		EmitterData->PreAllocationCount = MaxSpawnPerFrame * 8;
		EmitterData->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Fixed;
		EmitterData->FixedBounds = FBox(FVector(-2000.0f), FVector(2000.0f));

		const bool bSpawnRateSet = SetRapidIterationParameter(
			Emitter,
			EmitterData->EmitterUpdateScriptProps.Script,
			TEXT("/Niagara/Modules/Emitter/SpawnRate"),
			TEXT("SpawnRate"),
			FNiagaraTypeDefinition::GetFloatDef(),
			SpawnRate);
		const bool bVelocitySet = SetRapidIterationParameter(
			Emitter,
			EmitterData->SpawnScriptProps.Script,
			TEXT("/Niagara/Modules/Spawn/Velocity/AddVelocity"),
			TEXT("Velocity"),
			FNiagaraTypeDefinition::GetVec3Def(),
			Velocity);
		if (!bSpawnRateSet || !bVelocitySet)
		{
			UE_LOG(LogTemp, Error, TEXT("AFTERSHOCK Niagara emitter %s did not expose the expected SpawnRate/Velocity modules."), *Name);
			return nullptr;
		}

		UNiagaraSystem::RequestCompileForEmitter(FVersionedNiagaraEmitter(Emitter, Emitter->GetExposedVersion().VersionGuid));
		FAssetRegistryModule::AssetCreated(Emitter);
		if (!SaveAssetPackage(Emitter))
		{
			return nullptr;
		}

		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_NIAGARA_EMITTER %s SimTarget=GPUComputeSim Rate=%.1f MaxSpawnPerFrame=%d"), *ObjectPath, SpawnRate, MaxSpawnPerFrame);
		return Emitter;
	}

	UNiagaraSystem* CreateSystem(const FString& Name, UNiagaraEmitter* Emitter)
	{
		if (!Emitter)
		{
			return nullptr;
		}

		const FString PackagePath = FxRoot + TEXT("/Systems/") + Name;
		const FString ObjectPath = PackagePath + TEXT(".") + Name;
		if (UNiagaraSystem* ExistingSystem = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath))
		{
			ExistingSystem->RequestCompile(true);
			ExistingSystem->WaitForCompilationComplete(true, false);
			const bool bValidSystem = ExistingSystem->GetNumEmitters() > 0 && ExistingSystem->IsValid();
			UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_NIAGARA_SYSTEM_AUDIT %s EmitterCount=%d Valid=%s"),
				*ObjectPath, ExistingSystem->GetNumEmitters(), bValidSystem ? TEXT("true") : TEXT("false"));
			return bValidSystem ? ExistingSystem : nullptr;
		}

		UPackage* Package = CreatePackage(*PackagePath);
		UNiagaraSystem* System = NewObject<UNiagaraSystem>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
		if (!System)
		{
			return nullptr;
		}

		UNiagaraSystemFactoryNew::InitializeSystem(System, true);
		System->AddEmitterHandle(*Emitter, FName(*Name), Emitter->GetExposedVersion().VersionGuid);
		System->RequestCompile(true);
		System->WaitForCompilationComplete(true, false);
		FAssetRegistryModule::AssetCreated(System);
		if (!SaveAssetPackage(System))
		{
			return nullptr;
		}

		UE_LOG(LogTemp, Display, TEXT("AFTERSHOCK_NIAGARA_SYSTEM %s EmitterCount=%d Valid=%s"), *ObjectPath, System->GetNumEmitters(), System->IsValid() ? TEXT("true") : TEXT("false"));
		return System;
	}

	bool CreateEffect(const FString& SystemName, float SpawnRate, FVector3f Velocity, int32 MaxSpawnPerFrame)
	{
		const FString EmitterName = SystemName.Replace(TEXT("NS_"), TEXT("NE_"));
		UNiagaraEmitter* Emitter = CreateEmitter(EmitterName, SpawnRate, Velocity, MaxSpawnPerFrame);
		return CreateSystem(SystemName, Emitter) != nullptr;
	}
}

UAftershockFxSetupCommandlet::UAftershockFxSetupCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UAftershockFxSetupCommandlet::Main(const FString& Params)
{
	IFileManager::Get().MakeDirectory(*FPackageName::LongPackageNameToFilename(FxRoot, TEXT("")), true);
	IFileManager::Get().MakeDirectory(*FPackageName::LongPackageNameToFilename(FxRoot + TEXT("/Emitters"), TEXT("")), true);
	IFileManager::Get().MakeDirectory(*FPackageName::LongPackageNameToFilename(FxRoot + TEXT("/Systems"), TEXT("")), true);

	const bool bFloodCreated = CreateEffect(TEXT("NS_FloodSpray"), 38.0f, FVector3f(45.0f, 0.0f, 110.0f), 64);
	const bool bDustCreated = CreateEffect(TEXT("NS_EarthquakeDust"), 18.0f, FVector3f(0.0f, 0.0f, 24.0f), 32);
	const bool bWindCreated = CreateEffect(TEXT("NS_WindDebris"), 22.0f, FVector3f(220.0f, 15.0f, 28.0f), 32);
	return bFloodCreated && bDustCreated && bWindCreated ? 0 : 1;
}

#else

UAftershockFxSetupCommandlet::UAftershockFxSetupCommandlet() {}

int32 UAftershockFxSetupCommandlet::Main(const FString& Params)
{
	return 1;
}

#endif // WITH_EDITOR
