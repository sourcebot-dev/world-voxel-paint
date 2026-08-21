// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintBudgetViewModel.h"

#include "VoxelPaintSessionMachine.h"
#include "WorldVoxelPaintConstants.h"

namespace VoxelPaintBudgetViewModelPrivate
{
	static bool LabelLess(const FVoxelPaintBudgetVolumeRow& A, const FVoxelPaintBudgetVolumeRow& B)
	{
		return A.Label.Compare(B.Label, ESearchCase::IgnoreCase) < 0;
	}

	static bool MemoryLess(const FVoxelPaintBudgetVolumeRow& A, const FVoxelPaintBudgetVolumeRow& B)
	{
		if (A.ResidentBytes != B.ResidentBytes)
		{
			return A.ResidentBytes > B.ResidentBytes;
		}
		return LabelLess(A, B);
	}

	static void SortRows(TArray<FVoxelPaintBudgetVolumeRow>& Rows, EVoxelPaintBudgetSort Sort)
	{
		switch (Sort)
		{
		case EVoxelPaintBudgetSort::AllocatedVsGridSize:
			Rows.StableSort([](const FVoxelPaintBudgetVolumeRow& A, const FVoxelPaintBudgetVolumeRow& B)
			{
				const int64 DeficitA = static_cast<int64>(A.ResidentBricks) - A.BrickGridCells;
				const int64 DeficitB = static_cast<int64>(B.ResidentBricks) - B.BrickGridCells;
				if (DeficitA != DeficitB)
				{
					return DeficitA < DeficitB;
				}
				return MemoryLess(A, B);
			});
			break;
		case EVoxelPaintBudgetSort::Memory:
			Rows.StableSort(&MemoryLess);
			break;
		case EVoxelPaintBudgetSort::Density:
			Rows.StableSort([](const FVoxelPaintBudgetVolumeRow& A, const FVoxelPaintBudgetVolumeRow& B)
			{
				if (A.Density != B.Density)
				{
					return A.Density > B.Density;
				}
				return LabelLess(A, B);
			});
			break;
		case EVoxelPaintBudgetSort::Distance:
			Rows.StableSort([](const FVoxelPaintBudgetVolumeRow& A, const FVoxelPaintBudgetVolumeRow& B)
			{
				if (A.DistanceCm != B.DistanceCm)
				{
					return A.DistanceCm < B.DistanceCm;
				}
				return LabelLess(A, B);
			});
			break;
		}
	}

	static void FillFromDiagnosticsRow(FVoxelPaintBudgetVolumeRow& RowOut,
		const FVoxelPaintAssetDiagnosticsRow& Row)
	{
		RowOut.ResidentBricks = Row.ResidentBricks;
		RowOut.ResidentBytes = static_cast<int64>(Row.ResidentBricks) * WorldVoxelPaint::BrickPayloadBytes;
		RowOut.ResidencyState = Row.State;
		RowOut.bNeutralProxy = !Row.bPublished;
		RowOut.bNoTable = Row.bNoTable;
		RowOut.bConfigError = Row.bConfigError;
	}

	static FString BuildDerivedLaneStatus(const FVoxelPaintBudgetSessionInput& Session)
	{
		if (!Session.bSessionOpen)
		{
			return TEXT("-");
		}
		if (Session.bActiveAssetConfigError)
		{
			return TEXT("ConfigError - Retry/End Anyway");
		}
		FString Status = WorldVoxelPaint::LexToString(
			static_cast<WorldVoxelPaint::EVoxelPaintSessionStage>(Session.Stage));
		if (Session.DerivingWatchdogSeconds > 0.0f)
		{
			Status += FString::Printf(TEXT(" + watchdog %ds"),
				FMath::FloorToInt32(Session.DerivingWatchdogSeconds));
		}
		if (ShouldWarnDerivedLaneIdle(Session.Stage, Session.SecondsSinceDerivedServiced))
		{
			Status += TEXT(" | ");
			Status += GetDerivedLaneIdleHint();
		}
		return Status;
	}
}

