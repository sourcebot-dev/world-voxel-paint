// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "VoxelPaintCanonicalSource.h"
#include "VoxelPaintVolumeData.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPCG_CommitDelegate,
	"Game.WorldVoxelPaint.Unit.PCG.CommitDelegate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPCG_CommitDelegate::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	UVoxelPaintVolumeData* Asset = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	if (!TestNotNull(TEXT("Asset created"), Asset)) { return false; }
	Asset->BrickCounts = FIntVector(1, 1, 1);

	FVoxelPaintLogicalState State;
	BuildFixtureState(EVoxelPaintFixturePreset::Checker, FIntVector(1, 1, 1), State);
	FVoxelPaintCanonicalBuild Build;
	BuildCanonicalSource(State, Build);

	int32 BroadcastCount = 0;
	UVoxelPaintVolumeData* LastPayload = nullptr;
	uint64 GenerationAtBroadcast = 0;
	const FDelegateHandle Handle = OnVolumeDataCommitted().AddLambda(
		[&BroadcastCount, &LastPayload, &GenerationAtBroadcast](UVoxelPaintVolumeData* Committed)
		{
			++BroadcastCount;
			LastPayload = Committed;
			GenerationAtBroadcast = Committed ? Committed->GetSourceGeneration() : 0;
		});
	ON_SCOPE_EXIT { OnVolumeDataCommitted().Remove(Handle); };

	// Leg 1 - a successful commit broadcasts exactly once, after the generation bump: the
	// subscriber observes the already-advanced source generation.
	const uint64 GenerationBefore = Asset->GetSourceGeneration();
	Asset->CommitCanonicalSource(Build);
	TestEqual(TEXT("Commit broadcasts exactly once"), BroadcastCount, 1);
	TestTrue(TEXT("Broadcast carries the committed asset"), LastPayload == Asset);
	TestTrue(TEXT("Subscriber observes the advanced source generation"),
		GenerationAtBroadcast == GenerationBefore + 1);
	TestTrue(TEXT("Commit initialized the asset"), Asset->bInitialized);

	// Leg 2 - reset routes through the same funnel and broadcasts too: a paint-clear must
	// refresh consumers, so handlers must never be "optimized" to ignore uninitialized assets.
	Asset->ResetCanonicalData();
	TestEqual(TEXT("Reset broadcasts through the shared funnel tail"), BroadcastCount, 2);
	TestTrue(TEXT("Reset payload is the same asset"), LastPayload == Asset);
	TestFalse(TEXT("Reset cleared initialization"), Asset->bInitialized);

	// Leg 3 - a validation failure fails closed WITHOUT broadcasting (the broadcast sits after
	// the early-return validation block by design).
	AddExpectedErrorPlain(
		TEXT("an uninitialized payload must carry no source, manifest, or identity"),
		EAutomationExpectedErrorFlags::Contains, 0);
	FVoxelPaintCanonicalPayload BadPayload;
	BadPayload.bInitialized = false;
	BadPayload.ManifestBricks.Add(0u);
	Asset->InstallCanonicalState(BadPayload);
	TestEqual(TEXT("Failed install does not broadcast"), BroadcastCount, 2);

	return true;
}
