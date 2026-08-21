// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "VoxelPaintSessionCpuState.h"

class FVoxelPaintSessionResources;

namespace WorldVoxelPaint
{
	class FVoxelPaintSessionBudgetLedger;
}

// Game-thread entry points; each operation enqueues ONE render command running its own
// FRDGBuilder on the one graphics queue (no async compute), so editor-tick submissions land
// ahead of that frame's base pass in submission order. Resources and Ledger must outlive every
// in-flight operation: CancelAndDrain before releasing either.
class WORLDVOXELPAINTTOOLSET_API FVoxelPaintSessionTransferEngine
{
public:
	FVoxelPaintSessionTransferEngine() = default;
	~FVoxelPaintSessionTransferEngine();
	FVoxelPaintSessionTransferEngine(const FVoxelPaintSessionTransferEngine&) = delete;
	FVoxelPaintSessionTransferEngine& operator=(const FVoxelPaintSessionTransferEngine&) = delete;

	void Hydrate(FVoxelPaintSessionResources& Resources,
		WorldVoxelPaint::FVoxelPaintPackedBrickTransfer&& Transfer,
		WorldVoxelPaint::FVoxelPaintSessionBudgetLedger& Ledger, TFunction<void(bool)> OnComplete);

	void UploadAndPublish(FVoxelPaintSessionResources& Resources,
		WorldVoxelPaint::FVoxelPaintPackedBrickTransfer&& Transfer,
		WorldVoxelPaint::FVoxelPaintSessionBudgetLedger& Ledger, TFunction<void(bool)> OnComplete);

	void ReadbackDirtyBricks(FVoxelPaintSessionResources& Resources, TArray<uint32>&& DirtyBricks,
		WorldVoxelPaint::FVoxelPaintSessionBudgetLedger& Ledger,
		TFunction<void(bool, WorldVoxelPaint::FVoxelPaintPackedBrickTransfer&&)> OnComplete);

	void CopyRegionWorkingToPublished(FVoxelPaintSessionResources& Resources,
		const FIntVector& TexelMin, const FIntVector& TexelMax);

	bool IsTransferInFlight() const;
	void CancelAndDrain(TFunction<void()> OnDrained);

private:
	struct FReadbackRequest;

	void StartScatterOp(FVoxelPaintSessionResources& Resources,
		WorldVoxelPaint::FVoxelPaintPackedBrickTransfer&& Transfer,
		WorldVoxelPaint::FVoxelPaintSessionBudgetLedger& Ledger, TFunction<void(bool)> OnComplete,
		bool bHydrate);
	void FinalizeStreamOp(bool bOk, WorldVoxelPaint::FVoxelPaintSessionBudgetLedger& Ledger,
		int32 StagingHandle, TFunction<void(bool)>& OnComplete);
	bool TickReadback(float DeltaSeconds);
	void FinalizeReadback(bool bOk);
	void MaybeFireDrain();

	TSharedPtr<FReadbackRequest, ESPMode::ThreadSafe> ActiveReadback;
	FTSTicker::FDelegateHandle ReadbackTickerHandle;
	int32 PendingOps = 0;
	bool bCancelDraining = false;
	TFunction<void()> PendingDrain;
};
