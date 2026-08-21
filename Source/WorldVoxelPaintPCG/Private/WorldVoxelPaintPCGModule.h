// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

// M6: PCG-facing module of the World Voxel Paint plugin. Hosts the GPU sampling node
// (settings/kernel/data-interface) and, editor-only, the paint-commit refresh handler.
// Core modules never reference PCG; everything PCG-facing lives here (M6 plan section 4).
class FWorldVoxelPaintPCGModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
