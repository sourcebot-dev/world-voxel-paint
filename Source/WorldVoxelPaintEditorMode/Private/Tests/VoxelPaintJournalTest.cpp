// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelPaintSessionCpuState.h"
#include "VoxelPaintStrokeJournal.h"
#include "WorldVoxelPaintConstants.h"

namespace VoxelPaintJournalTestPrivate
{
	using namespace WorldVoxelPaint;

	static constexpr int32 MaskWords = (TexelsPerUniqueBrick + 63) / 64;

	static FVoxelPaintBrickDelta MakeBrickDelta(uint32 BrickIndex, TConstArrayView<int32> InBrickTexels,
		TConstArrayView<uint32> Before, TConstArrayView<uint32> After)
	{
		FVoxelPaintBrickDelta Brick;
		Brick.BrickIndex = BrickIndex;
		Brick.TouchedMask.SetNumZeroed(MaskWords);
		for (int32 InBrick : InBrickTexels)
		{
			Brick.TouchedMask[InBrick / 64] |= (1ull << (InBrick % 64));
		}
		Brick.Before = TArray<uint32>(Before.GetData(), Before.Num());
		Brick.After = TArray<uint32>(After.GetData(), After.Num());
		return Brick;
	}

	static FVoxelPaintStrokeDelta MakeStrokeDelta(uint32 BrickIndex, uint32 Seed)
	{
		const int32 Texels[] = { 0, 5, 100, 342 };
		TArray<uint32> Before;
		TArray<uint32> After;
		for (int32 Index = 0; Index < (int32)UE_ARRAY_COUNT(Texels); ++Index)
		{
			Before.Add(0u);
			After.Add(Seed + (uint32)Index);
		}
		FVoxelPaintStrokeDelta Delta;
		Delta.Bricks.Add(MakeBrickDelta(BrickIndex, Texels, Before, After));
		return Delta;
	}

