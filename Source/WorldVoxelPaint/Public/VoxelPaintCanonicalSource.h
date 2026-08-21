// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PixelFormat.h"
#include "VoxelPaintResidencyTypes.h"

namespace WorldVoxelPaint
{
	struct WORLDVOXELPAINT_API FVoxelPaintLogicalState
	{
		FIntVector BrickCounts = FIntVector::ZeroValue;
		TMap<FIntVector, FColor> Texels;

		void SetTexel(const FIntVector& Coord, FColor Value);
	};

	WORLDVOXELPAINT_API void BuildBrickManifest(const FVoxelPaintLogicalState& State,
		FVoxelPaintSourceManifest& OutManifest);

	WORLDVOXELPAINT_API FColor ResolveBrickTexel(const FVoxelPaintLogicalState& State,
		const FIntVector& BrickCoord, const FIntVector& PhysicalTexel);

	WORLDVOXELPAINT_API FIntVector ComputePackedSourceDimsInBricks(int32 NumPackedRecords);

	WORLDVOXELPAINT_API FIntVector PackedSourceRankToPhysicalBrick(int32 Rank);

	struct FVoxelPaintCanonicalBuild
	{
		FVoxelPaintSourceManifest Manifest;
		FIntVector PhysicalDimsInBricks = FIntVector(1, 1, 1);
		TArray<uint8> PackedSourceBGRA8;
		FVoxelPaintCanonicalIdentity Identity;
	};

	WORLDVOXELPAINT_API void BuildCanonicalSource(const FVoxelPaintLogicalState& State,
		FVoxelPaintCanonicalBuild& OutBuild);

	WORLDVOXELPAINT_API FVoxelPaintCanonicalIdentity ComputeCanonicalIdentity(
		const FVoxelPaintCanonicalBuild& Build, const FVector& StorageOrigin,
		const FVector& LocalHalfExtent);

	// Dense-view session-snapshot adapters: outputs are byte-identical to the TMap-based
	// functions above for equal logical state. DenseTexels is X-fastest Resolution order,
	// Resolution = UniqueBrickSize * BrickCounts.
	WORLDVOXELPAINT_API void BuildBrickManifestFromDense(const FIntVector& BrickCounts,
		TConstArrayView<FColor> DenseTexels, FVoxelPaintSourceManifest& OutManifest);

	WORLDVOXELPAINT_API void BuildCanonicalSourceFromDense(const FIntVector& BrickCounts,
		TConstArrayView<FColor> DenseTexels, FVoxelPaintCanonicalBuild& OutBuild);

	enum class EVoxelPaintPayloadVerdict : uint8 { Valid, WrongFormat, WrongGeometry, WrongSize };

	WORLDVOXELPAINT_API EVoxelPaintPayloadVerdict ClassifyBuiltPayload(
		EPixelFormat Format, const FIntVector& TexelDims, int32 ExpectedPackedRecords, int64 PayloadBytes);

	WORLDVOXELPAINT_API const TCHAR* LexToString(EVoxelPaintPayloadVerdict Verdict);

	WORLDVOXELPAINT_API bool ExtractPackedBrickPayloads(TConstArrayView<uint8> PackedMipBytes,
		const FIntVector& DimsInBricks, int32 NumPackedRecords, TArray<uint8>& OutBrickPayload);

	enum class EVoxelPaintFixturePreset : uint8 { Gradient, Checker, SparseBlob, BorderProbe };

	WORLDVOXELPAINT_API void BuildFixtureState(EVoxelPaintFixturePreset Preset,
		const FIntVector& BrickCounts, FVoxelPaintLogicalState& OutState);
}
