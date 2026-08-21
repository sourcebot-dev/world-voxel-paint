// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UVoxelPaintVolumeData;

namespace WorldVoxelPaint
{
	struct FVoxelPaintSeedClassifierInputs
	{
		bool bActorGuidValid = false;
		bool bInstanceGuidMatchesActorGuid = false;
		bool bWasLoaded = false;
		bool bEditorWorld = false;
	};

	struct FVoxelPaintPoolView
	{
		TArray<const UVoxelPaintVolumeData*> Assets;
		TArray<float> Weights;
	};

	WORLDVOXELPAINT_API uint64 MakeIdentitySeed(const FGuid& ActorGuid);

	// Inputs are the rigid placement frame (rotation + translation, scale excluded). Quantization
	// runs BEFORE sign canonicalization: canonicalizing raw floats is boundary-unstable at W ~ 0
	// (every 180-degree rotation), where float noise would flip the whole tuple pre-rounding.
	WORLDVOXELPAINT_API uint64 MakeTransformSeed(const FQuat& Rotation, const FVector& Translation);

	WORLDVOXELPAINT_API bool ClassifyAsIdentitySeeded(const FVoxelPaintSeedClassifierInputs& In);

	// Slot i is valid iff Assets[i] is non-null and no j < i has Assets[j] == Assets[i]
	// (a duplicate aliases its first occurrence).
	WORLDVOXELPAINT_API void ComputeValidSlots(const FVoxelPaintPoolView& Pool, TArray<int32>& OutValidSlots);

	// Returns a raw pool index; INDEX_NONE only when no valid slot exists. Non-finite/negative
	// weights clamp to 0; zero-weight slots never roll; an all-zero valid-slot sum falls back to
	// uniform weights over the valid slots.
	WORLDVOXELPAINT_API int32 ResolveWeightedVariantSlot(uint64 Seed, const FVoxelPaintPoolView& Pool);
}
