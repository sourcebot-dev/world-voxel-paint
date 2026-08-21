// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace WorldVoxelPaint
{
	struct FVoxelPaintBrickDelta
	{
		uint32 BrickIndex = 0;
		TArray<uint64> TouchedMask;
		TArray<uint32> Before;
		TArray<uint32> After;
	};

	struct WORLDVOXELPAINTTOOLSET_API FVoxelPaintStrokeDelta
	{
		TArray<FVoxelPaintBrickDelta> Bricks;
		int64 GetBytes() const;
	};

	class WORLDVOXELPAINTTOOLSET_API FVoxelPaintStrokeJournal
	{
	public:
		void Initialize(int64 InCapacityBytes);
		bool Reserve(int64 ConservativeBytes, int32& OutReservationId);
		bool GrowReservation(int32 ReservationId, int64 AdditionalBytes);
		void Commit(int32 ReservationId, FVoxelPaintStrokeDelta&& Delta);
		void CancelReservation(int32 ReservationId);
		bool CanUndo() const;
		bool CanRedo() const;
		const FVoxelPaintStrokeDelta* PeekUndo() const;
		const FVoxelPaintStrokeDelta* PeekRedo() const;
		void AcknowledgeUndo();
		void AcknowledgeRedo();
		int64 GetRetainedBytes() const;
		int64 GetCapacityBytes() const;
		int32 GetNumEntries() const;

	private:
		struct FJournalEntry
		{
			FVoxelPaintStrokeDelta Delta;
			int64 Bytes = 0;
			bool bEvictPending = false;
		};

		TArray<FJournalEntry> Entries;
		int32 UndoCursor = 0;
		int64 CapacityBytes = 0;
		int64 RetainedBytes = 0;
		int64 EvictPendingBytes = 0;
		int32 NumEvictPending = 0;
		int32 ActiveReservationId = INDEX_NONE;
		int64 ActiveReservationBytes = 0;
		int32 NextReservationId = 1;
	};

	class WORLDVOXELPAINTTOOLSET_API FVoxelPaintBaselineAccumulator
	{
	public:
		void RecordFirstTouch(uint32 BrickIndex, TConstArrayView<uint32> PackedBefore343);
		bool HasBrick(uint32 BrickIndex) const;
		const TMap<uint32, TArray<uint32>>& GetBaseline() const;
		int64 GetBytes() const;

	private:
		TMap<uint32, TArray<uint32>> Baseline;
	};
}
