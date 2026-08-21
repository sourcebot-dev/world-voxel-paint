// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Map.h"
#include "Containers/Set.h"
#include "CoreMinimal.h"
#include "VoxelPaintResidencyTypes.h"

namespace WorldVoxelPaint
{
	// Game-thread bookkeeping for desired-asset preparation snapshots. A pack stages the full
	// desired set between BeginPack and EndPack; EndPack commits it, pruning every key not
	// re-packed. A pending key is one whose snapshot capture returned capture-pending (an
	// asset build still in flight): while any key is pending the coordinator must keep packing
	// each serviced frame so the finished build is harvested.
	class FVoxelPaintSnapshotCaptureBook
	{
	public:
		bool IsCaptured(const FVoxelPaintAssetKey& Key, uint64 SourceGeneration) const
		{
			const uint64* Captured = CapturedGenerations.Find(Key);
			return Captured && *Captured == SourceGeneration;
		}

		bool HasPendingCaptures() const
		{
			return !PendingCaptures.IsEmpty();
		}

		void BeginPack()
		{
			StagedGenerations.Reset();
			StagedPending.Reset();
		}

		void PackCaptured(const FVoxelPaintAssetKey& Key, uint64 SourceGeneration)
		{
			StagedGenerations.Add(Key, SourceGeneration);
		}

		void PackPending(const FVoxelPaintAssetKey& Key)
		{
			StagedPending.Add(Key);
		}

		void EndPack()
		{
			CapturedGenerations = MoveTemp(StagedGenerations);
			PendingCaptures = MoveTemp(StagedPending);
			StagedGenerations.Reset();
			StagedPending.Reset();
		}

		void Remove(const FVoxelPaintAssetKey& Key)
		{
			CapturedGenerations.Remove(Key);
			PendingCaptures.Remove(Key);
			StagedGenerations.Remove(Key);
			StagedPending.Remove(Key);
		}

		void Empty()
		{
			CapturedGenerations.Empty();
			PendingCaptures.Empty();
			StagedGenerations.Empty();
			StagedPending.Empty();
		}

	private:
		TMap<FVoxelPaintAssetKey, uint64> CapturedGenerations;
		TSet<FVoxelPaintAssetKey> PendingCaptures;
		TMap<FVoxelPaintAssetKey, uint64> StagedGenerations;
		TSet<FVoxelPaintAssetKey> StagedPending;
	};
}
