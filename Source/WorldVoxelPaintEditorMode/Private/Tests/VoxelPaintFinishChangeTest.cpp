// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "VoxelPaintCanonicalSource.h"
#include "VoxelPaintFinishChange.h"
#include "VoxelPaintVolumeData.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintSession_FinishChangeSize,
	"Game.WorldVoxelPaint.Unit.Session.FinishChangeSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintSession_FinishChangeSize::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	const FIntVector BrickCounts(2, 2, 2);
	const FVector LocalHalfExtent(500.0);
	const FVector StorageOrigin(0.0, 0.0, 0.0);

	UVoxelPaintVolumeData* Asset = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	if (!TestNotNull(TEXT("Transient asset created"), Asset))
	{
		return false;
	}
	Asset->LocalHalfExtent = LocalHalfExtent;
	Asset->BrickCounts = BrickCounts;
	Asset->StorageOrigin = StorageOrigin;
	TestFalse(TEXT("Asset starts uninitialized"), Asset->bInitialized);

	FVoxelPaintLogicalState State;
	BuildFixtureState(EVoxelPaintFixturePreset::Checker, BrickCounts, State);
	FVoxelPaintCanonicalBuild Build;
	BuildCanonicalSource(State, Build);
	const FVoxelPaintCanonicalIdentity ExpectedIdentity =
		ComputeCanonicalIdentity(Build, StorageOrigin, LocalHalfExtent);
	const TArray<uint32> ExpectedManifest = Build.Manifest.PopulatedBricks;
	TestTrue(TEXT("Fixture produced a populated manifest"), ExpectedManifest.Num() > 0);

	FVoxelPaintCanonicalPayload Baseline;
	Baseline.BrickCounts = BrickCounts;
	Baseline.LocalHalfExtent = LocalHalfExtent;
	Baseline.StorageOrigin = StorageOrigin;
	Baseline.bInitialized = false;

	FVoxelPaintCanonicalPayload After;
	After.PackedSourceBGRA8 = Build.PackedSourceBGRA8;
	After.ManifestBricks = Build.Manifest.PopulatedBricks;
	After.IdentityBytes.SetNumUninitialized(sizeof(ExpectedIdentity.Hash.Hash));
	FMemory::Memcpy(After.IdentityBytes.GetData(), ExpectedIdentity.Hash.Hash, sizeof(ExpectedIdentity.Hash.Hash));
	After.BrickCounts = BrickCounts;
	After.LocalHalfExtent = LocalHalfExtent;
	After.StorageOrigin = StorageOrigin;
	After.bInitialized = true;

	const int64 BaselineBytes = Baseline.GetBytes();
	const int64 AfterBytes = After.GetBytes();
	const int64 PackedSourceBytes = After.PackedSourceBGRA8.Num();
	TestTrue(TEXT("After payload carries real packed source bytes"), PackedSourceBytes > 0);

	FVoxelPaintFinishChange Change(MoveTemp(Baseline), MoveTemp(After), Asset->GetName());

	TestTrue(TEXT("GetSize is non-zero (the Change.h default-zero trap is closed)"), Change.GetSize() != 0);
	TestTrue(TEXT("GetSize covers at least both owned payloads"),
		Change.GetSize() >= (SIZE_T)(BaselineBytes + AfterBytes));
	TestTrue(TEXT("GetSize covers the flat packed source buffer"), Change.GetSize() >= (SIZE_T)PackedSourceBytes);
	TestTrue(TEXT("ToString names the asset"), Change.ToString().Contains(Asset->GetName()));
	TestTrue(TEXT("The change owns the complete after state"),
		Change.GetAfterState().bInitialized
			&& Change.GetAfterState().PackedSourceBGRA8.Num() == PackedSourceBytes);
	TestFalse(TEXT("The change owns the uninitialized baseline"), Change.GetBaselineState().bInitialized);

	const uint64 GenerationBeforeApply = Asset->GetSourceGeneration();
	Change.Apply(Asset);
	TestTrue(TEXT("Apply installs the initialized after state"), Asset->bInitialized);
	TestTrue(TEXT("Apply installs the committed identity"), Asset->GetCanonicalIdentity() == ExpectedIdentity);
	TestTrue(TEXT("Apply installs the manifest"),
		Asset->GetManifestSnapshot().IsValid()
			&& Asset->GetManifestSnapshot()->PopulatedBricks == ExpectedManifest);
	TestTrue(TEXT("Apply installs the frozen topology"),
		Asset->GetBrickCounts() == BrickCounts && Asset->LocalHalfExtent.Equals(LocalHalfExtent, 0.0));
	TestTrue(TEXT("Apply bumps the source generation"), Asset->GetSourceGeneration() > GenerationBeforeApply);

	const uint64 GenerationBeforeRevert = Asset->GetSourceGeneration();
	Change.Revert(Asset);
	TestFalse(TEXT("First-Finish Revert restores the uninitialized state"), Asset->bInitialized);
	TestFalse(TEXT("Revert clears the canonical identity"), Asset->GetCanonicalIdentity().IsValid());
	TestTrue(TEXT("Revert clears the manifest"),
		Asset->GetManifestSnapshot().IsValid() && Asset->GetManifestSnapshot()->PopulatedBricks.Num() == 0);
	TestTrue(TEXT("Revert keeps the pre-session topology"), Asset->GetBrickCounts() == BrickCounts);
	TestTrue(TEXT("Revert bumps the source generation"), Asset->GetSourceGeneration() > GenerationBeforeRevert);

	Change.Apply(Asset);
	TestTrue(TEXT("Round-trip Apply reinstalls the complete after state"),
		Asset->bInitialized && Asset->GetCanonicalIdentity() == ExpectedIdentity
			&& Asset->GetManifestSnapshot()->PopulatedBricks == ExpectedManifest);

	{
		FVoxelPaintCanonicalPayload Captured;
		Asset->CaptureCanonicalPayload(Captured);
		TestTrue(TEXT("Captured payload round-trips the installed state"),
			Captured.bInitialized && Captured.ManifestBricks == ExpectedManifest
				&& Captured.PackedSourceBGRA8.Num() == PackedSourceBytes
				&& Captured.BrickCounts == BrickCounts);
	}

	GetTransientPackage()->SetDirtyFlag(false);
	return true;
}