	static uint32 PeekedBrickIndex(const FVoxelPaintStrokeDelta* Delta)
	{
		return (Delta && Delta->Bricks.Num() > 0) ? Delta->Bricks[0].BrickIndex : MAX_uint32;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintSession_Journal,
	"Game.WorldVoxelPaint.Unit.Session.Journal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintSession_Journal::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;
	using namespace VoxelPaintJournalTestPrivate;

	const int64 DeltaBytes = MakeStrokeDelta(0, 0x10u).GetBytes();
	const int64 CapacityBytes = DeltaBytes * 2 + DeltaBytes / 2;

	FVoxelPaintStrokeJournal Journal;
	Journal.Initialize(CapacityBytes);
	TestTrue(TEXT("Fresh journal is empty"),
		Journal.GetNumEntries() == 0 && Journal.GetRetainedBytes() == 0 && !Journal.CanUndo() && !Journal.CanRedo());
	TestTrue(TEXT("Capacity recorded"), Journal.GetCapacityBytes() == CapacityBytes);

	int32 ReservationId = INDEX_NONE;
	TestFalse(TEXT("Reserve beyond capacity rejects the op unchanged"),
		Journal.Reserve(CapacityBytes + 1, ReservationId));
	TestTrue(TEXT("Rejected reserve hands out no id"), ReservationId == INDEX_NONE);
	TestTrue(TEXT("Rejected reserve leaves the journal untouched"),
		Journal.GetNumEntries() == 0 && Journal.GetRetainedBytes() == 0);

	TestTrue(TEXT("Reserve A fits"), Journal.Reserve(DeltaBytes, ReservationId));
	Journal.Commit(ReservationId, MakeStrokeDelta(0, 0x10u));
	TestTrue(TEXT("Reserve B fits"), Journal.Reserve(DeltaBytes, ReservationId));
	Journal.Commit(ReservationId, MakeStrokeDelta(1, 0x20u));
	TestTrue(TEXT("Two entries retained"),
		Journal.GetNumEntries() == 2 && Journal.GetRetainedBytes() == 2 * DeltaBytes);
	TestTrue(TEXT("Undo available, no redo tail"), Journal.CanUndo() && !Journal.CanRedo());

	{
		TestTrue(TEXT("Reserve C marks the oldest entry evict-pending"), Journal.Reserve(DeltaBytes, ReservationId));
		Journal.CancelReservation(ReservationId);
		TestTrue(TEXT("Cancel restores history exactly: entries and bytes unchanged"),
			Journal.GetNumEntries() == 2 && Journal.GetRetainedBytes() == 2 * DeltaBytes);
		TestTrue(TEXT("Cancel restores history exactly: newest entry still peekable"),
			PeekedBrickIndex(Journal.PeekUndo()) == 1u);
	}

	{
		TestTrue(TEXT("Reserve C again"), Journal.Reserve(DeltaBytes, ReservationId));
		Journal.Commit(ReservationId, MakeStrokeDelta(2, 0x30u));
		TestTrue(TEXT("Commit finalizes the pending eviction: oldest entry gone"),
			Journal.GetNumEntries() == 2 && Journal.GetRetainedBytes() == 2 * DeltaBytes);
		TestTrue(TEXT("Newest entry is C"), PeekedBrickIndex(Journal.PeekUndo()) == 2u);
	}

	{
		TestTrue(TEXT("Reserve for per-stamp growth"), Journal.Reserve(DeltaBytes, ReservationId));
		TestTrue(TEXT("Growth within capacity succeeds"),
			Journal.GrowReservation(ReservationId, DeltaBytes / 2));
		TestFalse(TEXT("Growth beyond capacity rejects unchanged"),
			Journal.GrowReservation(ReservationId, CapacityBytes));
		Journal.CancelReservation(ReservationId);
		TestTrue(TEXT("Cancel after growth restores history exactly"),
			Journal.GetNumEntries() == 2 && Journal.GetRetainedBytes() == 2 * DeltaBytes
			&& PeekedBrickIndex(Journal.PeekUndo()) == 2u);
	}

	{
		Journal.AcknowledgeUndo();
		TestTrue(TEXT("Undo exposes the redo tail"), Journal.CanRedo() && Journal.CanUndo());
		TestTrue(TEXT("Redo peeks the undone stroke"), PeekedBrickIndex(Journal.PeekRedo()) == 2u);
		TestTrue(TEXT("Undo peeks the prior stroke"), PeekedBrickIndex(Journal.PeekUndo()) == 1u);

		Journal.AcknowledgeRedo();
		TestTrue(TEXT("Redo walks forward again"),
			!Journal.CanRedo() && PeekedBrickIndex(Journal.PeekUndo()) == 2u);
		Journal.AcknowledgeUndo();

		TestTrue(TEXT("Reserve D over an undone tail"), Journal.Reserve(DeltaBytes, ReservationId));
		Journal.Commit(ReservationId, MakeStrokeDelta(3, 0x40u));
		TestFalse(TEXT("New Commit clears the redo tail"), Journal.CanRedo());
		TestTrue(TEXT("New Commit is the newest entry"), PeekedBrickIndex(Journal.PeekUndo()) == 3u);
		TestTrue(TEXT("Tail drop and eviction both settled"),
			Journal.GetNumEntries() == 1 && Journal.GetRetainedBytes() == DeltaBytes);
	}

	{
		FVoxelPaintBaselineAccumulator Accumulator;
		TArray<uint32> FirstPage;
		FirstPage.Init(0xAABBCCDDu, TexelsPerUniqueBrick);
		TArray<uint32> SecondPage;
		SecondPage.Init(0x11223344u, TexelsPerUniqueBrick);

		Accumulator.RecordFirstTouch(5, FirstPage);
		TestTrue(TEXT("Accumulator holds the touched brick"), Accumulator.HasBrick(5));
		TestFalse(TEXT("Accumulator does not invent bricks"), Accumulator.HasBrick(6));

		Accumulator.RecordFirstTouch(5, SecondPage);
		const TArray<uint32>* Stored = Accumulator.GetBaseline().Find(5);
		TestTrue(TEXT("Second touch is ignored: the FIRST original page is retained"),
			Stored && Stored->Num() == TexelsPerUniqueBrick && (*Stored)[0] == 0xAABBCCDDu);
		TestTrue(TEXT("Accumulator reports its bytes"), Accumulator.GetBytes() > 0);
	}

	{
		FVoxelPaintSessionCpuState State;
		State.Initialize(FIntVector(2, 1, 1));
		FVoxelPaintSessionCpuState Original;
		Original.Initialize(FIntVector(2, 1, 1));

		const int32 TexelsBrick0[] = { 0, 5, 342 };
		const uint32 Before0[] = { 0u, 0u, 0u };
		const uint32 After0[] = {
			PackTexelBGRA(FColor(10, 20, 30, 255)),
			PackTexelBGRA(FColor(200, 100, 50, 128)),
			PackTexelBGRA(FColor(1, 2, 3, 4)) };
		const int32 TexelsBrick1[] = { 10 };
		const uint32 Before1[] = { 0u };
		const uint32 After1[] = { PackTexelBGRA(FColor(255, 255, 255, 255)) };

		FVoxelPaintStrokeDelta Delta;
		Delta.Bricks.Add(MakeBrickDelta(0, TexelsBrick0, Before0, After0));
		Delta.Bricks.Add(MakeBrickDelta(1, TexelsBrick1, Before1, After1));

		State.ApplyDelta(Delta, true);
		TestFalse(TEXT("Redo application changes the dense state"), State.EqualsDense(Original));
		TestTrue(TEXT("Redo writes the exact packed texel (in-brick 5 of brick 0)"),
			State.GetTexel(FIntVector(5, 0, 0)) == FColor(200, 100, 50, 128));
		TestTrue(TEXT("Redo writes the exact packed texel (in-brick 342 of brick 0)"),
			State.GetTexel(FIntVector(6, 6, 6)) == FColor(1, 2, 3, 4));
		TestTrue(TEXT("Redo writes the exact packed texel (in-brick 10 of brick 1)"),
			State.GetTexel(FIntVector(7 + 3, 1, 0)) == FColor(255, 255, 255, 255));

		State.ApplyDelta(Delta, false);
		TestTrue(TEXT("Undo application restores the dense state losslessly"), State.EqualsDense(Original));
	}

	return true;
}
