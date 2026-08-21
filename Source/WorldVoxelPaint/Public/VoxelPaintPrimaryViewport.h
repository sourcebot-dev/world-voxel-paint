// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FViewport;

namespace WorldVoxelPaint
{
	// Game thread only. The editor module pushes the active Level Editor viewport; the runtime
	// coordinator reads it for primary-family designation. Compared by identity, never
	// dereferenced.
	WORLDVOXELPAINT_API void SetActivePrimaryViewport(FViewport* Viewport);
	WORLDVOXELPAINT_API FViewport* GetActivePrimaryViewport();
}
