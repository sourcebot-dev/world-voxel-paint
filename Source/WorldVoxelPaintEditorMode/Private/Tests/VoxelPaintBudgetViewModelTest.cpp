// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "VoxelPaintBudgetViewModel.h"
#include "VoxelPaintDiagnostics.h"
#include "VoxelPaintResidencyTypes.h"
#include "VoxelPaintSessionMachine.h"
#include "VoxelPaintVolumeData.h"
#include "WorldVoxelPaintConstants.h"

namespace VoxelPaintBudgetViewModelTestPrivate
{
	static FString RowLabels(const FVoxelPaintBudgetSnapshot& Snapshot)
	{
		TArray<FString> Labels;
		Labels.Reserve(Snapshot.Rows.Num());
		for (const FVoxelPaintBudgetVolumeRow& Row : Snapshot.Rows)
		{
			Labels.Add(Row.Label);
		}
		return FString::Join(Labels, TEXT("|"));
	}

	static const FVoxelPaintBudgetVolumeRow* FindRow(const FVoxelPaintBudgetSnapshot& Snapshot,
		const TCHAR* Label)
	{
		return Snapshot.Rows.FindByPredicate([Label](const FVoxelPaintBudgetVolumeRow& Row)
		{
			return Row.Label == Label;
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintBudget_ViewModel,
	"Game.WorldVoxelPaint.Unit.Budget.ViewModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintBudget_ViewModel::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintBudgetViewModelTestPrivate;
	using namespace WorldVoxelPaint;

	UVoxelPaintVolumeData* AssetA = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	UVoxelPaintVolumeData* AssetB = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	UVoxelPaintVolumeData* AssetC = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	UVoxelPaintVolumeData* AssetD = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	if (!TestNotNull(TEXT("Fabricated key objects created"), AssetA) || !AssetB || !AssetC || !AssetD)
	{
		return false;
	}

	FVoxelPaintResidencyDiagnostics Diagnostics;
	Diagnostics.PoolSlotCapacity = 16384;
	Diagnostics.AllocatedBricks = 107;
	Diagnostics.PinnedBricks = 100;
	Diagnostics.WarmCacheBricks = 7;
	Diagnostics.RetryQueueDepth = 1;
	Diagnostics.RetryBlockedBehindHead = 2;
	Diagnostics.RetryHeadDebugName = TEXT("AssetB_Unref");
	Diagnostics.PageEntryCapacity = 65536;
	Diagnostics.PageEntriesActive = 321;
	Diagnostics.PublicationOrdinal = 42;

	{
		FVoxelPaintAssetDiagnosticsRow& RowA = Diagnostics.PerAsset.AddDefaulted_GetRef();
		RowA.Key = FVoxelPaintAssetKey(AssetA);
		RowA.DebugName = TEXT("AssetA");
		RowA.State = (uint8)EVoxelPaintAssetResidencyState::Published;
		RowA.ResidentBricks = 100;
		RowA.PageEntries = 800;
		RowA.bPublished = true;
	}
	{
		FVoxelPaintAssetDiagnosticsRow& RowB = Diagnostics.PerAsset.AddDefaulted_GetRef();
		RowB.Key = FVoxelPaintAssetKey(AssetB);
		RowB.DebugName = TEXT("AssetB_Unref");
		RowB.State = (uint8)EVoxelPaintAssetResidencyState::Blocked;
		RowB.ResidentBricks = 7;
		RowB.bPublished = false;
	}
	{
		FVoxelPaintAssetDiagnosticsRow& RowC = Diagnostics.PerAsset.AddDefaulted_GetRef();
		RowC.Key = FVoxelPaintAssetKey(AssetC);
		RowC.DebugName = TEXT("AssetC_NoTable");
		RowC.State = (uint8)EVoxelPaintAssetResidencyState::Published;
		RowC.ResidentBricks = 0;
		RowC.bPublished = true;
		RowC.bNoTable = true;
	}

	TArray<FVoxelPaintBudgetPlacementInput> Placements;
	{
		FVoxelPaintBudgetPlacementInput& Alpha = Placements.AddDefaulted_GetRef();
		Alpha.Label = TEXT("Alpha");
		Alpha.AssetName = TEXT("AssetA");
		Alpha.AssetKey = FObjectKey(AssetA);
		Alpha.BrickCounts = FIntVector(4, 4, 4);
		Alpha.PopulatedBricks = 32;
		Alpha.Translation = FVector(300.0, 400.0, 0.0);
	}
	{
		FVoxelPaintBudgetPlacementInput& Charlie = Placements.AddDefaulted_GetRef();
		Charlie.Label = TEXT("Charlie");
		Charlie.AssetName = TEXT("AssetC_NoTable");
		Charlie.AssetKey = FObjectKey(AssetC);
		Charlie.BrickCounts = FIntVector(2, 2, 2);
		Charlie.PopulatedBricks = 8;
		Charlie.Translation = FVector(0.0, 0.0, 100.0);
	}
	{
		FVoxelPaintBudgetPlacementInput& Delta = Placements.AddDefaulted_GetRef();
		Delta.Label = TEXT("delta");
		Delta.AssetName = TEXT("(none)");
		Delta.AssetKey = FObjectKey(AssetD);
		Delta.BrickCounts = FIntVector(1, 1, 1);
	}
	{
		FVoxelPaintBudgetPlacementInput& Echo = Placements.AddDefaulted_GetRef();
		Echo.Label = TEXT("Echo");
		Echo.AssetName = TEXT("(none)");
		Echo.BrickCounts = FIntVector(1, 1, 1);
	}

	FVoxelPaintBudgetSessionInput Session;
	Session.bSessionOpen = true;
	Session.ActiveAssetName = TEXT("AssetA");
	Session.Stage = (uint8)EVoxelPaintSessionStage::Active;
	Session.GridReadiness = 1;
	Session.JournalRetainedBytes = 1024;
	Session.JournalCapacityBytes = 4096;
	Session.JournalEntries = 3;
	Session.SessionGpuUsedBytes = 8 << 20;
	Session.SessionGpuBudgetBytes = 256ll << 20;
	Session.LastPickSource = FName(TEXT("GPU"));

	const FVector ViewOrigin = FVector::ZeroVector;

	FVoxelPaintBudgetSnapshot Snapshot;
	BuildBudgetSnapshot(Diagnostics, Placements, ViewOrigin, Session,
		EVoxelPaintBudgetSort::Memory, Snapshot);

	TestEqual(TEXT("Join: four placements plus one unreferenced diagnostics row"),
		Snapshot.Rows.Num(), 5);

	const FVoxelPaintBudgetVolumeRow* Alpha = FindRow(Snapshot, TEXT("Alpha"));
	const FVoxelPaintBudgetVolumeRow* Charlie = FindRow(Snapshot, TEXT("Charlie"));
	const FVoxelPaintBudgetVolumeRow* Delta = FindRow(Snapshot, TEXT("delta"));
	const FVoxelPaintBudgetVolumeRow* Echo = FindRow(Snapshot, TEXT("Echo"));
	const FVoxelPaintBudgetVolumeRow* Unreferenced = FindRow(Snapshot, TEXT("(unreferenced)"));
	if (!TestNotNull(TEXT("Join: Alpha row exists"), Alpha)
		|| !TestNotNull(TEXT("Join: Charlie row exists"), Charlie)
		|| !TestNotNull(TEXT("Join: delta row exists"), Delta)
		|| !TestNotNull(TEXT("Join: Echo row exists"), Echo)
		|| !TestNotNull(TEXT("Join: unreferenced diagnostics row is appended"), Unreferenced))
	{
		return false;
	}

	TestEqual(TEXT("Join: Alpha takes its diagnostics row's resident bricks"),
		Alpha->ResidentBricks, 100);
	TestEqual(TEXT("Derived: Memory is bricks * 512"),
		Alpha->ResidentBytes, (int64)100 * BrickPayloadBytes);
	TestEqual(TEXT("Derived: BrickGridCells is the brick-count product"),
		Alpha->BrickGridCells, (int64)64);
	TestEqual(TEXT("Derived: Density is populated over cells"), Alpha->Density, 0.5);
	TestEqual(TEXT("Derived: Distance is placement-to-view-origin"), Alpha->DistanceCm, 500.0);
	TestEqual(TEXT("Join: Alpha carries its residency state ordinal"),
		Alpha->ResidencyState, (uint8)EVoxelPaintAssetResidencyState::Published);

	TestTrue(TEXT("Join: placement-only rows read zero residency with false flags"),
		Delta->ResidentBricks == 0 && Delta->ResidentBytes == 0 && Delta->ResidencyState == 0
			&& !Delta->bNeutralProxy && !Delta->bNoTable && !Delta->bConfigError);
	TestTrue(TEXT("Join: the unreferenced row carries the diagnostics identity"),
		Unreferenced->AssetName == TEXT("AssetB_Unref") && Unreferenced->ResidentBricks == 7
			&& Unreferenced->BrickGridCells == 0);

	TestTrue(TEXT("Flags: an unpublished row reads as a neutral proxy, not no-table"),
		Unreferenced->bNeutralProxy && !Unreferenced->bNoTable);
	TestTrue(TEXT("Flags: a published all-zero row reads as no-table, not a neutral proxy"),
		!Charlie->bNeutralProxy && Charlie->bNoTable);

	TestEqual(TEXT("Passthrough: retry head is named"),
		Snapshot.Residency.RetryHeadDebugName, FString(TEXT("AssetB_Unref")));
	TestTrue(TEXT("Passthrough: retry depth and blocked-behind-head copy through"),
		Snapshot.Residency.RetryQueueDepth == 1 && Snapshot.Residency.RetryBlockedBehindHead == 2);
	TestEqual(TEXT("Passthrough: the publication ordinal copies through"),
		Snapshot.Residency.PublicationOrdinal, (uint64)42);
	TestTrue(TEXT("Passthrough: session inputs copy through"),
		Snapshot.Session.JournalRetainedBytes == 1024 && Snapshot.Session.JournalEntries == 3
			&& Snapshot.Session.LastPickSource == FName(TEXT("GPU")));

	TestEqual(TEXT("Sort: Memory descends with case-insensitive label tie-break"),
		RowLabels(Snapshot), FString(TEXT("Alpha|(unreferenced)|Charlie|delta|Echo")));
	{
		FVoxelPaintBudgetSnapshot Sorted;
		BuildBudgetSnapshot(Diagnostics, Placements, ViewOrigin, Session,
			EVoxelPaintBudgetSort::AllocatedVsGridSize, Sorted);
		TestEqual(TEXT("Sort: AllocatedVsGridSize ascends by resident-minus-cells deficit"),
			RowLabels(Sorted), FString(TEXT("Charlie|delta|Echo|(unreferenced)|Alpha")));
	}
	{
		FVoxelPaintBudgetSnapshot Sorted;
		BuildBudgetSnapshot(Diagnostics, Placements, ViewOrigin, Session,
			EVoxelPaintBudgetSort::Density, Sorted);
		TestEqual(TEXT("Sort: Density descends with label tie-break"),
			RowLabels(Sorted), FString(TEXT("Charlie|Alpha|(unreferenced)|delta|Echo")));
	}
	{
		FVoxelPaintBudgetSnapshot Sorted;
		BuildBudgetSnapshot(Diagnostics, Placements, ViewOrigin, Session,
			EVoxelPaintBudgetSort::Distance, Sorted);
		TestEqual(TEXT("Sort: Distance ascends with label tie-break"),
			RowLabels(Sorted), FString(TEXT("(unreferenced)|delta|Echo|Charlie|Alpha")));
	}

	{
		FVoxelPaintBudgetSessionInput ClosedSession;
		FVoxelPaintBudgetSnapshot StatusSnapshot;
		BuildBudgetSnapshot(FVoxelPaintResidencyDiagnostics(),
			TConstArrayView<FVoxelPaintBudgetPlacementInput>(), ViewOrigin, ClosedSession,
			EVoxelPaintBudgetSort::Memory, StatusSnapshot);
		TestEqual(TEXT("Status: no session reads '-'"),
			StatusSnapshot.DerivedLaneStatus, FString(TEXT("-")));

		FVoxelPaintBudgetSessionInput ActiveSession;
		ActiveSession.bSessionOpen = true;
		ActiveSession.Stage = (uint8)EVoxelPaintSessionStage::Active;
		BuildBudgetSnapshot(FVoxelPaintResidencyDiagnostics(),
			TConstArrayView<FVoxelPaintBudgetPlacementInput>(), ViewOrigin, ActiveSession,
			EVoxelPaintBudgetSort::Memory, StatusSnapshot);
		TestEqual(TEXT("Status: an open session reads its stage name"),
			StatusSnapshot.DerivedLaneStatus, FString(TEXT("Active")));

		FVoxelPaintBudgetSessionInput DerivingSession;
		DerivingSession.bSessionOpen = true;
		DerivingSession.Stage = (uint8)EVoxelPaintSessionStage::Deriving;
		DerivingSession.DerivingWatchdogSeconds = 2.7f;
		BuildBudgetSnapshot(FVoxelPaintResidencyDiagnostics(),
			TConstArrayView<FVoxelPaintBudgetPlacementInput>(), ViewOrigin, DerivingSession,
			EVoxelPaintBudgetSort::Memory, StatusSnapshot);
		TestEqual(TEXT("Status: Deriving appends the floored watchdog seconds"),
			StatusSnapshot.DerivedLaneStatus, FString(TEXT("Deriving + watchdog 2s")));

		FVoxelPaintBudgetSessionInput FailedSession;
		FailedSession.bSessionOpen = true;
		FailedSession.Stage = (uint8)EVoxelPaintSessionStage::DerivedFailed;
		BuildBudgetSnapshot(FVoxelPaintResidencyDiagnostics(),
			TConstArrayView<FVoxelPaintBudgetPlacementInput>(), ViewOrigin, FailedSession,
			EVoxelPaintBudgetSort::Memory, StatusSnapshot);
		TestEqual(TEXT("Status: DerivedFailed reads its stage name"),
			StatusSnapshot.DerivedLaneStatus, FString(TEXT("DerivedFailed")));

		FVoxelPaintBudgetSessionInput ConfigErrorSession;
		ConfigErrorSession.bSessionOpen = true;
		ConfigErrorSession.Stage = (uint8)EVoxelPaintSessionStage::Deriving;
		ConfigErrorSession.DerivingWatchdogSeconds = 5.0f;
		ConfigErrorSession.bActiveAssetConfigError = true;
		BuildBudgetSnapshot(FVoxelPaintResidencyDiagnostics(),
			TConstArrayView<FVoxelPaintBudgetPlacementInput>(), ViewOrigin, ConfigErrorSession,
			EVoxelPaintBudgetSort::Memory, StatusSnapshot);
		TestEqual(TEXT("Status: a config error takes precedence over the stage"),
			StatusSnapshot.DerivedLaneStatus, FString(TEXT("ConfigError - Retry/End Anyway")));
	}

	{
		TestFalse(TEXT("IdleHint: Deriving below the threshold does not warn"),
			ShouldWarnDerivedLaneIdle((uint8)EVoxelPaintSessionStage::Deriving,
				VoxelPaintDerivedLaneIdleWarnSeconds - 0.1f));
		TestTrue(TEXT("IdleHint: Deriving at the threshold warns"),
			ShouldWarnDerivedLaneIdle((uint8)EVoxelPaintSessionStage::Deriving,
				VoxelPaintDerivedLaneIdleWarnSeconds));
		TestTrue(TEXT("IdleHint: Publication past the threshold warns"),
			ShouldWarnDerivedLaneIdle((uint8)EVoxelPaintSessionStage::Publication, 10.0f));
		TestTrue(TEXT("IdleHint: Retirement past the threshold warns"),
			ShouldWarnDerivedLaneIdle((uint8)EVoxelPaintSessionStage::Retirement, 10.0f));
		TestFalse(TEXT("IdleHint: Active never warns regardless of idle time"),
			ShouldWarnDerivedLaneIdle((uint8)EVoxelPaintSessionStage::Active, 10.0f));
		TestFalse(TEXT("IdleHint: Closed never warns"),
			ShouldWarnDerivedLaneIdle((uint8)EVoxelPaintSessionStage::Closed, 10.0f));

		FVoxelPaintBudgetSnapshot StatusSnapshot;
		FVoxelPaintBudgetSessionInput IdleDeriving;
		IdleDeriving.bSessionOpen = true;
		IdleDeriving.Stage = (uint8)EVoxelPaintSessionStage::Deriving;
		IdleDeriving.SecondsSinceDerivedServiced = 10.0f;
		BuildBudgetSnapshot(FVoxelPaintResidencyDiagnostics(),
			TConstArrayView<FVoxelPaintBudgetPlacementInput>(), ViewOrigin, IdleDeriving,
			EVoxelPaintBudgetSort::Memory, StatusSnapshot);
		TestEqual(TEXT("IdleHint: an idle Deriving lane appends the servicing hint"),
			StatusSnapshot.DerivedLaneStatus,
			FString(TEXT("Deriving | ")) + GetDerivedLaneIdleHint());

		FVoxelPaintBudgetSessionInput ServicedDeriving;
		ServicedDeriving.bSessionOpen = true;
		ServicedDeriving.Stage = (uint8)EVoxelPaintSessionStage::Deriving;
		ServicedDeriving.DerivingWatchdogSeconds = 2.7f;
		ServicedDeriving.SecondsSinceDerivedServiced = 0.1f;
		BuildBudgetSnapshot(FVoxelPaintResidencyDiagnostics(),
			TConstArrayView<FVoxelPaintBudgetPlacementInput>(), ViewOrigin, ServicedDeriving,
			EVoxelPaintBudgetSort::Memory, StatusSnapshot);
		TestEqual(TEXT("IdleHint: a serviced Deriving lane shows no hint"),
			StatusSnapshot.DerivedLaneStatus, FString(TEXT("Deriving + watchdog 2s")));

		FVoxelPaintBudgetSessionInput IdleConfigError;
		IdleConfigError.bSessionOpen = true;
		IdleConfigError.Stage = (uint8)EVoxelPaintSessionStage::Deriving;
		IdleConfigError.SecondsSinceDerivedServiced = 10.0f;
		IdleConfigError.bActiveAssetConfigError = true;
		BuildBudgetSnapshot(FVoxelPaintResidencyDiagnostics(),
			TConstArrayView<FVoxelPaintBudgetPlacementInput>(), ViewOrigin, IdleConfigError,
			EVoxelPaintBudgetSort::Memory, StatusSnapshot);
		TestEqual(TEXT("IdleHint: the config-error fast path keeps precedence over the hint"),
			StatusSnapshot.DerivedLaneStatus, FString(TEXT("ConfigError - Retry/End Anyway")));
	}

	return true;
}
