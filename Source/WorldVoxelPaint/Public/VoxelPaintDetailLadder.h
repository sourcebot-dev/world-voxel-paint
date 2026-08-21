// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "WorldVoxelPaintConstants.h"

namespace WorldVoxelPaint
{
	struct FVoxelPaintLadderClamps
	{
		double MinVoxelSize = 0.5;
		int32 MaxBrickProduct = AssetAdmissionCap;
		int32 MaxBricksPerAxis = 0;
	};

	struct WORLDVOXELPAINT_API FVoxelPaintLadderState
	{
		FVector HalfExtent = FVector(500.0);
		FIntVector BrickCounts = FIntVector(4, 4, 4);

		FVector GetTexelSize() const;
		FIntVector GetResolution() const;
		int64 GetWorstCaseBricks() const;
		int64 GetWorstCasePoolBytes() const;
	};

	WORLDVOXELPAINT_API bool StepRung(FVoxelPaintLadderState& State, int32 Axis, int32 Delta,
		const FVoxelPaintLadderClamps& Clamps);

	WORLDVOXELPAINT_API bool SelectRungForTexelSize(FVoxelPaintLadderState& State, int32 Axis,
		double DesiredTexelSize, const FVoxelPaintLadderClamps& Clamps);

	WORLDVOXELPAINT_API bool ValidateLadderState(const FVoxelPaintLadderState& State,
		const FVoxelPaintLadderClamps& Clamps, FString& OutError);
}
