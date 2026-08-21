// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"
#include "RenderGraphResources.h"
#include "VoxelPaintGridSetup.h"
#include "VoxelPaintSceneUniformBuffer.h"

class WORLDVOXELPAINT_API FVoxelPaintGridRenderer
{
public:
	void Initialize(FRDGBuilder& GraphBuilder, const WorldVoxelPaint::FVoxelPaintGridCapacity& Capacity);
	void Reinitialize(FRDGBuilder& GraphBuilder, const WorldVoxelPaint::FVoxelPaintGridCapacity& Capacity);
	bool IsInitialized() const;

	// WITH_EDITOR OutErrorFlagBuffer: the rebuild's 4-byte error-flag RDG buffer (created and
	// cleared even for an empty build so a same-graph readiness readback can observe it), or
	// null when the rebuild was rejected fail-neutral.
	void Rebuild(FRDGBuilder& GraphBuilder, const WorldVoxelPaint::FVoxelPaintGridBuildInput& Input,
		FRDGBufferSRVRef VolumesSRV
#if WITH_EDITOR
		, FRDGBufferRef* OutErrorFlagBuffer = nullptr
#endif
	);
	// bCandidateOverflow: true only for candidate-emission overflow (E/TotalThreads exceeding
	// K_cap); other fail-neutral causes count as rejected rebuilds. GPU-detected error-flag
	// clears are intentionally uncounted (no readback).
	void FailNeutral(FRDGBuilder& GraphBuilder, bool bCandidateOverflow);
	void Register(FRDGBuilder& GraphBuilder, FVoxelPaintSceneParameters& Params) const;

	const WorldVoxelPaint::FVoxelPaintGridCapacity& GetCapacity() const { return Capacity; }
	uint32 GetLastLiveCellCount() const { return LastLiveCellCount; }
	uint32 GetLastEmitUpperBound() const { return LastEmitUpperBound; }
	uint64 GetSteadyBytes() const;
	uint64 GetLastRebuildScratchBytes() const { return LastRebuildScratchBytes; }
	uint64 GetRebuildCount() const { return RebuildCount; }
	uint64 GetOverflowEventCount() const { return OverflowEventCount; }
	uint64 GetRejectedRebuildCount() const { return RejectedRebuildCount; }

private:
	WorldVoxelPaint::FVoxelPaintGridCapacity Capacity;

	TRefCountPtr<FRDGPooledBuffer> GridHeadersBuffer;
	TRefCountPtr<FRDGPooledBuffer> GridIndexListBuffer;

	FIntVector PublishedGridDims = FIntVector::ZeroValue;
	FVector3f PublishedGridOrigin = FVector3f::ZeroVector;
	FVector2f PublishedCellSize = FVector2f::ZeroVector;

	uint32 LastLiveCellCount = 0;
	uint32 LastEmitUpperBound = 0;
	uint64 LastRebuildScratchBytes = 0;
	uint64 RebuildCount = 0;
	uint64 OverflowEventCount = 0;
	uint64 RejectedRebuildCount = 0;
};
