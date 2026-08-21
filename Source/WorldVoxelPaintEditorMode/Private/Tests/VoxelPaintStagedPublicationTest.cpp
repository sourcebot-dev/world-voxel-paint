// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "VoxelPaintPublicationAssembler.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintResidency.h"
#include "VoxelPaintSessionRenderTypes.h"
#include "WorldVoxelPaintConstants.h"

namespace VoxelPaintStagedPublicationTestPrivate
{
	using namespace WorldVoxelPaint;

	static FVoxelPaintCanonicalIdentity MakeIdentity(uint8 Seed)
	{
		FVoxelPaintCanonicalIdentity Identity;
		FMemory::Memset(Identity.Hash.Hash, Seed, sizeof(Identity.Hash.Hash));
		return Identity;
	}

	static FVoxelPaintDesiredAsset MakeDesired(const FVoxelPaintAssetKey& Key, uint8 IdentitySeed,
		const FIntVector& BrickCounts, const FString& DebugName)
	{
		TSharedPtr<FVoxelPaintSourceManifest> Manifest = MakeShared<FVoxelPaintSourceManifest>();
		Manifest->BrickCounts = BrickCounts;
		const int32 Total = BrickCounts.X * BrickCounts.Y * BrickCounts.Z;
		for (int32 Index = 0; Index < Total; ++Index)
		{
			Manifest->PopulatedBricks.Add((uint32)Index);
		}
		FVoxelPaintDesiredAsset Desired;
		Desired.Key = Key;
		Desired.SourceGeneration = 1;
		Desired.Identity = MakeIdentity(IdentitySeed);
		Desired.Manifest = Manifest;
		Desired.DebugName = DebugName;
		return Desired;
	}

	static void PrepareAndComplete(FVoxelPaintResidencyManager& Manager, const FVoxelPaintDesiredAsset& Desired)
	{
		Manager.OnPreparationLaunched(Desired.Key, Desired.SourceGeneration, 1);
		FVoxelPaintPreparedPayload Payload;
		Payload.AssetKey = Desired.Key;
		Payload.SourceGeneration = Desired.SourceGeneration;
		Payload.LaunchOwnerEpoch = 1;
		Payload.Identity = Desired.Identity;
		Payload.Manifest = Desired.Manifest;
		TSharedPtr<TArray<uint8>> Bytes = MakeShared<TArray<uint8>>();
		Bytes->SetNumZeroed(Desired.Manifest->PopulatedBricks.Num() * BrickPayloadBytes);
		Payload.BrickPayload = Bytes;
		Payload.bPayloadValid = true;
		Manager.OnPreparationCompleted(MoveTemp(Payload));
	}

	static FVoxelPaintPlacementState MakePlacement(const FVoxelPaintDesiredAsset& Desired)
	{
		FVoxelPaintPlacementState State;
		State.Flags = RecordFlag_Valid;
		State.AssetKey = Desired.Key;
		State.SourceGeneration = Desired.SourceGeneration;
		State.BrickCounts = Desired.Manifest->BrickCounts;
		return State;
	}

