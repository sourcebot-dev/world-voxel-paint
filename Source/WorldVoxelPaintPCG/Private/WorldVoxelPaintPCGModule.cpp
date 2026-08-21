// Copyright Epic Games, Inc. All Rights Reserved.

#include "WorldVoxelPaintPCGModule.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

#if WITH_EDITOR
#include "VoxelPaintPCGRefreshHandler.h"
#endif

void FWorldVoxelPaintPCGModule::StartupModule()
{
#if WITH_EDITOR
	// Kernel and DI-template text is read only at editor graph-compile time (GetCookedSource /
	// GetHLSL); cooked builds carry compiled compute shader maps and never resolve this
	// mapping - so it is editor-gated, mirroring PCG's own kernel-source mappings
	// (PCGModule.cpp / PCGComputeModule.cpp), not the core module's unconditional
	// global-shader mapping. The mapped directory must exist on disk before this runs
	// (AddShaderSourceDirectoryMapping checkf-fails otherwise); Shaders/PCG ships with the
	// plugin (kernel .usf + DI .ush live there). FindPlugin takes the PLUGIN name - this
	// module lives inside the WorldVoxelPaint plugin.
	const FString PluginShaderDir = FPaths::Combine(
		IPluginManager::Get().FindPlugin(TEXT("WorldVoxelPaint"))->GetBaseDir(), TEXT("Shaders"), TEXT("PCG"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/WorldVoxelPaintPCG"), PluginShaderDir);

	// Paint-commit -> PCG refresh fan-out (M6 plan 7.2); handler class owned/specified by
	// spec-refresh (VoxelPaintPCGRefreshHandler.h/.cpp). Editor-only by design: painting is
	// editor-only and PCG's change-tracking apparatus does not exist at runtime. Startup is
	// idempotent (delegate-handle guard inside).
	FVoxelPaintPCGRefreshHandler::Startup();
#endif
}

void FWorldVoxelPaintPCGModule::ShutdownModule()
{
#if WITH_EDITOR
	FVoxelPaintPCGRefreshHandler::Shutdown();
#endif
}

IMPLEMENT_MODULE(FWorldVoxelPaintPCGModule, WorldVoxelPaintPCG)
