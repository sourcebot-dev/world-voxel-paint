// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelPaintDetailLadder.h"

class AActor;
class UVoxelPaintVolumeComponent;
class UVoxelPaintVolumeData;
class UWorld;

namespace WorldVoxelPaint
{
	WORLDVOXELPAINTTOOLSET_API FVoxelPaintLadderClamps BuildLadderClamps(const UWorld* World);

	WORLDVOXELPAINTTOOLSET_API FText GetResetDataConfirmationText(const UVoxelPaintVolumeData* Asset);

	// Outside sessions only; authority-gated; ordinary durable transaction (Modify +
	// ResetCanonicalData - the accepted large-entry cost of the rare destructive op).
	WORLDVOXELPAINTTOOLSET_API bool ResetDataAndUnlockSize(UVoxelPaintVolumeData* Asset,
		const AActor* InitiatingActor, FText& OutError);

	// The sizing tool's commit primitive: authority + session + topology-lock gates, ladder
	// validation, one durable transaction applying extent and brick counts. A non-null
	// ExpectationTarget (callers route it via ResolveExpectationWriteTarget) has its
	// ExpectedTopology written in the same transaction; a template target additionally
	// propagates to matching archetype instances. Null is permitted only for asset-only
	// contexts with no consuming component.
	WORLDVOXELPAINTTOOLSET_API bool CommitPreInitTopologyEdit(UVoxelPaintVolumeData* Asset,
		const AActor* InitiatingActor, UVoxelPaintVolumeComponent* ExpectationTarget,
		const FVoxelPaintLadderState& NewState, FText& OutError);
}
