// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelPaintGridReadiness.h"

namespace VoxelPaintGridReadinessTestPrivate
{
	using namespace WorldVoxelPaint;

	static FVoxelPaintGridReadinessToken MakeToken(uint64 Epoch, uint64 Layout)
	{
		FVoxelPaintGridReadinessToken Token;
		Token.OwnerEpoch = Epoch;
		Token.LayoutGeneration = Layout;
		return Token;
	}

	static FVoxelPaintGridReadbackResult MakeResult(const FVoxelPaintGridReadinessToken& Token,
		bool bExplicitSuccess, bool bMalformed = false)
	{
		FVoxelPaintGridReadbackResult Result;
		Result.Token = Token;
		Result.bExplicitSuccess = bExplicitSuccess;
		Result.bMalformed = bMalformed;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintGrid_ReadinessTokens,
	"Game.WorldVoxelPaint.Unit.Grid.ReadinessTokens",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintGrid_ReadinessTokens::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;
	using namespace VoxelPaintGridReadinessTestPrivate;

	FVoxelPaintGridReadinessTracker Tracker;
	Tracker.Initialize(2);
	TestTrue(TEXT("Fresh tracker is Pending with no pending slots"),
		Tracker.GetReadiness() == EVoxelPaintGridReadiness::Pending && Tracker.GetNumPendingSlots() == 0);
	TestFalse(TEXT("Fresh tracker wants no recovery"), Tracker.WantsRecoveryRebuild());

	const FVoxelPaintGridReadinessToken T11 = MakeToken(1, 1);
	Tracker.OnExpectedGridChanged(T11);
	{
		const int32 Slot = Tracker.AcquireSlot(T11);
		TestTrue(TEXT("First readback acquires a slot"), Slot != INDEX_NONE);
		TestTrue(TEXT("One slot pending"), Tracker.GetNumPendingSlots() == 1);
		Tracker.OnReadbackResolved(Slot, MakeResult(T11, true));
		TestTrue(TEXT("Matching explicit success resolves Ready"),
			Tracker.GetReadiness() == EVoxelPaintGridReadiness::Ready);
		TestTrue(TEXT("Resolved slot freed"), Tracker.GetNumPendingSlots() == 0);
	}

	Tracker.OnExpectedGridChanged(T11);
	TestTrue(TEXT("Re-notifying the SAME token does not reset readiness"),
		Tracker.GetReadiness() == EVoxelPaintGridReadiness::Ready);

	const FVoxelPaintGridReadinessToken T12 = MakeToken(1, 2);
	Tracker.OnExpectedGridChanged(T12);
	TestTrue(TEXT("Layout-generation advance resets readiness to Pending"),
		Tracker.GetReadiness() == EVoxelPaintGridReadiness::Pending);
	{
		const int32 Slot = Tracker.AcquireSlot(T12);
		Tracker.OnReadbackResolved(Slot, MakeResult(T12, true));
		TestTrue(TEXT("New layout generation resolves Ready on its own token"),
			Tracker.GetReadiness() == EVoxelPaintGridReadiness::Ready);
	}

	const FVoxelPaintGridReadinessToken T21 = MakeToken(2, 1);
	Tracker.OnExpectedGridChanged(T21);
	TestTrue(TEXT("Epoch advance resets readiness to Pending"),
		Tracker.GetReadiness() == EVoxelPaintGridReadiness::Pending);

	{
		const int32 Slot = Tracker.AcquireSlot(T21);
		const FVoxelPaintGridReadinessToken T22 = MakeToken(2, 2);
		Tracker.OnExpectedGridChanged(T22);
		Tracker.OnReadbackResolved(Slot, MakeResult(T21, true));
		TestTrue(TEXT("Stale-generation success is discarded: readiness untouched"),
			Tracker.GetReadiness() == EVoxelPaintGridReadiness::Pending);
		TestTrue(TEXT("Stale result frees its slot"), Tracker.GetNumPendingSlots() == 0);

		const int32 SupersededSlot = Tracker.AcquireSlot(T22);
		const FVoxelPaintGridReadinessToken T31 = MakeToken(3, 1);
		Tracker.OnExpectedGridChanged(T31);
		Tracker.OnReadbackResolved(SupersededSlot, MakeResult(T22, true));
		TestTrue(TEXT("Superseded-epoch success is discarded: readiness untouched"),
			Tracker.GetReadiness() == EVoxelPaintGridReadiness::Pending);
		TestTrue(TEXT("Superseded result frees its slot"), Tracker.GetNumPendingSlots() == 0);
	}

	{
		const FVoxelPaintGridReadinessToken Current = MakeToken(3, 1);
		const int32 Slot = Tracker.AcquireSlot(Current);
		Tracker.OnReadbackResolved(Slot, MakeResult(Current, false));
		TestTrue(TEXT("Matching explicit failure resolves Failed"),
			Tracker.GetReadiness() == EVoxelPaintGridReadiness::Failed);

		const FVoxelPaintGridReadinessToken Next = MakeToken(3, 2);
		Tracker.OnExpectedGridChanged(Next);
		const int32 MalformedSlot = Tracker.AcquireSlot(Next);
		Tracker.OnReadbackResolved(MalformedSlot, MakeResult(Next, true, true));
		TestTrue(TEXT("Matching malformed readback resolves Failed"),
			Tracker.GetReadiness() == EVoxelPaintGridReadiness::Failed);
	}

	{
		const FVoxelPaintGridReadinessToken Old = MakeToken(4, 1);
		Tracker.OnExpectedGridChanged(Old);
		const int32 SlotA = Tracker.AcquireSlot(Old);
		const int32 SlotB = Tracker.AcquireSlot(Old);
		TestTrue(TEXT("Slots are never reused while pending: distinct slots handed out"),
			SlotA != INDEX_NONE && SlotB != INDEX_NONE && SlotA != SlotB);
		TestTrue(TEXT("Both slots pending"), Tracker.GetNumPendingSlots() == 2);

		const FVoxelPaintGridReadinessToken Current = MakeToken(4, 2);
		Tracker.OnExpectedGridChanged(Current);
		TestTrue(TEXT("Expected-grid change leaves in-flight slots pending"), Tracker.GetNumPendingSlots() == 2);

		TestTrue(TEXT("Saturated acquire reports INDEX_NONE"), Tracker.AcquireSlot(Current) == INDEX_NONE);
		TestTrue(TEXT("Saturation is observation-only: readiness stays Pending"),
			Tracker.GetReadiness() == EVoxelPaintGridReadiness::Pending);
		TestFalse(TEXT("Saturation alone requests no recovery"), Tracker.WantsRecoveryRebuild());

		Tracker.OnReadbackResolved(SlotA, MakeResult(Old, true));
		TestTrue(TEXT("Old result over an unobserved current generation is discarded"),
			Tracker.GetReadiness() == EVoxelPaintGridReadiness::Pending);
		TestTrue(TEXT("A freed slot over an unobserved current generation requests recovery"),
			Tracker.WantsRecoveryRebuild());
		Tracker.AcknowledgeRecoveryRequested();
		TestFalse(TEXT("Acknowledged recovery request clears the want"), Tracker.WantsRecoveryRebuild());

		Tracker.OnReadbackResolved(SlotB, MakeResult(Old, false));
		TestFalse(TEXT("Recovery is requested exactly once per unobserved generation"),
			Tracker.WantsRecoveryRebuild());

		const int32 RecoverySlot = Tracker.AcquireSlot(Current);
		TestTrue(TEXT("Recovery rebuild acquires a freed slot"), RecoverySlot != INDEX_NONE);
		Tracker.OnReadbackResolved(RecoverySlot, MakeResult(Current, true));
		TestTrue(TEXT("Recovery readback resolves Ready"),
			Tracker.GetReadiness() == EVoxelPaintGridReadiness::Ready);
		TestFalse(TEXT("Ready generation wants no recovery"), Tracker.WantsRecoveryRebuild());
	}

	{
		Tracker.OnReadbackResolved(INDEX_NONE, MakeResult(MakeToken(4, 2), true));
		Tracker.OnReadbackResolved(0, MakeResult(MakeToken(4, 2), false));
		TestTrue(TEXT("Invalid or already-freed slot resolutions are ignored"),
			Tracker.GetReadiness() == EVoxelPaintGridReadiness::Ready);
	}

	return true;
}
