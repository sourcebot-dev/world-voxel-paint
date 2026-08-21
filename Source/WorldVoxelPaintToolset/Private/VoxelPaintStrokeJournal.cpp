// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintStrokeJournal.h"

#include "WorldVoxelPaintConstants.h"

namespace WorldVoxelPaint
{

int64 FVoxelPaintStrokeDelta::GetBytes() const
{
	int64 Bytes = sizeof(FVoxelPaintStrokeDelta);
	for (const FVoxelPaintBrickDelta& Brick : Bricks)
	{
		Bytes += sizeof(FVoxelPaintBrickDelta);
		Bytes += (int64)Brick.TouchedMask.Num() * sizeof(uint64);
		Bytes += ((int64)Brick.Before.Num() + Brick.After.Num()) * sizeof(uint32);
	}
	return Bytes;
}

void FVoxelPaintStrokeJournal::Initialize(int64 InCapacityBytes)
{
	Entries.Reset();
	UndoCursor = 0;
	CapacityBytes = FMath::Max<int64>(0, InCapacityBytes);
	RetainedBytes = 0;
	EvictPendingBytes = 0;
	NumEvictPending = 0;
	ActiveReservationId = INDEX_NONE;
	ActiveReservationBytes = 0;
	NextReservationId = 1;
}

bool FVoxelPaintStrokeJournal::Reserve(int64 ConservativeBytes, int32& OutReservationId)
{
	OutReservationId = INDEX_NONE;

	if (ConservativeBytes < 0 || ConservativeBytes > CapacityBytes)
	{
		return false;
	}
	if (ActiveReservationId != INDEX_NONE)
	{
		return false;
	}

	int32 NumMarked = 0;
	int64 MarkedBytes = 0;
	while (RetainedBytes - MarkedBytes + ConservativeBytes > CapacityBytes && NumMarked < Entries.Num())
	{
		MarkedBytes += Entries[NumMarked].Bytes;
		++NumMarked;
	}
	if (RetainedBytes - MarkedBytes + ConservativeBytes > CapacityBytes)
	{
		return false;
	}

	for (int32 Index = 0; Index < NumMarked; ++Index)
	{
		Entries[Index].bEvictPending = true;
	}
	NumEvictPending = NumMarked;
	EvictPendingBytes = MarkedBytes;

	ActiveReservationId = NextReservationId++;
	ActiveReservationBytes = ConservativeBytes;
	OutReservationId = ActiveReservationId;
	return true;
}

bool FVoxelPaintStrokeJournal::GrowReservation(int32 ReservationId, int64 AdditionalBytes)
{
	checkf(ReservationId != INDEX_NONE && ReservationId == ActiveReservationId,
		TEXT("Grow with mismatched journal reservation: %d (active %d)"), ReservationId, ActiveReservationId);

	if (AdditionalBytes < 0)
	{
		return false;
	}
	if (AdditionalBytes == 0)
	{
		return true;
	}
	const int64 NewReservationBytes = ActiveReservationBytes + AdditionalBytes;
	if (NewReservationBytes > CapacityBytes)
	{
		return false;
	}

	int32 NumMarked = NumEvictPending;
	int64 MarkedBytes = EvictPendingBytes;
	while (RetainedBytes - MarkedBytes + NewReservationBytes > CapacityBytes && NumMarked < Entries.Num())
	{
		MarkedBytes += Entries[NumMarked].Bytes;
		++NumMarked;
	}
	if (RetainedBytes - MarkedBytes + NewReservationBytes > CapacityBytes)
	{
		return false;
	}

	for (int32 Index = NumEvictPending; Index < NumMarked; ++Index)
	{
		Entries[Index].bEvictPending = true;
	}
	NumEvictPending = NumMarked;
	EvictPendingBytes = MarkedBytes;
	ActiveReservationBytes = NewReservationBytes;
	return true;
}

void FVoxelPaintStrokeJournal::Commit(int32 ReservationId, FVoxelPaintStrokeDelta&& Delta)
{
	checkf(ReservationId != INDEX_NONE && ReservationId == ActiveReservationId,
		TEXT("Commit with mismatched journal reservation: %d (active %d)"), ReservationId, ActiveReservationId);

	for (int32 Index = UndoCursor; Index < Entries.Num(); ++Index)
	{
		RetainedBytes -= Entries[Index].Bytes;
		if (Entries[Index].bEvictPending)
		{
			EvictPendingBytes -= Entries[Index].Bytes;
			--NumEvictPending;
		}
	}
	Entries.SetNum(UndoCursor);

	int32 NumEvicted = 0;
	while (NumEvicted < Entries.Num() && Entries[NumEvicted].bEvictPending)
	{
		RetainedBytes -= Entries[NumEvicted].Bytes;
		EvictPendingBytes -= Entries[NumEvicted].Bytes;
		--NumEvictPending;
		++NumEvicted;
	}
	if (NumEvicted > 0)
	{
		Entries.RemoveAt(0, NumEvicted);
		UndoCursor -= NumEvicted;
	}
	checkf(NumEvictPending == 0 && EvictPendingBytes == 0,
		TEXT("Evict-pending entries survived finalization: %d"), NumEvictPending);

	FJournalEntry Entry;
	Entry.Bytes = Delta.GetBytes();
	Entry.Delta = MoveTemp(Delta);
	RetainedBytes += Entry.Bytes;
	Entries.Add(MoveTemp(Entry));
	UndoCursor = Entries.Num();

	ActiveReservationId = INDEX_NONE;
	ActiveReservationBytes = 0;
}

void FVoxelPaintStrokeJournal::CancelReservation(int32 ReservationId)
{
	checkf(ReservationId != INDEX_NONE && ReservationId == ActiveReservationId,
		TEXT("Cancel with mismatched journal reservation: %d (active %d)"), ReservationId, ActiveReservationId);

	for (FJournalEntry& Entry : Entries)
	{
		Entry.bEvictPending = false;
	}
	EvictPendingBytes = 0;
	NumEvictPending = 0;
	ActiveReservationId = INDEX_NONE;
	ActiveReservationBytes = 0;
}

bool FVoxelPaintStrokeJournal::CanUndo() const
{
	return UndoCursor > 0;
}

bool FVoxelPaintStrokeJournal::CanRedo() const
{
	return UndoCursor < Entries.Num();
}

const FVoxelPaintStrokeDelta* FVoxelPaintStrokeJournal::PeekUndo() const
{
	return CanUndo() ? &Entries[UndoCursor - 1].Delta : nullptr;
}

const FVoxelPaintStrokeDelta* FVoxelPaintStrokeJournal::PeekRedo() const
{
	return CanRedo() ? &Entries[UndoCursor].Delta : nullptr;
}

void FVoxelPaintStrokeJournal::AcknowledgeUndo()
{
	checkf(CanUndo(), TEXT("AcknowledgeUndo with no undoable entry"));
	--UndoCursor;
}

void FVoxelPaintStrokeJournal::AcknowledgeRedo()
{
	checkf(CanRedo(), TEXT("AcknowledgeRedo with no redoable entry"));
	++UndoCursor;
}

int64 FVoxelPaintStrokeJournal::GetRetainedBytes() const
{
	return RetainedBytes;
}

int64 FVoxelPaintStrokeJournal::GetCapacityBytes() const
{
	return CapacityBytes;
}

int32 FVoxelPaintStrokeJournal::GetNumEntries() const
{
	return Entries.Num();
}

void FVoxelPaintBaselineAccumulator::RecordFirstTouch(uint32 BrickIndex, TConstArrayView<uint32> PackedBefore343)
{
	checkf(PackedBefore343.Num() == TexelsPerUniqueBrick,
		TEXT("Baseline page must carry %d texels, got %d"), TexelsPerUniqueBrick, PackedBefore343.Num());

	if (Baseline.Contains(BrickIndex))
	{
		return;
	}
	Baseline.Emplace(BrickIndex, TArray<uint32>(PackedBefore343.GetData(), PackedBefore343.Num()));
}

bool FVoxelPaintBaselineAccumulator::HasBrick(uint32 BrickIndex) const
{
	return Baseline.Contains(BrickIndex);
}

const TMap<uint32, TArray<uint32>>& FVoxelPaintBaselineAccumulator::GetBaseline() const
{
	return Baseline;
}

int64 FVoxelPaintBaselineAccumulator::GetBytes() const
{
	return (int64)Baseline.Num() * ((int64)TexelsPerUniqueBrick * sizeof(uint32) + sizeof(uint32));
}

}
