// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelPaintPickProvider.h"

class FVoxelPaintSessionController;
class FVoxelPaintSessionResources;
class FVoxelPaintSessionTransferEngine;

namespace WorldVoxelPaint
{
	class FVoxelPaintSessionBudgetLedger;
	class FVoxelPaintSessionCpuState;
	struct FVoxelPaintPackedBrickTransfer;
	struct FVoxelPaintStrokeDelta;
}

struct FVoxelPaintStrokePipelineSessionBindings
{
	FVoxelPaintSessionController* Controller = nullptr;
	FVoxelPaintSessionResources* Resources = nullptr;
	FVoxelPaintSessionTransferEngine* Transfers = nullptr;
	WorldVoxelPaint::FVoxelPaintSessionCpuState* CpuState = nullptr;
	WorldVoxelPaint::FVoxelPaintSessionBudgetLedger* Ledger = nullptr;
	FIntVector BrickCounts = FIntVector::ZeroValue;
	FVector TexelSize = FVector::ZeroVector;

	bool IsValid() const
	{
		return Controller && Resources && Transfers && CpuState && Ledger
			&& BrickCounts.GetMin() > 0 && TexelSize.GetMin() > 0.0;
	}
};

// Game thread. The bound session objects must outlive every pending close/upload callback;
// the session controller drains the transfer engine before releasing them or this pipeline.
class WORLDVOXELPAINTTOOLSET_API FVoxelPaintStrokePipeline
{
public:
	static void BuildWholeVolumeGatherList(const FIntVector& BrickCounts, FVoxelPaintGatherList& Out);

	void BindSession(const FVoxelPaintStrokePipelineSessionBindings& InBindings);
	void UnbindSession();

	bool BeginStroke(EVoxelPaintStrokeOp Op, const FVoxelPaintBrushParams& Brush, FText& OutWhyNot);
	void ApplyStamp(FVoxelPaintGatherList&& Gather);
	void RequestCloseStroke();
	void CancelStroke();
	bool IsCloseResolved() const;
	bool IsStrokeOpen() const;

private:
	void EnqueueApplyDispatch(FVoxelPaintGatherList&& Gather, bool bMidStrokePublication);
	void AccumulateStrokeDirty(const FVoxelPaintGatherList& Gather);
	void RestoreStrokeRegionFromCpu();
	void OnCloseReadback(bool bOk, WorldVoxelPaint::FVoxelPaintPackedBrickTransfer&& Readback);

	FVoxelPaintStrokePipelineSessionBindings Bindings;
	EVoxelPaintStrokeOp StrokeOp = EVoxelPaintStrokeOp::Paint;
	FVoxelPaintBrushParams StrokeBrush;
	TArray<uint32> StrokeDirtyBricks;
	FIntVector StrokeDirtyMin = FIntVector::ZeroValue;
	FIntVector StrokeDirtyMax = FIntVector::ZeroValue;
	int64 ReservedStrokeBricks = 0;
	bool bStrokeDirtyValid = false;
	bool bStrokeOpen = false;
	bool bCloseResolved = true;
	bool bStrokeGrowthDeniedLogged = false;
};
