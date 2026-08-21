// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RHIFwd.h"
#include "Templates/RefCounting.h"
#include "Templates/SharedPointer.h"
#include "VoxelPaintResidencyTypes.h"

class FRDGBuilder;
class FRDGPooledBuffer;
class FVoxelPaintResidencyManager;
class FVoxelPaintSceneParameters;
struct FVoxelPaintExternalSamplingParams;

// Render-thread owned. Capacities are read once at Initialize and fixed for the process
// lifetime; pool slots round UP to full 128x128-brick Z layers. Preparation tasks and their
// completion render commands capture the residency manager WEAKLY: a completion delivered
// after the owning coordinator's teardown drops its payload instead of dereferencing freed
// memory (a detached task can outlive the render-command drain).
class WORLDVOXELPAINT_API FVoxelPaintPoolRenderer
{
public:
	FVoxelPaintPoolRenderer();
	~FVoxelPaintPoolRenderer();
	FVoxelPaintPoolRenderer(const FVoxelPaintPoolRenderer&) = delete;
	FVoxelPaintPoolRenderer& operator=(const FVoxelPaintPoolRenderer&) = delete;

	void Initialize(FRDGBuilder& GraphBuilder, int64 PoolCapacityBytes, int64 PageTableCapacityBytes);
	bool IsInitialized() const;
	int32 GetPoolSlotCapacity() const;
	int32 GetPageEntryCapacity() const;
	FIntVector GetPoolDimsInBricks() const;

	// Retains the newest snapshot per asset and drives LaunchPendingPreparations. At most one
	// task launches per (asset, source generation, owner epoch); relaunch after an epoch
	// advance happens at the next call of either launch method.
	void LaunchPreparation(TSharedPtr<FVoxelPaintPreparedSourceSnapshot> Snapshot,
		uint64 OwnerEpoch, const TSharedRef<FVoxelPaintResidencyManager, ESPMode::ThreadSafe>& Residency);
	void LaunchPendingPreparations(uint64 OwnerEpoch,
		const TSharedRef<FVoxelPaintResidencyManager, ESPMode::ThreadSafe>& Residency);

	// Retained snapshots follow the desired set: prune on every desired-set shipment, clear on
	// master-world teardown (retention only spans an epoch advance for still-desired assets).
	void PruneRetainedSnapshots(const TSet<FVoxelPaintAssetKey>& DesiredKeys);
	void ClearRetainedSnapshots();

	void ExecuteOps(FRDGBuilder& GraphBuilder, const FVoxelPaintIntegrationOps& Ops);
	void Register(FRDGBuilder& GraphBuilder, FVoxelPaintSceneParameters& Params) const;
	// Grid-free external-sampling variant (M6): fills only the pool-owned subset (BrickPool,
	// PageTables SRV, PoolSampler, pool geometry scalars). No-op while uninitialized - the
	// caller's neutral bindings survive.
	void Register(FRDGBuilder& GraphBuilder, FVoxelPaintExternalSamplingParams& Params) const;

private:
	struct FRetainedSnapshot
	{
		TSharedPtr<FVoxelPaintPreparedSourceSnapshot> Snapshot;
		uint64 LaunchedGeneration = 0;
		uint64 LaunchedEpoch = 0;
	};

	void LaunchTask(const TSharedPtr<FVoxelPaintPreparedSourceSnapshot>& Snapshot,
		uint64 OwnerEpoch, const TSharedRef<FVoxelPaintResidencyManager, ESPMode::ThreadSafe>& Residency);

	FTextureRHIRef PoolTexture;
	TRefCountPtr<FRDGPooledBuffer> PageTableBuffer;
	TMap<FVoxelPaintAssetKey, FRetainedSnapshot> RetainedSnapshots;
	FIntVector PoolDimsInBricks = FIntVector(1, 1, 1);
	int32 PoolSlotCapacity = 0;
	int32 PageEntryCapacity = 0;
	bool bInitialized = false;
};