	static FVoxelPaintSessionRenderBinding MakeBinding(const FVoxelPaintAssetKey& Key,
		EVoxelPaintSessionBindingPhase Phase, const FVoxelPaintCanonicalIdentity& CommittedIdentity)
	{
		FVoxelPaintSessionRenderBinding Binding;
		Binding.AssetKey = Key;
		Binding.SessionGeneration = 1;
		Binding.Phase = Phase;
		Binding.CommittedIdentity = CommittedIdentity;
		Binding.Resolution = FIntVector(28, 7, 7);
		return Binding;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintSession_StagedPublication,
	"Game.WorldVoxelPaint.Unit.Session.StagedPublication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintSession_StagedPublication::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;
	using namespace VoxelPaintStagedPublicationTestPrivate;

	const FVoxelPaintAssetKey KeyA(UObject::StaticClass());
	const FVoxelPaintCanonicalIdentity IdCommitted = MakeIdentity(1);
	const FVoxelPaintCanonicalIdentity IdOther = MakeIdentity(2);

	{
		const FVoxelPaintSessionRenderBinding NoBinding;
		TestTrue(TEXT("Phase None resolves UseResidency"),
			ResolveSessionStagedBinding(NoBinding, true, EVoxelPaintBindingKind::Data, IdCommitted)
				== EVoxelPaintStagedOutcome::UseResidency);

		const FVoxelPaintSessionRenderBinding PreCommit =
			MakeBinding(KeyA, EVoxelPaintSessionBindingPhase::PreCommit, FVoxelPaintCanonicalIdentity());
		TestTrue(TEXT("Pre-commit + origin master resolves the session texture"),
			ResolveSessionStagedBinding(PreCommit, true, EVoxelPaintBindingKind::NeutralProxy, IdCommitted)
				== EVoxelPaintStagedOutcome::UseSessionTexture);
		TestTrue(TEXT("Pre-commit + origin master ignores residency Data (session state wins)"),
			ResolveSessionStagedBinding(PreCommit, true, EVoxelPaintBindingKind::Data, IdCommitted)
				== EVoxelPaintStagedOutcome::UseSessionTexture);
		TestTrue(TEXT("Pre-commit + non-origin master forces the neutral proxy"),
			ResolveSessionStagedBinding(PreCommit, false, EVoxelPaintBindingKind::NeutralProxy, IdCommitted)
				== EVoxelPaintStagedOutcome::ForceNeutralProxy);
		TestTrue(TEXT("Pre-commit + non-origin: baseline BC7 NEVER binds even when residency says Data"),
			ResolveSessionStagedBinding(PreCommit, false, EVoxelPaintBindingKind::Data, IdCommitted)
				== EVoxelPaintStagedOutcome::ForceNeutralProxy);

		const FVoxelPaintSessionRenderBinding PostCommit =
			MakeBinding(KeyA, EVoxelPaintSessionBindingPhase::PostCommit, IdCommitted);
		TestTrue(TEXT("Post-commit + origin holds the session texture while the desired identity is stale"),
			ResolveSessionStagedBinding(PostCommit, true, EVoxelPaintBindingKind::Data, IdOther)
				== EVoxelPaintStagedOutcome::UseSessionTexture);
		TestTrue(TEXT("Post-commit + origin holds the session texture until the matching data is COMPLETE"),
			ResolveSessionStagedBinding(PostCommit, true, EVoxelPaintBindingKind::NeutralProxy, IdCommitted)
				== EVoxelPaintStagedOutcome::UseSessionTexture);
		TestTrue(TEXT("Post-commit + origin flips to residency on matching complete Data"),
			ResolveSessionStagedBinding(PostCommit, true, EVoxelPaintBindingKind::Data, IdCommitted)
				== EVoxelPaintStagedOutcome::UseResidency);
		TestTrue(TEXT("Post-commit + origin: a matching all-zero commit (NoTable) counts complete"),
			ResolveSessionStagedBinding(PostCommit, true, EVoxelPaintBindingKind::NoTable, IdCommitted)
				== EVoxelPaintStagedOutcome::UseResidency);
		TestTrue(TEXT("Post-commit + non-origin resolves residency only for matching complete Data"),
			ResolveSessionStagedBinding(PostCommit, false, EVoxelPaintBindingKind::Data, IdCommitted)
				== EVoxelPaintStagedOutcome::UseResidency);
		TestTrue(TEXT("Post-commit + non-origin forces neutral while the identity is stale (never stale BC7)"),
			ResolveSessionStagedBinding(PostCommit, false, EVoxelPaintBindingKind::Data, IdOther)
				== EVoxelPaintStagedOutcome::ForceNeutralProxy);
		TestTrue(TEXT("Post-commit + non-origin forces neutral while the data is incomplete"),
			ResolveSessionStagedBinding(PostCommit, false, EVoxelPaintBindingKind::NeutralProxy, IdCommitted)
				== EVoxelPaintStagedOutcome::ForceNeutralProxy);
	}

	const FVoxelPaintAssetKey KeyB(UClass::StaticClass());
	const FVoxelPaintDesiredAsset AssetA = MakeDesired(KeyA, 1, FIntVector(4, 1, 1), TEXT("/WVP/Fixtures/A"));
	const FVoxelPaintDesiredAsset AssetB = MakeDesired(KeyB, 2, FIntVector(5, 1, 1), TEXT("/WVP/Fixtures/B"));

	FVoxelPaintResidencyManager Manager;
	Manager.Initialize(16, 64);

	TArray<FVoxelPaintDesiredAsset> Desired;
	Desired.Add(AssetA);
	Desired.Add(AssetB);
	Manager.SetDesiredAssets(1, Desired);
	PrepareAndComplete(Manager, AssetA);
	PrepareAndComplete(Manager, AssetB);

	FVoxelPaintIntegrationBudget Budget;
	Budget.MaxUploadBytes = int64(1) << 40;
	Budget.MaxPageEntries = MAX_int32;
	FVoxelPaintIntegrationOps Ops;
	bool bBindingsChanged = false;
	Manager.IntegrateStep(1, 1, Budget, Ops, bBindingsChanged);
	TestTrue(TEXT("Both fixtures published"),
		Manager.GetAssetState(KeyA) == EVoxelPaintAssetResidencyState::Published
			&& Manager.GetAssetState(KeyB) == EVoxelPaintAssetResidencyState::Published);

	TArray<FVoxelPaintPlacementState> Placements;
	Placements.Add(MakePlacement(AssetA));
	Placements.Add(MakePlacement(AssetB));

	TArray<FVoxelPaintVolumeRecord> BaselineRecords;
	FVoxelPaintAssemblyStats BaselineStats;
	AssembleRecords(Placements, Manager, FVector::ZeroVector, BaselineRecords, BaselineStats, Desired);
	TestTrue(TEXT("M2 baseline: both placements resolve Data"),
		BaselineStats.NumData == 2 && BaselineStats.NumSessionSelector == 0);
	TestTrue(TEXT("M2 baseline: selectors stay zero"),
		BaselineRecords[0].SessionSelector == 0u && BaselineRecords[1].SessionSelector == 0u);

	{
		TArray<FVoxelPaintVolumeRecord> Records;
		FVoxelPaintAssemblyStats Stats;
		bool bSelectorPublished = true;
		AssembleRecords(Placements, Manager, FVector::ZeroVector, Records, Stats, Desired,
			nullptr, false, bSelectorPublished);
		TestTrue(TEXT("Null binding: records byte-identical to the M2 overload"),
			Records.Num() == BaselineRecords.Num()
				&& FMemory::Memcmp(Records.GetData(), BaselineRecords.GetData(),
					Records.Num() * sizeof(FVoxelPaintVolumeRecord)) == 0);
		TestTrue(TEXT("Null binding: stats identical to the M2 overload"),
			Stats.NumData == BaselineStats.NumData && Stats.NumNoTable == BaselineStats.NumNoTable
				&& Stats.NumNeutralProxy == BaselineStats.NumNeutralProxy
				&& Stats.NumInvalid == BaselineStats.NumInvalid
				&& Stats.NumSessionSelector == 0);
		TestFalse(TEXT("Null binding publishes no selector"), bSelectorPublished);
	}

	{
		const FVoxelPaintSessionRenderBinding Inactive;
		TArray<FVoxelPaintVolumeRecord> Records;
		FVoxelPaintAssemblyStats Stats;
		bool bSelectorPublished = true;
		AssembleRecords(Placements, Manager, FVector::ZeroVector, Records, Stats, Desired,
			&Inactive, true, bSelectorPublished);
		TestTrue(TEXT("Inactive binding behaves exactly like a null binding"),
			FMemory::Memcmp(Records.GetData(), BaselineRecords.GetData(),
				Records.Num() * sizeof(FVoxelPaintVolumeRecord)) == 0
				&& Stats.NumSessionSelector == 0 && !bSelectorPublished);
	}

	{
		const FVoxelPaintSessionRenderBinding Binding =
			MakeBinding(KeyA, EVoxelPaintSessionBindingPhase::PreCommit, FVoxelPaintCanonicalIdentity());
		TArray<FVoxelPaintVolumeRecord> Records;
		FVoxelPaintAssemblyStats Stats;
		bool bSelectorPublished = false;
		AssembleRecords(Placements, Manager, FVector::ZeroVector, Records, Stats, Desired,
			&Binding, true, bSelectorPublished);
		TestTrue(TEXT("Pre-commit origin: the active asset's record packs SessionSelectorActive"),
			Records[0].SessionSelector == SessionSelectorActive);
		TestTrue(TEXT("Pre-commit origin: the selector record packs the no-table sentinel"),
			Records[0].PageTableBaseIndex == NoPageTableSentinel);
		TestTrue(TEXT("Pre-commit origin: the other asset keeps its M2 Data binding"),
			Records[1].SessionSelector == 0u
				&& Records[1].PageTableBaseIndex == BaselineRecords[1].PageTableBaseIndex);
		TestTrue(TEXT("Pre-commit origin: stats count one selector and one Data"),
			Stats.NumSessionSelector == 1 && Stats.NumData == 1 && Stats.NumNeutralProxy == 0);
		TestTrue(TEXT("Pre-commit origin: selector publication reported"), bSelectorPublished);
	}

	{
		const FVoxelPaintSessionRenderBinding Binding =
			MakeBinding(KeyA, EVoxelPaintSessionBindingPhase::PreCommit, FVoxelPaintCanonicalIdentity());
		TArray<FVoxelPaintVolumeRecord> Records;
		FVoxelPaintAssemblyStats Stats;
		bool bSelectorPublished = false;
		AssembleRecords(Placements, Manager, FVector::ZeroVector, Records, Stats, Desired,
			&Binding, false, bSelectorPublished);
		TestTrue(TEXT("Pre-commit non-origin: forced neutral proxy despite published baseline BC7"),
			Records[0].SessionSelector == 0u && Records[0].PageTableBaseIndex == NoPageTableSentinel);
		TestTrue(TEXT("Pre-commit non-origin: stats count the forced neutral proxy"),
			Stats.NumSessionSelector == 0 && Stats.NumNeutralProxy == 1 && Stats.NumData == 1);
		TestFalse(TEXT("Pre-commit non-origin publishes no selector"), bSelectorPublished);
	}

	{
		const FVoxelPaintSessionRenderBinding Binding =
			MakeBinding(KeyA, EVoxelPaintSessionBindingPhase::PostCommit, AssetA.Identity);
		TArray<FVoxelPaintVolumeRecord> Records;
		FVoxelPaintAssemblyStats Stats;
		bool bSelectorPublished = true;
		AssembleRecords(Placements, Manager, FVector::ZeroVector, Records, Stats, Desired,
			&Binding, true, bSelectorPublished);
		TestTrue(TEXT("Post-commit flip: matching complete Data resolves residency for the origin master"),
			Records[0].SessionSelector == 0u
				&& Records[0].PageTableBaseIndex == BaselineRecords[0].PageTableBaseIndex);
		TestTrue(TEXT("Post-commit flip: no selector remains in the pack"),
			Stats.NumSessionSelector == 0 && Stats.NumData == 2 && !bSelectorPublished);
	}

	{
		const FVoxelPaintSessionRenderBinding Binding =
			MakeBinding(KeyA, EVoxelPaintSessionBindingPhase::PostCommit, MakeIdentity(9));
		TArray<FVoxelPaintVolumeRecord> Records;
		FVoxelPaintAssemblyStats Stats;
		bool bSelectorPublished = false;
		AssembleRecords(Placements, Manager, FVector::ZeroVector, Records, Stats, Desired,
			&Binding, true, bSelectorPublished);
		TestTrue(TEXT("Post-commit origin: stale published identity keeps the session texture bound"),
			Records[0].SessionSelector == SessionSelectorActive
				&& Records[0].PageTableBaseIndex == NoPageTableSentinel && bSelectorPublished);

		AssembleRecords(Placements, Manager, FVector::ZeroVector, Records, Stats, Desired,
			&Binding, false, bSelectorPublished);
		TestTrue(TEXT("Post-commit non-origin: stale published identity forces the neutral proxy"),
			Records[0].SessionSelector == 0u && Records[0].PageTableBaseIndex == NoPageTableSentinel
				&& Stats.NumNeutralProxy == 1 && !bSelectorPublished);
	}

	return true;
}
