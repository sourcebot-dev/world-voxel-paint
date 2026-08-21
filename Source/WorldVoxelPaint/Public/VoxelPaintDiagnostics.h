// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelPaintResidencyTypes.h"

#if WITH_EDITOR

struct FVoxelPaintAssetDiagnosticsRow
{
	FVoxelPaintAssetKey Key;
	FString DebugName;
	uint8 State = 0;                  // EVoxelPaintAssetResidencyState
	int32 ResidentBricks = 0;         // published representation's slot count (0 when none)
	int64 PageEntries = 0;            // published representation's table entries
	bool bPublished = false;
	bool bWarmCacheOnly = false;
	bool bConfigError = false;
	bool bOversized = false;
	bool bNoTable = false;            // published complete all-zero representation
};

struct FVoxelPaintResidencyDiagnostics
{
	int32 PoolSlotCapacity = 0;
	int32 AllocatedBricks = 0;
	int32 PinnedBricks = 0;
	int32 WarmCacheBricks = 0;
	int32 RetryQueueDepth = 0;
	int32 RetryBlockedBehindHead = 0;
	FString RetryHeadDebugName;
	int32 OversizedConfigErrors = 0;
	int32 ConfigErrorAssets = 0;
	int32 NeutralProxyAssets = 0;
	int32 NoTableAssets = 0;
	int32 PageEntryCapacity = 0;
	int32 PageEntriesActive = 0;
	int32 PageEntriesReserved = 0;
	int32 PageEntriesRetiring = 0;
	int32 PageEntriesFree = 0;
	int32 PageWasteEntries = 0;
	int32 PageHeadroomEntries = 0;
	uint64 PublicationOrdinal = 0;    // staleness marker for consumers
	TArray<FVoxelPaintAssetDiagnosticsRow> PerAsset;
};

#endif
