// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintBudgetTab.h"

#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "VoxelPaintCoordinator.h"
#include "VoxelPaintGridReadiness.h"
#include "VoxelPaintResidencyTypes.h"
#include "VoxelPaintSessionBudget.h"
#include "VoxelPaintSessionController.h"
#include "VoxelPaintSessionMachine.h"
#include "VoxelPaintTools.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "VoxelPaintWorldSubsystem.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "VoxelPaintBudgetTab"

const FName SVoxelPaintBudgetPanel::TabId(TEXT("VoxelPaintBudget"));

namespace VoxelPaintBudgetTabPrivate
{
	static const FName ColumnVolume(TEXT("Volume"));
	static const FName ColumnAsset(TEXT("Asset"));
	static const FName ColumnAllocVsGrid(TEXT("AllocVsGrid"));
	static const FName ColumnPopulated(TEXT("Populated"));
	static const FName ColumnMemory(TEXT("Memory"));
	static const FName ColumnDensity(TEXT("Density"));
	static const FName ColumnDistance(TEXT("Distance"));
	static const FName ColumnState(TEXT("State"));

	static constexpr float RefreshPeriodSeconds = 0.25f;

	static FString FormatBytes(int64 Bytes)
	{
		if (Bytes >= 1024 * 1024)
		{
			return FString::Printf(TEXT("%.1f MiB"), static_cast<double>(Bytes) / (1024.0 * 1024.0));
		}
		if (Bytes >= 1024)
		{
			return FString::Printf(TEXT("%.1f KiB"), static_cast<double>(Bytes) / 1024.0);
		}
		return FString::Printf(TEXT("%lld B"), static_cast<long long>(Bytes));
	}

	static const TCHAR* ResidencyStateText(uint8 State)
	{
		switch (static_cast<EVoxelPaintAssetResidencyState>(State))
		{
		case EVoxelPaintAssetResidencyState::NotResident: return TEXT("NotResident");
		case EVoxelPaintAssetResidencyState::Preparing: return TEXT("Preparing");
		case EVoxelPaintAssetResidencyState::Integrating: return TEXT("Integrating");
		case EVoxelPaintAssetResidencyState::Published: return TEXT("Published");
		case EVoxelPaintAssetResidencyState::Blocked: return TEXT("Blocked");
		case EVoxelPaintAssetResidencyState::ConfigError: return TEXT("ConfigError");
		default: return TEXT("Unknown");
		}
	}

	static const TCHAR* GridReadinessText(uint8 Readiness)
	{
		switch (static_cast<WorldVoxelPaint::EVoxelPaintGridReadiness>(Readiness))
		{
		case WorldVoxelPaint::EVoxelPaintGridReadiness::Ready: return TEXT("Ready");
		case WorldVoxelPaint::EVoxelPaintGridReadiness::Failed: return TEXT("Failed");
		default: return TEXT("Pending");
		}
	}

	static FText GetCellText(const FVoxelPaintBudgetVolumeRow& Row, const FName& ColumnName)
	{
		if (ColumnName == ColumnVolume)
		{
			return FText::FromString(Row.Label);
		}
		if (ColumnName == ColumnAsset)
		{
			return FText::FromString(Row.AssetName);
		}
		if (ColumnName == ColumnAllocVsGrid)
		{
			return FText::FromString(FString::Printf(TEXT("%d / %lld"),
				Row.ResidentBricks, static_cast<long long>(Row.BrickGridCells)));
		}
		if (ColumnName == ColumnPopulated)
		{
			return FText::AsNumber(Row.PopulatedBricks);
		}
		if (ColumnName == ColumnMemory)
		{
			return FText::FromString(FormatBytes(Row.ResidentBytes));
		}
		if (ColumnName == ColumnDensity)
		{
			return FText::FromString(FString::Printf(TEXT("%.2f%%"), Row.Density * 100.0));
		}
		if (ColumnName == ColumnDistance)
		{
			return FText::FromString(FString::Printf(TEXT("%.1f m"), Row.DistanceCm / 100.0));
		}
		if (ColumnName == ColumnState)
		{
			FString State = ResidencyStateText(Row.ResidencyState);
			if (Row.bNeutralProxy)
			{
				State += TEXT(" (neutral proxy)");
			}
			if (Row.bNoTable)
			{
				State += TEXT(" (no table)");
			}
			return FText::FromString(State);
		}
		return FText::GetEmpty();
	}