bool ShouldWarnDerivedLaneIdle(uint8 Stage, float SecondsSinceDerivedServiced)
{
	using namespace WorldVoxelPaint;
	const EVoxelPaintSessionStage SessionStage = static_cast<EVoxelPaintSessionStage>(Stage);
	const bool bWaitsOnPublication = SessionStage == EVoxelPaintSessionStage::Deriving
		|| SessionStage == EVoxelPaintSessionStage::Publication
		|| SessionStage == EVoxelPaintSessionStage::Retirement;
	return bWaitsOnPublication
		&& SecondsSinceDerivedServiced >= VoxelPaintDerivedLaneIdleWarnSeconds;
}

const TCHAR* GetDerivedLaneIdleHint()
{
	return TEXT("derived work not being serviced - is the originating map's viewport rendering (Realtime)?");
}

void BuildBudgetSnapshot(const FVoxelPaintResidencyDiagnostics& Diagnostics,
	TConstArrayView<FVoxelPaintBudgetPlacementInput> Placements, const FVector& ViewOrigin,
	const FVoxelPaintBudgetSessionInput& Session, EVoxelPaintBudgetSort Sort,
	FVoxelPaintBudgetSnapshot& Out)
{
	using namespace VoxelPaintBudgetViewModelPrivate;

	Out = FVoxelPaintBudgetSnapshot();
	Out.Residency = Diagnostics;
	Out.Session = Session;
	Out.DerivedLaneStatus = BuildDerivedLaneStatus(Session);

	TMap<FVoxelPaintAssetKey, const FVoxelPaintAssetDiagnosticsRow*> RowsByKey;
	RowsByKey.Reserve(Diagnostics.PerAsset.Num());
	for (const FVoxelPaintAssetDiagnosticsRow& Row : Diagnostics.PerAsset)
	{
		RowsByKey.Add(Row.Key, &Row);
	}

	Out.Rows.Reserve(Placements.Num() + Diagnostics.PerAsset.Num());
	TSet<FVoxelPaintAssetKey> JoinedKeys;
	for (const FVoxelPaintBudgetPlacementInput& Placement : Placements)
	{
		FVoxelPaintBudgetVolumeRow& RowOut = Out.Rows.AddDefaulted_GetRef();
		RowOut.Label = Placement.Label;
		RowOut.AssetName = Placement.AssetName;
		RowOut.BrickCounts = Placement.BrickCounts;
		RowOut.BrickGridCells = static_cast<int64>(Placement.BrickCounts.X)
			* Placement.BrickCounts.Y * Placement.BrickCounts.Z;
		RowOut.PopulatedBricks = Placement.PopulatedBricks;
		RowOut.Density = static_cast<double>(Placement.PopulatedBricks)
			/ static_cast<double>(FMath::Max<int64>(1, RowOut.BrickGridCells));
		RowOut.DistanceCm = FVector::Distance(Placement.Translation, ViewOrigin);
		if (const FVoxelPaintAssetDiagnosticsRow* const* Found = RowsByKey.Find(Placement.AssetKey))
		{
			FillFromDiagnosticsRow(RowOut, **Found);
			JoinedKeys.Add(Placement.AssetKey);
		}
	}

	for (const FVoxelPaintAssetDiagnosticsRow& Row : Diagnostics.PerAsset)
	{
		if (JoinedKeys.Contains(Row.Key))
		{
			continue;
		}
		FVoxelPaintBudgetVolumeRow& RowOut = Out.Rows.AddDefaulted_GetRef();
		RowOut.Label = TEXT("(unreferenced)");
		RowOut.AssetName = Row.DebugName;
		FillFromDiagnosticsRow(RowOut, Row);
	}

	SortRows(Out.Rows, Sort);
}
