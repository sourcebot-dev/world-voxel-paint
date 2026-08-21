// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Types/SlateEnums.h"
#include "VoxelPaintBudgetViewModel.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"

class FSpawnTabArgs;
class ITableRow;
class SDockTab;
class STableViewBase;
class UWorld;

class SVoxelPaintBudgetPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelPaintBudgetPanel)
	{
	}
	SLATE_END_ARGS()

	static const FName TabId;
	static TSharedRef<SDockTab> SpawnBudgetTab(const FSpawnTabArgs& Args);

	void Construct(const FArguments& InArgs);

private:
	EActiveTimerReturnType OnRefreshTimer(double InCurrentTime, float InDeltaTime);
	void RebuildSnapshot();
	static void GatherPlacements(const UWorld* MasterWorld,
		TArray<FVoxelPaintBudgetPlacementInput>& OutPlacements);
	static FVoxelPaintBudgetSessionInput GatherSessionInput(
		const FVoxelPaintResidencyDiagnostics& Diagnostics);

	TSharedRef<ITableRow> MakeVolumeRow(TSharedPtr<FVoxelPaintBudgetVolumeRow> Row,
		const TSharedRef<STableViewBase>& OwnerTable) const;
	EColumnSortMode::Type GetSortModeForColumn(FName ColumnId) const;
	void OnSortColumn(EColumnSortPriority::Type SortPriority, const FName& ColumnId,
		EColumnSortMode::Type SortMode);

	FText GetPoolText() const;
	FText GetAssetCountsText() const;
	FText GetRetryText() const;
	FText GetPageText() const;
	FText GetSessionText() const;
	FText GetGridReadinessText() const;
	FText GetJournalText() const;
	FText GetSessionGpuText() const;
	FText GetPickSourceText() const;
	FText GetDerivedLaneText() const;

	FVoxelPaintBudgetSnapshot Snapshot;
	TArray<TSharedPtr<FVoxelPaintBudgetVolumeRow>> RowItems;
	TSharedPtr<SListView<TSharedPtr<FVoxelPaintBudgetVolumeRow>>> RowListView;
	EVoxelPaintBudgetSort Sort = EVoxelPaintBudgetSort::AllocatedVsGridSize;
};
