// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelPaintAllocators.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPool_FreeListAllocator,
	"Game.WorldVoxelPaint.Unit.Pool.FreeListAllocator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPool_FreeListAllocator::RunTest(const FString& Parameters)
{
	FVoxelPaintBlockAllocator Allocator;
	Allocator.Initialize(8);

	TestTrue(TEXT("Fresh allocator capacity"), Allocator.GetMaxNumBlocks() == 8);
	TestTrue(TEXT("Fresh allocator all free"), Allocator.GetNumFree() == 8);
	TestTrue(TEXT("Fresh allocator none allocated"), Allocator.GetNumAllocated() == 0);

	TArray<int32> First;
	TestTrue(TEXT("Allocate 3 succeeds"), Allocator.Allocate(3, First));
	TestTrue(TEXT("Fresh pops ascend from 0"),
		First.Num() == 3 && First[0] == 0 && First[1] == 1 && First[2] == 2);
	TestTrue(TEXT("Free count after allocate"), Allocator.GetNumFree() == 5);

	TArray<int32> Untouched;
	Untouched.Add(42);
	TestFalse(TEXT("Over-request fails"), Allocator.Allocate(6, Untouched));
	TestTrue(TEXT("Failed allocate leaves free count untouched"), Allocator.GetNumFree() == 5);
	TestTrue(TEXT("Failed allocate leaves allocated count untouched"), Allocator.GetNumAllocated() == 3);
	TestTrue(TEXT("Failed allocate leaves the output array untouched"),
		Untouched.Num() == 1 && Untouched[0] == 42);

	TArray<int32> Second;
	TestTrue(TEXT("Allocate the remaining 5 succeeds"), Allocator.Allocate(5, Second));
	TestTrue(TEXT("Remaining pops continue ascending"),
		Second.Num() == 5 && Second[0] == 3 && Second[1] == 4 && Second[2] == 5 && Second[3] == 6 && Second[4] == 7);
	TestTrue(TEXT("Pool exhausted"), Allocator.GetNumFree() == 0);

	const int32 Returned[2] = { 1, 2 };
	Allocator.Free(MakeArrayView(Returned, 2));
	TestTrue(TEXT("Free returns blocks"), Allocator.GetNumFree() == 2);

	TArray<int32> Third;
	TestTrue(TEXT("Realloc after free succeeds"), Allocator.Allocate(2, Third));
	TestTrue(TEXT("Realloc pops the freed blocks deterministically (LIFO)"),
		Third.Num() == 2 && Third[0] == 2 && Third[1] == 1);
	TestTrue(TEXT("All blocks accounted for"), Allocator.GetNumAllocated() == 8);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPool_PageTableAllocator,
	"Game.WorldVoxelPaint.Unit.Pool.PageTableAllocator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPool_PageTableAllocator::RunTest(const FString& Parameters)
{
	FVoxelPaintPageTableAllocator Allocator;
	Allocator.Initialize(64);

	TestTrue(TEXT("Fresh capacity"), Allocator.GetCapacityEntries() == 64);
	TestTrue(TEXT("Fresh all free"), Allocator.GetFreeEntries() == 64);
	TestTrue(TEXT("Fresh largest free range spans everything"), Allocator.GetLargestFreeRange() == 64);

	const int32 RangeA = Allocator.Reserve(16);
	const int32 RangeB = Allocator.Reserve(16);
	const int32 RangeC = Allocator.Reserve(8);
	TestTrue(TEXT("First reservation at offset 0"), RangeA != INDEX_NONE && Allocator.GetRangeOffset(RangeA) == 0);
	TestTrue(TEXT("Second reservation packs at offset 16"), RangeB != INDEX_NONE && Allocator.GetRangeOffset(RangeB) == 16);
	TestTrue(TEXT("Third reservation packs at offset 32"), RangeC != INDEX_NONE && Allocator.GetRangeOffset(RangeC) == 32);
	TestTrue(TEXT("Range counts recorded"),
		Allocator.GetRangeCount(RangeA) == 16 && Allocator.GetRangeCount(RangeB) == 16 && Allocator.GetRangeCount(RangeC) == 8);
	TestTrue(TEXT("Reserved entries sum"), Allocator.GetEntries(EVoxelPaintRangeState::Reserved) == 40);
	TestTrue(TEXT("Free entries after reservations"), Allocator.GetFreeEntries() == 24);

	TestTrue(TEXT("Infeasible reservation returns INDEX_NONE"), Allocator.Reserve(32) == INDEX_NONE);
	TestTrue(TEXT("Failed reservation mutates nothing"), Allocator.GetFreeEntries() == 24);

	Allocator.SetState(RangeA, EVoxelPaintRangeState::Uploading);
	TestTrue(TEXT("Reserved to Uploading"),
		Allocator.GetRangeState(RangeA) == EVoxelPaintRangeState::Uploading
		&& Allocator.GetEntries(EVoxelPaintRangeState::Uploading) == 16);
	Allocator.SetState(RangeA, EVoxelPaintRangeState::Active);
	TestTrue(TEXT("Uploading to Active"),
		Allocator.GetRangeState(RangeA) == EVoxelPaintRangeState::Active
		&& Allocator.GetEntries(EVoxelPaintRangeState::Active) == 16);

	Allocator.Retire(RangeB, 5);
	TestTrue(TEXT("Retire from Reserved stamps Retiring"),
		Allocator.GetRangeState(RangeB) == EVoxelPaintRangeState::Retiring
		&& Allocator.GetEntries(EVoxelPaintRangeState::Retiring) == 16);

	Allocator.ReleaseRetired(4);
	TestTrue(TEXT("ReleaseRetired below the retire ordinal frees nothing"),
		Allocator.GetFreeEntries() == 24 && Allocator.GetEntries(EVoxelPaintRangeState::Retiring) == 16);
	Allocator.ReleaseRetired(5);
	TestTrue(TEXT("ReleaseRetired at the retire ordinal frees the range"),
		Allocator.GetFreeEntries() == 40 && Allocator.GetEntries(EVoxelPaintRangeState::Retiring) == 0);
	TestTrue(TEXT("Largest free range after release"), Allocator.GetLargestFreeRange() == 24);

	const int32 RangeD = Allocator.Reserve(10);
	TestTrue(TEXT("First-fit reuses the lowest freed offset"),
		RangeD != INDEX_NONE && Allocator.GetRangeOffset(RangeD) == 16);
	const int32 RangeE = Allocator.Reserve(8);
	TestTrue(TEXT("Range too big for the low gap lands at the next span"),
		RangeE != INDEX_NONE && Allocator.GetRangeOffset(RangeE) == 40);
	const int32 RangeF = Allocator.Reserve(4);
	TestTrue(TEXT("Small range back-fills the lowest remaining gap"),
		RangeF != INDEX_NONE && Allocator.GetRangeOffset(RangeF) == 26);

	TestTrue(TEXT("Free entries under fragmentation"), Allocator.GetFreeEntries() == 18);
	TestTrue(TEXT("Headroom is the largest free range"), Allocator.GetLargestFreeRange() == 16);
	TestTrue(TEXT("Waste is free minus headroom"),
		Allocator.GetFreeEntries() - Allocator.GetLargestFreeRange() == 2);

	Allocator.Retire(RangeD, 7);
	Allocator.Retire(RangeE, 7);
	Allocator.Retire(RangeF, 7);
	Allocator.ReleaseRetired(7);
	TestTrue(TEXT("Released spans coalesce"),
		Allocator.GetFreeEntries() == 40 && Allocator.GetLargestFreeRange() == 24);

	Allocator.Retire(RangeC, 9);
	Allocator.ReleaseRetired(8);
	TestTrue(TEXT("Later ordinal stays retired"),
		Allocator.GetEntries(EVoxelPaintRangeState::Retiring) == 8 && Allocator.GetFreeEntries() == 40);
	Allocator.ReleaseRetired(9);
	TestTrue(TEXT("Coalesce across the released middle range"),
		Allocator.GetFreeEntries() == 48 && Allocator.GetLargestFreeRange() == 48);

	Allocator.Retire(RangeA, 10);
	Allocator.ReleaseRetired(10);
	TestTrue(TEXT("Full release restores the whole span"),
		Allocator.GetFreeEntries() == 64 && Allocator.GetLargestFreeRange() == 64);

	return true;
}
