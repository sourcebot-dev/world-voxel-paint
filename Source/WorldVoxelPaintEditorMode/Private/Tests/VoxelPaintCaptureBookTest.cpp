// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "VoxelPaintCaptureBook.h"
#include "VoxelPaintVolumeData.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintCaptureBook_Transitions,
	"Game.WorldVoxelPaint.Unit.CaptureBook.Transitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintCaptureBook_Transitions::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	UVoxelPaintVolumeData* AssetA = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	UVoxelPaintVolumeData* AssetB = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	if (!TestNotNull(TEXT("Fabricated key objects created"), AssetA) || !AssetB)
	{
		return false;
	}
	const FVoxelPaintAssetKey KeyA(AssetA);
	const FVoxelPaintAssetKey KeyB(AssetB);

	FVoxelPaintSnapshotCaptureBook Book;
	TestFalse(TEXT("Fresh: nothing pending"), Book.HasPendingCaptures());
	TestFalse(TEXT("Fresh: nothing captured"), Book.IsCaptured(KeyA, 5));

	Book.BeginPack();
	Book.PackCaptured(KeyA, 5);
	Book.PackPending(KeyB);
	TestFalse(TEXT("Mid-pack: staged entries are not committed"), Book.IsCaptured(KeyA, 5));
	TestFalse(TEXT("Mid-pack: staged pending is not committed"), Book.HasPendingCaptures());
	Book.EndPack();

	TestTrue(TEXT("Pack: A captured at its packed generation"), Book.IsCaptured(KeyA, 5));
	TestFalse(TEXT("Pack: A not captured at another generation"), Book.IsCaptured(KeyA, 6));
	TestFalse(TEXT("Pack: pending B is not captured"), Book.IsCaptured(KeyB, 5));
	TestTrue(TEXT("Pack: a pending key arms the standing condition"), Book.HasPendingCaptures());

	Book.BeginPack();
	Book.PackCaptured(KeyA, 5);
	Book.PackCaptured(KeyB, 7);
	Book.EndPack();
	TestTrue(TEXT("Drain: B capture success clears pending"), !Book.HasPendingCaptures());
	TestTrue(TEXT("Drain: B now captured"), Book.IsCaptured(KeyB, 7));
	TestTrue(TEXT("Drain: A retained through the dedupe path"), Book.IsCaptured(KeyA, 5));

	Book.BeginPack();
	Book.PackPending(KeyB);
	Book.EndPack();
	TestFalse(TEXT("Prune: a key absent from the pack is dropped"), Book.IsCaptured(KeyA, 5));
	TestTrue(TEXT("Regression: a generation change can re-arm a captured key"),
		Book.HasPendingCaptures() && !Book.IsCaptured(KeyB, 7));

	Book.Remove(KeyB);
	TestFalse(TEXT("Remove: the key leaves the pending set"), Book.HasPendingCaptures());

	Book.BeginPack();
	Book.PackCaptured(KeyA, 9);
	Book.PackPending(KeyB);
	Book.Remove(KeyA);
	Book.Remove(KeyB);
	Book.EndPack();
	TestFalse(TEXT("Remove: a mid-pack removal drops the staged capture"), Book.IsCaptured(KeyA, 9));
	TestFalse(TEXT("Remove: a mid-pack removal drops the staged pending"), Book.HasPendingCaptures());

	Book.BeginPack();
	Book.PackCaptured(KeyA, 11);
	Book.PackPending(KeyB);
	Book.EndPack();
	Book.Empty();
	TestFalse(TEXT("Empty: nothing captured"), Book.IsCaptured(KeyA, 11));
	TestFalse(TEXT("Empty: nothing pending"), Book.HasPendingCaptures());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintVolumeData_PreparationSnapshotFinalNull,
	"Game.WorldVoxelPaint.Unit.VolumeData.PreparationSnapshotFinalNull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintVolumeData_PreparationSnapshotFinalNull::RunTest(const FString& Parameters)
{
	UVoxelPaintVolumeData* Asset = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	if (!TestNotNull(TEXT("Fabricated asset created"), Asset))
	{
		return false;
	}

	bool bCapturePending = true;
	TSharedPtr<FVoxelPaintPreparedSourceSnapshot> Snapshot =
		Asset->CreatePreparationSnapshot(&bCapturePending);
	TestFalse(TEXT("No committed source: snapshot is null"), Snapshot.IsValid());
	TestFalse(TEXT("No committed source: the null is FINAL, never capture-pending"),
		bCapturePending);
	TestFalse(TEXT("Null out-flag pointer is tolerated"),
		Asset->CreatePreparationSnapshot(nullptr).IsValid());

	return true;
}
