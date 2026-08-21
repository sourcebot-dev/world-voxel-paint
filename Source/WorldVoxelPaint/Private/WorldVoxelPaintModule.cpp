// Copyright Epic Games, Inc. All Rights Reserved.

#include "WorldVoxelPaintConstants.h"
#include "VoxelPaintCoordinator.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/CoreMisc.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY(LogWorldVoxelPaint);

class FWorldVoxelPaintModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void OnPostEngineInit();

	TSharedPtr<FVoxelPaintCoordinator, ESPMode::ThreadSafe> Coordinator;
	FDelegateHandle PostEngineInitHandle;
};

void FWorldVoxelPaintModule::StartupModule()
{
	const FString PluginShaderDir = FPaths::Combine(
		IPluginManager::Get().FindPlugin(TEXT("WorldVoxelPaint"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/WorldVoxelPaint"), PluginShaderDir);
	PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FWorldVoxelPaintModule::OnPostEngineInit);
}

void FWorldVoxelPaintModule::ShutdownModule()
{
	FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
	PostEngineInitHandle.Reset();
	// Clear the process-global on the game thread before releasing ownership: the last shared
	// reference can be released on the render thread (the engine keeps extensions one extra frame).
	FVoxelPaintCoordinator::Deactivate();
	Coordinator.Reset();
}

void FWorldVoxelPaintModule::OnPostEngineInit()
{
	if (!IsRunningDedicatedServer())
	{
		Coordinator = FVoxelPaintCoordinator::CreateAndRegister();
	}
}

IMPLEMENT_MODULE(FWorldVoxelPaintModule, WorldVoxelPaint)
