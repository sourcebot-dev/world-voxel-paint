// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintResidencyTypes.h"

class FVoxelPaintResidencyManager;
struct FVoxelPaintSessionRenderBinding;

namespace WorldVoxelPaint
{
	struct FVoxelPaintAssemblyStats
	{
		int32 NumData = 0;
		int32 NumNoTable = 0;
		int32 NumNeutralProxy = 0;
		int32 NumInvalid = 0;
		int32 NumSessionSelector = 0;
	};

	// THE one producer of packed records (single writer); performs the one LWC->origin rebase.
	// DesiredAssets is the pack's desired-asset side table (keyed by AssetKey): ResolveBinding's
	// match-or-neutral rule needs each placement's canonical identity, which rides this table,
	// never per-placement copies. Placements of SessionBinding's asset run
	// ResolveSessionStagedBinding first: UseSessionTexture packs SessionSelectorActive with the
	// no-table sentinel; ForceNeutralProxy packs the sentinel with selector 0; UseResidency
	// falls through to the residency path. A null binding behaves identically to the overloads
	// below. OutSelectorPublishedIdentities (optional) is reset, then receives State.Identity
	// for every record packed with SessionSelectorActive.
	WORLDVOXELPAINT_API void AssembleRecords(TConstArrayView<FVoxelPaintPlacementState> Placements,
		const FVoxelPaintResidencyManager& Residency, const FVector& EvaluationOrigin,
		TArray<FVoxelPaintVolumeRecord>& OutRecords, FVoxelPaintAssemblyStats& OutStats,
		TConstArrayView<FVoxelPaintDesiredAsset> DesiredAssets,
		const FVoxelPaintSessionRenderBinding* SessionBinding, bool bMasterIsOrigin,
		bool& bOutAnySelectorPublished,
		TArray<FVoxelPaintFrontierIdentity>* OutSelectorPublishedIdentities = nullptr);

	WORLDVOXELPAINT_API void AssembleRecords(TConstArrayView<FVoxelPaintPlacementState> Placements,
		const FVoxelPaintResidencyManager& Residency, const FVector& EvaluationOrigin,
		TArray<FVoxelPaintVolumeRecord>& OutRecords, FVoxelPaintAssemblyStats& OutStats,
		TConstArrayView<FVoxelPaintDesiredAsset> DesiredAssets);

	// Without the side table no identity can match: every asset-bearing placement resolves
	// FAIL-NEUTRAL (sentinel binding, zero contribution). Pass the side table for data bindings.
	WORLDVOXELPAINT_API void AssembleRecords(TConstArrayView<FVoxelPaintPlacementState> Placements,
		const FVoxelPaintResidencyManager& Residency, const FVector& EvaluationOrigin,
		TArray<FVoxelPaintVolumeRecord>& OutRecords, FVoxelPaintAssemblyStats& OutStats);
}
