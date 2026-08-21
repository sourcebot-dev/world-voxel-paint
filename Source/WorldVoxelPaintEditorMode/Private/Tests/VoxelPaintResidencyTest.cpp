// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintResidency.h"
#include "WorldVoxelPaintConstants.h"

namespace VoxelPaintResidencyTestPrivate
{
	static FVoxelPaintCanonicalIdentity MakeIdentity(uint8 Seed)
	{
		FVoxelPaintCanonicalIdentity Identity;
		FMemory::Memset(Identity.Hash.Hash, Seed, sizeof(Identity.Hash.Hash));
		return Identity;
	}

	static TSharedPtr<const FVoxelPaintSourceManifest> MakeFullManifest(const FIntVector& BrickCounts)
	{
		TSharedPtr<FVoxelPaintSourceManifest> Manifest = MakeShared<FVoxelPaintSourceManifest>();
		Manifest->BrickCounts = BrickCounts;
		const int32 Total = BrickCounts.X * BrickCounts.Y * BrickCounts.Z;
		for (int32 Index = 0; Index < Total; ++Index)
		{
			Manifest->PopulatedBricks.Add((uint32)Index);
		}
		return Manifest;
	}

	static TSharedPtr<const FVoxelPaintSourceManifest> MakeSparseManifest(const FIntVector& BrickCounts,
		TConstArrayView<uint32> Populated)
	{
		TSharedPtr<FVoxelPaintSourceManifest> Manifest = MakeShared<FVoxelPaintSourceManifest>();
		Manifest->BrickCounts = BrickCounts;
		Manifest->PopulatedBricks.Append(Populated.GetData(), Populated.Num());
		return Manifest;
	}

	static TSharedPtr<const FVoxelPaintSourceManifest> MakeEmptyManifest(const FIntVector& BrickCounts)
	{
		TSharedPtr<FVoxelPaintSourceManifest> Manifest = MakeShared<FVoxelPaintSourceManifest>();
		Manifest->BrickCounts = BrickCounts;
		return Manifest;
	}

	static FVoxelPaintDesiredAsset MakeDesired(const FVoxelPaintAssetKey& Key, uint64 SourceGeneration,
		const FVoxelPaintCanonicalIdentity& Identity, TSharedPtr<const FVoxelPaintSourceManifest> Manifest,
		const FString& DebugName)
	{
		FVoxelPaintDesiredAsset Desired;
		Desired.Key = Key;
		Desired.SourceGeneration = SourceGeneration;
		Desired.Identity = Identity;
		Desired.Manifest = Manifest;
		Desired.DebugName = DebugName;
		return Desired;
	}

	static FVoxelPaintPreparedPayload MakePayload(const FVoxelPaintDesiredAsset& Desired, uint64 LaunchOwnerEpoch)
	{
		FVoxelPaintPreparedPayload Payload;
		Payload.AssetKey = Desired.Key;
		Payload.SourceGeneration = Desired.SourceGeneration;
		Payload.LaunchOwnerEpoch = LaunchOwnerEpoch;
		Payload.Identity = Desired.Identity;
		Payload.Manifest = Desired.Manifest;
		TSharedPtr<TArray<uint8>> Bytes = MakeShared<TArray<uint8>>();
		Bytes->SetNumZeroed(Desired.Manifest->PopulatedBricks.Num() * WorldVoxelPaint::BrickPayloadBytes);
		Payload.BrickPayload = Bytes;
		Payload.bPayloadValid = true;
		return Payload;
	}

