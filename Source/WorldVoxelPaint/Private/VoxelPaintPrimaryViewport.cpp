// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintPrimaryViewport.h"

#include "CoreGlobals.h"

namespace WorldVoxelPaint
{
	static FViewport* GActivePrimaryViewport = nullptr;

	void SetActivePrimaryViewport(FViewport* Viewport)
	{
		check(IsInGameThread());
		GActivePrimaryViewport = Viewport;
	}

	FViewport* GetActivePrimaryViewport()
	{
		check(IsInGameThread());
		return GActivePrimaryViewport;
	}
}
