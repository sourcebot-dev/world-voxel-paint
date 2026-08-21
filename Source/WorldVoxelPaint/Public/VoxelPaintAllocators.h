// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

// Both allocators are pure CPU state, render-thread owned after coordinator init; tests may
// drive them single-threaded. Determinism is contractual: block pops ascend from 0 on a fresh
// allocator; range reservation is first-fit at the lowest free offset.

class WORLDVOXELPAINT_API FVoxelPaintBlockAllocator
{
public:
	void Initialize(int32 InMaxNumBlocks);
	bool Allocate(int32 NumBlocks, TArray<int32>& OutBlocks);
	void Free(TConstArrayView<int32> Blocks);

	int32 GetMaxNumBlocks() const
	{
		return MaxNumBlocks;
	}

	int32 GetNumFree() const
	{
		return FreeBlocks.Num();
	}

	int32 GetNumAllocated() const
	{
		return MaxNumBlocks - FreeBlocks.Num();
	}

private:
	int32 MaxNumBlocks = 0;
	TArray<int32> FreeBlocks;
};

enum class EVoxelPaintRangeState : uint8 { Reserved, Uploading, Active, Retiring };

class WORLDVOXELPAINT_API FVoxelPaintPageTableAllocator
{
public:
	void Initialize(int32 InCapacityEntries);
	int32 Reserve(int32 NumEntries);
	void SetState(int32 Handle, EVoxelPaintRangeState NewState);
	void Retire(int32 Handle, uint64 RetireOrdinal);
	void ReleaseRetired(uint64 CompletedOrdinal);
	int32 GetRangeOffset(int32 Handle) const;
	int32 GetRangeCount(int32 Handle) const;
	EVoxelPaintRangeState GetRangeState(int32 Handle) const;

	int32 GetEntries(EVoxelPaintRangeState State) const;

	int32 GetFreeEntries() const
	{
		return FreeEntries;
	}

	int32 GetLargestFreeRange() const;

	int32 GetCapacityEntries() const
	{
		return CapacityEntries;
	}

private:
	struct FRange
	{
		int32 Offset = 0;
		int32 Count = 0;
		EVoxelPaintRangeState State = EVoxelPaintRangeState::Reserved;
		uint64 RetireOrdinal = 0;
		bool bLive = false;
	};

	struct FFreeSpan
	{
		int32 Offset = 0;
		int32 Count = 0;
	};

	void ReleaseSpan(int32 Offset, int32 Count);

	int32 CapacityEntries = 0;
	int32 FreeEntries = 0;
	TArray<FRange> Ranges;
	TArray<int32> FreeHandles;
	TArray<FFreeSpan> FreeSpans;
};
