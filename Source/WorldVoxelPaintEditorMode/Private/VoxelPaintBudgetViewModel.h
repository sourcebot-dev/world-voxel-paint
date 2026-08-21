// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/ArrayView.h"
#include "CoreMinimal.h"
#include "UObject/ObjectKey.h"
#include "VoxelPaintDiagnostics.h"

struct FVoxelPaintBudgetPlacementInput
{
	FString Label;
	FString AssetName;
	FObjectKey AssetKey;
	FIntVector BrickCounts = FIntVector::ZeroValue;
	int32 PopulatedBricks = 0;
	FVector Translation = FVector::ZeroVector;
};

struct FVoxelPaintBudgetSessionInput
{
	bool bSessionOpen = false;
	FString ActiveAssetName;
	uint8 Stage = 0;
	uint8 GridReadiness = 0;
	int64 JournalRetainedBytes = 0;
	int64 JournalCapacityBytes = 0;
	int32 JournalEntries = 0;
	int64 SessionGpuUsedBytes = 0;
	int64 SessionGpuBudgetBytes = 0;
	float DerivingWatchdogSeconds = 0.0f;
	float SecondsSinceDerivedServiced = 0.0f;
	bool bActiveAssetConfigError = false;
	FName LastPickSource;
};

inline constexpr float VoxelPaintDerivedLaneIdleWarnSeconds = 3.0f;

// True when the session waits on publication (Deriving/Publication/Retirement) but no
// publication ordinal has advanced for VoxelPaintDerivedLaneIdleWarnSeconds of wall time.
bool ShouldWarnDerivedLaneIdle(uint8 Stage, float SecondsSinceDerivedServiced);
const TCHAR* GetDerivedLaneIdleHint();

struct FVoxelPaintBudgetVolumeRow
{
	FString Label;
	FString AssetName;
	FIntVector BrickCounts = FIntVector::ZeroValue;
	int64 BrickGridCells = 0;
	int32 PopulatedBricks = 0;
	int32 ResidentBricks = 0;
	int64 ResidentBytes = 0;
	double Density = 0.0;
	double DistanceCm = 0.0;
	uint8 ResidencyState = 0;
	bool bNeutralProxy = false;
	bool bNoTable = false;
	bool bConfigError = false;
};

enum class EVoxelPaintBudgetSort : uint8
{
	AllocatedVsGridSize,
	Memory,
	Density,
	Distance
};

struct FVoxelPaintBudgetSnapshot
{
	TArray<FVoxelPaintBudgetVolumeRow> Rows;
	FVoxelPaintResidencyDiagnostics Residency;
	FVoxelPaintBudgetSessionInput Session;
	FString DerivedLaneStatus;
};

void BuildBudgetSnapshot(const FVoxelPaintResidencyDiagnostics& Diagnostics,
	TConstArrayView<FVoxelPaintBudgetPlacementInput> Placements, const FVector& ViewOrigin,
	const FVoxelPaintBudgetSessionInput& Session, EVoxelPaintBudgetSort Sort,
	FVoxelPaintBudgetSnapshot& Out);