	static FVoxelPaintIntegrationBudget AmpleBudget()
	{
		FVoxelPaintIntegrationBudget Budget;
		Budget.MaxUploadBytes = int64(1) << 40;
		Budget.MaxPageEntries = MAX_int32;
		return Budget;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPool_WholeAssetClaim,
	"Game.WorldVoxelPaint.Unit.Pool.WholeAssetClaim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPool_WholeAssetClaim::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintResidencyTestPrivate;

	const FVoxelPaintAssetKey KeyA(UObject::StaticClass());
	const FVoxelPaintAssetKey KeyB(UClass::StaticClass());
	const FVoxelPaintDesiredAsset AssetA = MakeDesired(KeyA, 1, MakeIdentity(1), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Fixtures/A"));
	const FVoxelPaintDesiredAsset AssetB = MakeDesired(KeyB, 1, MakeIdentity(2), MakeFullManifest(FIntVector(5, 1, 1)), TEXT("/WVP/Fixtures/B"));

	FVoxelPaintResidencyManager Manager;
	Manager.Initialize(8, 64);

	TArray<FVoxelPaintDesiredAsset> Desired;
	Desired.Add(AssetA);
	Manager.SetDesiredAssets(1, Desired);

	uint64 PrepGeneration = 0;
	TestTrue(TEXT("Fresh desired asset needs preparation"), Manager.NeedsPreparation(KeyA, PrepGeneration));
	TestTrue(TEXT("Preparation names the desired generation"), PrepGeneration == 1u);
	Manager.OnPreparationLaunched(KeyA, 1, 1);
	TestTrue(TEXT("Launched asset is Preparing"), Manager.GetAssetState(KeyA) == EVoxelPaintAssetResidencyState::Preparing);
	Manager.OnPreparationCompleted(MakePayload(AssetA, 1));
	TestTrue(TEXT("Completed payload joins the claim queue"), Manager.GetAssetState(KeyA) == EVoxelPaintAssetResidencyState::Blocked);
	TestTrue(TEXT("Queue depth after completion"), Manager.GetRetryQueueDepth() == 1);

	FVoxelPaintIntegrationOps Ops;
	bool bBindingsChanged = false;
	Manager.IntegrateStep(1, 1, AmpleBudget(), Ops, bBindingsChanged);

	TestTrue(TEXT("Feasible claim publishes in one ample-budget step"), Manager.GetAssetState(KeyA) == EVoxelPaintAssetResidencyState::Published);
	TestTrue(TEXT("Publication is a binding change"), bBindingsChanged);
	TestTrue(TEXT("One brick upload per manifest brick"), Ops.BrickUploads.Num() == 4);
	TestTrue(TEXT("One page write per table entry"), Ops.PageWrites.Num() == 4);
	TestTrue(TEXT("Range activated this step"), Ops.bAnyRangeActivated);
	for (int32 Index = 0; Index < Ops.BrickUploads.Num(); ++Index)
	{
		TestTrue(TEXT("Fresh-pool brick uploads land in ascending slots"),
			Ops.BrickUploads[Index].PoolSlot == Index && Ops.BrickUploads[Index].PayloadBrickIndex == Index);
	}
	for (int32 Index = 0; Index < Ops.PageWrites.Num(); ++Index)
	{
		TestTrue(TEXT("Full-manifest page writes are slot+1 at ascending entries"),
			Ops.PageWrites[Index].EntryIndex == (uint32)Index && Ops.PageWrites[Index].Value == (uint32)Index + 1);
	}

	uint32 BaseIndex = 0;
	TestTrue(TEXT("Published asset resolves Data"),
		Manager.ResolveBinding(KeyA, 1, AssetA.Identity, BaseIndex) == EVoxelPaintBindingKind::Data);
	TestTrue(TEXT("First claim's page range starts at 0"), BaseIndex == 0u);
	TestTrue(TEXT("Published bricks are pinned"), Manager.GetPinnedBrickCount() == 4 && Manager.GetAllocatedBrickCount() == 4);

	TArray<FVoxelPaintDesiredAsset> NoneDesired;
	Manager.SetDesiredAssets(1, NoneDesired);
	TestTrue(TEXT("Undesired asset demotes to warm cache"),
		Manager.GetWarmCacheBrickCount() == 4 && Manager.GetPinnedBrickCount() == 0);
	TestTrue(TEXT("Warm bricks stay allocated"), Manager.GetAllocatedBrickCount() == 4);

	Manager.SetDesiredAssets(1, Desired);
	TestFalse(TEXT("Matching warm cache needs no re-preparation"), Manager.NeedsPreparation(KeyA, PrepGeneration));
	TestTrue(TEXT("Adoption claimant queues"), Manager.GetAssetState(KeyA) == EVoxelPaintAssetResidencyState::Blocked);
	Manager.IntegrateStep(1, 2, AmpleBudget(), Ops, bBindingsChanged);
	TestTrue(TEXT("Warm cache adopted back to Published"), Manager.GetAssetState(KeyA) == EVoxelPaintAssetResidencyState::Published);
	TestTrue(TEXT("Adoption emits no brick uploads"), Ops.BrickUploads.Num() == 0);
	TestTrue(TEXT("Adoption emits no page writes"), Ops.PageWrites.Num() == 0);
	TestTrue(TEXT("Adoption allocates nothing new"), Manager.GetAllocatedBrickCount() == 4);
	BaseIndex = MAX_uint32;
	TestTrue(TEXT("Adopted binding is Data again"),
		Manager.ResolveBinding(KeyA, 1, AssetA.Identity, BaseIndex) == EVoxelPaintBindingKind::Data);
	TestTrue(TEXT("Adopted binding keeps the original page range"), BaseIndex == 0u);

	Desired.Add(AssetB);
	Manager.SetDesiredAssets(1, Desired);
	TestTrue(TEXT("Second asset needs preparation"), Manager.NeedsPreparation(KeyB, PrepGeneration));
	Manager.OnPreparationLaunched(KeyB, 1, 1);
	Manager.OnPreparationCompleted(MakePayload(AssetB, 1));
	Manager.IntegrateStep(1, 3, AmpleBudget(), Ops, bBindingsChanged);

	TestTrue(TEXT("One-slot-short claim stays Blocked"), Manager.GetAssetState(KeyB) == EVoxelPaintAssetResidencyState::Blocked);
	TestFalse(TEXT("Failed claim is not a binding change"), bBindingsChanged);
	TestTrue(TEXT("Failed claim reserves no bricks"), Manager.GetAllocatedBrickCount() == 4);
	TestTrue(TEXT("Failed claim reserves no page range"), Manager.GetPageEntries(EVoxelPaintRangeState::Reserved) == 0);
	TestTrue(TEXT("Failed claim emits no ops"), Ops.BrickUploads.Num() == 0 && Ops.PageWrites.Num() == 0);
	TestTrue(TEXT("Blocked asset waits in the queue"),
		Manager.GetRetryQueueDepth() == 1 && Manager.GetRetryQueueHeadDebugName() == TEXT("/WVP/Fixtures/B"));
	BaseIndex = 0;
	TestTrue(TEXT("Blocked placements resolve NeutralProxy"),
		Manager.ResolveBinding(KeyB, 1, AssetB.Identity, BaseIndex) == EVoxelPaintBindingKind::NeutralProxy);
	TestTrue(TEXT("NeutralProxy carries the sentinel"), BaseIndex == WorldVoxelPaint::NoPageTableSentinel);
	TestTrue(TEXT("Pinned published asset was not evicted"), Manager.GetAssetState(KeyA) == EVoxelPaintAssetResidencyState::Published);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPool_LruVictimSelection,
	"Game.WorldVoxelPaint.Unit.Pool.LruVictimSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPool_LruVictimSelection::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintResidencyTestPrivate;

	const FVoxelPaintAssetKey KeyA(UObject::StaticClass());
	const FVoxelPaintAssetKey KeyB(UClass::StaticClass());
	const FVoxelPaintAssetKey KeyC(UPackage::StaticClass());
	const FVoxelPaintIntegrationBudget Budget = AmpleBudget();
	FVoxelPaintIntegrationOps Ops;
	bool bBindingsChanged = false;

	auto Admit = [&](FVoxelPaintResidencyManager& Manager, const FVoxelPaintDesiredAsset& Asset, uint64 Epoch)
	{
		uint64 PrepGeneration = 0;
		if (Manager.NeedsPreparation(Asset.Key, PrepGeneration))
		{
			Manager.OnPreparationLaunched(Asset.Key, PrepGeneration, Epoch);
			Manager.OnPreparationCompleted(MakePayload(Asset, Epoch));
		}
	};

	{
		const FVoxelPaintDesiredAsset AssetA = MakeDesired(KeyA, 1, MakeIdentity(1), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Lru/A"));
		const FVoxelPaintDesiredAsset AssetB = MakeDesired(KeyB, 1, MakeIdentity(2), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Lru/B"));
		const FVoxelPaintDesiredAsset AssetC = MakeDesired(KeyC, 1, MakeIdentity(3), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Lru/C"));

		FVoxelPaintResidencyManager Manager;
		Manager.Initialize(8, 64);

		TArray<FVoxelPaintDesiredAsset> Desired;
		Desired.Add(AssetA);
		Desired.Add(AssetB);
		Manager.SetDesiredAssets(1, Desired);
		Admit(Manager, AssetA, 1);
		Admit(Manager, AssetB, 1);
		Manager.IntegrateStep(1, 1, Budget, Ops, bBindingsChanged);
		TestTrue(TEXT("Both assets published"),
			Manager.GetAssetState(KeyA) == EVoxelPaintAssetResidencyState::Published
			&& Manager.GetAssetState(KeyB) == EVoxelPaintAssetResidencyState::Published);

		Desired.Reset();
		Desired.Add(AssetB);
		Manager.SetDesiredAssets(1, Desired);
		Manager.IntegrateStep(1, 2, Budget, Ops, bBindingsChanged);
		Desired.Reset();
		Manager.SetDesiredAssets(1, Desired);
		TestTrue(TEXT("Both demoted to warm cache"), Manager.GetWarmCacheBrickCount() == 8);

		Desired.Add(AssetC);
		Manager.SetDesiredAssets(1, Desired);
		Admit(Manager, AssetC, 1);
		Manager.IntegrateStep(1, 3, Budget, Ops, bBindingsChanged);
		TestTrue(TEXT("Claimant published after one eviction"), Manager.GetAssetState(KeyC) == EVoxelPaintAssetResidencyState::Published);
		TestTrue(TEXT("Only the oldest-stamped asset was evicted"), Manager.GetWarmCacheBrickCount() == 4);
		uint32 BaseIndex = MAX_uint32;
		TestTrue(TEXT("Claimant binds Data"), Manager.ResolveBinding(KeyC, 1, AssetC.Identity, BaseIndex) == EVoxelPaintBindingKind::Data);
		TestTrue(TEXT("Claimant reuses the evicted asset's released range"), BaseIndex == 0u);

		Desired.Add(AssetB);
		Manager.SetDesiredAssets(1, Desired);
		Manager.IntegrateStep(1, 4, Budget, Ops, bBindingsChanged);
		TestTrue(TEXT("Younger-stamped warm asset survived and adopts"), Manager.GetAssetState(KeyB) == EVoxelPaintAssetResidencyState::Published);
		TestTrue(TEXT("Survivor adoption uploads nothing"), Ops.BrickUploads.Num() == 0);
		BaseIndex = MAX_uint32;
		Manager.ResolveBinding(KeyB, 1, AssetB.Identity, BaseIndex);
		TestTrue(TEXT("Survivor kept its original range"), BaseIndex == 4u);
	}

	{
		const FVoxelPaintDesiredAsset AssetA1 = MakeDesired(KeyA, 1, MakeIdentity(1), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Lru/A"));
		const FVoxelPaintDesiredAsset AssetA2 = MakeDesired(KeyA, 2, MakeIdentity(9), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Lru/A"));
		const FVoxelPaintDesiredAsset AssetB = MakeDesired(KeyB, 1, MakeIdentity(2), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Lru/B"));
		const FVoxelPaintDesiredAsset AssetC = MakeDesired(KeyC, 1, MakeIdentity(3), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Lru/C"));

		FVoxelPaintResidencyManager Manager;
		Manager.Initialize(8, 64);

		TArray<FVoxelPaintDesiredAsset> Desired;
		Desired.Add(AssetA1);
		Desired.Add(AssetB);
		Manager.SetDesiredAssets(1, Desired);
		Admit(Manager, AssetA1, 1);
		Admit(Manager, AssetB, 1);
		Manager.IntegrateStep(1, 1, Budget, Ops, bBindingsChanged);

		Desired.Reset();
		Desired.Add(AssetA1);
		Manager.SetDesiredAssets(1, Desired);
		Manager.IntegrateStep(1, 2, Budget, Ops, bBindingsChanged);

		Desired.Reset();
		Desired.Add(AssetA2);
		Desired.Add(AssetC);
		Manager.SetDesiredAssets(1, Desired);
		TestTrue(TEXT("Supersession demotes the published generation"), Manager.GetWarmCacheBrickCount() == 8);
		Admit(Manager, AssetC, 1);
		Manager.IntegrateStep(1, 3, Budget, Ops, bBindingsChanged);
		TestTrue(TEXT("Claimant published"), Manager.GetAssetState(KeyC) == EVoxelPaintAssetResidencyState::Published);
		TestTrue(TEXT("One victim evicted"), Manager.GetWarmCacheBrickCount() == 4);

		Desired.Reset();
		Desired.Add(MakeDesired(KeyA, 1, MakeIdentity(1), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Lru/A")));
		Desired.Add(AssetC);
		Manager.SetDesiredAssets(1, Desired);
		Manager.IntegrateStep(1, 4, Budget, Ops, bBindingsChanged);
		TestTrue(TEXT("Supersession-refreshed stamp outlived the earlier demotion: old generation adopts"),
			Manager.GetAssetState(KeyA) == EVoxelPaintAssetResidencyState::Published);
		TestTrue(TEXT("Refreshed-stamp adoption uploads nothing"), Ops.BrickUploads.Num() == 0);
	}

	{
		const FVoxelPaintDesiredAsset AssetAlpha = MakeDesired(KeyA, 1, MakeIdentity(1), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Fixtures/Alpha"));
		const FVoxelPaintDesiredAsset AssetBeta = MakeDesired(KeyB, 1, MakeIdentity(2), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Fixtures/Beta"));
		const FVoxelPaintDesiredAsset AssetGamma = MakeDesired(KeyC, 1, MakeIdentity(3), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Fixtures/Gamma"));

		FVoxelPaintResidencyManager Manager;
		Manager.Initialize(8, 64);

		TArray<FVoxelPaintDesiredAsset> Desired;
		Desired.Add(AssetAlpha);
		Desired.Add(AssetBeta);
		Manager.SetDesiredAssets(1, Desired);
		Admit(Manager, AssetAlpha, 1);
		Admit(Manager, AssetBeta, 1);
		Manager.IntegrateStep(1, 1, Budget, Ops, bBindingsChanged);

		Desired.Reset();
		Manager.SetDesiredAssets(1, Desired);

		Desired.Add(AssetGamma);
		Manager.SetDesiredAssets(1, Desired);
		Admit(Manager, AssetGamma, 1);
		Manager.IntegrateStep(1, 2, Budget, Ops, bBindingsChanged);
		TestTrue(TEXT("Claimant published under a stamp tie"), Manager.GetAssetState(KeyC) == EVoxelPaintAssetResidencyState::Published);
		TestTrue(TEXT("Exactly one tie victim evicted"), Manager.GetWarmCacheBrickCount() == 4);

		Desired.Add(AssetBeta);
		Manager.SetDesiredAssets(1, Desired);
		Manager.IntegrateStep(1, 3, Budget, Ops, bBindingsChanged);
		TestTrue(TEXT("Tie broke by ascending DebugName: Beta survived Alpha"),
			Manager.GetAssetState(KeyB) == EVoxelPaintAssetResidencyState::Published);
		TestTrue(TEXT("Tie survivor adoption uploads nothing"), Ops.BrickUploads.Num() == 0);
	}

	{
		const FVoxelPaintDesiredAsset AssetA = MakeDesired(KeyA, 1, MakeIdentity(1), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Lru/A"));
		const FVoxelPaintDesiredAsset AssetB = MakeDesired(KeyB, 1, MakeIdentity(2), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Lru/B"));

		FVoxelPaintResidencyManager Manager;
		Manager.Initialize(4, 64);

		TArray<FVoxelPaintDesiredAsset> Desired;
		Desired.Add(AssetA);
		Manager.SetDesiredAssets(1, Desired);
		Admit(Manager, AssetA, 1);
		Manager.IntegrateStep(1, 1, Budget, Ops, bBindingsChanged);
		TestTrue(TEXT("Pool filled by the first asset"), Manager.GetAssetState(KeyA) == EVoxelPaintAssetResidencyState::Published);

		Desired.Reset();
		Desired.Add(AssetB);
		Manager.SetDesiredAssets(1, Desired);
		Admit(Manager, AssetB, 1);
		Manager.IntegrateStep(1, 1, Budget, Ops, bBindingsChanged);
		TestTrue(TEXT("A demotion stamped at the current ordinal is not yet a victim"),
			Manager.GetAssetState(KeyB) == EVoxelPaintAssetResidencyState::Blocked);
		TestTrue(TEXT("Deferred victim's bricks stay allocated"),
			Manager.GetWarmCacheBrickCount() == 4 && Manager.GetAllocatedBrickCount() == 4);

		Manager.IntegrateStep(1, 2, Budget, Ops, bBindingsChanged);
		TestTrue(TEXT("The next ordinal makes the retired stamp reusable"),
			Manager.GetAssetState(KeyB) == EVoxelPaintAssetResidencyState::Published);
		TestTrue(TEXT("Deferred eviction completed"), Manager.GetWarmCacheBrickCount() == 0 && Manager.GetAllocatedBrickCount() == 4);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPool_RetryFifo,
	"Game.WorldVoxelPaint.Unit.Pool.RetryFifo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPool_RetryFifo::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintResidencyTestPrivate;

	const FVoxelPaintAssetKey KeyA(UObject::StaticClass());
	const FVoxelPaintAssetKey KeyB(UClass::StaticClass());
	const FVoxelPaintAssetKey KeyC(UPackage::StaticClass());
	const FVoxelPaintIntegrationBudget Budget = AmpleBudget();
	FVoxelPaintIntegrationOps Ops;
	bool bBindingsChanged = false;
	uint64 PrepGeneration = 0;

	{
		const FVoxelPaintDesiredAsset AssetA = MakeDesired(KeyA, 1, MakeIdentity(1), MakeFullManifest(FIntVector(6, 1, 1)), TEXT("/WVP/Queue/A"));
		const FVoxelPaintDesiredAsset AssetB = MakeDesired(KeyB, 1, MakeIdentity(2), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Queue/B"));
		const FVoxelPaintDesiredAsset AssetC = MakeDesired(KeyC, 1, MakeIdentity(3), MakeFullManifest(FIntVector(2, 1, 1)), TEXT("/WVP/Queue/C"));

		FVoxelPaintResidencyManager Manager;
		Manager.Initialize(8, 64);

		TArray<FVoxelPaintDesiredAsset> Desired;
		Desired.Add(AssetA);
		Manager.SetDesiredAssets(1, Desired);
		Manager.NeedsPreparation(KeyA, PrepGeneration);
		Manager.OnPreparationLaunched(KeyA, 1, 1);
		Manager.OnPreparationCompleted(MakePayload(AssetA, 1));
		Manager.IntegrateStep(1, 1, Budget, Ops, bBindingsChanged);
		TestTrue(TEXT("First asset pins 6 of 8 slots"), Manager.GetAssetState(KeyA) == EVoxelPaintAssetResidencyState::Published);

		Desired.Add(AssetB);
		Desired.Add(AssetC);
		Manager.SetDesiredAssets(1, Desired);
		Manager.OnPreparationLaunched(KeyB, 1, 1);
		Manager.OnPreparationCompleted(MakePayload(AssetB, 1));
		Manager.OnPreparationLaunched(KeyC, 1, 1);
		Manager.OnPreparationCompleted(MakePayload(AssetC, 1));
		TestTrue(TEXT("Claimants queue in completion order"),
			Manager.GetRetryQueueDepth() == 2 && Manager.GetRetryQueueHeadDebugName() == TEXT("/WVP/Queue/B"));
		TestTrue(TEXT("One claimant blocked behind the head"), Manager.GetRetryBlockedBehindHeadCount() == 1);

		Manager.IntegrateStep(1, 2, Budget, Ops, bBindingsChanged);
		TestTrue(TEXT("Infeasible head blocks the line"), Manager.GetAssetState(KeyB) == EVoxelPaintAssetResidencyState::Blocked);
		TestTrue(TEXT("Feasible later arrival does not overtake"), Manager.GetAssetState(KeyC) == EVoxelPaintAssetResidencyState::Blocked);
		TestFalse(TEXT("Blocked step changes no bindings"), bBindingsChanged);
		TestTrue(TEXT("Blocked step allocates nothing"), Manager.GetAllocatedBrickCount() == 6);
		TestTrue(TEXT("Blocked step emits nothing"), Ops.BrickUploads.Num() == 0 && Ops.PageWrites.Num() == 0);
		TestTrue(TEXT("Queue order intact"),
			Manager.GetRetryQueueDepth() == 2 && Manager.GetRetryQueueHeadDebugName() == TEXT("/WVP/Queue/B"));

		Desired.RemoveAt(0);
		Manager.SetDesiredAssets(1, Desired);
		Manager.IntegrateStep(1, 3, Budget, Ops, bBindingsChanged);
		TestTrue(TEXT("Queue drains oldest-blocked-first once feasible"),
			Manager.GetAssetState(KeyB) == EVoxelPaintAssetResidencyState::Published
			&& Manager.GetAssetState(KeyC) == EVoxelPaintAssetResidencyState::Published);
		TestTrue(TEXT("Queue empty after drain"), Manager.GetRetryQueueDepth() == 0);
		TestTrue(TEXT("Drain is a binding change"), bBindingsChanged);
		uint32 BaseB = MAX_uint32;
		uint32 BaseC = MAX_uint32;
		Manager.ResolveBinding(KeyB, 1, AssetB.Identity, BaseB);
		Manager.ResolveBinding(KeyC, 1, AssetC.Identity, BaseC);
		TestTrue(TEXT("FIFO drain reserves ranges in queue order"), BaseB == 0u && BaseC == 4u);
	}

	{
		const FVoxelPaintDesiredAsset OversizedBricks = MakeDesired(KeyA, 1, MakeIdentity(1), MakeFullManifest(FIntVector(9, 1, 1)), TEXT("/WVP/Queue/OversizedBricks"));
		const FVoxelPaintDesiredAsset OversizedTable = MakeDesired(KeyB, 1, MakeIdentity(2),
			MakeSparseManifest(FIntVector(5, 5, 1), { 0u, 7u }), TEXT("/WVP/Queue/OversizedTable"));

		FVoxelPaintResidencyManager Manager;
		Manager.Initialize(8, 16);

		TArray<FVoxelPaintDesiredAsset> Desired;
		Desired.Add(OversizedBricks);
		Manager.SetDesiredAssets(1, Desired);
		TestTrue(TEXT("Manifest over pool capacity is ConfigError"), Manager.GetAssetState(KeyA) == EVoxelPaintAssetResidencyState::ConfigError);
		TestTrue(TEXT("Oversized asset never queues"), Manager.GetRetryQueueDepth() == 0);
		TestTrue(TEXT("Oversized counted"), Manager.GetOversizedConfigErrorCount() == 1);
		TestFalse(TEXT("Oversized asset never prepares"), Manager.NeedsPreparation(KeyA, PrepGeneration));

		Desired.Add(OversizedTable);
		Manager.SetDesiredAssets(1, Desired);
		TestTrue(TEXT("Table span over page capacity is ConfigError"), Manager.GetAssetState(KeyB) == EVoxelPaintAssetResidencyState::ConfigError);
		TestTrue(TEXT("Both oversized counted"), Manager.GetOversizedConfigErrorCount() == 2);
		TestTrue(TEXT("Queue still empty"), Manager.GetRetryQueueDepth() == 0);
	}

	{
		const FVoxelPaintDesiredAsset Pinned = MakeDesired(KeyA, 1, MakeIdentity(1), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Queue/Pinned"));
		const FVoxelPaintDesiredAsset Retryee1 = MakeDesired(KeyB, 1, MakeIdentity(2), MakeFullManifest(FIntVector(2, 1, 1)), TEXT("/WVP/Queue/Retryee"));
		const FVoxelPaintDesiredAsset Retryee2 = MakeDesired(KeyB, 2, MakeIdentity(3), MakeFullManifest(FIntVector(5, 1, 1)), TEXT("/WVP/Queue/Retryee"));
		const FVoxelPaintDesiredAsset Retryee3 = MakeDesired(KeyB, 3, MakeIdentity(4), MakeFullManifest(FIntVector(2, 1, 1)), TEXT("/WVP/Queue/Retryee"));

		FVoxelPaintResidencyManager Manager;
		Manager.Initialize(4, 64);

		TArray<FVoxelPaintDesiredAsset> Desired;
		Desired.Add(Pinned);
		Manager.SetDesiredAssets(1, Desired);
		Manager.NeedsPreparation(KeyA, PrepGeneration);
		Manager.OnPreparationLaunched(KeyA, 1, 1);
		Manager.OnPreparationCompleted(MakePayload(Pinned, 1));
		Manager.IntegrateStep(1, 1, Budget, Ops, bBindingsChanged);

		Desired.Add(Retryee1);
		Manager.SetDesiredAssets(1, Desired);
		Manager.OnPreparationLaunched(KeyB, 1, 1);
		Manager.OnPreparationCompleted(MakePayload(Retryee1, 1));
		Manager.IntegrateStep(1, 2, Budget, Ops, bBindingsChanged);
		TestTrue(TEXT("Retryee blocked in queue"),
			Manager.GetAssetState(KeyB) == EVoxelPaintAssetResidencyState::Blocked && Manager.GetRetryQueueDepth() == 1);

		Desired[1] = Retryee2;
		Manager.SetDesiredAssets(1, Desired);
		TestTrue(TEXT("Superseding to an oversized generation removes it from the queue"), Manager.GetRetryQueueDepth() == 0);
		TestTrue(TEXT("Oversized supersession is ConfigError"), Manager.GetAssetState(KeyB) == EVoxelPaintAssetResidencyState::ConfigError);
		TestTrue(TEXT("Oversized counted"), Manager.GetOversizedConfigErrorCount() == 1);
		TestFalse(TEXT("ConfigError generation never prepares"), Manager.NeedsPreparation(KeyB, PrepGeneration));

		Desired[1] = Retryee3;
		Manager.SetDesiredAssets(1, Desired);
		TestTrue(TEXT("ConfigError is terminal per generation only: a new generation clears it"),
			Manager.GetAssetState(KeyB) != EVoxelPaintAssetResidencyState::ConfigError);
		TestTrue(TEXT("New generation prepares again"), Manager.NeedsPreparation(KeyB, PrepGeneration) && PrepGeneration == 3u);
	}

	{
		const FVoxelPaintDesiredAsset Asset = MakeDesired(KeyA, 1, MakeIdentity(1), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Queue/StaleEpoch"));

		FVoxelPaintResidencyManager Manager;
		Manager.Initialize(8, 64);

		TArray<FVoxelPaintDesiredAsset> Desired;
		Desired.Add(Asset);
		Manager.SetDesiredAssets(1, Desired);
		Manager.NeedsPreparation(KeyA, PrepGeneration);
		Manager.OnPreparationLaunched(KeyA, 1, 1);

		Manager.NotifyEpochAdvance(2);
		Manager.SetDesiredAssets(2, Desired);
		Manager.OnPreparationCompleted(MakePayload(Asset, 1));
		TestTrue(TEXT("Stale-epoch payload never claims"), Manager.GetRetryQueueDepth() == 0);
		TestTrue(TEXT("Stale-epoch payload never allocates"), Manager.GetAllocatedBrickCount() == 0);
		TestTrue(TEXT("Stale-epoch payload never publishes"),
			Manager.GetAssetState(KeyA) != EVoxelPaintAssetResidencyState::Published
			&& Manager.GetAssetState(KeyA) != EVoxelPaintAssetResidencyState::Integrating);
		TestTrue(TEXT("Asset wants re-preparation under the new epoch"), Manager.NeedsPreparation(KeyA, PrepGeneration));

		Manager.OnPreparationLaunched(KeyA, 1, 2);
		Manager.OnPreparationCompleted(MakePayload(Asset, 2));
		Manager.IntegrateStep(2, 1, Budget, Ops, bBindingsChanged);
		TestTrue(TEXT("Fresh-epoch payload publishes"), Manager.GetAssetState(KeyA) == EVoxelPaintAssetResidencyState::Published);
	}

	{
		const FVoxelPaintDesiredAsset Gen1 = MakeDesired(KeyA, 1, MakeIdentity(1), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Queue/StaleGen"));
		const FVoxelPaintDesiredAsset Gen2 = MakeDesired(KeyA, 2, MakeIdentity(2), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/Queue/StaleGen"));

		FVoxelPaintResidencyManager Manager;
		Manager.Initialize(8, 64);

		TArray<FVoxelPaintDesiredAsset> Desired;
		Desired.Add(Gen1);
		Manager.SetDesiredAssets(1, Desired);
		Manager.NeedsPreparation(KeyA, PrepGeneration);
		Manager.OnPreparationLaunched(KeyA, 1, 1);

		Desired[0] = Gen2;
		Manager.SetDesiredAssets(1, Desired);
		Manager.OnPreparationCompleted(MakePayload(Gen1, 1));
		TestTrue(TEXT("Stale-generation payload never claims"), Manager.GetRetryQueueDepth() == 0);
		TestTrue(TEXT("Stale-generation payload never allocates"), Manager.GetAllocatedBrickCount() == 0);
		TestTrue(TEXT("Stale-generation payload never publishes"),
			Manager.GetAssetState(KeyA) == EVoxelPaintAssetResidencyState::NotResident);
		TestTrue(TEXT("Superseded generation wants preparation"),
			Manager.NeedsPreparation(KeyA, PrepGeneration) && PrepGeneration == 2u);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPool_NeutralProxyVsNoTable,
	"Game.WorldVoxelPaint.Unit.Pool.NeutralProxyVsNoTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPool_NeutralProxyVsNoTable::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintResidencyTestPrivate;

	const FVoxelPaintAssetKey KeyEmpty(UObject::StaticClass());
	const FVoxelPaintAssetKey KeyFiller(UClass::StaticClass());
	const FVoxelPaintAssetKey KeyBlocked(UPackage::StaticClass());
	const FVoxelPaintDesiredAsset EmptyAsset = MakeDesired(KeyEmpty, 1, MakeIdentity(1), MakeEmptyManifest(FIntVector(2, 2, 2)), TEXT("/WVP/EmptyManifest"));
	const FVoxelPaintDesiredAsset FillerAsset = MakeDesired(KeyFiller, 1, MakeIdentity(2), MakeFullManifest(FIntVector(8, 1, 1)), TEXT("/WVP/PoolFiller"));
	const FVoxelPaintDesiredAsset BlockedAsset = MakeDesired(KeyBlocked, 1, MakeIdentity(3), MakeFullManifest(FIntVector(4, 1, 1)), TEXT("/WVP/BlockedPainted"));

	FVoxelPaintResidencyManager Manager;
	Manager.Initialize(8, 64);

	TArray<FVoxelPaintDesiredAsset> Desired;
	Desired.Add(EmptyAsset);
	Desired.Add(FillerAsset);
	Desired.Add(BlockedAsset);
	Manager.SetDesiredAssets(1, Desired);

	uint64 PrepGeneration = 0;
	TestFalse(TEXT("Empty-manifest asset never prepares"), Manager.NeedsPreparation(KeyEmpty, PrepGeneration));
	TestTrue(TEXT("Painted assets prepare"), Manager.NeedsPreparation(KeyFiller, PrepGeneration));

	Manager.OnPreparationLaunched(KeyFiller, 1, 1);
	Manager.OnPreparationCompleted(MakePayload(FillerAsset, 1));
	Manager.OnPreparationLaunched(KeyBlocked, 1, 1);
	Manager.OnPreparationCompleted(MakePayload(BlockedAsset, 1));

	FVoxelPaintIntegrationOps Ops;
	bool bBindingsChanged = false;
	Manager.IntegrateStep(1, 1, AmpleBudget(), Ops, bBindingsChanged);

	TestTrue(TEXT("Empty-manifest asset publishes"), Manager.GetAssetState(KeyEmpty) == EVoxelPaintAssetResidencyState::Published);
	TestTrue(TEXT("Filler publishes"), Manager.GetAssetState(KeyFiller) == EVoxelPaintAssetResidencyState::Published);
	TestTrue(TEXT("Painted asset behind the full pool blocks"), Manager.GetAssetState(KeyBlocked) == EVoxelPaintAssetResidencyState::Blocked);

	uint32 BaseEmpty = 0;
	uint32 BaseFiller = MAX_uint32;
	uint32 BaseBlocked = 0;
	TestTrue(TEXT("Empty-manifest binding is NoTable"),
		Manager.ResolveBinding(KeyEmpty, 1, EmptyAsset.Identity, BaseEmpty) == EVoxelPaintBindingKind::NoTable);
	TestTrue(TEXT("Filler binding is Data"),
		Manager.ResolveBinding(KeyFiller, 1, FillerAsset.Identity, BaseFiller) == EVoxelPaintBindingKind::Data);
	TestTrue(TEXT("Blocked binding is NeutralProxy"),
		Manager.ResolveBinding(KeyBlocked, 1, BlockedAsset.Identity, BaseBlocked) == EVoxelPaintBindingKind::NeutralProxy);
	TestTrue(TEXT("NoTable packs the sentinel"), BaseEmpty == WorldVoxelPaint::NoPageTableSentinel);
	TestTrue(TEXT("NeutralProxy packs the same sentinel"), BaseBlocked == WorldVoxelPaint::NoPageTableSentinel);
	TestTrue(TEXT("Data packs a real base index"), BaseFiller == 0u);

	TestTrue(TEXT("NoTable publication claims zero bricks"), Manager.GetAllocatedBrickCount() == 8);
	TestTrue(TEXT("NoTable publication claims zero page entries"), Manager.GetPageEntries(EVoxelPaintRangeState::Active) == 8);
	TestTrue(TEXT("Diagnostics count exactly one no-table asset"), Manager.GetNoTableAssetCount() == 1);
	TestTrue(TEXT("Diagnostics count exactly one neutral-proxy asset"), Manager.GetNeutralProxyAssetCount() == 1);

	FVoxelPaintPlacementState Placement;
	Placement.Flags = WorldVoxelPaint::RecordFlag_Valid;
	const FVoxelPaintVolumeRecord Record = PackVolumeRecord(Placement, FVoxelPaintResolvedBinding(), FVector::ZeroVector);
	TestTrue(TEXT("The default (no-table / neutral-proxy) binding packs the record sentinel"),
		Record.PageTableBaseIndex == WorldVoxelPaint::NoPageTableSentinel);

	return true;
}
