// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintAllocators.h"

#include "Algo/BinarySearch.h"

void FVoxelPaintBlockAllocator::Initialize(int32 InMaxNumBlocks)
{
	check(InMaxNumBlocks >= 0);
	MaxNumBlocks = InMaxNumBlocks;
	FreeBlocks.Empty(InMaxNumBlocks);
	for (int32 BlockIndex = InMaxNumBlocks - 1; BlockIndex >= 0; --BlockIndex)
	{
		FreeBlocks.Add(BlockIndex);
	}
}

bool FVoxelPaintBlockAllocator::Allocate(int32 NumBlocks, TArray<int32>& OutBlocks)
{
	check(NumBlocks >= 0);
	if (NumBlocks > FreeBlocks.Num())
	{
		return false;
	}

	OutBlocks.Empty(NumBlocks);
	OutBlocks.AddUninitialized(NumBlocks);
	for (int32 Index = 0; Index < NumBlocks; ++Index)
	{
		OutBlocks[Index] = FreeBlocks[FreeBlocks.Num() - Index - 1];
	}
	FreeBlocks.RemoveAt(FreeBlocks.Num() - NumBlocks, NumBlocks, EAllowShrinking::No);
	return true;
}

void FVoxelPaintBlockAllocator::Free(TConstArrayView<int32> Blocks)
{
	FreeBlocks.Append(Blocks.GetData(), Blocks.Num());
}

void FVoxelPaintPageTableAllocator::Initialize(int32 InCapacityEntries)
{
	check(InCapacityEntries >= 0);
	CapacityEntries = InCapacityEntries;
	FreeEntries = InCapacityEntries;
	Ranges.Empty();
	FreeHandles.Empty();
	FreeSpans.Empty();
	if (InCapacityEntries > 0)
	{
		FreeSpans.Add({ 0, InCapacityEntries });
	}
}

int32 FVoxelPaintPageTableAllocator::Reserve(int32 NumEntries)
{
	check(NumEntries > 0);
	for (int32 SpanIndex = 0; SpanIndex < FreeSpans.Num(); ++SpanIndex)
	{
		if (FreeSpans[SpanIndex].Count >= NumEntries)
		{
			const int32 Offset = FreeSpans[SpanIndex].Offset;
			FreeSpans[SpanIndex].Offset += NumEntries;
			FreeSpans[SpanIndex].Count -= NumEntries;
			if (FreeSpans[SpanIndex].Count == 0)
			{
				FreeSpans.RemoveAt(SpanIndex);
			}
			FreeEntries -= NumEntries;

			int32 Handle;
			if (FreeHandles.Num() > 0)
			{
				Handle = FreeHandles.Pop(EAllowShrinking::No);
			}
			else
			{
				Handle = Ranges.AddDefaulted();
			}
			FRange& Range = Ranges[Handle];
			Range.Offset = Offset;
			Range.Count = NumEntries;
			Range.State = EVoxelPaintRangeState::Reserved;
			Range.RetireOrdinal = 0;
			Range.bLive = true;
			return Handle;
		}
	}
	return INDEX_NONE;
}

void FVoxelPaintPageTableAllocator::SetState(int32 Handle, EVoxelPaintRangeState NewState)
{
	FRange& Range = Ranges[Handle];
	check(Range.bLive);
	check(NewState != EVoxelPaintRangeState::Retiring);
	check(static_cast<uint8>(NewState) == static_cast<uint8>(Range.State) + 1);
	Range.State = NewState;
}

void FVoxelPaintPageTableAllocator::Retire(int32 Handle, uint64 RetireOrdinal)
{
	FRange& Range = Ranges[Handle];
	check(Range.bLive);
	check(Range.State != EVoxelPaintRangeState::Retiring);
	Range.State = EVoxelPaintRangeState::Retiring;
	Range.RetireOrdinal = RetireOrdinal;
}

void FVoxelPaintPageTableAllocator::ReleaseRetired(uint64 CompletedOrdinal)
{
	for (int32 Handle = 0; Handle < Ranges.Num(); ++Handle)
	{
		FRange& Range = Ranges[Handle];
		if (Range.bLive && Range.State == EVoxelPaintRangeState::Retiring && Range.RetireOrdinal <= CompletedOrdinal)
		{
			ReleaseSpan(Range.Offset, Range.Count);
			FreeEntries += Range.Count;
			Range.bLive = false;
			FreeHandles.Push(Handle);
		}
	}
}

int32 FVoxelPaintPageTableAllocator::GetRangeOffset(int32 Handle) const
{
	check(Ranges[Handle].bLive);
	return Ranges[Handle].Offset;
}

int32 FVoxelPaintPageTableAllocator::GetRangeCount(int32 Handle) const
{
	check(Ranges[Handle].bLive);
	return Ranges[Handle].Count;
}

EVoxelPaintRangeState FVoxelPaintPageTableAllocator::GetRangeState(int32 Handle) const
{
	check(Ranges[Handle].bLive);
	return Ranges[Handle].State;
}

int32 FVoxelPaintPageTableAllocator::GetEntries(EVoxelPaintRangeState State) const
{
	int32 NumEntries = 0;
	for (const FRange& Range : Ranges)
	{
		if (Range.bLive && Range.State == State)
		{
			NumEntries += Range.Count;
		}
	}
	return NumEntries;
}

int32 FVoxelPaintPageTableAllocator::GetLargestFreeRange() const
{
	int32 Largest = 0;
	for (const FFreeSpan& Span : FreeSpans)
	{
		Largest = FMath::Max(Largest, Span.Count);
	}
	return Largest;
}

void FVoxelPaintPageTableAllocator::ReleaseSpan(int32 Offset, int32 Count)
{
	const int32 InsertIndex = Algo::LowerBoundBy(FreeSpans, Offset, &FFreeSpan::Offset);
	if (InsertIndex > 0 && FreeSpans[InsertIndex - 1].Offset + FreeSpans[InsertIndex - 1].Count == Offset)
	{
		FreeSpans[InsertIndex - 1].Count += Count;
		if (InsertIndex < FreeSpans.Num() &&
			FreeSpans[InsertIndex - 1].Offset + FreeSpans[InsertIndex - 1].Count == FreeSpans[InsertIndex].Offset)
		{
			FreeSpans[InsertIndex - 1].Count += FreeSpans[InsertIndex].Count;
			FreeSpans.RemoveAt(InsertIndex);
		}
	}
	else if (InsertIndex < FreeSpans.Num() && Offset + Count == FreeSpans[InsertIndex].Offset)
	{
		FreeSpans[InsertIndex].Offset = Offset;
		FreeSpans[InsertIndex].Count += Count;
	}
	else
	{
		FreeSpans.Insert({ Offset, Count }, InsertIndex);
	}
}
