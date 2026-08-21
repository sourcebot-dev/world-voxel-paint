// Copyright Epic Games, Inc. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "VoxelPaintSessionController.h"
#include "VoxelPaintTools.h"

class FWorldVoxelPaintToolsetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
	}

	virtual void ShutdownModule() override
	{
		// Pick deliveries can reach the pipeline/controller; drain them before the controller
		// (and with it the session surfaces) is destroyed.
		FVoxelPaintToolsContext::Get().ShutdownGpuPickProvider();
		FVoxelPaintSessionController::Shutdown();
	}
};

IMPLEMENT_MODULE(FWorldVoxelPaintToolsetModule, WorldVoxelPaintToolset)
