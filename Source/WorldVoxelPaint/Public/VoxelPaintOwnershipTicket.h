// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneView.h"

enum class EVoxelPaintFamilyVerdict : uint8
{
	Neutral,
	MasterPrimary
};

// Written once at game-thread stamping, never mutated after: the shared ref is carried by the
// render-thread family copy.
class FVoxelPaintOwnershipTicket : public ISceneViewFamilyExtentionData
{
public:
	inline static const TCHAR* const GSubclassIdentifier = TEXT("FVoxelPaintOwnershipTicket");

	virtual const TCHAR* GetSubclassIdentifier() const override
	{
		return GSubclassIdentifier;
	}

	uint64 OwnerEpoch = 0;
	EVoxelPaintFamilyVerdict Verdict = EVoxelPaintFamilyVerdict::Neutral;
};

namespace WorldVoxelPaint
{
	// Render as master iff a ticket exists, its verdict is MasterPrimary, and its epoch EQUALS
	// the fenced render-thread current epoch; older is stale rejection, missing is neutral.
	WORLDVOXELPAINT_API bool ShouldRenderAsMaster(const FVoxelPaintOwnershipTicket* Ticket,
		uint64 RenderThreadCurrentEpoch);
}
