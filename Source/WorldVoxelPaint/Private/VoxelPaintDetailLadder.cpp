// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintDetailLadder.h"

namespace WorldVoxelPaint
{

namespace
{

double TexelSizeForCount(double HalfExtent, int64 BrickCount)
{
	return (2.0 * HalfExtent) / (double)(UniqueBrickSize * BrickCount);
}

int64 MaxCountForAxis(const FVoxelPaintLadderState& State, int32 Axis,
	const FVoxelPaintLadderClamps& Clamps)
{
	int64 MaxCount = MAX_int32;

	if (Clamps.MaxBricksPerAxis > 0)
	{
		MaxCount = FMath::Min<int64>(MaxCount, Clamps.MaxBricksPerAxis);
	}

	int64 OtherAxesProduct = 1;
	for (int32 OtherAxis = 0; OtherAxis < 3; ++OtherAxis)
	{
		if (OtherAxis != Axis)
		{
			OtherAxesProduct *= FMath::Max(1, State.BrickCounts[OtherAxis]);
		}
	}
	if (Clamps.MaxBrickProduct > 0)
	{
		MaxCount = FMath::Min<int64>(MaxCount, Clamps.MaxBrickProduct / OtherAxesProduct);
	}

	if (Clamps.MinVoxelSize > 0.0)
	{
		const double MaxByVoxel =
			(2.0 * State.HalfExtent[Axis]) / ((double)UniqueBrickSize * Clamps.MinVoxelSize);
		MaxCount = FMath::Min<int64>(MaxCount,
			(int64)FMath::FloorToDouble(MaxByVoxel * (1.0 + UE_DOUBLE_SMALL_NUMBER)));
	}

	return MaxCount;
}

} // namespace

FVector FVoxelPaintLadderState::GetTexelSize() const
{
	return (2.0 * HalfExtent) / FVector(GetResolution());
}

FIntVector FVoxelPaintLadderState::GetResolution() const
{
	return BrickCounts * UniqueBrickSize;
}

int64 FVoxelPaintLadderState::GetWorstCaseBricks() const
{
	return (int64)BrickCounts.X * BrickCounts.Y * BrickCounts.Z;
}

int64 FVoxelPaintLadderState::GetWorstCasePoolBytes() const
{
	return GetWorstCaseBricks() * BrickPayloadBytes;
}

bool StepRung(FVoxelPaintLadderState& State, int32 Axis, int32 Delta,
	const FVoxelPaintLadderClamps& Clamps)
{
	if (Axis < 0 || Axis > 2)
	{
		return false;
	}

	const int64 MaxCount = MaxCountForAxis(State, Axis, Clamps);
	if (MaxCount < 1)
	{
		return false;
	}

	const int64 Candidate =
		FMath::Clamp<int64>((int64)State.BrickCounts[Axis] + Delta, 1, MaxCount);
	if (Candidate == State.BrickCounts[Axis])
	{
		return false;
	}

	State.BrickCounts[Axis] = (int32)Candidate;
	return true;
}

bool SelectRungForTexelSize(FVoxelPaintLadderState& State, int32 Axis,
	double DesiredTexelSize, const FVoxelPaintLadderClamps& Clamps)
{
	if (Axis < 0 || Axis > 2 || DesiredTexelSize <= 0.0 || State.HalfExtent[Axis] <= 0.0)
	{
		return false;
	}

	const int64 MaxCount = MaxCountForAxis(State, Axis, Clamps);
	if (MaxCount < 1)
	{
		return false;
	}

	const double IdealCount =
		(2.0 * State.HalfExtent[Axis]) / ((double)UniqueBrickSize * DesiredTexelSize);
	const int64 CoarserCount = FMath::Max<int64>(1, (int64)FMath::FloorToDouble(IdealCount));
	const int64 FinerCount = FMath::Max<int64>(1, (int64)FMath::CeilToDouble(IdealCount));

	const double CoarserError =
		FMath::Abs(TexelSizeForCount(State.HalfExtent[Axis], CoarserCount) - DesiredTexelSize);
	const double FinerError =
		FMath::Abs(TexelSizeForCount(State.HalfExtent[Axis], FinerCount) - DesiredTexelSize);

	const int64 Nearest = (FinerError <= CoarserError) ? FinerCount : CoarserCount;
	State.BrickCounts[Axis] = (int32)FMath::Clamp<int64>(Nearest, 1, MaxCount);
	return true;
}

bool ValidateLadderState(const FVoxelPaintLadderState& State,
	const FVoxelPaintLadderClamps& Clamps, FString& OutError)
{
	OutError.Reset();

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (State.HalfExtent[Axis] <= 0.0)
		{
			OutError = FString::Printf(
				TEXT("Half extent must be positive on every axis (axis %d is %g)."),
				Axis, State.HalfExtent[Axis]);
			return false;
		}
		if (State.BrickCounts[Axis] < 1)
		{
			OutError = FString::Printf(
				TEXT("Brick count must be at least 1 on every axis (axis %d is %d)."),
				Axis, State.BrickCounts[Axis]);
			return false;
		}
		if (Clamps.MaxBricksPerAxis > 0 && State.BrickCounts[Axis] > Clamps.MaxBricksPerAxis)
		{
			OutError = FString::Printf(
				TEXT("Brick count %d on axis %d exceeds the per-axis platform texture limit of %d bricks."),
				State.BrickCounts[Axis], Axis, Clamps.MaxBricksPerAxis);
			return false;
		}
	}

	if (Clamps.MaxBrickProduct > 0 && State.GetWorstCaseBricks() > Clamps.MaxBrickProduct)
	{
		OutError = FString::Printf(
			TEXT("Brick grid %d x %d x %d exceeds the %d-brick admission cap - split into multiple volumes."),
			State.BrickCounts.X, State.BrickCounts.Y, State.BrickCounts.Z, Clamps.MaxBrickProduct);
		return false;
	}

	if (Clamps.MinVoxelSize > 0.0)
	{
		const FVector TexelSize = State.GetTexelSize();
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (TexelSize[Axis] * (1.0 + UE_DOUBLE_SMALL_NUMBER) < Clamps.MinVoxelSize)
			{
				OutError = FString::Printf(
					TEXT("Resolved texel size %g on axis %d is below MinVoxelSize %g - lower the detail rung or enlarge the volume."),
					TexelSize[Axis], Axis, Clamps.MinVoxelSize);
				return false;
			}
		}
	}

	return true;
}

} // namespace WorldVoxelPaint
