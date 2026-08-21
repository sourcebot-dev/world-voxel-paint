// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

WORLDVOXELPAINT_API DECLARE_LOG_CATEGORY_EXTERN(LogWorldVoxelPaint, Log, All);

namespace WorldVoxelPaint
{
	inline constexpr int32 BrickSize = 8;
	inline constexpr int32 UniqueBrickSize = 7;
	inline constexpr int32 MaxSelectedPlacements = 4096;
	inline constexpr int32 AssetAdmissionCap = 32768;
	inline constexpr int32 RecordNumFloat4Lanes = 7;
	inline constexpr uint32 NoPageTableSentinel = 0xFFFFFFFFu;
	inline constexpr uint32 RecordFlag_Valid = 0x1u;
	inline constexpr uint32 PageEntryAbsent = 0u;
	inline constexpr int32 BrickPayloadBytes = 512;
	inline constexpr int32 PoolBricksXY = 128;
	inline constexpr int32 SourceBricksXY = 16;
	inline constexpr uint32 GridHeaderCountBits = 9;
	inline constexpr uint32 GridHeaderCountMask = 0x1FFu;
	inline constexpr uint32 GridCandidateCapCeiling = 1u << 23;
	// Per-cell/per-pixel cap, NOT the placement frontier: canonicalization publishes and
	// materials evaluate at most this many candidates per grid cell; overfull cells silently
	// drop the tail. The 9-bit header count range above it is representational headroom only.
	inline constexpr uint32 MaxEvaluatedCandidatesPerCell = 32u;
	static_assert(MaxEvaluatedCandidatesPerCell <= GridHeaderCountMask,
		"Published per-cell Count must fit the header count field");
	inline constexpr uint32 CanonicalLayoutVersion = 1;
	inline constexpr int64 IntegrationBudgetUploadBytes = 8 << 20;
	inline constexpr int32 IntegrationBudgetPageEntries = 65536;

	inline constexpr uint32 SessionSelectorActive = 1u;
	inline constexpr int64 SessionGpuBudgetBytes = 256ll << 20;
	inline constexpr int32 SessionTexelBytes = 4;
	inline constexpr int32 TexelsPerUniqueBrick = 343;
	inline constexpr int32 GridReadinessReadbackSlots = 4;
	inline constexpr int32 StrokeReadbackSlots = 1;

	inline constexpr int32 PickReadbackSlots = 4;
	inline constexpr int32 PickWorkListTexelCap = 1 << 22;
	inline constexpr int32 MaxVisualizerGridLines = 4096;
	inline constexpr float ResidencyDiagnosticsRefreshSeconds = 0.25f;

	// Well-known tag carried by any actor hosting a registered UVoxelPaintVolumeComponent:
	// added idempotently in OnRegister, removed by OnUnregister only when the actor's last
	// volume component unregisters (dedicated AVoxelPaintVolume actors ride the same component
	// path). PCG change tracking (WorldVoxelPaintPCG) matches host actors through one culled
	// ByTag key on this exact name; tests pin the literal. Namespaced and wildcard-free so PCG
	// tag matching stays on its cheap Contains path. Unexported inline on purpose: FName
	// equality is by content, so per-module instances are interchangeable.
	inline const FName PaintVolumeHostTag = FName(TEXT("WVP.PaintVolume"));
}