	class SVoxelPaintBudgetTableRow : public SMultiColumnTableRow<TSharedPtr<FVoxelPaintBudgetVolumeRow>>
	{
	public:
		SLATE_BEGIN_ARGS(SVoxelPaintBudgetTableRow)
		{
		}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable,
			TSharedPtr<FVoxelPaintBudgetVolumeRow> InRow)
		{
			Row = InRow;
			SMultiColumnTableRow<TSharedPtr<FVoxelPaintBudgetVolumeRow>>::Construct(
				FSuperRowType::FArguments(), OwnerTable);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
		{
			return SNew(STextBlock)
				.Margin(FMargin(4.0f, 2.0f))
				.Text(Row.IsValid() ? GetCellText(*Row, ColumnName) : FText::GetEmpty());
		}

	private:
		TSharedPtr<FVoxelPaintBudgetVolumeRow> Row;
	};
}

TSharedRef<SDockTab> SVoxelPaintBudgetPanel::SpawnBudgetTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		.Label(LOCTEXT("BudgetTabLabel", "Voxel Paint Budget"))
		[
			SNew(SVoxelPaintBudgetPanel)
		];
}

void SVoxelPaintBudgetPanel::Construct(const FArguments& InArgs)
{
	using namespace VoxelPaintBudgetTabPrivate;

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 8.0f, 8.0f, 2.0f)
		[
			SNew(STextBlock).Text(this, &SVoxelPaintBudgetPanel::GetPoolText)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
		[
			SNew(STextBlock).Text(this, &SVoxelPaintBudgetPanel::GetAssetCountsText)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
		[
			SNew(STextBlock).Text(this, &SVoxelPaintBudgetPanel::GetRetryText)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
		[
			SNew(STextBlock).Text(this, &SVoxelPaintBudgetPanel::GetPageText)
		]
		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(8.0f, 6.0f)
		[
			SAssignNew(RowListView, SListView<TSharedPtr<FVoxelPaintBudgetVolumeRow>>)
			.ListItemsSource(&RowItems)
			.OnGenerateRow(this, &SVoxelPaintBudgetPanel::MakeVolumeRow)
			.SelectionMode(ESelectionMode::None)
			.HeaderRow(
				SNew(SHeaderRow)
				+ SHeaderRow::Column(ColumnVolume)
					.DefaultLabel(LOCTEXT("ColumnVolume", "Volume"))
					.FillWidth(1.2f)
				+ SHeaderRow::Column(ColumnAsset)
					.DefaultLabel(LOCTEXT("ColumnAsset", "Asset"))
					.FillWidth(1.2f)
				+ SHeaderRow::Column(ColumnAllocVsGrid)
					.DefaultLabel(LOCTEXT("ColumnAllocVsGrid", "Resident / Grid"))
					.FillWidth(0.8f)
					.SortMode(this, &SVoxelPaintBudgetPanel::GetSortModeForColumn, ColumnAllocVsGrid)
					.OnSort(this, &SVoxelPaintBudgetPanel::OnSortColumn)
				+ SHeaderRow::Column(ColumnPopulated)
					.DefaultLabel(LOCTEXT("ColumnPopulated", "Populated"))
					.FillWidth(0.6f)
				+ SHeaderRow::Column(ColumnMemory)
					.DefaultLabel(LOCTEXT("ColumnMemory", "Memory"))
					.FillWidth(0.6f)
					.SortMode(this, &SVoxelPaintBudgetPanel::GetSortModeForColumn, ColumnMemory)
					.OnSort(this, &SVoxelPaintBudgetPanel::OnSortColumn)
				+ SHeaderRow::Column(ColumnDensity)
					.DefaultLabel(LOCTEXT("ColumnDensity", "Density"))
					.FillWidth(0.5f)
					.SortMode(this, &SVoxelPaintBudgetPanel::GetSortModeForColumn, ColumnDensity)
					.OnSort(this, &SVoxelPaintBudgetPanel::OnSortColumn)
				+ SHeaderRow::Column(ColumnDistance)
					.DefaultLabel(LOCTEXT("ColumnDistance", "Distance"))
					.FillWidth(0.5f)
					.SortMode(this, &SVoxelPaintBudgetPanel::GetSortModeForColumn, ColumnDistance)
					.OnSort(this, &SVoxelPaintBudgetPanel::OnSortColumn)
				+ SHeaderRow::Column(ColumnState)
					.DefaultLabel(LOCTEXT("ColumnState", "State"))
					.FillWidth(1.0f))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
		[
			SNew(STextBlock).Text(this, &SVoxelPaintBudgetPanel::GetSessionText)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
		[
			SNew(STextBlock).Text(this, &SVoxelPaintBudgetPanel::GetGridReadinessText)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
		[
			SNew(STextBlock).Text(this, &SVoxelPaintBudgetPanel::GetJournalText)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
		[
			SNew(STextBlock).Text(this, &SVoxelPaintBudgetPanel::GetSessionGpuText)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
		[
			SNew(STextBlock).Text(this, &SVoxelPaintBudgetPanel::GetPickSourceText)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f, 8.0f, 8.0f)
		[
			SNew(STextBlock).Text(this, &SVoxelPaintBudgetPanel::GetDerivedLaneText)
		]
	];

	RegisterActiveTimer(RefreshPeriodSeconds,
		FWidgetActiveTimerDelegate::CreateSP(this, &SVoxelPaintBudgetPanel::OnRefreshTimer));
	RebuildSnapshot();
}

EActiveTimerReturnType SVoxelPaintBudgetPanel::OnRefreshTimer(double InCurrentTime, float InDeltaTime)
{
	RebuildSnapshot();
	return EActiveTimerReturnType::Continue;
}

void SVoxelPaintBudgetPanel::RebuildSnapshot()
{
	FVoxelPaintResidencyDiagnostics Diagnostics;
	const UWorld* MasterWorld = nullptr;
	FVector ViewOrigin = FVector::ZeroVector;
	if (const FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated())
	{
		Diagnostics = Coordinator->GetResidencyDiagnostics();
		MasterWorld = Coordinator->GetCurrentMasterWorld();
		FBox WindowBounds(ForceInit);
		if (Coordinator->GetExpectedWindowBounds(WindowBounds))
		{
			ViewOrigin = WindowBounds.GetCenter();
		}
	}

	TArray<FVoxelPaintBudgetPlacementInput> Placements;
	GatherPlacements(MasterWorld, Placements);
	const FVoxelPaintBudgetSessionInput Session = GatherSessionInput(Diagnostics);

	BuildBudgetSnapshot(Diagnostics, Placements, ViewOrigin, Session, Sort, Snapshot);

	RowItems.Reset(Snapshot.Rows.Num());
	for (const FVoxelPaintBudgetVolumeRow& Row : Snapshot.Rows)
	{
		RowItems.Add(MakeShared<FVoxelPaintBudgetVolumeRow>(Row));
	}
	if (RowListView.IsValid())
	{
		RowListView->RequestListRefresh();
	}
}

void SVoxelPaintBudgetPanel::GatherPlacements(const UWorld* MasterWorld,
	TArray<FVoxelPaintBudgetPlacementInput>& OutPlacements)
{
	const UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(MasterWorld);
	if (!Subsystem)
	{
		return;
	}

	Subsystem->EnumerateFrontierSlots([&OutPlacements](int32 /*SlotIndex*/,
		const UVoxelPaintVolumeComponent& Component, const FVoxelPaintPlacementState& State)
	{
		FVoxelPaintBudgetPlacementInput& Input = OutPlacements.AddDefaulted_GetRef();
		const AActor* Owner = Component.GetOwner();
		Input.Label = Owner ? Owner->GetActorLabel() : Component.GetName();
		const UVoxelPaintVolumeData* Asset =
			Cast<UVoxelPaintVolumeData>(State.AssetKey.ResolveObjectPtr());
		Input.AssetName = Asset ? Asset->GetName() : TEXT("(none)");
		Input.AssetKey = State.AssetKey;
		Input.BrickCounts = State.BrickCounts;
		Input.Translation = State.Translation;
		const TSharedPtr<const FVoxelPaintSourceManifest> Manifest =
			Asset ? Asset->GetManifestSnapshot() : nullptr;
		Input.PopulatedBricks = Manifest ? Manifest->PopulatedBricks.Num() : 0;
	});
}

FVoxelPaintBudgetSessionInput SVoxelPaintBudgetPanel::GatherSessionInput(
	const FVoxelPaintResidencyDiagnostics& Diagnostics)
{
	FVoxelPaintBudgetSessionInput Session;
	if (const FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated())
	{
		Session.GridReadiness = static_cast<uint8>(Coordinator->GetGridReadiness());
	}
	Session.LastPickSource = FVoxelPaintToolsContext::Get().GetLastPickSource();

	FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
	if (!Controller || !Controller->IsSessionOpen())
	{
		return Session;
	}
	Session.bSessionOpen = true;
	const UVoxelPaintVolumeData* ActiveAsset = Controller->GetActiveAsset();
	Session.ActiveAssetName = GetNameSafe(ActiveAsset);
	Session.Stage = static_cast<uint8>(Controller->GetStage());
	Session.JournalRetainedBytes = Controller->GetJournalRetainedBytes();
	Session.JournalCapacityBytes = Controller->GetJournalCapacityBytes();
	Session.JournalEntries = Controller->GetJournalEntryCount();
	Session.DerivingWatchdogSeconds = Controller->GetDerivingWatchdogSeconds();
	Session.SecondsSinceDerivedServiced = Controller->GetSecondsSinceDerivedServiced();
	if (const WorldVoxelPaint::FVoxelPaintSessionBudgetLedger* Ledger = Controller->GetBudgetLedger())
	{
		Session.SessionGpuUsedBytes = Ledger->GetUsedBytes();
		Session.SessionGpuBudgetBytes = Ledger->GetBudgetBytes();
	}
	if (ActiveAsset)
	{
		const FVoxelPaintAssetKey ActiveKey(ActiveAsset);
		for (const FVoxelPaintAssetDiagnosticsRow& Row : Diagnostics.PerAsset)
		{
			if (Row.Key == ActiveKey)
			{
				Session.bActiveAssetConfigError = Row.bConfigError || Row.bOversized;
				break;
			}
		}
	}
	return Session;
}

TSharedRef<ITableRow> SVoxelPaintBudgetPanel::MakeVolumeRow(TSharedPtr<FVoxelPaintBudgetVolumeRow> Row,
	const TSharedRef<STableViewBase>& OwnerTable) const
{
	return SNew(VoxelPaintBudgetTabPrivate::SVoxelPaintBudgetTableRow, OwnerTable, Row);
}

EColumnSortMode::Type SVoxelPaintBudgetPanel::GetSortModeForColumn(FName ColumnId) const
{
	using namespace VoxelPaintBudgetTabPrivate;
	if (ColumnId == ColumnAllocVsGrid)
	{
		return Sort == EVoxelPaintBudgetSort::AllocatedVsGridSize
			? EColumnSortMode::Ascending : EColumnSortMode::None;
	}
	if (ColumnId == ColumnMemory)
	{
		return Sort == EVoxelPaintBudgetSort::Memory
			? EColumnSortMode::Descending : EColumnSortMode::None;
	}
	if (ColumnId == ColumnDensity)
	{
		return Sort == EVoxelPaintBudgetSort::Density
			? EColumnSortMode::Descending : EColumnSortMode::None;
	}
	if (ColumnId == ColumnDistance)
	{
		return Sort == EVoxelPaintBudgetSort::Distance
			? EColumnSortMode::Ascending : EColumnSortMode::None;
	}
	return EColumnSortMode::None;
}

void SVoxelPaintBudgetPanel::OnSortColumn(EColumnSortPriority::Type SortPriority,
	const FName& ColumnId, EColumnSortMode::Type SortMode)
{
	using namespace VoxelPaintBudgetTabPrivate;
	if (ColumnId == ColumnAllocVsGrid)
	{
		Sort = EVoxelPaintBudgetSort::AllocatedVsGridSize;
	}
	else if (ColumnId == ColumnMemory)
	{
		Sort = EVoxelPaintBudgetSort::Memory;
	}
	else if (ColumnId == ColumnDensity)
	{
		Sort = EVoxelPaintBudgetSort::Density;
	}
	else if (ColumnId == ColumnDistance)
	{
		Sort = EVoxelPaintBudgetSort::Distance;
	}
	RebuildSnapshot();
}

FText SVoxelPaintBudgetPanel::GetPoolText() const
{
	const FVoxelPaintResidencyDiagnostics& R = Snapshot.Residency;
	return FText::FromString(FString::Printf(
		TEXT("Pool: %d / %d bricks allocated | %d pinned | %d warm"),
		R.AllocatedBricks, R.PoolSlotCapacity, R.PinnedBricks, R.WarmCacheBricks));
}

FText SVoxelPaintBudgetPanel::GetAssetCountsText() const
{
	const FVoxelPaintResidencyDiagnostics& R = Snapshot.Residency;
	return FText::FromString(FString::Printf(
		TEXT("Assets: %d neutral-proxy | %d no-table | %d config errors (%d oversized)"),
		R.NeutralProxyAssets, R.NoTableAssets, R.ConfigErrorAssets, R.OversizedConfigErrors));
}

FText SVoxelPaintBudgetPanel::GetRetryText() const
{
	const FVoxelPaintResidencyDiagnostics& R = Snapshot.Residency;
	return FText::FromString(FString::Printf(
		TEXT("Retry queue: depth %d | %d blocked behind head | head: %s"),
		R.RetryQueueDepth, R.RetryBlockedBehindHead,
		R.RetryHeadDebugName.IsEmpty() ? TEXT("-") : *R.RetryHeadDebugName));
}

FText SVoxelPaintBudgetPanel::GetPageText() const
{
	const FVoxelPaintResidencyDiagnostics& R = Snapshot.Residency;
	return FText::FromString(FString::Printf(
		TEXT("Page table: %d active | %d reserved | %d retiring | %d free | %d waste | %d headroom | %d capacity"),
		R.PageEntriesActive, R.PageEntriesReserved, R.PageEntriesRetiring, R.PageEntriesFree,
		R.PageWasteEntries, R.PageHeadroomEntries, R.PageEntryCapacity));
}

FText SVoxelPaintBudgetPanel::GetSessionText() const
{
	const FVoxelPaintBudgetSessionInput& S = Snapshot.Session;
	if (!S.bSessionOpen)
	{
		return LOCTEXT("SessionNone", "Session: none");
	}
	return FText::FromString(FString::Printf(TEXT("Session: %s on '%s'"),
		WorldVoxelPaint::LexToString(static_cast<WorldVoxelPaint::EVoxelPaintSessionStage>(S.Stage)),
		*S.ActiveAssetName));
}

FText SVoxelPaintBudgetPanel::GetGridReadinessText() const
{
	return FText::FromString(FString::Printf(TEXT("Grid readiness: %s"),
		VoxelPaintBudgetTabPrivate::GridReadinessText(Snapshot.Session.GridReadiness)));
}

FText SVoxelPaintBudgetPanel::GetJournalText() const
{
	using namespace VoxelPaintBudgetTabPrivate;
	const FVoxelPaintBudgetSessionInput& S = Snapshot.Session;
	return FText::FromString(FString::Printf(TEXT("Journal: %s / %s | %d entries"),
		*FormatBytes(S.JournalRetainedBytes), *FormatBytes(S.JournalCapacityBytes),
		S.JournalEntries));
}

FText SVoxelPaintBudgetPanel::GetSessionGpuText() const
{
	using namespace VoxelPaintBudgetTabPrivate;
	const FVoxelPaintBudgetSessionInput& S = Snapshot.Session;
	return FText::FromString(FString::Printf(TEXT("Session GPU: %s / %s"),
		*FormatBytes(S.SessionGpuUsedBytes), *FormatBytes(S.SessionGpuBudgetBytes)));
}

FText SVoxelPaintBudgetPanel::GetPickSourceText() const
{
	return FText::FromString(FString::Printf(TEXT("Pick source: %s"),
		*Snapshot.Session.LastPickSource.ToString()));
}

FText SVoxelPaintBudgetPanel::GetDerivedLaneText() const
{
	return FText::FromString(FString::Printf(TEXT("Derived lane: %s"), *Snapshot.DerivedLaneStatus));
}

static FAutoConsoleCommand GVoxelPaintOpenBudgetTabCommand(
	TEXT("wvp.Budget"),
	TEXT("Opens the Voxel Paint Budget panel."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->TryInvokeTab(FTabId(SVoxelPaintBudgetPanel::TabId));
		}
	}));

#undef LOCTEXT_NAMESPACE
