// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FVoxelPaintGatherList;

namespace WorldVoxelPaint
{
	// The canonical brush rejection/falloff formulas; VoxelPaintBrushMath.ush mirrors these
	// token-for-token for the GPU pick pass.
	WORLDVOXELPAINTTOOLSET_API bool PassesRadius(float DistWorld, float RadiusWorld);

	// Rejects when |signed distance along the brush normal| exceeds
	// max(RadiusWorld * (1 - FlattenPct), MinFlattenDepthWorld); the caller supplies
	// MinFlattenDepthWorld as half the texel diagonal.
	WORLDVOXELPAINTTOOLSET_API bool PassesFlatten(float SignedDepthWorld, float RadiusWorld,
		float FlattenPct, float MinFlattenDepthWorld);

	// Rejects when angle(SurfaceNormal, BrushNormal) > AngleToleranceDeg; degenerate/zero
	// normals pass.
	WORLDVOXELPAINTTOOLSET_API bool PassesAngle(const FVector3f& SurfaceNormal,
		const FVector3f& BrushNormal, float AngleToleranceDeg);

	// Apply-side weight: 1 inside FalloffHotspot * Radius, linear to 0 at Radius.
	WORLDVOXELPAINTTOOLSET_API float FalloffWeight(float DistTexels, float RadiusTexels,
		float FalloffHotspot);

	// The one pick-result -> gather-list normalizer. TexelIndices arrive deduped but unordered;
	// the output is deterministic: TexelIndices ascending, DirtyBricks ascending, DirtyMin/Max a
	// tight texel AABB, brush fields passed through. Out-of-domain indices are dropped with one
	// warning. An empty (or fully dropped) input yields a default-constructed empty list.
	WORLDVOXELPAINTTOOLSET_API void BuildGatherListFromPick(const FIntVector& BrickCounts,
		TArray<uint32>&& TexelIndices, const FVector3f& BrushCenterTexel, float BrushRadiusTexels,
		FVoxelPaintGatherList& Out);
}
