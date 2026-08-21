// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"
#include "VoxelPaintPickProvider.h"

// GPU visible-pixel selection provider. Requests ride a render-side pick pass injected through
// the coordinator's post-base-pass delegate and return by asynchronous readback (~2 frames);
// every unavailability routes the stamp to the CPU fallback through the same OnReady, so no
// stamp is ever dropped. Game-thread entry points; the pick pass never blocks the game thread
// on GPU work.
class WORLDVOXELPAINTTOOLSET_API FVoxelPaintGpuPickProvider final : public IVoxelPaintPickProvider
{
public:
	explicit FVoxelPaintGpuPickProvider(IVoxelPaintPickProvider& InCpuFallback);
	virtual ~FVoxelPaintGpuPickProvider() override;
	FVoxelPaintGpuPickProvider(const FVoxelPaintGpuPickProvider&) = delete;
	FVoxelPaintGpuPickProvider& operator=(const FVoxelPaintGpuPickProvider&) = delete;

	virtual void RequestStampSelection(const FVoxelPaintStampRequest& Request,
		TFunction<void(FVoxelPaintGatherList&&)> OnReady) override;

	// After OnDrained resolves no callback fires; the provider stays CPU-routing afterwards.
	void CancelAndDrain(TFunction<void()> OnDrained);
	bool HasInFlightRequests() const;
	FName GetLastPickSource() const;

private:
	struct FGpuPickRequest;
	struct FGpuPickRenderState;
	struct FGpuPickDrainState;
	using FGpuPickRequestRef = TSharedPtr<FGpuPickRequest, ESPMode::ThreadSafe>;

	void FallBackToCpu(const TCHAR* Reason, const FVoxelPaintStampRequest& Request,
		TFunction<void(FVoxelPaintGatherList&&)>&& OnReady);
	int32 FindFreeSlot() const;
	void IssueRequest(const FGpuPickRequestRef& Request, int32 SlotIndex);
	void TryIssueCoalesced();
	void EnsureDelegateBound();
	void EnsureTicker();
	bool Tick(float DeltaSeconds);
	void FinishSlot(int32 SlotIndex);
	void DeliverReadback(const FGpuPickRequestRef& Request);

	IVoxelPaintPickProvider& CpuFallback;
	TSharedPtr<FGpuPickRenderState, ESPMode::ThreadSafe> RenderState;
	// Fence bookkeeping outlives the provider: drain-completion tasks may resolve after a
	// teardown that destroyed it, so they touch only this shared state.
	TSharedPtr<FGpuPickDrainState, ESPMode::ThreadSafe> DrainState;
	TArray<FGpuPickRequestRef> SlotRequests;
	FGpuPickRequestRef CoalescedRequest;
	FTSTicker::FDelegateHandle TickerHandle;
	FName LastPickSource;
	bool bDelegateBound = false;
	bool bDraining = false;
	bool bFallbackLoggedSinceGpuDelivery = false;
};
